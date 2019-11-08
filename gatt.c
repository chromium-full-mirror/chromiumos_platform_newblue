/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdlib.h>
#include <string.h>
#include "newblue-macros.h"
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "sendQ.h"
#include "uniq.h"
#include "uuid.h"
#include "util.h"
#include "gatt.h"
#include "att.h"
#include "log.h"
#include "mt.h"
#include "bt.h"

// TODO: if multiple clients stage writes/try to commit them, we do not arbitrate that. Depending on the desired use of this, we may have to

/* A NOTE ABOUT LOCKS:
 * we call callbacks with locks dropped. This is on purpose, since the
 * callbacks can call back into GATT. We need the locks held to use the various
 * data structures though. This leads to the common structure you'll see here
 * as lock -> work -> unlock -> callback -> relock -> more work. How do we make
 * sure that nothing changes while the lock is dropped? We do not and cannot.
 * So what do we do? There are two things we care about. One is the connection
 * itself going away. We solve this easily by again looking it up when we
 * re-take the locks. If we do not find it, it went away and we can stop
 * whatever we were doing. Now, about that. We keep track of what we were doing
 * using a "ToDo item" structure. When a connection is torn down, all of its
 * associated ToDo items are freed. But, this may happen in a thread different
 * from the one currently in the callback. How do we avoid this race? Two
 * solutions exist. One is to have a per-todo-item lock. That would not be fun
 * as we'd be constantly creating and deleting locks. Another is simpler - a
 * flag in the connection saying "do not free *current* ToDo item". We set this
 * flag before dropping the locks, and clear it when we re-take them. If a
 * connection is closed and this flag is set, the cleanup code will not free
 * the *current* ToDo item. This makes sure the code after lock re-acquisition
 * can continue to run. When it fails to find the connection, it knows that
 * it itself must free the ToDo item it was working on. Thus: no memory leak
 * and no use-after-free.
 */


#define GATT_ITEM_TYPE_INCLUDE     1
#define GATT_ITEM_TYPE_CHAR_DECL   2
#define GATT_ITEM_TYPE_CHAR_VAL    3
#define GATT_ITEM_TYPE_CHAR_DESCR  4

#define GATT_HDR_HANDLES           1 /* handles required for a service header */
struct gattService;
struct gattCliNotif;

struct gattIncludeDefHdr {
    uint16_t startHandle;
    uint16_t endHandle;
    /* uuid here IFF it is a uuid-16 */
} __packed;

struct gattCharDefHdr {
    uint8_t charProps;
    uint16_t valHandle;
    /* uuid here 16 or 128 bits */
} __packed;

// we can have one ATT transaction outstanding per connection. This list serializes them
struct gattCliTodoItem {
    struct gattCliTodoItem            *next;
    gatt_client_conn_t                 connId;       // higher level ID - unused by us

    uint8_t                            type;         // what sort of an ATT request is it? GATT_CLI_TODO_*
    uint8_t                            purpose;      // why are we doing it? GATT_INTENT_*
    bool                               enqueued;     // for error checking

    // per-type info (selectively used for various types of requests)
    bool                               execute;
    uint16_t                           handle;
    uint16_t                           endHandle;
    uint16_t                           auxHandle[2]; // sometimes we need to store some handle values temporarily in a todo's state. eg for enum included svcs, when reading 128 bit uuid, it stores {next incl handle to search, end of cur handle }
    union {
        uint16_t                       offset;       // for some reads and writes
        uint16_t                       mtu;          // for MTU negotiations
    };
    struct uuid                        uuid;         // uuid16 for find-by-type-value, any uuid for read-by-type, read-by-group-type
    bool                               commit;       // for write-execute
    uint8_t                            authReq;      // for some reads and writes
    sg                                 data;         // sometimes we actually need data
    uniq_t                             notifLocator; // used to find a notif rec for cccd writes

    uniq_t                             clientTrans;  // unused by us. set and given to clients for state tracking
    void                              *userData;     // unused by us. set and given to clients for their own use

    struct uuid                        desiredUuid;  // find-service-by-uuid will save the desired uuid here as else we'll lose it

    // callbacks
    union {
        gattCliSvcEnumCbk              enumSvcsCbk;
        gattCliEnumCharacteristicsCbk  enumCharsCbk;
        gattCliEnumCharDescrsCbk       enumCharDscrsCbk;
        gattCliStagedWriteExecuteCb    executeCbk;
        gattCliEnumIncludedSvcsCbk     enumIncludedSvcsCbk;
        gattCliReadCbk                 readCbk;
        gattCliWriteCbk                writeCbk;
    };
};

#define GATT_CLI_TODO_FIND_INFO          0
#define GATT_CLI_TODO_FIND_BY_TYPE_VAL   1
#define GATT_CLI_TODO_READ_BY_TYPE       2
#define GATT_CLI_TODO_READ               3
#define GATT_CLI_TODO_READ_BLOB          4
#define GATT_CLI_TODO_READ_BY_GRP_TYPE   5
#define GATT_CLI_TODO_WRITE              6
#define GATT_CLI_TODO_WRITE_NO_ACK       7
#define GATT_CLI_TODO_SIGNED_WRITE       8
#define GATT_CLI_TODO_PREPARE_WRITE      9
#define GATT_CLI_TODO_WRITE_EXEC         10
#define GATT_CLI_TODO_SEND_MTU_REQ       11
#define GATT_CLI_TODO_WRITE_CCCD         12 // special case that will devolve into a write once we resolve what to write

#define GATT_INTENT_INTERNAL             0
#define GATT_INTENT_FIND_SVC             1
#define GATT_INTENT_ENUM_SVCS            2
#define GATT_INTENT_ENUM_INCL_SVCS       3
#define GATT_INTENT_ENUM_CHARS           4
#define GATT_INTENT_ENUM_CHAR_DESCRS     5
#define GATT_INTENT_READ                 6
#define GATT_INTENT_WRITE                7
#define GATT_INTENT_EXECUTE              8
#define GATT_INTENT_NOTIF_SUB_UNSUB      9
#define GATT_INTENT_MTU_XCHG             10


#define GATT_CONN_STATE_CONNECTING       0  /* only if initiating */
#define GATT_CONN_STATE_RUNNING          1

struct GattEnqueuedWrite {
    struct GattEnqueuedWrite *next;
    att_range_t rangeRef;
    uint16_t handleOfst;
    sg data;
};

struct gattConnState {
    struct gattConnState     *next;
    struct gattConnState     *prev;
    uniq_t                    gattConnId;

    struct gattCliNotif      *cliNotifs; // things we as a client are subscribed to
    struct bt_addr            addr;
    l2c_handle_t              conn;
    uint32_t                  mtu;
    uint8_t                   state;
    bool                      encr;

    uint32_t                  queuedLength; // how much we've buffered so far
    struct GattEnqueuedWrite *queuedWrites;

    struct gattCliTodoItem   *todoListH;
    struct gattCliTodoItem   *todoListT;
    struct gattCliTodoItem   *todoCurr; // or null if none
    bool                      doNotFreeTodoCurr; // used to synchronize who frees "todoCurr". See "A NOTE ABOUT LOCKS"

    // keep track of things we need to know to know when to close the connection
    bool                      hadRemoteUse; //set if remote ever did GATT-ing to us
    uint32_t                  numLocalUsers;  //number of local users of this connection
};

struct gattItem { /* a list of these exists in a a gatt service */
    struct gattItem    *next;
    struct gattItem    *prev;
    uint8_t             type;
    union {
        struct{
            struct gattService *incl;
        } include;
        struct {
            struct uuid         uuid;
            uint8_t             charProps;
        } charDecl;
        struct {
            uint32_t            valPerms;
        } charVal;
        struct {
            struct uuid         uuid;
            uint32_t            descrValPerms;
        } descriptor;
    };
};

struct gattService {
    struct gattService  *next;
    struct gattService  *prev;
    struct uuid          uuid;
    att_range_t          attRange;
    uint16_t             numItems;
    uint16_t             maxItems;
    gatt_service_t       svcRef;

    gattSrvValueReadCbk  readF;
    gattSrvValueWriteCbk writeF;
    gattSrvIndCbk        indF;
    gattSrvDisconnectCbk discF;

    struct gattItem     *itemsHead;
    struct gattItem     *itemsTail;
    uint16_t             useCount; /* how many services include this one */
    bool                 primary;
    bool                 online;
    bool                 allowLE;
    bool                 allowEDR;
};

struct gattCliNotif { /* keeps track of all notifications all clients are subscribed to */
    struct gattCliNotif     *prev;
    struct gattCliNotif     *next;
    uint32_t                 refCtInd;         // how many clients want indication
    uint32_t                 refCtNotif;       // how many clients want notification
    uniq_t                   locator;          // used to locate this cli-notif structure
    uint8_t                  curState;         // NOTIF_C_STATE_*

    /* cache of relevant values for speed and convenience. spec promises these can never change */
    uint16_t                 charHeaderHandle; /* first handle for this characteristic */
    uint16_t                 charValueHandle;  /* characteristic's actual value's handle */
    uint16_t                 charConfigHandle; /* characteristic's "config" handle for unsubscribing */
};

#define NOTIF_C_STATE_NOT_SUBBED         0
#define NOTIF_C_STATE_SUBBED_TO_IND      1
#define NOTIF_C_STATE_SUBBED_TO_NOTIF    2


struct gattCliNotifRec { /* used to keep track of all notifications that a client is subscribed to */
    struct gattCliNotifRec       *next;
    struct gattCliNotifRec       *prev;
    struct gattCliNotif          *notif;
    bool                          wantInd;       // else we'll ask for notif
    bool                          subCallMade;   // set once we have sent the notif sub status callback (to make sure we only do once)
    bool                          unsubCallMade; // set once we have sent the notif unsub status callback (to make sure we only do once)

    gattCliNotifArrivedCbk        arrivedCbk;
    gattCliNotifSubscribeStateCbk stateCbk;      // only set if we haven't yet called it
};

struct gattClientConnection {
    struct gattClientConnection  *next;
    struct gattClientConnection  *prev;
    uniq_t                        gattConnId;
    struct gattCliNotifRec       *notifs;
    gatt_client_conn_t            connId; // used by our callers
    gattCliConnectResultCbk       connCbk;
    void                         *userData;
};

struct gattWorkItem {
    uint8_t type;
    union {
        struct {
            l2c_handle_t             who;
            union {
                gattSrvValueReadCbk  readF;
                gattSrvValueWriteCbk writeF;
            };
            gatt_service_t           svcRef;
            att_cid_t                cid;
            att_trans_t              transId;
            uint16_t                 handle;
            uint16_t                 byteOfst;
            uint16_t                 len;
            uint8_t                  reason;
            /* data here, if write */
        } srvRwCall;
        struct {
            uint64_t                 ref;
            l2c_handle_t             who;
            gattSrvIndCbk            indF;
            gatt_service_t           svcRef;
            att_cid_t                cid;
            uint16_t                 handle;
            uint8_t                  evt;
        } srvIndCall;
        struct {
            l2c_handle_t             who;
            gattSrvDisconnectCbk     discF;
            gatt_service_t           svcRef;
        } srvDiscCall;
        struct {
            l2c_handle_t             who;
            att_cid_t                cid;
            struct GattEnqueuedWrite *q;
            att_trans_t              transId;
        } srvWriteExec;
        struct {
            gattCliConnectResultCbk  cbk;
            void                     *userData;
            gatt_client_conn_t       connId;
            uint8_t                  status;
        } cliConnStateNotif;
        struct {
            l2c_handle_t             conn;
            struct gattCliTodoItem  *todo;
        } cliDoNext;
        struct {
            gattCliNotifArrivedCbk   cbk;
            gatt_client_conn_t       conn;
            uint16_t                 handle;
            bool                     ind;
            sg                       data;
        } cliIndCall;
        struct {
            gattCliNotifSubscribeStateCbk  cbk;
            gatt_client_conn_t       conn;
            uint16_t                 handle;
            bool                     forSub;
            uint8_t                  stat;
        } cliSubUnsubCall;
    };
};

struct GattCliUtilSvcTraverseState {
    struct GattCliUtilSvcTraverseState *next;
    struct GattCliUtilSvcTraverseState *prev;
    uniq_t curTrans, clientTrans;
    struct GattTraversedService res;
    uint32_t numChars, numDescrs, numInclSvcs;
    struct GattTraversedServiceChar *allChars;
    struct GattTraversedServiceCharDescr* allDescrs;
    struct GattTraversedServiceInclSvc *allInclSvcs;
    gattCliUtilSvcTraversedCbk cbk;
    void *userData; // unused by us. set and given to clients for their own use
};

struct GattCliUtilLongReadState {
    struct GattCliUtilLongReadState *next;
    struct GattCliUtilLongReadState *prev;
    void*  userData;
    uniq_t curTrans, clientTrans;
    uint16_t handle;
    uint8_t authReq;
    sg dataSoFar;
    gattCliUtilLongReadCompletedCbk cbk;
};

#define GATT_WORK_SRV_READ_CALL          0
#define GATT_WORK_SRV_WRITE_CALL         1
#define GATT_WORK_SRV_IND_CALL           2
#define GATT_WORK_SRV_DISC_CALL          3
#define GATT_WORK_SRV_WRITE_EXEC         4
#define GATT_WORK_CLI_CONN_STATE_NOTIF   5
#define GATT_WORK_CLI_DO_NEXT            6 // run the next todo item, if none in flight currently
#define GATT_WORK_CLI_IND_CALL           7
#define GATT_WORK_CLI_SUB_UNSUB_CALL     8


/* our state */
static struct sendQ* mSendQ;
static struct workQueue* mWorkQ;
static pthread_t mWorker;

static pthread_mutex_t mGattLock = PTHREAD_MUTEX_INITIALIZER;
static struct gattConnState *mConns = NULL; /* one per L2C conn (max one per peer) */
static struct gattService *mSvcs = NULL;
static struct gattClientConnection *mClientConns = NULL; /* one per client's idea of a connection */

static pthread_mutex_t mGattUtilLock = PTHREAD_MUTEX_INITIALIZER;
static struct GattCliUtilSvcTraverseState *mUtilSvcEnumStates;
static struct GattCliUtilLongReadState *mUtilLongReadStates;

/* fwd decls */
static struct gattCliTodoItem* gattCliTodoMakeWrite(gatt_client_conn_t connId, uint8_t purpose, uint8_t writeType, uint16_t handle, uint8_t authReq, uint16_t offset, sg data);
static void gattCliTodoEnqItem(struct gattConnState *conn, struct gattCliTodoItem *todo, bool inFront);
static void gattCliTodoNext(l2c_handle_t conn, struct gattCliTodoItem *now);
static void gattCliTodoItemFree(struct gattCliTodoItem *todo);
static void gattWorkItemFree(struct gattWorkItem *wi);
static struct gattCliTodoItem* gattCliTodoMakeMtuReq(struct gattConnState* conn, uint16_t mtu, uint8_t purpose);
static uint8_t gattClientUtilLongReadScheduleNextChunk(gatt_client_conn_t conn, uint32_t curTrans, bool needCallback);

/*
 * FUNCTION: gattConnFindByL2cConn
 * USE:      find a per-connection structure for a given l2c handle
 * PARAMS:   conn - the l2c connection handle
 * RETURN:   connection structure or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattConnFindByL2cConn(l2c_handle_t conn)
{
    struct gattConnState *state;

    for (state = mConns; state && state->conn != conn; state = state->next);

    return state;
}

/*
 * FUNCTION: gattConnFindByAddr
 * USE:      find a per-connection structure for a given address
 * PARAMS:   addr - the address
 * RETURN:   connection structure or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattConnFindByAddr(const struct bt_addr *addr)
{
    struct gattConnState *state;

    for (state = mConns; state && memcmp(addr, &state->addr, sizeof(struct bt_addr)); state = state->next);

    return state;
}

/*
 * FUNCTION: gattConnFindById
 * USE:      find a per-connection structure for a given gatt connection id
 * PARAMS:   gattConnId - the connection id
 * RETURN:   connection structure or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattConnFindById(uniq_t gattConnId)
{
    struct gattConnState *state;

    for (state = mConns; state && state->gattConnId != gattConnId; state = state->next);

    return state;
}

/*
 * FUNCTION: gattNotifFindByLocatorInConn
 * USE:      find a notification record structure given a connection it belongs to and its locator value
 * PARAMS:   conn - connection structure
 *           notifLocator - unique notif record locator value
 * RETURN:   notif structure or NULL
 * NOTES:    call with mGattLock held
 */
static struct gattCliNotif* gattNotifFindByLocatorInConn(struct gattConnState* conn, uniq_t notifLocator)
{
    struct gattCliNotif *notif;

    for (notif = conn->cliNotifs; notif; notif = notif->next) {
        if (notif->locator == notifLocator)
            return notif;
    }

    return NULL;
}

/*
 * FUNCTION: gattQueuedWriteQueueFree
 * USE:      Free a queued wrote linked list properly
 * PARAMS:   qw - the queued write linked list
 * RETURN:   NONE
 * NOTES:
 */
static void gattQueuedWriteQueueFree(struct GattEnqueuedWrite *qw)
{
    struct GattEnqueuedWrite *t;

    while (qw) {
        t = qw;
        qw = qw->next;
        if (t->data)
            sgFree(t->data);
        free(t);
    }
}

/*
 * FUNCTION: gattConnStructDelete
 * USE:      Delete and unlink a connection struct
 * PARAMS:   state - said struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattConnStructDelete(struct gattConnState *state)
{
    struct gattCliNotif *nC, *nN;
    struct gattCliTodoItem *tC, *tN;

    for (nC = state->cliNotifs; nC; nC = nN) {
        nN = nC->next;
        free(nC);
    }

    for (tC = state->todoListH; tC; tC = tN) {
        tN = tC->next;
        gattCliTodoItemFree(tC);
    }

    if (state->todoCurr && !state->doNotFreeTodoCurr)
        gattCliTodoItemFree(state->todoCurr);

    gattQueuedWriteQueueFree(state->queuedWrites);

    if (state->next)
        state->next->prev = state->prev;
    if (state->prev)
        state->prev->next = state->next;
    else
        mConns = state->next;

    /* future free steps here */
    free(state);
}

/*
 * FUNCTION: gattServiceFindByUuid
 * USE:      Find a gatt service by uuid
 * PARAMS:   uuid - the uuid
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService* gattServiceFindByUuid(const struct uuid *uuid)
{
    struct gattService *svc = mSvcs;

    while (svc && !uuidCmp(uuid, &svc->uuid))
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattServiceFindBySvcRef
 * USE:      Find a gatt service by service reference
 * PARAMS:   svcRef - the service reference
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService* gattServiceFindBySvcRef(gatt_service_t svcRef)
{
    struct gattService *svc = mSvcs;

    while (svc && svcRef != svc->svcRef)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattClientConnFindById
 * USE:      Find a gatt client connection structure by connId
 * PARAMS:   connId - the connection id
 * RETURN:   client connection struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattClientConnection* gattClientConnFindById(gatt_client_conn_t connId)
{
    struct gattClientConnection *clicon = mClientConns;

    while (clicon && connId != clicon->connId)
        clicon = clicon->next;

    return clicon;
}

/*
 * FUNCTION: gattServiceFindByRangeRef
 * USE:      Find a gatt service by att range reference
 * PARAMS:   rangeRef - the service range reference
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService *gattServiceFindByRangeRef(att_range_t rangeRef)
{
    struct gattService *svc = mSvcs;

    while (svc && rangeRef != svc->attRange)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattAttSrvTxCbk
 * USE:      ATT calls this to TX to a client
 * PARAMS:   who - whom to send to
 *           s - the data to send
 * RETURN:   ATT_TX_RET_*
 * NOTES:
 */
static uint8_t gattAttSrvTxCbk(l2c_handle_t who, sg s)
{
    return sendQueueTx(mSendQ, who, s) ? ATT_TX_RET_ACCEPTED : ATT_TX_RET_L2C_ERR;
}

/*
 * FUNCTION: gattEnqueueSrvRwCall
 * USE:      Enqueue a work item to make a call to the higher layer for a read or a write
 * PARAMS:   workType - read or write?
 *           svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle
 *           byteOfst - byte offset for the read/write
 *           reason - ATT_READ_FOR_* or ATT_WRITE_FOR_*
 *           len - requested length
 *           data - NULL for read, data for write
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueSrvRwCall(uint8_t workType, struct gattService *svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem) + (data ? len : 0));

    if (wi) {
        wi->type = workType;
        wi->srvRwCall.who = who;
        wi->srvRwCall.svcRef = svc->svcRef;
        wi->srvRwCall.cid = cid;
        wi->srvRwCall.transId = transId;
        wi->srvRwCall.handle = handle;
        wi->srvRwCall.byteOfst = byteOfst;
        wi->srvRwCall.len = len;
        wi->srvRwCall.reason = reason;
        if (workType == GATT_WORK_SRV_READ_CALL)
            wi->srvRwCall.readF = svc->readF;
        else {
            wi->srvRwCall.writeF = svc->writeF;
            memcpy(wi + 1, data, len);
        }

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueSrvReadCall
 * USE:      Enqueue a work item to make a call to the higher layer for a read
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle
 *           byteOfst - byte offset for the read
 *           reason - ATT_READ_FOR_*
 *           len - requested length
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueSrvReadCall(struct gattService *svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
{
    return gattEnqueueSrvRwCall(GATT_WORK_SRV_READ_CALL, svc, who, cid, transId, handle, byteOfst, reason, maxLen, NULL);
}

/*
 * FUNCTION: gattEnqueueSrvWriteCall
 * USE:      Enqueue a work item to make a call to the higher layer for a write
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle
 *           byteOfst - byte offset for the write
 *           reason - ATT_WRITE_FOR_*
 *           len - requested length
 *           data - data to write
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueSrvWriteCall(struct gattService *svc, l2c_handle_t who, att_cid_t cid, att_trans_t transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    return gattEnqueueSrvRwCall(GATT_WORK_SRV_WRITE_CALL, svc, who, cid, transId, handle, byteOfst, reason, len, data);
}

/*
 * FUNCTION: gattEnqueueSrvIndCall
 * USE:      Enqueue a work item to make a call to the higher layer for a result of sending an indication/notification
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           handle - handle
 *           evt - what happened
 *           ref - the value to pass to callback
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueSrvIndCall(struct gattService *svc, l2c_handle_t who, att_cid_t cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));

    if (wi) {
        wi->type = GATT_WORK_SRV_IND_CALL;
        wi->srvIndCall.who = who;
        wi->srvIndCall.svcRef = svc->svcRef;
        wi->srvIndCall.cid = cid;
        wi->srvIndCall.handle = handle;
        wi->srvIndCall.evt = evt;
        wi->srvIndCall.ref = ref;
        wi->srvIndCall.indF = svc->indF;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueSrvDiscCall
 * USE:      Enqueue a work item to make a call to the higher layer for a result of a disconnection
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueSrvDiscCall(struct gattService *svc, l2c_handle_t who)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));

    if (wi) {
        wi->type = GATT_WORK_SRV_DISC_CALL;
        wi->srvDiscCall.who = who;
        wi->srvDiscCall.svcRef = svc->svcRef;
        wi->srvDiscCall.discF = svc->discF;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueCliConnStatusCall
 * USE:      Enqueue a work item to make a call to the higher layer for a result of sending a GATT connect/disconnetc
 * PARAMS:   clicon - the client connection structure
 *           status - the status to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliConnStatusCall(struct gattClientConnection *clicon, uint8_t status)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));

    if (wi) {
        wi->type = GATT_WORK_CLI_CONN_STATE_NOTIF;
        wi->cliConnStateNotif.cbk = clicon->connCbk;
        wi->cliConnStateNotif.userData = clicon->userData;
        wi->cliConnStateNotif.connId = clicon->connId;
        wi->cliConnStateNotif.status = status;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueCliDoNextTodoItem
 * USE:      Enqueue a work item to perform the next todo item if none is currently in flight for this connection
 * PARAMS:   conn - the connection structure
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliDoNextTodoItem(struct gattConnState *conn)
{
    struct gattCliTodoItem *now;
    struct gattWorkItem *wi;

    logv("gattEnqueueCliDoNextTodoItem cur=%p next=%p\n", conn->todoCurr, conn->todoListH);

    if (conn->todoCurr) {
        logv("gattEnqueueCliDoNextTodoItem called with an existing transaction running - not necessary\n");
        return true;
    }
    if (!conn->todoListH) {
        logv("gattEnqueueCliDoNextTodoItem called with no todo items - not necessary\n");
        return true;
    }

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {
        now = conn->todoListH;

        wi->type = GATT_WORK_CLI_DO_NEXT;
        wi->cliDoNext.conn = conn->conn;
        wi->cliDoNext.todo = now;
        now->enqueued = false;

        if (workQueuePut(mWorkQ, wi)) {
            conn->todoListH = conn->todoListH->next;
            if (!conn->todoListH)
                conn->todoListT = NULL;
            conn->todoCurr = now;
            now->next = NULL;

            return true;
        }
        free(wi);
    }

    return false;
}

/*
 * FUNCTION: gattEnqueueCliIndCall
 * USE:      Enqueue a work item to perform a notif/ind callback to the caller
 * PARAMS:   clicon - client connection struct
 *           nr - the notification record
 *           data - the data to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliIndCall(struct gattClientConnection *clicon, struct gattCliNotifRec *nr, sg data)
{
    struct gattWorkItem *wi;

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {

        wi->type = GATT_WORK_CLI_IND_CALL;
        wi->cliIndCall.cbk = nr->arrivedCbk;
        wi->cliIndCall.conn = clicon->connId;
        wi->cliIndCall.handle = nr->notif->charValueHandle;
        wi->cliIndCall.ind = nr->wantInd;
        wi->cliIndCall.data = data;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }

    return false;
}

/*
 * FUNCTION: gattEnqueueCliSubUnsubCall
 * USE:      Enqueue a work item to perform a subscribe/unsubscribe notification to a caller for ind/notifs
 * PARAMS:   clicon - client connection struct
 *           nr - the notification record
 *           cbk - the callback to call (passed directly as it is not always the one in nr that we want to call)
 *           forSub - is this call for a subscribe or an unsubscribe request
 *           stat - the statsu value to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliSubUnsubCall(struct gattClientConnection *clicon, struct gattCliNotifRec *nr, gattCliNotifSubscribeStateCbk cbk, bool forSub, uint8_t stat)
{
    struct gattWorkItem *wi;

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {

        wi->type = GATT_WORK_CLI_SUB_UNSUB_CALL;
        wi->cliSubUnsubCall.cbk = cbk;
        wi->cliSubUnsubCall.conn = clicon->connId;
        wi->cliSubUnsubCall.handle = nr->notif->charHeaderHandle;
        wi->cliSubUnsubCall.forSub = forSub;
        wi->cliSubUnsubCall.stat = stat;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }

    return false;
}

/*
 * FUNCTION: gattAttSrvIndCbk
 * USE:      ATT calls this with results of sending an indication/notification
 * PARAMS:   who - who the remote is
 *           cid - the connection Id to pass back to ATT
 *           rangeRef - the range this is related to
 *           offset - the value in the range
 *           evt - ATT_SRV_EVT_NOTIF_*
 *           ref - the reference passed to attSrvValueNotifyChanged() initially
 *           success - did we succeed?
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvIndCbk(l2c_handle_t to, att_cid_t cid, att_range_t rangeRef, uint16_t offset, uint8_t evt, uint64_t ref)
{
    struct gattConnState *conn;
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(to);

    // remember that our gatt server has been used by this connection
    if (conn)
        conn->hadRemoteUse = true;

    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc)
        logw("Failed to find service for read\n");
    else if (!gattEnqueueSrvIndCall(svc, to, cid, offset + attSrvHandleRangeGetBase(svc->attRange), evt, ref))
        loge("Failed to forward notif event up\n");
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttSvrReadCbk
 * USE:      ATT calls this to read an attribute
 * PARAMS:   who - who the remote is
 *           rangeRef - the range this is related to
 *           cid - the connection Id to pass back to ATT
 *           ofst - the value in the range
 *           transId - transaction ID
 *           byteOfst - how mnay bytes into the characteristic we want to readsrvcre
 *           reason - read reason (ATT_READ_FOR_*)
 *           maxLen - max bytes to return
 * RETURN:   false on immediate failure
 * NOTES:    when read completes, a call to attSrvCbkReply() should be made
 */
static bool gattAttSvrReadCbk(l2c_handle_t who, att_cid_t cid, att_range_t rangeRef, uint16_t ofst, att_trans_t transId, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
{
    uint8_t err = ATT_ERROR_NONE;
    struct gattConnState *conn;
    struct gattService *svc;
    struct gattItem *gi;
    uint8_t *ptr = NULL;
    uint16_t len = 0;
    uint16_t tmp;
    uint8_t buf[32];

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(who);

    // remember that our gatt server has been used by this connection
    if (conn)
        conn->hadRemoteUse = true;

    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc) {
        logw("Failed to find service for read\n");
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    }

    if (!ofst) { /* service header */
        if (uuidToUuid16(&tmp, &svc->uuid)) { /* if it fits in 16 bits, do that */
            utilSetLE16(buf, tmp);
            ptr = buf;
            len = sizeof(uint16_t);
        } else {                                 /* else, send the entire 16-bute uuid */
            uuidWriteLE(buf, &svc->uuid);
            ptr = buf;
            len = sizeof(struct uuid);
        }
    } else if (ofst < GATT_HDR_HANDLES) {
        logw("Unexpected offset %u after header and before handles!\n", ofst);
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    } else {
        uint16_t ctr = ofst;
        gi = svc->itemsHead;
        ctr -= GATT_HDR_HANDLES;
        while (gi && ctr--)
            gi = gi->next;

        if (!gi) {
            logw("Handle list ended abruptly with %u handles to go\n", ctr);
            err = ATT_ERROR_INVALID_HANDLE;
            goto out;
        }

        if (gi->type == GATT_ITEM_TYPE_INCLUDE) {
            struct gattIncludeDefHdr *hdr = (struct gattIncludeDefHdr*)buf;
            struct gattService *incl = gi->include.incl;
            uint16_t start;

            start = attSrvHandleRangeGetBase(incl->attRange);

            if (!attSrvValueGetGrpLen(incl->attRange, 0, &tmp)) {
                loge("Failed to get service len for included service\n");
                err = ATT_ERROR_UNLIKELY_ERROR;
                goto out;
            }
            utilSetLE16(&hdr->startHandle, start);
            utilSetLE16(&hdr->endHandle, start + tmp - 1);
            ptr = buf;
            len = sizeof(struct gattIncludeDefHdr);

            if (uuidToUuid16(&tmp, &incl->uuid)) { /* fits into a uuid-16 */
                utilSetLE16(hdr + 1, tmp);
                len += sizeof(uint16_t);
            }

        } else if (gi->type == GATT_ITEM_TYPE_CHAR_DECL) {
            struct gattCharDefHdr *hdr = (struct gattCharDefHdr*)buf;

            utilSetLE8(&hdr->charProps, gi->charDecl.charProps);
            utilSetLE16(&hdr->valHandle, ofst + attSrvHandleRangeGetBase(svc->attRange) + 1); /* always next */
            ptr = buf;
            len = sizeof(struct gattCharDefHdr);

            if (uuidToUuid16(&tmp, &gi->charDecl.uuid)) { /* fits into a uuid-16 */
                utilSetLE16(hdr + 1, tmp);
                len += sizeof(uint16_t);
            } else {
                uuidWriteLE(hdr + 1, &gi->charDecl.uuid);
                len += sizeof(struct uuid);
            }

        } else { /* in this case call to the higher layer for the read */
            if (!gattEnqueueSrvReadCall(svc, who, cid, transId, ofst + attSrvHandleRangeGetBase(svc->attRange), byteOfst, reason, maxLen))
                err = ATT_ERROR_UNLIKELY_ERROR;
            goto out;
        }
    }

    if (!ptr) {
        loge("No data to send at end of read callback\n");
        err = ATT_ERROR_UNLIKELY_ERROR;
    } else if (byteOfst > len)
        err = ATT_ERROR_INVALID_OFFSET;
    else {
        len -= byteOfst;
        ptr += byteOfst;
    }

out:
    pthread_mutex_unlock(&mGattLock);

    if (len || err != ATT_ERROR_NONE)
        attSrvCbkReply(cid, transId, 0, err, ptr, len);
    return true;
}

/*
 * FUNCTION: gattAttSrvProcessWriteReq
 * USE:      Handle a prepare write command
 * PARAMS:   who - who the remote is
 *           cid - ATT's connection ID
 *           rangeRef - the ATT range raference
 *           handleOfst - offset of the given handle in the range
 *           byteOfst - how many bytes into the characteristic we want to write
 *           len - bytes to write
 *           data - the data
 * RETURN:   ATT_ERROR_*
 * NOTES:    called with mGattLock held
 */
static uint8_t gattAttSrvProcessPrepareWriteReq(l2c_handle_t who, att_cid_t cid, att_range_t rangeRef, uint16_t handleOfst, uint32_t byteOfst, uint32_t len, const void *data)
{
    struct gattConnState *conn = gattConnFindByL2cConn(who);
    struct GattEnqueuedWrite *qw, *t;
    uint64_t totalBufSz;

    if (!conn)
        return ATT_ERROR_INVALID_HANDLE;

    totalBufSz = conn->queuedLength;
    qw = conn->queuedWrites;

    // verify buffering limits are met
    totalBufSz += len;
    if (totalBufSz >= GATT_SRV_PREPARE_WRITE_MAX_BYTES)
        return ATT_ERROR_PREPARE_QUEUE_FULL;

    // see if this is a continuation of an existing write and if so, append to it
    for (t = qw; t && (t->rangeRef != rangeRef || t->handleOfst != handleOfst); t = t->next);
    if (t) {
        // gaps arent really allowed when doing long writes in GATT, nor are overlaps
        if (sgLength(t->data) != byteOfst)
            return ATT_ERROR_INVALID_OFFSET;
        // also, attribute max len is 65536 bytes
        if (byteOfst + len > 0x10000)
            return ATT_ERROR_INVALID_OFFSET;
        // append
        if (!sgConcatBackCopy(t->data, data, len))
            return ATT_ERROR_UNLIKELY_ERROR;
        // account for it
        conn->queuedLength += len;
        return ATT_ERROR_NONE;
    }

    // new entry - verify sanity (GATT long write must start at zero)
    if (byteOfst)
        return ATT_ERROR_INVALID_OFFSET;

    // new entry - allocate it & data
    t = calloc(1, sizeof(struct GattEnqueuedWrite));
    if (!t)
        return ATT_ERROR_INSUFFICIENT_RSRCS;

    t->data = sgNewWithCopyData(data, len);
    if (!t->data) {
        free(t);
        return ATT_ERROR_INSUFFICIENT_RSRCS;
    }

    // fill in details
    t->rangeRef = rangeRef;
    t->handleOfst = handleOfst;

    // attach it & update totals
    t->next = qw;
    conn->queuedWrites = t;
    conn->queuedLength += len;

    return ATT_ERROR_NONE;
}

/*
 * FUNCTION: gattAttSvrWriteCbk
 * USE:      ATT calls this to write an attribute
 * PARAMS:   who - who the remote is
 *           rangeRef - the range this is related to
 *           cid - the connection Id to pass back to ATT
 *           handleOfst - the value in the range
 *           transId - transaction ID
 *           byteOfst - how mnay bytes into the characteristic we want to write
 *           reason - read reason (ATT_WRITE_FOR_*)
 *           len - bytes to write
 *           data - the data
 * RETURN:   false on immediate failure
 * NOTES:    when write completes, a call to attSrvCbkReply() should be made
 */
static bool gattAttSvrWriteCbk(l2c_handle_t who, att_cid_t cid, att_range_t rangeRef, uint16_t handleOfst, att_trans_t transId, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    /* attributes GATT creates internally for the user {service headers, includes, characteristic headers} are not writeable. All write pass to user */
    bool replyEvenIfNoErr = false;
    uint8_t err = ATT_ERROR_NONE;
    struct gattConnState *conn;
    struct gattService *svc;
    uint16_t handle = handleOfst + attSrvHandleRangeGetBase(rangeRef);

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(who);

    // remember that our gatt server has been used by this connection
    if (conn)
        conn->hadRemoteUse = true;

    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc) {
        logw("Failed to find service for read\n");
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    }

    if (reason == ATT_WRITE_FOR_PREPARE) {
        err = gattAttSrvProcessPrepareWriteReq(who, cid, rangeRef, handleOfst, byteOfst, len, data);
        replyEvenIfNoErr = true;
        goto out;
    }
    if (!gattEnqueueSrvWriteCall(svc, who, cid, transId, handle, byteOfst, reason, len, data)) {
        err = ATT_ERROR_INSUFFICIENT_RSRCS;
        goto out;
    }

out:
    if (err != ATT_ERROR_NONE || replyEvenIfNoErr)
        attSrvCbkReply(cid, transId, handle, err, data, len);
    pthread_mutex_unlock(&mGattLock);
    return true;
}

/*
 * FUNCTION: gattAttExecWriteCbk
 * USE:      ATT calls this when client asks us to execute a write
 * PARAMS:   who - who the remote is
 *           cid - the connection Id to pass back to ATT
 *           transId - the transaction ID
 *           exec - execute or dump data?
 * RETURN:   NONE
 * NOTES:
 */
static bool gattAttExecWriteCbk(l2c_handle_t who, att_cid_t cid, att_trans_t transId, bool exec)
{
    struct GattEnqueuedWrite *qw = NULL;
    uint8_t err = ATT_ERROR_NONE;
    bool replyEvenIfOk = true;
    struct gattConnState *conn;
    struct gattWorkItem *wi;

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(who);

    if (!conn) {
        err = ATT_ERROR_UNLIKELY_ERROR;
        goto out;
    }

    // remember that our gatt server has been used by this connection
    conn->hadRemoteUse = true;

    // if there are writes queued, detach them from the list
    qw = conn->queuedWrites;
    conn->queuedWrites = NULL;

    // if the request was to cancel, and we have writes queued delete them and reply; if request was to apply, and no writes were queued just reply
    if (!exec || !qw)
        goto out;

    // we only get this far if we had queued writes and we were asked to perform them - enqueue a work queue item to do that and only reply in case of failure to do so
    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (!wi) {
        err = ATT_ERROR_INSUFFICIENT_RSRCS;
        goto out;
    }

    wi->type = GATT_WORK_SRV_WRITE_EXEC;
    wi->srvWriteExec.who = who;
    wi->srvWriteExec.cid = cid;
    wi->srvWriteExec.q = qw;
    wi->srvWriteExec.transId = transId;

    if (workQueuePut(mWorkQ, wi)) {
        replyEvenIfOk = false;
        qw = NULL;
    }
    else {
        err = ATT_ERROR_UNLIKELY_ERROR;
        free(wi);
    }

out:
    gattQueuedWriteQueueFree(qw);

    pthread_mutex_unlock(&mGattLock);
    if (err != ATT_ERROR_NONE || replyEvenIfOk)
        attSrvCbkReply(cid, transId, 0, err, NULL, 0);
    return true;
}

/*
 * FUNCTION: gattMtuAdjust
 * USE:      Set MTU as per negotiation
 * PARAMS:   gatt - the gatt connection state
 *           mtu - the mtu to set
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattMtuAdjust(struct gattConnState* gatt, uint32_t mtu)
{
    if (gatt->mtu > mtu)
        loge("MTU decrease not allowed\n");
    else
        gatt->mtu = mtu;
}

/*
 * FUNCTION: gattAttCliMtuCbk
 * USE:      ATT calls this with results of MTU negotiation (if we are client)
 * PARAMS:   who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttCliMtuCbk(l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(who);
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        if (!todo || todo->type != GATT_CLI_TODO_SEND_MTU_REQ || todo->purpose != GATT_INTENT_MTU_XCHG)
            logw("Unexpected MTU reply\n");
        else {
            gattMtuAdjust(conn, mtu);
        }

        conn->todoCurr = NULL;
        gattCliTodoItemFree(todo);
        gattEnqueueCliDoNextTodoItem(conn);
    }
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttSrvMtuCbk
 * USE:      ATT calls this with results of MTU negotiation (if we are server)
 * PARAMS:   who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvMtuCbk(l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState* state;

    pthread_mutex_lock(&mGattLock);
    state = gattConnFindByL2cConn(who);
    if (!state)
        loge("Unknown connection for MTU change event\n");
    else
        gattMtuAdjust(state, mtu);
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttEdrMtuCbk
 * USE:      ATT calls this with results of MTU negotiation (on EDR)
 * PARAMS:   who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttEdrMtuCbk(l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState* state;

    pthread_mutex_lock(&mGattLock);
    state = gattConnFindByL2cConn(who);
    if (!state)
        loge("Unknown connection for MTU change event\n");
    else
        gattMtuAdjust(state, mtu);
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattNotifyServersOfConnClose
 * USE:      Tell all GATT servers that a connection closed
 * PARAMS:   who - the l2c conn handle for the connection
 * RETURN:   NONE
 * NOTES:
 */
static void gattNotifyServersOfConnClose(l2c_handle_t who)
{
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    for (svc = mSvcs; svc; svc = svc->next) {
        if (!svc->online)
            continue;
        if (!gattEnqueueSrvDiscCall(svc, who))
            loge("cannot notify server of connection closure\n");
    }
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattNotifyAllClientsOfConn
 * USE:      Tell all those waiting for a connection open/close that it happened
 * PARAMS:   inst - per-connection instance state (struct gattConnState)
 *           state - the state to notify with
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattNotifyAllClientsOfConn(struct gattConnState *inst, uint8_t state)
{
    struct gattClientConnection *clicon;

    for (clicon = mClientConns; clicon; clicon = clicon->next) {
        if (clicon->gattConnId != inst->gattConnId)
            continue;
        logd("telling client with gatt conn "GATTHANDLEFMT" about connection open status %u\n", GATTHANDLECNV(clicon->connId), state);
        gattEnqueueCliConnStatusCall(clicon, state);
    }
}

/*
 * FUNCTION: gattConnEvt
 * USE:      L2C calls this with events about connections
 * PARAMS:   userData - unused
 *           instance - per-connection instance state (struct gattConnState)
 *           evt - what event happened
 *           data - the data pertinent to the event
 *           len - the length of said data
 * RETURN:   NONE
 * NOTES:
 */
static void gattConnEvt(void *userData, void *instance, uint8_t evt, const void *data, uint32_t len)
{
    struct gattConnState *state = (struct gattConnState*)instance;
    l2c_handle_t conn;
    uint16_t mtu;
    sg s;

    switch (evt) {
    case L2C_STATE_OPEN:
        if (len != sizeof(l2c_handle_t)) {
            loge("invalid length for open event\n");
            break;
        }
        conn = *(l2c_handle_t*)data;
        pthread_mutex_lock(&mGattLock);
        if (state->state == GATT_CONN_STATE_CONNECTING) { // outbound conenction success

            state->conn = conn;
            logd("GATT outbound connection to "ADDRFMT" success: "HANDLEFMT"\n", ADDRCONV(state->addr), HANDLECNV(conn));
            state->state = GATT_CONN_STATE_RUNNING;
            if (BT_ADDR_IS_LE(state->addr)) { // try to negotiate mtu if LE
                logd("Requesting gatt MTU negotiation\n");
                if (!gattCliTodoMakeMtuReq(state, L2C_MAX_SAFE_MTU, GATT_INTENT_MTU_XCHG))
                    logw("Failed to make MTU request change todo - ATT may be slow\n");
            }
            gattNotifyAllClientsOfConn(state, GATT_CLI_STATUS_OK);
        }
        else if (state->state != GATT_CONN_STATE_RUNNING)
            loge("GATT L2C connection state 'opened' while in state %u\n", state->state);
        else if (state->conn != conn)
            loge("GATT L2C conn handle change is unexpected for inbound connections!!!\n");
        pthread_mutex_unlock(&mGattLock);
        break;

    case L2C_STATE_MTU:
        if (len != sizeof(uint16_t)) {
            loge("invalid length for mtu event\n");
            break;
        }
        mtu = *(uint16_t*)data;
        attSetMtu(state->conn, mtu);
        gattAttEdrMtuCbk(state->conn, mtu);
        break;

    case L2C_STATE_ENCR:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encr event\n");
            break;
        }
        attEncrAuthUpdate(state->conn, ((struct l2cEncrState*)data)->isEncr, ((struct l2cEncrState*)data)->isMitmSafe);
        // TODO: tell gatt clients & servers
        break;

    case L2C_STATE_ENCR_KEY_REF:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encr key event\n");
            break;
        }
        attEncrAuthUpdate(state->conn, ((struct l2cEncrState*)data)->isEncr, ((struct l2cEncrState*)data)->isMitmSafe);
        break;

    case L2C_STATE_RX:
        if (len != sizeof(sg)) {
            loge("invalid length for RX event\n");
            break;
        }
        s = *(sg*)data;
        attSrvDataRx(state->conn, s);
        break;

    case L2C_STATE_CLOSED:
        gattNotifyServersOfConnClose(state->conn);
        attNotifConnClose(state->conn);
        pthread_mutex_lock(&mGattLock);
        gattNotifyAllClientsOfConn(state, GATT_CLI_STATUS_OTHER_SIDE_DISC);
        sendQueueDelPackets(mSendQ, state->conn);
        gattConnStructDelete(state);
        pthread_mutex_unlock(&mGattLock);
        break;
    default:
        logd("unknown L2C event %u\n", evt);
        break;
    }
}

/*
 * FUNCTION: gattStateAllocLocked
 * USE:      Allocate a GATT state for an L2C link
 * PARAMS:   conn - the connection handle (0 if none yet)
 *           instanceP - we store per-connection instance here
 *           addr - the peer's address
 *           initialState - state to leave the connection in
 * RETURN:   SVC_ALLOC_*
 * NOTES:    call with mGattLock held
 */
static uint8_t gattStateAllocLocked(l2c_handle_t conn, void **instanceP, const struct bt_addr *addr, uint8_t initialState)
{
    struct gattConnState *state = (struct gattConnState*)calloc(1, sizeof(struct gattConnState));

    if (!state)
        return SVC_ALLOC_FAIL_OTHER;

    state->conn = conn;
    state->state = initialState;
    state->gattConnId = uniqGetNext();
    state->mtu = BT_ADDR_IS_LE(*addr) ? ATT_MTU_MIN_LE : ATT_MTU_MIN_EDR;
    memcpy(&state->addr, addr, sizeof(struct bt_addr));
    /* any other init here */

    state->next = mConns;
    if (mConns)
        mConns->prev = state;
    mConns = state;

    *instanceP = state;
    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: gattStateAlloc
 * USE:      Allocate a GATT state for an L2C link
 * PARAMS:   conn - the connection handle (0 if none yet)
 *           instanceP - we store per-connection instance here
 *           addr - the peer's address
 *           initialState - state to leave the connection in
 * RETURN:   SVC_ALLOC_*
 * NOTES:    just a version of gattStateAllocLocked that takes the lock as needed
 */
static uint8_t gattStateAlloc(l2c_handle_t conn, void **instanceP, const struct bt_addr *addr, uint8_t initialState)
{
    uint8_t ret;

    pthread_mutex_lock(&mGattLock);
    ret = gattStateAllocLocked(conn, instanceP, addr, initialState);
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattPsmSvcAlloc
 * USE:      L2C calls this with remote requests to open a PSM-based channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:    if allocate is called, we are being connected to. in that case go right to
 *           GATT_CONN_STATE_RUNNING state
 */
static uint8_t gattPsmSvcAlloc(void *userData, l2c_handle_t handle, void **instanceP)
{
    struct bt_addr peer;

    /* if we cannot get peer address, do not bother with anything else */
    if (!l2cApiGetBtAddr(handle, &peer))
        return SVC_ALLOC_FAIL_OTHER;

    /* we do not support PSM access to GATT over LE */
    if (BT_ADDR_IS_LE(peer))
        return SVC_ALLOC_FAIL_OTHER;

    return gattStateAlloc(handle, instanceP, &peer, GATT_CONN_STATE_RUNNING);
}

/*
 * FUNCTION: gattFixedChSvcAlloc
 * USE:      L2C calls this with requests to open a fixed channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           isAclMaster - whether we are the master in the underlying ACL
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:    if allocate is called, we are being connected to. in that case go right to
 *           GATT_CONN_STATE_RUNNING state
 */
static uint8_t gattFixedChSvcAlloc(void *userData, l2c_handle_t handle, bool isAclMaster, void **instanceP)
{
    struct bt_addr peer;

    /* if we cannot get peer address, do not bother with anything else */
    if (!l2cApiGetBtAddr(handle, &peer))
        return SVC_ALLOC_FAIL_OTHER;

    /* we do not support FixedCh access to GATT over EDR */
    if (BT_ADDR_IS_EDR(peer))
        return SVC_ALLOC_FAIL_OTHER;

    return gattStateAlloc(handle, instanceP, &peer, GATT_CONN_STATE_RUNNING);
}

/*
 * FUNCTION: gattConnectionCreateOrLocate
 * USE:      Make a new gatt L2C connection or find an existing one
 * PARAMS:   to - whom to connect to
 * RETURN:   connection struct or NULL
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattConnectionCreateOrLocate(const struct bt_addr *to)
{
    struct gattConnState *gattConn;
    bool connTrying;

    gattConn = gattConnFindByAddr(to);
    if (!gattConn) {

        if (gattStateAllocLocked(0, (void**)&gattConn, to, GATT_CONN_STATE_CONNECTING) != SVC_ALLOC_SUCCESS)
            logw("Failed to allocate a gatt connection instance for an outbound GATT connection to "ADDRFMT"\n", ADDRCONV(*to));
        else {

            if (BT_ADDR_IS_EDR(*to))
                connTrying = l2cApiCreatePsmConnection(GATT_PSM, to, L2C_MAX_SAFE_MTU, gattConnEvt, NULL, gattConn);
            else
                connTrying = l2cApiCreateFixedChConnection(GATT_FIXEDCH, to, gattConnEvt, NULL, gattConn);

            if (!connTrying) {
                logw("Failed to attempt to establish an outbound GATT connection to "ADDRFMT"\n", ADDRCONV(*to));
                gattConnStructDelete(gattConn);
                gattConn = NULL;
            }
        }
    }

    return gattConn;
}

/*
 * FUNCTION: gattClientConnect
 * USE:      Make a new gatt client connection
 * PARAMS:   userData - caller's data which will be given with the callback
 *           to - whom to connect to
 *           resultCbk - will be called with results
 * RETURN:   connection id or 0 on failure
 * NOTES:    the callback might be called before this func returnes (talk
 *           to your scheduler about that). This leaves you with a conundrum:
 *           How will you know the callback is for you when you do not yet
 *           know the connection id. Caller solves this themselves. eg: take
 *           a mutex in callback, and also take it before calling this, do
 *           not release it till this func returns.
 */
gatt_client_conn_t gattClientConnect(void *userData, const struct bt_addr *to,
                                     gattCliConnectResultCbk resultCbk)
{
    struct gattClientConnection *clicon;
    struct gattConnState *inst;
    gatt_client_conn_t ret = 0;

    clicon = calloc(1, sizeof(struct gattClientConnection));
    if (!clicon) {
        loge("Out of memory allocating a client connection for GATT\n");
        return 0;
    }

    pthread_mutex_lock(&mGattLock);
    inst = gattConnectionCreateOrLocate(to);
    if (!inst) {
        logw("Failed to create an outbound GATT connection. Client connection process will end\n");
        goto out;
    }

    inst->numLocalUsers++;
    clicon->next = mClientConns;
    if (mClientConns)
        mClientConns->prev = clicon;
    mClientConns = clicon;

    clicon->userData = userData;
    clicon->gattConnId = inst->gattConnId;
    clicon->connId = uniqGetNext();
    clicon->connCbk = resultCbk;

    // call callback right away?
    if (inst->state == GATT_CONN_STATE_RUNNING)
        gattEnqueueCliConnStatusCall(clicon, GATT_CLI_STATUS_OK);

    // success!
    ret = clicon->connId;
    pthread_mutex_unlock(&mGattLock);
    return ret;

out:
    pthread_mutex_unlock(&mGattLock);
    free(clicon);
    return 0;
}

/*
 * FUNCTION: gattWorkQueueFreeItem
 * USE:      Called to free a workQ item at workQ delete time
 * PARAMS:   item - the work item
 * RETURN:   NONE
 * NOTES:
 */
static void gattWorkQueueFreeItem(void *item)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)item;

    gattWorkItemFree(wi);
}

/*
 * FUNCTION: gattSrvCbkReply
 * USE:      Called from higher layer as a reply to us calling for a read/write
 * PARAMS:   cid - conenction ID from ATT
 *           transId - transaction ID from ATT
 *           handle - handle on which an error occured (unused for read & write)
 *           err - ATT_ERROR_*
 *           data - the read data
 *           len - the read len
 * RETURN:   unused
 * NOTES:
 */
void gattSrvCbkReply(att_cid_t cid, att_trans_t transId, uint16_t handle, uint8_t err, const void *data, uint16_t len)
{
    attSrvCbkReply(cid, transId, handle, err, data, len);
}

/*
 * FUNCTION: gattCliTodoItemFree
 * USE:      Properly free a todo item for GATT
 * PARAMS:   todo - the todo item to free
 * RETURN:   NONE
 * NOTES:
 */
static void gattCliTodoItemFree(struct gattCliTodoItem *todo)
{
    if (todo->data) // data may be NULL if the item had aleady been processed
        sgFree(todo->data);
    free(todo);
}


/*
 * FUNCTION: gattSrvDoOneQueuedWrite
 * USE:      Perform a queued write as the server. Take locks as needed
 * PARAMS:   who - the L2C cnnection
 *           cid - the att connection id
 *           rangeRef - the ATT handle range
 *           handleOfst - the offset of the handle in that range
 *           transId - ATT transaction ID (for reply)
 *           data - the data
 * RETURN:   ATT_ERROR_*
 * NOTES:    called from gatt worker thread. take locks as needed
 */
static uint8_t gattSrvDoOneQueuedWrite(l2c_handle_t who, att_cid_t cid, att_range_t rangeRef, uint16_t handleOfst, att_trans_t transId, sg data)
{
    uint16_t handle = handleOfst + attSrvHandleRangeGetBase(rangeRef);
    uint32_t len = sgLength(data);
    uint8_t err = ATT_ERROR_NONE;
    struct gattService *svc;
    void *buf = NULL;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc) {
        logw("Failed to find service for queued write\n");
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    }

    // we need to serialize the data here temporarily
    buf = malloc(len);
    if (!buf) {
        err = ATT_ERROR_INSUFFICIENT_RSRCS;
        goto out;
    }

    if (len != sgSerialize(data, 0, len, buf)) {
        err = ATT_ERROR_UNLIKELY_ERROR;
        goto out;
    }

    if (!svc->writeF(svc->svcRef, who, cid, transId, handle, 0, ATT_WRITE_FOR_PREPARE, len, buf)) {
        err = ATT_ERROR_UNLIKELY_ERROR;
        goto out;
    }

    //success? free data
    sgFree(data);

out:
    if (buf)
        free(buf);
    pthread_mutex_unlock(&mGattLock);
    return err;

}

/*
 * FUNCTION: gattSrvQueuedWritesPerformFromWorkerThread
 * USE:      Perform queued writes as the server. Take locks as needed. Send reply either way
 * PARAMS:   who - the L2C cnnection
 *           cid - the att connection id
 *           qw - writes currently queued that we need to execute
 *           transId - ATT transaction ID (for reply)
 * RETURN:   NONE
 * NOTES:    called from gatt worker thread. set "->data" to NULL if sg has been used or handed off
 */
static void gattSrvQueuedWritesPerformFromWorkerThread(l2c_handle_t who, att_cid_t cid, struct GattEnqueuedWrite *qw, att_trans_t transId)
{
    uint8_t err = ATT_ERROR_NONE;

    while (qw) {
        err = gattSrvDoOneQueuedWrite(who, cid, qw->rangeRef, qw->handleOfst, transId, qw->data);
        if (err != ATT_ERROR_NONE)
            break;

        qw->data = NULL;
        qw = qw->next;
    }

    //reply with err...
    attSrvCbkReply(cid, transId, qw ? qw->handleOfst + attSrvHandleRangeGetBase(qw->rangeRef) : 0, err, NULL, 0);
}

/*
 * FUNCTION: gattWorkItemFree
 * USE:      Properly free a work item for GATT
 * PARAMS:   wi - the work item to free
 * RETURN:   NONE
 * NOTES:
 */
static void gattWorkItemFree(struct gattWorkItem *wi)
{
    if (wi->type == GATT_WORK_CLI_DO_NEXT && wi->cliDoNext.todo)
        gattCliTodoItemFree(wi->cliDoNext.todo);
    else if (wi->type == GATT_WORK_CLI_IND_CALL && wi->cliIndCall.data)
        sgFree(wi->cliIndCall.data);
    else if (wi->type == GATT_WORK_SRV_WRITE_EXEC)
        gattQueuedWriteQueueFree(wi->srvWriteExec.q);

    free(wi);
}

/*
 * FUNCTION: gattWorker
 * USE:      Worker thread for calls to higher layer
 * PARAMS:   param - unused
 * RETURN:   unused
 * NOTES:
 */
static void* gattWorker(void *param)
{
    struct gattWorkItem *wi;
    int status;

    pthread_setname_np(pthread_self(), "gatt_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case GATT_WORK_SRV_READ_CALL:
            if (!wi->srvRwCall.readF(wi->srvRwCall.svcRef, wi->srvRwCall.who, wi->srvRwCall.cid, wi->srvRwCall.transId, wi->srvRwCall.handle, wi->srvRwCall.byteOfst, wi->srvRwCall.reason, wi->srvRwCall.len))
                gattSrvCbkReply(wi->srvRwCall.cid, wi->srvRwCall.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;
        case GATT_WORK_SRV_WRITE_CALL:
            if (!wi->srvRwCall.writeF(wi->srvRwCall.svcRef, wi->srvRwCall.who, wi->srvRwCall.cid, wi->srvRwCall.transId, wi->srvRwCall.handle, wi->srvRwCall.byteOfst, wi->srvRwCall.reason, wi->srvRwCall.len, wi + 1))
                gattSrvCbkReply(wi->srvRwCall.cid, wi->srvRwCall.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;
        case GATT_WORK_SRV_IND_CALL:
            wi->srvIndCall.indF(wi->srvIndCall.svcRef, wi->srvIndCall.who, wi->srvIndCall.cid, wi->srvIndCall.handle, wi->srvIndCall.evt, wi->srvIndCall.ref);
            break;
        case GATT_WORK_SRV_DISC_CALL:
            wi->srvDiscCall.discF(wi->srvDiscCall.svcRef, wi->srvDiscCall.who);
            break;
        case GATT_WORK_SRV_WRITE_EXEC:
            gattSrvQueuedWritesPerformFromWorkerThread(wi->srvWriteExec.who, wi->srvWriteExec.cid, wi->srvWriteExec.q, wi->srvWriteExec.transId);
            break;
        case GATT_WORK_CLI_CONN_STATE_NOTIF:
            wi->cliConnStateNotif.cbk(wi->cliConnStateNotif.userData, wi->cliConnStateNotif.connId, wi->cliConnStateNotif.status);
            break;
        case GATT_WORK_CLI_DO_NEXT:
            gattCliTodoNext(wi->cliDoNext.conn, wi->cliDoNext.todo);
            wi->cliDoNext.todo = NULL; // do not auto-free it
            break;
        case GATT_WORK_CLI_IND_CALL:
            wi->cliIndCall.cbk(wi->cliIndCall.conn, wi->cliIndCall.handle, wi->cliIndCall.ind, wi->cliIndCall.data);
            wi->cliIndCall.data = NULL; // do not auto-free it
            break;
        case GATT_WORK_CLI_SUB_UNSUB_CALL:
            wi->cliSubUnsubCall.cbk(wi->cliSubUnsubCall.conn, wi->cliSubUnsubCall.handle, wi->cliSubUnsubCall.forSub, wi->cliSubUnsubCall.stat);
            break;
        default:
            loge("Unknown work type %d\n", wi->type);
            break;
        }
        gattWorkItemFree(wi);
    }

    logd("GATT worker exiting\n");
    return NULL;
}

/*
 * FUNCTION: gattAttCbkCheckState
 * USE:      Common code for all GATT ATT client callbacks. Some basic checks mostly
 * PARAMS:   trans - transaction id (or 0 if none)
 *           to - peer's connection handle
 *           expectedWorkItemTypes - list of acceptable work item types (NULL if we have no expectations)
 *           numExpTypes - size of the above list
 *           cbkName - name (used for message printing only)
 * RETURN:   gattConnState if all is OK, NULL else
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattAttCbkCheckState(uniq_t trans, l2c_handle_t to, const uint8_t *expectedWorkItemTypes, unsigned numExpTypes, const char* cbkName)
{
    struct gattConnState *conn;
    unsigned i;

    conn = gattConnFindByL2cConn(to);
    if (!conn)
        logw("%s callback on a nonexistent connection\n", cbkName);
    else if (!conn->todoCurr)
        logw("%s callback with no todo item\n", cbkName);
    else {
        if (!numExpTypes)
            return conn;
        for (i = 0; i < numExpTypes; i++)
            if (*expectedWorkItemTypes++ == conn->todoCurr->type)
                return conn;
        logw("%s callback with wrong work type (%u)\n", cbkName, conn->todoCurr->type);
    }

    return NULL;
}

/*
 * FUNCTION: gattAttCliFindInfoCbk
 * USE:      Called by ATT as a reply to our Find request, if results were provided
 * PARAMS:   trans - transaction id
 *           to - peer's connection handle
 *           handle - the handle that was found
 *           uuid - the uuid that was found
 *           haveMore - will there be more calls to this func from the same ATT reply packet?
 * RETURN:   false to receive no more callbacks
 * NOTES:
 */
static bool gattAttCliFindInfoCbk(uniq_t trans, l2c_handle_t to, uint16_t handle, const struct uuid *uuid, bool haveMore)
{
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_FIND_INFO};
    bool killItem = true, ret = false;
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Read");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;
        bool done = false;

        switch (conn->todoCurr->purpose) {
        case GATT_INTENT_ENUM_CHAR_DESCRS:
            conn->doNotFreeTodoCurr = true;
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            todo->enumCharDscrsCbk(todo->connId, todo->clientTrans, uuid, handle, GATT_CLI_STATUS_OK);
            // if this is the end - signal that now
            if (handle == todo->endHandle) {
                todo->enumCharDscrsCbk(todo->connId, todo->clientTrans, NULL, 0, GATT_CLI_STATUS_OK);
                done = true;
            }
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->doNotFreeTodoCurr = false;
            else {
                gattCliTodoItemFree(todo);
                logd("Connection went away while processing\n");
            }
            if (conn && !done) {
                ret = true;
                killItem = false;
                todo->handle = handle + 1;
                if (!haveMore)
                    gattCliTodoEnqItem(conn, todo, false);
            }
            break;

        default:
            loge("Unknown reason for getting a find result\n");
            break;
        }

        if (conn) {
            if (!ret || !haveMore) { // either way we expect no more calls from ATT about this item
                conn->todoCurr = NULL;
                if (killItem)
                    gattCliTodoItemFree(todo);
            }
            else if (killItem)
                loge("Cannot kill todo item if we expect more callbacks on it!\n");
            gattEnqueueCliDoNextTodoItem(conn);
        }
    }

    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattAttCliFindByTypeValCbk
 * USE:      Called by ATT as a reply to our FindByTypeValue request, if results were provided
 * PARAMS:   trans - transaction id
 *           to - peer's connection handle
 *           handle - the handle that was found
 *           grpEndHandle - group end handle that was found
 *           haveMore - will there be more calls to this func from the same ATT reply packet?
 * RETURN:   false to receive no more callbacks
 * NOTES:
 */
static bool gattAttCliFindByTypeValCbk(uniq_t trans, l2c_handle_t to, uint16_t handle, uint16_t grpEndHandle, bool haveMore)
{
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_FIND_BY_TYPE_VAL};
    bool killItem = true, ret = false;
    struct uuid tempUuid;
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "FindByTypeVal");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        switch (conn->todoCurr->purpose) {
        case GATT_INTENT_FIND_SVC:
            uuidFromUuid16(&tempUuid, GATT_UUID_SVC_PRIMARY);
            conn->doNotFreeTodoCurr = true;
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            todo->enumSvcsCbk(todo->userData, todo->connId, todo->clientTrans, &todo->desiredUuid, uuidCmp(&tempUuid, &todo->uuid), handle, grpEndHandle - handle + 1, GATT_CLI_STATUS_OK);
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->doNotFreeTodoCurr = false;
            else {
                gattCliTodoItemFree(todo);
                logd("Connection went away while processing\n");
            }
            ret = true;
            killItem = false;
            break;
        default:
            loge("Unknown reason for getting a find result\n");
            break;
        }

        if (conn) {
            if (!ret || !haveMore) { // either way we expect no more calls from ATT about this item
                conn->todoCurr = NULL;
                if (killItem)
                    gattCliTodoItemFree(todo);
            }
            else if (killItem)
                loge("Cannot kill todo item if we expect more callbacks on it!\n");
            gattEnqueueCliDoNextTodoItem(conn);
        }
    }

    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattAttCliReadCbk
 * USE:      Called by ATT as a reply to our Read/ReadBlob/ReadByType/ReadByGroupType request, if results were provided
 * PARAMS:   trans - transaction id
 *           to - peer's connection handle
 *           handleP - points to the handle that was found or NULL if none provided for this type of read frequest
 *           endGrpHandleP - points to the group end handle that was found or NULL if none provided for this type of read frequest
 *           data - the data that was read
 *           haveMore - will there be more calls to this func from the same ATT reply packet?
 * RETURN:   false to receive no more callbacks
 * NOTES:
 */
static bool gattAttCliReadCbk(uniq_t trans, l2c_handle_t to, const uint16_t *handleP, const uint16_t *endGrpHandleP, sg data, bool haveMore)
{
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_READ_BY_TYPE, GATT_CLI_TODO_READ, GATT_CLI_TODO_READ_BLOB, GATT_CLI_TODO_READ_BY_GRP_TYPE};
    bool killItem = true, ret = false;
    struct gattIncludeDefHdr inclDef;
    struct gattCharDefHdr charDef;
    struct uuid uuid, tempUuid;
    struct gattConnState *conn;
    uint16_t tmpHandle;
    bool done = false;


    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Read");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        switch (todo->purpose) {
        case GATT_INTENT_ENUM_SVCS:
            ret = attReadUuid(&uuid, data);
            sgFree(data);
            if (!ret) {
                logw("Read for enum services did not produce a uuid\n");
                break;
            }
            uuidFromUuid16(&tempUuid, GATT_UUID_SVC_PRIMARY);
            conn->doNotFreeTodoCurr = true;
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            todo->enumSvcsCbk(todo->userData, todo->connId, todo->clientTrans, &uuid, uuidCmp(&tempUuid, &todo->uuid), *handleP, *endGrpHandleP - *handleP + 1, GATT_CLI_STATUS_OK);
            // if this is the end - signal that now
            if (*endGrpHandleP == ATT_LAST_VALID_HANDLE) {
                todo->enumSvcsCbk(todo->userData, todo->connId, todo->clientTrans, NULL, uuidCmp(&tempUuid, &todo->uuid), 0, 0, GATT_CLI_STATUS_OK);
                done = true;
            }
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->doNotFreeTodoCurr = false;
            else {
                gattCliTodoItemFree(todo);
                logd("Connection went away while processing\n");
            }
            if (conn && !done) {
                ret = true;
                killItem = false;
                todo->handle = *endGrpHandleP + 1;
                if (!haveMore)
                    gattCliTodoEnqItem(conn, todo, false);
            }
            break;

        case GATT_INTENT_ENUM_CHARS:
            if (!sgSerializeCutFront(data, &charDef, sizeof(charDef)))
                logw("Invalid char def\n");
            else if (!attReadUuid(&uuid, data))
                logw("Char definition does not end in a uuid\n");
            else {
                tmpHandle = utilGetLE16(&charDef.valHandle);
                logv("Found char @ 0x%04x with props 0x%02x & val @ 0x%04x: "UUIDFMT"\n", *handleP, utilGetLE8(&charDef.charProps), tmpHandle, UUIDCONV(uuid));

                conn->doNotFreeTodoCurr = true;
                pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                todo->enumCharsCbk(todo->connId, todo->clientTrans, &uuid, *handleP, tmpHandle, utilGetLE8(&charDef.charProps), GATT_CLI_STATUS_OK);
                // if this is the end - signal that now
                if (tmpHandle >= todo->endHandle) {
                    todo->enumCharsCbk(todo->connId, todo->clientTrans, NULL, 0, 0, 0, GATT_CLI_STATUS_OK);
                    done = true;
                }
                pthread_mutex_lock(&mGattLock);
                conn = gattConnFindByL2cConn(to);
                if (conn)
                    conn->doNotFreeTodoCurr = false;
                else {
                    gattCliTodoItemFree(todo);
                    logd("Connection went away while processing\n");
                }
                if (conn && !done) {
                    ret = true;
                    killItem = false;
                    todo->handle = tmpHandle + 1;
                    if (!haveMore)
                        gattCliTodoEnqItem(conn, todo, false);
                }
            }
            sgFree(data);
            break;

        case GATT_INTENT_ENUM_INCL_SVCS:
            if (todo->type == GATT_CLI_TODO_READ_BY_TYPE) { // an include definition
                if (!sgSerializeCutFront(data, &inclDef, sizeof(inclDef)))
                    logw("Invalid include def\n");
                else if (sgLength(data) == sizeof(uint16_t)) { // have uuid16 - report it
                    attReadUuid(&uuid, data);

                    // call up to report found include
                    conn->doNotFreeTodoCurr = true;
                    pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                    todo->enumIncludedSvcsCbk(todo->connId, todo->clientTrans, &uuid, *handleP, utilGetLE16(&inclDef.startHandle), utilGetLE16(&inclDef.endHandle), GATT_CLI_STATUS_OK);
                    // if this is the end - signal that now
                    if (*handleP >= todo->endHandle) {
                        done = true;
                        todo->enumIncludedSvcsCbk(todo->connId, todo->clientTrans, NULL, 0, 0, 0, GATT_CLI_STATUS_OK);
                    }
                    pthread_mutex_lock(&mGattLock);
                    conn = gattConnFindByL2cConn(to);
                    if (conn)
                        conn->doNotFreeTodoCurr = false;
                    else {
                        gattCliTodoItemFree(todo);
                        logd("Connection went away while processing\n");
                    }

                    if (conn && !done) {
                        ret = true;
                        killItem = false;
                        todo->handle = *handleP + 1;
                        if (!haveMore)
                            gattCliTodoEnqItem(conn, todo, false);
                    }
                }
                else if (sgLength(data))
                    logw("Invalid include def (uuid area)\n");
                else { // do not have uuit16 - read the uuid128 directly
                    todo->auxHandle[0] = *handleP; // save our iteration location
                    todo->auxHandle[1] = utilGetLE16(&inclDef.endHandle); // save group end location
                    todo->handle = utilGetLE16(&inclDef.startHandle);
                    todo->type = GATT_CLI_TODO_READ;
                    gattCliTodoEnqItem(conn, todo, false);
                    killItem = false;
                    // keep ret as false - we can only do one of these at a time
                }
            }
            else if (todo->type == GATT_CLI_TODO_READ) { // a service definition (to get a 128-bit uuid)
                if (!attReadUuid(&uuid, data))
                    logw("Cannot read the provided service definition uuid\n");
                else {
                    // call up to report now that we have all the info
                    conn->doNotFreeTodoCurr = true;
                    pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                    todo->enumIncludedSvcsCbk(todo->connId, todo->clientTrans, &uuid, todo->auxHandle[0], todo->handle, todo->auxHandle[1], GATT_CLI_STATUS_OK);
                    // if this is the end - signal that now
                    if (todo->auxHandle[0] >= todo->endHandle) {
                        done = true;
                        todo->enumIncludedSvcsCbk(todo->connId, todo->clientTrans, NULL, 0, 0, 0, GATT_CLI_STATUS_OK);
                    }
                    pthread_mutex_lock(&mGattLock);
                    conn = gattConnFindByL2cConn(to);
                    if (conn)
                        conn->doNotFreeTodoCurr = false;
                    else {
                        gattCliTodoItemFree(todo);
                        logd("Connection went away while processing\n");
                    }

                    if (conn && !done) {
                        // resume our initial inquiry from where we left off
                        todo->handle = todo->auxHandle[0] + 1;
                        todo->type = GATT_CLI_TODO_READ_BY_TYPE;
                        ret = true;
                        killItem = false;
                        gattCliTodoEnqItem(conn, todo, false);
                    }
                }
            }
            sgFree(data);
            break;

        case GATT_INTENT_READ:
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            todo->readCbk(todo->userData, todo->connId, todo->clientTrans, todo->handle, GATT_CLI_STATUS_OK, ATT_ERROR_NONE, data);
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            break;

        default:
            loge("Unknown reason for getting a read result. purpose = %u\n", todo->purpose);
            break;
        }

        if (conn) {
            if (!ret || !haveMore) { // either way we expect no more calls from ATT about this item
                conn->todoCurr = NULL;
                if (killItem)
                    gattCliTodoItemFree(todo);
            }
            else if (killItem)
                loge("Cannot kill todo item if we expect more callbacks on it!\n");
            gattEnqueueCliDoNextTodoItem(conn);
        }
    }

    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattAttCliWriteCbk
 * USE:      Called by ATT as a reply to our Write requests/commands and PrepareWrite
 * PARAMS:   trans - transaction id
 *           to - peer's connection handle
 * RETURN:   NONE
 * NOTES:    data from PrepareWrite reply is not provided.
 */
static void gattAttCliWriteCbk(uniq_t trans, l2c_handle_t to)
{
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_WRITE, GATT_CLI_TODO_PREPARE_WRITE, GATT_CLI_TODO_WRITE_EXEC, GATT_CLI_TODO_WRITE_CCCD};
    struct gattClientConnection *clicon;
    struct gattConnState *conn;
    struct gattCliNotif *notif;
    bool somethingDone;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Write");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        switch (todo->purpose) {
        case GATT_INTENT_NOTIF_SUB_UNSUB:
            notif = gattNotifFindByLocatorInConn(conn, todo->notifLocator);
            if (!notif)
                break;

            // update the notif rec's state
            switch (todo->offset) {
            default:
                logw("Weird unknown value written by us to CCCD: 0x%04x\n", todo->offset);
                // fallthrough
            case 0:
                notif->curState = NOTIF_C_STATE_NOT_SUBBED;
                break;
            case GATT_CLI_CFG_BIT_NOTIFS:
                notif->curState = NOTIF_C_STATE_SUBBED_TO_NOTIF;
                break;
            case GATT_CLI_CFG_BIT_INDS:
                notif->curState = NOTIF_C_STATE_SUBBED_TO_IND;
                break;
            }

            // tell everyone who needs to know - first iterate all client connections
            do {

                somethingDone = false;
                for (clicon = mClientConns; clicon && !somethingDone; clicon = clicon->next) {
                    struct gattCliNotifRec *nr;

                    // for each, iterate the notifications/indications requested
                    for (nr = clicon->notifs; nr; nr = nr->next) {

                        // for those applicable to this CCCD write, and in proper state, and not yet told we are subscribed, tell them
                        if (nr->notif == notif && !nr->subCallMade && ((nr->wantInd && notif->curState == NOTIF_C_STATE_SUBBED_TO_IND) || (notif->curState == NOTIF_C_STATE_SUBBED_TO_NOTIF))) {

                            gattCliNotifSubscribeStateCbk stateCbk = nr->stateCbk;

                            nr->subCallMade = true;
                            conn->doNotFreeTodoCurr = true;
                            pthread_mutex_unlock(&mGattLock);
                            stateCbk(clicon->connId, todo->auxHandle[0], true, GATT_CLI_STATUS_OK);
                            pthread_mutex_lock(&mGattLock);
                            conn->doNotFreeTodoCurr = false;
                            somethingDone = true;
                            conn = gattConnFindByL2cConn(to);
                            break;
                        }
                    }
                }
            } while (conn && somethingDone);
            break;

        case GATT_INTENT_WRITE:
            todo->writeCbk(todo->userData, todo->connId, todo->clientTrans, todo->handle, GATT_CLI_STATUS_OK, ATT_ERROR_NONE);
            conn = gattConnFindByL2cConn(to);
            break;

        default:
            loge("Unknown reason for getting a write result\n");
            break;
        }

        if (conn) {
            conn->todoCurr = NULL;
            gattCliTodoItemFree(todo);
            gattEnqueueCliDoNextTodoItem(conn);
        }
    }

    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttCliError
 * USE:      Called by ATT as a result of an error occuring
 * PARAMS:   trans - transaction id
 *           to - peer's connection handle
 *           handleP - points to the handle from the error message or NULL on local stack errors
 *           errP - points to the errno from the error message or NULL on local stack errors
 *           errOpcodeP - points to the error opcode from the error message or NULL on local stack errors
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttCliError(uniq_t trans, l2c_handle_t to, const uint16_t *handleP, const uint8_t *errP, const uint8_t *errOpcodeP)
{
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_FIND_INFO, GATT_CLI_TODO_FIND_BY_TYPE_VAL, GATT_CLI_TODO_READ_BY_TYPE, GATT_CLI_TODO_READ, GATT_CLI_TODO_READ_BLOB,
                                                GATT_CLI_TODO_READ_BY_GRP_TYPE, GATT_CLI_TODO_WRITE, GATT_CLI_TODO_PREPARE_WRITE, GATT_CLI_TODO_WRITE_EXEC, GATT_CLI_TODO_WRITE_CCCD};
    struct gattConnState *conn;
    bool handled = false;
    struct uuid tempUuid;

    logd("att signalled error on handle 0x%04x of type 0x%02x from opcode 0x%02x\n", handleP ? *handleP : -1, errP ? *errP : -1, errOpcodeP ? *errOpcodeP : -1);

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Error");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;
        conn->todoCurr = NULL;
        uint8_t status;

        switch (todo->purpose) {
        case GATT_INTENT_ENUM_SVCS:
            if (*errP == ATT_ERROR_ATTRIBUTE_NOT_FOUND ) { // end of enum (we could check handle, but even spec is conflicted on what it should be, so we do not)
                uuidFromUuid16(&tempUuid, GATT_UUID_SVC_PRIMARY);
                pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                todo->enumSvcsCbk(todo->userData, todo->connId, todo->clientTrans, NULL, uuidCmp(&tempUuid, &todo->uuid), 0, 0, GATT_CLI_STATUS_OK);
                pthread_mutex_lock(&mGattLock);
                conn = gattConnFindByL2cConn(to);
                if (!conn)
                    logd("Connection went away while processing\n");
                handled = true;
            }
            break;

        case GATT_INTENT_ENUM_INCL_SVCS:
            if (*errP == ATT_ERROR_ATTRIBUTE_NOT_FOUND) { // end of enum (we could check handle, but even spec is conflicted on what it should be, so we do not)
                pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                todo->enumIncludedSvcsCbk(todo->connId, todo->clientTrans, NULL, 0, 0, 0, GATT_CLI_STATUS_OK);
                pthread_mutex_lock(&mGattLock);
                conn = gattConnFindByL2cConn(to);
                if (!conn)
                    logd("Connection went away while processing\n");
                handled = true;
            }
            break;

        case GATT_INTENT_ENUM_CHARS:
            if (*errP == ATT_ERROR_ATTRIBUTE_NOT_FOUND) { // end of enum (we could check handle, but even spec is conflicted on what it should be, so we do not)
                pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                todo->enumCharsCbk(todo->connId, todo->clientTrans, NULL, 0, 0, 0, GATT_CLI_STATUS_OK);
                pthread_mutex_lock(&mGattLock);
                conn = gattConnFindByL2cConn(to);
                if (!conn)
                    logd("Connection went away while processing\n");
                handled = true;
            }
            break;

        case GATT_INTENT_ENUM_CHAR_DESCRS:
            if (*errP == ATT_ERROR_ATTRIBUTE_NOT_FOUND) { // end of enum (we could check handle, but even spec is conflicted on what it should be, so we do not)
                pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
                todo->enumCharDscrsCbk(todo->connId, todo->clientTrans, NULL, 0, GATT_CLI_STATUS_OK);
                pthread_mutex_lock(&mGattLock);
                conn = gattConnFindByL2cConn(to);
                if (!conn)
                    logd("Connection went away while processing\n");
                handled = true;
            }
            break;

        case GATT_INTENT_READ:
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            // "invalid offset" or "not long" are not error, just EOFs, so we treat them as not errors.
            status = ((*errP == ATT_ERROR_ATTRIBUTE_NOT_LONG || *errP == ATT_ERROR_INVALID_OFFSET) ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR);
            todo->readCbk(todo->userData, todo->connId, todo->clientTrans, todo->handle, status, *errP, NULL);
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            break;

        case GATT_INTENT_WRITE:
            pthread_mutex_unlock(&mGattLock); // see "A NOTE ABOUT LOCKS" above
            todo->writeCbk(todo->userData, todo->connId, todo->clientTrans, todo->handle, GATT_CLI_STATUS_ERR, *errP);
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            break;

        default:
            loge("Unknown reason for getting a error result\n");
            break;
        }

        if (!handled)
            logw("Unhandled ATT error. Who knows what this might cause?\n");

        gattCliTodoItemFree(todo);

        if (conn)
            gattEnqueueCliDoNextTodoItem(conn);
    }
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttCliIndNotif
 * USE:      Called by ATT as a result of getting a notification or indication
 * PARAMS:   to - peer's connection handle
 *           needsAck - set for indications
 *           handle - the handle value that changed
 *           data - the data that was sent by the server
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttCliIndNotif(l2c_handle_t to, bool needsAck, uint16_t handle, sg data)
{
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByL2cConn(to);
    if (!conn)
        logw("Notif callback on a nonexistent connection\n");
    else {
        struct gattCliNotif *n;

        /* find the per-connection notif structure */
        for (n = conn->cliNotifs; n && n->charValueHandle != handle; n = n->next);
        if (!n)
            logw("Got notif/ind which we have no record of\n");
        else {

            struct gattClientConnection *clicon;
            struct gattCliNotifRec *nr;
            sg dataCopy;

            /* find all clients who are subscribed and notify them */
            for (clicon = mClientConns; clicon; clicon = clicon->next) {
                /* skip all clients not connected to this connection */
                if (clicon->gattConnId != conn->gattConnId)
                    continue;

                /* skip all clients who are not subscribed to this notif */
                for (nr = clicon->notifs; nr && nr->notif != n; nr = nr->next);
                if (!nr)
                    continue;

                /* now we have a client who cares, tell them about this glorious event */
                dataCopy = sgDup(data);
                if (!dataCopy)
                    logw("Failed to copy data for ind/notif\n");
                else if (!gattEnqueueCliIndCall(clicon, nr, dataCopy)) {
                    logw("Failed to schedule a call for notif/ind\n");
                    sgFree(dataCopy);
                }
            }

        }
    }

    pthread_mutex_unlock(&mGattLock);

    sgFree(data);

    /* since multiple (or no) clients may be subscribedat this very moment, we generate the ACK ourselves */
    if (needsAck)
        attCliSendIndicationAck(to);
}

/*
 * FUNCTION: gattProfileInit
 * USE:      Initialize GATT
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool gattProfileInit(void)
{
    static const struct l2cServicePsmDescriptor gattPsm = {
        .serviceInstanceAlloc = gattPsmSvcAlloc,
        .serviceInstanceStateCbk = gattConnEvt,
        .mtu = L2C_MAX_SAFE_MTU,
    };
    static const struct l2cServiceFixedChDescriptor gattFixedCh = {
        .serviceFixedChAlloc = gattFixedChSvcAlloc,
        .serviceFixedChStateCbk = gattConnEvt,
    };

    attSrvInit(gattAttSrvTxCbk, gattAttSrvMtuCbk, gattAttSrvIndCbk, gattAttExecWriteCbk, gattAttSvrReadCbk, gattAttSvrWriteCbk);
    attCliInit(gattAttCliMtuCbk, gattAttCliFindInfoCbk, gattAttCliFindByTypeValCbk, gattAttCliReadCbk, gattAttCliWriteCbk, gattAttCliError, gattAttCliIndNotif);

    mWorkQ = workQueueAlloc(GATT_WORK_Q_SIZE);
    if (!mWorkQ) {
        loge("Failed create a workQ\n");
        goto out_workq;
    }

    if (pthread_create(&mWorker, NULL, gattWorker, NULL)) {
        loge("Failed create worker\n");
        goto out_thread;
    }

    mSendQ = sendQueueAlloc(8);
    if (!mSendQ) {
        loge("Failed create a sendQ\n");
        goto out_sendq;
    }

    if (!l2cApiServicePsmRegister(GATT_PSM, &gattPsm)) {
        loge("Failed to register GATT PSM\n");
        goto out_psm;
    }

    if (!l2cApiServiceFixedChRegister(GATT_FIXEDCH, &gattFixedCh)) {
        loge("Failed to register GATT FixedCh\n");
        goto out_fixed;
    }


    return true;


out_fixed:
    l2cApiServicePsmUnregister(GATT_PSM, NULL, 0);

out_psm:
    sendQueueFree(mSendQ);

out_sendq:
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);

out_thread:
    workQueueFree(mWorkQ, gattWorkQueueFreeItem);

out_workq:
    attSrvDeinit();
    attCliDeinit();

    return false;
}

/*
 * FUNCTION: gattProfileDeinit
 * USE:      Deinitialize GATT
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void gattProfileDeinit(void)
{
    l2cApiServiceFixedChUnregister(GATT_FIXEDCH, NULL, 0);
    l2cApiServicePsmUnregister(GATT_PSM, NULL, 0);
    attSrvDeinit();
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);
    workQueueFree(mWorkQ, gattWorkQueueFreeItem);
    sendQueueFree(mSendQ);
}

/*
 * FUNCTION: gattServiceCreate
 * USE:      Create a public-facing GATT service
 * PARAMS:   uuid - the uuid to use
 *           primary - primary or secondary service?
 *           numHandles - how many handles we expect this service to occupy
 *           readF - read callback for the values
 *           writeF - write callback for the values
 *           indF - callback to be notified of confirmations of indications
 *           discF - callback to be notified of disconnects
 * RETURN:   service reference or 0 on error
 * NOTES:
 */
gatt_service_t gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF, gattSrvDisconnectCbk discF)
{
    struct gattService *svc;
    gatt_service_t svcRef = 0;

    if (numHandles < GATT_HDR_HANDLES) {
        loge("Service with %u handles is impossible (%u min)\n", numHandles, GATT_HDR_HANDLES);
        return 0;
    }

    pthread_mutex_lock(&mGattLock);
    if (gattServiceFindByUuid(uuid)) {
        loge("Service ("UUIDFMT") already exists!\n", UUIDCONV(*uuid));
        goto out;
    }
    svc = (struct gattService*)calloc(1, sizeof(struct gattService));
    if (!svc)
        goto out;

    /* get a handle */
    svc->svcRef = uniqGetNext();

    /* grab a range */
    svc->attRange = attSrvHandleRangeReserve(numHandles);
    if (!svc->attRange) {
        loge("Failed to reserve %u-handle range\n", numHandles);
        goto out_att;
    }

    memcpy(&svc->uuid, uuid, sizeof(svc->uuid));
    svc->readF = readF;
    svc->writeF = writeF;
    svc->indF = indF;
    svc->discF = discF;

    svc->maxItems = numHandles;
    svc->primary = primary;
    svcRef = svc->svcRef;

    svc->next = mSvcs;
    if (mSvcs)
        mSvcs->prev = svc;
    mSvcs = svc;

    goto out;

out_att:
    free(svc);

out:
    pthread_mutex_unlock(&mGattLock);
    return svcRef;
}

/*
 * FUNCTION: gattPermToAttPerm
 * USE:      Convert our 32-bit permissions mask to something we can pass to att with its simple 2-flag setup
 * PARAMS:   gattPerms - bitmask of GATT_PERM_*
 * RETURN:   bitmask of ATT_PERM_*
 * NOTES:
 */
static uint8_t gattPermToAttPerm(uint32_t gattPerms)
{
    uint8_t ret = 0;

    if (gattPerms & GATT_PERM_READ)
        ret |= ATT_PERM_READ;
    if (gattPerms & GATT_PERM_READ_ENCR)
        ret |= ATT_PERM_READ_ENCR;
    if (gattPerms & GATT_PERM_READ_ENCR_MITM)
        ret |= ATT_PERM_READ_ENCR_MITM;
    if (gattPerms & GATT_PERM_WRITE)
        ret |= ATT_PERM_WRITE;
    if (gattPerms & GATT_PERM_WRITE_ENCR)
        ret |= ATT_PERM_WRITE_ENCR;
    if (gattPerms & GATT_PERM_WRITE_ENCR_MITM)
        ret |= ATT_PERM_WRITE_ENCR_MITM;
    if (gattPerms & (GATT_PERM_WRITE_SIGNED | GATT_PERM_WRITE_SIGNED_MITM))
        ret |= ATT_PERM_WRITE_SIGNED;

    return ret;
}

/*
 * FUNCTION: gattServiceOnline
 * USE:      Online a service
 * PARAMS:   svc - the service struct
 *           supportLE - does this service support LE
 *           supportEDR - does this service support EDR
 * RETURN:   success
 * NOTES:    call with mGattLock held
 */
static bool gattServiceOnline(struct gattService *svc, bool supportLE, bool supportEDR)
{
    static const uint8_t attAllReadPerms = ATT_PERM_READ | ATT_PERM_READ_ENCR | ATT_PERM_READ_ENCR_MITM;
    uint16_t ofst = 0, charOfst = 0;
    struct gattItem *gi;
    struct uuid uuid;
    uint8_t perms = 0;

    if (svc->online) {
        logw("Cannot online an online service\n");
        goto out;
    }

    svc->allowLE = supportLE;
    svc->allowEDR = supportEDR;
    if (!aapiSrvHandleRangeSetAllowedTransports(svc->attRange, supportLE, supportEDR)) {
        logw("Failed to set ATT transport perms\n");
        goto out;
    }

    uuidFromUuid16(&uuid, svc->primary ? GATT_UUID_SVC_PRIMARY : GATT_UUID_SVC_SECONDARY);
    if (!attSrvValueRegister(svc->attRange, ofst++, &uuid, attAllReadPerms)) {
        loge("Failed to add service header\n");
        goto out;
    }

    gi = svc->itemsHead;
    while (gi) {
        bool updateGrp = false;

        switch (gi->type) {
        case GATT_ITEM_TYPE_INCLUDE:
            uuidFromUuid16(&uuid, GATT_UUID_INCLUDE);
            perms = attAllReadPerms;
            break;
        case GATT_ITEM_TYPE_CHAR_DECL:
            uuidFromUuid16(&uuid, GATT_UUID_CHARACTERISTIC);
            perms = attAllReadPerms;
            charOfst = ofst;
            break;
        case GATT_ITEM_TYPE_CHAR_VAL:
            if (gi->prev && gi->prev->type == GATT_ITEM_TYPE_CHAR_DECL)
                memcpy(&uuid, &gi->prev->charDecl.uuid, sizeof(uuid));
            else {
                loge("Char value not following a char descr\n");
                goto out_dereg;
            }
            perms = gattPermToAttPerm(gi->charVal.valPerms);
            updateGrp = true;
            break;
        case GATT_ITEM_TYPE_CHAR_DESCR:
            memcpy(&uuid, &gi->descriptor.uuid, sizeof(uuid));
            perms = gattPermToAttPerm(gi->descriptor.descrValPerms);
            updateGrp = true;
            break;
        default:
            loge("Unknown item type %u\n", gi->type);
            goto out_dereg;
        }

        if (!attSrvValueRegister(svc->attRange, ofst, &uuid, perms)) {
            loge("Failed to item at offset %u\n", ofst - 1);
            goto out_dereg;
        }
        ofst++;
        if (updateGrp && !attSrvValueSetGrpLen(svc->attRange, charOfst, ofst - charOfst)) {
            loge("Failed to set group length for characteristic\n");
            goto out_dereg;
        }
        gi = gi->next;
    }

    if (!attSrvValueSetGrpLen(svc->attRange, 0, ofst)) {
        loge("Failed to set group length for service\n");
        goto out_dereg;
    }

    svc->online = true;
    return true;

out_dereg:
    while (ofst)
        attSrvValueUnregister(svc->attRange, --ofst);

out:
    return false;
}

/*
 * FUNCTION: gattServiceOffline
 * USE:      Offline a service
 * PARAMS:   svc - the service struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattServiceOffline(struct gattService *svc)
{
    uint16_t i;

    if (!svc->online) {
        loge("Cannot offline an offlined service\n");
        return;
    }
    svc->online = false;

    for (i = 0; i < svc->numItems; i++)
        if (!attSrvValueUnregister(svc->attRange, i + GATT_HDR_HANDLES))
            logw("Failed to unregister a value (ofst %u) for offlining!\n", i + GATT_HDR_HANDLES);

    if (!attSrvValueUnregister(svc->attRange, 0))
        logw("Failed to unregister a header value for offlining!\n");
}

/*
 * FUNCTION: gattServiceDelete
 * USE:      Delete a service
 * PARAMS:   svc - the service struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattServiceDelete(struct gattService *svc)
{
    struct gattItem *gi, *t;

    if (svc->online) {
        loge("Cannot delete an online service\n");
        return;
    }

    attSrvHandleRangeRelease(svc->attRange);
    if (svc->prev)
        svc->prev->next = svc->next;
    else
        mSvcs = svc->next;
    if (svc->next)
        svc->next->prev = svc->prev;

    gi = svc->itemsHead;
    while (gi) {
        t = gi;
        gi = gi->next;

        if (t->type == GATT_ITEM_TYPE_INCLUDE)
            t->include.incl->useCount--;

        free(t);
    }
    free(svc);
}

/*
 * FUNCTION: gattServiceDestroy
 * USE:      Destroy a public-facing GATT service
 * PARAMS:   svcRef - the service reference
 * RETURN:   success
 * NOTES:
 */
bool gattServiceDestroy(gatt_service_t svcRef)
{
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (svc) {
        if (svc->useCount)
            loge("Refusing to destroy service included elsewhere (%u times)\n", svc->useCount);
        else {
            if (svc->online) {
                logw("Destroying an online service\n");
                gattServiceOffline(svc);
            }
            gattServiceDelete(svc);
        }
    }
    pthread_mutex_unlock(&mGattLock);

    return !!svc;
}

/*
 * FUNCTION: gattServiceStart
 * USE:      Online a service
 * PARAMS:   svcRef - the service reference
 *           supportLE - does this service support LE
 *           supportEDR - does this service support EDR
 * RETURN:   success
 * NOTES:
 */
bool gattServiceStart(gatt_service_t svcRef, bool supportLE, bool supportEDR)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (svc)
        ret = gattServiceOnline(svc, supportLE, supportEDR);
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceStop
 * USE:      Offline a service
 * PARAMS:   svcRef - the service reference
 * RETURN:   success
 * NOTES:
 */
bool gattServiceStop(gatt_service_t svcRef)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (svc) {
        gattServiceOffline(svc);
        ret = true;
    }
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceIsRunning
 * USE:      Check if a service is online
 * PARAMS:   svcRef - the service reference
 * RETURN:   true if service exists and is running
 * NOTES:
 */
bool gattServiceIsRunning(gatt_service_t svcRef)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (svc)
        ret = svc->online;
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceGetUuid
 * USE:      Get a service's UUID
 * PARAMS:   svcRef - the service reference
 *           dst - store UUID here
 * RETURN:   true if service exists and UUID was gotten
 * NOTES:
 */
bool gattServiceGetUuid(gatt_service_t svcRef, struct uuid *dst)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (svc) {
        memcpy(dst, &svc->uuid, sizeof(struct uuid));
        ret = true;
    }
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceGetHandleBaseByUuid
 * USE:      Get the base handle for a service given its UUID. Only works for online services
 * PARAMS:   uuid - the desired uuid
 * RETURN:   handle value or 0 on error
 * NOTES:
 */
uint16_t gattServiceGetHandleBaseByUuid(const struct uuid *uuid)
{
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByUuid(uuid);
    if (!svc)
        loge("Cannot get base for not-found service\n");
    else if (!svc->online)
        loge("Non-online services have no base\n");
    else
        handle = attSrvHandleRangeGetBase(svc->attRange);
    pthread_mutex_unlock(&mGattLock);

    return handle;
}

/*
 * FUNCTION: gattServiceGetHandleBaseBySvc
 * USE:      Get the base handle for a service given its service ref
 * PARAMS:   uuid - the desired uuid
 * RETURN:   handle value or 0 on error
 * NOTES:
 */
uint16_t gattServiceGetHandleBaseBySvc(gatt_service_t svcRef)
{
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc)
        loge("Cannot get base for not-found service\n");
    else
        handle = attSrvHandleRangeGetBase(svc->attRange);
    pthread_mutex_unlock(&mGattLock);

    return handle;
}

/*
 * FUNCTION: gattServiceFindByHandle
 * USE:      Find the svcRef of a service by handle
 * PARAMS:   handle - the handle
 *           firstOnly - if set only search by start of range, else serach by anything IN range
 * RETURN:   srvcRef or 0 on error
 * NOTES:
 */
gatt_service_t gattServiceFindByHandle(uint16_t handle, bool firstOnly)
{
    struct gattService *svc;
    gatt_service_t ret = 0;

    pthread_mutex_lock(&mGattLock);
    svc = mSvcs;
    while (svc) {
       uint16_t base = attSrvHandleRangeGetBase(svc->attRange);
       uint16_t len = attSrvHandleRangeGetLen(svc->attRange);

       if (firstOnly && base == handle)
           break;

       if (base <= handle && base + len > handle)
           break;

       svc = svc->next;
    }

    if (svc)
        ret = svc->svcRef;
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattSeerviceItemAppend
 * USE:      Append an item to a service
 * PARAMS:   svc - service struct
 *           gi - the item
 * RETURN:   offset from range start
 * NOTES:    probably want to tall this with the lock held
 */
static uint16_t gattServiceItemAppend(struct gattService *svc, struct gattItem *gi)
{
    gi->prev = svc->itemsTail;
    if (svc->itemsTail)
        svc->itemsTail->next = gi;
    else
        svc->itemsHead = gi;
    svc->itemsTail = gi;
    return svc->numItems++;
}

/*
 * FUNCTION: gattServiceAddIncludedService
 * USE:      Append an "included service" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the service to include
 * RETURN:   handle or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddIncludedService(gatt_service_t svcRef, const struct uuid *uuid)
{
    struct gattService *svc, *included;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service "GATTHANDLEFMT"\n", GATTHANDLECNV(svcRef));
        goto out;
    }
    included = gattServiceFindByUuid(uuid);
    if (!included) {
        loge("Failed to find included service "UUIDFMT"n", UUIDCONV(*uuid));
        goto out;
    }
    if (svc->numItems >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }

    gi = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!gi) {
        loge("Failed to allocate new item for include\n");
        goto out;
    }

    gi->type = GATT_ITEM_TYPE_INCLUDE;
    gi->include.incl = included;
    handle = gattServiceItemAppend(svc, gi) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;
    included->useCount++;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceAddChar
 * USE:      Append a "characteristic" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the uuid of the value
 *           props - the value properties to show
 *           perms - actual permissions for the value
 * RETURN:   handle of the value attribute (not of the descriptor attribute which is first) or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddChar(gatt_service_t svcRef, const struct uuid *uuid, uint8_t props, uint32_t perms)
{
    struct gattItem *giDecl, *giVal;
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service "GATTHANDLEFMT"\n", GATTHANDLECNV(svcRef));
        goto out;
    }
    if (svc->numItems + 1 >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }

    giDecl = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!giDecl) {
        loge("Failed to allocate new item for chat decl\n");
        goto out;
    }

    giVal = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!giVal) {
        free(giDecl);
        loge("Failed to allocate new item for char val\n");
        goto out;
    }

    giDecl->type = GATT_ITEM_TYPE_CHAR_DECL;
    giDecl->charDecl.charProps = props;
    memcpy(&giDecl->charDecl.uuid, uuid, sizeof(giDecl->charDecl.uuid));

    giVal->type = GATT_ITEM_TYPE_CHAR_VAL;
    giVal->charVal.valPerms = perms;

    gattServiceItemAppend(svc, giDecl);
    handle = gattServiceItemAppend(svc, giVal) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceAddCharDescr
 * USE:      Append a "characteristic descriptor" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the uuid of the descriptor
 *           perms - actual permissions for the descriptor's value
 * RETURN:   handle or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddCharDescr(gatt_service_t svcRef, const struct uuid *uuid, uint32_t perms)
{
    struct gattService *svc;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service "GATTHANDLEFMT"\n", GATTHANDLECNV(svcRef));
        goto out;
    }
    if (svc->numItems >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }
    gi = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!gi) {
        loge("Failed to allocate new item for char descriptor\n");
        goto out;
    }

    gi->type = GATT_ITEM_TYPE_CHAR_DESCR;
    memcpy(&gi->descriptor.uuid, uuid, sizeof(gi->descriptor.uuid));
    gi->descriptor.descrValPerms = perms;
    handle = gattServiceItemAppend(svc, gi) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceSendInd
 * USE:      Send an indication or a notification
 * PARAMS:   svcRef - the reference to a service
 *           cid - ATT's connection Id
 *           handle - the handle to notify/indicate on
 *           buf - the data to send
 *           len - length of said data
 *           withConfirm - use an indication?
 *           ref - passed to callback later. unused by GATT
 * RETURN:   false on immediate failure
 * NOTES:
 */
bool gattServiceSendInd(gatt_service_t svcRef, att_cid_t cid, uint16_t handle, const void *buf, uint16_t len, bool withConfirm, uint64_t ref)
{
    uint8_t ret = L2C_TX_ERROR;
    struct gattService *svc;
    uint16_t base;
    sg data;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service "GATTHANDLEFMT"\n", GATTHANDLECNV(svcRef));
        goto out;
    }

    base = attSrvHandleRangeGetBase(svc->attRange);
    if (handle < base || handle >= base + svc->maxItems) {
        logd("Refusing to send notif for handle 0x%04X not in range 0x%04X+0x%04X\n", handle, base, svc->maxItems);
        goto out;
    }

    data = sgNewWithCopyData(buf, len);
    if (!data) {
        loge("Failed to contrust an SG\n");
        goto out;
    }

    ret = attSrvValueNotifyChanged(svc->attRange, handle - base, cid, data, !withConfirm, ref);
    if (ret == L2C_TX_ACCEPTED) {
        if (!withConfirm && !gattEnqueueSrvIndCall(svc, attSrvCidResolve(cid), cid, handle, ATT_SRV_EVT_NOTIF_SENT, ref))
            loge("Failed to notify caller of notif TX\n");
    } else
        sgFree(data);

out:
    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattCliHandleCccdWriteRequestIfNeeded
 * USE:      Figure out of a CCCD write is needed, and enqueue it if so
 * PARAMS:   conn - the conection handle
 *           now - the todo item
 * RETURN:   att transaction ID if we start a transaction, 0 else
 * NOTES:    called from the worker thread with mGattLock NOT held!
 */
static uniq_t gattCliHandleCccdWriteRequestIfNeeded(l2c_handle_t conn, struct gattCliTodoItem *now)
{
    uint8_t writeData[sizeof(uint16_t)];
    struct gattConnState *connState;
    struct gattCliNotif *notif;
    uint16_t cccdVal;
    uniq_t trans = 0;
    sg writeDataSg;

    pthread_mutex_lock(&mGattLock);
    connState = gattConnFindByL2cConn(conn);
    if (!connState)
        goto out;

    //find the notif by locator;
    notif = gattNotifFindByLocatorInConn(connState, now->notifLocator);
    if (!notif)
        goto out;

    //figure out what (if anything) to do with it
    if (notif->refCtInd && notif->curState == NOTIF_C_STATE_SUBBED_TO_IND)
        goto out;
    if (!notif->refCtInd && notif->refCtNotif && notif->curState == NOTIF_C_STATE_SUBBED_TO_NOTIF)
        goto out;

    if(!notif->refCtInd && !notif->refCtNotif && notif->curState != NOTIF_C_STATE_NOT_SUBBED)
        goto out;

    //we now know we need a CCCD write - figure out the value
    if (notif->refCtInd)
        cccdVal = GATT_CLI_CFG_BIT_INDS;
    else if (notif->refCtNotif)
        cccdVal = GATT_CLI_CFG_BIT_NOTIFS;
    else
        cccdVal = 0;
    utilSetLE16(writeData, cccdVal);

    //store the data into the todo and the value we're writing into offset (so we can know what we wrote when the ACK comes)
    writeDataSg = sgNewWithCopyData(writeData, sizeof(writeData));
    if (!writeDataSg)
        goto out;
    now->offset = cccdVal;

    //try to issue the write
    trans = attCliWrite(conn, now->handle, writeDataSg);
    if (!trans)
        sgFree(writeDataSg);

out:
    pthread_mutex_unlock(&mGattLock);
    return trans;
}

/*
 * FUNCTION: gattCliTodoNext
 * USE:      Start on the next to-do item for the connection
 * PARAMS:   conn - the conection handle
 *           now - the todo item
 * RETURN:   NONE
 * NOTES:    This should ONLY be called from the worker thread with mGattLock NOT held!
 */
static void gattCliTodoNext(l2c_handle_t conn, struct gattCliTodoItem *now)
{
    struct gattConnState *connState;
    uniq_t trans = 0;
    uint16_t uuid16;

    /* actually start it */
    logv("Current gatt cli todo command being sent: %u\n", now->type);
    switch (now->type) {
    case GATT_CLI_TODO_FIND_INFO:
        trans = attCliFindInformation(conn, now->handle, now->endHandle);
        if (!trans)
            logw("FindInfo failed\n");
        break;

    case GATT_CLI_TODO_FIND_BY_TYPE_VAL:
        if (!uuidToUuid16(&uuid16, &now->uuid)) {
            loge("Cannot do find-by-type-value with a non-16-bit-uuid. You're about to have a Bad Time (tm)\n");
            uuid16 = 0;
        }
        trans = attCliFindByTypeValue(conn, now->handle, now->endHandle, uuid16, now->data);
        if (!trans) {
            sgFree(now->data);
            logw("FindByTypeValue failed\n");
        }
        now->data = NULL;
        break;

    case GATT_CLI_TODO_READ_BY_TYPE:
        trans = attCliReadByType(conn, now->handle, now->endHandle, &now->uuid);
        if (!trans)
            logw("ReadByType failed\n");
        break;

    case GATT_CLI_TODO_READ:
        trans = attCliRead(conn, now->handle);
        if (!trans)
            logw("Read failed\n");
        break;

    case GATT_CLI_TODO_READ_BLOB:
        trans = attCliReadBlob(conn, now->handle, now->offset);
        if (!trans)
            logw("ReadBlob failed\n");
        break;

    case GATT_CLI_TODO_READ_BY_GRP_TYPE:
        trans = attCliReadByGroupType(conn, now->handle, now->endHandle, &now->uuid);
        if (!trans)
            logw("ReadByGroupType failed\n");
        break;

    case GATT_CLI_TODO_WRITE:
        trans = attCliWrite(conn, now->handle, now->data);
        if (!trans) {
            sgFree(now->data);
            logw("Write with confirm failed\n");
        }
        now->data = NULL;
        break;

    case GATT_CLI_TODO_WRITE_NO_ACK:
        if (!attCliWriteNoAck(conn, now->handle, now->data)) {
            sgFree(now->data);
            logi("Write with no confirm failed\n");
        }
        now->data = NULL;
        break;

    case GATT_CLI_TODO_SIGNED_WRITE:
        if (!attCliSignedWriteNoAck(conn, now->handle, now->data)) {
            sgFree(now->data);
            logi("Write with signature failed\n");
        }
        now->data = NULL;
        break;

    case GATT_CLI_TODO_PREPARE_WRITE:
        trans = attCliPrepareWrite(conn, now->handle, now->offset, now->data);
        if (!trans) {
            sgFree(now->data);
            logw("PrepareWrite failed\n");
        }
        now->data = NULL;
        break;

    case GATT_CLI_TODO_WRITE_EXEC:
        trans = attCliExecuteStagedWrites(conn, now->commit);
        if (!trans)
            logw("ExecWrite failed\n");
        break;

    case GATT_CLI_TODO_SEND_MTU_REQ:
        trans = attCliRequestMtu(conn, now->mtu);
        break;

    case GATT_CLI_TODO_WRITE_CCCD:
        trans = gattCliHandleCccdWriteRequestIfNeeded(conn, now);
        break;
    }

    pthread_mutex_lock(&mGattLock);
    connState = gattConnFindByL2cConn(conn);
    if (!connState)
        logd("ToDo item we just did no longer has a connection. Oops\n");
    else if (connState->todoCurr != now)
        loge("Inconsistent idea of what the current todo item is!\n");
    else if (!trans) { /* a transaction is NOT ongoing */
        gattCliTodoItemFree(now);
        if (connState) {
            connState->todoCurr = NULL;
            gattEnqueueCliDoNextTodoItem(connState);
        }
    }
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattCliNotifSubUnsubIssueWriteIfNeededNewTodo
 * USE:      Issue a write to the notif/ind subscription descriptor if needed
 * PARAMS:   conn - the connection structure
 *           notif - the notif struct
 * RETURN:   false on immediate error
 * NOTES:    call with mGattLock held. When is a write needed? If all clients we
 *           have need a different subscription model than we currently have, or
 *           all clients go away.
 */
static bool gattCliNotifSubUnsubIssueWriteIfNeededNewTodo(struct gattConnState *conn, struct gattCliNotif *notif)
{
    struct gattCliTodoItem *todo;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return false;

    todo->purpose = GATT_INTENT_NOTIF_SUB_UNSUB;
    todo->type = GATT_CLI_TODO_WRITE_CCCD;
    todo->handle = notif->charConfigHandle;
    todo->auxHandle[0] = notif->charValueHandle;
    todo->notifLocator = notif->locator;

    gattCliTodoEnqItem(conn, todo, false);

    return true;
}

/*
 * FUNCTION: gattCliNotifRecDelete
 * USE:      Delete a gattCliNotifRec. If needed, schedule a deletion for gattCliNotif as well
 * PARAMS:   clicon - the client connection structure
 *           nr - the notif rec
 * RETURN:   NONE
 * NOTES:    call with mGattLock held. "nr" WILL be invalid after this call, "notif" may be too
 */
static void gattCliNotifRecDelete(struct gattClientConnection *clicon, struct gattCliNotifRec *nr)
{
    struct gattConnState* conn = gattConnFindById(clicon->gattConnId);
    struct gattCliNotif *notif = nr->notif;

    if (!conn)
        return;

    /* adjust refcount */
    if (nr->wantInd)
        notif->refCtInd--;
    else
        notif->refCtNotif--;

    /* delete the notif rec itself */
    if (nr->next)
        nr->next->prev = nr->prev;
    if (nr->prev)
        nr->prev->next = nr->next;
    else
        clicon->notifs = nr->next;

    free(nr);

    /* see if notif itself needs deletion */
    if (!notif->refCtInd && !notif->refCtNotif) {
        if (conn->cliNotifs == notif)
            conn->cliNotifs = notif->next;
        else
            notif->prev->next = notif->next;
        if (notif->next)
            notif->next->prev = notif->prev;

        free(notif);
    }
}

/*
 * FUNCTION: gattClientNotifUnsubscribe
 * USE:      Unsubscribe fron a notification/indication
 * PARAMS:   clicon - the client connection structure
 *           nr - the gattCliNotifRec
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattClientNotifUnsubscribe(struct gattClientConnection *clicon, struct gattCliNotifRec *nr)
{
    struct gattConnState *conn = gattConnFindById(clicon->gattConnId);
    struct gattCliNotif *notif = nr->notif;

    gattCliNotifRecDelete(clicon, nr);

    /* if needed, do [another] char descr write */
    if (!conn || !gattCliNotifSubUnsubIssueWriteIfNeededNewTodo(conn, notif)) {
        loge("Unnotif write enqueue failed\n");
    }
}

/*
 * FUNCTION: gattClientConnClose
 * USE:      Close a client's connection (optionally notify said client)
 * PARAMS:   clicon - the client connection structure
 *           status - status to notify client with (if GATT_CLI_STATUS_OK, no notification will be sent)
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattClientConnClose(struct gattClientConnection *clicon, uint8_t status)
{
    struct gattConnState* conn = gattConnFindById(clicon->gattConnId);
    struct gattCliTodoItem *tC, *tN, *tP = NULL;

    /* notify client if needed */
    if (status != GATT_CLI_STATUS_OK && !gattEnqueueCliConnStatusCall(clicon, status))
        logw("Failed to notify client about connection closure. Pity.\n");

    /* enumerate all notifications this client had and unsubscribe from them. */
    while (clicon->notifs)
        gattClientNotifUnsubscribe(clicon, clicon->notifs);

    if (conn) {
        /*
         * Remove all todo list items that are in client's name. If one is
         * in flight, it will gracefully fail when it fails to find a client
         * on completion, so we need not do much for that case. In reality
         * this is all an optimization. We could leave all these items here
         * and let them all fail safely. But since it is easy to get rid of
         * them, we do.
         */
        for (tC = conn->todoListH; tC; tC = tN) {
            tN = tC->next;
            if (tC->connId == clicon->connId) {
                if (tC == conn->todoListH)
                    conn->todoListH = tN;
                else
                    tP->next = tN;
                if (tC == conn->todoListT)
                    conn->todoListT = tP;
                free(tC);
            } else
                tP = tC;
        }

        conn->numLocalUsers--;
    }

    /* destroy the structure(s) */
    if (clicon->next)
        clicon->next->prev = clicon->prev;
    if (clicon->prev)
        clicon->prev->next = clicon->next;
    else
        mClientConns = clicon->next;
    free(clicon);

    /* proactively close the connection */
    if (!conn->hadRemoteUse && !conn->numLocalUsers) {

        logd("GATT connection no longer being used, closing underlying L2C connection\n");
        l2cApiDisconnect(conn->conn);
    }
}

/*
 * FUNCTION: gattClientDisconnect
 * USE:      Disconnect an actual gatt connection
 * PARAMS:   connId - the connection
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientDisconnect(gatt_client_conn_t connId)
{
    static struct gattClientConnection* clicon;
    uint8_t ret = GATT_CLI_STATUS_OK;

    pthread_mutex_lock(&mGattLock);
    clicon = gattClientConnFindById(connId);
    if (clicon)
        gattClientConnClose(clicon, GATT_CLI_STATUS_WE_DISC);
    else
        ret = GATT_CLI_STATUS_ERR;
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattClientInvalidateCache
 * USE:      Invalidate cache and repopulate it
 * PARAMS:   whomTo - cache of what device to invalidate
 * RETURN:   false on immediate failure
 * NOTES:
 */
bool gattClientInvalidateCache(const struct bt_addr *whomTo)
{
    struct gattConnState *conn;
    bool ret = true;

    pthread_mutex_lock(&mGattLock);
    conn = gattConnFindByAddr(whomTo);
    if (!conn)
        logw("Caches of nonexistent GATT connections are by definition always clean :)\n");
    else
        ret = attClientInvalidateCache(conn->conn);

    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattClientGetMtu
 * USE:      retrieve the agreed MTU with a peer
 * PARAMS:   connId - the connection id
 * RETURN:   a non-zero MTU if the connection exists, 0 otherwise
 * NOTES:
 */
uint32_t gattClientGetMtu(gatt_client_conn_t connId)
{
    struct gattClientConnection *conn = NULL;
    struct gattConnState *connState = NULL;
    uint32_t mtu = ATT_MTU_INVALID;

    pthread_mutex_lock(&mGattLock);

    conn = gattClientConnFindById(connId);
    if (!conn) {
      logw("Failed to retrieve MTU with a nonexistent GATT Client connection\n");
      goto out;
    }

    connState = gattConnFindById(conn->gattConnId);
    if (!connState) {
      logw("Failed to retrieve MTU with a nonexistent GATT connection\n");
      goto out;
    }

    mtu = connState->mtu;
    if (mtu < ATT_MTU_MIN_LE)
      mtu = ATT_MTU_INVALID;
out:
    pthread_mutex_unlock(&mGattLock);
    return mtu;
}

/*
 * FUNCTION: gattCliTodoEnqItem
 * USE:      Enqueue a gatt to-do item in front or back of list
 * PARAMS:   conn - the connection structure
 *           todo - the item
 *           inFront - add it in front (instead of in order) this is used for confirmations
 * RETURN:   NONE
 * NOTES:    Call with mGattLock held
 */
static void gattCliTodoEnqItem(struct gattConnState *conn, struct gattCliTodoItem *todo, bool inFront)
{
    if (todo->enqueued) {
        loge("Cannot enqueue an item that is already in the list!\n");
        return;
    }
    todo->enqueued = true;

    logv("Enqueuing item %p with handle range 0x%04x-0x%04x in %s\n", todo, todo->handle, todo->endHandle, inFront ? "front" : "rear");

    if (conn->todoListH) { /* have items in list already? */
        if (inFront) {
            todo->next = conn->todoListH;
            conn->todoListH = todo;
        } else {
            todo->next = NULL;
            conn->todoListT->next = todo;
            conn->todoListT = todo;
        }
    } else {
        conn->todoListT = todo;
        conn->todoListH = todo;
        if (!conn->todoCurr) /* if no current task, start one */
            gattEnqueueCliDoNextTodoItem(conn);
    }
}

/*
 * FUNCTION: gattCliTodoMakeReadByGroupType
 * USE:      Enqueue a read-by-group-type todo item
 * PARAMS:   connId - the connection id
 *           type - the type of attribute to read (uuid16)
 *           purpose - the intent of this request
 *           firstHandle - first handle to consider
 *           lastHandle - last handle to consider
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeReadByGroupType(gatt_client_conn_t connId, uint16_t type, uint8_t purpose, uint16_t firstHandle, uint16_t lastHandle)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_READ_BY_GRP_TYPE;
    uuidFromUuid16(&todo->uuid, type);
    todo->handle = firstHandle;
    todo->endHandle = lastHandle;
    todo->purpose = purpose;
    todo->connId = clicon->connId;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeFindByTypeValue
 * USE:      Enqueue a find-by-type-value todo item
 * PARAMS:   connId - the connection id
 *           type - the type of attribute to find (uuid16)
 *           value - the value we want to find (a copy is made)
 *           valueLen - length of the above value
 *           purpose - the intent of this request
 *           firstHandle - first handle to consider
 *           lastHandle - last handle to consider
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeFindByTypeValue(gatt_client_conn_t connId, uint16_t type, const void* value, uint32_t valueLen, uint8_t purpose, uint16_t firstHandle, uint16_t lastHandle)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;
    sg val;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    val = sgNewWithCopyData(value, valueLen);
    if (!val) {
        free(todo);
        return NULL;
    }

    todo->type = GATT_CLI_TODO_FIND_BY_TYPE_VAL;
    uuidFromUuid16(&todo->uuid, type);
    todo->handle = firstHandle;
    todo->endHandle = lastHandle;
    todo->purpose = purpose;
    todo->connId = clicon->connId;
    todo->data = val;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeReadByType
 * USE:      Enqueue a read-by-type todo item
 * PARAMS:   connId - the connection id
 *           type - the type of attribute to read (uuid16)
 *           purpose - the intent of this request
 *           firstHandle - first handle to consider
 *           lastHandle - last handle to consider
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeReadByType(gatt_client_conn_t connId, uint16_t type, uint8_t purpose, uint16_t firstHandle, uint16_t lastHandle)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_READ_BY_TYPE;
    uuidFromUuid16(&todo->uuid, type);
    todo->handle = firstHandle;
    todo->endHandle = lastHandle;
    todo->purpose = purpose;
    todo->connId = clicon->connId;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeFindInfo
 * USE:      Enqueue a find-info todo item
 * PARAMS:   connId - the connection id
 *           purpose - the intent of this request
 *           firstHandle - first handle to consider
 *           lastHandle - last handle to consider
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeFindInfo(gatt_client_conn_t connId, uint8_t purpose, uint16_t firstHandle, uint16_t lastHandle)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_FIND_INFO;
    todo->handle = firstHandle;
    todo->endHandle = lastHandle;
    todo->purpose = purpose;
    todo->connId = clicon->connId;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeRead
 * USE:      Enqueue a read todo item
 * PARAMS:   connId - the connection id
 *           purpose - the intent of this request
 *           handle - handle to consider
 *           authReq - the authentication requirements
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeRead(gatt_client_conn_t connId, uint8_t purpose, uint16_t handle, uint8_t authReq)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_READ;
    todo->handle = handle;
    todo->purpose = purpose;
    todo->connId = clicon->connId;
    todo->authReq = authReq;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeReadBlob
 * USE:      Enqueue a read blob todo item
 * PARAMS:   connId - the connection id
 *           purpose - the intent of this request
 *           handle - handle to consider
 *           authReq - the authentication requirements
 *           offset - offset to read at
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeReadBlob(gatt_client_conn_t connId, uint8_t purpose, uint16_t handle, uint8_t authReq, uint16_t offset)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_READ_BLOB;
    todo->handle = handle;
    todo->purpose = purpose;
    todo->offset = offset;
    todo->connId = clicon->connId;
    todo->authReq = authReq;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeWrite
 * USE:      Enqueue a write todo item (specific type given by param)
 * PARAMS:   connId - the connection id
 *           purpose - the intent of this request
 *           writeType - the actual write type
 *           handle - handle to consider
 *           authReq - the authentication requirements
 *           offset - offset to write at
 *           data - the data to write
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not advance while mGattLock is held by you, you can customize the item safely after
 *           this func gives it to you and until you release mGattLock
 */
static struct gattCliTodoItem* gattCliTodoMakeWrite(gatt_client_conn_t connId, uint8_t purpose, uint8_t writeType, uint16_t handle, uint8_t authReq, uint16_t offset, sg data)
{
    struct gattClientConnection *clicon;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    clicon = gattClientConnFindById(connId);
    if (!clicon)
        return NULL;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = writeType;
    todo->handle = handle;
    todo->purpose = purpose;
    todo->offset = offset;
    todo->connId = clicon->connId;
    todo->authReq = authReq;
    todo->data = data;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattCliTodoMakeMtuReq
 * USE:      Create a MTU request todo
 * PARAMS:   conn - the connection structure
 *           mtu - the mtu to ask for
 *           purpose - the stated purpose
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static struct gattCliTodoItem* gattCliTodoMakeMtuReq(struct gattConnState* conn, uint16_t mtu, uint8_t purpose)
{
    struct gattCliTodoItem *todo;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_SEND_MTU_REQ;
    todo->purpose = purpose;
    todo->mtu = mtu;
    gattCliTodoEnqItem(conn, todo, false);

    return todo;
}

/*
 * FUNCTION: gattClientFindService
 * USE:      Find all services with a given uuid on a server
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           primary - search for primary services
 *           searchForThis - uuid of service to find
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results (we'll call it with NULL after all results have been returned)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientFindService(void *userData, gatt_client_conn_t conn, bool primary, const struct uuid *searchForThis, uniq_t trans, gattCliSvcEnumCbk cbk)
{
    uint8_t buf[sizeof(struct uuid)];
    struct gattCliTodoItem *todo;
    uint32_t uuidLen;
    uint16_t uuid16;

    // convert uuid to uuid16 bit if possible, either way write it to buffer and keep track of length
    if (uuidToUuid16(&uuid16, searchForThis)) {
        utilSetLE16(buf, uuid16);
        uuidLen = sizeof(uint16_t);
    }
    else {
        uuidWriteLE(buf, searchForThis);
        uuidLen = sizeof(struct uuid);
    }

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByTypeValue(conn, primary ? GATT_UUID_SVC_PRIMARY : GATT_UUID_SVC_SECONDARY, buf, uuidLen, GATT_INTENT_FIND_SVC, ATT_FIRST_VALID_HANDLE, ATT_LAST_VALID_HANDLE);
    if (todo) {
        todo->enumSvcsCbk = cbk;
        todo->desiredUuid = *searchForThis;
        todo->clientTrans = trans;
        todo->userData = userData;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumServices
 * USE:      Find all services on a server
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           primary - search for primary services
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results (we'll call it with NULL after all results have been returned)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumServices(void *userData, gatt_client_conn_t conn, bool primary, uniq_t trans, gattCliSvcEnumCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeReadByGroupType(conn, primary ? GATT_UUID_SVC_PRIMARY : GATT_UUID_SVC_SECONDARY, GATT_INTENT_ENUM_SVCS, ATT_FIRST_VALID_HANDLE, ATT_LAST_VALID_HANDLE);
    if (todo) {
        todo->enumSvcsCbk = cbk;
        todo->clientTrans = trans;
        todo->userData = userData;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumIncludedServices
 * USE:      Find included services in a service
 * PARAMS:   conn - the connection id
 *           fromHandle - first handle to search
 *           toHandle - last handle to search
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumIncludedServices(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumIncludedSvcsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeReadByType(conn, GATT_UUID_INCLUDE, GATT_INTENT_ENUM_INCL_SVCS, fromHandle, toHandle);
    if (todo) {
        todo->enumIncludedSvcsCbk = cbk;
        todo->clientTrans = trans;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumCharacteristics
 * USE:      Enumerate characteristics in a service
 * PARAMS:   conn - the connection id
 *           fromHandle - first handle to search
 *           toHandle - last handle to search
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:    will call callback with each char found
 */
uint8_t gattClientEnumCharacteristics(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumCharacteristicsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeReadByType(conn, GATT_UUID_CHARACTERISTIC, GATT_INTENT_ENUM_CHARS, fromHandle, toHandle);
    if (todo) {
        todo->enumCharsCbk = cbk;
        todo->clientTrans = trans;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumCharDescriptors
 * USE:      Find the next characteristic descriptor in a characteristic
 * PARAMS:   conn - the connection id
 *           fromHandle - first handle to search (value handle plus one)
 *           toHandle - last handle to search
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumCharDescriptors(gatt_client_conn_t conn, uint16_t fromHandle, uint16_t toHandle, uniq_t trans, gattCliEnumCharDescrsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindInfo(conn, GATT_INTENT_ENUM_CHAR_DESCRS, fromHandle, toHandle);
    if (todo) {
        todo->enumCharDscrsCbk = cbk;
        todo->clientTrans = trans;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientVerifyAuthReq
 * USE:      Verify an authReq param given to us is valid (simply numerically)
 * PARAMS:   authReq - said param
 * RETURN:   true if it is a valid value
 * NOTES:
 */
static bool gattClientVerifyAuthReq(uint8_t authReq)
{
    switch (authReq) {
    case GATT_CLI_AUTH_REQ_NONE:
    case GATT_CLI_AUTH_REQ_NO_MITM:
    case GATT_CLI_AUTH_REQ_MITM:
    case GATT_CLI_AUTH_REQ_SIGNED_NO_MITM:
    case GATT_CLI_AUTH_REQ_SIGNED_MITM:
        return true;
    default:
        return false;
    }
}

/*
 * FUNCTION: gattClientRead
 * USE:      Read an attribute (usually a characteristic or a descriptor)
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           handle - the handle to read
 *           authReq - authentication requirements (// TODO)
 *           offset - offset to start reading at
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientRead(void* userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uint16_t offset, uniq_t trans, gattCliReadCbk cbk)
{
    struct gattCliTodoItem *todo;

    if (!gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    pthread_mutex_lock(&mGattLock);
    if (offset)
        todo = gattCliTodoMakeReadBlob(conn, GATT_INTENT_READ, handle, authReq, offset);
    else
        todo = gattCliTodoMakeRead(conn, GATT_INTENT_READ, handle, authReq);
    if (todo) {
        todo->readCbk = cbk;
        todo->clientTrans = trans;
        todo->userData = userData;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientCharWrite
 * USE:      Write a characteristic
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           handle - the handle to read
 *           authReq - authentication requirements (// TODO)
 *           writeType - cmd? req? prepare? (GATT_CLI_WRITE_TYPE_*)
 *           ofst - write offset (used only for prepare write)
 *           data  -the data to write
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientWrite(void *userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, uniq_t trans, gattCliWriteCbk cbk)
{
    struct gattCliTodoItem *todo;
    uint8_t type;

    if (!gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    switch (writeType) {
    case GATT_CLI_WRITE_TYPE_SIGNED:
        type = GATT_CLI_TODO_SIGNED_WRITE;
        break;
    case GATT_CLI_WRITE_TYPE_WRITE_NO_RSP:
        type = GATT_CLI_TODO_WRITE_NO_ACK;
        break;
    case GATT_CLI_WRITE_TYPE_WRITE:
        type = GATT_CLI_TODO_WRITE;
        break;
    case GATT_CLI_WRITE_TYPE_PREPARE:
        type = GATT_CLI_TODO_PREPARE_WRITE;
        break;
    default:
        return GATT_CLI_STATUS_ERR;
    }

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeWrite(conn, GATT_INTENT_WRITE, type, handle, authReq, ofst, data);
    if (todo) {
        todo->userData = userData;
        todo->writeCbk = cbk;
        todo->clientTrans = trans;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientStagedWritesExecute
 * USE:      Execute or abandon previously staged writes
 * PARAMS:   connId - the connection id
 *           execute - true to commit them, false to abandon
 *           trans - transaction ID (unused, passed to callback)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientStagedWritesExecute(gatt_client_conn_t connId, bool execute, uniq_t trans, gattCliStagedWriteExecuteCb cbk)
{
    struct gattClientConnection *clicon;
    uint8_t ret = GATT_CLI_STATUS_ERR;
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    clicon = gattClientConnFindById(connId);
    if (!clicon)
        goto out;

    conn = gattConnFindById(clicon->gattConnId);
    if (!conn)
        goto out;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        goto out;

    todo->type = GATT_CLI_TODO_WRITE_EXEC;
    todo->execute = execute;
    todo->executeCbk = cbk;
    todo->clientTrans = trans;

    todo->connId = clicon->connId;
    gattCliTodoEnqItem(conn, todo, false);

out:
    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattClientNotifsSubscribe
 * USE:      Subscribe to a notification or indication
 * PARAMS:   connId - the connection id
 *           charHandle - the handle of the characteristic (header)
 *           charValueHandle - the handle of the characteristic value
 *           cccdHandle - the handle of the cccd
 *           reliable - use reliable delivery?
 *           stateCbk - callback to call when subscription state changes
 *           arrivedCbk - callback to call when notif/ind arrives
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientNotifsSubscribe(gatt_client_conn_t connId, uint16_t charHandle, uint16_t charValueHandle, uint16_t cccdHandle, bool reliable, gattCliNotifSubscribeStateCbk stateCbk, gattCliNotifArrivedCbk arrivedCbk)
{
    struct gattClientConnection *clicon;
    uint8_t sta = GATT_CLI_STATUS_ERR;
    struct gattCliNotifRec *nr;
    struct gattCliNotif *notif;
    struct gattConnState *conn;
    bool callStateCbk = false;

    pthread_mutex_lock(&mGattLock);
    clicon = gattClientConnFindById(connId);
    if (!clicon)
        logw("No client connection for subscribe\n");
    else {

        conn = gattConnFindById(clicon->gattConnId);
        if (!conn) {
           logw("No gatt for subscribe\n");
           goto out;
        }

        /* find the notif record, if one exists */
        for (nr = clicon->notifs; nr; nr = nr->next) {

            if (nr->notif->charHeaderHandle != charHandle)
                continue;
            if (nr->notif->charValueHandle != charValueHandle)
                continue;
            if (nr->notif->charConfigHandle != cccdHandle)
                continue;
            break;
        }
        if (nr) {
            logw("Duplicate subscribe\n");
            sta = GATT_CLI_STATUS_OK;
            goto out;
        }

        nr = calloc(1, sizeof(struct gattCliNotifRec));
        if (!nr) {
            loge("Allocation error of a notif rec\n");
            goto out;
        }
        nr->wantInd = reliable;
        nr->stateCbk = stateCbk;
        nr->arrivedCbk = arrivedCbk;

        /* find the notif structure, if it exists */
        for (notif = conn->cliNotifs; notif; notif = notif->next) {

            if (notif->charHeaderHandle != charHandle)
                continue;
            if (notif->charValueHandle != charValueHandle)
                continue;
            if (notif->charConfigHandle != cccdHandle)
                continue;
            break;
        }
        if (!notif) {

            logv("Nobody was previously subscribed to char at 0x%04x - making new struct\n", charHandle);

            /* no structure? create it */
            notif = calloc(1, sizeof(struct gattCliNotif));
            if (!notif) {
                free(nr);
                loge("Allocation error of a notif struct\n");
                goto out;
            }
            notif->charHeaderHandle = charHandle;
            notif->charValueHandle = charValueHandle;
            notif->charConfigHandle = cccdHandle;
            notif->curState = NOTIF_C_STATE_NOT_SUBBED;
            notif->locator = uniqGetNext();
            sta = GATT_CLI_STATUS_OK;
            notif->next = conn->cliNotifs;
            conn->cliNotifs = notif;
            if (notif->next)
                notif->next->prev = notif;
        } else {

            logd("Somebody was previously subscribed to char at 0x%04x - reusing struct\n", charHandle);

            /* see if we can already tell them it's ready */
            if (notif->curState == NOTIF_C_STATE_SUBBED_TO_IND || (!reliable && notif->curState == NOTIF_C_STATE_SUBBED_TO_NOTIF)) {
                callStateCbk = true;
                nr->subCallMade = true;
                sta = GATT_CLI_STATUS_OK;
            }
        }

        /* if needed, do [another] char descr write */
        if (!gattCliNotifSubUnsubIssueWriteIfNeededNewTodo(conn, notif)) {
            sta = GATT_CLI_STATUS_ERR;
            loge("Notif write enqueue failed\n");
            free(nr);
            nr = NULL;
            callStateCbk = false;
        }

        if (nr) {
            nr->notif = notif;
            if (reliable)
                notif->refCtInd++;
            else
                notif->refCtNotif++;

            nr->next = clicon->notifs;
            clicon->notifs = nr;
            if (nr->next)
                nr->next->prev = nr;
        }
    }

out:
    pthread_mutex_unlock(&mGattLock);

    if (callStateCbk)
        stateCbk(connId, charValueHandle, true, sta);

    return sta;
}

/*
 * FUNCTION: gattClientNotifsUnsubscribe
 * USE:      Unsubscribe from a notification or indication
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           thisChar - what characteristic?
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientNotifsUnsubscribe(gatt_client_conn_t conn, uint16_t charValueHandle)
{
    gattCliNotifSubscribeStateCbk stateCbk = NULL;
    struct gattClientConnection *clicon;
    uint8_t sta = GATT_CLI_STATUS_ERR;
    struct gattCliNotifRec *nr;

    pthread_mutex_lock(&mGattLock);
    clicon = gattClientConnFindById(conn);
    if (!clicon)
        logw("No connection for unsubscribe\n");
    else {

        /* find the notif record, if one exists */
        for (nr = clicon->notifs; nr; nr = nr->next) {

            if (nr->notif->charValueHandle != charValueHandle)
                continue;
            break;
        }
        if (!nr)
            logw("NotifRec not found for unsub\n");
        else {
            if (!nr->unsubCallMade)
                stateCbk = nr->stateCbk;
            nr->unsubCallMade = true;

            gattClientNotifUnsubscribe(clicon, nr);
            sta = GATT_CLI_STATUS_OK;
        }
    }
    pthread_mutex_unlock(&mGattLock);

    if (stateCbk)
        stateCbk(conn, charValueHandle, false, sta);

    return sta;
}

/*
 * FUNCTION: gattClientUtilSvcTraversalStructFindByTransId
 * USE:      find a service traversal state structure by a transaction id
 * PARAMS:   trans - the transaction Id to look for
 * RETURN:   structure pointer or NULL
 * NOTES:    call with mGattUtilLock held
 */
static struct GattCliUtilSvcTraverseState* gattClientUtilSvcTraversalStructFindByTransId(uniq_t trans)
{
    struct GattCliUtilSvcTraverseState* t;

    for (t = mUtilSvcEnumStates; t; t = t->next) {
        if (t->curTrans == trans)
            break;
    }

    return t;
}

/*
 * FUNCTION: gattClientUtilSvcTraversalStructUnlink
 * USE:      Unlink a service traversal state struct from the list
 * PARAMS:   state the service traversal state structure
 * RETURN:   NONE
 * NOTES:    call with mGattUtilLock held
 */
static void gattClientUtilSvcTraversalStructUnlink(struct GattCliUtilSvcTraverseState* state)
{
    // unlink from list
    if (state->prev)
        state->prev->next = state->next;
    else
        mUtilSvcEnumStates = state->next;
    if (state->next)
        state->next->prev = state->prev;
}

/*
 * FUNCTION: gattClientUtilSvcTraversalStructFreeData
 * USE:      Properly free a service traversal state structure's data
 * PARAMS:   state the service traversal state structure
 * RETURN:   NONE
 * NOTES:    call with mGattUtilLock held
 */
static void gattClientUtilSvcTraversalStructFreeData(struct GattCliUtilSvcTraverseState* state)
{
    uint32_t i;

    // free constituent parts & itself
    for (i = 0; i < state->res.numChars; i++)
        free(state->res.chars[i].descrs);
    free(state->res.inclSvcs);
    free(state->res.chars);
    free(state);
}

/*
 * FUNCTION: gattClientUtilSvcTraversalStructFree
 * USE:      Properly unlink & then free a service traversal state structure
 * PARAMS:   state the service traversal state structure
 * RETURN:   NONE
 * NOTES:    call with mGattUtilLock held
 */
static void gattClientUtilSvcTraversalStructFree(struct GattCliUtilSvcTraverseState* state)
{
    gattClientUtilSvcTraversalStructUnlink(state);
    gattClientUtilSvcTraversalStructFreeData(state);
}

/*
 * FUNCTION: gattClientUtilFindAndTraversePrimaryServiceDescrEnumCbk
 * USE:      Called by GATT code when we find a descriptor (or another attribute that looks like it)
 * PARAMS:   conn - the connection id
 *           trans - out transaction ID (matches to "->curTrans")
 *           uuid - the found uuid of the descriptor or NULL at end
 *           handle - the handle of the descriptor
 *           status - GATT status for this search
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static void gattClientUtilFindAndTraversePrimaryServiceDescrEnumCbk(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t handle, uint8_t status)
{
    struct GattCliUtilSvcTraverseState* st;
    gattCliUtilSvcTraversedCbk cbk = NULL;
    uniq_t clientTrans;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilSvcTraversalStructFindByTransId(trans);
    if (!st)
        logi("state for this transaction not found\n");
    else {
        if (status != GATT_CLI_STATUS_OK)
            logi("error finding descriptors");
        else if (uuid) { // a descriptor is potentially found

            struct GattTraversedServiceChar *inChr = NULL;
            bool discard;
            uint32_t i;

            logv("GATT util svc traversal: descr search produces "UUIDFMT" @ 0x%04x\n", UUIDCONV(*uuid), handle);

            // verify it is not something else (here we'll get "characteristic" declarations, characteristic values, "include service" declarations, and our actual "service" declaration)
            discard = handle == st->res.firstHandle;
            for (i = 0; i < st->res.numInclSvcs && !discard; i++) {
                discard = handle == st->res.inclSvcs[i].includeDefHandle;
            }
            // while doing that also find what char it is in
            for (i = 0; i < st->res.numChars && !discard; i++) {
                discard = handle == st->res.chars[i].firstHandle || handle == st->res.chars[i].valHandle;
                if (handle > st->res.chars[i].firstHandle && handle <= st->res.chars[i].lastHandle)
                    inChr = &st->res.chars[i];
            }
            if (discard) {
                logv("GATT util svc traversal: descr discarded\n");
                pthread_mutex_unlock(&mGattUtilLock);
                return;
            }

            if (!inChr)
                logi("This descriptor not in a characteristic!\n");
            else {
                struct GattTraversedServiceCharDescr *t = realloc(inChr->descrs, sizeof(struct GattTraversedServiceCharDescr) * (inChr->numDescrs + 1));

                if (!t)
                    logi("mem error\n");
                else {
                    inChr->descrs = t;
                    inChr->descrs[inChr->numDescrs].uuid = *uuid;
                    inChr->descrs[inChr->numDescrs].handle = handle;
                    inChr->numDescrs++;
                    pthread_mutex_unlock(&mGattUtilLock);
                    return;
                }
            }
        }
        else { // enum of descrs is done - tell user & then free data
            logd("GATT util svc traversal: descr search complete\n");

            // we unlink it from the list so we can drop the lock. We do not want to hold the lock during a callback. Once unliked it CANNOT disappear from under us
            gattClientUtilSvcTraversalStructUnlink(st);
            pthread_mutex_unlock(&mGattUtilLock);
            st->cbk(st->userData, conn, st->clientTrans, &st->res);
            gattClientUtilSvcTraversalStructFreeData(st);
            return;
        }
        // an error happened - clean up and tell user
        if (st) {
            clientTrans = st->clientTrans;
            cbk = st->cbk;
            gattClientUtilSvcTraversalStructFree(st);
        }
        pthread_mutex_unlock(&mGattUtilLock);
        if (cbk)
            cbk(st->userData, conn, clientTrans, NULL);
    }
}

/*
 * FUNCTION: gattClientUtilFindAndTraversePrimaryServiceCharEnumCbk
 * USE:      Called by GATT code when we find a characteristic
 * PARAMS:   conn - the connection id
 *           trans - out transaction ID (matches to "->curTrans")
 *           uuid - the found uuid of the characteristic or NULL at end
 *           charHandle - the handle of the "characteristic" definition
 *           valHandle - the handle of the characteristic's "value" attribute
 *           props - character properties as per GATT
 *           status - GATT status for this search
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static void gattClientUtilFindAndTraversePrimaryServiceCharEnumCbk(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t charHandle, uint16_t valHandle, uint8_t props, uint8_t status)
{
    struct GattCliUtilSvcTraverseState* st;
    gattCliUtilSvcTraversedCbk cbk = NULL;
    uniq_t clientTrans;
    uint32_t i, j;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilSvcTraversalStructFindByTransId(trans);
    if (!st)
        logi("state for this transaction not found\n");
    else {
        if (status != GATT_CLI_STATUS_OK)
            logi("error finding characteristics");
        else if (uuid) { // a characteristic is found
            logv("GATT util svc traversal: char search produces "UUIDFMT" {0x%04x,0x%04x,0x%02x}\n", UUIDCONV(*uuid), charHandle, valHandle, props);

            struct GattTraversedServiceChar *t = realloc(st->res.chars, sizeof(struct GattTraversedServiceChar) * (st->res.numChars + 1));
            if (!t)
                logi("mem error\n");
            else {
                st->res.chars = t;
                st->res.chars[st->res.numChars].uuid = *uuid;
                st->res.chars[st->res.numChars].charProps = props;
                st->res.chars[st->res.numChars].firstHandle = charHandle;
                st->res.chars[st->res.numChars].valHandle = valHandle;
                st->res.chars[st->res.numChars].lastHandle = st->res.lastHandle; // we'll calculate this properly later, but all chars DO end where service ends
                st->res.chars[st->res.numChars].numDescrs = 0;
                st->res.chars[st->res.numChars].descrs = NULL;
                st->res.numChars++;
                pthread_mutex_unlock(&mGattUtilLock);
                return;
            }
        }
        else { // enum of chars is done
            uint16_t firstHandle, lastHandle;

            logd("GATT util svc traversal: char search complete\n");

            // calculate char ends (this is N^2, but there are never too many chars so we can live with it). a char ends where next begins. last one ends where service ends
            for (i = 0; i < st->res.numChars; i++) {
                for (j = 0; j < st->res.numChars; j++) {
                    if (st->res.chars[i].firstHandle < st->res.chars[j].firstHandle && st->res.chars[i].lastHandle >= st->res.chars[j].firstHandle)
                        st->res.chars[i].lastHandle = st->res.chars[j].firstHandle - 1;
                }
                logv("GATT util svc traversal: char "UUIDFMT" range determined to be 0x%04x-0x%04x\n", UUIDCONV(st->res.chars[i].uuid), st->res.chars[i].firstHandle, st->res.chars[i].lastHandle);
            }

            // enum descriptors (doing it all at once is simpler, but we need to filter out non-descriptor things)

            st->curTrans = trans = uniqGetNext();
            firstHandle = st->res.firstHandle;
            lastHandle = st->res.lastHandle;
            pthread_mutex_unlock(&mGattUtilLock);
            status = gattClientEnumCharDescriptors(conn, firstHandle, lastHandle, trans, gattClientUtilFindAndTraversePrimaryServiceDescrEnumCbk);
            if (status == GATT_CLI_STATUS_OK)
                return;
            pthread_mutex_lock(&mGattUtilLock);
            st = gattClientUtilSvcTraversalStructFindByTransId(trans);
        }
        // an error happened - clean up and tell user
        if (st) {
            clientTrans = st->clientTrans;
            cbk = st->cbk;
            gattClientUtilSvcTraversalStructFree(st);
        }
        pthread_mutex_unlock(&mGattUtilLock);
        if (cbk)
            cbk(st->userData, conn, clientTrans, NULL);
    }
}

/*
 * FUNCTION: gattClientUtilFindAndTraversePrimaryServiceInclSvcEnumCbk
 * USE:      Called by GATT code when we find an included service
 * PARAMS:   conn - the connection id
 *           trans - out transaction ID (matches to "->curTrans")
 *           uuid - the found uuid of the included service or NULL at end
 *           includeDefHandle - the handle of the "include" definition
 *           firstHandle - the start of this included service's handle range
 *           lastHandle - the ed of this included service's handle range
 *           status - GATT status for this search
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static void gattClientUtilFindAndTraversePrimaryServiceInclSvcEnumCbk(gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, uint16_t includeDefHandle, uint16_t firstHandle, uint16_t lastHandle, uint8_t status)
{
    struct GattCliUtilSvcTraverseState* st;
    gattCliUtilSvcTraversedCbk cbk = NULL;
    uniq_t clientTrans;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilSvcTraversalStructFindByTransId(trans);
    if (!st)
        logi("state for this transaction not found\n");
    else {
        logd("GATT util svc traversal: include search produces return with status %u and uuid is %sNULL\n", status, uuid ? "non-" : "");

        if (status != GATT_CLI_STATUS_OK)
            logi("error finding included services");
        else if (uuid) { // an included service is found
            struct GattTraversedServiceInclSvc *t = realloc(st->res.inclSvcs, sizeof(struct GattTraversedServiceInclSvc) * (st->res.numInclSvcs + 1));
            if (!t)
                logi("mem error\n");
            else {
                st->res.inclSvcs = t;
                st->res.inclSvcs[st->res.numInclSvcs].uuid = *uuid;
                st->res.inclSvcs[st->res.numInclSvcs].includeDefHandle = includeDefHandle;
                st->res.inclSvcs[st->res.numInclSvcs].firstHandle = firstHandle;
                st->res.inclSvcs[st->res.numInclSvcs].lastHandle = lastHandle;
                st->res.numInclSvcs++;
                pthread_mutex_unlock(&mGattUtilLock);
                return;
            }
        }
        else { // enum of included services is done
            st->curTrans = trans = uniqGetNext();
            firstHandle = st->res.firstHandle;
            lastHandle = st->res.lastHandle;
            pthread_mutex_unlock(&mGattUtilLock);
            status = gattClientEnumCharacteristics(conn, firstHandle, lastHandle, trans, gattClientUtilFindAndTraversePrimaryServiceCharEnumCbk);
            if (status == GATT_CLI_STATUS_OK)
                return;
            pthread_mutex_lock(&mGattUtilLock);
            st = gattClientUtilSvcTraversalStructFindByTransId(trans);
        }
        // an error happened - clean up and tell user
        if (st) {
            clientTrans = st->clientTrans;
            cbk = st->cbk;
            gattClientUtilSvcTraversalStructFree(st);
        }
        pthread_mutex_unlock(&mGattUtilLock);
        if (cbk)
            cbk(st->userData, conn, clientTrans, NULL);
    }
}

/*
 * FUNCTION: gattClientUtilFindAndTraversePrimaryServiceSvcEnumCbk
 * USE:      Called by GATT code when we find the desired service (or find it does not exist)
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           trans - out transaction ID (matches to "->curTrans")
 *           uuid - the found uuid (should match "->res.uuid") or NULL at end
 *           primary - is it a primary service (we expect yes)
 *           firstHandle - the start of this service's handle range
 *           numHandles - how many handles the service contains
 *           status - GATT status for this search
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static void gattClientUtilFindAndTraversePrimaryServiceSvcEnumCbk(void* userData, gatt_client_conn_t conn, uniq_t trans, const struct uuid *uuid, bool primary, uint16_t firstHandle, uint16_t numHandles, uint8_t status)
{
    uint16_t lastHandle = firstHandle + numHandles - 1;
    struct GattCliUtilSvcTraverseState* st;
    gattCliUtilSvcTraversedCbk cbk = NULL;
    uniq_t clientTrans;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilSvcTraversalStructFindByTransId(trans);
    if (!st)
        logi("state for this transaction not found\n");
    else {
        if (status == GATT_CLI_STATUS_OK && uuid && uuidCmp(uuid, &st->res.uuid) && primary) {

            st->res.firstHandle = firstHandle;
            st->res.lastHandle = lastHandle;
            st->curTrans = trans = uniqGetNext();
            pthread_mutex_unlock(&mGattUtilLock);
            logv("GATT util svc traversal: service found at 0x%04x-0x%04x. Enumerating includes\n", firstHandle, lastHandle);
            status = gattClientEnumIncludedServices(conn, firstHandle, lastHandle, trans, gattClientUtilFindAndTraversePrimaryServiceInclSvcEnumCbk);
            if (status == GATT_CLI_STATUS_OK)
                return;
            pthread_mutex_lock(&mGattUtilLock);
            st = gattClientUtilSvcTraversalStructFindByTransId(trans);
        }

        // an error happened - clean up and tell user
        if (st) {
            clientTrans = st->clientTrans;
            cbk = st->cbk;
            gattClientUtilSvcTraversalStructFree(st);
        }
        pthread_mutex_unlock(&mGattUtilLock);
        if (cbk)
            cbk(st->userData, conn, clientTrans, NULL);
    }
}

/*
 * FUNCTION: gattClientUtilFindAndTraversePrimaryService
 * USE:      Perform all the work needed to completely traverse a service
 * PARAMS:   conn - the connection id
 *           svcUuid - the service uuid to find
 *           clientTrans - transaction ID (unused, passed to callback)
 *           cbk - callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:    if more than service with a given uuid exists, first is returned
 */
uint8_t gattClientUtilFindAndTraversePrimaryService(void* userData, gatt_client_conn_t conn, const struct uuid *svcUuid, uniq_t clientTrans, gattCliUtilSvcTraversedCbk cbk)
{
    struct GattCliUtilSvcTraverseState* st = (struct GattCliUtilSvcTraverseState*)calloc(1, sizeof(struct GattCliUtilSvcTraverseState));
    uniq_t trans;
    uint8_t ret;

    if (!st)
        return GATT_CLI_STATUS_ERR;

    st->userData = userData;
    st->res.uuid = *svcUuid;
    st->clientTrans = clientTrans;
    st->curTrans = trans = uniqGetNext();
    st->cbk = cbk;

    pthread_mutex_lock(&mGattUtilLock);
    st->next = mUtilSvcEnumStates;
    mUtilSvcEnumStates = st;
    if (st->next)
        st->next->prev = st;
    pthread_mutex_unlock(&mGattUtilLock);
    ret = gattClientFindService(NULL, conn, true, svcUuid, trans, gattClientUtilFindAndTraversePrimaryServiceSvcEnumCbk);
    if (ret != GATT_CLI_STATUS_OK) {
        pthread_mutex_lock(&mGattUtilLock);
        st = gattClientUtilSvcTraversalStructFindByTransId(trans);
        if(st)
            gattClientUtilSvcTraversalStructFree(st);
        pthread_mutex_unlock(&mGattUtilLock);
    }
    return ret;
}

/*
 * FUNCTION: gattClientUtilLongReadStructFindByTransId
 * USE:      find a long read state structure by a transaction id
 * PARAMS:   trans - the transaction id to look for
 * RETURN:   structure pointer or NULL
 * NOTES:    call with mGattUtilLock held
 */
static struct GattCliUtilLongReadState* gattClientUtilLongReadStructFindByTransId(uniq_t trans)
{
    struct GattCliUtilLongReadState* t;

    for (t = mUtilLongReadStates; t; t = t->next) {
        if (t->curTrans == trans)
            break;
    }

    return t;
}

/*
 * FUNCTION: gattClientUtilLongReadStructUnlink
 * USE:      Unlink a long read state state struct from the list
 * PARAMS:   state the long read state state structure
 * RETURN:   NONE
 * NOTES:    call with mGattUtilLock held
 */
static void gattClientUtilLongReadStructUnlink(struct GattCliUtilLongReadState* state)
{
    // unlink from list
    if (state->prev)
        state->prev->next = state->next;
    else
        mUtilLongReadStates = state->next;
    if (state->next)
        state->next->prev = state->prev;
}

/*
 * FUNCTION: gattClientUtilLongReadStructFreeData
 * USE:      Properly free a long read state state structure's data
 * PARAMS:   state the long read state state structure
 * RETURN:   NONE
 * NOTES:    call with mGattUtilLock held
 */
static void gattClientUtilLongReadStructFreeData(struct GattCliUtilLongReadState* state)
{
    // free constituent parts & itself
    if (state->dataSoFar)
        sgFree(state->dataSoFar);
    free(state);
}

/*
 * FUNCTION: gattClientUtilLongReadReadCbk
 * USE:      Called by GATT code when a reach chunk arrives
 * PARAMS:   userData - our data (unused here)
 *           conn - the connection id
 *           curTrans - current transaction ID used to look up this long read operation
 *           handle - the handle that was read
 *           status - GATT status of the read
 *           error - GATT error code if status is GATT_CLI_STATUS_ERR
 *           data - the data we got or NULL on EOF
 * RETURN:
 * NOTES:
 */
static void gattClientUtilLongReadReadCbk(void* userData, gatt_client_conn_t conn, uniq_t curTrans, uint16_t handle, uint8_t status, uint8_t error, sg data)
{
    struct GattCliUtilLongReadState* st;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilLongReadStructFindByTransId(curTrans);
    if (!st) {
        logi("state for this transaction not found\n");
        pthread_mutex_unlock(&mGattUtilLock);
        if (data)
            sgFree(data);
        return;
    }

    // unlikely, but why not check?
    if (handle != st->handle)
        logw("Handle mismatch is impossible with transaction match!\n");

    // if error, discard all data and tell user NO
    if (status != GATT_CLI_STATUS_OK) {
        if (st->dataSoFar)
            sgFree(st->dataSoFar);
        st->dataSoFar = NULL;
    }
    else if (data) { // some data in reply
        uint32_t rxedLen = sgLength(data);

        sgConcat(st->dataSoFar, data);
        if (rxedLen >= ATT_MTU_MIN_LE - 1) {
            /*
             * got data? ask for more if it is possibly up to MTU (MTU can change so
             * we only check for this as a heuristic, since some devices like Microsoft
             * Universal Keyboard model 1695 violate the spec and reply to blob reads
             * forever with a single byte), causing us to read them forever.
             */
            pthread_mutex_unlock(&mGattUtilLock);
            (void)gattClientUtilLongReadScheduleNextChunk(conn, curTrans, true);
            return;
        }
    }

    // only provide a real ATT_ERROR_* while |status| is no CATT_CLI_STATUS_OK. For the case where
    // the read reaches EOF, status would be GATT_CLI_STATUS_OK, but error code would be either
    // ATT_ERROR_ATTRIBUTE_NOT_LONG or ATT_ERROR_INVALID_OFFSET.
    error = (status == GATT_CLI_STATUS_OK ? ATT_ERROR_NONE : error);

    // done? tell caller
    gattClientUtilLongReadStructUnlink(st);
    pthread_mutex_unlock(&mGattUtilLock);
    st->cbk(st->userData, conn, st->clientTrans, st->handle, error, st->dataSoFar);
    st->dataSoFar = NULL; // callback owns it
    gattClientUtilLongReadStructFreeData(st);
}

/*
 * FUNCTION: gattClientUtilLongReadScheduleNextChunk
 * USE:      Schedule the next read chunk for a long read operation
 * PARAMS:   conn - the connection id
 *           curTrans - current transaction ID used to look up this long read operation
 *           needCallback - set to send callback in cases of error
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
static uint8_t gattClientUtilLongReadScheduleNextChunk(gatt_client_conn_t conn, uint32_t curTrans, bool needCallback)
{
    struct GattCliUtilLongReadState* st;
    uint8_t ret, authReq;
    uint32_t offset;
    uint16_t handle;
    uint8_t error = ATT_ERROR_NONE;

    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilLongReadStructFindByTransId(curTrans);
    if (!st) {
        logi("state for this transaction not found\n");
        pthread_mutex_unlock(&mGattUtilLock);
        return GATT_CLI_STATUS_ERR;
    }

    handle = st->handle;
    authReq = st->authReq;
    offset = sgLength(st->dataSoFar);
    st->curTrans = curTrans = uniqGetNext();
    pthread_mutex_unlock(&mGattUtilLock);

    if (offset != (uint16_t)offset) {
        logi("GATT util long read: offset too large\n");
        ret = GATT_CLI_STATUS_ERR;
        error = ATT_ERROR_INVALID_OFFSET;
    }
    else {
        ret = gattClientRead(st->userData, conn, handle, authReq, offset, curTrans, gattClientUtilLongReadReadCbk);
        if (ret == GATT_CLI_STATUS_OK)
            return GATT_CLI_STATUS_OK;
        logi("GATT util long read: failed to schedule a read: 0x%02x\n", ret);
        error = ATT_ERROR_UNLIKELY_ERROR;
    }

    //if we get this far, we failed - cleanup
    pthread_mutex_lock(&mGattUtilLock);
    st = gattClientUtilLongReadStructFindByTransId(curTrans);
    if (!st) {
        logi("Failed to delete struct since there is no more struct\n");
        pthread_mutex_unlock(&mGattUtilLock);
        return ret;
    }
    gattClientUtilLongReadStructUnlink(st);
    pthread_mutex_unlock(&mGattUtilLock);
    if (needCallback)
        st->cbk(st->userData, conn, st->clientTrans, st->handle, error, NULL);
    gattClientUtilLongReadStructFreeData(st);
    return ret;
}

/*
 * FUNCTION: gattClientUtilLongRead
 * USE:      Perform all the work needed to completely read a handle
 * PARAMS:   userData - caller's data which will be given with the callback
 *           conn - the connection id
 *           handle - the handle to read
 *           authReq - the authentication requirements
 *           clientTrans - transaction ID (unused, passed to callback)
 *           cbk - callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:    even handles cases of MTU changing during the read. Pessimistically does one extra read at end to avoid caring about MTUs
 */
uint8_t gattClientUtilLongRead(void* userData, gatt_client_conn_t conn, uint16_t handle, uint8_t authReq, uniq_t clientTrans, gattCliUtilLongReadCompletedCbk cbk)
{
    struct GattCliUtilLongReadState* st = (struct GattCliUtilLongReadState*)calloc(1, sizeof(struct GattCliUtilLongReadState));
    uniq_t trans;

    if (!st)
        return GATT_CLI_STATUS_ERR;

    st->dataSoFar = sgNew();
    if (!st->dataSoFar) {
        free(st);
        return GATT_CLI_STATUS_ERR;
    }

    st->userData = userData;
    st->handle = handle;
    st->authReq = authReq;
    st->clientTrans = clientTrans;
    st->curTrans = trans = uniqGetNext();
    st->cbk = cbk;

    pthread_mutex_lock(&mGattUtilLock);
    st->next = mUtilLongReadStates;
    mUtilLongReadStates = st;
    if (st->next)
        st->next->prev = st;
    pthread_mutex_unlock(&mGattUtilLock);
    return gattClientUtilLongReadScheduleNextChunk(conn, trans, false); // no need for callback this first time as we're sync
}

