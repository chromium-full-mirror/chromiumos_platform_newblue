/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _ATT_H_
#define _ATT_H_

#include "newblue-macros.h"
#include "l2cap.h"
#include "sm.h"
#include "types.h"
#include "uuid.h"
#include "uniq.h"

NEWBLUE_BEGIN_DECLS

typedef uniq_t att_range_t;
typedef uniq_t att_cid_t;
typedef uniq_t att_trans_t;

#define ATT_CID_FMT      UNIQ_FMT
#define ATT_CID_CONV(x)  UNIQ_CONV(x)

#define ATT_TRANS_FMT      UNIQ_FMT
#define ATT_TRANS_CONV(x)  UNIQ_CONV(x)


/* we WILL negotiate, but only this much is ever guaranteed */
#define ATT_MTU_MIN_EDR                48
#define ATT_MTU_MIN_LE                 23

#define ATT_SIG_LEN                    SM_SIG_LEN

#define ATT_PERM_READ                  0x01
#define ATT_PERM_READ_ENCR             0x02
#define ATT_PERM_READ_ENCR_MITM        0x04
#define ATT_PERM_WRITE                 0x08
#define ATT_PERM_WRITE_ENCR            0x10
#define ATT_PERM_WRITE_ENCR_MITM       0x20
#define ATT_PERM_WRITE_SIGNED          0x40

/* other permissions shall be handled by higher-layer code */

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

/* ind/notif srv events */
#define ATT_SRV_EVT_NOTIF_SENT         1
#define ATT_SRV_EVT_IND_ACKED          2
#define ATT_SRV_EVT_IND_TIMEOUT        3
#define ATT_SRV_EVT_IND_SRV_DESTROYED  4 /* could happen before a timeout */

/* att common api */
typedef void (*attMtuCbk)(l2c_handle_t to, uint32_t mtu);

bool attInit(void) NEWBLUE_EXPORT;
void attDeinit(void) NEWBLUE_EXPORT;
bool attSync(void); /* verifies all pending att work before now is finished. Use this after deleting things to make sure all refs to them are gone */
void attSetMtu(l2c_handle_t to, uint32_t mtu); /* EDR uses this since L2C negotiates MTU */
void attNotifConnClose(l2c_handle_t conn); /* allows server to no longer try to notify it */
bool attEncrAuthUpdate(l2c_handle_t who, bool encr, bool mitmSafe);

/* att server api */

/* these callbacks call attSrvCbkReply() on completion of actual work */
typedef bool (*attSrvValueReadCbk)(l2c_handle_t who, att_cid_t cid, att_range_t range, uint16_t ofst, att_trans_t transId, uint16_t byteOfst, uint8_t reason, uint16_t maxLen);
typedef bool (*attSrvValueWriteCbk)(l2c_handle_t who, att_cid_t cid, att_range_t range, uint16_t ofst, att_trans_t transId, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data);
typedef bool (*attSrvValueExecWriteCbk)(l2c_handle_t who, att_cid_t cid, att_trans_t transId, bool exec);

typedef uint8_t (*attSrvTxCbk)(l2c_handle_t to, sg s); /* -> ATT_TX_RET_* */

typedef void (*attSrvIndResultCbk)(l2c_handle_t to, att_cid_t cid, att_range_t rangeRef, uint16_t offset, uint8_t evt, uint64_t ref);

void attSrvInit(attSrvTxCbk txF, attMtuCbk srvMtuF, attSrvIndResultCbk indF, attSrvValueExecWriteCbk execF, attSrvValueReadCbk readF, attSrvValueWriteCbk writeF);
void attSrvDeinit(void);

l2c_handle_t attSrvCidResolve(att_cid_t cid) NEWBLUE_EXPORT; //or 0 on failure

att_range_t attSrvHandleRangeReserve(uint16_t len); /* -> 0 on failure */
void attSrvHandleRangeRelease(att_range_t rangeRef);
uint16_t attSrvHandleRangeGetBase(att_range_t rangeRef); /* -> 0 on failure */
uint16_t attSrvHandleRangeGetLen(att_range_t rangeRef);
bool aapiSrvHandleRangeSetAllowedTransports(att_range_t rangeRef, bool allowLE, bool allowEDR);
bool attSrvValueRegister(att_range_t rangeRef, uint16_t offset, const struct uuid *type, uint8_t perms);
bool attSrvValueSetGrpLen(att_range_t rangeRef, uint16_t offset, uint16_t grpLen);
bool attSrvValueGetGrpLen(att_range_t rangeRef, uint16_t offset, uint16_t *grpLenP);
bool attSrvValueUnregister(att_range_t rangeRef, uint16_t offset);
uint8_t attSrvValueNotifyChanged(att_range_t rangeRef, uint16_t offset, att_cid_t cid, sg data, bool notify, uint64_t ref); /* -> ATT_NOTIF_RET_* */
void attSrvDataRx(l2c_handle_t from, sg s);


void attSrvCbkReply(att_cid_t cid, att_trans_t transId, uint16_t handle, uint8_t err, const void *data, uint16_t len); /* err is ATT_ERROR_* */


/* att client api */
/* haveMore is used for requests that can return more than one item per read, handleP & endGrpHandleP are non-null when available. return false to get no more even if haveMore is true */
typedef bool (*attCliFindInfoCbk)(uniq_t trans, l2c_handle_t to, uint16_t handle, const struct uuid *uuid, bool haveMore);
typedef bool (*attCliFindByTypeValCbk)(uniq_t trans, l2c_handle_t to, uint16_t handle, uint16_t grpEndHandle, bool haveMore);
typedef bool (*attCliReadCbk)(uniq_t trans, l2c_handle_t to, const uint16_t *handleP, const uint16_t *endGrpHandleP, sg data, bool haveMore);
typedef void (*attCliWriteCbk)(uniq_t trans, l2c_handle_t to); /* prepare write data is not sent to you */
typedef void (*attCliError)(uniq_t trans, l2c_handle_t to, const uint16_t *handleP, const uint8_t *errP, const uint8_t *errOpcodeP); /* will be called with NULLs if case of timeout */
typedef void (*attCliIndNotif)(l2c_handle_t to, bool needsAck, uint16_t handle, sg data);

void attCliInit(attMtuCbk cliMtuF, attCliFindInfoCbk cliFindInfoCbk, attCliFindByTypeValCbk cliFindByTypeValCbk, attCliReadCbk cliReadF, attCliWriteCbk cliWriteF, attCliError cliErrF, attCliIndNotif cliIndNotifF);
void attCliDeinit(void);

bool attCliRequestMtu(l2c_handle_t to, uint32_t mtu); /* LE only. once per connection only */
bool attClientInvalidateCache(l2c_handle_t to);

/* these all return the Id of this transaction and callback will have it too for matching. zero is returned in case of failure to start operation */
uniq_t attCliFindInformation(l2c_handle_t to, uint16_t hStart, uint16_t hEnd);
uniq_t attCliFindByTypeValue(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, uint16_t typeUuid16, sg value);
uniq_t attCliReadByType(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, const struct uuid *type);
uniq_t attCliReadByGroupType(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, const struct uuid *type);
uniq_t attCliRead(l2c_handle_t to, uint16_t handle);
uniq_t attCliReadBlob(l2c_handle_t to, uint16_t handle, uint16_t offset);
uniq_t attCliWrite(l2c_handle_t to, uint16_t handle, sg data);
bool attCliWriteNoAck(l2c_handle_t to, uint16_t handle, sg data);
bool attCliSignedWriteNoAck(l2c_handle_t to, uint16_t handle, sg data);
uniq_t attCliPrepareWrite(l2c_handle_t to, uint16_t handle, uint16_t offset, sg data);
uniq_t attCliExecuteStagedWrites(l2c_handle_t to, bool commit);
bool attCliSendIndicationAck(l2c_handle_t to);



/* completely stateless helpers */
bool attReadUuid(struct uuid *dst, sg s); //read uuid-16 or uuid-128 from an sg assuming it is the only thing in there

NEWBLUE_END_DECLS

#endif
