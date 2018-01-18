//todo: only one LE connection attempt can exist at a time
//our "extra" param for event filtering canhandle other events to as long as we're willing to have a giant switch-case... and check against it in all structs
//todo: always request remote versions & features before using them (eg: le encrypt support) and before telling anyone a link is up!
//todo: always set flush timout to infinite
//todo: since only one LE connection attempt can exist at a time, enqueue others...
//todo: when link up do not upcall to l2cap until we have all features and versions from the other side gotten already!
//todo: The LE_Create_Connection_Cancel command is used to cancel the LE_Create_Connection command. This command shall only be issued after the LE_Create_Connection command has been issued, a Command Status event has been received for the LE Create Connection command and before the LE Connection Complete event

#define _INCLUDED_FROM_HCI_H_
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "workQueue.h"
#include "vendorLib.h"
#include "hci_int.h"
#include "persist.h"
#include "config.h"
#include "l2cap.h"
#include "aapi.h"
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

    uint16_t                extra; /* cmd for cmd complete/statue, subevent for LE */
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
#define CBK_WORK_LE_DISC_DEV        2
#define CBK_WORK_FLUSH              3
#define CBK_WORK_CONN_UP            4
#define CBK_WORK_CONN_DOWN          5
#define CBK_WORK_PARAMS_CHANGE      6
#define CBK_WORK_ENCR_CHANGE        7
#define CBK_WORK_ACL_DATA           8

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
            struct bt_addr                peerAddr;
            struct bt_addr                selfAddr;
            bool                          isMaster;
            bool                          isEncrypted;
            bool                          isMitmSafe;
        } connUp;

        struct {
            hci_conn_t                    conn;
            uint8_t                       reason;
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
            bool                          isMitmSafe;
        } encrChange;

        struct {
            hci_conn_t                    conn;
            sg                            packet;
            bool                          first;
        } aclDataRx;
    };
};

struct hciBacklogItemHdr {
    uint32_t len;
    uint16_t aclHdr;
} __packed;

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
    struct bt_addr     peerAddr;
    struct bt_addr     selfAddr; /* XXX */
    hci_conn_t         handle;

    uint16_t           id;
    bool               isMaster;
    bool               encrypted;
    bool               mitmSafe;
    uint8_t            state; /* CONN_STATE_* */
    sg                 rxBacklog; /* data we RXed during configuration stage */
    uint32_t           outstandingPackets;

    union {
        struct {
            uint64_t   ftrs;
            uint16_t   interval;
            uint16_t   latency;
            uint16_t   timeout;
            uint8_t    mca;
        } le;
        //EDR struct here if needed
    };
};

struct hciAdvSet {
    /* bookkeeping */
    struct hciAdvSet  *next;
    struct hciAdvSet  *prev;
    hci_adv_set_t      handle;

    /* hw state */
    uint16_t           hwSetNum;
    bool               hwEnabled;
    int8_t             hwPowerLevel; /* if hw supports power setting, must be populated when enabled */

    /* user data */
    uint32_t           advDataLen;
    uint32_t           scanRspDataLen;
    uint8_t            advData[HCI_ADV_DATA_MAX_LEN];
    uint8_t            scanRspData[HCI_SCAN_RSP_DATA_MAX_LEN];
    uint16_t           intervalMin;
    uint16_t           intervalMax;
    uint8_t            advType;
    uint8_t            ownAddressType;
    struct bt_addr     ownRandomAddr; //only used if random addr selected
    struct bt_addr     directAddr;
    uint8_t            channelMap;
    uint8_t            filterPolicy;
    int8_t             powerLevel;
};

#define CONN_STATE_WAIT            0
#define CONN_STATE_CFG             1
#define CONN_STATE_RUNNING         2
#define CONN_STATE_DELETING        3

#define CONN_CFG_STEP_LE           0x0000

#define STACK_STATE_DOWN           0 //down and not ready for up
#define STACK_STATE_READY_FOR_UP   1 //ready to be brought up
#define STACK_STATE_COMING_UP      2 //on the way up
#define STACK_STATE_UP             3 //working
#define STACK_STATE_GOING_DOWN     4 //on the way down


/* useful constants */
static const char *mBtVers[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1", "4.2", "5.0"};

/* our state */

static pthread_mutex_t mCmdWaitLock = PTHREAD_MUTEX_INITIALIZER;
static struct hciEvtWaitState *mEvtWaitHead = NULL;
static struct hciEvtWaitState *mEvtWaitTail = NULL;

static pthread_mutex_t mCmdLock = PTHREAD_MUTEX_INITIALIZER;
static sem_t mCmdSendSem;
static struct workQueue* mCmdsSendWork;
static pthread_t mCmdsSendThread;
static uint8_t mStackState = STACK_STATE_DOWN;

static struct workQueue* mCallbackWork;
static pthread_t mCallbackThread;

static pthread_mutex_t mConnsLock = PTHREAD_MUTEX_INITIALIZER;
static struct hciAclConn *mConns = NULL;

static pthread_mutex_t mAdvSetsLock = PTHREAD_MUTEX_INITIALIZER;
static struct hciAdvSet *mAdvSets = NULL;

static pthread_mutex_t mDiscoveryStateLock = PTHREAD_MUTEX_INITIALIZER;

static hciDeviceDiscoveredLeCbk mDiscoverLeCbk;
static void *mDiscoverLeData;
static uniq_t mDiscoverLeHandle = 0;



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
static sem_t mAclPacketsLe;
static sem_t mAclPacketsEdr;
static uint8_t mInquiryState = 0;
static uint8_t mLocalAddr[BT_MAC_LEN];
static uint16_t mMaxAdvSets;
static bool mCanSetAdvTxPower;


/* fwd declarations */
static bool hciBtStackUp(void);
static bool hciScheduleConnUpNotif(hci_conn_t conn, const struct bt_addr *peerAddr, const struct bt_addr *selfAddr, bool isMaster, bool isEncrypted, bool isMitmSafe);
static bool hciScheduleConnDownNotif(hci_conn_t conn, uint8_t reason);
static bool hciScheduleConnDataRx(hci_conn_t conn, sg packet, bool first);
static bool hciCmdSubmitSimpleWithStatusWithDoneCbk(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, hciOpDoneCbk cbk, void *cbkData);
static bool hciCmdSubmitSimpleWithCompleteSync(uint8_t ogf, uint16_t ocf, const void* paramData, uint8_t paramLen, void* evtBuf, uint8_t evtBufSz);
static bool hciCmdSubmitSimpleWithStatusSync(uint8_t ogf, uint16_t ocf, const void *paramData, uint8_t paramLen, struct hciEvtCmdStatus *dstEvt);
static void hciHandleConnAclDown(uint16_t cid, uint8_t reason);
static bool hciHandleConnAclLeUp(uint8_t status, uint16_t id, bool isMaster, const uint8_t *mac, bool addrRandom, uint16_t interval, uint16_t latency, uint16_t timeout, uint8_t mca);
static bool hciHandleConnAclEncr(uint16_t cid, bool encrOn);
static bool hciHandleConnAclAuth(uint16_t cid, bool authOn);
static bool hciHandleConnAclLeUpdate(uint8_t status, uint16_t cid, uint16_t interval, uint16_t latency, uint16_t timeout);
static bool hciConnUpdate(uint16_t cid, uint16_t minInt, uint16_t maxInt, uint16_t latency, uint16_t timeout);
static void hciCmdStatusCbk(void *cbkData, uint8_t status);
static bool hciConnDisconnect(uint16_t cid);
static bool hciConnLeCancel(void);
static void hciConnCfg(struct hciAclConn *c, uint32_t step, uint32_t aux);
static bool hciRxAclForRunningConn(struct hciAclConn* conn, uint16_t hdr, sg packet);
static void hciConnAclStructDel(struct hciAclConn* conn);
static bool hciSetupResetEventHandler(bool cmdWaitLockHeld);

/* adv indirection function pointers. Called with mAdvSetsLock held*/
static bool (*mHciAdvSetEnable)(struct hciAdvSet *set, bool enable);


/* templated commands - they need the above vars hence include here */
#include "hci_templated_commands.h"


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

    while (c && memcmp(&c->peerAddr, addr, sizeof(c->peerAddr)))
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
 * FUNCTION: hciEvtWaitEnqueueUnlocked
 * USE:      Enqueue a handler for an event we expect
 * PARAMS:   ewd - the event wait descriptor
 *           forCmdID - the commandID this is for
 * RETURN:   uniq_t identifying this event wait object or 0 on failure
 * NOTES:    must be called with mCmdWaitLock held
 */
static uniq_t hciEvtWaitEnqueueUnlocked(const struct hciEvtWaitDescr *ewd, uniq_t forCmdID)
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

    ws->prev = mEvtWaitTail;
    mEvtWaitTail = ws;
    if (ws->prev)
        ws->prev->next = ws;
    else
        mEvtWaitHead = ws;

    return ret;
}

/*
 * FUNCTION: hciEvtWaitEnqueue
 * USE:      Enqueue a handler for an event we expect
 * PARAMS:   ewd - the event wait descriptor
 *           forCmdID - the commandID this is for
 * RETURN:   uniq_t identifying this event wait object or 0 on failure
 * NOTES:
 */
static uniq_t hciEvtWaitEnqueue(const struct hciEvtWaitDescr *ewd, uniq_t forCmdID)
{
    uniq_t ret;

    pthread_mutex_lock(&mCmdWaitLock);
    ret = hciEvtWaitEnqueueUnlocked(ewd, forCmdID);
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
 * FUNCTION: hciRxAclForRunningConn
 * USE:      Called with a valid ACL packet/fragment for a running connection
 * PARAMS:   conn - the connection this is for
 *           hdr - packet header
 *           packet - the packet we got
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static bool hciRxAclForRunningConn(struct hciAclConn* conn, uint16_t hdr, sg packet)
{
    uint16_t pb = hdr & ACL_HDR_MASK_PB;
    uint16_t bc = hdr & ACL_HDR_MASK_BC;

    if (pb == ACL_HDR_PB_FIRST_NONAUTO || pb == ACL_HDR_PB_COMPLETE) {
        logw("Invalid acl.pb flag from chp: 0x%04X. Dropped\n", pb);
        return false;
    }

    return hciScheduleConnDataRx(conn->handle, packet, pb == ACL_HDR_PB_FIRST_AUTO);
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
    uint16_t cid = hdr & ACL_HDR_MASK_CONN_ID;
    struct hciAclConn* conn;


    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindById(cid);
    if (!conn) {
        logi("Dropping packet for unknown acl link %d\n", cid);
        sgFree(packet);
    } else if (conn->state == CONN_STATE_CFG) {
        struct hciBacklogItemHdr itemHdr = {.len = sgLength(packet), .aclHdr = hdr};

        if (!conn->rxBacklog)
            conn->rxBacklog = sgNew();

        if (conn->rxBacklog && sgConcatFrontCopy(packet, &itemHdr, sizeof(itemHdr)) && sgLength(conn->rxBacklog) + sgLength(packet) <= HCI_CONN_CFG_DATA_BACKLOG_MAX) {
            sgConcat(conn->rxBacklog, packet);
            logd("Added packet to RX backlog\n");
        } else {
            logi("Dropping connection -> we're unable to save RX in backlog\n");
            hciConnDisconnect(conn->id);
            sgFree(packet);
        }
    } else if (conn->state != CONN_STATE_RUNNING) {
        logi("Dropping arrived data for connection in state %u\n", conn->state);
        sgFree(packet);
    } else if (!hciRxAclForRunningConn(conn, hdr, packet)) {
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
            uint8_t acceptCredits = 0;

            conn = hciConnFindById(connID);
            if (!conn) {
                loge("Unknown conn 0x%04X in %d packets completed.\n", connID, numPackets);
            } else {
                if (conn->outstandingPackets >= numPackets) {
                    acceptCredits = numPackets;
                    conn->outstandingPackets -= numPackets;
                } else {
                    loge("Conn %u has %u outstanding packets but got %u credits back.\n", (unsigned)conn->id, conn->outstandingPackets, numPackets);
                    acceptCredits = conn->outstandingPackets;
                    conn->outstandingPackets = 0;
                }
            }
            if (HCI_TAKE_CREDS_FOR_UNKNOWN_CONNS && acceptCredits != numPackets) {
                logw("Accepting %d credits for unknown packets\n", numPackets - acceptCredits);
                acceptCredits = numPackets;
            }
            if (BT_ADDR_IS_LE(conn->peerAddr) && !mBtJointBuffers) {
                hadLeCreds = true;
                newCreditsLe += acceptCredits;
            } else {
                hadEdrCreds = true;
                newCreditsEdr += acceptCredits;
            }
        }
        pthread_mutex_unlock(&mConnsLock);
        while (newCreditsLe--)
            sem_post(&mAclPacketsLe);
        while (newCreditsEdr--)
            sem_post(&mAclPacketsEdr);
        if (hadLeCreds || (hadEdrCreds && mBtJointBuffers))
            l2cAclCreditAvail(true);
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
        if (mStackState == STACK_STATE_GOING_DOWN) {
            //if we are now in reset, delete all waiters. reset handler code cannot do it as we still hold the lock
            while (mEvtWaitHead)
                 hciEvtWaitDequeueInt(mEvtWaitHead);
            hciSetupResetEventHandler(true);
            mStackState = STACK_STATE_DOWN;
        }
        pthread_mutex_unlock(&mCmdWaitLock);
    }

    if (addCredit) {
        sem_post(&mCmdSendSem);
        if (mStackState == STACK_STATE_DOWN)
            mStackState = STACK_STATE_READY_FOR_UP;
    }

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
        loge("Got SCO packet\n");
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
        case CBK_WORK_LE_DISC_DEV:
            mDiscoverLeCbk(mDiscoverLeData, &w->leDev.addr, w->leDev.rssi, w->leDev.advType, w->leDev.adv, w->leDev.advLen);
            break;
        case CBK_WORK_FLUSH:
            sem_post(w->flush.sem);
            break;
        case CBK_WORK_CONN_UP:
            l2cAclLinkUp(w->connUp.conn, &w->connUp.peerAddr, &w->connUp.selfAddr, w->connUp.isMaster, w->connUp.isEncrypted, w->connUp.isMitmSafe);
            break;
        case CBK_WORK_CONN_DOWN:
            l2cAclLinkDown(w->connDown.conn, w->connDown.reason);
            break;
        case CBK_WORK_PARAMS_CHANGE:
            l2cAclLinkParamsChange(w->leParamChange.conn, w->leParamChange.success, w->leParamChange.interval, w->leParamChange.latency, w->leParamChange.timeout);
            break;
        case CBK_WORK_ENCR_CHANGE:
            l2cAclLinkEncrChange(w->encrChange.conn, w->encrChange.nowEncrypted, w->encrChange.isMitmSafe);
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
 *           peerAddr - peer address
 *           selfAddr - whom we were being when we made the connection
 *           isMaster - are we master on the link
 *           isEncrypted - is the link encrypted
 *           isMitmSafe - is th elink MITM-safe?
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnUpNotif(hci_conn_t conn, const struct bt_addr *peerAddr, const struct bt_addr *selfAddr, bool isMaster, bool isEncrypted, bool isMitmSafe)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_CONN_UP;
    w->connUp.conn = conn;
    w->connUp.isMaster = isMaster;
    w->connUp.isEncrypted = isEncrypted;

    memcpy(&w->connUp.peerAddr, peerAddr, sizeof(w->connUp.peerAddr));
    memcpy(&w->connUp.selfAddr, selfAddr, sizeof(w->connUp.selfAddr));

    if (workQueuePut(mCallbackWork, w))
        return true;

    free(w);
    return false;
}

/*
 * FUNCTION: hciScheduleConnDownNotif
 * USE:      Enqueue a call to the ACL link down notif
 * PARAMS:   conn - the connection handle
 *           reason - the reason to pass to upper layers
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnDownNotif(hci_conn_t conn, uint8_t reason)
{
    struct hciCbkWorkItem *w;

    w = (struct hciCbkWorkItem*)malloc(sizeof(struct hciCbkWorkItem));
    if (!w)
        return false;

    w->cbkWorkItemType = CBK_WORK_CONN_DOWN;
    w->connDown.conn = conn;
    w->connDown.reason = reason;

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
 *           isMitmSafe - are we now MITM-safe?
 * RETURN:   true if enqueued, false else
 * NOTES:
 */
static bool hciScheduleConnEncrChange(hci_conn_t conn, bool nowEncrypted, bool isMitmSafe)
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
    bool weExpectReply = false;
    struct timespec ts;
    int ret, i;

    pthread_setname_np(pthread_self(), "bt_hci_cmds_send");

    while (1) {

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += BT_COMMAND_TIMEOUT;
        if (r_sem_timedwait(&mCmdSendSem, &ts)) {
            loge("CHIP DEAD!\n");
            mt_analize_death();
            abort();
        }
        vendorTxDoneWithEvts();
        ret = workQueueGet(mCmdsSendWork, (void**)&w);
        if (ret)
            break;

        /*
         * This lock makes sure adding a handler for an event and sending the
         * corresponding command are handled as one atomic unit.
         */
        pthread_mutex_lock(&mCmdLock);

        for (i = 0; i < w->numEventWaiters; i++) {
            if (!w->ewds[i].descr.persistent)
                weExpectReply = true;
            w->ewds[i].id = hciEvtWaitEnqueue(&w->ewds[i].descr, w->cmdID);
            if (!w->ewds[i].id) {
                loge("Failed to enqueue handler [%d] \n", i);
                goto failed;
            }
        }

        if (vendorTx(HCI_PKT_TYP_CMD, w->cmd, weExpectReply))
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

    ret = sem_init(&mCmdSendSem, 0, 0);
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

    sem_init(&mAclPacketsLe, 0, 0);
    sem_init(&mAclPacketsEdr, 0, 0);

    hciSetupResetEventHandler(false);
    vendorStartRx();

    logd("Stack ready for up...awaiting reset event\n");
    //in a real stack we'd call hciBtStackUp() here. but with splitter, we just wait...
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
 *           on after this (in higher layers). does not cleanup
 *           chip state - assumes chip will be reset.
 */
void hciDown(void)
{
    logd("Cancelling all event waiters\n");
    pthread_mutex_lock(&mCmdWaitLock);
    while (mEvtWaitHead) {
        mEvtWaitHead->cbk(NULL, mEvtWaitHead->cbkData, mEvtWaitHead->evtWaitStateID, mEvtWaitHead->forCmdID);
        hciEvtWaitDequeueInt(mEvtWaitHead);
    }
    while (mConns) {
        struct hciAclConn *t = mConns;
        mConns = mConns->next;
        free(t);
    }
    while (mAdvSets) {
        struct hciAdvSet *t = mAdvSets;
        mAdvSets = mAdvSets->next;
        free(t);
    }
    pthread_mutex_unlock(&mCmdWaitLock);

    //more deinit here
    pthread_mutex_lock(&mDiscoveryStateLock);
    mDiscoverLeHandle = 0;
    pthread_mutex_unlock(&mDiscoveryStateLock);

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
 * USE:      Atomically enqueue a command to be sent to the chip and event handlers for
 *           events that will result.
 * PARAMS:   ogf - the command group
 *           ocf - the command code
 *           paramData - command params
 *           paramLen - parameter length
 *           ... - NULL-terminated list of const struct hciEvtWaitDescr* for wanted events
 * RETURN:   success
 * NOTES:    SHALL FINISH QUICKLY AND NOT BLOCK. WILL NOT DEADLOCK YOU! ALWAYS SAFE TO CALL!
 */
static bool hciCmdSubmit(uint8_t ogf, uint16_t ocf, const void* paramData, uint32_t paramLen, ...)
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
    uint8_t paramLen;
    uint8_t evtHdrLen;
    bool ret = false;

    if (!evt)
        return hciScheduleSimpleCbk(cbkd->cbk, cbkd->cbkData1, cbkd->cbkData2, true, NULL);

    paramLen = utilGetLE8(&evt->len);
    evtHdrLen = cbkd->complete ? sizeof(struct hciEvtCmdComplete) : sizeof(struct hciEvtCmdStatus);

    if (paramLen < evtHdrLen) {
        logw("Comand %s event too short @ %ub (wanted %ub+)\n", name, paramLen, evtHdrLen);
        goto out;
    }
    paramLen -= evtHdrLen;

    /* is param length within bounds? */
    if (paramLen < cbkd->expectedParamLenMin || paramLen > cbkd->expectedParamLenMax) {
        logw("Got expected %s event, but wrong size: %u !< %u !< %u\n", name,
            cbkd->expectedParamLenMin, paramLen, cbkd->expectedParamLenMax);
        goto out;
    }

    /* enqueue the callback */
    ret = hciScheduleSimpleCbk(cbkd->cbk, cbkd->cbkData1, cbkd->cbkData2, false, cbkd->wantEventItself ? evt : NULL);

out:
    free(cbkData);
    return ret;
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
 * FUNCTION: hciInquiryUpdatePersistDeviceDb
 * USE:      Update persistent "seen devices" DB with EIR data
 * PARAMS:   addr - the address of the remote device
 *           devClsP - device class or NULL if not known
 *           eir - the provided EIR data (if any)
 *           eirLen - size of the above buffer
 * RETURN:   NONE
 * NOTES:
 */
void hciInquiryUpdatePersistDeviceDb(const struct bt_addr *addr, const uint32_t *devClsP, const void *eir, uint8_t eirLen)
{
    const uint8_t *name = NULL;
    uint32_t nameLen = 0;
    uint32_t eirDevCls;
    bool nameIsFull = false;

    if (eirLen) { /* parse eir to find name */
        const uint8_t *eirP = (const uint8_t*)eir;
        const uint8_t *eirEnd = eirP + eirLen;

        while (eirP < eirEnd) {
            uint8_t type, len = *eirP++;
            if (!len) /* zero length item means end of EIR */
                break;
            type = *eirP++;
            len--;

            if (type == HCI_EIR_TYPE_SHORTENED_NAME || type == HCI_EIR_TYPE_FULL_NAME) {
                name = eirP;
                nameLen = len;
                nameIsFull = type == HCI_EIR_TYPE_FULL_NAME;
            } else if (type == HCI_EIR_DEV_CLS && len == 3) {
                eirDevCls = utilGetLE24(eirP);
                devClsP = &eirDevCls;
            }
            eirP += len;
        }
    }

    persistAddKnownDev(addr, name, name ? &nameLen : NULL, nameIsFull, devClsP);
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

    hciInquiryUpdatePersistDeviceDb(addr, NULL, advData, advDataLen);
    if (workQueuePut(mCallbackWork, w))
        return;

    free(w);
    logi("Dropping LE discovered item dues to queue error\n");
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
    if (evtType == HCI_EVT_Disconnection_Complete) {
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
        bool encrOn = !status && utilGetLE8(&encr->encrOn);

        logd("ACL conn %d encr %d with status %d\n", cid, encrOn, status);
        if (!hciHandleConnAclEncr(cid, encrOn))
            loge("Failed to handle conn encr\n");
    } else if (evtType == HCI_EVT_Authentication_Complete) {
        struct hciEvtEncrChange *encr = (struct hciEvtEncrChange*)(evt + 1);
        uint8_t status = utilGetLE8(&encr->status);
        uint16_t cid = utilGetLE16(&encr->conn);

        logd("ACL conn %d auth with status %d\n", cid, status);
        if (!hciHandleConnAclAuth(cid, !status))
            loge("Failed to handle conn auth\n");
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
    uint8_t i, num;

    if (!evt)
        return false;

    evtType = utilGetLE8(&evt->code);

    if(evtType == HCI_EVT_LE_Meta) {
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
    } else {
        loge("Unexpected event 0x%02X in discovery event handler\n", evtType);
        return false;
    }

    return true;
}

/*
 * FUNCTION: hciResetEvtCbk
 * USE:      Called when a reset complete event arrives. Tears down any existing state and sets the in-reset state.
 * PARAMS:   evt - the event (unused)
 *           cbkData - callback data (unused)
 *           evtWaitStateID - event wait state ID (unused)
 *           forCmdID - event command ID (unused)
 * RETURN:   false on error
 * NOTES:
 */
static bool hciResetEvtCbk(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    //notify everyone that all connections are down


    pthread_mutex_lock(&mConnsLock);
    while (mConns) {
        //for all connections upper layers know of, tell them they are no more
        if (mConns->state == CONN_STATE_CFG || mConns->state == CONN_STATE_RUNNING)
            if (!hciScheduleConnDownNotif(mConns->handle, HCI_ERR_Connection_Terminated_By_Local_Host))
                loge("Failed to schedule conn down notif\n");
        hciConnAclStructDel(mConns);
    }
    pthread_mutex_unlock(&mConnsLock);

    //remove command send authority if it exists (this event may give us a credit or not, that will be handled later)
    while(!sem_trywait(&mCmdSendSem));

    //remember that we're in reset
    mStackState = STACK_STATE_GOING_DOWN;

    return true;
}

/*
 * FUNCTION: hciSetupResetEventHandler
 * USE:      Set up the handler for the "reset" event
 * PARAMS:   cmdWaitLockHeld - set if caller has mCmdWaitLock
 * RETURN:   false on error
 * NOTES:    may be called with mCmdWaitLock held (see params)
 */
static bool hciSetupResetEventHandler(bool cmdWaitLockHeld)
{
    bool ret;
    struct hciEvtWaitDescr ewd = {
        .cbk = hciResetEvtCbk,
        .evtType = HCI_EVT_Command_Complete,
        .extra = CMD_MAKE_OPCODE(HCI_OGF_Controller_and_Baseband, HCI_CMD_Reset),
        .persistent = true,
    };

    if (cmdWaitLockHeld)
         ret = hciEvtWaitEnqueueUnlocked(&ewd, 0);
    else
         ret = hciEvtWaitEnqueue(&ewd, 0);

    return ret;
}

/*
 * FUNCTION: hciIsUp
 * USE:      Ask if chip is ready to be used
 * PARAMS:   none
 * RETURN:   the answer
 * NOTES:
 */
bool hciIsUp(void)
{
    loge(" *X* state = %d\n", mStackState);

    if (mStackState == STACK_STATE_READY_FOR_UP)
        return hciBtStackUp();

    return mStackState == STACK_STATE_UP;
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
 * FUNCTION: hciGetLocalAddress
 * USE:      Get the local static BT addr
 * PARAMS:   addr - the buffer to stash address in. not a bt_addr struct since type is not determinable
 * RETURN:   NONE
 * NOTES:
 */
void hciGetLocalAddress(uint8_t *addr)
{
    memcpy(addr, mLocalAddr, sizeof(mLocalAddr));
}

/*
 * FUNCTION: hciAdvSetEnableDisableMethodBT4
 * USE:      Enable or disable an adv set on normal BT4 devices
 * PARAMS:   set - the set to operate on
 *           enable - on or off?
 * RETURN:   success
 * NOTES:    called with mAdvSetsLock held
 *           This is one of the few "adv set on/off handlers". This one is used
 *           when neither the BT5.0 nor the android-vendor methods are available.
 */
static bool hciAdvSetEnableDisableMethodBT4(struct hciAdvSet *set, bool enable)
{
    struct hciAdvSet *t;
    int ret;

    if (!enable) { // we only have one set - simply disable it

        if (hciLeSetAdvEnableSync(false)) {
            logw("Failed to disable BT4 adv\n");
            return false;
        }
        set->hwEnabled = false;
        return true;
    }

    //we want to enable: first see if another set is already on
    for (t = mAdvSets; t; t = t->next) {
        if (t->hwEnabled) {
            logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because set 0x"HCI_ADV_SET_FMT"x is enabled.\n", HCI_ADV_SET_CONV(set->handle), HCI_ADV_SET_CONV(t->handle));
            return false;
        }
    }

    //we want to enable and no other set is enabled - simply configure the chip and go
    ret = hciLeSetAdvDataSync(set->advDataLen, set->advData);
    if (ret) {
        logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because SetAdvData command failed: %02X\n", HCI_ADV_SET_CONV(set->handle), ret);
        return false;
    }
    ret = hciLeSetScanResponseDataSync(set->scanRspDataLen, set->scanRspData);
    if (ret) {
        logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because SetScanRspData command failed: %02X\n", HCI_ADV_SET_CONV(set->handle), ret);
        return false;
    }
    ret = hciLeSetAdvParamsSync(set->intervalMin, set->intervalMax, set->advType, set->ownAddressType, set->directAddr.type == BT_ADDR_TYPE_LE_RANDOM ? 1 : 0, set->directAddr.addr, set->channelMap, set->filterPolicy);
    if (ret) {
        logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because SetAdvParams command failed: %02X\n", HCI_ADV_SET_CONV(set->handle), ret);
        return false;
    }
    if (set->ownAddressType == HCI_ADV_OWN_ADDR_TYPE_RANDOM || set->ownAddressType == HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_RANDOM) {
        ret = hciLeSetRandomAddressSync(set->ownRandomAddr.addr);
        if (ret) {
            logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because SetRandomAddr command failed: %02X\n", HCI_ADV_SET_CONV(set->handle), ret);
            return false;
        }
    }
    ret = hciLeSetAdvEnableSync(true);
    if (ret) {
        logw("Cannot enable adv 0x"HCI_ADV_SET_FMT"x because SetAdvEnable command failed: %02X\n", HCI_ADV_SET_CONV(set->handle), ret);
        return false;
    }

    set->hwEnabled = true;
    return true;
}

/*
 * FUNCTION: hciBtStackUp
 * USE:      Brings up the chip in a sane configuration, reads versions, etc
 * PARAMS:   NONE
 * RETURN:   false on error
 * NOTES:
 */
static bool hciBtStackUp(void)
{
    int ret = 0;
    uint8_t inqMode = HCI_INQ_MODE_STD;

    /* sanity and start */
    {
        if (mStackState != STACK_STATE_READY_FOR_UP) {
            loge("Stack cannot be brought up if not ready for bringup\n");
            return false;
        }
        mStackState = STACK_STATE_COMING_UP;
    }

    /* read local BT version */
    {
        struct hciCmplReadLocalVersion evt;
        uint8_t hciVersion, lmpVersion;

        logi("BT UP: reading version\n");
        ret = hciReadLocalVersionSync(&evt);
        if (ret)
            goto out;

        hciVersion = utilGetLE8(&evt.hciVersion);
        lmpVersion = utilGetLE8(&evt.lmpVersion);
        logi("BT UP: bt versions reported: %u, %d\n", hciVersion, lmpVersion);

        if (hciVersion > HCI_VER_MAX_SUPPORTED)
            hciVersion = HCI_VER_MAX_SUPPORTED;

        if (lmpVersion > HCI_VER_MAX_SUPPORTED)
            lmpVersion = HCI_VER_MAX_SUPPORTED;

        mBtVer = hciVersion < lmpVersion ? hciVersion : lmpVersion;

        if (mBtVer == HCI_VERSION_2_0) { /* do 2.0 chips really exist? */
            logi("BT 2.0 chip???\n");
            mBtVer = HCI_VERSION_1_2;
        }

        logi("BT UP: effective BT version: %u (BT spec %s)\n", mBtVer, mBtVers[mBtVer]);

        if (mBtVer < HCI_VERSION_4_0) {
            loge("BT 4.0 minimum!\n");
            ret = -3;
            goto out;
        }
        /* past this point, we can assume BT 4.0+ */
    }

    /* read supported features ad make sure LE is in there*/
    {
        struct hciCmplReadLocalSupportedFeatures evt;

        logi("BT UP: reading features\n");
        ret = hciReadLocalSupportedFeaturesSync(&evt);
        if (ret)
            goto out;

        mLocalFtrs[0] = utilGetLE64(&evt.features);
        logi("BT UP: bt features reported: "FMT64"X\n", CNV64(mLocalFtrs[0]));

        if (!(mLocalFtrs[0] & HCI_LMP_FTR_LE_SUPPORTED_CONTROLLER)) {
            loge("BT LE required!\n");
            ret = -3;
            goto out;
        }
    }

    /* read extended features if possible */
    if (mLocalFtrs[0] & HCI_LMP_FTR_EXTENDED_FEATURES) {
        uint8_t page, numPages = 1;

        for (page = 0; page < numPages && page < MAX_FTR_PAGES; page++) {
            struct hciCmplReadLocalExtendedFeatures evt;
            uint64_t ftrs;
            uint8_t claimedPage;

            ret = hciReadLocalExtendedFeaturesSync(page, &evt);
            if (ret)
                goto out;
            ftrs = utilGetLE64(&evt.features);
            claimedPage = utilGetLE8(&evt.page);
            numPages = utilGetLE8(&evt.maxPage) + 1;

            if (claimedPage < MAX_FTR_PAGES)
                mLocalFtrs[claimedPage] = ftrs;
            else
                logw("Ignoring report for ftr page %u\n", claimedPage);

            logi("BT UP: bt extended features[%d/%d, requested %u] reported: "FMT64"X\n", claimedPage, numPages, page, CNV64(ftrs));
        }
    }

    /* read MAC address and show it */
    {
        struct hciCmplReadBdAddr evt;

        logi("Reading local MAC\n");
        ret = hciReadBdAddrSync(&evt);
        if (ret)
            goto out;

        memcpy(mLocalAddr, evt.mac, sizeof(mLocalAddr));
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
    }

    /* read LE buffer sizes & features, if LE exists */
    {
        struct hciCmplLeReadLocalSupportedFeatures evtFtrs;
        struct hciCmplLeReadBufferSize evt;
        uint16_t i;

        logi("BT UP: reading LE buffer size\n");
        ret = hciLeReadBufferSizeSync(&evt);
        if (ret)
            goto out;

        mAclBufSzLe = utilGetLE16(&evt.leBufferSize);
        mAclBufNumLe = utilGetLE8(&evt.leNumBuffers);

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

    /* enable LE */
    logi("Enabling LE\n");
    ret = hciWriteLeHostSupportedSync(true, false);
    if (ret)
        goto out;
    logi("LE should be on\n");

    /* set event masks */
    {
        uint64_t evts = 0, evts2 = 0, evtsLe = 0;

        switch (mBtVer) {
        case HCI_VERSION_4_0:
            evts = HCI_EVENT_ALL_BT_4_0;
            evts2 = HCI_EVENT_P2_ALL_BT_4_0;
            evtsLe = HCI_LE_EVENT_ALL_BT_4_0;
            break;
        case HCI_VERSION_4_1:
            evts = HCI_EVENT_ALL_BT_4_1;
            evts2 = HCI_EVENT_P2_ALL_BT_4_1;
            evtsLe = HCI_LE_EVENT_ALL_BT_4_1;
            break;
        case HCI_VERSION_4_2:
            evts = HCI_EVENT_ALL_BT_4_2;
            evts2 = HCI_EVENT_P2_ALL_BT_4_2;
            evtsLe = HCI_LE_EVENT_ALL_BT_4_2;
            break;
        case HCI_VERSION_5_0:
            evts = HCI_EVENT_ALL_BT_5_0;
            evts2 = HCI_EVENT_P2_ALL_BT_5_0;
            evtsLe = HCI_LE_EVENT_ALL_BT_5_0;
            break;
        }
        evts &=~ HCI_EVENT_REMOTE_HOST_SUPPORTED_FEATURES; /* we do not want this one - it is spammy and useless for now */
        evts &=~ HCI_EVENT_MAX_SLOTS_CHANGE; /* same here, really */
        evtsLe &=~ HCI_LE_EVENT_SCAN_REQUEST_RECVD; /* same here */
        logi("Setting event mask to 0x"FMT64"X\n", CNV64(evts));
        ret = hciSetEventMaskSync(evts);
        if (ret)
            goto out;

        logi("Setting event mask page 2 to 0x"FMT64"X\n", CNV64(evts2));
        ret = hciSetEventMaskPage2Sync(evts2);
        if (ret)
            goto out;

        logi("Setting LE event mask to 0x"FMT64"X\n", CNV64(evtsLe));
        ret = hciLeSetEventMaskSync(evtsLe);
        if (ret)
            goto out;
    }

    /* set up event handler(s) for reset */
    hciSetupResetEventHandler(false);

    /* set up event handler(s) for discovery results */
    {
        struct hciEvtWaitDescr ewd = {
            .cbk = hciInquiryEvtCbk,
            .evtType = HCI_EVT_LE_Meta,
            .extra = HCI_EVTLE_Advertising_Report,
            .persistent = true,
        };

        if (!hciEvtWaitEnqueue(&ewd, 0)) {
            ret = -2;
            loge("Failed to enqueue scan handler\n");
            goto out;
        }
    }

    /* set event handlers for connection states */
    {
        static const uint8_t connEvtTypes[] = {HCI_EVT_Disconnection_Complete, HCI_EVT_Encryption_Change,
                                               HCI_EVT_Authentication_Complete, HCI_EVT_LE_Meta, HCI_EVT_LE_Meta, 0};
        static const uint16_t connEvtExtras[] = {0, 0, 0, HCI_EVTLE_Connection_Complete, HCI_EVTLE_Connection_Update_Complete};

        if (!hciSetManyEventPersistentHandlers(connEvtTypes, connEvtExtras, hciConnEvtCbk, NULL)) {
            ret = -2;
            loge("Failed to enqueue connection state handlers\n");
            goto out;
        }
    }

    /* figure out the multi-adv situation */
    mMaxAdvSets = 1;
    mCanSetAdvTxPower = false;
    mHciAdvSetEnable = hciAdvSetEnableDisableMethodBT4;
    if (mBtVer >= HCI_VERSION_5_0) {
        struct hciCmplLeReadNumSupportedAdvSets evt;

        logi("Will use BT5 multi-adv support\n");
        ret = hciLeReadNumberOfSupportedAdvertisingSetsSync(&evt);
        if (ret)
            logw(" Read number supported adv sets command errored out (0x%02x) - will use normal advertising\n", ret);
        else {
            mMaxAdvSets = evt.maxAdvsets;
            mCanSetAdvTxPower = true;
            logi(" Controller supports up to %u adv sets\n", mMaxAdvSets);
        }
    }
    else if (mBtVer >= HCI_VERSION_4_0) {
        struct hciCmplVendorLeGetVendorCapabilities evt;

        logi("Will try BT4 android-vendor multi-adv support\n");

        ret = hciVendorLeGetVendorCapabilitiesSync(&evt);
        if (!ret) {
            mMaxAdvSets = evt.maxAdvtInstances;
            mCanSetAdvTxPower = true;
            logi(" Controller supports up to %u adv sets\n", mMaxAdvSets);
        }
        else if (ret == HCI_ERR_Unknown_HCI_Command)
            logi(" android-vendor extensions not found - will use normal advertising\n");
        else
            logw(" Vendor test command errored out (0x%02x) - will use normal advertising\n", ret);
    }
    else {
        mMaxAdvSets = 0;
        mCanSetAdvTxPower = false;
        logi("BT4.0 required for advetising. Advertising will not be supported\n");
    }

out:
    if (ret) {
        loge("Status was 0x%02X\n", ret);
        mStackState = STACK_STATE_DOWN;
        return false;
    }
    mStackState = STACK_STATE_UP;
    return true;
}

/*
 * FUNCTION: hciAdvSetFindByHandle
 * USE:      Find an adv set by handle
 * PARAMS:   handle - the handle
 * RETURN:   the set structure or NULL
 * NOTES:    call with mAdvSetsLock held
 */
static struct hciAdvSet* hciAdvSetFindByHandle(hci_adv_set_t handle)
{
    struct hciAdvSet* t;

    for (t = mAdvSets; t; t = t->next) {
        if (t->handle == handle)
            return t;
    }

    return NULL;
}

/*
 * FUNCTION: hciAdvIsPowerLevelSettingSupported
 * USE:      Can adv tx power be set with our chip?
 * PARAMS:   none
 * RETURN:   the answer
 * NOTES:
 */
bool hciAdvIsPowerLevelSettingSupported(void)
{
    return mCanSetAdvTxPower;
}

/*
 * FUNCTION: hciAdvGetMaxAdvSetsSupported
 * USE:      How many adv sets can we support (max) on hardware
 * PARAMS:   none
 * RETURN:   the answer
 * NOTES:
 */
uint32_t hciAdvGetMaxAdvSetsSupported(void)
{
    return mMaxAdvSets;
}

/*
 * FUNCTION: hciAdvSetAllocate
 * USE:      Allocate an advertising set structure
 * PARAMS:   none
 * RETURN:   the handle to this set
 * NOTES:    this does not in any way touch hardware
 */
hci_adv_set_t hciAdvSetAllocate(void)
{
    struct hciAdvSet *set = (struct hciAdvSet*)calloc(1, sizeof(struct hciAdvSet));
    hci_adv_set_t handle;

    if (!set)
        return 0;

    set->handle = handle = uniqGetNext();
    set->ownRandomAddr.type = BT_ADDR_TYPE_EDR; //definitely not valid for adv
    set->directAddr.type = BT_ADDR_TYPE_EDR;    //definitely not valid for adv
    pthread_mutex_lock(&mAdvSetsLock);
    set->next = mAdvSets;
    if (mAdvSets)
        mAdvSets->prev = set;
    mAdvSets = set;
    pthread_mutex_unlock(&mAdvSetsLock);

    return handle;
}

/*
 * FUNCTION: hciAdvSetFree
 * USE:      Free an advertising set structure. Set must not be enabled!
 * PARAMS:   handle - the handle to this set
 * RETURN:   success
 * NOTES:    this does not in any way touch hardware
 */
bool hciAdvSetFree(hci_adv_set_t handle)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is enabled - cannot delete it\n", HCI_ADV_SET_CONV(handle));
    else {
        if (set->next)
            set->next->prev = set->prev;
        if (set->prev)
            set->prev->next = set->next;
        else
            mAdvSets = set->next;
        free(set);
        ret = true;
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetConfigureData
 * USE:      Configure an advertising set's data. Set must not be enabled!
 * PARAMS:   handle - the handle to this set
 *           scanRsp - which data to config? SCAN_RSP or ADV
 *           data - the data to use
 *           len - length of said data
 * RETURN:   success
 * NOTES:    this does not in any way touch hardware
 */
bool hciAdvSetConfigureData(hci_adv_set_t handle, bool scanRsp, const uint8_t *data, uint32_t len)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is enabled - cannot edit it\n", HCI_ADV_SET_CONV(handle));
    else {

        uint32_t *lenP;
        uint8_t *dataP;
        uint32_t lenLimit;

        if (scanRsp) {
            lenP = &set->scanRspDataLen;
            dataP = set->scanRspData;
            lenLimit = HCI_SCAN_RSP_DATA_MAX_LEN;
        }
        else {
            lenP = &set->advDataLen;
            dataP = set->advData;
            lenLimit = HCI_ADV_DATA_MAX_LEN;
        }

        if (len > lenLimit)
            logw("%s data is too long (%u > %u)\n", scanRsp ? "SCAN_RSP" : "ADV", len, lenLimit);
        else {

            memcpy(dataP, data, len);
            *lenP = len;
            ret = true;
        }
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetOwnRandomAddr
 * USE:      Configure an advertising set's random adress. Set must not be enabled!
 * PARAMS:   handle - the handle to this set
 *           ownRandomAddr - the random address to use when random address requested for advertising
 * RETURN:   success
 * NOTES:    this does not in any way touch hardware
 */
bool hciAdvSetOwnRandomAddr(hci_adv_set_t handle, struct bt_addr *ownRandomAddr)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is enabled - cannot edit it\n", HCI_ADV_SET_CONV(handle));
    else if (ownRandomAddr->type != BT_ADDR_TYPE_LE_RANDOM)
        logw("Set 0x"HCI_ADV_SET_FMT"x random address cannot be set - provided addr is nto random\n", HCI_ADV_SET_CONV(handle));
    else {

        set->ownRandomAddr = *ownRandomAddr;
        ret = true;
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetSetAdvParams
 * USE:      Configure an advertising set's parameters. Set must not be enabled!
 * PARAMS:   handle - the handle to this set
 *           advIntervalMin - minimum adv interval (in units of 0.625ms)
 *           advIntervalMax - maximum adv interval (in units of 0.625ms)
 *           advType - adv type: HCI_ADV_TYPE_ADV_*
 *           ownAddressType - own addr type: HCI_ADV_OWN_ADDR_TYPE_*
 *           directAddr - if direct adv, the address of target, else ignored
 *           advChannelMap - channels to use: bitmask of HCI_ADV_CHAN_MAP_USE_*
 *           advFilterPolicy - filter policy: HCI_ADV_FILTER_POL_*
 *           advPower - TX power in dBm, ignored if not suported, see hciAdvIsPowerLevelSettingSupported()
 * RETURN:   success
 * NOTES:    this does not in any way touch hardware
 */
bool hciAdvSetSetAdvParams(hci_adv_set_t handle, uint16_t advIntervalMin, uint16_t advIntervalMax, uint8_t advType, uint8_t ownAddressType, struct bt_addr *directAddr, uint8_t advChannelMap, uint8_t advFilterPolicy, int8_t advPower)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is enabled - cannot edit it\n", HCI_ADV_SET_CONV(handle));
    else {

        set->intervalMin = advIntervalMin;
        set->intervalMax = advIntervalMax;
        set->advType = advType;
        set->ownAddressType = ownAddressType;
        if (directAddr)
            set->directAddr = *directAddr;
        else
            set->directAddr.type = BT_ADDR_TYPE_EDR; //definitely not valid for adv
        set->channelMap = advChannelMap;
        set->filterPolicy = advFilterPolicy;
        set->powerLevel = advPower;

        ret = true;
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetGetCurTxPowerLevel
 * USE:      Get the current tx power level of an adv set. Set must be enabled!
 * PARAMS:   handle - the handle to this set
 *           advTxPowerLevelP - where to store the result
 * RETURN:   success
 * NOTES:    this does not in any way touch hardware
 */
bool hciAdvSetGetCurTxPowerLevel(hci_adv_set_t handle, int8_t *advTxPowerLevelP)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (!set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is not enabled - cannot get its power\n", HCI_ADV_SET_CONV(handle));
    else {

        if (advTxPowerLevelP)
            *advTxPowerLevelP = set->hwPowerLevel;

        ret = true;
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetEnable
 * USE:      Try to enable a given adv set
 * PARAMS:   handle - the handle to this set
 * RETURN:   success
 * NOTES:    this may fail sometimes and succeed other times based on hw state
 */
bool hciAdvSetEnable(hci_adv_set_t handle)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is enabled - cannot enable it again\n", HCI_ADV_SET_CONV(handle));
    else if (!mHciAdvSetEnable)
        logw("No method for enable/disable defined for this chip - cannot enable adv set\n");
    else {

        ret = true;

        /* Sanity checks. All constantrs are from spec */
        if (!set->advDataLen) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has no adv data.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if (set->advType != HCI_ADV_TYPE_ADV_DIRECT_IND && (set->intervalMin > set->intervalMax || set->intervalMin < 0x20 || set->intervalMax > 0x4000)) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid interval params.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if (set->advType != HCI_ADV_TYPE_ADV_IND && set->advType != HCI_ADV_TYPE_ADV_DIRECT_IND && set->advType != HCI_ADV_TYPE_ADV_SCAN_IND && set->advType != HCI_ADV_TYPE_ADV_NONCONN_IND && set->advType != HCI_ADV_TYPE_ADV_DIRECT_IND_LOW_DUTY) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid adv type.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if (set->ownAddressType != HCI_ADV_OWN_ADDR_TYPE_PUBLIC && set->ownAddressType != HCI_ADV_OWN_ADDR_TYPE_RANDOM && set->ownAddressType != HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_PUBLIC && set->ownAddressType != HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_RANDOM) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid own addr type.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if ((set->advType == HCI_ADV_TYPE_ADV_DIRECT_IND || set->advType == HCI_ADV_TYPE_ADV_DIRECT_IND_LOW_DUTY) && set->directAddr.type != BT_ADDR_TYPE_LE_PUBLIC && set->directAddr.type != BT_ADDR_TYPE_LE_RANDOM) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid peer addr type.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if (!(set->channelMap & 7)) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid channel map.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if (set->filterPolicy != HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_ALL && set->filterPolicy != HCI_ADV_FILTER_POL_SCAN_LIST_CONNECT_ALL && set->filterPolicy != HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_LIST && set->filterPolicy != HCI_ADV_FILTER_POL_SCAN_LIST_CONNECT_LIST) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has invalid filter policy.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }
        if ((set->ownAddressType == HCI_ADV_OWN_ADDR_TYPE_RANDOM || set->ownAddressType == HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_RANDOM) && set->ownRandomAddr.type != BT_ADDR_TYPE_LE_RANDOM) {
            logw("Cannot enable adv set 0x"HCI_ADV_SET_FMT"x since it has no valid random address and might need it.\n", HCI_ADV_SET_CONV(handle));
            ret = false;
        }

        //if all checks pass - do the actual ON procedure
        ret = ret && mHciAdvSetEnable(set, true);
    }
    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
}

/*
 * FUNCTION: hciAdvSetDisable
 * USE:      Try to disable a given adv set
 * PARAMS:   handle - the handle to this set
 * RETURN:   success
 * NOTES:
 */
bool hciAdvSetDisable(hci_adv_set_t handle)
{
    struct hciAdvSet *set;
    bool ret = false;

    pthread_mutex_lock(&mAdvSetsLock);
    set = hciAdvSetFindByHandle(handle);
    if (!set)
        logw("Set handle 0x"HCI_ADV_SET_FMT"x not found\n", HCI_ADV_SET_CONV(handle));
    else if (!set->hwEnabled)
        logw("Set 0x"HCI_ADV_SET_FMT"x is disabled - cannot disable it again\n", HCI_ADV_SET_CONV(handle));
    else if (!mHciAdvSetEnable)
        logw("No method for enable/disable defined for this chip - cannot enable adv set\n");
    else
        ret = mHciAdvSetEnable(set, false);

    pthread_mutex_unlock(&mAdvSetsLock);

    return ret;
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
 * FUNCTION: hciInfoScoBufSize
 * USE:      Provide info on SCO buffer size in our controller and SCO support
 * PARAMS:   bufSz - where to store buffer sizes (or NULL if uninterested)
 *           bufNum - where to store number of buffers (or NULL if uninterested)
 * RETURN:   true if SCO supported
 * NOTES:
 */
bool hciInfoScoBufSize(uint16_t *bufSz, uint16_t *bufNum)
{
    return false;
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
 * FUNCTION: hciConnEncryptLe
 * USE:      Start an encryption attempt for an LE connection
 * PARAMS:   conn - the connection
 *           demandMitmSafe - do we want mitm-safety?
 * RETURN:   true if attempt will be made
 * NOTES:    call with mConnsLock held
 */
static bool hciConnEncryptLe(struct hciAclConn *conn, bool demandMitmSafe)
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
    if (conn->outstandingPackets) {
        if (BT_ADDR_IS_EDR(conn->peerAddr)) {
            while (conn->outstandingPackets--)
                sem_post(&mAclPacketsEdr);
            l2cAclCreditAvail(false);
        } else {
            while (conn->outstandingPackets--)
                sem_post(&mAclPacketsLe);
            l2cAclCreditAvail(true);
        }
    }

    if (conn->next)
        conn->next->prev = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        mConns = conn->next;

    if (conn->rxBacklog)
        sgFree(conn->rxBacklog);

    logd("deleting conn struct for "ADDRFMT" h="HANDLEFMT"\n", ADDRCONV(conn->peerAddr), HANDLECNV(conn->handle));
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

    memcpy(&conn->peerAddr, addr, sizeof(conn->peerAddr));
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
            if (!BT_ADDR_IS_EDR(*addr))
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
        bool isLE = BT_ADDR_IS_LE(conn->peerAddr);

        switch (conn->state) {
        case CONN_STATE_WAIT:
            if (isLE) {
                if (!hciConnLeCancel())
                    loge("Failed to request LE conection cancellation for "ADDRFMT"\n", ADDRCONV(conn->peerAddr));
            } else
                logd("cannot cancel in-progress EDR connection\n");
            conn->state = CONN_STATE_DELETING;
            break;
        case CONN_STATE_CFG:
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
bool hciDemandEncr(hci_conn_t aclConn, bool demandMitmSafe)
{
    struct hciAclConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn)
        logw("Conection for encrypt not found\n");
    else if (conn->state != CONN_STATE_RUNNING)
        logw("Cannot update encryption for not-yet-established connection\n");
    else if (conn->encrypted && (conn->mitmSafe || !demandMitmSafe))
        logw("Connection already encrypted up to requested level\n");
    else if (BT_ADDR_IS_LE(conn->peerAddr))
        ret = hciConnEncryptLe(conn, demandMitmSafe);
    pthread_mutex_unlock(&mConnsLock);

    return conn && ret;
}

/*
 * FUNCTION: hciLeConnGetInfo
 * USE:      Query info for an LE connection
 * PARAMS:   aclConn - the connection
 *           masterP - where to stor if we're master (or NULL)
 *           peerFtrsP - where to store peer features (or NULL)
 *           intP - where to store connection interval (or NULL)
 *           latP - where to sotre connectio nlatency (or NULL)
 *           toP - where to store connection timeout (or NULL)
 *           mcaP - where to store MCA (meaningless if we're master) (or NULL)
 * RETURN:   true if connection was found and results have been returned
 * NOTES:
 */
bool hciLeConnGetInfo(hci_conn_t aclConn, bool *masterP, uint64_t *peerFtrsP, uint16_t *intP, uint16_t *latP, uint16_t *toP, uint8_t *mcaP)
{
    struct hciAclConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn)
        logw("Conection for encrypt not found\n");
    else if (!BT_ADDR_IS_LE(conn->peerAddr))
        logw("Not an LE connection\n");
    else {
        if (masterP)
            *masterP = conn->isMaster;
        if (peerFtrsP)
            *peerFtrsP = conn->le.ftrs;
        if (intP)
            *intP = conn->le.interval;
        if (latP)
            *latP = conn->le.latency;
        if (toP)
            *toP = conn->le.timeout;
        if (mcaP)
            *mcaP = conn->le.mca;
        ret = true;
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
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
    uint8_t ret = HCI_TX_SEND_ERROR;

    pthread_mutex_lock(&mConnsLock);
    conn = hciConnFindByHandle(aclConn);
    if (!conn) {
        ret = HCI_TX_SEND_NO_CONN;
        logw("Cannot send on a nonexistent connection\n");
    } else if (conn->state != CONN_STATE_RUNNING) {
        ret = HCI_TX_SEND_NO_CONN;
        logw("Cannot send on a not-yet-established connection\n");
    } else {
        bool isLE = BT_ADDR_IS_LE(conn->peerAddr);
        bool useLeBuffers = isLE && !mBtJointBuffers;
        sem_t *relevantSem = useLeBuffers ? &mAclPacketsLe : &mAclPacketsEdr;
        int rv;

        rv = r_sem_trywait(relevantSem);
        if (rv && errno == EAGAIN)
            ret = HCI_TX_SEND_NO_CREDITS;
        else if (!rv) {
            struct hciAclHdr hdr;
            uint16_t pb = 0xFFFF; /* this will catch all other cases */
            uint16_t start_pb = mBtVer >= HCI_VERSION_2_1 ? ACL_HDR_PB_FIRST_NONAUTO : ACL_HDR_PB_FIRST_AUTO;
            uint16_t complete_pb = /*mBtVer >= HCI_VERSION_3_0 ? ACL_HDR_PB_COMPLETE : */start_pb; /* XXX: this causes problems with TI BT chips - they send "complete" as a "continue" confusing the other side */

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

            if (sgConcatFrontCopy(data, &hdr, sizeof(hdr))) {
                if (vendorTx(HCI_PKT_TYP_ACL, data, false)) {
                    conn->outstandingPackets++;
                    ret = HCI_TX_SEND_OK;
                } else {
                    /* undo our concat of header in case caller wants to use this sg again */
                    sgTruncFront(data, sizeof(hdr));
                }
            }
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
        case CONN_STATE_CFG:
        case CONN_STATE_RUNNING:
            if (!hciScheduleConnDownNotif(conn->handle, reason))
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
 * FUNCTION: hciConnCfgCbk
 * USE:      event handler for hciConnCfg()
 * PARAMS:   evt - the resuting event
 *           cbkData - the data from hciConnCfg (step and aux encoded into a pointer)
 *           evtWaitStateID - unused
 *           forCmdID - unused
 * RETURN:   true if this is our event (always yes)
 * NOTES:    call with mConnsLock held
 */
static bool hciConnCfgCbk(const struct hciEvtHdr *evt, void *cbkData, uniq_t evtWaitStateID, uniq_t forCmdID)
{
    struct hciEvtLeMeta *leMetaEvt = (struct hciEvtLeMeta*)(evt + 1);
    struct hciEvtReadRemoteSupportedFeaturesComplete *rrsf = (struct hciEvtReadRemoteSupportedFeaturesComplete*)(evt + 1);
    struct hciEvtLeReadRemoteFeaturesComplete *rrf = (struct hciEvtLeReadRemoteFeaturesComplete*)(leMetaEvt + 1);
    struct hciEvtReadRemoteExtFeturesComplete *rref = (struct hciEvtReadRemoteExtFeturesComplete*)(evt + 1);
    struct hciEvtReadRemoteVersionComplete *rrv = (struct hciEvtReadRemoteVersionComplete*)(evt + 1);
    struct hciEvtCmdComplete *cmpl = (struct hciEvtCmdComplete*)(evt + 1);
    struct hciEvtCmdStatus *stat = (struct hciEvtCmdStatus*)(evt + 1);
    struct hciCmplWriteAutoFlushTimeout *waft = (struct hciCmplWriteAutoFlushTimeout*)(cmpl + 1);
    uint16_t step = ((uint32_t)(uintptr_t)cbkData) >> 16;
    uint16_t aux = (uint16_t)(uintptr_t)cbkData;
    uint8_t evtType = utilGetLE8(&evt->code);
    struct hciAclConn *c = NULL;
    uint8_t sta = 0xff;
    uint16_t cid;

    pthread_mutex_lock(&mConnsLock);

    switch (step) {
    case CONN_CFG_STEP_LE + 0: /* first LE config step = get remote features */
        cid = utilGetLE16(&rrf->conn);
        sta = utilGetLE8(&rrf->status);
        c = hciConnFindById(cid);
        if (!c)
            break;
        if (sta)
            break;
        c->le.ftrs = utilGetLE64(&rrf->leFeatures);
        step++;
        break;
    default:
        loge("unknown connection config step step 0x%04X.05%04X\n", step, aux);
        break;
    }

    if (!c)
        loge("connection config failed: unable to find connection\n");
    else if (!sta)
        hciConnCfg(c, step, aux);
    else if (sta) {
        loge("connection config failed: sta 0x%02X. Closing.\n", sta);
        hciConnDisconnect(c->id);
    }

    pthread_mutex_unlock(&mConnsLock);

    return true;
}

/*
 * FUNCTION: hciConnCfg
 * USE:      Configure a new connection to our liking and tell upper layer when done
 * PARAMS:   c - the connection struct
 *           step - the step to perform
 *           aux - substep
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void hciConnCfg(struct hciAclConn *c, uint32_t step, uint32_t aux)
{
    struct hciEvtWaitDescr ewd[2] = {{.cbk = hciConnCfgCbk, .persistent = false}, {.cbk = hciConnCfgCbk, .persistent = false}};
    void *userData = (void*)(uintptr_t)((step << 16) | aux);
    struct hciReadRemoteSupportedFeatures rrsf;
    struct hciReadRemoteExtendedFeatures rref;
    struct hciLeReadRemoteUsedFeatures rrf;
    struct hciWriteAutoFlushTimeout waft;
    struct hciReadRemoteVersionInfo rrv;
    bool success = true;

    if (c->state != CONN_STATE_CFG) {
        logw("Tried to config a connection not in config state\n");
        return;
    }

    switch (step) {
    case CONN_CFG_STEP_LE + 0: /* first LE config step = get remote features */
        if (!c->isMaster)
            break; /* as a slave there is no config to do */
        utilSetLE16(&rrf.conn, c->id);
        ewd[0].evtType = HCI_EVT_LE_Meta;
        ewd[0].extra = HCI_EVTLE_Read_Remote_Used_Features_Complete;
        ewd[0].cbkData = userData;
        if (!hciCmdSubmit(HCI_OGF_LE, HCI_CMD_LE_Read_Remote_Used_Features, &rrf, sizeof(rrf), ewd + 0, NULL)) {
            success = false;
            loge("Failed to send read remote used features command\n");
            break;
        }
        return;

    case CONN_CFG_STEP_LE + 1: /* second LE config step = done! */
        break;
    }

    /* if we got here, the configuration step ended and it is time to proceed */
    if (success) {
        c->state = CONN_STATE_RUNNING; 
        if (hciScheduleConnUpNotif(c->handle, &c->peerAddr, &c->selfAddr, c->isMaster, c->encrypted, c->mitmSafe)) {
            /* if there are any backlogged RX messages, queue them up too */
            while (c->rxBacklog) {
                struct hciBacklogItemHdr itemHdr;
                sg packet;

                if (!sgSerializeCutFront(c->rxBacklog, &itemHdr, sizeof(itemHdr))) {
                    loge("backlog header dequque fail!\n");
                    goto fail;
                }
                packet = sgSplit(c->rxBacklog, itemHdr.len);
                if (!packet) {
                    loge("backlog packet dequeue fail\n");
                    goto fail;
                }
                sgSwap(packet, c->rxBacklog);
                if (!sgLength(c->rxBacklog)) {
                    sgFree(c->rxBacklog);
                    c->rxBacklog = NULL;
                }

                if (!hciRxAclForRunningConn(c, itemHdr.aclHdr, packet)) {
                    loge("backlog packet rx enqueue fail\n");
                    sgFree(packet);
                    goto fail;
                }
            }
            return;

        } else
            loge("Failed to enqueue upcall for conn up\n");
    }

fail:
    hciConnDisconnect(c->id);
}

/*
 * FUNCTION: hciConnAclUp
 * USE:      Setup a local ACL conn struct for this conn
 * PARAMS:   established - is it established?
 *           id - the id
 *           mac - the mac
 *           addrType - the address type for bt_addr struct
 *           encrypted - true if encrypted
 *           isMaster - am i master?
 * RETURN:   connection struct or NULL if error
 * NOTES:    call with mConnsLock held
 */
static struct hciAclConn* hciConnAclUp(bool established, uint16_t id, const uint8_t *mac, uint8_t addrType, bool encrypted, bool isMaster)
{
    bool isLE = addrType != BT_ADDR_TYPE_EDR;
    struct hciAclConn *c;
    struct bt_addr addr;

    memcpy(addr.addr, mac, sizeof(addr.addr));
    addr.type = addrType;

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

    if (!c) {
        c = hciConnAclStructNew(id, &addr, CONN_STATE_CFG, isMaster);
        if (!c) {
            loge("Failed to alloc conn struct for new ACL link\n");
            hciConnDisconnect(id);
            return NULL;
        }
        c->isMaster = isLE ? isMaster : false;
    } else if (c->state == CONN_STATE_WAIT) {
        c->state = CONN_STATE_CFG;
        if (isLE)
            c->isMaster = isMaster;
        /* TODO: start next LE connection/restart adv/etc? */
    } else {
        loge("Connection in state %d at up time\n", c->state);
    }
    c->encrypted = encrypted;

    return c;
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
        /* XXX: TODO: once we start advertising random addresses, this will need to evolve */
        memcpy(c->selfAddr.addr, mLocalAddr, sizeof(c->selfAddr.addr));
        c->selfAddr.type = BT_ADDR_TYPE_LE_PUBLIC;

        hciConnCfg(c, CONN_CFG_STEP_LE, 0);
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
        ret = hciScheduleConnEncrChange(c->handle, c->encrypted, c->mitmSafe
);
    }
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: hciHandleConnAclAuth
 * USE:      Notify all interested in authentication change
 * PARAMS:   cid - the connection id
 *           authOn - is auth on
 * RETURN:   true on success
 * NOTES:
 */
static bool hciHandleConnAclAuth(uint16_t cid, bool authOn)
{
    struct hciAclConn *c;
    bool ret = false;

    logd("cid %d now %sauthed\n", cid, authOn ? "" : "not ");

    pthread_mutex_lock(&mConnsLock);
    c = hciConnFindById(cid);
    if (c) {
        logd("cid %d was %sauthed\n", cid, c->mitmSafe ? "" : "not ");
        c->mitmSafe = authOn;
        ret = hciScheduleConnEncrChange(c->handle, c->encrypted, c->mitmSafe);
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

