/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/uhid.h>

#include "log.h"
#include "btleHid.h"
#include "uhid.h"


// Refer to include/uapi/linux/hid.h in kernel.
#define HID_MAX_DESCRIPTOR_SIZE 4096
#define MAX_DEVICE_NAME_SIZE 128

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

struct UhidDeviceInfo {
    int fd;
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

static struct UhidDeviceInfo *mDevInfo = NULL;


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

    ev_size = sizeof(*ev);
    while (1) {
        ret = write(fd, ev, ev_size);
        logd("ret of write: %d (ev size: %d)\n", ret, ev_size);

        if (ret < 0) {
            if (errno == EINTR) {
                loge("Error in writing to uhid due to EINTR. Retry.\n");
                continue;
            }
            loge("Error in writing to uhid: %d\n", -errno);
            return -errno;
        }
        else if (ret != ev_size) {
            loge("Wrong size written to uhid: actual=%d expected=%d\n", ret, ev_size);
            return -EFAULT;
        }
        else
            return 0;
    }
}

/*
 * FUNCTION: createDevice
 * USE:      Create the uhid device in kernel
 * PARAMS:   devInfo - device data
 * RETURN:   0 on success or an error code
 * NOTES:    Do not use UHID_CREATE2 since some older kernels do not support it.
 */
static int createDevice(const struct UhidDeviceInfo *devInfo)
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
 * FUNCTION: destroyDevice
 * USE:      Destroy the uhid device in kernel
 * PARAMS:   devInfo - device data
 * RETURN:   0 on success or an error code
 * NOTES:
 */
static int destroyDevice(const struct UhidDeviceInfo *devInfo)
{
    struct uhid_event ev = {.type = UHID_DESTROY};

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
static bool parseReportDescriptor(const uint8_t *rdData, uint32_t rdSize, bool *reportIdPresent)
{
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

/*
 * FUNCTION: extractBtleUhidDeviceInfo
 * USE:      Extract the device data for the connection
 * PARAMS:   hidId - HID device handle
 *           devInfo - device data
 * RETURN:   true on success
 * NOTES:
 */
static bool extractBtleUhidDeviceInfo(ble_hid_conn_t hidId, struct UhidDeviceInfo *devInfo)
{
    uint32_t rdSize;
    uint8_t *rdData;
    bool reportIdPresent;

    logd("Extract device data.\n");
    devInfo->bus = BUS_USB;
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

    // TODO: Extract these data from remote device.
    strncpy((char *)devInfo->name, (char *)"newblue-uhid-device", MAX_DEVICE_NAME_SIZE);
    devInfo->vendor = 0x00E0;   // Google
    devInfo->product = 0x0000;
    return true;
}

/*
 * FUNCTION: hidConnStateCbk
 * USE:      Create a uhid device in kernel when a connection is up and
 *           destroy the device when the connection is terminated
 * PARAMS:   hidId - HID device handle
 *           state - the connection state; only cares about
 *                   BTLE_HID_CONN_STATE_UP and
 *                   BTLE_HID_CONN_STATE_TEARDOWN
 * RETURN:
 * NOTES:
 */
void hidConnStateCbk(ble_hid_conn_t hidId, uint8_t state)
{
    int fd;
    int ret = 0;

    logi("HID state: %u\n", state);

    if (state == BTLE_HID_CONN_STATE_UP) {
        const char *path = "/dev/uhid";

        logi("Open uhid-cdev %s\n", path);

        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            loge("Cannot open uhid-cdev %s\n", path);
            return;
        }

        mDevInfo = calloc(sizeof(struct UhidDeviceInfo), 1);
        if (!mDevInfo) {
            loge("Failed to allocate memory for UhidDeviceInfo.\n");
            close(fd);
            return;
        }
        mDevInfo->fd = fd;

        // Get device data from this connection.
        if (!extractBtleUhidDeviceInfo(hidId, mDevInfo)) {
            loge("Failed to extract btle device data.\n");
            goto out;
        }

        ret = createDevice(mDevInfo);
        if (ret) {
            loge("Failed to create uhid device.\n");
            goto out;
        }
        logi("Created uhid device\n");
        return;

    }
    else if (state == BTLE_HID_CONN_STATE_TEARDOWN) {
        if (!mDevInfo) {
            loge("To destroy a BLE HID device before it is created.");
            return;
        }

        ret = destroyDevice(mDevInfo);
        if (ret)
            loge("Error in destroying the device: %d\n", ret);
        else
            logi("Destroyed uhid device\n");
    }
    else {
        logd("uhid does not handle this state: %d\n", state);
        return;
    }

out:
    free(mDevInfo->rdData);
    free(mDevInfo);
    mDevInfo = NULL;
    close(mDevInfo->fd);
}

/*
 * FUNCTION: hidReportRxCbk
 * USE:      Send a report event to kernel
 * PARAMS:   hidId - HID device handle
 *           reportId - the report we read
 *           data - data that were read
 *           byRequest - is this operation performed by request?
 * RETURN:
 * NOTES:
 */
void hidReportRxCbk(ble_hid_conn_t hidId, int32_t reportId, sg data, bool byRequest)
{
    // TODO: Every connection should have its own device data. Assume only one connection for now.
    struct uhid_event ev = {.type = UHID_INPUT};
    uint32_t sgLen;
    uint32_t max_size;

    logi("HID report ID %d RXed, %u bytes%s\n", reportId, sgLength(data), byRequest ? " by request" : "");

    if (!mDevInfo) {
        loge("Recived the HID report before a valid uhid device is created. Ignored.\n");
        goto out;
    }

    sgLen = sgLength(data);
    // If the report map contains report ID(s), we need to prepend the report ID
    // to the report data when sending to kernel. In this case, the max_size for
    // actual report data would be decreased by one.
    max_size = mDevInfo->reportIdPresent ? HID_MAX_DESCRIPTOR_SIZE - 1 : HID_MAX_DESCRIPTOR_SIZE;
    if (sgLen > max_size) {
        loge("HID pdu data is too long (%d).\n", sgLen);
        goto out;
    }

    // Prepend the report ID to the report data if the report ID(s) exists.
    if (mDevInfo->reportIdPresent) {
      ev.u.input.data[0] = reportId;
      ev.u.input.size = sgLen + 1;
      sgSerialize(data, 0, sgLen, ev.u.input.data + 1);
    }
    else {
      ev.u.input.size = sgLen;
      sgSerialize(data, 0, sgLen, ev.u.input.data);
    }

    uhidWrite(mDevInfo->fd, &ev);

out:
    sgFree(data);
}
