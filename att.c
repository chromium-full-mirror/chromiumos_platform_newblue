//note: not allowed to sign packets on encrypted links
//commands an dnotifications can be dropped due to overflowing buffers and cannot be treated as reliable
//any transaction (packet requiring response) taking over 30 sec is a timeout and should serve to close the ATT bearer and refuse all further TX until new link is eastablished
#include <stdlib.h>
#include <string.h>
#include "l2cap.h"
#include "types.h"
#include "uniq.h"
#include "util.h"
#include "log.h"
#include "att.h"
#include "bt.h"
#include "sg.h"
#include "mt.h"





/* ERRORS */
#define ATT_OPCODE_ERROR                 0x01 /* struct attPduError */

/* REQUESTS */
#define ATT_OPCODE_MTU_EXCH_REQ          0x02 /* struct attPduMtu */
#define ATT_OPCODE_FIND_INFO_REQ         0x04 /* struct attPduFindInfoReq */
#define ATT_OPCODE_FIND_BY_TYPE_VAL_REQ  0x06 /* struct attPduFindByTypeValReq */
#define ATT_OPCODE_READ_BY_TYPE_REQ      0x08 /* struct attPduReadByTypeReq */
#define ATT_OPCODE_READ_REQ              0x0A /* struct attPduReadReq */
#define ATT_OPCODE_READ_BLOB_REQ         0x0C /* struct attPduReadBlobReq */
#define ATT_OPCODE_READ_MULT_REQ         0x0E /* uint16_t[] - at least two handles required */ */
#define ATT_OPCODE_READ_BY_GRP_TYPE_REQ  0x10 /* struct attPduReadByGrpTypeReq */
#define ATT_OPCODE_WRITE_REQ             0x12 /* struct attPduWriteReq */
#define ATT_OPCODE_PREPARE_WRITE_REQ     0x16 /* struct attPduPrepareWriteReq */
#define ATT_OPCODE_EXECUTE_WRITE_REQ     0x18 /* struct attPduExecuteWriteReq */

/* RESPONSES */
#define ATT_OPCODE_MTU_EXCH_RSP          0x03 /* struct attPduMtu */
#define ATT_OPCODE_FIND_INFO_RSP         0x05 /* struct attPduFindInfoRsp */
#define ATT_OPCODE_FIND_BY_TYPE_VAL_RSP  0x07 /* struct attPduFindByTypeValRspItem[] */
#define ATT_OPCODE_READ_BY_TYPE_RSP      0x09 /* struct attPduReadByTypeRsp */
#define ATT_OPCODE_READ_RSP              0x0B /* uint8_t[] */
#define ATT_OPCODE_READ_BLOB_RSP         0x0D /* uint8_t[] */
#define ATT_OPCODE_READ_MULT_RSP         0x0F /* uint8_t[] - values concatenated together */
#define ATT_OPCODE_READ_BY_GRP_TYPE_RSP  0x11 /* struct attPduReadByGrpTypeRsp */
#define ATT_OPCODE_WRITE_RSP             0x13 /* struct attPduWriteRsp */
#define ATT_OPCODE_PREPARE_WRITE_RSP     0x17 /* struct attPduPrepareWriteRsp */
#define ATT_OPCODE_EXECUTE_WRITE_RSP     0x19 /* NO ADDITIONAL DATA SENT */

/* NOTIFICATIONS */
#define ATT_OPCODE_HANDLE_VALUE_NOTIF    0x1B /* struct attPduHandleValueNotif */

/* INDICATIONS */
#define ATT_OPCODE_HANDLE_VALUE_IND      0x1D /* struct attPduHandleValueInd */

/* CONFRATIONS */
#define ATT_OPCODE_HANDLE_VALUE_CONF     0x1E /* NO ADDITIONAL DATA SENT */

/* COMMANDS */
#define ATT_OPCODE_WRITE_CMD             0x52 /* struct attPduWriteCmd */
#define ATT_OPCODE_SIGNED_WRITE_CMD      0xD2 /* struct attPduSignedWriteCmd */



#define ATT_FIND_INFO_RSP_FMT_16         0x01 /* attPduFindInfoData16 */
#define ATT_FIND_INFO_RSP_FMT_128        0x02 /* attPduFindInfoData128 */

#define ATT_EXECUTE_WRITE_FLG_CANCEL     0x00 /* cancel all prepared writes */
#define ATT_EXECUTE_WRITE_FLG_EXECUTE    0x01 /* perform all prepared writes */



struct attHdr {
    uint8_t opcode; /* ATT_OPCODE_* */
} __packed;

struct attPduError {
    uint8_t reqOpcode;
    uint16_t handle;
    uint8_t err; /* ATT_ERROR_* */
} __packed;

struct attPduMtu {
    uint16_t mtu;
} __packed;

struct attPduFindInfoReq {
    uint16_t startHandle;
    uint16_t endHandle;
}__packed;

struct attPduFindInfoData16 {
    uint16_t handle;
    uint16_t uuid16;
} __packed;

struct attPduFindInfoData128 {
    uint16_t handle;
    struct att_uuid uuid128;
} __packed;

struct attPduFindInfoRsp {
    uint8_t format; /* ATT_FIND_INFO_RSP_FMT_* */
    /* data here: attPduFindInfoData16[] or attPduFindInfoData128[] */
} __packed;

struct attPduFindByTypeValReq {
    uint16_t startHandle;
    uint16_t endHandle;
    uint16_t uuid16;
    /* value follows as a byte array */
} __packed;

struct attPduFindByTypeValRspItem {
    uint16_t firstHandle;
    uint16_t groupEndHandle; /* meaning defined by higher layer */
} __packed;

struct attPduReadByTypeReq {
    uint16_t startHandle;
    uint16_t endHandle;
    /* either a 16 or a 128 bit uuid follows */
} __packed;

struct attHandleValuePair {
    uint16_t handle;
    /* value bytes here */
} __packed;

struct attPduReadByTypeRsp {
    uint8_t handleValuePairSz;
    /* attHandleValuePair array follows */
} __packed;

struct attPduReadReq {
    uint16_t handle;
} __packed;

struct attPduReadBlobReq {
    uint16_t handle;
    uint16_t offset;
} __packed;

struct attPduReadByGrpTypeReq {
    uint16_t startHandle;
    uint16_t endHandle;
    /* either a 16 or a 128 bit uuid follows */
} __packed;

struct attGrpValuePair {
    uint16_t handle;
    uint16_t groupEndHadle;
    /* data here */
} __packed;

struct attPduReadByGrpTypeRsp {
    uint8_t attGrpValuePairSz;
    /* attGrpValuePair array follows */
} __packed;

struct attPduWriteReq {
    uint16_t handle;
    /* data follows */
} __packed;

struct attPduPrepareWriteReq {
    uint16_t handle;
    uint16_t offset;
    /* data */
} __packed;

struct attPduPrepareWriteRsp {
    uint16_t handle;
    uint16_t offset;
    /* data echo */
} __packed;

struct attPduExecuteWriteReq {
    uint8_t flags; /* ATT_EXECUTE_WRITE_FLG_* */
} __packed;

struct attPduHandleValueNotif {
    uint16_t handle;
    /* data */
} __packed;

struct attPduHandleValueInd {
    uint16_t handle;
    /* data */
} __packed;

struct attPduWriteCmd {
     uint16_t handle;
     /* data */
} __packed;

struct attPduSignedWriteCmd {
     uint16_t handle;
     /* data */
     /* 12 bytes of signature follow from pdu type byte to last data byte */
} __packed;


#define ATT_CONN_LIST_GROW_BY     16

struct attConnList {
    uint32_t listCap;
    uint32_t listNum;
    l2c_handle_t *listItems;
};

struct attVal {
    struct attVal *next;
    struct attVal *prev;
    struct att_uuid type;
    attValueReadCbk readF;
    attValueWriteCbk writeF;
    void *cbkData;
    uint16_t offset;
    uint8_t perms;

    struct attConnList notifs;
};

struct attRange {
    att_range_t rangeRef;
    struct attRange *next;
    struct attRange *prev;
    uint16_t start;
    uint16_t len;
    struct attVal *valsHead;
    struct attVal *valsTail;
    struct attConnList seenBy;
};

struct attServer {
    struct attRange *rangesHead;
    struct attRange *rangesTail;
    pthread_rwlock_t lock;
    attTxCbk txCbk;
    uint32_t mtu;
};










/*
 * FUNCTION: attUuidFromUuid16
 * USE:      convert a uuid-16 into a uuid-128
 * PARAMS:   dst - where to store the resulting uuid-128
 *           uuid16 - the incoming uuid16
 * RETURN:   NONE
 * NOTES:
 */
static void attUuidFromUuid16(struct att_uuid *dst, uint16_t uuid16)
{
    const struct att_uuid base = ATT_UUID_BASE;

    dst->lo = base.lo;
    dst->hi = base.hi + (((uint64_t)uuid16) << 32);
}

/*
 * FUNCTION: attUuidCmp
 * USE:      compare two uuid-128 values
 * PARAMS:   a - one value
 *           b - the other value
 * RETURN:   true if they are equal
 * NOTES:
 */
static bool attUuidCmp(const struct att_uuid *a, const struct att_uuid *b)
{
    return a->lo == b->lo && a->hi == b->hi;
}

/*
 * FUNCTION: attUuidToUuid16
 * USE:      try to convert a uuid-128 into a uuid-16
 * PARAMS:   dst - where to store the result
 *           uuid128 - the input
 * RETURN:   true if we succeeded
 * NOTES:
 */
static bool attUuidToUuid16(uint16_t *dst, const struct att_uuid *uuid128)
{
    const struct att_uuid base = ATT_UUID_BASE;

    if (uuid128->lo ^ base.lo)
        return false;
    if ((uuid128->hi ^ base.hi) & 0xFFFF0000FFFFFFFFULL)
        return false;
    *dst = base.hi >> 32;
    return true;
}

/*
 * FUNCTION: attConnListAdd
 * USE:      Add a given connection to a connection list, if it is not yet present there
 * PARAMS:   list - the list
 *           conn - the connection
 * RETURN:   false on error
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static bool attConnListAdd(struct attConnList *list, l2c_handle_t conn)
{
    uint32_t i;

    for (i = 0; i < list->listNum; i++)
        if (list->listItems[i] == conn)
            return true;

    if (list->listCap == list->listNum) {
        uint32_t newCap = list->listCap + ATT_CONN_LIST_GROW_BY;
        l2c_handle_t *t;

        t = realloc(list->listItems, sizeof(l2c_handle_t[newCap]));
        if (!t)
            return false;
        list->listCap = newCap;
        list->listItems = t;
    }
    list->listItems[list->listNum++] = conn;
    return true;
}

/*
 * FUNCTION: attConnListDel
 * USE:      Delete a given connection from a connection list, if it is present there
 * PARAMS:   list - the list
 *           conn - the connection
 * RETURN:   false on error
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static void attConnListDel(struct attConnList *list, l2c_handle_t conn)
{
    uint32_t i;

    for (i = 0; i < list->listNum; i++) {
        if (list->listItems[i] == conn) {
            list->listItems[i] = list->listItems[--list->listNum];
            break;
        }
    }
}

/*
 * FUNCTION: attConnListIsEmpty
 * USE:      Check if a given connection list is empty
 * PARAMS:   list - the list
 * RETURN:   true if it is empty
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static bool attConnListIsEmpty(const struct attConnList *list)
{
    return !list->listNum;
}

/*
 * FUNCTION: attConnListFree
 * USE:      Free the storage occupied by a given connection list
 * PARAMS:   list - the list
 * RETURN:   NONE
 * NOTES:    IDEMPOTENT, attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static void attConnListFree(struct attConnList *list)
{
    if (list->listItems)
        free(list->listItems);
    list->listItems = NULL;
}

/*
 * FUNCTION: attSrvCreate
 * USE:      create an ATT server
 * PARAMS:   txF - callback to use to TX data to clients
 *           defaultMtu - default MTU to use
 * RETURN:   the server struct or NULL on error
 * NOTES:
 */
struct attServer* attSrvCreate(attTxCbk txF, uint32_t defaultMtu)
{
    struct attServer *ret = (struct attServer*)calloc(1, sizeof(struct attServer));

    if (!ret)
        return NULL;

    if (pthread_rwlock_init(&ret->lock, NULL)) {
        free(ret);
        return NULL;
    }

    ret->txCbk = txF;
    ret->mtu = defaultMtu;
    return ret;
}

/*
 * FUNCTION: attSrvDestroy
 * USE:      destroy an ATT server
 * PARAMS:   srv - the server
 * RETURN:   NONE
 * NOTES:
 */
void attSrvDestroy(struct attServer* srv)
{
    struct attRange *range;
    struct attVal *val;

    pthread_rwlock_wrlock(&srv->lock);
    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; ) {
            struct attVal *t = val;
            val = val->next;
            attConnListFree(&t->notifs);
            free(t);
        }
        attConnListFree(&range->seenBy);
    }
    pthread_rwlock_unlock(&srv->lock);
    free(srv);
}

/*
 * FUNCTION: attRangeDelete
 * USE:      delete al values in a given range
 * PARAMS:   range - the range
 * RETURN:   NONE
 * NOTES:    call with server's write lock held
 */
static void attRangeDeleteAllVals(struct attRange *range)
{
    struct attVal *val, *t;

    val = range->valsHead;
    while (val) {
        t = val;
        val = val->next;
        if (!attConnListIsEmpty(&t->notifs))
            logw("non-empty notif list in deleted value 0x%04x+0x%04x\n", range->start, t->offset);
        attConnListFree(&t->notifs);
        free(t);
    }
    range->valsHead = NULL;
    range->valsTail = NULL;
}

/*
 * FUNCTION: attRangeDelete
 * USE:      delete a given att range struct
 * PARAMS:   srv - the server
 *           range - the range
 * RETURN:   NONE
 * NOTES:    call with server's write lock held
 */
static void attRangeDelete(struct attServer* srv, struct attRange *range)
{

    if (range->prev)
        range->prev->next = range->next;
    else
        srv->rangesHead = range->next;

    if (range->next)
        range->next->prev = range->prev;
    else
        srv->rangesTail = range->prev;

    attRangeDeleteAllVals(range);
    if (!attConnListIsEmpty(&range->seenBy))
        logw("Deleted range 0x%04x+0x%04x has been seen\n", range->start, range->len);
    attConnListFree(&range->seenBy);
    free(range);
}

/*
 * FUNCTION: attSrvNotifConnClose
 * USE:      tell an ATT server about a closed connection (in case it cares)
 * PARAMS:   srv - the server
 *           conn - the connection
 * RETURN:   NONE
 * NOTES:    if said connection substribed to notifications, allows us to unsubstribe
 *           them proactively instead of waiting for TX errors
 */
void attSrvNotifConnClose(struct attServer* srv, l2c_handle_t conn)
{
    struct attRange *range, *t;
    struct attVal *val;
    uint32_t i;

    pthread_rwlock_wrlock(&srv->lock);
    for (range = srv->rangesHead; range;) {
        for (val = range->valsHead; val; val = val->next) {
            attConnListDel(&val->notifs, conn);
        }
        attConnListDel(&range->seenBy, conn);
        t = range;
        range = range->next;
        if (attConnListIsEmpty(&t->seenBy) && !t->rangeRef) /* delete a disused range after all who knew about it are gone */
            attRangeDelete(srv, t);
    }
    pthread_rwlock_unlock(&srv->lock);
}

/*
 * FUNCTION: attSrvHandleRangeReserve
 * USE:      reserve a range of handles in the ATT server
 * PARAMS:   srv - the server
 *           len - the wanted handle range length
 * RETURN:   a reference to the handle range or 0 on failure
 * NOTES:
 */
att_range_t attSrvHandleRangeReserve(struct attServer* srv, uint16_t len)
{
    uint32_t searchH = ATT_FIRST_VALID_HANDLE;
    struct attRange *next, *prev = NULL, *newRange;
    att_range_t ret = 0;

    pthread_rwlock_wrlock(&srv->lock);
    next = srv->rangesHead;
    while (searchH <= ATT_LAST_VALID_HANDLE && next) {
        if (next->start - searchH >= len)
            break;
        else {
            searchH = next->start + next->len;
            prev = next;
            next = next->next;
        }
    }
    if (searchH + len - 1 > ATT_LAST_VALID_HANDLE)
        goto out;

    /* next is currently the next item for us, or NULL if we're the new last */
    /* prev is currently the prev item for us, or NULL if we're the new first */

    newRange = (struct attRange*)calloc(1, sizeof(struct attRange));
    if (!newRange)
        goto out;

    newRange->start = searchH;
    newRange->len = len;
    newRange->rangeRef = uniqGetNext();
    newRange->prev = prev;
    newRange->next = next;
    if (next)
        next->prev = newRange;
    else
        srv->rangesTail = newRange;
    if (prev)
        prev->next = newRange;
    else
        srv->rangesHead = newRange;

    ret = newRange->rangeRef;
out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}

/*
 * FUNCTION: attRangeFind
 * USE:      find a range structure for a given range reference in a given ATT server
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 * RETURN:   the range struct or NULL if not found
 * NOTES:    call with srv->lock held
 */
static struct attRange* attRangeFind(struct attServer* srv, att_range_t rangeRef)
{
    struct attRange *range;

    for (range = srv->rangesHead; range && range->rangeRef != rangeRef; range = range->next);

    return range;
}

/*
 * FUNCTION: attValFind
 * USE:      find a value structure for a given range structure and a given offset
 * PARAMS:   range - the range
 *           offset - the offset
 * RETURN:   the value struct or NULL if not found
 * NOTES:    call with srv->lock held
 */
static struct attVal* attValFind(struct attRange *range, uint16_t offset)
{
    struct attVal *val;

    for (val = range->valsHead; val && val->offset != offset; val = val->next);

    return val;
}

/*
 * FUNCTION: attValRangeFind
 * USE:      find a value structure and a range structure for a ATT server, a range handle, and an offset
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - the offset
 *           rangeP - range gets stored here
 * RETURN:   the value struct or NULL if not found
 * NOTES:    call with srv->lock held
 */
static struct attVal* attValRangeFind(struct attServer* srv, att_range_t rangeRef, uint16_t offset, struct attRange **rangeP)
{
    *rangeP = attRangeFind(srv, rangeRef);
    if (*rangeP)
        return  attValFind(*rangeP, offset);
    return NULL;
}

/*
 * FUNCTION: attSrvHandleRangeRelease
 * USE:      release a range of handles in the ATT server
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 * RETURN:   NONE
 * NOTES:
 */
void attSrvHandleRangeRelease(struct attServer* srv, att_range_t rangeRef)
{
    struct attRange *range;

    pthread_rwlock_wrlock(&srv->lock);
    range = attRangeFind(srv, rangeRef);
    if (!range)
        logw("Attempt to delete a non-existent range\n");
    else {
        if (attConnListIsEmpty(&range->seenBy))
            attRangeDelete(srv, range);
        else {
            attRangeDeleteAllVals(range);
            range->rangeRef = 0;
        }
    }
    pthread_rwlock_unlock(&srv->lock);
}

/*
 * FUNCTION: attSrvValueRegister
 * USE:      Register a given value in a given range
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - handle's offset from start of the range
 *           type - the type of the registered value
 *           perms - permissions (ATT_PERM_*)
 *           readF - the value read callback (may be NULL if read not supported)
 *           writeF - the value write callback (may be NULL if write not supported)
 *           cbkData - data to pass to readF and writeF when calling them
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueRegister(struct attServer* srv, att_range_t rangeRef, uint16_t offset, const struct att_uuid *type, uint8_t perms, attValueReadCbk readF, attValueWriteCbk writeF, void *cbkData)
{
    struct attVal *next, *prev = NULL, *val;
    struct attRange *range;
    bool ret = false;

    if ((perms & ATT_PERM_READ) && !readF) {
        logw("Cannot add a readable value with no read callback\n");
        return false;
    }
    if ((perms & ATT_PERM_WRITE) && !writeF) {
        logw("Cannot add a writeable value with no write callback\n");
        return false;
    }

    pthread_rwlock_wrlock(&srv->lock);
    range = attRangeFind(srv, rangeRef);
    if (!range) {
        logw("Attempt to add a value to a non-existent range\n");
        goto out;
    }

    for (next = range->valsHead; next && next->offset < offset; prev = next, next = next->next);
    /*
     * prev is our prev or NULL if we're first
     * next is our next or NULL if we're last
     * If this is a duplication, next is the item we're trying to duplicate
     */

    if (next->offset == offset) {
        logw("Attempt to create duplicate-offset value (0x%04X+0x%04X\n", range->start, offset);
        goto out;
    }

    val = (struct attVal*)calloc(1, sizeof(struct attVal));
    if (!val)
        goto out;

    memcpy(&val->type, type, sizeof(val->type));
    val->readF = readF;
    val->writeF = writeF;
    val->cbkData = cbkData;
    val->offset = offset;
    val->perms = perms;
    val->next = next;
    val->prev = prev;
    if (next)
        next->prev = val;
    else
        range->valsTail = val;
    if (prev)
        prev->next = val;
    else
        range->valsHead = val;
    ret = true;

out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}

/*
 * FUNCTION: attSrvValueUnregister
 * USE:      Unregister a given value in a given range
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - handle's offset from start of the rangeted)
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueUnregister(struct attServer* srv, att_range_t rangeRef, uint16_t offset)
{
    struct attRange *range;
    struct attVal *val;
    bool ret = false;

    pthread_rwlock_wrlock(&srv->lock);
    val = attValRangeFind(srv, rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to remove a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to remove a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    attConnListFree(&val->notifs);
    if (val->next)
        val->next->prev = val->prev;
    else
        range->valsTail = val->prev;
    if (val->prev)
        val->prev->next = val->next;
    else
        range->valsHead = val->next;
    free(val);
    ret = true;

out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}

/*
 * FUNCTION: attSrvValueNotifyRegister
 * USE:      Register a client to be notified of a value's value change
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - handle's offset from start of the range
 *           conn - the client connection
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueNotifyRegister(struct attServer* srv, att_range_t rangeRef, uint16_t offset, l2c_handle_t conn)
{
    struct attRange *range;
    struct attVal *val;
    bool ret = false;

    pthread_rwlock_wrlock(&srv->lock);
    val = attValRangeFind(srv, rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to subscribe-notify on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to subscribe-notify on a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }
    ret = attConnListAdd(&val->notifs, conn);

out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}

/*
 * FUNCTION: attSrvSendNotif
 * USE:      Send notifications to all clients who wanted them for this value
 * PARAMS:   srv - the server
 *           range - the range
 *           val - the value
 * RETURN:   NONE
 * NOTES:
 */
static void attSrvSendNotif(struct attServer* srv, struct attRange *range, struct attVal *val)
{
    //TODO: go throgh val->notifs and tell eberyone. on failure remove it from all othe rnotifs in server too...
}

/*
 * FUNCTION: attSrvValueUnregister
 * USE:      Unregister a given value in a given range
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - handle's offset from start of the range
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueNotifyChanged(struct attServer* srv, att_range_t rangeRef, uint16_t offset)
{
    struct attRange *range;
    struct attVal *val;
    bool ret = false;
    uint32_t i;

    pthread_rwlock_rdlock(&srv->lock);
    val = attValRangeFind(srv, rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to notify on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to notify on a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    attSrvSendNotif(srv, range, val);
    ret = true;

out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}



















