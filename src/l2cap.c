//#define L2C_STATE_OPEN       0 /* dataPtr points to l2c_handle_t, len has mtu */    - make sure len has mtu when we call it
//todo: idea: sending thread get blocke don actual sending code which is blocke don hci buffer. l2cap layer itself ahs a send buffer, and a send thread whcih is also blocked. l2c's send data always higher priority than all other, so as long as it exists, all other send data waits...
//TODO: audit lock sequence everywhere...
//todo: peer & i both open psm connection ot each other @ same time. discuss...
//todo: do we close fixedch and psm services properly?
//todo: stateCbk for peer-initiated connections is null. this should work. will it?
//todo: close acl link when last psm link down, close acl link when last attempted psm link is cancelled, etc...
//todo: timers/timouts for unspecced BT waits (config, etc)
//todo: set flush timout at hci layer to 0xffff for a reliable link
//todo: there are states where instance,stateCbk,userData are null. we shoudl care for them...
//todo: at acl link up send info req for connectionless mtu just so we know it...
//todo: check all lengths followed, all log levels correct

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "util.h"
#include "log.h"
#include "hci.h"


/* constants from spec */
#define L2C_EXT_FTRS_SUPPORTED     0x180 /* support fixedCh & unicast data Rx */
#define L2C_FIXED_CHS_SUPPORTED    0x6   /* as mandated by the spec */
#define L2C_MIN_CONNECTIONLESS_MTU 48

/* connection bit to indicate in-progress connection */
#define L2C_CONN_IN_PROGRESS     (1 + HCI_CONN_ID_MASK)

/* also, the first non-fixed channel */
#define L2C_NUM_FIXED_CHANNELS   64

/* PSM length minimum as per spec */
#define L2C_PSM_MIN_LEN          2

/* L2CAP channel defines as per spec */
#define L2C_CH_SIGNALLING_EDR    0x0001
#define L2C_CH_CONNECTIONLESS    0x0002
#define L2C_CH_SIGNALLING_LE     0x0005


typedef uint64_t uniq_t;

/* L2CAP signalling packet codes and respective formats */
#define L2C_SIG_CMD_REJ           0x01
struct l2cSigCmdRej {
    uint16_t reason;
    /* optional data */
} __packed;
#define L2C_SIG_CMD_NOT_UNDERSTOD 0x0001
/* no data */
#define L2C_SIG_INVALID_CID       0x0003
struct l2cSigInvalidCid {
    uint16_t rxedDcid;
    uint16_t rxedScid;
} __packed;

#define L2C_SIG_CONN_REQ          0x02
struct l2cSigConnReq {
    psm_t    psm;
    uint16_t scid;
} __packed;

#define L2C_SIG_CONN_RSP          0x03
struct l2cSigConnRsp {
    uint16_t dcid;
    uint16_t scid;
    uint16_t result;
    uint16_t status;
} __packed;
#define L2C_SIG_CONN_RSP_OK       0x0000
#define L2C_SIG_CONN_RSP_PEND     0x0001
#define L2C_SIG_CONN_RSP_BAD_PSM  0x0002
#define L2C_SIG_CONN_RSP_BAD_SEC  0x0003
#define L2C_SIG_CONN_RSP_NO_RSRC  0x0004

#define L2C_SIG_CONF_REQ          0x04
struct l2cSigConfReq {
    uint16_t dcid;
    uint16_t flags;
    /* optional data */
} __packed;

#define L2C_CONF_MTU              0x01 /* payload: uint16 mtu */
#define L2C_CONF_FLUSH_TIMEOUT    0x02 /* payload: uint16 timeout */
#define L2C_CONF_QOS              0x03 /* 22 bytes of payload, beginning with uint8 SBZ, uint8 type (=1 for us) */
#define L2C_CONF_RETR_AND_FLOW    0x04 /* 9 bytes of payload, beginnign with mode (=0 for us) */
#define L2C_CONF_FCS              0x05 /* payload: uint8_t fcsType */
#define L2C_CONF_EXT_FLOW         0x06 /* 16 bytes of payload, beginning with uint8 id(=1) and svc(=1) */

struct l2cConfQosOpts {
    uint8_t flags;
    uint8_t serviceType;
    uint32_t tokenRate;
    uint32_t tockenBucketSz;
    uint32_t peakBandwidth;
    uint32_t latency;
    uint32_t delayVariation;
} __packed;
#define L2C_QOS_SVC_TYPE_BEST_EFFORT 0x01
#define L2C_QOS_SVC_TYPE_GUARANTEED  0x02

struct l2cConfRetrOpts {
    uint8_t mode;
    uint8_t txWindowSize;
    uint8_t maxTransmit;
    uint16_t retrTimeout;
    uint16_t monitorTimeout;
    uint16_t maxPduSz;
} __packed;
#define L2C_RETR_MODE_BASIC          0x00



#define L2C_SIG_CONF_RSP          0x05
struct l2cSigConfRsp {
    uint16_t scid;
    uint16_t flags;
    uint16_t result;
    /* optional data */
} __packed;
#define L2C_SIG_CONF_RSP_OK            0x0000
#define L2C_SIG_CONF_RSP_UNACCEPTABLE  0x0001
#define L2C_SIG_CONF_RSP_REJECTED      0x0002
#define L2C_SIG_CONF_RSP_UNKNOWN_OPTN  0x0003
#define L2C_SIG_CONF_RSP_PENDING       0x0004
#define L2C_SIG_CONF_RSP_FLOW_SPEC_REJ 0x0005

#define L2C_SIG_CONF_FLAG_C            0x0001

#define L2C_SIG_DISC_REQ          0x06
struct l2cSigDiscReq {
    uint16_t dcid;
    uint16_t scid;
} __packed;

#define L2C_SIG_DISC_RSP          0x07
struct l2cSigDiscRsp {
    uint16_t dcid;
    uint16_t scid;
} __packed;

#define L2C_SIG_ECHO_REQ          0x08
/* all data is optional */

#define L2C_SIG_ECHO_RSP          0x09
/* all data is optional */

#define L2C_SIG_INFO_REQ          0x0A
struct l2cSigInfoReq {
    uint16_t infoType;
} __packed;

#define L2C_SIG_INFO_TYPE_UNCONN_MTU 0x0001
#define L2C_SIG_INFO_TYPE_EXT_FTRS   0x0002
#define L2C_SIG_INFO_TYPE_FIXED_CHS  0x0003

#define L2C_SIG_INFO_RSP          0x0B
struct l2cSigInfoRsp {
    uint16_t infoType;
    uint16_t result;
    /* optional data */
} __packed;

#define L2C_SIG_INFO_RESULT_OK     0x0000
#define L2C_SIG_INFO_RESULT_NO     0x0001


#define L2C_SIG_CH_CREAT_REQ      0x0C
struct l2cSigChCreatReq {
    psm_t    psm;
    uint16_t scid;
    uint8_t  controller;
} __packed;

#define L2C_SIG_CH_CREAT_RSP      0x0D
struct l2cSigChCreatRsp {
    uint16_t dcid;
    uint16_t scid;
    uint16_t result;
    uint16_t status;
} __packed;

#define L2C_SIG_CH_MOVE_REQ       0x0E
struct l2cSigChMoveReq {
    uint16_t icid;
    uint8_t  controller;
} __packed;

#define L2C_SIG_CH_MOVE_RSP       0x0F
struct l2cSigChMoveRsp {
    uint16_t icid;
    uint16_t result;
} __packed;
#define L2C_SIG_CH_MOVE_CNF_REQ   0x10
struct l2cSigChMoveCnfReq {
    uint16_t icid;
    uint16_t result;
} __packed;

#define L2C_SIG_CH_MOVE_CNF_RSP   0x11
struct l2cSigChMoveCnfRsp {
    uint16_t icid;
} __packed;

#define L2C_SIG_CONN_UPDT_REQ     0x12
struct l2cSigConnUpdtReq {
    uint16_t intMin;
    uint16_t intMax;
    uint16_t latency;
    uint16_t timeoutMult;
} __packed;

#define L2C_SIG_CONN_UPDT_RSP     0x13
struct l2cSigConnUpdtRsp {
    uint16_t result;
} __packed;




/* low level L2CAP frame */
struct l2cFrm {
    uint16_t len;
    uint16_t cid;
} __packed;

/* L2CAP signalling channel frame */
struct l2cSigFrm {
    uint8_t  code;
    uint8_t  ident;
    uint16_t len;
} __packed;

/* our per-ACL-connection structure */
struct l2cAclConn {
    struct l2cAclConn *next;
    struct l2cAclConn *prev;
    uniq_t             uniq;
    uint8_t            ident; /* next ident to use */
    uint16_t           nextChan; /* a guess to a currently-free channel */
    uint16_t           conn;
    uint8_t            connectionlessMtu;
    struct bt_addr     addr;
};



/* our work struct for service worker theread */
struct l2cSvcWork {
    int type;
    void* reqID;
    union{
        psm_t psm;
        uint16_t chan;
    };
    union {
        struct {
            l2c_handle_t handle;
        } i_alloc;
        struct {
            l2c_handle_t handle;
            void* data;
            uint32_t size;
        } cl_rx;
        struct {
            void (*doneCbk)(void*);
            void *cbkData;
        } unreg;
        struct {
            l2cStateCbk stateCbk;
            void *userData;
            void *instance;
            uint8_t state;
            const void *data;
            uint32_t len;
        } state;
    };
};
#define L2C_SVC_WORK_PSM_INST_ALLOC          0
#define L2C_SVC_WORK_PSM_CONNECTIONLESS_RX   1
#define L2C_SVC_WORK_PSM_UNREGISTER          2 /* does not call service itself */
#define L2C_SVC_WORK_FIXEDCH_INST_ALLOC      3
#define L2C_SVC_WORK_FIXEDCH_UNREGISTER      4 /* does not call service itself */
#define L2C_SVC_WORK_STATE_CBK               5

/* our per-service struct for PSM-based services */
struct l2cPsmSvc {
    struct l2cPsmSvc *next;
    struct l2cPsmSvc *prev;
    psm_t psm;
    struct l2cServicePsmDescriptor desc;
    uint32_t beingRemoved : 1;
};

/* our per-service struct for PSM-based services */
struct l2cFixedChSvc {
    struct l2cFixedChSvc *next;
    struct l2cFixedChSvc *prev;
    uint16_t chan;
    struct l2cServiceFixedChDescriptor desc;
    uint32_t beingRemoved : 1;
};


/* our per-l2c connection struct */
/* also used for fixed ch "connections" */
struct l2cConn {
    struct l2cConn    *next;
    struct l2cConn    *prev;
    uint8_t            state;
    union {            /* per-state info */
        struct {       /*  config */
            uint8_t cfgTxState;
            uint8_t cfgRxState;
        };
    };
    uint8_t            ident; /* for control channel ops */
    l2cStateCbk        stateCbk;
    void              *userData;
    void              *instance;
    uniq_t             acl; /* the acl connection */
    psm_t              psm; /* 0 for fixedCh */
    uint16_t           localCh;
    uint16_t           remCh;
    uint16_t           mtu; /* for other side */
};
#define L2C_CONN_STATE_CONN_WAIT       0 /* waiting for ACL link up */
#define L2C_CONN_STATE_RX_OPEN_SVC     1 /* call to service in pipeline */
#define L2C_CONN_STATE_TX_OPEN_TXED    2 /* request sent to other side */
#define L2C_CONN_STATE_CFG             3 /* configuration ongoing */
#define L2C_CONN_STATE_ESTABLISHED     4 /* connected */
#define L2C_CONN_STATE_DIE_REQD        5 /* we closed, call to svc in pipeline */
#define L2C_CONN_STATE_DIE_TXED        6 /* we closed, request sent to peer */
#define L2C_CONN_STATE_DIE_RXED        7 /* other side closed, call to svc in pipeline */
#define L2C_CONN_STATE_TEARDOWN        8 /* tearing down */

#define L2C_CFG_STATE_INITIAL          0
#define L2C_CFG_STATE_REQ_SENT         1
#define L2C_CFG_STATE_CFG_ACCEPTED     2


/* some fwd declarations */
static uint8_t l2cSigSendErrCmdNotUnderstood(struct l2cAclConn *aclConn, uint8_t ident);
static uint8_t l2cSigSendErrInvalidCid(struct l2cAclConn *aclConn, uint8_t ident, uint16_t rxedDcid, uint16_t rxedScid);
static uint16_t l2cAclFreeCid(struct l2cAclConn *aclConn);
static uint8_t l2cSigSendConnectionRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh, uint16_t result, uint16_t status);
static struct l2cConn* l2cFindConnByHandle(l2c_handle_t handle);
static void l2cConnStructDelete(struct l2cConn *conn);
static void l2cPsmSendConfig(struct l2cAclConn *aclConn, struct l2cConn *conn);
static uint8_t l2cSigSendConfigReq(struct l2cAclConn *aclConn, uint16_t remCh, uint16_t flags, const void *cfgData, uint32_t cfgLen);
static uint8_t l2cSigSendConfigRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t flags, uint16_t result, const void *cfgData, uint32_t cfgLen);
static uint8_t l2cSigSendInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, const void *infoData, uint32_t infoLen);
static uint8_t l2cSigSendEchoRsp(struct l2cAclConn *aclConn, uint8_t ident, const void *echoData, uint32_t echoLen);

/* our state */
static pthread_mutex_t mUniqLock = PTHREAD_MUTEX_INITIALIZER;
static uniq_t mUniq = 1;

static pthread_t mSvcWorker;
static struct workQueue *mServiceWork = NULL;

static pthread_mutex_t mSvcsLock = PTHREAD_MUTEX_INITIALIZER;
static struct l2cPsmSvc *mPsmSvcs = NULL;
static struct l2cFixedChSvc *mFixedSvcs = NULL;
static psm_t nextPsm = 0x1001;

static pthread_mutex_t mConnsLock = PTHREAD_MUTEX_INITIALIZER;
static struct l2cAclConn *mAclConns = NULL;
static struct l2cConn *mConns = NULL;





/*
 * FUNCTION: l2cUniqNext
 * USE:      Produce the next "forever"-unique number
 * PARAMS:   NONE
 * RETURN:   the next "unique" number
 * NOTES:
 */
static uniq_t l2cUniqNext(void)
{
    uniq_t ret;
    pthread_mutex_lock(&mUniqLock);
    ret = mUniq++;
    pthread_mutex_unlock(&mUniqLock);
    return ret;
}

/*
 * FUNCTION: l2cAclConnFindById
 * USE:      Find a l2cConn structure for a given ACL connection ID
 * PARAMS:   aclConn - the ACL connection ID
 * RETURN:   the found struct or null
 * NOTES:    requires mConnsLock held
 */
static struct l2cAclConn* l2cAclConnFindById(uint16_t aclConn)
{
    struct l2cAclConn *c = mAclConns;

    while (c && c->conn != aclConn)
        c = c->next;

    return c;
}

/*
 * FUNCTION: l2cAclConnFindByUniq
 * USE:      Find a l2cConn structure for a given a UniqID
 * PARAMS:   uniq - the UniqID
 * RETURN:   the found struct or null
 * NOTES:    requires mConnsLock held
 */
static struct l2cAclConn* l2cAclConnFindByUniq(uniq_t uniq)
{
    struct l2cAclConn *c = mAclConns;

    while (c && c->uniq != uniq)
        c = c->next;

    return c;
}

/*
 * FUNCTION: l2cAclConnFindByAddr
 * USE:      Find a l2cConn structure for a given a peer addr
 * PARAMS:   addr - the addr
 * RETURN:   the found struct or null
 * NOTES:    requires mConnsLock held
 */
static struct l2cAclConn* l2cAclConnFindByAddr(const struct bt_addr* addr)
{
    struct l2cAclConn *c = mAclConns;

    while (c && memcmp(&c->addr, addr, sizeof(struct bt_addr)));
        c = c->next;

    return c;
}

/*
 * FUNCTION: l2cPsmValid
 * USE:      Check a PSM for validity
 * PARAMS:   psm - the PSM
 * RETURN:   true if valid, else false
 * NOTES:    we only support PSMs of 16-bit size
 */
static bool l2cPsmValid(psm_t psm)
{
    /* PSMs are odd */
    if (!(psm & 1))
        return false;

    /* LSB of top byte shall be zero */
    if (psm & 0x0100)
        return false;

    return true;
}

/*
 * FUNCTION: l2cHandleCreate
 * USE:      Create an externally-visible handle that can be used to identify an
 *           L2CAP connection/connectionless pipe
 * PARAMS:   aclConn - the acl connection ID
 *           localCh - local channel number
 *           remCh - remote channel number
 *           psm - the PSM in use
 * RETURN:   the handle
 * NOTES:
 */
static l2c_handle_t l2cHandleCreate(uint16_t aclConn, uint16_t localCh, uint16_t remCh, psm_t psm)
{
    l2c_handle_t ret = aclConn;

    ret = (ret << 16) + localCh;
    ret = (ret << 16) + remCh;
    ret = (ret << 16) + psm;

    return ret;
}

/*
 * FUNCTION: l2cHandleDecode
 * USE:      Decode an externally-visible handle to parts that help us identify a
 *           connection/connectionless pipe
 * PARAMS:   handle - the handle
 *           aclConnP - store here the acl connection ID
 *           localChP - store here the local channel number
 *           remChP - store here the remote channel number
 *           psmP - store here the PSM in use
 * RETURN:   NONE
 * NOTES:
 */
static void l2cHandleDecode(l2c_handle_t handle, uint16_t *aclConnP, uint16_t *localChP, uint16_t *remChP, psm_t *psmP)
{
    if (psmP)
        *psmP = handle & 0xFFFF;
    handle >>= 16;

    if(remChP)
        *remChP = handle & 0xFFFF;
    handle >>= 16;

    if(localChP)
        *localChP = handle & 0xFFFF;
    handle >>= 16;

    if(aclConnP)
        *aclConnP = handle & 0xFFFF;
}

/*
 * FUNCTION: l2cSvcWorkAlloc
 * USE:      Allocate a work item for the service worker
 * PARAMS:   type - the type of work
 *           psm - the psm this pertains to (or 0 is chan is used)
 *           chan - the channel this pertains to (or 0 if psm is used)
 *           reqID - the request ID used to signal completion
 * RETURN:   work item or null
 * NOTES:
 */
static struct l2cSvcWork* l2cSvcWorkAlloc(int type, psm_t psm, uint16_t chan, void* reqID)
{
    struct l2cSvcWork* w = (struct l2cSvcWork*)malloc(sizeof(struct l2cSvcWork));

    if (w) {
        w->type = type;
        w->reqID = reqID;
        if (psm)
            w->psm = psm;
        else if (chan)
            w->chan = chan;
        else {
            w->psm = 0;
            w->chan = 0;
        }
    } else
        loge("Failed to allocate service work item\n");

    return w;
}

/*
 * FUNCTION: l2cSvcWorkSchedulePsmAlloc
 * USE:      Schedule a service allocation
 * PARAMS:   psm - the psm which this is for
 *           reqID - request ID to signal completion
 *           handle - the handle to pass to the service alloc handler
 * RETURN:   NONE
 * NOTES:    actual call will happen in the worker thread
 */
static bool l2cSvcWorkSchedulePsmAlloc(psm_t psm, void* reqID, l2c_handle_t handle)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_SVC_WORK_PSM_INST_ALLOC, psm, 0, reqID);

    if (!w)
        return false;

    w->i_alloc.handle = handle;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service instance allocation!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkSchedulePsmConnectionlessRx
 * USE:      Schedule a service data RX for connectionless data
 * PARAMS:   psm - the psm which this is for
 *           reqID - request ID to signal completion
 *           handle - the handle to pass to the service RX connectionless handler
 *           data - the data buffer (a copy will be made)
 *           size - length of the data buffer
 * RETURN:   NONE
 * NOTES:    actual call will happen in the worker thread. Buffer is copied
 */
static bool l2cSvcWorkSchedulePsmConnectionlessRx(psm_t psm, void* reqID, l2c_handle_t handle, const void* data, uint32_t size)
{
    void* copy;
    struct l2cSvcWork* w;


    copy = malloc(size);
    if (!copy) {
        loge("Failed to allocate service work data copy for service connectionless RX!\n");
        return false;
    }

    w = l2cSvcWorkAlloc(L2C_SVC_WORK_PSM_CONNECTIONLESS_RX, psm, 0, reqID);
    if (!w) {
        free(copy);
        return false;
    }

    memcpy(copy, data, size);

    w->cl_rx.handle = handle;
    w->cl_rx.data = copy;
    w->cl_rx.size = size;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service connectionless RX!\n");
    free(copy);
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleFixedChAlloc
 * USE:      Schedule a service allocation
 * PARAMS:   chan - the channel this is for
 *           reqID - request ID to signal completion
 *           handle - the handle to pass to the service alloc handler
 * RETURN:   NONE
 * NOTES:    actual call will happen in the worker thread
 */
static bool l2cSvcWorkScheduleFixedChAlloc(uint16_t chan, void* reqID, l2c_handle_t handle)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_SVC_WORK_FIXEDCH_INST_ALLOC, 0, chan, reqID);

    if (!w)
        return false;

    w->i_alloc.handle = handle;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service instance allocation!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleFixedChUnregister
 * USE:      Schedule a service unregistering
 * PARAMS:   chan - the channel this is for
 *           reqID - request ID to signal completion
 *           doneCbk - callback to call when done (or NULL)
 *           cbkData - data to pass to callback
 * RETURN:   NONE
 * NOTES:    actual work will happen in the worker thread
 */
static bool l2cSvcWorkScheduleFixedChUnregister(uint16_t chan, void* reqID, void (*doneCbk)(void*), void *cbkData)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_SVC_WORK_FIXEDCH_UNREGISTER, 0, chan, reqID);

    if (!w)
        return false;

    w->unreg.doneCbk = doneCbk;
    w->unreg.cbkData = cbkData;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service unregistering!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkSchedulePsmUnregister
 * USE:      Schedule a service unregistering
 * PARAMS:   psm - the psm which this is for
 *           reqID - request ID to signal completion
 *           doneCbk - callback to call when done (or NULL)
 *           cbkData - data to pass to callback
 * RETURN:   NONE
 * NOTES:    actual work will happen in the worker thread
 */
static bool l2cSvcWorkSchedulePsmUnregister(psm_t psm, void* reqID, void (*doneCbk)(void*), void *cbkData)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_SVC_WORK_PSM_UNREGISTER, psm, 0, reqID);

    if (!w)
        return false;

    w->unreg.doneCbk = doneCbk;
    w->unreg.cbkData = cbkData;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service unregistering!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleStateCallDirect
 * USE:      Schedule a "state" callback with directly-passed params
 * PARAMS:   reqID - request ID to signal completion
 *           stateCbk - the callback to call
 *           userData - data pointer to pass to the callback
 *           instance - second data pointer to pass to the callback
 *           state - state vaklue to pass to the callback
 *           data - buffer a copy of which is passed to the callback
 *           len - the length of the buffer
 * RETURN:   NONE
 * NOTES:    actual call will happen in the worker thread. Buffer is copied
 */
static bool l2cSvcWorkScheduleStateCallDirect(void* reqID, l2cStateCbk stateCbk, void *userData, void *instance, uint8_t state, const void *data, uint32_t len)
{
    struct l2cSvcWork* w;
    void* copy;

    copy = malloc(len);
    if (!copy) {
        loge("Failed to allocate service work data copy for state call!\n");
        return false;
    }

    memcpy(copy, data, len);


    w = l2cSvcWorkAlloc(L2C_SVC_WORK_STATE_CBK, 0, 0, reqID);
    if (!w) {
        free(copy);
        return false;
    }

    w->state.stateCbk = stateCbk;
    w->state.userData = userData;
    w->state.instance = instance;
    w->state.state = state;
    w->state.data = data;
    w->state.len = len;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for state call!\n");
    free(copy);
    free(w);
    return false;
}


/*
 * FUNCTION: l2cSvcWorkScheduleStateCall
 * USE:      Schedule a "state" callback for a connection
 * PARAMS:   reqID - request ID to signal completion
 *           conn - the connection
 *           state - state vaklue to pass to the callback
 *           data - buffer a copy of which is passed to the callback
 *           len - the length of the buffer
 * RETURN:   NONE
 * NOTES:    actual call will happen in the worker thread. Buffer is copied
 */
static bool l2cSvcWorkScheduleStateCall(void* reqID, struct l2cConn* conn, uint8_t state, const void *data, uint32_t len)
{
    return l2cSvcWorkScheduleStateCallDirect(reqID, conn->stateCbk, conn->userData, conn->instance, state, data, len);
}

/*
 * FUNCTION: l2cHandleConnReq
 * USE:      Process request to open a PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           ident - the ident to use for response
 *           psm - the requested psm
 *           remCh - the remote channel used to make the request
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConnReq(struct l2cAclConn *aclConn, uint8_t ident, psm_t psm, uint16_t remCh)
{
    bool invalidPsm = false;
    struct l2cPsmSvc *svc = NULL;
    struct l2cConn *conn = NULL;
    uint16_t localCh = 0;
    bool enqueued = false;
    uint16_t result;

    if (!l2cPsmValid(psm))
        goto out_nolocks;

    pthread_mutex_lock(&mSvcsLock);
    svc = mPsmSvcs;
    while (svc && (svc->psm != psm || svc->beingRemoved || !svc->desc.serviceInstanceAlloc))
        svc = svc->next;
    if (!svc)
        goto out_svcs_lock;

    conn = (struct l2cConn*)malloc(sizeof(struct l2cConn));
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        goto out_svcs_lock;
    }
    conn->acl = aclConn->uniq;
    conn->psm = psm;
    conn->mtu = 0xFFFF;
    conn->instance = NULL;
    conn->stateCbk = NULL;
    conn->userData = NULL;
    conn->remCh = remCh;
    localCh = l2cAclFreeCid(aclConn);
    if (!localCh) {
        logw("No free channels for conn %d\n", aclConn->conn);
        free(conn);
        goto out_svcs_lock;
    }
    conn->prev = NULL;
    conn->localCh = localCh;
    conn->ident = ident;
    conn->state = L2C_CONN_STATE_RX_OPEN_SVC;

    /* safe to call this enqueue here , since worker thread cannot work on it till we release this lock */
    enqueued = l2cSvcWorkSchedulePsmAlloc(psm, NULL, l2cHandleCreate(aclConn->conn, localCh, remCh, psm));
    if (enqueued) {
        conn->next = mConns;
        mConns = conn;
    } else {
        free(conn);
    }

out_svcs_lock:
    pthread_mutex_unlock(&mSvcsLock);

out_nolocks:
    if (!svc) {
        result = L2C_SIG_CONN_RSP_BAD_PSM;
        logi("Request for connect to unknown pesm %d\n", psm);
    } else if (!conn) {
        result = L2C_SIG_CONN_RSP_NO_RSRC;
        logi("Couldnt allocate a connection struct for incoming psm %d open request\n", psm);
    } else if (!localCh) {
        result = L2C_SIG_CONN_RSP_NO_RSRC;
        logi("Couldnt find a local channel for incoming psm %d open request\n", psm);
    } else if (!enqueued) {
        result = L2C_SIG_CONN_RSP_NO_RSRC;
        logi("Couldnt enqueue a service alloc request for incoming psm %d open request\n", psm);
    } else {
        logd("Scheduled service open for psm %d\n", psm);
        return;
    }

    if (!l2cSigSendConnectionRsp(aclConn, ident, 0, remCh, result, 0))
        loge("Failed to sent negative connection request response frame\n");
}

/*
 * FUNCTION: l2cHandleConnRsp
 * USE:      Randle a response to a open PSM channel request we had sent
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           remCh - remote channel number
 *           localCh -local channel number
 *           result - the result the remote side sent
 *           status - the status from the remote side
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConnRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t localCh, uint16_t result, uint16_t status)
{
    struct l2cConn *conn = NULL;


    conn = mConns;
    while (conn && conn->localCh != localCh)
        conn = conn->next;

    if (!conn) {
        logw("Unexpected Connection Response id %d for ch %d %d with result %d\n", ident, localCh, remCh, result);
        return;
    }

    if (ident != conn->ident) {
        logi("RXed a Connection Response witrh an invalid ident %d (expected %d). Dropping\n", ident, conn->ident);
        return;
    }

    if (!conn->psm) {
        logw("Unexpected Connection Response for a FixedCh conn chan %d\n", localCh);
        return;
    }

    if (conn->state != L2C_CONN_STATE_TX_OPEN_TXED) {
        logw("Connection Response for connection in bad state %d %d for ch %d %d with result %d\n", conn->state, ident, localCh, remCh, result);
        //todo: what to do here?
        return;
    }

    if (!conn->stateCbk) {
        logw("Connection Response for connection we did not create\n");
        //todo: what to do here?
        return;
    }

    if (result == L2C_SIG_CONN_RSP_BAD_PSM || result == L2C_SIG_CONN_RSP_BAD_SEC || result == L2C_SIG_CONN_RSP_NO_RSRC) {
        logi("Other side refused our connection for psm %d\n", conn->psm);
        if (!l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    if (result == L2C_SIG_CONN_RSP_PEND) {

        /* TODO: when we support unencrypted links, this code block will need to evolve */

        loge("Unexpected 'pending' reply to open request - we are not expecting this on a secure link\n");
        if (!l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    if (result != L2C_SIG_CONN_RSP_OK) {

        loge("Unexpected result %d for connection to psm %d\n", result, conn->psm);
        if (!l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    conn->state = L2C_CONN_STATE_CFG;
    conn->cfgTxState = L2C_CFG_STATE_INITIAL;
    conn->cfgRxState = L2C_CFG_STATE_INITIAL;
    conn->remCh = remCh;
    conn->mtu = 0xFFFF;

    l2cPsmSendConfig(aclConn, conn);
}

/*
 * FUNCTION: l2cPsmSendConfig
 * USE:      RStart a config negotiation with a peer for a new PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           conn - the l2c connection struct
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held, when *first* entering config state
 */
static void l2cPsmSendConfig(struct l2cAclConn *aclConn, struct l2cConn *conn)
{
    /* for now our hardwired config is a large MTU */
    uint8_t ident;
    static const uint8_t config[] = {
        L2C_CONF_MTU, 0x02, OUR_MTU & 0xFF, OUR_MTU >> 8
    };

    if (conn->cfgTxState != L2C_CFG_STATE_INITIAL) {

        //todo: properly abort
        return;
    }

    ident = l2cSigSendConfigReq(aclConn, conn->remCh, 0, config, sizeof(config));
    if (ident) {
        conn->ident = ident;
        conn->cfgTxState = L2C_CFG_STATE_REQ_SENT;
    } else {
        loge("Failed to send config request on conn for chs %d %d on acl %d\n", conn->localCh, conn->remCh, aclConn->conn);
        if (!l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
    }
}

/*
 * FUNCTION: l2cChannelReadyForData
 * USE:      Called when a channel is ready for data to flow
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           conn - the connection struct
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cChannelReadyForData(struct l2cAclConn *aclConn, struct l2cConn *conn)
{
    l2c_handle_t handle = l2cHandleCreate(aclConn->conn, conn->localCh, conn->remCh, conn->psm);
    uint32_t mtu = conn->mtu;

    l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_OPEN, &handle, sizeof(handle));
    if (conn->psm)
        l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_MTU, &mtu, sizeof(mtu));

    conn->state = L2C_CONN_STATE_ESTABLISHED;
}

/*
 * FUNCTION: l2cHandleConfReq
 * USE:      Handle a configuration request for a PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           localCh -local channel number
 *           flags - the flags the remote side sent
 *           data - the data from the remote side
 *           len - the length of the abovementioned data
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConfReq(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t flags, const void *dataP, uint16_t len)
{
    struct l2cConn *conn = NULL;
    uint8_t *reply, *end;
    const uint8_t *data = (const uint8_t*)dataP;
    uint16_t result = L2C_SIG_CONF_RSP_OK;

    conn = mConns;
    while (conn && conn->localCh != localCh)
        conn = conn->next;

    if (!conn) {
        logw("Unexpected Configuration Request id %d for ch %d with result %d\n", ident, localCh, result);
        return;
    }

    if (!conn->psm) {
        logw("Unexpected Configuration Request for a FixedCh conn chan %d\n", localCh);
        return;
    }

    if (conn->state != L2C_CONN_STATE_CFG) {
        logw("Configuration Request for connection in bad state %d for ch %d with flags %d\n", conn->state, localCh, flags);
        //todo: what to do here?
        return;
    }

    if (conn->cfgRxState != L2C_CFG_STATE_INITIAL)
        loge("Unexpected internal inconsistency on conf req: expected %d saw %d\n", L2C_CFG_STATE_INITIAL, conn->cfgRxState);

    /* go through every option and amkeup a reply. Note: no reply is longer than request, so this allocation works */
    end = reply = (uint8_t*)malloc(len);
    if (!reply) {
        loge("Cannot allocate a conf reply\n");
        //todo: drop it? send error?
        return;
    }

    while (len > 2) {
        uint8_t optType = utilGetLE8(data++);
        uint8_t optLen = utilGetLE8(data++);
        const uint8_t *opt;

        len -= 2;

        if (optLen > len) {
            logw("Got invalid conf req option len %d/%d\n", optLen, len);
            //todo: send comamdn not understood
            break;
        }
        opt = data;
        data += optLen;
        len -= optLen;

        switch (optType){
        case L2C_CONF_MTU:
            if (optLen != sizeof(uint16_t)) {
                logi("other side sent bad sized mtu conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                conn->mtu = utilGetLE16(opt);
                utilSetLE8(end + 0, L2C_CONF_MTU);
                utilSetLE8(end + 1, sizeof(uint16_t));
                utilSetLE16(end + 2, conn->mtu);
                end += 4;
            }
            break;
        case L2C_CONF_FLUSH_TIMEOUT:
            /* we accept whatever the other side says here */
            if (optLen != sizeof(uint16_t)) {
                logi("other side sent bad sized flush to conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                utilSetLE8(end + 0, L2C_CONF_FLUSH_TIMEOUT);
                utilSetLE8(end + 1, sizeof(uint16_t));
                utilSetLE16(end + 2, utilGetLE16(opt));
                end += 4;
            }
            break;
/*
        case L2C_CONF_QOS:
            if (optLen != sizeof(struct l2cConfQosOpts)) {
                logi("other side sent bad sized qos to conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                struct l2cConfQosOpts *qosOut;

                // copy over all data first
                utilSetLE8(end + 0, L2C_CONF_QOS);
                utilSetLE8(end + 1, sizeof(struct l2cConfQosOpts));
                end += 2;
                qosOut = (struct l2cConfQosOpts*)end;
                end += sizeof(struct l2cConfQosOpts);
                memcpy(qosOut, opt, sizeof(struct l2cConfQosOpts));

                // change any "required"s to "best effort"s
                if (utilGetLE8(&qosOut->serviceType) == L2C_QOS_SVC_TYPE_GUARANTEED)
                    utilSetLE8(&qosOut->serviceType, L2C_QOS_SVC_TYPE_BEST_EFFORT);
            }
            break;
        case L2C_CONF_RETR_AND_FLOW:
            // we do not care what they ask for, we send back basic mode only
            if (optLen != sizeof(struct l2cConfRetrOpts)) {
                logi("other side sent bad sized retr to conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                struct l2cConfRetrOpts *retrOut;

                // copy over all data first
                utilSetLE8(end + 0, L2C_CONF_RETR_AND_FLOW);
                utilSetLE8(end + 1, sizeof(struct l2cConfRetrOpts));
                end += 2;
                retrOut = (struct l2cConfRetrOpts*)end;
                end += sizeof(struct l2cConfRetrOpts);
                memset(retrOut, 0, sizeof(struct l2cConfRetrOpts));
                utilSetLE8(&retrOut->mode, L2C_RETR_MODE_BASIC);
            }
            break;
        case L2C_CONF_FCS:
            // we do not care what they ask for, we send back "no FCS" only
            if (optLen != sizeof(suint8_t)) {
                logi("other side sent bad sized fcs to conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                struct l2cConfRetrOpts *retrOut;
                utilSetLE8(end + 0, L2C_CONF_FCS);
                utilSetLE8(end + 1, sizeof(uint8_t));
                utilSetLE8(end + 2, 0);
                end += 3;
            }
            break;
*/
        default:
            logw("Unexpected config data received (t=%d l=%d) ignoring\n", optType, optLen);
            break;
        }
    }

    if (len) {
        free(reply);
        logw("Conf req not processed: %d\n", len);
        //todo: send command not understood
        return;
    }

    if (!l2cSigSendConfigRsp(aclConn, ident, conn->remCh, flags, result, reply, end - reply))
        loge("Failed to sent configuration request response frame\n");
    free(reply);

    if (flags & L2C_SIG_CONF_FLAG_C)
        logi("Conf Rsp continues\n");
    else {
        conn->cfgTxState = L2C_CFG_STATE_CFG_ACCEPTED;
        if (conn->cfgRxState)
            l2cChannelReadyForData(aclConn, conn);
    }
}

/*
 * FUNCTION: l2cHandleConfRsp
 * USE:      Handle a configuration request for a PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           localCh -local channel number
 *           flags - the flags the remote side sent
 *           result - the result the remote side sent
 *           data - the data from the remote side
 *           len - the length of the abovementioned data
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConfRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t flags, uint16_t result, const void *data, uint16_t len)
{
    struct l2cConn *conn = NULL;

    conn = mConns;
    while (conn && conn->localCh != localCh)
        conn = conn->next;

    if (!conn) {
        logw("Unexpected Configuration Response id %d for ch %d with result %d\n", ident, localCh, result);
        return;
    }

    if (ident != conn->ident) {
        logi("RXed a Configuration Response witrh an invalid ident %d (expected %d). Dropping\n", ident, conn->ident);
        return;
    }

    if (!conn->psm) {
        logw("Unexpected Configuration Response for a FixedCh conn chan %d\n", localCh);
        return;
    }

    if (conn->state != L2C_CONN_STATE_CFG) {
        logw("Configuration Response for connection in bad state %d %d for ch %d with result %d\n", conn->state, ident, localCh, result);
        //todo: what to do here?
        return;
    }

    if (result == L2C_SIG_CONF_RSP_UNACCEPTABLE || result == L2C_SIG_CONF_RSP_REJECTED ||
        result == L2C_SIG_CONF_RSP_UNKNOWN_OPTN || result == L2C_SIG_CONF_RSP_PENDING ||
        result == L2C_SIG_CONF_RSP_FLOW_SPEC_REJ) {

        logi("Other side refused our configuration for psm %d\n", conn->psm);
        //todo: close it properly
        return;
    }

    if (result != L2C_SIG_CONF_RSP_OK) {

        loge("Unexpected result %d for connection to psm %d\n", result, conn->psm);
        //todo: close it properly
        return;
    }

    if (conn->cfgTxState != L2C_CFG_STATE_REQ_SENT)
        loge("Unexpected internal inconsistency on conf rsp: expected %d saw %d\n", L2C_CFG_STATE_REQ_SENT, conn->cfgTxState);

    if (flags & L2C_SIG_CONF_FLAG_C)
        logi("Conf Rsp continues\n");
    else {
        conn->cfgTxState = L2C_CFG_STATE_CFG_ACCEPTED;
        if (conn->cfgRxState)
            l2cChannelReadyForData(aclConn, conn);
    }
}

/*
 * FUNCTION: l2cHandleInfoReq
 * USE:      Handle an incoming information request
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           infoType - the requested info type
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleInfoReq(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType)
{
    uint8_t reply[8];
    uint32_t infoLen = 0;
    uint16_t result = L2C_SIG_INFO_RESULT_OK;

    switch (infoType) {
    case L2C_SIG_INFO_TYPE_UNCONN_MTU:
        utilSetLE16(reply, OUR_MTU);
        infoLen = sizeof(uint16_t);
        break;
    case L2C_SIG_INFO_TYPE_EXT_FTRS:
        utilSetLE32(reply, L2C_EXT_FTRS_SUPPORTED);
        infoLen = sizeof(uint32_t);
        break;
    case L2C_SIG_INFO_TYPE_FIXED_CHS:
        utilSetLE64(reply, L2C_FIXED_CHS_SUPPORTED);
        infoLen = sizeof(uint64_t);
        break;
    default:
        result = L2C_SIG_INFO_RESULT_NO;
        break;
    }

    if (!l2cSigSendInfoRsp(aclConn, ident, infoType, result, reply, infoLen))
        loge("Failed to sent information request response frame\n");
}

/*
 * FUNCTION: l2cHandleInfoRsp
 * USE:      Handle an incoming information response
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           infoType - the requested info type
 *           result - the result of the request
 *           infoData - the returned data
 *           infoLen - the returned data's length
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, const void* infoData, uint32_t infoLen)
{
    uint16_t val;

    if (infoType != L2C_SIG_INFO_TYPE_UNCONN_MTU) {
        logi("Got unexpected info rsp with type %d\n", infoType);
        return;
    }

    if (result != L2C_SIG_INFO_RESULT_OK) {
        logi("Got unexpected info rsp error (result=%d)\n", result);
        return;
    }

    if (infoLen != sizeof(uint16_t)) {
        logi("Got unexpected info rsp size (infoLen=%d)\n", infoLen);
        return;
    }

    val = utilGetLE16(infoData);
    if (val < L2C_MIN_CONNECTIONLESS_MTU) {
        logi("Peer reports connectionless mtu %d for acl conn %d. correcting to %d\n", val, aclConn->conn, L2C_MIN_CONNECTIONLESS_MTU);
        val = L2C_MIN_CONNECTIONLESS_MTU;
    }

    aclConn->connectionlessMtu = val;
}

/*
 * FUNCTION: l2cHandleEchoReq
 * USE:      Handle an incoming echo request
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           echoData - the data we got
 *           echoLen - the len we got
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleEchoReq(struct l2cAclConn *aclConn, uint8_t ident, const void* echoData, uint32_t echoLen)
{
    logi("Got echo packet with %db of data\n", echoLen);

    /* no data in reply is valid as per spec */
    if (!l2cSigSendEchoRsp(aclConn, ident, NULL, 0))
        loge("Failed to sent echo response frame\n");
}

/*
 * FUNCTION: l2cSignallingRx
 * USE:      Process arriving data over signalling channels
 * PARAMS:   conn - the per-ACL-connection structure
 *           data - buffer with the data
 *           len - how much data there is
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:    call with mConnsLock held
 */
static void l2cSignallingRx(struct l2cAclConn *conn, const void *data, uint32_t datalen)
{
    struct l2cSigFrm *sig;

    while (datalen >= sizeof(struct l2cSigFrm)) {

        const uint8_t *paramsP;
        uint8_t code, ident;
        uint16_t len;

        sig = (struct l2cSigFrm*)data;
        paramsP = (const uint8_t*)(sig + 1);
        code = utilGetLE8(&sig->code);
        ident = utilGetLE8(&sig->ident);
        len = utilGetLE16(&sig->len);
        data = paramsP + len;
        datalen -= sizeof(struct l2cSigFrm) + len;

        if (!ident)
            logw("Got signalling ident 0. Dropping\n");
        else switch (code){
        case L2C_SIG_CMD_REJ: {
            struct l2cSigCmdRej *params = (struct l2cSigCmdRej*)paramsP;
            if (len < sizeof(*params))
                logw("Got a short Command Reject packet\n");
            else
                logw("Got a reject for id %d with reason %d\n", ident,
                    utilGetLE16(&params->reason));
            break;
        }
        case L2C_SIG_CONN_REQ: {
            struct l2cSigConnReq *params = (struct l2cSigConnReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Connection Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Connection Request packet\n");
            else {
                psm_t psm = utilGetLE16(&params->psm);
                uint16_t scid = utilGetLE16(&params->scid);

                l2cHandleConnReq(conn, ident, psm, scid);
            }
            break;
        }
        case L2C_SIG_CONN_RSP: {
            struct l2cSigConnRsp *params = (struct l2cSigConnRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Connection Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Connection Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params->dcid);
                uint16_t scid = utilGetLE16(&params->scid);
                uint16_t result = utilGetLE16(&params->result);
                uint16_t status = utilGetLE16(&params->status);

                l2cHandleConnRsp(conn, ident, dcid, scid, result, status);
            }
            break;
        }
        case L2C_SIG_CONF_REQ: {
            struct l2cSigConfReq *params = (struct l2cSigConfReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Configuration Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Configuration Request packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params->dcid);
                uint16_t flags = utilGetLE16(&params->flags);

                l2cHandleConfReq(conn, ident, dcid, flags,
                    (const void*)(params + 1), len - sizeof(*params));
            }
            break;
        }
        case L2C_SIG_CONF_RSP: {
            struct l2cSigConfRsp *params = (struct l2cSigConfRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Configuration Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Configuration Response packet\n");
            else {
                uint16_t scid = utilGetLE16(&params->scid);
                uint16_t flags = utilGetLE16(&params->flags);
                uint16_t result = utilGetLE16(&params->result);

                l2cHandleConfRsp(conn, ident, scid, flags, result,
                    (const void*)(params + 1), len - sizeof(*params));
            }
            break;
        }
        case L2C_SIG_DISC_REQ: {
            struct l2cSigDiscReq *params = (struct l2cSigDiscReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Disconnect Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Disconnect Request packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params->dcid);
                uint16_t scid = utilGetLE16(&params->scid);

                l2cHandleDiscReq(conn, ident, dcid, scid);
            }
            break;
        }
        case L2C_SIG_DISC_RSP: {
            struct l2cSigDiscRsp *params = (struct l2cSigDiscRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Disconnect Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Disconnect Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params->dcid);
                uint16_t scid = utilGetLE16(&params->scid);

                l2cHandleDiscRsp(conn, ident, dcid, scid);
            }
            break;
        }
        case L2C_SIG_ECHO_REQ: {
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Echo Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else
                l2cHandleEchoReq(conn, ident, paramsP, len);
            break;
        }
/*
        case L2C_SIG_ECHO_RSP: {
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Echo Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else
                l2cHandleEchoRsp(conn, ident, paramsP, len);
            break;
        }
*/
        case L2C_SIG_INFO_REQ: {
            struct l2cSigInfoReq *params = (struct l2cSigInfoReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Info Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Info Request packet\n");
            else {
                uint16_t infoType = utilGetLE16(&params->infoType);

                l2cHandleInfoReq(conn, ident, infoType);
            }
            break;
        }
        case L2C_SIG_INFO_RSP: {
            struct l2cSigInfoRsp *params = (struct l2cSigInfoRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Info Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Info Response packet\n");
            else {
                uint16_t infoType = utilGetLE16(&params->infoType);
                uint16_t result = utilGetLE16(&params->result);

                l2cHandleInfoRsp(conn, ident, infoType, result,
                    (const void*)(params + 1), len - sizeof(*params));
            }
            break;
        }
/*
        case L2C_SIG_CH_CREAT_REQ: {
            struct l2cSigChCreatReq *params = (struct l2cSigChCreatReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Create Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Create Request packet\n");
            else {
                psm_t psm = utilGetLE16(&params->psm);
                uint16_t scid = utilGetLE16(&params->scid);
                uint8_t controller = utilGetLE8(&params->controller);

                l2cHandleChCreatReq(conn, ident, psm, scid, controller);
            }
            break;
        }
        case L2C_SIG_CH_CREAT_RSP: {
            struct l2cSigChCreatRsp *params = (struct l2cSigChCreatRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Create Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Create Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params->dcid);
                uint16_t scid = utilGetLE16(&params->scid);
                uint16_t result = utilGetLE16(&params->result);
                uint16_t status = utilGetLE16(&params->status);

                l2cHandleChCreatRsp(conn, ident, dcid, scid, result, status);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_REQ: {
            struct l2cSigChMoveReq *params = (struct l2cSigChMoveReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Move Request packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Move Request packet\n");
            else {
                uint16_t icid = utilGetLE16(&params->icid);
                uint8_t controller = utilGetLE8(&params->controller);

                l2cHandleChMoveReq(conn, ident, icid, controller);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_RSP: {
            struct l2cSigChMoveRsp *params = (struct l2cSigChMoveRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Move Response packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Move Response packet\n");
            else {
                uint16_t icid = utilGetLE16(&params->icid);
                uint16_t result = utilGetLE8(&params->result);

                l2cHandleChMoveRsp(conn, ident, icid, result);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_CNF_REQ: {
            struct l2cSigChMoveCnfReq *params = (struct l2cSigChMoveCnfReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Move Confirmation packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Move Confirmation packet\n");
            else {
                uint16_t icid = utilGetLE16(&params->icid);
                uint16_t result = utilGetLE8(&params->result);

                l2cHandleChMoveCnfReq(conn, ident, icid, result);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_CNF_RSP: {
            struct l2cSigChMoveCnfRsp *params = (struct l2cSigChMoveCnfRsp*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Channel Move Confirmation Response "
                    "packet on a non-EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Channel Move Confirmation Response packet\n");
            else {
                uint16_t icid = utilGetLE16(&params->icid);

                l2cHandleChMoveCnfRsp(conn, ident, icid);
            }
            break;
        }
*/
        case L2C_SIG_CONN_UPDT_REQ: {
            struct l2cSigConnUpdtReq *params = (struct l2cSigConnUpdtReq*)paramsP;
            if (!BT_ADDR_IS_EDR(conn->addr)) {
                logw("Got a Connection Update Request packet on an EDR link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Connection Update Request packet\n");
            else {
                uint16_t intMin = utilGetLE16(&params->intMin);
                uint16_t intMax = utilGetLE16(&params->intMax);
                uint16_t latency = utilGetLE16(&params->latency);
                uint16_t timeoutMult = utilGetLE16(&params->timeoutMult);

                l2cHandleConnUpdtReq(conn, ident, intMin, intMax, latency, timeoutMult);
            }
            break;
        }
        case L2C_SIG_CONN_UPDT_RSP: {
            struct l2cSigConnUpdtRsp *params = (struct l2cSigConnUpdtRsp*)paramsP;
            if (!BT_ADDR_IS_LE(conn->addr)) {
                logw("Got a Connection Update Response packet on a non-LE link\n");
                l2cSigSendErrCmdNotUnderstood(conn, ident);
            } else if (len < sizeof(*params))
                logw("Got a short Connection Update Response packet\n");
            else {
                uint16_t result = utilGetLE8(&params->result);

                l2cHandleConnUpdtRsp(conn, ident, result);
            }
            break;
        }
        default: {
            logw("Got unknown code %d on signalling channel\n", code);
            l2cSigSendErrCmdNotUnderstood(conn, ident);
        }
        }

        if (!BT_ADDR_IS_EDR(conn->addr)) /* only EDR can put multiple commands here */
            break;
    }

    if (datalen)
        loge("Leftover %d bytes in signalling frame\n", datalen);
}

/*
 * FUNCTION: l2cFixedChRx
 * USE:      Called when given channel data arrives. Dispatches it
 *           to the right destination
 * PARAMS:   aclConn - the ACL connection
 *           cid - the channel data arrived on
 *           data - buffer with the data
 *           len - how much data there is
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cSendRxToChannel(struct l2cAclConn *aclConn, uint16_t chan, const void *data, uint32_t len)
{
    struct l2cConn *iter, *cur;
    bool ret;

    iter = mConns;
    while (iter) {
        cur = iter;
        iter = iter->next;

        if (cur->acl != aclConn->uniq) /* wrong ACL connection */
            continue;

        if (cur->localCh != chan) /* wrong channel */
            continue;

        if (cur->state != L2C_CONN_STATE_ESTABLISHED) { /* not open yet */
            logw("Got data for a not-yet-open channel\n");
            continue;
        }

        break;
    }

    return cur && l2cSvcWorkScheduleStateCall(NULL, cur, L2C_STATE_RX, data, len);
}

/*
 * FUNCTION: l2cAclDataRx
 * USE:      Called by lower layers when ACL data arrives
 * PARAMS:   aclConn - the connection ID
 *           data - buffer with the data
 *           len - how much data there is
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:
 */
void l2cAclDataRx(uint16_t aclConn, const void *data, uint32_t datalen)
{
    const struct l2cFrm *frm;
    uint16_t len, cid;
    struct l2cAclConn *conn;


    if (datalen < sizeof(struct l2cFrm)) {

        loge("Short L2CAP frame RXed: %db\n", datalen);
        return;
    }

    /* get the header */
    frm = (struct l2cFrm*)data;
    len = utilGetLE16(&frm->len);
    cid = utilGetLE16(&frm->cid);

    /* re-point data to point to the actual post-header data */
    data = (const void*)(frm + 1);
    datalen -= sizeof(struct l2cFrm);

    pthread_mutex_lock(&mConnsLock);
    conn = l2cAclConnFindById(aclConn);
    if (conn) {

        if (len != datalen) {
            logw("XXX: packet len=%d datalen=%d\n", len, datalen);
            if (len > datalen)
                len = datalen;
        }

        if (cid == L2C_CH_SIGNALLING_EDR) {
            if (!BT_ADDR_IS_EDR(conn->addr))
                loge("Received EDR signalling packet on non-EDR link. Dropping.\n");
            else
                l2cSignallingRx(conn, data, len);
        } else if (cid == L2C_CH_SIGNALLING_LE) {
            if (!BT_ADDR_IS_LE(conn->addr))
                loge("Received LE signalling packet on non-LE link. Dropping.\n");
            else
                l2cSignallingRx(conn, data, len);
        } else if (cid == L2C_CH_CONNECTIONLESS) {
            l2c_handle_t handle;
            psm_t psm;

            if (len < sizeof(psm_t))
                loge("Short connectionless L2CAP data RXed: %db\n", len);
            else {

                psm = utilGetLE16(data);
                data = ((uint16_t*)data) + 1;
                len -= sizeof(uint16_t);
                handle = l2cHandleCreate(aclConn, L2C_CH_CONNECTIONLESS, L2C_CH_CONNECTIONLESS, psm);
                if (!l2cSvcWorkSchedulePsmConnectionlessRx(psm, NULL, handle, data, len))
                    loge("Failed to schedule call to connectionless RX for psm %d\n", psm);
            }
        } else {
            if (!l2cSendRxToChannel(conn, cid, data, len)) {
                if (cid < L2C_NUM_FIXED_CHANNELS)
                    logw("Dropping RXed FixedCh data for cid %d\n", cid);
                else
                    logw("Dropping RXed PSM data for unknown cid %d\n", cid);
            }
        }
    } else {
        loge("Unable to find ACL connection %d. Dropping packet\n", aclConn);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cPsmSvcFindByPsm
 * USE:      Find a service structure by PSM
 * PARAMS:   psm - the psm
 * RETURN:   service structure or NULL if not found
 * NOTES:    Must hold the mSvcsLock lock while calling this
 */
static struct l2cPsmSvc* l2cPsmSvcFindByPsm(psm_t psm)
{
    struct l2cPsmSvc* svc = mPsmSvcs;

    while (svc && svc->psm != psm)
        svc = svc->next;

    return svc;
}
/*
 * FUNCTION: l2cFixedChSvcFindByChan
 * USE:      Find a service structure by fixed channel
 * PARAMS:   chan - the channel
 * RETURN:   service structure or NULL if not found
 * NOTES:    Must hold the mSvcsLock lock while calling this
 */
static struct l2cFixedChSvc* l2cFixedChSvcFindByChan(uint16_t chan)
{
    struct l2cFixedChSvc* svc = mFixedSvcs;

    while (svc && svc->chan != chan)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: l2cPsmSvcInstAllocDone
 * USE:      Called when a service call to allocate a PSM service instance finishes
 * PARAMS:   reqID - request ID originally passed to l2cSvcWorkSchedulePsmAlloc
 *           l2cStateCbk - the state cbk for this conn
 *           userData - param for state callbacks
 *           instance - the allocated instance or NULL
 *           handle - the handle param for the connection
 * RETURN:   NONE
 * NOTES:
 */
static void l2cPsmSvcInstAllocDone(void *reqID, l2cStateCbk stateCbk, void *userData, void *instance, l2c_handle_t handle)
{
    struct l2cConn* conn;
    struct l2cAclConn* aclConn;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cFindConnByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByUniq(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (!instance) {
        logi("Service allocations failed for psm %d\n", conn->psm);
        if (!l2cSigSendConnectionRsp(aclConn, conn->ident, 0, conn->remCh, L2C_SIG_CONN_RSP_NO_RSRC, 0))
            loge("Failed to sent negative connection request response frame\n");
        logd("Dropping connection struct\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    if (!l2cSigSendConnectionRsp(aclConn, conn->ident, conn->localCh, conn->remCh, L2C_SIG_CONN_RSP_OK, 0)) {
        loge("Failed to sent positive connection request response frame - dropping connection\n");
        if (!l2cSvcWorkSchedulePsmFree(conn->psm, NULL, instance))
            loge("Failed to free instance. this will cause a memory leak\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;
    conn->state = L2C_CONN_STATE_CFG;
    conn->cfgTxState = L2C_CFG_STATE_INITIAL;
    conn->cfgRxState = L2C_CFG_STATE_INITIAL;
    l2cPsmSendConfig(aclConn, conn);

out:
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cFixedChSvcInstAllocDone
 * USE:      Called when a service call to allocate a FixedCh service instance finishes
 * PARAMS:   reqID - request ID originally passed to l2cSvcWorkScheduleFixedChAlloc
 *           l2cStateCbk - the state cbk for this conn
 *           userData - param for state callbacks
 *           instance - the allocated instance or NULL
 *           handle - the handle param for the connection
 * RETURN:   NONE
 * NOTES:
 */
static void l2cFixedChSvcInstAllocDone(void *reqID, l2cStateCbk stateCbk, void *userData, void *instance, l2c_handle_t handle)
{
    struct l2cConn* conn;
    struct l2cAclConn* aclConn;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cFindConnByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByUniq(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (!instance) {
        logi("Service allocations failed for FixedCh %d\n", conn->localCh);
        logd("Dropping connection struct\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    logd("Service up for FixedCh %d\n", conn->localCh);

    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;
    l2cChannelReadyForData(aclConn, conn);

out:
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cSvcWorker
 * USE:      Worker thread to call up to services
 * PARAMS:   unused - not used
 * RETURN:   NONE
 * NOTES:    Used to call services and not get blocked by them.
 */
static void* l2cSvcWorker(void* unused)
{
    struct l2cSvcWork *work;
    int status;

    while(1) {

        status = workQueueGet(mServiceWork, (void**)&work);

        /* only one thing we use the work queue status for - exiting */
        if (status)
            break;

        pthread_mutex_lock(&mSvcsLock);
        switch (work->type) {
        case L2C_SVC_WORK_PSM_INST_ALLOC: {
            struct l2cPsmSvc *svc = l2cPsmSvcFindByPsm(work->psm);
            if (!svc) {
                logw("Got work for a not-found PSM-based service %d\n", work->psm);
                break;
            }
            if (svc->beingRemoved) {
                logi("Ignoring instance alloc for service (psm=%d) being removed\n",
                    work->psm);
                l2cPsmSvcInstAllocDone(work->reqID, NULL, NULL, NULL, work->i_alloc.handle);
            } else {
                l2cPsmSvcInstAllocDone(work->reqID, svc->desc.serviceInstanceStateCbk, svc->desc.userData,
                    svc->desc.serviceInstanceAlloc(svc->desc.userData, 
                    work->i_alloc.handle), work->i_alloc.handle);
            }
            break;
        }
        case L2C_SVC_WORK_PSM_CONNECTIONLESS_RX: {
            struct l2cPsmSvc *svc = l2cPsmSvcFindByPsm(work->psm);
            if (!svc) {
                logw("Got work for a not-found PSM-based service %d\n", work->psm);
                break;
            }
            if (svc->beingRemoved) {
                logi("Ignoring connectionless rx for service (psm=%d) being removed\n",
                    work->psm);
            } else {
                svc->desc.serviceConnectionlessRx(svc->desc.userData,
                    work->cl_rx.handle, work->cl_rx.data, work->cl_rx.size);
            }
            free(work->cl_rx.data);
            break;
        }
        case L2C_SVC_WORK_PSM_UNREGISTER: {
            struct l2cPsmSvc *svc = l2cPsmSvcFindByPsm(work->psm);
            if (!svc) {
                logw("Got work for a not-found PSM-based service %d\n", work->psm);
                break;
            }
            if (!svc->beingRemoved)
                logw("Unregistering a service (psm=%d) unexpectedly\n", work->psm);
            if (svc->next)
                svc->next->prev = svc->prev;
            if (svc->prev)
                svc->prev->next = svc->next;
            else
                mPsmSvcs = svc->next;
            free(svc);
            if (work->unreg.doneCbk)
                work->unreg.doneCbk(work->unreg.cbkData);
            break;
        }
        case L2C_SVC_WORK_FIXEDCH_INST_ALLOC: {
            struct l2cFixedChSvc *svc = l2cFixedChSvcFindByChan(work->chan);
            if (!svc) {
                logw("Got work for a not-found FixedCh service %d\n", work->chan);
                break;
            }
            if (svc->beingRemoved) {
                logi("Ignoring instance alloc for service (chan=%d) being removed\n",
                    work->chan);
                l2cFixedChSvcInstAllocDone(work->reqID, NULL, NULL, NULL, work->i_alloc.handle);
            } else {
                l2cFixedChSvcInstAllocDone(work->reqID, svc->desc.serviceFixedChStateCbk, svc->desc.userData,
                    svc->desc.serviceFixedChAlloc(svc->desc.userData,
                    work->i_alloc.handle), work->i_alloc.handle);
            }
            break;
        }
        default: {
            loge("Unknown work type %d\n", work->type);
            break;
        }
        }
        pthread_mutex_unlock(&mSvcsLock);
        free(work);
    }

    return NULL;
}

/*
 * FUNCTION: l2cAclEnqueueTx
 * USE:      Request a send on an ACL connection
 * PARAMS:   aclConn - the ACL connection structure
 *           data - the data
 *           size - how much data there is
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    Call only with mConnsLock held. buffer becomes owned by this code. will be freed.
 */
static bool l2cAclEnqueueTx(struct l2cAclConn *aclConn, const void *data, uint32_t len)
{
    //TODO: this
}

/*
 * FUNCTION: l2cInit
 * USE:      Called to init of L2CAP state
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    Initializes the l2cap layer
 */
int l2cInit(void)
{
    int i, ret;


    mServiceWork = workQueueAlloc(L2C_SVC_NUM_OUTSTANDING_REQS);
    if (!mServiceWork){
        loge("Failed to allocate service workqueue\n");
        return ENOMEM;
    }

    ret = pthread_create(&mSvcWorker, NULL, l2cSvcWorker, NULL);
    if (ret) {
        loge("Failed(%d) to create service worker\n", ret);
        workQueueFree(mServiceWork, NULL);
    }

    return 0;
}

/*
 * FUNCTION: l2cConnStructDelete
 * USE:      Delete a conn structure
 * PARAMS:   conn - the connection structure
 * RETURN:   NONE
 * NOTES:    Call with mConnsLock held.
 */
static void l2cConnStructDelete(struct l2cConn *conn)
{
    if (conn->next)
        conn->next = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        mConns = conn->next;

    free(conn);
}

/*
 * FUNCTION: l2cAclSigNextIdent
 * USE:      Request an "ident" value for signalling channel on ACL link
 * PARAMS:   aclConn - the ACL connection structure
 *           commit - whether to increment internal counter or not
 * RETURN:   the ident value
 * NOTES:    Call with mConnsLock held
 */
static uint8_t l2cAclSigNextIdent(struct l2cAclConn *aclConn, bool commit)
{
    uint8_t ret = aclConn->ident;

    if (commit)
        aclConn->ident = (ret == 0xFF) ? 1 : ret + 1;

    return ret;
}

/*
 * FUNCTION: l2cAclFreeCid
 * USE:      Find a free channel ID for an ACL connection
 * PARAMS:   aclConn - the acl connection to send it on
 * RETURN:   a free channel ID or zero on failure
 * NOTES:    call with mConnsLock held
 */
static uint16_t l2cAclFreeCid(struct l2cAclConn *aclConn)
{
    struct l2cConn* conn;
    uint16_t guess = aclConn->nextChan, next, initial = aclConn->nextChan;

    while(1) {
        /* calculate next guess */
        next = guess + 1;
        if (next < L2C_NUM_FIXED_CHANNELS)
            next = L2C_NUM_FIXED_CHANNELS;

        /* see if the guessed free channel is actually free */
        conn = mConns;
        while (conn && (conn->acl != aclConn->conn || conn->localCh != guess))
            conn = conn->next;

        /* if so, we're done! */
        if (!conn)
            break;

        /* else move on to the next one */
        guess = next;

        /* unless we've come a full circle, then give up */
        if (guess == initial)
            return 0;
    }

    /* store away where the next search should start */
    aclConn->nextChan = next;
    return guess;
}

/*
 * FUNCTION: l2cSigSend
 * USE:      Send a generic l2cap signalling frame
 * PARAMS:   aclConn - the ACL connection
 *           ident - ident to use, 0 for auto
 *           code - the ACL command code
 *           sigFrm - the signalling frame to send (a copy will be made)
 *           sigFrmLen - length of said signalling frame
 *           extraPtr - extra data to attach, if any (a copy will be made)
 *           extraLen - length of the extra data
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSend(struct l2cAclConn *aclConn, uint8_t ident, uint8_t code, const void *sigFrm, uint32_t sigFrmLen, const void *extraPtr, uint32_t extraLen)
{
    const int len = sizeof(struct l2cFrm) + sizeof(struct l2cSigFrm) + sigFrmLen + extraLen;
    struct l2cFrm *frm = (struct l2cFrm*)malloc(len);
    struct l2cSigFrm *sig = (struct l2cSigFrm*)(frm + 1);
    void *req = sig + 1;
    bool newIdent = ident == 0;

    if (!frm) {
        loge("out of memory");
        return 0;
    }

    utilSetLE16(&frm->len, sizeof(struct l2cSigFrm) + sigFrmLen + extraLen);
    utilSetLE16(&frm->cid, BT_ADDR_IS_LE(aclConn->addr) ? L2C_CH_SIGNALLING_LE : L2C_CH_SIGNALLING_EDR);
    utilSetLE8(&sig->code, code);
    utilSetLE16(&sig->len, sigFrmLen + extraLen);
    memcpy(req, sigFrm, sigFrmLen);
    memcpy(((uint8_t*)req) + sigFrmLen, extraPtr, extraLen);

    if (newIdent)
        ident = l2cAclSigNextIdent(aclConn, false);
    utilSetLE8(&sig->ident, ident);

    if (l2cAclEnqueueTx(aclConn, frm, len)) {
        if (newIdent)
            l2cAclSigNextIdent(aclConn, true);
        return ident;
    } else {
        free(frm);
        return 0;
    }
}

/*
 * FUNCTION: l2cSigSendConnectionReq
 * USE:      Send a request to open a PSM channel
 * PARAMS:   aclConn - the acl connection to send it on
 *           psm - the PSM
 *           localCh - local channel number
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConnectionReq(struct l2cAclConn *aclConn, psm_t psm, uint16_t localCh)
{
    struct l2cSigConnReq req;

    utilSetLE16(&req.psm, psm);
    utilSetLE16(&req.scid, localCh); 

    return l2cSigSend(aclConn, 0, L2C_SIG_CONN_REQ, &req, sizeof(req), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendConnectionRsp
 * USE:      Send a response to an open request for a PSM channel
 * PARAMS:   aclConn - the acl connection to send it on
 *           ident - ident to use
 *           localCh - local channel number
 *           remCh - remote channel number
 *           result - the result value to send
 *           status - the status value to send
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConnectionRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh, uint16_t result, uint16_t status)
{
    struct l2cSigConnRsp rsp;

    utilSetLE16(&rsp.dcid, localCh);
    utilSetLE16(&rsp.scid, remCh);
    utilSetLE16(&rsp.result, result);
    utilSetLE16(&rsp.status, status);

    return l2cSigSend(aclConn, ident, L2C_SIG_CONN_RSP, &rsp, sizeof(rsp), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendConnectionReq
 * USE:      Send a request to configure a PSM channel
 * PARAMS:   aclConn - the acl connection to send it on
 *           remCh - remote channel number
 *           flags - the flags value to send
 *           cfgData - config data to send (copy will be made)
 *           cfgLen - length of config data
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConfigReq(struct l2cAclConn *aclConn, uint16_t remCh, uint16_t flags, const void *cfgData, uint32_t cfgLen)
{
    struct l2cSigConfReq req;

    utilSetLE16(&req.dcid, remCh);
    utilSetLE16(&req.flags, flags);

    return l2cSigSend(aclConn, 0, L2C_SIG_CONF_REQ, &req, sizeof(req), cfgData, cfgLen);
}

/*
 * FUNCTION: l2cSigSendConnectionRsp
 * USE:      Send a response to an configure request for a PSM channel
 * PARAMS:   aclConn - the acl connection to send it on
 *           ident - ident to use
 *           remCh - remote channel number
 *           flags - the flags value to send
 *           result - the result value to send
 *           cfgData - config data to send (copy will be made)
 *           cfgLen - length of config data
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConfigRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t flags, uint16_t result, const void *cfgData, uint32_t cfgLen)
{
    struct l2cSigConfRsp rsp;

    utilSetLE16(&rsp.scid, remCh);
    utilSetLE16(&rsp.flags, flags);
    utilSetLE16(&rsp.result, result);

    return l2cSigSend(aclConn, ident, L2C_SIG_CONF_RSP, &rsp, sizeof(rsp), cfgData, cfgLen);
}

/*
 * FUNCTION: l2cSigSendDiscReq
 * USE:      Send a request to close a PSM channel
 * PARAMS:   aclConn - the acl connection
 *           localCh - local channel number
 *           remCh - remote channel number
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendDiscReq(struct l2cAclConn *aclConn, uint16_t localCh, uint16_t remCh)
{
    struct l2cSigDiscReq req;

    utilSetLE16(&req.scid, localCh);
    utilSetLE16(&req.dcid, remCh);

    return l2cSigSend(aclConn, 0, L2C_SIG_DISC_REQ, &req, sizeof(req), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendDiscRsp
 * USE:      Send a response to an close request for a PSM channel
 * PARAMS:   aclConn - the acl connection
 *           ident - ident to use
 *           localCh - local channel number
 *           remCh - remote channel number
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendDiscRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh)
{
    struct l2cSigDiscRsp rsp;

    utilSetLE16(&rsp.scid, remCh);
    utilSetLE16(&rsp.dcid, localCh);

    return l2cSigSend(aclConn, ident, L2C_SIG_DISC_RSP, &rsp, sizeof(rsp), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendEchoRsp
 * USE:      Send a response to an echo request
 * PARAMS:   aclConn - the acl connection to send it on
 *           ident - ident to use
 *           echoData - echo data to send (copy will be made)
 *           echoLen - length of config data
 * RETURN:   ident vlue used for request or 0 if none was sent
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendEchoRsp(struct l2cAclConn *aclConn, uint8_t ident, const void *echoData, uint32_t echoLen)
{
    return l2cSigSend(aclConn, ident, L2C_SIG_ECHO_RSP, NULL, 0, echoData, echoLen);
}

/*
 * FUNCTION: l2cSigSendInfoReq
 * USE:      Send an info request
 * PARAMS:   aclConn - the acl connection
 *           infoType - requested info type
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendInfoReq(struct l2cAclConn *aclConn, uint16_t infoType)
{
    struct l2cSigInfoReq req;

    utilSetLE16(&req.infoType, infoType);

    return l2cSigSend(aclConn, 0, L2C_SIG_INFO_REQ, &req, sizeof(req), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendInfoRsp
 * USE:      Send a response to an info request
 * PARAMS:   aclConn - the acl connection
 *           ident - ident to use
 *           infoType - requested info type
 *           result - requested info type
 *           infoData - info data to send (copy will be made)
 *           infoLen - length of config data
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, const void *infoData, uint32_t infoLen)
{
    struct l2cSigInfoRsp rsp;

    utilSetLE16(&rsp.infoType, infoType);
    utilSetLE16(&rsp.result, result);

    return l2cSigSend(aclConn, ident, L2C_SIG_INFO_RSP, &rsp, sizeof(rsp), infoData, infoLen);
}

/*
 * FUNCTION: l2cSigSendConnUpdateReq
 * USE:      Send a connection update request for an LE link
 * PARAMS:   aclConn - the acl connection
 *           intMin - minimum requested interval in units of 1.25ms
 *           intMax - maximum requested interval in units of 1.25ms
 *           latency - requested latency
 *           timeoutMult - requested timeout in units of 10ms
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConnUpdateReq(struct l2cAclConn *aclConn, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeoutMult)
{
    struct l2cSigConnUpdtReq req;

    utilSetLE16(&req.intMin, intMin);
    utilSetLE16(&req.intMax, intMax);
    utilSetLE16(&req.latency, latency);
    utilSetLE16(&req.timeoutMult, timeoutMult);

    return l2cSigSend(aclConn, 0, L2C_SIG_CONN_UPDT_REQ, &req, sizeof(req), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendConnUpdateRsp
 * USE:      Send a connection update request for an LE link
 * PARAMS:   aclConn - the acl connection
 *           ident - ident to use
 *           result - requested info type
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendConnUpdateRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t result)
{
    struct l2cSigConnUpdtRsp rsp;

    utilSetLE16(&rsp.result, result);

    return l2cSigSend(aclConn, ident, L2C_SIG_CONN_UPDT_RSP, &rsp, sizeof(rsp), NULL, 0);
}

/*
 * FUNCTION: l2cSigSendErrCmdReject
 * USE:      Send a "command rejected" signalling frame
 * PARAMS:   aclConn - the acl connection
 *           ident - the ident value to use
 *           reason - the reason to send
 *           rejData - rejection data to send (copy will be made)
 *           rejLen - length of rejection data
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendErrCmdReject(struct l2cAclConn *aclConn, uint8_t ident, uint16_t reason, const void *rejData, uint32_t rejLen)
{
    struct l2cSigCmdRej rej;

    utilSetLE16(&rej.reason, reason);

    return l2cSigSend(aclConn, ident, L2C_SIG_CMD_REJ, &rej, sizeof(rej), rejData, rejLen);
}

/*
 * FUNCTION: l2cSigSendErrCmdNotUnderstood
 * USE:      Send a "command not understood" error
 * PARAMS:   aclConn - the acl connection
 *           ident - the ident value to use
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendErrCmdNotUnderstood(struct l2cAclConn *aclConn, uint8_t ident)
{
    return l2cSigSendErrCmdReject(aclConn, ident, L2C_SIG_CMD_NOT_UNDERSTOD, NULL, 0);
}

/*
 * FUNCTION: l2cSigSendErrInvalidCid
 * USE:      Send a "invalid channel id" error
 * PARAMS:   aclConn - the acl connection
 *           rxedDcid - the received "dcid" value
 *           rxedScid - the received "scid" value
 * RETURN:   ident if request was sent, else 0
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cSigSendErrInvalidCid(struct l2cAclConn *aclConn, uint8_t ident, uint16_t rxedDcid, uint16_t rxedScid)
{
    struct l2cSigInvalidCid info;

    utilSetLE16(&info.rxedDcid, rxedDcid);
    utilSetLE16(&info.rxedScid, rxedScid);

    return l2cSigSendErrCmdReject(aclConn, ident, L2C_SIG_INVALID_CID, &info, sizeof(info));
}

/*
 * FUNCTION: l2cConnRequestClose
 * USE:      Request an orderly closing of a channel
 * PARAMS:   con - the connection structure
 * RETURN:   ident if request was sent, else 0
 * NOTES:    Call with mConnsLock held. Will call service's free(), cbk if appropriate
 */
static void l2cConnRequestClose(struct l2cConn *conn)
{
    struct l2cAclConn *acl;

    acl = l2cAclConnFindByUniq(conn->acl); //todo: we should check if this is truly th eonly waiting connection for this ACL link. perhaps keep a "num_waiters" in the aclConn struct?

    switch (conn->state) {
    case L2C_CONN_STATE_RX_OPEN_SVC:
        conn->state = L2C_CONN_STATE_TEARDOWN;
        break;
    case L2C_CONN_STATE_CFG:
    case L2C_CONN_STATE_ESTABLISHED:
        
        if (conn->instance) {
            if(conn->psm)
                l2cSvcWorkSchedulePsmFree(conn->psm, conn, conn->instance);
            else
                l2cSvcWorkScheduleFixedChFree(conn->localCh, conn, conn->instance);
            conn->state = L2C_CONN_STATE_DIE_REQD;
        } else {
            if (acl){
                l2cSigSendDiscReq(acl, conn->localCh, conn->remCh);
                conn->state = L2C_CONN_STATE_DIE_TXED;
            } else {
                /* let other side, if any, care */
                l2cConnStructDelete(conn);
            }
        }
        break;
    case L2C_CONN_STATE_CONN_WAIT:
        /* cancel an in-progress connection, if we're the last one waiting for it */
        if (acl) //todo: we should check if this is truly th eonly waiting connection for this ACL link. perhaps keep a "num_waiters" in the aclConn struct?
            l2cAclLinkClose(&acl->addr);
        else
            logw("Cannot close in-progress acl conn - not found\n");

        /* fallthrough intentional */
    case L2C_CONN_STATE_TX_OPEN_TXED:
        /* let other side care */
        l2cConnStructDelete(conn);
        break;
    case L2C_CONN_STATE_DIE_REQD:
    case L2C_CONN_STATE_DIE_TXED:
    case L2C_CONN_STATE_DIE_RXED:
    case L2C_CONN_STATE_TEARDOWN:
        /* we're already on it - nothing to do */
        break;
    default:
        logw("Unexpected connection state %d when trying to close\n", conn->state);
    }
}

/*
 * FUNCTION: l2cApiServicePsmRegister
 * USE:      Registers a service that works on top of L2CAP PSM-based channels
 * PARAMS:   psm - the PSM you want, or zero to be assigned one
 *           service - the service descriptor for the service
 * RETURN:   the psm you were assigned or zero in case of some error
 * NOTES:    service callbacks may be called before this function returns
 */
psm_t l2cApiServicePsmRegister(psm_t psm, const struct l2cServicePsmDescriptor *service)
{
    struct l2cPsmSvc *svc;

    if (!l2cPsmValid(psm)) {
        logw("Invalid PSM %d\n", psm);
        return false;
    }

    svc = (struct l2cPsmSvc*)malloc(sizeof(struct l2cPsmSvc));
    if (!svc)
        return false;

    memcpy(&svc->desc, service, sizeof(svc->desc));
    svc->beingRemoved = false;
    svc->prev = NULL;

    pthread_mutex_lock(&mSvcsLock);
    if (psm) {    /* check for existence */

        if (l2cPsmSvcFindByPsm(psm))
            psm = 0;
    } else {     /* find a free one */
        psm_t orig_next_psm = nextPsm;

        do {
            psm = nextPsm;

            /* calculate the next potential valid PSM */
            nextPsm += 2; /* only odds allowed */
            if (nextPsm & 0x0100) /* no low bit on high byte alowed */
                nextPsm += 0x100;
            if (nextPsm < 0x1000 || nextPsm > 0xFFFF) /* enforce max and min */
                nextPsm = 0x1001;

            /* check if existing PSM is taken */
            if (l2cPsmSvcFindByPsm(psm))
                psm = 0;
            else
                break;

        } while(nextPsm != orig_next_psm);
    }

    if (psm) { /* we have one that we can use */

        svc->psm = psm;
        svc->next = mPsmSvcs;
        if (mPsmSvcs)
            mPsmSvcs->prev = svc;
        mPsmSvcs = svc;
    } else
        free(svc);

    pthread_mutex_unlock(&mSvcsLock);

    return psm;
}

/*
 * FUNCTION: l2cApiServicePsmUnregister
 * USE:      Unregisters a service that works on top of L2CAP PSM-based channels
 * PARAMS:   psm - the PSM that was previously registered
 *           doneCbk - called when removal is finished (can be NULL)
 *           callbackData - data to pass to callback
 * RETURN:   true success
 * NOTES:    Your job to make sure service is not in use.
 */
bool l2cApiServicePsmUnregister(psm_t psm, void (*doneCbk)(void*), void* cbkData)
{
    bool success = false;
    struct l2cPsmSvc *svc;
    struct l2cConn *conn;
    bool anythingDone;

    /* terminate all connections with that psm */
    pthread_mutex_lock(&mConnsLock);
    do {
         anythingDone = false;
         conn = mConns;
         if (conn->psm == psm) {

             l2cConnRequestClose(conn);
             anythingDone = true;
         }

    } while(anythingDone);
    pthread_mutex_unlock(&mConnsLock);

    /* schedule demolition of the psm itself */
    pthread_mutex_lock(&mSvcsLock);
    svc = l2cPsmSvcFindByPsm(psm);
    if (svc) {
        svc->beingRemoved = true;
        l2cSvcWorkSchedulePsmUnregister(psm, NULL, doneCbk, cbkData);
    }
    pthread_mutex_unlock(&mSvcsLock);

    return success;
}

/*
 * FUNCTION: l2cApiServiceFixedChRegister
 * USE:      Registers a service that works on top of L2CAP FixedCh
 * PARAMS:   chan - the channel you want
 *           service - the service descriptor for the service
 * RETURN:   truwe on success
 * NOTES:    service callbacks may be called before this function returns
 */
bool l2cApiServiceFixedChRegister(uint16_t chan, const struct l2cServiceFixedChDescriptor *service)
{
    struct l2cFixedChSvc *svc;

    if (chan == L2C_CH_SIGNALLING_EDR || chan == L2C_CH_CONNECTIONLESS ||
        chan == L2C_CH_SIGNALLING_LE || !chan || chan >= L2C_NUM_FIXED_CHANNELS)
        return false;

    svc = (struct l2cFixedChSvc*)malloc(sizeof(struct l2cFixedChSvc));
    if (!svc)
        return false;

    memcpy(&svc->desc, service, sizeof(svc->desc));
    svc->beingRemoved = false;
    svc->prev = NULL;

    pthread_mutex_lock(&mSvcsLock);
    if (chan && !l2cFixedChSvcFindByChan(chan)) { /* check for validity & existence */

        svc->chan = chan;
        svc->next = mFixedSvcs;
        if (mFixedSvcs)
            mFixedSvcs->prev = svc;
        mFixedSvcs = svc;
    } else {
        chan = 0;
        free(svc);
    }

    pthread_mutex_unlock(&mSvcsLock);

    return chan != 0;
}

/*
 * FUNCTION: l2cApiServiceFixedChUnregister
 * USE:      Unregisters a service that works on top of L2CAP FixedCh
 * PARAMS:   chan - the channel
 *           doneCbk - called when removal is finished (can be NULL)
 *           callbackData - data to pass to callback
 * RETURN:   true success
 * NOTES:    Your job to make sure service is not in use.
 */
bool l2cApiServiceFixedChUnregister(uint16_t chan, void (*doneCbk)(void*), void* cbkData)
{
    bool success = false;
    struct l2cFixedChSvc *svc;
    struct l2cConn *conn;
    bool anythingDone;

    /* terminate all connections with that psm */
    pthread_mutex_lock(&mConnsLock);
    do {
         anythingDone = false;
         conn = mConns;
         if (!conn->psm && conn->localCh == chan) {

             if (conn->instance)
                 l2cSvcWorkScheduleFixedChFree(chan, conn, conn->instance);
             else {
                 logw("no instance found for fixed ch %d while unregistering\n", chan);
                 l2cConnStructDelete(conn);
             }
             anythingDone = true;
         }

    } while(anythingDone);
    pthread_mutex_unlock(&mConnsLock);

    /* schedule demolition of the fixedCh registration itself */
    pthread_mutex_lock(&mSvcsLock);
    svc = l2cFixedChSvcFindByChan(chan);
    if (svc) {
        svc->beingRemoved = true;
        l2cSvcWorkScheduleFixedChUnregister(chan, NULL, doneCbk, cbkData);
    }
    pthread_mutex_unlock(&mSvcsLock);

    return success;
}

/*
 * FUNCTION: l2cFindConnByHandle
 * USE:      Find a connection structure by handle
 * PARAMS:   handle - the handle
 * RETURN:   connection structure or NULL
 * NOTES:    Call with mConnsLock held
 */
static struct l2cConn* l2cFindConnByHandle(l2c_handle_t handle)
{
    struct l2cConn *conn;
    uint16_t aclConnID, localCh, remCh;
    psm_t psm;

    l2cHandleDecode(handle, &aclConnID, &localCh, &remCh, &psm);

    conn = mConns;

    while (conn) {

        if (conn->psm == psm && conn->state != L2C_CONN_STATE_CONN_WAIT &&
            conn->localCh == localCh && conn->remCh == remCh) {
            struct l2cAclConn *aclConn;
            aclConn = l2cAclConnFindByUniq(conn->acl);
            if (aclConn && aclConn->conn == aclConnID)
                return conn;
        }

        conn = conn->next;
    }

    return NULL;
}

/*
 * FUNCTION: l2cSvcGetBtAddr
 * USE:      Get BT addr of whomeevr is on the other end of a pipe
 * PARAMS:   handle - the handle
 *           addrP - here is stored the address
 * RETURN:   true on success
 * NOTES:
 */
bool l2cSvcGetBtAddr(l2c_handle_t handle, struct bt_addr* addrP)
{
    uint16_t aclConnID;
    struct l2cAclConn *aclConn;
    bool ret = false;

    l2cHandleDecode(handle, &aclConnID, NULL, NULL, NULL);
    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindById(aclConnID);
    if (aclConn) {
        memcpy(addrP, &aclConn->addr, sizeof(struct bt_addr));
        ret = true;
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cSvcDisconnect
 * USE:      Request a disconnect of a pipe
 * PARAMS:   handle - the handle
 * RETURN:   NONE
 * NOTES:    Disconnect is async and will finish later
 */
void l2cSvcDisconnect(l2c_handle_t handle)
{
    struct l2cConn *conn;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cFindConnByHandle(handle);
    if (conn)
        l2cConnRequestClose(conn);
    else
        logw("Got request to close nonexistent handle "HANDLEFMT"\n", HANDLECNV(handle));

    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cConnRequestSend
 * USE:      Request a send on a channel
 * PARAMS:   conn - the connection structure
 *           data - the data
 *           size - how much data there is
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    A copy of the data is made. Call only with mConnsLock held
 */
static bool l2cConnRequestSend(struct l2cConn *conn, const void *data, uint32_t len)
{
    struct l2cAclConn *aclConn;
    uint32_t buflen = len + sizeof(struct l2cFrm);
    struct l2cFrm *frm;
    void *buf;

    /* we only support L2CAP simple mode for now */

    if (conn->state != L2C_CONN_STATE_ESTABLISHED)
        return false;

    buf = malloc(buflen);

    if (!buf)
        return false;

    frm = (struct l2cFrm*)buf;
    memcpy(frm + 1, data, len);
    utilSetLE16(&frm->len, len);
    utilSetLE16(&frm->cid, conn->remCh);

    aclConn = l2cAclConnFindByUniq(conn->acl);
    if (aclConn && l2cAclEnqueueTx(aclConn, buf, buflen))
        return true;

    free(buf);
    return false;
}

/*
 * FUNCTION: l2cConnectionlessRequestSend
 * USE:      Request a send on a connectioneless pipe
 * PARAMS:   aclConn - the ACL connection structure
 *           psm - the psm
 *           data - the data
 *           size - how much data there is
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    A copy of the data is made. Call only with mConnsLock held
 */
static bool l2cConnectionlessRequestSend(struct l2cAclConn *aclConn, psm_t psm, const void *data, uint32_t len)
{
    uint32_t buflen = len + sizeof(struct l2cFrm) + sizeof(psm_t);
    void *buf = malloc(buflen);
    struct l2cFrm *frm;
    psm_t *psmP;
    /* connectionless data is always in a simple frame */

    if (!buf)
        return false;

    frm = (struct l2cFrm*)buf;
    psmP = (psm_t*)(frm + 1);
    memcpy(psmP + 1, data, len);
    utilSetLE16(psmP, psm);
    utilSetLE16(&frm->len, len + sizeof(psm_t));
    utilSetLE16(&frm->cid, L2C_CH_CONNECTIONLESS);

    if (!l2cAclEnqueueTx(aclConn, buf, buflen)) {
        free(buf);
        return false;
    }

    return true;
} 

/*
 * FUNCTION: l2cSvcDataTx
 * USE:      Request a send on a pipe
 * PARAMS:   handle - the handle
 *           data - the data
 *           size - how much data there is
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    A copy of the data is made
 */
bool l2cSvcDataTx(l2c_handle_t handle, const void* data, uint32_t size)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    uint16_t aclConnID, localCh, remCh;
    bool ret = true;
    psm_t psm;

    l2cHandleDecode(handle, &aclConnID, &localCh, &remCh, &psm);

    pthread_mutex_lock(&mConnsLock);
    if (localCh && remCh) { /* connection-oriented pipe */
        conn = l2cFindConnByHandle(handle);
        if (conn)
            ret = l2cConnRequestSend(conn, data, size);
        else
            ret = false;
    } else { /* connectionless pipe */
        aclConn = l2cAclConnFindById(aclConnID);
        if (aclConn)
            ret = l2cConnectionlessRequestSend(aclConn, psm, data, size);
        else
            ret = false;
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cCreateAndOpenNewConn
 * USE:      Request an ACL connection be created
 * PARAMS:   addr - the peer address
 * RETURN:   true is it was requested. False on immediate error.
 * NOTES:    call with mConnsLock held
 */
static struct l2cAclConn* l2cCreateAndOpenNewConn(const struct bt_addr* addr)
{
    struct l2cAclConn *conn;

    conn = l2cAclConnFindByAddr(addr);
    if (conn) {
        logw("Asked to establish existing ACL conn\n");
        return conn;
    }

    conn = (struct l2cAclConn*)malloc(sizeof(struct l2cAclConn));
    if (!conn) {
        loge("Out of memory\n");
        return NULL;
    }

    memcpy(&conn->addr, addr, sizeof(conn->addr));
    conn->uniq = l2cUniqNext();
    conn->conn = L2C_CONN_IN_PROGRESS;
    conn->prev = NULL;
    conn->next = mAclConns;
    mAclConns->prev = conn;
    mAclConns = conn;

    l2cAclLinkEstablish(addr);

    return conn;
}

/*
 * FUNCTION: l2cApiCreatePsmConnection
 * USE:      Request a PSM-based connection be created
 * PARAMS:   psm - the PSM to connect to
 *           addr - the peer address
 *           mtu - the mtu we're willing to accept
 *           doneCbk - function to call when action completes
 *           userData - data to pass to stateCbk
 *           instance - data to pass to stateCbk
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    callback may be called before this function returns, if
 *           true was returned, at least one call to the callback
 *           will happen
 */
bool l2cApiCreatePsmConnection(psm_t psm, const struct bt_addr* addr, l2cStateCbk stateCbk, void* userData, void* instance)
{
    struct l2cAclConn *aclConn;
    struct l2cConn *conn = NULL, *t;
    bool ret = false;

    if (!stateCbk) {
        logw("Invalid stateCbk\n");
        return false;
    }

    if (!l2cPsmValid(psm)) {
        logw("Invalid PSM %d\n", psm);
        return false;
    }

    if (BT_ADDR_IS_LE(*addr)) {
        logw("Cannot create a PSM connection to an LE address\n");
        return false;
    }

    conn = (struct l2cConn*)malloc(sizeof(struct l2cConn));
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        return false;
    }

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByAddr(addr);
    if (!aclConn) {
        logd("acl not found for connection requested, making one\n");
        aclConn = l2cCreateAndOpenNewConn(addr);
    }
    if (!aclConn) {
        loge("Failed to create/open/find ACL conn\n");
        goto out;
    }
    /* check is same PSM already exists */
    t = mConns;
    while(t && t->acl != aclConn->uniq && t->psm != psm)
        t = t->next;
    if (t) {
        logi("Ignoring attempt to open psm %d conn when existing exists\n", psm);
        goto out;
    }
    /* create the conn */
    conn->acl = aclConn->uniq;
    conn->psm = psm;
    conn->mtu = 0xFFFF;
    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;

    if (aclConn->conn & L2C_CONN_IN_PROGRESS)
        conn->state = L2C_CONN_STATE_CONN_WAIT;
    else {
        uint8_t ident;
        uint16_t localCh = l2cAclFreeCid(aclConn);

        if (!localCh) {
            logw("No free channels for conn %d\n", aclConn->conn);
            goto out;
        }

        ident = l2cSigSendConnectionReq(aclConn, psm, localCh);
        if (!ident) {
            l2cSvcWorkScheduleStateCallDirect(NULL, stateCbk, userData, instance, L2C_STATE_CLOSED, NULL, 0);
            loge("Failed to request opening of a psm channel\n");
            goto out;
        }
        conn->localCh = localCh;
        conn->state = L2C_CONN_STATE_TX_OPEN_TXED;
        conn->ident = ident;
    }
    conn->prev = NULL;
    conn->next = mConns;
    mConns = conn;
    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    if (!ret)
        free(conn);
    return ret;
}

/*
 * FUNCTION: l2cApiCreatePsmConnection
 * USE:      Request a FixedCh connection be created
 * PARAMS:   chan - the channel to connect to
 *           addr - the peer address
 *           stateCbk - function to call with state
 *           userData - data to pass to stateCbk
 *           instance - data to pass to stateCbk
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    callback may be called before this function returns, if
 *           true was returned, at least one call to the callback
 *           will happen
 */
bool l2cApiCreateFixedChConnection(uint16_t chan, const struct bt_addr* addr, l2cStateCbk stateCbk, void* userData, void* instance)
{
    struct l2cAclConn *aclConn;
    struct l2cConn *conn = NULL, *t;
    bool ret = false;

    if (chan == L2C_CH_SIGNALLING_EDR || chan == L2C_CH_CONNECTIONLESS ||
        chan == L2C_CH_SIGNALLING_LE || !chan || chan >= L2C_NUM_FIXED_CHANNELS) {
        logw("Invalid channel for creating a FixedCh connection\n");
        return false;
    }

    if (!stateCbk) {
        logw("Invalid stateCbk\n");
        return false;
    }

    conn = (struct l2cConn*)malloc(sizeof(struct l2cConn));
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        return false;
    }

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByAddr(addr);
    if (!aclConn) {
        logd("acl not found for connection requested, making one\n");
        aclConn = l2cCreateAndOpenNewConn(addr);
    }
    if (!aclConn) {
        loge("Failed to create/open/find ACL conn\n");
        goto out;
    }
    /* check is same FixedCh already exists */
    t = mConns;
    while(t && t->acl != aclConn->uniq && (t->psm || t->localCh != chan))
        t = t->next;
    if (t) {
        logi("Ignoring attempt to open FixedCh %d conn when existing exists\n", chan);
        goto out;
    }
    /* create the conn */
    conn->acl = aclConn->uniq;
    conn->psm = 0;
    conn->localCh = chan;
    conn->remCh = chan;
    conn->mtu = 0xFFFF;
    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;
    if (aclConn->conn & L2C_CONN_IN_PROGRESS)
        conn->state = L2C_CONN_STATE_CONN_WAIT;
    else
        l2cChannelReadyForData(aclConn, conn);

    conn->prev = NULL;
    conn->next = mConns;
    mConns = conn;
    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    free(conn);
    return ret;
}

/*
 * FUNCTION: l2cAclLinkUp
 * USE:      Called when a new ACL link comes up
 * PARAMS:   aclConn - the conection ID
 *           addr - the peer address
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkUp(uint16_t aclConnID, const struct bt_addr* addr)
{
    struct l2cAclConn *aclConn;
    struct l2cConn *connIter;
    struct l2cFixedChSvc *svc;

    pthread_mutex_lock(&mConnsLock);
    if (l2cAclConnFindById(aclConnID))
        loge("Duplicate ACL connection ID up %d\n", aclConnID);
    else {
        aclConn = l2cAclConnFindByAddr(addr);
        if (aclConn) {
            if (aclConn->conn & L2C_CONN_IN_PROGRESS)
                logd("Us-initiated ACL connection up %d\n", aclConnID);
            else {
                loge("Duplicate ACL link up with same address and different connID (old %d new %d)."
                    " Ignored.\n", aclConn->conn, aclConnID);
                goto out;
            }
        } else {
            logd("Remote-initiated ACL connection up %d\n", aclConnID);
            aclConn = (struct l2cAclConn*)malloc(sizeof(struct l2cAclConn));
            if (aclConn) {
                aclConn->uniq = l2cUniqNext();
                aclConn->prev = NULL;
                aclConn->next = mAclConns;
                mAclConns->prev = aclConn;
                mAclConns = aclConn;
            } else {
                loge("Failed to allocate a new l2cAclConn for connection %d\n", aclConnID);
                goto out;
            }
        }
        memcpy(&aclConn->addr, addr, sizeof(aclConn->addr));
        aclConn->ident = 1;
        aclConn->connectionlessMtu = L2C_MIN_CONNECTIONLESS_MTU;
        aclConn->nextChan = L2C_NUM_FIXED_CHANNELS;
        aclConn->conn = aclConnID;

        /* handle all pending-open connections */
        connIter = mConns;
        while (connIter){
            uint8_t ident = 0;
            uint16_t localCh;
            struct l2cConn *conn = connIter;
            connIter = connIter->next;

            if (conn->acl != aclConn->uniq) /* not for this ACL link */
                continue;

            if (conn->state != L2C_CONN_STATE_CONN_WAIT) { /* wrong state? */
                loge("L2C connection for new acl link %d not in wait state on link up! Dropping\n", aclConnID);
                //todo: drop it appropriately (is this even possible, given how confused we aleady are about its state?)
                continue;
            }

            if (!conn->psm) { /* FixedCh */
                l2cChannelReadyForData(aclConn, conn);
                continue;
            }

            /* PSM connection */
            localCh = l2cAclFreeCid(aclConn);
            if (localCh) {
                 ident = l2cSigSendConnectionReq(aclConn, conn->psm, localCh);
                 conn->localCh = localCh;
            }

            if (!ident) {
                l2cSvcWorkScheduleStateCall(NULL, conn, L2C_STATE_CLOSED, NULL, 0);
                loge("Failed to request opening of a psm channel\n");
                //TODO: drop it appropriately
            }
            conn->state = L2C_CONN_STATE_TX_OPEN_TXED;
            conn->ident = ident;
        }

        /* Bring up all FixedCh services */
        pthread_mutex_lock(&mSvcsLock);
        svc = mFixedSvcs;
        while (svc) {
            if (!svc->beingRemoved)
                if (!l2cSvcWorkScheduleFixedChAlloc(svc->chan, NULL, l2cHandleCreate(aclConn->conn, svc->chan, svc->chan, 0)))
                    loge("Failed to schedule FixedCh alloc for ch %d\n", svc->chan);
            svc = svc->next;
        }
        pthread_mutex_unlock(&mSvcsLock);
    }

out:
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclLinkDown
 * USE:      Called when a new ACL link goes down
 * PARAMS:   aclConn - the conection ID
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkDown(uint16_t aclConn)
{
    //todo: lots...
}








