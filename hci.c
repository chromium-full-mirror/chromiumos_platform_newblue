//todo: only one LE connection attempt can exist at a time
//our "extra" param for event filtering canhandle other events to as long as we're willing to have a giant switch-case... and check against it in all structs
//todo: always request remote versions & features before using them (eg: le encrypt support) and before telling anyone a link is up!
//todo: always set flush timout to infinite
//todo: since only one LE connection attempt can exist at a time, enqueue others...
//todo: when link up do not upcall to l2cap until we have all features and versions from the other side gotten already!
//todo: The LE_Create_Connection_Cancel command is used to cancel the LE_Create_Connection command. This command shall only be issued after the LE_Create_Connection command has been issued, a Command Status event has been received for the LE Create Connection command and before the LE Connection Complete event


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
#include "stack.h"
#include "util.h"
#include "uniq.h"
#include "hci.h"
#include "log.h"
#include "mt.h"


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

#define ACL_CONN_ID_INVALID       (ACL_HDR_MASK_CONN_ID + 2)

/* returns true if this is the event we wanted, false else. Do not block! If evt is passed as NULL, stack is shutting down. Do not try to enqueue/dequeue handlers here either */
typedef bool (*hciEvtCbk)(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID);
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
    sg cmd;

    /* even handlers */
    struct hciEvtWaitDescrWithID ewds[];
};

#define CBK_WORK_ITEM_DONE_CBK      0
#define CBK_WORK_ITEM_SIMPLE_CBK    1
#define CBK_WORK_EDR_DISC_DEV       2
#define CBK_WORK_EDR_DISC_NAME      3
#define CBK_WORK_LE_DISC_DEV        4
#define CBK_WORK_FLUSH              5
#define CBK_WORK_CONN_UP            6
#define CBK_WORK_CONN_DOWN          7
#define CBK_WORK_PARAMS_CHANGE      8
#define CBK_WORK_ENCR_CHANGE        9
#define CBK_WORK_ACL_DATA           10

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

        struct {
            hci_conn_t                    conn;
            struct bt_addr                addr;
            bool                          isMaster;
            bool                          isEncrypted;
        } connUp;

        struct {
            hci_conn_t                    conn;
        } connDown;

        struct {
            hci_conn_t                    conn;
            bool                          success;
            uint16_t                      interval;
            uint16_t                      latency;
            uint16_t                      timeout;
        } leParamChange;

        struct {
            hci_conn_t                    conn;
            bool                          nowEncrypted;
        } encrChange;

        struct {
            hci_conn_t                    conn;
            sg                            packet;
            bool                          first;
        } aclDataRx;
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
    hci_conn_t         handle;

    uint16_t           id;
    bool               isMaster;
    bool               encrypted;
    uint8_t            state; /* CONN_STATE_* */

    union {
        struct {
            uint64_t   ftrs;
            uint16_t   interval;
            uint16_t   latency;
            uint16_t   timeout;
            uint8_t    mca;
        } le;
        struct {
            uint64_t   ftrs[MAX_FTR_PAGES];
        } edr;
    };
};

#define CONN_STATE_WAIT            0
#define CONN_STATE_RUNNING         1
#define CONN_STATE_DELETING        2

struct hciDiscoverNameState {
    struct hciDiscoverNameState   *next;
    hciDeviceDiscoveredEdrNameCbk  cbk;
    void                          *cbkData;
    uint8_t                        mac[6];
    uniq_t                         handle;
};

/* our state */

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
static uint64_t mLocalLeFtrs = 0;
static uint16_t mScoBufSz = 0;
static uint16_t mAclBufSzEdr = 0;
static uint16_t mAclBufSzLe = 0;
static uint16_t mScoBufNum = 0;
static uint16_t mAclBufNumEdr = 0;
static uint16_t mAclBufNumLe = 0;
static bool mBtJointBuffers = true; /* use EDR buffers for LE */
static sem_t mAclPacketsEdr;
static sem_t mAclPacketsLe;
static sem_t mScoPackets;
static bool mHaveLe = false;
static bool mHaveEdr = true;
static bool mSimulLe = false;
static uint8_t mInquiryState = 0;


/* fwd declarations */
static bool hciBtStackUp(void);
static bool hciScheduleConnUpNotif(hci_conn_t conn, const struct bt_addr *addr, bool isMaster, bool isEncrypted);
static bool hciScheduleConnDownNotif(hci_conn_t conn);
static bool hciScheduleConnDataRx(hci_conn_t conn, sg packet, bool first);
static bool hciCmdSubmitSimpleWithStatusWithDoneCbk(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciOpDoneCbk cbk, void *cbkData);
static void hciHandleConnAclDown(uint16_t cid, uint8_t reason);
static bool hciHandleConnAclEdrUp(uint8_t status, uint16_t id, const uint8_t *mac, bool encrypted);
static bool hciHandleConnAclLeUp(uint8_t status, uint16_t id, bool isMaster, const uint8_t *mac, bool addrRandom, uint16_t interval, uint16_t latency, uint16_t timeout, uint8_t mca);
static bool hciHandleConnAclEncr(uint16_t cid, bool encrOn);
static bool hciHandleConnAclRoleChange(const uint8_t* mac, bool amSlave);
static bool hciHandleConnAclLeUpdate(uint8_t status, uint16_t cid, uint16_t interval, uint16_t latency, uint16_t timeout);
static bool hciConnUpdate(uint16_t cid, uint16_t minInt, uint16_t maxInt, uint16_t latency, uint16_t timeout);
static void hciCmdStatusCbk(void *cbkData, uint8_t status);
static bool hciConnDisconnect(uint16_t cid);
static bool hciConnLeCancel(void);



/*
 * FUNCTION: hciConnFindById
 * USE:      Find a connection struct given an ID
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
 * FUNCTION: hciConnFindByHandle
 * USE:      Find a connection struct given a handle
 * PARAMS:   addr - the address
 * RETURN:   connection struct or NULL if not found
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnFindByHandle(hci_conn_t handle)
{
    struct hciAclConn *c = mConns;

    while (c && c->handle != handle)
        c = c->next;

    return c;
}

/*
 * FUNCTION: hciConnFindByAddr
 * USE:      Find a connection struct given an address
 * PARAMS:   addr - the address
 * RETURN:   connection struct or NULL if not found
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnFindByAddr(const struct bt_addr *addr)
{
    struct hciAclConn *c = mConns;

    while (c && memcmp(&c->addr, addr, sizeof(c->addr)))
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
 *           packet - the packet we got
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxSco(uint16_t hdr, sg packet)
{
    uint16_t pb = hdr & ACL_HDR_MASK_PB;
    uint16_t bc = hdr & ACL_HDR_MASK_BC;
    uint16_t cid = hdr & ACL_HDR_MASK_CONN_ID;
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

    //TODO: do something with the sco data
    sgFree(packet);
}

/*
 * FUNCTION: hciRxAcl
 * USE:      Called with a valid ACL packet/fragment
 * PARAMS:   hdr - packet header
 *           packet - the packet we got
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxAcl(uint16_t hdr, sg packet)
{
    uint16_t pb = hdr & ACL_HDR_MASK_PB;
    uint16_t bc = hdr & ACL_HDR_MASK_BC;
    uint16_t cid = hdr & ACL_HDR_MASK_CONN_ID;
    struct hciAclConn* conn;

    if (pb == ACL_HDR_PB_FIRST_NONAUTO || pb == ACL_HDR_PB_COMPLETE) {
        logw("Invalid acl.pb flag from chp: 0x%04X. Dropped\n", pb);
        return;
    }

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindById(cid);
    if (!conn) {
        logi("Dropping packet for unknown acl link %d\n", cid);
        sgFree(packet);
    } else if (!hciScheduleConnDataRx(conn->handle, packet, pb == ACL_HDR_PB_FIRST_AUTO)) {
        logi("Failed to enqueue packet RX for link %d\n", cid);
        sgFree(packet);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: hciRxEvt
 * USE:      Called with a valid EVT packet
 * PARAMS:   packet - the packet we got
 * RETURN:   NONE
 * NOTES:
 */
static void hciRxEvt(sg packet)
{
    struct hciEvtWaitState *ws = NULL, *cur = NULL;
    bool handled = false, addCredit = false;
    const struct hciEvtCmdComplete *cmdComplete;
    const struct hciEvtCmdStatus *cmdStatus;
    const struct hciEvtLeMeta *leMeta;
    const struct hciEvtHdr *evt;
    uint8_t evtType, len;
    void* sgiter;
    uint32_t i;


    /*
     * The SG may or may not be contiguous on the inside. If it is not, we'll alloc a new buffer
     * and serialize the SG into it. If it is, we can just use the SG's internal buffer. How do
     * we check? Iterate SG of course... Why? To save a copy and a malloc() & a free()
     */
    sgiter = sgIterStart(packet);
    if (sgiter && sgIterCurLen(sgiter) == sgLength(packet))
        evt = (const struct hciEvtHdr*)sgIterCurData(sgiter);
    else {
        evt = malloc(sgLength(packet));
        if (!evt) {
            loge("Failed to alloc flat event representation\n");
            sgFree(packet);
        }
        sgSerialize(packet, 0, sgLength(packet), (struct hciEvtHdr*)evt);
        sgiter = NULL;
        sgFree(packet);
    }

    leMeta = (struct hciEvtLeMeta*)(evt + 1);
    cmdComplete = (struct hciEvtCmdComplete*)(evt + 1);
    cmdStatus = (struct hciEvtCmdStatus*)(evt + 1);

    evtType = utilGetLE8(&evt->code);
    len = utilGetLE8(&evt->len);

    /* handle the cases of credits: command complete */
    if (evtType == HCI_EVT_Command_Complete && utilGetLE8(&cmdComplete->numCmdCredits))
        addCredit = true;

    /* handle the cases of credits: command status */
    if (evtType == HCI_EVT_Command_Status && utilGetLE8(&cmdStatus->numCmdCredits))
        addCredit = true;

    /* handle data credits */
    if (evtType == HCI_EVT_Number_Of_Completed_Packets) {
        bool hadLeCreds = false, hadEdrCreds = false;
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
            else if (BT_ADDR_IS_LE(conn->addr) && !mBtJointBuffers) {
                hadLeCreds = true;
                newCreditsLe += numPackets;
            } else {
                hadEdrCreds = true;
                newCreditsEdr += numPackets;
            }
        }
        pthread_mutex_unlock(&mConnsLock);
        while (newCreditsLe--)
            sem_post(&mAclPacketsLe);
        while (newCreditsEdr--)
            sem_post(&mAclPacketsEdr);
        if (hadLeCreds)
            l2cAclCreditAvail(true);
        if (hadEdrCreds)
            l2cAclCreditAvail(false);
        handled = true;
    }

    /* handle command complete with ocf == 0 and ogf == 0 by ignoring it as per spec */
    if (evtType == HCI_EVT_Command_Complete && !utilGetLE16(&cmdComplete->opcode))
        handled = true;

    /* handle HW failure */
    if (evtType == HCI_EVT_Hardware_Error) {
        struct hciEvtHwError *hwErr = (struct hciEvtHwError*)(evt + 1);
        loge("Chip reported HW failure, code %d -> catastropic stack failure\n", utilGetLE8(&hwErr->errCode));
        abort();
    }

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

    if (sgiter)
        sgFree(packet);
    else
        free((struct hciEvtHdr*)evt);
}

/*
 * FUNCTION: hciRx
 * USE:      Called when the chip gives us data/events
 * PARAMS:   cbkData - unused
 *           typ - type of thing we're getting
 *           packet - the packet we got
 * RETURN:   true on success, false else
 * NOTES:    we must free the "data" buffer at some point in time - we own it now
 */
static void hciRx(void *cbkData, uint8_t typ, sg packet)
{
    struct hciAclHdr hdrAcl;
    struct hciScoHdr hdrSco;
    struct hciEvtHdr hdrEvt;
    uint32_t datalen = sgLength(packet);
    uint32_t len;
    uint16_t hdr;

    switch (typ){
    case HCI_PKT_TYP_ACL:
        if (datalen < sizeof(struct hciAclHdr)) {
            loge("Got ACL packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciAclHdr);
        sgSerialize(packet, 0, sizeof(struct hciAclHdr), &hdrAcl);
        hdr = utilGetLE16(&hdrAcl.hdr);
        len = utilGetLE16(&hdrAcl.len);
        if (len != datalen) {
            loge("ACL packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        sgTruncFront(packet, sizeof(struct hciAclHdr));
        hciRxAcl(hdr, packet);
        packet = NULL;
        break;
    case HCI_PKT_TYP_SCO:
        if (datalen < sizeof(struct hciScoHdr)) {
            loge("Got SCO packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciScoHdr);
        sgSerialize(packet, 0, sizeof(struct hciScoHdr), &hdrSco);
        hdr = utilGetLE16(&hdrSco.hdr);
        len = utilGetLE8(&hdrSco.len);
        if (len != datalen) {
            loge("SCO packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        sgTruncFront(packet, sizeof(struct hciScoHdr));
        hciRxSco(hdr, packet);
        packet = NULL;
        break;
    case HCI_PKT_TYP_EVT:
        if (datalen < sizeof(struct hciEvtHdr)) {
            loge("Got EVT packet of %ub\n", datalen);
            break;
        }
        datalen -= sizeof(struct hciEvtHdr);
        sgSerialize(packet, 0, sizeof(struct hciEvtHdr), &hdrEvt);
        len = utilGetLE8(&hdrEvt.len);
        if (len != datalen) {
            loge("EVT packet claims %ub but had %ub\n", len, datalen);
            break;
        }
        hciRxEvt(packet);
        packet = NULL;
        break;
    default:
        loge("Dropping unknown packet with type %d\n", typ);
        break;
    }

    if (packet)
        sgFree(packet);
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
    struct hciCbkWorkItem *w = (struct hciCbkWorkItem*)workItem;


    if (w->cbkWorkItemType == CBK_WORK_ACL_DATA)
        sgFree(w->aclDataRx.packet);

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
    struct hciCmdSendWorkItem *w = (struct hciCmdSendWorkItem*)workItem;

    sgFree(w->cmd);
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

    pthread_setname_np(pthread_self(), "bt_hci_callbacks");

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
        case CBK_WORK_CONN_UP:
            l2cAclLinkUp(w->connUp.conn, &w->connUp.addr, w->connUp.isMaster, w->connUp.isEncrypted);
            break;
        case CBK_WORK_CONN_DOWN:
            l2cAclLinkDown(w->connDown.conn);
            break;
        case CBK_WORK_PARAMS_CHANGE:
            l2cAclLinkParamsChange(w->leParamChange.conn, w->leParamChange.success, w->leParamChange.interval, w->leParamChange.latency, w->leParamChange.timeout);
            break;
        case CBK_WORK_ENCR_CHANGE:
            l2cAclLinkEncrChange(w->leParamChange.conn, w->encrChange.nowEncrypted);
            break;
        case CBK_WORK_ACL_DATA:
            l2cAclDataRx(w->aclDataRx.conn, w->aclDataRx.packet, w->aclDataRx.first);
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
 * RETURN:   true if enqueued, false else
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
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleSimpleCbk(hciSimpleCbk cbk, void *cbkData1, void *cbkData2, bool stackGoingDown, const struct hciEvtHdr *evt)
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
 * FUNCTION: hciScheduleConnUpNotif
 * USE:      Enqueue a call to the ACL link up notif
 * PARAMS:   conn - the connection
 *           addr - peer address
 *           isMaster - are we master on the link
 *           isEncrypted - is the link encrypted
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnUpNotif(hci_conn_t conn, const struct bt_addr *addr, bool isMaster, bool isEncrypted)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_CONN_UP;
    w->connUp.conn = conn;
    w->connUp.isMaster = isMaster;
    w->connUp.isEncrypted = isEncrypted;

    memcpy(&w->connUp.addr, addr, sizeof(w->connUp.addr));

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciScheduleConnDownNotif
 * USE:      Enqueue a call to the ACL link down notif
 * PARAMS:   conn - the connection
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnDownNotif(hci_conn_t conn)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_CONN_DOWN;
    w->connDown.conn = conn;

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciScheduleConnParamChange
 * USE:      Enqueue a call to the connection param change notif
 * PARAMS:   conn - the conenction
 *           success - success?
 *           interval - new interval
 *           latency - new latency
 *           timeout - new timeout
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnParamChange(hci_conn_t conn, bool success, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_PARAMS_CHANGE;
    w->leParamChange.conn = conn;
    w->leParamChange.success = success;
    w->leParamChange.interval = interval;
    w->leParamChange.latency = latency;
    w->leParamChange.timeout = timeout;

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciScheduleConnEncrChange
 * USE:      Enqueue a call to the connection encr change notif
 * PARAMS:   conn - the conenction
 *           nowEncrypted - are we now encrypted?
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnEncrChange(hci_conn_t conn, bool nowEncrypted)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_ENCR_CHANGE;
    w->encrChange.conn = conn;
    w->encrChange.nowEncrypted = nowEncrypted;

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}


/*
 * FUNCTION: hciScheduleConnDataRx
 * USE:      Enqueue a call to the connection RX notif
 * PARAMS:   conn - the conenction
 *           packet - the packet
 *           first - was packet marked as first?
 * RETURN:   true if enqueued, false else
 * NOTES:    data is copied
 */
static bool hciScheduleConnDataRx(hci_conn_t conn, sg packet, bool first)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_ACL_DATA;
    w->aclDataRx.conn = conn;
    w->aclDataRx.packet = packet;
    w->aclDataRx.first = first;

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
    int ret, i;

    pthread_setname_np(pthread_self(), "bt_hci_cmds_send");

    while (1) {

        r_sem_wait(&mCmdSendSem);
        ret = workQueueGet(mCmdsSendWork, (void**)&w);
        if (ret)
            break;

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

        if (vendorTx(HCI_PKT_TYP_CMD, w->cmd))
            goto success;

        loge("Failed to TX a cmd\n");
        sgFree(w->cmd);

failed:
        loge("Command not sent\n");
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
 * RETURN:   true on success, false else
 * NOTES:    may take a while
 */
bool hciUp(const uint8_t *addr)
{
    int ret;


    logd("Vendor open\n");
    if (!vendorOpen()) {
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
    struct hciCmdHdr cmdhdr;
    va_list vl;
    sg cmd;

    /* count up the events we want to wait for */
    va_start(vl, paramLen);
    while (va_arg(vl, const struct hciEvtWaitDescr*))
        numHandlers++;
    va_end(vl);

    cmd = sgNew();
    if (!cmd) {
        loge("Failed to alloc cmd sg\n");
        goto out_ret;
    }

    /* allocate the work item */
    w = (struct hciCmdSendWorkItem*)malloc(sizeof(struct hciCmdSendWorkItem) + sizeof(struct hciEvtWaitDescrWithID[numHandlers]));
    if (!w) {
        loge("Failed to alloc command work for cmd 0x%02X.%04X\n", ogf, ocf);
        goto out_sgfree;
    }

    /* copy wait state objects into it */
    numHandlers = 0;
    va_start(vl, paramLen);
    while ((ewd = va_arg(vl, const struct hciEvtWaitDescr*)) != NULL)
        memcpy(&w->ewds[numHandlers++].descr, ewd, sizeof(struct hciEvtWaitDescr));
    va_end(vl);
    w->numEventWaiters = numHandlers;

    /* create the command */
    utilSetLE16(&cmdhdr.opcode, CMD_MAKE_OPCODE(ogf, ocf));
    utilSetLE16(&cmdhdr.paramLen, paramLen);
    if (!sgConcatBackCopy(cmd, &cmdhdr, sizeof(cmdhdr)) || !sgConcatBackCopy(cmd, paramData, paramLen)) {
        loge("Failed to fill command sg\n");
        goto out_wfree;
    }
    w->cmd = cmd;

    /* try to enqueue it */
    if (workQueuePut(mCmdsSendWork, w))
        return true;

    /* in case of failure, free all that we allocated */
    loge("Failed to enqueue command\n");

out_wfree:
    free(w);

out_sgfree:
    sgFree(cmd);

out_ret:
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
static bool hciCmdSubmitSimpleCbk(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
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
    struct hciEvtCmdStatus *stat = (struct hciEvtCmdStatus*)(evt + 1);
    uint8_t *paramP = (uint8_t*)(cmpl + 1); //for compelte 
    hciOpDoneCbk cbkF = (hciOpDoneCbk)cbkData1;
    uint8_t status;

    if (goingDown)
        status = HCI_STAT_GOING_DOWN;
    else if (utilGetLE8(&evt->code) == HCI_EVT_Command_Complete)
        status = utilGetLE8(paramP);
    else
        status = utilGetLE8(&stat->status);

    if (!hciScheduleOpDoneCbk(cbkF, cbkData2, status))
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
 * FUNCTION: hciCmdSubmitSimpleWithStatusWithDoneCbk
 * USE:      Send a command and setup a handler for a command status for it. call DoneCbk
 * PARAMS:   ogf - command group
 *           ocf - command code
 *           paramData - command params
 *           paramLen - length of command params
 *           cbk - doneCbk
 *           cbkData - data1 to pass to doneCbk
 * RETURN:   true if we enqueued it, false else
 * NOTES:
 */
static bool hciCmdSubmitSimpleWithStatusWithDoneCbk(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciOpDoneCbk cbk, void *cbkData)
{
    return hciCmdSubmitSimple(ogf, ocf, paramData, paramLen, hciCmdSubmitWithDoneSimpleCbk, (void*)cbk, cbkData, 0, 255, true, false);
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

    if (!mHaveEdr) {
        loge("Refusing name setting with no EDR support\n");
        return false;
    }

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

    if (!mHaveEdr) {
        loge("Refusing scan state setting with no EDR support\n");
        return false;
    }

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

    if (!mHaveEdr) {
        loge("Refusing device class setting with no EDR support\n");
        return false;
    }

    if (cls >> 24)
        logw("Truncating device class to 24 bits\n");

    utilSetLE24(&cmd.cls, cls);
    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Class_Of_Device, &cmd, sizeof(cmd), cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetEirData
 * USE:      Set EIR data for EDR
 * PARAMS:   data - the data
 *           dlen - length of said data
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:    We always use FEC
 */
bool hciSetEirData(const uint8_t *data, uint8_t dlen, bool fecRequired, hciOpDoneCbk cbkDone, void *cbkData)
{
    struct hciWriteEIR cmd = {0, };
    if (!mHaveEdr) {
        loge("Refusing EIR setting with no EDR support\n");
        return false;
    }

    if (mBtVer <= HCI_VERSION_2_1 || !(mLocalFtrs[0] & HCI_LMP_FTR_EXTENDED_INQUIRY_RESPONSE)) {
        loge("Refusing EIR setting with no EIR support\n");
        return false;
    }

    if (dlen > sizeof(cmd.data)) {
        loge("Refusing EIR setting that is too long\n");
        return false;
    }

    utilSetLE8(&cmd.useFec, 1);
    memcpy(cmd.data, data, dlen);

    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Controller_and_Baseband, HCI_CMD_Write_Extended_Inquiry_Response, &cmd, sizeof(cmd), cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetAdvData
 * USE:      Set EIR data for LE advertisements
 * PARAMS:   data - the data
 *           dlen - length of said data
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetAdvData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData)
{
    struct hciLeSetAdvData cmd;

    if (!mHaveLe) {
        loge("Refusing adv data setting with no EDR support\n");
        return false;
    }

    if (dlen > sizeof(cmd.advData)) {
        loge("Refusing LE adv setting that is too long\n");
        return false;
    }

    memcpy(cmd.advData, data, dlen);
    utilSetLE8(&cmd.advDataLen, dlen);

    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_LE, HCI_CMD_LE_Set_Advertising_Data, &cmd, sizeof(cmd), cbkDone, cbkData);
}

/*
 * FUNCTION: hciSetRspData
 * USE:      Set EIR data for LE scan responses
 * PARAMS:   data - the data
 *           dlen - length of said data
 *           cbkDone - callback to call when operation completes
 *           cbkData - data to pass to the callback
 * RETURN:   false on error, true if you should expect callback call
 * NOTES:
 */
bool hciSetRspData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData)
{
    struct hciSetScanResponseData cmd;

    if (!mHaveLe) {
        loge("Refusing rsp data setting with no EDR support\n");
        return false;
    }

    if (dlen > sizeof(cmd.scanRspData)) {
        loge("Refusing LE rsp setting that is too long\n");
        return false;
    }

    memcpy(cmd.scanRspData, data, dlen);
    utilSetLE8(&cmd.scanRspDataLen, dlen);

    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_LE, HCI_CMD_LE_Set_Scan_Response_Data, &cmd, sizeof(cmd), cbkDone, cbkData);
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

#define REQUIRE_FEATURE(_page, _v)             if (!(mLocalFtrs[_page] & _v)) {                                       \
                                                   loge("attempt to call 0x%02X.%04X w/o feature\n", _ogf, _ocf);    \
                                                   return -2;                                                        \
                                               }

#define REQUIRE_LE_FEATURE(_v)                 if (!(mLocalLeFtrs & _v)) {                                           \
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
 * FUNCTION: LeReadLocalSupportedFeatures
 * USE:      Set LE scan parameters
 * PARAMS:   scanOn - scan on/off
 *           filterDuplicates - this rarely works, do not use
 */
HCI_SIMPLY_SYNC_READ_NOCMDPARAMS(LeReadLocalSupportedFeatures, (struct hciCmplLeReadLocalSupportedFeatures* evt), HCI_OGF_LE, HCI_CMD_LE_Read_Local_Supported_Features)
    REQUIRE_VERSION(HCI_VERSION_4_0)
    REQUIRE_FEATURE(0, HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER);
HCI_SIMPLY_SYNC_END

/*
 * FUNCTION: hciRemoteNameRequestCancelSync
 * USE:      Cancel a remote name request
 * PARAMS:   mac -the mac
 */
HCI_SIMPLY_SYNC_WRITE(RemoteNameRequestCancel, (const uint8_t *mac), HCI_OGF_Link_Control, HCI_CMD_Remote_Name_Request_Cancel)
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
 * FUNCTION: hciConnRequestReplyYes
 * USE:      Called externally to accept a connection request
 * PARAMS:   addr - the address of requestor
 *           becomeMaster - try to become master?
 * RETURN:   false on error
 * NOTES:
 */
bool hciConnRequestReplyYes(const struct bt_addr *addr, bool becomeMaster)
{
    struct hciAcceptConnection cmd;

    if (!BT_ADDR_IS_EDR(*addr)) {
        logw("Acoonot accept non-EDR connection\n");
        return false;
    }

    memcpy(cmd.mac, addr->addr, sizeof(cmd.mac));
    utilSetLE8(&cmd.remainSlave, becomeMaster ? 1 : 0);

    return hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Accept_Connection_Request, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL);
}

/*
 * FUNCTION: hciConnRequestReplyNo
 * USE:      Called externally to refuse a connection request
 * PARAMS:   addr - the address of requestor
 * RETURN:   false on error
 * NOTES:
 */
bool hciConnRequestReplyNo(const struct bt_addr *addr)
{
    struct hciRejectConnection cmd;

    if (!BT_ADDR_IS_EDR(*addr)) {
        logw("Acoonot accept non-EDR connection\n");
        return false;
    }

    memcpy(cmd.mac, addr->addr, sizeof(cmd.mac));
    utilSetLE8(&cmd.reason, 0x13); //TODO: better dealings with this value...

    return hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Reject_Connection_Request, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL);
}

/*
 * FUNCTION: hciSecUserCbkPinCodeReq
 * USE:      Called externally to give us the PIN to use for a connection
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           pin - the pin
 *           len - pin length 1..HCI_PIN_MAX_LEN or 0 for none
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkPinCodeReq(const struct bt_addr *addr, void *cbkData, const uint8_t *pin, uint8_t len)
{
    struct hciPinCodeRequestNegativeReply cmdN;
    struct hciPinCodeRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    if (len >= HCI_LINK_KEY_LEN)
        len = HCI_LINK_KEY_LEN;

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));
    memcpy(cmdY.pinCode, pin, len);
    utilSetLE8(&cmdY.pinCodeLen, len);

    if (len)
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_PIN_Code_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_PIN_Code_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to pin code request\n");
}

/*
 * FUNCTION: hciSecUserCbkLinkKeyReq
 * USE:      Called externally to give us the LinkKey to use for a connection
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           key - the key (HCI_LINK_KEY_LE bytes) or NULL if none
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkLinkKeyReq(const struct bt_addr *addr, void *cbkData, const uint8_t *key)
{
    struct hciLinkKeyRequestNegativeReply cmdN;
    struct hciLinkKeyRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));

    if (key){
        memcpy(&cmdY.key, key, HCI_LINK_KEY_LEN);
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Link_Key_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    } else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Link_Key_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to link key request\n");
}

/*
 * FUNCTION: hciSecUserCbkSspIoCapabilityReq
 * USE:      Called externally to give us our IO capabilities
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           cap - our capabilities (HCI_DISP_CAP_*)
 *           haveOobData - do we have OOB data
 *           authReq - what we want (HCI_AUTH_REQMENT_*) or HCI_AUTH_REQMENT_CANCEL_SSP
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkSspIoCapabilityReq(const struct bt_addr *addr, void *cbkData, uint8_t cap, bool haveOobData, uint8_t authReq)
{
    struct hciIoCapabilityRequestNegativeReply cmdN;
    struct hciIoCapabilityRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));
    utilSetLE8(&cmdN.reason, 0x13); //TODO: better handing of this value
    utilSetLE8(&cmdY.cap, cap);
    utilSetLE8(&cmdY.oobPresent, haveOobData);
    utilSetLE8(&cmdY.authReqments, authReq);

    if (authReq != HCI_AUTH_REQMENT_CANCEL_SSP)
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_IO_Capability_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_IO_Capability_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to IO cap request\n");
}

/*
 * FUNCTION: hciSecUserCbkSspUserConfirmationReq
 * USE:      Called externally to give us confrimation (or not) of number displayed
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           confirmed - confirmed?
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkSspUserConfirmationReq(const struct bt_addr *addr, void *cbkData, bool confirmed)
{
    struct hciUserConfRequestNegativeReply cmdN;
    struct hciUserConfRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));

    if (confirmed)
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_User_Confirmation_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_User_Confirmation_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to user confirmation request\n");
}

/*
 * FUNCTION: hciSecUserCbkSspUserPasskeyReq
 * USE:      Called externally to give us the number user entered
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           numeric - what user entered or HCI_SSP_NUMERIC_INVALID if user cancelled
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkSspUserPasskeyReq(const struct bt_addr *addr, void *cbkData, uint32_t numeric)
{
    struct hciUserPasskeyRequestNegativeReply cmdN;
    struct hciUserPasskeyRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));

    if (numeric != HCI_SSP_NUMERIC_INVALID) {
        utilSetLE32(&cmdY.num, numeric);
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_User_Passkey_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    } else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_User_Passkey_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to user passkey request\n");
}

/*
 * FUNCTION: hciSecUserCbkSspUserPasskeyReqProgress
 * USE:      Called externally to give us the user's progress on number entry
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           evt - the user event (HCI_SSP_ENTRY_*)
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkSspUserPasskeyReqProgress(const struct bt_addr *addr, void *cbkData, uint8_t evt)
{
    struct hciSendKeypressNotification cmd;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmd.mac, addr->addr, sizeof(cmd.mac));
    utilSetLE8(&cmd.notifType, evt);

    if (!hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Send_Keypress_Notification, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL))
        loge("Failed to send user passkey progress event\n");
}

/*
 * FUNCTION: hciSecUserCbkSspUserOobReq
 * USE:      Called externally to give us the OOD data
 * PARAMS:   addr - the address of peer
 *           cbkData - data we sent to external code
 *           C - the C value (HCI_OOB_LEN bytes) or NULL if none
 *           R - the R value (HCI_OOB_LEN bytes) or NULL if none
 * RETURN:   NONE
 * NOTES:
 */
static void hciSecUserCbkSspUserOobReq(const struct bt_addr *addr, void *cbkData, const uint8_t *C, const uint8_t *R)
{
    struct hciRemoteOobDataRequestNegativeReply cmdN;
    struct hciRemoteOobDataRequestReply cmdY;
    bool ret;

    if (!BT_ADDR_IS_EDR(*addr)) {
        loge("cannot use LinkControl commands to reply to non-EDR security-related requests\n");
        return;
    }

    memcpy(cmdY.mac, addr->addr, sizeof(cmdY.mac));
    memcpy(cmdN.mac, addr->addr, sizeof(cmdN.mac));

    if (C && R) {
        memcpy(cmdY.C, C, sizeof(cmdY.C));
        memcpy(cmdY.R, R, sizeof(cmdY.R));
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Remote_OOB_Data_Request_Reply, &cmdY, sizeof(cmdY), hciCmdStatusCbk, NULL);
    } else
        ret = hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Remote_OOB_Data_Request_Negative_Reply, &cmdN, sizeof(cmdN), hciCmdStatusCbk, NULL);

    if (!ret)
        loge("Failed to reply to OOB data request\n");
}

/*
 * FUNCTION: hciSecUserEvtCbk
 * USE:      Called with user-interaction-related security events
 * PARAMS:   evt - the event
 *           cbkData - callback data
 *           evtWaitStateID - event wait state ID (unused)
 *           forCmdID - event commadn ID (unused)
 * RETURN:   false on error
 * NOTES:
 */
static bool hciSecUserEvtCbk(const struct hciEvtHdr *evtHdr, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    uint8_t evtType;
    struct bt_addr addr = {.type = BT_ADDR_TYPE_EDR};

    if (!evtHdr)
        return false;

    evtType = utilGetLE8(&evtHdr->code);
    if (evtType == HCI_EVT_PIN_Code_Request) {
        struct hciEvtPinCodeReq *evt = (struct hciEvtPinCodeReq*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackPinCodeRequest(&addr, hciSecUserCbkPinCodeReq, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkPinCodeReq(&addr, NULL, NULL, 0);
        }
    } else if (evtType == HCI_EVT_Link_Key_Request) {
        struct hciEvtPinCodeReq *evt = (struct hciEvtPinCodeReq*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackLinkKeyRequest(&addr, hciSecUserCbkLinkKeyReq, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkLinkKeyReq(&addr, NULL, NULL);
        }
    } else if (evtType == HCI_EVT_Link_Key_Notification) {
        struct hciEvtLinkKeyNotif *evt = (struct hciEvtLinkKeyNotif*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackLinkKeyNotif(&addr, evt->key, utilGetLE8(&evt->keyType)))
            logw("callback for EDR security event 0x%02X failed\n", evtType);

    } else if (evtType == HCI_EVT_IO_Capability_Request) {
        struct hciEvtIoCapRequest *evt = (struct hciEvtIoCapRequest*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspIoCapabilityReq(&addr, hciSecUserCbkSspIoCapabilityReq, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkSspIoCapabilityReq(&addr, NULL, 0, false, HCI_AUTH_REQMENT_CANCEL_SSP);
        }
    } else if (evtType == HCI_EVT_IO_Capability_Response) {
        struct hciEvtIoCapResponse *evt = (struct hciEvtIoCapResponse*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspIoCapabilityNotif(&addr, utilGetLE8(&evt->ioCapability), !!utilGetLE8(&evt->oobDataPresent), utilGetLE8(&evt->authReqments)))
            logw("callback for EDR security event 0x%02X failed\n", evtType);

    } else if (evtType == HCI_EVT_User_Confirmation_Request) {
        struct hciEvtUserConfRequest *evt = (struct hciEvtUserConfRequest*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspUserConfirmationReq(&addr, utilGetLE32(&evt->numericValue), hciSecUserCbkSspUserConfirmationReq, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkSspUserConfirmationReq(&addr, NULL, false);
        }
    } else if (evtType == HCI_EVT_User_Passkey_Request) {
        struct hciEvtUserPasskeyRequest *evt = (struct hciEvtUserPasskeyRequest*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspUserPasskeyReq(&addr, hciSecUserCbkSspUserPasskeyReq, NULL, hciSecUserCbkSspUserPasskeyReqProgress, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkSspUserPasskeyReq(&addr, NULL, HCI_SSP_NUMERIC_INVALID);
        }
    } else if (evtType == HCI_EVT_Remote_OOB_Data_Request) {
        struct hciEvtRemoteOobRequest *evt = (struct hciEvtRemoteOobRequest*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspUserOobReq(&addr, hciSecUserCbkSspUserOobReq, NULL)) {
            logw("callback for EDR security event 0x%02X failed -> sending negative reply\n", evtType);
            hciSecUserCbkSspUserOobReq(&addr, NULL, NULL, NULL);
        }
    } else if (evtType == HCI_EVT_Simple_Pairing_Complete) {
        struct hciEvtSimplePairingComplete *evt = (struct hciEvtSimplePairingComplete*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspComplete(&addr, !utilGetLE8(&evt->status)))
            logw("callback for EDR security event 0x%02X failed\n", evtType);

    } else if (evtType == HCI_EVT_User_Passkey_Notification) {
        struct hciEvtUserPasskeyNotif *evt = (struct hciEvtUserPasskeyNotif*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspUserPasskeyNotif(&addr, utilGetLE32(&evt->passkey)))
            logw("callback for EDR security event 0x%02X failed\n", evtType);

    } else if (evtType == HCI_EVT_Keypress_Notification) {
        struct hciEvtKeypressNotification *evt = (struct hciEvtKeypressNotification*)(evtHdr + 1);

        memcpy(addr.addr, evt->mac, sizeof(addr.addr));
        if (!stackSspRemoteEntryNotif(&addr, utilGetLE8(&evt->notifType)))
            logw("callback for EDR security event 0x%02X failed\n", evtType);

    } else
        return false;

    return true;
}

/*
 * FUNCTION: hciConnEvtCbk
 * USE:      Called with connection state events
 * PARAMS:   evt - the event
 *           cbkData - callback data
 *           evtWaitStateID - event wait state ID (unused)
 *           forCmdID - event commadn ID (unused)
 * RETURN:   false on error
 * NOTES:
 */
static bool hciConnEvtCbk(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    uint8_t evtType;
    if (!evt)
        return false;

    evtType = utilGetLE8(&evt->code);
    if (evtType == HCI_EVT_Connection_Request) {
        struct hciEvtConnRequest *req = (struct hciEvtConnRequest*)(evt + 1);

        //TODO: ask user?
        logd("ACL EDR conn request from "MACFMT" of class "CLSFMT" -> accepting\n", MACCONV(req->mac), CLSCONV(req->deviceClass));

        //TODO: ask higher layer/user/whatever?
        {
            struct bt_addr addr;
            memcpy(addr.addr, req->mac, sizeof(addr.addr));
            addr.type = BT_ADDR_TYPE_EDR;

            if (!hciConnRequestReplyYes(&addr, true))
                loge("Failed to accept this connection\n");
        }


    } else if (evtType == HCI_EVT_Connection_Complete) {
        struct hciEvtConnComplete *conn = (struct hciEvtConnComplete*)(evt + 1);
        uint16_t cid = utilGetLE16(&conn->conn);
        bool isAcl = !!utilGetLE8(&conn->isAclLink);
        bool encr = !!utilGetLE8(&conn->encrypted);
        uint8_t status = utilGetLE8(&conn->status);

        if (!isAcl) {
            cid &= SCO_HDR_MASK_CONN_ID;

            //TODO: SCO EDR conn
        } else {
            cid &= ACL_HDR_MASK_CONN_ID;

            logd("ACL EDR conn status %d to "MACFMT" w/ handle %d encr %d\n", status, MACCONV(conn->mac), cid, encr);

            if (!hciHandleConnAclEdrUp(status, cid, conn->mac, encr))
                loge("Failed to handle EDR conn up\n");
        }
    } else if (evtType == HCI_EVT_Disconnection_Complete) {
        struct hciEvtDiscComplete *disc = (struct hciEvtDiscComplete*)(evt + 1);
        uint8_t status = utilGetLE8(&disc->status);
        uint16_t cid = utilGetLE16(&disc->conn);
        uint8_t reason = utilGetLE8(&disc->reason);

        logd("ACL conn %d down for reason %d with status %d\n", cid, reason, status);
        hciHandleConnAclDown(cid, reason);

    } else if (evtType == HCI_EVT_Encryption_Change) {
        struct hciEvtEncrChange *encr = (struct hciEvtEncrChange*)(evt + 1);
        uint8_t status = utilGetLE8(&encr->status);
        uint16_t cid = utilGetLE16(&encr->conn);
        bool encrOn = !!utilGetLE8(&encr->encrOn);

        logd("ACL conn %d encr %d with status %d\n", cid, encrOn, status);
        if (!hciHandleConnAclEncr(cid, encrOn))
            loge("Failed to handle conn encr\n");
    } else if (evtType == HCI_EVT_Role_Change) {
        struct hciEvtRoleChange *role = (struct hciEvtRoleChange*)(evt + 1);
        uint8_t status = utilGetLE8(&role->status);
        bool amSlave = !!utilGetLE8(&role->amSlave);

        logd("ACL EDR conn to "MACFMT" changed role to %c with status %d\n", MACCONV(role->mac), amSlave ? 'S' : 'M', status);
        if (!hciHandleConnAclRoleChange(role->mac, amSlave))
            loge("Failed to handle EDR conn role change\n");
    } else if (evtType == HCI_EVT_LE_Meta) {
        struct hciEvtLeMeta *meta = (struct hciEvtLeMeta*)(evt + 1);
        evtType = utilGetLE8(&meta->subevent);

        if (evtType == HCI_EVTLE_Connection_Complete) {
            struct hciEvtLeConnectionComplete *conn = (struct hciEvtLeConnectionComplete*)(meta + 1);
            uint8_t status = utilGetLE8(&conn->status);
            uint16_t cid = utilGetLE16(&conn->conn);
            bool amSlave = !!utilGetLE8(&conn->amSlave);
            bool peerAddrRandom = !!utilGetLE8(&conn->peerAddrRandom);
            uint16_t interval = utilGetLE16(&conn->connInterval);
            uint16_t latency = utilGetLE16(&conn->connLatency);
            uint16_t timeout = utilGetLE16(&conn->supervisionTimeout);
            uint8_t mca = utilGetLE8(&conn->masterClockAccuracy);

            logd("ACL LE conn %d up to %c"MACFMT" as %c: {%d,%d,%d,%d}\n", cid, peerAddrRandom ? 'R' : 'P', MACCONV(conn->peerMac), amSlave ? 'S' : 'M', interval, latency, timeout, mca);
            if (!hciHandleConnAclLeUp(status, cid, !amSlave, conn->peerMac, peerAddrRandom, interval, latency, timeout, mca))
                loge("Failed to handle conn LE conn up\n");
        } else if (evtType == HCI_EVTLE_Connection_Update_Complete) {
            struct hciEvtLeConnectionUpdateComplete *updt = (struct hciEvtLeConnectionUpdateComplete*)(meta + 1);
            uint8_t status = utilGetLE8(&updt->status);
            uint16_t cid = utilGetLE16(&updt->conn);
            uint16_t interval = utilGetLE16(&updt->connInterval);
            uint16_t latency = utilGetLE16(&updt->connLatency);
            uint16_t timeout = utilGetLE16(&updt->supervisionTimeout);

            logd("ACL LE conn %d update to {%d,%d,%d}\n", cid, interval, latency, timeout);
            if (!hciHandleConnAclLeUpdate(status, cid, interval, latency, timeout))
                loge("Failed to handle LE conn update\n");
        } else
            return false;
    } else
        return false;

    return true;
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
static bool hciInquiryEvtCbk(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
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
 * FUNCTION: hciSetManyEventPersistentHandlers
 * USE:      Set a few identical event handlers for variosu events
 * PARAMS:   evtTypes - event types list (ended with a 0)
 *           evtExtras - event extras array (or NULL if all zeros are to be assumed)
 *           cbk - the handler
 *           cbkData - data for the handler
 * RETURN:   false on error
 * NOTES:
 */
static bool hciSetManyEventPersistentHandlers(const uint8_t *evtTypes, const uint16_t *evtExtras, hciEvtCbk cbk, void *cbkData)
{

    struct hciEvtWaitDescr ewd = {0, };
    uint8_t i;

    ewd.cbk = cbk;
    ewd.cbkData = cbkData;
    ewd.persistent = true;

    for (i = 0; evtTypes[i]; i++) {
        ewd.evtType = evtTypes[i];
        ewd.extra = evtExtras ? evtExtras[i] : 0;

        if (!hciEvtWaitEnqueue(&ewd, 0)) {
            loge("Failed to enqueue handler %d: 0x%02X.%04X\n", i, ewd.evtType, ewd.extra);
            return false;
        }
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
        uint16_t i;

        logi("BT UP: reading EDR buffer size\n");
        ret = hciReadBufferSizeSync(&evt);
        if (ret)
            goto out;

        mScoBufSz = utilGetLE8(&evt.scoBufferLen);
        mAclBufSzEdr = utilGetLE16(&evt.aclBufferLen);
        mAclBufNumEdr = utilGetLE16(&evt.numAclBuffers);
        mScoBufNum = utilGetLE16(&evt.numScoBuffers);

        logi("BT UP: EDR buffers: %ux%ub ACL + %ux%ub SCO\n", mAclBufNumEdr, mAclBufSzEdr, mScoBufNum, mScoBufSz);


        for (i = 0; i < mAclBufNumEdr; i++)
            sem_post(&mAclPacketsEdr);

        for (i = 0; i < mScoBufNum; i++)
            sem_post(&mScoPackets);
    }

    /* read LE buffer sizes & features, if LE exists */
    if (mBtVer >= HCI_VERSION_4_0 && (mLocalFtrs[0] & HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER)) {
        struct hciCmplLeReadLocalSupportedFeatures evtFtrs;
        struct hciCmplLeReadBufferSize evt;
        uint16_t i;

        logi("BT UP: reading LE buffer size\n");
        ret = hciLeReadBufferSizeSync(&evt);
        if (ret)
            goto out;

        mAclBufSzLe = utilGetLE16(&evt.leBufferSize);
        mAclBufNumLe = utilGetLE8(&evt.leNumBuffers);
        mHaveLe = true;
        if (mLocalFtrs[0] & HCI_LMP_FTR_BR_EDR_NOT_SUPPORTED)
            mHaveEdr = false;

        logi("BT UP: LE buffers: %ux%ub\n", mAclBufNumLe, mAclBufSzLe);

        if (!mAclBufNumLe)
            logi("BT UP: joint EDR/LE ACL buffers detected\n");
        else
            mBtJointBuffers = false;

        for (i = 0; i < mAclBufNumLe; i++)
            sem_post(&mAclPacketsLe);

        logi("Reading local LE features\n");
        ret = hciLeReadLocalSupportedFeaturesSync(&evtFtrs);
        if (ret)
            goto out;

        mLocalLeFtrs = utilGetLE64(&evtFtrs.leFeatures);
        logi("BT UP: le features reported: "FMT64"X\n", CNV64(mLocalLeFtrs));
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

        logi("Setting connection timeout\n");
        ret = hciWritePageTimeoutSync(HCI_CONN_TX_TIMEOUT);
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
        evts &=~ HCI_EVENT_MAX_SLOTS_CHANGE; /* same here really */
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
        static const uint8_t discEvtTypes[] = {HCI_EVT_Inquiry_Complete, HCI_EVT_Inquiry_Result, HCI_EVT_Inquiry_Result_With_RSSI,
                                               HCI_EVT_Extended_Inquiry_Result, HCI_EVT_Remote_Name_Request_Complete, HCI_EVT_LE_Meta, 0};
        static const uint16_t discEvtExtras[] = {0, 0, 0, 0, 0, HCI_EVTLE_Advertising_Report};

        if (!hciSetManyEventPersistentHandlers(discEvtTypes, discEvtExtras, hciInquiryEvtCbk, NULL)) {
            ret = -2;
            loge("Failed to enqueue inquiry handlers\n");
            goto out;
        }
    }

    /* set event handlers for connection states */
    {
        static const uint8_t connEvtTypes[] = {HCI_EVT_Connection_Request, HCI_EVT_Connection_Complete, HCI_EVT_Disconnection_Complete,
                                               HCI_EVT_Encryption_Change, HCI_EVT_Role_Change, HCI_EVT_LE_Meta, HCI_EVT_LE_Meta, 0};
        static const uint16_t connEvtExtras[] = {0, 0, 0, 0, 0, HCI_EVTLE_Connection_Complete, HCI_EVTLE_Connection_Update_Complete};

        if (!hciSetManyEventPersistentHandlers(connEvtTypes, connEvtExtras, hciConnEvtCbk, NULL)) {
            ret = -2;
            loge("Failed to enqueue connection state handlers\n");
            goto out;
        }
    }

    /* set event handlers for security events with user interaction */
    {
        static const uint8_t connEvtTypes[] = {HCI_EVT_PIN_Code_Request, HCI_EVT_Link_Key_Request, HCI_EVT_Link_Key_Notification, HCI_EVT_IO_Capability_Request,
                                               HCI_EVT_IO_Capability_Response, HCI_EVT_User_Confirmation_Request, HCI_EVT_User_Passkey_Request,
                                               HCI_EVT_Remote_OOB_Data_Request, HCI_EVT_Simple_Pairing_Complete, HCI_EVT_User_Passkey_Notification,
                                               HCI_EVT_Keypress_Notification, 0};

        if (!hciSetManyEventPersistentHandlers(connEvtTypes, NULL, hciSecUserEvtCbk, NULL)) {
            ret = -2;
            loge("Failed to enqueue user-interacting-security-related event handlers\n");
            goto out;
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

    if (!mHaveEdr) {
        loge("Refusing EDR discovery with no EDR support\n");
        return 0;
    }

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

    if (!mHaveEdr) {
        loge("Refusing EDR name discovery with no EDR support\n");
        return 0;
    }

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


/*
 * FUNCTION: hciInfoSharedBuffers
 * USE:      Provide info on whether EDR & LE ACL buffers are shared in our controller
 * PARAMS:   NONE
 * RETURN:   the answer
 * NOTES:
 */
bool hciInfoSharedBuffers(void)
{
   return mBtJointBuffers;
}

/*
 * FUNCTION: hciInfoSharedBuffers
 * USE:      Provide info on SCO buffer size in our controller and SCO support
 * PARAMS:   bufSz - where to store buffer sizes (or NULL if uninterested)
 *           bufNum - where to store number of buffers (or NULL if uninterested)
 * RETURN:   true if SCO supported
 * NOTES:
 */
bool hciInfoScoBufSize(uint16_t *bufSz, uint16_t *bufNum) //false if no EDR support
{
    if (!(mLocalFtrs[0] & HCI_LMP_FTR_SCO_LINKS))
        return false;

    if (bufSz)
        *bufSz = mScoBufSz;
    if (bufNum)
        *bufNum = mScoBufNum;

    return true;
}

/*
 * FUNCTION: hciInfoSharedBuffers
 * USE:      Provide info on EDR ACL buffer size in our controller and EDR support
 * PARAMS:   bufSz - where to store buffer sizes (or NULL if uninterested)
 *           bufNum - where to store number of buffers (or NULL if uninterested)
 * RETURN:   true if EDR supported
 * NOTES:
 */
bool hciInfoAclBufSizeEdr(uint16_t *bufSz, uint16_t *bufNum)
{
    if (!mHaveEdr)
        return false;

    if (bufSz)
        *bufSz = mAclBufSzEdr;
    if (bufNum)
        *bufNum = mAclBufNumEdr;

    return true;
}

/*
 * FUNCTION: hciInfoSharedBuffers
 * USE:      Provide info on LE ACL buffer size in our controller and LE support
 * PARAMS:   bufSz - where to store buffer sizes (or NULL if uninterested)
 *           bufNum - where to store number of buffers (or NULL if uninterested)
 * RETURN:   true if LE supported
 * NOTES:
 */
bool hciInfoAclBufSizeLe(uint16_t *bufSz, uint16_t *bufNum)
{
    if (!mHaveLe)
        return false;

    if (bufSz)
        *bufSz = mAclBufSzLe;
    if (bufNum)
        *bufNum = mAclBufNumLe;

    return true;
}

/*
 * FUNCTION: hciCmdStatusCbk
 * USE:      Gives us the status of a command we started
 * PARAMS:   cbkData - non-null for LE
 *           status - status from the chip
 * RETURN:   NONE
 * NOTES:    This is the universal sink for status events we do not care much about
 */
static void hciCmdStatusCbk(void *cbkData, uint8_t status)
{
    logd("status: %d\n", status);
}

/*
 * FUNCTION: hciConnectEdr
 * USE:      Start an EDR connection to a given address
 * PARAMS:   mac - the mac
 * RETURN:   true if attempt will be made
 * NOTES:    call with mConnsLock held
 */
static bool hciConnectEdr(const uint8_t *mac)
{
    struct hciCreateConnection cmd;

    if (!mHaveEdr) {
        logw("Refusing attempt to create EDR connection with no EDR support\n");
        return false;
    }

    memcpy(cmd.mac, mac, sizeof(cmd.mac));
    utilSetLE16(&cmd.allowedPackets, HCI_PKT_TYP_DEFAULT);
    utilSetLE8(&cmd.PSRM, 0);
    utilSetLE16(&cmd.clockOffset, 0);
    utilSetLE8(&cmd.allowRoleSwitch, 0);

    if (!hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Create_Connection, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL)) {
        logi("Failed to request connect to "MACFMT"\n", MACCONV(mac));
        return false;
    }
    return true;
}

/*
 * FUNCTION: hciConnEncryptLe
 * USE:      Start an encryption attempt for an LE connection
 * PARAMS:   conn - the connection
 * RETURN:   true if attempt will be made
 * NOTES:    call with mConnsLock held
 */
static bool hciConnEncryptLe(struct hciAclConn *conn)
{
    if (!(mLocalLeFtrs & HCI_LE_FTR_ENCRYPTION)) {
        loge("Encryption not supported\n");
        return false;
    }

    if (!(conn->le.ftrs & HCI_LE_FTR_ENCRYPTION)) {
        loge("Encryption not supported remotely\n");
        return false;
    }
    
    //TODO: LE Security Manager integration

    return false;
}

/*
 * FUNCTION: hciConnEncryptEdr
 * USE:      Start an encryption attempt for an EDR connection
 * PARAMS:   conn - the connection
 * RETURN:   true if attempt will be made
 * NOTES:    call with mConnsLock held
 */
static bool hciConnEncryptEdr(struct hciAclConn *conn)
{
    struct hciSetConnectionEncryption cmd;

    if (!(mLocalFtrs[0] & HCI_LMP_FTR_ENCRYPTION)) {
        loge("Encryption not supported locally\n");
        return false;
    }

    if (!(conn->edr.ftrs[0] & HCI_LMP_FTR_ENCRYPTION)) {
        loge("Encryption not supported remotely\n");
        return false;
    }
    
    utilSetLE16(&cmd.conn, conn->id);
    utilSetLE8(&cmd.encrOn, 1);
    if (!hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Set_Connection_Encryption, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL)) {
        logi("Failed to request encr to cid %d\n", conn->id);
        return false;
    }
    return true;
}

/*
 * FUNCTION: hciConnectLe
 * USE:      Start an LE connection to a given address
 * PARAMS:   mac - the mac
 *           randomAddr - peer address is random?
 *           scanInt - connect scan interval 
 *           scanWindow - connect scan window
 *           intMin - minimum wanted interval
 *           intMax - maximum wanted interval
 *           latency - wanted latency
 *           timeout - wanted timeout
 *           useOwnRandomAddr - send peer our random addr?
 * RETURN:   true if attempt will be made
 * NOTES:    call with mConnsLock held
 */
static bool hciConnectLe(const uint8_t *mac, bool randomAddr, uint16_t scanInt, uint16_t scanWindow, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeout, bool useOwnRandomAddr)
{
    struct hciLeCreateConnection cmd;

    if (!mHaveLe) {
        logw("Refusing attempt to create LE connection with no LE support\n");
        return false;
    }

    utilSetLE16(&cmd.scanInterval, scanInt);
    utilSetLE16(&cmd.scanWindow, scanWindow);
    utilSetLE8(&cmd.connectToAnyWhitelistedDevice, 0);
    utilSetLE8(&cmd.peerRandomAddr, randomAddr ? 1 : 0);
    memcpy(cmd.peerMac, mac, sizeof(cmd.peerMac));
    utilSetLE8(&cmd.useOwnRandomAddr, useOwnRandomAddr ? 1 : 0);
    utilSetLE16(&cmd.connIntervalMin, intMin);
    utilSetLE16(&cmd.connIntervalMax, intMax);
    utilSetLE16(&cmd.connLatency, latency);
    utilSetLE16(&cmd.supervisionTimeout, timeout);
    utilSetLE16(&cmd.minConnLen, 0); /* not a useful hint, i know */
    utilSetLE16(&cmd.maxConnLen, intMin * 2);


    if (!hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_LE_Create_Connection, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL)) {
        logi("Failed to request connect to "MACFMT"\n", MACCONV(mac));
        return false;
    }
    return true;
}

/*
 * FUNCTION: hciConnAclStructDel
 * USE:      Delete a connection structure
 * PARAMS:   conn - the structure
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void hciConnAclStructDel(struct hciAclConn* conn)
{
    if (conn->next)
        conn->next->prev = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        mConns = conn->next;


    logd("deleting conn struct for "ADDRFMT" h="HANDLEFMT"\n", ADDRCONV(conn->addr), HANDLECNV(conn->handle));
    free(conn);
}

/*
 * FUNCTION: hciConnAclStructNew
 * USE:      Create a connection structure
 * PARAMS:   id - the ACL connection ID
 *           addr - the peer address
 *           state - the state to create the connection in
 *           isMaster - set us as master?
 * RETURN:   the new conn struct or NULL
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnAclStructNew(uint16_t id, const struct bt_addr *addr, uint8_t state, bool isMaster)
{
    struct hciAclConn *conn = (struct hciAclConn*)calloc(1, sizeof(struct hciAclConn));
    if (!conn)
        return NULL;

    memcpy(&conn->addr, addr, sizeof(conn->addr));
    conn->state = state;
    conn->isMaster = isMaster;
    conn->id = id;
    conn->handle = uniqGetNext();

    conn->next = mConns;
    mConns = conn;

    return conn;
}

/*
 * FUNCTION: hciConnect
 * USE:      Start a connection to a given address
 * PARAMS:   addr - the address
 * RETURN:   true if attempt will be made
 * NOTES:    For LE default params will be used
 */
bool hciConnect(const struct bt_addr* addr)
{
    bool ret = false;

    logd("Requested a conect to "ADDRFMT"\n", ADDRCONV(*addr));

    pthread_mutex_lock(&mConnsLock);
    if (hciConnFindByAddr(addr))
        logw("refusing to connect to address i already have a connection to\n");
    else {
        struct hciAclConn* conn = hciConnAclStructNew(ACL_CONN_ID_INVALID, addr, CONN_STATE_WAIT, true);
        if (!conn)
            loge("Failed to allocate conn struct\n");
        else {
            if (BT_ADDR_IS_EDR(*addr))
                ret = hciConnectEdr(addr->addr);
            else
                ret = hciConnectLe(addr->addr, addr->type == BT_ADDR_TYPE_LE_RANDOM, HCI_LE_CONN_SCAN_INTERVAL,
                                   HCI_LE_CONN_SCAN_WINDOW, HCI_LE_CONN_INT_MIN, HCI_LE_CONN_INT_MAX,
                                   HCI_LE_CONN_LATENCY, HCI_LE_CONN_TIMEOUT, HCI_LE_CONN_USE_RAND_ADDR);

            if (!ret)
                hciConnAclStructDel(conn);
       }
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: hciDisconnect
 * USE:      Kill a connection [attempt] to a given address
 * PARAMS:   addr - the address
 * RETURN:   true if attempt will be made
 * NOTES:
 */
bool hciDisconnect(const struct bt_addr* addr)
{
    struct hciAclConn* conn;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByAddr(addr);
    if (!conn)
        logw("Failed to find connection to disconnect "ADDRFMT"\n", ADDRCONV(*addr));
    else {
        bool isLE = BT_ADDR_IS_LE(conn->addr);

        switch (conn->state) {
        case CONN_STATE_WAIT:
            if (isLE) {
                if (!hciConnLeCancel())
                    loge("Failed to request LE conection cancellation for "ADDRFMT"\n", ADDRCONV(conn->addr));
            } else
                logd("cannot cancel in-progress EDR connection\n");
            conn->state = CONN_STATE_DELETING;
            break;
        case CONN_STATE_RUNNING:
            hciConnDisconnect(conn->id);
            hciConnAclStructDel(conn);
            break;
        case CONN_STATE_DELETING:
            logd("cannot cancel already-in-cancellation connection\n");
            break;
        default:
            loge("Invalid connection state %d\n", conn->state);
            break;
        }
    }
    pthread_mutex_unlock(&mConnsLock);

    return !!conn;
}

/*
 * FUNCTION: hciUpdateLeParams
 * USE:      Try to update LE params
 * PARAMS:   aclConn - the connection
 *           minInt - the minimum wanted interval
 *           maxInt - the maximum wanted interval
 *           lat - wanted latency
 *           to - wanted timeout
 * RETURN:   true if attempt will be made
 * NOTES:
 */
bool hciUpdateLeParams(hci_conn_t aclConn, uint16_t minInt, uint16_t maxInt, uint16_t lat, uint16_t to)
{
    struct hciAclConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn)
        logw("Conection for update not found\n");
    else if (conn->state != CONN_STATE_RUNNING)
        logw("Cannot update params for not-yet-established connection\n");
    else
        ret = hciConnUpdate(conn->id, minInt, maxInt, lat, to);
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: hciDemandEncr
 * USE:      Try to encrypt a connection
 * PARAMS:   aclConn - the connection
 * RETURN:   true if attempt will be made
 * NOTES:
 */
bool hciDemandEncr(hci_conn_t aclConn)
{
    struct hciAclConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn)
        logw("Conection for encrypt not found\n");
    else if (conn->state != CONN_STATE_RUNNING)
        logw("Cannot update encryption for not-yet-established connection\n");
    else if (conn->encrypted)
        logw("Connection already encrypted\n");
    else if (BT_ADDR_IS_LE(conn->addr))
        ret = hciConnEncryptLe(conn);
    else
        ret = hciConnEncryptEdr(conn);
    pthread_mutex_unlock(&mConnsLock);

    return !!conn;
}

/*
 * FUNCTION: hciTryToTx
 * USE:      Try to send some data
 * PARAMS:   aclConn - the connection
 *           data - the data
 *           first - mark as first fragment?
 * RETURN:   true if attempt will be made
 * NOTES:    all data larger then controller's buffer WILL be dropped. WILL NOT BLOCK!
 */
uint8_t hciTryToTx(hci_conn_t aclConn, sg data, uint8_t boundary)
{
    struct hciAclConn* conn;
    uint8_t ret;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn) {
        ret = HCI_TX_SEND_NO_CONN;
        logw("Cannot send on a nonexistent connection\n");
    } else if (conn->state != CONN_STATE_RUNNING) {
        ret = HCI_TX_SEND_NO_CONN;
        logw("Cannot send on a not-yet-established connection\n");
    } else {
        bool isLE = BT_ADDR_IS_LE(conn->addr);
        bool useLeBuffers = isLE && !mBtJointBuffers;
        sem_t *relevantSem = useLeBuffers ? &mAclPacketsLe : &mAclPacketsEdr;
        int rv;

        rv = r_sem_trywait(relevantSem);
        if (rv && errno == EAGAIN)
            ret = HCI_TX_SEND_NO_CREDITS;
        else if (rv)
            ret = HCI_TX_SEND_ERROR;
        else {
            struct hciAclHdr hdr;
            uint16_t pb = 0xFFFF; /* this will catch all other cases */
            uint16_t start_pb = mBtVer >= HCI_VERSION_2_1 ? ACL_HDR_PB_FIRST_NONAUTO : ACL_HDR_PB_FIRST_AUTO;
            uint16_t complete_pb = mBtVer >= HCI_VERSION_3_0 ? ACL_HDR_PB_COMPLETE : start_pb;

            switch(boundary) {
            case HCI_BOUNDARY_COMPLETE:
                pb = isLE ? start_pb : complete_pb;
                break;
            case HCI_BOUNDARY_START:
                pb = start_pb;
                break;
            case HCI_BOUNDARY_CONT:
                pb = ACL_HDR_PB_CONINUED;
                break;
            }

            utilSetLE16(&hdr.len, sgLength(data));
            utilSetLE16(&hdr.hdr, conn->id | pb);

            ret = (sgConcatFrontCopy(data, &hdr, sizeof(hdr)) && vendorTx(HCI_PKT_TYP_ACL, data)) ? HCI_TX_SEND_OK : HCI_TX_SEND_ERROR;
        }
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: hciHandleConnAclDown
 * USE:      Called when a connection has gone down
 * PARAMS:   cid - the connection ID
 *           reason - the reason
 * RETURN:   NONE
 * NOTES:
 */
static void hciHandleConnAclDown(uint16_t cid, uint8_t reason)
{
    struct hciAclConn* conn;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindById(cid);
    if (!conn)
        logd("Conn down not found %d\n", cid);
    else {
        switch(conn->state) {
        case CONN_STATE_WAIT:
            logw("Unexpected connection down in wait state!\n");
            /* fallthrough */
        case CONN_STATE_RUNNING:
            if (!hciScheduleConnDownNotif(conn->handle))
                loge("Failed to schedule conn down notif\n");
            break;
        case CONN_STATE_DELETING:
            /* nothing here really */
            break;
        }
        hciConnAclStructDel(conn);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: hciConnAclUp
 * USE:      Setup a local ACL conn struct for this conn
 * PARAMS:   established - is it established?
 *           id - the id
 *           mac - the mac
 *           addrType - the address type for bt_addr struct
 *           encrypted - true if encrypted
 *           isMaster - am i master? (not authoritative in EDR cases)
 * RETURN:   connection struct or NULL if error
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnAclUp(bool established, uint16_t id, const uint8_t *mac, uint8_t addrType, bool encrypted, bool isMaster)
{
    bool isLE = addrType != BT_ADDR_TYPE_EDR;
    struct hciAclConn *c;
    struct bt_addr addr;

    c = hciConnFindByAddr(&addr);

    if (!established) {
        if (!c)
            logw("cannection we didn't know about failed. Do we care?\n");
        else
            hciConnAclStructDel(c);
        return NULL;
    }

    if (hciConnFindById(id)) {
        loge("Refusing to create conn struct for existing connection %d\n", id);
        return NULL;
    }

    memcpy(addr.addr, mac, sizeof(addr.addr));
    addr.type = addrType;

    if (!c) {
        c = hciConnAclStructNew(id, &addr, CONN_STATE_RUNNING, isMaster);
        c->isMaster = false;
    } else if (c->state == CONN_STATE_WAIT) {
        c->encrypted = encrypted;
        if (isLE)
            c->isMaster = isMaster;
        //TODO: start next LE connection/restart adv/etc?
    } else {
        loge("Connection in state %d at up time\n", c->state);
    }

    c->encrypted = encrypted;

    /*XXX: TODO: before scheduling upcall, we need to set flush timeout and get remote features */
    if (!hciScheduleConnUpNotif(c->handle, &c->addr, isMaster, encrypted))
        loge("Failed to enqueue upcall for conn up\n");

    return c;
}

/*
 * FUNCTION: hciHandleConnAclEdrUp
 * USE:      Finish setup of a local ACL.EDR conn struct for this conn if success
 * PARAMS:   status - the connection status
 *           id - the id
 *           mac - the mac
 *           encrypted - true if encrypted
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclEdrUp(uint8_t status, uint16_t id, const uint8_t *mac, bool encrypted)
{
    struct hciAclConn *c;

    pthread_mutex_lock(&mConnsLock);
    c = hciConnAclUp(!status, id, mac, BT_ADDR_TYPE_EDR, encrypted, false);
    pthread_mutex_unlock(&mConnsLock);

    return !!c;
}

/*
 * FUNCTION: hciHandleConnAclLeUp
 * USE:      Setup a local ACL.LE conn struct for this conn
 * PARAMS:   status - the connection status
 *           id - the conection id
 *           isMaster - true if we're the master
 *           mac - the mac
 *           addrRandom - true if adddress is random
 *           interval - LE connection interval
 *           latency - LE conenction latency
 *           timeout - LE connection timeout
 *           mca - LE connection master clock accuracy
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclLeUp(uint8_t status, uint16_t id, bool isMaster, const uint8_t *mac, bool addrRandom, uint16_t interval, uint16_t latency, uint16_t timeout, uint8_t mca)
{
    struct hciAclConn *c;

    pthread_mutex_lock(&mConnsLock);
    c = hciConnAclUp(!status, id, mac, addrRandom ? BT_ADDR_TYPE_LE_RANDOM : BT_ADDR_TYPE_LE_PUBLIC, false, isMaster);
    if (c) {
        c->le.interval = interval;
        c->le.latency = latency;
        c->le.timeout = timeout;
        c->le.mca = mca;
    }
    pthread_mutex_unlock(&mConnsLock);
    return !!c;
}

/*
 * FUNCTION: hciHandleConnAclEncr
 * USE:      Notify all interested in encryption change
 * PARAMS:   cid - the connection id
 *           encrOn - is encryption on?
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclEncr(uint16_t cid, bool encrOn)
{
    struct hciAclConn *c;
    bool ret = false;

    logd("cid %d now %sencrypted\n", cid, encrOn ? "" : "not ");

    pthread_mutex_lock(&mConnsLock);
    c = hciConnFindById(cid);
    if (c) {
        logd("cid %d was %sencrypted\n", cid, c->encrypted ? "" : "not ");
        c->encrypted = encrOn;
        ret = hciScheduleConnEncrChange(c->handle, encrOn);
    }
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: hciHandleConnAclRoleChange
 * USE:      Notify all interested in role change
 * PARAMS:   mac - the peer address
 *           amSlave - am i now slave?
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclRoleChange(const uint8_t* mac, bool amSlave)
{
    struct hciAclConn *c;
    struct bt_addr addr;
    bool ret = false;

    memcpy(addr.addr, mac, sizeof(addr.addr));
    addr.type = BT_ADDR_TYPE_EDR;

    logd("conn to "MACFMT" now %s\n", MACCONV(mac), amSlave? "slave" : "master");

    pthread_mutex_lock(&mConnsLock);
    c = hciConnFindByAddr(&addr);
    if (c) {
        logd("conn to "MACFMT" was %s\n", MACCONV(mac), c->isMaster? "master" : "slave");
        c->isMaster = !amSlave;
        ret = true;
    }
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: hciHandleConnAclLeUpdate
 * USE:      Notify all interested in conn param update
 * PARAMS:   status - did update succeed?
 *           cid - the connection id
 *           interval - the new interval (valid only in case of success)
 *           latency - the new latency (valid only in case of success)
 *           timeout - the new timeout (valid only in case of success)
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclLeUpdate(uint8_t status, uint16_t cid, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    struct hciAclConn *c;
    bool ret = false;

    logd("cid %d update success: %d\n", cid, status);

    pthread_mutex_lock(&mConnsLock);
    c = hciConnFindById(cid);
    if (c) {
        if (!status) {
            logd("update: {%d,%d,%d} -> {%d,%d,%d}\n", c->le.interval, c->le.latency, c->le.timeout, interval, latency, timeout);
            c->le.interval = interval;
            c->le.latency = latency;
            c->le.timeout = timeout;
        }
        ret = hciScheduleConnParamChange(c->handle, !status, c->le.interval, c->le.latency, c->le.timeout);
    }
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: hciConnLeCancel
 * USE:      Cancel an ongoing LE connection attempt
 * PARAMS:   addr - whom the connection was to
 * RETURN:   true on success
 * NOTES:
 */
static bool hciConnLeCancel(void)
{
    return hciCmdSubmitSimpleWithCompleteWithDoneCbk(HCI_OGF_LE, HCI_CMD_LE_Create_Connection_Cancel, NULL, 0, hciCmdStatusCbk, NULL);
}


/*
 * FUNCTION: hciConnUpdate
 * USE:      Start a connection update for an LE connection
 * PARAMS:   cid - the cid
 *           intMin - minimum wanted interval
 *           intMax - maximum wanted itnerval
 *           latency - wanted latency
 *           timeout - wanted timeout
 * RETURN:   false on failure. else wait.
 * NOTES:
 */
static bool hciConnUpdate(uint16_t cid, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeout)
{
    struct hciLeConnectionUpdate cmd;

    utilSetLE16(&cmd.conn, cid);
    utilSetLE16(&cmd.connIntervalMin, intMin);
    utilSetLE16(&cmd.connIntervalMax, intMax);
    utilSetLE16(&cmd.connLatency, latency);
    utilSetLE16(&cmd.supervisionTimeout, timeout);
    utilSetLE16(&cmd.minConnLen, 0); /* not a useful hint, i know */
    utilSetLE16(&cmd.maxConnLen, intMin * 2);

    return hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_LE, HCI_CMD_LE_Connection_Update, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL);
}

/*
 * FUNCTION: hciConnDisconnect
 * USE:      Start a disconnection process
 * PARAMS:   cid - the cid
 * RETURN:   false on failure. else wait.
 * NOTES:
 */
static bool hciConnDisconnect(uint16_t cid)
{
    struct hciDisconnect cmd;

    utilSetLE16(&cmd.conn, cid);
    utilSetLE16(&cmd.reason, 0x13); //TODO: better dealings with this value...

    return hciCmdSubmitSimpleWithStatusWithDoneCbk(HCI_OGF_Link_Control, HCI_CMD_Disconnect, &cmd, sizeof(cmd), hciCmdStatusCbk, NULL);
}



