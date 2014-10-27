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

struct attConn {

    /* list */
    struct attConn *next;
    struct attConn *prev;

    /* comon for client and server */
    l2c_handle_t conn;
    uint32_t mtu;
    bool isEncr;
    bool isMitmProtected;
    bool isLE;

    /* used if we're acting as client on this connection */
    struct {
        uniq_t ongoingTrans; /* is a timer ID */
    } cli;

    /* used if we're acting as server */
    struct {
        att_range_t indRangeRef;
        uniq_t indTimer;
        uint64_t indRef;
        uint16_t indOffset;

        int cid; /* for higher layer */
        int nextTransId;

        struct attSrvPendingOp penOp;
    } srv;
};

struct attConnListNode {
    struct attConnListNode *next;
    l2c_handle_t conn;
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
    struct attConnListNode *seenBy;
    bool allowLE;
    bool allowEDR;
};

struct attWorkItem {
    uint8_t type;
    l2c_handle_t who;
    union {
        struct {
            int cid; /* having both this and "who" here makes life a bit easier */
        } srv;
        struct {
            uniq_t trans;
        } cli;
    };

    union {
        struct {
            att_range_t          range;
            uint16_t             ofst;
            int                  transId;
            uint16_t             byteOfst;
            uint16_t             maxLen;
            uint8_t              reason;
        } srvRead;
        struct {
            att_range_t          range;
            uint16_t             ofst;
            int                  transId;
            uint16_t             byteOfst;
            uint16_t             len;
            uint8_t              reason;
            /* data follows */
        } srvWrite;
        struct {
            int                  transId;
            bool                 exec;
        } srvExecWrite;
        struct {
            att_range_t          range;
            uint16_t             ofst;
            uint64_t             ref;
            uint8_t              evt;
        } srvIndResult;
        struct {
            uint16_t             mtu;
            bool                 asServer;
        } mtuInfo;
        struct {
            sem_t               *sem;
        } sync;
        struct {
            sg                   data;
        } cliJustData;
        struct {
            uint16_t             handle;
            uint8_t              err;
            bool                 isAttErr; /* else is timout and no err info is avail */
        } cliErr;
        struct {
            uint16_t             handle;
            bool                 needsAck;
            sg                   data;
        } cliIndNotif;
    };
};

#define WORK_ATT_MTU_INFO                0
#define WORK_ATT_SYNC                    1
#define WORK_SRV_READ_CALL               2
#define WORK_SRV_WRITE_CALL              3
#define WORK_SRV_EXEC_WRITE_CALL         4
#define WORK_SRV_IND_RESULT              5
#define WORK_CLI_FIND_INFO_CALL          6
#define WORK_CLI_FIND_BY_TYPE_VAL_CALL   7
#define WORK_CLI_READ_BY_TYPE_CALL       8
#define WORK_CLI_READ_CALL               9 /* or read blob */
#define WORK_CLI_READ_BY_GRP_TYPE_CALL   10
#define WORK_CLI_WRITE_CALL              11
#define WORK_CLI_ERR_CALL                12
#define WORK_CLI_IND_NOTIF_CALL          13


/* our state */
static pthread_mutex_t mAttLock = PTHREAD_MUTEX_INITIALIZER;
static struct attSrvRange *mRangesHead;
static struct attSrvRange *mRangesTail;

static attSrvTxCbk mTxCbk;
static attMtuCbk mSrvMtuCbk;
static attSrvIndResultCbk mIndCbk;
static attSrvValueExecWriteCbk mExecCbk;
static attSrvValueReadCbk mReadCbk;
static attSrvValueWriteCbk mWriteCbk;

static attMtuCbk mCliMtuCbk;
static attCliReadCbk mCliReadF;
static attCliWriteCbk mCliWriteF;
static attCliError mCliErrF;
static attCliIndNotif mCliIndNotifF;
static attCliFindInfoCbk mCliFindInfoCbk;
static attCliFindByTypeValCbk mCliFindByTypeValCbk;

static struct attConn *mConns;
static bool mInTeardown;
static struct workQueue *mWorkQ;
static pthread_t mWorker;
static unsigned mNumUsers = 0; /* of ATT itself */




/*
 * FUNCTION: attConnFindByL2cConn
 * USE:      Find a per-connection state given an l2cap connection handle
 * PARAMS:   who - l2cap connection handle
 * RETURN:   struct attConn or NULL if not found
 * NOTES:    plase call while holding mAttLock
 */
static struct attConn* attConnFindByL2cConn(l2c_handle_t who)
{
    struct attConn *conn = mConns;

    while (conn && conn->conn != who)
        conn = conn->next;

    return conn;
}

/*
 * FUNCTION: attConnFindBySrvCid
 * USE:      Find a per-connection state given a server "cid" value (used by android if sadly)
 * PARAMS:   srvCid - the cid
 * RETURN:   struct attConn or NULL if not found
 * NOTES:    plase call while holding mAttLock
 */
static struct attConn* attConnFindBySrvCid(int srvCid)
{
    struct attConn *conn = mConns;

    while (conn && conn->srv.cid != srvCid)
        conn = conn->next;

    return conn;
}

/*
 * FUNCTION: attConnListFree
 * USE:      Free a conn-list
 * PARAMS:   listP - where list head is stored
 * RETURN:   NONE
 * NOTES:    call while holding mAttLock
 */
static void attConnListFree(struct attConnListNode **listP)
{
    while (*listP) {
        struct attConnListNode *t = (*listP)->next;
        free(*listP);
        *listP = t;
    }
}

/*
 * FUNCTION: attConnListIsEmpty
 * USE:      See if a conn-list has items
 * PARAMS:   listP - where list head is stored
 * RETURN:   true if an item is found
 * NOTES:    call while holding mAttLock
 */
static bool attConnListIsEmpty(struct attConnListNode **listP)
{
    return !*listP;
}

/*
 * FUNCTION: attConnListDel
 * USE:      Delete a given connection from a conn-list
 * PARAMS:   listP - where list head is stored
 *           conn - the connection to remove
 * RETURN:   NONE
 * NOTES:    call while holding mAttLock
 */
static void attConnListDel(struct attConnListNode **listP, l2c_handle_t conn)
{
    struct attConnListNode *c = *listP, *p = NULL;

    while (c && c->conn != conn) {
        p = c;
        c = c->next;
    }

    if (c) {
        if (p)
            p->next = c->next;
        else
            *listP = c->next;

        free(c);
    }
}

/*
 * FUNCTION: attWorkItemAlloc
 * USE:      Allocate a work item of a given type
 * PARAMS:   type - the type
 *           extraSz - the extra size to allocate
 * RETURN:   success
 * NOTES:
 */
static struct attWorkItem* attWorkItemAlloc(uint8_t type, uint32_t extraSz)
{
    struct attWorkItem *wi = (struct attWorkItem*)calloc(1, sizeof(struct attWorkItem) + extraSz);
    if (wi)
        wi->type = type;

    return wi;
}

/*
 * FUNCTION: attSrvSchedCbkGetNextTid
 * USE:      Get the transaction ID to use
 * PARAMS:   conn - the per-connection data
 * RETURN:   the transaction ID to use (positive int)
 * NOTES:    call with mAttLock held
 */
static int attSrvSchedCbkGetNextTid(struct attConn* conn)
{
    int tid;

    conn->srv.nextTransId++;
    if (conn->srv.nextTransId < 0)
        conn->srv.nextTransId = 1;

    return conn->srv.nextTransId;
}

/*
 * FUNCTION: attSrvSchedReadCbk
 * USE:      Schedule a call to the read callback
 * PARAMS:   range - the handle range
 *           val - the value struct
 *           conn - the per-connection structure
 *           reason - reason value to pass to read callback
 *           byteOfst - byte offset to pass to read callback
 *           maxLen - maxLength to read
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mAttLock held Do not release the lock until all your state in conn->penOp is set up
 */
static bool attSrvSchedReadCbk(struct attSrvRange *range, struct attSrvVal* val, struct attConn* conn, uint8_t reason, uint16_t byteOfst, uint16_t maxLen)
{
    struct attWorkItem *wi;
    int tid;

    wi = attWorkItemAlloc(WORK_SRV_READ_CALL, 0);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(conn);

    wi->who = conn->conn;
    wi->srv.cid = conn->srv.cid;
    wi->srvRead.range = range->rangeRef;
    wi->srvRead.ofst = val->offset;
    wi->srvRead.transId = tid;
    wi->srvRead.byteOfst = byteOfst;
    wi->srvRead.maxLen = maxLen;
    wi->srvRead.reason = reason;

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
 *           conn - the per-connection structure
 *           reason - reason value to pass to write callback
 *           replace - repalce the existing slot (else will fail if slot not empty)
 *           byteOfst - byte offset to pass to write callback
 *           data - the sg with the data to write
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mAttLock held  Do not release the lock until all state in conn->penOp is set up. WILL NOT FREE "data". Caller must!
 */
static bool attSrvSchedWriteCbk(struct attSrvRange *range, struct attSrvVal* val, struct attConn* conn, uint8_t reason, uint16_t byteOfst, sg data)
{
    uint16_t len = sgLength(data);
    struct attWorkItem *wi;
    const void *dataPtr = NULL;
    bool needFree = false;
    void *iter;
    bool ret;
    int tid;


    wi = attWorkItemAlloc(WORK_SRV_WRITE_CALL, len);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(conn);
    sgSerialize(data, 0, len, wi + 1);

    wi->who = conn->conn;
    wi->srv.cid = conn->srv.cid;
    wi->srvWrite.range = range->rangeRef;
    wi->srvWrite.ofst = val->offset;
    wi->srvWrite.transId = tid;
    wi->srvWrite.byteOfst = byteOfst;
    wi->srvWrite.len = len;
    wi->srvWrite.reason = reason;

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
 * PARAMS:   conn - the per-connection structure
 *           exec - true to execute, false to clear state
 * RETURN:   true if we should expect callback. false on immediate failute
 * NOTES:    call with mAttLock held  Do not release the lock until all state in conn->penOp is set up. WILL NOT FREE "data". Caller must!
 */
static bool attSrvSchedExecCbk(struct attConn* conn, bool exec)
{
    struct attWorkItem *wi;
    int tid;

    wi = attWorkItemAlloc(WORK_SRV_EXEC_WRITE_CALL, 0);
    if (!wi)
        return false;

    tid = attSrvSchedCbkGetNextTid(conn);

    wi->who = conn->conn;
    wi->srv.cid = conn->srv.cid;
    wi->srvExecWrite.transId = tid;
    wi->srvExecWrite.exec = exec;

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
 *           conn - the per-connection structure
 *           evt - what happened?
 *           ref - the reference value provided by the higher layer
 * RETURN:   success
 * NOTES:    call with mAttLock held Do not release the lock until all state in conn->penOp is set up
 */
static bool attSrvSchedIndResultCbk(att_range_t rangeRef, uint16_t offst, struct attConn* conn, uint8_t evt, uint64_t ref)
{
    struct attWorkItem *wi;

    wi = attWorkItemAlloc(WORK_SRV_IND_RESULT, 0);
    if (!wi)
        return false;

    wi->who = conn->conn;
    wi->srv.cid = conn->srv.cid;
    wi->srvIndResult.range = rangeRef;
    wi->srvIndResult.ofst = offst;
    wi->srvIndResult.ref = ref;
    wi->srvIndResult.evt = evt;

    if (workQueuePut(mWorkQ, wi))
        return true;

    free(wi);
    return false;
}

/*
 * FUNCTION: attSrvSchedMtuCbk
 * USE:      Schedule a call to the MTU callback
 * PARAMS:   conn - the per-connection structure
 *           mtu - the new mtu
 *           asServer - are we acting as server for this negotiation
 * RETURN:   success
 * NOTES:    call with mAttLock held Do not release the lock until all state in conn->penOp is set up
 */
static bool attSrvSchedMtuCbk(struct attConn* conn, uint32_t mtu, bool asServer)
{
    struct attWorkItem *wi;
    
    wi = attWorkItemAlloc(WORK_ATT_MTU_INFO, 0);
    if (!wi)
        return false;

    wi->who = conn->conn;
    wi->srv.cid = conn->srv.cid;
    wi->mtuInfo.mtu = mtu;
    wi->mtuInfo.asServer = asServer;

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

    wi = attWorkItemAlloc(WORK_ATT_SYNC, 0);
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
 * FUNCTION: attConnListAddIfNew
 * USE:      Add a given connection to a connection list, if it is not yet present there
 * PARAMS:   l2cConn - the l2c connection handle
 * RETURN:   false on error
 * NOTES:    call with mAttLock held
 */
static bool attConnListAddIfNew(l2c_handle_t l2cConn)
{
    struct attConn *conn;
    struct bt_addr peer;
    uint32_t i;

    /* check for existence & alloc as needed */
    if (attConnFindByL2cConn(l2cConn))
        return true;
    conn = calloc(1, sizeof(struct attConn));
    if (!conn)
        return false;

    /* grab the peer address */
    conn->conn = l2cConn;
    if (!l2cApiGetBtAddr(l2cConn, &peer)) {
        loge("Failed to get peer address\n");
        free(conn);
        return false;
    }

    /* get initial encr and mitm state */
    conn->isEncr = l2cApiIsConnEncrypted(conn->conn);
    conn->isMitmProtected = l2cApiIsConnMitmSafe(conn->conn);
    conn->isLE = BT_ADDR_IS_LE(peer);

    /* find a free server CID */
    conn->srv.cid = mConns ? mConns->srv.cid : 1;
    while (attConnFindBySrvCid(conn->srv.cid))
        if (++conn->srv.cid < 0)
            conn->srv.cid = 1;

    /* link it in */
    conn->next = mConns;
    if (mConns)
        mConns->prev = conn;
    mConns = conn;

    return true;
}

/*
 * FUNCTION: attEncrAuthUpdate
 * USE:      Update the ATT state about a change in a connection to a client pertaining to auth/encr
 * PARAMS:   who - the connection to the client
 *           encr - is it now encrypted?
 *           mitmSafe - is it now mitm-safe?
 * RETURN:   success
 * NOTES:
 */
bool attEncrAuthUpdate(l2c_handle_t who, bool encr, bool mitmSafe)
{
    struct attConn *conn;
    bool ret = false;

    pthread_mutex_lock(&mAttLock);
    attConnListAddIfNew(who);
    conn = attConnFindByL2cConn(who);
    if (conn) {
        conn->isEncr = encr;
        conn->isMitmProtected = mitmSafe;
        ret = true;
    }
    pthread_mutex_unlock(&mAttLock);

    return true;
}

/*
 * FUNCTION: attSrvInit
 * USE:      Init our ATT server
 * PARAMS:   txF - callback to use to TX data to clients
 *           srvMtuF - callback to tell higher-leyer of new MTU
 *           indF - callback to tell higher-layer of ind TX progress
 *           execF - callback to execute or cancel pending writes
 *           readF - read callback
 *           writeF - write callback
 * RETURN:   success
 * NOTES:
 */
bool attSrvInit(attSrvTxCbk txF, attMtuCbk srvMtuF, attSrvIndResultCbk indF, attSrvValueExecWriteCbk execF, attSrvValueReadCbk readF, attSrvValueWriteCbk writeF)
{
    bool ret = false;

    pthread_mutex_lock(&mAttLock);
    if (mTxCbk)
        loge("RE-initing ATT server\n");
    else {
        mTxCbk = txF;
        mSrvMtuCbk = srvMtuF;
        mIndCbk = indF;
        mExecCbk = execF;
        mReadCbk = readF;
        mWriteCbk = writeF;
        ret = true;
    }
    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attSrvDeinit
 * USE:      Deinit our ATT server
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void attSrvDeinit(void)
{
    struct attSrvRange *range;
    struct attSrvVal *val;
    struct attConn *conn;
    int newCid;
    uint32_t i;

    pthread_mutex_lock(&mAttLock);
    for (conn = mConns; conn; conn = conn->next) {
        if (conn->srv.indTimer) {
            if (timerCancel(conn->srv.indTimer))
                attSrvSchedIndResultCbk(conn->srv.indRangeRef, conn->srv.indOffset, conn, ATT_SRV_EVT_IND_SRV_DESTROYED, conn->srv.indRef);
            conn->srv.indTimer = 0;
        }
    }
    /* all uncancelled timers need to be run before we destroy state, so we let them */
    pthread_mutex_unlock(&mAttLock);
    timerSync();
    pthread_mutex_lock(&mAttLock);

    for (range = mRangesHead; range; range = range->next) {
        for (val = range->valsHead; val; ) {
            struct attSrvVal *t = val;
            val = val->next;
            free(t);
        }
        attConnListFree(&range->seenBy);
    }

    pthread_mutex_unlock(&mAttLock);

    mTxCbk = NULL;
    mSrvMtuCbk = NULL;
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
 * FUNCTION: attNotifConnClose
 * USE:      tell the ATT code about a closed connection (in case it cares)
 * PARAMS:   l2cConn - the l2c connection handle
 * RETURN:   NONE
 * NOTES:    if said connection substribed to notifications, allows us to unsubstribe
 *           them proactively instead of waiting for TX errors
 */
void attNotifConnClose(l2c_handle_t l2cConn)
{
    struct attSrvRange *range, *t;
    struct attSrvVal *val;
    struct attConn *conn;
    uint32_t i;

    pthread_mutex_lock(&mAttLock);
    for (range = mRangesHead; range;) {
        attConnListDel(&range->seenBy, l2cConn);
        t = range;
        range = range->next;
        if (attConnListIsEmpty(&t->seenBy) && !t->rangeRef) /* delete a disused range after all who knew about it are gone */
            attSrvRangeDelete(t);
    }

    conn = attConnFindByL2cConn(l2cConn);
    if (conn) {
        if (conn->prev)
            conn->prev->next = conn->next;
        else
            mConns = conn->next;
        if (conn->next)
             conn->next->prev = conn->prev;

        //TODO: XXX: what other cleanup is needed?
        free(conn);
    }
    else
        logi("ATT: notified of a connection closure for a connection we never knew about");

    pthread_mutex_unlock(&mAttLock);
}

/*
 * FUNCTION: attSrvHandleRangeReserve
 * USE:      reserve a range of handles in the ATT server
 * PARAMS:   len - the wanted handle range length
 * RETURN:   a reference to the handle range or 0 on failure
 * NOTES:    by default both LE and EDR accesses are disabled until a call to aapiSrvHandleRangeSetAllowedTransports() is amde
 */
att_range_t attSrvHandleRangeReserve(uint16_t len)
{
    uint32_t searchH = ATT_FIRST_VALID_HANDLE;
    struct attSrvRange *next, *prev = NULL, *newRange;
    att_range_t ret = 0;

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attSrvRangeFind
 * USE:      find a range structure for a given range reference in a given ATT server
 * PARAMS:   rangeRef - the range reference
 * RETURN:   the range struct or NULL if not found
 * NOTES:    call with mAttLock held

 */
static struct attSrvRange* attSrvRangeFind(att_range_t rangeRef)
{
    struct attSrvRange *range;

    for (range = mRangesHead; range && range->rangeRef != rangeRef; range = range->next);

    return range;
}

/*
 * FUNCTION: aapiSrvHandleRangeSetAllowedTransports
 * USE:      Set which transports may access a range
 * PARAMS:   rangeRef - the range reference
 *           allowLE - accessible by LE?
 *           allowEDR - accessible by EDR?
 * RETURN:   a reference to the handle range or 0 on failure
 * NOTES:
 */
bool aapiSrvHandleRangeSetAllowedTransports(att_range_t rangeRef, bool allowLE, bool allowEDR)
{
    struct attSrvRange *range;

    pthread_mutex_lock(&mAttLock);
    range = attSrvRangeFind(rangeRef);
    if (!range)
        logw("Attempt to delete a non-existent range\n");
    else {
        range->allowLE = allowLE;
        range->allowEDR = allowEDR;
    }
    pthread_mutex_unlock(&mAttLock);

    return !!range;
}

/*
 * FUNCTION: attSrvValFind
 * USE:      find a value structure for a given range structure and a given offset
 * PARAMS:   range - the range
 *           offset - the offset
 * RETURN:   the value struct or NULL if not found
 * NOTES:    call with mAttLock held
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
 * NOTES:    call with mAttLock held
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

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
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

    pthread_mutex_lock(&mAttLock);
    range = attSrvRangeFind(rangeRef);
    ret = range ? range->start : 0;
    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attSrvHandleRangeGetLen
 * USE:      Get the actual length (in handled) of a range
 * PARAMS:   rangeRef - the range reference
 * RETURN:   length or 0 on error
 * NOTES:
 */
uint16_t attSrvHandleRangeGetLen(att_range_t rangeRef)
{
    struct attSrvRange *range;
    uint16_t ret;

    pthread_mutex_lock(&mAttLock);
    range = attSrvRangeFind(rangeRef);
    ret = range ? range->len : 0;
    pthread_mutex_unlock(&mAttLock);
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


    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
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

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
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

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
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

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attConnMtuGetByConn
 * USE:      Get the MTU to someone
 * PARAMS:   conn - per-connection structure
 *           tryDefaults - try to find the default to use?
 * RETURN:   the mtu to use or 0 on error
 * NOTES:    call with mAttLock held
 */
static uint32_t attConnMtuGetByConn(struct attConn *conn, bool tryDefaults)
{
    uint32_t mtu = 0;

    if (!(mtu = conn->mtu) && tryDefaults) {
        struct bt_addr peer;

        if (!l2cApiGetBtAddr(conn->conn, &peer)) {
            logw("Failed to get peer address\n");
            return 0;
        }
        mtu = BT_ADDR_IS_EDR(peer) ? ATT_MTU_MIN_EDR : ATT_MTU_MIN_LE;
    }

    return mtu;
}
/*
 * FUNCTION: attConnMtuGetByL2c
 * USE:      Get the MTU to someone
 * PARAMS:   to - whom to
 *           tryDefaults - try to find the default to use?
 * RETURN:   the mtu to use or 0 on error
 * NOTES:    call with mAttLock held
 */
static uint32_t attConnMtuGetByL2c(l2c_handle_t to, bool tryDefaults)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    uint32_t mtu = 0;

    if ((!conn || !(mtu = conn->mtu)) && tryDefaults) {
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
 * NOTES:    call with mAttLock held
 */
static bool attConnMtuSet(l2c_handle_t to, uint32_t mtu)
{
    struct attConn *conn = attConnFindByL2cConn(to);

    if (!conn) {
        logw("Setting MTU for unknown connection\n");
        return false;
    }

    conn->mtu = mtu;
    return true;
}

/*
 * FUNCTION: attSrvTimerCbk
 * USE:      Called when a timeout for a confirmation for a sent notification comes
 * PARAMS:   which - the timer
 *           userData - unused
 * RETURN:   NONE
 * NOTES:    XXX: penOp interaction with this should be explored
 */
static void attSrvTimerCbk(uniq_t which,  uint64_t userData)
{
    struct attConn *conn = NULL;

    pthread_mutex_lock(&mAttLock);

    /* find the relant conn */
    for (conn = mConns; conn && conn->srv.indTimer != which; conn = conn->next);

    /* if we found it, send the callback */
    if (conn)
        attSrvSchedIndResultCbk(conn->srv.indRangeRef, conn->srv.indOffset, conn, ATT_SRV_EVT_IND_TIMEOUT, conn->srv.indRef);

    pthread_mutex_unlock(&mAttLock);
}


/*
 * FUNCTION: attSendPacketRaw
 * USE:      Sends an assembled packet to the peer
 * PARAMS:   to - whom the data is to be sent to
 *           preData - first part of the packet or NULL
 *           preLen - length of fais data
 *           data - the middle of the packet or NULL
 *           postData - last part of the packet or NULL
 *           postLen - length of said data
 * RETURN:   ATT_TX_RET_*
 * NOTES:
 */
static uint8_t attSendPacketRaw(l2c_handle_t to, const void *preData, uint32_t preLen, sg data, const void *postData, uint32_t postLen)
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
 * FUNCTION: attSrvSendPacketNotif
 * USE:      Send notifications to all clients who wanted them for this value
 * PARAMS:   range - the range
 *           val - the value struct
 *           conn - the per-connection structure
 *           data - the value
 *           notify - send notification (as opposed to indication)
 *           ref - pased to callbacks so you know which notif/ind they are tellking you about
 * RETURN:   L2C_TX_*
 * NOTES:    call with mAttLock held
 */
static uint8_t attSrvSendPacketNotif(struct attSrvRange *range, struct attSrvVal *val, struct attConn* conn, sg data, bool notify, uint64_t ref)
{
    uint8_t hdrBuf[sizeof(struct attHdr) + sizeof(struct attPduHandleValueNotif) + sizeof(struct attPduHandleValueInd)]; /* definitely big enoguh to fit the fist and either second or third */
    struct attHdr *hdr = (struct attHdr*)hdrBuf;
    struct attPduHandleValueNotif *notifH = (struct attPduHandleValueNotif*)(hdr + 1);
    struct attPduHandleValueInd *indH = (struct attPduHandleValueInd*)(hdr + 1);
    uint8_t hdrLen = sizeof(*hdr);
    uint32_t mtu;
    uint8_t ret;

    if (notify) {
        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_NOTIF);
        utilSetLE16(&notifH->handle, val->offset + range->start);
        hdrLen += sizeof(*notifH);
    } else {

        if (conn->srv.indTimer)
            return ATT_TX_RET_ATT_BUSY;

        utilSetLE8(&hdr->opcode, ATT_OPCODE_HANDLE_VALUE_IND);
        utilSetLE16(&indH->handle, val->offset + range->start);
        hdrLen += sizeof(*indH);
    }

    mtu = attConnMtuGetByConn(conn, true);
    if (!mtu)
        return ATT_TX_RET_ATT_ERROR;

    mtu -= hdrLen;
    if (sgLength(data) > mtu) {
        logd("Truncating wanted notif/ind data to %ub\n", mtu);
        sgTruncBack(data, sgLength(data) - mtu);
    }

    if (!notify) {
        conn->srv.indRangeRef = range->rangeRef;
        conn->srv.indOffset = val->offset;
        conn->srv.indTimer = timerSet(ATT_IND_TIMEOUT, attSrvTimerCbk, 0);
    }

    return attSendPacketRaw(conn->conn, hdrBuf, hdrLen, data, NULL, 0);
}

/*
 * FUNCTION: attSrvSendPacketErrorReply
 * USE:      Sends an error to the client
 * PARAMS:   to - whom to send to
 *           err - the error
 *           opcode - opcode for which the error happened
 *           handle - the pertinent handle
 * RETURN:   success
 * NOTES:
 */
static bool attSrvSendPacketErrorReply(l2c_handle_t to, uint8_t errNo, uint8_t opcode, uint16_t handle)
{
    struct attPduError err;
    struct attHdr hdr;

    utilSetLE8(&hdr.opcode, ATT_OPCODE_ERROR);
    utilSetLE8(&err.reqOpcode, opcode);
    utilSetLE8(&err.err, errNo);
    utilSetLE16(&err.handle, handle);

    return attSendPacketRaw(to, &hdr, sizeof(hdr), NULL, &err, sizeof(err)) == ATT_TX_RET_ACCEPTED;
}

/*
 * FUNCTION: attSendPacket
 * USE:      Sends a packet to a client
 * PARAMS:   to - whom to send to
 *           opcode - opcode to send
 *           hdr - a header to prepend
 *           hdrLen - length of said header
 *           data - the data to send (may be NULL)
 * RETURN:   success
 * NOTES:
 */
static bool attSendPacket(l2c_handle_t to, uint8_t opcode, const void *hdr, uint32_t hdrLen, sg data)
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

    if (attSendPacketRaw(to, &attHdr, sizeof(attHdr), data, NULL, 0) == ATT_TX_RET_ACCEPTED)
        return true;

    sgTruncFront(data, hdrLen);
    /* fallthough */

out:
    if (freeData)
        sgFree(data);
    return false;
}

/*
 * FUNCTION: attSendPacketEmptyPacket
 * USE:      Sends an empy packet to a client
 * PARAMS:   to - whom to send to
 *           opcode - opcode to send
 * RETURN:   success
 * NOTES:    call with mAttLock held
 */
static bool attSendPacketEmptyPacket(l2c_handle_t to, uint8_t opcode)
{
    return attSendPacket(to, opcode, NULL, 0, NULL);
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
    struct attConn *conn;
    uint32_t i;

    pthread_mutex_lock(&mAttLock);
    conn = attConnFindBySrvCid(cid);
    if (!conn) {
        loge("Got notif request reply for unknown connection\n");
        goto out;
    }

    val = attSrvValRangeFind(rangeRef, offset, &range);
    if (!range) {
        logw("Attempt to notify on a value from a non-existent range\n");
        goto out;
    }
    if (!val) {
        logw("Attempt to notify on a non-existent value from an existent range (0x%04X+0x%04X)\n", range->start, offset);
        goto out;
    }

    ret = attSrvSendPacketNotif(range, val, conn, data, notify, ref);

out:
    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attSrvCidResolve
 * USE:      Attempt to resolve a CID to a L2C connection handle
 * PARAMS:   srvCid - the server cid
 * RETURN:   L2C connection handle or 0 on failure
 * NOTES:    this is used externally
 */
l2c_handle_t attSrvCidResolve(int srvCid)
{
    struct attConn* conn;
    l2c_handle_t ret = 0;

    pthread_mutex_lock(&mAttLock);
    conn = attConnFindBySrvCid(srvCid);
    if (conn)
        ret = conn->conn;
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attSetMtu
 * USE:      Called to set the MTU for an ATT server to a given client directly and with no negotiation
 * PARAMS:   to - whom the MTU is to
 *           mtu - the mtu
 * RETURN:   NONE
 * NOTES:    used by EDR only. LE does its own negotiation of this over special ATT packets
 */
void attSetMtu(l2c_handle_t to, uint32_t mtu)
{
    pthread_mutex_lock(&mAttLock);
    attConnListAddIfNew(to);
    attConnMtuSet(to, mtu);
    pthread_mutex_unlock(&mAttLock);
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
 * FUNCTION: attSrvIsValReadableBy
 * USE:      Decide if a given value is readable by a given connection
 * PARAMS:   range -the value's range
 *           val - the value
 *           conn - per-connection state for this connection
 * RETURN:   true if allowed to read, false else
 * NOTES:    call with mAttLock held
 */
static bool attSrvIsValReadableBy(struct attSrvRange *range, struct attSrvVal *val, struct attConn *conn)
{
    if (conn->isEncr && conn->isMitmProtected && (val->perms & ATT_PERM_READ_ENCR_MITM))
        return true;

    if (conn->isEncr && !conn->isMitmProtected && (val->perms & ATT_PERM_READ_ENCR))
        return true;

    if (!conn->isEncr && (val->perms & ATT_PERM_READ))
        return true;

    return false;
}

/*
 * FUNCTION: attSrvIsValWriteableBy
 * USE:      Decide if a given value is writeable by a given connection
 * PARAMS:   range -the value's range
 *           val - the value
 *           conn - per-connection state for this connection
 *           isSigned - is this a signed write?
 * RETURN:   true if allowed to write, false else
 * NOTES:    call with mAttLock held
 */
static bool attSrvIsValWriteableBy(struct attSrvRange *range, struct attSrvVal *val, struct attConn *conn, bool isSigned)
{
    if (!conn->isEncr && isSigned && (val->perms & ATT_PERM_WRITE_SIGNED))
        return true;

    if (!isSigned && conn->isEncr && conn->isMitmProtected && (val->perms & ATT_PERM_WRITE_ENCR_MITM))
        return true;

    if (!isSigned && conn->isEncr && !conn->isMitmProtected && (val->perms & ATT_PERM_WRITE_ENCR))
        return true;

    if (!isSigned && !conn->isEncr && (val->perms & ATT_PERM_WRITE))
        return true;

    return false;
}

/*
 * FUNCTION: attRangeIsVisibleBy
 * USE:      Decide if a given range is visibile to a given connection
 * PARAMS:   range - the range
 *           conn - per-connection state for this connection
 * RETURN:   true if visible, false else
 * NOTES:    call with mAttLock held
 */
static bool attSrvIsRangeVisibleTo(struct attSrvRange *range, struct attConn *conn)
{
    return (conn->isLE && range->allowLE) || (!conn->isLE && range->allowEDR);
}

/*
 * FUNCTION: attSrvOpFindInfo
 * USE:      Perform a find info request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           startHandle - first handle the client cares about
 *           endHandle - last handle the client cares about
 * RETURN:   success
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpFindInfo(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    struct attPduFindInfoRsp rsp;
    bool allUuidsSmall = true;
    struct attSrvRange *range;
    struct attSrvVal *val;
    sg results = NULL;
    bool err = false;

    if (!conn) {
        loge("No connection found\n");
        return attSrvSendPacketErrorReply(to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_INFO_REQ, startHandle);
    }

    if (!startHandle || startHandle > endHandle)
        return attSrvSendPacketErrorReply(to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_INFO_REQ, startHandle);

    results = sgNew();

    if (!results)
        return attSrvSendPacketErrorReply(to, ATT_ERROR_INSUFFICIENT_RSRCS, ATT_OPCODE_FIND_INFO_REQ, startHandle);

    mtu -= sizeof(struct attPduFindInfoRsp) + sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    for (range = mRangesHead; range; range = range->next) {
        if (!attSrvIsRangeVisibleTo(range, conn))
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
        return attSrvSendPacketErrorReply(to, ATT_ERROR_ATTRIBUTE_NOT_FOUND, ATT_OPCODE_FIND_INFO_REQ, startHandle);
    }

    /* we're ready to send a reply */
    utilSetLE8(&rsp.format, allUuidsSmall ? ATT_FIND_INFO_RSP_FMT_16 : ATT_FIND_INFO_RSP_FMT_128);
    if (attSendPacket(to, ATT_OPCODE_FIND_INFO_RSP, &rsp, sizeof(rsp), results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */

unrecoverable:
    if (results)
        sgFree(results);
    return attSrvSendPacketErrorReply(to, ATT_ERROR_UNLIKELY_ERROR, ATT_OPCODE_FIND_INFO_REQ, 0);
}

/*
 * FUNCTION: attSrvOpFindByTypeValContinue
 * USE:      Continue a find by type value request
 * PARAMS:   conn - the per-connection struct
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpFindByTypeValContinue(struct attConn *conn, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint16_t startHandle;
    uint16_t endHandle;
    uint32_t mtu;
    sg results;

    if (conn->srv.penOp.origOpcode != ATT_OPCODE_FIND_BY_TYPE_VAL_REQ)
        return false;

    startHandle = conn->srv.penOp.startHandle;
    endHandle = conn->srv.penOp.endHandle;
    mtu = conn->srv.penOp.mtu;
    results = conn->srv.penOp.replyInProgress;

    mtu -= sizeof(struct attHdr); /* mtu is now how many bytes we can produce */

    if (lastReadRet == ATT_ERROR_NONE && leastReadLen == conn->srv.penOp.findByTypeValue.wantedLen && !memcmp(lastReadData, conn->srv.penOp.findByTypeValue.wantedVal, leastReadLen)) {

        /* if the last read succeeded, append the result if it is a result */
        struct attPduFindByTypeValRspItem result;

        utilSetLE16(&result.firstHandle, startHandle);
        utilSetLE16(&result.groupEndHandle, startHandle + conn->srv.penOp.findByTypeValue.valGrpLen - 1);

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
        if (!attSrvIsRangeVisibleTo(range, conn))
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
            if (!attSrvIsValReadableBy(range, val, conn))
                continue;

            /* must be of a given type */
            if (!uuidCmp(&val->type, &conn->srv.penOp.findByTypeValue.uuid))
                continue;

            /* we found a falue to read - schedule it */
            conn->srv.penOp.startHandle = handle;
            conn->srv.penOp.findByTypeValue.valGrpLen = effectiveGrpLen;
            if (!attSrvSchedReadCbk(range, val, conn, ATT_READ_FOR_FIND_BY_TYPE_VAL, 0, conn->srv.penOp.findByTypeValue.wantedLen + 1))
                break;

            return false;
        }
    }

done:
    conn->srv.penOp.origOpcode = 0;
    free(conn->srv.penOp.findByTypeValue.wantedVal);

    /* make sure we do not send too much */
    while (sgLength(results) > mtu)
        sgTruncBack(results, sizeof(struct attPduFindByTypeValRspItem));

    /* a lack of results is reported via error */
    if (!sgLength(results)) {
        err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
        startHandle = conn->srv.penOp.origStartHandle;
    } else if (attSendPacket(conn->conn, ATT_OPCODE_FIND_BY_TYPE_VAL_RSP, NULL, 0, results))
        return true;

    /* if we fall though to here, try to send an error */
    sgFree(results);
    return attSrvSendPacketErrorReply(conn->conn, err, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);
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
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpFindByTypeValStart(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct uuid *uuid, sg s)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    sg results = NULL;
    bool ret;

    if (!startHandle || startHandle > endHandle) {
        ret = attSrvSendPacketErrorReply(to, ATT_ERROR_INVALID_HANDLE, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);
        goto out;
    }

    if (!conn)
        loge("No connection found\n");
    else if (conn->srv.penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {

        results = sgNew();
        if (results) {

            conn->srv.penOp.findByTypeValue.wantedVal = malloc(sgLength(s));
            if (conn->srv.penOp.findByTypeValue.wantedVal) {

                sgSerialize(s, 0, sgLength(s), conn->srv.penOp.findByTypeValue.wantedVal);

                conn->srv.penOp.origOpcode = ATT_OPCODE_FIND_BY_TYPE_VAL_REQ;
                conn->srv.penOp.replyInProgress = results;
                conn->srv.penOp.origStartHandle = startHandle;
                conn->srv.penOp.startHandle = startHandle - 1;
                conn->srv.penOp.endHandle = endHandle;
                conn->srv.penOp.mtu = mtu;
                memcpy(&conn->srv.penOp.findByTypeValue.uuid, uuid, sizeof(conn->srv.penOp.findByTypeValue.uuid));
                conn->srv.penOp.findByTypeValue.wantedLen = sgLength(s);
                conn->srv.penOp.findByTypeValue.valGrpLen = 0;

                if (attSrvOpFindByTypeValContinue(conn, NULL, 0, ATT_FIRST_CONTINUE_CALL)) {
                    sgFree(s);
                    return true;
                }

                err = ATT_ERROR_UNLIKELY_ERROR;
                conn->srv.penOp.origOpcode = 0;

                free(conn->srv.penOp.findByTypeValue.wantedVal);
            }
            sgFree(results);
        }
    }

    ret = attSrvSendPacketErrorReply(to, err, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ, startHandle);

out:
    if (ret)
        sgFree(s);
    return ret;
}

/*
 * FUNCTION: attSrvOpReadByTypeContinue
 * USE:      Continue a read by type / read by group type request
 * PARAMS:   conn - the per-connection structure
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpReadByTypeContinue(struct attConn *conn, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
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


    grpType = conn->srv.penOp.readByType.grpType;
    packetHdrSz = grpType ? sizeof(struct attPduReadByGrpTypeRsp) : sizeof(struct attPduReadByTypeRsp);
    itemHdrSz = grpType ? sizeof(struct attGrpValuePair) : sizeof(struct attHandleValuePair);
    itemMaxLen = grpType ? ATT_MAX_LEN_FOR_RD_BY_GRP_TYP : ATT_MAX_LEN_FOR_RD_BY_TYPE;
    readReason = grpType ? ATT_READ_FOR_READ_BY_GRP_TYPE : ATT_READ_FOR_READ_BY_TYPE;
    reqOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_REQ : ATT_OPCODE_READ_BY_TYPE_REQ;
    rspOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_RSP : ATT_OPCODE_READ_BY_TYPE_RSP;

    if (conn->srv.penOp.origOpcode != reqOpcode)
        return false;

    startHandle = conn->srv.penOp.startHandle;
    endHandle = conn->srv.penOp.endHandle;
    mtu = conn->srv.penOp.mtu;
    results = conn->srv.penOp.replyInProgress;

    mtu -= sizeof(struct attHdr) + packetHdrSz; /* mtu is now how many bytes we can produce */
    if (itemMaxLen > mtu - itemHdrSz)
        itemMaxLen = mtu - itemHdrSz;

    if (lastReadRet == ATT_ERROR_NONE) {
        sg thisResult;
        uint16_t perResultSize;

        /* only items of the same size shall be provided, so the first result sets the size */
        if (sgLength(results) && leastReadLen != conn->srv.penOp.readByType.itemLen)
            goto done;
        conn->srv.penOp.readByType.itemLen = leastReadLen;

        /* prepare the per-item header */
        if (grpType) {
            struct attGrpValuePair gvp;
            utilSetLE16(&gvp.handle, startHandle);
            utilSetLE16(&gvp.groupEndHadle, startHandle + conn->srv.penOp.readByType.valGrpLen - 1);

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
        if (!attSrvIsRangeVisibleTo(range, conn))
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
            if (!attSrvIsValReadableBy(range, val, conn))
                continue;

            /* must be of a given type */
            if (!uuidCmp(&val->type, &conn->srv.penOp.readByType.uuid))
                continue;

            /* we found a falue to read - schedule it */
            conn->srv.penOp.startHandle = handle;
            conn->srv.penOp.readByType.valGrpLen = effectiveGrpLen;
            if (attSrvSchedReadCbk(range, val, conn, readReason, 0, itemMaxLen))
                return true;

            goto done;
        }
    }

done:
    conn->srv.penOp.origOpcode = 0;

    if (sgLength(results) > mtu) {
        /* make sure we do not send too much */
        logw("Impossible result length %u over mtu %u in read by [grp] type\n", sgLength(results), mtu);
        startHandle = conn->srv.penOp.origStartHandle;
     } else if (!sgLength(results)) {
        /* a lack of results is reported thusly */
        err = ATT_ERROR_ATTRIBUTE_NOT_FOUND;
        startHandle = conn->srv.penOp.origStartHandle;
    } else {
        struct attPduReadByTypeRsp rbt;
        struct attPduReadByGrpTypeRsp gtr;
        void *hdr;

        if (grpType) {
            hdr = &gtr;
            utilSetLE8(&gtr.attGrpValuePairSz, conn->srv.penOp.readByType.itemLen + sizeof(struct attGrpValuePair));
        } else {
            hdr = &rbt;
            utilSetLE8(&rbt.handleValuePairSz, conn->srv.penOp.readByType.itemLen + sizeof(struct attHandleValuePair));
        }

        if (attSendPacket(conn->conn, rspOpcode, hdr, packetHdrSz, results))
            return true;
    }

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */
    sgFree(results);
    return attSrvSendPacketErrorReply(conn->conn, err, reqOpcode, startHandle);
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
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpReadByTypeStart(l2c_handle_t to, uint32_t mtu, uint16_t startHandle, uint16_t endHandle, const struct uuid *uuid, bool grpType)
{
    const uint8_t reqOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_REQ : ATT_OPCODE_READ_BY_TYPE_REQ;
    const uint8_t rspOpcode = grpType ? ATT_OPCODE_READ_BY_GRP_TYPE_RSP : ATT_OPCODE_READ_BY_TYPE_RSP;
    const uint8_t readReason = grpType ? ATT_READ_FOR_READ_BY_GRP_TYPE : ATT_READ_FOR_READ_BY_TYPE;
    struct attConn *conn = attConnFindByL2cConn(to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    sg results = NULL;

    if (!startHandle || startHandle > endHandle)
        return attSrvSendPacketErrorReply(to, ATT_ERROR_INVALID_HANDLE, reqOpcode, startHandle);

    if (!conn)
        loge("No connection found\n");
    else if (conn->srv.penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {
        results = sgNew();
        if (results) {

            conn->srv.penOp.origOpcode = reqOpcode;
            conn->srv.penOp.replyInProgress = results;
            conn->srv.penOp.origStartHandle = startHandle;
            conn->srv.penOp.startHandle = startHandle - 1;
            conn->srv.penOp.endHandle = endHandle;
            conn->srv.penOp.mtu = mtu;
            memcpy(&conn->srv.penOp.readByType.uuid, uuid, sizeof(conn->srv.penOp.readByType.uuid));
            conn->srv.penOp.readByType.grpType = grpType;
            conn->srv.penOp.readByType.valGrpLen = 0;
            conn->srv.penOp.readByType.itemLen = 0;

            if (attSrvOpReadByTypeContinue(conn, NULL, 0, ATT_FIRST_CONTINUE_CALL))
                return true;

            err = ATT_ERROR_UNLIKELY_ERROR;
            conn->srv.penOp.origOpcode = 0;

            sgFree(results);
        }
    }

    return attSrvSendPacketErrorReply(to, err, reqOpcode, startHandle);
}

/*
 * FUNCTION: attSrvOpReadContinue
 * USE:      Continue a read / read blob request
 * PARAMS:   conn - the per-connection structure
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpReadContinue(struct attConn *conn, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = lastReadRet;
    uint16_t wantedHandle;
    uint8_t opc, rspOpc;
    sg result;


    if (conn->srv.penOp.origOpcode == ATT_OPCODE_READ_REQ)
        rspOpc = ATT_OPCODE_READ_RSP;
    else if (conn->srv.penOp.origOpcode == ATT_OPCODE_READ_BLOB_REQ)
        rspOpc = ATT_OPCODE_READ_BLOB_RSP;
    else
        return false;

    wantedHandle = conn->srv.penOp.startHandle;
    opc = conn->srv.penOp.origOpcode;
    conn->srv.penOp.origOpcode = 0;
    
    if (err == ATT_ERROR_NONE) {
        result = sgNewWithCopyData(lastReadData, leastReadLen);
        if (!result)
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
        else if (!attSendPacket(conn->conn, rspOpc, NULL, 0, result)) {
            err = ATT_ERROR_UNLIKELY_ERROR;
            sgFree(result);
        } else
            return true;
    }

    return attSrvSendPacketErrorReply(conn->conn, err, opc, wantedHandle);
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
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpReadStart(l2c_handle_t to, uint32_t mtu, uint16_t wantedHandle, uint16_t offset, uint8_t opcode)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    uint8_t err = ATT_ERROR_INSUFFICIENT_RSRCS;
    struct attSrvRange *range;
    struct attSrvVal *val;


    if (!wantedHandle)
        err = ATT_ERROR_INVALID_HANDLE;
    else if (!conn) {
        err = ATT_ERROR_UNLIKELY_ERROR;
        wantedHandle = 0;
        loge("No connection found\n");
    } else if (conn->srv.penOp.origOpcode)
        err = ATT_ERROR_UNLIKELY_ERROR;
    else {
        for (range = mRangesHead; range; range = range->next) {
            if (!attSrvIsRangeVisibleTo(range, conn))
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
                if (!attSrvIsValReadableBy(range, val, conn)) {
                    err = ATT_ERROR_READ_NOT_ALLOWED;
                    goto done;
                }

                /* do a read */
                conn->srv.penOp.origOpcode = opcode;
                conn->srv.penOp.mtu = mtu;
                conn->srv.penOp.startHandle = wantedHandle;
                conn->srv.penOp.read.offset = offset;

                if (attSrvSchedReadCbk(range, val, conn, readReason, offset, mtu - sizeof(struct attHdr)))
                    return true;

                conn->srv.penOp.origOpcode = 0;
                err = ATT_ERROR_UNLIKELY_ERROR;
                goto done;
            }
        }
    }
done:

    return attSrvSendPacketErrorReply(to, err, opcode, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadMultContinue
 * USE:      Continue a read multiple request
 * PARAMS:   conn - per-connection structure
 *           lastReadData - return from the READ call
 *           leastReadLen - length last read result produced
 *           lastReadRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpReadMultContinue(struct attConn *conn, const void *lastReadData, uint16_t leastReadLen, uint8_t lastReadRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;
    uint16_t wantedHandle = 0;
    sg results, handles;
    uint16_t mtu;


    if (conn->srv.penOp.origOpcode != ATT_OPCODE_READ_MULT_REQ)
        return false;

    mtu = conn->srv.penOp.mtu;
    results = conn->srv.penOp.replyInProgress;
    handles = conn->srv.penOp.readMult.handles;

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
        if (!attSrvIsRangeVisibleTo(range, conn))
            continue;
        for (val = range->valsHead; val; val = val->next) {
            uint16_t handle = range->start + val->offset;

            /* range check */
            if (handle < wantedHandle)
                continue;
            if (handle > wantedHandle)
                goto done;

            conn->srv.penOp.readMult.lastReadHandle = wantedHandle;

            /* must be readable */
            if (!attSrvIsValReadableBy(range, val, conn)) {
                err = ATT_ERROR_READ_NOT_ALLOWED;
                goto error;
            }

            /* we found the falue to read - schedule it */
            conn->srv.penOp.startHandle = handle;
            if (attSrvSchedReadCbk(range, val, conn, ATT_READ_FOR_READ_MULT, 0, mtu))
                return true;

            goto error;
        }
    }

error:
    sgTruncFront(results, sgLength(results));

done:
    conn->srv.penOp.origOpcode = 0;

    /* mind the length */
    if (sgLength(results) > mtu)
        sgTruncBack(results, sgLength(results) - mtu);

    if (!sgLength(results)) {
        /* a lack of results is reported as an error */
        wantedHandle = conn->srv.penOp.readMult.lastReadHandle;
    } else if (attSendPacket(conn->conn, ATT_OPCODE_READ_MULT_RSP, NULL, 0, results))
        return true;

    /* if we fall though to here, try to send an error (even if it is unlikely to work) */
    sgFree(results);
    sgFree(handles);
    return attSrvSendPacketErrorReply(conn->conn, err, ATT_OPCODE_READ_MULT_REQ, wantedHandle);
}

/*
 * FUNCTION: attSrvOpReadMultStart
 * USE:      Start a read multiple request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           s - sg with uint16_t list of handles
 * RETURN:   success
 * NOTES:    call with mAttLock held. WILL FREE "handles" eventually, if true is returned
 */
static bool attSrvOpReadMultStart(l2c_handle_t to, uint32_t mtu, sg handles)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;

    if (!conn)
        loge("No connection found\n");
    else if (!conn->srv.penOp.origOpcode) {

        conn->srv.penOp.replyInProgress = sgNew();
        if (!conn->srv.penOp.replyInProgress)
            err = ATT_ERROR_INSUFFICIENT_RSRCS;
        else {

            conn->srv.penOp.origOpcode = ATT_OPCODE_READ_MULT_REQ;
            conn->srv.penOp.readMult.handles = handles;
            conn->srv.penOp.mtu = mtu;

            if (attSrvOpReadContinue(conn, NULL, 0, ATT_FIRST_CONTINUE_CALL))
                return true;

            sgFree(conn->srv.penOp.replyInProgress);
        }
    }

    if (attSrvSendPacketErrorReply(to, err, ATT_OPCODE_READ_MULT_REQ, 0)) {
        sgFree(handles);
        return true;
    }
    return false;
}

/*
 * FUNCTION: attSrvWriteContinue
 * USE:      Continue a [signed] write request / command
 * PARAMS:   conn - per-connection structure
 *           lastWriteData - return from the READ call
 *           leastWriteLen - length last read result produced
 *           lastWriteRet - last read's result
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvWriteContinue(struct attConn *conn, const void *lastWriteData, uint16_t leastWriteLen, uint8_t lastWriteRet)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;


    if (conn->srv.penOp.origOpcode != ATT_OPCODE_WRITE_REQ && conn->srv.penOp.origOpcode != ATT_OPCODE_SIGNED_WRITE_CMD &&
         conn->srv.penOp.origOpcode != ATT_OPCODE_WRITE_REQ && conn->srv.penOp.origOpcode != ATT_OPCODE_PREPARE_WRITE_REQ)
        return false;

    conn->srv.penOp.origOpcode = 0;

    if (conn->srv.penOp.origOpcode == ATT_OPCODE_WRITE_REQ)
        return attSendPacketEmptyPacket(conn->conn, ATT_OPCODE_WRITE_RSP);
    if (conn->srv.penOp.origOpcode == ATT_OPCODE_PREPARE_WRITE_REQ)
        return attSendPacket(conn->conn, ATT_OPCODE_PREPARE_WRITE_RSP, lastWriteData, leastWriteLen, NULL);

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
 * NOTES:    call with mAttLock held
 */
static bool attSrvWriteStart(l2c_handle_t to, uint32_t mtu, uint16_t wantedHandle, sg data, uint16_t byteOfst, uint8_t opcode, uint8_t writeReason)
{
    struct attConn *conn = attConnFindByL2cConn(to);
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attSrvRange *range;
    struct attSrvVal *val;

    if (!conn)
        loge("No connection found\n");
    else if (!conn->srv.penOp.origOpcode) {

        for (range = mRangesHead; range; range = range->next) {
            if (!attSrvIsRangeVisibleTo(range, conn))
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
        else if (!attSrvIsValWriteableBy(range, val, conn, writeReason == ATT_WRITE_FOR_SIGNED_WRITE_CMD))
            err = ATT_ERROR_WRITE_NOT_ALLOWED;
        else {
            conn->srv.penOp.origOpcode = opcode;
            conn->srv.penOp.mtu = mtu;
            conn->srv.penOp.startHandle = wantedHandle;
            if (attSrvSchedWriteCbk(range, val, conn, writeReason, 0, data))
                return true;
            conn->srv.penOp.origOpcode = 0;
        }
    }

    if (attSrvSendPacketErrorReply(to, err, ATT_OPCODE_WRITE_REQ, wantedHandle)) {
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
 * NOTES:    call with mAttLock held  WILL NOT FREE "data". Caller must!
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
 * PARAMS:   conn - per-connection structure
 *           to - the connection handle
 *           execResult - the result of the execution attempt
 *           errorHandle - in case of error stores the handle the caused it
 * RETURN:   false to fail immediately, true if more work will be done
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpExecuteWriteContinue(struct attConn *conn, uint8_t execResult, uint16_t errorHandle)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;


    if (conn->srv.penOp.origOpcode != ATT_OPCODE_EXECUTE_WRITE_REQ)
        return false;

    conn->srv.penOp.origOpcode = 0;

    if (!execResult) {
        if (attSendPacketEmptyPacket(conn->conn, ATT_OPCODE_EXECUTE_WRITE_RSP))
            return true;
        execResult = ATT_ERROR_UNLIKELY_ERROR;
    }

    return attSrvSendPacketErrorReply(conn->conn, execResult, ATT_OPCODE_EXECUTE_WRITE_REQ, errorHandle);
}

/*
 * FUNCTION: attSrvOpExecuteWriteStart
 * USE:      Start an execute write request
 * PARAMS:   to - whom to reply to
 *           mtu - the mtu to use
 *           flags - ATT_EXECUTE_WRITE_FLG_*
 * RETURN:   success
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpExecuteWriteStart(l2c_handle_t to, uint32_t mtu, uint8_t flags)
{
    uint8_t err = ATT_ERROR_UNLIKELY_ERROR;
    struct attConn* conn = attConnFindByL2cConn(to);
    bool ret = ATT_ERROR_NONE;

    if (!conn) {
        /* not much to do here - nothing we CAN do */
    } else if (flags == ATT_EXECUTE_WRITE_FLG_CANCEL || flags == ATT_EXECUTE_WRITE_FLG_EXECUTE) {
        if (attSrvSchedExecCbk(conn, flags == ATT_EXECUTE_WRITE_FLG_EXECUTE))
            return true;
    } else {
        err = ATT_ERROR_INVALID_PDU;
        goto err;
    }

    if (ret == ATT_ERROR_NONE && attSendPacketEmptyPacket(to, ATT_OPCODE_EXECUTE_WRITE_RSP))
        return true;
    /* if we fail to send, fall through and try to signal an error */

err:
    return attSrvSendPacketErrorReply(to, ret, ATT_OPCODE_EXECUTE_WRITE_REQ, 0);
}

/*
 * FUNCTION: attSrvOpHandleValueConf
 * USE:      Handle a value confirmation
 * PARAMS:   to - whom to reply to (if any)
 *           mtu - the mtu to use
 * RETURN:   success
 * NOTES:    call with mAttLock held
 */
static bool attSrvOpHandleValueConf(l2c_handle_t to, uint32_t mtu)
{
    struct attConn *conn;

    conn = attConnFindByL2cConn(to);
    if (!conn)
        loge("No connection found\n");
    else if (conn->srv.indTimer) { /* we're not late */
        attSrvSchedIndResultCbk(conn->srv.indRangeRef, conn->srv.indOffset, conn, ATT_SRV_EVT_IND_ACKED, conn->srv.indRef);
        timerCancel(conn->srv.indTimer);
        conn->srv.indTimer = 0;
    } else /* we are late */
        logw("Confirmation is late\n");

    pthread_mutex_lock(&mAttLock);
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
    pthread_mutex_lock(&mAttLock);
    attConnListAddIfNew(from);

    switch (opcode) {
    case ATT_OPCODE_MTU_EXCH_REQ:
    case ATT_OPCODE_MTU_EXCH_RSP:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &mtuExch, sizeof(mtuExch)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            struct attConn* conn = attConnFindByL2cConn(from);

            newMtu = utilGetLE16(&mtuExch.mtu);
            mtu = attConnMtuGetByL2c(from, false);
            if (!mtu || mtu > newMtu)
                mtu = newMtu;
            attConnMtuSet(from, mtu);
            if (opcode == ATT_OPCODE_MTU_EXCH_REQ) { /* it was a request - we should reply */
                utilSetLE16(&mtuExch.mtu, mtu); /* we unconditionally accept */
                ret = attSendPacket(from, ATT_OPCODE_MTU_EXCH_RSP, &mtuExch, sizeof(mtuExch), NULL);
            }
            if (conn)
                attSrvSchedMtuCbk(conn, mtu, opcode == ATT_OPCODE_MTU_EXCH_REQ);
        }
        break;

    case ATT_OPCODE_FIND_INFO_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &findInfoReq, sizeof(findInfoReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpFindInfo(from, mtu, utilGetLE16(&findInfoReq.startHandle), utilGetLE16(&findInfoReq.endHandle));
        break;

    case ATT_OPCODE_FIND_BY_TYPE_VAL_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &findByTypeValReq, sizeof(findByTypeValReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else {
            uuidFromUuid16(&uuid, utilGetLE16(&findByTypeValReq.uuid16));
            if (attSrvOpFindByTypeValStart(from, mtu, utilGetLE16(&findByTypeValReq.startHandle), utilGetLE16(&findByTypeValReq.endHandle), &uuid, s)) {
                ret = true;
                s = NULL;
            }
        }
        break;

    case ATT_OPCODE_READ_BY_TYPE_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &readByTypeReq, sizeof(readByTypeReq)) || !attReadUuid(&uuid, s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByTypeStart(from, mtu, utilGetLE16(&readByTypeReq.startHandle), utilGetLE16(&readByTypeReq.endHandle), &uuid, false);
        break;

    case ATT_OPCODE_READ_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &readReq, sizeof(readReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadStart(from, mtu, utilGetLE16(&readReq.handle), 0, opcode);
        break;

    case ATT_OPCODE_READ_BLOB_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &readBlobReq, sizeof(readBlobReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadStart(from, mtu, utilGetLE16(&readBlobReq.handle), utilGetLE16(&readBlobReq.offset), opcode);
        break;

    case ATT_OPCODE_READ_MULT_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if ((sgLength(s) % sizeof(uint16_t)) || sgLength(s) < sizeof(uint16_t[ATT_MIN_HANDLES_IN_READ_MULT]))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvOpReadMultStart(from, mtu, s)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_READ_BY_GRP_TYPE_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &readByGrpType, sizeof(readByGrpType)) || !attReadUuid(&uuid, s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpReadByTypeStart(from, mtu, utilGetLE16(&readByGrpType.startHandle), utilGetLE16(&readByGrpType.endHandle), &uuid, true);
        break;

    case ATT_OPCODE_WRITE_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &writeReq, sizeof(writeReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvWriteStart(from, mtu, utilGetLE16(&writeReq.handle), s, 0, opcode, ATT_WRITE_FOR_WRITE_REQ)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_PREPARE_WRITE_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &prepareWriteReq, sizeof(prepareWriteReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (attSrvWriteStart(from, mtu, utilGetLE16(&prepareWriteReq.handle), s, utilGetLE16(&prepareWriteReq.offset), opcode, ATT_WRITE_FOR_PREPARE)) {
            ret = true;
            s = NULL;
        }
        break;

    case ATT_OPCODE_EXECUTE_WRITE_REQ:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &execWriteReq, sizeof(execWriteReq)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpExecuteWriteStart(from, mtu, utilGetLE16(&execWriteReq.flags));
        break;

    case ATT_OPCODE_HANDLE_VALUE_CONF:
        mtu = attConnMtuGetByL2c(from, true);
        if (sgLength(s))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else
            ret = attSrvOpHandleValueConf(from, mtu);
        break;

    case ATT_OPCODE_WRITE_CMD:
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &writeCmd, sizeof(writeCmd)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
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
        mtu = attConnMtuGetByL2c(from, true);
        if (!sgSerializeCutFront(s, &signedWriteCmd, sizeof(signedWriteCmd)))
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
        else if (sgLength(s) < ATT_SIG_LEN)
            ret = attSrvSendPacketErrorReply(from, ATT_ERROR_INVALID_PDU, opcode, 0);
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
        ret = attSrvSendPacketErrorReply(from, ATT_ERROR_REQ_NOT_SUPPORTED, opcode, 0);
        break;
    }

out:
    pthread_mutex_unlock(&mAttLock);
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
    struct attConn* conn;
    uint8_t opcode;
    bool ret = false;

    pthread_mutex_lock(&mAttLock);
    conn = attConnFindBySrvCid(cid);
    if (!conn)
        loge("Got callback reply for unknown connection\n");
    else if (conn->srv.nextTransId != transId)
        loge("Got callback reply for unknown transcation id %u\n", transId);
    else {
        switch (conn->srv.penOp.origOpcode) {
        case ATT_OPCODE_FIND_BY_TYPE_VAL_REQ:
            ret = attSrvOpFindByTypeValContinue(conn, data, len, err);
            break;
        case ATT_OPCODE_READ_BY_TYPE_REQ:
        case ATT_OPCODE_READ_BY_GRP_TYPE_REQ:
            ret = attSrvOpReadByTypeContinue(conn, data, len, err);
            break;
        case ATT_OPCODE_READ_REQ:
        case ATT_OPCODE_READ_BLOB_REQ:
            ret = attSrvOpReadContinue(conn, data, len, err);
            break;
        case ATT_OPCODE_READ_MULT_REQ:
            ret = attSrvOpReadMultContinue(conn, data, len, err);
            break;
        case ATT_OPCODE_WRITE_REQ:
        case ATT_OPCODE_SIGNED_WRITE_CMD:
        case ATT_OPCODE_WRITE_CMD:
        case ATT_OPCODE_PREPARE_WRITE_REQ:
            ret = attSrvWriteContinue(conn, data, len, err);
            break;
        case ATT_OPCODE_EXECUTE_WRITE_REQ:
            ret = attSrvOpExecuteWriteContinue(conn, err, handle);
            break;
        default:
            loge("Unknown original opcode\n");
            conn->srv.penOp.origOpcode = 0;
        }
    }
    pthread_mutex_unlock(&mAttLock);
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
    struct uuid uuid;
    uint8_t format, length;
    bool haveMore;
    int status;

    pthread_setname_np(pthread_self(), "att_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case WORK_ATT_MTU_INFO:
            (wi->mtuInfo.asServer ? mSrvMtuCbk : mCliMtuCbk)(wi->who, wi->mtuInfo.mtu);
            break;

        case WORK_ATT_SYNC:
            sem_post(wi->sync.sem);
            break;

        case WORK_SRV_READ_CALL:
            if (!mReadCbk(wi->who, wi->srv.cid, wi->srvRead.range, wi->srvRead.ofst, wi->srvRead.transId, wi->srvRead.byteOfst, wi->srvRead.reason, wi->srvRead.maxLen))
                attSrvCbkReply(wi->srv.cid, wi->srvRead.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_SRV_WRITE_CALL:
            if (!mWriteCbk(wi->who, wi->srv.cid, wi->srvWrite.range, wi->srvWrite.ofst, wi->srvWrite.transId, wi->srvWrite.byteOfst, wi->srvWrite.reason, wi->srvWrite.len, wi + 1))
                attSrvCbkReply(wi->srv.cid, wi->srvWrite.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_SRV_EXEC_WRITE_CALL:
            if (!mExecCbk(wi->who, wi->srv.cid, wi->srvExecWrite.transId, wi->srvExecWrite.exec))
                attSrvCbkReply(wi->srv.cid, wi->srvExecWrite.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;

        case WORK_SRV_IND_RESULT:
            mIndCbk(wi->who, wi->srv.cid, wi->srvIndResult.range, wi->srvIndResult.ofst, wi->srvIndResult.evt, wi->srvIndResult.ref);
            break;

        case WORK_CLI_FIND_INFO_CALL:
            if (!sgSerializeCutFront(wi->cliJustData.data, &format, sizeof(format)))
                logw("Failed to get format from received data for Find\n");
            else if (format != ATT_FIND_INFO_RSP_FMT_16 && format != ATT_FIND_INFO_RSP_FMT_128)
                logw("Format %u unknown for Find\n", format);
            else {

                do {
                    uint16_t handle;

                    if (format == ATT_FIND_INFO_RSP_FMT_16) {
                        uint8_t buf[sizeof(uint16_t)];

                        if (!sgSerializeCutFront(wi->cliJustData.data, buf, sizeof(handle)))
                            break;
                        handle = utilGetLE16(buf);
                        if (!sgSerializeCutFront(wi->cliJustData.data, buf, sizeof(buf)))
                            break;
                        uuidFromUuid16(&uuid, utilGetLE16(buf));
                        haveMore = sgLength(wi->cliJustData.data) >= sizeof(buf) + sizeof(handle);
                    } else {
                        uint8_t buf[sizeof(struct uuid)];

                        if (!sgSerializeCutFront(wi->cliJustData.data, buf, sizeof(handle)))
                            break;
                        handle = utilGetLE16(buf);
                        if (!sgSerializeCutFront(wi->cliJustData.data, &buf, sizeof(buf)))
                            break;
                        uuidReadLE(&uuid, buf);
                        haveMore = sgLength(wi->cliJustData.data) >= sizeof(buf) + sizeof(handle);
                    }
                    mCliFindInfoCbk(wi->cli.trans, wi->who, handle, &uuid, haveMore);
                } while (haveMore);
                if (sgLength(wi->cliJustData.data))
                    logw("Unexpected leftover data for Find: %ub\n", sgLength(wi->cliJustData.data));
            }
            sgFree(wi->cliJustData.data);
            break;
         
        case WORK_CLI_FIND_BY_TYPE_VAL_CALL:
            do {
                uint16_t attrH, grpEndH;
                uint8_t buf[sizeof(uint16_t) * 2];

                if (!sgSerializeCutFront(wi->cliJustData.data, buf, sizeof(buf)))
                    break;

                haveMore = sgLength(wi->cliJustData.data) >= sizeof(buf);
                attrH = utilGetLE16(buf);
                grpEndH = utilGetLE16(buf + sizeof(attrH));

                mCliFindByTypeValCbk(wi->cli.trans, wi->who, attrH, grpEndH, haveMore);
            } while (haveMore);
            if (sgLength(wi->cliJustData.data))
                logw("Unexpected leftover data for FindByTypeValue: %ub\n", sgLength(wi->cliJustData.data));
            sgFree(wi->cliJustData.data);
            break;


        case WORK_CLI_READ_BY_TYPE_CALL:
            if (!sgSerializeCutFront(wi->cliJustData.data, &length, sizeof(length)))
                logw("Failed to get length from received data for ReadByType\n");
            else if (length < sizeof(uint16_t))
                logw("Length %u impossible for ReadByType\n", length);
            else if (sgLength(wi->cliJustData.data) < length)
                logw("Sg length %u impossible for ReadByType (expected %u+)\n", sgLength(wi->cliJustData.data), length);
            else {
                uint8_t buf[sizeof(uint16_t)];
                uint16_t handle;
                sg s;

                do {
                    if (!sgSerializeCutFront(wi->cliJustData.data, buf, sizeof(buf)))
                        break;
                    handle = utilGetLE16(buf);

                    s = sgSplit(wi->cliJustData.data, length - sizeof(buf));
                    if (!s)
                        break;
                    sgSwap(wi->cliJustData.data, s);

                    haveMore = sgLength(wi->cliJustData.data) >= length;
                    mCliReadF(wi->cli.trans, wi->who, &handle, NULL, s, haveMore);
                } while(haveMore);
            }
            if (sgLength(wi->cliJustData.data))
                logw("Unexpected leftover data for ReadByGroupType: %ub\n", sgLength(wi->cliJustData.data));
            sgFree(wi->cliJustData.data);
            break;


        case WORK_CLI_READ_CALL:
        case WORK_CLI_READ_BY_GRP_TYPE_CALL:
        case WORK_CLI_WRITE_CALL:
        case WORK_CLI_ERR_CALL:
        case WORK_CLI_IND_NOTIF_CALL:

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
 * NOTES:    internally ref-counted as multiple users exist
 */
bool attInit(void)
{
    bool ret = false;

    pthread_mutex_lock(&mAttLock);
    if (!mNumUsers) {

        mConns = NULL;
        mWorkQ = workQueueAlloc(ATT_WORK_Q_SIZE);
        if (!mWorkQ)
            loge("Failed to create work Q\n");
        else if (pthread_create(&mWorker, NULL, attWorker, NULL)) {
            loge("Failed to create worker\n");
            workQueueFree(mWorkQ, attWorkItemFree);
        } else {
            ret = true;
        }
    } else
        ret = true;

    if (ret)
        mNumUsers++;

    pthread_mutex_unlock(&mAttLock);
    return ret;
}

/*
 * FUNCTION: attDeinit
 * USE:      Called to deinit all of ATT
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    internally ref-counted as multiple users exist
 */
void attDeinit(void)
{
    pthread_mutex_lock(&mAttLock);
    if (!mNumUsers)
        loge("too many ATT closures!\n");
    else if (!--mNumUsers) {
        workQueueWakeAll(mWorkQ, 1);
        pthread_join(mWorker, NULL);
        workQueueFree(mWorkQ, attWorkItemFree);
        while (mConns) {
            struct attConn *t = mConns->next;
            free(mConns);
            mConns = t;
        }
    }
    pthread_mutex_unlock(&mAttLock);
}

/*
 * FUNCTION: attCliRequestMtu
 * USE:      As a client request an MTU value from the server
 * PARAMS:   to - the server l2c connection
 *           mtu - the mtu to request
 * RETURN:   false on error, true if will try
 * NOTES:
 */
bool attCliRequestMtu(l2c_handle_t to, uint32_t mtu)
{
    struct attPduMtu mtuExch;
    bool ret;

    utilSetLE16(&mtuExch.mtu, mtu);
    return attSendPacket(to, ATT_OPCODE_MTU_EXCH_REQ, &mtuExch, sizeof(mtuExch), NULL);
}

/*
 * FUNCTION: attCliInit
 * USE:      Init ATT client code (by providing callbacks)
 * PARAMS:   cliMtuF - calback about MTU negotiation results
 *           cliReadF - read callback for clients
 *           cliWriteF - write callabck for clients
 *           cliErrF - error callback for clients
 * RETURN:   false on error, true if will try
 * NOTES:
 */
bool attCliInit(attMtuCbk cliMtuF, attCliFindInfoCbk cliFindInfoCbk, attCliFindByTypeValCbk cliFindByTypeValCbk, attCliReadCbk cliReadF, attCliWriteCbk cliWriteF, attCliError cliErrF, attCliIndNotif cliIndNotifF)
{
    pthread_mutex_lock(&mAttLock);
    mCliMtuCbk = cliMtuF;
    mCliFindInfoCbk = cliFindInfoCbk;
    mCliFindByTypeValCbk = cliFindByTypeValCbk;
    mCliReadF = cliReadF;
    mCliWriteF = cliWriteF;
    mCliErrF = cliErrF;
    mCliIndNotifF = cliIndNotifF;
    pthread_mutex_unlock(&mAttLock);

    return true;
}

/*
 * FUNCTION: attCliDeinit
 * USE:      Deinit ATT client code
 * PARAMS:   NONE
 * RETURN:
 * NOTES:
 */
void attCliDeinit(void)
{
    //TODO: maybe?
}

/*
 * FUNCTION: attCliSendIndicationAck
 * USE:      Send a value confirmation (an ack to a value indication)
 * PARAMS:   to - whom to
 * RETURN:   false on error
 * NOTES:
 */
bool attCliSendIndicationAck(l2c_handle_t to)
{
    return attSendPacketEmptyPacket(to, ATT_OPCODE_HANDLE_VALUE_CONF);
}

/*
 * FUNCTION: attCliTransTimeout
 * USE:      Timer callback for ATT trasnaction timer
 * PARAMS:   timer - the timer
 *           cbkData - the L2C connection handle
 * RETURN:   NONE
 * NOTES:
 */
static void attCliTransTimeout(uniq_t timer,  uint64_t cbkData)
{
    l2c_handle_t to = (l2c_handle_t)cbkData;
    struct attConn* conn;
    pthread_mutex_lock(&mAttLock);
    conn = attConnFindByL2cConn(to);
    if (!conn)
        logw("Timer for transaction to for unknown connection\n");
    else if (!conn->cli.ongoingTrans)
        logw("Stale timer for transaction fired\n");
    else if (conn->cli.ongoingTrans != timer)
        logw("Wrong timer for transaction fired\n");
    else {
        conn->cli.ongoingTrans = 0;
        //TODO: schedule callback for timeout error
    }
    pthread_mutex_unlock(&mAttLock);
}

/*
 * FUNCTION: attCliReqStart
 * USE:      Prepare to send a request that we expect a response for
 * PARAMS:   to - whom to
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    call with mAttLock held
 */
static uniq_t attCliReqStart(l2c_handle_t to)
{
    struct attConn* conn;

    attConnListAddIfNew(to);
    conn = attConnFindByL2cConn(to);

    if (!conn)
        loge("Cannot find connection for enw ATT transaction\n");
    else if (conn->cli.ongoingTrans)
        logw("Refusing to start ATT transaction while another inflight\n");
    else {
        conn->cli.ongoingTrans = timerSet(ATT_REQ_TIMOUT, attCliTransTimeout, (uint64_t)to);
        return conn->cli.ongoingTrans;
    }

    return 0;
}

/*
 * FUNCTION: attCliReqStop
 * USE:      Clean up state for a sent transaction for ATT client
 * PARAMS:   to - whom to
 * RETURN:   NONE
 * NOTES:    call with mAttLock held
 */
static void attCliReqStop(l2c_handle_t to)
{
    struct attConn* conn;

    attConnListAddIfNew(to);
    conn = attConnFindByL2cConn(to);

    if (!conn)
        loge("Cannot find connection for enw ATT transaction\n");
    else if (!conn->cli.ongoingTrans)
        logw("Refusing to stop ATT transaction while none inflight\n");
    else {
        timerCancel(conn->cli.ongoingTrans);
        conn->cli.ongoingTrans = 0;
    }
}

/*
 * FUNCTION: attCliFindInformation
 * USE:      Send a find-information request to the server
 * PARAMS:   to - whom to
 *           hStart - start handle
 *           hEnd - end handle
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    find or error callbacks passed to attCliInit may be called
 */
uniq_t attCliFindInformation(l2c_handle_t to, uint16_t hStart, uint16_t hEnd)
{
    struct attPduFindInfoReq req;
    uniq_t ret;

    utilSetLE16(&req.startHandle, hStart);
    utilSetLE16(&req.endHandle, hEnd);

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_FIND_INFO_REQ, &req, sizeof(req), NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliFindByTypeValue
 * USE:      Send a find-by-type-value request to the server
 * PARAMS:   to - whom to
 *           hStart - start handle
 *           hEnd - end handle
 *           typeUuid16 - type (must be a uuid16)
 *           valueData - the value to look for
 *           valueLen - the length of the abovementioned value
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    find or error callbacks passed to attCliInit may be called
 */
uniq_t attCliFindByTypeValue(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, uint16_t typeUuid16, const void *valueData, uint32_t valueLen)
{
    uint8_t buf[sizeof(struct attHdr) + sizeof(struct attPduFindByTypeValReq)];
    struct attHdr *hdr = (struct attHdr*)buf;
    struct attPduFindByTypeValReq *req = (struct attPduFindByTypeValReq*)(hdr + 1);
    uint32_t mtu;
    uniq_t ret;

    utilSetLE8(&hdr->opcode, ATT_OPCODE_FIND_BY_TYPE_VAL_REQ);
    utilSetLE16(&req->startHandle, hStart);
    utilSetLE16(&req->endHandle, hEnd);
    utilSetLE16(&req->uuid16, typeUuid16);

    pthread_mutex_lock(&mAttLock);
    mtu = attConnMtuGetByL2c(to, true);
    if (sizeof(req) + valueLen > mtu) {
        logw("Truncating value in FindByTypeValue to fit into mtu %u -> %u\n", valueLen, mtu - sizeof(req));
        valueLen = mtu - sizeof(req);
    }

    ret = attCliReqStart(to);
    if (ret && attSendPacketRaw(to, buf, sizeof(buf), NULL, valueData, valueLen) != ATT_TX_RET_ACCEPTED) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliReadByType
 * USE:      Send a read-by-type request to the server
 * PARAMS:   to - whom to
 *           hStart - start handle
 *           hEnd - end handle
 *           type - the type of values to read
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    read or error callbacks passed to attCliInit may be called
 */
uniq_t attCliReadByType(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, const struct uuid *type)
{
    uint8_t hdrBuf[sizeof(struct attPduReadByTypeReq) + sizeof(struct uuid) + sizeof(uint16_t)]; /* definitely big enoguh to fit both formats */
    struct attPduReadByTypeReq *req = (struct attPduReadByTypeReq*)hdrBuf;
    struct uuid *uuid128 = (struct uuid*)(req + 1);
    uint16_t *uuid16 = (uint16_t*)(req + 1);
    uint16_t tmpUuid;
    unsigned uuidSz;
    uniq_t ret;

    utilSetLE16(&req->startHandle, hStart);
    utilSetLE16(&req->endHandle, hEnd);

    if (uuidToUuid16(&tmpUuid, type)) {
        utilSetLE16(uuid16, tmpUuid);
        uuidSz = sizeof(uint16_t);
    } else {
        uuidWriteLE(uuid128, type);
        uuidSz = sizeof(struct uuid);
    }

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_READ_BY_TYPE_REQ, hdrBuf, sizeof(struct attPduReadByTypeReq) + uuidSz, NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliReadByGroupType
 * USE:      Send a read-by-group-type request to the server
 * PARAMS:   to - whom to
 *           hStart - start handle
 *           hEnd - end handle
 *           type - the group type of values to read
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    read or error callbacks passed to attCliInit may be called
 */
uniq_t attCliReadByGroupType(l2c_handle_t to, uint16_t hStart, uint16_t hEnd, const struct uuid *type)
{
    uint8_t hdrBuf[sizeof(struct attPduReadByGrpTypeReq) + sizeof(struct uuid) + sizeof(uint16_t)]; /* definitely big enoguh to fit both formats */
    struct attPduReadByGrpTypeReq *req = (struct attPduReadByGrpTypeReq*)hdrBuf;
    struct uuid *uuid128 = (struct uuid*)(req + 1);
    uint16_t *uuid16 = (uint16_t*)(req + 1);
    uint16_t tmpUuid;
    unsigned uuidSz;
    uniq_t ret;

    utilSetLE16(&req->startHandle, hStart);
    utilSetLE16(&req->endHandle, hEnd);

    if (uuidToUuid16(&tmpUuid, type)) {
        utilSetLE16(uuid16, tmpUuid);
        uuidSz = sizeof(uint16_t);
    } else {
        uuidWriteLE(uuid128, type);
        uuidSz = sizeof(struct uuid);
    }

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_READ_BY_GRP_TYPE_REQ, hdrBuf, sizeof(struct attPduReadByTypeReq) + uuidSz, NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliRead
 * USE:      Send a read request to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    read or error callbacks passed to attCliInit may be called
 */
uniq_t attCliRead(l2c_handle_t to, uint16_t handle)
{
    struct attPduReadReq req;
    uniq_t ret;

    utilSetLE16(&req.handle, handle);

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_READ_REQ, &req, sizeof(req), NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliReadBlob
 * USE:      Send a read-blob request to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           offset - the offset to request into the value
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    read or error callbacks passed to attCliInit may be called
 */
uniq_t attCliReadBlob(l2c_handle_t to, uint16_t handle, uint16_t offset)
{
    struct attPduReadBlobReq req;
    uniq_t ret;

    utilSetLE16(&req.handle, handle);
    utilSetLE16(&req.offset, offset);

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_READ_BLOB_REQ, &req, sizeof(req), NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}


/*
 * FUNCTION: attCliWriteInt
 * USE:      Send a write request/command to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           data - the data
 *           len - the len
 * RETURN:   non-zero transaction ID or 0 on failure (transaction ID is fake for write command)
 * NOTES:    write or error callbacks passed to attCliInit may be called in case of request. none for command
 */
uniq_t attCliWriteInt(l2c_handle_t to, uint16_t handle, const void* data, uint32_t len, bool cmd)
{
    uint8_t buf[sizeof(struct attHdr) + sizeof(struct attPduWriteCmd)];
    struct attHdr *hdr = (struct attHdr*)buf;
    struct attPduWriteCmd *req = (struct attPduWriteCmd*)(hdr + 1);
    uint32_t mtu;
    uniq_t ret;

    utilSetLE8(&hdr->opcode, cmd ? ATT_OPCODE_WRITE_CMD : ATT_OPCODE_WRITE_REQ);
    utilSetLE16(&req->handle, handle);

    pthread_mutex_lock(&mAttLock);
    mtu = attConnMtuGetByL2c(to, true);
    if (sizeof(buf) + len > mtu) {
        logw("Truncating value in Write to fit into mtu %u -> %u\n", len, mtu - sizeof(req));
        len = mtu - sizeof(buf);
    }

    ret = cmd ? 1 : attCliReqStart(to);
    if (ret && attSendPacketRaw(to, buf, sizeof(buf), NULL, data, len) != ATT_TX_RET_ACCEPTED) {
        if (!cmd)
            attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliWrite
 * USE:      Send a write request to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           data - the data
 *           len - the len
 * RETURN:   non-zero transaction ID or 0 on failure
 * NOTES:    write or error callbacks passed to attCliInit may be called
 */
uniq_t attCliWrite(l2c_handle_t to, uint16_t handle, const void* data, uint32_t len)
{
    return attCliWriteInt(to, handle, data, len, false);
}

/*
 * FUNCTION: attCliWrite
 * USE:      Send a write command to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           data - the data
 *           len - the len
 * RETURN:   false on failure
 * NOTES:    no callbacks will be called.
 */
bool attCliWriteNoAck(l2c_handle_t to, uint16_t handle, const void* data, uint32_t len)
{
    return !!attCliWriteInt(to, handle, data, len, true);
}

/*
 * FUNCTION: attCliSignedWriteNoAck
 * USE:      Send a signed write command to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           data - the data
 *           len - the len
 * RETURN:   false on failure
 * NOTES:    no callbacks will be called.
 */
bool attCliSignedWriteNoAck(l2c_handle_t to, uint16_t handle, const void* data, uint32_t len)
{
    uint8_t hdr[sizeof(struct attHdr) + sizeof(struct attPduSignedWriteCmd)];
    struct attHdr *attHdr = (struct attHdr*)hdr;
    struct attPduSignedWriteCmd *cmd = (struct attPduSignedWriteCmd*)(hdr + 1);
    uint8_t sig[ATT_SIG_LEN];
    uint8_t k[SM_BLOCK_LEN];
    bool ret = false;
    uint32_t mtu;
    sg s;

    utilSetLE8(&attHdr->opcode, ATT_OPCODE_SIGNED_WRITE_CMD);
    utilSetLE16(&cmd->handle, handle);

    if (!persistGetDevKey(NULL, KEY_TYPE_CSRK, k)) {
        logw("Cannot get ownCSRK for signed write\n");
        return false;
    }

    pthread_mutex_lock(&mAttLock);
    mtu = attConnMtuGetByL2c(to, true);
    if (sizeof(hdr) + sizeof(sig) + len > mtu) {
        logw("Truncating value in SignedWrite to fit into mtu %u -> %u\n", len, mtu - sizeof(hdr) - sizeof(sig));
        len = mtu - sizeof(hdr) - sizeof(sig);
    }

    s = sgNewWithCopyData(data, len);
    if (!s)
        loge("Failed to alloc SG for signed write\n");
    else if (!sgConcatFrontCopy(s, hdr, sizeof(hdr)))
        loge("Failed to concat SG for signed write\n");
    else {
        smSignatureCalc(s, k, sig);

        ret = attSendPacketRaw(to, NULL, 0, s, sig, sizeof(sig)) == ATT_TX_RET_ACCEPTED;
        if (ret)
            s = NULL;
    }

    pthread_mutex_unlock(&mAttLock);

    if (s)
        sgFree(s);
    return ret;
}

/*
 * FUNCTION: attCliPrepareWrite
 * USE:      Send a prepare-write request to the server
 * PARAMS:   to - whom to
 *           handle - the handle
 *           offset - the offset into the data to write
 *           data - the data
 *           len - the len
 * RETURN:   transaction id or 0 on failure
 * NOTES:    write or error callbacks passed to attCliInit may be called
 */
uniq_t attCliPrepareWrite(l2c_handle_t to, uint16_t handle, uint16_t offset, const void* data, uint32_t len)
{
    uint8_t buf[sizeof(struct attHdr) + sizeof(struct attPduPrepareWriteReq)];
    struct attHdr *hdr = (struct attHdr*)buf;
    struct attPduPrepareWriteReq *req = (struct attPduPrepareWriteReq*)(hdr + 1);
    uint32_t mtu;
    uniq_t ret;

    utilSetLE8(&hdr->opcode, ATT_OPCODE_PREPARE_WRITE_REQ);
    utilSetLE16(&req->handle, handle);
    utilSetLE16(&req->offset, offset);

    pthread_mutex_lock(&mAttLock);
    mtu = attConnMtuGetByL2c(to, true);
    if (sizeof(buf) + len > mtu) {
        logw("Truncating value in PrepareWrite to fit into mtu %u -> %u\n", len, mtu - sizeof(req));
        len = mtu - sizeof(buf);
    }

    ret = attCliReqStart(to);
    if (ret && attSendPacketRaw(to, buf, sizeof(buf), NULL, data, len) != ATT_TX_RET_ACCEPTED) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}

/*
 * FUNCTION: attCliExecuteStagedWrites
 * USE:      Send a execute-write request to the server
 * PARAMS:   to - whom to
 *           commit - commit data or destroy it?
 * RETURN:   transaction id or 0 on failure
 * NOTES:    write or error callbacks passed to attCliInit may be called
 */
uniq_t attCliExecuteStagedWrites(l2c_handle_t to, bool commit)
{
    struct attPduExecuteWriteReq req;
    uniq_t ret;

    utilSetLE8(&req.flags, commit ? ATT_EXECUTE_WRITE_FLG_EXECUTE : ATT_EXECUTE_WRITE_FLG_CANCEL);

    pthread_mutex_lock(&mAttLock);
    ret = attCliReqStart(to);
    if (ret && !attSendPacket(to, ATT_OPCODE_EXECUTE_WRITE_REQ, &req, sizeof(req), NULL)) {
        attCliReqStop(to);
        ret = 0;
    }
    pthread_mutex_unlock(&mAttLock);

    return ret;
}







