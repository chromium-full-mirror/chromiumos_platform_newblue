#ifndef _ATT_H_
#define _ATT_H_

#include "types.h"
#include "l2cap.h"
#include "uniq.h"
#include "uuid.h"
#include "sm.h"

typedef uniq_t att_range_t;


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
#define ATT_MAX_LEN_FOR_RD_BY_TYPE     253 /* no more than this many bytes of an att will ever be returned for a ReadByType request */
#define ATT_MAX_LEN_FOR_RD_BY_GRP_TYP  251 /* no more than this many bytes of an att will ever be returned for a ReadByGroupType request */
#define ATT_MIN_HANDLES_IN_READ_MULT   2


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


#define ATT_FIRST_VALID_HANDLE         0x0001
#define ATT_LAST_VALID_HANDLE          0xFFFF


#define ATT_TX_RET_ACCEPTED            L2C_TX_ACCEPTED
#define ATT_TX_RET_NO_CONN             L2C_TX_NO_CONN
#define ATT_TX_RET_TRY_LATER           L2C_TX_TRY_LATER
#define ATT_TX_RET_L2C_ERR             L2C_TX_ERROR
#define ATT_TX_RET_OOM                 0x20
#define ATT_TX_RET_ATT_ERROR           0x21
#define ATT_TX_RET_ATT_BUSY            0x22


#define ATT_READ_FOR_FIND_BY_TYPE_VAL  0x01
#define ATT_READ_FOR_READ_BY_TYPE      0x02
#define ATT_READ_FOR_READ              0x03
#define ATT_READ_FOR_READ_BLOB         0x04
#define ATT_READ_FOR_READ_MULT         0x05
#define ATT_READ_FOR_READ_BY_GRP_TYPE  0x06

#define ATT_WRITE_FOR_WRITE_REQ        0x01
#define ATT_WRITE_FOR_WRITE_CMD        0x02
#define ATT_WRITE_FOR_SIGNED_WRITE_CMD 0x03
#define ATT_WRITE_FOR_PREPARE          0x04


/* att common api */
bool attInit(void);
void attDeinit(void);
bool attSync(void); /* verifies all pending att work before now is finished. Use this after deleting things to make sure all refs to them are gone */

/* att server api */

/* these callbacks call attSrvReadCbkReply/attSrvWriteCbkReply on completion of actual work */
typedef bool (*attSrvValueReadCbk)(l2c_handle_t who, att_range_t range, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t maxLen);
typedef bool (*attSrvValueWriteCbk)(l2c_handle_t who, att_range_t range, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data);
typedef bool (*attSrvValueExecWrite)(l2c_handle_t who, int transId, bool exec);

typedef uint8_t (*attSrvTxCbk)(l2c_handle_t to, sg s); /* -> ATT_TX_RET_* */
typedef void (*attSrvMtuCbk)(l2c_handle_t to, uint32_t mtu);
typedef void (*attSrvIndResultCbk)(l2c_handle_t to, att_range_t rangeRef, uint16_t offset, bool success, uint64_t ref);

bool attSrvCreate(attSrvTxCbk txF, attSrvMtuCbk mtuF, attSrvIndResultCbk indF, attSrvValueExecWrite execF);
void attSrvSetMtu(l2c_handle_t to, uint32_t mtu); /* EDR uses this since L2C negotiates MTU */
void attSrvNotifConnClose(l2c_handle_t conn); /* allows server to no longer try to notify it */
void attSrvDestroy(void);

att_range_t attSrvHandleRangeReserve(uint16_t len); /* -> 0 on failure */
void attSrvHandleRangeRelease(att_range_t rangeRef);
uint16_t attSrvHandleRangeGetBase(att_range_t rangeRef); /* -> 0 on failure */
bool attSrvValueRegister(att_range_t rangeRef, uint16_t offset, const struct uuid *type, uint8_t perms,
                         uint16_t grpLen, attSrvValueReadCbk readF, attSrvValueWriteCbk writeF);
bool attSrvValueUnregister(att_range_t rangeRef, uint16_t offset);
uint8_t attSrvValueNotifyChanged(att_range_t rangeRef, uint16_t offset, l2c_handle_t to, sg data, bool notify, uint64_t ref); /* -> ATT_NOTIF_RET_* */
bool attSrvDataRx(l2c_handle_t from, sg s);

void attSrvCbkReply(int cid, int transId, uint16_t handle, uint8_t err, const void *data, uint16_t len); /* err is ATT_ERROR_* */


/* att client api */
//TODO


#endif
