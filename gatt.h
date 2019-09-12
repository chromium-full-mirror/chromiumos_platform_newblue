/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _GATT_H_
#define _GATT_H_

#include "newblue-macros.h"
#include "att.h"
#include "sg.h"
#include "types.h"
#include "uuid.h"
#include "uniq.h"


NEWBLUE_BEGIN_DECLS

typedef uniq_t gatt_service_t;
typedef uniq_t gatt_client_conn_t;


#define GATTHANDLEFMT    UNIQ_FMT"x"
#define GATTHANDLECNV(x) UNIQ_CONV(x)


#define GATT_PSM                     0x001F
#define GATT_FIXEDCH                 0x0004


/* UUIDs & other GATT defines */
#define GATT_UUID_SVC_GAP            0x1800
#define GATT_UUID_SVC_GATT_SVR       0x1801
#define GATT_UUID_SVC_PRIMARY        0x2800
#define GATT_UUID_SVC_SECONDARY      0x2801
#define GATT_UUID_INCLUDE            0x2802
#define GATT_UUID_CHARACTERISTIC     0x2803
#define GATT_UUID_CHAR_EXTD_PROPS    0x2900
#define GATT_UUID_CHAR_USER_DESCR    0x2901
#define GATT_UUID_CHAR_CLI_CHAR_CFG  0x2902 /* uint16_t, see GATT_CLI_CFG_BIT_* */
#define GATT_UUID_CHAR_SVR_CHAR_CFG  0x2903
#define GATT_UUID_CHAR_PRES_FMT      0x2904
#define GATT_UUID_CHAR_AGGR_FMT      0x2905

#define GATT_CLI_CFG_BIT_NOTIFS      0x0001
#define GATT_CLI_CFG_BIT_INDS        0x0002

/* what other device sees */
#define GATT_PROP_BROADCAST          0x01
#define GATT_PROP_READ               0x02
#define GATT_PROP_WRITE_NO_RSP       0x04
#define GATT_PROP_WRITE              0x08
#define GATT_PROP_NOTIFY             0x10
#define GATT_PROP_INDICATE           0x20
#define GATT_PROP_AUTH_SIG_WRITE     0x40
#define GATT_PROP_EXTD_PROPS         0x80

/* what we enforce */
#define GATT_PERM_READ               0x00000001
#define GATT_PERM_READ_ENCR          0x00000002
#define GATT_PERM_READ_ENCR_MITM     0x00000004
#define GATT_PERM_WRITE              0x00000010
#define GATT_PERM_WRITE_ENCR         0x00000020
#define GATT_PERM_WRITE_ENCR_MITM    0x00000040
#define GATT_PERM_WRITE_SIGNED       0x00000080
#define GATT_PERM_WRITE_SIGNED_MITM  0x00000100
#define GATT_PERM_ALL_READ           (GATT_PERM_READ | GATT_PERM_READ_ENCR | GATT_PERM_READ_ENCR_MITM)
#define GATT_PERM_ALL_WRITE          (GATT_PERM_WRITE | GATT_PERM_WRITE_ENCR | GATT_PERM_WRITE_ENCR_MITM | GATT_PERM_WRITE_SIGNED | GATT_PERM_WRITE_SIGNED_MITM)


/* ==== GATT SERVER CODE (AKA PERIPHERAL MODE) ==== */

/* profile init/deinit */
bool gattProfileInit(void) NEWBLUE_EXPORT;
void gattProfileDeinit(void) NEWBLUE_EXPORT;

/* replies to callbacks */
void gattSrvCbkReply(att_cid_t cid, att_trans_t transId, uint16_t handle, uint8_t err,
    const void *data, uint16_t len) NEWBLUE_EXPORT; /* err is ATT_ERROR_* */

/* callbacks */
typedef bool (*gattSrvValueReadCbk)(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len);
typedef bool (*gattSrvValueWriteCbk)(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data);
typedef void (*gattSrvIndCbk)(gatt_service_t svc, l2c_handle_t who, att_cid_t cid, uint16_t handle, uint8_t evt, uint64_t ref); /* GATT_SRV_EVT_* */
typedef void (*gattSrvDisconnectCbk)(gatt_service_t svc, l2c_handle_t who);

/* service lifecycle */
gatt_service_t gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles,
    gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF,
    gattSrvDisconnectCbk discF) NEWBLUE_EXPORT;
bool gattServiceDestroy(gatt_service_t svc) NEWBLUE_EXPORT;
uint16_t gattServiceGetHandleBaseByUuid(const struct uuid *uuid) NEWBLUE_EXPORT; /* or 0 on error */
uint16_t gattServiceGetHandleBaseBySvc(gatt_service_t svc) NEWBLUE_EXPORT; /* or 0 on error */
gatt_service_t gattServiceFindByHandle(uint16_t handle, bool firstOnly) NEWBLUE_EXPORT; /* 0 on error */

/* service control */
bool gattServiceStart(gatt_service_t svc, bool supportLE, bool supportEDR) NEWBLUE_EXPORT;
bool gattServiceStop(gatt_service_t svc) NEWBLUE_EXPORT;
bool gattServiceIsRunning(gatt_service_t svc) NEWBLUE_EXPORT;
bool gattServiceGetUuid(gatt_service_t svc, struct uuid *uuid) NEWBLUE_EXPORT;

/* service construction (order of calls matters and is important). All return handle value or 0 on
 * error
 */
uint16_t gattServiceAddIncludedService(gatt_service_t svc, const struct uuid *uuid) NEWBLUE_EXPORT;
uint16_t gattServiceAddChar(gatt_service_t svc, const struct uuid *uuid, uint8_t props,
    uint32_t perms) NEWBLUE_EXPORT;
uint16_t gattServiceAddCharDescr(gatt_service_t svc, const struct uuid *uuid,
    uint32_t perms) NEWBLUE_EXPORT;

/* TX path */
bool gattServiceSendInd(gatt_service_t svc, att_cid_t cid, uint16_t handle, const void *data,
    uint16_t len, bool withConfirm, uint64_t ref) NEWBLUE_EXPORT;



/* ==== GATT CLIENT CODE (AKA CENTRAL MODE) ==== */

#define GATT_CLI_STATUS_OK               0
#define GATT_CLI_STATUS_OTHER_SIDE_DISC  1 /* the peer told us to go away */
#define GATT_CLI_STATUS_ERR              2 /* an error from either local stack or peer - see ATT_ERROR_* for the actual error */
#define GATT_CLI_STATUS_WE_DISC          3 /* we disconnected */

#define GATT_CLI_WRITE_TYPE_SIGNED       0 /* bluedroid has no support of this so we can assign this value to whatever we want. we pick zero for better switch() compilation */
#define GATT_CLI_WRITE_TYPE_WRITE_NO_RSP 1 /* these next values must match undocumented in the *android* includes values that bluedroid uses */
#define GATT_CLI_WRITE_TYPE_WRITE        2
#define GATT_CLI_WRITE_TYPE_PREPARE      3

#define GATT_CLI_AUTH_REQ_NONE           0 /* these next values must match undocumented in the *android* includes values that bluedroid uses */
#define GATT_CLI_AUTH_REQ_NO_MITM        1 /* the following 4 options are all meant to be encrypted */
#define GATT_CLI_AUTH_REQ_MITM           2
#define GATT_CLI_AUTH_REQ_SIGNED_NO_MITM 3
#define GATT_CLI_AUTH_REQ_SIGNED_MITM    4

struct GattTraversedServiceCharDescr {
  struct uuid uuid;
  uint16_t handle;
};

struct GattTraversedServiceChar {
    struct uuid uuid;
    uint8_t charProps;
    uint16_t firstHandle, valHandle, lastHandle;
    uint16_t numDescrs;
    struct GattTraversedServiceCharDescr *descrs;
};

struct GattTraversedServiceInclSvc {
  struct uuid uuid;
  uint16_t includeDefHandle, firstHandle, lastHandle;
};

struct GattTraversedService {
    struct uuid uuid;
    uint16_t firstHandle, lastHandle;
    uint16_t numChars;
    struct GattTraversedServiceChar *chars;
    uint16_t numInclSvcs;
    struct GattTraversedServiceInclSvc *inclSvcs;
};


/* gatt client callbacks */

//called when a connection attempt has finished. status will say whether it was successful or not. Expect a call to this from gattClientConnect() and gattClientDisconnect()
typedef void (*gattCliConnectResultCbk)(void *userData, gatt_client_conn_t conn, uint8_t status);

//called when a service is found. Expect one or more calls to this after calling gattClientFindService() or gattClientEnumServices(). UUID set to NULL when enumeration is complete
typedef void (*gattCliSvcEnumCbk)(void *userData, gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, bool primary, uint16_t firstHandle, uint16_t numHandles, uint8_t status);

//called when an included service has been discovered. Expect one or more calls to this after calling gattClientEnumIncludedServices(). UUID set to NULL when enumeration is complete
typedef void (*gattCliEnumIncludedSvcsCbk)(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t includeDefHandle, uint16_t firstHandle, uint16_t lastHandle, uint8_t status);

//called when a characteristic has been discovered. Expect one or more calls to this after calling gattClientEnumCharacteristics(). UUID set to NULL when enumeration is complete
typedef void (*gattCliEnumCharacteristicsCbk)(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t charHandle, uint16_t valHandle, uint8_t props, uint8_t status);

//called when a characteristic descriptor has been discovered. Expect one or more calls to this after calling gattClientEnumCharDescriptors(). UUID set to NULL when enumeration is complete
typedef void (*gattCliEnumCharDescrsCbk)(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t handle, uint8_t status);

//called as a response to gattClientRead(). data will be non-NULL if the read was a success. if status is GATT_CLI_STATUS_ERR, check error for ATT_ERROR_*.
typedef void (*gattCliReadCbk)(void* userData, gatt_client_conn_t conn, uniq_t trans, uint16_t handle, uint8_t status, uint8_t error, sg data);

//called as a response to gattClientWrite()
typedef void (*gattCliWriteCbk)(void* userData, gatt_client_conn_t conn, uniq_t trans, uint16_t handle, uint8_t status, uint8_t error);

//called as a response to gattClientStagedWritesExecute() when the other side has replied
typedef void (*gattCliStagedWriteExecuteCb)(gatt_client_conn_t conn, uniq_t trans, bool success);

//Expect this after a call to gattClientNotifsSubscribe() or gattClientNotifsUnsubscribe(), telling you whether your request succeeded
typedef void (*gattCliNotifSubscribeStateCbk)(gatt_client_conn_t conn, uint16_t cccdHandle, bool forSubscribe, uint8_t status);

//Expect this to be called when a notification or an indication you had subscribed to has arrived. NOTE: This is async, so this may even be called before gattClientNotifsSubscribe() returns
typedef void (*gattCliNotifArrivedCbk)(gatt_client_conn_t conn, uint16_t charValueHandle, bool reliable, sg data);

//Expect this to be called as a result of gattClientUtilFindAndTraversePrimaryService(). Data is NULL for failures. Data pointer NO LONGER VALID after this func returns - make copies!
typedef void (*gattCliUtilSvcTraversedCbk)(void* userData, gatt_client_conn_t conn, uniq_t trans, const struct GattTraversedService* data);

//Expect this to be called as a result of gattClientUtilLongRead(). Data is NULL for failures. For failures, see |error| for ATT_ERROR_*. You own the sg.
typedef void (*gattCliUtilLongReadCompletedCbk)(void* userData, gatt_client_conn_t conn, uniq_t trans, uint16_t handle, uint8_t error, sg data);



/* connection lifecycle */
gatt_client_conn_t gattClientConnect(void *userData, const struct bt_addr *to, gattCliConnectResultCbk resultCbk) NEWBLUE_EXPORT;
uint8_t gattClientDisconnect(gatt_client_conn_t conn) NEWBLUE_EXPORT; /* -> GATT_CLI_*, calls the gattCliConnectResultCbk passed to gattClientConnect() with GATT_CLI_OTHER_SIDE_DISC */

/* cache maintenance */
bool gattClientInvalidateCache(const struct bt_addr *whomTo) NEWBLUE_EXPORT; //true if process started

/* all of the following return GATT_CLI_* */

/* service search */
uint8_t gattClientFindService(void *userData, gatt_client_conn_t conn, bool primary, const struct uuid *searchForThis, uniq_t trans, gattCliSvcEnumCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientEnumServices(void *userData, gatt_client_conn_t conn, bool primary, uniq_t trans, gattCliSvcEnumCbk cbk) NEWBLUE_EXPORT;

/* various enumerations */
uint8_t gattClientEnumIncludedServices(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumIncludedSvcsCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientEnumCharacteristics(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumCharacteristicsCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientEnumCharDescriptors(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumCharDescrsCbk cbk) NEWBLUE_EXPORT;

/* reads/writes (writes MUST fit into MTU minus whatever, or they will be truncated) */
uint8_t gattClientRead(void* userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uint16_t offset, uniq_t trans, gattCliReadCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientWrite(void* userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, uniq_t trans, gattCliWriteCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientStagedWritesExecute(gatt_client_conn_t conn, bool execute, uniq_t trans, gattCliStagedWriteExecuteCb cbk) NEWBLUE_EXPORT; //if not execute then drop them all

/* notification subscription/unsubscription. gattClientNotifsSubscribe assumes you do not ask for the impossible */
uint8_t gattClientNotifsSubscribe(gatt_client_conn_t conn, uint16_t charHandle, uint16_t charValueHandle, uint16_t cccdHandle, bool reliable, gattCliNotifSubscribeStateCbk stateCbk, gattCliNotifArrivedCbk arrivedCbk) NEWBLUE_EXPORT;
uint8_t gattClientNotifsUnsubscribe(gatt_client_conn_t conn, uint16_t charValueHandle) NEWBLUE_EXPORT;

/* higher-level utilities */
uint8_t gattClientUtilFindAndTraversePrimaryService(void* userData, gatt_client_conn_t conn, const struct uuid *svcUuid, uniq_t trans, gattCliUtilSvcTraversedCbk cbk) NEWBLUE_EXPORT;
uint8_t gattClientUtilLongRead(void* userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uniq_t trans, gattCliUtilLongReadCompletedCbk cbk) NEWBLUE_EXPORT;


NEWBLUE_END_DECLS




#endif


