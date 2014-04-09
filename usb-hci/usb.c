/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "usbhost.h"
#include <bt_vendor_lib.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/socket.h>

#define CTRL_HEADER_SIZE   3
#define ACL_HEADER_SIZE    4

#ifndef TEMP_FAILURE_RETRY
/* Used to retry syscalls that can return EINTR. */
#define TEMP_FAILURE_RETRY(exp) ({         \
    typeof (exp) _rc;                      \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#endif


// packet type for packet_buffer
enum {
    TYPE_CTRL,
    TYPE_ACL,
};

typedef int (*hci_send_func)(void *data, size_t length);

struct packet_buffer {
    uint8_t    buffer[4096];

    // packet type (TYPE_CTRL or TYPE_ACL)
    int type;

    // file to read from
    int fd;

    // current offset for reading
    int read_offset;

    // for writing
    hci_send_func send_func;
};

static struct usb_device *device = NULL;
static struct usb_endpoint_descriptor *acl_in_ep = NULL;
static struct usb_endpoint_descriptor *acl_out_ep = NULL;
static struct usb_endpoint_descriptor *event_ep = NULL;

static int ctrl_pipe[2];
static int event_pipe[2];
static int acl_in_pipe[2];
static int acl_out_pipe[2];

static pthread_t read_thread_id;
static pthread_t write_thread_id;

static int device_added(const char *dev_name, void *client_data) {
    struct usb_device *dev = usb_device_open(dev_name);
    if (!dev) {
        fprintf(stderr, "can't open device %s: %s\n", dev_name, strerror(errno));
        return 0;
    }

    const struct usb_device_descriptor* desc = usb_device_get_device_descriptor(dev);
    if (desc->bDeviceClass == USB_CLASS_WIRELESS_CONTROLLER &&
        desc->bDeviceSubClass == 1 && desc->bDeviceProtocol == 1) {
        // got USB Bluetooth dongle
        fprintf(stderr, "Found USB bluetooth adapter at %s\n", usb_device_get_name(dev));
        fprintf(stderr, "%s %s\n", usb_device_get_manufacturer_name(dev), usb_device_get_product_name(dev));
        device = dev;
        return 1;
    } else {
        usb_device_close(dev);
        return 0;
    }
}

static int device_removed(const char *dev_name, void *client_data) {
    // FIXME - handle device disconnect
    return 0;
}


static int discovery_done(void *client_data) {
    return 1;
}

// initialize a packet_buffer
static void packet_buffer_init(struct packet_buffer* buffer, int type, int fd,
        hci_send_func send_func) {
     buffer->type = type;
     buffer->fd = fd;
     buffer->read_offset = 0;
     buffer->send_func = send_func;
}

// processes incoming data and into individual packets for writing
static int packet_buffer_read(struct packet_buffer* buffer) {
    int ret;
    int count = read(buffer->fd, buffer->buffer + buffer->read_offset,
            sizeof(buffer->buffer) - buffer->read_offset);
    if (count <= 0) {
        return -1;
    }
    buffer->read_offset += count;

    uint8_t* start = buffer->buffer;
    uint8_t* end = start + buffer->read_offset;

    // process all packets between start and end
    while (end > start) {
        int packet_length;
        if (buffer->type == TYPE_CTRL) {
            if (end - start < CTRL_HEADER_SIZE) {
                // not a complete header
                break;
            }
            packet_length = start[2] + CTRL_HEADER_SIZE;
        } else { // ACL
            if (end - start < ACL_HEADER_SIZE) {
                // not a complete header
                break;
            }
            packet_length = start[2] + ((int)start[3] << 8) + ACL_HEADER_SIZE;
        }
        if (packet_length > end - start) {
            // not a complete packet
            break;
        }

        ret = buffer->send_func(start, packet_length);
        if (ret < 0) {
            return -1;
        }
        start += packet_length;
    }

    // copy any remaining bytes to beginning of buffer
    int remaining = end - start;
    if (remaining > 0) {
        // in practice this does not seem to ever be necessary
        // if this log fires often, lets come back here and further optimize this.
        fprintf(stderr, "shifting remaining %d bytes to beginning of buffer\n", remaining);
        memmove(buffer->buffer, start, remaining);
    }
    buffer->read_offset = remaining;

    return 0;
}

static int ctrl_send(void *data, size_t length) {
    int ret = usb_device_control_transfer(device,
                    USB_TYPE_CLASS | USB_RECIP_DEVICE | USB_DIR_OUT,
                    0, 0, 0, data, length, 1000);
    if (ret < 0) {
        fprintf(stderr, "usb_device_control_transfer failed %d (%s)\n", ret, strerror(errno));
    }
    return ret;
}

static int acl_send(void *data, size_t length) {
    int ret = usb_device_bulk_transfer(device, acl_out_ep->bEndpointAddress, data, length, 1000);
    if (ret < 0) {
        fprintf(stderr, "usb_device_bulk_transfer failed %d (%s)\n", ret, strerror(errno));
    }
    return ret;
}

static void* write_thread(void* arg) {
    int ctrl_fd = ctrl_pipe[0];
    int acl_fd = acl_out_pipe[0];
    int ret;

    struct packet_buffer    ctrl_buffer;
    struct packet_buffer    acl_buffer;

    packet_buffer_init(&ctrl_buffer, TYPE_CTRL, ctrl_fd, ctrl_send);
    packet_buffer_init(&acl_buffer, TYPE_ACL, acl_fd, acl_send);

    while (1) {
        struct pollfd  fds[2];
        fds[0].fd = ctrl_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = acl_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        ret = TEMP_FAILURE_RETRY(poll(fds, sizeof(fds) / sizeof(*fds), -1));
        if (ret < 0) {
            fprintf(stderr, "poll failed\n");
           break;
        }

        if (fds[0].revents == POLLIN) {
            if (packet_buffer_read(&ctrl_buffer) < 0) {
                fprintf(stderr, "read ctrl_buffer failed (%s)\n", strerror(errno));
                break;
            }
        }

        if (fds[1].revents == POLLIN) {
            if (packet_buffer_read(&acl_buffer) < 0) {
                fprintf(stderr, "read acl_buffer failed (%s)\n", strerror(errno));
                break;
            }
        }
    }

    fprintf(stderr, "write_thread exit\n");
    return NULL;
}

static void* read_thread(void* arg) {
    struct usb_request* bulk_req = usb_request_new(device, acl_in_ep);
    struct usb_request* event_req1 = usb_request_new(device, event_ep);
    struct usb_request* event_req2 = usb_request_new(device, event_ep);
    struct usb_request* event_req3 = usb_request_new(device, event_ep);
    char bulk_buffer[512];
    char event_buffer1[512];
    char event_buffer2[512];
    char event_buffer3[512];
    struct usb_request* req;

    bulk_req->buffer = bulk_buffer;
    bulk_req->buffer_length = sizeof(bulk_buffer);
    event_req1->buffer = event_buffer1;
    event_req1->buffer_length = sizeof(event_buffer1);
    event_req2->buffer = event_buffer2;
    event_req2->buffer_length = sizeof(event_buffer2);
    event_req3->buffer = event_buffer3;
    event_req3->buffer_length = sizeof(event_buffer3);

    usb_request_queue(bulk_req);
    usb_request_queue(event_req1);
    usb_request_queue(event_req2);
    usb_request_queue(event_req3);

    while (1) {
        req = usb_request_wait(device);
        if (req) {
            if (req == bulk_req) {
                //printf("bulk in size %d\n", req->actual_length);
                write(acl_in_pipe[1], req->buffer, req->actual_length);
            } else {
                //printf("event size %d\n", req->actual_length);
                write(event_pipe[1], req->buffer, req->actual_length);
            }

            usb_request_queue(req);
        }
    }

    usb_request_free(bulk_req);
    usb_request_free(event_req1);
    usb_request_free(event_req2);
    usb_request_free(event_req3);

    fprintf(stderr, "read_thread exit\n");
    return NULL;
}

int usb_serial_open(void* param) {
    int (*fd_array)[] = (int (*) []) param;

    if (pipe(ctrl_pipe) || pipe(event_pipe) || pipe(acl_in_pipe) || pipe(acl_out_pipe)) {
        fprintf(stderr, "pipe failed (%s)\n", strerror(errno));
        return -1;
    }

    (*fd_array)[CH_CMD] = ctrl_pipe[1];
    (*fd_array)[CH_EVT] = event_pipe[0];
    (*fd_array)[CH_ACL_OUT] = acl_out_pipe[1];
    (*fd_array)[CH_ACL_IN] = acl_in_pipe[0];

    pthread_create(&read_thread_id, NULL, read_thread, NULL);
    pthread_create(&write_thread_id, NULL, write_thread, NULL);

    return CH_MAX;
}

int usb_serial_close() {

    close(ctrl_pipe[0]);
    close(ctrl_pipe[1]);
    close(event_pipe[0]);
    close(event_pipe[1]);
    close(acl_in_pipe[0]);
    close(acl_in_pipe[1]);
    close(acl_out_pipe[0]);
    close(acl_out_pipe[1]);

    pthread_join(read_thread_id, NULL);
    pthread_join(write_thread_id, NULL);
    return 0;
}

int init_usb()
{
    struct usb_host_context *ctx;

    ctx = usb_host_init();
    if (!ctx) {
        perror("usb_host_init:");
        return 1;
    }

    usb_host_run(ctx,
                 device_added,
                 device_removed,
                 discovery_done,
                 NULL);

    if (device) {
        struct usb_descriptor_iter iter;
        struct usb_descriptor_header *desc;

        usb_descriptor_iter_init(device, &iter);
         struct usb_interface_descriptor* bt_interface = NULL;

        while ((desc = usb_descriptor_iter_next(&iter)) != NULL) {
            if (desc->bDescriptorType == USB_DT_INTERFACE) {
                struct usb_interface_descriptor* intf = (struct usb_interface_descriptor *)desc;
                if (intf->bInterfaceClass == USB_CLASS_WIRELESS_CONTROLLER &&
                    intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 1) {
                    bt_interface = intf;
                } else {
                    bt_interface = NULL;
                }
            }

            if (bt_interface && desc->bDescriptorType == USB_DT_ENDPOINT) {
                struct usb_endpoint_descriptor* ep_desc = (struct usb_endpoint_descriptor *)desc;
                int xferType = ep_desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
                int direction = ep_desc->bEndpointAddress & USB_ENDPOINT_DIR_MASK;

                if (xferType == USB_ENDPOINT_XFER_INT) {
                    event_ep = ep_desc;
                } else if (xferType == USB_ENDPOINT_XFER_BULK) {
                    if (direction == USB_DIR_IN) {
                        acl_in_ep = ep_desc;
                    } else {
                        acl_out_ep = ep_desc;
                    }
                }
            }

            if (acl_in_ep && acl_out_ep && event_ep) break;
        }
    }

    if (!acl_in_ep || !acl_out_ep || !event_ep) {
        fprintf(stderr, "could not find endpoints\n");
        return -1;
    }

    if (usb_device_claim_interface(device, 0)) {
        usb_device_connect_kernel_driver(device, 0, 0);

        if (usb_device_claim_interface(device, 0)) {
            fprintf(stderr, "could not claim interface (%s)\n", strerror(errno));
            return -1;
        }
    }

    return 0;
}
