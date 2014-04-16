#ifndef _GATT_H_
#define _GATT_H_

#include "types.h"
#include "uuid.h"
#include "uniq.h"
#include "att.h"

/* sadly android interface stop sus from using uniq_t for handles here */

#define GATT_PSM                     0x001F
#define GATT_FIXEDCH                 0x0004


/* UUIDs */
#define GATT_UUID_SVC_PRIMARY        0x2800
#define GATT_UUID_SVC_SECONDARY      0x2801
#define GATT_UUID_INCLUDE            0x2802
#define GATT_UUID_CHARACTERISTIC     0x2803
#define GATT_UUID_CHAR_EXTD_PROPS    0x2900
#define GATT_UUID_CHAR_USER_DESCR    0x2901
#define GATT_UUID_CHAR_CLI_CHAR_CFG  0x2902
#define GATT_UUID_CHAR_SVR_CHAR_CFG  0x2903
#define GATT_UUID_CHAR_PRES_FMT      0x2904
#define GATT_UUID_CHAR_AGGR_FMT      0x2905

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

/* ind/notif srv events */
#define GATT_SRV_EVT_NOTIF_SENT      1
#define GATT_SRV_EVT_IND_SENT        2
#define GATT_SRV_EVT_IND_ACKED       3
#define GATT_SRV_EVT_IND_TIMEOUT     4

/* profile init/deinit */
bool gattProfileInit(void);
void gattProfileDeinit(void);


/* callbacks */
typedef uint8_t (*gattSrvValueReadCbk)(int svc, l2c_handle_t who, int cid, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len); /* -> ATT_ERROR_* */
typedef uint8_t (*gattSrvValueWriteCbk)(int svc, l2c_handle_t who, int cid, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data); /* -> ATT_ERROR_* */
typedef void (*gattSrvIndCbk)(int svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt); /* GATT_SRV_EVT_* */

/* service lifecycle */
int gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF);
bool gattServiceDestroy(int svc);
uint16_t gattServiceGetHandleBase(const struct uuid *uuid); /* or 0 on error */

/* service control */
bool gattServiceStart(int svc);
bool gattServiceStop(int svc);

/* service construction (order of calls matters and is important). All return handle value or 0 on error */
uint16_t gattServiceAddIncludedService(int svc, const struct uuid *uuid);
uint16_t gattServiceAddChar(int svc, const struct uuid *uuid, uint8_t props, uint32_t perms);
uint16_t gattServiceAddCharDescr(int svc, const struct uuid *uuid, uint32_t perms);

/* TX path */
bool gattServiceSendInd(int svc, l2c_handle_t to, uint16_t handle, const void *data, uint32_t len, bool withConfirm);


#endif


