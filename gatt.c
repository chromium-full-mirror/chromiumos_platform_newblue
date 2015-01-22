#include <stdlib.h>
#include <string.h>
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "sendQ.h"
#include "uuid.h"
#include "util.h"
#include "gatt.h"
#include "att.h"
#include "sdp.h"
#include "log.h"
#include "mt.h"
#include "bt.h"

//TODO: if multiple clients stage writes/try to commit them, we do not arbitrate that. Depending on android's use of this, we may have to
//This code RELIES on ATT layer having some heavy caching. Without that, this will be woefully inefficient!
//TODO: it woudl help some if we coudl have notifrefs linked in a list per clicon. we do not expect too many notifrecs, so it is not a necessity. but it woudl be nice


#define GATT_ITEM_TYPE_INCLUDE     1
#define GATT_ITEM_TYPE_CHAR_DECL   2
#define GATT_ITEM_TYPE_CHAR_VAL    3
#define GATT_ITEM_TYPE_CHAR_DESCR  4

#define GATT_HDR_HANDLES           1 /* handles required for a service header */
struct gattService;
struct gattCliNotif;
struct gattClient;
struct gattCliCon;

struct gattIncludeDefHdr {
    uint16_t startHandle;
    uint16_t endHandle;
    /* uuid here if it is a uuid-16 */
} __packed;

struct gattCharDefHdr {
    uint8_t charProps;
    uint16_t valHandle;
    /* uuid here */
} __packed;

/* we can have one transaction outstanding per connection. This list serializes them */
struct gattCliTodoItem {

    struct gattCliTodoItem   *next;
    uint8_t                   type; /* GATT_CLI_TODO_* */

    /* this describes what to do (per-type data) */
    union {
        struct {
            uint16_t          startHandle;
            uint16_t          endHandle;
            uint16_t          uuid16; /* for findByTypeValue only */
            sg                val;    /* for findByTypeValue only */
        } find;
        struct {
            uint16_t          handle;
            uint16_t          ofst;      /* used for read-blob */
            uint16_t          endHandle; /* used for read-by-type, read-by-group-type */
            struct uuid       type;      /* used for read-by-type, read-by-group-type */
        } read;
        struct {
            uint16_t          handle;
            sg                data;
            uint16_t          ofst; /* only used for prepare-write */
        } write;
        struct {
            bool              commit;
        } exec;
    };

    /* this describes why and whom for */
    int                       clientId;  /* higher level ID - unused by us */
    int                       connId;    /* higher level ID - unused by us */
    uint8_t                   reqIntent; /* GATT_INTENT_* */
    uint8_t                   reqStep;
    struct uuid               uuidSvc;
    struct uuid               uuidChr; // or included service
    struct uuid               uuidDsc;

    /* per-intent data */
    union {
        gattCliSvcEnumCbk                 enumSvcsCbk;
        gattCliEnumCharacteristicsCbk     enumCharsCbk;
        gattCliEnumCharDescrsCbk          enumCharDscrsCbk;
        gattCliStagedWriteExecuteCb       executeCbk;

        struct {
            gattCliEnumIncludedSvcsCbk    cbk;
            uint16_t                      curH; /* this and the next one used to save progress while looking for included uuid-128 services */
            uint16_t                      lastH;
            uint16_t                      inclStartH;
            uint16_t                      inclEndH;
        } enumInclSvcs;
        struct {
            gattCliCharReadCbk            cbk;
            uint8_t                       authReq;
        } read;
        struct {
            gattCliCharDescrReadCbk       cbk;
            uint8_t                       authReq;
        } readDescr;
        struct {
            gattCliCharWriteCbk           cbk;
            sg                            data;
            uint16_t                      ofst;
            uint8_t                       authReq;
            uint8_t                       writeType; //TODO: may not be needed
        } write;
        struct {
            gattCliCharDescrWriteCbk      cbk;
            sg                            data;
            uint16_t                      ofst;
            uint8_t                       authReq;
            uint8_t                       writeType; //TODO: may not be needed
        } writeDescr;
        struct {
            uint16_t                      charHdrH; //these are collected as we go along
            uint16_t                      charValH;
            uint8_t                       charProps;
        } notif;
    } intentData;
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
#define GATT_CLI_TOTO_WRITE_EXEC         10

#define GATT_INTENT_INTERNAL             0 /* not a user request but an internal one - no further ation needed */
#define GATT_INTENT_ENUM_SVCS            1
#define GATT_INTENT_ENUM_INCL_SVCS       2
#define GATT_INTENT_ENUM_CHARS           3
#define GATT_INTENT_ENUM_CHAR_DESCRS     4
#define GATT_INTENT_READ_CHAR            5
#define GATT_INTENT_WRITE_CHAR           6
#define GATT_INTENT_READ_DESCR           7
#define GATT_INTENT_WRITE_DESCR          8
#define GATT_INTENT_EXECUTE              9
#define GATT_INTENT_NOTIF_SUB            10
#define GATT_INTENT_NOTIF_UNSUB          11

#define GATT_INTENT_STEP_FIND_SVC        0
#define GATT_INTENT_STEP_FIND_CHAR       1
#define GATT_INTENT_STEP_FIND_DESCR      2
#define GATT_INTENT_STEP_GET_INCL_UUID   3 /* used to find UUID of included services that are not uuid-16 */
#define GATT_INTENT_STEP_FINAL           4 /* this step was the actual thing we expected to do. Action will depend on intent */



#define CACHE_DESCR_VAL_LEN              2

struct gattConnState {
    struct gattConnState     *next;
    struct gattConnState     *prev;

    struct gattCliNotif      *cliNotifs; /* things we as a client are subscribed to */
    struct bt_addr            addr;
    l2c_handle_t              conn;
    uint32_t                  mtu;
    bool                      encr;

    struct gattCliTodoItem   *todoListH;
    struct gattCliTodoItem   *todoListT;
    struct gattCliTodoItem   *todoCurr; /* or null if none */
    bool                      inFinalStep; /* used to syncronize who frees "todoCurr" */
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
    int                  svcRef;
    uint32_t             sdpHandle; /* zero if no current SDP record exists (not online) */

    gattSrvValueReadCbk  readF;
    gattSrvValueWriteCbk writeF;
    gattSrvIndCbk        indF;

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
    uint32_t                 refCtInd;         /* how many clients want indication */
    uint32_t                 refCtNotif;       /* how many clients want notification */
    bool                     ready;            /* set once actual subscription takes place */

    /* cache of relevant values for speed and convenience. spec promises these can never change */
    struct uuid              svc;              /* the uuid of the service */
    struct uuid              chr;              /* the uuid of the char */
    uint16_t                 charHeaderHandle; /* first handle for this characteristic */
    uint16_t                 charValueHandle;  /* characteristic's actual value's handle */
    uint16_t                 charConfigHandle; /* characteristic's "config" handle for unsubscribing */
    bool                     supportsInds;
    bool                     supportsNotifs;
    bool                     isInd;            /* are we currently subscribed to inds [or notifs?] */
};

struct gattCliNotifRec { /* used to keep track of all notifications that a client is subscribed to */
    struct gattCliNotifRec       *next;
    struct gattCliNotifRec       *prev;
    struct gattCliNotif          *notif;
    bool                          wantInd; /* else we'll ask for notif */
    gattCliNotifArrivedCbk        arrivedCbk;
    gattCliNotifSubscribeStateCbk subStateCbk; /* only set if we haven't yet called it */
};

struct gattClient {
    struct gattClient       *next;
    struct gattClient       *prev;
    struct uuid              uuid;
    int                      clientId; /* used by our callers */
};

struct gattCliCon { /* many-to-many table between gatt clients and gatt client connections */
    struct gattCliCon       *next;
    struct gattCliCon       *prev;
    struct gattClient       *cli;
    struct gattConnState    *conn;
    struct gattCliNotifRec  *notifs;
    int                      connId; /* used by our callers */
    gattCliConnectResultCbk  connCbk;
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
            int                      svcRef;
            int                      cid;
            int                      transId;
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
            int                      svcRef;
            int                      cid;
            uint16_t                 handle;
            uint8_t                  evt;
        } srvIndCall;
        struct {
            gattCliConnectResultCbk  cbk;
            int                      clientId;
            int                      connId;
            struct bt_addr           addr;
            uint8_t                  status;
        } cliConnStateNotif;
        struct {
            l2c_handle_t             conn;
            struct gattCliTodoItem  *todo;
        } cliDoNext;
        struct {
            gattCliNotifArrivedCbk   cbk;
            int                      cli;
            int                      con;
            struct uuid              svc;
            struct uuid              chr;
            bool                     ind;
            sg                       data;
        } cliIndCall;
        struct {
            gattCliNotifSubscribeStateCbk  cbk;
            int                      cli;
            int                      con;
            struct uuid              svc;
            struct uuid              chr;
            bool                     forSub;
            uint8_t                  stat;
        } cliSubUnsubCall;
    };
};

#define GATT_WORK_SRV_READ_CALL          0
#define GATT_WORK_SRV_WRITE_CALL         1
#define GATT_WORK_SRV_IND_CALL           2
#define GATT_WORK_CLI_CONN_STATE_NOTIF   3
#define GATT_WORK_CLI_DO_NEXT            4 /* run the next todo item, if none inflight currently */
#define GATT_WORK_CLI_IND_CALL           5
#define GATT_WORK_CLI_SUB_UNSUB_CALL     6


/* our state */
static struct sendQ* mSendQ;
static struct workQueue* mWorkQ;
static pthread_t mWorker;

static pthread_mutex_t mGattLock = PTHREAD_MUTEX_INITIALIZER;
static struct gattConnState *mConns = NULL;
static struct gattService *mSvcs = NULL;
static struct gattClient *mClients = NULL;
static struct gattCliCon *mCliCons = NULL;


/* fwd decls */
static void gattCliTodoEnqItem(struct gattConnState *conn, struct gattCliTodoItem *todo, bool inFront);
static void gattCliTodoNext(l2c_handle_t conn, struct gattCliTodoItem *now);
static void gattTodoItemFree(struct gattCliTodoItem *todo); 
static void gattWorkItemFree(struct gattWorkItem *wi);


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
        free(tC);
    }

    if (state->todoCurr)
        free(state->todoCurr);

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
static struct gattService* gattServiceFindBySvcRef(int svcRef)
{
    struct gattService *svc = mSvcs;

    while (svc && svcRef != svc->svcRef)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattCliconFindById
 * USE:      Find a gatt client-connection structure by connId
 * PARAMS:   connId - the connection id
 * RETURN:   client struct-connection or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattCliCon* gattCliconFindById(int connId)
{
    struct gattCliCon *clicon = mCliCons;

    while (clicon && connId != clicon->connId)
        clicon = clicon->next;

    return clicon;
}
/*
 * FUNCTION: gattClientFindById
 * USE:      Find a gatt client by clientId
 * PARAMS:   clientId - the client id
 * RETURN:   client struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattClient* gattClientFindById(int clientId)
{
    struct gattClient *cli = mClients;

    while (cli && clientId != cli->clientId)
        cli = cli->next;

    return cli;
}

/*
 * FUNCTION: gattClientFindByUuid
 * USE:      Find a gatt client by the client uuid (used to prevent duplciate clients and not much more)
 * PARAMS:   uuid - the client uuid
 * RETURN:   client struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattClient* gattClientFindByUuid(const struct uuid *uuid)
{
    struct gattClient *cli = mClients;

    while (cli && memcmp(&cli->uuid, uuid, sizeof(struct uuid)))
        cli = cli->next;

    return cli;
}

/*
 * FUNCTION: gattServiceFindByRangeRef
 * USE:      Find a gatt service by att range reference
 * PARAMS:   svcRef - the service reference
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
static bool gattEnqueueSrvRwCall(uint8_t workType, struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
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
static bool gattEnqueueSrvReadCall(struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
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
static bool gattEnqueueSrvWriteCall(struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
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
static bool gattEnqueueSrvIndCall(struct gattService *svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref)
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

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueCliConnStatusCall
 * USE:      Enqueue a work item to make a call to the higher layer for a result of sending a GATT connect/disconnetc
 * PARAMS:   clicon - the client-connection structure
 *           status - the status to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliConnStatusCall(struct gattCliCon *clicon, uint8_t status)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));

    if (wi) {
        wi->type = GATT_WORK_CLI_CONN_STATE_NOTIF;
        wi->cliConnStateNotif.cbk = clicon->connCbk;
        wi->cliConnStateNotif.clientId = clicon->cli->clientId;
        wi->cliConnStateNotif.connId = clicon->connId;
        wi->cliConnStateNotif.status = status;
        memcpy(&wi->cliConnStateNotif.addr, &clicon->conn->addr, sizeof(struct bt_addr));

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueCliDoNextTodoItem
 * USE:      Enqueue a work item to perform the next todo item if none is currently inflight for this connection
 * PARAMS:   conn - the connection structure
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliDoNextTodoItem(struct gattConnState *conn)
{
    struct gattCliTodoItem *now;
    struct gattWorkItem *wi;

    if (conn->todoCurr) {
        logd("gattEnqueueCliDoNextTodoItem called with an existing transaction running - not necessary\n");
        return true;
    }
    if (!conn->todoListH) {
        logd("gattEnqueueCliDoNextTodoItem called with no todo items - not necessary\n");
        return true;
    }

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {
        now = conn->todoListH;

        wi->type = GATT_WORK_CLI_DO_NEXT;
        wi->cliDoNext.conn = conn->conn;
        wi->cliDoNext.todo = now;

        if (workQueuePut(mWorkQ, wi)) {
            conn->todoListH = conn->todoListH->next;
            if (!conn->todoListH)
                conn->todoListT = NULL;
            conn->todoCurr = now;

            return true;
       }
    }

    return false;
}

/*
 * FUNCTION: gattEnqueueCliIndCall
 * USE:      Enqueue a work item to perform a notif/ind callback to the caller
 * PARAMS:   clicon - client-connection struct
 *           nr - the notification record
 *           data - the data to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliIndCall(struct gattCliCon *clicon, struct gattCliNotifRec *nr, sg data)
{
    struct gattWorkItem *wi;

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {

        wi->type = GATT_WORK_CLI_IND_CALL;
        wi->cliIndCall.cbk = nr->arrivedCbk;
        wi->cliIndCall.cli = clicon->cli->clientId;
        wi->cliIndCall.con = clicon->connId;
        memcpy(&wi->cliIndCall.svc, &nr->notif->svc, sizeof (struct uuid));
        memcpy(&wi->cliIndCall.chr, &nr->notif->chr, sizeof (struct uuid));
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
 * PARAMS:   clicon - client-connection struct
 *           nr - the notification record
 *           cbk - the callback to call (passed directly as it is not always the one in nr that we want to call)
 *           forSub - is this call for a subscribe or an unsubscribe request
 *           stat - the statsu value to send
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueCliSubUnsubCall(struct gattCliCon *clicon, struct gattCliNotifRec *nr, gattCliNotifSubscribeStateCbk cbk, bool forSub, uint8_t stat)
{
    struct gattWorkItem *wi;

    wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));
    if (wi) {

        wi->type = GATT_WORK_CLI_SUB_UNSUB_CALL;
        wi->cliSubUnsubCall.cbk = cbk;
        wi->cliSubUnsubCall.cli = clicon->cli->clientId;
        wi->cliSubUnsubCall.con = clicon->connId;
        memcpy(&wi->cliSubUnsubCall.svc, &nr->notif->svc, sizeof (struct uuid));
        memcpy(&wi->cliSubUnsubCall.chr, &nr->notif->chr, sizeof (struct uuid));
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
 * PARAMS:   who - whom the indidation/notification was sent to
 *           cid - the connection Id to pass back to ATT
 *           rangeRef - the range this is related to
 *           offset - the value in the range
 *           evt - ATT_SRV_EVT_NOTIF_*
 *           ref - the reference passed to attSrvValueNotifyChanged() initially
 *           success - did we succeed?
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvIndCbk(l2c_handle_t to, int cid, att_range_t rangeRef, uint16_t offset, uint8_t evt, uint64_t ref)
{
    uint8_t err = ATT_ERROR_NONE;
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
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
 * PARAMS:   who - whom the indidation/notification was sent to
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
static bool gattAttSvrReadCbk(l2c_handle_t who, int cid, att_range_t rangeRef, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
{
    uint8_t err = ATT_ERROR_NONE;
    struct gattService *svc;
    struct gattItem *gi;
    uint8_t *ptr = NULL;
    uint16_t len = 0;
    uint16_t tmp;
    uint8_t buf[32];

    pthread_mutex_lock(&mGattLock);
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
 * FUNCTION: gattAttSvrWriteCbk
 * USE:      ATT calls this to write an attribute
 * PARAMS:   who - whom the indidation/notification was sent to
 *           rangeRef - the range this is related to
 *           cid - the connection Id to pass back to ATT
 *           ofst - the value in the range
 *           transId - transaction ID
 *           byteOfst - how mnay bytes into the characteristic we want to write
 *           reason - read reason (ATT_WRITE_FOR_*)
 *           len - bytes to write
 *           data - the data
 * RETURN:   false on immediate failure
 * NOTES:    when write completes, a call to attSrvCbkReply() should be made
 */
static bool gattAttSvrWriteCbk(l2c_handle_t who, int cid, att_range_t rangeRef, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    /* attributes GATT creates internally for the user {service headers, includes, characteristic headers} are not writeable. All write pass to user */
    uint8_t err = ATT_ERROR_NONE;
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc) {
        logw("Failed to find service for read\n");
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    }

    if (!gattEnqueueSrvWriteCall(svc, who, cid, transId, ofst + attSrvHandleRangeGetBase(svc->attRange), byteOfst, reason, len, data)) {
        attSrvCbkReply(cid, transId, 0, err, NULL, 0);
        goto out;
    }

out:
    pthread_mutex_unlock(&mGattLock);
    return true;
}

/*
 * FUNCTION: gattAttExecWriteCbk
 * USE:      ATT calls this when client asks us to execute a write
 * PARAMS:   who - whom the indidation/notification was sent to
 *           cid - the connection Id to pass back to ATT
 *           transId - the transcationID
 *           exec - execute or dump data?
 * RETURN:   NONE
 * NOTES:
 */
static bool gattAttExecWriteCbk(l2c_handle_t who, int cid, int transId, bool exec)
{
    //TODO
    return false;
}

/*
 * FUNCTION: gattAttMtuCbk
 * USE:      ATT calls this with results of MTU negotiation
 * PARAMS:   who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttMtuCbk(l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState* state;

    pthread_mutex_lock(&mGattLock);
    state = gattConnFindByL2cConn(who);
    if (!state)
        loge("Unknown connection for MTU change event\n");
    else if (state->mtu > mtu)
        loge("MTU decrease not allowed\n");
    else
       state->mtu = mtu;
    pthread_mutex_unlock(&mGattLock);
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
        if (state->conn != conn) /* when we support outbound connections, this code will need to evolve */
            loge("Handle change!!!\n");
        break;

    case L2C_STATE_MTU:
        if (len != sizeof(uint16_t)) {
            loge("invalid length for mtu event\n");
            break;
        }
        mtu = *(uint16_t*)data;
        attSetMtu(state->conn, mtu);
        gattAttMtuCbk(state->conn, mtu);
        break;

    case L2C_STATE_ENCR:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encr event\n");
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
        attNotifConnClose(state->conn);
        pthread_mutex_lock(&mGattLock);
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
 * FUNCTION: gattStateAlloc
 * USE:      Allocate a GATT state for an L2C link
 * PARAMS:   conn - the connection handle
 *           instanceP - we store per-connection instance here
 *           addr - the peer's address
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t gattStateAlloc(l2c_handle_t conn, void **instanceP, const struct bt_addr *addr)
{
    struct gattConnState *state = (struct gattConnState*)calloc(1, sizeof(struct gattConnState));

    if (!state)
        return SVC_ALLOC_FAIL_OTHER;

    state->conn = conn;
    state->mtu = BT_ADDR_IS_LE(*addr) ? ATT_MTU_MIN_LE : ATT_MTU_MIN_EDR;
    memcpy(&state->addr, addr, sizeof(struct bt_addr));
    /* any other init here */

    pthread_mutex_lock(&mGattLock);
    state->next = mConns;
    if (mConns)
        mConns->prev = state;
    mConns = state;
    pthread_mutex_unlock(&mGattLock);

    *instanceP = state;
    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: gattPsmSvcAlloc
 * USE:      L2C calls this with remote requests to open a PSM-based channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
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

    return gattStateAlloc(handle, instanceP, &peer);
}

/*
 * FUNCTION: gattFixedChSvcAlloc
 * USE:      L2C calls this with requests to open a fixed channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t gattFixedChSvcAlloc(void *userData, l2c_handle_t handle, void **instanceP)
{
    struct bt_addr peer;

    /* if we cannot get peer address, do not bother with anything else */
    if (!l2cApiGetBtAddr(handle, &peer))
        return SVC_ALLOC_FAIL_OTHER;

    /* we do not support FixedCh access to GATT over EDR */
    if (BT_ADDR_IS_EDR(peer))
        return SVC_ALLOC_FAIL_OTHER;

    return gattStateAlloc(handle, instanceP, &peer);
}

/*
 * FUNCTION: gattSdpInit
 * USE:      Called to add a GATT profile to SDP
 * PARAMS:   uuid - the uuid
 *           firstHandle - the first handle
 *           numHandles - how many handles the profile has
 * RETURN:   sdp handle or 0 on error
 * NOTES:
 */
static uint32_t gattSdpAdd(struct uuid *uuid, uint16_t firstHandle, uint16_t numHandles)
{
    const uint16_t lastHandle = firstHandle + numHandles - 1;
    static const uint8_t hdrUuid16[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2)
    };
    static const uint8_t hdrUuid128[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 17,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_16)
    };
    uint8_t footer[] = {
        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 19,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(GATT_PSM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 9,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_ATT),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(firstHandle),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(lastHandle),

        //browse group list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),
    };
    uint8_t descr[sizeof(hdrUuid16) + sizeof(hdrUuid128) + sizeof(footer)]; /* surely big enough */
    uint16_t uuid16;
    uint32_t len;

    if (uuidToUuid16(&uuid16, uuid)) {
        memcpy(descr, hdrUuid16, len = sizeof(hdrUuid16));
        utilSetBE16(descr + len, uuid16);
        len += sizeof(uuid16);
    } else {
        memcpy(descr, hdrUuid128, len = sizeof(hdrUuid128));
        uuidWriteBE(descr + len, uuid);
        len += sizeof(struct uuid);
    }
    memcpy(descr + len, footer, sizeof(footer));
    len += sizeof(footer);

    return sdpServiceDescriptorAdd(descr, len);
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
void gattSrvCbkReply(int cid, int transId, uint16_t handle, uint8_t err, const void *data, uint16_t len)
{
    attSrvCbkReply(cid, transId, handle, err, data, len);
}

/*
 * FUNCTION: gattTodoItemFree
 * USE:      Properly free a todo item for GATT
 * PARAMS:   todo - the todo item to free
 * RETURN:   NONE
 * NOTES:
 */
static void gattTodoItemFree(struct gattCliTodoItem *todo)
{
    switch (todo->type) {
    case GATT_CLI_TODO_FIND_BY_TYPE_VAL:
        if (todo->find.val) /* we clear it when we use it */
            sgFree(todo->find.val);
        break;
    case GATT_CLI_TODO_WRITE:
    case GATT_CLI_TODO_WRITE_NO_ACK:
    case GATT_CLI_TODO_SIGNED_WRITE:
    case GATT_CLI_TODO_PREPARE_WRITE:
        if (todo->write.data) /* we clear it when we use it */
            sgFree(todo->write.data);
        break;
    }

    switch (todo->reqIntent) {
    case GATT_INTENT_WRITE_CHAR:
        if (todo->intentData.write.data) /* we claer it whenwe use it */
            sgFree(todo->intentData.write.data);
        break;
    case GATT_INTENT_WRITE_DESCR:
        if (todo->intentData.writeDescr.data) /* we claer it whenwe use it */
            sgFree(todo->intentData.writeDescr.data);
        break;
    }

    free(todo);
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
    if (wi->type == GATT_WORK_CLI_DO_NEXT)
        gattTodoItemFree(wi->cliDoNext.todo);
    else if (wi->type == GATT_WORK_CLI_IND_CALL)
        sgFree(wi->cliIndCall.data);
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
    struct gattConnState *conn;
    struct gattCliCon *clicon;
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
        case GATT_WORK_CLI_CONN_STATE_NOTIF:
            wi->cliConnStateNotif.cbk(wi->cliConnStateNotif.clientId, wi->cliConnStateNotif.connId, &wi->cliConnStateNotif.addr, wi->cliConnStateNotif.status);
            break;
        case GATT_WORK_CLI_DO_NEXT:
            gattCliTodoNext(wi->cliDoNext.conn, wi->cliDoNext.todo);
            break;
        case GATT_WORK_CLI_IND_CALL:
            wi->cliIndCall.cbk(wi->cliIndCall.cli, wi->cliIndCall.con, &wi->cliIndCall.svc, &wi->cliIndCall.chr, wi->cliIndCall.ind, wi->cliIndCall.data);
            break;
        case GATT_WORK_CLI_SUB_UNSUB_CALL:
            wi->cliSubUnsubCall.cbk(wi->cliSubUnsubCall.cli, wi->cliSubUnsubCall.con, &wi->cliSubUnsubCall.svc, &wi->cliSubUnsubCall.chr, wi->cliSubUnsubCall.forSub, wi->cliSubUnsubCall.stat);
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
 * FUNCTION: gattCliFinalStepSvcIncludeFound
 * USE:      Called when a final step results arrive for finding a service include
 * PARAMS:   conn - the conenction structure
 *           todo - the to-do item
 *           startHandle - the handle in the reply
 *           endHandle - the end group handle in the reply
 *           uu - uuid
 *           haveMore - is there more in this same reply packet?
 *           killTodoItemP - set to true to destroy the todo list item. false to keep it (only makes sense if you re-enqueued it). only used if func itself returns false or haveMore is false
 * RETURN:   true to allow further calls here false to stop (in cases of multiple replies in a radio packet). ignored if haveMore is false
 * NOTES:    this is a separate func since we can get here via two ways (depending on whether the included servive has a uuid-16 or a uuid-128)
 */
static bool gattCliFinalStepSvcIncludeFound(struct gattConnState *conn, struct gattCliTodoItem *todo, uint16_t startHandle, uint16_t endHandle, const struct uuid *uu, bool haveMore, bool *killTodoItemP)
{
    if (uuidIsZero(&todo->uuidChr)) { //case 3
        todo->intentData.enumInclSvcs.cbk(todo->clientId, todo->connId, &todo->uuidSvc, uu, startHandle, endHandle - startHandle + 1, GATT_CLI_STATUS_OK);
        *killTodoItemP = true;
        return false;
    } else { //cases 2 and 1
        if (uuidCmp(&todo->uuidChr, uu)) //case 2
            uuidZero(&todo->uuidChr);

        /* go on searching */
        if (!haveMore) {

            if (todo->intentData.enumInclSvcs.curH == todo->intentData.enumInclSvcs.lastH) { //done
                todo->intentData.enumInclSvcs.cbk(todo->clientId, todo->connId, &todo->uuidSvc, NULL, 0, 0, GATT_CLI_STATUS_OK);
                *killTodoItemP = true;
                return false;
            }

            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ_BY_TYPE;
            todo->read.handle = todo->intentData.enumInclSvcs.curH + 1;
            todo->read.endHandle = todo->intentData.enumInclSvcs.lastH;
            uuidFromUuid16(&todo->read.type, GATT_UUID_INCLUDE);

            pthread_mutex_lock(&mGattLock);
            gattCliTodoEnqItem(conn, todo, false);
            pthread_mutex_unlock(&mGattLock);
        }
    }
    return true;
}

/*
 * FUNCTION: gettCliNotifSubDone
 * USE:      Called when the final step of notification subscription completes
 * PARAMS:   conn - the connection structure
 *           todo - the to-do item
 *           cfgHandle - the handle for the configuration descriptor
 * RETURN:   true to save the todo item, false to delete it
 * NOTES:    takes mGattLock. we come here twice. once after we found the descriptor and ocne when we're written it. yes, a bit messy
 */
static bool gettCliNotifSubDone(struct gattConnState *conn, struct gattCliTodoItem *todo, uint16_t cfgHandle)
{
    struct gattCliNotif *notif;
    uint16_t cfgToWrite = 0;

    pthread_mutex_lock(&mGattLock);

    if (todo->type == GATT_CLI_TODO_WRITE) { /* we finished the write and are really done */

        /* find the notif structure */
        for (notif = conn->cliNotifs; notif && (!uuidCmp(&todo->uuidSvc, &notif->svc) || !uuidCmp(&todo->uuidChr, &notif->chr)); notif = notif->next);
        if (!notif)
            logw("No notif structure at subscribe done.2 time\n");
        else if (notif->ready)
            logw("Notif already ready at subscribe done.2 time\n");
        else if (notif->refCtInd && !notif->isInd) /* an ind request came in while we werr busy subscribing to notifs. time to resubscribe */
            cfgToWrite = GATT_CLI_CFG_BIT_INDS;
        else if (notif->refCtInd || notif->refCtNotif) { /* if we still have clients - tell them all that we're ready! */
 
            struct gattCliCon *clicon;
            struct gattCliNotifRec *nr, *t;

            for (clicon = mCliCons; clicon; clicon = clicon->next) {
                if (clicon->conn != conn) /* we only care about this connection */
                    continue;
                for (nr = clicon->notifs; nr; nr = nr->next) {
                    if (nr->notif != notif) /* we only care about this characteristic's notifs/inds */
                        continue;
                    if (!nr->subStateCbk) /* only send messages to the deserving requestors */
                        continue;

                    /* send OK message */
                    if (!gattEnqueueCliSubUnsubCall(clicon, nr, nr->subStateCbk, true, GATT_CLI_STATUS_OK))
                        loge("Cannot send sub OK message for subscribe to notifs/inds\n");
                }
            }

            notif->ready = true;
        }

    } else { /* we found the descriptor but havent written yet */

        /* find the notif structure */
        for (notif = conn->cliNotifs; notif && (!uuidCmp(&todo->uuidSvc, &notif->svc) || !uuidCmp(&todo->uuidChr, &notif->chr)); notif = notif->next);
        if (!notif)
            logw("No notif structure at subscribe done.1 time\n");
        else if (notif->ready)
            logw("Notif already ready at subscribe done.1 time\n");
        else if (notif->refCtInd || notif->refCtNotif) {
            bool notifAvail = false;
            bool indAvail = false;

            indAvail = !!(todo->intentData.notif.charProps & GATT_PROP_INDICATE);
            notifAvail = !!(todo->intentData.notif.charProps & GATT_PROP_NOTIFY);

            notif->charHeaderHandle = todo->intentData.notif.charHdrH;
            notif->charValueHandle = todo->intentData.notif.charValH;
            notif->charConfigHandle = cfgHandle;
            notif->supportsInds = indAvail;
            notif->supportsNotifs = notifAvail;

            /* now decide what to do */
            if (indAvail && (notif->refCtInd || !notifAvail)) {
                notif->isInd = true;
                cfgToWrite = GATT_CLI_CFG_BIT_INDS;
            } else if (notifAvail)
                cfgToWrite = GATT_CLI_CFG_BIT_NOTIFS;

            /* tell all clients with impossible requests that they are SOL */
            if (!indAvail) {
                struct gattCliCon *clicon;
                struct gattCliNotifRec *nr, *t;

                for (clicon = mCliCons; clicon; clicon = clicon->next) {
                    if (clicon->conn != conn) /* we only care about this connection */
                        continue;
                    for (nr = clicon->notifs; nr;) {
                        if (nr->notif != notif) { /* we only care about this characteristic's notifs/inds */
                            nr = nr->next;
                            continue;
                        }
                        if (!nr->wantInd && notifAvail) { /* only send SOL messages to the deserving requestors */
                            nr = nr->next;
                            continue;
                        }

                        /* send SOL message */
                        if (!nr->subStateCbk || !gattEnqueueCliSubUnsubCall(clicon, nr, nr->subStateCbk, true, GATT_CLI_STATUS_ERR))
                            loge("Cannot send SOL message for failure to subscribe to notifs/inds\n");

                        /* adjust refcount */
                        if (nr->wantInd)
                            notif->refCtInd--;
                        else
                            notif->refCtNotif--;

                        /* unlink and free */
                        if (nr->next)
                            nr->next->prev = nr->prev;
                        if (nr->prev)
                            nr->prev->next = nr->next;
                        else
                            clicon->notifs = nr->next;
                        t = nr;
                        nr = nr->next;
                        free(t);
                    }
                }
            }
        }
    }

    /* if we somehow have no [more] clients, delete this struct */
    if (!notif->refCtInd && !notif->refCtNotif) {
        /* delete notif struct since nobody wants it anymore */
        if (notif->prev)
            notif->prev->next = notif->next;
        else
            conn->cliNotifs = notif->next;
        if (notif->next)
            notif->next->prev = notif->prev;
        free(notif);
    }

    pthread_mutex_unlock(&mGattLock);

    if (cfgToWrite) {
        uint8_t buf[sizeof(cfgToWrite)];
        sg writeVal;

        utilSetLE16(buf, cfgToWrite);
        writeVal = sgNewWithCopyData(buf, sizeof(buf));

        if (!writeVal)
            loge("Failed to alloc SG for notif/ind sub. You are probably screwed\n");
        else {

            todo->type = GATT_CLI_TODO_WRITE;
            todo->write.handle = cfgHandle;
            todo->write.data = writeVal;
            return true;
        }
    }

    return false;
}

/*
 * FUNCTION: gettCliNotifUnsubDone
 * USE:      Called when the final step of notification unsubscription completes
 * PARAMS:   conn - the connection structure
 *           todo - the to-do item
 * RETURN:   NONE
 * NOTES:    takes mGattLock
 */
static void gettCliNotifUnsubDone(struct gattConnState *conn, struct gattCliTodoItem *todo)
{
    //todo: free state structs?
    //todo: tell everyone
}

/*
 * FUNCTION: gattCliFinalStep
 * USE:      Called when a final step results arrive
 * PARAMS:   conn - the connection structure
 *           todo - the to-do item
 *           handle - the handle in the reply or NULL if none
 *           endGrpHandle - the end group handle in the reply or NULL if none
 *           uu - uuid (if relevant for this kind of request)
 *           data - if relevant for this kind of request
 *           haveMore - is there more in this same reply packet?
 *           killTodoItem - set to true to destroy the todo list item. false (or leave as is) to keep it (only makes sense if you re-enqueued it). only used if func itself returns false or haveMore is false
 * RETURN:   true to allow further calls here false to stop (in cases of multiple replies in a radio packet). ignored if haveMore is false
 * NOTES:
 */
static bool gattCliFinalStep(struct gattConnState *conn, struct gattCliTodoItem *todo, const uint16_t *handle, const uint16_t *endGrpHandle, const struct uuid *uu, sg data, bool haveMore, bool *killTodoItem)
{
    struct gattIncludeDefHdr inclDef;
    struct gattCharDefHdr charDef;
    struct uuid uuid;
    uint16_t uuid16, inclStartH, inclEndH;
    uint8_t aByte;

    switch (todo->reqIntent) {
    case GATT_INTENT_INTERNAL:
        /* nothing to do here, really */
        if (data) {
            loge("Unexpected data for internal intent completion. Would leak\n");
            sgFree(data);
        }
        *killTodoItem = true;
        return false;

    case GATT_INTENT_ENUM_SVCS: /* we get a call for each found service. callbacks are ok here (we are in ATT's thread and hold no locks) */
        if (!attReadUuid(&uuid, data)) {
            sgFree(data);
            logw("Service definition is not a uuid\n");
            *killTodoItem = true;
            return false;
        }
        sgFree(data);

        if (uuidIsZero(&todo->uuidSvc) || uuidCmp(&todo->uuidSvc, &uuid))
            todo->intentData.enumSvcsCbk(todo->clientId, todo->connId, &uuid, true /*we always find a primary here. Is that wrong? */, *handle, *endGrpHandle - *handle + 1, GATT_CLI_STATUS_OK);

        if (!haveMore) {
            if (*endGrpHandle == 0xffff) { //the rare case when we're done here
                todo->intentData.enumSvcsCbk(todo->clientId, todo->connId, NULL, true, 0, 0, GATT_CLI_STATUS_OK);
                *killTodoItem = true;
            } else {
                todo->read.handle = *endGrpHandle + 1;
                pthread_mutex_lock(&mGattLock);
                gattCliTodoEnqItem(conn, todo, false);
                pthread_mutex_unlock(&mGattLock);
            }
        }
        return true;
 /*
  * When we're doing enumerations that have a "prev" we have the uuid stored
  * till we find the prev, then we clear it and report the next as result.
  * This work well since to get first we get NULL in, which becomes a zero.
  * Cool. This means that arriving here we have three cases:
  *  1. We have a uuid and the one we just found does not match - we go on
  *  2. We have a uuid and the one we just found matches - we zero ours and go on
  *  3. We have no uuid and we just found one - we return it
  */
    case GATT_INTENT_ENUM_INCL_SVCS:
        /*
         * includes are weird in that they may have a uuid-16 of the service but may
         * not have a uuid128, so we have special code to get those for us. For that we
         * use the GATT_INTENT_STEP_GET_INCL_UUID step and a read command. We end up back
         * here with data == NULL and uu being our uuid. we must handle this gracefully.
         * The service start/end handles are stashed in todo->intentData.enumInclSvcs
         */
        if (!data && uu) {
            inclStartH = todo->intentData.enumInclSvcs.inclStartH;
            inclEndH = todo->intentData.enumInclSvcs.inclEndH;
            memcpy(&uuid, uu, sizeof(struct uuid));
        } else {

            if (!sgSerializeCutFront(data, &inclDef, sizeof(inclDef))) {
                sgFree(data);
                logw("Invalid include def\n");
                *killTodoItem = true;
                return false;
            }

            inclStartH = utilGetLE16(&inclDef.startHandle);
            inclEndH = utilGetLE16(&inclDef.endHandle);

            if (sgLength(data) == sizeof(uint16_t)) { /* has a uuid-16 */
                attReadUuid(&uuid, data);
                sgFree(data);
            } else if (sgLength(data)) { 
                sgFree(data);
                logw("Invalid include def (uuid area)\n");
                *killTodoItem = true;
                return false;
            } else { /* does not have uuid - save state and branch off to get uuid */
                todo->reqStep = GATT_INTENT_STEP_GET_INCL_UUID;
                todo->type = GATT_CLI_TODO_READ;
                todo->intentData.enumInclSvcs.curH = *handle;
                todo->intentData.enumInclSvcs.lastH = todo->read.endHandle;
                todo->intentData.enumInclSvcs.inclStartH = inclStartH;
                todo->intentData.enumInclSvcs.inclEndH = inclEndH;

                todo->read.handle = inclStartH;
                *killTodoItem = false;

                pthread_mutex_lock(&mGattLock);
                gattCliTodoEnqItem(conn, todo, false);
                pthread_mutex_unlock(&mGattLock);
                return false;
            }
        }
        /* at this point we have a valid complete service include info - deal with it */
        return gattCliFinalStepSvcIncludeFound(conn, todo, inclStartH, inclEndH, &uuid, haveMore, killTodoItem);
        break;

    case GATT_INTENT_ENUM_CHARS:
        if (!sgSerializeCutFront(data, &charDef, sizeof(charDef))) {
            sgFree(data);
            logw("Invalid char def\n");
            *killTodoItem = true;
            return false;
        }
        if (!attReadUuid(&uuid, data)) {
            sgFree(data);
            logw("Char definition does not end in a uuid\n");
            *killTodoItem = true;
            return false;
        }
        sgFree(data);

        if (uuidIsZero(&todo->uuidChr)) { //case 3
            todo->intentData.enumCharsCbk(todo->clientId, todo->connId, &todo->uuidSvc, &uuid, *handle, *endGrpHandle - *handle + 1, GATT_CLI_STATUS_OK);
            *killTodoItem = true;
            return false;
        } else { //cases 2 and 1
            if (uuidCmp(&todo->uuidChr, &uuid)) //case 2
                uuidZero(&todo->uuidChr);

            /* go on searching */
            if (!haveMore) {
                if (*endGrpHandle == todo->read.endHandle) { //the rare case when we're done here
                    todo->intentData.enumCharsCbk(todo->clientId, todo->connId, &todo->uuidSvc, NULL, 0, 0, GATT_CLI_STATUS_OK);
                    *killTodoItem = true;
                } else {
                    todo->read.handle = *endGrpHandle + 1;
                    pthread_mutex_lock(&mGattLock);
                    gattCliTodoEnqItem(conn, todo, false);
                    pthread_mutex_unlock(&mGattLock);
                }
            }
        }
        return true;

    case GATT_INTENT_ENUM_CHAR_DESCRS:
        if (uuidIsZero(&todo->uuidDsc)) { //case 3
            todo->intentData.enumCharDscrsCbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, uu, *handle, *endGrpHandle - *handle + 1, GATT_CLI_STATUS_OK);
            *killTodoItem = true;
            return false;
        } else { //cases 2 and 1
            if (uuidCmp(&todo->uuidDsc, uu)) //case 2
                uuidZero(&todo->uuidDsc);

            /* go on searching */
            if (!haveMore) {
                if (*endGrpHandle == todo->read.endHandle) { //the rare case when we're done here
                    todo->intentData.enumCharDscrsCbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, NULL, 0, 0, GATT_CLI_STATUS_OK);
                    *killTodoItem = true;
                } else {
                    todo->find.startHandle = *endGrpHandle + 1;
                    pthread_mutex_lock(&mGattLock);
                    gattCliTodoEnqItem(conn, todo, false);
                    pthread_mutex_unlock(&mGattLock);
                }
            }
        }
        return true;

    case GATT_INTENT_READ_CHAR:
        todo->intentData.read.cbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, GATT_CLI_STATUS_OK, data);
        *killTodoItem = true;
        return false;

    case GATT_INTENT_WRITE_CHAR:
        todo->intentData.write.cbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, GATT_CLI_STATUS_OK);
        *killTodoItem = true;
        return false;

    case GATT_INTENT_READ_DESCR:
        todo->intentData.readDescr.cbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, &todo->uuidDsc, GATT_CLI_STATUS_OK, data);
        *killTodoItem = true;
        return false;

    case GATT_INTENT_WRITE_DESCR:
        todo->intentData.writeDescr.cbk(todo->clientId, todo->connId, &todo->uuidSvc, &todo->uuidChr, &todo->uuidDsc, GATT_CLI_STATUS_OK);
        *killTodoItem = true;
        return false;

    case GATT_INTENT_EXECUTE:
        todo->intentData.executeCbk(todo->clientId, todo->connId, GATT_CLI_STATUS_OK);
        *killTodoItem = true;
        return false;

    case GATT_INTENT_NOTIF_SUB:
        if (gettCliNotifSubDone(conn, todo, *handle))
            gattCliTodoEnqItem(conn, todo, false);
        else
            *killTodoItem = true;
        return false;

    case GATT_INTENT_NOTIF_UNSUB:
        gettCliNotifUnsubDone(conn, todo);
        *killTodoItem = true;
        return false;
    default:
        loge("Unknonw intent %u in final step\n", todo->reqIntent);
        *killTodoItem = true;
        return false;
    }
    return true;
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
    struct gattConnState *conn;
    bool killTodoItem = false;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Read");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        switch (todo->reqStep) {
        case GATT_INTENT_STEP_FINAL:
            /*
             * This takes some careful thought. Really we're only operating on conn->todoCurr,
             * and no external call can change it, so this is safe to do here. We have to drop
             * the lock to make it ok to make callbacks to higher layer directly from here. One
             * must note, however, that the connection itself might go away. In that case
             * it is not clear who'll free conn->todoCurr. We use the conn->inFinalStep var
             * to provide this arbitration. If it is set, connection destruction will NOT free
             * conn->todoCurr and instead this code will. This leaves no ambiguities and is safe
             */
            conn->inFinalStep = true;
            pthread_mutex_unlock(&mGattLock);
            ret = gattCliFinalStep(conn, todo, &handle, NULL, uuid, NULL, haveMore, &killTodoItem); 
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->inFinalStep = true;
            else {
                gattTodoItemFree(todo);
                logd("Connection went away while processing final step\n");
            }
            break;

        default:
            killTodoItem = true;
            logw("Unexpected step %u in FindInfoCbk. Dropping\n", todo->reqStep);
            break;
        }

        if (conn) {
            if (!ret || !haveMore) { /* aka "if for whatever reasons we expect no more callbacks for this todo item" */
                conn->todoCurr = NULL;
                if (killTodoItem)
                    gattTodoItemFree(todo);
                gattEnqueueCliDoNextTodoItem(conn);
            } else if (killTodoItem)
                logw("Cannot kill todo item if we expect more callbacks on it!\n");
        }
    }

    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattAttCliFindByTypeValCbk
 * USE:      Called by ATT as a reply to our FindByType request, if results were provided
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
    struct gattCliTodoItem *todo;
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "FindByTypeVal");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        logw("Unexpected FindByTypeValCbk. Dropping\n");
        gattTodoItemFree(todo);
        gattEnqueueCliDoNextTodoItem(conn);
    }
    pthread_mutex_unlock(&mGattLock);
    return true;
}

/*
 * FUNCTION: gattCliStepFailed
 * USE:      Called when a step of the form "find *" fails
 * PARAMS:   conn - the conenction structure
 *           todo - the to-do item
 * RETURN:   true to destroy the todo list item. false to keep it (only makes sense if you re-enqueued it)
 * NOTES:
 */
static bool gattCliStepFailed(struct gattConnState *conn, struct gattCliTodoItem *todo)
{
    //todo
    return true;
}

/*
 * FUNCTION: gattCliStepSucceeded
 * USE:      Called when a step of the form "find *" succeeds. should take the next step needed for the intent
 * PARAMS:   conn - the conenction structure
 *           todo - the to-do item
 *           handle - the handle in the reply
 *           endGrpHandle - the end group handle in the reply
 *           charProps - character properties if this is a "find char" step else unused
 *           valHandle - character value handle if this is a "find char" step else unused
 * RETURN:   true to destroy the todo list item. false to keep it (only makes sense if you re-enqueued it)
 * NOTES:
 */
static bool gattCliStepSucceeded(struct gattConnState *conn, struct gattCliTodoItem *todo, uint16_t handle, uint16_t endGrpHandle, uint8_t charProps, uint16_t valHandle)
{
    if (todo->reqStep == GATT_INTENT_STEP_FIND_SVC) { //we found the wanted service. let's see what's next
        switch (todo->reqIntent) {
        case GATT_INTENT_ENUM_INCL_SVCS:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ_BY_TYPE;
            uuidFromUuid16(&todo->read.type, GATT_UUID_INCLUDE);
            break;

        case GATT_INTENT_ENUM_CHARS:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            uuidFromUuid16(&todo->read.type, GATT_UUID_CHARACTERISTIC);
            break;

        case GATT_INTENT_ENUM_CHAR_DESCRS:
        case GATT_INTENT_READ_CHAR:
        case GATT_INTENT_READ_DESCR:
        case GATT_INTENT_WRITE_CHAR:
        case GATT_INTENT_WRITE_DESCR:
        case GATT_INTENT_NOTIF_SUB:
        case GATT_INTENT_NOTIF_UNSUB:
            todo->reqStep = GATT_INTENT_STEP_FIND_CHAR;
            uuidFromUuid16(&todo->read.type, GATT_UUID_CHARACTERISTIC);
            break;
        }

        todo->read.handle = handle + 1;
        todo->read.endHandle = endGrpHandle;
    } else if (todo->reqStep == GATT_INTENT_STEP_FIND_CHAR) { //we found the wanted char. let's see what's next

//todo: use charprops to enforce security for the other side to avoid needless trips there. also check on authReq

        switch (todo->reqIntent) {
        case GATT_INTENT_ENUM_CHAR_DESCRS:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->find.startHandle = valHandle + 1; //descriptos MUST follow value
            todo->find.endHandle = endGrpHandle;
            todo->type = GATT_CLI_TODO_FIND_INFO;
            break;

        case GATT_INTENT_READ_CHAR:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ; /* we start with a normal read, and continue with read-blob if the reply is long enough to warrant it */
            todo->read.handle = valHandle;
            break;

        case GATT_INTENT_READ_DESCR:
        case GATT_INTENT_WRITE_DESCR:
            todo->reqStep = GATT_INTENT_STEP_FIND_DESCR;
            todo->find.startHandle = valHandle + 1; //descriptos MUST follow value
            todo->find.endHandle = endGrpHandle;
            todo->type = GATT_CLI_TODO_FIND_INFO;
            break;

        case GATT_INTENT_WRITE_CHAR:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            switch (todo->intentData.write.writeType) {
            case GATT_CLI_WRITE_TYPE_SIGNED:  //todo: make sure write with no reply calls our done callback anyways!
                todo->type = GATT_CLI_TODO_SIGNED_WRITE;
                break;
            case GATT_CLI_WRITE_TYPE_WRITE_NO_RSP:
                todo->type = GATT_CLI_TODO_WRITE;
                break;
            case GATT_CLI_WRITE_TYPE_WRITE:
                todo->type = GATT_CLI_TODO_WRITE_NO_ACK;
                break;
            case GATT_CLI_WRITE_TYPE_PREPARE:
                todo->type = GATT_CLI_TODO_PREPARE_WRITE;
                break;
            }
            todo->write.handle = valHandle;
            todo->write.data = todo->intentData.write.data; 
            todo->write.ofst = todo->intentData.write.ofst;
            todo->intentData.write.data = NULL;
            break;

        case GATT_INTENT_NOTIF_SUB:
        case GATT_INTENT_NOTIF_UNSUB: /* we issue a read to find the client config descriptor */
            todo->intentData.notif.charHdrH = handle;
            todo->intentData.notif.charValH = valHandle;
            todo->intentData.notif.charProps = charProps;
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ_BY_TYPE;
            todo->read.handle = valHandle + 1;
            todo->read.endHandle = endGrpHandle;
            uuidFromUuid16(&todo->read.type, GATT_UUID_CHAR_CLI_CHAR_CFG);
            break;
        }

    } else if (todo->reqStep == GATT_INTENT_STEP_FIND_DESCR) { //we found the wanted descriptor. let's see what's next

//todo: use charprops to enforce security for the other side to avoid needless trips there. also check on authReq

        switch (todo->reqIntent) {

        case GATT_INTENT_READ_DESCR:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ; /* we start with a normal read, and continue with read-blob if the reply is long enough to warrant it */
            todo->read.handle = valHandle;
            break;

        case GATT_INTENT_WRITE_DESCR:
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            switch (todo->intentData.write.writeType) {
            case GATT_CLI_WRITE_TYPE_SIGNED:  //todo: make sure write with no reply calls our done callback anyways!
                todo->type = GATT_CLI_TODO_SIGNED_WRITE;
                break;
            case GATT_CLI_WRITE_TYPE_WRITE_NO_RSP:
                todo->type = GATT_CLI_TODO_WRITE;
                break;
            case GATT_CLI_WRITE_TYPE_WRITE:
                todo->type = GATT_CLI_TODO_WRITE_NO_ACK;
                break;
            case GATT_CLI_WRITE_TYPE_PREPARE:
                todo->type = GATT_CLI_TODO_PREPARE_WRITE;
                break;
            }
            todo->write.handle = valHandle;
            todo->write.data = todo->intentData.writeDescr.data; 
            todo->write.ofst = todo->intentData.writeDescr.ofst;
            todo->intentData.writeDescr.data = NULL;
            break;
        }

        gattCliTodoEnqItem(conn, todo, false);
        return false;
    } else {
        logw("Strange intent & step: %u.%u. Dropping\n", todo->reqIntent, todo->reqStep);
        return true;
    }

    gattCliTodoEnqItem(conn, todo, false);
    return false;
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
    struct uuid uuid, *finalStepUuidP = NULL;
    struct gattConnState *conn;
    bool killTodoItem = false;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Read");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;
        struct gattCharDefHdr charDefHdr;
        struct uuid uuid;

        switch (todo->reqStep) {
        case GATT_INTENT_STEP_FIND_SVC: /* we're looking for a service - so let's look */
            if (!handleP || !endGrpHandleP || !attReadUuid(&uuid, data))
                loge("Invalid reply type for this step\n");
            else if (uuidCmp(&todo->uuidSvc, &uuid))
                killTodoItem = gattCliStepSucceeded(conn, todo, *handleP, *endGrpHandleP, 0, 0); //returns false to stop current thing
            else if (haveMore)
                ret = true;
            else { /* keep trying further down if there is more to try */
                if (*handleP == todo->read.endHandle) { //no more
                    killTodoItem = gattCliStepFailed(conn, todo);
                } else { // keep going
                    todo->read.handle = *handleP + 1;
                    killTodoItem = false;
                    gattCliTodoEnqItem(conn, todo, false);
                }
            }
            break;

        case GATT_INTENT_STEP_FIND_CHAR:
            if (!handleP || !endGrpHandleP || !sgSerializeCutFront(data, &charDefHdr, sizeof(charDefHdr)) || !attReadUuid(&uuid, data))
                loge("Invalid reply type for this step\n");
            else if (uuidCmp(&todo->uuidChr, &uuid))
                killTodoItem = gattCliStepSucceeded(conn, todo, *handleP, *endGrpHandleP, utilGetLE8(&charDefHdr.charProps), utilGetLE16(&charDefHdr.valHandle));
            else if (haveMore)
                ret = true;
            else { /* keep trying further down if there is more to try */
                if (*handleP == todo->read.endHandle) { //no more
                    killTodoItem = gattCliStepFailed(conn, todo);
                } else { // keep going
                    todo->read.handle = *handleP + 1;
                    killTodoItem = false;
                    gattCliTodoEnqItem(conn, todo, false);
                }
            }
            break;

        case GATT_INTENT_STEP_GET_INCL_UUID:
            /* change it back to final step enum incl req and call into final step */
            finalStepUuidP = &uuid;
            todo->read.handle = todo->intentData.enumInclSvcs.curH;
            todo->read.endHandle = todo->intentData.enumInclSvcs.lastH;
            todo->reqStep = GATT_INTENT_STEP_FINAL;
            todo->type = GATT_CLI_TODO_READ_BY_TYPE;
            uuidFromUuid16(&todo->read.type, GATT_UUID_INCLUDE);
            //fall through (data shuold be NULL in this case)


        case GATT_INTENT_STEP_FINAL:
            /*
             * This takes some careful thought. Really we're only operating on conn->todoCurr,
             * and no external call can change it, so this is safe to do here. We have to drop
             * the lock to make it ok to make callbacks to higher layer directly from here. One
             * must note, however, that the connection itself might go away. In that case
             * it is not clear who'll free conn->todoCurr. We use the conn->inFinalStep var
             * to provide this arbitration. If it is set, connection destruction will NOT free
             * conn->todoCurr and instead this code will. This leaves no ambiguities and is safe
             */
            conn->inFinalStep = true;
            pthread_mutex_unlock(&mGattLock);
            ret = gattCliFinalStep(conn, todo, handleP, endGrpHandleP, finalStepUuidP, data, haveMore, &killTodoItem); 
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->inFinalStep = true;
            else {
                gattTodoItemFree(todo);
                logd("Connection went away while processing final step\n");
            }
            data = NULL;
            break;

        default:
            killTodoItem = true;
            logw("Unexpected step %u in ReadCbk. Dropping\n", todo->reqStep);
            break;
        }

        if (conn) {
            if (!ret || !haveMore) { /* aka "if for whatever reasons we expect no more callbacks for this todo item" */
                conn->todoCurr = NULL;
                if (killTodoItem)
                    gattTodoItemFree(todo);
                gattEnqueueCliDoNextTodoItem(conn);
            } else if (killTodoItem)
                logw("Cannot kill todo item if we expect more callbacks on it!\n");
        }
    }
    if (data)
        sgFree(data);

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
    static const uint8_t expectedWorkTypes[] = {GATT_CLI_TODO_WRITE, GATT_CLI_TODO_PREPARE_WRITE, GATT_CLI_TOTO_WRITE_EXEC};
    struct gattConnState *conn;
    bool killTodoItem = false;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Write");
    if (conn) {
        struct gattCliTodoItem *todo = conn->todoCurr;

        switch (todo->reqStep) {
        case GATT_INTENT_STEP_FINAL:
            /*
             * This takes some careful thought. Really we're only operating on conn->todoCurr,
             * and no external call can change it, so this is safe to do here. We have to drop
             * the lock to make it ok to make callbacks to higher layer directly from here. One
             * must note, however, that the connection itself might go away. In that case
             * it is not clear who'll free conn->todoCurr. We use the conn->inFinalStep var
             * to provide this arbitration. If it is set, connection destruction will NOT free
             * conn->todoCurr and instead this code will. This leaves no ambiguities and is safe
             */
            conn->inFinalStep = true;
            pthread_mutex_unlock(&mGattLock);
            ret = gattCliFinalStep(conn, todo, NULL, NULL, NULL, NULL, false, &killTodoItem); 
            pthread_mutex_lock(&mGattLock);
            conn = gattConnFindByL2cConn(to);
            if (conn)
                conn->inFinalStep = true;
            else {
                gattTodoItemFree(todo);
                logd("Connection went away while processing final step\n");
            }
            break;

        default:
            killTodoItem = true;
            logw("Unexpected step %u in WriteCbk. Dropping\n", todo->reqStep);
            break;
        }

        if (conn) {
            if (!ret) { /* aka "if for whatever reasons we expect no more callbacks for this todo item" */
                conn->todoCurr = NULL;
                if (killTodoItem)
                    gattTodoItemFree(todo);
                gattEnqueueCliDoNextTodoItem(conn);
            } else if (killTodoItem)
                logw("Cannot kill todo item if we expect more callbacks on it!\n");
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
                                                GATT_CLI_TODO_READ_BY_GRP_TYPE, GATT_CLI_TODO_WRITE, GATT_CLI_TODO_PREPARE_WRITE, GATT_CLI_TOTO_WRITE_EXEC};
    struct gattConnState *conn;

    pthread_mutex_lock(&mGattLock);
    conn = gattAttCbkCheckState(trans, to, expectedWorkTypes, sizeof(expectedWorkTypes), "Error");
    if (conn) {
        //todo
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
    conn = gattAttCbkCheckState(0, to, NULL, 0, "Notif");
    if (conn) {
        struct gattCliNotif *n;

        /* find the per-connection notif structure */
        for (n = conn->cliNotifs; n && n->charValueHandle != handle; n = n->next);
        if (!n)
            logw("Got notif/ind which we have no record of\n");
        else {

            struct gattCliCon *clicon;
            struct gattCliNotifRec *nr;
            sg dataCopy;

            /* find all clients who are subscribed and notify them */
            for (clicon = mCliCons; clicon; clicon = clicon->next) {
                /* skip all clients not connected to this connection */
                if (clicon->conn != conn)
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
        .mtu = 0xFFFF,
    };
    static const struct l2cServiceFixedChDescriptor gattFixedCh = {
        .serviceFixedChAlloc = gattFixedChSvcAlloc,
        .serviceFixedChStateCbk = gattConnEvt,
    };

    attSrvInit(gattAttSrvTxCbk, gattAttMtuCbk, gattAttSrvIndCbk, gattAttExecWriteCbk, gattAttSvrReadCbk, gattAttSvrWriteCbk);
    attCliInit(gattAttMtuCbk, gattAttCliFindInfoCbk, gattAttCliFindByTypeValCbk, gattAttCliReadCbk, gattAttCliWriteCbk, gattAttCliError, gattAttCliIndNotif);

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
 * RETURN:   service reference or 0 on error
 * NOTES:
 */
int gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF)
{
    struct gattService *svc;
    int svcRef = 0;

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
    svc->svcRef = mSvcs ? mSvcs->svcRef : 1;
    while (gattServiceFindBySvcRef(svc->svcRef)) {
        svc->svcRef++;
        if (svc->svcRef <= 0)
            svc->svcRef = 1;
    }

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

    if (svc->allowEDR) {
        svc->sdpHandle = gattSdpAdd(&svc->uuid, attSrvHandleRangeGetBase(svc->attRange), ofst);
        if (!svc->sdpHandle) {
            loge("Failed to add EDR service to SDP\n");
            goto out_dereg;
        }
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
    if (svc->sdpHandle)
        sdpServiceDescriptorDel(svc->sdpHandle);
    svc->sdpHandle = 0;
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
bool gattServiceDestroy(int svcRef)
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
bool gattServiceStart(int svcRef, bool supportLE, bool supportEDR)
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
bool gattServiceStop(int svcRef)
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
bool gattServiceIsRunning(int svcRef)
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
bool gattServiceGetUuid(int svcRef, struct uuid *dst)
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
uint16_t gattServiceGetHandleBaseBySvc(int svcRef)
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
 * RETURN:   srvcRef or negative number on error
 * NOTES:
 */
int gattServiceFindByHandle(uint16_t handle, bool firstOnly)
{
    struct gattService *svc;
    int ret = -1;

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
uint16_t gattServiceAddIncludedService(int svcRef, const struct uuid *uuid)
{
    struct gattService *svc, *included;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
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
uint16_t gattServiceAddChar(int svcRef, const struct uuid *uuid, uint8_t props, uint32_t perms)
{
    struct gattItem *giDecl, *giVal;
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
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
uint16_t gattServiceAddCharDescr(int svcRef, const struct uuid *uuid, uint32_t perms)
{
    struct gattService *svc;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
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
bool gattServiceSendInd(int svcRef, int cid, uint16_t handle, const void *buf, uint16_t len, bool withConfirm, uint64_t ref)
{
    uint8_t ret = L2C_TX_ERROR;
    struct gattService *svc;
    uint16_t base;
    sg data;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindBySvcRef(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
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
 * FUNCTION: gattCliTodoNext
 * USE:      Start on the next to-do item for the connection
 * PARAMS:   conn - the conection handle
 *           now - the todo item
 * RETURN:   NONE
 * NOTES:    This shoudl ONLY be called fro mthe worker thread with mGattLock NOT held!
 */
static void gattCliTodoNext(l2c_handle_t conn, struct gattCliTodoItem *now)
{
    struct gattConnState *connState;
    uniq_t trans = 0;

    /* actually start it */
    switch (now->type) {
    case GATT_CLI_TODO_FIND_INFO:
        trans = attCliFindInformation(conn, now->find.startHandle, now->find.endHandle);
        if (!trans)
            logw("FindInfo failed\n");
        break;

    case GATT_CLI_TODO_FIND_BY_TYPE_VAL:
        trans = attCliFindByTypeValue(conn, now->find.startHandle, now->find.endHandle, now->find.uuid16, now->find.val);
        if (!trans) {
            sgFree(now->find.val);
            logw("FindByTypeValue failed\n");
         }
        break;

    case GATT_CLI_TODO_READ_BY_TYPE:
        trans = attCliReadByType(conn, now->read.handle, now->read.endHandle, &now->read.type);
        if (!trans)
            logw("ReadByType failed\n");
        break;

    case GATT_CLI_TODO_READ:
        trans = attCliRead(conn, now->read.handle);
        if (!trans)
            logw("Read failed\n");
        break;

    case GATT_CLI_TODO_READ_BLOB:
        trans = attCliReadBlob(conn, now->read.handle, now->read.ofst);
        if (!trans)
            logw("ReadBlob failed\n");
        break;

    case GATT_CLI_TODO_READ_BY_GRP_TYPE:
        trans = attCliReadByGroupType(conn, now->read.handle, now->read.endHandle, &now->read.type);
        if (!trans)
            logw("ReadByGroupType failed\n");
        break;

    case GATT_CLI_TODO_WRITE:
        trans = attCliWrite(conn, now->write.handle, now->write.data);
        if (!trans) {
            sgFree(now->write.data);
            logw("Write with confirm failed\n");
        }
        break;

    case GATT_CLI_TODO_WRITE_NO_ACK:
        if (!attCliWriteNoAck(conn, now->write.handle, now->write.data)) {
            sgFree(now->write.data);
            logi("Write with no confirm failed\n");
        }
        break;

    case GATT_CLI_TODO_SIGNED_WRITE:
        if (!attCliSignedWriteNoAck(conn, now->write.handle, now->write.data)) {
            sgFree(now->write.data);
            logi("Write with signature failed\n");
        }
        break;

    case GATT_CLI_TODO_PREPARE_WRITE:
        trans = attCliPrepareWrite(conn, now->write.handle, now->write.ofst, now->write.data);
        if (!trans) {
            sgFree(now->write.data);
            logw("PrepareWrite failed\n");
        }
        break;

    case GATT_CLI_TOTO_WRITE_EXEC:
        trans = attCliExecuteStagedWrites(conn, now->exec.commit);
        if (!trans)
            logw("ExecWrite failed\n");
        break;
    }

    pthread_mutex_lock(&mGattLock);
    connState = gattConnFindByL2cConn(conn);
    if (!connState)
        logd("ToDo item we just did no longer has a connection. Oops\n");
    else if (connState->todoCurr != now)
        loge("Inconsistent idea of what the current todo item is!\n");

    if (!trans) { /* a transaction is NOT ongoing */
        gattTodoItemFree(now);
        if (connState) {
            connState->todoCurr = NULL;
            gattEnqueueCliDoNextTodoItem(connState);
        }
    }
    pthread_mutex_unlock(&mGattLock);
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
    if (conn->todoListH) { /* have items in list already? */
        if (inFront) {
            todo->next = conn->todoListH;
            conn->todoListH = todo;
        } else {
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
 * FUNCTION: gattCliTodoEnqItemWithData
 * USE:      Enqueue a gatt to-do item after filling it with proper uuids & intents
 * PARAMS:   conn - the connection structure
 *           clientId - the client ID
 *           connId - the connection ID
 *           todo - the item
 *           intent - the intended request type (in gatt terms) GATT_INTENT_*
 *           step - the step number in the intent
 *           uuidSvc - the uuid for the service we INTEND to act on (or NULL if none)
 *           uuidChr - the uuid for the characteristic we INTEND to act on (or NULL if none)
 *           uuidDsc - the uuid for the descriptor we INTEND to act on (or NULL if none)
 *           inFront - add it in front (instead of in order) this is used for confirmations
 * RETURN:   NONE
 * NOTES:    Call with mGattLock held
 */
static void gattCliTodoEnqItemWithData(struct gattConnState *conn, int clientId, int connId, struct gattCliTodoItem *todo, uint8_t intent, uint8_t step,
                               const struct uuid *uuidSvc, const struct uuid *uuidChr, const struct uuid *uuidDsc, bool inFront)
{
    todo->next = NULL;
    todo->clientId = clientId;
    todo->connId = connId;
    todo->reqIntent = intent;
    if (uuidSvc)
        memcpy(&todo->uuidSvc, uuidSvc, sizeof(struct uuid));
    else
        uuidZero(&todo->uuidSvc);
    if (uuidChr)
        memcpy(&todo->uuidChr, uuidChr, sizeof(struct uuid));
    else
        uuidZero(&todo->uuidChr);
    if (uuidDsc)
        memcpy(&todo->uuidDsc, uuidDsc, sizeof(struct uuid));
    else
        uuidZero(&todo->uuidDsc);

    gattCliTodoEnqItem(conn, todo, inFront);
}

/*
 * FUNCTION: gattCliEnqueueWriteReq
 * USE:      Enqueue a gatt to-do item to perform a write command or request
 * PARAMS:   clicon - the client-connection structure
 *           handle - the handle to write
 *           data - the data to write
 *           len - length of said data
 *           withReply - use write with reply? (if no, callback will not be fired on success, but may be on failure)
 *           intent - the intended request type (in gatt terms) GATT_INTENT_*
 *           step - the step number in the intent
 *           uuidSvc - the uuid for the service we INTEND to act on (or NULL if none)
 *           uuidChr - the uuid for the characteristic we INTEND to act on (or NULL if none)
 *           uuidDsc - the uuid for the descriptor we INTEND to act on (or NULL if none)
 *           inFront - add it in front (instead of in order) this is used for confirmations
 * RETURN:   false on immediate failure
 * NOTES:    If this returns true, the callback WILL be called (but it may be a while). Call with mGattLock held
 */
static bool gattCliTodoEnqWriteReq(struct gattCliCon *clicon, uint16_t handle, const void *data, uint32_t len, bool withReply, uint8_t intent,
                                   uint8_t step, const struct uuid *uuidSvc, const struct uuid *uuidChr, const struct uuid *uuidDsc)
{
    struct gattCliTodoItem *todo = (struct gattCliTodoItem*)calloc(1, sizeof(struct gattCliTodoItem));
    sg s;

    if (!todo)
        goto fail_todo;

    s = sgNewWithCopyData(data, len);
    if (!s)
        goto fail_s;

    todo->type = withReply ? GATT_CLI_TODO_WRITE : GATT_CLI_TODO_WRITE_NO_ACK;
    todo->write.handle = handle;
    todo->write.data = s;

    gattCliTodoEnqItemWithData(clicon->conn, clicon->cli->clientId, clicon->connId, todo, intent, step, uuidSvc, uuidChr, uuidDsc, false);
    return true;


fail_s:
    free(todo);
fail_todo:
    return false;
}

/*
 * FUNCTION: gattClientNotifUnsubscribe
 * USE:      Unsubscribe fron a notification/indication
 * PARAMS:   clicon - the client-connection structure
 *           nr - the gattCliNotifRec
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattClientNotifUnsubscribe(struct gattCliCon *clicon, struct gattCliNotifRec *nr)
{
    struct gattCliNotif *n = nr->notif;
    uint8_t newFlags[sizeof(uint16_t)];
    uint16_t writeHandle;
    bool doUpdate = true;

    /* unlink */
    if (nr->prev)
        nr->prev->next = nr->next;
    else
        clicon->notifs = nr->next;
    if (nr->next)
        nr->next->prev = nr->prev;

    if (nr->wantInd)
        n->refCtInd--;
    else
        n->refCtNotif--;

    /*
     * See if we need to change our notif/ind subscription. There are three cases:
     * 1. Nobody wants anything anymore - unsubscribe alltogether
     * 2. The last guy who wanted indications went away, but we still have
     *    clients who want notifications. In this case we'll unsubscribe from
     *    indications and subscribe to notifications if the given characteristic
     *    supports it. Else we'll keep it as is.
     * 3. We still have clients for whatever mode we're currently in and thus
     *    we do nothing.
     * Note that if we were subscribed already we are guaranteed to be secured/authed
     * enough to change said subscription since LE does not allow us to ever drop
     * security once connection is secure. This is why there are no checks here.
     */

    writeHandle = n->charConfigHandle;

    if (!n->refCtInd && !n->refCtNotif) { /* case 1 */
        if (n->prev)
            n->prev->next = n->next;
        else
            clicon->conn->cliNotifs = n->next;
        if (n->next)
            n->next->prev = n->prev;

        utilSetLE16(newFlags, 0);
        free(n);

    } else if (!n->refCtInd && n->supportsNotifs) { /* case 2 */

        utilSetLE16(newFlags, GATT_CLI_CFG_BIT_NOTIFS);

    } else { /* case 3 */
        doUpdate = false;
    }

    /* we do not bother with callbacks since there are no errors that can happen that we can actually handle */
    if (doUpdate && !gattCliTodoEnqWriteReq(clicon, writeHandle, newFlags, sizeof(newFlags), true, GATT_INTENT_INTERNAL, 0, NULL, NULL, NULL))
        loge("Failed to update notification settings on client unsubscription\n");

    free(nr);
}

/*
 * FUNCTION: gattClientConnClose
 * USE:      Close a client's connection (optionally notify said client)
 * PARAMS:   clicon - the client-connection structure
 *           status - status to notify client with (if GATT_CLI_STATUS_OK, no notification will be sent)
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattClientConnClose(struct gattCliCon *clicon, uint8_t status)
{
    struct gattCliTodoItem *tC, *tN, *tP = NULL;

    /* notify client if needed */
    if (status != GATT_CLI_STATUS_OK && !gattEnqueueCliConnStatusCall(clicon, GATT_CLI_STATUS_OK))
        logw("Failed to notify client about connection closure. Pity.\n");

    /* enumerate all notifications this client had and unsubscribe from them. */
    while (clicon->notifs)
        gattClientNotifUnsubscribe(clicon, clicon->notifs);

    /*
     * Remove all todo list items that are in client's name. If one is
     * inflight, it will gracefully fail when it fails to find a client
     * on completion, so we need not do much for that case. In reality
     * this is all an optimization. We could leave all these items here
     * and let them all fail safely. But since it is easy to get rid of
     * them, we do.
     */
    for (tC = clicon->conn->todoListH; tC; tC = tN) {
        tN = tC->next;
        if (tC->clientId == clicon->cli->clientId) {
            if (tC == clicon->conn->todoListH)
                clicon->conn->todoListH = tN;
            if (tC == clicon->conn->todoListT)
                clicon->conn->todoListT = tP;
            free(tC);
        } else
            tP = tC;
    }

    /* destroy the structure(s) */
    if (clicon->next)
        clicon->next->prev = clicon->prev;
    if (clicon->prev)
        clicon->prev->next = clicon->next;
    else
        mCliCons = clicon->next;
    free(clicon);
}

/*
 * FUNCTION: gattClientCreate
 * USE:      Allocate a per-gatt-client structure
 * PARAMS:   uuid - client uuid (not used for anything really)
 * RETURN:   client_id value or 0 on failure
 * NOTES:
 */
int gattClientCreate(const struct uuid *uuid)
{
    struct gattClient *cli;
    int ret = 0;

    pthread_mutex_lock(&mGattLock);

    cli = gattClientFindByUuid(uuid);
    if (cli)
        logw("Refusing to create duplciate gatt client with UUID "UUIDFMT" (current is client %d)\n", UUIDCONV(*uuid), cli->clientId);
    else {
        cli = (struct gattClient*)calloc(1, sizeof(struct gattClient));
        if (cli) {

           cli->clientId = mClients ? mClients->clientId : 1;
           while (gattClientFindById(cli->clientId)) {
               cli->clientId++;
               if (cli->clientId <= 0)
                   cli->clientId = 1;
           }

           memcpy(&cli->uuid, uuid, sizeof(struct uuid));
           cli->next = mClients;
           if (mClients)
               mClients->prev = cli;
           mClients = cli;
           ret = cli->clientId;
        }
    }

    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattClientDestroy
 * USE:      Destroy a per-gatt-client structure
 * PARAMS:   clientId - the clientId
 * RETURN:   client_id value or 0 on failure
 * NOTES:
 */
void gattClientDestroy(int clientId)
{
    struct gattCliCon *cliCon;
    struct gattClient *cli;

    pthread_mutex_lock(&mGattLock);

    cli = gattClientFindById(clientId);
    if (!cli)
        logw("Attempting to destroy a nonexistent GATT client %d\n", clientId);
    else {

        /* schedule a force-close of all its connections */
        for (cliCon = mCliCons; cliCon; cliCon = cliCon->next)
            if (cliCon->cli == cli)
                gattClientConnClose(cliCon, false /* do not notify client -> no need they're about to go away awnyways */);

        /* destroy the client */
        if (cli->next)
            cli->next->prev = cli->prev;
        if (cli->prev)
            cli->prev->next = cli->next;
        else
            mClients = cli->next;
        free(cli);
    }

    pthread_mutex_unlock(&mGattLock);
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
    return true;
}

/*
 * FUNCTION: gattCliTodoMakeFindByGroupType
 * USE:      Enqueue a read-by-group-type (uuid16) todo item
 * PARAMS:   conn - the connection id
 *           type - the type of attribute to read
 *           svc - in what service (or NULL)
 *           charOrIncSvc - what characteristic/incl service (or NULL)
 *           descr - what descriptor (or NULL)
 *           intent - the intent of this request
 *           step - the step of this request
 *           firstHandle - first handle to consider
 *           lastHandle - last handle to consider
 * RETURN:   todo item or NULL.
 * NOTES:    call with mGattLock held, item WILL be added to the queue, but since the queue will
 *           not move while the lock is held by you, you can customize the item safely after this
 *           func gives it to you
 */
static struct gattCliTodoItem* gattCliTodoMakeFindByGroupType(int conn, uint16_t type, const struct uuid *svc, const struct uuid *charOrIncSvc, const struct uuid *descr,
                                                              uint8_t intent,  uint8_t step, uint16_t firstHandle, uint16_t lastHandle)
{
    struct gattCliTodoItem *todo;
    struct gattCliCon *clicon;

    clicon = gattCliconFindById(conn);
    if (!clicon)
        return NULL;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        return NULL;

    todo->type = GATT_CLI_TODO_READ_BY_GRP_TYPE;
    todo->read.handle = firstHandle;
    todo->read.endHandle = lastHandle;
    uuidFromUuid16(&todo->read.type, type);

    gattCliTodoEnqItemWithData(clicon->conn, clicon->cli->clientId, clicon->connId, todo, intent, step, svc, charOrIncSvc, descr, false);

    return todo;
}

/*
 * FUNCTION: gattClientEnumServices
 * USE:      Find all services (or a particular one) on a server
 * PARAMS:   conn - the connection id
 *           searchForThis - uuid of service to find or NULL if we want them all
 *           cbk - the callback to call with results (we'll call it with NULL after all results have been returned)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumServices(int conn, const struct uuid *searchForThis, gattCliSvcEnumCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, searchForThis, NULL, NULL, GATT_INTENT_ENUM_SVCS, GATT_INTENT_STEP_FINAL, 0x0001, 0xffff);
    if (todo)
        todo->intentData.enumSvcsCbk = cbk;
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumIncludedServices
 * USE:      Find the next included service in a service given the previous one
 * PARAMS:   conn - the connection id
 *           inThisService - in what service are we looking?
 *           prev - previous included service (pass NULL to get the first)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumIncludedServices(int conn, const struct uuid *inThisService, const struct uuid *prev, gattCliEnumIncludedSvcsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, prev, NULL, GATT_INTENT_ENUM_INCL_SVCS, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo)
        todo->intentData.enumInclSvcs.cbk = cbk;
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumCharacteristics
 * USE:      Find the next characteristic in a service given the previous one
 * PARAMS:   conn - the connection id
 *           inThisService - in what service are we looking?
 *           prev - previous characteristic (pass NULL to get the first)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumCharacteristics(int conn, const struct uuid *inThisService, const struct uuid *prev, gattCliEnumCharacteristicsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, prev, NULL, GATT_INTENT_ENUM_CHARS, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo)
        todo->intentData.enumCharsCbk = cbk;
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientEnumCharDescriptors
 * USE:      Find the next characteristic descriptor in a characteristic given the previous one
 * PARAMS:   conn - the connection id
 *           inThisService - in what service are we looking?
 *           inThisChar - in what characteristic are we looking?
 *           prev - previous characteristic descriptor (pass NULL to get the first)
 *           cbk - the callback to call with results (UUID will be passed as NULL if no more were found)
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientEnumCharDescriptors(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *prev, gattCliEnumCharDescrsCbk cbk)
{
    struct gattCliTodoItem *todo;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, inThisChar, prev, GATT_INTENT_ENUM_CHAR_DESCRS, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo)
        todo->intentData.enumCharDscrsCbk = cbk;
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
 * FUNCTION: gattClientVerifyWriteType
 * USE:      Verify an writeType param given to us is valid (simply numerically)
 * PARAMS:   writeType - said param
 * RETURN:   true if it is a valid value
 * NOTES:
 */
static bool gattClientVerifyWriteType(uint8_t writeType)
{
    switch (writeType) {
    case GATT_CLI_WRITE_TYPE_SIGNED:
    case GATT_CLI_WRITE_TYPE_WRITE_NO_RSP:
    case GATT_CLI_WRITE_TYPE_WRITE:
    case GATT_CLI_WRITE_TYPE_PREPARE:
        return true;
    default:
        return false;
    }
}

/*
 * FUNCTION: gattClientCharRead
 * USE:      Read a characteristic
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           thisChar - what characteristic?
 *           authReq - authentication requirements (//TODO)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientCharRead(int conn, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t authReq, gattCliCharReadCbk cbk)
{
    struct gattCliTodoItem *todo;

    if (!gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, thisChar, NULL, GATT_INTENT_READ_CHAR, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo) {
        todo->intentData.read.cbk = cbk;
        todo->intentData.read.authReq = authReq;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientCharWrite
 * USE:      Write a characteristic
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           thisChar - what characteristic?
 *           authReq - authentication requirements (//TODO)
 *           writeType - cmd? req? prepare?
 *           ofst - write offset (used only for prepare write)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientCharWrite(int conn, const struct uuid *inThisService, const struct uuid *thisChar, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, gattCliCharWriteCbk cbk)
{
    struct gattCliTodoItem *todo;

    if (!gattClientVerifyWriteType(writeType) || !gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, thisChar, NULL, GATT_INTENT_WRITE_CHAR, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo) {
        todo->intentData.write.cbk = cbk;
        todo->intentData.write.authReq = authReq;
        todo->intentData.write.writeType = writeType;
        todo->intentData.write.ofst = ofst;
        todo->intentData.write.data = data;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientCharDescrRead
 * USE:      Read a characteristic descriptor
 * PARAMS:   conn - the connstruct gattCliTodoItem*ection id
 *           inThisService - in what service?
 *           inThisChar - what characteristic?
 *           thisDescr - what descriptor?
 *           authReq - authentication requirements (//TODO)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientCharDescrRead(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t authReq, gattCliCharDescrReadCbk cbk)
{
    struct gattCliTodoItem *todo;

    if (!gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, inThisChar, thisDescr, GATT_INTENT_READ_DESCR, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo) {
        todo->intentData.readDescr.cbk = cbk;
        todo->intentData.readDescr.authReq = authReq;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientCharWrite
 * USE:      Write a characteristic descriptor
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           inThisChar - what characteristic?
 *           thisDescr - what descriptor?
 *           authReq - authentication requirements (//TODO)
 *           writeType - cmd? req? prepare?
 *           ofst - write offset (used only for prepare write)
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientCharDescrWrite(int conn, const struct uuid *inThisService, const struct uuid *inThisChar, const struct uuid *thisDescr, uint8_t authReq, uint8_t writeType, uint16_t ofst, sg data, gattCliCharDescrWriteCbk cbk)
{
    struct gattCliTodoItem *todo;

    if (!gattClientVerifyWriteType(writeType) || !gattClientVerifyAuthReq(authReq))
        return GATT_CLI_STATUS_ERR;

    pthread_mutex_lock(&mGattLock);
    todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, inThisChar, thisDescr, GATT_INTENT_WRITE_DESCR, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
    if (todo) {
        todo->intentData.writeDescr.cbk = cbk;
        todo->intentData.writeDescr.authReq = authReq;
        todo->intentData.writeDescr.writeType = writeType;
        todo->intentData.writeDescr.ofst = ofst;
        todo->intentData.writeDescr.data = data;
    }
    pthread_mutex_unlock(&mGattLock);

    return todo ? GATT_CLI_STATUS_OK : GATT_CLI_STATUS_ERR;
}

/*
 * FUNCTION: gattClientStagedWritesExecute
 * USE:      Execute or abandon previously staged writes
 * PARAMS:   conn - the connection id
 *           execute - true to commit them, false to abandon
 *           cbk - the callback to call with results
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientStagedWritesExecute(int conn, bool execute, gattCliStagedWriteExecuteCb cbk)
{
    uint8_t ret = GATT_CLI_STATUS_ERR;
    struct gattCliTodoItem *todo;
    struct gattCliCon *clicon;

    pthread_mutex_lock(&mGattLock);
    clicon = gattCliconFindById(conn);
    if (!clicon)
        goto out;

    todo = calloc(1, sizeof(struct gattCliTodoItem));
    if (!todo)
        goto out;

    todo->type = GATT_CLI_TOTO_WRITE_EXEC;
    todo->exec.commit = execute;
    todo->intentData.executeCbk = cbk;

    gattCliTodoEnqItemWithData(clicon->conn, clicon->cli->clientId, clicon->connId, todo, GATT_INTENT_EXECUTE, GATT_INTENT_STEP_FINAL, NULL, NULL, NULL, false);

out:
    pthread_mutex_unlock(&mGattLock);
    return ret;
}

/*
 * FUNCTION: gattClientNotifsSubscribe
 * USE:      Subscribe to a notification or indication
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           thisChar - what characteristic?
 *           reliable - use reliable delivery?
 *           stateCbk - callback to call when subscription state changes
 *           arrivedCbk - callback to call when notif/ind arrives
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientNotifsSubscribe(int conn, const struct uuid *inThisService, const struct uuid *thisChar, bool reliable, gattCliNotifSubscribeStateCbk stateCbk, gattCliNotifArrivedCbk arrivedCbk)
{
    uint8_t sta = GATT_CLI_STATUS_ERR;
    struct gattCliNotifRec *nr;
    struct gattCliNotif *notif;
    struct gattCliCon *clicon;
    uint32_t *relevantReftCtP;
    bool callStateCbk = true;
    bool startSub = false;
    int cli = 0;

    pthread_mutex_lock(&mGattLock);
    clicon = gattCliconFindById(conn);
    if (!clicon)
        logw("No connection for subscribe\n");
    else {
        cli = clicon->cli->clientId;

        /* find the notif record, if one exists */
        for (nr = clicon->notifs; nr && (!uuidCmp(inThisService, &nr->notif->svc) || !uuidCmp(thisChar, &nr->notif->chr)); nr = nr->next);
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
        nr->arrivedCbk = arrivedCbk;

        /* find the notif structure, if it exists */
        for (notif = clicon->conn->cliNotifs; notif && (!uuidCmp(inThisService, &notif->svc) || !uuidCmp(thisChar, &notif->chr)); notif = notif->next);

        if (!notif) { /* no structure? create it */
            notif = calloc(1, sizeof(struct gattCliNotif));
            if (!notif) {
                free(nr);
                loge("Allocation error of a notif struct\n");
                goto out;
            }
            memcpy(&notif->svc, inThisService, sizeof(struct uuid));
            memcpy(&notif->chr, thisChar, sizeof(struct uuid));
            startSub = true;
        }
        relevantReftCtP = reliable ? &notif->refCtInd : &notif->refCtNotif;

        if (!notif->ready) {
            /* if it is not yet ready, just add ourselves to the list and move on. cbk later */
            callStateCbk = false;
            sta = GATT_CLI_STATUS_OK;
        } else if (*relevantReftCtP || !reliable) {
            /* ready and correct or better type - we're done */
            sta = GATT_CLI_STATUS_OK;
        } else if (!notif->supportsInds) {
            /* caller wants reliable notifs but this is not possible. fail */
            sta = GATT_CLI_STATUS_ERR;
        } else {
            /*
             * We need to resubscribe to reliable notifications and we can. Technically, since
             * we already know the handles, we can just do a write, but this makes the code
             * simpler and thanks to mandatory caching in ATT, it is not really any slower.
             */
            notif->ready = false;
            startSub = true;
        }

        /* if requested, start the actual [re]subscription process */
        if (startSub) {
            struct gattCliTodoItem *todo;

            todo = gattCliTodoMakeFindByGroupType(conn, GATT_UUID_SVC_PRIMARY, inThisService, thisChar, NULL, GATT_INTENT_NOTIF_SUB, GATT_INTENT_STEP_FIND_SVC, 0x0001, 0xffff);
            if (!todo)
                loge("Notif todo allocation failed. Leaks will result\n");
        }

        /* if we are not calling the state callback now, we should later */
        if (!callStateCbk)
            nr->subStateCbk = stateCbk;

        /* link ourselves into the struct and increment the refctr */
        (*relevantReftCtP)++;
        nr->next = clicon->notifs;
        clicon->notifs = nr;
    }


out:
    pthread_mutex_unlock(&mGattLock);

    if (callStateCbk)
        stateCbk(cli, conn, inThisService, thisChar, true, sta);

    return sta;
}

/*
 * FUNCTION: gattClientNotifsUnsubscribe
 * USE:      Unsubscribe from a notification or indication
 * PARAMS:   conn - the connection id
 *           inThisService - in what service?
 *           thisChar - what characteristic?
 *           stateCbk - callback to call when subscription state changes
 * RETURN:   GATT_CLI_STATUS_*
 * NOTES:
 */
uint8_t gattClientNotifsUnsubscribe(int conn, const struct uuid *inThisService, const struct uuid *thisChar, gattCliNotifSubscribeStateCbk stateCbk)
{
    uint8_t sta = GATT_CLI_STATUS_ERR;
    struct gattCliNotifRec *nr;
    struct gattCliCon *clicon;
    int cli = 0;

    pthread_mutex_lock(&mGattLock);
    clicon = gattCliconFindById(conn);
    if (!clicon)
        logw("No connection for unsubscribe\n");
    else {
        cli = clicon->cli->clientId;

        /* find the notif record, if one exists */
        for (nr = clicon->notifs; nr && (!uuidCmp(inThisService, &nr->notif->svc) || !uuidCmp(thisChar, &nr->notif->chr)); nr = nr->next);

        if (nr) {
            gattClientNotifUnsubscribe(clicon, nr);
            sta = GATT_CLI_STATUS_OK;
        }
    }
    pthread_mutex_unlock(&mGattLock);

    stateCbk(cli, conn, inThisService, thisChar, false, sta);

    return sta;
}




























