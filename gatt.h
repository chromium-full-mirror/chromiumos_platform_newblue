#ifndef _GATT_H_
#define _GATT_H_

#include "types.h"
#include "uniq.h"
#include "att.h"

typedef uniq_t gatt_svc_t;

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


/* callbacks */
typedef uint8_t (*gattSrvValueReadCbk)(gatt_svc_t svc, l2c_handle_t who, void *cbkData, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t *lenP, void *dst); /* -> ATT_ERROR_* lenP: in/out*/
typedef uint8_t (*gattSrvValueWriteCbk)(gatt_svc_t svc, l2c_handle_t who, void *cbkData, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t *lenP, const void *dst); /* -> ATT_ERROR_* lenP: in/out*/

/* service lifecycle */
gatt_svc_t gattServiceCreate(const struct att_uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF);
bool gattServiceDestroy(gatt_svc_t svc);
uint16_t gattServiceGetHandleBase(const struct att_uuid *uuid); /* or 0 on error */

/* service control */
bool gattServiceStart(gatt_svc_t svc);
bool gattServiceStop(gatt_svc_t svc);

/* service construction (order of calls matters and is important). All return handle value or 0 on error */
uint16_t gattServiceAddIncludedService(gatt_svc_t svc, uint16_t includedSvcHandle);
uint16_t gattServiceAddChar(gatt_svc_t svc, const struct att_uuid *uuid, uint8_t props, uint32_t perms, void *userData);
uint16_t gattServiceAddCharDescr(gatt_svc_t svc, const struct att_uuid *uuid, uint32_t perms, void *userData);




#endif


