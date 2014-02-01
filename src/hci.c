#include <semaphore.h>
#include <pthread.h>
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


/* returns true if this is the event we wanted, false else. Do not block! If evt is passed as NULL, stack is shutting down */
typedef bool (*hciEvtCbk)(struct hciEvtHdr *evt, void *cbkData1, void *cbkData2, uniq_t evtWaitStateID);

struct hciEvtWaitState {
    uniq_t                  evtWaitStateID;

    struct hciEvtWaitState *next;
    struct hciEvtWaitState *prev;

    hciEvtCbk               cbk;
    void                   *cbkData1;
    void                   *cbkData2;

    uint16_t                extra; /* cmd for cmd complete/statue, subever for LE */
    uint8_t                 evtType;

    bool                    persistent; /* do not delete when it fires */
};


struct hciCmdSendWorkItem {

    /* first even handler, if any */
    uint8_t firstEvtType;
    uint16_t firstEvtExtra;
    hciEvtCbk firstEvtCbk;
    void *firstEvtCbkData1;
    void *firstEvtCbkData2;

    /* second even handler, if any */
    uint8_t secondEvtType;
    uint16_t secondEvtExtra;
    hciEvtCbk secondEvtCbk;
    void *secondEvtCbkData1;
    void *secondEvtCbkData2;

    /* command itself goes here */
};

struct hciCbkWorkItem {
    hciOpDoneCbk  cbk;
    void         *cbkData;
    bool          success;
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
 * FUNCTION: hciEvtWaitEnqueue
 * USE:      Enqueue a handler for an event we expect
 * PARAMS:   evtType - the expected event
 *           extra - more info on expected event (command
 *                   code for command complete/status, LE
 *                   subevent code for LE metaevent)
 *           cbk - the callabck to call
 *           cbkData1 - the 1st data to pass to the callback
 *           cbkData2 - the 2nd data to pass to the callback
 *           persistent - if set, handler will not be removed
 *                       when it fires and must be removed manually
 * RETURN:   uniq_t identifying this event wait object or 0 on failure
 * NOTES:
 */
static uniq_t hciEvtWaitEnqueue(uint8_t evtType, uint16_t extra, hciEvtCbk cbk, void *cbkData1, void *cbkData2, bool persistent)
{
    struct hciEvtWaitState *ws = (struct hciEvtWaitState*)malloc(sizeof(struct hciEvtWaitState));
    uniq_t ret;

    if (!ws) {
        loge("Failed to enqueue handler for event 0x%02X.%04X\n", evtType, extra);
        return 0;
    }

    ws->cbk = cbk;
    ws->cbkData1 = cbkData1;
    ws->cbkData2 = cbkData2;
    ws->evtType = evtType;
    ws->extra = extra;
    ws->persistent = persistent;
    ws->next = NULL;
    ws->evtWaitStateID = ret = uniqGetNext();

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
    struct hciEvtWaitState *ws = NULL, *cur;
    bool handled = false, addCredit = false;
    uint8_t evtType = utilGetLE8(&evt->code);
    uint8_t len = utilGetLE8(&evt->len);
    struct hciEvtLeMeta *leMeta = (struct hciEvtLeMeta*)(evt + 1);
    struct hciEvtCmdComplete *cmdComplete = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciEvtCmdStatus *cmdStatus = (struct hciEvtCmdStatus*)(evt + 1);

    /* handle the cases of credits: command complete */
    if (evtType == HCI_EVT_Command_Complete_Event && utilGetLE8(&cmdComplete->numCmdCredits))
        addCredit = true;

    /* handle the cases of credits: command status */
    if (evtType == HCI_EVT_Command_Status_Event && utilGetLE8(&cmdStatus->numCmdCredits))
        addCredit = true;

    /* handle command complete with ocf == 0 and ogf == 0 by ignoring it as per spec */
    if (evtType == HCI_EVT_Command_Complete_Event && !utilGetLE16(&cmdComplete->opcode))
        handled = true;

    if (!handled) {
        pthread_mutex_lock(&mCmdWaitLock);
        ws = mEvtWaitHead;
        while (ws && !handled) {
            cur = ws;
            ws = ws->next;

            /* type match? If no, do not go on */
            if (evtType != ws->evtType)
                continue;

            /* if LE event, check code and skip handler if wrong */
            if (evtType == HCI_EVT_LE_Meta_Event) {
                if(len < sizeof(struct hciEvtLeMeta)) {
                    logw("Got LE meta event of len %ub\n", len);
                    break;
                }
                if (utilGetLE8(&leMeta->subevent) != ws->extra)
                    continue;
            }

            /* same for command complete */
            if (evtType == HCI_EVT_Command_Complete_Event) {
                if(len < sizeof(struct hciEvtCmdComplete)) {
                    logw("Got command complete event of len %ub\n", len);
                    break;
                }
                if (utilGetLE8(&cmdComplete->opcode) != ws->extra)
                    continue;
            }

            /* same for command status */
            if (evtType == HCI_EVT_Command_Status_Event) {
                if(len < sizeof(struct hciEvtCmdStatus)) {
                    logw("Got command status event of len %ub\n", len);
                    break;
                }
                if (utilGetLE8(&cmdStatus->opcode) != ws->extra)
                    continue;
            }

            /* if we're here, this handler matches - let's see what it says */
            handled = ws->cbk(evt, ws->cbkData1, ws->cbkData2, ws->evtWaitStateID);
        }
        if (handled && ws && !ws->persistent)
            hciEvtWaitDequeueInt(ws);

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
            loge("Got ACL packet of %ub\n", len);
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
            loge("Got SCO packet of %ub\n", len);
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
            loge("Got EVT packet of %ub\n", len);
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

        w->cbk(w->cbkData, w->success);
        free(w);
    }
    logd("HCI callback worker exiting\n");

    return NULL;
}

/*
 * FUNCTION: hciScheduleOpDoneCbk
 * USE:      Enqueue a done callback to be called
 * PARAMS:   cbk - the callback to call
 *           cbkData - the data for said callback
 *           success - the result to pass to said callback
 * RETURN:   true if enqueue, false else
 * NOTES:
 */
static bool hciScheduleOpDoneCbk(hciOpDoneCbk cbk, void *cbkData, bool success)
{
    struct hciCbkWorkItem *w;

    if (!cbk) {
        logw("Not scheduling done callback with a NULL function pointer\n");
        return true;
    }

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbk = cbk;
    w->cbkData = cbkData;
    w->success = success;

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
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
    struct hciCmdHdr *cmd;
    uniq_t h1 = 0, h2 = 0;
    uint16_t len;
    int ret;

    while (1) {

        sem_wait(&mCmdSendSem);
        ret = workQueueGet(mCmdsSendWork, (void**)&w);
        if (ret)
            break;

        cmd = (struct hciCmdHdr*)(w + 1);
        len = sizeof(struct hciCmdHdr) + utilGetLE8(&cmd->paramLen);

        /*
         * This lock makes sure adding a handler for an event and sending the
         * corresponding command are handled as one atomic unit.
         */
        pthread_mutex_lock(&mCmdLock);

        if (w->firstEvtType) {
            h1 = hciEvtWaitEnqueue(w->firstEvtType, w->firstEvtExtra, w->firstEvtCbk, w->firstEvtCbkData1, w->firstEvtCbkData2, false);
            if (!h1) {
                loge("Failed to enqueue first handler\n");
                goto failed_1st;
            }
        }

        if (w->secondEvtType) {
            h2 = hciEvtWaitEnqueue(w->secondEvtType, w->secondEvtExtra, w->secondEvtCbk, w->secondEvtCbkData1, w->secondEvtCbkData2, false);
            if (!h2) {
                loge("Failed to enqueue second handler\n");
                goto failed_2nd;
            }
        }

        if (vendorTx(HCI_PKT_TYP_CMD, cmd, len))
            goto success;

        loge("Failed to TX\n");
        hciEvtWaitDequeue(h2);

failed_2nd:
        hciEvtWaitDequeue(h1);

failed_1st:
        loge("Command 0x%04X not sent\n", utilGetLE16(&cmd->opcode));

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
 * PARAMS:   NONE
 * RETURN:   true on success, false else
 * NOTES:    may take a while
 */
bool hciUp(const uint8_t *addr, hciAclRxCbk aclRxF, hciScoRxCbk scoRxF)
{
    int ret;


    if (!vendorOpen()) {
        loge("Failed to open vendor lib\n");
        goto out_lib_open;
    }

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


    //more init here

    mAclRxF = aclRxF;
    mScoRxF = scoRxF;

    return true;

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
        mEvtWaitHead->cbk(NULL, mEvtWaitHead->cbkData1, mEvtWaitHead->cbkData2, mEvtWaitHead->evtWaitStateID);
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
 * PARAMS:   cmd - the command to send
 *           firstEvtType - first event to look for
 *           firstEvtExtra - extra data for first event search
 *           firstEvtCbk - event callback for first event
 *           firstEvtCbkData1 - 1st data for first event callback
 *           firstEvtCbkData2 - 2nd data for first event callback
 *           secondEvtType - second event to look for
 *           secondEvtExtra - extra data for second event search
 *           secondEvtCbk - event callback for second event
 *           secondEvtCbkData1 - 1st data for second event callback
 *           secondEvtCbkData2 - 2nd data for second event callback
 * RETURN:   false on error, true if you should expect callback call(s)
 * NOTES:
 */
static bool hciCmdSubmit(const struct hciCmdHdr *cmd,
                         uint8_t firstEvtType, uint16_t firstEvtExtra, hciEvtCbk firstEvtCbk,
                         void *firstEvtCbkData1, void *firstEvtCbkData2,
                         uint8_t secondEvtType, uint16_t secondEvtExtra, hciEvtCbk secondEvtCbk,
                         void *secondEvtCbkData1, void *secondEvtCbkData2)
{
    uint16_t cmdSz = utilGetLE8(&cmd->paramLen) + sizeof(struct hciCmdHdr);
    struct hciCmdSendWorkItem *w;

    w = (struct hciCmdSendWorkItem*)malloc(sizeof(struct hciCmdSendWorkItem) + cmdSz);
    if (!w) {
        loge("Failed to alloc command work for cmd size %ub\n", cmdSz);
        return false;
    }
    w->firstEvtType = firstEvtType;
    w->firstEvtExtra = firstEvtExtra;
    w->firstEvtCbk = firstEvtCbk;
    w->firstEvtCbkData1 = firstEvtCbkData2;
    w->firstEvtCbkData2 = firstEvtCbkData2;
    w->secondEvtType = secondEvtType;
    w->secondEvtExtra = secondEvtExtra;
    w->secondEvtCbk = secondEvtCbk;
    w->secondEvtCbkData1 = secondEvtCbkData2;
    w->secondEvtCbkData2 = secondEvtCbkData2;
    memcpy(w + 1, cmd, cmdSz);

    if (workQueuePut(mCmdsSendWork, w))
        return true;

    free(w);
    loge("Failed to enqueue command\n");
    return false;
}

/*
 * FUNCTION: hciCmdSimpleCompleteCbk
 * USE:      Event callback for command complete event used by hciCmdSubmitWithCmdComplete()
 * PARAMS:   evt - the arrived event
 *           cbkData1 - 1st callback data (actually the original hciOpDoneCbk)
 *           cbkData2 - 2nd callback data (actually original data for hciOpDoneCbk)
 *           evtWaitStateID - the wait state ID for original evt wait request (unused here)
 * RETURN:   true if we handled the event (always yes)
 * NOTES:
 */
static bool hciCmdSimpleCompleteCbk(struct hciEvtHdr *evt, void *cbkData1, void *cbkData2, uniq_t evtWaitStateID)
{
    struct hciEvtCmdComplete *cmpl = (struct hciEvtCmdComplete*)(evt + 1);
    uint8_t *paramP = (uint8_t*)(cmpl + 1);
    hciOpDoneCbk cbkF = (hciOpDoneCbk)cbkData1;
    uint8_t status, len = utilGetLE8(&evt->len);

    if (len < sizeof(struct hciEvtCmdComplete) + 1) {
        logw("hciCmdSimpleCompleteCbk called with command with params only %ub in size!\n", len);
        return false;
    }

    status = utilGetLE8(paramP);
    if (!hciScheduleOpDoneCbk((hciOpDoneCbk)cbkData1, cbkData2, !status))
        loge("Failed to scheduel done callback for simple command complete handler\n");

    return true;
}

/*
 * FUNCTION: hciCmdSubmitWithCmdComplete
 * USE:      Enqueue a command to be sent to the chip that will reply with a
 *           simple command complete event whose first byte of params is a
 *           status can be used as the basis of the "success/fail" reply to 
 *           the caller.
 * PARAMS:   cmd - the command to send
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciCmdSubmitWithCmdComplete(const struct hciCmdHdr *cmd, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint16_t opcode = utilGetLE16(&cmd->opcode);

    return hciCmdSubmit(cmd,
        HCI_EVT_Command_Complete_Event, opcode, hciCmdSimpleCompleteCbk, (void*)cbkDone, cbkData,
        0, 0, NULL, NULL, NULL);
}

/*
 * FUNCTION: hciCmdSimpleStatusCbk
 * USE:      Event callback for command status event used by hciCmdSubmitWithCmdStatus()
 * PARAMS:   evt - the arrived event
 *           cbkData1 - 1st callback data (actually the original hciOpDoneCbk)
 *           cbkData2 - 2nd callback data (actually original data for hciOpDoneCbk)
 *           evtWaitStateID - the wait state ID for original evt wait request (unused here)
 * RETURN:   true if we handled the event (always yes)
 * NOTES:
 */
static bool hciCmdSimpleStatusCbk(struct hciEvtHdr *evt, void *cbkData1, void *cbkData2, uniq_t evtWaitStateID)
{
    struct hciEvtCmdStatus *sta = (struct hciEvtCmdStatus*)(evt + 1);
    hciOpDoneCbk cbkF = (hciOpDoneCbk)cbkData1;
    uint8_t status = utilGetLE8(&sta->status);

    if (!hciScheduleOpDoneCbk((hciOpDoneCbk)cbkData1, cbkData2, !status))
        loge("Failed to scheduel done callback for simple command status handler\n");

    return true;
}

/*
 * FUNCTION: hciCmdSubmitWithCmdStatus
 * USE:      Enqueue a command to be sent to the chip that will reply with a
 *           simple command status event and then another event
 * PARAMS:   cmd - the command to send
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 *           finalEvtType - final event to look for
 *           finalEvtExtra - extra data for final event search
 *           finalEvtCbk - event callback for final event
 *           finalEvtCbkData1 - 1st data for final event callback
 *           finalEvtCbkData2 - 2nd data for final event callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciCmdSubmitWithCmdStatus(const struct hciCmdHdr *cmd, hciOpDoneCbk cbkDone, void *cbkData,
                                      uint8_t finalEvtType, uint16_t finalEvtExtra, hciEvtCbk finalEvtCbk,
                                      void *finalEvtCbkData1, void *finalEvtCbkData2)
{
    uint16_t opcode = utilGetLE16(&cmd->opcode);

    return hciCmdSubmit(cmd,
        HCI_EVT_Command_Status_Event, opcode, hciCmdSimpleStatusCbk, (void*)cbkDone, cbkData,
        finalEvtType, finalEvtExtra, finalEvtCbk, finalEvtCbkData1, finalEvtCbkData2);
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
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciCmdWriteLocalName)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciCmdWriteLocalName *cmd = (struct hciCmdWriteLocalName*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Local_Name);
    hdr->paramLen = sizeof(struct hciCmdWriteLocalName);
    memcpy(cmd->name, name, sizeof(cmd->name));

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
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
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciCmdWriteClassOfDevice)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciCmdWriteClassOfDevice *cmd = (struct hciCmdWriteClassOfDevice*)(buf + 1);

    if (cls >> 24) 
        logw("Truncating device class to 24 bits\n");

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Class_Of_Device);
    hdr->paramLen = sizeof(struct hciCmdWriteClassOfDevice);
    utilSetLE24(cmd->cls, cls);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciReset
 * USE:      Reset the chip
 * PARAMS:   cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciReset(hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Reset);
    hdr->paramLen = 0;

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetEventMask
 * USE:      Set event mask
 * PARAMS:   mask - the wanted mask
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciSetEventMask(uint64_t mask, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciCmdSetEventMask)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciCmdSetEventMask *cmd = (struct hciCmdSetEventMask*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Set_Event_Mask);
    hdr->paramLen = sizeof(struct hciCmdSetEventMask);
    utilSetLE64(&cmd->mask, mask);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWritePinType
 * USE:      Set whether we have a fixed or variable PIN
 * PARAMS:   isFixed - is the pin fixed?
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWritePinType(bool isFixed, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWritePinType)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWritePinType *cmd = (struct hciWritePinType*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_PIN_Type);
    hdr->paramLen = sizeof(struct hciWritePinType);
    utilSetLE8(&cmd->isFixed, isFixed ? 1 : 0);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWritePageTimeout
 * USE:      Set how long we try to connect at link layer
 * PARAMS:   timeout - the timeout in unit of 0.625ms
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWritePageTimeout(uint16_t timeout, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWritePageTimeout)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWritePageTimeout *cmd = (struct hciWritePageTimeout*)(buf + 1);

    if (!timeout) {
        loge("Invalid page timeout %d. Corrected to 1\n", timeout);
        timeout = 1;
    }

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Page_Timeout);
    hdr->paramLen = sizeof(struct hciWritePageTimeout);
    utilSetLE16(&cmd->timeout, timeout);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWriteScanEnable
 * USE:      Set whether we reply to page or inqury scane (are connectable & discoverable)
 * PARAMS:   state - the state to send in the command (bitfield of HCI_SCAN_ENABLE_* )
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteScanEnable(uint8_t state, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteScanEnable)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteScanEnable *cmd = (struct hciWriteScanEnable*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Scan_Enable);
    hdr->paramLen = sizeof(struct hciWriteScanEnable);
    utilSetLE8(&cmd->state, state);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWriteAuthEnable
 * USE:      Set whether we require authentication for all connections
 * PARAMS:   authRequired - set if we requite auth on all connection
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteAuthEnable(bool authRequired, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteAuthEnable)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteAuthEnable *cmd = (struct hciWriteAuthEnable*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Authentication_Enable);
    hdr->paramLen = sizeof(struct hciWriteAuthEnable);
    utilSetLE8(&cmd->authRequired, authRequired ? 1 : 0);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWriteAutoFlushTimeout
 * USE:      Set how long a packet is retired before it is abandonned
 * PARAMS:   conn - the ACL connection this pertains to
 *           timeout - the timeout (in units of 0.625 ms, 0 => inifinite)
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteAutoFlushTimeout(uint16_t conn, uint16_t timeout, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteAutoFlushTimeout)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteAutoFlushTimeout *cmd = (struct hciWriteAutoFlushTimeout*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Automatic_Flush_Timeout);
    hdr->paramLen = sizeof(struct hciWriteAutoFlushTimeout);
    utilSetLE16(&cmd->conn, conn);
    utilSetLE16(&cmd->timeout, timeout);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}


/*
 * FUNCTION: hciWriteSyncFlowCtrlEnable
 * USE:      Set whether we get command complete packets for SCO data
 * PARAMS:   syncFlowCtrlOn - set if we will get thos epckaets, false else
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteSyncFlowCtrlEnable(bool syncFlowCtrlOn, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteSyncFlowCtrl)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteSyncFlowCtrl *cmd = (struct hciWriteSyncFlowCtrl*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_SCO_Flow_Control_Enable);
    hdr->paramLen = sizeof(struct hciWriteSyncFlowCtrl);
    utilSetLE8(&cmd->syncFlowCtrlOn, syncFlowCtrlOn ? 1 : 0);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWriteInquiryMode
 * USE:      Set what kind of inquiry result we want
 * PARAMS:   inqMode - the result format (INQ_MODE_*)
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteInquiryMode(uint8_t inqMode, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteInquryMode)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteInquryMode *cmd = (struct hciWriteInquryMode*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_InquiryMode);
    hdr->paramLen = sizeof(struct hciWriteInquryMode);
    utilSetLE8(&cmd->inqMode, inqMode);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetEirData
 * USE:      Set our EIR data and EIR FEC settings
 * PARAMS:   data - the EIR data
 *           dlen - length of said data
 *           fecRequired - send data with error correction?
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetEirData(const uint8_t *data, uint8_t dlen, bool fecRequired, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteEIR)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteEIR *cmd = (struct hciWriteEIR*)(buf + 1);

    if (dlen > sizeof(cmd->data)) {
        logw("EIR data cut off after %ub\n", (int)sizeof(cmd->data));
        dlen = sizeof(cmd->data);
    }

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Extended_Inquiry_Response);
    hdr->paramLen = sizeof(struct hciWriteEIR);
    cmd->useFec = fecRequired ? 1 : 0;
    memset(cmd->data, 0, sizeof(cmd->data));
    memcpy(cmd->data, data, dlen);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}

/*
 * FUNCTION: hciWriteSimplePairingMode
 * USE:      Enable/disable SSP
 * PARAMS:   sspOn - use ssp?
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
static bool hciWriteSimplePairingMode(bool sspOn, hciOpDoneCbk cbkDone, void *cbkData)
{
    uint8_t buf[sizeof(struct hciCmdHdr) + sizeof(struct hciWriteSimplePairingMore)];
    struct hciCmdHdr *hdr = (struct hciCmdHdr*)buf;
    struct hciWriteSimplePairingMore *cmd = (struct hciWriteSimplePairingMore*)(buf + 1);

    hdr->opcode = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_InquiryMode);
    hdr->paramLen = sizeof(struct hciWriteSimplePairingMore);
    utilSetLE8(&cmd->useSsp, sspOn ? 1 : 0);

    return hciCmdSubmitWithCmdComplete(hdr, cbkDone, cbkData);
}
