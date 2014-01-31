#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "vendorLib.h"
#include "hci_int.h"
#include "config.h"
#include "l2cap.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"


struct hciAclHdr {
    uint16_t hdr;
    uint16_t len;
} __packed;
#define ACL_HDR_MASK_CONN_ID     0x0FFF
#define ACL_HDR_MASK_PB          0x3000
#define ACL_HDR_MASK_BC          0xC000
#define ACL_HDR_PB_FIRST_NONAUTO 0x0000
#define ACL_HDR_PB_CONINUED      0x1000
#define ACL_HDR_PB_FIRST_AUTO    0x2000
#define ACL_HDR_PB_COMPLETE      0x3000

struct hciScoHdr {
    uint16_t hdr;
    uint8_t len;
} __packed;
#define SCO_HDR_MASK_CONN_ID     0x0FFF
#define SCO_HDR_MASK_STATUS      0x3000
#define SCO_STATUS_ALL_OK        0x0000
#define SCO_STATUS_UNKNOWN       0x1000
#define SCO_STATUS_NO_DATA       0x2000
#define SCO_STATUS_SOME_DATA     0x3000

struct hciEvtHdr {
    uint8_t code;
    uint8_t len;
} __packed;


/* returns true if this is the event we wanted, false else. Do not block! If evt is passed as NULL, stack is shutting down */
typedef bool (*hciEvtCbk)(struct hciEvtHdr *evt, void *userData, uniq_t evtWaitStateID);

struct hciEvtWaitState {
    uniq_t                  evtWaitStateID;

    struct hciEvtWaitState *next;
    struct hciEvtWaitState *prev;

    hciEvtCbk               cbk;
    void                   *cbkData;

    uint16_t                extra; /* cmd for cmd complete/statue, subever for LE */
    uint8_t                 evtType;
};
        

/* our state */
static hciAclRxCbk mAclRxF;
static hciScoRxCbk mScoRxF;

static pthread_mutex_t mEvtWaitLock = PTHREAD_MUTEX_INITIALIZER;
struct hciEvtWaitState *mEvtWaitHead = NULL;
struct hciEvtWaitState *mEvtWaitTail = NULL;


/*
 * FUNCTION: hciEvtWaitDequeueInt
 * USE:      Dequeue a handler for an event we expect
 * PARAMS:   ws - the handler struct
 * RETURN:   NONE
 * NOTES:    call with mEvtWaitLock held
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

    pthread_mutex_lock(&mEvtWaitLock);

    ws = mEvtWaitHead;
    while (ws && ws->evtWaitStateID != evtWaitStateID)
        ws = ws->next;

    if (ws)
        hciEvtWaitDequeueInt(ws);
    else
        found = false;

    pthread_mutex_unlock(&mEvtWaitLock);

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
 *           cbkData - the data to pass to the callback
 * RETURN:   uniq_t identifying this event wait object or 0 on failure
 * NOTES:
 */
static uniq_t hciEvtWaitEnqueue(uint8_t evtType, uint16_t extra, hciEvtCbk cbk, void *cbkData)
{
    struct hciEvtWaitState *ws = (struct hciEvtWaitState*)malloc(sizeof(struct hciEvtWaitState));
    uniq_t ret;

    if (!ws) {
        loge("Failed to enqueue handler for event 0x%02X.%04X\n", evtType, extra);
        return 0;
    }

    ws->cbk = cbk;
    ws->cbkData = cbkData;
    ws->evtType = evtType;
    ws->extra = extra;
    ws->next = NULL;
    ws->evtWaitStateID = ret = uniqGetNext();

    pthread_mutex_lock(&mEvtWaitLock);
    ws->prev = mEvtWaitTail;
    mEvtWaitTail = ws;
    if (ws->prev)
        ws->prev->next = ws;
    else
        mEvtWaitHead = ws;
    pthread_mutex_unlock(&mEvtWaitLock);

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
    struct hciEvtWaitState *ws, *cur;
    bool handled = false;
    uint8_t evtType = utilGetLE8(&evt->code);
    uint8_t len = utilGetLE8(&evt->len);
    struct hciEvtLeMeta *leMeta = (struct hciEvtLeMeta*)(evt + 1);
    struct hciEvtCmdComplete *cmdComplete = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciEvtCmdStatus *cmdStatus = (struct hciEvtCmdStatus*)(evt + 1);

    pthread_mutex_lock(&mEvtWaitLock);
    ws = mEvtWaitHead;
    while (ws) {
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
        handled = ws->cbk(evt, ws->cbkData, ws->evtWaitStateID);
        if (handled) {
            hciEvtWaitDequeueInt(ws);
            break;
        }
    }
    pthread_mutex_unlock(&mEvtWaitLock);

    if (!handled)
        logw("Unahdneld HCI event code 0x%02X with %ub of params\n", evtType, len);
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
 * FUNCTION: hciUp
 * USE:      Bring up HCI (comms to bt chip)
 * PARAMS:   NONE
 * RETURN:   true on success, false else
 * NOTES:    may take a while
 */
bool hciUp(const uint8_t *addr, hciAclRxCbk aclRxF, hciScoRxCbk scoRxF)
{
    if (!vendorOpen()) {
        loge("Failed to open vendor lib\n");
        goto out_lib_open;
    }

    if (!vendorUp(addr, hciCbkChipHwUp, hciRx, NULL)) {
        loge("Failed to bring up the chip\n");
        goto out_chip_up;
    }

    //more init here
    mAclRxF = aclRxF;
    mScoRxF = scoRxF;

    return true;

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
    pthread_mutex_lock(&mEvtWaitLock);
    while (mEvtWaitHead) {
        mEvtWaitHead->cbk(NULL, mEvtWaitHead->cbkData, mEvtWaitHead->evtWaitStateID);
        hciEvtWaitDequeueInt(mEvtWaitHead);
    }
    pthread_mutex_unlock(&mEvtWaitLock);

    //more deinit here
    vendorDown();
    vendorClose();
}


