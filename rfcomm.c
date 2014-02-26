#include <pthread.h>
#include "rfcomm.h"
#include "sendQ.h"
#include "l2cap.h"
#include "types.h"
#include "uniq.h"
#include "log.h"
#include "sg.h"
#include "bt.h"




#define RFC_STATE_DISABLED      0
/* ... */
#define RFC_STATE_TEARDOWN      7

struct rfcInst;

struct rfcConn { /* one per RFC channel */
    struct rfcConn        *next;
    struct rfcConn        *prev;
    struct rfcInst        *inst;
    rfc_conn_t             handle;

    rfcSvcEvent            evtCbk;
    void                  *evtCbkData;

    uint32_t               maxQueueSz; /* max we'll queue for transmission */
    sg                     txQueue;

    uint16_t               mtu;
    uint8_t                dlci;
    uint8_t                state;

    uint8_t                numTxCredits; /* how many packets we can send */
    uint8_t                numRxCredits; /* how many credits we owe the other side */
};

struct rfcInst { /* one per L2C connection */
    struct rfcInst        *next; /* list fo all the instances */
    struct rfcInst        *prev;

    l2c_handle_t           conn;
    pthread_mutex_t        lock;
    struct bt_addr         addr;
    uint16_t               mtu;
    bool                   encr;

    struct rfcConn        *connList;
};

struct rfcListenState {
    struct rfcListenState *next;

    rfcSvcAlloc            allocCbk;
    rfcSvcEvent            evtCbk;
    void                  *cbkData;
    uint8_t                dlci;
};


/* our global state */
static struct sendQ *mSendQ = NULL;

static pthread_mutex_t mInstsLock = PTHREAD_MUTEX_INITIALIZER;
static struct rfcInst *mInsts = NULL;

static pthread_mutex_t mListenersLock = PTHREAD_MUTEX_INITIALIZER;
static struct rfcListenState *mListeners = NULL;




/*
 * FUNCTION: rfcommFindConnByHandle
 * USE:      Find an rfcConn given its handle
 * PARAMS:   handle - the handle
 * RETURN:   the rfcConn struct or NULL if not found
 * NOTES:    If struct is found, this func will take the parent
 *           instance's lock. It is up to the caller to release
 *           it! This takes the mInstsLock during its operation
 *           too, but releases it internally.
 */
static struct rfcConn* rfcommFindConnByHandle(rfc_conn_t handle)
{
    struct rfcConn *c = NULL;
    struct rfcInst *i;

    pthread_mutex_lock(&mInstsLock);
    for (i = mInsts; i; i = i->next) {
        pthread_mutex_lock(&i->lock);
        for(c = i->connList; c && c->handle != handle; c = c->next)
        if (c)
            break;
        pthread_mutex_unlock(&i->lock);
    }
    pthread_mutex_unlock(&mInstsLock);

    return c;
}

/*
 * FUNCTION: rfcommRx
 * USE:      Handle incoming packets
 * PARAMS:   inst - the instance allocated by rfcommServiceAlloc()
 *           s - the incoming packet
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcommRx(struct rfcInst *inst, sg s)
{
    //TODO
}

/*
 * FUNCTION: rfcommConnect
 * USE:      Called to connect to a remote RFCOMM DLCI
 * PARAMS:   peerAddr - the address of the other device
 *           dlci - the DLCI we want to connect to
 *           evtF - callback to notify this caller of events
 *           cbkData - data to pass to the above calbacks
 * RETURN:   tru if you shoudl expect callbacks. false on immediate failure
 * NOTES:
 */
bool rfcommConnect(const struct bt_addr *peerAddr, uint8_t dlci, rfcSvcEvent evtF, void *cbkData)
{
    //TODO
    return false;
}

/*
 * FUNCTION: rfcommTx
 * USE:      Called to TX data using RFCOMM
 * PARAMS:   connHandle - the connection handle
 *           s - the SG to send
 * RETURN:   non-negative number for bytes sent, negative for other status (RFC_TX_RET_*)
 * NOTES:    If some number of bytes were sent, they will be truncated off the front of the
 *           SG for you. If entire SG is sent, it will be freed and RFC_TX_RET_ENTIRE_SENT
 *           will be returned
 */
int32_t rfcommTx(rfc_conn_t connHandle, sg s)
{
    struct rfcConn* conn;
    int32_t ret;

    conn = rfcommFindConnByHandle(connHandle);
    if (!conn)
        return RFC_TX_RET_NO_SUCH_CONN;

    /* we now hold conn->inst->lock */

    if (sgLength(s) + sgLength(conn->txQueue) <= conn->maxQueueSz) { /* queue all */
        sgConcat(conn->txQueue, s);
        ret = RFC_TX_RET_ENTIRE_SENT;
    } else if (sgLength(conn->txQueue) < conn->maxQueueSz) { /* queue as much as we can */

        sg t = sgSplit(s, conn->maxQueueSz - sgLength(conn->txQueue));
        if (!t)
            ret = RFC_TX_RET_ERROR;
        else {
            sgSwap(t, s); /* now t has the front, and s the remainder */
            ret = sgLength(t);
            sgConcat(conn->txQueue, t);
        }
    } else
        ret = RFC_TX_RET_STOP_SENDING;

    //tell worker thread to try make progress on this instance....

    pthread_mutex_unlock(&conn->inst->lock);

    return ret;
}

/*
 * FUNCTION: rfcommInstanceState
 * USE:      Handle state changes on the RFCOMM instance
 * PARAMS:   userData - passed to l2cApiServicePsmRegister, unused
 *           instance - the instance allocated by rfcommServiceAlloc()
 *           state - what happened?
 *           data - data pertinent to what happened
 *           len - length of said data
 * RETURN:   NONE
 * NOTES:
 */
static void rfcommInstanceState(void *userData, void *instance, uint8_t state, const void *data, uint32_t len)
{
    struct rfcInst *inst = (struct rfcInst*)instance;

    switch(state) {
    case L2C_STATE_OPEN:
        logd("RFCOMM opened\n");
        break;
    case L2C_STATE_MTU:
        logd("RFCOMM MTU\n");
        if (len != sizeof(uint16_t))
            loge("MTU size unexpectedly %ub\n", len);
        inst->mtu = *(uint16_t*)data;
        break;
    case L2C_STATE_ENCR:
        logi("RFCOMM ENCR\n");
        inst->encr = true;
        break;
    case L2C_STATE_RX:
        if (len != sizeof(sg))
            loge("unexpected length for RX\n");
        else {
            pthread_mutex_lock(&inst->lock);
            rfcommRx(inst, *(sg*)data);
            pthread_mutex_unlock(&inst->lock);
        }
        break;
    case L2C_STATE_CLOSED:
        //TODO: tell all clients! (lock their locks before telling them, to avoid concurrency issues...
        pthread_mutex_destroy(&inst->lock);
        free(inst);
        logd("RFCOMM CLOSED\n");
        break;
    default:
        loge("RFCOMM unknown state command: %d\n", state);
        break;
    }
}

/*
 * FUNCTION: rfcommServiceAlloc
 * USE:      Allocate an instance of RFCOMM
 * PARAMS:   userData - passed to l2cApiServicePsmRegister, unused
 *           conn - the connection
 *           instanceP - store the instance we allocate here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t rfcommServiceAlloc(void *userData, l2c_handle_t conn, void **instanceP){

    struct rfcInst *inst = calloc(1, sizeof(struct rfcInst));

    if(!inst)
        return SVC_ALLOC_FAIL_OTHER;

    if (!l2cApiGetBtAddr(conn, &inst->addr) || pthread_mutex_init(&inst->lock, NULL)) {
        free(inst);
        return SVC_ALLOC_FAIL_OTHER;
    }

    inst->conn = conn;
    inst->prev = NULL;
    *instanceP = inst;

    pthread_mutex_lock(&mInstsLock);
    inst->next = mInsts;
    mInsts = inst;
    if (inst->next)
        inst->next->prev = inst;
    pthread_mutex_unlock(&mInstsLock);

    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: rfcommRegisterProfile
 * USE:      Register RFCOMM with L2C & init it
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
bool rfcommRegisterProfile(void)
{

    static const struct l2cServicePsmDescriptor rfcommDescr = {
        .userData = NULL,
        .serviceInstanceAlloc = rfcommServiceAlloc,
        .serviceInstanceStateCbk = rfcommInstanceState,
        .serviceConnectionlessRx = NULL,
        .mtu = 0xFFE0, /* no real limits here */
    };

    mSendQ = sendQueueAlloc(8, "RFCOMM sendq");
    if (!mSendQ)
        return false;

    if (l2cApiServicePsmRegister(PSM_RFCOMM, &rfcommDescr))
        return true;

    sendQueueFree(mSendQ);
    return false;
}

/*
 * FUNCTION: rfcommDlciListenAvail
 * USE:      See if a given DLCI can be used to listen
 * PARAMS:   dlci - the dlci
 * RETURN:   the answer
 * NOTES:    call with mListenersLock held
 */
static bool rfcommDlciListenAvail(uint8_t dlci)
{
    struct rfcListenState *c;

    for (c = mListeners; c && c->dlci != dlci; c = c->next);

    return !c;
}

/*
 * FUNCTION: rfcommListen
 * USE:      Listen on a DLCI
 * PARAMS:   dlciReq - wanted DLCI or RFC_DLCI_REQ_*
 *           allocF - callback to alloc an instance of this service
 *           evtF - callback to notify this service of events
 *           cbkData - data to pass to the above calbacks
 * RETURN:   the allocated DLCI or 0 on error
 * NOTES:    if you request an exat DLCI and it is not available, this will fail!
 */
uint8_t rfcommListen(uint8_t dlciReq, rfcSvcAlloc allocF, rfcSvcEvent evtF, void *cbkData) //returns DLCI. input = dlci specifically or RFCOMM_DLCI_*
{
    struct rfcListenState *c;
    uint8_t i, dlci = 0, dlciSearchFirst = 0, dlciSearchIncr = 0;



    /* allocate and link in the listener struct */
    c = (struct rfcListenState*)malloc(sizeof(struct rfcListenState));
    if (!c)
        return 0;

    c->allocCbk = allocF;
    c->evtCbk = evtF;
    c->cbkData = cbkData;

    pthread_mutex_lock(&mListenersLock);

    /* figure out what the user wanted */
    if (dlciReq < RFC_FIRST_DLCI + RFC_NUM_DLCIS && dlciReq >= RFC_FIRST_DLCI) {
        if (!rfcommDlciListenAvail(dlciReq))
            goto out;
        dlci = dlciReq;
    } else if (dlciReq == RFC_DLCI_REQ_PREFERENCE_NONE) {
        dlciSearchFirst = RFC_FIRST_DLCI;
        dlciSearchIncr = 1;
    } else if (dlciReq == RFC_DLCI_REQ_NEED_EVEN) {
        dlciSearchFirst = (RFC_FIRST_DLCI + 1) &~ 1; /* next even */
        dlciSearchIncr = 2;
    } else if (dlciReq == RFC_DLCI_REQ_PREFERENCE_NONE) {
        dlciSearchFirst = (RFC_FIRST_DLCI &~ 1) + 1; /* next odd */
        dlciSearchIncr = 2;
    } else {
        loge("Attempt to listen on stange DLCI %u\n", dlciReq);
        goto out;
    }

    /* search for a DLCI if needed */
    for (i = dlciSearchFirst; !dlci && i < RFC_FIRST_DLCI + RFC_NUM_DLCIS; i += dlciSearchIncr)
        if (rfcommDlciListenAvail(i))
            dlci = i;

    if (!dlci) /* no DLCI found */
        goto out;

    /* finish preparing & link in the listener struct */
    c->dlci = dlci;
    c->next = mListeners;
    mListeners = c;

out:
    if (!dlci)
        free(c);
    pthread_mutex_unlock(&mListenersLock);
    return dlci;
}

/*
 * FUNCTION: rfcommStopListen
 * USE:      Stop listening on a DLCI
 * PARAMS:   dlci - the DLCI to free
 * RETURN:   true if listener was found and deleted
 * NOTES:
 */
bool rfcommStopListen(uint8_t dlci)
{
    struct rfcListenState *c, *p = NULL;
    bool ret = false;

    pthread_mutex_lock(&mListenersLock);

    /*find it */
    for (c = mListeners; c && c->dlci != dlci; p = c, c = c->next);

    if (c) {

        if (p)
            p->next = c->next;
        else
            mListeners = c->next;

        free(c);
        ret = true;
    }

    pthread_mutex_unlock(&mListenersLock);
    return ret;
}





