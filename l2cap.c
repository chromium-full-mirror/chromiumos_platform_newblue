/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
//todo: check all lengths followed, all log levels correct
//todo: hci QoS is same as L2CAP QoS, so we can probably support L2CAP QoS via HCI QoS easily

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include "multiNotif.h"
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "timer.h"
#include "util.h"
#include "uniq.h"
#include "log.h"
#include "hci.h"
#include "mt.h"


/* constants from spec */
#define L2C_FTR_FLOW_CTRL_MODE     0x00000001
#define L2C_FTR_RETR_MODE          0x00000002
#define L2C_FTR_BIDIR_QOS          0x00000004
#define L2C_FTR_ENH_RETR_MODE      0x00000008
#define L2C_FTR_STREAMING_MODE     0x00000010
#define L2C_FTR_FCS_OPTION         0x00000020
#define L2C_FTR_EXTD_FLOW_SPEC     0x00000040
#define L2C_FTR_FIXED_CHS          0x00000080
#define L2C_FTR_EXTD_WINDOW_SZ     0x00000100
#define L2C_FTR_CONNLESS_DATA_RX   0x00000200

#define L2C_EXT_FTRS_SUPPORTED     (L2C_FTR_FIXED_CHS)
#define L2C_FIXED_CHS_SUPPORTED    0x6   /* as mandated by the spec */
#define L2C_MIN_CONNECTIONLESS_MTU 48
#define L2C_MIN_SIG_MTU            48

#define L2C_RETR_NUM_BUFFERS       64

/* also, the first non-fixed channel */
#define L2C_NUM_FIXED_CHANNELS     64

/* PSM length minimum as per spec */
#define L2C_PSM_MIN_LEN            2

/* L2CAP channel defines as per spec */
#define L2C_CH_SIGNALLING_EDR      0x0001
#define L2C_CH_CONNECTIONLESS      0x0002
#define L2C_CH_SIGNALLING_LE       0x0005


#define L2C_MODE_BASIC             0
#define L2C_MODE_RETR              1
#define L2C_MODE_FLOW_CTRL         2
#define L2C_MODE_STREAM            3
#define L2C_MODE_ENH_RETR          4

#define L2C_FLG_S_FRM              0x0001
#define L2C_FLG_R                  0x0080
#define L2C_FLG_F                  0x0080
#define L2C_MASK_SAR               0xC000
#define L2C_SHFT_SAR               14
#define L2C_MASK_S                 0x000C
#define L2C_SHFT_S                 2
#define L2C_FLG_P                  0x0010
#define L2C_FLG_RSVD_S             0x0002
#define L2C_FLG_MASK_REQ_SEQ       0x3F00
#define L2C_FLG_SHFT_REQ_SEQ       8
#define L2C_FLG_MASK_TX_SEQ        0x007E
#define L2C_FLG_SHFT_TX_SEQ        1

#define L2C_SAR_COMPLETE           0
#define L2C_SAR_START              1
#define L2C_SAR_END                2
#define L2C_SAR_CONT               3

#define L2C_S_RR                   0
#define L2C_S_REJ                  1
#define L2C_S_RNR                  2
#define L2C_S_SREJ                 3


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

#define L2C_SIG_CONN_PEND_UNKNOWN 0x0000
#define L2C_SIG_CONN_PEND_AUTH    0x0001
#define L2C_SIG_CONN_PEND_ATHORIZ 0x0002

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

#define L2C_SIG_CONN_UPDT_OK      0x0000
#define L2C_SIG_CONN_UPDT_NO      0x0001



/* low level L2CAP frame */
struct l2cFrmHdr { /* header of all frames */
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
    uniq_t             handle;
    uniq_t             openTimer;
    uniq_t             teardownTimer;
    hci_conn_t         conn;
    sg                 reassSg;

    uint32_t           peerFeatures;
    uint64_t           peerFixedChs;
    uint8_t            ident; /* next ident to use */
    uint16_t           nextChan; /* a guess to a currently-free channel */
    uint16_t           connectionlessMtu;
    uint16_t           sigMtu;
    struct bt_addr     peerAddr; /* who the peer is */
    struct bt_addr     selfAddr; /* who we were acting as when we made the connection */
    uint8_t            state;
    uint8_t            isMaster  : 1;
    uint8_t            encrypted : 1;
    uint8_t            mitmSafe  : 1;

    uint8_t            reqIdent; /* ident for an outstanding request */

    l2c_handle_t       securityManager;      /* 0 if none yet */

    struct {
        union { /* LE-only data */
            l2cConnUpdateDoneCbk connUpdtDoneCbk;
            void                *connUpdtDoneCbkData;
            uint8_t              connUpdtRequestIdent; /* 0 if none inflight */
        };
        /* EDR-only data here, if any */
    };
};

struct l2cSigPduSendItem {
    struct l2cSigPduSendItem *next;
    struct l2cSigPduSendItem *prev;
    hci_conn_t                aclConnID;
    sg                        pdu;
    uint16_t                  cid;
    bool                      hasL2cHdr; /* if we've added a header already, we can no longer merge */
};

/* our work struct for service worker theread */
union l2cPsmOrChan {
    psm_t psm;
    uint16_t chan;
};

struct l2cSvcWork {
    int type;
    union {
        struct {
            l2c_handle_t handle;
            union l2cPsmOrChan which;
        } i_alloc;
        struct {
            l2c_handle_t handle;
            sg payload;
            uint32_t size;
            psm_t psm;
        } cl_rx;
        struct {
            union l2cPsmOrChan which;
            void (*doneCbk)(void*);
            void *cbkData;
        } unreg;
        struct {
            l2cStateCbk stateCbk;
            void *userData;
            void *instance;
            uint8_t state;
            uint32_t len;
        } state;
        struct {
            l2cConnUpdateDoneCbk doneCbk;
            void *cbkData;
            bool success;
        } connUpdtCbk;
        struct {
            struct bt_addr addr;
        } aclLink;
        struct {
            hci_conn_t aclConnID;
            uint16_t minInt;
            uint16_t maxInt;
            uint16_t lat;
            uint16_t to;
        } connUpdt;
        struct {
            hci_conn_t aclConnID;
            bool demandMitmSafe;
        } demandEncr;
        struct {
            bool useLeBuffers;
        } tryTx;
        struct {
            L2capDefaultSecurityManagerCbk cbk;
            hci_conn_t hciConn;
            uint64_t randomNum;
            uint16_t diversifier;
        } defaultSm;
        struct {
            sem_t *sem;
        } flush;
    };
};
#define L2C_WORK_PSM_INST_ALLOC          0
#define L2C_WORK_PSM_CONNECTIONLESS_RX   1
#define L2C_WORK_PSM_UNREGISTER          2 /* does not call service itself */
#define L2C_WORK_FIXEDCH_INST_ALLOC      3
#define L2C_WORK_FIXEDCH_UNREGISTER      4 /* does not call service itself */
#define L2C_WORK_STATE_CBK               5
#define L2C_WORK_CONN_UPDT_CBK           6
#define L2C_WORK_ACL_LINK_OPEN           7
#define L2C_WORK_ACL_LINK_CLOSE          8
#define L2C_WORK_ACL_CONN_UPDT           9
#define L2C_WORK_ACL_DEMAND_ENCR         10
#define L2C_WORK_TRY_HCI_TX              11
#define L2C_WORK_DEFAULT_SM_CALL         12
#define L2C_WORK_FLUSH                   13

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

struct l2cAdvFrag {
    sg                 data;
    uint8_t            sar;
};

/* our per-l2c connection struct */
/* also used for fixed ch "connections" */
struct l2cConn {
    uniq_t             handle;
    struct l2cConn    *next;
    struct l2cConn    *prev;
    uniq_t             timer;

    l2cStateCbk        stateCbk;
    void              *userData;
    void              *instance;

    uniq_t             acl; /* the acl connection */
    uint8_t            ident; /* for control channel ops */

    uint8_t            state;
    uint8_t            mode; /* L2C_MODE_* */

    psm_t              psm; /* 0 for fixedCh */
    uint16_t           localCh; /* both this & remCh ==0 means */
    uint16_t           remCh; /* this is connectionless conn struct */
    uint16_t           ourMtu;   /* for our side */
    uint16_t           theirMtu; /* for other side */

    uint8_t            weOpened : 1; /* 1 if we asked for this, 0 if peer */
    uint8_t            inUse    : 1; /* 1 if connection is in use, 0 else */
    uint8_t            fcs      : 1; /* 1 if FCS is on */

    sg                 queuedRx;

    union {            /* per-state info */
        struct {       /*  config */
            uint8_t cfgTxState;
            uint8_t cfgRxState;
        }cfg;
    };

    /* advanced mode structures for TX */
    struct l2cAdvFrag  advTxFrags[L2C_RETR_NUM_BUFFERS]; /* our tx buffers */
    sg                 advEnqueuedPacket;                /* may be more than a fragment long */
    uint8_t            advTxSendPos;                     /* where next send will come from */
    uint8_t            advTxWritePos;                    /* where the next write will go */
    uint8_t            advTxFirstUnacked;                /* what the first non-acked packet was */
    uint8_t            advTxWindowSz;                    /* how many outstanding not-acked packets there can be in our TX path */
    bool               advTxShutUp;                      /* set if peer wants us to stop sending *DATA* */
    bool               advTxWeOweAck;                    /* do we owe them an ack */
    uint16_t           advTxMps;                         /* max payload per fragment */

    /* advanced mode structures for RX */
    struct l2cAdvFrag  advRxFrags[L2C_RETR_NUM_BUFFERS]; /* our rx buffers */
    sg                 advTxReassBuf;                    /* our current work on reassembling a frame */
    uint16_t           advTxReassExpectLen;              /* expected extra length of packet we're reassembling */
    uint8_t            advRxFirstMissing;                /* first fragment we need */
    uint8_t            advRxWindowSz;                    /* how many outstanding not-acked packets there can be in our RX path */
    bool               advRxWeOweAck;                    /* do we owe them an ack */
};

#define L2C_CONN_STATE_CONN_WAIT       0 /* waiting for ACL link up */
#define L2C_CONN_STATE_RX_OPEN_SVC     1 /* call to service in pipeline */
#define L2C_CONN_STATE_TX_OPEN_TXED    2 /* request sent to other side */
#define L2C_CONN_STATE_ENCR_WAIT       3 /* waiting for encr/auth */
#define L2C_CONN_STATE_ATHORIZ_WAIT    4 /* waiting for user athorization */
#define L2C_CONN_STATE_CFG             5 /* configuration ongoing */
#define L2C_CONN_STATE_ESTABLISHED     6 /* connected */
#define L2C_CONN_STATE_DIE_REQD        7 /* we closed, call to svc in pipeline */
#define L2C_CONN_STATE_DIE_RXED        8 /* other side closed, call to svc in pipeline */
#define L2C_CONN_STATE_TEARDOWN        9 /* tearing down */

#define L2C_ACL_STATE_PENDING          0 /* not yet established */
#define L2C_ACL_STATE_CFG              1 /* we are still finding out peer config. do not yet start outbound connections */
#define L2C_ACL_STATE_ESTABLISHED      2 /* we can now start outbound connections */
#define L2C_ACL_STATE_TEARDOWN         3 /* going down - do not use */

#define L2C_CFG_STATE_INITIAL          0
#define L2C_CFG_STATE_REQ_SENT         1
#define L2C_CFG_STATE_CFG_ACCEPTED     2


/* work for seg send thread */
struct l2cSigFrameWork {
    hci_conn_t  aclConnID;
    void       *data;
    uint32_t    len;
    bool        isLE;
};

/* some fwd declarations */
static uint8_t l2cSigSendErrCmdNotUnderstood(struct l2cAclConn *aclConn, uint8_t ident);
static uint8_t l2cSigSendErrInvalidCid(struct l2cAclConn *aclConn, uint8_t ident, uint16_t rxedDcid, uint16_t rxedScid);
static uint16_t l2cAclFreeCid(struct l2cAclConn *aclConn);
static uint8_t l2cSigSendConnectionRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh, uint16_t result, uint16_t status);
static struct l2cConn* l2cConnFindByHandle(l2c_handle_t handle);
static void l2cConnStructDelete(struct l2cConn *conn);
static void l2cAclConnStructDelete(struct l2cAclConn *conn);
static void l2cPsmSendConfig(struct l2cAclConn *aclConn, struct l2cConn *conn);
static uint8_t l2cSigSendConfigReq(struct l2cAclConn *aclConn, uint16_t remCh, uint16_t flags, const void *cfgData, uint16_t cfgLen);
static uint8_t l2cSigSendConfigRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t flags, uint16_t result, const void *cfgData, uint16_t cfgLen);
static uint8_t l2cSigSendInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, const void *infoData, uint16_t infoLen);
static uint8_t l2cSigSendEchoRsp(struct l2cAclConn *aclConn, uint8_t ident, const void *echoData, uint16_t echoLen);
static uint8_t l2cSigSendConnUpdateRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t result);
static uint8_t l2cSigSendConnUpdateReq(struct l2cAclConn *aclConn, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeoutMult);
static uint8_t l2cSigSendDiscRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh);
static void l2cConnRequestClose(struct l2cAclConn* aclConn, struct l2cConn *conn, bool tellOtherSide);
static void l2cAclDownTimerStart(struct l2cAclConn *aclConn);
static void l2cAclDownTimerStop(struct l2cAclConn *aclConn);
static void l2cAclLinkDownInt(struct l2cAclConn *aclConn, bool tellOtherSide);
static void l2cPsmCfgStart(struct l2cAclConn *aclConn, struct l2cConn *conn);
static void l2cTryMakeSendProgress(bool forLeBuffers);
static uint8_t l2cSigSendInfoReq(struct l2cAclConn *aclConn, uint16_t infoType);
static uint16_t l2cUtilFcs(uint16_t fcs, const void *data, uint32_t len);
static uint16_t l2cUtilFcsSg(uint16_t fcs, sg s);
static bool l2cAdvMayTx(void);
static bool l2cAdvMayTxLocked(void);
static uint8_t l2cAdvUserTx(struct l2cConn *conn, sg data); /* -> L2C_TX_* */
static uint8_t l2cDataSendOnAclLink(struct l2cAclConn *aclConn, const void *hdr, uint32_t hdrLen, sg data);


/* our state */
static bool mChipJointBuffers;
static uint16_t mChipBufLenEdr;
static uint16_t mChipBufLenLe;
static uint16_t mChipBufNumEdr;
static uint16_t mChipBufNumLe;

static pthread_t mSvcWorker;
static struct workQueue *mServiceWork = NULL;

static pthread_mutex_t mSvcsLock = PTHREAD_MUTEX_INITIALIZER;
static struct l2cPsmSvc *mPsmSvcs = NULL;
static struct l2cFixedChSvc *mFixedSvcs = NULL;
static psm_t mNextPsm;

static pthread_mutex_t mConnsLock = PTHREAD_MUTEX_INITIALIZER;
static struct l2cAclConn *mAclConns = NULL;
static struct l2cConn *mConns = NULL;
static struct l2cConn *mNextToTx = NULL;
static sg mQueueBufLe = NULL;
static sg mQueueBufEdr = NULL;
static bool mQueueFirstLe;
static bool mQueueFirstEdr;
static hci_conn_t mQueueConnLe = 0;
static hci_conn_t mQueueConnEdr = 0;
static struct l2cSigPduSendItem *sigSendHeadEdr = NULL;
static struct l2cSigPduSendItem *sigSendTailEdr = NULL;
static struct l2cSigPduSendItem *sigSendHeadLe = NULL;
static struct l2cSigPduSendItem *sigSendTailLe = NULL;

static struct multiNotifList *mWriteNotif = NULL;

static pthread_mutex_t mSecMgrLock = PTHREAD_MUTEX_INITIALIZER;
static L2capDefaultSecurityManagerCbk mDefaultSecMgr = NULL;



/*
 * FUNCTION: l2cAclConnFindById
 * USE:      Find a l2cConn structure for a given ACL connection ID
 * PARAMS:   aclConn - the ACL connection ID
 * RETURN:   the found struct or null
 * NOTES:    requires mConnsLock held
 */
static struct l2cAclConn* l2cAclConnFindById(hci_conn_t aclConn)
{
    struct l2cAclConn *c = mAclConns;

    while (c && c->conn != aclConn)
        c = c->next;

    return c;
}

/*
 * FUNCTION: l2cAclConnFindByHandle
 * USE:      Find a l2cConn structure for a given a UniqID
 * PARAMS:   uniq - the UniqID
 * RETURN:   the found struct or null
 * NOTES:    requires mConnsLock held
 */
static struct l2cAclConn* l2cAclConnFindByHandle(uniq_t handle)
{
    struct l2cAclConn *c = mAclConns;

    while (c && c->handle != handle)
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

    while (c && memcmp(&c->peerAddr, addr, sizeof(struct bt_addr)))
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
 * FUNCTION: l2cSvcWorkAlloc
 * USE:      Allocate a work item for the service worker
 * PARAMS:   type - the type of work
 *           extraLen - how many bytes extra to allocate (for data past work struct)
 * RETURN:   work item or null
 * NOTES:
 */
static struct l2cSvcWork* l2cSvcWorkAlloc(int type, uint32_t extraLen)
{
    struct l2cSvcWork* w = (struct l2cSvcWork*)calloc(1, sizeof(struct l2cSvcWork) + extraLen);

    if (w)
        w->type = type;
    else
        loge("Failed to allocate service work item\n");

    return w;
}

/*
 * FUNCTION: l2cSvcWorkSchedulePsmAlloc
 * USE:      Schedule a service allocation
 * PARAMS:   psm - the psm which this is for
 *           handle - the handle to pass to the service alloc handler
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread
 */
static bool l2cSvcWorkSchedulePsmAlloc(psm_t psm, l2c_handle_t handle)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_PSM_INST_ALLOC, 0);

    if (!w)
        return false;

    w->i_alloc.handle = handle;
    w->i_alloc.which.psm = psm;

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
 *           handle - the handle to pass to the service RX connectionless handler
 *           payload - the data that arrived
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread. Buffer is copied
 */
static bool l2cSvcWorkSchedulePsmConnectionlessRx(psm_t psm, l2c_handle_t handle, sg payload)
{
    struct l2cSvcWork* w;

    w = l2cSvcWorkAlloc(L2C_WORK_PSM_CONNECTIONLESS_RX, 0);
    if (!w) {
        return false;
    }

    w->cl_rx.handle = handle;
    w->cl_rx.payload = payload;
    w->cl_rx.psm = psm;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service connectionless RX!\n");
    sgFree(payload);
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleFixedChAlloc
 * USE:      Schedule a service allocation
 * PARAMS:   chan - the channel this is for
 *           handle - the handle to pass to the service alloc handler
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread
 */
static bool l2cSvcWorkScheduleFixedChAlloc(uint16_t chan, l2c_handle_t handle)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_FIXEDCH_INST_ALLOC, 0);

    if (!w)
        return false;

    w->i_alloc.handle = handle;
    w->i_alloc.which.chan = chan;

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
 *           doneCbk - callback to call when done (or NULL)
 *           cbkData - data to pass to callback
 * RETURN:   false on immediate error
 * NOTES:    actual work will happen in the worker thread
 */
static bool l2cSvcWorkScheduleFixedChUnregister(uint16_t chan, void (*doneCbk)(void*), void *cbkData)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_FIXEDCH_UNREGISTER, 0);

    if (!w)
        return false;

    w->unreg.doneCbk = doneCbk;
    w->unreg.cbkData = cbkData;
    w->unreg.which.chan = chan;

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
 *           doneCbk - callback to call when done (or NULL)
 *           cbkData - data to pass to callback
 * RETURN:   false on immediate error
 * NOTES:    actual work will happen in the worker thread
 */
static bool l2cSvcWorkSchedulePsmUnregister(psm_t psm, void (*doneCbk)(void*), void *cbkData)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_PSM_UNREGISTER, 0);

    if (!w)
        return false;

    w->unreg.doneCbk = doneCbk;
    w->unreg.cbkData = cbkData;
    w->unreg.which.psm = psm;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for service unregistering!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleStateCallDirect
 * USE:      Schedule a "state" callback with directly-passed params
 * PARAMS:   stateCbk - the callback to call
 *           userData - data pointer to pass to the callback
 *           instance - second data pointer to pass to the callback
 *           state - state vaklue to pass to the callback
 *           data - buffer a copy of which is passed to the callback
 *           len - the length of the buffer
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread. cannot supply a buffer!
 */
static bool l2cSvcWorkScheduleStateCallDirect(l2cStateCbk stateCbk, void *userData, void *instance, uint8_t state, const void *data, uint32_t len)
{
    struct l2cSvcWork* w;

    w = l2cSvcWorkAlloc(L2C_WORK_STATE_CBK, len);
    if (!w)
        return false;

    w->state.stateCbk = stateCbk;
    w->state.userData = userData;
    w->state.instance = instance;
    w->state.state = state;
    w->state.len = len;

    memcpy(w + 1, data, len);

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for state call!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleStateCall
 * USE:      Schedule a "state" callback for a connection
 * PARAMS:   conn - the connection
 *           state - state vaklue to pass to the callback
 *           data - buffer a copy of which is passed to the callback
 *           len - the length of the buffer
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread. Buffer is copied
 */
static bool l2cSvcWorkScheduleStateCall(struct l2cConn* conn, uint8_t state, const void *data, uint32_t len)
{
    return l2cSvcWorkScheduleStateCallDirect(conn->stateCbk, conn->userData, conn->instance, state, data, len);
}

/*
 * FUNCTION: l2cSvcWorkScheduleConnUpdtCbk
 * USE:      Schedule a "connection update" callback
 * PARAMS:   doneCbk - the callback to call
 *           cbkData - data pointer to pass to the callback
 *           success - success sttaus to pass to the callback
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleConnUpdtCbk(l2cConnUpdateDoneCbk doneCbk, void *cbkData, bool success)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_CONN_UPDT_CBK, 0);
    if (!w)
        return false;

    w->connUpdtCbk.doneCbk = doneCbk;
    w->connUpdtCbk.cbkData = cbkData;
    w->connUpdtCbk.success = success;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for connection update callback!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleAclLinkOpen
 * USE:      Schedule an ACL link open call to the lower layer
 * PARAMS:   addr - the address of peer to connect to
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleAclLinkOpen(const struct bt_addr *addr)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_ACL_LINK_OPEN, 0);
    if (!w)
        return false;

    memcpy(&w->aclLink.addr, addr, sizeof(struct bt_addr));
    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL link open!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleAclLinkClose
 * USE:      Schedule an ACL link close call to the lower layer
 * PARAMS:   addr - the address of peer to disconnect from
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleAclLinkClose(const struct bt_addr *addr)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_ACL_LINK_CLOSE, 0);
    if (!w)
        return false;

    memcpy(&w->aclLink.addr, addr, sizeof(struct bt_addr));
    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL link close!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleAclConnUpdt
 * USE:      Schedule a connection update call to the lower layer
 * PARAMS:   aclConnID - the connection ID
 *           minInt - update command parameter
 *           maxInt - update command parameter
 *           lat - update command parameter
 *           to - update command parameter
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleAclConnUpdt(hci_conn_t aclConnID, uint16_t minInt, uint16_t maxInt, uint16_t lat, uint16_t to)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_ACL_CONN_UPDT, 0);
    if (!w)
        return false;

    w->connUpdt.aclConnID = aclConnID;
    w->connUpdt.minInt = minInt;
    w->connUpdt.maxInt = maxInt;
    w->connUpdt.lat = lat;
    w->connUpdt.to = to;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL connection update!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleAclDemandEncr
 * USE:      Schedule a connection encryption request
 * PARAMS:   aclConnID - the connection ID
 *           demandMitmSafe - demand authentication too?
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleAclDemandEncr(hci_conn_t aclConnID, bool demandMitmSafe)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_ACL_DEMAND_ENCR, 0);
    if (!w)
        return false;

    w->demandEncr.aclConnID = aclConnID;
    w->demandEncr.demandMitmSafe = demandMitmSafe;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL connection encprypt!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleTryTx
 * USE:      Schedule an attempt to do some TX
 * PARAMS:   useLeBuffers - which buffer set to try TX on
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleTryTx(bool useLeBuffers)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_TRY_HCI_TX, 0);
    if (!w)
        return false;

    w->tryTx.useLeBuffers = useLeBuffers;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL TX!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleDefaultSmCall
 * USE:      Schedule an attempt to call the dafult security manager
 * PARAMS:   mgrCbk - the manager to call
 *           hciConn - the hci acl connection
 *           randomNum - the random number gotten from peer
 *           diversifier - the diversifier gotten from peer
 * RETURN:   false on immediate error
 * NOTES:    actual call will happen in the worker thread.
 */
static bool l2cSvcWorkScheduleDefaultSmCall(L2capDefaultSecurityManagerCbk mgrCbk, hci_conn_t hciConn, uint64_t randomNum, uint16_t diversifier)
{
    struct l2cSvcWork* w = l2cSvcWorkAlloc(L2C_WORK_DEFAULT_SM_CALL, 0);
    if (!w)
        return false;

    w->defaultSm.cbk = mgrCbk;
    w->defaultSm.hciConn = hciConn;
    w->defaultSm.randomNum = randomNum;
    w->defaultSm.diversifier = diversifier;

    if (workQueuePut(mServiceWork, w))
        return true;

    loge("Failed to enqueue service work for ACL default SM call!\n");
    free(w);
    return false;
}

/*
 * FUNCTION: l2cSvcWorkScheduleFlush
 * USE:      Flush the workQueue
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:    blocks till all existing work items have been finished
 */
static bool l2cSvcWorkScheduleFlush(void)
{
    struct l2cSvcWork *w = l2cSvcWorkAlloc(L2C_WORK_FLUSH,0);
    bool ret = false;
    sem_t sem;

    if (!w)
        return false;

    if (sem_init(&sem, 0, 0)) {
        free(w);
        loge("Failed to alloc workQ flush sem\n");
        return false;
    }

    w->flush.sem = &sem;

    if (workQueuePut(mServiceWork, w)) {
        r_sem_wait(&sem);
        ret = true;
    } else
        free(w);

    sem_destroy(&sem);

    return ret;
}


/*
 * FUNCTION: l2cConnTimeout
 * USE:      Timer timeout callback for per-connection timers
 * PARAMS:   timerH - the timer handle
 *           connH - the connection handle
 * RETURN:   NONE
 * NOTES:
 */
static void l2cConnTimeout(uniq_t timerH,  uint64_t connH)
{
    struct l2cConn *conn;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(connH);
    if (!conn)
        logw("Timer callback "HANDLEFMT" for nonexistent connection "HANDLEFMT"\n", HANDLECNV(timerH), HANDLECNV(connH));
    else if (conn->timer != timerH)
        logd("Stale timer callback "HANDLEFMT" for connection "HANDLEFMT"\n", HANDLECNV(timerH), HANDLECNV(connH));
    else {
        /* We found a genuine timeout - figure out what kind by looking at state
         * Note that this is purely for logging purposes, since we handle them all
         * the same - by closing the connection.
         */
        switch (conn->state) {
        case L2C_CONN_STATE_RX_OPEN_SVC:
        case L2C_CONN_STATE_CONN_WAIT:
        case L2C_CONN_STATE_ESTABLISHED:
        case L2C_CONN_STATE_DIE_REQD:
        case L2C_CONN_STATE_DIE_RXED:
        case L2C_CONN_STATE_TEARDOWN:
            loge("Unexpected timeout in state %d for "HANDLEFMT" - closing connection\n", conn->state, HANDLECNV(connH));
            break;
        case L2C_CONN_STATE_TX_OPEN_TXED:
            logi("Connection open timeout for "HANDLEFMT" - closing connection\n", HANDLECNV(connH));
            break;
        case L2C_CONN_STATE_CFG:
            logi("Connection configuration timeout for "HANDLEFMT" - closing connection\n", HANDLECNV(connH));
            break;
        case L2C_CONN_STATE_ENCR_WAIT:
            logi("Connection open after auth timeout for "HANDLEFMT" - closing connection\n", HANDLECNV(connH));
            break;
        default:
            loge("Unknown state %d for "HANDLEFMT" at timeout time - closing connection\n", conn->state, HANDLECNV(connH));
            break;
        }
        l2cConnRequestClose(NULL, conn, true);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cConnTimer
 * USE:      Set or clear a per-L2C-connection timer
 * PARAMS:   conn - the connection
 *           timeout - time to set ot 0 to clear
 * RETURN:   NONE
 * NOTES:    call mConnsLock held
 */
static void l2cConnTimer(struct l2cConn *conn, uint64_t timeout)
{
    if (conn->timer) {
        logd("Removing existing timer "HANDLEFMT" for conn "HANDLEFMT"\n", HANDLECNV(conn->timer), HANDLECNV(conn->handle));
        timerCancel(conn->timer);
        conn->timer = 0;
    }
    if (timeout) {
        logd("Setting a timer for %llu ms for conn "HANDLEFMT"\n", (unsigned long long)timeout, HANDLECNV(conn->handle));
        conn->timer = timerSet(timeout, l2cConnTimeout, conn->handle);
        if (!conn->timer)
            loge("Failed to set per-conn timer\n");
    }
}

/*
 * FUNCTION: l2cNewConnStruct
 * USE:      Create a new l2cConn and prepopulate with some  values
 * PARAMS:   acl - acl connection handle
 *           state - state value to set
 *           localCh - localCh value to set
 *           remCh - remCh value to set
 *           psm - psm value to set
 *           ourMtu - the MTU we're willing to accept
 *           remCh - the remote channel used to make the request
 *           weOpened - true if we're opening this conn, false if peer
 * RETURN:   The struct or NULL on error
 * NOTES:
 */
static struct l2cConn* l2cNewConnStruct(uint8_t state, uniq_t acl, uint16_t localCh, uint16_t remCh, psm_t psm, uint16_t ourMtu, bool weOpened)
{
    struct l2cConn* conn = (struct l2cConn*)calloc(1, sizeof(struct l2cConn));

    if (!conn)
        return NULL;

    conn->acl = acl;
    conn->psm = psm;
    conn->theirMtu = 0xFFFF;
    conn->ourMtu = ourMtu;
    conn->handle = uniqGetNext();
    conn->remCh = remCh;
    conn->localCh = localCh;
    conn->state = state;
    conn->weOpened = weOpened ? 1 : 0;
    conn->timer = 0;

    return conn;
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

    conn = l2cNewConnStruct(L2C_CONN_STATE_RX_OPEN_SVC, aclConn->handle, 0, remCh, psm, svc->desc.mtu, false);
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        goto out_svcs_lock;
    }
    localCh = l2cAclFreeCid(aclConn);
    if (!localCh) {
        logw("No free channels for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
        free(conn);
        goto out_svcs_lock;
    }
    conn->localCh = localCh;
    conn->ident = ident;
    conn->inUse = 1;
    l2cAclDownTimerStop(aclConn);

    /* safe to call this enqueue here, since worker thread cannot work on it till we release this lock */
    enqueued = l2cSvcWorkSchedulePsmAlloc(psm, conn->handle);
    if (enqueued) {
        conn->next = mConns;
        if (mConns)
            mConns->prev = conn;
        mConns = conn;
    } else {
        free(conn);
    }

out_svcs_lock:
    pthread_mutex_unlock(&mSvcsLock);

out_nolocks:
    if (!svc) {
        result = L2C_SIG_CONN_RSP_BAD_PSM;
        logi("Request for connect to unknown psm %d\n", psm);
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
 * NOTES:    call with mConnsLock held. May destroy your conn
 */
static void l2cHandleConnRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t localCh, uint16_t result, uint16_t status)
{
    struct l2cConn *conn = NULL;


    conn = mConns;
    while (conn && (conn->acl != aclConn->handle || conn->localCh != localCh))
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
        if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    l2cConnTimer(conn, 0);

    if (result == L2C_SIG_CONN_RSP_BAD_PSM || result == L2C_SIG_CONN_RSP_BAD_SEC || result == L2C_SIG_CONN_RSP_NO_RSRC) {
        logi("Other side refused our connection for psm %d\n", conn->psm);
        if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    if (result == L2C_SIG_CONN_RSP_PEND) {
        logd("Connection pending for reason %d\n", status);
        l2cConnTimer(conn, L2C_L2CAP_OPEN_PEND_TIMEOUT);
        /* stay in current state until next reply received */
        return;
    }

    if (result != L2C_SIG_CONN_RSP_OK) {

        loge("Unexpected result %d for connection to psm %d\n", result, conn->psm);
        if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to schedule 'closed' state call\n");
        l2cConnStructDelete(conn);
        return;
    }

    conn->state = L2C_CONN_STATE_CFG;
    l2cConnTimer(conn, L2C_L2CAP_CONFIG_TIMEOUT);
    conn->cfg.cfgTxState = L2C_CFG_STATE_INITIAL;
    conn->cfg.cfgRxState = L2C_CFG_STATE_INITIAL;
    conn->remCh = remCh;

    l2cPsmSendConfig(aclConn, conn);
}

/*
 * FUNCTION: l2cPsmSendConfig
 * USE:      RStart a config negotiation with a peer for a new PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           conn - the l2c connection struct
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held, when *first* entering config state. May destroy your conn
 */
static void l2cPsmSendConfig(struct l2cAclConn *aclConn, struct l2cConn *conn)
{
    /* for now our hardwired config is a large MTU */
    uint8_t ident;
    uint8_t config[] = {
        L2C_CONF_MTU, 0x02, conn->ourMtu & 0xFF, conn->ourMtu >> 8
    };

    if (conn->cfg.cfgTxState != L2C_CFG_STATE_INITIAL) {
        loge("Unexpectred call to l2cPsmSendConfig with state %d\n", conn->cfg.cfgTxState);
        return;
    }

    ident = l2cSigSendConfigReq(aclConn, conn->remCh, 0, config, sizeof(config));
    if (ident) {
        conn->ident = ident;
        conn->cfg.cfgTxState = L2C_CFG_STATE_REQ_SENT;
    } else {
        loge("Failed to send config request on conn for chs %d %d on acl "HCI_CONN_FMT"x\n", conn->localCh, conn->remCh, HCI_CONN_CONV(aclConn->conn));
        l2cConnRequestClose(aclConn, conn, true);
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
    uint32_t len;
    uint16_t mtu = conn->theirMtu;

    if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_OPEN, &conn->handle, sizeof(l2c_handle_t))) {
        loge("Failed to send open call\n");
        l2cConnRequestClose(aclConn, conn, true);
        return;
    }

    /* this failure is non-fatal. they can just ask later */
    if (conn->psm && !l2cSvcWorkScheduleStateCall(conn, L2C_STATE_MTU, &mtu, sizeof(mtu)))
        logw("Failed to notify user of MTU\n");

    /* send all queued data, if any */
    if (conn->queuedRx) {

        sg packet;

        while (sgLength(conn->queuedRx) >= sizeof(uint32_t)) {
            if (!sgSerializeCutFront(conn->queuedRx, &len, sizeof(len))) {
                loge("Failed to grab queued packet len\n");
                break;
            }
            if (sgLength(conn->queuedRx) < len) {
                loge("Queued packet claims %ub, left %ub\n", len, sgLength(conn->queuedRx));
                break;
            }
            packet = sgSplit(conn->queuedRx, len);
            if (!packet) {
                loge("Failed to split off packet\n");
                break;
            }
            sgSwap(packet, conn->queuedRx);
            if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_RX, &packet, sizeof(packet))) {
                loge("Failed to send queued packet\n");
                break;
            }
        }
        if (sgLength(conn->queuedRx))
            loge("QueuedRX had leftover data!\n");
        sgFree(conn->queuedRx);
        conn->queuedRx = NULL;
    }

    conn->state = L2C_CONN_STATE_ESTABLISHED;
}

/*
 * FUNCTION: l2cHandleConfReq
 * USE:      Handle a configuration request for a PSM channel
 * PARAMS:   aclConn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           localCh -local channel number
 *           flags - the flags the remote side sent
 *           payload - the conf payload
 *           len - the length of the abovementioned payload
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConfReq(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t flags, sg payload, uint16_t len)
{
    struct l2cConn *conn = NULL;
    uint8_t *reply, *end;
    uint16_t result = L2C_SIG_CONF_RSP_OK;
    uint32_t offset = 0;

    conn = mConns;
    while (conn && (conn->acl != aclConn->handle || conn->localCh != localCh))
        conn = conn->next;

    if (!conn) {
        logw("Unexpected Configuration Request id %d for ch %d with result %d\n", ident, localCh, result);
        if (!l2cSigSendErrInvalidCid(aclConn, ident, localCh, 0))
            loge("Failed to send 'invalid cid' reply\n");
        return;
    }

    if (!conn->psm) {
        logw("Unexpected Configuration Request for a FixedCh conn chan %d\n", localCh);
        return;
    }

    if (conn->state != L2C_CONN_STATE_CFG) {
        logw("Configuration Request for connection in bad state %d for ch %d with flags %d\n", conn->state, localCh, flags);
        return;
    }

    if (conn->cfg.cfgRxState != L2C_CFG_STATE_INITIAL)
        loge("Unexpected internal inconsistency on conf req: expected %d saw %d\n", L2C_CFG_STATE_INITIAL, conn->cfg.cfgRxState);

    /* go through every option and make up a reply. Note: no reply is longer than request, so this allocation works */
    end = reply = (uint8_t*)malloc(len);
    if (!reply) {
        loge("Cannot allocate a conf reply\n");
        return;
    }

    while (len > 2) {
        uint8_t optType;
        uint8_t optLen;
        uint32_t optOfst;

        sgSerialize(payload, offset++, 1, &optType);
        sgSerialize(payload, offset++, 1, &optLen);

        len -= 2;

        if (optLen > len) {
            logw("Got invalid conf req option len %d/%d\n", optLen, len);
            optLen = len;
            break;
        }
        optOfst = offset;
        offset += optLen;
        len -= optLen;

        switch (optType){
        case L2C_CONF_MTU:
            if (optLen != sizeof(uint16_t)) {
                logi("other side sent bad sized mtu conf: %db\n", optLen);
                result = L2C_SIG_CONF_RSP_REJECTED;
            } else {
                utilSetLE8(end + 0, L2C_CONF_MTU);
                utilSetLE8(end + 1, sizeof(uint16_t));
                sgSerialize(payload, optOfst, sizeof(uint16_t), end + 2); /* copy their mtu */
                conn->theirMtu = utilGetLE16(end + 2);
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
                sgSerialize(payload, optOfst, sizeof(uint16_t), end + 2); /* copy the timeout */
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
                sgSerialize(payload, optOfst, sizeof(struct l2cConfQosOpts), qosOut);

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
        return;
    }

    if (!l2cSigSendConfigRsp(aclConn, ident, conn->remCh, flags, result, reply, end - reply))
        loge("Failed to sent configuration request response frame\n");
    free(reply);

    if (flags & L2C_SIG_CONF_FLAG_C)
        logi("Conf Rsp continues\n");
    else {
        conn->cfg.cfgRxState = L2C_CFG_STATE_CFG_ACCEPTED;
        if (conn->cfg.cfgTxState == L2C_CFG_STATE_CFG_ACCEPTED) {
            l2cConnTimer(conn, 0);
            l2cChannelReadyForData(aclConn, conn);
        }
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
 *           payload - the payload from the remote side
 *           len - the length of the abovementioned payload
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConfRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t flags, uint16_t result, sg payload, uint16_t len)
{
    struct l2cConn *conn = NULL;

    conn = mConns;
    while (conn && (conn->acl != aclConn->handle || conn->localCh != localCh))
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
        return;
    }

    if (result == L2C_SIG_CONF_RSP_UNACCEPTABLE || result == L2C_SIG_CONF_RSP_REJECTED ||
        result == L2C_SIG_CONF_RSP_UNKNOWN_OPTN || result == L2C_SIG_CONF_RSP_PENDING ||
        result == L2C_SIG_CONF_RSP_FLOW_SPEC_REJ) {

        logi("Other side refused our configuration for psm %d\n", conn->psm);
        l2cConnRequestClose(aclConn, conn, true);
        return;
    }

    if (result != L2C_SIG_CONF_RSP_OK) {

        loge("Unexpected result %d for connection to psm %d\n", result, conn->psm);
        l2cConnRequestClose(aclConn, conn, true);
        return;
    }

    if (conn->cfg.cfgTxState != L2C_CFG_STATE_REQ_SENT)
        loge("Unexpected internal inconsistency on conf rsp: expected %d saw %d\n", L2C_CFG_STATE_REQ_SENT, conn->cfg.cfgTxState);

    if (flags & L2C_SIG_CONF_FLAG_C)
        logi("Conf Rsp continues\n");
    else {
        conn->cfg.cfgTxState = L2C_CFG_STATE_CFG_ACCEPTED;
        if (conn->cfg.cfgRxState == L2C_CFG_STATE_CFG_ACCEPTED) {
            l2cChannelReadyForData(aclConn, conn);
            l2cConnTimer(conn, 0);
        }
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
 * USE:      Handle an incoming information response, enqueue the next one we need, if we need one
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           infoType - the requested info type
 *           result - the result of the request
 *           payload - the returned data
 *           infoLen - the returned data's length
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, sg payload, uint32_t infoLen)
{
    uint8_t buf[8];

    if (aclConn->state != L2C_ACL_STATE_CFG)
        logw("ACL conn in state %u when RXing info RSPs\n", aclConn->state);

    if (ident != aclConn->reqIdent) {
        logw("Got info resp with mismatching ident (got %d wanted %d)\n", ident, aclConn->reqIdent);
        return;
    }
    aclConn->reqIdent = 0;

    switch (infoType) {
    case L2C_SIG_INFO_TYPE_UNCONN_MTU:
        aclConn->connectionlessMtu = 0;
        if (result == L2C_SIG_INFO_RESULT_OK && sgSerializeCutFront(payload, buf, sizeof(uint16_t)))
            aclConn->connectionlessMtu = utilGetLE16(buf);
        else
            logi("Peer did not properly tell us the connectionless MTU. Asuming not supported\n");
        infoType = L2C_SIG_INFO_TYPE_EXT_FTRS;
        break;
    case L2C_SIG_INFO_TYPE_EXT_FTRS:
        aclConn->peerFeatures = 0;
        if (result == L2C_SIG_INFO_RESULT_OK && sgSerializeCutFront(payload, buf, sizeof(uint32_t)))
            aclConn->peerFeatures = utilGetLE32(buf);
        else
            logi("Peer did not properly tell us supported features. Asuming none.\n");

        /* if peer supports fixed channels, find out which */
        infoType = (aclConn->peerFeatures & L2C_FTR_FIXED_CHS) ? L2C_SIG_INFO_TYPE_FIXED_CHS : 0;
        break;
    case L2C_SIG_INFO_TYPE_FIXED_CHS:
        aclConn->peerFixedChs = 0;
        if (result == L2C_SIG_INFO_RESULT_OK && sgSerializeCutFront(payload, buf, sizeof(uint64_t)))
            aclConn->peerFixedChs = utilGetLE64(buf);
        else
            logi("Peer did not properly tell us supported fixed channels. Asuming none.\n");
        if (aclConn->state == L2C_ACL_STATE_CFG)
            aclConn->state = L2C_ACL_STATE_ESTABLISHED;
        infoType = 0;
        break;
    default:
        logw("Unknown info type 0x%04X RXed. Ignoring\n", infoType);
        infoType = 0;
        break;
    }

    if (infoType) {
        aclConn->reqIdent = l2cSigSendInfoReq(aclConn, infoType);
        if (!aclConn->reqIdent) {
            aclConn->state = L2C_ACL_STATE_ESTABLISHED;
            logw("Failed to send req for info %u for acl "HCI_CONN_FMT"x. It will not be known\n", infoType, HCI_CONN_CONV(aclConn->conn));
        }
    }
}

/*
 * FUNCTION: l2cHandleEchoReq
 * USE:      Handle an incoming echo request
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           payload - the data we got
 *           echoLen - the len we got
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleEchoReq(struct l2cAclConn *aclConn, uint8_t ident, sg payload, uint32_t echoLen)
{
    logi("Got echo packet with %db of data\n", echoLen);

    /* no data in reply is valid as per spec */
    if (!l2cSigSendEchoRsp(aclConn, ident, NULL, 0))
        loge("Failed to sent echo response frame\n");
}

/*
 * FUNCTION: l2cHandleConnUpdtReq
 * USE:      Process an incoming connection update request
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           intMin - minimum requester interval
 *           intMax - maximum requested interval
 *           latency - requested latency
 *           timeoutMult - requester timeout multiplier
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:    NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConnUpdtReq(struct l2cAclConn *conn, uint8_t ident, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeoutMult)
{
    /* verify intervals are in valid range */
    if (intMin < BT_LE_CONN_INTERVAL_MIN || intMax < intMin || intMax > BT_LE_CONN_INTERVAL_MAX)
        goto error;

    /* verify latency and to params are in valid ranges */
    if (latency > BT_LE_LATENCY_MAX || timeoutMult < BT_LE_TIMEOUT_MIN || timeoutMult > BT_LE_TIMEOUT_MAX)
        goto error;

    /* verify timout meets BTLE criteria for valid timeout given latency & max interval */
    if (timeoutMult < BT_LE_TIMEOUT_MIN_VALID(intMin, latency))
        goto error;

    /* see if we're the master on this cunnection at all */
    if (!conn->isMaster)
        goto error;

    /* see if another is inflight for this ACL connection and reject if so */
    if (conn->connUpdtRequestIdent)
        goto error;

    /* try to execute it */
    if (!l2cSvcWorkScheduleAclConnUpdt(conn->conn, intMin, intMax, latency, timeoutMult))
        loge("Failed to schedule connection update work call\n");
    else
        return;


error:
    logw("Unable to request connection update to {[%d, %d], %d, %d}, inflight=%d master=%d\n",
        intMin, intMax, latency, timeoutMult, conn->connUpdtRequestIdent, conn->isMaster);
    if (!l2cSigSendConnUpdateRsp(conn, ident, L2C_SIG_CONN_UPDT_NO))
        loge("Failed to sent connection update response frame\n");
}

/*
 * FUNCTION: l2cHandleConnUpdtReq
 * USE:      Process an incoming connection update response
 * PARAMS:   conn - the per-ACL-connection structure
 *           ident - the ident in the response packet
 *           result - success or failure of our request
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:    NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cHandleConnUpdtRsp(struct l2cAclConn *conn, uint8_t ident, uint16_t result)
{
    if (conn->isMaster) /* this shouldnt happen if we're master */
        loge("Unexpected connection update (ac master) on acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(conn->conn));
    else if (!conn->connUpdtRequestIdent) /* if we made no request for this, - error */
        loge("Unexpected connection update response on acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(conn->conn));
    else {
        conn->connUpdtRequestIdent = 0;
        if (conn->connUpdtDoneCbk) {
            if (!l2cSvcWorkScheduleConnUpdtCbk(conn->connUpdtDoneCbk, conn->connUpdtDoneCbkData, result == L2C_SIG_CONN_UPDT_OK))
                loge("Failed to schedule connection update callback\n");
            conn->connUpdtDoneCbk = NULL;
        }
    }
}

/*
 * FUNCTION: l2cApiUpdateConnectionParams
 * USE:      Try to update params on an LE link
 * PARAMS:   handle - the handle
 *           intMin - minimum requester interval
 *           intMax - maximum requested interval
 *           latency - requested latency
 *           timeoutMult - requester timeout multiplier
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:    NONE
 */
bool l2cApiUpdateConnectionParams(l2c_handle_t handle, uint16_t intMin, uint16_t intMax, uint16_t latency, uint16_t timeout, l2cConnUpdateDoneCbk doneCbk, void *userData)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    /* verify intervals are in valid range */
    if (intMin < BT_LE_CONN_INTERVAL_MIN || intMax < intMin || intMax > BT_LE_CONN_INTERVAL_MAX)
        return false;

    /* verify latency and to params are in valid ranges */
    if (latency > BT_LE_LATENCY_MAX || timeout < BT_LE_TIMEOUT_MIN || timeout > BT_LE_TIMEOUT_MAX)
        return false;

    /* verify timout meets BTLE criteria for valid timeout given latency & max interval */
    if (timeout < BT_LE_TIMEOUT_MIN_VALID(intMin, latency))
        return false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (!BT_ADDR_IS_LE(aclConn->peerAddr)) {
        logw("Got a Connection Update Request API call on a non-LE link\n");
        goto out;
    }

    if (aclConn->isMaster) {
        aclConn->connUpdtDoneCbk = doneCbk;
        aclConn->connUpdtDoneCbkData = userData;
        ret = l2cSvcWorkScheduleAclConnUpdt(aclConn->conn, intMin, intMax, latency, timeout);
        if (!ret)
            loge("Failed to schedule connection update work\n");
    }
    else if (aclConn->connUpdtRequestIdent)
        logw("Connection update request while another still in progress on acl "HCI_CONN_FMT"x. Ignored\n", HCI_CONN_CONV(aclConn->conn));
    else {
        uint8_t ident;

        aclConn->connUpdtDoneCbk = doneCbk;
        aclConn->connUpdtDoneCbkData = userData;

        ident = l2cSigSendConnUpdateReq(aclConn, intMin, intMax, latency, timeout);
        if (!ident)
            loge("Failed to send connection update request for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
        else
            aclConn->connUpdtRequestIdent = ident;
    }

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiIsConnEncrypted
 * USE:      Check if connection is encrypted
 * PARAMS:   handle - the handle
 * RETURN:   true if exists and encrypted, false else
 * NOTES:    NONE
 */
bool l2cApiIsConnEncrypted(l2c_handle_t handle)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    ret = aclConn->encrypted;

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiIsConnMitmSafe
 * USE:      Check if connection is encrypted using a MITM-safe key
 * PARAMS:   handle - the handle
 * RETURN:   true if exists and encrypted, false else
 * NOTES:    NONE
 */
bool l2cApiIsConnMitmSafe(l2c_handle_t handle)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    ret = aclConn->mitmSafe;

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiDemandEncryption
 * USE:      Demand a connection be encrypted
 * PARAMS:   handle - the handle
 *           demandMitmSafe - shoudl it be MITM-safe?
 * RETURN:   true if request taken or already encrypted, false else
 * NOTES:    NONE
 */
bool l2cApiDemandEncryption(l2c_handle_t handle, bool demandMitmSafe)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (aclConn->encrypted && (aclConn->mitmSafe || !demandMitmSafe))
        logd("Ignoring redundant encr request\n");
    else if (!l2cSvcWorkScheduleAclDemandEncr(aclConn->conn, demandMitmSafe)) {
        loge("Failed to demand encryption/authentication by api call\n");
        goto out;
    }

    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiUserAthorizNotify
 * USE:      Notify other size of successful (or not) user athorization.
 * PARAMS:   handle - the handle
 * RETURN:   true if request taken or already encrypted, false else
 * NOTES:    NONE
 */
bool l2cApiUserAthorizNotify(l2c_handle_t handle, bool authorized)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        logi("Connection not found for authorization\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (conn->state != L2C_CONN_STATE_ATHORIZ_WAIT) {
        loge("Authorization notification for connection in wrong state\n");
        goto out;
    }

    if (authorized) {
        if (conn->psm) {

            if (!l2cSigSendConnectionRsp(aclConn, conn->ident, conn->localCh, conn->remCh, L2C_SIG_CONN_RSP_OK, 0)) {
                loge("Failed to sent positive connection request response frame for athoriz - dropping connection\n");
                l2cConnRequestClose(aclConn, conn, true);
                goto out;
            }

            logd("config start for PSM %d up after athoriz\n", conn->psm);
            l2cPsmCfgStart(aclConn, conn);
        } else {
            /* handle FixedCh */

            logd("Service up for FixedCh %d up after athoriz\n", conn->localCh);
            l2cChannelReadyForData(aclConn, conn);
        }
    } else {
        logd("Closing conn due to lack of user athorization\n");
        l2cConnRequestClose(aclConn, conn, true);
    }

    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cHandleDiscReq
 * USE:      Handle a disconnect request from peer
 * PARAMS:   handle - the handle
 *           ident - the ident in the response packet
 *           localCh - local channel number
 *           remCh - remote channel number
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
void l2cHandleDiscReq(struct l2cAclConn *aclConn, uint8_t ident, uint16_t localCh, uint16_t remCh)
{
    struct l2cConn *conn = NULL;

    conn = mConns;
    while (conn && (conn->acl != aclConn->handle || conn->localCh != localCh || conn->remCh != remCh))
        conn = conn->next;

    if (!conn) {
        logw("Unexpected Disconnection Request id %d for ch %d %d\n", ident, localCh, remCh);
        if (!l2cSigSendErrInvalidCid(aclConn, ident, localCh, remCh))
            loge("Failed to send 'invalid cid' reply\n");
        return;
    }

    if (!conn->psm) {
        logw("Unexpected Disconnection Request for a FixedCh conn chan %d %d\n", localCh, remCh);
        return;
    }

    l2cConnRequestClose(aclConn, conn, false);

    if (!l2cSigSendDiscRsp(aclConn, ident, localCh, remCh))
        loge("Failed to sent disconnect request response frame\n");
}

/*
 * FUNCTION: l2cHandleDiscRsp
 * USE:      Handle a disconnect response from peer
 * PARAMS:   handle - the handle
 *           ident - the ident in the response packet
 *           localCh - local channel number
 *           remCh - remote channel number
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
void l2cHandleDiscRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t localCh)
{
    /* We do not care about this, really. We commit to the disconnect whenwe send the request */
    logd("Disconnect response from acl "HCI_CONN_FMT"x for ch %d %d\n", HCI_CONN_CONV(aclConn->conn), localCh, remCh);
}

/*
 * FUNCTION: l2cSignallingRx
 * USE:      Process arriving data over signalling channels
 * PARAMS:   conn - the per-ACL-connection structure
 *           payload - the signalling packet payload
 * RETURN:   num bytes used/needed or 0 for error
 * NOTES:    call with mConnsLock held
 */
static void l2cSignallingRx(struct l2cAclConn *conn, sg payload)
{
    struct l2cSigFrm sig;

    while (sgLength(payload) >= sizeof(struct l2cSigFrm)) {
        uint8_t code, ident;
        bool cmdParamsSkipped = false;
        uint16_t len;

        sgSerialize(payload, 0, sizeof(struct l2cSigFrm), &sig);
        sgTruncFront(payload, sizeof(struct l2cSigFrm));

        code = utilGetLE8(&sig.code);
        ident = utilGetLE8(&sig.ident);
        len = utilGetLE16(&sig.len);

        if (!ident)
            logw("Got signalling ident 0. Dropping\n");
        else switch (code){
        case L2C_SIG_CMD_REJ: {
            struct l2cSigCmdRej params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (len < sizeof(params))
                logw("Got a short Command Reject packet\n");
            else
                logw("Got a reject for id %d with reason %d\n", ident, utilGetLE16(&params.reason));
            break;
        }
        case L2C_SIG_CONN_REQ: {
            struct l2cSigConnReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Connection Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Connection Request packet\n");
            else {
                psm_t psm = utilGetLE16(&params.psm);
                uint16_t scid = utilGetLE16(&params.scid);

                l2cHandleConnReq(conn, ident, psm, scid);
            }
            break;
        }
        case L2C_SIG_CONN_RSP: {
            struct l2cSigConnRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Connection Response packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Connection Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params.dcid);
                uint16_t scid = utilGetLE16(&params.scid);
                uint16_t result = utilGetLE16(&params.result);
                uint16_t status = utilGetLE16(&params.status);

                l2cHandleConnRsp(conn, ident, dcid, scid, result, status);
            }
            break;
        }
        case L2C_SIG_CONF_REQ: {
            struct l2cSigConfReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Configuration Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Configuration Request packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params.dcid);
                uint16_t flags = utilGetLE16(&params.flags);

                sgTruncFront(payload, sizeof(params));
                l2cHandleConfReq(conn, ident, dcid, flags, payload, len - sizeof(params));
                sgTruncFront(payload, len - sizeof(params));
                cmdParamsSkipped = true;
            }
            break;
        }
        case L2C_SIG_CONF_RSP: {
            struct l2cSigConfRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Configuration Response packet on a non-EDR link\n");
               if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Configuration Response packet\n");
            else {
                uint16_t scid = utilGetLE16(&params.scid);
                uint16_t flags = utilGetLE16(&params.flags);
                uint16_t result = utilGetLE16(&params.result);

                sgTruncFront(payload, sizeof(params));
                l2cHandleConfRsp(conn, ident, scid, flags, result, payload, len - sizeof(params));
                sgTruncFront(payload, len - sizeof(params));
                cmdParamsSkipped = true;
            }
            break;
        }
        case L2C_SIG_DISC_REQ: {
            struct l2cSigDiscReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Disconnect Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Disconnect Request packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params.dcid);
                uint16_t scid = utilGetLE16(&params.scid);

                l2cHandleDiscReq(conn, ident, dcid, scid);
            }
            break;
        }
        case L2C_SIG_DISC_RSP: {
            struct l2cSigDiscRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Disconnect Response packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Disconnect Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params.dcid);
                uint16_t scid = utilGetLE16(&params.scid);

                l2cHandleDiscRsp(conn, ident, dcid, scid);
            }
            break;
        }
        case L2C_SIG_ECHO_REQ: {
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Echo Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else
                l2cHandleEchoReq(conn, ident, payload, len);
            break;
        }

/*      case L2C_SIG_ECHO_RSP: is not needed (we do not ping anyone) */

        case L2C_SIG_INFO_REQ: {
            struct l2cSigInfoReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Info Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Info Request packet\n");
            else {
                uint16_t infoType = utilGetLE16(&params.infoType);

                l2cHandleInfoReq(conn, ident, infoType);
            }
            break;
        }
        case L2C_SIG_INFO_RSP: {
            struct l2cSigInfoRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Info Response packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Info Response packet\n");
            else {
                uint16_t infoType = utilGetLE16(&params.infoType);
                uint16_t result = utilGetLE16(&params.result);

                sgTruncFront(payload, sizeof(params));
                l2cHandleInfoRsp(conn, ident, infoType, result, payload, len - sizeof(params));
                sgTruncFront(payload, len - sizeof(params));
                cmdParamsSkipped = true;
            }
            break;
        }
/*
        case L2C_SIG_CH_CREAT_REQ: {
            struct l2cSigChCreatReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Create Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Create Request packet\n");
            else {
                psm_t psm = utilGetLE16(&params.psm);
                uint16_t scid = utilGetLE16(&params.scid);
                uint8_t controller = utilGetLE8(&params.controller);

                l2cHandleChCreatReq(conn, ident, psm, scid, controller);
            }
            break;
        }
        case L2C_SIG_CH_CREAT_RSP: {
            struct l2cSigChCreatRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Create Response packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Create Response packet\n");
            else {
                uint16_t dcid = utilGetLE16(&params.dcid);
                uint16_t scid = utilGetLE16(&params.scid);
                uint16_t result = utilGetLE16(&params.result);
                uint16_t status = utilGetLE16(&params.status);

                l2cHandleChCreatRsp(conn, ident, dcid, scid, result, status);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_REQ: {
            struct l2cSigChMoveReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Move Request packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Move Request packet\n");
            else {
                uint16_t icid = utilGetLE16(&params.icid);
                uint8_t controller = utilGetLE8(&params.controller);

                l2cHandleChMoveReq(conn, ident, icid, controller);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_RSP: {
            struct l2cSigChMoveRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Move Response packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Move Response packet\n");
            else {
                uint16_t icid = utilGetLE16(&params.icid);
                uint16_t result = utilGetLE8(&params.result);

                l2cHandleChMoveRsp(conn, ident, icid, result);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_CNF_REQ: {
            struct l2cSigChMoveCnfReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Move Confirmation packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Move Confirmation packet\n");
            else {
                uint16_t icid = utilGetLE16(&params.icid);
                uint16_t result = utilGetLE8(&params.result);

                l2cHandleChMoveCnfReq(conn, ident, icid, result);
            }
            break;
        }
        case L2C_SIG_CH_MOVE_CNF_RSP: {
            struct l2cSigChMoveCnfRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Channel Move Confirmation Response "
                    "packet on a non-EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Channel Move Confirmation Response packet\n");
            else {
                uint16_t icid = utilGetLE16(&params.icid);

                l2cHandleChMoveCnfRsp(conn, ident, icid);
            }
            break;
        }
*/
        case L2C_SIG_CONN_UPDT_REQ: {
            struct l2cSigConnUpdtReq params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_EDR(conn->peerAddr)) {
                logw("Got a Connection Update Request packet on an EDR link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Connection Update Request packet\n");
            else {
                uint16_t intMin = utilGetLE16(&params.intMin);
                uint16_t intMax = utilGetLE16(&params.intMax);
                uint16_t latency = utilGetLE16(&params.latency);
                uint16_t timeoutMult = utilGetLE16(&params.timeoutMult);

                l2cHandleConnUpdtReq(conn, ident, intMin, intMax, latency, timeoutMult);
            }
            break;
        }
        case L2C_SIG_CONN_UPDT_RSP: {
            struct l2cSigConnUpdtRsp params;

            sgSerialize(payload, 0, sizeof(params), &params);
            if (!BT_ADDR_IS_LE(conn->peerAddr)) {
                logw("Got a Connection Update Response packet on a non-LE link\n");
                if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                    loge("Failed to send command not understood msg to the other side\n");
            } else if (len < sizeof(params))
                logw("Got a short Connection Update Response packet\n");
            else {
                uint16_t result = utilGetLE8(&params.result);

                l2cHandleConnUpdtRsp(conn, ident, result);
            }
            break;
        }
        default: {
            logw("Got unknown code %d on signalling channel\n", code);
            if (!l2cSigSendErrCmdNotUnderstood(conn, ident))
                loge("Failed to send command not understood msg to the other side\n");
        }
        }

        /* truncate off all the params this command had */
        if (!cmdParamsSkipped)
            sgTruncFront(payload, len);

        if (!BT_ADDR_IS_EDR(conn->peerAddr)) /* only EDR can put multiple commands here */
            break;
    }

    if (sgLength(payload))
        loge("Leftover %d bytes in signalling frame\n", sgLength(payload));
    sgFree(payload);
}

/*
 * FUNCTION: l2cAdvTxCreateFragments
 * USE:      Called when we may have space in the buffer list and may want to create a fragment
 * PARAMS:   conn - the conn struct
 *           first - will this be a first fragment?
 * RETURN:   success
 * NOTES:    call with mConnsLock held.
 */
static bool l2cAdvTxCreateFragments(struct l2cConn *conn, bool first)
{
    bool ret = false;
    uint16_t len;

    while (conn->advEnqueuedPacket && !conn->advTxFrags[conn->advTxWritePos].data) {

        len = sgLength(conn->advEnqueuedPacket);

        if (len <= conn->advTxMps) { /* fist in one fragment */
            if (first)
                conn->advTxFrags[conn->advTxWritePos].sar = L2C_SAR_COMPLETE;
            else
                conn->advTxFrags[conn->advTxWritePos].sar = L2C_SAR_END;
            conn->advTxFrags[conn->advTxWritePos].data = conn->advEnqueuedPacket;
            conn->advEnqueuedPacket = NULL;
        } else {
            sg data = sgSplit(conn->advEnqueuedPacket, conn->advTxMps - (first ? sizeof(uint16_t) : 0));

            if (!data) {
                loge("Failed to split data sg\n");
                return false;
            }
            sgSwap(data, conn->advEnqueuedPacket);

            if (first) {
                conn->advTxFrags[conn->advTxWritePos].sar = L2C_SAR_START;
                utilSetLE16(&len, len);
                if (!sgConcatFrontAlloced(data, &len, sizeof(len))) {
                    loge("Failed to append len. Dropping fragment.\n");
                    sgFree(data);
                    return false;
                }
            } else
                conn->advTxFrags[conn->advTxWritePos].sar = L2C_SAR_CONT;
            conn->advTxFrags[conn->advTxWritePos].data = data;
        }
        conn->advTxWritePos = (conn->advTxWritePos + 1) % L2C_RETR_NUM_BUFFERS;
        first = false;
        ret = true;
    }

    return ret;
}

/*
 * FUNCTION: l2cAdvUserTx
 * USE:      Called when a user wants us to send data on an advanced connection
 * PARAMS:   conn - the conn struct
 *           data - the data
 * RETURN:   L2C_TX_*
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cAdvUserTx(struct l2cConn *conn, sg data) /* -> L2C_TX_* */
{
    if (conn->advEnqueuedPacket) /* if a packet is half-sent, we have no space */
        return L2C_TX_TRY_LATER;

    if (conn->advTxFrags[conn->advTxWritePos].data) /* if current write pointer is not empty - we have no space */
        return L2C_TX_TRY_LATER;

    conn->advEnqueuedPacket = data;
    if (l2cAdvTxCreateFragments(conn, true))
        return L2C_TX_ACCEPTED;

    conn->advEnqueuedPacket = NULL;
    return L2C_TX_ERROR;
}

/*
 * FUNCTION: l2cAdvTxWrapNextPacket
 * USE:      Produce a valid I-frame for a given connection
 * PARAMS:   NONE
 * RETURN:   true if we enqueued a frame
 * NOTES:    call with mConnsLock held. Note that conn is not modified here
 */
static sg l2cAdvTxWrapNextPacket(struct l2cConn *conn)
{
    uint16_t len, control = 0, fcs = 0, hdrSz = sizeof(struct l2cFrmHdr) + sizeof(uint16_t);
    struct l2cAdvFrag *frag = conn->advTxFrags + conn->advTxSendPos;
    uint8_t buf[sizeof(struct l2cFrmHdr) + sizeof(uint16_t)];
    struct l2cFrmHdr *hdr = (struct l2cFrmHdr*)buf;
    uint16_t *ctrlP = (uint16_t*)(hdr + 1);
    sg packet = frag->data;
    bool needLen = false;

    if (!packet)
        return NULL;

    control |= (uint16_t)conn->advRxFirstMissing << L2C_FLG_SHFT_REQ_SEQ;
    control |= (uint16_t)conn->advTxSendPos << L2C_FLG_SHFT_TX_SEQ;
    control |= (uint16_t)frag->sar << L2C_SHFT_SAR;

    utilSetLE16(&hdr->cid, conn->remCh);
    utilSetLE16(&hdr->len, sgLength(packet) + hdrSz + (conn->fcs ? sizeof(uint16_t) : 0));
    utilSetLE16(ctrlP, control);

    if (!sgConcatFrontCopy(packet, hdr, hdrSz)) {
        logw("Failed to append header\n");
        return NULL;
    }

    if (conn->fcs) {
        fcs = l2cUtilFcsSg(fcs, packet);
        utilSetLE16(&fcs, fcs);
        if (!sgConcatBackCopy(packet, &fcs, sizeof(uint16_t))) {
            loge("Failed to append FCS\n");
            sgTruncFront(packet, hdrSz);
            return NULL;
        }
    }

    return packet;
}

/*
 * FUNCTION: l2cAdvTxUnWrapNextPacket
 * USE:      Remove I-frame headers and footers and put it back into the connection's buffer list.
 *           Essentially undoes what l2cAdvTxWrapNextPacket() did. Only safe to call after a call
 *           to l2cAdvTxWrapNextPacket() and with the struct not modifie din between!
 * PARAMS:   NONE
 * RETURN:   true if we enqueued a frame
 * NOTES:    call with mConnsLock held. Note that conn is not modified here
 */
static void l2cAdvTxUnWrapNextPacket(struct l2cConn *conn, sg packet)
{
    struct l2cAdvFrag *frag = conn->advTxFrags + conn->advTxSendPos;

    if (conn->fcs)
        sgTruncBack(packet, sizeof(uint16_t));

    sgTruncFront(packet, sizeof(struct l2cFrmHdr) + sizeof(uint16_t));
}

/*
 * FUNCTION: l2cAdvTxPrepareSFrame
 * USE:      Prepare an S-frame for transmission
 * PARAMS:   conn - the conn struct
 * RETURN:   said frame or NULL
 * NOTES:    call with mConnsLockHeld
 */
static sg l2cAdvTxPrepareSFrame(struct l2cConn *conn)
{
    const uint16_t l2cPayloadLen = sizeof(uint16_t) + (conn->fcs ? sizeof(uint16_t) : 0);
    const uint16_t maxPacketLen = sizeof(struct l2cFrmHdr) + sizeof(uint16_t[2]); /* control and fcs */
    const uint16_t checksummedLen = sizeof(struct l2cFrmHdr) + sizeof(uint16_t);
    const uint16_t packetLen = sizeof(struct l2cFrmHdr) + l2cPayloadLen;
    uint8_t buf[maxPacketLen];
    struct l2cFrmHdr *hdr = (struct l2cFrmHdr*)buf;
    uint16_t *ctrlP = (uint16_t*)(hdr + 1);
    uint16_t control = L2C_FLG_S_FRM;
    uint16_t *fcsP = ctrlP + 1;

    control |= (uint16_t)conn->advRxFirstMissing << L2C_FLG_SHFT_REQ_SEQ;
    control |= (uint16_t)L2C_S_RR; /* we only do this for now. it i snot necessarily the best idea */

    utilSetLE16(&hdr->len, l2cPayloadLen);
    utilSetLE16(&hdr->cid, conn->remCh);
    utilSetLE16(ctrlP, control);

    if (conn->fcs)
        utilSetLE16(fcsP, l2cUtilFcs(0, buf, checksummedLen));

    return sgNewWithCopyData(buf, packetLen);
}

/*
 * FUNCTION: l2cAdvMayTxLocked
 * USE:      Called to try to TX on any advanced connection
 * PARAMS:   NONE
 * RETURN:   true if we enqueued a frame
 * NOTES:    call with mConnsLockHeld
 */
static bool l2cAdvMayTxLocked(void)
{
    struct l2cConn *orig, *conn;
    struct l2cAclConn *aclConn;
    uint8_t sendResult;
    bool ret = false, isFrmS = false;
    sg packet;

    if (!mConns) /* no connections -> we have nothing to send */
        return false;

    orig = mNextToTx;
    do {
        if (!mNextToTx)
            mNextToTx = mConns;
        conn = mNextToTx;
        mNextToTx = mNextToTx->next;

        /* basic connections do not participate */
        if (conn->mode == L2C_MODE_BASIC)
            continue;

        /* send S-frames first and foremost */
        if (conn->advRxWeOweAck) {
            packet = l2cAdvTxPrepareSFrame(conn);
            if (!packet) {
                loge("Failed to produce S frame\n");
                continue;
            }
            isFrmS = true;
        } else {

            /* if the window is full, we cannot send */
            if ((conn->advTxSendPos + L2C_RETR_NUM_BUFFERS - conn->advTxFirstUnacked) % L2C_RETR_NUM_BUFFERS >= conn->advTxWindowSz)
                continue;

            /* if there is no more data, we cannot send */
            if (!conn->advTxFrags[conn->advTxSendPos].data)
                continue;

            /* if peer asked us to stop sending data, do so, except s-frames */
            if (conn->advTxShutUp)
                continue;

            /* if we got here, this connection can send */
            packet = l2cAdvTxWrapNextPacket(conn);
            if (!packet) {
                logw("Failed to wrap a packet for tx\n");
                continue;
            }
        }

        aclConn = l2cAclConnFindByHandle(conn->acl);
        if (!aclConn) {
            logw("Cannot find the ACL link for this connection\n");
            continue;
        }

        switch (l2cDataSendOnAclLink(aclConn, NULL, 0, packet)) {
        case L2C_TX_ACCEPTED:
            conn->advRxWeOweAck = false;
            if (!isFrmS) {
                conn->advTxFrags[conn->advTxSendPos++].data = NULL;
                conn->advTxSendPos %= L2C_RETR_NUM_BUFFERS;
            }
            ret = true;
            break;
        case L2C_TX_NO_CONN:
        case L2C_TX_ERROR:
            logd("ADV TX Failed\n");
            sgFree(packet);
            break;
        case L2C_TX_TRY_LATER:
            if (isFrmS)
                sgFree(packet);
            else
                l2cAdvTxUnWrapNextPacket(conn, packet);
            return false;
        }

    } while (mNextToTx != orig);

    return ret;
}


/*
 * FUNCTION: l2cAdvMayTx
 * USE:      Called when TX buffer may have space for more frames
 * PARAMS:   NONE
 * RETURN:   true if we enqueued a frame
 * NOTES:    takes mConnsLock
 */
static bool l2cAdvMayTx(void)
{
    bool ret;

    pthread_mutex_lock(&mConnsLock);
    ret = l2cAdvMayTxLocked();
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cAdvRxAck
 * USE:      Called when the peer acks some packets
 * PARAMS:   conn - the connection struct
 *           newFirstUnackedPacket - new index of first unacked packet
 * RETURN:   NONE
 * NOTES:    call with mConnsLock lock held.
 */
static void l2cAdvRxAck(struct l2cConn *conn, uint16_t newFirstUnackedPacket)
{
    uint8_t ackedPackets = (newFirstUnackedPacket + L2C_RETR_NUM_BUFFERS - conn->advTxFirstUnacked) % L2C_RETR_NUM_BUFFERS;
    bool somethingNewAcked = false;

    if (ackedPackets > conn->advTxWindowSz) {
        loge("Peer acked more packets than allowed. Ignoring\n");
        return;
    }

    while (conn->advTxFirstUnacked != newFirstUnackedPacket) {
        if (conn->advTxFrags[conn->advTxFirstUnacked].data)
            sgFree(conn->advTxFrags[conn->advTxFirstUnacked].data);
        else
            loge("Peer acked NULL data\n");
        conn->advTxFrags[conn->advTxFirstUnacked++].data = NULL;
        conn->advTxFirstUnacked %= L2C_RETR_NUM_BUFFERS;
        somethingNewAcked = true;
    }
    if (somethingNewAcked && l2cAdvTxCreateFragments(conn, false))
        l2cSvcWorkScheduleTryTx(false /*advanced L2C works only on EDR links */);
}

/*
 * FUNCTION: l2cAdvEnhFrameRxS
 * USE:      Called to RX an enhanced L2C S-frame with FCS checked
 * PARAMS:   conn - the connection struct
 *           reqSeq - sent reqSeq as per spec
 *           flgF - F flag as per spec
 *           flgP - P flag as per spec
 *           s - sent S value as per spec
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cAdvEnhFrameRxS(struct l2cConn *conn, uint16_t reqSeq, bool flgF, bool flgP, uint8_t s)
{
    //TODO
    loge("enhanced mode.s not impl yet\n");
    return false;
}


/*
 * FUNCTION: l2cAdvEnhFrameRxI
 * USE:      Called to RX an enhanced L2C I-frame with FCS checked
 * PARAMS:   conn - the connection struct
 *           sar - the far flags as per spec
 *           reqSeq - sent reqSeq as per spec
 *           flgF - F flag as per spec
 *           txSeq - sent txSeq as per spec
 *           payload - the actual data
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cAdvEnhFrameRxI(struct l2cConn *conn, uint8_t sar, uint16_t reqSeq, bool flgF, uint16_t txSeq, sg payload)
{
    //TODO
    loge("enhanced mode.i not impl yet\n");
    return false;
}

/*
 * FUNCTION: l2cAdvFrameRxS
 * USE:      Called to RX a non-enhanced L2C S-frame with FCS checked
 * PARAMS:   conn - the connection struct
 *           reqSeq - sent reqSeq as per spec
 *           flgR - R flag as per spec
 *           s - sent S value as per spec
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cAdvFrameRxS(struct l2cConn *conn, uint16_t reqSeq, bool flgR, uint8_t s) //XXX: TODO: timers for adv modes!
{
    l2cAdvRxAck(conn, reqSeq);
    conn->advTxShutUp = flgR;

    if (s == L2C_S_RR) {
        //TODO: timers
    } else if (s == L2C_S_REJ) { // two of these shall never happen again, see "rej exception condition" in spec
        //TODO: timers
        conn->advTxSendPos = reqSeq;
    } else {
        logw("unexpected S value %d\n", s);
        return false;
    }
    return true;
}

/*
 * FUNCTION: l2cAdvRxReass
 * USE:      Called to attempt to reassemble an incoming fragment using ONE fragment at idx conn->advRxFirstMissing
 * PARAMS:   conn - the connection struct
 * RETURN:   true if we used ONE fragment
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cAdvRxReass(struct l2cConn *conn)
{
    struct l2cAdvFrag *frag = conn->advRxFrags + conn->advRxFirstMissing;
    uint8_t buf[sizeof(uint16_t)];
    bool ret = false;

    if (!frag->data)
        return false;

    if (conn->advTxReassBuf && (frag->sar == L2C_SAR_START || frag->sar == L2C_SAR_COMPLETE)) {
        logd("Dropping existing reass buffer because of new packet seen\n");
        sgFree(conn->advTxReassBuf);
        conn->advTxReassBuf = NULL;
    }

    if (!conn->advTxReassBuf && (frag->sar == L2C_SAR_CONT || frag->sar == L2C_SAR_END)) {
        logd("Incosistent RX buf state\n");
        return false;
    }

    switch (frag->sar) {
    case L2C_SAR_COMPLETE:
        conn->advTxReassExpectLen = 0;
        conn->advTxReassBuf = frag->data;
        frag->data = NULL;
        break;
    case L2C_SAR_START:
        conn->advTxReassExpectLen = sgLength(frag->data);
        conn->advTxReassBuf = frag->data;
        frag->data = NULL;
        if (!sgSerializeCutFront(conn->advTxReassBuf, buf, sizeof(buf))) {
            loge("Failed to read length\n");
            return false;
        }
        conn->advTxReassExpectLen = utilGetLE16(buf);
        if (conn->advTxReassExpectLen < sgLength(conn->advTxReassBuf)) {
            loge("Start fragment longer than entire packet\n");
            sgFree(conn->advTxReassBuf);
            conn->advTxReassBuf = NULL;
            return false;
        }
        conn->advTxReassExpectLen -= sgLength(conn->advTxReassBuf);
        return true;
    case L2C_SAR_END:
    case L2C_SAR_CONT:
        if (sgLength(frag->data) > conn->advTxReassExpectLen) {
            loge("Non-start fragment longer than expected\n");
            return false;
        }
        if (sgLength(frag->data) == conn->advTxReassExpectLen && frag->sar == L2C_SAR_CONT) {
            loge("Non-end fragment ends the expected length\n");
            return false;
        } else if (sgLength(frag->data) != conn->advTxReassExpectLen && frag->sar == L2C_SAR_END) {
            loge("End fragment does not end the expected length\n");
            return false;
        }
        conn->advTxReassExpectLen -= sgLength(frag->data);
        sgConcat(conn->advTxReassBuf, frag->data);
        frag->data = NULL;
        break;
    default:
        loge("Unknown sar value %u\n", frag->sar);
        return false;
    }

    /* if we got here, we need to send data to the user */
    if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_RX, &conn->advTxReassBuf, sizeof(conn->advTxReassBuf))) {
        loge("Failed to send data to user. Dropped\n");
        sgFree(conn->advTxReassBuf);
    }

    conn->advTxReassBuf = NULL;
    return true;
}

/*
 * FUNCTION: l2cAdvFrameRxI
 * USE:      Called to RX a non-enhanced L2C I-frame with FCS checked
 * PARAMS:   conn - the connection struct
 *           sar - the far flags as per spec
 *           reqSeq - sent reqSeq as per spec
 *           flgR - R flag as per spec
 *           txSeq - sent txSeq as per spec
 *           payload - the actual data
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cAdvFrameRxI(struct l2cConn *conn, uint8_t sar, uint16_t reqSeq, bool flgR, uint16_t txSeq, sg payload)
{
    l2cAdvRxAck(conn, reqSeq);
    conn->advTxShutUp = flgR;
    uint8_t numUsed = 0;

    //TODO: timers

    if (conn->advRxFrags[txSeq].data) {
        if (sgLength(conn->advRxFrags[txSeq].data) != sgLength(payload) || conn->advRxFrags[txSeq].sar != sar) {
            logd("Dropping old fragment of %ub with sar %u in favour of existing one: %ub, sar %u\n",
                sgLength(conn->advRxFrags[txSeq].data), conn->advRxFrags[txSeq].sar, sgLength(payload), sar);
            sgFree(conn->advRxFrags[txSeq].data);
        } else {
            /* it is a duplicate - move on */
            sgFree(payload);
            return true;
        }
    }

    /* new packet we had not yet seen */
    conn->advRxFrags[txSeq].data = payload;
    conn->advRxFrags[txSeq].sar = sar;

    while (numUsed < conn->advRxWindowSz && l2cAdvRxReass(conn))
        numUsed++;

    if (numUsed)
        conn->advRxWeOweAck = true;

    if (conn->advTxFrags[conn->advTxSendPos].data && !conn->advTxShutUp)
        l2cAdvMayTxLocked();

    return true;
}

/*
 * FUNCTION: l2cHandleCompleteDataFrameRx
 * USE:      Called when given channel data arrives. Dispatches it
 *           to the right destination
 * PARAMS:   aclConn - the ACL connection
 *           frm - the frame header if the packet
 *           payload - the arrived payload
 * RETURN:   false if data went nowhere
 * NOTES:    call with mConnsLock lock held.
 */
static bool l2cHandleCompleteDataFrameRx(struct l2cAclConn *aclConn, struct l2cFrmHdr *frm, sg payload)
{
    uint16_t cid = utilGetLE16(&frm->cid);
    struct l2cConn *cur = NULL;
    bool ret = false;

    cur = mConns;
    while (cur) {
        if (cur->acl == aclConn->handle && cur->localCh == cid)
            break;
        cur = cur->next;
    }

    if (!cur) {
        logw("Connection for incoming data on ch %u not found\n", cid);
    } else if (cur->state == L2C_CONN_STATE_RX_OPEN_SVC && cid < L2C_NUM_FIXED_CHANNELS) {
        /* Queue the data for later */

        uint32_t len = sgLength(payload);
        if (len + (cur->queuedRx ? sgLength(cur->queuedRx) : 0) > L2C_FIXED_CH_CONN_RX_BACKLOG_MAX) {
            logw("too much queued for FixedCh connection already. Dropping packet\n");
        } else {
            if (!sgConcatFrontCopy(payload, &len, sizeof(len)))
                logw("Failed to concat length to queued data. Dropping\n");
            else if (cur->queuedRx)
                sgConcat(cur->queuedRx, payload);
            else
                cur->queuedRx = payload;
        }
        ret = true;
    } else if (cur->state != L2C_CONN_STATE_ESTABLISHED) {
        logw("Connection in state %u on ch %u unable to accept data\n", cur->state, cid);
    } else {
        if (!cur->inUse) {
            logd("Marking conn "HANDLEFMT" as in use due to RX\n", HANDLECNV(cur->handle));
            cur->inUse = 1;
            l2cAclDownTimerStop(aclConn);
        }
        if (cur->mode == L2C_MODE_BASIC)
            ret = l2cSvcWorkScheduleStateCall(cur, L2C_STATE_RX, &payload, sizeof(payload));
        else {
            uint16_t ctrl, fcsGot, fcsCalc, reqSeq;

            if (cur->fcs) {
                void *iter;
                if (sgLength(payload) < 2 || sizeof(fcsGot) != sgSerialize(payload, sgLength(payload) - sizeof(fcsGot), sizeof(fcsGot), &fcsGot)) {
                    logd("FCS on and cannot read FCS\n");
                    goto badFrame;
                }
                sgTruncBack(payload, sizeof(uint16_t));
                fcsGot = utilGetLE16(&fcsGot);
                fcsCalc = l2cUtilFcs(0, frm, sizeof(struct l2cFrmHdr));
                fcsCalc = l2cUtilFcsSg(fcsCalc, payload);

                if (fcsCalc != fcsGot) {
                    logd("FCS bad. Got 0x%04X Calc 0x%04X\n", fcsGot, fcsCalc);
                    goto badFrame;
                }
            }
            if (!sgSerializeCutFront(payload, &ctrl, sizeof(ctrl))) {
                logd("Cannot find ctrl\n");
                goto badFrame;
            }
            reqSeq = (ctrl & L2C_FLG_MASK_REQ_SEQ) >> L2C_FLG_SHFT_REQ_SEQ;
            if (ctrl & L2C_FLG_S_FRM) { /* S frame */
                if (ctrl & L2C_FLG_RSVD_S)
                    goto badFrame;

                if (cur->mode == L2C_MODE_ENH_RETR)
                    ret = l2cAdvEnhFrameRxS(cur, reqSeq, !!(ctrl & L2C_FLG_F), !!(ctrl & L2C_FLG_P), (ctrl & L2C_MASK_S) >> L2C_SHFT_S);
                else
                    ret = l2cAdvFrameRxS(cur, reqSeq, !!(ctrl & L2C_FLG_R), (ctrl & L2C_MASK_S) >> L2C_SHFT_S);
            } else {                    /* I frame */
                uint16_t txSeq = (ctrl & L2C_FLG_MASK_TX_SEQ) >> L2C_FLG_SHFT_TX_SEQ;

                if (cur->mode == L2C_MODE_ENH_RETR)
                    ret = l2cAdvEnhFrameRxI(cur, (ctrl & L2C_MASK_SAR) >> L2C_SHFT_SAR, reqSeq, !!(ctrl & L2C_FLG_F), txSeq, payload);
                else
                    ret = l2cAdvFrameRxI(cur, (ctrl & L2C_MASK_SAR) >> L2C_SHFT_SAR, reqSeq, !!(ctrl & L2C_FLG_R), txSeq, payload);
            }
            goto out;

badFrame:
            logw("got invalid L2C advanced frame\n");
        }
    }
out:
    return ret;
}

/*
 * FUNCTION: l2cAclDataRxCompletePacket
 * USE:      Called when a complete reassembled packet has arrived
 * PARAMS:   aclConn - the acl connection struct
 *           frm - the frame header if the packet
 *           packet - a complete received ACL packet
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cAclDataRxCompletePacket(struct l2cAclConn *aclConn, struct l2cFrmHdr *frm, sg packet)
{
    uint16_t cid = utilGetLE16(&frm->cid);

    if (cid == L2C_CH_SIGNALLING_EDR) {
        if (!BT_ADDR_IS_EDR(aclConn->peerAddr)) {
            loge("Received EDR signalling packet on non-EDR link. Dropping.\n");
            sgFree(packet);
        } else
            l2cSignallingRx(aclConn, packet);
    } else if (cid == L2C_CH_SIGNALLING_LE) {
        if (!BT_ADDR_IS_LE(aclConn->peerAddr)) {
            loge("Received LE signalling packet on non-LE link. Dropping.\n");
            sgFree(packet);
        } else
            l2cSignallingRx(aclConn, packet);
    } else if (cid == L2C_CH_CONNECTIONLESS) {
        psm_t psm;

        if (sgLength(packet) < sizeof(psm_t)) {
            loge("Short connectionless L2CAP data RXed: %db\n", sgLength(packet));
            sgFree(packet);
        } else {
            sgSerialize(packet, 0, sizeof(psm_t), &psm);
            sgTruncFront(packet, sizeof(psm_t));
            if (!l2cSvcWorkSchedulePsmConnectionlessRx(psm, aclConn->handle, packet)) {
                loge("Failed to schedule call to connectionless RX for psm %d\n", psm);
                sgFree(packet);
            }
        }
    } else if (!l2cHandleCompleteDataFrameRx(aclConn, frm, packet)) {
        if (cid < L2C_NUM_FIXED_CHANNELS)
            logw("Dropping RXed FixedCh data for cid %d\n", cid);
        else
            logw("Dropping RXed PSM data for unknown cid %d\n", cid);
        sgFree(packet);
    }
}

/*
 * FUNCTION: l2cAclDataRxProcessIfComplete
 * USE:      Called with a [partially] RXed packet to see if it is
 *           complete, and if so process it
 * PARAMS:   conn - the ACL connection struct
 *           packet - the incoming (possibly complete) packet
 * RETURN:   true if packet was complete and was processed
 *           false if more data is needed
 * NOTES:    call with mConnsLock held
 */
static bool l2cAclDataRxProcessIfComplete(struct l2cAclConn *conn, sg packet)
{
    struct l2cFrmHdr frm;
    uint16_t frmLen, totalLen;
    uint32_t len = sgLength(packet);

    /* must be long enough to fit a header */
    if (len < sizeof(struct l2cFrmHdr))
        return false;

    sgSerialize(packet, 0, sizeof(struct l2cFrmHdr), &frm);

    frmLen = utilGetLE16(&frm.len);
    totalLen = frmLen + sizeof(struct l2cFrmHdr);

    /* must fit header and data */
    if (totalLen > len)
        return false;

    /* throw a warning if extra data */
    if (totalLen < len)
        logw("ACL data packet %ub but L2C expects %ub\n", len, totalLen);

    /* full packet - process it */
    sgTruncFront(packet, sizeof(struct l2cFrmHdr));
    l2cAclDataRxCompletePacket(conn, &frm, packet);
    return true;
}

/*
 * FUNCTION: l2cAclDataRx
 * USE:      Called by lower layers when ACL data arrives
 * PARAMS:   aclConnID - the connection ID
 *           packet - the incoming packet
 *           first - whethere this is first or continued packet
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclDataRx(hci_conn_t aclConnID, sg packet, bool first)
{
    struct l2cAclConn *conn;
    uint32_t datalen = sgLength(packet);

    pthread_mutex_lock(&mConnsLock);
    conn = l2cAclConnFindById(aclConnID);
    if (conn) {
        if (!first && !conn->reassSg)
            logw("Got %ub non-first fragment while no reassemble buffer. Dropping\n", datalen);
        else {
            if (first) {
                if (conn->reassSg) {
                    logw("Got a first packet while reassemble buffer has %ub. Dropping previous fragment\n", sgLength(conn->reassSg));
                    sgFree(conn->reassSg);
                    conn->reassSg = NULL;
                }
                if (!l2cAclDataRxProcessIfComplete(conn, packet)) {
                    conn->reassSg = packet;
                }
                packet = NULL;
            } else if (!conn->reassSg)
                logw("Got non-first packet when reassembly buffer empty. Dropping %ud\n", datalen);
            else {
                sgConcat(conn->reassSg, packet);
                packet = NULL;

                if (l2cAclDataRxProcessIfComplete(conn, conn->reassSg))
                    conn->reassSg = NULL;
            }
        }
    } else
        loge("Unable to find acl "HCI_CONN_FMT"x. Dropping packet\n", HCI_CONN_CONV(aclConnID));

    if (packet)
        sgFree(packet);

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
 * FUNCTION: l2cCommonSvcInstAllocDone
 * USE:      Called when a service call to allocate a PSM of a FixedCh service instance finishes
 * PARAMS:   handle - the handle param for the connection
 *           connP - where to put pointer to per-conn struct
 *           aclConnP - where to put pointer to per-ACL-link struct
 * RETURN:   true if all was successful, false else
 * NOTES:    call with mConnsLock held
 */
static bool l2cCommonSvcInstAllocDone(l2c_handle_t handle, struct l2cConn **connP, struct l2cAclConn **aclConnP)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        return false;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        return false;
    }

    *connP = conn;
    *aclConnP = aclConn;

    return true;
}

/*
 * FUNCTION: l2cPsmCfgStart
 * USE:      Start config (once channel open reply has been sent)
 * PARAMS:   aclConn - the acl connection struct
 *           conn - the l2c connection struct
 * RETURN:   NONE
 * NOTES:    call with mConnsLock held
 */
static void l2cPsmCfgStart(struct l2cAclConn *aclConn, struct l2cConn *conn)
{
    conn->state = L2C_CONN_STATE_CFG;
    l2cConnTimer(conn, L2C_L2CAP_CONFIG_TIMEOUT);
    conn->cfg.cfgTxState = L2C_CFG_STATE_INITIAL;
    conn->cfg.cfgRxState = L2C_CFG_STATE_INITIAL;
    l2cPsmSendConfig(aclConn, conn);
}

/*
 * FUNCTION: l2cPsmSvcInstAllocDone
 * USE:      Called when a service call to allocate a PSM service instance finishes
 * PARAMS:   l2cStateCbk - the state cbk for this conn
 *           userData - param for state callbacks
 *           result - SVC_ALLOC_* from service instance allocate callback
 *           instance - the allocated instance or NULL
 *           handle - the handle param for the connection
 * RETURN:   NONE
 * NOTES:    May destroy your conn
 */
static void l2cPsmSvcInstAllocDone(psm_t psm, l2cStateCbk stateCbk, void *userData, uint8_t result, void *instance, l2c_handle_t handle)
{
    struct l2cConn* conn;
    struct l2cAclConn* aclConn;

    pthread_mutex_lock(&mConnsLock);

    if (!l2cCommonSvcInstAllocDone(handle, &conn, &aclConn))
        goto out;

    if (result == SVC_ALLOC_SUCCESS && conn->state == L2C_CONN_STATE_TEARDOWN) {
        if (!l2cSvcWorkScheduleStateCallDirect(stateCbk, userData, instance, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to close PSM after teardown request in alloc done handler\n");
        /* this is the closes error code to what we want here */
        if (!l2cSigSendConnectionRsp(aclConn, conn->ident, 0, conn->remCh, L2C_SIG_CONN_RSP_NO_RSRC, 0))
            loge("Failed to sent negative connection request response frame\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    if (result == SVC_ALLOC_DEMAND_ENCR || result == SVC_ALLOC_DEMAND_AUTH) {
        if (!l2cSigSendConnectionRsp(aclConn, conn->ident, 0, conn->remCh, L2C_SIG_CONN_RSP_PEND, L2C_SIG_CONN_PEND_AUTH)) {
            loge("Failed to sent pending.auth connection request response frame\n");
            l2cConnRequestClose(aclConn, conn, false);
            goto out;
        }
        conn->state = L2C_CONN_STATE_ENCR_WAIT;
        if (aclConn->encrypted && (aclConn->mitmSafe || result != SVC_ALLOC_DEMAND_AUTH)) {
            logi("Link acl "HCI_CONN_FMT"x already encrypted to the requested level\n", HCI_CONN_CONV(aclConn->conn));
            if (!l2cSvcWorkSchedulePsmAlloc(psm, handle))
                loge("Failed to re-enquque PSM alloc\n");
        } else {
            if (!l2cSvcWorkScheduleAclDemandEncr(aclConn->conn, result == SVC_ALLOC_DEMAND_AUTH))
                loge("Failed to demand encr/auth\n");
            logd("Demanded encr/auth on acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
        }
        goto out;
    }

    if (result != SVC_ALLOC_SUCCESS && result != SVC_ALLOC_WAIT_FOR_AUTHORIZ) {
        uint16_t retCode = L2C_SIG_CONN_RSP_NO_RSRC;

        if (result == SVC_ALLOC_FAIL_SECURITY)
            retCode = L2C_SIG_CONN_RSP_BAD_SEC;
        else if (result == SVC_ALLOC_FAIL_BEING_REMOVED || result == SVC_ALLOC_FAIL_NO_CONNS)
            retCode = L2C_SIG_CONN_RSP_BAD_PSM;

        logi("Service allocations failed for psm %d with result %d code %d\n", conn->psm, result, retCode);
        if (!l2cSigSendConnectionRsp(aclConn, conn->ident, 0, conn->remCh, retCode, 0))
            loge("Failed to sent negative connection request response frame\n");
        logd("Dropping connection struct\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;

    if (result == SVC_ALLOC_WAIT_FOR_AUTHORIZ) {
        if (!l2cSigSendConnectionRsp(aclConn, conn->ident, 0, conn->remCh, L2C_SIG_CONN_RSP_PEND, L2C_SIG_CONN_PEND_ATHORIZ)) {
            loge("Failed to sent pending.athoriz connection request response frame\n");
            l2cConnRequestClose(aclConn, conn, true);
            goto out;
        }
        conn->state = L2C_CONN_STATE_ATHORIZ_WAIT;
        goto out;
    }

    if (!l2cSigSendConnectionRsp(aclConn, conn->ident, conn->localCh, conn->remCh, L2C_SIG_CONN_RSP_OK, 0)) {
        loge("Failed to sent positive connection request response frame - dropping connection\n");
        l2cConnRequestClose(aclConn, conn, false);
        goto out;
    }

    l2cPsmCfgStart(aclConn, conn);

out:
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cFixedChSvcInstAllocDone
 * USE:      Called when a service call to allocate a FixedCh service instance finishes
 * PARAMS:   chan - the channel originally requested
 *           l2cStateCbk - the state cbk for this conn
 *           userData - param for state callbacks
 *           result - SVC_ALLOC_* from service instance allocate callback
 *           instance - the allocated instance or NULL
 *           handle - the handle param for the connection
 * RETURN:   NONE
 * NOTES:
 */
static void l2cFixedChSvcInstAllocDone(uint16_t chan, l2cStateCbk stateCbk, void *userData, uint8_t result, void *instance, l2c_handle_t handle)
{
    struct l2cConn* conn;
    struct l2cAclConn* aclConn;

    pthread_mutex_lock(&mConnsLock);

    if (!l2cCommonSvcInstAllocDone(handle, &conn, &aclConn))
        goto out;

    if (result == SVC_ALLOC_SUCCESS && conn->state == L2C_CONN_STATE_TEARDOWN) {
        if (!l2cSvcWorkScheduleStateCallDirect(stateCbk, userData, instance, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to close FixedCh after teardown request in alloc done handler\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    if (conn->state != L2C_CONN_STATE_RX_OPEN_SVC)
        logw("Unexpected FixedCh conn state %d after alloc\n", conn->state);

    if (result == SVC_ALLOC_DEMAND_ENCR || result == SVC_ALLOC_DEMAND_AUTH) {
        if (aclConn->encrypted && (aclConn->mitmSafe || result != SVC_ALLOC_DEMAND_AUTH)) {
            logi("Link acl "HCI_CONN_FMT"x already encrypted to the requested level\n", HCI_CONN_CONV(aclConn->conn));
            if (!l2cSvcWorkScheduleFixedChAlloc(chan, handle)) {
                loge("Failed to re-enqueue fixed alloc\n");
                l2cConnRequestClose(aclConn, conn, false);
                goto out;
            }
        } else {
            if (!l2cSvcWorkScheduleAclDemandEncr(aclConn->conn, result == SVC_ALLOC_DEMAND_AUTH)) {
                loge("Failed to demand encryption\n");
                l2cConnRequestClose(aclConn, conn, false);
                goto out;
            }
            logd("Demanded encryption on acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
            conn->state = L2C_CONN_STATE_ENCR_WAIT;
        }
        goto out;
    }

    if (result != SVC_ALLOC_SUCCESS && result != SVC_ALLOC_WAIT_FOR_AUTHORIZ) {
        logi("Service allocations failed for FixedCh %d\n", conn->localCh);
        logd("Dropping connection struct\n");
        l2cConnStructDelete(conn);
        goto out;
    }

    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;

    if (result == SVC_ALLOC_WAIT_FOR_AUTHORIZ) {
        conn->state = L2C_CONN_STATE_ATHORIZ_WAIT;
        goto out;
    }

    logd("Service up for FixedCh %d\n", conn->localCh);
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
    struct l2cPsmSvc *psmSvc;
    struct l2cFixedChSvc *fixedSvc;
    psm_t psm;
    uint16_t chan;
    int status;

    pthread_setname_np(pthread_self(), "bt_l2cap_worker");

    while(1) {
        void *instance = NULL;
        uint8_t result;

        status = workQueueGet(mServiceWork, (void**)&work);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (work->type) {
        case L2C_WORK_PSM_INST_ALLOC:
            psm = work->i_alloc.which.psm;
            pthread_mutex_lock(&mSvcsLock);
            psmSvc = l2cPsmSvcFindByPsm(psm);
            if (!psmSvc) {
                logw("Got work for a not-found PSM-based service %d\n", psm);
                result = SVC_ALLOC_FAIL_OTHER;
            } else if (psmSvc->beingRemoved) {
                logi("Ignoring instance alloc for service (psm=%d) being removed\n", psm);
                result = SVC_ALLOC_FAIL_BEING_REMOVED;
            } else if (!psmSvc->desc.serviceInstanceAlloc) {
                logi("Ignoring instance alloc for service (psm=%d) not supporting connections\n", psm);
                result = SVC_ALLOC_FAIL_NO_CONNS;
            } else {
                result = psmSvc->desc.serviceInstanceAlloc(psmSvc->desc.userData, work->i_alloc.handle, &instance);
            }
            pthread_mutex_unlock(&mSvcsLock);
            l2cPsmSvcInstAllocDone(psm, psmSvc->desc.serviceInstanceStateCbk, psmSvc->desc.userData, result, instance, work->i_alloc.handle);
            break;
        case L2C_WORK_PSM_CONNECTIONLESS_RX:
            psm = work->cl_rx.psm;
            pthread_mutex_lock(&mSvcsLock);
            psmSvc = l2cPsmSvcFindByPsm(psm);
            if (!psmSvc) {
                logw("Got work for a not-found PSM-based service %d\n", psm);
            } else {
                if (psmSvc->beingRemoved) {
                    logi("Ignoring connectionless rx for service (psm=%d) being removed\n", psm);
                } else {
                    if (psmSvc->desc.serviceConnectionlessRx) {
                        psmSvc->desc.serviceConnectionlessRx(psmSvc->desc.userData, work->cl_rx.handle, work->cl_rx.payload);
                        work->cl_rx.payload = NULL;
                    }
                }
                if (work->cl_rx.payload)
                    sgFree(work->cl_rx.payload);
            }
            pthread_mutex_unlock(&mSvcsLock);
            break;
        case L2C_WORK_PSM_UNREGISTER:
            psm = work->unreg.which.psm;
            pthread_mutex_lock(&mSvcsLock);
            psmSvc = l2cPsmSvcFindByPsm(psm);
            if (!psmSvc) {
                logw("Got work for a not-found PSM-based service %d\n", psm);
            } else {
                if (!psmSvc->beingRemoved)
                    logw("Unregistering a service (psm=%d) unexpectedly\n", psm);
                if (psmSvc->next)
                    psmSvc->next->prev = psmSvc->prev;
                if (psmSvc->prev)
                    psmSvc->prev->next = psmSvc->next;
                else
                    mPsmSvcs = psmSvc->next;
                free(psmSvc);
                if (work->unreg.doneCbk)
                    work->unreg.doneCbk(work->unreg.cbkData);
            }
            pthread_mutex_unlock(&mSvcsLock);
            break;
        case L2C_WORK_FIXEDCH_INST_ALLOC:
            chan = work->i_alloc.which.chan;
            pthread_mutex_lock(&mSvcsLock);
            fixedSvc = l2cFixedChSvcFindByChan(chan);
            if (!fixedSvc) {
                logw("Got work for a not-found FixedCh service %d\n", chan);
                result = SVC_ALLOC_FAIL_OTHER;
            } else if (fixedSvc->beingRemoved) {
                logi("Ignoring instance alloc for service (chan=%d) being removed\n", chan);
                result = SVC_ALLOC_FAIL_BEING_REMOVED;
            } else {
                result = fixedSvc->desc.serviceFixedChAlloc(fixedSvc->desc.userData, work->i_alloc.handle, &instance);
            }
            pthread_mutex_unlock(&mSvcsLock);
            l2cFixedChSvcInstAllocDone(chan, fixedSvc->desc.serviceFixedChStateCbk, fixedSvc->desc.userData, result, instance, work->i_alloc.handle);
            break;
        case L2C_WORK_FIXEDCH_UNREGISTER:
            chan = work->unreg.which.chan;
            pthread_mutex_lock(&mSvcsLock);
            fixedSvc = l2cFixedChSvcFindByChan(chan);
            if (!fixedSvc) {
                logw("Got work for a not-found FixedCh service %d\n", chan);
            } else {
                if (!fixedSvc->beingRemoved)
                    logw("Unregistering a service (chan=%d) unexpectedly\n", chan);
                if (fixedSvc->next)
                    fixedSvc->next->prev = fixedSvc->prev;
                if (fixedSvc->prev)
                    fixedSvc->prev->next = fixedSvc->next;
                else
                    mFixedSvcs = fixedSvc->next;
                free(fixedSvc);
                if (work->unreg.doneCbk)
                    work->unreg.doneCbk(work->unreg.cbkData);
            }
            pthread_mutex_unlock(&mSvcsLock);
            break;
        case L2C_WORK_STATE_CBK:
            work->state.stateCbk(work->state.userData, work->state.instance, work->state.state, work + 1, work->state.len);
            break;
        case L2C_WORK_CONN_UPDT_CBK:
            work->connUpdtCbk.doneCbk(work->connUpdtCbk.cbkData, work->connUpdtCbk.success);
            break;
        case L2C_WORK_ACL_LINK_OPEN:
            if (!hciConnect(&work->aclLink.addr))
                loge("HCI refused to connect\n");
            break;
        case L2C_WORK_ACL_LINK_CLOSE:
            if (!hciDisconnect(&work->aclLink.addr))
                loge("HCI refused to disconnect\n");
            break;
        case L2C_WORK_ACL_CONN_UPDT:
            if (!hciUpdateLeParams(work->connUpdt.aclConnID, work->connUpdt.minInt, work->connUpdt.maxInt, work->connUpdt.lat, work->connUpdt.to))
                loge("HCI refused to update LE params\n");
            break;
        case L2C_WORK_ACL_DEMAND_ENCR:
            if (!hciDemandEncr(work->demandEncr.aclConnID, work->demandEncr.demandMitmSafe))
                loge("HCI refused to encrypt\n");
            break;
        case L2C_WORK_TRY_HCI_TX:
            l2cTryMakeSendProgress(work->tryTx.useLeBuffers);
            break;
        case L2C_WORK_DEFAULT_SM_CALL:
            work->defaultSm.cbk(work->defaultSm.hciConn, work->defaultSm.randomNum, work->defaultSm.diversifier);
            break;
        case L2C_WORK_FLUSH:
            sem_post(work->flush.sem);
            break;
        default: {
            loge("Unknown work type %d\n", work->type);
            break;
        }
        }
        free(work);
    }

    return NULL;
}

/*
 * FUNCTION: l2cAclEnqueueSigTx
 * USE:      Enqueue a signalling PDU
 * PARAMS:   aclConn - the ACL connection structure
 *           pdu - the PDU data
 * RETURN:   true is a send was requested. False on immediate error.
 * NOTES:    Call only with mConnsLock held.
 *           ONLY FOR SIGNALLING FRAMES. WILL NOT BLOCK.
 */
static bool l2cAclEnqueueSigTx(struct l2cAclConn *aclConn, sg pdu)
{
    bool isLE = BT_ADDR_IS_LE(aclConn->peerAddr);
    bool useLeBuffers = isLE && !mChipJointBuffers;
    struct l2cSigPduSendItem **qH = isLE ? &sigSendHeadLe : &sigSendHeadEdr;
    struct l2cSigPduSendItem **qT = isLE ? &sigSendTailLe : &sigSendTailEdr;
    struct l2cSigPduSendItem *item;
    uint16_t len = sgLength(pdu);

    if (len > aclConn->sigMtu) {
        loge("Rejecting L2C SIG TX over MTU len: %u > %u\n", len, aclConn->sigMtu);
        return false;
    }

    /* if not LE, we try to merge. Find last SIG frame to same connection and see if we can merge with it. */
    if (!isLE) {
        struct l2cSigPduSendItem *i = *qT;
        while (i && i->aclConnID != aclConn->conn)
            i = i->prev;
        if (i && !i->hasL2cHdr && sgLength(i->pdu) + len <= aclConn->sigMtu) {
            sgConcat(i->pdu, pdu);
            return true;
        }
    }

    item = (struct l2cSigPduSendItem*)calloc(1, sizeof(struct l2cSigPduSendItem));
    if (!item)
        return false;

    item->aclConnID = aclConn->conn;
    item->pdu = pdu;
    item->next = NULL;
    item->prev = (*qT);
    item->cid = BT_ADDR_IS_LE(aclConn->peerAddr) ? L2C_CH_SIGNALLING_LE : L2C_CH_SIGNALLING_EDR;
    item->hasL2cHdr = false;
    if (item->prev)
        item->prev->next = item;
    else
        (*qH) = item;
    *qT = item;

    if (!l2cSvcWorkScheduleTryTx(useLeBuffers))
        loge("Failed to schedule TX attempt\n");

    return true;
}

/*
 * FUNCTION: l2cWorkFreeSigTx
 * USE:      Called to cleanup each item of signalling TX queue at deinit time
 * PARAMS:   workItem - a work item
 * RETURN:   NONE
 * NOTES:
 */
void l2cWorkFreeSigTx(void *workItem)
{
    struct l2cSigFrameWork *work = (struct l2cSigFrameWork*)workItem;

    free(work->data);
    free(work);
}

/*
 * FUNCTION: l2cWorkFreeService
 * USE:      Called to cleanup each item of work queue at deinit time
 * PARAMS:   workItem - a work item
 * RETURN:   NONE
 * NOTES:
 */
void l2cWorkFreeService(void *workItem)
{
    struct l2cSvcWork *work = (struct l2cSvcWork*)workItem;

    switch (work->type) {
    case L2C_WORK_PSM_CONNECTIONLESS_RX:
        sgFree(work->cl_rx.payload);
        break;
    }
    free(work);
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


    mNextPsm = 0x1001;

    mQueueFirstLe = true;
    mQueueFirstEdr = true;
    mQueueConnLe = 0;
    mQueueConnEdr = 0;

    mServiceWork = workQueueAlloc(L2C_SVC_NUM_OUTSTANDING_REQS);
    if (!mServiceWork){
        loge("Failed to allocate service workqueue\n");
        ret = ENOMEM;
        goto out_service_q;
    }

    ret = pthread_create(&mSvcWorker, NULL, l2cSvcWorker, NULL);
    if (ret) {
        loge("Failed(%d) to create service worker\n", ret);
        goto out_service_thread;
    }

    mWriteNotif = multiNotifCreate();
    if (!mWriteNotif) {
        loge("Failed to create write notif list\n");
        goto out_multinotif;
    }

    mChipJointBuffers = hciInfoSharedBuffers();
    if (!hciInfoAclBufSizeLe(&mChipBufLenLe, &mChipBufNumLe))
        logi("No LE support found\n");

    return 0;

out_multinotif:
    workQueueWakeAll(mServiceWork, 1);
    pthread_join(mSvcWorker, NULL);

out_service_thread:
    workQueueFree(mServiceWork, l2cWorkFreeService);

out_service_q:
    return ret;
}

/*
 * FUNCTION: l2cDeinit
 * USE:      Unilaterally (and quickly) close l2cap
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    the other side never gets to know what hit it
 */
void l2cDeinit(void)
{
    pthread_mutex_lock(&mConnsLock);
    while (mAclConns)
        l2cAclLinkDownInt(mAclConns, false);
    if (mConns)
        logw("Connections list not empty even when all acl links closed\n");
    mConns = NULL;
    mNextToTx =  NULL;
    pthread_mutex_unlock(&mConnsLock);
    while (mPsmSvcs)
        l2cApiServicePsmUnregister(mPsmSvcs->psm, NULL, NULL);
    while (mFixedSvcs)
        l2cApiServiceFixedChUnregister(mFixedSvcs->chan, NULL, NULL);

    if (mQueueBufLe)
        sgFree(mQueueBufLe);

    if (mQueueBufEdr)
        sgFree(mQueueBufEdr);

    mQueueBufLe = NULL;
    mQueueBufEdr = NULL;

    while (sigSendHeadEdr) {
        sigSendTailEdr = sigSendHeadEdr;
        sigSendHeadEdr = sigSendHeadEdr->next;
        sgFree(sigSendTailEdr->pdu);
        free(sigSendTailEdr);
    }
    sigSendTailEdr = NULL;

    while (sigSendHeadLe) {
        sigSendTailLe = sigSendHeadEdr;
        sigSendHeadLe = sigSendHeadLe->next;
        sgFree(sigSendTailLe->pdu);
        free(sigSendTailLe);
    }
    sigSendTailLe = NULL;

    multiNotifDestroy(mWriteNotif);
    workQueueWakeAll(mServiceWork, 1);
    pthread_join(mSvcWorker, NULL);
    workQueueFree(mServiceWork, l2cWorkFreeService);
}


/*
 * FUNCTION: l2cConnStructDelete
 * USE:      Delete an l2cConn structure
 * PARAMS:   conn - the connection structure
 * RETURN:   NONE
 * NOTES:    Call with mConnsLock held.
 */
static void l2cConnStructDelete(struct l2cConn *conn)
{
    uint8_t i;

    if (mNextToTx == conn)
        mNextToTx = NULL;

    if (conn->queuedRx) {
        sgFree(conn->queuedRx);
        conn->queuedRx = NULL;
    }

    if (conn->next)
        conn->next->prev = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        mConns = conn->next;

    for (i = 0; i < L2C_RETR_NUM_BUFFERS; i++) {
        if (conn->advTxFrags[i].data)
            sgFree(conn->advTxFrags[i].data);
        conn->advTxFrags[i].data = NULL;
        if (conn->advRxFrags[i].data)
            sgFree(conn->advRxFrags[i].data);
        conn->advRxFrags[i].data = NULL;
    }
    if (conn->advEnqueuedPacket)
        sgFree(conn->advEnqueuedPacket);
    conn->advEnqueuedPacket = NULL;

    if (conn->advTxReassBuf)
        sgFree(conn->advTxReassBuf);
    conn->advTxReassBuf = NULL;

    free(conn);
}

/*
 * FUNCTION: l2cAclConnStructDelete
 * USE:      Delete an l2cAclConn structure
 * PARAMS:   conn - the connection structure
 * RETURN:   NONE
 * NOTES:    Call with mConnsLock held.
 */
static void l2cAclConnStructDelete(struct l2cAclConn *conn)
{
    if (conn->reassSg) {
        logw("ACL conn struct delete with %ub still in reassembly buffer\n", sgLength(conn->reassSg));
        sgFree(conn->reassSg);
    }

    if (conn->next)
        conn->next->prev = conn->prev;
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        mAclConns = conn->next;

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
        while (conn && (conn->acl != aclConn->handle || conn->localCh != guess))
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
static uint8_t l2cSigSend(struct l2cAclConn *aclConn, uint8_t ident, uint8_t code, const void *sigFrm, uint16_t sigFrmLen, const void *extraPtr, uint16_t extraLen)
{
    struct l2cSigFrm sig;
    bool newIdent = ident == 0;
    sg pdu;

    utilSetLE8(&sig.code, code);
    utilSetLE16(&sig.len, sigFrmLen + extraLen);

    if (newIdent)
        ident = l2cAclSigNextIdent(aclConn, false);
    utilSetLE8(&sig.ident, ident);

    pdu = sgNew();
    if (!pdu) {
        loge("L2C PDU oom - will not send\n");
        return 0;
    }

    if (!sgConcatBackCopy(pdu, &sig, sizeof(sig)) || !sgConcatBackCopy(pdu, sigFrm, sigFrmLen) || !sgConcatBackCopy(pdu, extraPtr, extraLen)) {
        sgFree(pdu);
        loge("L2C PDU concat error - will not send\n");
        return 0;
    }

    if (l2cAclEnqueueSigTx(aclConn, pdu)) {
        if (newIdent)
            l2cAclSigNextIdent(aclConn, true);
        return ident;
    } else {
        sgFree(pdu);
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
static uint8_t l2cSigSendConfigReq(struct l2cAclConn *aclConn, uint16_t remCh, uint16_t flags, const void *cfgData, uint16_t cfgLen)
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
static uint8_t l2cSigSendConfigRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t remCh, uint16_t flags, uint16_t result, const void *cfgData, uint16_t cfgLen)
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
static uint8_t l2cSigSendEchoRsp(struct l2cAclConn *aclConn, uint8_t ident, const void *echoData, uint16_t echoLen)
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
static uint8_t l2cSigSendInfoRsp(struct l2cAclConn *aclConn, uint8_t ident, uint16_t infoType, uint16_t result, const void *infoData, uint16_t infoLen)
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
static uint8_t l2cSigSendErrCmdReject(struct l2cAclConn *aclConn, uint8_t ident, uint16_t reason, const void *rejData, uint16_t rejLen)
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
 * PARAMS:   acl - the ACL link structure (pass NULL to auto-find it)
 *           conn - the connection struct
 *           tellOtherSide - TX a message to other side?
 * RETURN:   ident if request was sent, else 0
 * NOTES:    Call with mConnsLock held. Will call service's free(), cbk if appropriate
 */
static void l2cConnRequestClose(struct l2cAclConn* acl, struct l2cConn *conn, bool tellOtherSide)
{
    struct l2cConn *t = mConns;
    uint32_t numConnsOnThisAcl = 0;
    bool destroyStruct = false;
    bool tellOurSide = true;
    bool closeLink = false;

    /* find the ACL connection, if none was given */
    if (!acl)
        acl = l2cAclConnFindByHandle(conn->acl);

    if (!acl) {
        loge("Failed to close connection psm %d chs %d %d because acl link was not found\n", conn->psm, conn->localCh, conn->remCh);
        return;
    }

    /* see how many connections exist on this ACL link */
    while (t) {
        if (t->acl == acl->handle && t->inUse)
            numConnsOnThisAcl++;
        t = t->next;
    }

    if (!numConnsOnThisAcl) {
        loge("Number of in-use connections on acl "HCI_CONN_FMT"x: %u\n", HCI_CONN_CONV(acl->conn), numConnsOnThisAcl);
        numConnsOnThisAcl = 1;
    }

    switch (conn->state) {
    case L2C_CONN_STATE_CONN_WAIT:
        if (numConnsOnThisAcl == 1)
            destroyStruct = true;
        tellOtherSide = false;
        closeLink = true;
        break;
    case L2C_CONN_STATE_RX_OPEN_SVC:
        /* cannot destroy struct now, mark it for deletion when service alloc is done */
        conn->state = L2C_CONN_STATE_TEARDOWN;
        tellOtherSide = tellOurSide = false; /* we do not need to do this now */
        break;
    case L2C_CONN_STATE_TX_OPEN_TXED:
        /*
         * The spec has no way for us to cancel a connection request, and we don't want
         * to bother waiting for an unbound time for establishment. We'll just drop it
         * and let the other side deal with it. XXX: should we be wiser about this?
         */
        destroyStruct = true;
        break;

    case L2C_CONN_STATE_ENCR_WAIT:
        tellOurSide = false; /* nodody to tell to */
    case L2C_CONN_STATE_ATHORIZ_WAIT:
    case L2C_CONN_STATE_CFG:
    case L2C_CONN_STATE_ESTABLISHED:
        destroyStruct = true;
        break;

    case L2C_CONN_STATE_DIE_REQD:
    case L2C_CONN_STATE_DIE_RXED:
    case L2C_CONN_STATE_TEARDOWN:
        /* nothing to do here - we're already on it*/
        tellOtherSide = tellOurSide = false; /* we do not need to do this now */
        break;

    default:
       loge("Unknown state %d in disc. this is invariably bad.", conn->state);
       destroyStruct = true;
       break;
    }
    if (tellOurSide)
        if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
            loge("Failed to tell our side that connection is closed\n");
    if (tellOtherSide)
        if (!l2cSigSendDiscReq(acl, conn->localCh, conn->remCh))
             loge("Failed to send disc req msg to the other side\n");
    if (destroyStruct)
        l2cConnStructDelete(conn);
    /*
     * If you ever have both tellOtherSide and closeLink set, you'll have a bad time
     * since data may not go out befor elink dies. we do not do that, but if you ever
     * want to, heed this warning and add acl link destruction to the work queue
     * instead of doing it here.
     */
    if (closeLink)
        l2cAclDownTimerStart(acl);
}

/*
 * FUNCTION: l2cApiServicePsmRegister
 * USE:      Registers a service that works on top of L2CAP PSM-based channels
 * PARAMS:   psm - the PSM you want, or zero to be assigned one
 *           service - the service descriptor for the service
 * RETURN:   the psm you were assigned or zero in case of some error
 * NOTES:    service callbacks may be called before this function returns
 *           DOES NOT AND CANNOT TAKE mConnsLock
 */
psm_t l2cApiServicePsmRegister(psm_t psm, const struct l2cServicePsmDescriptor *service)
{
    struct l2cPsmSvc *svc;

    if (!l2cPsmValid(psm)) {
        logw("Invalid PSM %d\n", psm);
        return false;
    }

    if (!service->mtu && service->serviceInstanceAlloc) {
        logw("Invalid MTU 0 for conn-oriented svc for psm %d\n", psm);
        return false;
    }

    svc = (struct l2cPsmSvc*)calloc(1, sizeof(struct l2cPsmSvc));
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
        psm_t orig_next_psm = mNextPsm;

        do {
            uint64_t mNextPsm_t = mNextPsm + 2; /* only odds allowed */
            psm = mNextPsm;

            /* calculate the next potential valid PSM */
            mNextPsm += 2;
            if (mNextPsm_t & 0x0100) /* no low bit on high byte alowed */
                mNextPsm_t += 0x100;
            if (mNextPsm_t < 0x1000 || mNextPsm_t > 0xFFFF) /* enforce max and min */
                mNextPsm_t = 0x1001;
            mNextPsm = mNextPsm_t;

            /* check if existing PSM is taken */
            if (l2cPsmSvcFindByPsm(psm))
                psm = 0;
            else
                break;

        } while(mNextPsm != orig_next_psm);
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

    /* terminate all connections with that psm that the other side opened */
    pthread_mutex_lock(&mConnsLock);
    do {
         anythingDone = false;
         conn = mConns;
         while (conn){
             if (conn->psm == psm && !conn->weOpened) {

                 l2cConnRequestClose(NULL, conn, true);
                 anythingDone = true;
                 break;
             }
             conn = conn->next;
         }

    } while(anythingDone);
    pthread_mutex_unlock(&mConnsLock);

    /* schedule demolition of the psm itself */
    /* cannot take services lock staring here */
    pthread_mutex_lock(&mSvcsLock);
    svc = l2cPsmSvcFindByPsm(psm);
    if (svc) {
        svc->beingRemoved = true;
        l2cSvcWorkSchedulePsmUnregister(psm, doneCbk, cbkData);
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
 *           DOES NOT AND CANNOT TAKE mConnsLock
 */
bool l2cApiServiceFixedChRegister(uint16_t chan, const struct l2cServiceFixedChDescriptor *service)
{
    struct l2cFixedChSvc *svc;

    if (chan == L2C_CH_SIGNALLING_EDR || chan == L2C_CH_CONNECTIONLESS ||
        chan == L2C_CH_SIGNALLING_LE || !chan || chan >= L2C_NUM_FIXED_CHANNELS)
        return false;

    svc = (struct l2cFixedChSvc*)calloc(1, sizeof(struct l2cFixedChSvc));
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

    /* terminate all connections to that FixedCh that the other side opened */
    pthread_mutex_lock(&mConnsLock);
    do {
         anythingDone = false;
         conn = mConns;
         while(conn) {
             if (!conn->psm && conn->localCh == chan && !conn->weOpened && conn->state != L2C_CONN_STATE_TEARDOWN) {

                 if (conn->instance) {
                     conn->state = L2C_CONN_STATE_TEARDOWN;
                     if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
                         loge("Failed to send close state\n");
                 } else {
                     logw("no instance found for fixed ch %d while unregistering\n", chan);
                     l2cConnStructDelete(conn);
                 }
                 anythingDone = true;
                 break;
             }
             conn = conn->next;
         }

    } while(anythingDone);
    pthread_mutex_unlock(&mConnsLock);

    /* schedule demolition of the fixedCh registration itself */
    /* cannot take services lock staring here */
    pthread_mutex_lock(&mSvcsLock);
    svc = l2cFixedChSvcFindByChan(chan);
    if (svc) {
        svc->beingRemoved = true;
        l2cSvcWorkScheduleFixedChUnregister(chan, doneCbk, cbkData);
    }
    pthread_mutex_unlock(&mSvcsLock);

    return success;
}

/*
 * FUNCTION: l2cConnFindByHandle
 * USE:      Find a connection structure by handle
 * PARAMS:   handle - the handle
 * RETURN:   connection structure or NULL
 * NOTES:    Call with mConnsLock held. Not possible for connections
 *           in conn-wait state since they do not have valid handles
 *           due to lack of an acl channel if
 */
static struct l2cConn* l2cConnFindByHandle(l2c_handle_t handle)
{
    struct l2cConn *conn;

    conn = mConns;

    while (conn && conn->handle != handle)
        conn = conn->next;

    return conn;
}

/*
 * FUNCTION: l2cApiGetBtAddr
 * USE:      Get BT addr of whoever is on the other end of a pipe
 * PARAMS:   handle - the handle
 *           addrP - here is stored the address
 * RETURN:   true on success
 * NOTES:
 */
bool l2cApiGetBtAddr(l2c_handle_t handle, struct bt_addr* addrP)
{
    struct l2cAclConn *aclConn;
    struct l2cConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(handle);
    if (conn) {
        aclConn = l2cAclConnFindByHandle(conn->acl);
        if (aclConn) {
            memcpy(addrP, &aclConn->peerAddr, sizeof(struct bt_addr));
            ret = true;
        }
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cApiGetSelfBtAddr
 * USE:      Get BT addr we presented when we made the connection
 * PARAMS:   handle - the handle
 *           addrP - here is stored the address
 * RETURN:   true on success
 * NOTES:
 */
bool l2cApiGetSelfBtAddr(l2c_handle_t handle, struct bt_addr *addrP)
{
    struct l2cAclConn *aclConn;
    struct l2cConn* conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(handle);
    if (conn) {
        aclConn = l2cAclConnFindByHandle(conn->acl);
        if (aclConn) {
            memcpy(addrP, &aclConn->selfAddr, sizeof(struct bt_addr));
            ret = true;
        }
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cApiGetAclHandle
 * USE:      Get ACL handle for the L2CAP connection
 * PARAMS:   handle - the handle
 * RETURN:   ACL handle, or zero for invalid L2CAP handle
 * NOTES:
 */
acl_handle_t l2cApiGetAclHandle(l2c_handle_t handle)
{
    struct l2cConn* conn;
    acl_handle_t ret = 0;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(handle);
    if (conn) {
        ret = conn->acl;
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}


/*
 * FUNCTION: l2cApiDisconnect
 * USE:      Request a disconnect of a pipe
 * PARAMS:   handle - the handle
 * RETURN:   NONE
 * NOTES:    Disconnect is async and will finish later
 */
void l2cApiDisconnect(l2c_handle_t handle)
{
    struct l2cConn *conn;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (conn)
        l2cConnRequestClose(NULL, conn, true);
    else
        logw("Got request to close nonexistent handle "HANDLEFMT"\n", HANDLECNV(handle));

    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cTryMakeSendProgress
 * USE:      Try to make forward progress on sending data to HCI for radio transmssion.
 *           Called when we either have new data or new credits.
 * PARAMS:   forLeBuffers - which set of buffers we should work on
 * RETURN:   NONE
 * NOTES:    will take mConnsLock
 */
static void l2cTryMakeSendProgress(bool forLeBuffers)
{
    struct l2cSigPduSendItem *picked = NULL;
    struct l2cSigPduSendItem **sigQHP;
    struct l2cSigPduSendItem **sigQTP;
    uint16_t chipBufSz;
    hci_conn_t *qConnP;
    bool *qFirstP;
    uint8_t ret;
    sg *qBufP;
    hci_conn_t sendConn;
    uint16_t wantSendLen, actualSendLen;
    bool sendFirst;
    sg sendSg, remainSg;
    bool hadData = true;

    while(1) {
        pthread_mutex_lock(&mConnsLock);

        if (forLeBuffers) {
            qBufP = &mQueueBufLe;
            qFirstP = &mQueueFirstLe;
            qConnP = &mQueueConnLe;
            sigQHP = &sigSendHeadLe;
            sigQTP = &sigSendTailLe;
            chipBufSz = mChipBufLenLe;
        } else {
            qBufP = &mQueueBufEdr;
            qFirstP = &mQueueFirstEdr;
            qConnP = &mQueueConnEdr;
            sigQHP = &sigSendHeadEdr;
            sigQTP = &sigSendTailEdr;
            chipBufSz = mChipBufLenEdr;
        }

        /*
         * We always send SIG frames ahead of DATA frames, but we cannot do that if we
         * already sent a fragmented data frame's start on the same ACL link, so we
         * check for that special case here. We can reorder SIG frames for different
         * links though. So this case only bites us if we only have SIG frames enqueued
         * for the same link we have data frame half-sent on. The code segment that
         * follows will detect if we cannot send SIG frames for some conection at this
         * time and will find us another SIG frame to send, if there is one. Else it will
         * just pick the first SIG frame. A further complication is if we pick a SIG frame
         * that does not fit in the controller's buffer. In this case we'd have to somehow
         * rememebr to send the rest of it later, but if our queue already has a DATA
         * packet it in (in a half-sent state), we're royally boned. For this case, we also
         * avoid SIG packets over controller's buffer size while we have a half-sent DATA
         * packet in the queue.
         */
        picked = *sigQHP;
        while (picked && *qBufP && ((!*qFirstP && picked->aclConnID == *qConnP) || (sgLength(picked->pdu) > chipBufSz)) )
            picked = picked->next;

        if (picked){ /* we have a SIG frame to send */
            sendConn = picked->aclConnID;
            sendSg = picked->pdu;
            sendFirst = true;

            if (!picked->hasL2cHdr) {
                struct l2cFrmHdr frm;

                utilSetLE16(&frm.len, sgLength(picked->pdu));
                utilSetLE16(&frm.cid, picked->cid);

                if (!sgConcatFrontCopy(picked->pdu, &frm, sizeof(frm)))
                    loge("catastrophic failure in concatting l2c header to SIG frm\n");
                else
                    picked->hasL2cHdr = true;
            }

        } else { /* we continue the existing DATA frame */
            sendConn = *qConnP;
            sendSg = *qBufP;
            sendFirst = *qFirstP;
        }

        if (!sendConn) { /* nothing to send */
            pthread_mutex_unlock(&mConnsLock);
            if (!hadData)
                break;
            hadData = false;
            if (forLeBuffers || !l2cAdvMayTx())
                multiNotifNotify(mWriteNotif, NULL);
            continue;
        }
        hadData = true;

        if (sgLength(sendSg) > chipBufSz) {
            remainSg = sgSplit(sendSg, chipBufSz);
            if (!remainSg) {
                loge("Failed to split TX sg\n");
                ///TODO: this?
            }
        } else
            remainSg = NULL;

        ret = hciTryToTx(sendConn, sendSg, sendFirst ? (remainSg ? HCI_BOUNDARY_START : HCI_BOUNDARY_COMPLETE) : HCI_BOUNDARY_CONT);
        if (ret == HCI_TX_SEND_ERROR)
            logw("State indeterminate after hci tx fail!\n");

        if (ret == HCI_TX_SEND_OK || ret == HCI_TX_SEND_NO_CONN || ret == HCI_TX_SEND_ERROR) { /* either way we do not resend this in its current state */

            if (ret != HCI_TX_SEND_OK)
                sgFree(sendSg);

            if (picked) { /* dequeue the SIG frame we [partially] sent */
                if (picked->prev)
                    picked->prev->next = picked->next;
                else
                    *sigQHP = picked->next;

                if (picked->next)
                    picked->next->prev = picked->prev;
                else
                    *sigQTP = picked->prev;

                free(picked);
            } else { /* clear out the global queue state - if needed it will be reset later */
                *qConnP = 0;
                *qBufP = NULL;
                *qFirstP = true;
            }
        }

        if (ret == HCI_TX_SEND_ERROR && remainSg) {
            logw("Dropping continuation of a msg that failed to send\n");
            sgFree(remainSg);
            remainSg = NULL;
        }

        if (ret == HCI_TX_SEND_OK || ret == HCI_TX_SEND_ERROR) {

            if (remainSg) { /* not done? enqueue the rest of the fragment */
                *qConnP = sendConn;
                *qBufP = remainSg;
                *qFirstP = false;
            }
        }

        /* releasing the lock here allows threads waiting to write to do so and helps keep the pipeline full */
        pthread_mutex_unlock(&mConnsLock);

        if (ret == HCI_TX_SEND_NO_CREDITS)
            break;
    }
}

/*
 * FUNCTION: l2cApiRegisterWriteBufNotif
 * USE:      Register for write buffer notifs
 * PARAMS:   cbk - the cbk to call
 *           cbkData - the data to pass to it
 * RETURN:   handle fo the notif for l2cApiUnegisterWriteBufNotif()
 * NOTES:    cbk may be called before this func returns
 */
uniq_t l2cApiRegisterWriteBufNotif(multiNotifCbk cbk, void *cbkData)
{
    return multiNotifRegister(mWriteNotif, cbk, cbkData);
}

/*
 * FUNCTION: l2cApiUnegisterWriteBufNotif
 * USE:      Unregister from write buffer notif created by l2cApiRegisterWriteBufNotif()
 * PARAMS:   notifHandle - the notif handle
 * RETURN:   NONE
 * NOTES:    cbk will definitely not be called once this func returns
 */
void l2cApiUnegisterWriteBufNotif(uniq_t notifHandle)
{
    multiNotifUnregister(mWriteNotif, notifHandle);
}

/*
 * FUNCTION: l2cAclCreditAvail
 * USE:      Called by lower layers when a send credit becomes available
 * PARAMS:   le - was it an LE credit
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclCreditAvail(bool le)
{
    if (!l2cSvcWorkScheduleTryTx(le))
        loge("Failed to schedule TX attempt\n");
}

/*
 * FUNCTION: l2cDataSendOnAclLink
 * USE:      Request a send on an ACL link of data with a header
 * PARAMS:   aclConn - the link
 *           hdr - header to prepend IFF accepted for TX (can be NULL if hdrLen is 0)
 *           hdrLen - size fo said header
 *           data - the data to send (can be null if hdrLen is nonzero)
 * RETURN:   L2C_TX_*
 * NOTES:    call with mConnsLock held
 */
static uint8_t l2cDataSendOnAclLink(struct l2cAclConn *aclConn, const void *hdr, uint32_t hdrLen, sg data)
{
    uint8_t ret = L2C_TX_ACCEPTED;
    sg *queueBufP;
    hci_conn_t *queueConnP;
    bool *queueFirstP;
    bool useLeBuffers;
    bool freeData = false;

    /* see which queue (LE/EDR) we need to use */
    useLeBuffers = BT_ADDR_IS_LE(aclConn->peerAddr) && !mChipJointBuffers;
    if (useLeBuffers) {
        queueBufP = &mQueueBufLe;
        queueFirstP = &mQueueFirstLe;
        queueConnP = &mQueueConnLe;
    } else {
        queueBufP = &mQueueBufEdr;
        queueFirstP = &mQueueFirstEdr;
        queueConnP = &mQueueConnEdr;
    }

    /* if queue is not busy, make it busy */
    if (*queueBufP)
        ret = L2C_TX_TRY_LATER;
    else {
        if (!data){
            data = sgNew();
            freeData = true;
        }

        if (!data) {
            loge("Failed to alloc sg for data-free l2c acl send\n");
            ret = L2C_TX_ERROR;
        } else if (!sgConcatFrontCopy(data, hdr, hdrLen)) {
            loge("Failed to prepend L2C header\n");
            ret = L2C_TX_ERROR;
        } else {
            *queueBufP = data;
            *queueFirstP = true;
            *queueConnP = aclConn->conn;
            if (!l2cSvcWorkScheduleTryTx(useLeBuffers))
                loge("Failed to schedule TX attempt\n");
        }
    }

    if (ret != L2C_TX_ACCEPTED && freeData && data)
        sgFree(data);

    return ret;
}

/*
 * FUNCTION: l2cApiDataTx
 * USE:      Request a send on a pipe
 * PARAMS:   handle - the handle
 *           data - the data
 * RETURN:   L2C_TX_*
 * NOTES:    Never blocks!
 */
uint8_t l2cApiDataTx(l2c_handle_t handle, sg data)
{
    struct l2cAclConn *aclConn = NULL;
    uint8_t ret = L2C_TX_ACCEPTED;
    struct l2cConn *conn;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(handle);
    if (conn)
        aclConn = l2cAclConnFindByHandle(conn->acl);

    /* we only support L2CAP simple mode for now */

    if (!conn) {
        ret = L2C_TX_NO_CONN;
        logw("Request send on invalid handle "HANDLEFMT"\n", HANDLECNV(handle));
    } else if (!aclConn) {
        ret = L2C_TX_NO_CONN;
        logw("Request send on invalid ACL handle "HANDLEFMT"\n", HANDLECNV(conn->acl));
    } else if (conn->state != L2C_CONN_STATE_ESTABLISHED) {
        /*
         * In this case, we could send a retry-later return code, but if you tried to send
         * at this point, you clearly do not know right from wrong, and do not deserve
         * another chance
         */
        ret = L2C_TX_ERROR;
        logw("Attempt to send data over connection in state %d\n", conn->state);
    } else if (sgLength(data) > conn->theirMtu) {
        ret = L2C_TX_ERROR;
        logw("Attempt to send data over MTU size (%u > %u)\n", sgLength(data), conn->theirMtu);
    } else {
        struct l2cFrmHdr frm;

        if (!conn->inUse) {
            logd("Marking conn "HANDLEFMT" as in use due to TX\n", HANDLECNV(conn->handle));
            conn->inUse = 1;
            l2cAclDownTimerStop(aclConn);
        }

        if (conn->mode == L2C_MODE_BASIC) {

            utilSetLE16(&frm.len, sgLength(data));
            utilSetLE16(&frm.cid, conn->remCh);

            ret = l2cDataSendOnAclLink(aclConn, &frm, sizeof(frm), data);
        } else
            ret = l2cAdvUserTx(conn, data);
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cApiConnectionlessDataTx
 * USE:      Request a send on a connectionless pipe
 * PARAMS:   addr - the peer address
 *           psm - the PSM to send to
 *           data - the data
 * RETURN:   L2C_TX_*
 * NOTES:    Never blocks!
 */
uint8_t l2cApiConnectionlessDataTx(const struct bt_addr *addr, psm_t psm, sg data)
{
    struct l2cAclConn* aclConn;
    hci_conn_t aclConnID = 0;
    bool ret;

    if (!l2cPsmValid(psm)) {
        logw("Invalid PSM %d\n", psm);
        return false;
    }

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByAddr(addr);
    if (!aclConn) {
        logw("Dropping connectionless send to a device whom we aren't connected to\n");
        ret = L2C_TX_ERROR;
    } else if (!BT_ADDR_IS_EDR(aclConn->peerAddr)) {
        logw("Dropping connectionless send to an LE device\n");
        ret = L2C_TX_ERROR;
    } else if (aclConn->connectionlessMtu < sgLength(data)) {
        logw("Dropping connectionless send over MTU %u > %u\n", sgLength(data), aclConn->connectionlessMtu);
        ret = L2C_TX_ERROR;
    } else {
        uint8_t hdr[sizeof(struct l2cFrmHdr) + sizeof(psm_t)];
        struct l2cFrmHdr *frm = (struct l2cFrmHdr*)hdr;

        utilSetLE16(&frm->len, sgLength(data) + sizeof(psm_t));
        utilSetLE16(&frm->cid, L2C_CH_CONNECTIONLESS);
        utilSetLE16(frm + 1, psm);

        ret = l2cDataSendOnAclLink(aclConn, hdr, sizeof(hdr), data);
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2cApiLeEncryptConn
 * USE:      Encrypt a connection with the given key
 * PARAMS:   handle - L2CAP connection handle
 *           rand - random value
 *           ediv - encrypted diversifier value
 *           key - LTK or STK applied during encryption
 * RETURN:
 * NOTES:
 */
bool l2cApiLeEncryptConn(l2c_handle_t handle, uint64_t rand, uint16_t ediv, const uint8_t *key)
{
    bool ret = false;
    struct l2cAclConn *aclConn;
    struct l2cConn *conn;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Cannot find Connection to encrypt\n");
    } else {
        aclConn = l2cAclConnFindByHandle(conn->acl);
        if (!aclConn)
            loge("Cannot find Connection to encrypt\n");
        else if (aclConn->encrypted)
            logw("Connection was encrypted already\n");
        else
            ret = hciLeEncryptConn(aclConn->conn, rand, ediv, key);
    }
    pthread_mutex_unlock(&mConnsLock);

    return ret;
}

/*
 * FUNCTION: l2ccAclLinkUpTimeout
 * USE:      Timeout timer for ACL link opening
 * PARAMS:   which - the timer ID
 *           aclConnHandle - acl connection handle
 * RETURN:   tNONE
 * NOTES:
 */
static void l2cAclLinkUpTimeout(uniq_t which,  uint64_t aclConnHandle)
{
    struct l2cAclConn *aclConn;

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByHandle(aclConnHandle);
    if (aclConn && aclConn->openTimer == which) {
        logi("ACL link open failed for "HANDLEFMT"\n", HANDLECNV(aclConn->handle));
        aclConn->state = L2C_ACL_STATE_TEARDOWN;
        if (!l2cSvcWorkScheduleAclLinkClose(&aclConn->peerAddr))
            loge("Failed to schedule call to close ACL link "HANDLEFMT"\n", HANDLECNV(aclConn->handle));
        l2cAclLinkDownInt(aclConn, false);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cCreateAndOpenNewAclConn
 * USE:      Request an ACL connection be created
 * PARAMS:   addr - the peer address
 * RETURN:   true is it was requested. False on immediate error.
 * NOTES:    call with mConnsLock held
 */
static struct l2cAclConn* l2cCreateAndOpenNewAclConn(const struct bt_addr* addr)
{
    struct l2cAclConn *conn;

    conn = l2cAclConnFindByAddr(addr);
    if (conn) {
        if (conn->state == L2C_ACL_STATE_TEARDOWN) {
            logw("Asked to establish existing ACL conn as it goes down. Bad luck\n");
            return NULL;
        } else {
            logw("Asked to establish existing ACL conn\n");
            return conn;
        }
    }

    conn = (struct l2cAclConn*)calloc(1, sizeof(struct l2cAclConn));
    if (!conn) {
        loge("Out of memory\n");
        return NULL;
    }

    memcpy(&conn->peerAddr, addr, sizeof(conn->peerAddr));
    conn->handle = uniqGetNext();
    conn->state = L2C_ACL_STATE_PENDING;
    conn->prev = NULL;
    conn->reassSg = NULL;
    conn->sigMtu = L2C_MIN_SIG_MTU;

    if (!l2cSvcWorkScheduleAclLinkOpen(&conn->peerAddr)) {
        free(conn);
        conn = NULL;
    } else {
        conn->next = mAclConns;
        if (mAclConns)
            mAclConns->prev = conn;
        mAclConns = conn;
        conn->openTimer = timerSet(L2C_ACL_LINK_UP_TIMEOUT, l2cAclLinkUpTimeout, conn->handle);
        if (!conn->openTimer)
            loge("Failed to set open timeout for ACL link\n");
    }

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
bool l2cApiCreatePsmConnection(psm_t psm, const struct bt_addr* addr, uint16_t mtu, l2cStateCbk stateCbk, void* userData, void* instance)
{
    struct l2cAclConn *aclConn;
    struct l2cConn *conn = NULL;
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

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByAddr(addr);
    if (aclConn && aclConn->state == L2C_ACL_STATE_TEARDOWN) {
        logw("Trying to open a connection over a going-down ACL link. Bad luck\n");
        goto out;
    } else if (!aclConn) {
        logd("acl not found for connection requested, making one\n");
        aclConn = l2cCreateAndOpenNewAclConn(addr);
    }
    if (!aclConn) {
        loge("Failed to create/open/find ACL conn\n");
        goto out;
    }

    conn = l2cNewConnStruct(L2C_CONN_STATE_CONN_WAIT, aclConn->handle, 0, 0, psm, mtu, true);
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        goto out;
    }

    /* create the conn */
    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;
    conn->inUse = 1;

    if (aclConn->conn == L2C_ACL_STATE_ESTABLISHED) {
        uint8_t ident;
        uint16_t localCh = l2cAclFreeCid(aclConn);

        if (!localCh) {
            logw("No free channels for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
            goto out;
        }

        ident = l2cSigSendConnectionReq(aclConn, psm, localCh);
        if (!ident) {
            l2cSvcWorkScheduleStateCallDirect(stateCbk, userData, instance, L2C_STATE_CLOSED, NULL, 0);
            loge("Failed to request opening of a psm channel\n");
            goto out;
        }
        conn->localCh = localCh;
        conn->state = L2C_CONN_STATE_TX_OPEN_TXED;
        conn->ident = ident;
        l2cAclDownTimerStop(aclConn);
        l2cConnTimer(conn, L2C_L2CAP_OPEN_TIMEOUT);
    }
    conn->next = mConns;
    if (mConns)
        mConns->prev = conn;
    mConns = conn;
    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    if (!ret && conn)
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
    struct l2cConn *conn = NULL;
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

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByAddr(addr);
    if (aclConn && aclConn->state == L2C_ACL_STATE_TEARDOWN) {
        logw("Trying to open a connection over a going-down ACL link. Bad luck\n");
        goto out;
    } else if (!aclConn) {
        logd("acl not found for connection requested, making one\n");
        aclConn = l2cCreateAndOpenNewAclConn(addr);
    }
    if (!aclConn) {
        loge("Failed to create/open/find ACL conn\n");
        goto out;
    }

    /* check is same FixedCh already exists */
    conn = mConns;
    while(conn && (conn->acl != aclConn->handle || conn->psm || conn->localCh != chan))
         conn = conn->next;
    if (conn) {
        logi("Ignoring attempt to open FixedCh %d conn when existing exists\n", chan);
        goto out;
    }

    conn = l2cNewConnStruct(L2C_CONN_STATE_CONN_WAIT, aclConn->handle, chan, chan, 0, 0, true);
    if (!conn) {
        loge("Failed to allocate an L2C conn\n");
        return false;
    }

    /* create the conn */
    conn->handle = uniqGetNext();
    conn->stateCbk = stateCbk;
    conn->userData = userData;
    conn->instance = instance;
    conn->inUse = 1;

    if (aclConn->conn == L2C_ACL_STATE_ESTABLISHED) {
        l2cChannelReadyForData(aclConn, conn);
        l2cAclDownTimerStop(aclConn);
    }

    conn->next = mConns;
    if (mConns)
        mConns->prev = conn;
    mConns = conn;
    ret = true;

out:
    pthread_mutex_unlock(&mConnsLock);
    if (!ret && conn)
        free(conn);
    return ret;
}

/*
 * FUNCTION: l2cAclLinkEncrChangeInt
 * USE:      Called to change an ACL link's encr settings
 * PARAMS:   aclConn - the ACL link at hand
 *           isEncrypted - is it now encrypted?
 *           isMitmSafe - is it now MITM-safe?
 *           isKeyRefreshed - is a new key used for encryption
 * RETURN:   NONE
 * NOTES:    Call with mConnsLock held
 */
static void l2cAclLinkEncrChangeInt(struct l2cAclConn *aclConn, bool isEncrypted, bool isMitmSafe,
                                    bool isKeyRefreshed)
{
    struct l2cConn *iter = mConns, *conn;
    struct l2cEncrState es;
    bool changed;

    changed = (!aclConn->encrypted != !isEncrypted) || (!aclConn->mitmSafe != !isMitmSafe);
    if (!isKeyRefreshed && !changed) {
        logd("Unchanged encr call\n");
        return;
    }

    aclConn->encrypted = isEncrypted;
    aclConn->mitmSafe = isMitmSafe;
    es.isEncr = isEncrypted;
    es.isMitmSafe = isMitmSafe;

    /* at this point we're sure connection encryption status changed for non-key-refreshed, so
     * inform everyone
     */
    while (iter) {
        conn = iter;
        iter = iter->next;

        /* wrong link? */
        if (conn->acl != aclConn->handle)
            continue;

        /* handle connections waiting for encryption */
        if (conn->state == L2C_CONN_STATE_ENCR_WAIT) {

            if (conn->psm) {
                /* handle PSM connections */
                if(!l2cSvcWorkSchedulePsmAlloc(conn->psm, conn->handle))
                    loge("Failed to re-enquque PSM alloc on encr\n");
            } else if (!l2cSvcWorkScheduleFixedChAlloc(conn->localCh, conn->handle)) {
                /* handle FixedCh */
                loge("Failed to re-enquque fixed alloc on encr\n");
            }
        }

        /* notify existing connections */
        if (conn->state == L2C_CONN_STATE_ESTABLISHED &&
            !l2cSvcWorkScheduleStateCall(
                conn, isKeyRefreshed ? L2C_STATE_ENCR_KEY_REF : L2C_STATE_ENCR, &es, sizeof(es))) {
            loge("Failed to send close state\n");
        }
    }
}

/*
 * FUNCTION: l2cAclConnTeardownTimerCbk
 * USE:      Timer callback for ACL link down
 * PARAMS:   which - timer handle
 *           cbkData - the callback data
 * RETURN:   NONE
 * NOTES:
 */
static void l2cAclConnTeardownTimerCbk(uniq_t which, uint64_t cbkData)
{
    uniq_t aclHandle = (uniq_t)cbkData;
    struct l2cAclConn *aclConn;

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindByHandle(aclHandle);
    if (aclConn && aclConn->teardownTimer == which) {
        logd("Link timer expired for acl "HCI_CONN_FMT"x. Closing\n", HCI_CONN_CONV(aclConn->conn));
        aclConn->state = L2C_ACL_STATE_TEARDOWN;
        if (!l2cSvcWorkScheduleAclLinkClose(&aclConn->peerAddr))
            loge("Failed to schedule call to close acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
        l2cAclLinkDownInt(aclConn, true);
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclDownTimerStart
 * USE:      Schedule a timer for ACL link shutdown
 * PARAMS:   aclConn - the connection
 * RETURN:   NONE
 * NOTES:    Call this when you're done with the link and you're the last one.
 *           Call with mConnsLock held
 */
static void l2cAclDownTimerStart(struct l2cAclConn *aclConn)
{
    if (aclConn->teardownTimer)
        timerCancel(aclConn->teardownTimer);

    aclConn->teardownTimer = timerSet(L2C_ACL_LINK_TIMOUT, l2cAclConnTeardownTimerCbk, aclConn->handle);
    if (!aclConn->teardownTimer)
        loge("Failed to set teardown timer for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
}

/*
 * FUNCTION: l2cAclDownTimerStop
 * USE:      Remove a timer for ACL link shutdown
 * PARAMS:   aclConn - the connection
 * RETURN:   NONE
 * NOTES:    Call this when you start using a link
 *           Call with mConnsLock held
 */
static void l2cAclDownTimerStop(struct l2cAclConn *aclConn)
{
    if (aclConn->teardownTimer)
        timerCancel(aclConn->teardownTimer);
    aclConn->teardownTimer = 0;
}

/*
 * FUNCTION: l2cAclLinkUp
 * USE:      Called when a new ACL link comes up
 * PARAMS:   aclConn - the conection ID
 *           peerAddr - the peer address
 *           selfAddr - whom we pretended to be when we connected
 *           isMaster - are we the master on the link?
 *           isEncrypted - is link encrypted?
 *           isMitmSafe - is link MITM safe?
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkUp(hci_conn_t aclConnID, const struct bt_addr *peerAddr, const struct bt_addr *selfAddr, bool isMaster, bool isEncrypted, bool isMitmSafe)
{
    struct l2cAclConn *aclConn;
    struct l2cConn *connIter;
    struct l2cFixedChSvc *svc;
    bool inUse = false;

    pthread_mutex_lock(&mConnsLock);
    if (l2cAclConnFindById(aclConnID))
        loge("Duplicate ID up (acl "HCI_CONN_FMT"x)\n", HCI_CONN_CONV(aclConnID));
    else {
        /* TODO(mcchou): if including "hardware/bluetooth.h", complilatino fails with error
         * aapiGatt.h:4:32: fatal error: hardware/bluetooth.h: No such file or directory
         * As an workaround, we marked out the following function call to the userspace for now.
         * aapiAclStateChanged(0, peerAddr, true);
         */
        aclConn = l2cAclConnFindByAddr(peerAddr);
        if (aclConn) {
            if (aclConn->state == L2C_ACL_STATE_PENDING) {
                aclConn->openTimer = 0;
                logd("Us-initiated link up (acl "HCI_CONN_FMT"x)\n", HCI_CONN_CONV(aclConnID));
            } else {
                loge("Duplicate link up with same address and different acl (old "HCI_CONN_FMT"x new "HCI_CONN_FMT"x)."
                    " Ignored.\n", HCI_CONN_CONV(aclConn->conn), HCI_CONN_CONV(aclConnID));
                goto out;
            }
        } else {
            logd("Remote-initiated link up acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConnID));
            aclConn = (struct l2cAclConn*)calloc(1, sizeof(struct l2cAclConn));
            if (aclConn) {
                aclConn->handle = uniqGetNext();
                aclConn->prev = NULL;
                aclConn->reassSg = NULL;
                aclConn->next = mAclConns;
                aclConn->sigMtu = L2C_MIN_SIG_MTU;
                if (mAclConns)
                    mAclConns->prev = aclConn;
                mAclConns = aclConn;
            } else {
                loge("Failed to allocate a new l2cAclConn for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConnID));
                goto out;
            }
        }
        memcpy(&aclConn->peerAddr, peerAddr, sizeof(aclConn->peerAddr));
        memcpy(&aclConn->selfAddr, selfAddr, sizeof(aclConn->selfAddr));
        aclConn->ident = 1;
        aclConn->teardownTimer = 0;
        aclConn->isMaster = isMaster ? 1 : 0;
        aclConn->connectionlessMtu = L2C_MIN_CONNECTIONLESS_MTU;
        aclConn->connUpdtRequestIdent = 0;
        aclConn->nextChan = L2C_NUM_FIXED_CHANNELS;
        aclConn->conn = aclConnID;
        aclConn->encrypted = isEncrypted;
        aclConn->mitmSafe = isMitmSafe;

        /* send an info req if EDR */
        if (BT_ADDR_IS_EDR(*peerAddr)) {
            aclConn->state = L2C_ACL_STATE_CFG;
            aclConn->reqIdent = l2cSigSendInfoReq(aclConn, L2C_SIG_INFO_TYPE_UNCONN_MTU);
            if (!aclConn->reqIdent) {
                aclConn->state = L2C_ACL_STATE_ESTABLISHED;
                logw("Failed to send req for connectionless mtu for acl "HCI_CONN_FMT"x. It will not be known\n", HCI_CONN_CONV(aclConn->conn));
            }
        } else
            aclConn->state = L2C_ACL_STATE_ESTABLISHED;

        /* handle all pending-open connections */
        connIter = mConns;
        while (connIter){
            uint8_t ident = 0;
            uint16_t localCh;
            struct l2cConn *conn = connIter;
            connIter = connIter->next;

            if (conn->acl != aclConn->handle) /* not for this ACL link */
                continue;

            if (conn->state != L2C_CONN_STATE_CONN_WAIT) { /* wrong state? */
                loge("L2C connection for new acl "HCI_CONN_FMT"x not in wait state on link up! Dropping\n", HCI_CONN_CONV(aclConnID));
                /* There is no sure way to delete this - we have no idea what state it is in. Do our best to not make it worse and do nothing */
                l2cConnStructDelete(conn);
                continue;
            }

            if (!conn->psm) { /* FixedCh */
                inUse = true;
                l2cChannelReadyForData(aclConn, conn);
                continue;
            }

            /* PSM connection */
            localCh = l2cAclFreeCid(aclConn);
            if (localCh) {
                 ident = l2cSigSendConnectionReq(aclConn, conn->psm, localCh);
                 conn->localCh = localCh;
            }

            if (ident)
                inUse = true;
            else {
                loge("Failed to request opening of a psm channel\n");
                if (!l2cSvcWorkScheduleStateCall(conn, L2C_STATE_CLOSED, NULL, 0))
                    loge("Failed to send close state\n");
                l2cConnStructDelete(conn);
                continue;
            }
            conn->state = L2C_CONN_STATE_TX_OPEN_TXED;
            conn->ident = ident;
            l2cConnTimer(conn, L2C_L2CAP_OPEN_TIMEOUT);
        }

        /* Bring up all FixedCh services */
        pthread_mutex_lock(&mSvcsLock);
        svc = mFixedSvcs;
        while (svc) {
            if (!svc->beingRemoved) {
                /* See if a conn is already pending. If so we'll not instantiate the service.
                 * This may seem strange, but unlike PSM world, here we cannot have two FixedCh
                 * connections at the same time */
                struct l2cConn *conn = mConns;
                while (conn && (conn->acl != aclConn->handle || conn->psm || conn->localCh != svc->chan))
                    conn = conn->next;

                if (conn) {
                    logi("Not initing FixedCh %d svc since connection is pending\n", svc->chan);
                    continue;
                }

                conn = l2cNewConnStruct(L2C_CONN_STATE_RX_OPEN_SVC, aclConn->handle, svc->chan, svc->chan, 0, 0, false);
                if (!conn) {
                    logw("Not initing FixedCh %d svc - cannot alloc conn struct\n", svc->chan);
                    continue;
                }

                conn->inUse = 0; /* until actually used */
                if (l2cSvcWorkScheduleFixedChAlloc(svc->chan, conn->handle)) {
                    conn->next = mConns;
                    if (mConns)
                        mConns->prev = conn;
                    mConns = conn;
                } else {
                    loge("Failed to schedule FixedCh alloc for ch %d\n", svc->chan);
                    free(conn);
                }
            }
            svc = svc->next;
        }
        pthread_mutex_unlock(&mSvcsLock);

        if (!inUse) {
            l2cAclDownTimerStart(aclConn);
            aclConn->teardownTimer = timerSet(L2C_ACL_LINK_TIMOUT, l2cAclConnTeardownTimerCbk, aclConn->handle);
            if (!aclConn->teardownTimer)
                loge("Failed to set teardown timer for acl "HCI_CONN_FMT"x\n", HCI_CONN_CONV(aclConn->conn));
        }
    }

out:
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclLinkParamsChange
 * USE:      Called when LE ACL link params change
 * PARAMS:   aclConnID - the conection ID
 *           interval - the new interval
 *           latency - the new latency
 *           timeout - the new timeout
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkParamsChange(hci_conn_t aclConnID, bool success, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    struct l2cAclConn *conn;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cAclConnFindById(aclConnID);
    if (!conn)
        loge("Unable to find acl "HCI_CONN_FMT"x. Dropping params change notification\n", HCI_CONN_CONV(aclConnID));
    else if (!conn->isMaster)
        logi("Got update for acl "HCI_CONN_FMT"x: {%d,%d,%d}\n", HCI_CONN_CONV(aclConnID), interval, latency, timeout);
    else if (conn->connUpdtRequestIdent) {
        if (!l2cSigSendConnUpdateRsp(conn, conn->connUpdtRequestIdent, success ? L2C_SIG_CONN_UPDT_OK : L2C_SIG_CONN_UPDT_NO))
            loge("Failed to sent connection update response frame\n");
        conn->connUpdtRequestIdent = 0;
    } else if (conn->connUpdtDoneCbk) {
        if (!l2cSvcWorkScheduleConnUpdtCbk(conn->connUpdtDoneCbk, conn->connUpdtDoneCbkData, success))
            loge("Failed to schedule connection update callback\n");
        conn->connUpdtDoneCbk = NULL;
    }
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclLinkDownInt
 * USE:      Called when an ACL link goes down or is about to go down
 *           Shuts down all our state for said link.
 * PARAMS:   aclConn - the ACL connection struct
 * RETURN:   NONE
 * NOTES:    call withmConnsLock held
 */
static void l2cAclLinkDownInt(struct l2cAclConn *aclConn, bool tellOtherSide)
{
    struct l2cConn *conn = mConns, *t;

    while (conn && conn->acl == aclConn->handle) {
        t = conn;
        conn = conn->next;
        l2cConnRequestClose(aclConn, t, tellOtherSide);
    }
    l2cAclConnStructDelete(aclConn);
}

/*
 * FUNCTION: l2cAclLinkDown
 * USE:      Called when a new ACL link goes down
 * PARAMS:   aclConn - the conection ID
 *           reason - the hci reason code
 * RETURN:   NONEI
 * NOTES:
 */
void l2cAclLinkDown(hci_conn_t aclConnID, uint8_t reason)
{
    struct l2cAclConn *aclConn;

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindById(aclConnID);
    if (aclConn) {
        /* TODO(mcchou): if including "hardware/bluetooth.h", complilatino fails with error
         * aapiGatt.h:4:32: fatal error: hardware/bluetooth.h: No such file or directory
         * As an workaround, we marked out the following function call to the userspace for now.
         * aapiAclStateChanged(0, &aclConn->peerAddr, false);
         */
        logd("Link acl "HCI_CONN_FMT"x actually down. Handling\n", HCI_CONN_CONV(aclConn->conn));
        l2cAclLinkDownInt(aclConn, false);
    } else
        logd("Link acl "HCI_CONN_FMT"x not found. Down ignored.\n", HCI_CONN_CONV(aclConnID));
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclLinkEncrChange
 * USE:      Called when a new ACL encryption changed
 * PARAMS:   aclConnID - the conection ID
 *           isEncrypted - is link now encrypted?
 *           isMitmSafe - is link now MITM-safe?
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkEncrChange(hci_conn_t aclConnID, bool isEncrypted, bool isMitmSafe)
{
    struct l2cAclConn *aclConn;

    logd("l2cAclLinkEncrChange(acl "HCI_CONN_FMT"x, %d, %d)\n", HCI_CONN_CONV(aclConnID), isEncrypted, isMitmSafe);

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindById(aclConnID);
    if (aclConn) {
        logd("Link acl "HCI_CONN_FMT"x encr change to %d,%d. Handling\n", HCI_CONN_CONV(aclConn->conn), isEncrypted,isMitmSafe);
        l2cAclLinkEncrChangeInt(aclConn, isEncrypted, isMitmSafe, false);
    } else
        logd("Link acl "HCI_CONN_FMT"x not found. Encr change ignored.\n", HCI_CONN_CONV(aclConnID));
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cAclLeLtkRequest
 * USE:      Called when an Le connection needs a key
 * PARAMS:   aclConnID - the conection ID
 *           randomNum - provided by other side
 *           diversifier - provided by other side
 * RETURN:   false on immediate failure
 * NOTES:
 */
bool l2cAclLeLtkRequest(hci_conn_t aclConnID, uint64_t randomNum, uint16_t diversifier)
{
    struct l2cKeyReqState keyReq = {.rand = randomNum, .ediv = diversifier, };
    L2capDefaultSecurityManagerCbk defaultSecMgr;
    struct l2cAclConn *aclConn;
    struct l2cConn *conn;
    bool ret = false;

    pthread_mutex_lock(&mSecMgrLock);
    defaultSecMgr = mDefaultSecMgr;
    pthread_mutex_unlock(&mSecMgrLock);

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindById(aclConnID);
    if ((!aclConn || !aclConn->securityManager) && defaultSecMgr && !l2cSvcWorkScheduleDefaultSmCall(defaultSecMgr, aclConnID, randomNum, diversifier))
        logi("Default security manager call failed for acl "HCI_CONN_FMT"x. LTK req ignored.\n", HCI_CONN_CONV(aclConnID));
    else if (!aclConn)
        logw("Link acl "HCI_CONN_FMT"x not found. LTK req ignored.\n", HCI_CONN_CONV(aclConnID));
    else if (!aclConn->securityManager)
        logi("Link acl "HCI_CONN_FMT"x has no assigned security manager. LTK req ignored.\n", HCI_CONN_CONV(aclConnID));
    else {
        conn = l2cConnFindByHandle(aclConn->securityManager);
        if (!conn)
            logi("Link acl "HCI_CONN_FMT"x's assigned security manager does nt exist anymore. LTK req ignored.\n", HCI_CONN_CONV(aclConnID));
        else
            ret = l2cSvcWorkScheduleStateCall(conn, L2C_STATE_KEY_REQ, &keyReq, sizeof(keyReq));
    }

    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiLeProvideLtk
 * USE:      Called by SM - provide key
 * PARAMS:   handle - connection handle
 *           key - key to use or NULL if none
 * RETURN:   false on error
 * NOTES:    proper hci reply command will be called
 */
bool l2cApiLeProvideLtk(l2c_handle_t handle, const uint8_t *key)
{
    struct l2cAclConn *aclConn = NULL;
    struct l2cConn *conn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);
    conn = l2cConnFindByHandle(handle);
    if (conn)
        aclConn = l2cAclConnFindByHandle(conn->acl);

    if (!conn)
        logw("L2C conn "HANDLEFMT"x not found. LTK reply ignored.\n", HANDLECNV(handle));
    else if (!aclConn)
        logw("L2C conn "HANDLEFMT"x's acl conn not found. LTK reply ignored. Also, this is bad!\n", HANDLECNV(handle));
    else
        ret = hciLeProvideLtk(aclConn->conn, key);

    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cAclLinkEncrKeyRefresh
 * USE:      Called when a ACL encryption is refreshed with a new key
 * PARAMS:   aclConnID - the conection ID
 *           isEncrypted - is link now encrypted?
 *           isMitmSafe - is link now MITM-safe?
 * RETURN:   NONE
 * NOTES:
 */
void l2cAclLinkEncrKeyRefresh(hci_conn_t aclConnID, bool isEncrypted, bool isMitmSafe)
{
    struct l2cAclConn *aclConn;

    logd("l2cAclLinkEncrKeyRefresh(acl "HCI_CONN_FMT"x, %d, %d)\n", HCI_CONN_CONV(aclConnID), isEncrypted, isMitmSafe);

    pthread_mutex_lock(&mConnsLock);
    aclConn = l2cAclConnFindById(aclConnID);
    if (aclConn) {
        logd("Link acl "HCI_CONN_FMT"x encr key refreshed and changed to %d,%d. Handling\n", HCI_CONN_CONV(aclConn->conn), isEncrypted,isMitmSafe);
        l2cAclLinkEncrChangeInt(aclConn, isEncrypted, isMitmSafe, true);
    } else
        logd("Link acl "HCI_CONN_FMT"x not found. Encr key refreshed ignored.\n", HCI_CONN_CONV(aclConnID));
    pthread_mutex_unlock(&mConnsLock);
}

/*
 * FUNCTION: l2cApiLeSetSecurityManagerForAclConn
 * USE:      Called by a security manager to become one for a given acl connection
 * PARAMS:   handle - connection handle
 * RETURN:   true if no security manager already existed for a given acl
 *           connection and the requestor became one, requestor already
 *           was the security manager for this acl connection, or the previous
 *           security manager was no longer in existence (l2c conn had been closed)
 * NOTES:
 */
bool l2cApiLeSetSecurityManagerForAclConn(l2c_handle_t handle)
{
    struct l2cConn *conn;
    struct l2cAclConn *aclConn;
    bool ret = false;

    pthread_mutex_lock(&mConnsLock);

    conn = l2cConnFindByHandle(handle);
    if (!conn) {
        loge("Connection not found\n");
        goto out;
    }

    aclConn = l2cAclConnFindByHandle(conn->acl);
    if (!aclConn) {
        loge("ACL link not found\n");
        goto out;
    }

    if (!aclConn->securityManager || !l2cConnFindByHandle(aclConn->securityManager))
        aclConn->securityManager = handle;

    ret = (aclConn->securityManager == handle);

out:
    pthread_mutex_unlock(&mConnsLock);
    return ret;
}

/*
 * FUNCTION: l2cApiLeSetDefaultSecurityManager
 * USE:      Called by a security manager to become one by default, is no per-connection
 *           security manager has been set
 * PARAMS:   cbk - the function to call when a key is needed or NULL
 * RETURN:   none
 * NOTES:    This sets the security manager to be used when no l2c connection yet exists
 *           on an hci acl connection. This means that sm had not yet had a chance to call
 *           l2cApiLeSetSecurityManagerForAclConn(). This can happen if someone connects
 *           and immediately tries to encrypt, with no prior traffic. This is allowed,
 *           but no traffic means that SM has not been opened, and thus has no l2c conn
 *           associated with it. No l2c conn means we have no instance to send the key
 *           state to. This this callback is used. If an l2c conn exists that has called
 *           l2cApiLeSetSecurityManagerForAclConn, it is used for security management
 *           instead.
 */
void l2cApiLeSetDefaultSecurityManager(L2capDefaultSecurityManagerCbk cbk)
{
    /*
     * Since we actualyl pass the callback pointer in the work items on the work queue,
     * we need to make sure none escape this change func call. To do this we use the
     * ability to "flush" the work queue - wait till all current items finish. We do it
     * twice. Once before changing and once after. The first case makes sure that change
     * to NULL indeed no longer calls this callback after this func ends. Second makes
     * sure that a change to a pointer take effect before this func returns.
     */
    pthread_mutex_lock(&mSecMgrLock);
    l2cSvcWorkScheduleFlush();
    mDefaultSecMgr = cbk;
    l2cSvcWorkScheduleFlush();
    pthread_mutex_unlock(&mSecMgrLock);
}

/*
 * FUNCTION: l2cUtilFcs
 * USE:      Calculate an L2CAP FCS (CRC-16 with poly 0x8005) for some data
 * PARAMS:   fcs - input fcs
 *           data - the data
 *           len - the length of said data
 * RETURN:   the fcs
 * NOTES:
 */
static uint16_t l2cUtilFcs(uint16_t fcs, const void *data, uint32_t len)
{
    static const uint16_t crctab[] = {
        0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241, 0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481,
        0x0440, 0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40, 0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0,
        0x0880, 0xC841, 0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40, 0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01,
        0x1DC0, 0x1C80, 0xDC41, 0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641, 0xD201, 0x12C0, 0x1380, 0xD341,
        0x1100, 0xD1C1, 0xD081, 0x1040, 0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240, 0x3600, 0xF6C1, 0xF781,
        0x3740, 0xF501, 0x35C0, 0x3480, 0xF441, 0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41, 0xFA01, 0x3AC0,
        0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840, 0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41, 0xEE01,
        0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40, 0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
        0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041, 0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281,
        0x6240, 0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441, 0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0,
        0x6E80, 0xAE41, 0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840, 0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01,
        0x7BC0, 0x7A80, 0xBA41, 0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40, 0xB401, 0x74C0, 0x7580, 0xB541,
        0x7700, 0xB7C1, 0xB681, 0x7640, 0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041, 0x5000, 0x90C1, 0x9181,
        0x5140, 0x9301, 0x53C0, 0x5280, 0x9241, 0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440, 0x9C01, 0x5CC0,
        0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40, 0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841, 0x8801,
        0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40, 0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
        0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641, 0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081
    };

    const uint8_t *ptr = (const uint8_t*)data;

    while (len--)
        fcs = (fcs >> 8) ^ crctab[(fcs & 0xFF) ^ *ptr++];

    return fcs;
}

/*
 * FUNCTION: l2cUtilFcsSg
 * USE:      Calculate an L2CAP FCS (CRC-16 with poly 0x8005) for an SG
 * PARAMS:   fcs - input fcs
 *           s - the sg
 * RETURN:   the fcs
 * NOTES:
 */
static uint16_t l2cUtilFcsSg(uint16_t fcs, sg s)
{
    void *iter;

    for (iter = sgIterStart(s); iter; iter = sgIterAdvance(iter))
        fcs = l2cUtilFcs(fcs, sgIterCurData(iter), sgIterCurLen(iter));

    return fcs;
}


