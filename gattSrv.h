#ifndef _GATT_SRV_H_
#define _GATT_SRV_H_

#include "types.h"
#include "uuid.h"
#include "uniq.h"
#include "gatt.h"
#include "att.h"
#include "sdp.h"


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


#endif


