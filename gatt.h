#ifndef _GATT_H_
#define _GATT_H_

#include "types.h"
#include "uuid.h"
#include "uniq.h"
#include "att.h"
#include "sdp.h"
#include "sg.h"

/* sadly android interface stop sus from using uniq_t for handles here */

#define GATT_PSM                     0x001F
#define GATT_FIXEDCH                 0x0004


/* UUIDs & other GATT defines */
#define GATT_UUID_SVC_GAP            SDP_SVC_CLASS_GAP
#define GATT_UUID_SVC_GATT_SVR       SDP_SVC_CLASS_GATT
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
bool gattProfileInit(void);
void gattProfileDeinit(void);

/* replies to callbacks */
void gattSrvCbkReply(int cid, int transId, uint16_t handle, uint8_t err, const void *data, uint16_t len); /* err is ATT_ERROR_* */

/* callbacks */
typedef bool (*gattSrvValueReadCbk)(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len);
typedef bool (*gattSrvValueWriteCbk)(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data);
typedef void (*gattSrvIndCbk)(int svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref); /* GATT_SRV_EVT_* */

/* service lifecycle */
int gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF);
bool gattServiceDestroy(int svc);
uint16_t gattServiceGetHandleBaseByUuid(const struct uuid *uuid); /* or 0 on error */
uint16_t gattServiceGetHandleBaseBySvc(int svc); /* or 0 on error */
int gattServiceFindByHandle(uint16_t handle, bool firstOnly); /* negative on error */

/* service control */
bool gattServiceStart(int svc, bool supportLE, bool supportEDR);
bool gattServiceStop(int svc);
bool gattServiceIsRunning(int svc);
bool gattServiceGetUuid(int svc, struct uuid *uuid);

/* service construction (order of calls matters and is important). All return handle value or 0 on error */
uint16_t gattServiceAddIncludedService(int svc, const struct uuid *uuid);
uint16_t gattServiceAddChar(int svc, const struct uuid *uuid, uint8_t props, uint32_t perms);
uint16_t gattServiceAddCharDescr(int svc, const struct uuid *uuid, uint32_t perms);

/* TX path */
bool gattServiceSendInd(int svc, int cid, uint16_t handle, const void *data, uint16_t len, bool withConfirm, uint64_t ref);



/* ==== GATT CLIENT CODE (AKA CENTRAL MODE) ==== */

#define GATT_CLI_STATUS_OK               0
#define GATT_CLI_STATUS_OTHER_SIDE_DISC  1 /* the peer told us to go away */
#define GATT_CLI_STATUS_ERR              2 /* an unknown error type in the local stack */

/* various callbacks */
typedef void (*gattCliConnectResultCbk)(int cli, int conn, const struct bt_addr *to, uint8_t status);
typedef void (*gattCliSvcEnumCbk)(int cli, int con, const struct uuid *uuid, bool primary, uint16_t firstHandle, uint16_t numHandles, uint8_t status); /* uuid passed as set to null after final result */
typedef void (*gattCliEnumIncludedSvcsCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *uuid, uint16_t firstHandle, uint16_t numHandles, uint8_t status);
typedef void (*gattCliEnumCharacteristicsCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *uuid, uint16_t firstHandle, uint16_t numHandles, uint8_t status);
typedef void (*gattCliEnumCharDescrsCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *uuid, uint16_t firstHandle, uint16_t numHandles, uint8_t status);
typedef void (*gattCliCharReadCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t status, const void *data, uint32_t len);
typedef void (*gattCliCharWriteCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t status);
typedef void (*gattCliCharDescrReadCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t status, sg data);
typedef void (*gattCliCharDescrWriteCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t status);
typedef void (*gattCliStagedWriteExecuteCb)(int cli, int con, bool success);
typedef void (*gattCliNotifSubscribeStateCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *thisChar, bool forSubscribe, uint8_t status);
typedef void (*gattCliNotifArrivedCbk)(int cli, int con, const struct uuid *inThisService, const struct uuid *thisChar, bool reliable, sg data);


/* client lifecycle */
int gattClientCreate(const struct uuid *uuid);
void gattClientDestroy(int cli);

/* connection lifecycle */
uint8_t gattClientConnect(int cli, const struct bt_addr *to, gattCliConnectResultCbk resultCbk);
uint8_t gattClientDisconnect(int conn); /* calls the gattCliConnectResultCbk passed to gattClientConnect() with GATT_CLI_OTHER_SIDE_DISC */

/* cache maintenance */
bool gattClientInvalidateCache(const struct bt_addr *whomTo); //true if process started

/* service search */
uint8_t gattClientEnumServices(int conn, const struct uuid *searchForThis, gattCliSvcEnumCbk cbk); /* searchForThis can be null to enumerate all services without filtering for that uuid. if another enumeration is ongoing, yours will wait till that one is done */

/* various enumerations. Each returns "next" item given "current" item. "current" is passed as NULL to get first */
uint8_t gattClientEnumIncludedServices(int conn, const struct uuid *inThisService, const struct uuid *prev, gattCliEnumIncludedSvcsCbk cbk); 
uint8_t gattClientEnumCharacteristics(int conn, const struct uuid *inThisService, const struct uuid *prev, gattCliEnumCharacteristicsCbk cbk);
uint8_t gattClientEnumCharDescriptors(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *prev, gattCliEnumCharDescrsCbk cbk);

/* reads/writes */
uint8_t gattClientCharRead(int conn, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t authReq, gattCliCharReadCbk cbk);
uint8_t gattClientCharWrite(int conn, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, gattCliCharWriteCbk cbk);
uint8_t gattClientCharDescrRead(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t authReq, gattCliCharDescrReadCbk cbk);
uint8_t gattClientCharDescrWrite(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, gattCliCharDescrWriteCbk cbk);
uint8_t gattClientStagedWritesExecute(int conn, bool execute, gattCliStagedWriteExecuteCb cbk); //if not execute then drop them all

/* notification subscription/unsubscription */
uint8_t gattClientNotifsSubscribe(int conn, const struct uuid *inThisService, const struct uuid *thisChar, gattCliNotifSubscribeStateCbk stateCbk, gattCliNotifArrivedCbk arrivedCbk);
uint8_t gattClientNotifsUnsubscribe(int conn, const struct uuid *inThisService, const struct uuid *thisChar, gattCliNotifSubscribeStateCbk stateCbk);





#endif


