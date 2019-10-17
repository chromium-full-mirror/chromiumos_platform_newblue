/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/uhid.h>

#include "log.h"
#include "btleHid.h"
#include "uhid.h"
#include "mt.h"


// Refer to include/uapi/linux/hid.h in kernel.
#define HID_MAX_DESCRIPTOR_SIZE 4096

// Define report descriptor items.
// Refer to http://www.usb.org/developers/hidpage/HID1_11.pdf
#define COLLECTION 0xa0
#define END_COLLECTION 0xc0
#define REPORT_ID 0x84
#define USB_HID_LONG_ITEM_TYPE 0xfe
#define USB_HID_SHORT_ITEM_BSIZE_MASK 0x03
#define USB_HID_SHORT_ITEM_PREFIX_MASK 0xfc
#define USB_HID_SHORT_ITEM_HEADER_SIZE 1
#define USB_HID_LONG_ITEM_HEADER_SIZE 3

#define DEV_UHID_PATH "/dev/uhid" /* The device node to support user-space transport. */
#define MAX_SUPPORTED_HID_CONNECTIONS 20     /* The value should not exceed RLIMIT_NOFILE, i.e., 1024 */

struct UhidDeviceInfo {
    struct UhidDeviceInfo *next;

    int fd;
    ble_hid_conn_t hidId;
    uint8_t state;  /* indicating the device state such as UP or TEARDOWN */
    uint8_t reportIdPresent;
    uint8_t name[MAX_DEVICE_NAME_SIZE];
    uint16_t bus;
    uint32_t vendor;
    uint32_t product;
    uint32_t version;
    uint32_t country;
    uint8_t flags;
    uint8_t *rdData;
    uint16_t rdSize;
};

struct UhidDevices {
    int control_pipe[2];

    /* pfds[0]: reserved for the read fd of the control pipe */
    struct pollfd pfds[MAX_SUPPORTED_HID_CONNECTIONS + 1];

    uint8_t numDevices;
    struct UhidDeviceInfo *head;
    struct UhidDeviceInfo *tail;
};

static pthread_mutex_t mDevLock = PTHREAD_MUTEX_INITIALIZER;
static struct UhidDevices *mDevices = NULL;                            /* the uhid devices */

pthread_t mOutputReportThread;                                         /* the thread polling output report events */

/* fwd decls */
static void hidDeinit(void);


/*
 * FUNCTION: uhidWrite
 * USE:      Write hid event data to kernel
 * PARAMS:   fd - the file descriptor of /dev/uhid
 * RETURN:   0 on success or an error code
 * NOTES:
 */
static int uhidWrite(int fd, const struct uhid_event *ev)
{
    size_t ev_size;
    int ret;

    if (fd <= 0) {
        loge("fd to write is invalid: %d\n", fd);
        return -EBADR;
    }

    ev_size = sizeof(*ev);
    while (1) {
        ret = write(fd, ev, ev_size);
        logv("ret of write: %d (ev size: %zu)\n", ret, ev_size);

        if (ret < 0) {
            if (errno == EINTR) {
                loge("Error in writing to uhid due to EINTR. Retry.\n");
                continue;
            }
            loge("Error in writing to uhid: %d\n", -errno);
            return -errno;
        }
        else if (ret != ev_size) {
            loge("Wrong size written to uhid: actual=%d expected=%zu\n", ret, ev_size);
            return -EFAULT;
        }
        else
            return 0;
    }
}

/*
 * FUNCTION: createDeviceInKernel
 * USE:      Create the uhid device in kernel
 * PARAMS:   devInfo - device data
 * RETURN:   0 on success or an error code
 * NOTES:    Do not use UHID_CREATE2 since some older kernels do not support it.
 */
static int createDeviceInKernel(const struct UhidDeviceInfo *devInfo)
{
    struct uhid_event ev = {.type = UHID_CREATE};

    strncpy((char *)ev.u.create.name, (char *)devInfo->name, MAX_DEVICE_NAME_SIZE);
    ev.u.create.rd_data = devInfo->rdData;
    ev.u.create.rd_size = devInfo->rdSize;
    ev.u.create.bus = devInfo->bus;
    ev.u.create.vendor = devInfo->vendor;
    ev.u.create.product = devInfo->product;
    ev.u.create.version = devInfo->version;
    ev.u.create.country = devInfo->country;

    return uhidWrite(devInfo->fd, &ev);
}

/*
 * FUNCTION: destroyDeviceInKernel
 * USE:      Destroy the uhid device in kernel
 * PARAMS:   devInfo - device data
 * RETURN:   0 on success or an error code
 * NOTES:
 */
static int destroyDeviceInKernel(struct UhidDeviceInfo *devInfo)
{
    struct uhid_event ev = {.type = UHID_DESTROY};

    if (!devInfo)
        return -EFAULT;

    devInfo->state = BTLE_HID_CONN_STATE_TEARDOWN;

    return uhidWrite(devInfo->fd, &ev);
}

/*
 * FUNCTION: parseReportDescriptor
 * USE:      parse the report descriptor for sanity check and determine if
 *           report IDs are present
 * PARAMS:   rdData - report descriptor data
 *           rdSize - length of the report descriptor
 *           reportIdPresent - indicating if report IDs are present
 * RETURN:   true if the report descriptor passes sanity check
 * NOTES:    Refer to "USB Device Class Definition for Human Interface Devices"
 *           http://www.usb.org/developers/hidpage/HID1_11.pdf
 */
static bool parseReportDescriptor(const uint8_t *rdData,
        const uint32_t rdSize,
        bool *reportIdPresent) {
    uint8_t bSize;
    uint8_t prefix;
    int collectionDepth = 0;
    uint16_t i;

    if (!rdData) {
        loge("Null report map.\n");
        return false;
    }

    if (rdSize <= 0) {
        loge("Empty report map (size: %d).\n", rdSize);
        return false;
    }

    *reportIdPresent = false;
    for (i = 0; i < rdSize;) {
        /* There are two item types: long item and short item.
         * Most USB HID items are short items.
         * The index i will always point to next item header.
         * The length of every item is dynamic. Hence, it is required
         * to increase the index i by every item length in the for loop.
         */
        if (rdData[i] == USB_HID_LONG_ITEM_TYPE) {
            // Increase the index by the long item data size plus the long item header size.
            if (i + 1 >= rdSize) {
              loge("report map: out of range at position: %d\n", i + 1);
              return false;
            }
            i += rdData[i + 1] + USB_HID_LONG_ITEM_HEADER_SIZE;
        }
        else {
            bSize = rdData[i] & USB_HID_SHORT_ITEM_BSIZE_MASK;
            /* bSize:
             * 0: data dize is 0 byte
             * 1: data dize is 1 byte
             * 2: data dize is 2 bytes
             * 3: data dize is 4 bytes
             */
            if (bSize == 3)
                bSize = 4;

            prefix = rdData[i] & USB_HID_SHORT_ITEM_PREFIX_MASK;
            switch (prefix) {
            case REPORT_ID:
                if (i + 1 >= rdSize) {
                  loge("report map: out of range at position: %d\n", i + 1);
                  return false;
                }
                *reportIdPresent = true;
                logi("    Report ID found: %d\n", rdData[i + 1]);
                break;
            case COLLECTION:
                logi("    COLLECTION found\n");
                ++collectionDepth;
                break;
            case END_COLLECTION:
                logi("    END_COLLECTION found\n");
                if (--collectionDepth < 0) {
                    loge("report map: COLLECTION is closed before it is opened.\n");
                    return false;
                }
                break;
            }

            // Increase the index by the short item data size plus the short item header size.
            i += bSize + USB_HID_SHORT_ITEM_HEADER_SIZE;
            // Note: "i == rdSize" indicates that the report map is parsed completely.
            // However, "i > rdSize" indicates that the report map is malformed.
            if (i > rdSize) {
                loge("report map: out of range at position: %d\n", i);
                return false;
            }
        }
    }

    if (collectionDepth > 0) {
        loge("report map: COLLECTION is opened without being closed.");
        return false;
    }

    return true;
}

bool parseReportDescriptorForTest(const uint8_t* rdData,
    const uint32_t rdSize,
    bool* reportIdPresent) {
    return parseReportDescriptor(rdData, rdSize, reportIdPresent);
}

/*
 * FUNCTION: extractBtleUhidDeviceInfo
 * USE:      Extract the device data for the connection
 * PARAMS:   devInfo - device data
 *           fd - file descriptor of the associated uhid device
 *           hidId - HID connection id
 * RETURN:   true on success
 * NOTES:
 */
static bool extractBtleUhidDeviceInfo(struct UhidDeviceInfo *devInfo, int fd, ble_hid_conn_t hidId)
{
    uint32_t rdSize;
    uint8_t *rdData;
    bool reportIdPresent;

    logd("Extract device data.\n");
    devInfo->fd = fd;
    devInfo->hidId = hidId;
    devInfo->state = BTLE_HID_CONN_STATE_UP;
    devInfo->bus = BUS_USB;
    devInfo->next = NULL;
    btleHidGetHidName(hidId, (char*) devInfo->name);
    btleHidGetHidInfo(hidId, (uint16_t *)&devInfo->version, (uint8_t *)&devInfo->country, &devInfo->flags);
    btleHidGetReportDescriptors(hidId, (const void **)&rdData, &rdSize);

    logd("Size of report map: %d\n", rdSize);

    if (!parseReportDescriptor(rdData, rdSize, &reportIdPresent)) {
        loge("Error in parsing the report map.\n");
        return false;
    }

    // Make a local copy of the report map in case that a device might go away any time.
    devInfo->rdData = calloc(rdSize, sizeof(uint8_t));
    if (!devInfo->rdData) {
        loge("Error in calloc for rd data.\n");
        return false;
    }
    memcpy((void *)devInfo->rdData, (void *)rdData, rdSize);
    devInfo->rdSize = (uint16_t)rdSize;

    devInfo->reportIdPresent = reportIdPresent;

    // TODO(b:139126336): Extract Product Related Information from HID Descriptor
    devInfo->vendor = 0x00E0;   // Google
    devInfo->product = 0x0000;
    return true;
}

/*
 * FUNCTION: uhidFindDeviceById
 * USE:      Find device by HID connection id
 * PARAMS:   hidId - HID connection id
 * RETURN:   the device
 * NOTES:
 */
static struct UhidDeviceInfo *uhidFindDeviceById(ble_hid_conn_t hidId) {
    struct UhidDeviceInfo *d;

    if (!mDevices) {
        logd("mDevices is not initialized yet.\n");
        return NULL;
    }

    for (d = mDevices->head; d && d->hidId != hidId; d = d->next);
    return d;
}

/*
 * FUNCTION: uhidFindDeviceByFd
 * USE:      Find device by the device fd
 * PARAMS:   fd - the file descriptor of /dev/uhid
 * RETURN:   the device
 * NOTES:
 */
static struct UhidDeviceInfo *uhidFindDeviceByFd(int fd) {
    struct UhidDeviceInfo *d;

    if (!mDevices) {
        logd("mDevices is not initialized yet.\n");
        return NULL;
    }

    for (d = mDevices->head; d && d->fd != fd; d = d->next);
    return d;
}

/*
 * FUNCTION: printmDevices
 * USE:      print the fd's of devices
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void printmDevices() {
    if (!mDevices)
        return;

    logd("mDevices has %d devices\n", mDevices->numDevices);

    for (struct UhidDeviceInfo *d = mDevices->head; d; d = d->next)
        logd("  %d\n", d->fd);

    for (int i = 0; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++) {
        if (mDevices->pfds[i].fd >= 0)
            logd("  pfds[%d].fd %d\n", i, mDevices->pfds[i].fd);
    }
}

/*
 * FUNCTION: uhidDeviceAppend
 * USE:      append a device to mDevices
 * PARAMS:   devInfo - device data
 * RETURN:   true on success
 * NOTES:    call with mDevLock held.
 */
static bool uhidDeviceAppend(struct UhidDeviceInfo *devInfo) {
    if (!mDevices) {
        loge("mDevices is not initialized yet.\n");
        return false;
    }

    if (mDevices->numDevices >= MAX_SUPPORTED_HID_CONNECTIONS) {
        loge("Rejected because max number of hid connections are reached.\n");
        return false;
    }

    devInfo->next = NULL;
    if (mDevices->tail)
        mDevices->tail->next = devInfo;
    else
        mDevices->head = devInfo;
    mDevices->tail = devInfo;
    mDevices->numDevices++;
    printmDevices();

    return true;
}

/*
 * FUNCTION: freeDeviceInfo
 * USE:      free device data
 * PARAMS:   devInfo - device data
 * RETURN:   NONE
 * NOTES:
 */
static void freeDeviceInfo(struct UhidDeviceInfo *devInfo) {
    if (devInfo) {
        free(devInfo->rdData);
        free(devInfo);
    }
}

/*
 * FUNCTION: uhidDeviceDel
 * USE:      delete a device from mDevices
 * PARAMS:   hidId - HID connection id
 * RETURN:   true on success
 * NOTES:
 */
static bool uhidDeviceDel(ble_hid_conn_t hidId) {
    struct UhidDeviceInfo *p = NULL;
    struct UhidDeviceInfo *d = NULL;

    if (!mDevices) {
        loge("mDevices is NULL.\n");
        return false;
    }

    d = mDevices->head;
    while (d && d->hidId != hidId) {
        p = d;
        d = d->next;
    }

    if (d) {
        if (p)
            p->next = d->next;
        else
            mDevices->head = d->next;

        if (!d->next)
            mDevices->tail = p;

        mDevices->numDevices--;
    }
    logd("Removed the device with conn id %" PRIu64 " from mDevices.\n", hidId);

    return true;
}

/*
 * FUNCTION: sendOutputReport
 * USE:      send output report through Gatt client
 * PARAMS:   devInfo - device data
 *           ev - a uhid event received from kernel
 * RETURN:   true on success
 * NOTES:
 */
static bool sendOutputReport(const struct UhidDeviceInfo *devInfo, struct uhid_event *ev) {
    sg reportData;
    int32_t reportId;
    bool ret;

    if (ev->u.output.rtype != UHID_OUTPUT_REPORT) {
        logw("The event is not expected type: UHID_OUTPUT_REPORT.\n");
        return false;
    }

    /* The output data consists of report ID in data[0] and report data in the remaining. */
    reportId = ev->u.output.data[0];
    reportData = sgNewWithCopyData(&ev->u.output.data[1], ev->u.output.size - sizeof(ev->u.output.data[0]));
    if (!reportData) {
        loge("Failed to construct an sg for output report data.\n");
        return false;
    }

    ret = btleHidWriteReport(devInfo->hidId, reportId, reportData);
    if (!ret)
        sgFree(reportData);

    return ret;
}

/*
 * FUNCTION: closeDevInfo
 * USE:      close a device
 * PARAMS:   devInfo - device data
 * RETURN:   NONE
 * NOTES:
 */
static void closeDevInfo(struct UhidDeviceInfo *devInfo) {
    if (!devInfo)
        return;

    if (!uhidDeviceDel(devInfo->hidId))
        logw("Cannot find HID conn id %" PRIu64 " to delete.\n", devInfo->hidId);

    for (int i = 1; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++) {
        if (mDevices->pfds[i].fd == devInfo->fd) {
            close(mDevices->pfds[i].fd);
            mDevices->pfds[i].fd = -1;
            break;
        }
    }
    freeDeviceInfo(devInfo);

    if (mDevices->numDevices <= 0)
        hidDeinit();

    printmDevices();
}

/*
 * FUNCTION: handleUhidOutputEvent
 * USE:      read output events and send output reports accordingly
 * PARAMS:   devInfo - device data
 * RETURN:   0 on success or an error code
 * NOTES:
 */
static int handleUhidOutputEvent(struct UhidDeviceInfo *devInfo)
{
    struct uhid_event ev = {0,};
    ssize_t ret;

    if (!devInfo) {
        loge("No devInfo found.\n");
        return -EFAULT;
    }

    ret = read(devInfo->fd, &ev, sizeof(ev));
    if (ret == 0) {
        loge("Read HUP on uhid fd %d\n", devInfo->fd);
        return -EFAULT;
    }

    switch (ev.type) {
    case UHID_START:
        logd("ev type: UHID_START %d. The HID device is started.\n", ev.type);
        break;
    case UHID_OPEN:
        logd("ev type: UHID_OPEN %d. The HID device is opened.\n", ev.type);
        break;
    case UHID_CLOSE:
        if (devInfo->state == BTLE_HID_CONN_STATE_TEARDOWN) {
            logd("ev type: UHID_CLOSE %d. The HID device is closed.\n", ev.type);
            closeDevInfo((struct UhidDeviceInfo *)devInfo);
        } else {
            logd("ev type: UHID_CLOSE %d. Ignored.\n", ev.type);
        }
        break;
    case UHID_OUTPUT:
        logv("ev type: UHID_OUTPUT %d\n", ev.type);
        sendOutputReport(devInfo, &ev);
        break;
    default:
        logd("ev type %d is not expected\n", ev.type);
    }

    return 0;
}

/*
 * FUNCTION: closeAllDevices
 * USE:      close all devices
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void closeAllDevices(void) {
    if (mDevices) {
        logd("Closing all devices...\n");

        // Close any fd that is still open.
        for (int i = 1; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++) {
            if (mDevices->pfds[i].fd > 0)
                close(mDevices->pfds[i].fd);
        }

        close(mDevices->control_pipe[0]);
        free(mDevices);
        mDevices = NULL;
    }
}

/*
 * FUNCTION: outputReportPoller
 * USE:      poll file descriptors and handle the received events
 * PARAMS:   unused - unused
 * RETURN:   void* - unused
 * NOTES:
 */
static void* outputReportPoller(void *unused) {
    logd("Thread outputReportPoller is created.\n");
    while (1) {
        int ret = poll(mDevices->pfds, MAX_SUPPORTED_HID_CONNECTIONS + 1, 1000);
        if (ret < 0) {
            loge("Poll failed.\n");
        } else if (ret == 0) {
            continue;
        } else {
            for (int i = 1; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++) {
                if (mDevices->pfds[i].revents & POLLIN) {
                    logd("POLLIN for the device[%d].fd %u\n", i, mDevices->pfds[i].fd);
                    handleUhidOutputEvent(uhidFindDeviceByFd(mDevices->pfds[i].fd));
                }
            }

            if (mDevices->pfds[1].revents & POLLHUP)
                loge("Received unexpected POLLHUP from pfd 1. Ignored.\n");

            if (mDevices->pfds[0].revents & POLLHUP) {
                logi("Received POLLHUP from pfds 0. Exiting.\n");
                closeAllDevices();
                break;
            }
        }
    }

    logi("Thread outputReportPoller Exiting.\n");
    return NULL;
}

/*
 * FUNCTION: hidInit
 * USE:      initialize structures for uhid devices
 * PARAMS:   NONE
 * RETURN:   true on success
 * NOTES:
 */
static bool hidInit(void) {
    if (mDevices)
        return true;

    mDevices = calloc(sizeof(struct UhidDevices), 1);
    if (!mDevices) {
        loge("OOM in allocating memory for UhidDevices.\n");
        goto out;
    }

    /* The control pipe is used to terminate mOutputReportThread.*/
    if (pipe(mDevices->control_pipe) < 0) {
        loge("Failed to create a control pipe.\n");
        goto out_pipe;
    }

    mDevices->pfds[0].fd = mDevices->control_pipe[0];
    for (int i = 1; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++)
        mDevices->pfds[i].fd = -1;

    if (pthread_create(&mOutputReportThread, NULL, outputReportPoller, NULL)) {
        loge("Failed to create mOutputReportThread.\n");
        goto out_pthread;
    }

    mDevices->numDevices = 0;
    mDevices->head = NULL;
    mDevices->tail = NULL;

    logd("hidInit done.\n");

    return true;

out_pthread:
    close(mDevices->control_pipe[0]);
    close(mDevices->control_pipe[1]);

out_pipe:
    free(mDevices);
    mDevices = NULL;

out:
    return false;
}

/*
 * FUNCTION: createDevInfo
 * USE:      create device data info
 * PARAMS:   hidId - HID connection id
 * RETURN:   the device
 * NOTES:
 */
static struct UhidDeviceInfo* createDevInfo(ble_hid_conn_t hidId) {
    int fd;
    struct UhidDeviceInfo *devInfo;

    if (!mDevices) {
        loge("mDevices is NULL.\n");
        goto out;
    }

    if (mDevices->numDevices >= MAX_SUPPORTED_HID_CONNECTIONS) {
        loge("HID connections are full.\n");
        goto out;
    }

    fd = open(DEV_UHID_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        loge("Cannot open uhid-cdev %s\n", DEV_UHID_PATH);
        goto out;
    }
    logi("Open uhid-cdev %s with fd %d\n", DEV_UHID_PATH, fd);

    devInfo = calloc(1, sizeof(struct UhidDeviceInfo));
    if (!devInfo) {
        loge("OOM in allocating UhidDeviceInfo\n");
        goto out_fd;
    }

    // Get device data from this connection.
    if (!extractBtleUhidDeviceInfo(devInfo, fd, hidId)) {
        loge("Failed to extract btle device data.\n");
        goto out_devinfo;
    }

    for (int i = 1; i <= MAX_SUPPORTED_HID_CONNECTIONS; i++) {
        if (mDevices->pfds[i].fd < 0) {
            mDevices->pfds[i].fd = fd;
            mDevices->pfds[i].events = POLLIN;
            logd("devInfo created for pfds[%d] id %" PRIu64 " fd %d.\n", i, hidId, fd);
            return devInfo;
        }
    }

    loge("Unexpected error: no available pfds slot is found.\n");

out_devinfo:
    free(devInfo);

out_fd:
    close(fd);

out:
    return NULL;
}

/*
 * FUNCTION: hidDeinit
 * USE:      close pipe and file descriptors and then free devices
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void hidDeinit(void) {
    if (!mDevices)
        return;

    /* Close the control_pipe so that mOutputReportThread exits with POLLHUP. */
    close(mDevices->control_pipe[1]);
}

/*
 * FUNCTION: hidConnStateCbk
 * USE:      Create a uhid device in kernel when a connection is up and
 *           destroy the device when the connection is terminated
 * PARAMS:   hidId - HID connection id
 *           state - the connection state; only cares about
 *                   BTLE_HID_CONN_STATE_UP and
 *                   BTLE_HID_CONN_STATE_TEARDOWN
 * RETURN:   NONE
 * NOTES:
 */
void hidConnStateCbk(ble_hid_conn_t hidId, uint8_t state)
{
    int ret;
    struct UhidDeviceInfo *devInfo;

    logi("HID state: %u, hidId: %" PRIu64 "\n", state, hidId);

    pthread_mutex_lock(&mDevLock);
    if (state == BTLE_HID_CONN_STATE_UP) {
        if (uhidFindDeviceById(hidId)) {
            loge("A device with the same conn Id %" PRIu64 " already exists.\n", hidId);
            goto out;
        }

        if (!mDevices) {
            if (!hidInit()) {
                loge("Failed to initialize the uhid devices.\n");
                goto out;
            }
            logi("Initialized the uhid devices.\n");
        }

        devInfo = createDevInfo(hidId);
        if (!devInfo) {
            loge("Failed to create devInfo for that hidId %" PRIu64 ".\n", hidId);
            goto out;
        }

        if (!uhidDeviceAppend(devInfo)) {
            loge("Failure to append the device.\n");
            goto out_device;
        }

        ret = createDeviceInKernel(devInfo);
        if (ret) {
            loge("Failed to create uhid device: %d.\n", ret);
            goto del_device;
        }

        logi("Created a uhid device. %u devices in total.\n", mDevices->numDevices);
        goto out;
    }
    else if (state == BTLE_HID_CONN_STATE_TEARDOWN) {
        if (!mDevices) {
            loge("To destroy a BLE HID device before it is created.");
            goto out;
        }

        devInfo = uhidFindDeviceById(hidId);
        if (!devInfo) {
            loge("Failed to find the device with that hidId %" PRIu64 ".\n", hidId);
            goto out;
        }

        ret = destroyDeviceInKernel(devInfo);
        if (ret)
            loge("Error in destroying the device: %d\n", ret);

        goto out;
    }
    else {
        logd("uhid does not handle this state: %d\n", state);
        goto out;
    }

del_device:
    uhidDeviceDel(devInfo->hidId);

out_device:
    freeDeviceInfo(devInfo);

out:
    pthread_mutex_unlock(&mDevLock);
}

/*
 * FUNCTION: hidReportRxCbk
 * USE:      Send a report event to kernel
 * PARAMS:   hidId - HID connection id
 *           reportId - the report we read
 *           data - data that were read
 *           byRequest - is this operation performed by request?
 * RETURN:   NONE
 * NOTES:
 */
void hidReportRxCbk(ble_hid_conn_t hidId, int32_t reportId, sg data, bool byRequest)
{
    // TODO: Every connection should have its own device data. Assume only one connection for now.
    struct uhid_event ev = {.type = UHID_INPUT};
    uint32_t sgLen;
    uint32_t max_size;
    struct UhidDeviceInfo *devInfo;

    logv("HID conn id %" PRIu64 " report ID %d RXed, %u bytes%s\n", hidId, reportId, sgLength(data), byRequest ? " by request" : "");

    if (!mDevices) {
        loge("Recived the HID report before a valid uhid device is created. Ignored.\n");
        goto out;
    }

    devInfo = uhidFindDeviceById(hidId);
    if (!devInfo) {
        loge("Failed to find the device with that hidId %" PRIu64 ".\n", hidId);
        goto out;
    }

    sgLen = sgLength(data);
    // If the report map contains report ID(s), we need to prepend the report ID
    // to the report data when sending to kernel. In this case, the max_size for
    // actual report data would be decreased by one.
    max_size = devInfo->reportIdPresent ? HID_MAX_DESCRIPTOR_SIZE - 1 : HID_MAX_DESCRIPTOR_SIZE;
    if (sgLen > max_size) {
        loge("HID pdu data is too long (%d).\n", sgLen);
        goto out;
    }

    // Prepend the report ID to the report data if the report ID(s) exists.
    if (devInfo->reportIdPresent) {
      ev.u.input.data[0] = reportId;
      ev.u.input.size = sgLen + 1;
      sgSerialize(data, 0, sgLen, ev.u.input.data + 1);
    }
    else {
      ev.u.input.size = sgLen;
      sgSerialize(data, 0, sgLen, ev.u.input.data);
    }

    uhidWrite(devInfo->fd, &ev);

out:
    sgFree(data);
}
