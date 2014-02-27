#include <pthread.h>
#include "workQueue.h"
#include "config.h"
#include "rfcomm.h"
#include "sendQ.h"
#include "l2cap.h"
#include "types.h"
#include "util.h"
#include "uniq.h"
#include "log.h"
#include "sg.h"
#include "bt.h"


//TODO: handle case of both peer and me establishing RFCOMM at the same time...
//TODO: audit lock sequence



#define RFC_STATE_INVALID       0 /* definitely not a velid state */
#define RFC_STATE_SVC_OPENING   1 /* connection was remotely opened and now we await our side's server to respond */
/* ... */
#define RFC_STATE_DISC_SENT     6 /* we sent a DISC and are waiting for a confirming UA */
#define RFC_STATE_TEARDOWN      7

struct rfcInst;

struct rfcConn { /* one per RFC channel */
    struct rfcConn        *next;
    struct rfcConn        *prev;
    struct rfcInst        *inst;
    rfc_conn_t             handle;

    rfcSvcEvent            evtCbk;
    void                  *evtCbkData;
    void                  *evtCbkInst;

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
    bool                   initiator; /* set if we initiated this conection *///XXX: set this on ones we initiated. all servers' DLCI values on initiator are odd, on other even

    struct rfcConn        *connList;
};

struct rfcListenState {
    struct rfcListenState *next;

    rfcSvcAlloc            allocCbk;
    rfcSvcEvent            evtCbk;
    void                  *cbkData;
    uint8_t                chan;
};

struct rfcSvcWork {
    uint8_t type;

    union {
        int more_real_things_here_later;
    };
};




#define RFC_CTRL_PF_MASK   0x10

#define RFC_BIT_EA         0x01
#define RFC_BIT_CR         0x02
#define RFC_DLCI_SHIFT     2 /* shifts off those two bits above */


/* ctrl values */
#define RFC_CTRL_DM        0x0F /* Disconnected Mode                 */
#define RFC_CTRL_SABM      0x2F /* Set Asynchronous Balanced Mode    */
#define RFC_CTRL_DISC      0x43 /* Disconnect command                */
#define RFC_CTRL_UA        0x63 /* Unnumbered Acknowledgement        */
#define RFC_CTRL_UIH       0xEF /* Unnumbered Info With Header Check */


/* DLCI 0 commands (C/R bit is bit 1) */
#define RFC_CMD_NSC        0x11 /* Non Supported Command   */
#define RFC_CMD_TEST       0x21 /* Test Command            */
#define RFC_CMD_RLS        0x51 /* Remote Line Status      */
#define RFC_CMD_FCOFF      0x61 /* Flow Control Off        */
#define RFC_CMD_PN         0x81 /* Parameter Negotiation   */
#define RFC_CMD_RPN        0x91 /* Remote Port Negotiation */
#define RFC_CMD_FCON       0xA1 /* Flow Control On         */
#define RFC_CMD_MSC        0xE1 /* Modem Status Command    */



struct rfcHdr {
    uint8_t                addr;
    uint8_t                ctrl;
    uint8_t                len; /* or start of len */

    /* extra length */
    /* data */
} __packed;





/* our global state */
static struct sendQ *mSendQ = NULL;
static struct workQueue *mWorkQ = NULL;
static pthread_t mWorkThread;

static pthread_mutex_t mInstsLock = PTHREAD_MUTEX_INITIALIZER;
static struct rfcInst *mInsts = NULL;

static pthread_mutex_t mListenersLock = PTHREAD_MUTEX_INITIALIZER;
static struct rfcListenState *mListeners = NULL;




/* fwd decls */
static uint8_t rfcFcs(const void *dataP, uint32_t len);


/*
 * FUNCTION: rfcFindConnByHandle
 * USE:      Find an rfcConn given its handle
 * PARAMS:   handle - the handle
 * RETURN:   the rfcConn struct or NULL if not found
 * NOTES:    If struct is found, this func will take the parent
 *           instance's lock. It is up to the caller to release
 *           it! This takes the mInstsLock during its operation
 *           too, but releases it internally.
 */
static struct rfcConn* rfcFindConnByHandle(rfc_conn_t handle)
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
 * FUNCTION: rfcCreateConnStruct
 * USE:      Create an rfcConn on a given RFCOMM instance
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI to use
 *           state - the state to set for the connection initially
 * RETURN:   the rfcConn struct or NULL on error
 * NOTES:    call with inst->lock held
 */
static struct rfcConn* rfcCreateConnStruct(struct rfcInst *inst, uint8_t dlci, uint8_t state)
{
    struct rfcConn *c = (struct rfcConn*)calloc(1, sizeof(struct rfcConn));

    if (!c)
        return NULL;

    c->txQueue = sgNew();
    if (!c->txQueue) {
        free(c);
        return NULL;
    }

    c->dlci = dlci;
    c->state = state;
    c->prev = NULL;
    c->inst = inst;
    c->handle = uniqGetNext();

    c->next = inst->connList;
    inst->connList = c;
    if (c->next)
        c->next->prev = c;

    return c;
}

/*
 * FUNCTION: rfcDeleteConnStruct
 * USE:      Delete an rfcConn
 * PARAMS:   c - the rfcConn
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcDeleteConnStruct(struct rfcConn* c)
{
    struct rfcInst *inst = c->inst;

    if (c->next)
        c->next->prev = c->prev;
    if (c->prev)
        c->prev->next = c->next;
    else
        inst->connList = c->next;

    if (c->evtCbk)
        c->evtCbk(c->evtCbkData, c->evtCbkInst, RFC_EVT_CLOSE, NULL);

    sgFree(c->txQueue);

    if (!inst->connList) {

        //TODO: close the RFCOMM instance after a timer?
    }
}

/*
 * FUNCTION: rfcFindConnByDlci
 * USE:      Find an rfcConn given its RFCOMM instance and the DLCI
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI in question
 * RETURN:   the rfcConn struct or NULL if not found
 * NOTES:    call with inst->lock held
 */
static struct rfcConn* rfcFindConnByDlci(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn* c;

    for(c = inst->connList; c && c->dlci != dlci; c = c->next);

    return c;
}

/*
 * FUNCTION: rfcSendCmd
 * USE:      Send a command packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           cmd - the command value to send
 *           dlci - the relevant DLCI the packet came in for
 *           setCrBit - set the C/R bit?
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendCmd(struct rfcInst *inst, uint8_t cmd, uint8_t dlci, bool setCrBit)
{
    uint8_t buf[sizeof(struct rfcHdr) + sizeof(uint8_t)/*FCS*/];
    struct rfcHdr *hdr = (struct rfcHdr*)buf;
    uint8_t addr = (dlci << 2) | RFC_BIT_EA;
    sg s;

    if (setCrBit)
        addr |= RFC_BIT_CR;

    utilSetLE8(&hdr->len, RFC_BIT_EA);
    utilSetLE8(&hdr->ctrl, cmd);
    utilSetLE8(&hdr->addr, addr);
    utilSetLE8(hdr + 1, rfcFcs(hdr, sizeof(struct rfcHdr)));

    s = sgNewWithCopyData(buf, sizeof(buf));
    if (!s)
        return false;

    if (sendQueueTx(mSendQ, inst->conn, s))
        return true;

    sgFree(s);
    return false;
}

/*
 * FUNCTION: rfcSendDm
 * USE:      Send a DM packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI the packet came in for
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendDm(struct rfcInst *inst, uint8_t dlci)
{
    return rfcSendCmd(inst, RFC_CTRL_DM, dlci, !inst->initiator);
}

/*
 * FUNCTION: rfcSendDisc
 * USE:      Send a DISC packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI the packet came in for
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendDisc(struct rfcInst *inst, uint8_t dlci)
{
    return rfcSendCmd(inst, RFC_CTRL_DISC, dlci, inst->initiator);
}

/*
 * FUNCTION: rfcSendSabm
 * USE:      Send a SABM packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI the packet came in for
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendSabm(struct rfcInst *inst, uint8_t dlci)
{
    return rfcSendCmd(inst, RFC_CTRL_SABM, dlci, inst->initiator);
}

/*
 * FUNCTION: rfcSendUa
 * USE:      Send a UA packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI the packet came in for
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendUa(struct rfcInst *inst, uint8_t dlci)
{
    return rfcSendCmd(inst, RFC_CTRL_UA, dlci, !inst->initiator);
}


/*
 * FUNCTION: rfcRxData
 * USE:      Handle incoming UIH (Data) packets
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 *           pfBit - was the P/F bit set in the packet header
 *           s - the incoming packet
 * RETURN:   true if we handled this RX and sg was/will be freed by us
 * NOTES:    call with inst->lock held
 */
static bool rfcRxData(struct rfcInst *inst, uint8_t dlci, bool pfBit, sg s)
{
    struct rfcConn *c = rfcFindConnByDlci(inst, dlci);

/* handle p/F bit ... */

    if (!c) {
        logi("Got data on unconnected DLCI %u\n", dlci);
        if (!rfcSendDm(inst, dlci))
            loge("Failed to send DM\n");
        return false;
    }

    if (rfcSchedEvt(inst, RFC_EVT_RX, s))
        return true;

    loge("Failed to schedule RFC RX work\n");
    return false;
}

/*
 * FUNCTION: rfcCloseConn
 * USE:      Close an rfcConn
 * PARAMS:   c - the rfcConn
 *           toolOtherSide - set to send a DISC packet to the other side
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcCloseConn(struct rfcConn *c, bool toolOtherSide)
{
    struct rfcInst *inst = c->inst;

    //TODO: switch on connnection state


    if (toolOtherSide) {
        if (rfcSendDisc(inst, c->dlci)) {
            c->state = RFC_STATE_DISC_SENT;
            //TODO: timer
            return;
        }
        loge("Failed to send DISC\n");
    }

    rfcDeleteConnStruct(c);
}

/*
 * FUNCTION: rfcRxChClose
 * USE:      Handle incoming DISC command and close the channel
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxChClose(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn *c = rfcFindConnByDlci(inst, dlci);

    if (!c) {
        logi("Got close on unconnected DLCI %u\n", dlci);
        if (!rfcSendDm(inst, dlci))
            loge("Failed to send DM\n");
    } else {

        if (!rfcSendUa(inst, dlci))
            loge("Failed to send UA\n");
        rfcCloseConn(c, false/* do not send disc*/);
    }
}

/*
 * FUNCTION: rfcRxChOpen
 * USE:      Handle incoming SABM command and open the channel
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxChOpen(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn *c = rfcFindConnByDlci(inst, dlci);

    if (c) {
        logi("Got open on connected DLCI %u\n", dlci);
        /* XXX: we will do nothing - is this a good idea? */

    } else if (!inst->initiator != !(dlci & 1)){ /* Server DLCIs are odd on initiator */

        logi("Attempt to connect to non-server DLCI %u\n", dlci);
        if (!rfcSendDm(inst, dlci))
            loge("Failed to send DM\n");

    } else {
        struct rfcListenState *ls;
        uint8_t chan = dlci >> 1; /* convert to channel number */

        pthread_mutex_lock(&mListenersLock);
        for (ls = mListeners; ls && ls->chan != chan; ls = ls->next);
        if (!ls) {
            logi("Attempt to connect to non-listening DLCI %u\n", dlci);
            if (!rfcSendDm(inst, dlci))
                loge("Failed to send DM\n");
        } else {
            c = rfcCreateConnStruct(inst, dlci, RFC_STATE_SVC_OPENING);
            if (!c) {
                logi("Failed to alloc conn struct for DLCI %u\n", dlci);
                if (!rfcSendDm(inst, dlci))
                    loge("Failed to send DM\n");
            } else if (!rfcSchedSvcAlloc(ls, c)) { //TODO: this will send a DM or a UA depending on success
                logi("Failed to schedule SVC open for DLCI %u\n", dlci);
                rfcDeleteConnStruct(c);
                if (!rfcSendDm(inst, dlci))
                    loge("Failed to send DM\n");
            }
        }
        pthread_mutex_unlock(&mListenersLock);
    }
}

/*
 * FUNCTION: rfcRx
 * USE:      Handle incoming packets
 * PARAMS:   inst - the instance allocated by rfcServiceAlloc()
 *           s - the incoming packet
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRx(struct rfcInst *inst, sg s)
{
    uint8_t buf[sizeof(struct rfcHdr) + sizeof(uint8_t)]; /* max length of header */
    struct rfcHdr *hdr = (struct rfcHdr*)buf;
    uint8_t t, addr, ctrl, wantedFcs, haveFcs;
    bool longLen = false, pfBit;
    uint16_t len;

    /* some sanity checking is in order */
    if (!sgLength(s)) {
        loge("RFCOMM frame too short to contain an FCS\n");
        goto err;
    }

    /* grab the FCS */
    sgSerialize(s, sgLength(s) - 1, 1, &wantedFcs);
    sgTruncBack(s, 1);
    wantedFcs = utilGetLE8(&wantedFcs);

    /* grab the header */
    if (!sgSerializeCutFront(s, hdr, sizeof(struct rfcHdr))) {
        loge("RFCOMM frame too short to contain a header\n");
        goto err;
    }
    addr = utilGetLE8(&hdr->addr);
    ctrl = utilGetLE8(&hdr->ctrl);
    pfBit = !!(ctrl & RFC_CTRL_PF_MASK);
    ctrl &= !RFC_CTRL_PF_MASK;

    /* grab length as per spec */
    len = utilGetLE8(&hdr->len);
    if (!(len & RFC_BIT_EA)) {
        if (!sgSerializeCutFront(s, buf + sizeof(struct rfcHdr), sizeof(uint8_t))) {
            loge("RFCOMM frame too short to contain long length\n");
            goto err;
        }
        longLen = true;
        len += ((uint16_t)utilGetLE8(buf + sizeof(struct rfcHdr))) << 8;
    }
    len >>= 1;

    /* some more sanity checking */
    if (sgLength(s) != len) {
        loge("RFCOMM frame data len %u and reported %u\n", sgLength(s), len);
        goto err;
    }

    /* calculate the FCS and check */
    haveFcs = (ctrl == RFC_CTRL_UIH) ? rfcFcs(hdr, sizeof(struct rfcHdr) - sizeof(uint8_t)) : rfcFcs(hdr, sizeof(struct rfcHdr) + longLen ?  sizeof(uint8_t) : 0);
    if (wantedFcs != haveFcs) {
        loge("RFCOMM FCS calculated %02X reported 0x%02X\n", haveFcs, wantedFcs);
        goto err;
    }

    if (!(addr & RFC_BIT_EA)) {
        loge("Packet's address is more than one byte!\n");
        goto err;
    }

    if (ctrl == RFC_CTRL_UIH) { /* data. Expect C/R != inst->initiator */
        if (!inst->initiator == !(addr & RFC_BIT_CR)) {
            loge("RFCOMM C/R for UIH set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
            goto err;
        }
        addr >>= RFC_DLCI_SHIFT;
        if (addr) {
            if (!rfcRxData(inst, addr, pfBit, s)) /* data */
                goto err;
        } else {
            if (!rfcRxControl(inst, pfBit, s)) /* control */
                goto err;
        }
    } else { /* control */
        if (sgLength(s))
            logw("Unexpected data in command packet: %ub. Ignored.\n", sgLength(s));
        sgFree(s);
        s = NULL;

        if (pfBit)
            logw("Unexpected P/F bit in command packet\n");

        switch (ctrl) {
        case RFC_CTRL_SABM: /* command. Expect C/R != inst->initiator */
            if (!inst->initiator == !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for SABM set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            rfcRxChOpen(inst, addr >> RFC_DLCI_SHIFT);
            break;
        case RFC_CTRL_UA: /* response. Expect C/R == inst->initiator */
            if (!inst->initiator != !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for UA set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            rfcRxAck(inst, addr >> RFC_DLCI_SHIFT);
            break;
        case RFC_CTRL_DM: /* response. Expect C/R == inst->initiator */
            if (!inst->initiator != !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for DM set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            rfcRxDm(inst, addr >> RFC_DLCI_SHIFT);
            break;
        case RFC_CTRL_DISC: /* command. Expect C/R != inst->initiator */
            if (!inst->initiator == !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for DISC set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            rfcRxChClose(inst, addr >> RFC_DLCI_SHIFT);
            break;
        default:
            loge("Unknown RFCOMM ctrl 0x%02X\n", ctrl);
            goto err;
            break;
        }
    }

    return;

err:
    loge("RFCOMM RX processing: abandoning %ub\n", sgLength(s));
    if (s)
        sgFree(s);
}

/*
 * FUNCTION: rfcConnect
 * USE:      Called to connect to a remote RFCOMM DLCI
 * PARAMS:   peerAddr - the address of the other device
 *           dlci - the DLCI we want to connect to
 *           evtF - callback to notify this caller of events
 *           cbkData - data to pass to the above callback
 *           instance - instance pointer to pass to the above callback
 * RETURN:   tru if you should  expect callbacks. false on immediate failure
 * NOTES:
 */
bool rfcConnect(const struct bt_addr *peerAddr, uint8_t dlci, rfcSvcEvent evtF, void *cbkData, void *instance)
{
    //TODO
    return false;
}

/*
 * FUNCTION: rfcTx
 * USE:      Called to TX data using RFCOMM
 * PARAMS:   connHandle - the connection handle
 *           s - the SG to send
 * RETURN:   non-negative number for bytes sent, negative for other status (RFC_TX_RET_*)
 * NOTES:    If some number of bytes were sent, they will be truncated off the front of the
 *           SG for you. If entire SG is sent, it will be freed and RFC_TX_RET_ENTIRE_SENT
 *           will be returned
 */
int32_t rfcTx(rfc_conn_t connHandle, sg s)
{
    struct rfcConn* conn;
    bool gotMoreData = false;
    int32_t ret;

    conn = rfcFindConnByHandle(connHandle);
    if (!conn)
        return RFC_TX_RET_NO_SUCH_CONN;

    /* we now hold conn->inst->lock */

    if (sgLength(s) + sgLength(conn->txQueue) <= conn->maxQueueSz) { /* queue all */
        sgConcat(conn->txQueue, s);
        gotMoreData = true;
        ret = RFC_TX_RET_ENTIRE_SENT;
    } else if (sgLength(conn->txQueue) < conn->maxQueueSz) { /* queue as much as we can */

        sg t = sgSplit(s, conn->maxQueueSz - sgLength(conn->txQueue));
        if (!t)
            ret = RFC_TX_RET_ERROR;
        else {
            sgSwap(t, s); /* now t has the front, and s the remainder */
            ret = sgLength(t);
            sgConcat(conn->txQueue, t);
            gotMoreData = true;
        }
    } else
        ret = RFC_TX_RET_STOP_SENDING;

    if (gotMoreData && !rfcSchedTryTx(conn))
        loge("Failed to schedule a TX attempt\n");

    pthread_mutex_unlock(&conn->inst->lock);

    return ret;
}

/*
 * FUNCTION: rfcInstanceState
 * USE:      Handle state changes on the RFCOMM instance
 * PARAMS:   userData - passed to l2cApiServicePsmRegister, unused
 *           instance - the instance allocated by rfcServiceAlloc()
 *           state - what happened?
 *           data - data pertinent to what happened
 *           len - length of said data
 * RETURN:   NONE
 * NOTES:
 */
static void rfcInstanceState(void *userData, void *instance, uint8_t state, const void *data, uint32_t len)
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
            rfcRx(inst, *(sg*)data);
            pthread_mutex_unlock(&inst->lock);
        }
        break;
    case L2C_STATE_CLOSED:
        //TODO: tell all clients! (lock their locks before telling them, to avoid concurrency issues...
        pthread_mutex_destroy(&inst->lock);
        sendQueueDelPackets(mSendQ, inst->conn);
        free(inst);
        logd("RFCOMM CLOSED\n");
        break;
    default:
        loge("RFCOMM unknown state command: %d\n", state);
        break;
    }
}

/*
 * FUNCTION: rfcServiceAlloc
 * USE:      Allocate an instance of RFCOMM
 * PARAMS:   userData - passed to l2cApiServicePsmRegister, unused
 *           conn - the connection
 *           instanceP - store the instance we allocate here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t rfcServiceAlloc(void *userData, l2c_handle_t conn, void **instanceP){

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
 * FUNCTION: rfcWorkQItemFreeCbk
 * USE:      Free an RFC work item in the WorkQ at WorkQ deletion time
 * PARAMS:   workItem - the work item
 * RETURN:   NONE
 * NOTES:
 */
static void rfcWorkQItemFreeCbk(void *workItem)
{
    struct rfcSvcWork *work = (struct rfcSvcWork*)workItem;

    //TODO

    free(work);
}

/*
 * FUNCTION: rfcWorkerThread
 * USE:      RFCOMM Worker thread
 * PARAMS:   unused- unused
 * RETURN:   ignored
 * NOTES:
 */
static void* rfcWorkerThread(void *unused)
{
    struct rfcSvcWork *work;
    int status;

    pthread_setname_np(pthread_self(), "bt_rfcomm_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&work);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (work->type) {
        //TODO
        default:
            loge("Unknown work type %d\n", work->type);
            break;
        }
        free(work);
    }

    return NULL;
}

/*
 * FUNCTION: rfcRegisterProfile
 * USE:      Register RFCOMM with L2C & init it
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
bool rfcRegisterProfile(void)
{
    static const struct l2cServicePsmDescriptor rfcommDescr = {
        .userData = NULL,
        .serviceInstanceAlloc = rfcServiceAlloc,
        .serviceInstanceStateCbk = rfcInstanceState,
        .serviceConnectionlessRx = NULL,
        .mtu = 0xFFE0, /* no real limits here */
    };

    mWorkQ = workQueueAlloc(RFC_NUM_OUTSTANDING_WORK_ITEMS);
    if (!mWorkQ)
        goto fail_work_q;

    if (pthread_create(&mWorkThread, NULL, rfcWorkerThread, NULL))
        goto fail_worker;

    mSendQ = sendQueueAlloc(8, "RFCOMM sendq");
    if (!mSendQ)
        goto fail_send_q;

    if (!l2cApiServicePsmRegister(PSM_RFCOMM, &rfcommDescr))
        goto fail_l2c_register;

    return true;


fail_l2c_register:
    sendQueueFree(mSendQ);

fail_send_q:
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorkThread, NULL);

fail_worker:
    workQueueFree(mWorkQ, rfcWorkQItemFreeCbk);

fail_work_q:
    return false;
}

/*
 * FUNCTION: rfcChanListenAvail
 * USE:      See if a given channel can be used to listen
 * PARAMS:   dlci - the dlci
 * RETURN:   the answer
 * NOTES:    call with mListenersLock held
 */
static bool rfcChanListenAvail(uint8_t chan)
{
    struct rfcListenState *c;

    for (c = mListeners; c && c->chan != chan; c = c->next);

    return !c;
}

/*
 * FUNCTION: rfcListen
 * USE:      Listen on a DLCI
 * PARAMS:   chReq - wanted channel or RFC_CHNUM_REQ_ANY
 *           allocF - callback to alloc an instance of this service
 *           evtF - callback to notify this service of events
 *           cbkData - data to pass to the above calbacks
 * RETURN:   the allocated DLCI or 0 on error
 * NOTES:    if you request an exat DLCI and it is not available, this will fail!
 */
uint8_t rfcListen(uint8_t chReq, rfcSvcAlloc allocF, rfcSvcEvent evtF, void *cbkData)
{
    struct rfcListenState *c;
    uint8_t ch = 0;

    /* allocate and link in the listener struct */
    c = (struct rfcListenState*)malloc(sizeof(struct rfcListenState));
    if (!c)
        return 0;

    c->allocCbk = allocF;
    c->evtCbk = evtF;
    c->cbkData = cbkData;

    pthread_mutex_lock(&mListenersLock);

    /* figure out what the user wanted */
    if (chReq < RFC_FIRST_CHAN + RFC_NUM_CHANS && chReq >= RFC_FIRST_CHAN) {
        if (!rfcChanListenAvail(chReq))
            goto out;
        ch = chReq;
    } else if (chReq == RFC_CHNUM_REQ_ANY) {
        uint8_t i;
        for (i = RFC_FIRST_CHAN; !ch && i < RFC_FIRST_CHAN + RFC_NUM_CHANS; i++)
            if (rfcChanListenAvail(i))
                ch = i;
    } else {
        loge("Attempt to listen on stange channel %u\n", chReq);
        goto out;
    }

    if (!ch) /* no channel found */
        goto out;

    /* finish preparing & link in the listener struct */
    c->chan = ch;
    c->next = mListeners;
    mListeners = c;

out:
    if (!ch)
        free(c);
    pthread_mutex_unlock(&mListenersLock);
    return ch;
}

/*
 * FUNCTION: rfcStopListen
 * USE:      Stop listening on a DLCI
 * PARAMS:   ch - the channel to stop listening on
 * RETURN:   true if listener was found and deleted
 * NOTES:
 */
bool rfcStopListen(uint8_t ch)
{
    struct rfcListenState *c, *p = NULL;
    bool ret = false;

    pthread_mutex_lock(&mListenersLock);

    /*find it */
    for (c = mListeners; c && c->chan != ch; p = c, c = c->next);

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


/*
 * FUNCTION: rfcFcs
 * USE:      Calculate an RFCOMM FCS value
 * PARAMS:   data - the data to calculate over
 *           len - length of said data
 * RETURN:   the FCS value
 * NOTES:
 */
static uint8_t rfcFcs(const void *dataP, uint32_t len)
{
    static const uint8_t crctab[] = {
        0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
        0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
        0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
        0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
        0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
        0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
        0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
        0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
        0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
        0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
        0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
        0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
        0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
        0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
        0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
        0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
        0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
        0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
        0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
        0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
        0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
        0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
        0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
        0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
        0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
        0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
        0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
        0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
        0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
        0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
        0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
        0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
    };

    const uint8_t *data = (const uint8_t*)dataP;
    uint8_t crcval = 0xFF;

    while(len--)
        crcval = crctab[crcval ^ (*data++)];

    return 0xFF - crcval;
}













