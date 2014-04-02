//note: not allowed to sign packets on encrypted links
//commands an dnotifications can be dropped due to overflowing buffers and cannot be treated as reliable
//any transaction (packet requiring response) taking over 30 sec is a timeout and should serve to close the ATT bearer and refuse all further TX until new link is eastablished
#include <stdlib.h>
#include <string.h>
#include "persist.h"
#include "l2cap.h"
#include "types.h"
#include "uniq.h"
#include "util.h"
#include "log.h"
#include "att.h"
#include "bt.h"
#include "sg.h"
#include "mt.h"
#include "sm.h"


/* ERRORS */
#define ATT_OPCODE_ERROR                 0x01 /* struct attPduError */

/* REQUESTS */
#define ATT_OPCODE_MTU_EXCH_REQ          0x02 /* struct attPduMtu */
#define ATT_OPCODE_FIND_INFO_REQ         0x04 /* struct attPduFindInfoReq */
#define ATT_OPCODE_FIND_BY_TYPE_VAL_REQ  0x06 /* struct attPduFindByTypeValReq */
#define ATT_OPCODE_READ_BY_TYPE_REQ      0x08 /* struct attPduReadByTypeReq */
#define ATT_OPCODE_READ_REQ              0x0A /* struct attPduReadReq */
#define ATT_OPCODE_READ_BLOB_REQ         0x0C /* struct attPduReadBlobReq */
#define ATT_OPCODE_READ_MULT_REQ         0x0E /* uint16_t[] - at least two handles required */
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

/* CONFIRMATION */
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
    l2c_handle_t *conns;
    uint32_t *mtus;
    bool hasMtus;
};

struct attVal {
    struct attVal *next;
    struct attVal *prev;
    struct att_uuid type;
    attValueReadCbk readF;
    attValueWriteCbk writeF;
    void *cbkData;
    uint16_t offset;
    uint16_t grpLen;
    uint8_t perms;
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
    attMtuCbk mtuCbk;
    struct attConnList allClients;
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
 * FUNCTION: attUuidWrite
 * USE:      Write a UUID-128 (for TX path)
 * PARAMS:   dst - where to write
 *           src - where to get the value
 * RETURN:   true if they are equal
 * NOTES:
 */
static void attUuidWrite(struct att_uuid *dst, const struct att_uuid *src)
{
    utilSetLE64(&dst->lo, src->lo);
    utilSetLE64(&dst->hi, src->hi);
}

/*
 * FUNCTION: attUuidRead
 * USE:      Read a UUID-128 (for RX path)
 * PARAMS:   dst - where to read to
 *           src - where to get the value
 * RETURN:   true if they are equal
 * NOTES:
 */
static void attUuidRead(struct att_uuid *dst, const struct att_uuid *src)
{
    dst->lo = utilGetLE64(&src->lo);
    dst->hi = utilGetLE64(&src->hi);
}

/*
 * FUNCTION: attUuidToUuid16
 * USE:      try to convert a uuid-128 into a uuid-16
 * PARAMS:   dst - where to store the result or NULL if we don't need it
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
    if (dst)
        *dst = base.hi >> 32;
    return true;
}

/*
 * FUNCTION: attConnListAdd
 * USE:      Add a given connection to a connection list, if it is not yet present there. update MTU if list has them
 * PARAMS:   list - the list
 *           conn - the connection
 *           mtu - the mtu to store (or 0 if none or not known)
 * RETURN:   false on error
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static bool attConnListAdd(struct attConnList *list, l2c_handle_t conn, uint32_t mtu)
{
    uint32_t i;

    if (!mtu && list->hasMtus) {
        loge("Adding connection with no MTU to a list with MTUs not supported\n");
        return false;
    }

    for (i = 0; i < list->listNum; i++) {
        if (list->conns[i] == conn) {
            if (list->hasMtus && mtu)
                list->mtus[i] = mtu;
            return true;
        }
    }

    if (list->listCap == list->listNum) {
        uint32_t newCap = list->listCap + ATT_CONN_LIST_GROW_BY;
        l2c_handle_t *conns;
        uint32_t *mtus;

        conns = realloc(list->conns, sizeof(l2c_handle_t[newCap]));
        if (!conns)
            return false;
        list->conns = conns;

        if (list->hasMtus) {
            mtus = realloc(list->mtus, sizeof(uint32_t[newCap]));
            if (!mtus)
                return false;
            list->mtus = mtus;
        }

        list->listCap = newCap;
    }
    list->conns[list->listNum] = conn;
    if (list->hasMtus)
        list->mtus[list->listNum] = mtu;
    list->listNum++;
    return true;
}

/*
 * FUNCTION: attConnListDel
 * USE:      Delete a given connection from a connection list, if it is present there
 * PARAMS:   list - the list
 *           conn - the connection
 * RETURN:   true if deleted
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static bool attConnListDel(struct attConnList *list, l2c_handle_t conn)
{
    uint32_t i;

    for (i = 0; i < list->listNum; i++) {
        if (list->conns[i] == conn) {
            list->listNum--;
            list->conns[i] = list->conns[list->listNum];
            if (list->hasMtus)
                list->mtus[i] = list->mtus[list->listNum];
            return true;
        }
    }

    return false;
}

/*
 * FUNCTION: attConnListGetMtu
 * USE:      Retrieve a given connection's MTU from a connection list, if it is present there
 * PARAMS:   list - the list
 *           conn - the connection
 * RETURN:   the MTU if found, else 0
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static uint32_t attConnListGetMtu(struct attConnList *list, l2c_handle_t conn)
{
    uint32_t i;

    for (i = 0; i < list->listNum; i++)
        if (list->conns[i] == conn)
            return list->hasMtus ? list->mtus[i] : 0;

    return 0;
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
    if (list->conns)
        free(list->conns);
    if (list->mtus)
        free(list->mtus);
    list->conns = NULL;
}

/*
 * FUNCTION: attSrvCreate
 * USE:      create an ATT server
 * PARAMS:   txF - callback to use to TX data to clients
 * RETURN:   the server struct or NULL on error
 * NOTES:
 */
struct attServer* attSrvCreate(attTxCbk txF, attMtuCbk mtuF)
{
    struct attServer *ret = (struct attServer*)calloc(1, sizeof(struct attServer));

    if (!ret)
        goto out_alloc;

    if (pthread_rwlock_init(&ret->lock, NULL))
        goto out_lock;

    ret->txCbk = txF;
    ret->mtuCbk = mtuF;
    return ret;


out_lock:
    free(ret);

out_alloc:
    return NULL;
}

/*
 * FUNCTION: attSrvDestroy
 * USE:      destroy an ATT server
 * PARAMS:   srv - the server
 * RETURN:   NONE
 * NOTES:
 */
void attSrvDestroy(struct attServer *srv)
{
    struct attRange *range;
    struct attVal *val;

    pthread_rwlock_wrlock(&srv->lock);
    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; ) {
            struct attVal *t = val;
            val = val->next;
            free(t);
        }
        attConnListFree(&range->seenBy);
    }
    attConnListFree(&srv->allClients);
    pthread_rwlock_unlock(&srv->lock);
    pthread_rwlock_destroy(&srv->lock);
    free(srv);
}

/*
 * FUNCTION: attRangeDelete
 * USE:      delete all values in a given range
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
static void attRangeDelete(struct attServer *srv, struct attRange *range)
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
void attSrvNotifConnClose(struct attServer *srv, l2c_handle_t conn)
{
    struct attRange *range, *t;
    struct attVal *val;
    uint32_t i;

    pthread_rwlock_wrlock(&srv->lock);
    for (range = srv->rangesHead; range;) {
        attConnListDel(&range->seenBy, conn);
        t = range;
        range = range->next;
        if (attConnListIsEmpty(&t->seenBy) && !t->rangeRef) /* delete a disused range after all who knew about it are gone */
            attRangeDelete(srv, t);
    }
    attConnListDel(&srv->allClients, conn);
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
att_range_t attSrvHandleRangeReserve(struct attServer *srv, uint16_t len)
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
 * NOTES:    call with srv->lock held for read
 */
static struct attRange* attRangeFind(struct attServer *srv, att_range_t rangeRef)
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
 * NOTES:    call with srv->lock held for read
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
 * NOTES:    call with srv->lock held for read
 */
static struct attVal* attValRangeFind(struct attServer *srv, att_range_t rangeRef, uint16_t offset, struct attRange **rangeP)
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
void attSrvHandleRangeRelease(struct attServer *srv, att_range_t rangeRef)
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
 *           grpLen - if this value is start of a group, how many values the group is (0 if not start of group)
 *           readF - the value read callback (may be NULL if read not supported)
 *           writeF - the value write callback (may be NULL if write not supported)
 *           cbkData - data to pass to readF and writeF when calling them
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueRegister(struct attServer *srv, att_range_t rangeRef, uint16_t offset, const struct att_uuid *type, uint8_t perms, uint16_t grpLen, attValueReadCbk readF, attValueWriteCbk writeF, void *cbkData)
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

    if (offset >= range->len || offset + grpLen > range->len) {
        logw("Value or group end out of range\n");
        goto out;
    }

    for (next = range->valsHead; next && next->offset < offset; prev = next, next = next->next);
    /*
     * prev is our prev or NULL if we're first
     * next is our next or NULL if we're last
     * If this is a duplication, next is the item we're trying to duplicate
     */

    if (next->offset == offset) {
        logw("Attempt to create duplicate-offset value (0x%04X+0x%04X)\n", range->start, offset);
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
    val->grpLen = grpLen;
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
bool attSrvValueUnregister(struct attServer *srv, att_range_t rangeRef, uint16_t offset)
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
 * FUNCTION: attGetMtu
 * USE:      Get the MTU to someone
 * PARAMS:   srv - the server
 *           to - whom to
 *           tryDefaults - try to find the default to use?
 * RETURN:   the mtu to use or 0 on error
 * NOTES:    call with srv->lock held for read
 */
static uint32_t attGetMtu(struct attServer *srv, l2c_handle_t to, bool tryDefaults)
{
    uint32_t mtu;

    mtu = attConnListGetMtu(&srv->allClients, to);
    if (!mtu && tryDefaults) {
        struct bt_addr peer;

        logw("Sending notif with no known MTU\n");
        if (!l2cApiGetBtAddr(to, &peer)) {
            logw("Failed to get peer address, not sending notification there\n");
            return 0;
        }
        mtu = BT_ADDR_IS_EDR(peer) ? ATT_MTU_MIN_EDR : ATT_MTU_MIN_LE;
    }

    return mtu;
}

/*
 * FUNCTION: attSrvSend
 * USE:      Sends an assembled packet to the peer
 * PARAMS:   srv - the server
 *           to - whom the data is to be sent to
 *           preData - first part of the packet or NULL
 *           preLen - length of fais data
 *           data - the middle of the packet or NULL
 *           postData - last part of the packet or NULL
 *           postLen - length of said data
 * RETURN:   ATT_TX_RET_*
 * NOTES:    call with srv->lock held for read
 */
static uint8_t attSrvSend(struct attServer *srv, l2c_handle_t to, const void *preData, uint32_t preLen, sg data, const void *postData, uint32_t postLen)
{
    uint8_t ret;
    sg tmpData = NULL;

    if (!data) {
        tmpData = data = sgNew();
        if (!tmpData)
            return ATT_TX_RET_OOM;
    }

    if (preLen && !sgConcatFrontCopy(data, preData, preLen)) {
        if (tmpData)
            sgFree(tmpData);
        return ATT_TX_RET_OOM;
    }

    if (postLen && !sgConcatBackCopy(data, preData, preLen)) {
        if (tmpData)
            sgFree(tmpData);
        else
            sgTruncFront(data, preLen);
        return ATT_TX_RET_OOM;
    }

    ret = srv->txCbk(srv, to, data);
    if (ret == ATT_TX_RET_ACCEPTED)
        return ret;

    if (tmpData)
        sgFree(tmpData);
    else {
        sgTruncBack(data, postLen);
        sgTruncFront(data, preLen);
    }
    return ret;
}

/*
 * FUNCTION: attSrvSendNotif
 * USE:      Send notifications to all clients who wanted them for this value
 * PARAMS:   srv - the server
 *           range - the range
 *           val - the value struct
 *           to - whom to send to
 *           data - the value
 *           notify - send notification (as opposed to indication)
 * RETURN:   L2C_TX_*
 * NOTES:    call with srv->lock held for read
 */
static uint8_t attSrvSendNotif(struct attServer *srv, struct attRange *range, struct attVal *val, l2c_handle_t to, sg data, bool notify)
{
    uint8_t hdrBuf[sizeof(struct attHdr) + sizeof(struct attPduHandleValueNotif) + sizeof(struct attPduHandleValueInd)]; /* definitely big enoguh to fit the fist and either second or third */
    struct attHdr *hdr = (struct attHdr*)hdrBuf;
    struct attPduHandleValueNotif *notifH = (struct attPduHandleValueNotif*)(hdr + 1);
    struct attPduHandleValueInd *indH = (struct attPduHandleValueInd*)(hdr + 1);
    uint8_t hdrLen = sizeof(*hdr);
    uint32_t mtu;

    if (notify) {
        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_NOTIF);
        utilSetLE16(&notifH->handle, val->offset + range->start);
        hdrLen += sizeof(*notifH);
    } else {
        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_IND);
        utilSetLE16(&indH->handle, val->offset + range->start);
        hdrLen += sizeof(*indH);
    }

    mtu = attGetMtu(srv, to, true);
    if (!mtu)
        return ATT_TX_RET_ATT_ERROR;

    mtu -= hdrLen;
    if (sgLength(data) > mtu) {
        logd("Truncating wanted notif/ind data to %ub\n", mtu);
        sgTruncBack(data, sgLength(data) - mtu);
    }

    return attSrvSend(srv, to, hdrBuf, hdrLen, data, NULL, 0);
}

/*
 * FUNCTION: attSrvValueNotifyChanged
 * USE:      Sen a notification or an indication
 * PARAMS:   srv - the server
 *           rangeRef - the range reference
 *           offset - handle's offset from start of the range
 *           to - whom to send to
 *           data - the data to send
 *           notify - send notification (as opposed to indication)
 * RETURN:   ATT_TX_RET_*
 * NOTES:    call with srv->lock held for read
 */
uint8_t attSrvValueNotifyChanged(struct attServer *srv, att_range_t rangeRef, uint16_t offset, l2c_handle_t to, sg data, bool notify)
{
    struct attRange *range;
    struct attVal *val;
    uint8_t ret = ATT_TX_RET_ATT_ERROR;
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

    ret = attSrvSendNotif(srv, range, val, to, data, notify);

out:
    pthread_rwlock_unlock(&srv->lock);
    return ret;
}

/*
 * FUNCTION: attSrvSetMtu
 * USE:      Called to set the MTU for an ATT server to a given client
 * PARAMS:   srv - the server
 *           to - whom the MTU is to
 *           mtu - the mtu
 * RETURN:   NONE
 * NOTES:    used by EDR only. LE does its own negotiation
 */
void attSrvSetMtu(struct attServer *srv, l2c_handle_t to, uint32_t mtu)
{
    pthread_rwlock_wrlock(&srv->lock);
    attConnListAdd(&srv->allClients, to, mtu);
    pthread_rwlock_unlock(&srv->lock);
}

/*
 * FUNCTION: attReadUuid
 * USE:      Read a UUID-128 or a UUID16 from an sg, if it is the only thing there
 * PARAMS:   dst - we store the read UUID here
 *           s - the data
 * RETURN:   success
 * NOTES:
 */
static bool attReadUuid(struct att_uuid *dst, sg s)
{
    uint8_t buf[16];
    struct att_uuid *ru = (struct att_uuid*)buf;

    if (sgLength(s) == sizeof(uint16_t)) {
        sgSerializeCutFront(s, buf, sizeof(uint16_t));
        attUuidFromUuid16(dst, utilGetLE16(buf));
    } else if (sgLength(s) == sizeof(struct att_uuid)) {
        sgSerializeCutFront(s, buf, sizeof(struct att_uuid));
        attUuidRead(dst, ru);
    } else
        return false;

    return true;
}

/*
 * FUNCTION: attSrvSendErrorReply
 * USE:      Sends an error to the client
 * PARAMS:   srv - the server
 *           to - whom to send to
 *           err - the error
 *           opcode - opcode for which the error happened
 *           handle - the pertinent handle
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvSendErrorReply(struct attServer *srv, l2c_handle_t to, uint8_t errNo, uint8_t opcode, uint16_t handle)
{
    struct attPduError err;
    struct attHdr hdr;

    utilSetLE8(&hdr.opcode, ATT_OPCODE_ERROR);
    utilSetLE8(&err.reqOpcode, opcode);
    utilSetLE8(&err.err, errNo);
    utilSetLE16(&err.handle, handle);

    return attSrvSend(srv, to, &hdr, sizeof(hdr), NULL, &err, sizeof(err)) == ATT_TX_RET_ACCEPTED;
}

/*
 * FUNCTION: attSrvSendPacket
 * USE:      Sends a packet to a client
 * PARAMS:   srv - the server
 *           to - whom to send to
 *           opcode - opcode to send
 *           hdr - a header to prepend
 *           hdrLen - length of said header
 *           data - the data to send
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvSendPacket(struct attServer *srv, l2c_handle_t to, uint8_t opcode, const void *hdr, uint32_t hdrLen, sg data)
{
    struct attHdr attHdr;

    utilSetLE8(&attHdr.opcode, opcode);

    if (hdr && !sgConcatFrontCopy(data, hdr, hdrLen))
        return false;

    if (attSrvSend(srv, to, &attHdr, sizeof(attHdr), data, NULL, 0) == ATT_TX_RET_ACCEPTED)
        return true;

    sgTruncFront(data, hdrLen);
    return false;
}

/*
 * FUNCTION: attSrvOpFindInfo
 * USE:      Perform a find info request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpFindInfo(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle)
{
    struct attPduFindInfoRsp rsp;
    bool allUuidsSmall = true;
    struct attRange *range;
    struct attVal *val;
    sg results;
    bool err = false;

    if (!startHandle || startHandle > endHandle)
        return attSrvSendErrorReply(srv, to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_INFO_REQ, startHandle);

    results = sgNew();

    if (!results)
        goto unrecoverable;

    mtu -= sizeof(struct attPduFindInfoRsp) + sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; val = val->next) {
            uint16_t uuid16, handle = range->start + val->offset;

            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto out;

            if (allUuidsSmall && attUuidToUuid16(&uuid16, &val->type)) {
                struct attPduFindInfoData16 result;
                uint16_t uuid;

                utilSetLE16(&result.handle, handle);
                utilSetLE16(&result.uuid16, uuid16);
                if (!sgConcatBackCopy(results, &result, sizeof(result))) {
                    err = true;
                    goto out;
                }
            } else {
                struct attPduFindInfoData128 result;
                if (allUuidsSmall) { /* need to convert to all-large */
                    uint32_t numResults = sgLength(results) / sizeof(struct attPduFindInfoData16);
                    struct attPduFindInfoData16 result16;
                    struct att_uuid uuid;

                    while(numResults--) {
                        if (!sgSerializeCutFront(results, &result16, sizeof(result16)))
                            goto unrecoverable;
                        utilSetLE16(&result.handle, utilGetLE16(&result16.handle));
                        attUuidFromUuid16(&uuid, utilGetLE16(&result16.uuid16));
                        attUuidWrite(&result.uuid128, &uuid);
                        if (!sgConcatBackCopy(results, &result, sizeof(result)))
                            goto unrecoverable;
                    }
                    allUuidsSmall = false;
                }
                utilSetLE16(&result.handle, handle);
                attUuidWrite(&result.uuid128, &val->type);
                if (!sgConcatBackCopy(results, &result, sizeof(result))) {
                    err = true;
                    goto out;
                }
            }

            /* keep length in check */
            if (sgLength(results) >= mtu)
                goto out;
        }
    }

out:
    /* if we have results AND an error, favour sending results */
    if (!sgLength(results) && err)
        goto unrecoverable;

    /* make sure we do not send too much */
    while (sgLength(results) > mtu)
        sgTruncBack(results, allUuidsSmall ? sizeof(struct attPduFindInfoData16) : sizeof(struct attPduFindInfoData128));

    /* a lack of results is reported thusly */
    if (!sgLength(results)) {
        sgFree(results);
        return attSrvSendErrorReply(srv, to, ATT_ERROR_ATTRIBUTE_NOT_FOUND, ATT_OPCODE_FIND_INFO_REQ, startHandle);
    }

    /* we're ready to send a reply */
    utilSetLE8(&rsp.format, allUuidsSmall ? ATT_FIND_INFO_RSP_FMT_16 : ATT_FIND_INFO_RSP_FMT_128);
    if (attSrvSendPacket(srv, to, ATT_OPCODE_FIND_INFO_RSP, &rsp, sizeof(rsp), results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */

unrecoverable:
    if (results)
        sgFree(results);
    return attSrvSendErrorReply(srv, to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_INFO_REQ, 0);
}

/*
 * FUNCTION: attSrvOpFindByTypeVal
 * USE:      Perform a find by type value request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 *           uuid - the uuid to find
 *           s - the value to find
 * RETURN:   success
 * NOTES:    call with srv->lock held for read. WILL NOT FREE "s". Caller must!
 */
static bool attSrvOpFindByTypeVal(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct att_uuid *uuid, sg s)
{
    struct attRange *range;
    struct attVal *val;
    sg results;
    void *wantedVal = NULL, *readVal = NULL;

    if (!startHandle || startHandle > endHandle)
        return attSrvSendErrorReply(srv, to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);

    results = sgNew();
    if (!results)
        goto unrecoverable;

    wantedVal = malloc(sgLength(s));
    if (!wantedVal)
        goto unrecoverable;

    readVal = malloc(sgLength(s) + 1); /* we try to read one more byte to make sure it only reads length bytes, since else we have a mismatch */
    if (!readVal)
        goto unrecoverable;

    sgSerialize(s, 0, sgLength(s), wantedVal);
    mtu -= sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset, len = sgLength(s) + 1;
            uint16_t effectiveGrpLen = val->grpLen ? val->grpLen : 1;
            struct attPduFindByTypeValRspItem result;
            uint8_t readRet;

            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto out;

            /* must be readable */
            if (!(val->perms & ATT_PERM_READ))
                continue;

            /* must be of a given type */
            if (!attUuidCmp(&val->type, uuid))
                continue;

            /* must have correct content length and value */
            readRet = val->readF(srv, to, val->cbkData, range->rangeRef, val->offset, 0, ATT_READ_FOR_FIND_BY_TYPE_VAL, &len, readVal);
            if (readRet != ATT_ERROR_NONE || len != sgLength(s) || memcmp(readVal, wantedVal, len))
                continue;

            /* if we're here, we found a matching value */
            utilSetLE16(&result.firstHandle, handle);
            utilSetLE16(&result.groupEndHandle, handle + effectiveGrpLen - 1);

            if (!sgConcatBackCopy(results, &result, sizeof(result)))
                goto out;

            /* keep length in check */
            if (sgLength(results) >= mtu)
                goto out;
        }
    }

out:
    free(wantedVal);
    wantedVal = NULL;
    free(readVal);
    readVal = NULL;

    /* make sure we do not send too much */
    while (sgLength(results) > mtu)
        sgTruncBack(results, sizeof(struct attPduFindByTypeValRspItem));

    /* a lack of results is reported thusly */
    if (!sgLength(results)) {
        sgFree(results);
        return attSrvSendErrorReply(srv, to, ATT_ERROR_ATTRIBUTE_NOT_FOUND, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);
    }

    /* we're ready to send a reply */
    if (attSrvSendPacket(srv, to, ATT_OPCODE_FIND_BY_TYPE_VAL_RSP, NULL, 0, results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */

unrecoverable:
    if (results)
        sgFree(results);
    if (wantedVal)
        free(wantedVal);
    if (readVal)
        free(readVal);
    return attSrvSendErrorReply(srv, to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, 0);
}

/*
 * FUNCTION: attSrvOpReadByType
 * USE:      Perform a read by type request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 *           uuid - the uuid to find
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpReadByType(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct att_uuid *uuid)
{
    uint8_t valBuf[ATT_MAX_LEN_FOR_READ_BY_TYPE + sizeof(struct attHandleValuePair)];
    uint8_t readRet = ATT_ERROR_NONE;
    uint16_t handle = startHandle;
    struct attRange *range;
    uint8_t itemLen = 0;
    uint32_t itemMaxLen;
    struct attVal *val;
    sg results;

    if (!startHandle || startHandle > endHandle)
        return attSrvSendErrorReply(srv, to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_READ_BY_TYPE_REQ, startHandle);

    mtu -= sizeof(struct attHdr) + sizeof(struct attPduReadByTypeRsp); /* mtu is now how many bytes we can produce */
    itemMaxLen = mtu - sizeof(uint16_t);
    if (itemMaxLen > ATT_MAX_LEN_FOR_READ_BY_TYPE)
        itemMaxLen = ATT_MAX_LEN_FOR_READ_BY_TYPE;

    results = sgNew();
    if (!results)
        goto unrecoverable;

    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; val = val->next) {
            struct attHandleValuePair *hvp = (struct attHandleValuePair*)valBuf;
            uint16_t len = itemMaxLen;

            handle = range->start + val->offset;

            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto out;

            /* we will never call read calabck on things higher layer said are nto treadable, so we handle this one for them */
            if (!(val->perms & ATT_PERM_READ)) {
                readRet = ATT_ERROR_READ_NOT_ALLOWED;
                goto out;
            }

            /* must be of a given type */
            if (!attUuidCmp(&val->type, uuid))
                continue;

            /* do the read, stop on any errors */
            readRet = val->readF(srv, to, val->cbkData, range->rangeRef, val->offset, 0, ATT_READ_FOR_READ_BY_TYPE, &len, valBuf + sizeof(struct attHandleValuePair));
            if (readRet != ATT_ERROR_NONE)
                goto out;

            /* if we already have results and lengths aren't the same, bail out */
            if (sgLength(results) && len != itemLen)
                goto out;
            itemLen = len;

            /* prepare the per-item header */
            len += sizeof(struct attHandleValuePair);
            utilSetLE16(&hvp->handle, handle);

            if (!sgConcatBackCopy(results, valBuf, len))
                goto out;

            /* keep length in check */
            if (sgLength(results) >= mtu)
                goto out;
        }
    }

out:
    itemLen += sizeof(struct attHandleValuePair);

    /* make sure we do not send too much */
    while (sgLength(results) > mtu)
        sgTruncBack(results, itemLen);

    if (sgLength(results)) {
        /* if we have results, send them */
        struct attPduReadByTypeRsp hdr;

        utilSetLE8(&hdr.handleValuePairSz, itemLen);
        if (attSrvSendPacket(srv, to, ATT_OPCODE_READ_BY_TYPE_RSP, &hdr, sizeof(hdr), results))
            return true;
    } else {
        /* else report the error */
        uint8_t err;

        switch (readRet) {
        case ATT_ERROR_NONE:
            err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
            handle = startHandle;
            break;

        case ATT_ERROR_INSUFFICIENT_ATHOR:
        case ATT_ERROR_INSUFFICIENT_AUTH:
        case ATT_ERROR_INSUFF_ENCR_KEY_SZ:
        case ATT_ERROR_INSUFFICIENT_ENCR:
        case ATT_ERROR_READ_NOT_ALLOWED:
            err = readRet;
            break;

        default:
            logw("Unexpected error %u while reading for ReadByTypeReq\n", readRet);
            err = ATT_ERROR_UNLIKELY_ERROR;
            handle = 0;
            break;
        }

        return attSrvSendErrorReply(srv, to, err, ATT_OPCODE_READ_BY_TYPE_RSP, handle);
    }

unrecoverable:
    if (results)
        sgFree(results);
    return attSrvSendErrorReply(srv, to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_READ_BY_TYPE_REQ, 0);
}

/*
 * FUNCTION: attSrvOpRead
 * USE:      Perform a read request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           wantedHandle - the handle the client cares about
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpRead(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t wantedHandle)
{
    struct attRange *range;
    struct attVal *val;
    uint8_t err;

    mtu -= sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    for (range = srv->rangesHead; range; range = range->next) {
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset;

            if (handle > wantedHandle)
                val = NULL;
            if (handle < wantedHandle)
                continue;
            goto out;
        }
    }

out:
    if (!val)
        err = ATT_ERROR_INVALID_HANDLE;
    else if (!(val->perms & ATT_PERM_READ))
        err = ATT_ERROR_READ_NOT_ALLOWED;
    else {
        uint16_t len = 0;
        err = val->readF(srv, to, val->cbkData, range->rangeRef, val->offset, 0, ATT_READ_FOR_READ, &len, NULL);
        if (err == ATT_ERROR_NONE) {

            void *data;

            if (len >= mtu)
                len = mtu;

            data = malloc(len);
            if (!data)
                err = ATT_ERROR_UNLIKELY_ERROR;
            else {
                err = val->readF(srv, to, val->cbkData, range->rangeRef, val->offset, 0, ATT_READ_FOR_READ, &len, data);
                if (err == ATT_ERROR_NONE) {
                    sg reply = sgNewWithAllocedData(data, len);
                    if (reply) {
                        data = NULL;

                        if (attSrvSendPacket(srv, to, ATT_OPCODE_READ_RSP, NULL, 0, reply))
                            return true;
                        sgFree(reply);
                    }
                    err = ATT_ERROR_UNLIKELY_ERROR;
                }
                if (data)
                    free(data);
            }
        }
    }

    return attSrvSendErrorReply(srv, to, err, ATT_OPCODE_READ_REQ, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadBlob
 * USE:      Perform a read blob request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           handle - first handle the client cares about
 *           offset - offset into a long attribute
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpReadBlob(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t handle, uint16_t offset)
{
    //
    return false;
}


/*
 * FUNCTION: attSrvOpReadMult
 * USE:      Perform a read multiple request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           s - sg with uint16_t list of handles
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpReadMult(struct attServer *srv, l2c_handle_t to, uint32_t mtu, sg handles)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpReadByGrpType
 * USE:      Perform a read by group type request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           s - sg with uint16_t list of handles
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpReadByGrpType(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct att_uuid *uuid)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpWrite
 * USE:      Perform a write request / command
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           handle - the handle to write
 *           data - sg with the data to write
 *           isReq - set if this was a request and needs a reply, else clear
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpWrite(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t handle, sg data, bool isReq)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpPrepareWrite
 * USE:      Perform a prepare write request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           handle - the handle to write
 *           offset - the offset into said attribute
 *           data - sg with the data to write
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpPrepareWrite(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t handle, uint16_t offset, sg data)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpExecuteWrite
 * USE:      Perform an execute write request
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           flags - ATT_EXECUTE_WRITE_FLG_*
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpExecuteWrite(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint8_t flags)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpHandleValueConf
 * USE:      Handle a value confirmation
 * PARAMS:   srv - the server
 *           to - whom to reply to (if any)
 *           mtu - the mtu to use
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpHandleValueConf(struct attServer *srv, l2c_handle_t to, uint32_t mtu)
{
    //
    return false;
}

/*
 * FUNCTION: attSrvOpSignedWrite
 * USE:      Perform a signed write command
 * PARAMS:   srv - the server
 *           to - whom to reply to
 *           mtu - the mtu to use
 *           handle - the handle to write
 *           rxedSig - the SIG we got
 *           data - sg with the data to write
 * RETURN:   success
 * NOTES:    call with srv->lock held for read
 */
static bool attSrvOpSignedWrite(struct attServer *srv, l2c_handle_t to, uint32_t mtu, uint16_t handle, const uint8_t *rxedSig, sg data)
{
    uint8_t hdr[sizeof(struct attHdr) + sizeof(struct attPduSignedWriteCmd)];
    struct attHdr *attHdr = (struct attHdr*)hdr;
    struct attPduSignedWriteCmd *cmd = (struct attPduSignedWriteCmd*)(hdr + 1);
    uint8_t calcedSig[ATT_SIG_LEN];
    uint8_t k[SM_BLOCK_LEN];
    struct bt_addr addr;

    if (!l2cApiGetBtAddr(to, &addr)) {
        logw("Cannot get address for signed write. Dropping\n");
        return true;
    }

    if (!persistGetDevKey(&addr, KEY_TYPE_CSRK, k)) {
        logw("Cannot get CSRK for signed write. Dropping\n");
        return true;
    }

    /* Signature is over packet header too, but we stripped it. Luckily it is easy to reconstruct temporarily */
    utilSetLE8(&attHdr->opcode, ATT_OPCODE_SIGNED_WRITE_CMD);
    utilSetLE16(&cmd->handle, handle);
    if (!sgConcatFrontCopy(data, hdr, sizeof(hdr)))
        return false;

    smSignatureCalc(data, k, calcedSig);
    sgTruncFront(data, sizeof(hdr));

    if (memcmp(calcedSig, rxedSig, ATT_SIG_LEN)) {
        logw("Sig check failed. Dropping\n");
        return true;
    }

    //todo

    return false;
}


/*
 * FUNCTION: attSrvDataRx
 * USE:      Called when data arrives from a given connection
 * PARAMS:   srv - the server
 *           from - whom the data arived from
 *           s - the data
 * RETURN:   success
 * NOTES:
 */
bool attSrvDataRx(struct attServer *srv, l2c_handle_t from, sg s)
{
    struct attPduFindByTypeValReq findByTypeValReq;
    struct attPduPrepareWriteReq prepareWriteReq;
    struct attPduReadByGrpTypeReq readByGrpType;
    struct attPduSignedWriteCmd signedWriteCmd;
    struct attPduExecuteWriteReq execWriteReq;
    struct attPduReadByTypeReq readByTypeReq;
    struct attPduFindInfoReq findInfoReq;
    struct attPduReadBlobReq readBlobReq;
    struct attPduWriteReq writeReq;
    struct attPduWriteCmd writeCmd;
    struct attPduReadReq readReq;
    struct attPduMtu mtuExch;
    struct att_uuid uuid;
    struct attHdr hdr;
    bool ret = false;
    uint32_t newMtu;
    uint8_t opcode;
    uint32_t mtu;

    if (!sgSerializeCutFront(s, &hdr, sizeof(hdr)))
        return false;
    opcode = utilGetLE8(&hdr.opcode);

    /* add to known client list */
    pthread_rwlock_wrlock(&srv->lock);
    attConnListAdd(&srv->allClients, from, 0);
    pthread_rwlock_unlock(&srv->lock);

    /* grab MTU and get to work */
    pthread_rwlock_rdlock(&srv->lock);
    mtu = attGetMtu(srv, from, true);

    switch (opcode) {
    case ATT_OPCODE_MTU_EXCH_REQ:
        pthread_rwlock_unlock(&srv->lock);
        pthread_rwlock_wrlock(&srv->lock);
        if (!sgSerializeCutFront(s, &mtuExch, sizeof(mtuExch)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            newMtu = utilGetLE16(&mtuExch.mtu);
            mtu = attGetMtu(srv, from, false);
            if (!mtu || mtu > newMtu)
                mtu = newMtu;
            attConnListAdd(&srv->allClients, from, mtu);
            utilSetLE16(&mtuExch.mtu, mtu);
            ret = attSrvSend(srv, from, &mtuExch, sizeof(mtuExch), NULL, NULL, 0);
        }
        break;

    case ATT_OPCODE_FIND_INFO_REQ:
        if (!sgSerializeCutFront(s, &findInfoReq, sizeof(findInfoReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpFindInfo(srv, from, mtu, utilGetLE16(&findInfoReq.startHandle), utilGetLE16(&findInfoReq.endHandle));
        break;

    case ATT_OPCODE_FIND_BY_TYPE_VAL_REQ:
        if (!sgSerializeCutFront(s, &findByTypeValReq, sizeof(findByTypeValReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            attUuidFromUuid16(&uuid, utilGetLE16(&findByTypeValReq.uuid16));
            ret = attSrvOpFindByTypeVal(srv, from, mtu, utilGetLE16(&findByTypeValReq.startHandle), utilGetLE16(&findByTypeValReq.endHandle), &uuid, s);
        }
        break;

    case ATT_OPCODE_READ_BY_TYPE_REQ:
        if (!sgSerializeCutFront(s, &readByTypeReq, sizeof(readByTypeReq)) || !attReadUuid(&uuid, s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (!attReadUuid(&uuid, s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByType(srv, from, mtu, utilGetLE16(&readByTypeReq.startHandle), utilGetLE16(&readByTypeReq.endHandle), &uuid);
        break;

    case ATT_OPCODE_READ_REQ:
        if (!sgSerializeCutFront(s, &readReq, sizeof(readReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpRead(srv, from, mtu, utilGetLE16(&readReq.handle));
        break;

    case ATT_OPCODE_READ_BLOB_REQ:
        if (!sgSerializeCutFront(s, &readBlobReq, sizeof(readBlobReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadBlob(srv, from, mtu, utilGetLE16(&readBlobReq.handle), utilGetLE16(&readBlobReq.offset));
        break;

    case ATT_OPCODE_READ_MULT_REQ:
        if ((sgLength(s) % sizeof(uint16_t)) || sgLength(s) < sizeof(uint16_t[2])) /* sanity check as per spec */
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadMult(srv, from, mtu, s);
        break;

    case ATT_OPCODE_READ_BY_GRP_TYPE_REQ:
        if (!sgSerializeCutFront(s, &readByGrpType, sizeof(readByGrpType)) || !attReadUuid(&uuid, s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByGrpType(srv, from, mtu, utilGetLE16(&readByGrpType.startHandle), utilGetLE16(&readByGrpType.endHandle), &uuid);
        break;

    case ATT_OPCODE_WRITE_REQ:
        if (!sgSerializeCutFront(s, &writeReq, sizeof(writeReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpWrite(srv, from, mtu, utilGetLE16(&writeReq.handle), s, true);
        break;

    case ATT_OPCODE_PREPARE_WRITE_REQ:
        if (!sgSerializeCutFront(s, &prepareWriteReq, sizeof(prepareWriteReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpPrepareWrite(srv, from, mtu, utilGetLE16(&prepareWriteReq.handle), utilGetLE16(&prepareWriteReq.offset), s);
        break;

    case ATT_OPCODE_EXECUTE_WRITE_REQ:
        if (!sgSerializeCutFront(s, &execWriteReq, sizeof(execWriteReq)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpExecuteWrite(srv, from, mtu, utilGetLE16(&execWriteReq.flags));
        break;

    case ATT_OPCODE_HANDLE_VALUE_CONF:
        if (sgLength(s))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpHandleValueConf(srv, from, mtu);
        break;

    case ATT_OPCODE_WRITE_CMD:
        if (!sgSerializeCutFront(s, &writeCmd, sizeof(writeCmd)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpWrite(srv, from, mtu, utilGetLE16(&writeCmd.handle), s, false);
        break;

    case ATT_OPCODE_SIGNED_WRITE_CMD:
        if (!sgSerializeCutFront(s, &signedWriteCmd, sizeof(signedWriteCmd)))
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s) < ATT_SIG_LEN)
            ret = attSrvSendErrorReply(srv, from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            uint8_t sig[ATT_SIG_LEN];
            sgSerialize(s, sgLength(s) - ATT_SIG_LEN, ATT_SIG_LEN, sig);
            sgTruncBack(s, ATT_SIG_LEN);
            ret = attSrvOpSignedWrite(srv, from, mtu, utilGetLE16(&signedWriteCmd.handle), sig, s);
        }
        break;

    default:
        logw("Unexpected ATT opcode 0x%02X\n", opcode);
        ret = attSrvSendErrorReply(srv, from, ATT_ERROR_REQ_NOT_SUPPORTED, opcode, 0);
        break;
    }

out:
    pthread_rwlock_unlock(&srv->lock);
    if (ret)
        sgFree(s);
    return ret;
}





