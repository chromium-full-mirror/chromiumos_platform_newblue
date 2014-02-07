//todo: only one LE connection attempt can exist at a time
//our "extra" param for event filtering canhandle other events to as long as we're willing to have a giant switch-case... and check against it in all structs
//todo: always request remote versions & features before using them (eg: le encrypt support) and before telling anyone a link is up!
//todo: always set flush timout to infinite

#include <semaphore.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "workQueue.h"
#include "vendorLib.h"
#include "hci_int.h"
#include "config.h"
#include "l2cap.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"


#define MAX_FTR_PAGES         4 /* we'll not get any more than this */
#define HCI_STAT_GOING_DOWN   0xFF


struct hciCmdHdr {
    uint16_t opcode;
    uint8_t paramLen;
} __packed;
#define CMD_MAKE_OPCODE(ogf, ocf) ((((uint16_t)((ogf) & 0x3f)) << 10) | ((ocf) & 0x03ff))
#define CMD_GET_OGF(opcode)       (((opcode) >> 10) & 0x3f)
#define CMD_GET_OCF(opcode)       ((opcode) & 0x03ff)


struct hciAclHdr {
    uint16_t hdr;
    uint16_t len;
} __packed;
#define ACL_HDR_MASK_CONN_ID      0x0FFF
#define ACL_HDR_MASK_PB           0x3000
#define ACL_HDR_MASK_BC           0xC000
#define ACL_HDR_PB_FIRST_NONAUTO  0x0000
#define ACL_HDR_PB_CONINUED       0x1000
#define ACL_HDR_PB_FIRST_AUTO     0x2000
#define ACL_HDR_PB_COMPLETE       0x3000

struct hciScoHdr {
    uint16_t hdr;
    uint8_t len;
} __packed;
#define SCO_HDR_MASK_CONN_ID      0x0FFF
#define SCO_HDR_MASK_STATUS       0x3000
#define SCO_STATUS_ALL_OK         0x0000
#define SCO_STATUS_UNKNOWN        0x1000
#define SCO_STATUS_NO_DATA        0x2000
#define SCO_STATUS_SOME_DATA      0x3000

struct hciEvtHdr {
    uint8_t code;
    uint8_t len;
} __packed;


/* returns true if this is the event we wanted, false else. Do not block! If evt is passed as NULL, stack is shutting down. Do not try to enqueue/dequeue handlers here either */
typedef bool (*hciEvtCbk)(struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID);
typedef void (*hciSimpleCbk)(void* cbkData1, void* cbkData2, bool stackGoingDown, struct hciEvtHdr *evt);

struct hciEvtWaitState {
    uniq_t                  evtWaitStateID; /* used to cancel THIS waiter */
    uniq_t                  forCmdID; /* used to cancel ALL waiters for this CMD */

    struct hciEvtWaitState *next;
    struct hciEvtWaitState *prev;

    hciEvtCbk               cbk;
    void                   *cbkData;

    uint16_t                extra; /* cmd for cmd complete/statue, subever for LE */
    uint8_t                 evtType;

    bool                    persistent; /* do not delete when it fires */
};

struct hciEvtWaitDescr {
    uint8_t    evtType; /* == 0 -> not valid/not enabled */
    uint16_t   extra;
    hciEvtCbk  cbk;
    void      *cbkData;
    bool       persistent;
};

struct hciEvtWaitDescrWithID {
    struct hciEvtWaitDescr descr;
    uniq_t id;
};

struct hciCmdSubmitWithCompleteSyncData {
    sem_t   *sem;
    void    *evtBuf;
    uint8_t  evtBufSz;
    bool     goingDown;
};

struct hciCmdSendWorkItem {
    uniq_t cmdID;
    uint8_t numEventWaiters;
    struct hciCmdHdr *cmd;

    /* even handlers */
    struct hciEvtWaitDescrWithID ewds[];

    /* command itself goes here */
};

#define CBK_WORK_ITEM_DONE_CBK      0
#define CBK_WORK_ITEM_SIMPLE_CBK    1
#define CBK_WORK_EDR_DISC_DEV       2
#define CBK_WORK_EDR_DISC_NAME      3
#define CBK_WORK_LE_DISC_DEV        4
#define CBK_WORK_FLUSH              5


struct hciCbkWorkItem {
    uint8_t       cbkWorkItemType;

    union {
        struct {
            hciOpDoneCbk                  cbk;
            void                         *cbkData;
            uint8_t                       status;
        } done;

        struct {
            hciSimpleCbk                  cbk;
            void                         *cbkData1;
            void                         *cbkData2;
            bool                          stackGoingDown;
            bool                          haveEvt;
        } simple;

        struct {
            struct bt_addr                addr;
            uint32_t                      devCls;
            struct hciNameGetInfo         fng;
            int8_t                        rssi;
            uint8_t                       eirLen;
            uint8_t                       eir[];
        } edrDev;

        struct {
            hciDeviceDiscoveredEdrNameCbk cbk;
            void                         *cbkData;
            uint8_t                       nameReqState;
            char                          name[];
        } edrName;

        struct {
            struct bt_addr                addr;
            uint8_t                       advType; 
            int8_t                        rssi;
            uint8_t                       advLen;
            uint8_t                       adv[];
        } leDev;

        struct {
            sem_t                        *sem;
        } flush;
    };
};

struct hciSimpleCbkData {
    hciSimpleCbk  cbk; /* may be null */
    void         *cbkData1;
    void         *cbkData2;
    uint8_t       expectedParamLenMin;
    uint8_t       expectedParamLenMax;
    uint8_t       wantEventItself  :1;
    uint8_t       complete         :1;
};

struct hciAclConn {
    struct hciAclConn *next;
    struct hciAclConn *prev;
    struct bt_addr     addr;

    uint16_t           id;

    uint8_t            state; //TODO: this
};

struct hciDiscoverNameState {
    struct hciDiscoverNameState   *next;
    hciDeviceDiscoveredEdrNameCbk  cbk;
    void                          *cbkData;
    uint8_t                        mac[6];
    uniq_t                         handle;
};

/* our state */

static hciAclRxCbk mAclRxF;
static hciScoRxCbk mScoRxF;

static pthread_mutex_t mCmdWaitLock = PTHREAD_MUTEX_INITIALIZER;
static struct hciEvtWaitState *mEvtWaitHead = NULL;
static struct hciEvtWaitState *mEvtWaitTail = NULL;

static pthread_mutex_t mCmdLock = PTHREAD_MUTEX_INITIALIZER;
static sem_t mCmdSendSem;
static struct workQueue* mCmdsSendWork;
static pthread_t mCmdsSendThread;

static struct workQueue* mCallbackWork;
static pthread_t mCallbackThread;

static pthread_mutex_t mConnsLock = PTHREAD_MUTEX_INITIALIZER;
static struct hciAclConn *mConns = NULL;

static pthread_mutex_t mDiscoveryStateLock = PTHREAD_MUTEX_INITIALIZER;

static hciDeviceDiscoveredEdrCbk mDiscoverEdrCbk;
static void *mDiscoverEdrData;
static uniq_t mDiscoverEdrHandle = 0;

static hciDeviceDiscoveredLeCbk mDiscoverLeCbk;
static void *mDiscoverLeData;
static uniq_t mDiscoverLeHandle = 0;

static struct hciDiscoverNameState *mDiscoverNames = NULL;





/* read from chip */
static uint8_t mBtVer = 0; /* HCI_VERSION_* */
static uint64_t mLocalFtrs[MAX_FTR_PAGES] = {0, };
static uint16_t mScoBufSz = 0;
static uint16_t mAclBufSzEdr = 0;
static uint16_t mAclBufSzLe = 0;
static bool mBtJointBuffers = true; /* use EDR buffers for LE */
static sem_t mAclPacketsEdr;
static sem_t mAclPacketsLe;
static sem_t mScoPackets;
static bool mHaveLe = false;
static bool mSimulLe = false;
static uint8_t mInquiryState = 0;


/* fwd declarations */
static bool hciBtStackUp(void);


/*
 * FUNCTION: hciConnFindById
 * USE:      Fidn a connection struct given an ID
 * PARAMS:   id - the id
 * RETURN:   connection struct or NULL if not found
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnFindById(uint16_t id)
{
    struct hciAclConn *c = mConns;

    while (c && c->id != id)
        c = c->next;

    return c;
}

/*
 * FUNCTION: hciEvtWaitDequeueInt
 * USE:      Dequeue a handler for an event we expect
 * PARAMS:   ws - the handler struct
 * RETURN:   NONE
 * NOTES:    call with mCmdWaitLock held
 */
static void hciEvtWaitDequeueInt(struct hciEvtWaitState *ws)
{
    if (ws->next)
        ws->next->prev = ws->prev;
    else
        mEvtWaitTail = ws->prev;

    if (ws->prev)
        ws->prev->next = ws->next;
    else
        mEvtWaitHead = ws->next;

    free(ws);
}

/*
 * FUNCTION: hciEvtWaitDequeue
 * USE:      Dequeue a handler for an event we expect
 * PARAMS:   evtWaitStateID - wait state ID from hciEvtWaitEnqueue()
 * RETURN:   true if dequeued, false if not found (already fired or never existed)
 * NOTES:
 */
static bool hciEvtWaitDequeue(uniq_t evtWaitStateID)
{
    struct hciEvtWaitState *ws;
    bool found = true;

    pthread_mutex_lock(&mCmdWaitLock);

    ws = mEvtWaitHead;
    while (ws && ws->evtWaitStateID != evtWaitStateID)
        ws = ws->next;

    if (ws)
        hciEvtWaitDequeueInt(ws);
    else
        found = false;

    pthread_mutex_unlock(&mCmdWaitLock);

    return found;
}
/*
 * FUNCTION: hciEvtWaitDequeueAllForCmd
 * USE:      Dequeue all handler for a given command ID
 * PARAMS:   forCmdID - command ID as passed to hciEvtWaitEnqueue();
 * RETURN:   how many handlers were dequeued
 * NOTES:
 */
static uint8_t hciEvtWaitDequeueAllForCmd(uniq_t forCmdID)
{
    struct hciEvtWaitState *ws, *t;
    uint8_t found = 0;

    pthread_mutex_lock(&mCmdWaitLock);

    ws = mEvtWaitHead;
    while (ws) {
        t = ws;
        ws = ws->next;

        if (t->forCmdID == forCmdID) {
            hciEvtWaitDequeueInt(ws);
            found++;
        }
    }

    pthread_mutex_unlock(&mCmdWaitLock);

    return found;
}

/*
 * FUNCTION: hciEvtWaitEnqueue
 * USE:      Enqueue a handler for an event we expect
 * PARAMS:   ewd - the event wait descriptor
 *           persistent - if set, handler will not be removed
 *                        when it fires and must be removed manually
 *           eventWaitID - the eventWaitID for this waiter. If 0, one will be allocated
 *           forCmdID - the commandID this is for
 * RETURN:   uniq_t identifying this event wait object or 0 on failure
 * NOTES:
 */
static uniq_t hciEvtWaitEnqueue(const struct hciEvtWaitDescr *ewd, uniq_t forCmdID)
{
    struct hciEvtWaitState *ws = (struct hciEvtWaitState*)malloc(sizeof(struct hciEvtWaitState));
    uniq_t ret;

    if (!ws) {
        loge("Failed to enqueue handler for event 0x%02X.%04X\n", ewd->evtType, ewd->extra);
        return 0;
    }

    ret = uniqGetNext();

    ws->cbk = ewd->cbk;
    ws->cbkData = ewd->cbkData;
    ws->evtType = ewd->evtType;
    ws->extra = ewd->extra;
    ws->persistent = ewd->persistent;
    ws->next = NULL;
    ws->evtWaitStateID = ret;
    ws->forCmdID = forCmdID;

    pthread_mutex_lock(&mCmdWaitLock);
    ws->prev = mEvtWaitTail;
    mEvtWaitTail = ws;
    if (ws->prev)
        ws->prev->next = ws;
    else
        mEvtWaitHead = ws;
    pthread_mutex_unlock(&mCmdWaitLock);

    return ret;
}

/*
 * FUNCTION: hciCbkChipHwUp
 * USE:      Called when the chip hw or fw are up
 * PARAMS:   cbkData - unused 
 *           fw - true if fw status, false if hw
 *           up - success of bringing said component up
 * RETURN:   true on success, false else
 * NOTES:
 */
static void hciCbkChipHwUp(void *cbkData, bool fw, bool up)
{
    logi("BT %cW %s!\n", fw ? 'F' : 'H', up ? "UP" : "DOWN");
}

/*
 * FUNCTION: hciRxSco
 * USE:      Called with a valid SCO packet
 * PARAMS:   hdr - packet header 
 *           data - the thing we got
 *           len - data length
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxSco(uint16_t hdr, void *data, uint8_t len)
{
    uint8_t loss = 0;

    switch (hdr & SCO_HDR_MASK_STATUS){
    case SCO_STATUS_ALL_OK:
        loss = SCO_LOSS_AMOUNT_NONE;
        break;
    case SCO_STATUS_UNKNOWN:
        loss = SCO_LOSS_AMOUNT_UNKNOWN;
        break;
    case SCO_STATUS_NO_DATA:
        loss = SCO_LOSS_AMOUNT_ALL;
        break;
    case SCO_STATUS_SOME_DATA:
        loss = SCO_LOSS_AMOUNT_SOME;
        break;
    }

    mScoRxF(hdr & SCO_HDR_MASK_CONN_ID, data, len, loss);
}

/*
 * FUNCTION: hciRxAcl
 * USE:      Called with a valid ACL packet/fragment
 * PARAMS:   hdr - packet header
 *           data - the thing we got
 *           len - data length
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxAcl(uint16_t hdr, void *data, uint16_t len)
{
    uint16_t pb = hdr & ACL_HDR_MASK_PB;
    uint16_t bc = hdr & ACL_HDR_MASK_BC;


    if (pb == ACL_HDR_PB_FIRST_NONAUTO || pb == ACL_HDR_PB_COMPLETE) {
        logw("Invalid acl.pb flag from chp: 0x%04X. Dropped\n", pb);
        return;
    }

    mAclRxF(hdr & SCO_HDR_MASK_CONN_ID, data, len, pb == ACL_HDR_PB_FIRST_AUTO);
}

/*
 * FUNCTION: hciRxEvt
 * USE:      Called with a valid EVT packet
 * PARAMS:   evt - the event
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxEvt(struct hciEvtHdr *evt)
{
    struct hciEvtWaitState *ws = NULL, *cur = NULL;
    bool handled = false, addCredit = false;
    uint8_t evtType = utilGetLE8(&evt->code);
    uint8_t len = utilGetLE8(&evt->len);
    uint32_t i;
    struct hciEvtLeMeta *leMeta = (struct hciEvtLeMeta*)(evt + 1);
    struct hciEvtCmdComplete *cmdComplete = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciEvtCmdStatus *cmdStatus = (struct hciEvtCmdStatus*)(evt + 1);

    /* handle the cases of credits: command complete */
    if (evtType == HCI_EVT_Command_Complete && utilGetLE8(&cmdComplete->numCmdCredits))
        addCredit = true;

    /* handle the cases of credits: command status */
    if (evtType == HCI_EVT_Command_Status && utilGetLE8(&cmdStatus->numCmdCredits))
        addCredit = true;

    /* handle data credits */
    if (evtType == HCI_EVT_Number_Of_Completed_Packets) {
        uint16_t newCreditsEdr = 0, newCreditsLe = 0;
        struct hciAclConn* conn;
        struct hciEvtNumCompletedPackets *ncp = (struct hciEvtNumCompletedPackets*)(evt + 1);
        struct hciEvtNumCompletedPacketsItem *items = ncp->items;

        pthread_mutex_lock(&mConnsLock);
        for (i = 0; i < utilGetLE8(&ncp->numHandles); i++, items++) {
            uint16_t connID = utilGetLE16(&items->conn);
            uint16_t numPackets = utilGetLE16(&items->numPackets);

            conn = hciConnFindById(connID);
            if (!conn)
                loge("Unknown conn 0x%04X in packets completed. Credits leaked: %d\n", connID, numPackets);
            else if (BT_ADDR_IS_LE(conn->addr) && !mBtJointBuffers)
                newCreditsLe += numPackets;
            else
                newCreditsEdr += numPackets;
        }
        pthread_mutex_unlock(&mConnsLock);
        while (newCreditsLe--)
            sem_post(&mAclPacketsLe);
        while (newCreditsEdr--)
            sem_post(&mAclPacketsEdr);
        handled = true;
    }

    /* handle command complete with ocf == 0 and ogf == 0 by ignoring it as per spec */
    if (evtType == HCI_EVT_Command_Complete && !utilGetLE16(&cmdComplete->opcode))
        handled = true;

    if (!handled) {
        pthread_mutex_lock(&mCmdWaitLock);
        ws = mEvtWaitHead;
        while (ws && !handled) {
            cur = ws;
            ws = ws->next;

            /* type match? If no, do not go on */
            if (evtType != cur->evtType)
                continue;

            /* if LE event, check code and skip handler if wrong */
            if (evtType == HCI_EVT_LE_Meta) {
                if(len < sizeof(struct hciEvtLeMeta)) {
                    logw("Got LE meta event of len %ub\n", len);
                    break;
                }
                if (utilGetLE8(&leMeta->subevent) != cur->extra)
                    continue;
            }

            /* same for command complete */
            if (evtType == HCI_EVT_Command_Complete) {
                if(len < sizeof(struct hciEvtCmdComplete)) {
                    logw("Got command complete event of len %ub\n", len);
                    break;
                }
                if (utilGetLE16(&cmdComplete->opcode) != cur->extra)
                    continue;
            }

            /* same for command status */
            if (evtType == HCI_EVT_Command_Status) {
                if(len < sizeof(struct hciEvtCmdStatus)) {
                    logw("Got command status event of len %ub\n", len);
                    break;
                }
                if (utilGetLE16(&cmdStatus->opcode) != cur->extra)
                    continue;
            }

            /* if we're here, this handler matches - let's see what it says */
            handled = cur->cbk(evt, cur->cbkData, cur->evtWaitStateID, cur->forCmdID);
        }
        if (handled && cur && !cur->persistent)
            hciEvtWaitDequeueInt(cur);

        pthread_mutex_unlock(&mCmdWaitLock);
    }

    if (addCredit)
        sem_post(&mCmdSendSem);

    if (!handled)
        logw("Unhandled HCI event code 0x%02X with %ub of params\n", evtType, len);
}

/*
 * FUNCTION: hciRx
 * USE:      Called when the chip gives us data/events
 * PARAMS:   cbkData - unused 
 *           typ - type of thing we're getting
 *           data - the thing we got
 *           len - the lenght
 * RETURN:   true on success, false else
 * NOTES:    we must free the "data" buffer at some point in time - we own it now
 */
static void hciRx(void *cbkData, uint8_t typ, void *data, uint32_t datalen)
{
    struct hciAclHdr *hdrAcl = (struct hciAclHdr*)data;
    struct hciScoHdr *hdrSco = (struct hciScoHdr*)data;
    struct hciEvtHdr *hdrEvt = (struct hciEvtHdr*)data;
    uint32_t len;
    uint16_t hdr;

    switch (typ){
    case HCI_PKT_TYP_ACL:
        if (datalen < sizeof(struct hciAclHdr)) {
            loge("Got ACL packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciAclHdr);
        hdr = utilGetLE16(&hdrAcl->hdr);
        len = utilGetLE16(&hdrAcl->len);
        if (len != datalen) {
            loge("ACL packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        hciRxAcl(hdr, hdrAcl + 1, len);
        break;
    case HCI_PKT_TYP_SCO:
        if (datalen < sizeof(struct hciScoHdr)) {
            loge("Got SCO packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciScoHdr);
        hdr = utilGetLE16(&hdrSco->hdr);
        len = utilGetLE8(&hdrSco->len);
        if (len != datalen) {
            loge("SCO packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        hciRxSco(hdr, hdrSco + 1, len);
        break;
    case HCI_PKT_TYP_EVT:
        if (datalen < sizeof(struct hciEvtHdr)) {
            loge("Got EVT packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciEvtHdr);
        len = utilGetLE8(&hdrEvt->len);
        if (len != datalen) {
            loge("EVT packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        hciRxEvt(hdrEvt);
        break;
    default:
        loge("Dropping unknown packet with type %d\n", typ);
        break;
    }
    free(data);
}

/*
 * FUNCTION: hciCallbackWorkDeallocator
 * USE:      Called to cleanup each item of callback workqueue at deinit time
 * PARAMS:   workItem - a work item
 * RETURN:   NONE
 * NOTES:
 */
static void hciCallbackWorkDeallocator(void *workItem)
{
    free(workItem);
}

/*
 * FUNCTION: hciCallbackWorkDeallocator
 * USE:      Called to cleanup each item of command send workqueue at deinit time
 * PARAMS:   workItem - a work item
 * RETURN:   NONE
 * NOTES:
 */
static void hciCmdSendWorkDeallocator(void *workItem)
{
    free(workItem);
}

/*
 * FUNCTION: hciCbkWorker
 * USE:      Worker thread for calling callbacks for HCI completed items
 * PARAMS:   unused - unused
 * RETURN:   unused
 * NOTES:
 */
static void* hciCbkWorker(void *unused)
{
    struct hciCbkWorkItem *w;
    int ret;

    while (1) {

        ret = workQueueGet(mCallbackWork, (void**)&w);
        if (ret)
            break;

        switch (w->cbkWorkItemType){
        case CBK_WORK_ITEM_DONE_CBK:
            w->done.cbk(w->done.cbkData, w->done.status);
            break;
        case CBK_WORK_ITEM_SIMPLE_CBK:
            w->simple.cbk(w->simple.cbkData1, w->simple.cbkData2, w->simple.stackGoingDown, w->simple.haveEvt ? (struct hciEvtHdr*)(w + 1) : NULL);
            break;
        case CBK_WORK_EDR_DISC_DEV:
            mDiscoverEdrCbk(mDiscoverEdrData, &w->edrDev.addr, w->edrDev.devCls, &w->edrDev.fng, w->edrDev.rssi, w->edrDev.eir, w->edrDev.eirLen);
            break;
        case CBK_WORK_EDR_DISC_NAME:
            w->edrName.cbk(w->edrName.cbkData, w->edrName.nameReqState, w->edrName.name);
            break;
        case CBK_WORK_LE_DISC_DEV:
            mDiscoverLeCbk(mDiscoverLeData, &w->leDev.addr, w->leDev.rssi, w->leDev.advType, w->leDev.adv, w->leDev.advLen);
            break;
        case CBK_WORK_FLUSH:
            sem_post(w->flush.sem);
            break;
        default:
            loge("Unknown callback work type %u\n", w->cbkWorkItemType);
            break;
        }
        free(w);
    }
    logd("HCI callback worker exiting\n");

    return NULL;
}

/*
 * FUNCTION: hciScheduleOpDoneCbk
 * USE:      Enqueue an op_done callback to be called
 * PARAMS:   cbk - the callback to call
 *           cbkData - the data for said callback
 *           status - the status byte
 * RETURN:   true if enqueue, false else
 * NOTES:
 */
static bool hciScheduleOpDoneCbk(hciOpDoneCbk cbk, void *cbkData, uint8_t status)
{
    struct hciCbkWorkItem *w;

    if (!cbk) {
        logw("Not scheduling done callback with a NULL function pointer\n");
        return true;
    }

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_ITEM_DONE_CBK;
    w->done.cbk = cbk;
    w->done.cbkData = cbkData;
    w->done.status = status;

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciScheduleSimpleCbk
 * USE:      Enqueue simple callback to be called
 * PARAMS:   cbk - the callback to call
 *           cbkData1 - the data for said callback
 *           cbkData2 - the data for said callback
 *           stackGoingDown - as the name implies...
 *           evt - event to provide, or NULL if none
 * RETURN:   true if enqueue, false else
 * NOTES:
 */
static bool hciScheduleSimpleCbk(hciSimpleCbk cbk, void *cbkData1, void *cbkData2, bool stackGoingDown, struct hciEvtHdr *evt)
{
    uint16_t evtLen = evt ? (sizeof(struct hciEvtHdr) + utilGetLE8(&evt->len)) : 0;
    uint32_t neededLen = sizeof(struct hciCbkWorkItem) + evtLen;
    struct hciCbkWorkItem *w;

    if (!cbk) {
        logw("Not scheduling simple callback with a NULL function pointer\n");
        return true;
    }

    w = (struct hciCbkWorkItem*)malloc(neededLen);
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_ITEM_SIMPLE_CBK;
    w->simple.cbk = cbk;
    w->simple.cbkData1 = cbkData1;
    w->simple.cbkData2 = cbkData2;
    w->simple.stackGoingDown = stackGoingDown;
    w->simple.haveEvt = evt != NULL;

    if (evt)
        memcpy(w + 1, evt, evtLen);

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciWorkFlush
 * USE:      Flush the workQueue
 * PARAMS:   NONE
 * RETURN:   true if success, false else
 * NOTES:    blocks till all existing work items have been finished
 */
static bool hciWorkFlush(void)
{
    struct hciCbkWorkItem *w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    bool ret = false;
    sem_t sem;

    if (!w) {
        loge("Failed to alloc workQ flush cmd\n");
        return false;
    }

    if (sem_init(&sem, 0, 0)) {
        free(w);
        loge("Failed to alloc workQ flush sem\n");
        return false;
    }

    w->cbkWorkItemType = CBK_WORK_FLUSH;
    w->flush.sem = &sem;

    if (workQueuePut(mCallbackWork, w)) {
        r_sem_wait(&sem);
        ret = true;
    } else
        free(w);

    sem_destroy(&sem);

    return ret;
}

/*
 * FUNCTION: hciCmdSendWorker
 * USE:      Worker thread for sending commands to the chip
 * PARAMS:   unused - unused
 * RETURN:   unused
 * NOTES:
 */
static void* hciCmdSendWorker(void *unused)
{
    struct hciCmdSendWorkItem *w;
    uint16_t len;
    int ret, i;

    while (1) {

        r_sem_wait(&mCmdSendSem);
        ret = workQueueGet(mCmdsSendWork, (void**)&w);
        if (ret)
            break;

        len = sizeof(struct hciCmdHdr) + utilGetLE8(&w->cmd->paramLen);

        /*
         * This lock makes sure adding a handler for an event and sending the
         * corresponding command are handled as one atomic unit.
         */
        pthread_mutex_lock(&mCmdLock);

        for (i = 0; i < w->numEventWaiters; i++) {
            w->ewds[i].id = hciEvtWaitEnqueue(&w->ewds[i].descr, w->cmdID);
            if (!w->ewds[i].id) {
                loge("Failed to enqueue handler [%d] \n", i);
                goto failed;
            }
        }

        if (vendorTx(HCI_PKT_TYP_CMD, w->cmd, len))
            goto success;

        loge("Failed to TX\n");

failed:
        loge("Command 0x%04X not sent\n", utilGetLE16(&w->cmd->opcode));
        while (i)
            hciEvtWaitDequeue(w->ewds[--i].id);

success:
        pthread_mutex_unlock(&mCmdLock);
        free(w);
    }
    logd("HCI command send worker exiting\n");

    return NULL;
}

/*
 * FUNCTION: hciUp
 * USE:      Bring up HCI (comms to bt chip)
 * PARAMS:   addr - the BT addr
 *           aclRxF - acl data callback
 *           scoRxF - sco data callback
 *           logAll - log all traffic?
 * RETURN:   true on success, false else
 * NOTES:    may take a while
 */
bool hciUp(const uint8_t *addr, hciAclRxCbk aclRxF, hciScoRxCbk scoRxF, bool logAll)
{
    int ret;


    logd("Vendor open\n");
    if (!vendorOpen(logAll)) {
        loge("Failed to open vendor lib\n");
        goto out_lib_open;
    }

    logd("Vendor up\n");
    if (!vendorUp(addr, hciCbkChipHwUp, hciRx, NULL)) {
        loge("Failed to bring up the chip\n");
        goto out_chip_up;
    }

    ret = sem_init(&mCmdSendSem, 0, 1);
    if (ret) {
        loge("Sem init failure\n");
        goto out_sem;
    }

    mCmdsSendWork = workQueueAlloc(HCI_NUM_OUTSTANDING_CMDS);
    if (!mCmdsSendWork) {
        loge("Failed to create command send workqueue\n");
        goto out_cmd_workq;
    }

    mCallbackWork = workQueueAlloc(HCI_NUM_OUTSTANDING_CBKS);
    if (!mCallbackWork) {
        loge("Failed to create hci callback workqueue\n");
        goto out_cbk_workq;
    }

    ret = pthread_create(&mCallbackThread, NULL, hciCbkWorker, NULL);
    if (ret) {
        loge("Failed(%d) to create hci callback worker\n", ret);
        goto out_cbk_worker;
    }

    ret = pthread_create(&mCmdsSendThread, NULL, hciCmdSendWorker, NULL);
    if (ret) {
        loge("Failed(%d) to create hci cmd send worker\n", ret);
        goto out_cmd_worker;
    }

    mAclRxF = aclRxF;
    mScoRxF = scoRxF;

    logd("Stack up\n");
    if (!hciBtStackUp()) {
        loge("Failed to bring up BT stack\n");
        goto out_stack_up;
    }

    return true;

out_stack_up:
    workQueueWakeAll(mCmdsSendWork, 1);
    sem_post(&mCmdSendSem); /* wakes up cmdSend worker if it is waiting on a cmd credit */
    pthread_join(mCmdsSendThread, NULL);

out_cmd_worker:
    workQueueWakeAll(mCallbackWork, 1);
    pthread_join(mCallbackThread, NULL);

out_cbk_worker:
    workQueueFree(mCallbackWork, hciCallbackWorkDeallocator);

out_cbk_workq:
    workQueueFree(mCmdsSendWork, hciCmdSendWorkDeallocator);

out_cmd_workq:
    sem_destroy(&mCmdSendSem);

out_sem:
    vendorDown();

out_chip_up:
    vendorClose();

out_lib_open:
    return false;
}

/*
 * FUNCTION: hciDown
 * USE:      Bring down HCI (comms to bt chip)
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    may take a while, some shutdown actions may go
 *           on after this (in higher layers)
 */
void hciDown(void)
{
    logd("Cancelling all event waiters\n");
    pthread_mutex_lock(&mCmdWaitLock);
    while (mEvtWaitHead) {
        mEvtWaitHead->cbk(NULL, mEvtWaitHead->cbkData, mEvtWaitHead->evtWaitStateID, mEvtWaitHead->forCmdID);
        hciEvtWaitDequeueInt(mEvtWaitHead);
    }
    pthread_mutex_unlock(&mCmdWaitLock);

    

    //more deinit here
    workQueueWakeAll(mCallbackWork, 1);
    workQueueWakeAll(mCmdsSendWork, 1);
    sem_post(&mCmdSendSem); /* wakes up cmdSend worker if it is waiting on a cmd credit */
    pthread_join(mCallbackThread, NULL);
    pthread_join(mCmdsSendThread, NULL);
    workQueueFree(mCallbackWork, hciCallbackWorkDeallocator);
    workQueueFree(mCmdsSendWork, hciCmdSendWorkDeallocator);
    sem_destroy(&mCmdSendSem);
    vendorDown();
    vendorClose();
}

/*
 * FUNCTION: hciCmdSubmit
 * USE:      Enqueue a command to be sent to the chip that will reply with a
 *           simple command complete/command status event. IF status, status
 *           will be used as the basis of callback "success" value. If
 *           complete, first byte of params.
 * PARAMS:   ogf - the command group
 *           ocf - the command code
 *           paramData - command params
 *           paramLen - parameter length
 *           ... - NULL-terminated list of const struct hciEvtWaitDescr* for wanted events
 * RETURN:   success
 * NOTES:    SHALL FINISH QUICKLY AND NOT BLOCK. WILL NOT DEADLOCK YOU! ALWAYS SAFE TO CALL!
 */
static bool hciCmdSubmit(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, ...)
{
    const struct hciEvtWaitDescr *ewd; 
    struct hciCmdSendWorkItem *w;
    uint8_t numHandlers = 0;
    va_list vl;

    /* count up the events we want to wait for */
    va_start(vl, paramLen);
    while (va_arg(vl, const struct hciEvtWaitDescr*))
        numHandlers++;
    va_end(vl);

    /* allocate the work item */
    w = (struct hciCmdSendWorkItem*)malloc(sizeof(struct hciCmdSendWorkItem) + sizeof(struct hciEvtWaitDescrWithID[numHandlers]) + paramLen + sizeof(struct hciCmdHdr));
    w->cmd = (struct hciCmdHdr*)(w->ewds + numHandlers);
    if (!w) {
        loge("Failed to alloc command work for cmd 0x%02X.%04X\n", ogf, ocf);
        return false;
    }

    /* copy wait state objects into it */
    numHandlers = 0;
    va_start(vl, paramLen);
    while ((ewd = va_arg(vl, const struct hciEvtWaitDescr*)) != NULL)
        memcpy(&w->ewds[numHandlers++].descr, ewd, sizeof(struct hciEvtWaitDescr));
    va_end(vl);
    w->numEventWaiters = numHandlers;

    /* copy the command in */
    utilSetLE16(&w->cmd->opcode, CMD_MAKE_OPCODE(ogf, ocf));
    utilSetLE16(&w->cmd->paramLen, paramLen);
    memcpy(w->cmd + 1, paramData, paramLen);

    /* try to enqueue it */
    if (workQueuePut(mCmdsSendWork, w))
        return true;

    /* in case of failure, free all that we allocated */
    free(w);
    loge("Failed to enqueue command\n");
    return false;
}

/*
 * FUNCTION: hciCmdSubmitSimpleCbk
 * USE:      Command callback for hciCmdSubmitSimple
 * PARAMS:   evt - the event
 *           cbkData - struct hciSimpleCbkData - our data
 *           evtWaitStateID - event waiterID
 *           forCmdID - commandID
 * RETURN:   true if we handled it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleCbk(struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    struct hciSimpleCbkData *cbkd = (struct hciSimpleCbkData*)cbkData;
    struct hciEvtCmdComplete *cmpl = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciEvtCmdStatus *stat = (struct hciEvtCmdStatus*)(evt + 1);
    const char* name = cbkd->complete ? "complete" : "status";
    uint8_t evtType;
    uint8_t paramLen;
    uint8_t evtHdrLen;

    if (!evt)
        return hciScheduleSimpleCbk(cbkd->cbk, cbkd->cbkData1, cbkd->cbkData2, true, NULL);

    evtType = utilGetLE8(&evt->code);
    paramLen = utilGetLE8(&evt->len);
    evtHdrLen = cbkd->complete ? sizeof(struct hciEvtCmdComplete) : sizeof(struct hciEvtCmdStatus);

    if (paramLen < evtHdrLen) {
        logw("Comand %s event too short @ %ub (wanted %ub+)\n", name, paramLen, evtHdrLen);
        return false;
    }
    paramLen -= evtHdrLen;

    /* is param length within bounds? */
    if (paramLen < cbkd->expectedParamLenMin || paramLen > cbkd->expectedParamLenMax) {
        logw("Got expected %s event, but wrong size: %u !< %u !< %u\n", name, 
            cbkd->expectedParamLenMin, paramLen, cbkd->expectedParamLenMax);
        return false;
    }

    /* enqueue the callback */
    return hciScheduleSimpleCbk(cbkd->cbk, cbkd->cbkData1, cbkd->cbkData2, false, cbkd->wantEventItself ? evt : NULL);
}

/*
 * FUNCTION: hciCmdSubmitSimple
 * USE:      Send a command and setup a handler for a command complete/command status for it
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - callback to call when command complete event arrives
 *           cbkData1 - data1 to pass to that callback
 *           cbkData2 - data2 to pass to that callback
 *           minEvtDataLen - minimum event data length to accept (excluding command complete/status event itself)
 *           maxEvtDataLen - maximum event data length to accept (excluding command complete/status event itself)
 *           wantEvt - pass a copy of the event to callback?
 *           expectComplete - expect command complete event? (else command status)
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimple(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciSimpleCbk cbk, void *cbkData1,
                               void *cbkData2, uint8_t minEvtDataLen, uint8_t maxEvtDataLen, bool wantEvt, bool expectComplete)
{
    struct hciEvtWaitDescr ewd;
    struct hciSimpleCbkData *cbkd = (struct hciSimpleCbkData*)malloc(sizeof(struct hciSimpleCbkData));

    if (!cbkd)
        return false;

    cbkd->cbk = cbk;
    cbkd->cbkData1 = cbkData1;
    cbkd->cbkData2 = cbkData2;
    cbkd->complete = expectComplete ? 1 : 0;
    cbkd->wantEventItself = wantEvt ? 1 : 0;
    cbkd->expectedParamLenMin = minEvtDataLen;
    cbkd->expectedParamLenMax = maxEvtDataLen;

    ewd.evtType = expectComplete ? HCI_EVT_Command_Complete : HCI_EVT_Command_Status;
    ewd.extra = CMD_MAKE_OPCODE(ogf, ocf);
    ewd.cbk = hciCmdSubmitSimpleCbk;
    ewd.cbkData = cbkd;
    ewd.persistent = false;

    if (hciCmdSubmit(ogf, ocf, paramData, paramLen, &ewd, NULL))
        return true;

    free(cbkd);
    return false;
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithComplete
 * USE:      Send a command and setup a handler for a command complete for it
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - callback to call when command complete event arrives
 *           cbkData1 - data1 to pass to that callback
 *           cbkData2 - data2 to pass to that callback
 *           minEvtDataLen - minimum event data length to accept (excluding command complete event itself)
 *           maxEvtDataLen - maximum event data length to accept (excluding command complete event itself)
 *           wantEvt - pass a copy of the event to callback?
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleWithComplete(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciSimpleCbk cbk,
                                           void *cbkData1, void *cbkData2, uint8_t minEvtDataLen, uint8_t maxEvtDataLen, bool wantEvt)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, cbk, cbkData1, cbkData2, minEvtDataLen, maxEvtDataLen, wantEvt, true);
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithStatus
 * USE:      Send a command and setup a handler for a command status for it
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - callback to call when command status event arrives
 *           cbkData1 - data1 to pass to that callback
 *           cbkData2 - data2 to pass to that callback
 *           minEvtDataLen - minimum event data length to accept (excluding command status event itself)
 *           maxEvtDataLen - maximum event data length to accept (excluding command status event itself)
 *           wantEvt - pass a copy of the event to callback?
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleWithStatus(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciSimpleCbk cbk,
                                         void *cbkData1, void *cbkData2, uint8_t minEvtDataLen, uint8_t maxEvtDataLen, bool wantEvt)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, cbk, cbkData1, cbkData2, minEvtDataLen, maxEvtDataLen, wantEvt, false);
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithCompleteNoReplyData
 * USE:      Send a command and setup a handler for a command complete for it. we do not want reply data
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - callback to call when command complete event arrives
 *           cbkData1 - data1 to pass to that callback
 *           cbkData2 - data2 to pass to that callback
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleWithCompleteNoReplyData(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciSimpleCbk cbk, void *cbkData1, void *cbkData2)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, cbk, cbkData1, cbkData2, 0, 0, false, true);
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithStatusNoReplyData
 * USE:      Send a command and setup a handler for a command status for it. we do not want reply data
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - callback to call when command status event arrives
 *           cbkData1 - data1 to pass to that callback
 *           cbkData2 - data2 to pass to that callback
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleWithStatusNoReplyData(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciSimpleCbk cbk, void *cbkData1, void *cbkData2)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, cbk, cbkData1, cbkData2, 0, 0, false, false);
}

/*
 * FUNCTION: hciCmdSubmitWithDoneSimpleCbk
 * USE:      Event callback for command complete event used by hciCmdSubmitSimpleWithCompleteWithDoneCbk()
 * PARAMS:   cbkData1 - 1st callback data (actually the original hciOpDoneCbk)
 *           cbkData2 - 2nd callback data (actually original data for hciOpDoneCbk)
 *           evt - the arrived event
 * RETURN:   true if we handled the event (always yes)
 * NOTES:
 */
static void hciCmdSubmitWithDoneSimpleCbk(void* cbkData1, void* cbkData2, bool goingDown, struct hciEvtHdr *evt)
{
    struct hciEvtCmdComplete *cmpl = (struct hciEvtCmdComplete*)(evt + 1);
    uint8_t *paramP = (uint8_t*)(cmpl + 1);
    hciOpDoneCbk cbkF = (hciOpDoneCbk)cbkData1;

    if (!hciScheduleOpDoneCbk(cbkF, cbkData2, goingDown ? HCI_STAT_GOING_DOWN : utilGetLE8(paramP)))
        loge("Failed to shedule done callback for simple command complete handler\n");
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithCompleteWithDoneCbk
 * USE:      Send a command and setup a handler for a command complete for it. call DoneCbk
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - doneCbk
 *           cbkData - data1 to pass to doneCbk
 * RETURN:   true if we enqueued it, false else
 * NOTES:    This only works if the first byte of the event is a "status" byte. WATCH OUT FOR THIS!!!
 */
static bool hciCmdSubmitSimpleWithCompleteWithDoneCbk(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciOpDoneCbk cbk, void *cbkData)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, hciCmdSubmitWithDoneSimpleCbk, (void*)cbk, cbkData, 1/* status byte required */, 255, true, true);
}

/*
 * FUNCTION: hciCmdSubmitCompleteSyncSimpleCbk
 * USE:      Event callback for command complete event used by hciCmdSubmitSimpleWithCompleteSync()
 * PARAMS:   cbkData1 - 1st callback data (struct hciCmdSubmitWithCompleteSyncData*)
 *           cbkData2 - 2nd callback data (unused)
 *           evt - the arrived event
 * RETURN:   NONE
 * NOTES:
 */
static void hciCmdSubmitCompleteSyncSimpleCbk(void* cbkData1, void* cbkData2, bool stackGoingDown, struct hciEvtHdr *evt)
{
    struct hciEvtCmdComplete *cmpl = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciCmdSubmitWithCompleteSyncData *data = (struct hciCmdSubmitWithCompleteSyncData*)cbkData1;
    void* evtData = (void*)(cmpl + 1);
    uint8_t paramLen;

    data->goingDown = stackGoingDown;

    if (stackGoingDown)
        sem_post(data->sem);

    paramLen = utilGetLE8(&evt->len) - sizeof(struct hciEvtCmdComplete);

    if (paramLen > data->evtBufSz) {
        logw("Sync complete event shortened %ud->%ud\n", paramLen, data->evtBufSz);
        paramLen = data->evtBufSz;
    }
    memcpy(data->evtBuf, evtData, paramLen);
    sem_post(data->sem);
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithCompleteSync
 * USE:      Send a command and setup a handler for a command complete for it. Wait for it to complete. Optionally provide resulting event back.
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           evtBuf - buffer where to put event (command complete event will be stripped, only params will remain)
 *           evtBufSz - size of said buffer. Event will be truncated if it does not fit
 * RETURN:   true if it happened. false else
 * NOTES:    Will block!
 */
static bool hciCmdSubmitSimpleWithCompleteSync(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, void* evtBuf, uint8_t evtBufSz)
{
    bool ret = true;
    sem_t sem;
    struct hciCmdSubmitWithCompleteSyncData data;

    if (pthread_self() == mCallbackThread) {
        loge("Refusing to issue sync command from callback thread. This WILL deadlock\n");
        return false;
    }

    data.sem = &sem;
    data.evtBuf = evtBuf;
    data.evtBufSz = evtBufSz;

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return false;
    }

    if (hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, hciCmdSubmitCompleteSyncSimpleCbk, &data, NULL, 0, 255, true, true)) {
        r_sem_wait(&sem);
        if (data.goingDown) {
            logi("going down\n");
            ret = false;
        }
    } else
        ret = false;

    sem_destroy(&sem);

    return ret;
}

/*
 * FUNCTION: hciCmdSubmitStatusSyncSimpleCbk
 * USE:      Event callback for command complete event used by hciCmdSubmitSimpleWithCompleteSync()
 * PARAMS:   cbkData1 - 1st callback data (sem_t* sem)
 *           cbkData2 - 2nd callback data (struct hciEvtCmdStatus* dstEvt)
 *           evt - the arrived event
 * RETURN:   NONE
 * NOTES:
 */
static void hciCmdSubmitStatusSyncSimpleCbk(void* cbkData1, void* cbkData2, bool stackGoingDown, struct hciEvtHdr *evt)
{
    struct hciEvtCmdStatus *stat = (struct hciEvtCmdStatus*)(evt + 1);
    sem_t *sem = (sem_t*)cbkData1;
    struct hciEvtCmdStatus* dstSta = (struct hciEvtCmdStatus*)cbkData2;

    if (stackGoingDown)
        utilSetLE8(&dstSta->status, HCI_STAT_GOING_DOWN);
    else
        memcpy(dstSta, stat, sizeof(struct hciEvtCmdStatus));
    sem_post(sem);
}

/*
 * FUNCTION: hciCmdSubmitSimpleWithStatusSync
 * USE:      Send a command and setup a handler for a command status for it. Wait for it. Provide resulting event back.
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           dstEvt - will store event here
 * RETURN:   true if it happened. false else
 * NOTES:    Will block!
 */
static bool hciCmdSubmitSimpleWithStatusSync(uint8_t ogf, uint16_t ocf, const void *paramData, uint8_t paramLen, struct hciEvtCmdStatus *dstEvt)
{
    bool ret = true;
    sem_t sem;

    if (pthread_self() == mCallbackThread) {
        loge("Refusing to issue sync command from callback thread. This WILL deadlock\n");
        return false;
    }

    if (sem_init(&sem, 0, 0)) {
        loge("sem init failed\n");
        return false;
    }

    if (hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, hciCmdSubmitStatusSyncSimpleCbk, &sem, dstEvt, 0, 0, true, false))
        r_sem_wait(&sem);
    else
        ret = false;

    sem_destroy(&sem);

    return ret;
}

/*
 * FUNCTION: hciSetLocalName
 * USE:      Set local device name
 * PARAMS:   name - the name (up to 248 non-NULL chars, NULL-terminated)
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetLocalName(const char *name, hciOpDoneCbk cbkDone, void *cbkData)
{
    struct hciWriteLocalName cmd = {{0,}, };
    unsigned len = strlen(name);
    
    if (len >= sizeof(cmd.name)) {
        memcpy(cmd.name, name, sizeof(cmd.name));
        if (len > sizeof(cmd.name))
            logw("Truncating BT name to %u chars\n", (int)sizeof(cmd.name));
    } else
        strcpy(cmd.name, name);

    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Local_Name, &cmd, sizeof(cmd), cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetDiscoverableConnectable
 * USE:      Set local discoverability/connectability
 * PARAMS:   discoverableState - NULL to leave as is, else pointer to wanted discoverability state
 *           connectableStateP - NULL to leave as is, else pointer to wanted connectablity state
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetDiscoverableConnectable(const bool *discoverableStateP, const bool *connectableStateP, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t newState = mInquiryState;

    if (discoverableStateP)
        newState = (newState &~ HCI_SCAN_ENABLE_INQUIRY) | (*discoverableStateP ? HCI_SCAN_ENABLE_INQUIRY : 0);

    if (connectableStateP)
        newState = (newState &~ HCI_SCAN_ENABLE_PAGE) | (*connectableStateP ? HCI_SCAN_ENABLE_PAGE : 0);

    if (newState == mInquiryState) { /* nothing to change - tell caller we're done */
        if (!hciScheduleOpDoneCbk(cbkDone, cbkData, 0)) {
            loge("Failed to shedule done callback\n");
            return false;
        }
    } else { /* things to do - do them */
        struct hciWriteScanEnable cmd;

        utilSetLE8(&cmd.state, newState);
        if (hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Scan_Enable, &cmd, sizeof(cmd), cbkDone, cbkData))
            mInquiryState = newState;
        else
            return false;
    }

    return true;
}

/*
 * FUNCTION: hciSetDeviceClass
 * USE:      Set local device class
 * PARAMS:   class - the class value
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetDeviceClass(uint32_t cls, hciOpDoneCbk cbkDone, void *cbkData)
{
    struct hciWriteClassOfDevice cmd;

    if (cls >> 24) 
        logw("Truncating device class to 24 bits\n");

    utilSetLE24(&cmd.cls, cls);
    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Class_Of_Device, &cmd, sizeof(cmd), cbkDone, cbkData);
}



/*
 * This generates simple SYNC functions for BT commands.
 * We need a lot of them, so we play with macros here.
 * It's either this, magic number mess, or hundreds of
 * functions. Deal with it.
 *
 *
 *
 * for all generated funcs this applies:
 *
 * RETURN:   status byte or negatives on error
 * NOTES:    sync
 *
 * WRITE funcs just return status value, the reset of the event is discarded. expects cmd complete event
 * READ funcs take "evt" param to save event to. expects cmd complete event
 * NOCMDPARAMS funcs do not have params. expects cmd complete event
 * W_STATUS - expects cmd status event
 */

#define HCI_SIMPLY_SYNC_CMD_W_STATUS(name, params, ogf, ocf)                                                         \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               struct hciEvtCmdStatus evtD;                                          \
                                               struct hciEvtCmdStatus *evt = &evtD;                                  \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = true;                                           \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_WRITE(name, params, ogf, ocf)                                                                \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               struct hciCmpl##name evtD;                                            \
                                               struct hciCmpl##name *evt = &evtD;                                    \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_WRITE_NOCMDPARAMS(name, ogf, ocf)                                                            \
                                           static int hci##name##Sync (void)                                         \
                                           {                                                                         \
                                               struct hci##name *cmd = NULL;                                         \
                                               const uint32_t cmdSz = 0;                                             \
                                               struct hciCmpl##name evtD;                                            \
                                               struct hciCmpl##name *evt = &evtD;                                    \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_READ(name, params, ogf, ocf)                                                                 \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name cmdD;                                                \
                                               struct hci##name *cmd = &cmdD;                                        \
                                               const uint32_t cmdSz = sizeof(*cmd);                                  \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(name, params, ogf, ocf)                                                     \
                                           static int hci##name##Sync params                                         \
                                           {                                                                         \
                                               struct hci##name *cmd = NULL;                                         \
                                               const uint32_t cmdSz = 0;                                             \
                                               const uint8_t _ogf = ogf;                                             \
                                               const uint16_t _ocf = ocf;                                            \
                                               const bool want_sta = false;                                          \
                                               uint32_t _len = 0;

#define REQUIRE_VERSION(_v)                    if (mBtVer < _v) {                                                    \
                                                   loge("attempt to call 0x%02X.%04X on v%u\n", _ogf, _ocf, mBtVer); \
                                                   return -2;                                                        \
                                               }

#define REQUIRE_FEATURE(_page, _v)             if (!(mLocalFtrs[_page] & _v)) {                                      \
                                                   loge("attempt to call 0x%02X.%04X w/o feature\n", _ogf, _ocf);    \
                                                   return -2;                                                        \
                                               }

#define HCI_COPY_IN_U64(name)                  utilSetLE64(&cmd->name, name);
#define HCI_COPY_IN_U32(name)                  utilSetLE32(&cmd->name, name);
#define HCI_COPY_IN_U24(name)                  utilSetLE24(&cmd->name, name);
#define HCI_COPY_IN_U16(name)                  utilSetLE16(&cmd->name, name);
#define HCI_COPY_IN_U8(name)                   utilSetLE8(&cmd->name, name);
#define HCI_COPY_IN_BOOL(name)                 utilSetLE8(&cmd->name, name ? 1 : 0);
#define HCI_COPY_IN_MAC(name)                  memcpy(cmd->name, name, sizeof(cmd->name));
#define HCI_COPY_IN_DATA(name,len)             if (len > sizeof(cmd->name)) {                                        \
                                                   logw("Truncating "#name" to %ub\n",(int)sizeof(cmd->name));       \
                                                   len = sizeof(cmd->name);                                          \
                                               }                                                                     \
                                               memcpy(cmd->name, name, len);

#define HCI_SIMPLY_SYNC_END                    if (!want_sta && !hciCmdSubmitSimpleWithCompleteSync(_ogf, _ocf, cmd, \
                                                      cmdSz, evt, sizeof(*evt)))                                     \
                                                   return -1;                                                        \
                                               if (want_sta && !hciCmdSubmitSimpleWithStatusSync(_ogf, _ocf, cmd,    \
                                                      cmdSz, (struct hciEvtCmdStatus*)evt))                          \
                                                   return -1;                                                        \
                                               return (unsigned)utilGetLE8(&evt->status);                            \
                                           }

/*
 * FUNCTION: hciSetEventMaskSync
 * USE:      Set event mask
 * PARAMS:   mask - the wanted mask
 */
HCI_SIMPLY_SYNC_WRITE(SetEventMask, (uint64_t mask), HCI_OGF_Controller_and_Baseband, HCI_CMD_Set_Event_Mask)
    HCI_COPY_IN_U64(mask)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWritePinTypeSync
 * USE:      Set whether we have a fixed or variable PIN
 * PARAMS:   isFixed - is the pin fixed?
 */
HCI_SIMPLY_SYNC_WRITE(WritePinType, (bool isFixed), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_PIN_Type)
    HCI_COPY_IN_BOOL(isFixed)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWritePageTimeoutSync
 * USE:      Set how long we try to connect at link layer
 * PARAMS:   timeout - the timeout in unit of 0.625ms
 */
HCI_SIMPLY_SYNC_WRITE(WritePageTimeout, (uint16_t timeout), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Page_Timeout)
    HCI_COPY_IN_U16(timeout)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteAuthEnableSync
 * USE:      Set whether we require authentication for all connections
 * PARAMS:   authRequired - set if we requite auth on all connection
 */
HCI_SIMPLY_SYNC_WRITE(WriteAuthEnable, (bool authRequired), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Authentication_Enable)
    HCI_COPY_IN_BOOL(authRequired)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteAutoFlushTimeoutSync
 * USE:      Set how long a packet is retired before it is abandonned
 * PARAMS:   conn - the ACL connection this pertains to
 */
HCI_SIMPLY_SYNC_WRITE(WriteAutoFlushTimeout, (uint16_t conn, uint16_t timeout), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Automatic_Flush_Timeout)
    HCI_COPY_IN_U16(conn)
    HCI_COPY_IN_U16(timeout)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteSyncFlowCtrlEnableSync
 * USE:      Set whether we get command complete packets for SCO data
 * PARAMS:   syncFlowCtrlOn - set if we will get those packets, false else
 */
HCI_SIMPLY_SYNC_WRITE(WriteSyncFlowCtrlEnable, (bool syncFlowCtrlOn), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_SCO_Flow_Control_Enable)
    HCI_COPY_IN_BOOL(syncFlowCtrlOn)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteConnAcceptTimeoutSync
 * USE:      Set how long after a connection request comes in taht we auto-reject it
 * PARAMS:   timeout - timeout in units of 0.625ms
 */
HCI_SIMPLY_SYNC_WRITE(WriteConnAcceptTimeout, (uint16_t timeout), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Connection_Accept_Timeout)
    HCI_COPY_IN_U16(timeout)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteInquiryModeSync
 * USE:      Set what kind of inquiry result we want
 * PARAMS:   inqMode - the result format (INQ_MODE_*)
 */
HCI_SIMPLY_SYNC_WRITE(WriteInquiryMode, (uint8_t inqMode), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Inquiry_Mode)
    REQUIRE_VERSION(HCI_VERSION_1_2)
    HCI_COPY_IN_U8(inqMode)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteEIRSync
 * USE:      Set our EIR data and EIR FEC settings
 * PARAMS:   data - the EIR data
 *           dlen - length of said data
 *           fecRequired - send data with error correction?
 */
HCI_SIMPLY_SYNC_WRITE(WriteEIR, (const uint8_t *data, uint8_t dlen, bool useFec), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Extended_Inquiry_Response)
    REQUIRE_VERSION(HCI_VERSION_2_1)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_EXTENDED_INQUIRY_RESPONSE)
    HCI_COPY_IN_BOOL(useFec)
    HCI_COPY_IN_DATA(data, dlen)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteSimplePairingModeSync
 * USE:      Enable/disable SSP
 * PARAMS:   sspOn - use ssp?
 */
HCI_SIMPLY_SYNC_WRITE(WriteSimplePairingMode, (bool useSsp), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Simple_Pairing_Mode)
    REQUIRE_VERSION(HCI_VERSION_2_1)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_SSP)
    HCI_COPY_IN_BOOL(useSsp)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciWriteLeHostSupportedSync
 * USE:      Enable/disable LE
 * PARAMS:   leSupportedHost - does host support LE?
 *           simultaneousLeHost - does host support simultaneous LE/EDR to same device
 */
HCI_SIMPLY_SYNC_WRITE(WriteLeHostSupported, (uint8_t leSupportedHost, uint8_t simultaneousLeHost), HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_LE_Host_Supported)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER)

    if (simultaneousLeHost) {
        REQUIRE_FEATURE(0, HCI_LMP_FTR_SIMUL_LE_EDR_CAPABLE_CONTROLLER)
    }

    HCI_COPY_IN_BOOL(leSupportedHost)
    HCI_COPY_IN_BOOL(simultaneousLeHost)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadLocalVersionSync
 * USE:      read local version
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadLocalVersion, (struct hciCmplReadLocalVersion *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Version_Information)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadLocalSupportedFeaturesSync
 * USE:      read local supported features
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadLocalSupportedFeatures, (struct hciCmplReadLocalSupportedFeatures *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Supported_Features)
HCI_SIMPLY_SYNC_END


/*
 * FUNCTION: hciReadLocalExtendedFeaturesSync
 * USE:      read local extended features
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ(ReadLocalExtendedFeatures, (uint8_t page, struct hciCmplReadLocalExtendedFeatures *evt), HCI_OGF_Informational, HCI_CMD_Read_Local_Extended_Features)
    REQUIRE_VERSION(HCI_VERSION_2_1)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_EXTENDED_FEATURES)
    HCI_COPY_IN_U8(page)
HCI_SIMPLY_SYNC_END


/*
 * FUNCTION: hciReadBufferSizeSync
 * USE:      read local EDR buffer sizes
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadBufferSize, (struct hciCmplReadBufferSize *evt), HCI_OGF_Informational, HCI_CMD_Read_Buffer_Size)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeReadBufferSizeSync
 * USE:      read local LE buffer sizes
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(LeReadBufferSize, (struct hciCmplLeReadBufferSize *evt), HCI_OGF_LE, HCI_CMD_LE_Read_Buffer_Size)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciReadBdAddrSync
 * USE:      read local MAC Address
 * PARAMS:   evt - event to return
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(ReadBdAddr, (struct hciCmplReadBdAddr *evt), HCI_OGF_Informational, HCI_CMD_Read_BD_ADDR)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciInquiryCancelSync
 * USE:      Cancel inquiry
 * PARAMS:   NONE
 */
HCI_SIMPLY_SYNC_WRITE_NOCMDPARAMS(InquiryCancel, HCI_OGF_Link_Control, HCI_CMD_Inquiry_Cancel)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeSetScanEnableSync
 * USE:      Enable/disable LE scan
 * PARAMS:   scanOn - scan on/off
 *           filterDuplicates - this rarely works, do not use
 */
HCI_SIMPLY_SYNC_WRITE(LeSetScanEnable, (bool scanOn, bool filterDuplicates), HCI_OGF_LE, HCI_CMD_LE_Set_Scan_Enable)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_BOOL(scanOn)
    HCI_COPY_IN_BOOL(filterDuplicates)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciLeSetScanParamsSync
 * USE:      Set LE scan parameters
 * PARAMS:   scanOn - scan on/off
 *           filterDuplicates - this rarely works, do not use
 */
HCI_SIMPLY_SYNC_WRITE(LeSetScanParams, (uint8_t activeScan, uint16_t scanInterval, uint16_t scanWindow, bool useOwnRandomAddr, bool onlyWhitelist), HCI_OGF_LE, HCI_CMD_LE_Set_Scan_Parameters)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
    HCI_COPY_IN_U16(activeScan);
    HCI_COPY_IN_U16(scanInterval);
    HCI_COPY_IN_U16(scanWindow);
    HCI_COPY_IN_BOOL(useOwnRandomAddr);
    HCI_COPY_IN_BOOL(onlyWhitelist);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciRemoteNameRequestCancelSync
 * USE:      Cancel a remote name request
 * PARAMS:   mac -the mac
 */
HCI_SIMPLY_SYNC_WRITE(RemoteNameRequestCancel, (const uint8_t *mac), HCI_OGF_LE, HCI_CMD_Remote_Name_Request_Cancel)
    HCI_COPY_IN_MAC(mac);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciInquirySync
 * USE:      Enable EDR scan
 * PARAMS:   lap - the lap to use
 *           inqLen - how long for (max is 0x30 -> 61.44 sec)
 *           numResp - max responses (0 = unlimited)
 */
HCI_SIMPLY_SYNC_CMD_W_STATUS(Inquiry, (uint32_t lap, uint8_t inqLen, uint8_t numResp), HCI_OGF_Link_Control, HCI_CMD_Inquiry)
    HCI_COPY_IN_U24(lap)
    HCI_COPY_IN_U8(inqLen)
    HCI_COPY_IN_U8(numResp)
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciInquiryEvtEdrSingleItem
 * USE:      Enqueue a callback to discovery EDR cbk
 * PARAMS:   addr - the address of the remote device
 *           devCls - device class
 *           fng - opaque struct to allow a name request
 *           rssi - the RSSI
 *           eir - the provided EIR data (if any)
 *           eirLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:    a copy of all params is made
 */
static void hciInquiryEvtEdrSingleItem(const struct bt_addr *addr, uint32_t devCls, struct hciNameGetInfo* fng, int8_t rssi, const void *eir, uint8_t eirLen)
{
    struct hciCbkWorkItem *w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem) + eirLen);
    if (!w) {
        logw("Dropping EDR discovered item due to memory error\n");
        return;
    }
    w->cbkWorkItemType = CBK_WORK_EDR_DISC_DEV;
    memcpy(&w->edrDev.addr, addr, sizeof(w->edrDev.addr));
    memcpy(&w->edrDev.fng, fng, sizeof(w->edrDev.fng));
    memcpy(&w->edrDev.eir, eir, eirLen);
    w->edrDev.devCls = devCls;
    w->edrDev.rssi = rssi;
    w->edrDev.eirLen = eirLen;

    if (workQueuePut(mCallbackWork, w))
        return;

    free(w);
    logi("Dropping EDR discovered item dues to queue error\n");
}

/*
 * FUNCTION: hciInquiryEvtLeSingleItem
 * USE:      Enqueue a callback to discovery LE cbk
 * PARAMS:   addr - the address of the remote device
 *           advType - type of reply
 *           rssi - the RSSI
 *           advData - the provided advertisement data
 *           advDataLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:    a copy of all params is made
 */
static void hciInquiryEvtLeSingleItem(const struct bt_addr *addr, uint8_t advType, int8_t rssi, const void *advData, uint8_t advDataLen)
{
    struct hciCbkWorkItem *w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem) + advDataLen);
    if (!w) {
        logw("Dropping LE discovered item due to memory error\n");
        return;
    }

    w->cbkWorkItemType = CBK_WORK_LE_DISC_DEV;
    memcpy(&w->leDev.addr, addr, sizeof(w->leDev.addr));
    memcpy(w->leDev.adv, advData, advDataLen);
    w->leDev.advType = advType;
    w->leDev.advLen = advDataLen;
    w->leDev.rssi = rssi;

    if (workQueuePut(mCallbackWork, w))
        return;

    free(w);
    logi("Dropping LE discovered item dues to queue error\n");
}

/*
 * FUNCTION: hciInquiryEvtEdrName
 * USE:      Enqueue a callback to discovery EDR name cbk
 * PARAMS:   dns - the request state we care about
 *           prev - the previous request state in line
 *           nameReqState - HCI_NAME_REQ_STATUS_*
 *           lastCbk - if set, we'll free the hciDiscoverNameState struct
 *           name - name buffer pointer (may not be null-terminated)
 *           nameMaxLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:    a copy of all params is made as needed. call with mDiscoveryStateLock held
 */
static void hciInquiryEvtEdrName(struct hciDiscoverNameState *dns, struct hciDiscoverNameState *prev, uint8_t nameReqState, bool lastCbk, const char *name, uint8_t nameMaxLen)
{
    struct hciCbkWorkItem *w;
    bool success = false;
    uint8_t nameLen;

    for (nameLen = 0; nameLen < nameMaxLen && name[nameLen]; nameLen++);

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem) + nameLen + 1);
    if (!w) {
        logw("Dropping EDR name discovered item due to memory error\n");
        return;
    }

    w->cbkWorkItemType = CBK_WORK_EDR_DISC_NAME;
    memcpy(&w->edrName.name, name, nameLen);
    w->edrName.name[nameLen] = 0;
    w->edrName.nameReqState = nameReqState;

    if (!dns)
        logd("Got name reply with nobody waiting for it. Dropping\n");
    else{
        w->edrName.cbk = dns->cbk;
        w->edrName.cbkData = dns->cbkData;
        if (workQueuePut(mCallbackWork, w))
            success = true;

        /* remove the item if needed */
        if (lastCbk) {
            if (prev)
                prev->next = dns->next;
            else
                mDiscoverNames = dns->next;

            free(dns);
        }
    }

    if (!success) {
        free(w);
        logi("Dropping EDR name discovered item due to queue or other error\n");
    }
}


/*
 * FUNCTION: hciInquiryEvtEdrNameMac
 * USE:      Enqueue a callback to discovery EDR name cbk (find by MAC)
 * PARAMS:   mac - the raw mac address of the remote device
 *           nameReqState - HCI_NAME_REQ_STATUS_*
 *           lastCbk - if set, we'll free the hciDiscoverNameState struct
 *           name - name buffer pointer (may not be null-terminated)
 *           nameMaxLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:    a copy of all params is made as needed
 */
static void hciInquiryEvtEdrNameMac(const uint8_t *mac, uint8_t nameReqState, bool lastCbk, const char *name, uint8_t nameMaxLen)
{
    struct hciDiscoverNameState *dns, *prev = NULL;

    /* now find the callback & enqueue the work*/
    pthread_mutex_lock(&mDiscoveryStateLock);
    dns = mDiscoverNames;
    while (dns && memcmp(&dns->mac, mac, sizeof(dns->mac))) {
        prev = dns;
        dns = dns->next;
    }

    hciInquiryEvtEdrName(dns, prev, nameReqState, lastCbk, name, nameMaxLen);
    pthread_mutex_unlock(&mDiscoveryStateLock);
}

/*
 * FUNCTION: hciInquiryEvtEdrNameHandle
 * USE:      Enqueue a callback to discovery EDR name cbk (find by handle)
 * PARAMS:   handle - the handle fo the request
 *           nameReqState - HCI_NAME_REQ_STATUS_*
 *           lastCbk - if set, we'll free the hciDiscoverNameState struct
 *           name - name buffer pointer (may not be null-terminated)
 *           nameMaxLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:    a copy of all params is made as needed
 */
static void hciInquiryEvtEdrNameHandle(uniq_t handle, uint8_t nameReqState, bool lastCbk, const char *name, uint8_t nameMaxLen)
{
    struct hciDiscoverNameState *dns, *prev = NULL;

    /* now find the callback & enqueue the work*/
    pthread_mutex_lock(&mDiscoveryStateLock);
    dns = mDiscoverNames;
    while (dns && dns->handle != handle) {
        prev = dns;
        dns = dns->next;
    }

    hciInquiryEvtEdrName(dns, prev, nameReqState, lastCbk, name, nameMaxLen);
    pthread_mutex_unlock(&mDiscoveryStateLock);
}

/*
 * FUNCTION: hciInquiryEvtCbk
 * USE:      Called with inquiry events
 * PARAMS:   evt - the event
 *           cbkData - callback data
 *           evtWaitStateID - event wait state ID (unused)
 *           forCmdID - event commadn ID (unused)
 * RETURN:   false on error
 * NOTES:
 */
static bool hciInquiryEvtCbk(struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    uint8_t evtType;
    struct bt_addr addr;
    struct hciNameGetInfo fng;
    uint8_t i, num;

    if (!evt)
        return false;

    evtType = utilGetLE8(&evt->code);

    if(evtType == HCI_EVT_Inquiry_Result) {
        struct hciEvtInquiryResult *res = (struct hciEvtInquiryResult*)(evt + 1);
        struct hciEvtInquiryResultItem *item = res->items;

        addr.type = BT_ADDR_TYPE_EDR;
        num = utilGetLE8(&res->numResponses);

        for (i = 0; i < num; i++, item++) {

            memcpy(addr.addr, item->mac, sizeof(addr.addr));
            fng.PSRM = utilGetLE8(&item->PSRM);
            fng.PSM = mBtVer >= HCI_VERSION_1_2 ? 0 : utilGetLE8(&item->PSM);
            fng.clockOffset = utilGetLE16(&item->clockOffset) | HCI_CLOCK_OFST_VALID;
            memcpy(fng.mac, item->mac, sizeof(fng.mac));
            hciInquiryEvtEdrSingleItem(&addr, utilGetLE24(&item->deviceClass), &fng, HCI_RSSI_UNKNOWN, NULL, 0);
        }
    } else if(evtType == HCI_EVT_Inquiry_Result_With_RSSI) {
        struct hciEvtInquiryResultWithRssi *res = (struct hciEvtInquiryResultWithRssi*)(evt + 1);
        struct hciEvtInquiryResultWithRssiItem *item = res->items;

        addr.type = BT_ADDR_TYPE_EDR;
        num = utilGetLE8(&res->numResponses);

        for (i = 0; i < num; i++, item++) {

            memcpy(addr.addr, item->mac, sizeof(addr.addr));
            fng.PSRM = utilGetLE8(&item->PSRM);
            fng.PSM = 0;
            fng.clockOffset = utilGetLE16(&item->clockOffset) | HCI_CLOCK_OFST_VALID;
            memcpy(fng.mac, item->mac, sizeof(fng.mac));
            hciInquiryEvtEdrSingleItem(&addr, utilGetLE24(&item->deviceClass), &fng, utilGetLE8(&item->RSSI), NULL, 0);
        }
    } else if(evtType == HCI_EVT_Extended_Inquiry_Result) {
        struct hciEvtExtendedInquiryResult *res = (struct hciEvtExtendedInquiryResult*)(evt + 1);

        addr.type = BT_ADDR_TYPE_EDR;
        num = utilGetLE8(&res->numResponses);

        if (num != 1)
            logw("eir reply with num = %d dropped\n", num);
        else {
            memcpy(addr.addr, res->mac, sizeof(addr.addr));
            fng.PSRM = utilGetLE8(&res->PSRM);
            fng.PSM = 0;
            fng.clockOffset = utilGetLE16(&res->clockOffset) | HCI_CLOCK_OFST_VALID;
            memcpy(fng.mac, res->mac, sizeof(fng.mac));
            hciInquiryEvtEdrSingleItem(&addr, utilGetLE24(&res->deviceClass), &fng, utilGetLE8(&res->RSSI), res->EIR, sizeof(res->EIR));
        }
    } else if(evtType == HCI_EVT_LE_Meta) {
        struct hciEvtLeMeta *meta = (struct hciEvtLeMeta*)(evt + 1);
        struct hciEvtLeAdvReport *adv = (struct hciEvtLeAdvReport*)(meta + 1);
        struct hciEvtLeAdvReportItem *item = (struct hciEvtLeAdvReportItem*)(adv + 1);
        uint8_t subevent = utilGetLE8(&meta->subevent);
        uint8_t *rssiP;

        if (subevent != HCI_EVTLE_Advertising_Report) {
            loge("Unexpected LE subevent 0x%02X in discovery event handler\n", subevent);
            return false;
        }

        num = utilGetLE8(&adv->numReports);

        for (i = 0; i < num; i++) {
            uint8_t dataLen = utilGetLE8(&item->dataLen);

            addr.type = utilGetLE8(&item->randomAddr) ? BT_ADDR_TYPE_LE_RANDOM : BT_ADDR_TYPE_LE_PUBLIC;
            memcpy(addr.addr, item->mac, sizeof(addr.addr));
            rssiP = item->data + dataLen;
            hciInquiryEvtLeSingleItem(&addr, utilGetLE8(&item->advType), utilGetLE8(rssiP), item->data, dataLen);
            item = (struct hciEvtLeAdvReportItem*)(rssiP + 1);
        }
    } else if (evtType == HCI_EVT_Command_Status) {
        struct hciEvtCmdStatus *staEvt = (struct hciEvtCmdStatus*)(evt + 1);
        uint8_t status = utilGetLE8(&staEvt->status);
        struct hciDiscoverNameState *dns;
        uniq_t handle = *(uniq_t*)cbkData;

        if (utilGetLE16(&staEvt->opcode) != CMD_MAKE_OPCODE(HCI_OGF_Link_Control, HCI_CMD_Remote_Name_Request)) {
            loge("Unexpected status subevent 0x%04X in discovery event handler\n", utilGetLE16(&staEvt->opcode));
            return false;
        }

        free(cbkData);
        hciInquiryEvtEdrNameHandle(handle, status ? HCI_NAME_REQ_STATUS_ERROR : HCI_NAME_REQ_STATUS_IN_PROGRESS, !!status, NULL, 0);

    } else if (evtType == HCI_EVT_Remote_Name_Request_Complete) {
        struct hciEvtRemoteNameReqComplete *cmpl = (struct hciEvtRemoteNameReqComplete*)(evt + 1);
        uint8_t status = utilGetLE8(&cmpl->status);

        hciInquiryEvtEdrNameMac(cmpl->mac, status ? HCI_NAME_REQ_STATUS_ERROR : HCI_NAME_REQ_STATUS_COMPLETE, true, cmpl->name, sizeof(cmpl->name));

    } else if (evtType == HCI_EVT_Inquiry_Complete) {
        logd("inquiry ended uncancelled\n");
    } else {
        loge("Unexopected event 0x%02X in discovery event handler\n", evtType);
        return false;
    }

    return true;
}

/*
 * FUNCTION: hciBtStackUp
 * USE:      Brings up the chip in a sane configuration, reags versions, etc
 * PARAMS:   NONE
 * RETURN:   false on error
 * NOTES:
 */
static bool hciBtStackUp(void)
{
    int ret = 0;
    uint8_t inqMode = HCI_INQ_MODE_STD;

    /* read local BT version */
    {
        struct hciCmplReadLocalVersion evt;
        static const char *btVers[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1"};
        uint8_t hciVersion, lmpVersion;

        logi("BT UP: reading version\n");
        ret = hciReadLocalVersionSync(&evt);
        if (ret)
            goto out;

        hciVersion = utilGetLE8(&evt.hciVersion);
        lmpVersion = utilGetLE8(&evt.lmpVersion);
        logi("BT UP: bt versions reported: %u, %d\n", hciVersion, lmpVersion);

        if (hciVersion > HCI_VERSION_4_1)
            hciVersion = HCI_VERSION_4_1;

        if (lmpVersion > HCI_VERSION_4_1)
            lmpVersion = HCI_VERSION_4_1;

        mBtVer = hciVersion < lmpVersion ? hciVersion : lmpVersion;

        if (mBtVer == HCI_VERSION_2_0) { /* do 2.0 chips really exist? */
            logi("BT 2.0 chip???\n");
            mBtVer = HCI_VERSION_1_2;
        }

        logi("BT UP: effective BT version: %u (BT spec %s)\n", mBtVer, btVers[mBtVer]);

        if (mBtVer < HCI_VERSION_1_1) {
            loge("BT 1.1 minimum!\n");
            ret = -3;
            goto out;
        }
    }

    /* read supported features */
    {
        struct hciCmplReadLocalSupportedFeatures evt;

        logi("BT UP: reading features\n");
        ret = hciReadLocalSupportedFeaturesSync(&evt);
        if (ret)
            goto out;

        mLocalFtrs[0] = utilGetLE64(&evt.features);
        logi("BT UP: bt features reported: "FMT64"X\n", CNV64(mLocalFtrs[0]));

        if (mLocalFtrs[0] & HCI_LMP_FTR_BR_EDR_NOT_SUPPORTED) {
            loge("Currently we do not support controllers with no EDR support\n");
            ret = -3;
            goto out;
        }
    }

    /* read extended features if possible */
    if (mBtVer >= HCI_VERSION_2_1 && (mLocalFtrs[0] & HCI_LMP_FTR_EXTENDED_FEATURES)) {

        uint8_t page, numPages = 1;

        for (page = 0; page < numPages; page++) {
            struct hciCmplReadLocalExtendedFeatures evt;

            ret = hciReadLocalExtendedFeaturesSync(page, &evt);
            if (ret)
                goto out;

            mLocalFtrs[page] = utilGetLE64(&evt.features);
            if (!page)
                numPages = utilGetLE8(&evt.maxPage) + 1;
            logi("BT UP: bt extended features[%d/%d] reported: "FMT64"X\n", page, numPages, CNV64(mLocalFtrs[page]));
        }
    }

    /* read MAC address and show it */
    {
        struct hciCmplReadBdAddr evt;

        logi("Reading local MAC\n");
        ret = hciReadBdAddrSync(&evt);
        if (ret)
            goto out;

        logi("Local MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", evt.mac[5], evt.mac[4], evt.mac[3], evt.mac[2], evt.mac[1], evt.mac[0]);
    }

    /* read EDR buffer sizes */
    {
        struct hciCmplReadBufferSize evt;
        uint16_t numBufAcl, numBufSco;

        logi("BT UP: reading EDR buffer size\n");
        ret = hciReadBufferSizeSync(&evt);
        if (ret)
            goto out;

        mScoBufSz = utilGetLE8(&evt.scoBufferLen);
        mAclBufSzEdr = utilGetLE16(&evt.aclBufferLen);
        numBufAcl = utilGetLE16(&evt.numAclBuffers);
        numBufSco = utilGetLE16(&evt.numScoBuffers);

        logi("BT UP: EDR buffers: %ux%ub ACL + %ux%ub SCO\n", numBufAcl, mAclBufSzEdr, numBufSco, mScoBufSz);

        while (numBufAcl--)
            sem_post(&mAclPacketsEdr);

        while (numBufSco--)
            sem_post(&mScoPackets);
    }

    /* read LE buffer sizes, if LE exists */
    if (mBtVer >= HCI_VERSION_4_0 && (mLocalFtrs[0] & HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER)) {
        struct hciCmplLeReadBufferSize evt;
        uint8_t numLeBuffers;

        logi("BT UP: reading LE buffer size\n");
        ret = hciLeReadBufferSizeSync(&evt);
        if (ret)
            goto out;

        mAclBufSzLe = utilGetLE16(&evt.leBufferSize);
        numLeBuffers = utilGetLE8(&evt.leNumBuffers);
        mHaveLe = true;

        logi("BT UP: LE buffers: %ux%ub\n", numLeBuffers, mAclBufSzLe);

        if (!numLeBuffers)
            logi("BT UP: joint EDR/LE ACL buffers detected\n");
        else
            mBtJointBuffers = false;

        while (numLeBuffers--)
            sem_post(&mAclPacketsLe);
    }

    /* setup a few simple settings */
    {
        logi("Setting PIN type to variable\n");
        ret = hciWritePinTypeSync(false);
        if (ret)
            goto out;

        logi("Setting auto-accept timeout\n");
        ret = hciWriteConnAcceptTimeoutSync(HCI_CONN_RX_TIMEOUT);
        if (ret)
            goto out;
   
        logi("Requiting authentication\n");
        ret = hciWriteAuthEnableSync(true);
        if (ret)
            goto out;
    }

    /* set event mask */
    {
        uint64_t evts = 0;

        switch (mBtVer) {
        case HCI_VERSION_1_1:
            evts = HCI_EVENT_ALL_BT_1_1;
            break;
        case HCI_VERSION_1_2:
            evts = HCI_EVENT_ALL_BT_1_2;
            break;
        case HCI_VERSION_2_1:
            evts = HCI_EVENT_ALL_BT_2_1;
            break;
        case HCI_VERSION_3_0:
            evts = HCI_EVENT_ALL_BT_3_0;
            break;
        case HCI_VERSION_4_0:
            evts = HCI_EVENT_ALL_BT_4_0;
            break;
        case HCI_VERSION_4_1:
            evts = HCI_EVENT_ALL_BT_4_1;
            break;
        }
        evts &=~ HCI_EVENT_REMOTE_HOST_SUPPORTED_FEATURES; /* we do not want this one - it is spammy and useless for now */
        logi("Setting event mask to 0x"FMT64"X\n", CNV64(evts));
        ret = hciSetEventMaskSync(evts);
        if (ret)
            goto out;
    }

    /* enable SSP, if possible */
    if (mBtVer >= HCI_VERSION_2_1 && (mLocalFtrs[0] & HCI_LMP_FTR_SSP)) {
        logi("Enabling SSP\n");
        ret = hciWriteSimplePairingModeSync(true);
        if (ret)
            goto out;
        logi("SSP should be on\n");
    }

    /* enable LE, if possible */
    if (mHaveLe) {
        mSimulLe = (mLocalFtrs[0] & HCI_LMP_FTR_SIMUL_LE_EDR_CAPABLE_CONTROLLER) != 0;
        logi("Enabling LE. Simul=%s\n", mSimulLe ? "YES" : "NO");
        ret = hciWriteLeHostSupportedSync(true, mSimulLe);
        if (ret)
            goto out;
        logi("LE should be on\n");
    }

    /* set scan results to most informative format */
    if (mBtVer >= HCI_VERSION_1_2) {

        if (mLocalFtrs[0] & HCI_LMP_FTR_RSSI_WITH_INQUIRY_RESULTS)
            inqMode = HCI_INQ_MODE_RSSI;

        if (mBtVer >= HCI_VERSION_2_1 && (mLocalFtrs[0] & HCI_LMP_FTR_EXTENDED_INQUIRY_RESPONSE))
            inqMode = HCI_INQ_MODE_EIR;

        logi("Setting inquiry mode to %d\n", inqMode);
        ret = hciWriteInquiryModeSync(inqMode);
        if (ret)
            goto out;
    }

    /* set up event handler(s) for discovery results */
    {
        struct hciEvtWaitDescr ewd = {0, };
        static const uint8_t discEvtTypes[] = {HCI_EVT_Inquiry_Complete, HCI_EVT_Inquiry_Result, HCI_EVT_Inquiry_Result_With_RSSI,
                                               HCI_EVT_Extended_Inquiry_Result, HCI_EVT_Remote_Name_Request_Complete, HCI_EVT_LE_Meta, 0};
        static const uint16_t discEvtExtras[] = {0, 0, 0, 0, 0, HCI_EVTLE_Advertising_Report};
        uint8_t i;


        ewd.cbk = hciInquiryEvtCbk;
        ewd.persistent = true;

        for (i = 0; discEvtTypes[i]; i++) {
            ewd.evtType = discEvtTypes[i];
            ewd.extra = discEvtExtras[i];

            if (!hciEvtWaitEnqueue(&ewd, 0)) {
                ret = -2;
                loge("Failed to enqueue inquiry handler %d: 0x%02X.%04X\n", i, ewd.evtType, ewd.extra);
                goto out;
            }
        }
    }

out:
    if (ret) {
        loge("Status was 0x%02X\n", ret);
        return false;
    }
    return true;
}

/*
 * FUNCTION: hciDiscoverStopAndFreeNameReq
 * USE:      Stops EDR name request if possible and frees the struct
 * PARAMS:   toFree - the name discovery struct
 * RETURN:   NONE
 * NOTES:
 */
static void hciDiscoverStopAndFreeNameReq(struct hciDiscoverNameState *toFree)
{
    if (mBtVer >= HCI_VERSION_1_2)
        hciRemoteNameRequestCancelSync(toFree->mac); /* error ignored on purpose */
    free(toFree);
}

/*
 * FUNCTION: hciDiscoverEdrStart
 * USE:      Starts EDR discovery
 * PARAMS:   cbk - device discovered callback
 *           cbkData - data for said callback
 *           msec - how long to run for
 * RETURN:   discovery handle or 0 for error
 * NOTES:    Callbacks might be called even if func returns false, but
 *           will stop once said return value is actually returned.
 *           61440 msec maximum.
 */
uniq_t hciDiscoverEdrStart(hciDeviceDiscoveredEdrCbk cbk, void *cbkData, uint32_t msec)
{
    uniq_t ret = 0;
    uint32_t len;
    int sta;

    if (!cbk) {
        loge("Refusing discovery with no callback\n");
        return 0;
    }

    pthread_mutex_lock(&mDiscoveryStateLock);
    if (mDiscoverEdrHandle)
        logw("Refusing to do EDR discovery while another in progress\n");
    else {
        ret = mDiscoverEdrHandle = uniqGetNext();

        mDiscoverEdrCbk = cbk;
        mDiscoverEdrData = cbkData;
    }
    pthread_mutex_unlock(&mDiscoveryStateLock);

    if (!ret)
        return 0;

    /* round length to units - err on doing it longer */
    len = (msec + HCI_INQUIRY_LENGTH_UNIT - 1) / HCI_INQUIRY_LENGTH_UNIT;
    if (len > HCI_INQUIRY_LENGTH_MAX)
        len = HCI_INQUIRY_LENGTH_MAX;

    logd("Discovery request for %u msec will run for %u units\n", msec, len);

    sta = hciInquirySync(HCI_LAP_Unlimited_Inquiry, len, 0x00);
    if (!sta)
        return ret;

    pthread_mutex_lock(&mDiscoveryStateLock);
    mDiscoverEdrHandle = 0;
    pthread_mutex_unlock(&mDiscoveryStateLock);
    loge("Failed to start inquiry: %d\n", sta);

    return 0;
}

/*
 * FUNCTION: hciDiscoverEdrStop
 * USE:      Stops EDR discovery and any extant name-getting callbacks for it
 * PARAMS:   handle - the handle from hciDiscoverEdrStart()
 * RETURN:   false on error
 * NOTES:    Callbacks will stop once this returns
 */
bool hciDiscoverEdrStop(uniq_t handle)
{
    if (!handle) {
        loge("Cannot pass NULL handle here\n");
        return false;
    }

    pthread_mutex_lock(&mDiscoveryStateLock);
    if (mDiscoverEdrHandle != handle)
        handle = 0;
    else
        mDiscoverEdrHandle = uniqGetNext(); /* not us so nobdy can re-cancel. not zero so nobody can start another scan */
    pthread_mutex_unlock(&mDiscoveryStateLock);

    hciInquiryCancelSync();
    hciWorkFlush();

    return !!handle;
}

/*
 * FUNCTION: hciDiscoverEdrGetName
 * USE:      Get a device name during EDR discovery
 * PARAMS:   cbk - name callback
 *           cbkData - data for said callback
 *           forNameGetting - data passed to edr discovery callback
 * RETURN:   name-getting operation handle or 0 on error
 * NOTES:
 */
uniq_t hciDiscoverEdrGetName(hciDeviceDiscoveredEdrNameCbk cbk, void *cbkData, const struct hciNameGetInfo *forNameGetting)
{
    struct hciDiscoverNameState *dns = (struct hciDiscoverNameState*)malloc(sizeof(struct hciDiscoverNameState));
    struct hciRemoteNameRequest cmd;
    struct hciEvtWaitDescr ewd;
    uniq_t *handleCopy, ret;

    if (!dns)
        return 0;

    handleCopy = malloc(sizeof(uniq_t));
    if (!handleCopy) {
        free(dns);
        return 0;
    }

    dns->cbk = cbk;
    dns->cbkData = cbkData;
    dns->handle = *handleCopy = ret = uniqGetNext();
    memcpy(dns->mac, forNameGetting->mac, sizeof(dns->mac));

    ewd.extra = CMD_MAKE_OPCODE(HCI_OGF_Link_Control, HCI_CMD_Remote_Name_Request);
    ewd.evtType = HCI_EVT_Command_Status;
    ewd.cbk = hciInquiryEvtCbk;
    ewd.cbkData = handleCopy;
    ewd.persistent = false;

    memcpy(cmd.mac, forNameGetting->mac, sizeof(cmd.mac));
    utilSetLE8(&cmd.PSRM, forNameGetting->PSRM);
    utilSetLE8(&cmd.PSM, forNameGetting->PSM);
    utilSetLE16(&cmd.clockOffset, forNameGetting->clockOffset);

    pthread_mutex_lock(&mDiscoveryStateLock);
    if (hciCmdSubmit(HCI_OGF_Link_Control, HCI_CMD_Remote_Name_Request, &cmd, sizeof(cmd), &ewd, NULL)) {

        dns->next = mDiscoverNames;
        mDiscoverNames = dns;
    } else {
        free(handleCopy);
        free(dns);
        ret = 0;
    }

    pthread_mutex_unlock(&mDiscoveryStateLock);

    return ret;
}

/*
 * FUNCTION: hciDiscoverEdrGetNameCancel
 * USE:      Cancel a device name discovery during EDR discovery
 * PARAMS:   handle - the handle from hciDiscoverEdrGetName()
 * RETURN:   false on error
 * NOTES:    cCallback will certainly not fire once this returns
 */
bool hciDiscoverEdrGetNameCancel(uniq_t handle)
{
    struct hciDiscoverNameState *c = NULL, *p;

    pthread_mutex_lock(&mDiscoveryStateLock);
    c = mDiscoverNames;
    p = NULL;
    while (c && c->handle != handle) {
        p = c;
        c = c->next;
    }
    if (c) {
        if (p)
            p->next = c->next;
        else
            mDiscoverNames = c->next;
    }
    pthread_mutex_unlock(&mDiscoveryStateLock);
    if (c)
        hciDiscoverStopAndFreeNameReq(c);

    return !!c;
}

/*
 * FUNCTION: hciDiscoverLeStart
 * USE:      Starts LE discovery
 * PARAMS:   cbk - device discovered callback
 *           cbkData - data for said callback
 *           active - do active scan?
 * RETURN:   discovery handle or 0 for error
 * NOTES:    Callbacks might be called even if func returns false, but
 *           will stop once said return value is actually returned.
 *           Discovery goes on until stopped.
 */
uniq_t hciDiscoverLeStart(hciDeviceDiscoveredLeCbk cbk, void *cbkData, bool active, bool useRandomAddr)
{
    uniq_t ret = 0;
    uint32_t len;
    int sta;

    if (!cbk) {
        loge("Refusing discovery with no callback\n");
        return 0;
    }

    if (!mHaveLe) {
        loge("Failing request to LE discovery due to lack of LE\n");
        return 0;
    }

    pthread_mutex_lock(&mDiscoveryStateLock);
    if (mDiscoverLeHandle)
        logw("Refusing to do LE discovery while another in progress\n");
    else {
        ret = mDiscoverLeHandle = uniqGetNext();

        mDiscoverLeCbk = cbk;
        mDiscoverLeData = cbkData;
    }
    pthread_mutex_unlock(&mDiscoveryStateLock);

    if (!ret)
        return 0;

    sta = hciLeSetScanParamsSync(active, HCI_LE_SCAN_INTERVAL, HCI_LE_SCAN_WINDOW, useRandomAddr, false);
    if (sta) {
        loge("Failed to set LE scan params: %d\n", sta);
        goto err;
    }

    sta = hciLeSetScanEnableSync(true, false);
    if (sta) {
        loge("Failed to set LE scan on: %d\n", sta);
        goto err;
    }

    return ret;

err:
    pthread_mutex_lock(&mDiscoveryStateLock);
    mDiscoverLeHandle = 0;
    pthread_mutex_unlock(&mDiscoveryStateLock);

    return 0;
}

/*
 * FUNCTION: hciDiscoverLeStop
 * USE:      Stops LE discovery
 * PARAMS:   handle - the handle from hciDiscoverLeStart()
 * RETURN:   false on error
 * NOTES:    Callbacks will stop once this returns
 */
bool hciDiscoverLeStop(uniq_t handle)
{
    int sta;

    if (!handle) {
        loge("Cannot pass NULL handle here\n");
        return false;
    }

    pthread_mutex_lock(&mDiscoveryStateLock);
    if (mDiscoverLeHandle != handle)
        handle = 0;
    else
        mDiscoverLeHandle = 0;
    pthread_mutex_unlock(&mDiscoveryStateLock);

    if (handle){
        sta = hciLeSetScanEnableSync(false, false);
        if (sta)
            loge("Failed to set LE scan off: %d\n", sta);
    }

    hciWorkFlush();

    return !!handle;
}

