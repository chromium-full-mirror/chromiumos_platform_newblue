#ifndef _GATT_H_
#define _GATT_H_

#include "types.h"
#include "uuid.h"
#include "uniq.h"
#include "att.h"
#include "sdp.h"

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
int gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF, bool supportLE, bool supportEDR);
bool gattServiceDestroy(int svc);
uint16_t gattServiceGetHandleBaseByUuid(const struct uuid *uuid); /* or 0 on error */
uint16_t gattServiceGetHandleBaseBySvc(int svc); /* or 0 on error */

/* service control */
bool gattServiceStart(int svc);
bool gattServiceStop(int svc);
bool gattServiceIsRunning(int svc);
bool gattServiceGetUuid(int svc, struct uuid *uuid);

/* service construction (order of calls matters and is important). All return handle value or 0 on error */
uint16_t gattServiceAddIncludedService(int svc, const struct uuid *uuid);
uint16_t gattServiceAddChar(int svc, const struct uuid *uuid, uint8_t props, uint32_t perms);
uint16_t gattServiceAddCharDescr(int svc, const struct uuid *uuid, uint32_t perms);

/* TX path */
bool gattServiceSendInd(int svc, int cid, uint16_t handle, const void *data, uint16_t len, bool withConfirm, uint64_t ref);


#endif


