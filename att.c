#include <stdlib.h>
#include <string.h>
#include "workQueue.h"
#include "persist.h"
#include "config.h"
#include "timer.h"
#include "l2cap.h"
#include "types.h"
#include "uuid.h"
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
#define ATT_OPCODE_WRITE_RSP             0x13 /* NO ADDITIONAL DATA SENT */
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
    struct uuid uuid128;
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
#define ATT_FIRST_CONTINUE_CALL   0xFF

struct attSrvPendingOp { /* an op we have an outstanding callback for */

    uint8_t  origOpcode; /* set to 0 when nothing is in progress, since all possible in-progress opcodes are nonzero */
    sg       replyInProgress;
    uint16_t origStartHandle;
    uint16_t startHandle;
    uint16_t endHandle;
    uint16_t mtu;

    union {
        struct {
            struct uuid      uuid;
            void            *wantedVal;
            uint16_t         wantedLen;
            uint16_t         valGrpLen;
        } findByTypeValue;
        struct {
            struct uuid      uuid;
            bool             grpType;
            uint16_t         valGrpLen;
            uint16_t         itemLen;
        } readByType;
        struct {
            uint16_t         offset;
        } read;
        struct {
            sg               handles;
            uint16_t         lastReadHandle;
        } readMult;
    };
};


struct attSrvConnExtra {
    uniq_t indTimer;
    att_range_t indRangeRef;
    uint64_t indRef;
    uint16_t indOffset;

    uint32_t mtu;
    int cid; /* for higher layer */
    bool isEncr;
    bool isMitmProtected;
    bool isLE;

    int nextTransId;
    struct attSrvPendingOp penOp;
};

struct attConnList {
    uint32_t listCap;
    uint32_t listNum;
    l2c_handle_t *conns;
    struct attSrvConnExtra *extras;
    bool hasExtras;
};

struct attSrvVal {
    struct attSrvVal *next;
    struct attSrvVal *prev;
    struct uuid type;
    uint16_t offset;
    uint16_t grpLen;
    uint8_t perms;
};

struct attSrvRange {
    att_range_t rangeRef;
    struct attSrvRange *next;
    struct attSrvRange *prev;
    uint16_t start;
    uint16_t len;
    struct attSrvVal *valsHead;
    struct attSrvVal *valsTail;
    struct attConnList seenBy;
    bool allowLE;
    bool allowEDR;
};

struct attWorkItem {
    uint8_t type;
    l2c_handle_t who;
    int cid; /* having both this and "who" here makes life a bit easier */

    union {
        struct {
            att_range_t          range;
            uint16_t             ofst;
            int                  transId;
            uint16_t             byteOfst;
            uint16_t             maxLen;
            uint8_t              reason;
        } read;
        struct {
            att_range_t          range;
            uint16_t             ofst;
            int                  transId;
            uint16_t             byteOfst;
            uint16_t             len;
            uint8_t              reason;
            /* data follows */
        } write;
        struct {
            int                  transId;
            bool                 exec;
        } execWrite;
        struct {
            att_range_t          range;
            uint16_t             ofst;
            uint64_t             ref;
            uint8_t              evt;
        } indResult;
        struct {
            uint16_t             mtu;
        } mtuInfo;
        struct {
            sem_t               *sem;
        } sync;
    };
};

#define WORK_TYPE_READ_CALL            0
#define WORK_TYPE_WRITE_CALL           1
#define WORK_TYPE_EXEC_WRITE_CALL      2
#define WORK_TYPE_IND_RESULT           3
#define WORK_TYPE_MTU_INFO             4
#define WORK_TYPE_SYNC                 5


/* our state */
static pthread_mutex_t mSrvLock = PTHREAD_MUTEX_INITIALIZER;
static struct attSrvRange *mRangesHead;
static struct attSrvRange *mRangesTail;
static attSrvTxCbk mTxCbk;
static attSrvMtuCbk mMtuCbk;
static attSrvIndResultCbk mIndCbk;
static attSrvValueExecWrite mExecCbk;
static attSrvValueReadCbk mReadCbk;
static attSrvValueWriteCbk mWriteCbk;
static struct attConnList mAllClients;
static bool mInTeardown;
static struct workQueue *mWorkQ;
static pthread_t mWorker;






/*
 * FUNCTION: attWorkItemAlloc
 * USE:      Allocate a work item of a given type
 * PARAMS:   type - the type
 *           extraSz - the extra size to allocate
 * RETURN:   success
 * NOTES:    blocks till done
 */
static struct attWorkItem* attWorkItemAlloc(uint8_t type, uint32_t extraSz)
{
    struct attWorkItem *wi = (struct attWorkItem*)calloc(1, sizeof(struct attWorkItem) + extraSz);
    if (wi)
        wi->type = type;

    return wi;
}

/*
 * FUNCTION: attConnListGetExtra
 * USE:      Retrieve a given connection's "extra" data from a connection list, if it is present there
 * PARAMS:   list - the list
 *           conn - the connection
 * RETURN:   the extra data pointer or NULL
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static struct attSrvConnExtra* attConnListGetExtra(struct attConnList *list, l2c_handle_t conn)
{
    uint32_t i;

    for (i = 0; i < list->listNum; i++)
        if (list->conns[i] == conn)
            return list->hasExtras ? list->extras + i : NULL;

    return NULL;
}

/*
 * FUNCTION: attSrvSchedCbkGetNextTid
 * USE:      Get the transaction ID to use
 * PARAMS:   extra - the per-conenction extra data
 * RETURN:   the transaction ID to use (positive int)
 * NOTES:    call with mSrvLock help
 */
static int attSrvSchedCbkGetNextTid(struct attSrvConnExtra* extra)
{
    int tid;

    extra->nextTransId++;
    if (extra->nextTransId < 0)
        extra->nextTransId = 1;

    return extra->nextTransId;
}

/*
 * FUNCTION: attSrvSchedReadCbk
 * USE:      Schedule a call to the read callback
 * PARAMS:   range - the handle range
 *           val - the value struct
 *           who - whom the connection is to
 *           reason - reason value to pass to read callback
 *           byteOfst - byte offset to pass to read callback
 *           maxLen - maxLength to read
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mSrvLock held Do not release the lock until all state in extra->penOp is set up
 */
static bool attSrvSchedReadCbk(struct attSrvRange *range, struct attSrvVal* val, l2c_handle_t who, uint8_t reason, uint16_t byteOfst, uint16_t maxLen)
{
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, who);
    struct attWorkItem *wi;
    int tid;
    
    if (!extra)
        return false;

    wi = attWorkItemAlloc(WORK_TYPE_READ_CALL, 0);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(extra);

    wi->who = who;
    wi->cid = extra->cid;
    wi->read.range = range->rangeRef;
    wi->read.ofst = val->offset;
    wi->read.transId = tid;
    wi->read.byteOfst = byteOfst;
    wi->read.maxLen = maxLen;
    wi->read.reason = reason;

    if (workQueuePut(mWorkQ, wi))
        return true;

    free(wi);
    return false;
}

/*
 * FUNCTION: attSrvSchedWriteCbk
 * USE:      Schedule a call to the write callback
 * PARAMS:   range - the handle range
 *           val - the value struct
 *           who - whom the connection is to
 *           reason - reason value to pass to write callback
 *           replace - repalce the existing slot (else will fail if slot not empty)
 *           byteOfst - byte offset to pass to write callback
 *           data - the sg with the data to write
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mSrvLock held  Do not release the lock until all state in extra->penOp is set up. WILL NOT FREE "data". Caller must!
 */
static bool attSrvSchedWriteCbk(struct attSrvRange *range, struct attSrvVal* val, l2c_handle_t who, uint8_t reason, uint16_t byteOfst, sg data)
{
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, who);
    uint16_t len = sgLength(data);
    struct attWorkItem *wi;
    const void *dataPtr = NULL;
    bool needFree = false;
    void *iter;
    bool ret;
    int tid;

    
    if (!extra)
        return false;

    wi = attWorkItemAlloc(WORK_TYPE_WRITE_CALL, len);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(extra);
    sgSerialize(data, 0, len, wi + 1);

    wi->who = who;
    wi->cid = extra->cid;
    wi->write.range = range->rangeRef;
    wi->write.ofst = val->offset;
    wi->write.transId = tid;
    wi->write.byteOfst = byteOfst;
    wi->write.len = len;
    wi->write.reason = reason;

    if (workQueuePut(mWorkQ, wi)) {
        sgFree(data);
        return true;
    }

    free(wi);
    return false;
}

/*
 * FUNCTION: attSrvSchedExecCbk
 * USE:      Schedule a call to the execute-write callback
 * PARAMS:   who - whom the connection is to
 *           exec - true to execute, false to clear state
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mSrvLock held  Do not release the lock until all state in extra->penOp is set up. WILL NOT FREE "data". Caller must!
 */
static bool attSrvSchedExecCbk(l2c_handle_t who, bool exec)
{
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, who);
    struct attWorkItem *wi;
    int tid;
    
    if (!extra)
        return false;

    wi = attWorkItemAlloc(WORK_TYPE_EXEC_WRITE_CALL, 0);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(extra);

    wi->who = who;
    wi->cid = extra->cid;
    wi->execWrite.transId = tid;
    wi->execWrite.exec = exec;

    if (workQueuePut(mWorkQ, wi))
        return true;

    free(wi);
    return false;
}

/*
 * FUNCTION: attSrvSchedIndResultCbk
 * USE:      Schedule a call to the indication result callback
 * PARAMS:   rangeRef - the range reference
 *           offst - the value offset in the range
 *           who - whom the connection is to
 *           evt - what happened?
 *           ref - the reference value provided by the higher layer
 * RETURN:   success
 * NOTES:    call with mSrvLock held Do not release the lock until all state in extra->penOp is set up
 */
static bool attSrvSchedIndResultCbk(att_range_t rangeRef, uint16_t offst, l2c_handle_t who, uint8_t evt, uint64_t ref)
{
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, who);
    struct attWorkItem *wi;
    
    if (!extra)
        return false;

    wi = attWorkItemAlloc(WORK_TYPE_IND_RESULT, 0);
    if (!wi)
        return false;

    wi->who = who;
    wi->cid = extra->cid;
    wi->indResult.range = rangeRef;
    wi->indResult.ofst = offst;
    wi->indResult.ref = ref;
    wi->indResult.evt = evt;

    if (workQueuePut(mWorkQ, wi))
        return true;

    free(wi);
    return false;
}

/*
 * FUNCTION: attSrvSchedMtuCbk
 * USE:      Schedule a call to the MTU callback
 * PARAMS:   who - whom the connection is to
 *           mtu - the new mtu
 * RETURN:   success
 * NOTES:    call with mSrvLock held Do not release the lock until all state in extra->penOp is set up
 */
static bool attSrvSchedMtuCbk(l2c_handle_t who, uint64_t mtu)
{
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, who);
    struct attWorkItem *wi;
    
    if (!extra)
        return false;

    wi = attWorkItemAlloc(WORK_TYPE_MTU_INFO, 0);
    if (!wi)
        return false;

    wi->who = who;
    wi->cid = extra->cid;
    wi->mtuInfo.mtu = mtu;

    if (workQueuePut(mWorkQ, wi))
        return true;

    free(wi);
    return false;
}

/*
 * FUNCTION: attSync
 * USE:      Verifies all pending att work before now is finished. Use this after deleting things to make sure all refs to them are gone
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:    blocks till done
 */
bool attSync(void)
{
    struct attWorkItem *wi;
    sem_t sem;

    wi = attWorkItemAlloc(WORK_TYPE_SYNC, 0);
    if (!wi)
        goto fail_alloc;

    if (sem_init(&sem, 0, 0))
        goto fail_sem;

    wi->sync.sem = &sem;

    if (!workQueuePut(mWorkQ, wi))
        goto fail_enq;

    r_sem_wait(&sem);
    sem_destroy(&sem);
    return true;

fail_enq:
    sem_destroy(&sem);

fail_sem:
    free(wi);

fail_alloc:
    return false;
}

/*
 * FUNCTION: attConnListFindByCid
 * USE:      Find a connection by its "cid" value
 * PARAMS:   list - the list
 *           cid - the cid
 *           connP - conenction handle gets stored here if not NULL
 *           extraP - extra pointer gets stored her eif not NULL
 * RETURN:   true if found
 * NOTES:    attConnList has no locks itself - please hold the parent data structure's lock when calling
 */
static bool attConnListFindByCid(struct attConnList *list, int cid, l2c_handle_t *connP, struct attSrvConnExtra **extraP)
{
    uint32_t i;

    if (!list->hasExtras) {
        loge("CIDs not supported in lists with no extras\n");
        return false;
    }

    for (i = 0; i < list->listNum; i++) {
        if (list->extras[i].cid == cid) {
            if (connP)
                *connP = list->conns[i];
            if (extraP)
                *extraP = list->extras + i;
            return true;
        }
    }

    return false;
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
    struct bt_addr peer;
    uint32_t i;

    /* check for existence */
    for (i = 0; i < list->listNum; i++)
        if (list->conns[i] == conn)
            return true;

    /* grab the peer address */
    if (!l2cApiGetBtAddr(conn, &peer)) {
        loge("Failed to get peer address\n");
        return false;
    }

    /* if list is full, expand it */
    if (list->listCap == list->listNum) {
        uint32_t newCap = list->listCap + ATT_CONN_LIST_GROW_BY;
        l2c_handle_t *conns;
        struct attSrvConnExtra *extras;

        conns = (l2c_handle_t*)realloc(list->conns, sizeof(l2c_handle_t[newCap]));
        if (!conns)
            return false;
        list->conns = conns;

        if (list->hasExtras) {
            extras = (struct attSrvConnExtra*)realloc(list->extras, sizeof(struct attSrvConnExtra[newCap]));
            if (!extras)
                return false;
            list->extras = extras;
        }

        list->listCap = newCap;
    }

    /* init the item */
    list->conns[list->listNum] = conn;
    if (list->hasExtras) {
        struct attSrvConnExtra *extra = list->extras + list->listNum;

        /* zero-init the extra struct */
        memset(extra, 0, sizeof(struct attSrvConnExtra));

        /* find a free CID */
        extra->cid = list->listNum ? list->extras[list->listNum - 1].cid : 1;
        while (attConnListFindByCid(list, extra->cid, NULL, NULL))
            if (++extra->cid < 0)
                extra->cid = 1;

        /* get initial encr and mitm state */
        extra->isEncr = l2cApiIsConnEncrypted(conn);
        extra->isMitmProtected = l2cApiIsConnMitmSafe(conn);
        extra->isLE = BT_ADDR_IS_LE(peer);
    }
    list->listNum++;
    return true;
}

/*
 * FUNCTION: attConnListFreeExtra
 * USE:      Free the storage occupied by the "extra" data in a connection list
 * PARAMS:   extra the extra struct
 * RETURN:   NONE
 * NOTES:
 */
static void attConnListFreeExtra(struct attSrvConnExtra *extra)
{
    /* future extra freeing steps here */
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
            if (list->hasExtras) {
                attConnListFreeExtra(list->extras + i);
                memmove(list->extras + i, list->extras + list->listNum, sizeof(struct attSrvConnExtra));
            }
            return true;
        }
    }

    return false;
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
    if (list->hasExtras && list->extras) {
        uint32_t i;
        for(i = 0; i < list->listNum; i++)
            attConnListFreeExtra(list->extras + i);
        free(list->extras);
    }
    list->conns = NULL;
}

/*
 * FUNCTION: attSrvEncrAuthUpdate
 * USE:      Update the ATT server about a change in a connection to a client pertaining to auth/encr
 * PARAMS:   who - the connection to the client
 *           encr - is it now encrypted?
 *           mitmSafe - is it now mitm-safe?
 * RETURN:   success
 * NOTES:
 */
bool attSrvEncrAuthUpdate(l2c_handle_t who, bool encr, bool mitmSafe)
{
    struct attSrvConnExtra *extra;
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    extra = attConnListGetExtra(&mAllClients, who);
    if (extra) {
        extra->isEncr = encr;
        extra->isMitmProtected = mitmSafe;
        ret = true;
    }
    pthread_mutex_unlock(&mSrvLock);

    return true;
}

/*
 * FUNCTION: attSrvCreate
 * USE:      create an ATT server
 * PARAMS:   txF - callback to use to TX data to clients
 *           mtuF - callback to tell client of new MTU
 *           indF - callback to tell client of ind TX progress
 *           execF - callback to execute or cancel pending writes
 *           readF - read callback
 *           writeF - write callback
 * RETURN:   success
 * NOTES:
 */
bool attSrvCreate(attSrvTxCbk txF, attSrvMtuCbk mtuF, attSrvIndResultCbk indF, attSrvValueExecWrite execF, attSrvValueReadCbk readF, attSrvValueWriteCbk writeF)
{
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    if (mTxCbk)
        loge("RE-initing ATT server\n");
    else {
        mTxCbk = txF;
        mMtuCbk = mtuF;
        mIndCbk = indF;
        mExecCbk = execF;
        mReadCbk = readF;
        mWriteCbk = writeF;
        ret = true;
    }
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvDestroy
 * USE:      destroy an ATT server
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void attSrvDestroy(void)
{
    struct attSrvConnExtra *extra;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint32_t i;

    pthread_mutex_lock(&mSrvLock);
    for (i = 0; i < mAllClients.listNum; i++) {
        extra = mAllClients.extras + i;
        if (extra->indTimer) {
            if (timerCancel(extra->indTimer))
                attSrvSchedIndResultCbk(extra->indRangeRef, extra->indOffset, mAllClients.conns[i], ATT_SRV_EVT_IND_SRV_DESTROYED, extra->indRef);
            extra->indTimer = 0;
        }
    }
    mInTeardown = true;
    /* all uncancelled timers need to be run before we destroy state, so we let them */
    pthread_mutex_unlock(&mSrvLock);
    timerSync();
    pthread_mutex_lock(&mSrvLock);

    for (range = mRangesHead; range; range = range->next) {
        for (val = range->valsHead; val; ) {
            struct attSrvVal *t = val;
            val = val->next;
            free(t);
        }
        attConnListFree(&range->seenBy);
    }
    attConnListFree(&mAllClients);
    pthread_mutex_unlock(&mSrvLock);

    mTxCbk = NULL;
    mMtuCbk = NULL;
    mIndCbk = NULL;
}

/*
 * FUNCTION: attSrvRangeDelete
 * USE:      delete all values in a given range
 * PARAMS:   range - the range
 * RETURN:   NONE
 * NOTES:    call with server's write lock held
 */
static void attSrvRangeDeleteAllVals(struct attSrvRange *range)
{
    struct attSrvVal *val, *t;

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
 * FUNCTION: attSrvRangeDelete
 * USE:      delete a given att range struct
 * PARAMS:   range - the range
 * RETURN:   NONE
 * NOTES:    call with server's write lock held
 */
static void attSrvRangeDelete(struct attSrvRange *range)
{

    if (range->prev)
        range->prev->next = range->next;
    else
        mRangesHead = range->next;

    if (range->next)
        range->next->prev = range->prev;
    else
        mRangesTail = range->prev;

    attSrvRangeDeleteAllVals(range);
    if (!attConnListIsEmpty(&range->seenBy))
        logw("Deleted range 0x%04x+0x%04x has been seen\n", range->start, range->len);
    attConnListFree(&range->seenBy);
    free(range);
}

/*
 * FUNCTION: attSrvNotifConnClose
 * USE:      tell an ATT server about a closed connection (in case it cares)
 * PARAMS:   conn - the connection
 * RETURN:   NONE
 * NOTES:    if said connection substribed to notifications, allows us to unsubstribe
 *           them proactively instead of waiting for TX errors
 */
void attSrvNotifConnClose(l2c_handle_t conn)
{
    struct attSrvRange *range, *t;
    struct attSrvVal *val;
    uint32_t i;

    pthread_mutex_lock(&mSrvLock);
    for (range = mRangesHead; range;) {
        attConnListDel(&range->seenBy, conn);
        t = range;
        range = range->next;
        if (attConnListIsEmpty(&t->seenBy) && !t->rangeRef) /* delete a disused range after all who knew about it are gone */
            attSrvRangeDelete(t);
    }
    attConnListDel(&mAllClients, conn);
    pthread_mutex_unlock(&mSrvLock);
}

/*
 * FUNCTION: attSrvHandleRangeReserve
 * USE:      reserve a range of handles in the ATT server
 * PARAMS:   len - the wanted handle range length
 *           allowLE - accessible by LE?
 *           allowEDR - accessible by EDR?
 * RETURN:   a reference to the handle range or 0 on failure
 * NOTES:
 */
att_range_t attSrvHandleRangeReserve(uint16_t len, bool allowLE, bool allowEDR)
{
    uint32_t searchH = ATT_FIRST_VALID_HANDLE;
    struct attSrvRange *next, *prev = NULL, *newRange;
    att_range_t ret = 0;

    pthread_mutex_lock(&mSrvLock);
    next = mRangesHead;
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

    newRange = (struct attSrvRange*)calloc(1, sizeof(struct attSrvRange));
    if (!newRange)
        goto out;

    newRange->start = searchH;
    newRange->len = len;
    newRange->rangeRef = uniqGetNext();
    newRange->prev = prev;
    newRange->next = next;
    newRange->allowLE = allowLE;
    newRange->allowEDR = allowEDR;
    if (next)
        next->prev = newRange;
    else
        mRangesTail = newRange;
    if (prev)
        prev->next = newRange;
    else
        mRangesHead = newRange;

    ret = newRange->rangeRef;
out:
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvRangeFind
 * USE:      find a range structure for a given range reference in a given ATT server
 * PARAMS:   rangeRef - the range reference
 * RETURN:   the range struct or NULL if not found
 * NOTES:    call with mSrvLock help
 */
static struct attSrvRange* attSrvRangeFind(att_range_t rangeRef)
{
    struct attSrvRange *range;

    for (range = mRangesHead; range && range->rangeRef != rangeRef; range = range->next);

    return range;
}

/*
 * FUNCTION: attSrvValFind
 * USE:      find a value structure for a given range structure and a given offset
 * PARAMS:   range - the range
 *           offset - the offset
 * RETURN:   the value struct or NULL if not found
 * NOTES:    call with mSrvLock help
 */
static struct attSrvVal* attSrvValFind(struct attSrvRange *range, uint16_t offset)
{
    struct attSrvVal *val;

    for (val = range->valsHead; val && val->offset != offset; val = val->next);

    return val;
}

/*
 * FUNCTION: attSrvValRangeFind
 * USE:      find a value structure and a range structure for a ATT server, a range handle, and an offset
 * PARAMS:   rangeRef - the range reference
 *           offset - the offset
 *           rangeP - range gets stored here
 * RETURN:   the value struct or NULL if not found
 * NOTES:    call with mSrvLock help
 */
static struct attSrvVal* attSrvValRangeFind(att_range_t rangeRef, uint16_t offset, struct attSrvRange **rangeP)
{
    *rangeP = attSrvRangeFind(rangeRef);
    if (*rangeP)
        return  attSrvValFind(*rangeP, offset);
    return NULL;
}

/*
 * FUNCTION: attSrvHandleRangeRelease
 * USE:      release a range of handles in the ATT server
 * PARAMS:   rangeRef - the range reference
 * RETURN:   NONE
 * NOTES:
 */
void attSrvHandleRangeRelease(att_range_t rangeRef)
{
    struct attSrvRange *range;

    pthread_mutex_lock(&mSrvLock);
    range = attSrvRangeFind(rangeRef);
    if (!range)
        logw("Attempt to delete a non-existent range\n");
    else {
        if (attConnListIsEmpty(&range->seenBy))
            attSrvRangeDelete(range);
        else {
            attSrvRangeDeleteAllVals(range);
            range->rangeRef = 0;
        }
    }
    pthread_mutex_unlock(&mSrvLock);
}

/*
 * FUNCTION: attSrvHandleRangeGetBase
 * USE:      Get the actual handle value of the first handle in a range
 * PARAMS:   rangeRef - the range reference
 * RETURN:   handle value or 0 on error
 * NOTES:
 */
uint16_t attSrvHandleRangeGetBase(att_range_t rangeRef)
{
    struct attSrvRange *range;
    uint16_t ret;

    pthread_mutex_lock(&mSrvLock);
    range = attSrvRangeFind(rangeRef);
    ret = range ? range->start : 0;
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvValueRegister
 * USE:      Register a given value in a given range
 * PARAMS:   rangeRef - the range reference
 *           offset - handle's offset from start of the range
 *           type - the type of the registered value
 *           perms - permissions (ATT_PERM_*)
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueRegister(att_range_t rangeRef, uint16_t offset, const struct uuid *type, uint8_t perms)
{
    struct attSrvVal *next, *prev = NULL, *val;
    struct attSrvRange *range;
    bool ret = false;


    pthread_mutex_lock(&mSrvLock);
    range = attSrvRangeFind(rangeRef);
    if (!range) {
        logw("Attempt to add a value to a non-existent range\n");
        goto out;
    }

    if (offset >= range->len) {
        logw("Value or group end out of range\n");
        goto out;
    }

    for (next = range->valsHead; next && next->offset < offset; prev = next, next = next->next);
    /*
     * prev is our prev or NULL if we're first
     * next is our next or NULL if we're last
     * If this is a duplication, next is the item we're trying to duplicate
     */

    if (next && next->offset == offset) {
        logw("Attempt to create duplicate-offset value (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    val = (struct attSrvVal*)calloc(1, sizeof(struct attSrvVal));
    if (!val)
        goto out;

    memcpy(&val->type, type, sizeof(val->type));
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
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvValueGetGrpLen
 * USE:      Get a given value's "group length" parameter
 * PARAMS:   rangeRef - the range reference
 *           offset - handle's offset from start of the range)
 *           grpLenP - the group length gets stored here
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueGetGrpLen(att_range_t rangeRef, uint16_t offset, uint16_t *grpLenP)
{
    struct attSrvRange *range;
    struct attSrvVal *val;
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    val = attSrvValRangeFind(rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to set group len on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to set group len on a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    if (grpLenP)
        *grpLenP = val->grpLen;
    ret = true;

out:
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}
/*
 * FUNCTION: attSrvValueSetGrpLen
 * USE:      Set a given value's "group length" parameter
 * PARAMS:   rangeRef - the range reference
 *           offset - handle's offset from start of the range)
 *           grpLen - the group length to set
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueSetGrpLen(att_range_t rangeRef, uint16_t offset, uint16_t grpLen)
{
    struct attSrvRange *range;
    struct attSrvVal *val;
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    val = attSrvValRangeFind(rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to set group len on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to set group len on a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    if (offset + grpLen > range->len) {
        logw("Group end out of range\n");
        goto out;
    }

    val->grpLen = grpLen;
    ret = true;

out:
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvValueUnregister
 * USE:      Unregister a given value in a given range
 * PARAMS:   rangeRef - the range reference
 *           offset - handle's offset from start of the range)
 * RETURN:   success
 * NOTES:
 */
bool attSrvValueUnregister(att_range_t rangeRef, uint16_t offset)
{
    struct attSrvRange *range;
    struct attSrvVal *val;
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    val = attSrvValRangeFind(rangeRef, offset, &range);
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
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attConnMtuGet
 * USE:      Get the MTU to someone
 * PARAMS:   to - whom to
 *           tryDefaults - try to find the default to use?
 * RETURN:   the mtu to use or 0 on error
 * NOTES:    call with mSrvLock help
 */
static uint32_t attConnMtuGet(l2c_handle_t to, bool tryDefaults)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint32_t mtu = 0;

    if ((!extra || !(mtu = extra->mtu)) && tryDefaults) {
        struct bt_addr peer;

        if (!l2cApiGetBtAddr(to, &peer)) {
            logw("Failed to get peer address\n");
            return 0;
        }
        mtu = BT_ADDR_IS_EDR(peer) ? ATT_MTU_MIN_EDR : ATT_MTU_MIN_LE;
    }

    return mtu;
}

/*
 * FUNCTION: attConnMtuSet
 * USE:      Set the MTU to someone
 * PARAMS:   to - whom to
 *           mtu - the MTU to set
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attConnMtuSet(l2c_handle_t to, uint32_t mtu)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);

    if (!extra) {
        logw("Setting MTU for unknown connection\n");
        return false;
    }

    extra->mtu = mtu;
    return true;
}

/*
 * FUNCTION: attSrvSend
 * USE:      Sends an assembled packet to the peer
 * PARAMS:   to - whom the data is to be sent to
 *           preData - first part of the packet or NULL
 *           preLen - length of fais data
 *           data - the middle of the packet or NULL
 *           postData - last part of the packet or NULL
 *           postLen - length of said data
 * RETURN:   ATT_TX_RET_*
 * NOTES:    call with mSrvLock help
 */
static uint8_t attSrvSend(l2c_handle_t to, const void *preData, uint32_t preLen, sg data, const void *postData, uint32_t postLen)
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

    if (postLen && !sgConcatBackCopy(data, postData, postLen)) {
        if (tmpData)
            sgFree(tmpData);
        else
            sgTruncFront(data, preLen);
        return ATT_TX_RET_OOM;
    }

    ret = mTxCbk(to, data);
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
 * FUNCTION: attSrvTimerCbk
 * USE:      Called when a timeout for a confirmation for a sent notification comes
 * PARAMS:   which - the timer
 *           userData - unused
 * RETURN:   NONE
 * NOTES:
 */
static void attSrvTimerCbk(uniq_t which,  uint64_t userData)
{
    struct attSrvConnExtra *extra = NULL;
    l2c_handle_t conn;
    uint32_t i;

    pthread_mutex_lock(&mSrvLock);
    /* find the relant conn */
    for (i = 0; i < mAllClients.listNum; i++) {
        if (mAllClients.extras[i].indTimer == which) {
            extra = mAllClients.extras + i;
            conn = mAllClients.conns[i];
            break;
        }
    }

    if (extra) /* we found it */
        attSrvSchedIndResultCbk(extra->indRangeRef, extra->indOffset, conn, ATT_SRV_EVT_IND_TIMEOUT, extra->indRef);
    pthread_mutex_unlock(&mSrvLock);
}

/*
 * FUNCTION: attSrvSendNotif
 * USE:      Send notifications to all clients who wanted them for this value
 * PARAMS:   range - the range
 *           val - the value struct
 *           to - whom to send to
 *           data - the value
 *           notify - send notification (as opposed to indication)
 *           ref - pased to callbacks so you know which notif/ind they are tellking you about
 * RETURN:   L2C_TX_*
 * NOTES:    call with mSrvLock help
 */
static uint8_t attSrvSendNotif(struct attSrvRange *range, struct attSrvVal *val, l2c_handle_t to, sg data, bool notify, uint64_t ref)
{
    uint8_t hdrBuf[sizeof(struct attHdr) + sizeof(struct attPduHandleValueNotif) + sizeof(struct attPduHandleValueInd)]; /* definitely big enoguh to fit the fist and either second or third */
    struct attHdr *hdr = (struct attHdr*)hdrBuf;
    struct attPduHandleValueNotif *notifH = (struct attPduHandleValueNotif*)(hdr + 1);
    struct attPduHandleValueInd *indH = (struct attPduHandleValueInd*)(hdr + 1);
    struct attSrvConnExtra* extra = attConnListGetExtra(&mAllClients, to);
    uint8_t hdrLen = sizeof(*hdr);
    uint32_t mtu;
    uint8_t ret;

    if (notify) {
        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_NOTIF);
        utilSetLE16(&notifH->handle, val->offset + range->start);
        hdrLen += sizeof(*notifH);
    } else {
        if (!extra)
            return ATT_TX_RET_ATT_ERROR;

        if (extra->indTimer)
            return ATT_TX_RET_ATT_BUSY;

        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_IND);
        utilSetLE16(&indH->handle, val->offset + range->start);
        hdrLen += sizeof(*indH);
    }

    mtu = attConnMtuGet(to, true);
    if (!mtu)
        return ATT_TX_RET_ATT_ERROR;

    mtu -= hdrLen;
    if (sgLength(data) > mtu) {
        logd("Truncating wanted notif/ind data to %ub\n", mtu);
        sgTruncBack(data, sgLength(data) - mtu);
    }

    if (!notify) {
        extra->indRangeRef = range->rangeRef;
        extra->indOffset = val->offset;
        extra->indTimer = timerSet(ATT_IND_TIMEOUT, attSrvTimerCbk, 0);
    }

    return attSrvSend(to, hdrBuf, hdrLen, data, NULL, 0);
}

/*
 * FUNCTION: attSrvValueNotifyChanged
 * USE:      Sen a notification or an indication
 * PARAMS:   rangeRef - the range reference
 *           offset - handle's offset from start of the range
 *           cid - the connection id
 *           data - the data to send
 *           notify - send notification (as opposed to indication)
 *           ref - pased to callbacks so you know which notif/ind they are tellking you about
 * RETURN:   ATT_TX_RET_*
 * NOTES:
 */
uint8_t attSrvValueNotifyChanged(att_range_t rangeRef, uint16_t offset, int cid, sg data, bool notify, uint64_t ref)
{
    uint8_t ret = ATT_TX_RET_ATT_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    l2c_handle_t who;
    uint32_t i;

    pthread_mutex_lock(&mSrvLock);
    if (!attConnListFindByCid(&mAllClients, cid, &who, NULL)) {
        loge("Got notif request reply for unknown connection\n");
        goto out;
    }

    val = attSrvValRangeFind(rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to notify on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to notify on a non-existent value from a non-existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    ret = attSrvSendNotif(range, val, who, data, notify, ref);

out:
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvCidResolve
 * USE:      Attempt to resolve a CID to a L2C connection ID
 * PARAMS:   cid - the cid
 * RETURN:   conenction ID or 0 on failure
 * NOTES:
 */
l2c_handle_t attSrvCidResolve(int cid)
{
    l2c_handle_t ret = 0;
    struct attSrvRange *range;

    pthread_mutex_lock(&mSrvLock);
    if (!attConnListFindByCid(&mAllClients, cid, &ret, NULL))
        ret = 0;
    pthread_mutex_unlock(&mSrvLock);
    return ret;
}

/*
 * FUNCTION: attSrvSetMtu
 * USE:      Called to set the MTU for an ATT server to a given client
 * PARAMS:   to - whom the MTU is to
 *           mtu - the mtu
 * RETURN:   NONE
 * NOTES:    used by EDR only. LE does its own negotiation
 */
void attSrvSetMtu(l2c_handle_t to, uint32_t mtu)
{
    pthread_mutex_lock(&mSrvLock);
    attConnMtuSet(to, mtu);
    pthread_mutex_unlock(&mSrvLock);
}

/*
 * FUNCTION: attReadUuid
 * USE:      Read a UUID-128 or a UUID16 from an sg, if it is the only thing there
 * PARAMS:   dst - we store the read UUID here
 *           s - the data
 * RETURN:   success
 * NOTES:
 */
static bool attReadUuid(struct uuid *dst, sg s)
{
    uint8_t buf[16];
    struct uuid *ru = (struct uuid*)buf;

    if (sgLength(s) == sizeof(uint16_t)) {
        sgSerializeCutFront(s, buf, sizeof(uint16_t));
        uuidFromUuid16(dst, utilGetLE16(buf));
    } else if (sgLength(s) == sizeof(struct uuid)) {
        sgSerializeCutFront(s, buf, sizeof(struct uuid));
        uuidReadLE(dst, ru);
    } else
        return false;

    return true;
}

/*
 * FUNCTION: attSrvSendErrorReply
 * USE:      Sends an error to the client
 * PARAMS:   to - whom to send to
 *           err - the error
 *           opcode - opcode for which the error happened
 *           handle - the pertinent handle
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvSendErrorReply(l2c_handle_t to, uint8_t errNo, uint8_t opcode, uint16_t handle)
{
    struct attPduError err;
    struct attHdr hdr;

    utilSetLE8(&hdr.opcode, ATT_OPCODE_ERROR);
    utilSetLE8(&err.reqOpcode, opcode);
    utilSetLE8(&err.err, errNo);
    utilSetLE16(&err.handle, handle);

    return attSrvSend(to, &hdr, sizeof(hdr), NULL, &err, sizeof(err)) == ATT_TX_RET_ACCEPTED;
}

/*
 * FUNCTION: attSrvSendPacket
 * USE:      Sends a packet to a client
 * PARAMS:   to - whom to send to
 *           opcode - opcode to send
 *           hdr - a header to prepend
 *           hdrLen - length of said header
 *           data - the data to send (may be NULL)
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvSendPacket(l2c_handle_t to, uint8_t opcode, const void *hdr, uint32_t hdrLen, sg data)
{
    struct attHdr attHdr;
    bool freeData = false;

    if (!data) {
        data = sgNew();
        if (!data)
            return false;
        freeData = true;
    }

    utilSetLE8(&attHdr.opcode, opcode);

    if (hdr && !sgConcatFrontCopy(data, hdr, hdrLen))
        goto out;

    if (attSrvSend(to, &attHdr, sizeof(attHdr), data, NULL, 0) == ATT_TX_RET_ACCEPTED)
        return true;

    sgTruncFront(data, hdrLen);
    /* fallthough */

out:
    if (freeData)
        sgFree(data);
    return false;
}

/*
 * FUNCTION: attSrvSendEmptyPacket
 * USE:      Sends an empy packet to a client
 * PARAMS:   to - whom to send to
 *           opcode - opcode to send
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvSendEmptyPacket(l2c_handle_t to, uint8_t opcode)
{
    return attSrvSendPacket(to, opcode, NULL, 0, NULL);
}

/*
 * FUNCTION: attValIsReadableBy
 * USE:      Decide if a given value is readable by a given connection
 * PARAMS:   range -the value's range
 *           val - the value
 *           extra - per-connection state for this connection
 * RETURN:   true if allowed to read, false else
 * NOTES:    call with mSrvLock held
 */
static bool attValIsReadableBy(struct attSrvRange *range, struct attSrvVal *val, struct attSrvConnExtra *extra)
{
    if (extra->isEncr && extra->isMitmProtected && (val->perms & ATT_PERM_READ_ENCR_MITM))
        return true;

    if (extra->isEncr && !extra->isMitmProtected && (val->perms & ATT_PERM_READ_ENCR))
        return true;

    if (!extra->isEncr && (val->perms & ATT_PERM_READ))
        return true;

    return false;
}

/*
 * FUNCTION: attValIsWriteableBy
 * USE:      Decide if a given value is writeable by a given connection
 * PARAMS:   range -the value's range
 *           val - the value
 *           extra - per-connection state for this connection
 *           isSigned - is this a signed write?
 * RETURN:   true if allowed to write, false else
 * NOTES:    call with mSrvLock held
 */
static bool attValIsWriteableBy(struct attSrvRange *range, struct attSrvVal *val, struct attSrvConnExtra *extra, bool isSigned)
{
    if (!extra->isEncr && isSigned && (val->perms & ATT_PERM_WRITE_SIGNED))
        return true;

    if (!isSigned && extra->isEncr && extra->isMitmProtected && (val->perms & ATT_PERM_WRITE_ENCR_MITM))
        return true;

    if (!isSigned && extra->isEncr && !extra->isMitmProtected && (val->perms & ATT_PERM_WRITE_ENCR))
        return true;

    if (!isSigned && !extra->isEncr && (val->perms & ATT_PERM_WRITE))
        return true;

    return false;
}

/*
 * FUNCTION: attRangeIsVisibleBy
 * USE:      Decide if a given range is visibile to a given connection
 * PARAMS:   range - the range
 *           extra - per-connection state for this connection
 * RETURN:   true if visible, false else
 * NOTES:    call with mSrvLock held
 */
static bool attRangeIsVisibleTo(struct attSrvRange *range, struct attSrvConnExtra *extra)
{
    return (extra->isLE && range->allowLE) || (!extra->isLE && range->allowEDR);
}

/*
 * FUNCTION: attSrvOpFindInfo
 * USE:      Perform a find info request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvOpFindInfo(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    struct attPduFindInfoRsp rsp;
    bool allUuidsSmall = true;
    struct attSrvRange *range;
    struct attSrvVal *val;
    sg results = NULL;
    bool err = false;

    if (!extra) {
        loge("No extra found for connection\n");
        return attSrvSendErrorReply(to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_INFO_REQ, startHandle);
    }

    if (!startHandle || startHandle > endHandle)
        return attSrvSendErrorReply(to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_INFO_REQ, startHandle);

    results = sgNew();

    if (!results)
        return attSrvSendErrorReply(to, ATT_ERROR_INSUFFICIENT_RSRCS, ATT_OPCODE_FIND_INFO_REQ, startHandle);

    mtu -= sizeof(struct attPduFindInfoRsp) + sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    for (range = mRangesHead; range; range = range->next) {
        if (!attRangeIsVisibleTo(range, extra))
            continue;
        for (val = range->valsHead; val; val = val->next) {
            uint16_t uuid16, handle = range->start + val->offset;

            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto out;

            if (allUuidsSmall && uuidToUuid16(&uuid16, &val->type)) {
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
                    struct uuid uuid;

                    while(numResults--) {
                        if (!sgSerializeCutFront(results, &result16, sizeof(result16)))
                            goto unrecoverable;
                        utilSetLE16(&result.handle, utilGetLE16(&result16.handle));
                        uuidFromUuid16(&uuid, utilGetLE16(&result16.uuid16));
                        uuidWriteLE(&result.uuid128, &uuid);
                        if (!sgConcatBackCopy(results, &result, sizeof(result)))
                            goto unrecoverable;
                    }
                    allUuidsSmall = false;
                }
                utilSetLE16(&result.handle, handle);
                uuidWriteLE(&result.uuid128, &val->type);
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
        return attSrvSendErrorReply(to, ATT_ERROR_ATTRIBUTE_NOT_FOUND, ATT_OPCODE_FIND_INFO_REQ, startHandle);
    }

    /* we're ready to send a reply */
    utilSetLE8(&rsp.format, allUuidsSmall ? ATT_FIND_INFO_RSP_FMT_16 : ATT_FIND_INFO_RSP_FMT_128);
    if (attSrvSendPacket(to, ATT_OPCODE_FIND_INFO_RSP, &rsp, sizeof(rsp), results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */

unrecoverable:
    if (results)
        sgFree(results);
    return attSrvSendErrorReply(to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_INFO_REQ, 0);
}

/*
 * FUNCTION: attSrvOpFindByTypeValContinue
 * USE:      Continue a find by type value request
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpFindByTypeValContinue(struct attSrvConnExtra *extra, l2c_handle_t to, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint16_t startHandle;
    uint16_t endHandle;
    uint32_t mtu;
    sg results;

    if (extra->penOp.origOpcode != ATT_OPCODE_FIND_BY_TYPE_VAL_REQ)
        return false;

    startHandle = extra->penOp.startHandle;
    endHandle = extra->penOp.endHandle;
    mtu = extra->penOp.mtu;
    results = extra->penOp.replyInProgress;

    mtu -= sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    if (lastReadRet == ATT_ERROR_NONE && leastReadLen == extra->penOp.findByTypeValue.wantedLen && !memcmp(lastReadData, extra->penOp.findByTypeValue.wantedVal, leastReadLen)) {

        /* if the last read succeeded, append the result if it is a result */
        struct attPduFindByTypeValRspItem result;

        utilSetLE16(&result.firstHandle, startHandle);
        utilSetLE16(&result.groupEndHandle, startHandle + extra->penOp.findByTypeValue.valGrpLen - 1);

        if (!sgConcatBackCopy(results, &result, sizeof(result)))
            goto done;

        if (sgLength(results) >= mtu)
            goto done;

    } else if (lastReadRet != ATT_FIRST_CONTINUE_CALL) {
        /* if the last read failed - stop searching */
        goto done;
    }

    if (startHandle == endHandle)
        goto done;
    startHandle++;

    for (range = mRangesHead; range; range = range->next) {
        if (!attRangeIsVisibleTo(range, extra))
            continue;
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset;
            uint16_t effectiveGrpLen = val->grpLen ? val->grpLen : 1;


            /* range check */
            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto done;

            /* must be readable */
            if (!attValIsReadableBy(range, val, extra))
                continue;

            /* must be of a given type */
            if (!uuidCmp(&val->type, &extra->penOp.findByTypeValue.uuid))
                continue;

            /* we found a falue to read - schedule it */
            extra->penOp.startHandle = handle;
            extra->penOp.findByTypeValue.valGrpLen = effectiveGrpLen;
            if (!attSrvSchedReadCbk(range, val, to, ATT_READ_FOR_FIND_BY_TYPE_VAL, 0, extra->penOp.findByTypeValue.wantedLen + 1))
                break;

            return false;
        }
    }

done:
    extra->penOp.origOpcode = 0;
    free(extra->penOp.findByTypeValue.wantedVal);

    /* make sure we do not send too much */
    while (sgLength(results) > mtu)
        sgTruncBack(results, sizeof(struct attPduFindByTypeValRspItem));

    /* a lack of results is reported via error */
    if (!sgLength(results)) {
        err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
        startHandle = extra->penOp.origStartHandle;
    } else if (attSrvSendPacket(to, ATT_OPCODE_FIND_BY_TYPE_VAL_RSP, NULL, 0, results))
        return true;

    /* if we fall though to here, try to send an error */
    sgFree(results);
    return attSrvSendErrorReply(to, err, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);
}

/*
 * FUNCTION: attSrvOpFindByTypeValStart
 * USE:      Start a find by type value request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 *           uuid - the uuid to find
 *           s - the value to find
 * RETURN:   success
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpFindByTypeValStart(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct uuid *uuid, sg s)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    sg results = NULL;
    bool ret;

    if (!startHandle || startHandle > endHandle) {
        ret = attSrvSendErrorReply(to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);
        goto out;
    }

    if (!extra)
        loge("No extra found for connection\n");
    else if (extra->penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {

        results = sgNew();
        if (results) {

            extra->penOp.findByTypeValue.wantedVal = malloc(sgLength(s));
            if (extra->penOp.findByTypeValue.wantedVal) {

                sgSerialize(s, 0, sgLength(s), extra->penOp.findByTypeValue.wantedVal);

                extra->penOp.origOpcode = ATT_OPCODE_FIND_BY_TYPE_VAL_REQ;
                extra->penOp.replyInProgress = results;
                extra->penOp.origStartHandle = startHandle;
                extra->penOp.startHandle = startHandle - 1;
                extra->penOp.endHandle = endHandle;
                extra->penOp.mtu = mtu;
                memcpy(&extra->penOp.findByTypeValue.uuid, uuid, sizeof(extra->penOp.findByTypeValue.uuid));
                extra->penOp.findByTypeValue.wantedLen = sgLength(s);
                extra->penOp.findByTypeValue.valGrpLen = 0;

                if (attSrvOpFindByTypeValContinue(extra, to, NULL, 0, ATT_FIRST_CONTINUE_CALL)) {
                    sgFree(s);
                    return true;
                }

                err = ATT_ERROR_UNLIKELY_ERROR;
                extra->penOp.origOpcode = 0;

                free(extra->penOp.findByTypeValue.wantedVal);
            }
            sgFree(results);
        }
    }

    ret = attSrvSendErrorReply(to, err, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);

out:
    if (ret)
        sgFree(s);
    return ret;
}

/*
 * FUNCTION: attSrvOpReadByTypeContinue
 * USE:      Continue a read by type / read by group type request
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpReadByTypeContinue(struct attSrvConnExtra *extra, l2c_handle_t to, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint16_t packetHdrSz;
    uint16_t startHandle;
    uint16_t itemMaxLen;
    uint16_t endHandle;
    uint8_t readReason;
    uint8_t itemHdrSz;
    uint8_t reqOpcode;
    uint8_t rspOpcode;
    uint16_t mtu;
    bool grpType;
    sg results;


    grpType = extra->penOp.readByType.grpType;
    packetHdrSz = grpType ? sizeof(struct attPduReadByGrpTypeRsp) : sizeof(struct attPduReadByTypeRsp);
    itemHdrSz = grpType ? sizeof(struct attGrpValuePair) : sizeof(struct attHandleValuePair);
    itemMaxLen = grpType ? ATT_MAX_LEN_FOR_RD_BY_GRP_TYP : ATT_MAX_LEN_FOR_RD_BY_TYPE;
    readReason = grpType ? ATT_READ_FOR_READ_BY_GRP_TYPE : ATT_READ_FOR_READ_BY_TYPE;
    reqOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_REQ : ATT_OPCODE_READ_BY_TYPE_REQ;
    rspOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_RSP : ATT_OPCODE_READ_BY_TYPE_RSP;

    if (extra->penOp.origOpcode != reqOpcode)
        return false;

    startHandle = extra->penOp.startHandle;
    endHandle = extra->penOp.endHandle;
    mtu = extra->penOp.mtu;
    results = extra->penOp.replyInProgress;

    mtu -= sizeof(struct attHdr) + packetHdrSz; /* mtu is now how many bytes we can produce */
    if (itemMaxLen > mtu - itemHdrSz)
        itemMaxLen = mtu - itemHdrSz;

    if (lastReadRet == ATT_ERROR_NONE) {
        sg thisResult;
        uint16_t perResultSize;

        /* only items of the same size shall be provided, so the first result sets the size */
        if (sgLength(results) && leastReadLen != extra->penOp.readByType.itemLen)
            goto done;
        extra->penOp.readByType.itemLen = leastReadLen;

        /* prepare the per-item header */
        if (grpType) {
            struct attGrpValuePair gvp;
            utilSetLE16(&gvp.handle, startHandle);
            utilSetLE16(&gvp.groupEndHadle, startHandle + extra->penOp.readByType.valGrpLen - 1);

            thisResult = sgNewWithCopyData(&gvp, sizeof(gvp));
        } else {
            struct attHandleValuePair hvp;
            utilSetLE16(&hvp.handle, startHandle);

            thisResult = sgNewWithCopyData(&hvp, sizeof(hvp));
        }

        if (!thisResult) {
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
            goto done;
        }

        if (!sgConcatBackCopy(thisResult, lastReadData, leastReadLen)) {
            sgFree(thisResult);
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
            goto done;
        }

        perResultSize = sgLength(thisResult);

        /* append */
        sgConcat(results, thisResult);

        /*
         * If we cannot fit another result (since they are all the same size), get out.
         * One result is guaranteed to fit!
         */
        if (sgLength(results) + perResultSize > mtu)
            goto done;

    } else if (lastReadRet != ATT_FIRST_CONTINUE_CALL) {
        /* if the last read failed - stop searching */
        goto done;
    }

    if (startHandle == endHandle)
        goto done;
    startHandle++;

    for (range = mRangesHead; range; range = range->next) {
        if (!attRangeIsVisibleTo(range, extra))
            continue;
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset;
            uint16_t effectiveGrpLen = val->grpLen ? val->grpLen : 1;

            /* range check */
            if (handle < startHandle)
                continue;
            if (handle > endHandle)
                goto done;

            /* must be readable */
            if (!attValIsReadableBy(range, val, extra))
                continue;

            /* must be of a given type */
            if (!uuidCmp(&val->type, &extra->penOp.readByType.uuid))
                continue;

            /* we found a falue to read - schedule it */
            extra->penOp.startHandle = handle;
            extra->penOp.readByType.valGrpLen = effectiveGrpLen;
            if (attSrvSchedReadCbk(range, val, to, readReason, 0, itemMaxLen))
                return true;

            goto done;
        }
    }

done:
    extra->penOp.origOpcode = 0;

    if (sgLength(results) > mtu) {
        /* make sure we do not send too much */
        logw("Impossible result length %u over mtu %u in read by [grp] type\n", sgLength(results), mtu);
        startHandle = extra->penOp.origStartHandle;
     } else if (!sgLength(results)) {
        /* a lack of results is reported thusly */
        err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
        startHandle = extra->penOp.origStartHandle;
    } else {
        struct attPduReadByTypeRsp rbt;
        struct attPduReadByGrpTypeRsp gtr;
        void *hdr;

        if (grpType) {
            hdr = &gtr;
            utilSetLE8(&gtr.attGrpValuePairSz, extra->penOp.readByType.itemLen + sizeof(struct attGrpValuePair));
        } else {
            hdr = &rbt;
            utilSetLE8(&rbt.handleValuePairSz, extra->penOp.readByType.itemLen + sizeof(struct attHandleValuePair));
        }

        if (attSrvSendPacket(to, rspOpcode, hdr, packetHdrSz, results))
            return true;
    }

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */
    sgFree(results);
    return attSrvSendErrorReply(to, err, reqOpcode, startHandle);
}

/*
 * FUNCTION: attSrvOpReadByTypeStart
 * USE:      Start a read by type / read by group type request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 *           uuid - the uuid to find
 *           grpType - is this a "read by group type" request ?
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvOpReadByTypeStart(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct uuid *uuid, bool grpType)
{
    const uint8_t reqOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_REQ : ATT_OPCODE_READ_BY_TYPE_REQ;
    const uint8_t rspOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_RSP : ATT_OPCODE_READ_BY_TYPE_RSP;
    const uint8_t readReason = grpType ? ATT_READ_FOR_READ_BY_GRP_TYPE : ATT_READ_FOR_READ_BY_TYPE;
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    sg results = NULL;

    if (!startHandle || startHandle > endHandle)
        return attSrvSendErrorReply(to, ATT_ERROR_INVALID_HANDLE, reqOpcode, startHandle);

    if (!extra)
        loge("No extra found for connection\n");
    else if (extra->penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {
        results = sgNew();
        if (results) {

            extra->penOp.origOpcode = reqOpcode;
            extra->penOp.replyInProgress = results;
            extra->penOp.origStartHandle = startHandle;
            extra->penOp.startHandle = startHandle - 1;
            extra->penOp.endHandle = endHandle;
            extra->penOp.mtu = mtu;
            memcpy(&extra->penOp.readByType.uuid, uuid, sizeof(extra->penOp.readByType.uuid));
            extra->penOp.readByType.grpType = grpType;
            extra->penOp.readByType.valGrpLen = 0;
            extra->penOp.readByType.itemLen = 0;

            if (attSrvOpReadByTypeContinue(extra, to, NULL, 0, ATT_FIRST_CONTINUE_CALL))
                return true;

            err = ATT_ERROR_UNLIKELY_ERROR;
            extra->penOp.origOpcode = 0;

            sgFree(results);
        }
    }

    return attSrvSendErrorReply(to, err, reqOpcode, startHandle);
}

/*
 * FUNCTION: attSrvOpReadByTypeContinue
 * USE:      Continue a read / read blob request
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpReadContinue(struct attSrvConnExtra *extra, l2c_handle_t to, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = lastReadRet;
    uint16_t wantedHandle;
    uint8_t opc, rspOpc;
    sg result;


    if (extra->penOp.origOpcode == ATT_OPCODE_READ_REQ)
        rspOpc = ATT_OPCODE_READ_RSP;
    else if (extra->penOp.origOpcode == ATT_OPCODE_READ_BLOB_REQ)
        rspOpc = ATT_OPCODE_READ_BLOB_RSP;
    else
        return false;

    wantedHandle = extra->penOp.startHandle;
    opc = extra->penOp.origOpcode;
    extra->penOp.origOpcode = 0;
    
    if (err == ATT_ERROR_NONE) {
        result = sgNewWithCopyData(lastReadData, leastReadLen);
        if (!result)
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
        else if (!attSrvSendPacket(to, rspOpc, NULL, 0, result)) {
            err = ATT_ERROR_UNLIKELY_ERROR;
            sgFree(result);
        } else
            return true;
    }

    return attSrvSendErrorReply(to, err, opc, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadStart
 * USE:      Start a read/readBlob request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           wantedHandle - the handle the client cares about
 *           offset - offset to start at (if needed for the opcode)
 *           opcode - the opcode we were called with
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvOpReadStart(l2c_handle_t to, uint32_t mtu, uint16_t wantedHandle, uint16_t offset, uint8_t opcode)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    struct attSrvRange *range;
    struct attSrvVal *val;


    if (!wantedHandle)
        err = ATT_ERROR_INVALID_HANDLE;
    else if (!extra) {
        err = ATT_ERROR_UNLIKELY_ERROR;
        wantedHandle = 0;
        loge("No extra found for connection\n");
    } else if (extra->penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {
        for (range = mRangesHead; range; range = range->next) {
            if (!attRangeIsVisibleTo(range, extra))
                continue;
            for (val = range->valsHead; val; val = val->next) {
                uint8_t readReason = opcode == ATT_OPCODE_READ_REQ ? ATT_READ_FOR_READ : ATT_READ_FOR_READ_BLOB;
                uint16_t handle = range->start + val->offset;

                /* range check */
                if (handle < wantedHandle)
                    continue;
                if (handle > wantedHandle) {
                    err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
                    goto done;
                }

                /* must be readable */
                if (!attValIsReadableBy(range, val, extra)) {
                    err = ATT_ERROR_READ_NOT_ALLOWED;
                    goto done;
                }

                /* do a read */
                extra->penOp.origOpcode = opcode;
                extra->penOp.mtu = mtu;
                extra->penOp.startHandle = wantedHandle;
                extra->penOp.read.offset = offset;

                if (attSrvSchedReadCbk(range, val, to, readReason, offset, mtu - sizeof(struct attHdr)))
                    return true;

                extra->penOp.origOpcode = 0;
                err = ATT_ERROR_UNLIKELY_ERROR;
                goto done;
            }
        }
    }
done:

    return attSrvSendErrorReply(to, err, opcode, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadMultContinue
 * USE:      Continue a read multiple request
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpReadMultContinue(struct attSrvConnExtra *extra, l2c_handle_t to, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint16_t wantedHandle = 0;
    sg results, handles;
    uint16_t mtu;


    if (extra->penOp.origOpcode != ATT_OPCODE_READ_MULT_REQ)
        return false;

    mtu = extra->penOp.mtu;
    results = extra->penOp.replyInProgress;
    handles = extra->penOp.readMult.handles;

    mtu -= sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    if (lastReadRet == ATT_ERROR_NONE) {

        if (!sgConcatBackCopy(results, lastReadData, leastReadLen)) {
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
            goto done;
        }

        /* if no more handles to read, stop */
        if (!sgLength(handles))
            goto done;

       /* even if we go way over mtu, we keep reading because as per spec we need to generate errors correctly */

    } else if (lastReadRet != ATT_FIRST_CONTINUE_CALL) {
        /* if the last read failed - stop searching */
        goto error;
    }

    if (!sgSerializeCutFront(handles, &wantedHandle, sizeof(wantedHandle)))
        goto done;
    wantedHandle = utilGetLE16(&wantedHandle);

    for (range = mRangesHead; range; range = range->next) {
        if (!attRangeIsVisibleTo(range, extra))
            continue;
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset;

            /* range check */
            if (handle < wantedHandle)
                continue;
            if (handle > wantedHandle)
                goto done;

            extra->penOp.readMult.lastReadHandle = wantedHandle;

            /* must be readable */
            if (!attValIsReadableBy(range, val, extra)) {
                err = ATT_ERROR_READ_NOT_ALLOWED;
                goto error;
            }

            /* we found the falue to read - schedule it */
            extra->penOp.startHandle = handle;
            if (attSrvSchedReadCbk(range, val, to, ATT_READ_FOR_READ_MULT, 0, mtu))
                return true;

            goto error;
        }
    }

error:
    sgTruncFront(results, sgLength(results));

done:
    extra->penOp.origOpcode = 0;

    /* mind the length */
    if (sgLength(results) > mtu)
        sgTruncBack(results, sgLength(results) - mtu);

    if (!sgLength(results)) {
        /* a lack of results is reported as an error */
        wantedHandle = extra->penOp.readMult.lastReadHandle;
    } else if (attSrvSendPacket(to, ATT_OPCODE_READ_MULT_RSP, NULL, 0, results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */
    sgFree(results);
    sgFree(handles);
    return attSrvSendErrorReply(to, err, ATT_OPCODE_READ_MULT_REQ, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadMultStart
 * USE:      Start a read multiple request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           s - sg with uint16_t list of handles
 * RETURN:   success
 * NOTES:    call with mSrvLock held. WILL FREE "handles" eventually, if true is returned
 */
static bool attSrvOpReadMultStart(l2c_handle_t to, uint32_t mtu, sg handles)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;

    if (!extra)
        loge("No extra found for connection\n");
    else if (!extra->penOp.origOpcode) {

        extra->penOp.replyInProgress = sgNew();
        if (!extra->penOp.replyInProgress)
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
        else {

            extra->penOp.origOpcode = ATT_OPCODE_READ_MULT_REQ;
            extra->penOp.readMult.handles = handles;
            extra->penOp.mtu = mtu;

            if (attSrvOpReadContinue(extra, to, NULL, 0, ATT_FIRST_CONTINUE_CALL))
                return true;

            sgFree(extra->penOp.replyInProgress);
        }
    }

    if (attSrvSendErrorReply(to, err, ATT_OPCODE_READ_MULT_REQ, 0)) {
        sgFree(handles);
        return true;
    }
    return false;
}

/*
 * FUNCTION: attSrvWriteContinue
 * USE:      Continue a [signed] write request / command
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           lastWriteData - return from the READ call
 *           leastWriteLen - length last read result produced
 *           lastWriteRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvWriteContinue(struct attSrvConnExtra *extra, l2c_handle_t to, const void *lastWriteData, uint16_t leastWriteLen, uint8_t lastWriteRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;


    if (extra->penOp.origOpcode != ATT_OPCODE_WRITE_REQ && extra->penOp.origOpcode != ATT_OPCODE_SIGNED_WRITE_CMD &&
         extra->penOp.origOpcode != ATT_OPCODE_WRITE_REQ && extra->penOp.origOpcode != ATT_OPCODE_PREPARE_WRITE_REQ)
        return false;

    extra->penOp.origOpcode = 0;

    if (extra->penOp.origOpcode == ATT_OPCODE_WRITE_REQ)
        return attSrvSendEmptyPacket(to, ATT_OPCODE_WRITE_RSP);
    if (extra->penOp.origOpcode == ATT_OPCODE_PREPARE_WRITE_REQ)
        return attSrvSendPacket(to, ATT_OPCODE_PREPARE_WRITE_RSP, lastWriteData, leastWriteLen, NULL);
    return true;
}

/*
 * FUNCTION: attSrvWriteStart
 * USE:      Start a [signed] write request / command
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           wantedHandle - the handle to write
 *           data - sg with the data to write
 *           byteOfst - offset into the attribute to write
 *           opcode - the opcode the brought us here
 *           writeReason - the write reason to pass to write callback
 * RETURN:   success
 * NOTES:    call with mSrvLock held
 */
static bool attSrvWriteStart(l2c_handle_t to, uint32_t mtu, uint16_t wantedHandle, sg data, uint16_t byteOfst, uint8_t opcode, uint8_t writeReason)
{
    struct attSrvConnExtra *extra = attConnListGetExtra(&mAllClients, to);
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;

    if (!extra)
        loge("No extra found for connection\n");
    else if (!extra->penOp.origOpcode) {

        for (range = mRangesHead; range; range = range->next) {
            if (!attRangeIsVisibleTo(range, extra))
                continue;
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
        else if (!attValIsWriteableBy(range, val, extra, writeReason == ATT_WRITE_FOR_SIGNED_WRITE_CMD))
            err = ATT_ERROR_WRITE_NOT_ALLOWED;
        else {
            extra->penOp.origOpcode = opcode;
            extra->penOp.mtu = mtu;
            extra->penOp.startHandle = wantedHandle;
            if (attSrvSchedWriteCbk(range, val, to, writeReason, 0, data))
                return true;
            extra->penOp.origOpcode = 0;
        }
    }

    if (attSrvSendErrorReply(to, err, ATT_OPCODE_WRITE_REQ, wantedHandle)) {
        sgFree(data);
        return true;
    }
    return false;
}

/*
 * FUNCTION: attSrvOpSignedWrite
 * USE:      Perform a signed write command
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           handle - the handle to write
 *           rxedSig - the SIG we got
 *           data - sg with the data to write
 * RETURN:   success
 * NOTES:    call with mSrvLock held  WILL NOT FREE "data". Caller must!
 */
static bool attSrvOpSignedWrite(l2c_handle_t to, uint32_t mtu, uint16_t handle, const uint8_t *rxedSig, sg data)
{
    uint8_t hdr[sizeof(struct attHdr) + sizeof(struct attPduSignedWriteCmd)];
    struct attHdr *attHdr = (struct attHdr*)hdr;
    struct attPduSignedWriteCmd *cmd = (struct attPduSignedWriteCmd*)(hdr + 1);
    uint8_t calcedSig[ATT_SIG_LEN];
    uint8_t k[SM_BLOCK_LEN];
    struct bt_addr addr;

    if (!l2cApiGetBtAddr(to, &addr)) {
        logw("Cannot get address for signed write. Dropping\n");
        goto out;
    }

    /* XXX: may need to resolve address first */
    if (!persistGetDevKey(&addr, KEY_TYPE_CSRK, k)) {
        logw("Cannot get CSRK for signed write. Dropping\n");
        goto out;
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
        goto out;
    }

    return attSrvWriteStart(to, mtu, handle, data, 0, ATT_OPCODE_SIGNED_WRITE_CMD, ATT_WRITE_FOR_SIGNED_WRITE_CMD);

out:
    sgFree(data);
    return true;
}


/*
 * FUNCTION: attSrvOpExecuteWriteContinue
 * USE:      Continue an execute write request
 * PARAMS:   extra - the extra struct
 *           to - the connection handle
 *           execResult - the result of the execution attempt
 *           errorHandle - in case of error stores the handle the caused it
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mSrvLock held
 */
static bool attSrvOpExecuteWriteContinue(struct attSrvConnExtra *extra, l2c_handle_t to, uint8_t execResult, uint16_t errorHandle)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;


    if (extra->penOp.origOpcode != ATT_OPCODE_EXECUTE_WRITE_REQ)
        return false;

    extra->penOp.origOpcode = 0;

    if (!execResult) {
        if (attSrvSendEmptyPacket(to, ATT_OPCODE_EXECUTE_WRITE_RSP))
            return true;
        execResult = ATT_ERROR_UNLIKELY_ERROR;
    }

    return attSrvSendErrorReply(to, execResult, ATT_OPCODE_EXECUTE_WRITE_REQ, errorHandle);
}

/*
 * FUNCTION: attSrvOpExecuteWriteStart
 * USE:      Start an execute write request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           flags - ATT_EXECUTE_WRITE_FLG_*
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvOpExecuteWriteStart(l2c_handle_t to, uint32_t mtu, uint8_t flags)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvConnExtra* extra;
    bool ret = ATT_ERROR_NONE;

    extra = attConnListGetExtra(&mAllClients, to);
    if (!extra) {
        /* no tmuch to do here, nothing we CAN do */
    } else if (flags == ATT_EXECUTE_WRITE_FLG_CANCEL || flags == ATT_EXECUTE_WRITE_FLG_EXECUTE) {
        if (attSrvSchedExecCbk(to, flags == ATT_EXECUTE_WRITE_FLG_EXECUTE))
            return true;
    } else {
        err = ATT_ERROR_INVALID_PDU;
        goto err;
    }

    if (ret == ATT_ERROR_NONE && attSrvSendEmptyPacket(to, ATT_OPCODE_EXECUTE_WRITE_RSP))
        return true;
    /* if we fail to send, fall through and try to signal an error */

err:
    return attSrvSendErrorReply(to, ret, ATT_OPCODE_EXECUTE_WRITE_REQ, 0);
}

/*
 * FUNCTION: attSrvOpHandleValueConf
 * USE:      Handle a value confirmation
 * PARAMS:   to - whom to reply to (if any)
 *           mtu - the mtu to use
 * RETURN:   success
 * NOTES:    call with mSrvLock help
 */
static bool attSrvOpHandleValueConf(l2c_handle_t to, uint32_t mtu)
{
    struct attSrvConnExtra *extra;

    extra = attConnListGetExtra(&mAllClients, to);
    if (!extra)
        loge("Failed to find extra for confirmation\n");
    else if (extra->indTimer) { /* we're not late */
        attSrvSchedIndResultCbk(extra->indRangeRef, extra->indOffset, to, ATT_SRV_EVT_IND_ACKED, extra->indRef);
        timerCancel(extra->indTimer);
        extra->indTimer = 0;
    } else /* we are late */
        logw("Confirmation is late\n");

    pthread_mutex_lock(&mSrvLock);
    return true;
}

/*
 * FUNCTION: attSrvDataRx
 * USE:      Called when data arrives from a given connection
 * PARAMS:   from - whom the data arived from
 *           s - the data
 * RETURN:   NONE
 * NOTES:
 */
void attSrvDataRx(l2c_handle_t from, sg s)
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
    struct uuid uuid;
    struct attHdr hdr;
    bool ret = false;
    uint32_t newMtu;
    uint8_t opcode;
    uint32_t mtu;

    if (!sgSerializeCutFront(s, &hdr, sizeof(hdr))) {
        loge("ATT cannot find header\n");
        sgFree(s);
        return;
    }

    opcode = utilGetLE8(&hdr.opcode);

    /* add to known client list */
    pthread_mutex_lock(&mSrvLock);
    attConnListAdd(&mAllClients, from);

    switch (opcode) {
    case ATT_OPCODE_MTU_EXCH_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &mtuExch, sizeof(mtuExch)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            newMtu = utilGetLE16(&mtuExch.mtu);
            mtu = attConnMtuGet(from, false);
            if (!mtu || mtu > newMtu)
                mtu = newMtu;
            attConnMtuSet(from, mtu);
            utilSetLE16(&mtuExch.mtu, mtu);
            ret = attSrvSendPacket(from, ATT_OPCODE_MTU_EXCH_RSP, &mtuExch, sizeof(mtuExch), NULL);
            attSrvSchedMtuCbk(from, mtu);
        }
        break;

    case ATT_OPCODE_FIND_INFO_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &findInfoReq, sizeof(findInfoReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpFindInfo(from, mtu, utilGetLE16(&findInfoReq.startHandle), utilGetLE16(&findInfoReq.endHandle));
        break;

    case ATT_OPCODE_FIND_BY_TYPE_VAL_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &findByTypeValReq, sizeof(findByTypeValReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            uuidFromUuid16(&uuid, utilGetLE16(&findByTypeValReq.uuid16));
            if (attSrvOpFindByTypeValStart(from, mtu, utilGetLE16(&findByTypeValReq.startHandle), utilGetLE16(&findByTypeValReq.endHandle), &uuid, s)) {
                ret = true;
                s = NULL;
            }
        }
        break;

    case ATT_OPCODE_READ_BY_TYPE_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &readByTypeReq, sizeof(readByTypeReq)) || !attReadUuid(&uuid, s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByTypeStart(from, mtu, utilGetLE16(&readByTypeReq.startHandle), utilGetLE16(&readByTypeReq.endHandle), &uuid, false);
        break;

    case ATT_OPCODE_READ_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &readReq, sizeof(readReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadStart(from, mtu, utilGetLE16(&readReq.handle), 0, opcode);
        break;

    case ATT_OPCODE_READ_BLOB_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &readBlobReq, sizeof(readBlobReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadStart(from, mtu, utilGetLE16(&readBlobReq.handle), utilGetLE16(&readBlobReq.offset), opcode);
        break;

    case ATT_OPCODE_READ_MULT_REQ:
        mtu = attConnMtuGet(from, true);
        if ((sgLength(s) % sizeof(uint16_t)) || sgLength(s) < sizeof(uint16_t[ATT_MIN_HANDLES_IN_READ_MULT]))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvOpReadMultStart(from, mtu, s)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_READ_BY_GRP_TYPE_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &readByGrpType, sizeof(readByGrpType)) || !attReadUuid(&uuid, s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByTypeStart(from, mtu, utilGetLE16(&readByGrpType.startHandle), utilGetLE16(&readByGrpType.endHandle), &uuid, true);
        break;

    case ATT_OPCODE_WRITE_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &writeReq, sizeof(writeReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvWriteStart(from, mtu, utilGetLE16(&writeReq.handle), s, 0, opcode, ATT_WRITE_FOR_WRITE_REQ)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_PREPARE_WRITE_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &prepareWriteReq, sizeof(prepareWriteReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvWriteStart(from, mtu, utilGetLE16(&prepareWriteReq.handle), s, utilGetLE16(&prepareWriteReq.offset), opcode, ATT_WRITE_FOR_PREPARE)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_EXECUTE_WRITE_REQ:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &execWriteReq, sizeof(execWriteReq)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpExecuteWriteStart(from, mtu, utilGetLE16(&execWriteReq.flags));
        break;

    case ATT_OPCODE_HANDLE_VALUE_CONF:
        mtu = attConnMtuGet(from, true);
        if (sgLength(s))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpHandleValueConf(from, mtu);
        break;

    case ATT_OPCODE_WRITE_CMD:
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &writeCmd, sizeof(writeCmd)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvWriteStart(from, mtu, utilGetLE16(&writeReq.handle), s, 0, opcode, ATT_WRITE_FOR_WRITE_CMD)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_SIGNED_WRITE_CMD:
        if (l2cApiIsConnEncrypted(from)) {
            /* ignore signed writes on encrypted connections as per spec */
            ret = true;
            break;
        }
        mtu = attConnMtuGet(from, true);
        if (!sgSerializeCutFront(s, &signedWriteCmd, sizeof(signedWriteCmd)))
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s) < ATT_SIG_LEN)
            ret = attSrvSendErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            uint8_t sig[ATT_SIG_LEN];
            sgSerialize(s, sgLength(s) - ATT_SIG_LEN, ATT_SIG_LEN, sig);
            sgTruncBack(s, ATT_SIG_LEN);
            if (attSrvOpSignedWrite(from, mtu, utilGetLE16(&signedWriteCmd.handle), sig, s)) {
                ret = true;
                s = NULL;
            }
        }
        break;

    default:
        logw("Unexpected ATT opcode 0x%02X\n", opcode);
        ret = attSrvSendErrorReply(from, ATT_ERROR_REQ_NOT_SUPPORTED, opcode, 0);
        break;
    }

out:
    pthread_mutex_unlock(&mSrvLock);
    if (!ret)
        loge("ATT failed to handle this message\n");
    if (s)
        sgFree(s);
}

/*
 * FUNCTION: attSrvCbkReply
 * USE:      Called by higher layer when a callback we made to them completes
 * PARAMS:   cid - the connection ID in android-gatt terms
 *           transId - the transaction Id we originally passed to the higher layer
 *           handle - the handle value (if relevant -> only in exec write)
 *           err - ATT_ERROR_*
 *           data - the read data
 *           len - read length
 * RETURN:   unused
 * NOTES:
 */
void attSrvCbkReply(int cid, int transId, uint16_t handle, uint8_t err, const void *data, uint16_t len)
{
    struct attSrvConnExtra* extra; //attConnListGetExtra(struct attConnList *list, l2c_handle_t conn)
    l2c_handle_t who;
    uint8_t opcode;
    bool ret = false;

    pthread_mutex_lock(&mSrvLock);
    if (!attConnListFindByCid(&mAllClients, cid, &who, &extra))
        loge("Got callback reply for unknown connection\n");
    else if (extra->nextTransId != transId)
        loge("Got callback reply for unknown transcation id %u\n", transId);
    else {
        switch (extra->penOp.origOpcode) {
        case ATT_OPCODE_FIND_BY_TYPE_VAL_REQ:
            ret = attSrvOpFindByTypeValContinue(extra, who, data, len, err);
            break;
        case ATT_OPCODE_READ_BY_TYPE_REQ:
        case ATT_OPCODE_READ_BY_GRP_TYPE_REQ:
            ret = attSrvOpReadByTypeContinue(extra, who, data, len, err);
            break;
        case ATT_OPCODE_READ_REQ:
        case ATT_OPCODE_READ_BLOB_REQ:
            ret = attSrvOpReadContinue(extra, who, data, len, err);
            break;
        case ATT_OPCODE_READ_MULT_REQ:
            ret = attSrvOpReadMultContinue(extra, who, data, len, err);
            break;
        case ATT_OPCODE_WRITE_REQ:
        case ATT_OPCODE_SIGNED_WRITE_CMD:
        case ATT_OPCODE_WRITE_CMD:
        case ATT_OPCODE_PREPARE_WRITE_REQ:
            ret = attSrvWriteContinue(extra, who, data, len, err);
            break;
        case ATT_OPCODE_EXECUTE_WRITE_REQ:
            ret = attSrvOpExecuteWriteContinue(extra, who, err, handle);
            break;
        default:
            loge("Unknown original opcode\n");
            extra->penOp.origOpcode = 0;
        }
    }
    pthread_mutex_unlock(&mSrvLock);
}

/*
 * FUNCTION: attWorker
 * USE:      ATT worker thread
 * PARAMS:   unused - unused
 * RETURN:   unused
 * NOTES:
 */
static void* attWorker(void *unused)
{
    struct attWorkItem *wi;
    int status;

    pthread_setname_np(pthread_self(), "att_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case WORK_TYPE_READ_CALL:
            if (!mReadCbk(wi->who, wi->cid, wi->read.range, wi->read.ofst, wi->read.transId, wi->read.byteOfst, wi->read.reason, wi->read.maxLen))
                attSrvCbkReply(wi->cid, wi->read.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_TYPE_WRITE_CALL:
            if (!mWriteCbk(wi->who, wi->cid, wi->write.range, wi->write.ofst, wi->write.transId, wi->write.byteOfst, wi->write.reason, wi->write.len, wi + 1))
                attSrvCbkReply(wi->cid, wi->write.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_TYPE_EXEC_WRITE_CALL:
            if (!mExecCbk(wi->who, wi->cid, wi->execWrite.transId, wi->execWrite.exec))
                attSrvCbkReply(wi->cid, wi->execWrite.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_TYPE_IND_RESULT:
            mIndCbk(wi->who, wi->cid, wi->indResult.range, wi->indResult.ofst, wi->indResult.evt, wi->indResult.ref);
            break;

        case WORK_TYPE_MTU_INFO:
            mMtuCbk(wi->who, wi->mtuInfo.mtu);
            break;

        case WORK_TYPE_SYNC:
            sem_post(wi->sync.sem);
            break;

        default:
            loge("Unknown work type %d\n", wi->type);
            break;
        }
        free(wi);
    }

    logd("ATT worker exiting\n");
    return NULL;
}

/*
 * FUNCTION: attWorkItemFree
 * USE:      Free an ATT work item at time of work quque destruction
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
static void attWorkItemFree(void *item)
{
    struct attWorkItem *wi = (struct attWorkItem*)item;

    /* future clean steps here */

    free(wi);
}

/*
 * FUNCTION: attInit
 * USE:      Called to init all of ATT
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool attInit(void)
{
    mWorkQ = workQueueAlloc(ATT_WORK_Q_SIZE);
    if (!mWorkQ) {
        loge("Failed to create work Q\n");
        goto fail_workq;
    }

    if (pthread_create(&mWorker, NULL, attWorker, NULL)) {
        loge("Failed to create worker\n");
        goto fail_thread;
    }

    mAllClients.hasExtras = true;

    return true;

fail_thread:
    workQueueFree(mWorkQ, attWorkItemFree);

fail_workq:
    return false;
}

/*
 * FUNCTION: attDeinit
 * USE:      Called to deinit all of ATT
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void attDeinit(void)
{
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);
    workQueueFree(mWorkQ, attWorkItemFree);
}


