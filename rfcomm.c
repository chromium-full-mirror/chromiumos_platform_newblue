#include <stdlib.h>
#include <string.h>
#include "workQueue.h"
#include "config.h"
#include "rfcomm.h"
#include "timer.h"
#include "sendQ.h"
#include "l2cap.h"
#include "types.h"
#include "util.h"
#include "uniq.h"
#include "log.h"
#include "sg.h"
#include "bt.h"
#include "mt.h"




//TODO: handle case of both peer and me establishing RFCOMM at the same time...
//TODO: audit lock sequence
//TODO: honor "stopFlow" when sending back credits...


#define RFC_NUM_DLCIS           ((RFC_NUM_CHANS) * 2)
#define RFC_FIRST_DLCI          ((RFC_FIRST_CHAN) * 2)

#define RFC_MTU                 0xFF00 /* no limits */
#define RFC_MAX_RX_FRAME        0x3FFF /* to make sure it is representable in all stacks */

/* states for an rfcConn connection */
#define RFC_CONN_ST_INVALID       0 /* definitely not a valid state */
#define RFC_CONN_ST_WAITING       1 /* waiting for link to open */
#define RFC_CONN_ST_SVC_OPENING   2 /* connection was remotely opened and now we await our side's server to respond */
#define RFC_CONN_ST_SVC_OPENED    3 /* service open call finished. we do not stray here long */
#define RFC_CONN_ST_PN_SENT       4 /* we sent a PN to open a channel */
#define RFC_CONN_ST_NEED_SABM     5 /* we need to send a SABM */
#define RFC_CONN_ST_SENT_SABM     6 /* we sent a SABM */
#define RFC_CONN_ST_ESTABLISHED   7 /* connection is live */
#define RFC_CONN_ST_NEED_DISC     8 /* we need to send a DISC and are waiting for a chance to send it */
#define RFC_CONN_ST_SENT_DISC     9 /* we sent a DISC and are waiting for a confirming UA */
#define RFC_CONN_ST_GOT_DISC      10 /* we got a DISC replied */
#define RFC_CONN_ST_TEARDOWN      11 /* connection is being torn down */

/* states for an rfcInst instance */
#define RFC_INST_ST_INVALID       0 /* definitely not a valid state */
#define RFC_INST_ST_OPENED        1 /* other side opened connection */
#define RFC_INST_ST_WAITING       2 /* we requested a connection, it is waiting to be opened */
#define RFC_INST_ST_SABM_SENT     3 /* we sent a SABM on DLCI 0, waiting for UA */
#define RFC_INST_ST_SABM_WAIT     4 /* they opened a connection. We're waiting for SABM */
#define RFC_INST_ST_ESTABLISHED   5 /* we have a live link to the other side */
#define RFC_INST_ST_NEED_DISC     6 /* we need to send a DISC, waiting for a chance */
#define RFC_INST_ST_SENT_DISC     7 /* we sent a DISC, waiting for UA */
#define RFC_INST_ST_TEARDOWN      8 /* being torn down */



struct rfcInst;

struct rfcConn { /* one per RFC channel */
    struct rfcInst        *rfcInst;
    rfc_conn_t             handle;

    rfcSvcEvent            evtCbk;
    void                  *evtCbkData;
    void                  *evtCbkInst;

    uint32_t               maxQueueSz; /* max we'll queue for transmission */
    sg                     txQueue;

    uint16_t               mtu;
    uint8_t                dlci;
    uint8_t                cState;   /* RFC_CONN_ST_* */
    bool                   stopFlow; /* set if other side may not TX to us */
    bool                   stopTx;   /* set so we no longer send things */

    uint8_t                numTxCredits; /* how many packets we can send */
    uint8_t                numRxCredits; /* how many credits we owe the other side */
};

struct rfcInst { /* one per L2C connection */
    struct rfcInst        *next; /* list fo all the instances */
    struct rfcInst        *prev;

    l2c_handle_t           conn;
    uniq_t                 closeTimer;
    pthread_mutex_t        lock;
    struct bt_addr         addr;
    uint16_t               mtu;
    uint8_t                iState;
    bool                   encr;
    bool                   initiator; /* set if we initiated this conection */
    bool                   reqInFlight; /* set if there is a req inflight requiring a UA/DM reply */
    bool                   stopFlow; //TODO: check this before TXing *DATA*

    struct {

        uint16_t           maxFrm;
        uint8_t            numInitialCreds;
        struct rfcConn    *conn; /* or NULL */

    } dlcis[RFC_NUM_DLCIS + RFC_FIRST_DLCI/*makes addressing easier*/];
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
        struct {
            rfcSvcAlloc    allocCbk;
            rfcSvcEvent    evtCbk;
            void          *cbkData;
            rfc_conn_t     conn;
        }svcAlloc;
        struct {
            void          *evtCbkData;
            void          *evtCbkInst;
            rfcSvcEvent    evtCbk;
            sg             data;
            uint8_t        evtType;
        }evt;
    };
};

#define RFC_WORK_SVC_ALLOC 0
#define RFC_WORK_CONN_EVT  1





#define RFC_INIT_MAX_FRM   31

#define RFC_DLCI_CTRL      0

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


/* DLCI 0 commands (C/R bit is bit 1, E/A bit is 1) */
#define RFC_RSP_NSC        0x11 /* Non Supported Command Rsp   */
#define RFC_RSP_TEST       0x21 /* Test Response               */
#define RFC_CMD_TEST       0x23 /* Test Command                */
#define RFC_RSP_RLS        0x51 /* Remote Line Status Rsp      */
#define RFC_CMD_RLS        0x53 /* Remote Line Status          */
#define RFC_RSP_FCOFF      0x61 /* Flow Control Off Rsp        */
#define RFC_CMD_FCOFF      0x63 /* Flow Control Off            */
#define RFC_RSP_PN         0x81 /* Parameter Negotiation Rsp   */
#define RFC_CMD_PN         0x83 /* Parameter Negotiation       */
#define RFC_RSP_RPN        0x91 /* Remote Port Negotiation Rsp */
#define RFC_CMD_RPN        0x93 /* Remote Port Negotiation     */
#define RFC_RSP_FCON       0xA1 /* Flow Control On Rsp         */
#define RFC_CMD_FCON       0xA3 /* Flow Control On             */
#define RFC_RSP_MSC        0xE1 /* Modem Status Response       */
#define RFC_CMD_MSC        0xE3 /* Modem Status Command        */


/* all packets have this header */
struct rfcHdr {
    uint8_t                addr;
    uint8_t                ctrl;
    uint8_t                len; /* or start of len */

    /* extra length byte may be here */
    /* data  may be here*/
    /* FCS here */
} __packed;

/* packets on DLCI 0 have this header */
struct rfcSigFrm {
    uint8_t type;   /* plus EA bit */
    uint8_t length; /* plus EA bit */
} __packed;

struct rfcSigPn {
    uint8_t dlci;
    uint8_t carrier; /* bottom nibble: 0, top: credit-based negotiation */
    uint8_t priority;
    uint8_t zero1;      /* always zero */
    uint16_t maxFrameSz;
    uint8_t zero2;      /* always zero */
    uint8_t numCredits; /* 1..7 */
} __packed;

struct rfcSigMsc { /* we do not care for break signalling */
    uint8_t dlci_shl_2;
    uint8_t msc;
} __packed;

struct rfcSigRpnLong {/*if len == 8 */
    uint8_t dlci_shl_2;
    uint8_t baudrate;
    uint8_t linecfg; //bits/sym, parity, stop bits
    uint8_t flc;
    uint8_t xOnChr;
    uint8_t xOffChr;
    uint16_t pmMask;
} __packed;

struct rfcSigRpnShort {/*if len == 1 */
    uint8_t dlci_shl_2;
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
static void rfcCloseConn(struct rfcConn *c, bool tellOtherSide, bool tellOurSide);
static void rfcInstCloseTimerStart(struct rfcInst *inst, uint32_t timelen);
static bool rfcSendDm(struct rfcInst *inst, uint8_t dlci);
static bool rfcSendDisc(struct rfcInst *inst, uint8_t dlci);
static bool rfcSendSabm(struct rfcInst *inst, uint8_t dlci);
static void rfcTryTx(struct rfcConn* c);


static void rfcShow(const char *lbl, sg s)
{
#ifdef RFC_DEBUG
        char line[128] = "rfcomm *D* <ZLP>";
        uint32_t i;
        uint8_t v;

        for(i = 0; i < sgLength(s); i++) {
            sgSerialize(s, i, 1, &v);
            if (i & 15)
                sprintf(line + strlen(line), " %02X", v);
            else {
                if (i)
                    logd(line);
                sprintf(line, "rfcomm %s [%03X] %02X", lbl, i, v);
            }
        }
        logd(line);
#endif
}


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
    uint8_t idx;

    pthread_mutex_lock(&mInstsLock);
    for (i = mInsts; i; i = i->next) {
        pthread_mutex_lock(&i->lock);
        for(idx = RFC_FIRST_DLCI; idx < RFC_FIRST_DLCI + RFC_NUM_DLCIS && !c; idx++)
            if (i->dlcis[idx].conn && i->dlcis[idx].conn->handle == handle) {
                c = i->dlcis[idx].conn;
                break;
            }
        if (c)
            break;
        pthread_mutex_unlock(&i->lock);
    }
    pthread_mutex_unlock(&mInstsLock);

    return c;
}

/*
 * FUNCTION: rfcFindInstByAddr
 * USE:      Find an RFCOMM instance for a given address
 * PARAMS:   addr - the address
 * RETURN:   the instance or NULL if not found
 * NOTES:    call with mInstsLock
 */
static struct rfcInst* rfcFindInstByAddr(const struct bt_addr *addr)
{
    struct rfcInst *i;

    for (i = mInsts; i && memcmp(&i->addr, addr, sizeof(i->addr)); i = i->next);

    return i;
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
    return inst->dlcis[dlci].conn;
}

/*
 * FUNCTION: rfcInstCloseTimerCbk
 * USE:      Timer callback for RFCOMM per-instance close timer
 * PARAMS:   which - the timer that fired
 *           cbkData- unused
 * RETURN:   NONE
 * NOTES:
 */
static void rfcInstCloseTimerCbk(uniq_t which,  uint64_t cbkData)
{
    struct rfcInst *i;
    bool closeItNow = true;

    pthread_mutex_lock(&mInstsLock);
    for (i = mInsts; i && i->closeTimer != which; i = i->next);
    if (i) {
        pthread_mutex_lock(&i->lock);
        switch (i->iState){
        case RFC_INST_ST_INVALID:
            logw("timer fired for inst in unknown state\n");
            break;
        case RFC_INST_ST_WAITING:
        case RFC_INST_ST_SABM_SENT:
        case RFC_INST_ST_ESTABLISHED:
            i->iState = RFC_INST_ST_NEED_DISC;
            if (!i->reqInFlight) {
                if (rfcSendDisc(i, RFC_DLCI_CTRL)) {
                    i->iState = RFC_INST_ST_SENT_DISC;
                    closeItNow = false;
                 } else
                    logw("Failed to send DM on DLCI 0\n");
            } else
                closeItNow = false;
            break;
        case RFC_INST_ST_SABM_WAIT:
            if (!rfcSendDm(i, RFC_DLCI_CTRL))
                logw("Failed to send DM on DLCI 0\n");
            break;
        case RFC_INST_ST_SENT_DISC:
            logi("Other side timed out replying to DISC on DLCI 0. Forcefully closing conection\n");
            break;
        case RFC_INST_ST_NEED_DISC:
            logw("RFC inst timer while already trying to shut down. Forcing\n");
            break;
        case RFC_INST_ST_TEARDOWN:
            logd("Close requested while tearing down. ignoring\n");
            closeItNow = false;
            break;
        }
        if (closeItNow) {

            if (i->iState == RFC_INST_ST_WAITING)
                i->iState = RFC_INST_ST_TEARDOWN; /* not much else we can do */
            else
                l2cApiDisconnect(i->conn);
        }
        pthread_mutex_unlock(&i->lock);
    }
    pthread_mutex_unlock(&mInstsLock);
}

/*
 * FUNCTION: rfcInstCloseTimerStart
 * USE:      Set the auto-close timer for an RFC instance
 * PARAMS:   inst - the RFCOMM instance
 *           timelen - how long the timer is for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcInstCloseTimerStart(struct rfcInst *inst, uint32_t timelen)
{
    if (inst->closeTimer)
        timerCancel(inst->closeTimer);

    inst->closeTimer = timerSet(timelen, rfcInstCloseTimerCbk, 0);
    /* if that fails, there isn't much we can do anyways - but it is not fate, so leave it be */
}

/*
 * FUNCTION: rfcInstCloseTimerStop
 * USE:      Stop the auto-close timer for an RFC instance
 * PARAMS:   inst - the RFCOMM instance
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcInstCloseTimerStop(struct rfcInst *inst)
{
    if (inst->closeTimer)
        timerCancel(inst->closeTimer);

    inst->closeTimer = 0;
}

/*
 * FUNCTION: rfcNewWorkItem
 * USE:      Allocate a new work item
 * PARAMS:   evtType - the type to set
 * RETURN:   true if the work was scheduled
 * NOTES:
 */
static struct rfcSvcWork* rfcNewWorkItem(uint8_t evtType)
{
    struct rfcSvcWork* w = calloc(1, sizeof(struct rfcSvcWork));
    if (w)
        w->type = evtType;

    return w;
}

/*
 * FUNCTION: rfcSchedEvt
 * USE:      Schedule a call to the event callback of a connection in the worker thread
 * PARAMS:   conn - the connection for whom we want to TX
 *           evt - what event happened (RFC_EVT_*)
 *           sg - the data sg or NULL. If this func returns true, it owns sg now
 * RETURN:   true if the work was scheduled
 * NOTES:
 */
static bool rfcSchedEvt(struct rfcConn *conn, uint8_t evt, sg data)
{
    struct rfcSvcWork *wi = rfcNewWorkItem(RFC_WORK_CONN_EVT);
    if (wi) {

        wi->evt.evtCbkData = conn->evtCbkData;
        wi->evt.evtCbkInst = conn->evtCbkInst;
        wi->evt.evtCbk = conn->evtCbk;
        wi->evt.data = data;
        wi->evt.evtType = evt;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: rfcSchedEvtOpen
 * USE:      Schedule an open call to the event callback of a connection in the worker thread
 * PARAMS:   conn - the connection for whom we want to TX
 * RETURN:   true if the work was scheduled
 * NOTES:
 */
static bool rfcSchedEvtOpen(struct rfcConn *conn)
{
    sg s = sgNewWithCopyData(&conn->handle, sizeof(rfc_conn_t));

    if (!s)
        return false;

    if (rfcSchedEvt(conn, RFC_EVT_OPEN, s))
        return true;

    sgFree(s);
    return false;
}

/*
 * FUNCTION: rfcSchedSvcAlloc
 * USE:      Schedule an allocation of a service in the worker thread
 * PARAMS:   conn - the connection for whom we want to TX
 * RETURN:   true if the work was scheduled
 * NOTES:
 */
static bool rfcSchedSvcAlloc(struct rfcListenState *ls, struct rfcConn *conn)
{
    struct rfcSvcWork *wi = rfcNewWorkItem(RFC_WORK_SVC_ALLOC);
    if (wi) {

        wi->svcAlloc.allocCbk = ls->allocCbk;
        wi->svcAlloc.evtCbk = ls->evtCbk;
        wi->svcAlloc.cbkData = ls->cbkData;
        wi->svcAlloc.conn = conn->handle;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
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
    c->cState = state;
    c->rfcInst = inst;
    c->handle = uniqGetNext();
    c->numTxCredits = inst->dlcis[dlci].numInitialCreds;
    inst->dlcis[dlci].conn = c;

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
    struct rfcInst *inst = c->rfcInst;
    uint8_t i, nUsed = 0;

    sgFree(c->txQueue);
    inst->dlcis[c->dlci].numInitialCreds = 0;
    inst->dlcis[c->dlci].maxFrm = RFC_INIT_MAX_FRM;
    inst->dlcis[c->dlci].conn = NULL;
    free(c);

    for (i = RFC_FIRST_DLCI; i < RFC_NUM_DLCIS + RFC_FIRST_DLCI; i++)
        if (inst->dlcis[i].conn)
            nUsed++;

    if (!nUsed)
        rfcInstCloseTimerStart(inst, RFC_USE_TIMEOUT);
}

/*
 * FUNCTION: rfcCmdOrRspSend
 * USE:      Send a command/response packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           cmd - the command value to send
 *           dlci - the relevant DLCI the packet came in for
 *           setCrBit - set the C/R bit?
 * RETURN:   false on error
 * NOTES:    call with inst->lock held. This is for sending SABM/DISC/DM/UA type packets with no data
 */
static bool rfcCmdOrRspSend(struct rfcInst *inst, uint8_t cmd, uint8_t dlci, bool setCrBit)
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

    rfcShow("TX.c", s);
    if (sendQueueForceTx(mSendQ, inst->conn, s))
        return true;

    sgFree(s);
    return false;
}

/*
 * FUNCTION: rfcSendDm
 * USE:      Send a DM packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendDm(struct rfcInst *inst, uint8_t dlci)
{
    return rfcCmdOrRspSend(inst, RFC_CTRL_DM | RFC_CTRL_PF_MASK, dlci, !inst->initiator);
}

/*
 * FUNCTION: rfcSendDisc
 * USE:      Send a DISC packet to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the relevant DLCI
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcSendDisc(struct rfcInst *inst, uint8_t dlci)
{
    if (inst->reqInFlight)
        logw("ORDERING VIOLATION! Sent DISC while something was in flight\n");

    inst->reqInFlight = true;
    rfcInstCloseTimerStart(inst, RFC_CMD_TIMEOUT);
    return rfcCmdOrRspSend(inst, RFC_CTRL_DISC | RFC_CTRL_PF_MASK, dlci, inst->initiator);
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
    if (inst->reqInFlight)
        logw("ORDERING VIOLATION! Sent SABM while something was in flight\n");

    inst->reqInFlight = true;
    rfcInstCloseTimerStart(inst, RFC_CMD_TIMEOUT);
    return rfcCmdOrRspSend(inst, RFC_CTRL_SABM | RFC_CTRL_PF_MASK, dlci, inst->initiator);
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
    return rfcCmdOrRspSend(inst, RFC_CTRL_UA | RFC_CTRL_PF_MASK, dlci, !inst->initiator);
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
    bool ret = false;

    if (!c) {
        logi("Got data on unconnected DLCI %u\n", dlci);
        if (!rfcSendDm(inst, dlci))
            loge("Failed to send DM\n");
        return false;
    }

    if (pfBit && inst->dlcis[dlci].numInitialCreds) {
        uint8_t creds;
        if (!sgSerializeCutFront(s, &creds, sizeof(uint8_t)))
            logw("Got too short a packet for credits!\n");
        creds = utilGetLE8(&creds);
        c->numTxCredits += creds;
    } else if (pfBit)
        logw("P/F UIH RXed on non-credit-based DLCI\n");

    if (sgLength(s)) {
        c->numRxCredits++;

        if (rfcSchedEvt(c, RFC_EVT_RX, s))
            ret = true;
        else
            loge("Failed to schedule RFC RX work\n");
    } else {
        sgFree(s);
        ret = true;
    }

    if (c->numRxCredits >= RFC_THRESH_HELD_CREDITS)
        rfcTryTx(c); /* among other things sends back credits */

    return ret;
}

/*
 * FUNCTION: rfcUnwrapUIH
 * USE:      Undo what rfcWrapUIH() did. Do not call on buffers not created by rfcWrapUIH()
 * PARAMS:   nCreds - how many credits to send
 *           s - the SG with the data
 * RETURN:   success
 * NOTES:    call with inst->lock held
 */
static void rfcUnwrapUIH(int nCreds, sg s)
{
    struct rfcHdr hdr;

    sgTruncBack(s, sizeof(uint8_t)); /* delete FCS */
    sgSerializeCutFront(s, &hdr, sizeof(struct rfcHdr));
    if (!(utilGetLE8(&hdr.len) & RFC_BIT_EA))
        sgTruncFront(s, sizeof(uint8_t)); /* delete second length byte */

    if (nCreds)
        sgTruncFront(s, sizeof(uint8_t)); /* delete credits byte */
}

/*
 * FUNCTION: rfcWrapUIH
 * USE:      Wrap some data into a UIH frame
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI
 *           nCreds - how many credits to send
 *           s - the SG with the data
 * RETURN:   success
 * NOTES:    call with inst->lock held
 */
static bool rfcWrapUIH(struct rfcInst *inst, uint8_t dlci, int nCreds, sg s)
{
    uint8_t buf[sizeof(struct rfcHdr) + 2 * sizeof(uint8_t)], fcs;
    struct rfcHdr *hdr = (struct rfcHdr*)buf;
    uint8_t hdrLen = sizeof(struct rfcHdr);
    uint16_t len = sgLength(s);

    if (nCreds)
        len++;

    utilSetLE8(&hdr->addr, RFC_BIT_EA | (inst->initiator ? RFC_BIT_CR : 0) | (dlci << 2));
    utilSetLE8(&hdr->ctrl, RFC_CTRL_UIH | (nCreds ? RFC_CTRL_PF_MASK : 0));

    if (len <= 0x7F)
        utilSetLE8(&hdr->len, (len << 1) | RFC_BIT_EA);
    else {
        utilSetLE8(&hdr->len, len << 1);
        utilSetLE8(hdr + 1, len >> 7);
        hdrLen++;
    }
    if (nCreds)
        utilSetLE8(buf + hdrLen++, nCreds);

    utilSetLE8(&fcs, rfcFcs(hdr, sizeof(struct rfcHdr) - sizeof(uint8_t))); /* do not checksum length/credits */

    return sgConcatFrontCopy(s, hdr, hdrLen) && sgConcatBackCopy(s, &fcs, sizeof(uint8_t));
}

/*
 * FUNCTION: rfcWrapNoCredUIH
 * USE:      Wrap some data into a UIH frame
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI
 *           s - the SG with the data
 * RETURN:   success
 * NOTES:    call with inst->lock held
 */
static bool rfcWrapNoCredUIH(struct rfcInst *inst, uint8_t dlci, sg s)
{
    return rfcWrapUIH(inst, dlci, 0, s);
}

/*
 * FUNCTION: rfcTxControlSg
 * USE:      Send a control frame to the other side (data provided as an SG)
 * PARAMS:   inst - the RFCOMM instance
 *           sType - the type of frame to send
 *           data - the data SG
 * RETURN:   true if we queued the send, false else
 * NOTES:    call with inst->lock held
 */
static bool rfcTxControlSg(struct rfcInst *inst, uint8_t sType, sg data)
{
    struct rfcSigFrm sig;

    utilSetLE8(&sig.type, sType);
    utilSetLE8(&sig.length, (sgLength(data) << 1) | RFC_BIT_EA);

    
    if (sgConcatFrontCopy(data, &sig, sizeof(struct rfcSigFrm)) && rfcWrapNoCredUIH(inst, RFC_DLCI_CTRL, data)) {
        rfcShow("TX.c", data);
        if (sendQueueForceTx(mSendQ, inst->conn, data))
            return true;
    }

    return false;
}

/*
 * FUNCTION: rfcTxControl
 * USE:      Send a control frame to the other side
 * PARAMS:   inst - the RFCOMM instance
 *           sType - the type of frame to send
 *           data - the data to attach (will be copied)
 *           len - length of said data
 * RETURN:   true if we queued the send, false else
 * NOTES:    call with inst->lock held
 */
static bool rfcTxControl(struct rfcInst *inst, uint8_t sType, const void *data, uint8_t len)
{
    sg s;

    s = sgNewWithCopyData(data, len);
    if (!s)
        return false;

    if (rfcTxControlSg(inst, sType, s))
        return true;

    sgFree(s);
    return false;
}

/*
 * FUNCTION: rfcSendMsc
 * USE:      Send an MSC packet to the other side
 * PARAMS:   c - the connection struct
 *           command - send MSC command (else response)
 *           stopFlow - tell other side to not send?
 * RETURN:   true if we enqueued the TX
 * NOTES:    call with inst->lock held. Note that while we may send MSC frames to our heart's
 *           content anytime we want, they will only affect flow if credit-based flow control
 *           is not being used.
 */
static bool rfcSendMsc(struct rfcConn *c, bool command, bool stopFlow)
{
    struct rfcSigMsc msc;

    utilSetLE8(&msc.msc, RFC_BIT_EA | 0x8C /*good flags */ | (stopFlow ? 0x02 : 0x00));
    utilSetLE8(&msc.dlci_shl_2, (c->dlci << 2) | 0x02 | RFC_BIT_EA);

    return rfcTxControl(c->rfcInst, command ? RFC_CMD_MSC : RFC_RSP_MSC, &msc, sizeof(struct rfcSigMsc));
}

/*
 * FUNCTION: rfcInstFlowControl
 * USE:      Handle other side's requests for flow control
 * PARAMS:   inst - the RFCOMM instance
 *           allowFlow - the request from the other side
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcInstFlowControl(struct rfcInst *inst, bool allowFlow)
{
    inst->stopFlow = !allowFlow;
}

/*
 * FUNCTION: rfcRxControl
 * USE:      Handle incoming UIH (Data) packets for DLCI 0
 * PARAMS:   inst - the RFCOMM instance
 *           pfBit - was the P/F bit set in the packet header
 *           s - the incoming packet
 * RETURN:   true if we handled this RX and sg was/will be freed by us
 * NOTES:    call with inst->lock held
 */
static bool rfcRxControl(struct rfcInst *inst, bool pfBit, sg s)
{
    uint8_t sType, sLen, dlci;
    struct rfcSigFrm sig;
    struct rfcConn* conn;
    struct rfcSigMsc msc;
    struct rfcSigPn pn;
    struct rfcSigRpnShort rpnS;
    struct rfcSigRpnLong rpnL;

    if (!sgSerializeCutFront(s, &sig, sizeof(struct rfcSigFrm))) {
        logw("Too short a SIG frame\n");
        return false;
    }
    sType = utilGetLE8(&sig.type);
    if (!(sType & RFC_BIT_EA)) {
        logw("SIG frame type too long\n");
        return false;
    }
    sLen = utilGetLE8(&sig.length);
    if (!(sLen & RFC_BIT_EA)) {
        logw("SIG frame len too long\n");
        return false;
    }
    sLen >>= 1; /* convert to useful form */

    if (sLen != sgLength(s)) {
        logw("SIG frame claims %ub, SG claims %ub\n", sLen, sgLength(s));
        return false;
    }

    switch (sType) {
    case RFC_RSP_NSC: /* this should never happen since all we send are required commands */
        if (!sgSerializeCutFront(s, &sType, sizeof(uint8_t))) {
            logw("Too short a SIG.NSC frame\n");
            return false;
        }
        sType = utilGetLE8(&sType);
        logw("Other side refuses our command 0x%u\n", sType);
        break;
    case RFC_CMD_TEST:
        if (!rfcTxControlSg(inst, RFC_RSP_TEST, s)) {
            logw("Failed to reply to SIG.TEST\n");
            return false;
        }
        break;
 /* case RFC_RSP_TEST: - this can never happen since we never send a TEST command */
    case RFC_CMD_RLS:
        logd("RFCOMM RXed RLS\n");
        /* we send back same RLS data at we RXed */
        if (!rfcTxControlSg(inst, RFC_RSP_RLS, s)) {
            logw("Failed to reply to SIG.RLS\n");
            return false;
        }
        break;

 /* case RFC_RSP_RLS: - this can never happen since we never send a RLS command */
    case RFC_CMD_FCOFF:
        rfcInstFlowControl(inst, false);
        if (!rfcTxControl(inst, RFC_RSP_FCOFF, NULL, 0))
            logw("Failed to send SIG.RPN\n");
        break;
 /* case RFC_RSP_FCOFF: - this can never happen since we never send a FCOFF command */
    case RFC_CMD_PN:
    case RFC_RSP_PN:
        if (!sgSerializeCutFront(s, &pn, sizeof(struct rfcSigPn))) {
            logw("Too short a SIG.PN frame\n");
            return false;
        }
        if (utilGetLE8(&pn.zero1) || utilGetLE8(&pn.zero2) || (utilGetLE8(&pn.carrier) & 0x0F))
            logw("SIG.PN frame zeroes are not zeroed\n");

        dlci = utilGetLE8(&pn.dlci);
        if (dlci < RFC_FIRST_DLCI || dlci >= RFC_FIRST_DLCI + RFC_NUM_DLCIS) {
            logw("Invalid DLCI in PN: %u\n", dlci);
            return false;
        }

        conn = rfcFindConnByDlci(inst, dlci);
        if (conn) { /* fewer things allowed here */
            if (sType == RFC_CMD_PN) {
                if ((utilGetLE8(&pn.carrier) >> 4) == 0x0F) {
                    utilSetLE8(&pn.carrier, 0xE0);
                    utilSetLE8(&pn.numCredits, 0);
                } else {
                    utilSetLE8(&pn.carrier, 0x00);
                    utilSetLE8(&pn.numCredits, 0);
                }
                utilSetLE16(&pn.maxFrameSz, inst->dlcis[dlci].maxFrm);
                if (!rfcTxControl(inst, RFC_RSP_PN, &pn, sizeof(struct rfcSigPn)))
                    logw("Failed to send SIG.PN\n");
            } else {              /* see what they said */
                if (conn->cState == RFC_CONN_ST_PN_SENT) {

                    if ((utilGetLE8(&pn.carrier) >> 4) == 0x0E) {
                        inst->dlcis[dlci].numInitialCreds = utilGetLE8(&pn.numCredits);
                        logd("other side supports credit-based FC & gave us %u creds\n", inst->dlcis[dlci].numInitialCreds);
                    } else {
                       logd("Other side does not support credit-based FC\n");
                    }
                    inst->dlcis[dlci].maxFrm = utilGetLE16(&pn.maxFrameSz);
                    logd("Max len: %ub\n", inst->dlcis[dlci].maxFrm);

                    conn->cState = RFC_CONN_ST_NEED_SABM;
                    if (!inst->reqInFlight) {
                        conn->maxQueueSz = inst->dlcis[dlci].numInitialCreds ? inst->dlcis[dlci].numInitialCreds * inst->dlcis[dlci].maxFrm : RFC_MAX_CREDITLESS_QUEUE;
                        if (!rfcSendSabm(inst, dlci)) {
                            logw("Failed to send SABM\n");
                            rfcCloseConn(conn, false, true);
                        } else
                            conn->cState = RFC_CONN_ST_SENT_SABM;
                    }
                } else
                    logw("Unexpected SIG.PN.r while connected in state %u\n", conn->cState);
            }
        } else {
            if (sType == RFC_CMD_PN) { /* we accept all terms */
                if ((utilGetLE8(&pn.carrier) >> 4) == 0x0F) {
                    inst->dlcis[dlci].numInitialCreds = utilGetLE8(&pn.numCredits);
                    logd("other side supports credit-based FC & gave us %u creds\n", inst->dlcis[dlci].numInitialCreds);
                    utilSetLE8(&pn.carrier, 0xE0);
                    utilSetLE8(&pn.numCredits, RFC_NUM_REMOTE_CREDITS);
                } else {
                    logd("Other side does not support credit-based FC\n");
                    utilSetLE8(&pn.carrier, 0x00);
                    utilSetLE8(&pn.numCredits, 0);
                }
                if (!rfcTxControl(inst, RFC_RSP_PN, &pn, sizeof(struct rfcSigPn)))
                    logw("Failed to send SIG.PN\n");
                inst->dlcis[dlci].maxFrm = utilGetLE16(&pn.maxFrameSz);
                logd("Max len: %ub\n", inst->dlcis[dlci].maxFrm);
            } else
                logw("Unexpected SIG.PN.r while not connected\n");
        }
        break;
    case RFC_CMD_RPN:
        if (sLen == sizeof(struct rfcSigRpnShort)) {
            if (!sgSerializeCutFront(s, &rpnS, sizeof(struct rfcSigRpnShort))) {
                logw("Too short a SIG.RPN.1 frame\n");
                return false;
            }
            /* convert to as if long request*/
            dlci = utilGetLE8(&rpnS.dlci_shl_2) >> 2;
            utilSetLE8(&rpnL.baudrate, 0x08); /* 230,400 bps */
            utilSetLE8(&rpnL.linecfg, 0x30); /* 8N1 */
            utilSetLE8(&rpnL.flc, 0); /* none */
            utilSetLE8(&rpnL.xOnChr, 0x11); /* DC1 as usual */
            utilSetLE8(&rpnL.xOffChr, 0x13); /* DC3 as usual */
            utilSetLE16(&rpnL.pmMask, 0); /* no change at all */
        } else if (sLen == sizeof(struct rfcSigRpnLong)) {
            static const char *brs[] = {"2.4","4.8","7.2","9.6","19.2","38.4","57.6","115.2","230.4"};
            static const char *cfgs[] = {"N1","N1.5","O1","O1.5","N1","N1.5", "E1","E1.5", "N1","N1.5", "M1","M1.5", "N1","N1.5", "S1","S1.5"};
            if (!sgSerializeCutFront(s, &rpnL, sizeof(struct rfcSigRpnLong))) {
                logw("Too short a SIG.RPN.8 frame\n");
                return false;
            }
            dlci = utilGetLE8(&rpnS.dlci_shl_2) >> 2;
            if (utilGetLE8(&rpnL.baudrate) < sizeof(brs) / sizeof(*brs))
                logd("Conf[%2u].br = %sk\n", dlci, brs[utilGetLE8(&rpnL.baudrate)]);
            else
                logd("Conf[%2u].br = ?(%u)\n", dlci, utilGetLE8(&rpnL.baudrate));
            logd("Conf[%2u].lc = %u%s\n", dlci, (utilGetLE8(&rpnL.linecfg) & 3) + 5, cfgs[(utilGetLE8(&rpnL.linecfg) >> 2) & 0x0F]);
            logd("Conf[%2u].flc = 0x%02X\n", dlci, utilGetLE8(&rpnL.flc));
            logd("Conf[%2u].xOnChr  = 0x%02X\n", dlci, utilGetLE8(&rpnL.xOnChr));
            logd("Conf[%2u].xOffChr = 0x%02X\n", dlci, utilGetLE8(&rpnL.xOffChr));
        } else {
            logw("Unknown SIG.RPN len %u\n", sLen);
            return false;
        }
        /* send the request right back, say we accept */
        utilSetLE8(&rpnL.dlci_shl_2, (dlci << 2) | 0x02 | RFC_BIT_EA);
        utilSetLE16(&rpnL.pmMask, 0x3F77); /* accept all */
        if (!rfcTxControl(inst, RFC_RSP_RPN, &rpnL, sizeof(struct rfcSigRpnLong)))
            logw("Failed to send SIG.RPN\n");
        break;
 /* case RFC_RSP_RPN: - this can never happen since we never send a RPM command */
    case RFC_CMD_FCON:
        rfcInstFlowControl(inst, true);
        if (!rfcTxControl(inst, RFC_RSP_FCON, NULL, 0))
            logw("Failed to send SIG.RPN\n");
        break;
 /* case RFC_RSP_FCON: - this can never happen since we never send a FCON command */
    case RFC_CMD_MSC:
    case RFC_RSP_MSC:
        if (!sgSerializeCutFront(s, &msc, sizeof(struct rfcSigMsc))) {
            logw("Too short a SIG.MSC frame\n");
            return false;
        }
        conn = rfcFindConnByDlci(inst, utilGetLE8(&msc.dlci_shl_2) >> 2);
        if (!conn) {
            logw("SIG.MSC frame for non-connected DLCI\n");
            return false;
        }
        if (sType == RFC_CMD_MSC && !rfcSendMsc(conn, false, conn->stopFlow))
            logw("Failed to reply to SIG.MSC cmd\n");

        if (!inst->dlcis[conn->dlci].numInitialCreds)
            conn->stopTx = !!(utilGetLE8(&msc.msc) & 0x02);
        break;
    default:
        logw("Unknow SIG type 0x%02x\n", sType);
        return false;
    }
    
    sgFree(s);
    return true;
}

/*
 * FUNCTION: rfcFlow
 * USE:      Stop or restart flow from other side
 * PARAMS:   conn - the connection handle
 *           stop - thep the flow or start it
 * RETURN:   NONE
 * NOTES:    Effects may not be immediate!
 */
bool rfcFlow(rfc_conn_t connH, bool stop)
{
    struct rfcConn* c = rfcFindConnByHandle(connH);
    bool sendMsc = false, ret = true;

    if (!c)
        return false;

    sendMsc = (!c->stopFlow != !stop) && !c->rfcInst->dlcis[c->dlci].numInitialCreds;
    c->stopFlow = stop;
    if (sendMsc)
        ret = rfcSendMsc(c, true, c->stopFlow);
    pthread_mutex_unlock(&c->rfcInst->lock);

    return ret;
}

/*
 * FUNCTION: rfcCloseConn
 * USE:      Close an rfcConn
 * PARAMS:   c - the rfcConn
 *           tellOtherSide - set to send a DISC packet to the other side
 *           tellOurSide - send event to our client
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcCloseConn(struct rfcConn *c, bool tellOtherSide, bool tellOurSide)
{
    struct rfcInst *inst = c->rfcInst;
    bool delStruct = true, useDM = true;

    switch (c->cState) {
    case RFC_CONN_ST_INVALID:
        loge("Attempt to close a connection in INVALID state\n");
        tellOurSide = false;
        /* fallthrough */
    case RFC_CONN_ST_WAITING:
    case RFC_CONN_ST_PN_SENT:
    case RFC_CONN_ST_NEED_SABM:
        tellOtherSide = false;
        break;
    case RFC_CONN_ST_SVC_OPENING:
    case RFC_CONN_ST_SENT_SABM:
        c->cState = RFC_CONN_ST_TEARDOWN; /* when current op finishes, it will finish this up */
        tellOtherSide = tellOurSide = delStruct = false;
        break;
    case RFC_CONN_ST_ESTABLISHED:
    case RFC_CONN_ST_GOT_DISC:
        useDM = false;
    case RFC_CONN_ST_SVC_OPENED:
        if (tellOtherSide)
            delStruct = false;
        break;
    case RFC_CONN_ST_NEED_DISC:
    case RFC_CONN_ST_SENT_DISC:
    case RFC_CONN_ST_TEARDOWN:
        logw("Trying to disconnect a connection in state %u\n", c->cState);
        tellOtherSide = tellOurSide = delStruct = false;
        break;
    default:
        loge("Weird conn state %u in disconnect\n", c->cState);
        break;
    }

    if (tellOtherSide) {
        if (delStruct)
            logw("Cannot del conn struct if telling other side...need to wait for reply first!\n");
        if (useDM) {
            if (!rfcSendDm(inst, c->dlci))
                loge("Failed to send DM\n");
        } else if (inst->reqInFlight) {
            c->cState = RFC_CONN_ST_NEED_DISC;
        } else {
            if (!rfcSendDisc(inst, c->dlci))
                loge("Failed to send DISC\n");
            c->cState = RFC_CONN_ST_SENT_DISC;
        }
    }
    if (tellOurSide) {
        if (!rfcSchedEvt(c, RFC_EVT_CLOSE, NULL))
            logw("Failed to tell our side of close()\n");
    }
    if (delStruct)
        rfcDeleteConnStruct(c);
}


/*
 * FUNCTION: rfcSendNextCmd
 * USE:      Send the next command
 * PARAMS:   inst - the RFCOMM instance
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcSendNextCmd(struct rfcInst *inst)
{
    struct rfcConn *c;

    if (inst->iState == RFC_INST_ST_NEED_DISC) {

        if (!rfcSendDisc(inst, RFC_DLCI_CTRL)){
            loge("Failed to send DISC to DLCI 0\n");
            inst->iState = RFC_INST_ST_TEARDOWN;
            l2cApiDisconnect(inst->conn);
        } else {
            inst->iState = RFC_INST_ST_SENT_DISC;
        }
    } else {
        uint8_t i;

        for (i = RFC_FIRST_DLCI; i < RFC_FIRST_DLCI + RFC_NUM_DLCIS; i++) {
            c = rfcFindConnByDlci(inst, i);
            if (!c)
                continue;

            if (c->cState == RFC_CONN_ST_NEED_SABM) {
                if (rfcSendSabm(inst, i)) {
                    c->cState = RFC_CONN_ST_SENT_SABM;
                    break;
                } else {
                    loge("Failed to send SABM on DLCI %u\n", i);
                    rfcCloseConn(c, false, true);
                }
            } else if (c->cState == RFC_CONN_ST_NEED_DISC) {
                if (rfcSendDisc(inst, i)) {
                    c->cState = RFC_CONN_ST_SENT_DISC;
                    break;
                } else {
                    loge("Failed to send DISC on DLCI %u\n", i);
                    rfcCloseConn(c, false, true);
                }
            } else
                continue;
            break;
        }
    }
}

/*
 * FUNCTION: rfcConnSendPn
 * USE:      Send a proper PN CMD for purposes of opening a connection
 * PARAMS:   conn - the conn struct
 * RETURN:   false on error
 * NOTES:    call with inst->lock held
 */
static bool rfcConnSendPn(struct rfcConn *conn)
{
    struct rfcInst *inst = conn->rfcInst;
    struct rfcSigPn pn;

    utilSetLE8(&pn.dlci, conn->dlci);
    utilSetLE8(&pn.carrier, 0xF0);
    utilSetLE8(&pn.priority, 2);
    utilSetLE8(&pn.zero1, 0);
    utilSetLE8(&pn.zero2, 0);
    utilSetLE8(&pn.numCredits, RFC_NUM_REMOTE_CREDITS);
    utilSetLE16(&pn.maxFrameSz, RFC_MAX_RX_FRAME);

    return rfcTxControl(inst, RFC_CMD_PN, &pn, sizeof(struct rfcSigPn));
}

/*
 * FUNCTION: rfcOpenWaitingConns
 * USE:      Open all conns we've been waiting to open
 * PARAMS:   inst - the RFCOMM instance
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcOpenWaitingConns(struct rfcInst *inst)
{
    struct rfcConn *c;
    uint8_t i;

    for (i = RFC_FIRST_DLCI; i < RFC_FIRST_DLCI + RFC_NUM_DLCIS; i++) {
        c = rfcFindConnByDlci(inst, i);
        if (c && c->cState == RFC_CONN_ST_WAITING) {
            if (rfcConnSendPn(c))
                c->cState = RFC_CONN_ST_PN_SENT;
            else {
                loge("Failed to send PN for pending connection\n");
                rfcCloseConn(c, false, true);
            }
        }
    }

    if (!inst->reqInFlight)
        rfcSendNextCmd(inst); /* will open waiting conns one by one */
}

/*
 * FUNCTION: rfcRxUa
 * USE:      Handle incoming UA response
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxUa(struct rfcInst *inst, uint8_t dlci)
{
    bool sendNextInflight = false;
    struct rfcConn *c;
    uint8_t i;


    if (!inst->reqInFlight)
        logw("ORDERING VIOLATION! Got UA while nothing was in flight\n");

    if (inst->iState == RFC_INST_ST_SABM_SENT && dlci == RFC_DLCI_CTRL) {
        inst->iState = RFC_INST_ST_ESTABLISHED;
        rfcInstCloseTimerStop(inst);
        rfcOpenWaitingConns(inst);
    } else if (inst->iState == RFC_INST_ST_SENT_DISC && dlci == RFC_DLCI_CTRL) {
        inst->iState = RFC_INST_ST_TEARDOWN;
        rfcInstCloseTimerStop(inst);
        l2cApiDisconnect(inst->conn);
    } else if (dlci == RFC_DLCI_CTRL) {
        logw("Weird UA on DLCI 0 in state %u\n", inst->iState);
    } else {
        c = rfcFindConnByDlci(inst, dlci);
        if (!c)
            logd("Got UA on a non-connected DLCI %u\n", dlci);
        else {
            switch (c->cState){
            case RFC_CONN_ST_INVALID:
            case RFC_CONN_ST_WAITING:
            case RFC_CONN_ST_SVC_OPENING:
            case RFC_CONN_ST_SVC_OPENED:
            case RFC_CONN_ST_PN_SENT:
            case RFC_CONN_ST_NEED_SABM:
            case RFC_CONN_ST_ESTABLISHED:
            case RFC_CONN_ST_NEED_DISC:
            case RFC_CONN_ST_TEARDOWN:
            default:
                logw("Got unexpected UA for conn in state %u\n", c->cState);
                rfcCloseConn(c, true, true);
                break;
            case RFC_CONN_ST_SENT_SABM:
                rfcInstCloseTimerStop(inst);
                c->cState = RFC_CONN_ST_ESTABLISHED;
                if (!rfcSendMsc(c, true, false)) { /* must send this */
                    loge("Failed to send MSC.C\n");
                    rfcCloseConn(c, true, true);
                }
                rfcSchedEvtOpen(c);
                inst->reqInFlight = false;
                sendNextInflight = true;
                break;
            case RFC_CONN_ST_SENT_DISC:
                rfcInstCloseTimerStop(inst);
                c->cState = RFC_CONN_ST_TEARDOWN;
                inst->reqInFlight = false;
                sendNextInflight = true;
                rfcDeleteConnStruct(c);
                break;
            }
        }
    }

    inst->reqInFlight = false;
    rfcSendNextCmd(inst);
}

/*
 * FUNCTION: rfcRxDm
 * USE:      Handle incoming DM response
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxDm(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn *c;
    bool sendNextInflight = false;


    if (dlci == RFC_DLCI_CTRL) { /* in this case all else is irrelevant! */
        logi("Got DM on DLCI 0\n");
        if (inst->iState != RFC_INST_ST_TEARDOWN)
            l2cApiDisconnect(inst->conn);
        inst->iState = RFC_INST_ST_TEARDOWN;
        return;
    }

    c = rfcFindConnByDlci(inst, dlci);

    if (!c)
        logd("Got DM on a non-connected DLCI %u\n", dlci);
    else {
        if (inst->reqInFlight && (c->cState == RFC_CONN_ST_SENT_SABM || c->cState == RFC_CONN_ST_SENT_DISC))  {
            inst->reqInFlight = false;
            sendNextInflight = true;
        }
        rfcCloseConn(c, false, true);
    }

    if (sendNextInflight)
        rfcSendNextCmd(inst);
}

/*
 * FUNCTION: rfcRxDisc
 * USE:      Handle incoming DISC command and close the channel
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxDisc(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn *c = rfcFindConnByDlci(inst, dlci);


    if (dlci == RFC_DLCI_CTRL) {
        logd("Got DISC on DLCI 0\n");
        rfcSendUa(inst, RFC_DLCI_CTRL);
        inst->iState = RFC_INST_ST_TEARDOWN;
        l2cApiDisconnect(inst->conn);
    } else if (!c) {
        logi("Got close on unconnected DLCI %u\n", dlci);
        if (!rfcSendDm(inst, dlci))
            loge("Failed to send DM\n");
    } else {
        if (!rfcSendUa(inst, dlci))
            loge("Failed to send UA\n");
        c->cState = RFC_CONN_ST_GOT_DISC;
        rfcCloseConn(c, false, true);
    }
}

/*
 * FUNCTION: rfcRxSabm
 * USE:      Handle incoming SABM command and open the channel
 * PARAMS:   inst - the RFCOMM instance
 *           dlci - the DLCI the packet came in for
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcRxSabm(struct rfcInst *inst, uint8_t dlci)
{
    struct rfcConn *c;


    if (dlci == RFC_DLCI_CTRL) {

        if (inst->iState == RFC_INST_ST_SABM_WAIT) {
            if (rfcSendUa(inst, RFC_DLCI_CTRL)) {
                inst->iState = RFC_INST_ST_ESTABLISHED;
                rfcOpenWaitingConns(inst);
            } else {
                loge("Failed to send UA to SABM on DLCI 0\n");
                inst->iState = RFC_INST_ST_TEARDOWN;
                l2cApiDisconnect(inst->conn);
            }
        } else
            logw("Unexpected instance state %u on RXing SABM for DLCI 0\n", inst->iState);

        return;
    }

    c = rfcFindConnByDlci(inst, dlci);
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
            c = rfcCreateConnStruct(inst, dlci, RFC_CONN_ST_SVC_OPENING);
            if (!c) {
                logi("Failed to alloc conn struct for DLCI %u\n", dlci);
                if (!rfcSendDm(inst, dlci))
                    loge("Failed to send DM\n");
            } else if (!rfcSchedSvcAlloc(ls, c)) {
                logi("Failed to schedule SVC open for DLCI %u\n", dlci);
                c->cState = RFC_CONN_ST_SVC_OPENED;
                rfcCloseConn(c, true, false);
            } else
                c->maxQueueSz = inst->dlcis[dlci].numInitialCreds ? inst->dlcis[dlci].numInitialCreds * inst->dlcis[dlci].maxFrm : RFC_MAX_CREDITLESS_QUEUE;
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

    rfcShow("RX", s);

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
    ctrl &= ~RFC_CTRL_PF_MASK;

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
    if (ctrl == RFC_CTRL_UIH && pfBit)
        len++; /* allow a byte for credits */

    if (sgLength(s) != len) {
        loge("RFCOMM frame data len %u and reported %u\n", sgLength(s), len);
        goto err;
    }

    /* calculate the FCS and check */
    haveFcs = (ctrl == RFC_CTRL_UIH) ? rfcFcs(hdr, sizeof(struct rfcHdr) - sizeof(uint8_t)) : rfcFcs(hdr, sizeof(struct rfcHdr) + (longLen ?  sizeof(uint8_t) : 0));
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

        switch (ctrl) {
        case RFC_CTRL_SABM: /* command. Expect C/R != inst->initiator */
            if (!inst->initiator == !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for SABM set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            if (!pfBit) {
                logw("SABM w/o PF bit!\n");
                goto err;
            }
            rfcRxSabm(inst, addr >> RFC_DLCI_SHIFT);
            break;
        case RFC_CTRL_UA: /* response. Expect C/R == inst->initiator */
            if (!inst->initiator != !(addr & RFC_BIT_CR)) {
                loge("RFCOMM C/R for UA set wrong (i=%d,addr=x%02X)\n", inst->initiator, addr);
                goto err;
            }
            if (!pfBit) {
                logw("UA w/o PF bit!\n");
                goto err;
            }
            rfcRxUa(inst, addr >> RFC_DLCI_SHIFT);
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
            if (!pfBit) {
                logw("DISC w/o PF bit!\n");
                goto err;
            }
            rfcRxDisc(inst, addr >> RFC_DLCI_SHIFT);
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
 * FUNCTION: rfcInstNew
 * USE:      Allocate an instance of RFCOMM
 * PARAMS:   peerAddr - the peer address
 * RETURN:   the instance or NULL
 * NOTES:    call with mInstsLock held
 */
static struct rfcInst* rfcInstNew(const struct bt_addr *peerAddr)
{
    struct rfcInst *inst = calloc(1, sizeof(struct rfcInst));
    uint8_t i;

    if(!inst)
        return NULL;

    if (pthread_mutex_init(&inst->lock, NULL)) {
        free(inst);
        return NULL;
    }

    memcpy(&inst->addr, peerAddr, sizeof(inst->addr));
    inst->prev = NULL;

    for (i = 0; i < RFC_NUM_DLCIS + RFC_FIRST_DLCI; i++)
        inst->dlcis[i].maxFrm = RFC_INIT_MAX_FRM;

    inst->next = mInsts;
    mInsts = inst;
    if (inst->next)
        inst->next->prev = inst;

    return inst;
}

/*
 * FUNCTION: rfcInstFree
 * USE:      Free an instance of RFCOMM
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:    call with mInstsLock held. Do not call with any channels still open!
 */
static void rfcInstFree(struct rfcInst *inst)
{
    uint8_t i;

    for (i = RFC_FIRST_DLCI; i < RFC_NUM_DLCIS + RFC_FIRST_DLCI; i++)
        if (inst->dlcis[i].conn)
            logw("DLCI %u not closed in conn to "ADDRFMT" on free. Will leak!\n", i, ADDRCONV(inst->addr));

    if (inst->next)
        inst->next->prev = inst->prev;
    if (inst->prev)
        inst->prev->next = inst->next;
    else
        mInsts = inst->next;

    free(inst);
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

    if (gotMoreData)
        rfcTryTx(conn);

    pthread_mutex_unlock(&conn->rfcInst->lock);

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
    uint8_t i;

    switch(state) {
    case L2C_STATE_OPEN:
        logd("RFCOMM opened\n");
        if (len != sizeof(l2c_handle_t)) {
            loge("Bad data size for L2C_OPEN evt\n");
            break;
        }
        memcpy(&inst->conn, data, sizeof(l2c_handle_t));

        if (inst->iState == RFC_INST_ST_OPENED) {
            inst->iState = RFC_INST_ST_SABM_WAIT;
            break;
        } else if (inst->iState == RFC_INST_ST_WAITING) {
            if (rfcSendSabm(inst, RFC_DLCI_CTRL)) {
                inst->iState = RFC_INST_ST_SABM_SENT;
                break;
            } else
                loge("Failed to send SABM on DLCI 0\n");
        } else if (inst->iState == RFC_INST_ST_TEARDOWN)
            logd("Conn in teardown just as established. Closing\n");
        else
            logw("L2C conn opened but instance is in state %u. closing\n", inst->iState);

        l2cApiDisconnect(inst->conn);
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
        logd("RFCOMM CLOSED\n");
        pthread_mutex_lock(&inst->lock);
        inst->iState = RFC_INST_ST_TEARDOWN;
        for (i = RFC_FIRST_DLCI; i < RFC_NUM_DLCIS + RFC_FIRST_DLCI; i++) {
            if (!inst->dlcis[i].conn)
                continue;
            logw("Force-closing connection for dlci %u\n", i);
            rfcCloseConn(inst->dlcis[i].conn, false, true);
        }
        pthread_mutex_unlock(&inst->lock);
        pthread_mutex_destroy(&inst->lock);
        sendQueueDelPackets(mSendQ, inst->conn);
        rfcInstFree(inst);
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
static uint8_t rfcServiceAlloc(void *userData, l2c_handle_t conn, void **instanceP)
{
    struct bt_addr peer;
    struct rfcInst *i;
    uint8_t ret = SVC_ALLOC_FAIL_OTHER;


    pthread_mutex_lock(&mInstsLock);

    if (!l2cApiGetBtAddr(conn, &peer))
        goto out;

    i = rfcFindInstByAddr(&peer);
    if (i) {
        logi("Rejecting RFCOMM conn to "ADDRFMT" because a connection already exists\n", ADDRCONV(peer));
        goto out;
    }

    i = rfcInstNew(&peer);
    if (!i)
        goto out;

    i->conn = conn;
    i->iState = RFC_INST_ST_OPENED;
    *instanceP = i;
    ret = SVC_ALLOC_SUCCESS;

out:
    pthread_mutex_unlock(&mInstsLock);
    return ret;
}

/*
 * FUNCTION: rfcConnect
 * USE:      Called to connect to a remote RFCOMM DLCI
 * PARAMS:   peerAddr - the address of the other device
 *           dlci - the DLCI we want to connect to
 *           evtF - callback to notify this caller of events
 *           cbkData - data to pass to the above callback
 *           instance - instance pointer to pass to the above callback
 * RETURN:   true if you should  expect callbacks. false on immediate failure
 * NOTES:
 */
bool rfcConnect(const struct bt_addr *peerAddr, uint8_t ch, rfcSvcEvent evtF, void *cbkData, void *instance)
{
    uint8_t dlci = ch * 2;
    struct rfcInst *inst;
    struct rfcConn *conn;
    bool ret = false;

    if (ch < RFC_FIRST_CHAN || ch >= RFC_FIRST_CHAN + RFC_NUM_CHANS)
        return false;

    pthread_mutex_lock(&mInstsLock);
    inst = rfcFindInstByAddr(peerAddr);
    if (!inst) { /* need a new link? */

        inst = rfcInstNew(peerAddr);
        if (!inst)
            goto out;
        inst->iState = RFC_INST_ST_WAITING;
        inst->initiator = true;

        if (!l2cApiCreatePsmConnection(PSM_RFCOMM, peerAddr, RFC_MTU, rfcInstanceState, NULL, inst)) {
            rfcInstFree(inst);
            goto out;
        }
        //set timer for close....
    }
    if (!inst->initiator)
        dlci |= 1; /* initiator's server DLCIs are odd */

    conn = rfcFindConnByDlci(inst, dlci);
    if (conn) {
        logw("Refusing to open already open DLCI %u to "ADDRFMT"\n", dlci, ADDRCONV(*peerAddr));
        goto out;
    }

    conn = rfcCreateConnStruct(inst, dlci, RFC_CONN_ST_WAITING);
    if (!conn) {
        logw("Failed to create conn struct\n");
        goto out;
    }
    conn->evtCbk = evtF;
    conn->evtCbkData = cbkData;
    conn->evtCbkInst = instance;

    if (inst->iState == RFC_INST_ST_ESTABLISHED){
        if (!rfcConnSendPn(conn)) {/* we can only do more if a connection alreay exists */
            logw("Failed to send SIG.PN\n");
            rfcCloseConn(conn, false, true);
            goto out;
        }
        conn->cState = RFC_CONN_ST_PN_SENT;
    }
    ret = true;

out:
    pthread_mutex_unlock(&mInstsLock);
    return ret;
}

/*
 * FUNCTION: rfcTryTx
 * USE:      Try to make forward progress on TX for a given conn
 * PARAMS:   c - the conn
 * RETURN:   NONE
 * NOTES:    call with inst->lock held
 */
static void rfcTryTx(struct rfcConn* c)
{
    struct rfcInst *inst = c->rfcInst;
    uint32_t sendSz, maxData = inst->dlcis[c->dlci].maxFrm;
    uint8_t attachCreds = 0;
    sg s;

    /* if we cannot send - do not bother checking anything else */
    if (!sendQueueCanTx(mSendQ, inst->conn))
        return;

    if (!inst->dlcis[c->dlci].numInitialCreds) { /* not credit based */

        if (inst->stopFlow || c->stopTx || !sgLength(c->txQueue))
            return;
    } else { /* credit based */

        if (inst->stopFlow)
            logw("FCOFF ignored for credit-based conn\n");

        if (c->stopTx)
            logw("MSC.FC ignored for credit-based conn\n");

        if (!c->numTxCredits) {
            maxData = 0;          /* no credits to send - we cannot send data */
            if (!c->numRxCredits || !c->stopFlow) /* no credits to return - nothing to do */
                return;
            attachCreds = c->numRxCredits;
        } else if (c->numRxCredits && !c->stopFlow) {
            attachCreds = c->numRxCredits;
            maxData--; /* save space for credit return */
        } else if (!sgLength(c->txQueue))
            return;
    }

    sendSz = sgLength(c->txQueue);
    if (sendSz > maxData)
        sendSz = maxData;

    s = sgSplit(c->txQueue, sendSz);
    if (!s) {
        loge("Failed to split TX buf\n");
        return;
    }
    sgSwap(c->txQueue, s);

    if (rfcWrapUIH(inst, c->dlci, c->numRxCredits, s)) {
        rfcShow("TD.d", s);
        if (sendQueueTx(mSendQ, inst->conn, s)) {
            if (sendSz)
                c->numTxCredits--;
            c->numRxCredits = 0;
            return;
        } else
            rfcUnwrapUIH(c->numRxCredits, s);
    } else {
        sgConcat(s, c->txQueue);
        sgSwap(c->txQueue, s);
    }
    sgFree(s);
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
    struct rfcSvcWork *wi = (struct rfcSvcWork*)workItem;

    if (wi->type == RFC_WORK_CONN_EVT && wi->evt.data)
        sgFree(wi->evt.data);

    free(wi);
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
    void *evtCbkData, *evtCbkInst;
    struct rfcSvcWork *wi;
    struct rfcInst *inst;
    rfcSvcEvent evtCbk;
    struct rfcConn *c;
    int status;

    pthread_setname_np(pthread_self(), "bt_rfcomm_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case RFC_WORK_SVC_ALLOC:
            c = rfcFindConnByHandle(wi->svcAlloc.conn);
            if (!c) {
                logd("Saw stale svc alloc\n");
                break;
            }
            inst = c->rfcInst;
            if (c->cState == RFC_CONN_ST_TEARDOWN) {
                logd("connection closed before it could be opened\n");
                if (!rfcSendDm(c->rfcInst, c->dlci))
                    loge("Failed to send DM\n");
                rfcDeleteConnStruct(c);
            }
            else if (c->cState != RFC_CONN_ST_SVC_OPENING) {
                logw("rfc conn in bad state at svc aloc time\n");
                rfcCloseConn(c, true, true);
            } else {
                c->evtCbkInst = wi->svcAlloc.allocCbk(wi->svcAlloc.cbkData, &c->rfcInst->addr, c->dlci >> 1);
                if (c->evtCbkInst) {
                    c->cState = RFC_CONN_ST_ESTABLISHED;
                    c->evtCbk = wi->svcAlloc.evtCbk;
                    c->evtCbkData = wi->svcAlloc.cbkData;
                    rfcSchedEvtOpen(c);
                    if (!rfcSendUa(c->rfcInst, c->dlci)) {
                        loge("Failed to send ua\n");
                        rfcCloseConn(c, false, true);
                    }
                    if (!rfcSendMsc(c, true, false)) { /* must send this */
                        loge("Failed to send MSC.C\n");
                        rfcCloseConn(c, true, true);
                    }
                } else {
                    logd("rfc conn refused for dlci %u\n", c->dlci >> 1);
                    c->cState = RFC_CONN_ST_SVC_OPENED;
                    rfcCloseConn(c, true, false);
                }
            }
            pthread_mutex_unlock(&inst->lock);
            break;
        case RFC_WORK_CONN_EVT:
            wi->evt.evtCbk(wi->evt.evtCbkData, wi->evt.evtCbkInst, wi->evt.evtType, wi->evt.data);
            break;
        default:
            loge("Unknown work type %d\n", wi->type);
            break;
        }
        free(wi);
    }

    return NULL;
}

/*
 * FUNCTION: rfcInit
 * USE:      Register RFCOMM with L2C & init it
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool rfcInit(void)
{
    static const struct l2cServicePsmDescriptor rfcommDescr = {
        .userData = NULL,
        .serviceInstanceAlloc = rfcServiceAlloc,
        .serviceInstanceStateCbk = rfcInstanceState,
        .serviceConnectionlessRx = NULL,
        .mtu = RFC_MTU,
    };

    mWorkQ = workQueueAlloc(RFC_NUM_OUTSTANDING_WORK_ITEMS);
    if (!mWorkQ)
        goto fail_work_q;

    if (pthread_create(&mWorkThread, NULL, rfcWorkerThread, NULL))
        goto fail_worker;

    mSendQ = sendQueueAlloc(8);
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
 * FUNCTION: rfcDeinit
 * USE:      Cleanup RFC
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void rfcDeinit(void)
{
    l2cApiServicePsmUnregister(PSM_RFCOMM, NULL, NULL);
    sendQueueFree(mSendQ);
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorkThread, NULL);
    workQueueFree(mWorkQ, rfcWorkQItemFreeCbk);
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
 * FUNCTION: rfcClose
 * USE:      Request a close an RFC connection
 * PARAMS:   connH - the connection handle
 * RETURN:   NONE
 * NOTES:    callback will be called with "close" when done
 */
void rfcClose(rfc_conn_t connH)
{
    struct rfcConn *c = rfcFindConnByHandle(connH);
    struct rfcInst *inst;

    if (!c)
        return;

    inst = c->rfcInst;
    rfcCloseConn(c, true, true);
    pthread_mutex_unlock(&inst->lock);
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













