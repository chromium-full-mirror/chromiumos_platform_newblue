/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _BTLEHID_H_
#define _BTLEHID_H_

#include "gatt.h"
#include "uuid.h"
#include "uuid.h"

NEWBLUE_BEGIN_DECLS

// Refer to include/uapi/linux/hid.h in kernel.
#define MAX_DEVICE_NAME_SIZE 128

//uuid of service
#define BTLE_UUID_HID_SERVICE                          0x1812  // org.bluetooth.service.human_interface_device


typedef uniq_t ble_hid_conn_t;

//normal states
#define BTLE_HID_CONN_STATE_INVALID                    0x00 // should never be seen
#define BTLE_HID_CONN_STATE_GATT_DISCOVERY             0x01 // looking for main HID service & enumerating its insides
#define BTLE_HID_CONN_STATE_READING                    0x02 // reading required things
#define BTLE_HID_CONN_STATE_WRITING_CCCDS              0x03 // writing CCCDs as needed
#define BTLE_HID_CONN_STATE_UP                         0x04 // ready to be used
#define BTLE_HID_CONN_STATE_TEARDOWN                   0x05 // going down

#define BTLE_HID_REPORT_ID_BOOT_KEYBOARD_IN            -1  //fake report ID for boot mode keyboard input
#define BTLE_HID_REPORT_ID_BOOT_KEYBOARD_OUT           -2  //fake report ID for boot mode keyboard output
#define BTLE_HID_REPORT_ID_BOOT_MOUSE_IN               -3  //fake report ID for boot mode mouse input


typedef void (*BtleHidConnStateCbk)(ble_hid_conn_t hidId, uint8_t state);
typedef void (*BtleHidReportRxCbk)(ble_hid_conn_t hidId, int32_t reportId, sg data, bool byRequest);

//init the subsystem
void btleHidInit(BtleHidConnStateCbk stateCbk, BtleHidReportRxCbk rxCbk) NEWBLUE_EXPORT;

//attach/detach
ble_hid_conn_t btleHidAttach(gatt_client_conn_t hidId, const char *deviceName) NEWBLUE_EXPORT;
bool btleHidDetach(ble_hid_conn_t hidId) NEWBLUE_EXPORT;
bool btleHidDetachFromGatt(gatt_client_conn_t hidId) NEWBLUE_EXPORT;

//descriptors & info
bool btleHidGetReportDescriptors(ble_hid_conn_t hidId, const void **descriptorDataP, uint32_t *descriptorLenP) NEWBLUE_EXPORT;
bool btleHidGetHidInfo(ble_hid_conn_t hidId, uint16_t *hidVerP, uint8_t *countryP, uint8_t *flagsP) NEWBLUE_EXPORT;
bool btleHidGetHidName(ble_hid_conn_t hidId, char *hidName) NEWBLUE_EXPORT;

//reports
bool btleHidWriteReport(ble_hid_conn_t hidId, int32_t reportId, sg data) NEWBLUE_EXPORT;
bool btleHidReadReport(ble_hid_conn_t hidId, int32_t reportId) NEWBLUE_EXPORT; // -> BtleHidReportRxCbk

NEWBLUE_END_DECLS

#endif

