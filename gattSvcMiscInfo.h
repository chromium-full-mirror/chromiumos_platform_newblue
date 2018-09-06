/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _GATT_SVC_MISC_INFO_H_
#define _GATT_SVC_MISC_INFO_H_

#include "gatt.h"
#include "uuid.h"
#include "uuid.h"

NEWBLUE_BEGIN_DECLS

//For info see: https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.service.device_information.xml
//For info see: https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.service.generic_access.xml


//uuid of services
#define BTLE_UUID_DEVICE_INFO_SERVICE                            0x180A  // org.bluetooth.service.device_information
#define BTLE_UUID_GAP_SERVICE                                    0x1800  // org.bluetooth.service.generic_access

typedef uniq_t gatt_svc_misc_info_conn_t;

//normal states
#define BTLE_MISC_INFO_SVC_CONN_STATE_INVALID                    0x00 // should never be seen
#define BTLE_MISC_INFO_SVC_CONN_STATE_GATT_DISCOVERY             0x01 // looking for services & enumerating their insides
#define BTLE_MISC_INFO_SVC_CONN_STATE_UP                         0x02 // ready to be used
#define BTLE_MISC_INFO_SVC_CONN_STATE_TEARDOWN                   0x03 // going down

enum GattSvcMiscInfoType { // do NOT make this sparse
    GattMiscInfoTypeInvalid = 0,                 // does not exist - marker for invalid type
    GattMiscInfoTypeManufNameString,             // optional (from Device Info profile)
    GattMiscInfoTypeModelNumberString,           // optional (from Device Info profile)
    GattMiscInfoTypeSerialNumberString,          // optional (from Device Info profile)
    GattMiscInfoTypeHardwareRevisionString,      // optional (from Device Info profile)
    GattMiscInfoTypeFirmwareRevisionString,      // optional (from Device Info profile)
    GattMiscInfoTypeSoftwareRevisionString,      // optional (from Device Info profile)
    GattMiscInfoTypeSystemID,                    // optional (from Device Info profile)
    GattMiscInfoTypeIeee11073_20601regCertList,  // optional (from Device Info profile)
    GattMiscInfoTypePnpID,                       // optional (from Device Info profile)
    GattMiscInfoTypeDeviceName,                  // mandatory (but still often missing)  (from GAP)
    GattMiscInfoTypeAppearance,                  // mandatory (but still often missing)  (from GAP)

    GattMiscInfoEnumMax,                         // always make sure this is the last item in this enum
};

typedef void (*GattSvcMiscInfoConnStateCbk)(gatt_svc_misc_info_conn_t handle, uint8_t state);
typedef void (*GattSvcMiscInfoResultCbk)(gatt_svc_misc_info_conn_t handle, enum GattSvcMiscInfoType type, sg data);

//init the subsystem
void gattSvcMiscInfoInit(GattSvcMiscInfoConnStateCbk stateCbk, GattSvcMiscInfoResultCbk rxCbk) NEWBLUE_EXPORT;

//attach/detach
gatt_svc_misc_info_conn_t gattSvcMiscInfoAttach(gatt_client_conn_t gattConn) NEWBLUE_EXPORT;
bool gattSvcMiscInfoDetach(gatt_svc_misc_info_conn_t handle) NEWBLUE_EXPORT;
bool gattSvcMiscInfoDetachFromGatt(gatt_client_conn_t gattConn) NEWBLUE_EXPORT;

//descriptors & info
bool gattSvcMiscInfoRequestRead(gatt_svc_misc_info_conn_t handle, enum GattSvcMiscInfoType type) NEWBLUE_EXPORT;

NEWBLUE_END_DECLS

#endif

