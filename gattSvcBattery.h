/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _GATT_SVC_BATT_H_
#define _GATT_SVC_BATT_H_

#include "gatt.h"
#include "uuid.h"
#include "uuid.h"

NEWBLUE_BEGIN_DECLS


//uuid of service
#define BTLE_UUID_BATTERY_SERVICE                           0x180F  // org.bluetooth.service.battery_service


typedef uniq_t gatt_svc_batt_conn_t;

//normal states
#define BTLE_BATT_SVC_CONN_STATE_INVALID                    0x00 // should never be seen
#define BTLE_BATT_SVC_CONN_STATE_GATT_DISCOVERY             0x01 // looking for main "Battery Service" service & enumerating its insides
#define BTLE_BATT_SVC_CONN_STATE_WRITING_CCCDS              0x02 // writing CCCDs as needed
#define BTLE_BATT_SVC_CONN_STATE_UP                         0x03 // ready to be used
#define BTLE_BATT_SVC_CONN_STATE_TEARDOWN                   0x04 // going down


typedef void (*GattSvcBattConnStateCbk)(gatt_svc_batt_conn_t handle, uint8_t state);
typedef void (*GattSvcBattUpdateCbk)(gatt_svc_batt_conn_t handle, uint8_t percent, bool byRequest);

//init the subsystem
void gattSvcBattInit(GattSvcBattConnStateCbk stateCbk, GattSvcBattUpdateCbk rxCbk) NEWBLUE_EXPORT;

//attach/detach
gatt_svc_batt_conn_t gattSvcBattAttach(gatt_client_conn_t gattConn) NEWBLUE_EXPORT;
bool gattSvcBattDetach(gatt_svc_batt_conn_t handle) NEWBLUE_EXPORT;
bool gattSvcBattDetachFromGatt(gatt_client_conn_t gattConn) NEWBLUE_EXPORT;

//descriptors & info
bool gattSvcBattRequestRead(gatt_svc_batt_conn_t handle) NEWBLUE_EXPORT;
bool gattSvcBattIsNotifSupported(gatt_svc_batt_conn_t handle) NEWBLUE_EXPORT;

NEWBLUE_END_DECLS

#endif

