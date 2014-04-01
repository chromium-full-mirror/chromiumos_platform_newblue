#ifndef _ATT_H_
#define _ATT_H_

#include "types.h"
#include "l2cap.h"
#include "uniq.h"
#include "sm.h"

typedef uniq_t att_range_t;
struct attServer;

/* we WILL negotiate, but only this much is ever guaranteed */
#define ATT_MTU_MIN_EDR                48
#define ATT_MTU_MIN_LE                 23

#define ATT_SIG_LEN                    SM_SIG_LEN

#define ATT_PERM_READ                  0x01
#define ATT_PERM_WRITE                 0x02
#define ATT_PERM_NEED_ENCR             0x04
#define ATT_PERM_NEED_AUTH             0x08
/* "need athorization" shall be handled bu higher layer code */

#define ATT_ATTR_MAX_LEN               512


#define ATT_ERROR_NONE                 0x00
#define ATT_ERROR_INVALID_HANDLE       0x01
#define ATT_ERROR_READ_NOT_ALLOWED     0x02
#define ATT_ERROR_WRITE_NOT_ALLOWED    0x03
#define ATT_ERROR_INVALID_PDU          0x04
#define ATT_ERROR_INSUFFICIENT_AUTH    0x05
#define ATT_ERROR_REQ_NOT_SUPPORTED    0x06
#define ATT_ERROR_INVALID_OFFSET       0x07
#define ATT_ERROR_INSUFFICIENT_ATHOR   0x08
#define ATT_ERROR_PREPARE_QUEUE_FULL   0x09
#define ATT_ERROR_ATTRIBUTE_NOT_FOUND  0x0A
#define ATT_ERROR_ATTRIBUTE_NOT_LONG   0x0B
#define ATT_ERROR_INSUFF_ENCR_KEY_SZ   0x0C
#define ATT_ERROR_INVAL_ATTR_VAL_LEN   0x0D
#define ATT_ERROR_UNLIKELY_ERROR       0x0E
#define ATT_ERROR_INSUFFICIENT_ENCR    0x0F
#define ATT_ERROR_UNSUPP_GROUP_TYPE    0x10
#define ATT_ERROR_INSUFFICIENT_RSRCS   0x11
#define ATT_ERROR_APP_ERR_FIRST        0x80
#define ATT_ERROR_APP_ERR_LAST         0x9F
#define ATT_ERROR_COMMON_FIRST         0xE0
#define ATT_ERROR_COMMON_LAST          0xFF

struct att_uuid {
    uint64_t lo;
    uint64_t hi;
} __packed;

#define ATT_UUID_BASE                  {0x800000805F9B34FBULL, 0x0000000000001000ULL}
#define ATT_FIRST_VALID_HANDLE         0x0001
#define ATT_LAST_VALID_HANDLE          0xFFFF


#define ATT_TX_RET_ACCEPTED         L2C_TX_ACCEPTED
#define ATT_TX_RET_NO_CONN          L2C_TX_NO_CONN
#define ATT_TX_RET_TRY_LATER        L2C_TX_TRY_LATER
#define ATT_TX_RET_L2C_ERR          L2C_TX_ERROR
#define ATT_TX_RET_OOM              0x20
#define ATT_TX_RET_ATT_ERROR        0x21
#define ATT_TX_RET_ATT_BUSY         0x22


/* att server api */
typedef uint8_t (*attValueReadCbk)(struct attServer* srv, l2c_handle_t who, void *cbkData, att_range_t range, uint16_t ofst, uint16_t byteOfst, uint16_t *lenP, void *dst); /* -> ATT_ERROR_* lenP: in/out*/
typedef uint8_t (*attValueWriteCbk)(struct attServer* srv, l2c_handle_t who, void *cbkData, att_range_t range, uint16_t ofst, uint16_t byteOfst, uint16_t *lenP, const void *dst); /* -> ATT_ERROR_* lenP: in/out*/
typedef uint8_t (*attTxCbk)(struct attServer* srv, l2c_handle_t to, sg s); /* -> ATT_TX_RET_* */
typedef void (*attMtuCbk)(struct attServer* srv, l2c_handle_t to, uint32_t mtu);

struct attServer* attSrvCreate(attTxCbk txF, attMtuCbk mtuF);
void attSrvSetMtu(struct attServer* srv, l2c_handle_t to, uint32_t mtu); /* EDR uses this since L2D negotiates MTU */
void attSrvNotifConnClose(struct attServer* srv, l2c_handle_t conn); /*allows server to no longer try to notify it */
void attSrvDestroy(struct attServer* srv);

att_range_t attSrvHandleRangeReserve(struct attServer* srv, uint16_t len); /* -> 0 on failure */
void attSrvHandleRangeRelease(struct attServer* srv, att_range_t rangeRef);
uint8_t attSrvValueNotifyChanged(struct attServer* srv, att_range_t rangeRef, uint16_t offset, l2c_handle_t to, sg data, bool notify); /* -> ATT_NOTIF_RET_* */
bool attSrvDataRx(struct attServer* srv, l2c_handle_t from, sg s);


/* att client api */
//TODO


//todo: PS: 0x1F. channel: 0x0004
//mtu negotioation only over LE! l2c mtu negotiation is used ov er edr

#endif
