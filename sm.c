/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bt.h"
#include "config.h"
#include "l2cap.h"
#include "log.h"
#include "mt.h"
#include "multiNotif.h"
#include "persist.h"
#include "sendQ.h"
#include "sg.h"
#include "timer.h"
#include "types.h"
#include "util.h"

#include "sm.h"

#ifdef _UNITTEST_
#define static_or_test
#else
#define static_or_test static
#endif

/* definition of structs and macros based on Bluetooth spec v4.0 */
/* packet types */
#define SM_PAIRING_REQ                  0x01 /* struct smPairReqResp */
#define SM_PAIRING_RSP                  0x02 /* struct smPairReqResp */
#define SM_PAIRING_CONF                 0x03 /* struct smPairConf */
#define SM_PAIRING_RAND                 0x04 /* struct smPairRand */
#define SM_PAIRING_FAIL                 0x05 /* struct smPairFailed */
#define SM_ENCR_INFO                    0x06 /* struct smKey => LTK */
#define SM_MASTER_INFO                  0x07 /* struct smMasterInfo */
#define SM_IDENTITY_INFO                0x08 /* struct smKey => IRK */
#define SM_IDENTITY_ADDR_INFO           0x09 /* struct smIdentityAddrInfo */
#define SM_SIGNING_INFO                 0x0A /* struct smKey => CSRK */
#define SM_SECURITY_REQUEST             0x0B /* struct smAuthReq */

struct smHdr {
    uint8_t typ;
} __packed;

struct smPairReqResp {
    uint8_t ioCap;
    uint8_t oobDataFlag;
    uint8_t authReq; /* SM_AUTH_REQ_* */
    uint8_t maxKeySz;
    uint8_t initiatorKeyDistr; /* bitfield of SM_KEY_DISTR_* */
    uint8_t responderKeyDistr; /* bitfield of SM_KEY_DISTR_* */
} __packed;

struct smPairConf {
    uint8_t confirmVal[SM_BLOCK_LEN]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smPairRand {
    uint8_t rand[SM_BLOCK_LEN]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smPairFailed {
    uint8_t reason; /* SM_ERR_* */
} __packed;

struct smKey {
    uint8_t key[HCI_LE_KEY_LEN]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smMasterInfo {
    uint16_t ediv;
    uint64_t rand;
} __packed;

struct smIdentityAddrInfo {
    uint8_t addrType;
    uint8_t mac[BT_MAC_LEN];
} __packed;

struct smAuthReq {
    uint8_t authReq; /* SM_AUTH_REQ_* */
} __packed;

/* fail codes */
#define SM_ERR_PASSKEY_ENTRY_FAILED     0x01 /* user cancelled or failed to input key */
#define SM_ERR_OOB_NOT_AVAIL            0x02 /* no OOB data */
#define SM_ERR_AUTH_REQMENTS            0x03 /* auth requirements cannot be met due to IO caps */
#define SM_ERR_CONF_VAL_FAILED          0x04 /* confirm value does not match calculated value */
#define SM_ERR_PAIRING_NOT_SUPPORTED    0x05 /* pairing not supported by this device */
#define SM_ERR_ENCR_KEY_SZ              0x06 /* resulting key size is not long enough */
#define SM_ERR_CMD_NOT_SUPP             0x07 /* received command not supported by this device */
#define SM_ERR_UNSPECCED_REASON         0x08 /* pairing failed due to an unspecified reason */
#define SM_ERR_REPEATED_ATTEMPTS        0x09 /* too rapid an attempt to pair after a failure */
#define SM_ERR_INVALID_PARAMS           0x0A /* unparsable command or invalid params in command */

/* things for authReq fields */
#define SM_AUTH_REQ_BOND_MASK           0x03
#define SM_AUTH_REQ_BOND_NO             0x00
#define SM_AUTH_REQ_BOND_BOND           0x01
#define SM_AUTH_REQ_MITM_FLAG           0x04
#define SM_AUTH_REQ_SC_FLAG             0x08
#define SM_AUTH_REQ_KEYPRESS_FLAG       0x10

/* things for key distr fields */
#define SM_KEY_DISTR_LTK                0x01
#define SM_KEY_DISTR_IRK                0x02
#define SM_KEY_DISTR_CSRK               0x04

/* misc defines */
#define SM_MIN_KEY_LEN                  7  /* bytes */
#define SM_MAX_KEY_LEN                  16 /* bytes */


/* definition of structs and macros based on our implementation */
#define SM_PHASE_START                  0  /* nothing happened yet. */
#define SM_PHASE_SEC_REQ_SENT           1  /* we sent security req. waiting for pair req. */
#define SM_PHASE_REQ_SENT               2  /* we sent pair req. waiting for pair resp. */
#define SM_PHASE_REQ_RESP               3  /* the peer has sent us a pair request, we replied. waiting for Mconfim. OR we sent req and got resp */
#define SM_PHASE_CONF_SENT              4  /* we sent Mconfirm. waiting for Sconfirm. */
#define SM_PHASE_CONF_RESP              5  /* the peer has sent us Mconfirm, we replied with Sconfirm. waiting for Mrand. OR we sent Mconfirm and got Sconfim. */
#define SM_PHASE_RAND_SENT              6  /* we sent Mrand. waiting for Srand. */
#define SM_PHASE_RAND_RESP              7  /* the peer has sent us Mrand, we replied with Srand. OR we sent Mrand and got Srand. */
#define SM_PHASE_STK_SENT               8  /* the controller asked us for LTK, we replied with STK. OR we started encryption with STK. */
#define SM_PHASE_STK_ENCRYPTED          9  /* we received encryption changed event with STK sent previously */
#define SM_PHASE_ENCRYPT_INFO_SENT      10 /* we sent our LTK via encryption information */
#define SM_PHASE_ENCRYPT_INFO_RECEIVED  11 /* we received peer's LTK via encryption information*/
#define SM_PHASE_MASTER_ID_SENT         12 /* we sent our EDIV and Rand via master identification */
#define SM_PHASE_MASTER_ID_RECEIVED     13 /* we received peer's EDIV and Rand via master identification */
#define SM_PHASE_ID_INFO_SENT           14 /* we sent our IRK via identity information */
#define SM_PHASE_ID_INFO_RECEIVED       15 /* we received peer's IRK via identity information */
#define SM_PHASE_ID_ADDR_INFO_SENT      16 /* we sent our public address via identity address information */
#define SM_PHASE_ID_ADDR_INFO_RECEIVED  17 /* we received peer's address via identity address information */
#define SM_PHASE_SIGN_INFO_SENT         18 /* we sent our CSRK via signing information */
#define SM_PHASE_SIGN_INFO_RECEIVED     19 /* we received peer's CSRK via signing information */
#define SM_PHASE_DONE                   20 /* pairing is done. OR the peer was paired previously. */
#define SM_PHASE_LTK_SENT               21 /* the controller asked us for LTK, we replied with LTK. OR we started encryption with LTK. */
#define SM_PHASE_LTK_ENCRYPTED          22 /* pairing is done and we received encryption changed event with LTK sent previously */

#define SM_WAIT_INTVL_FACTOR            2     /* the wait interval grows exponentially with base 2 */
#define SM_DEF_WAIT_INTVL               0     /* 0 sec in msec - default wait interval */
#define SM_MIN_WAIT_INTVL               4000  /* 4 sec in msec - minimum wait interval */
#define SM_MAX_WAIT_INTVL               64000 /* 64 sec in msec - maximum wait interval */
#define SM_STALL_INTVL                  30000 /* 30 sec in msec - timeout of a pairing process */

#define SM_UNAUTH_NO_MITM_PROTECT       0 /* Unauthenticated no MITM protection */
#define SM_AUTH_MITM_PROTECT            1 /* Authenticated MITM protection */

struct smInstance {
    struct smInstance*    prev;
    struct smInstance*    next;

    struct bt_addr        peerAddr;
    struct bt_addr        myAddr;
    l2c_handle_t          conn;
    bool                  isInitiator; /* whether we are the initiator */

    uint8_t               phase;
    uniq_t                stallTimer;  /* the timer tracking whether a pairing session is stalled */

    uint8_t               alg;         /* the SM_ALG_* selected */
    struct smKey          tk;
    struct smKey          stk;
    bool                  isMitmSafe;  /* the security property decided when the pairing algorithm is chosen */

    uint8_t               peerIoCap;
    bool                  peerHasOob;
    uint8_t               peerAuthReq;
    uint8_t               peerMaxKeySz;
    uint8_t               peerKeyDistr;
    struct smPairRand     peerRandNum;
    struct smPairConf     peerConfVal;

    uint8_t               myAuthReq;
    uint8_t               myMaxKeySz;
    uint8_t               myKeyDistr;
    struct smPairRand     myRandNum;
    struct smPairConf     myConfVal;
};

struct smDevNode {
    struct smDevNode      *prev;
    struct smDevNode      *next;
    struct bt_addr        addr;       /* the address of the banned device */
    uniq_t                devId;      /* an unique ID associated with the device */

    uint64_t              waitInvl;   /* the wait interval of initiating/responding the pairing */
    uniq_t                waitTimer;  /* the timer tracking the wait interval of a banned device */
};

/* local settings and state */
static uint8_t mIoCap;
static bool mHasOob;                                          /* whether we support OOB data */

static pthread_mutex_t mInstLock = PTHREAD_MUTEX_INITIALIZER; /* lock for the active instance list */
static struct smInstance *mActInstList = NULL;                /* the active instances */

static pthread_mutex_t mDevLock = PTHREAD_MUTEX_INITIALIZER;  /* lock for the banned device list */
static struct smDevNode *mBannedDevList = NULL;               /* the device banned on pairing */

static struct multiNotifList *mPairObservers = NULL;          /* the observers of pairing state changes */

/* fwd decls */
static bool smGenSTK(struct smInstance *inst);
static bool smGenTK(struct smInstance *inst);
static bool smGenConfVal(struct smPairConf *conf, const struct smInstance *inst, bool check);
static bool smGenRandNum(uint8_t *out, uint8_t numLen);
static bool smGenLtk(struct smInstance *inst);
static bool smGenEdiv(uint16_t *out, const struct smInstance* inst, uint64_t rand, bool check);
static_or_test void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);
static void smNotifyPairObserver(const struct bt_addr *addr, smPairState state, smPairErr err);

/* sendQ for packet transmissions */
static struct sendQ *mSmSendQ;

/* pairing algorithms */
#define SM_ALG_JUST_WORK                0x00
#define SM_ALG_PASS_KEY                 0x01
#define SM_ALG_OOB                      0x02
#define SM_ALG_UNKNOWN                  0xFF

/* table of pairing algorithms - mPairAlgs[responder][initiator] */
static const uint8_t mPairAlgs[5][5] = {
    {SM_ALG_JUST_WORK, SM_ALG_JUST_WORK, SM_ALG_PASS_KEY,  SM_ALG_JUST_WORK, SM_ALG_PASS_KEY },
    {SM_ALG_JUST_WORK, SM_ALG_JUST_WORK, SM_ALG_PASS_KEY,  SM_ALG_JUST_WORK, SM_ALG_PASS_KEY },
    {SM_ALG_PASS_KEY,  SM_ALG_PASS_KEY,  SM_ALG_PASS_KEY,  SM_ALG_JUST_WORK, SM_ALG_PASS_KEY },
    {SM_ALG_JUST_WORK, SM_ALG_JUST_WORK, SM_ALG_JUST_WORK, SM_ALG_JUST_WORK, SM_ALG_JUST_WORK},
    {SM_ALG_PASS_KEY,  SM_ALG_PASS_KEY,  SM_ALG_PASS_KEY,  SM_ALG_JUST_WORK, SM_ALG_PASS_KEY },
};

/*
 * FUNCTION: smSameAddr
 * USE:      Given two struct bt_addr, tell whether they are the same
 * PARAMS:   a - a device address
 *           b - the other device address
 * RETURN:   true if they are the same; false otherwise
 * NOTES:
 */
static bool smSameAddr(const struct bt_addr *a, const struct bt_addr *b)
{
    if (!a || !b)
        return false;
    return !memcmp(a, b, sizeof(struct bt_addr));
}

/*
 * FUNCTION: smBannedDevFree
 * USE:      Free the given device node
 * PARAMS:   dev - the to-be-freed device node
 * RETURN:   NONE
 * NOTES:
 */
static void smBannedDevFree(struct smDevNode *dev)
{
    if (!dev)
        return;

    if (dev->waitTimer)
        timerCancel(dev->waitTimer);

    free(dev);
}

/*
 * FUNCTION: smBannedDevFindByAddr
 * USE:      Find a device node by the address of the device on the banned device list
 * PARAMS:   addr - the device address
 * RETURN:   the pointer to the device node if matched; NULL otherwise
 * NOTES:    call with mDevLock held
 */
static struct smDevNode* smBannedDevFindByAddr(const struct bt_addr *addr)
{
    struct smDevNode *n = mBannedDevList;

    if (!addr)
        return NULL;

    while (n) {
        if (smSameAddr(addr, &n->addr))
            return n;
        n = n->next;
    }
    return NULL;
}

/*
 * FUNCTION: smBannedDevFindById
 * USE:      Find a device node by the device ID on the banned device list
 * PARAMS:   id - the device ID
 * RETURN:   the pointer to the device node if matched; NULL otherwise
 * NOTES:    call with mDevLock held
 */
static struct smDevNode* smBannedDevFindById(uniq_t id)
{
    struct smDevNode *n = mBannedDevList;

    if (!id)
        return NULL;

    while (n) {
        if (n->devId == id)
            return n;
        n = n->next;
    }
    return NULL;
}

/*
 * FUNCTION: smBannedDevDel
 * USE:      Find device node by Device ID and delete it from the banned device list if matched
 * PARAMS:   id - the device ID
 * RETURN:   NONE
 * NOTES:    call with mDevLock held
 */
static void smBannedDevDel(uniq_t id)
{
    struct smDevNode *n;

    if (!id)
        return;

    n = smBannedDevFindById(id);
    if (!n)
        return;

    logd("%s(): Remove device "MACFMT" from the banned list\n", __func__, MACCONV(n->addr.addr));
    if (n->next)
        n->next->prev = n->prev;
    if (n->prev)
        n->prev->next = n->next;
    else
        mBannedDevList = n->next;

    smBannedDevFree(n);
}

/*
 * FUNCTION: smWaitTimerCb
 * USE:      Called when the timer with wait interval expires to allow sending a Pairing request, a
 *           Security request or a Pairing response for a banned device
 * PARAMS:   timerId - an unique ID of a timer
 *           devId - a device ID
 * RETURN:   NONE
 * NOTES:
 */
static void smWaitTimerCb(uniq_t timerId,  uint64_t devId)
{
    pthread_mutex_lock(&mDevLock);
    smBannedDevDel(devId);
    pthread_mutex_unlock(&mDevLock);
}

/*
 * FUNCTION: smBannedDevAdd
 * USE:      Add a device to the banned device list
 * PARAMS:   addr - the device address
 * RETURN:   the pointer to the device node; NULL otherwise
 * NOTES:    call with mDevLock held
 */
static struct smDevNode* smBannedDevAdd(const struct bt_addr *addr)
{
    struct smDevNode *n = NULL;

    if (!addr)
        return NULL;

    n = (struct smDevNode*)calloc(1, sizeof(struct smDevNode));
    if (!n)
        return NULL;

    n->addr = *addr;
    n->devId = uniqGetNext();
    n->waitInvl = SM_MIN_WAIT_INTVL;
    n->waitTimer = timerSet(n->waitInvl, smWaitTimerCb, n->devId);
    if (!n->waitTimer)
        goto fail;

    if (mBannedDevList) {
        n->next = mBannedDevList;
        mBannedDevList->prev = n;
    }
    mBannedDevList = n;
    logd("%s(): Add device "MACFMT" to the banned list\n", __func__, MACCONV(addr->addr));
    return n;

fail:
    logw("%s(): Failed to add device "MACFMT" to the banned list\n", __func__, MACCONV(addr->addr));
    smBannedDevFree(n);
    return NULL;
}

/*
 * FUNCTION: smBannedDevFreeAll
 * USE:      Free all nodes of the banned device list
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void smBannedDevFreeAll(void)
{
    struct smDevNode *n;

    pthread_mutex_lock(&mDevLock);
    while(mBannedDevList) {
        n = mBannedDevList->next;
        smBannedDevFree(mBannedDevList);
        mBannedDevList = n;
    }
    pthread_mutex_unlock(&mDevLock);
}

/*
 * FUNCTION: smBanDeviceForTime
 * USE:      Start or update the wait interval timer to prevent from sending a Pairing request, a
 *           Security request or a Pairing response before the wait interval expires
 * PARAMS:   addr - the address of the device
 * RETURN:   NONE
 * NOTES:
 */
static void smBanDeviceForTime(const struct bt_addr *addr)
{
    struct smDevNode *n;

    if (!addr)
        return;

    pthread_mutex_lock(&mDevLock);

    /* add the newly failed device in the banned device list */
    if (!(n = smBannedDevFindByAddr(addr))) {
        n = smBannedDevAdd(addr);
        logd("%s(): Add device "MACFMT" to banned list and start wait timer with "FMT64"u secs\n",
             __func__, MACCONV(n->addr.addr), CNV64(n->waitInvl / 1000));
    } else {
        /* if the failed device was previously added to the banned device list, increase the wait
         * interval and update timers
         */
        if (n->waitTimer)
            timerCancel(n->waitTimer);

        if (n->waitInvl == SM_DEF_WAIT_INTVL)
            n->waitInvl = SM_MIN_WAIT_INTVL;
        else
            n->waitInvl = n->waitInvl * SM_WAIT_INTVL_FACTOR > SM_MAX_WAIT_INTVL ?
                SM_MAX_WAIT_INTVL : n->waitInvl * SM_WAIT_INTVL_FACTOR;

        n->waitTimer = timerSet(n->waitInvl, smWaitTimerCb, n->devId);
        logd("%s(): Update device "MACFMT" and start wait timer with "FMT64"u secs\n", __func__,
             MACCONV(n->addr.addr), CNV64(n->waitInvl / 1000));
    }

    pthread_mutex_unlock(&mDevLock);
}

/*
 * FUNCTION: smIsDevBanned
 * USE:      Start or update the wait interval timer to prevent from sending a Pairing request, a
 *           Security request or a Pairing response before the wait interval expires
 * PARAMS:   addr - the address of the device
 * RETURN:   is banned or not
 * NOTES:
 */
static bool smIsDevBanned(const struct bt_addr *addr)
{
    struct smDevNode *n;

    pthread_mutex_lock(&mDevLock);
    n = smBannedDevFindByAddr(addr);
    if (n)
        logd("%s(): Device "MACFMT" is banned currently for "FMT64"u secs\n", __func__,
             MACCONV(addr->addr), CNV64(n->waitInvl / 1000));
    pthread_mutex_unlock(&mDevLock);

    return n;
}

/*
 * FUNCTION: smSelPairAlg
 * USE:      Select the pairing algorithm base on IO capability, OOB data and
 *           Authentication request
 * PARAMS:   inst - the instance
 * RETURN:   pairing algorithm used
 * NOTES:
 */
static uint8_t smSelPairAlg(const struct smInstance *inst)
{
    if (!inst)
        return SM_ALG_UNKNOWN;

    if ((mIoCap != HCI_DISP_CAP_DISP_ONLY && mIoCap != HCI_DISP_CAP_DISP_YES_NO &&
         mIoCap != HCI_DISP_CAP_KBD_ONLY && mIoCap != HCI_DISP_CAP_NONE &&
         mIoCap != HCI_DISP_CAP_KBD_DISP)||
        (inst->peerIoCap != HCI_DISP_CAP_DISP_ONLY && inst->peerIoCap != HCI_DISP_CAP_DISP_YES_NO &&
         inst->peerIoCap != HCI_DISP_CAP_KBD_ONLY && inst->peerIoCap != HCI_DISP_CAP_NONE &&
         inst->peerIoCap != HCI_DISP_CAP_KBD_DISP))
        return SM_ALG_UNKNOWN;

    /* check OOB support */
    if (inst->peerHasOob && mHasOob)
        return SM_ALG_OOB;

    /* check MITM flag */
    /* TODO(mcchou): we use what ever requested by the peer device */
    if (!(inst->peerAuthReq & SM_AUTH_REQ_MITM_FLAG))
        return SM_ALG_JUST_WORK;

    /* check IO capability*/
    return inst->isInitiator ? mPairAlgs[inst->peerIoCap][mIoCap] :
                               mPairAlgs[mIoCap][inst->peerIoCap];
}

/*
 * FUNCTION: smIsEncryptExpected
 * USE:      Check if the link is encrypted as negotiated given the encryption
 *           changed state
 * PARAMS:   state - the new l2cap encryption state
 * RETURN:   true if the link encrypted as negotiated
 * NOTES:
 */
static bool smIsEncryptExpected(const struct l2cEncrState *state, const struct smInstance *inst)
{
    if (!inst || !state)
        return false;

    return state->isEncr && state->isMitmSafe == inst->isMitmSafe;
}

/*
 * FUNCTION: smTx
 * USE:      Send a packet to the peer
 * PARAMS:   inst - the instance
 *           pktTyp - packet type
 *           data - the data to send
 *           len - length of said data
 * RETURN:   success
 * NOTES:
 */
static bool smTx(const struct smInstance *inst, uint8_t pktTyp, const void *data, uint8_t len)
{
    sg tmpData = NULL;
    struct smHdr hdr;

    utilSetLE8(&hdr.typ, pktTyp);

    tmpData = sgNewWithCopyData(data, len);
    if (!tmpData) {
        logw("%s(): Failed to alloc SG packet\n", __func__);
        return false;
    }

    if (!sgConcatFrontCopy(tmpData, &hdr, sizeof(hdr))) {
        logw("%s(): Failed to copy SG header data\n", __func__);
        goto tx_err;
    }

    if (!sendQueueTx(mSmSendQ, inst->conn, tmpData)) {
        logw("%s(): Failed to SG packet to sendQueue\n", __func__);
        goto tx_err;
    }

    logd("%s(): Packet successfully sent\n", __func__);
    return true;

tx_err:
        sgFree(tmpData);
        return false;
}

/*
 * FUNCTION: smParsePairReqResp
 * USE:      Parse a struct smPairReqResp into relevant instance variables
 * PARAMS:   err - the output pairing error if there is any
 *           inst - the instance
 *           pairReqResp - the struct
 *           isReq - indicate if we are parsing a pairing request
 * RETURN:   return 0 if all values were read & found valid; SM_ERR_* otherwise
 * NOTES:
 */
static uint8_t smParsePairReqResp(smPairState *err, struct smInstance *inst,
                                  const struct smPairReqResp *pairReqResp, bool isReq)
{
    uint8_t tmp, initKeyDistr, rspKeyDistr;

    tmp = utilGetLE8(&pairReqResp->ioCap);
    if (tmp != HCI_DISP_CAP_DISP_ONLY && tmp != HCI_DISP_CAP_DISP_YES_NO &&
        tmp != HCI_DISP_CAP_KBD_ONLY && tmp != HCI_DISP_CAP_NONE &&
        tmp != HCI_DISP_CAP_KBD_DISP) {
        logd("Invalid io caps RXed 0x%02X\n", tmp);
        if (err)
            *err = SM_PAIR_ERR_INVALID_PARAM;
        return SM_ERR_INVALID_PARAMS;
    }
    inst->peerIoCap = tmp;

    tmp = utilGetLE8(&pairReqResp->oobDataFlag);
    if (tmp != 0 && tmp != 1) {
        logd("Invalid oob flag RXed 0x%02X\n", tmp);
        if (err)
            *err = SM_PAIR_ERR_INVALID_PARAM;
        return SM_ERR_INVALID_PARAMS;
    }
    inst->peerHasOob = !!tmp;

    tmp = utilGetLE8(&pairReqResp->authReq);
    if ((tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_NO &&
         (tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_BOND) {
        logd("Invalid authReq RXed 0x%02X\n", tmp);
        if (err)
            *err = SM_PAIR_ERR_INVALID_PARAM;
        return SM_ERR_INVALID_PARAMS;
    }
    inst->peerAuthReq = tmp;

    if ((inst->peerAuthReq & SM_AUTH_REQ_MITM_FLAG) && inst->peerIoCap == HCI_DISP_CAP_NONE) {
        logd("Infeasible authReq RXed 0x%02X due to io caps 0x%02X\n", tmp, inst->peerIoCap);
        if (err)
            *err = SM_PAIR_ERR_AUTH_REQ_INFEASIBLE;
        return SM_ERR_AUTH_REQMENTS;
    }

    tmp = utilGetLE8(&pairReqResp->maxKeySz);
    if (tmp < SM_MIN_KEY_LEN || tmp > SM_MAX_KEY_LEN) {
        logd("Invalid maxKeySize RXed 0x%02X\n", tmp);
        if (err)
            *err = SM_PAIR_ERR_ENCR_KEY_SIZE;
        /* the invalid key size in pairing request will be handled when receiving confirm value */
        if (!isReq)
            return SM_ERR_ENCR_KEY_SZ;
    }
    inst->peerMaxKeySz = tmp;

    initKeyDistr = utilGetLE8(&pairReqResp->initiatorKeyDistr);
    rspKeyDistr  = utilGetLE8(&pairReqResp->responderKeyDistr);

    if (inst->isInitiator) { /* must be a response. accept as many as given */
        /* TODO(mcchou): do not send any keys we did not already commit to send inst->myKeyDistr &= initKeyDistr; */
        inst->myKeyDistr = initKeyDistr;
        inst->peerKeyDistr = rspKeyDistr;
    } else { /* must be a request - agree to send all keys they want, accept whatever was given */
        inst->myKeyDistr = rspKeyDistr;
        inst->peerKeyDistr = initKeyDistr;
    }

    return 0;
}

/*
 * FUNCTION: smReadReversedBytes
 * USE:      Read the bytes stream of LSB first order and store the bytes stream to out in MSB
 *           first order
 * PARAMS:   out - the output byte array
 *           outLen - the length of out
 *           in - the input byte array
 *           InLen - the length of in
 * RETURN:   success
 * NOTES:    Used for reading fields of SM commands or L2CAP event
 */
static bool smReadReversedBytes(uint8_t *out, int outLen, const uint8_t *in, int inLen)
{
    int i;

    if (outLen <= 0 || inLen <= 0 || outLen != inLen)
        return false;

    for (i = 0; i < inLen; ++i)
        out[i] = utilGetLE8(&in[inLen - i - 1]);
    return true;
}

/*
 * FUNCTION: smWriteReversedBytes
 * USE:      Read the bytes stream of MSB first order and store the bytes stream to out in LSB
 *           first order
 * PARAMS:   out - the output byte array
 *           outLen - the length of out
 *           in - the input byte array
 *           InLen - the length of in
 * RETURN:   success
 * NOTES:    Used for writing fields of SM commands
 */
static bool smWriteReversedBytes(uint8_t *out, int outLen, const uint8_t *in, int inLen)
{
    int i;

    if (outLen <= 0 || inLen <= 0 || outLen != inLen)
        return false;

    for (i = 0; i < inLen; ++i)
        utilSetLE8(&out[i], in[inLen - i - 1]);
    return true;
}

/*
 * FUNCTION: smSendSecurityReq
 * USE:      Send security request
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendSecurityReq(struct smInstance *inst)
{
    struct smAuthReq req;

    inst->isInitiator = false;
    utilSetLE8(&req.authReq, inst->myAuthReq);
    if (!smTx(inst, SM_SECURITY_REQUEST, &req, sizeof(req))) {
        logw("%s(): Failed to send security request command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendPairReq
 * USE:      Send pairing request
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairReq(struct smInstance *inst)
{
    struct smPairReqResp req;

    if (!inst->isInitiator)
        return false;

    utilSetLE8(&req.ioCap, mIoCap);
    /* TODO(mcchou): this needs to be changed accordingly for OOB support */
    utilSetLE8(&req.oobDataFlag, mHasOob);
    utilSetLE8(&req.authReq, inst->myAuthReq);
    utilSetLE8(&req.maxKeySz, inst->myMaxKeySz);
    utilSetLE8(&req.initiatorKeyDistr, inst->myKeyDistr);
    utilSetLE8(&req.responderKeyDistr, inst->myKeyDistr);

    if (!smTx(inst, SM_PAIRING_REQ, &req, sizeof(req))) {
        logw("%s(): Failed to send pairing request command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendPairResp
 * USE:      Send pairing response
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairResp(const struct smInstance *inst)
{
    struct smPairReqResp resp;

    if (inst->isInitiator) {
        logw("%s(): Unexpected to send pairing response\n", __func__);
        return false;
    }

    utilSetLE8(&resp.ioCap, mIoCap);
    /* TODO(mcchou): this needs to be changed accordingly for OOB support */
    utilSetLE8(&resp.oobDataFlag, mHasOob);
    utilSetLE8(&resp.authReq, inst->myAuthReq);
    utilSetLE8(&resp.maxKeySz, inst->myMaxKeySz);
    utilSetLE8(&resp.initiatorKeyDistr, inst->peerKeyDistr); /* accept whatever they promised us */
    utilSetLE8(&resp.responderKeyDistr, inst->myKeyDistr); /* give up whatever they asked for */

    if (!smTx(inst, SM_PAIRING_RSP, &resp, sizeof(resp))) {
        logw("%s(): Failed to send pairing response command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendPairConf
 * USE:      Send pairing confirm value
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairConf(const struct smInstance *inst)
{
    uint16_t i, confSz;
    struct smPairConf conf;

    smWriteReversedBytes(conf.confirmVal, sizeof(conf.confirmVal), inst->myConfVal.confirmVal,
                         sizeof(inst->myConfVal.confirmVal));

    if (!smTx(inst, SM_PAIRING_CONF, &conf, sizeof(conf))) {
        logw("%s(): Failed to send pairing confirm command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendPairRand
 * USE:      Send pairing Random number
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairRand(const struct smInstance *inst)
{
    uint16_t i, randSz;
    struct smPairRand rand;

    smWriteReversedBytes(rand.rand, sizeof(rand.rand), inst->myRandNum.rand,
                         sizeof(inst->myRandNum.rand));

    if (!smTx(inst, SM_PAIRING_RAND, &rand, sizeof(rand))) {
        logw("%s(): Failed to send pairing random command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendEncryptInfo
 * USE:      Send LTK via Encryption Information
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendEncryptInfo(const struct smInstance *inst)
{
    uint16_t i, keySz;
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    struct smKey ltk;

    if (!persistGetDevKey(&inst->peerAddr, KEY_TYPE_MY_LTK, key))
        return false;

    smWriteReversedBytes(ltk.key, sizeof(ltk.key), key, sizeof(key));

    if (!smTx(inst, SM_ENCR_INFO, &ltk, sizeof(ltk))) {
        logw("%s(): Failed to send encryption information command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendMasterId
 * USE:      Send EDIV and random number
 * PARAMS:   inst - the instance
 *           ediv - our EDIV associated with the peer device
 *           rand - our random number associated with the peer device
 * RETURN:   success
 * NOTES:
 */
static bool smSendMasterId(const struct smInstance *inst, uint16_t ediv, uint64_t rand)
{
    struct smMasterInfo info;

    utilSetLE16(&info.ediv, ediv);
    utilSetLE64(&info.rand, rand);
    if (!smTx(inst, SM_MASTER_INFO, &info, sizeof(info))) {
        logw("%s(): Failed to send master identification command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendIdInfo
 * USE:      Send IRK
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendIdInfo(const struct smInstance *inst)
{
    uint16_t i, keySz;
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    struct smKey irk;

    if (!persistGetDevKey(NULL, KEY_TYPE_IRK, key))
        return false;

    smWriteReversedBytes(irk.key, sizeof(irk.key), key, sizeof(key));

    if (!smTx(inst, SM_IDENTITY_INFO, &irk, sizeof(irk))) {
        logw("%s(): Failed to send identity information command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendIdAddrInfo
 * USE:      Send public device address or static random address
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendIdAddrInfo(const struct smInstance *inst)
{
    uint16_t i, addrSz;
    struct smIdentityAddrInfo addr;

    hciGetLocalAddress(addr.mac);
    utilSetLE8(&addr.addrType, 0x00); /* 0x00: public address, 0x01: static random address */

    if (!smTx(inst, SM_IDENTITY_ADDR_INFO, &addr, sizeof(addr))) {
        logw("%s(): Failed to send identity address information command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendSignInfo
 * USE:      Send CSRK
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:
 */
static bool smSendSignInfo(const struct smInstance *inst)
{
    uint16_t i, keySz;
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    struct smKey csrk;

    if (!persistGetDevKey(NULL, KEY_TYPE_CSRK, key))
        return false;

    smWriteReversedBytes(csrk.key, sizeof(csrk.key), key, sizeof(key));

    if (!smTx(inst, SM_SIGNING_INFO, &csrk, sizeof(csrk))) {
        logw("%s(): Failed to send signing information command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smTransitPhase
 * USE:      Log and transit the phase of pairing process
 * PARAMS:   inst - the instance
 *           phase - the phase to transit to
 * RETURN:   NONE
 * NOTES:
 */
static void smTransitPhase(struct smInstance *inst, uint8_t phase) {
    if (phase < SM_PHASE_START || phase > SM_PHASE_LTK_ENCRYPTED) {
        logd("Invalid phase to transit, phase %d\n", phase);
        return;
    }
    if (inst->phase == phase) {
        logd("Phase transition skipped, phase %d\n", phase);
        return;
    }
    if ((phase == SM_PHASE_DONE || phase == SM_PHASE_LTK_ENCRYPTED)
        && inst->stallTimer) {
        timerCancel(inst->stallTimer);
        inst->stallTimer = 0;
    }

    logd("Transit from phase %d to phase %d\n", inst->phase, phase);
    inst->phase = phase;

    if (inst->phase == SM_PHASE_DONE)
        smNotifyPairObserver(&inst->peerAddr, SM_PAIR_STATE_PAIRED, SM_PAIR_ERR_NONE);
}

/*
 * FUNCTION: smDistributeKeys
 * USE:      Distribute our LTK, EDIV, Rand, IRK, Addr, CSRK based on the key distribution field
 * PARAMS:   inst - the instance
 * RETURN:   success
 * NOTES:    the phase will be transited based on the key distribution field
 */
static bool smDistributeKeys(struct smInstance *inst)
{
    uint16_t ediv;
    uint64_t rand;

    if (inst->myKeyDistr & SM_KEY_DISTR_LTK) {
        if (!smGenLtk(inst) || !smSendEncryptInfo(inst))
            goto fail;
        smTransitPhase(inst, SM_PHASE_ENCRYPT_INFO_SENT);

        if (!smGenRandNum((uint8_t *)&rand, sizeof(rand)) ||
            !persistAddDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_MY_RANDOM, rand) ||
            !smGenEdiv(&ediv, inst, rand, false))
            goto fail;

        if (!smSendMasterId(inst, ediv, rand))
            goto fail;
        smTransitPhase(inst, SM_PHASE_MASTER_ID_SENT);
    }

    if (inst->myKeyDistr & SM_KEY_DISTR_IRK) {
        if (!smSendIdInfo(inst))
            goto fail;
        smTransitPhase(inst, SM_PHASE_ID_INFO_SENT);

        if (!smSendIdAddrInfo(inst))
            goto fail;
        smTransitPhase(inst, SM_PHASE_ID_ADDR_INFO_SENT);
    }

    if (inst->myKeyDistr & SM_KEY_DISTR_CSRK) {
        if (!smSendSignInfo(inst))
            goto fail;
        smTransitPhase(inst, SM_PHASE_SIGN_INFO_SENT);
    }

    return true;
fail:
    logw("Failed to distribute keys when we are the %s\n",
         inst->isInitiator ? "initiator" : "responder");
    return false;
}

/*
 * FUNCTION: smSendPairFail
 * USE:      Send a pairing error to the peer
 * PARAMS:   inst - the instance
 *           reason - the error reason
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairFail(const struct smInstance *inst, uint8_t reason)
{
    struct smPairFailed fail;
    smPairState pairState = SM_PAIR_ERR_UNKNOWN;

    /* delete keys and numbers associated with the peer device */
    persistDelDevKeys(&inst->peerAddr);
    persistDelDevNumbers(&inst->peerAddr);

    smBanDeviceForTime(&inst->peerAddr);
    utilSetLE8(&fail.reason, reason);

    return smTx(inst, SM_PAIRING_FAIL, &fail, sizeof(fail));
}

/*
 * FUNCTION: smIsPaired
 * USE:      check if the peer device is previously paired by checking the exchanged
 *           LTK, EDIV and random number
 * PARAMS:   mitm - indicate whether the existing pairing is MITM protected; valid if paired
 *           bonded - indicate whether the existing pairing is bonded; valid if paired
 *           addr - the address of the peer device
 * RETURN:   true if previously paired; false otherwise
 * NOTES:
 */
static bool smIsPaired(bool *mitm, bool *bonded, const struct bt_addr *addr)
{
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    uint64_t num;
    bool paired;

    if (!addr)
        return false;

    paired = persistGetDevKey(addr, KEY_TYPE_LTK, key) &&
             persistGetDevKey(addr, KEY_TYPE_MY_LTK, key) &&
             persistGetDevNumber(addr, PERSIST_NUM_TYPE_SM_EDIV, &num) &&
             persistGetDevNumber(addr, PERSIST_NUM_TYPE_SM_RANDOM, &num) &&
             persistGetDevNumber(addr, PERSIST_NUM_TYPE_SM_MY_RANDOM, &num) &&
             persistGetDevNumber(addr, PERSIST_NUM_TYPE_SM_MITM_PROTECT, &num);
    if (!paired)
        return false;

    if (mitm)
        *mitm = !!num;

    if (bonded) {
        *bonded = false;
        if (persistGetDevNumber(addr, PERSIST_NUM_TYPE_SM_BOND, &num) && !!num &&
            persistGetDevKey(addr, KEY_TYPE_IRK, key)) {
            *bonded = true;
        }
    }

    return true;
}

/*
 * FUNCTION: smLtkEncrypt
 * USE:      (Re-)encrypt the link using peer's random number, EVID and LTK
 * PARAMS:   inst - the instance
 * RETURN:   true if encryption request sent
 * NOTES:    This should be called when we are the initiator of the link
 */
static bool smLtkEncrypt(struct smInstance *inst)
{
    uint64_t rand, ediv;
    uint8_t ltk[HCI_LE_KEY_LEN] = {0,};
    uint8_t rawLtk[HCI_LE_KEY_LEN] = {0,};

    if (!inst)
        return false;

    if (!persistGetDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_EDIV, &ediv) ||
        !persistGetDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_RANDOM, &rand) ||
        !persistGetDevKey(&inst->peerAddr, KEY_TYPE_LTK, ltk)) {
        logw("Failed to retrieve encryption info for encryption using LTK\n");
        return false;
    } else if (!smReadReversedBytes(rawLtk, sizeof(rawLtk), ltk, sizeof(ltk)) ||
               !l2cApiLeEncryptConn(inst->conn, rand, ediv, rawLtk)) {
        logw("Failed to encrypt using LTK\n");
        return false;
    } else {
        logi("Encrypt using LTK\n");
    }
    return true;
}

/*
 * FUNCTION: smRx
 * USE:      Data arrived - handle it
 * PARAMS:   inst - the instance
 *           packet - the data that arrived
 * RETURN:   NONE
 * NOTES:
 */
static void smRx(struct smInstance *inst, sg packet)
{
    struct smHdr hdr;
    struct smPairReqResp pairReqResp;
    struct smPairConf pairConf;
    struct smPairRand pairRand;
    struct smPairFailed pairFailed;
    struct smKey k, key;
    struct smMasterInfo masterInfo;
    struct smIdentityAddrInfo addrInfo;
    struct smAuthReq authReq;
    smPairErr pairErr = SM_PAIR_ERR_UNKNOWN;
    uint8_t typ, addrType, err, auth;
    uint64_t rand, ediv;
    uint8_t mac[BT_MAC_LEN] = {0,};
    uint8_t stk[HCI_LE_KEY_LEN] = {0,};
    uint8_t ltk[HCI_LE_KEY_LEN] = {0,};

    if (!sgSerializeCutFront(packet, &hdr, sizeof(hdr))) {
        logw("Incoming SM packet header extraction failed\n");
        sgFree(packet);
        return;
    }

    typ = utilGetLE8(&hdr.typ);
    switch (typ) {
    case SM_PAIRING_REQ:
        if (!sgSerializeCutFront(packet, &pairReqResp, sizeof(pairReqResp))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        if (smIsDevBanned(&inst->peerAddr)) {
            smSendPairFail(inst, SM_ERR_REPEATED_ATTEMPTS);
            smBanDeviceForTime(&inst->peerAddr);
            pairErr = SM_PAIR_ERR_REPEATED_ATTEMPT;
            break;
        }

        if ((inst->phase == SM_PHASE_SEC_REQ_SENT && inst->isInitiator) ||
            (inst->phase != SM_PHASE_START) || inst->isInitiator) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        inst->isInitiator = false;
        if ((pairErr = smParsePairReqResp(&err, inst, &pairReqResp, true))) {
            smSendPairFail(inst, err);
            break;
        }

        if (inst->peerHasOob && !mHasOob) {
            smSendPairFail(inst, SM_ERR_OOB_NOT_AVAIL);
            pairErr = SM_PAIR_ERR_OOB_NOT_AVAILABLE;
            break;
        }

        /* as a responsder, we set our authentication requirement based on our IO capabilities and
         * peer's requirement */
        inst->myAuthReq = inst->peerAuthReq;
        if (mIoCap == HCI_DISP_CAP_NONE)
            inst->myAuthReq &= ~SM_AUTH_REQ_MITM_FLAG;
        inst->myMaxKeySz = SM_MAX_KEY_LEN;

        if (!smGenRandNum(inst->myRandNum.rand, sizeof(inst->myRandNum.rand)) ||
            !smGenTK(inst) ||
            !smGenConfVal(&inst->myConfVal, inst, false))
            break;

        if (!smSendPairResp(inst)) {
            pairErr = SM_PAIR_ERR_SEND_SM_CMD;
            break;
        }
        smTransitPhase(inst, SM_PHASE_REQ_RESP);

        goto done;

    case SM_PAIRING_RSP:
        if (!sgSerializeCutFront(packet, &pairReqResp, sizeof(pairReqResp))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (inst->phase != SM_PHASE_REQ_SENT || !inst->isInitiator) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        if ((pairErr = smParsePairReqResp(&err, inst, &pairReqResp, false))) {
            smSendPairFail(inst, err);
            break;
        }

        /* fail the pairing if peer does not distribute IRK when bonding is requested by us */
        if ((inst->myAuthReq & SM_AUTH_REQ_BOND_MASK) == SM_AUTH_REQ_BOND_BOND &&
             !(inst->peerKeyDistr & SM_KEY_DISTR_IRK)) {
            smSendPairFail(inst, SM_ERR_AUTH_REQMENTS);
            pairErr = SM_PAIR_ERR_AUTH_REQ_INFEASIBLE;
            break;
        }

        if (!smGenRandNum(inst->myRandNum.rand, sizeof(inst->myRandNum.rand)) ||
            !smGenTK(inst) ||
            !smGenConfVal(&inst->myConfVal, inst, false))
            break;

        if (!smSendPairConf(inst)) {
            pairErr = SM_PAIR_ERR_SEND_SM_CMD;
            break;
        }
        smTransitPhase(inst, SM_PHASE_CONF_SENT);

        goto done;

    case SM_PAIRING_CONF:
        if (!sgSerializeCutFront(packet, &pairConf, sizeof(pairConf))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if ((inst->isInitiator && inst->phase != SM_PHASE_CONF_SENT) ||
            (!inst->isInitiator && inst->phase != SM_PHASE_REQ_RESP)) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        if (!inst->isInitiator &&
            (inst->peerMaxKeySz < SM_MIN_KEY_LEN || inst->peerMaxKeySz > SM_MAX_KEY_LEN)) {
            smSendPairFail(inst, SM_ERR_ENCR_KEY_SZ);
            pairErr = SM_PAIR_ERR_ENCR_KEY_SIZE;
            break;
        }

        if (!smReadReversedBytes(inst->peerConfVal.confirmVal, sizeof(inst->peerConfVal.confirmVal),
                                 pairConf.confirmVal, sizeof(pairConf.confirmVal)))
            break;

        if (inst->isInitiator) {
            if (!smSendPairRand(inst)) {
                pairErr = SM_PAIR_ERR_SEND_SM_CMD;
                break;
            }
            smTransitPhase(inst, SM_PHASE_RAND_SENT);
        } else {
            if (!smSendPairConf(inst)) {
                pairErr = SM_PAIR_ERR_SEND_SM_CMD;
                break;
            }
            smTransitPhase(inst, SM_PHASE_CONF_RESP);
        }

        goto done;

    case SM_PAIRING_RAND:
        if (!sgSerializeCutFront(packet, &pairRand, sizeof(pairRand))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if ((inst->isInitiator && inst->phase != SM_PHASE_RAND_SENT) ||
            (!inst->isInitiator && inst->phase != SM_PHASE_CONF_RESP))
            break;

        if (!smReadReversedBytes(inst->peerRandNum.rand, sizeof(inst->peerRandNum.rand),
                                 pairRand.rand, sizeof(pairRand.rand)))
            break;

        /* check if the peer confirm value generated from the random value matches */
        smGenConfVal(&pairConf, inst, true);
        if (memcmp(pairConf.confirmVal, inst->peerConfVal.confirmVal,
                   sizeof(pairConf.confirmVal)) != 0) {
            smSendPairFail(inst, SM_ERR_CONF_VAL_FAILED);
            pairErr = SM_PAIR_ERR_CONF_VALUE_MISMATCHED;
            break;
        }
        if (!inst->isInitiator && !smSendPairRand(inst)) {
            pairErr = SM_PAIR_ERR_SEND_SM_CMD;
            break;
        }
        smTransitPhase(inst, SM_PHASE_RAND_RESP);

        if (!smGenSTK(inst)) {
            if ((inst->peerAuthReq & SM_AUTH_REQ_MITM_FLAG) && inst->alg == SM_ALG_JUST_WORK) {
                logw("%s(): Chosen pairing algorithm 0x%02X failed to meet MITM\n", __func__,
                     inst->alg);
                smSendPairFail(inst, SM_ERR_AUTH_REQMENTS);
                pairErr = SM_PAIR_ERR_AUTH_REQ_INFEASIBLE;
            }
            break;
        }

        /* Rand value and EDIV should be set to 0 when setting up encryption with STK */
        if (inst->isInitiator) {
            if (!smReadReversedBytes(stk, sizeof(stk), inst->stk.key, sizeof(inst->stk.key))) {
                break;
            } else if (!l2cApiLeEncryptConn(inst->conn, 0, 0, stk)) {
                pairErr = SM_PAIR_ERR_L2C_CONN;
                break;
            } else {
                smTransitPhase(inst, SM_PHASE_STK_SENT);
            }
        }

        goto done;

    case SM_PAIRING_FAIL:
        if (!sgSerializeCutFront(packet, &pairFailed, sizeof(pairFailed))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        switch(pairFailed.reason) {
        case SM_ERR_PASSKEY_ENTRY_FAILED:
            pairErr = SM_PAIR_ERR_PASSKEY_FAILED;
            break;
        case SM_ERR_OOB_NOT_AVAIL:
            pairErr = SM_PAIR_ERR_OOB_NOT_AVAILABLE;
            break;
        case SM_ERR_AUTH_REQMENTS:
            pairErr = SM_PAIR_ERR_AUTH_REQ_INFEASIBLE;
            break;
        case SM_ERR_CONF_VAL_FAILED:
            pairErr = SM_PAIR_ERR_CONF_VALUE_MISMATCHED;
            break;
        case SM_ERR_PAIRING_NOT_SUPPORTED:
            pairErr = SM_PAIR_ERR_PAIRING_NOT_SUPPORTED;
            break;
        case SM_ERR_ENCR_KEY_SZ:
            pairErr = SM_PAIR_ERR_ENCR_KEY_SIZE;
            break;
        case SM_ERR_CMD_NOT_SUPP:
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        case SM_ERR_REPEATED_ATTEMPTS:
            pairErr = SM_PAIR_ERR_REPEATED_ATTEMPT;
            break;
        case SM_ERR_INVALID_PARAMS:
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        case SM_ERR_UNSPECCED_REASON:
        default:
            break;
        }

        smBanDeviceForTime(&inst->peerAddr);
        break;

    case SM_ENCR_INFO:
        if (!sgSerializeCutFront(packet, &k, sizeof(k))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        if (inst->isInitiator) {
            if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        } else {
            /* the phase check can differ based on the responder key distribution field */
            if (inst->myKeyDistr & SM_KEY_DISTR_CSRK) {
                if (inst->phase != SM_PHASE_SIGN_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_IRK) {
                if (inst->phase != SM_PHASE_ID_ADDR_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        }

        /* process and store peer's LTK */
        if (!smReadReversedBytes(key.key, sizeof(key.key), k.key, sizeof(k.key)))
            break;
        if (!persistAddDevKey(&inst->peerAddr, KEY_TYPE_LTK, key.key))
            break;
        smTransitPhase(inst, SM_PHASE_ENCRYPT_INFO_RECEIVED);

        goto done;

    case SM_MASTER_INFO:
        if (!sgSerializeCutFront(packet, &masterInfo, sizeof(masterInfo))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        if (inst->phase != SM_PHASE_ENCRYPT_INFO_RECEIVED) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        /* process and store peer's EDIV and random number */
        ediv = utilGetLE16(&masterInfo.ediv);
        rand = utilGetLE64(&masterInfo.rand);
        if (!persistAddDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_EDIV, ediv) ||
            !persistAddDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_RANDOM, rand)) {
            break;
        }
        smTransitPhase(inst, SM_PHASE_MASTER_ID_RECEIVED);

        if (!(inst->peerKeyDistr & (SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK))) {
            /* as the initiator, if we don't expect more keys, we start key distribution. according
             * to the spec, a device "may" re-encrypt the link using LTK, but we don't until the
             * next re-connection.
             */
            if (inst->isInitiator && !smDistributeKeys(inst)) {
                pairErr = SM_PAIR_ERR_SEND_SM_CMD;
                break;
            }

            smTransitPhase(inst, SM_PHASE_DONE);
        }

        goto done;

    case SM_IDENTITY_INFO:
        if (!sgSerializeCutFront(packet, &k, sizeof(k))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        /* the phase check can differ based on the key distribution fields */
        if (inst->isInitiator) {
            if (inst->peerKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        } else {
            if (inst->peerKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_CSRK) {
                if (inst->phase != SM_PHASE_SIGN_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_IRK) {
                if (inst->phase != SM_PHASE_ID_ADDR_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        }

        /* process and store peer's IRK */
        if (!smReadReversedBytes(key.key, sizeof(key.key), k.key, sizeof(k.key)))
            break;
        if (!persistAddDevKey(&inst->peerAddr, KEY_TYPE_IRK, key.key))
            break;
        smTransitPhase(inst, SM_PHASE_ID_INFO_RECEIVED);

        goto done;

    case SM_IDENTITY_ADDR_INFO:
        if (!sgSerializeCutFront(packet, &addrInfo, sizeof(addrInfo))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        if (inst->phase != SM_PHASE_ID_INFO_RECEIVED) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        /* TODO(mcchou): store the address as a property in persist, note that according to spec,
         * the address is considered valid after reconnecting using previously-exchanged LTK */
        smTransitPhase(inst, SM_PHASE_ID_ADDR_INFO_RECEIVED);

        if (!(inst->peerKeyDistr & SM_KEY_DISTR_CSRK)) {
            /* as the initiator, if we don't expect more keys, we start key distribution. according
             * to the spec, a device "may" re-encrypt the link using LTK, but we don't until the
             * next re-connection.
             */
            if (inst->isInitiator && !smDistributeKeys(inst)) {
                pairErr = SM_PAIR_ERR_SEND_SM_CMD;
                break;
            }

            smTransitPhase(inst, SM_PHASE_DONE);
        }

        goto done;
    case SM_SIGNING_INFO:
        if (!sgSerializeCutFront(packet, &k, sizeof(k))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        /* the phase check can differ based on the key distribution fields */
        if (inst->isInitiator) {
            if (inst->peerKeyDistr & SM_KEY_DISTR_IRK) {
                if (inst->phase != SM_PHASE_ID_ADDR_INFO_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->peerKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        } else {
            if (inst->peerKeyDistr & SM_KEY_DISTR_IRK) {
                if (inst->phase != SM_PHASE_ID_ADDR_INFO_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->peerKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_RECEIVED) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_CSRK) {
                if (inst->phase != SM_PHASE_SIGN_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_IRK) {
                if (inst->phase != SM_PHASE_ID_ADDR_INFO_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->myKeyDistr & SM_KEY_DISTR_LTK) {
                if (inst->phase != SM_PHASE_MASTER_ID_SENT) {
                    pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                    break;
                }
            } else if (inst->phase != SM_PHASE_STK_ENCRYPTED) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
                break;
            }
        }

        /* process and store peer's CSRK */
        if (!smReadReversedBytes(key.key, sizeof(key.key), k.key, sizeof(k.key)))
            break;
        if (!persistAddDevKey(&inst->peerAddr, KEY_TYPE_CSRK, key.key))
            break;
        smTransitPhase(inst, SM_PHASE_SIGN_INFO_RECEIVED);

        /* as the initiator, if we start key distribution. according to the spec, a device "may"
         * re-encrypt the link using LTK, but we don't until the next re-connection.
         */
        if (inst->isInitiator && !smDistributeKeys(inst)) {
            pairErr = SM_PAIR_ERR_SEND_SM_CMD;
            break;
        }

        smTransitPhase(inst, SM_PHASE_DONE);

        goto done;

    case SM_SECURITY_REQUEST:
        if (!sgSerializeCutFront(packet, &authReq, sizeof(authReq))) {
            logw("Incoming SM packet data extraction failed\n");
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            pairErr = SM_PAIR_ERR_INVALID_PARAM;
            break;
        }

        if (smIsDevBanned(&inst->peerAddr)) {
            smSendPairFail(inst, SM_ERR_REPEATED_ATTEMPTS);
            smBanDeviceForTime(&inst->peerAddr);
            pairErr = SM_PAIR_ERR_REPEATED_ATTEMPT;
            break;
        }

        if ((inst->phase == SM_PHASE_DONE || inst->phase == SM_PHASE_LTK_ENCRYPTED) && inst->isInitiator) {
            inst->isInitiator = true; /* for re-establishment, we are the initiator for sure */
            if (!smLtkEncrypt(inst)) {
                pairErr = SM_PAIR_ERR_ENCR_CONN;
                break;
            }
        } else if (inst->phase == SM_PHASE_START) {
            auth = utilGetLE8(&authReq.authReq);
            if ((auth & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_NO &&
                (auth & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_BOND) {
                logw("Invalid authReq in security request\n");
                pairErr = SM_PAIR_ERR_INVALID_PARAM;
                break;
            }
            inst->peerAuthReq = auth; /* TODO(mcchou): use whether received from the responder */
            smSendPairReq(inst); /* all fields required were set when the instance was allocated */
        } else {
            pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
            break;
        }

        goto done;

    default:
        logw("Unexpected packet type 0x%02X in SM. Dropping\n", typ);
        smSendPairFail(inst, SM_ERR_CMD_NOT_SUPP);
        pairErr = SM_PAIR_ERR_UNEXPECTED_SM_CMD;
        break;
    }

    logd("Got packet type 0x%02X in phase %u when we are the %s\n", typ, inst->phase,
         inst->isInitiator ? "initiator" : "responder");
    smNotifyPairObserver(&inst->peerAddr, SM_PAIR_STATE_FAILED, pairErr);
    smTransitPhase(inst, SM_PHASE_START);
done:
    sgFree(packet);
    return;
}

/*
 * FUNCTION: smInstFindByConn
 * USE:      Find the instance node by the connection ID
 * PARAMS:   conn - the connection ID
 * RETURN:   the instance if matched; NULL otherwise
 * NOTES:    call with mInstLock held
 */
static struct smInstance* smInstFindByConn(l2c_handle_t conn)
{
    struct smInstance *inst = mActInstList;

    if (!conn)
        return NULL;

    while(inst) {
        if (inst->conn == conn)
            return inst;
        inst = inst->next;
    }
    return NULL;
}

/*
 * FUNCTION: smInstFindByAddr
 * USE:      Find the instance node by the peer address
 * PARAMS:   addr
 * RETURN:   the instance if matched; NULL otherwise
 * NOTES:    call with mInstLock held
 */
static struct smInstance* smInstFindByAddr(const struct bt_addr *addr)
{
    struct smInstance *inst = mActInstList;

    if (!addr)
        return NULL;

    while(inst) {
        if (smSameAddr(addr, &inst->peerAddr))
            return inst;
        inst = inst->next;
    }
    return NULL;
}

/*
 * FUNCTION: smStallTimerCb
 * USE:      Called when the stallTimer of an instance is fired to terminate a stall pairing session
 * PARAMS:   timerId - an unique ID of a timer
 *           conn - a connection handle
 * RETURN:   NONE
 * NOTES:
 */
static void smStallTimerCb(uniq_t timerId, uint64_t conn)
{
    struct smInstance *inst;

    pthread_mutex_lock(&mInstLock);

    inst = smInstFindByConn(conn);
    if (!inst) {
        pthread_mutex_unlock(&mInstLock);
        return;
    }

    inst->stallTimer = 0;
    logd("%s(): Pairing with device "MACFMT" is stalled\n", __func__,
         MACCONV(inst->peerAddr.addr));

    smBanDeviceForTime(&inst->peerAddr);

    pthread_mutex_unlock(&mInstLock);

    l2cApiDisconnect(conn);
    smNotifyPairObserver(&inst->peerAddr, SM_PAIR_STATE_FAILED, SM_PAIR_ERR_STALLED);
}

/*
 * FUNCTION: smInstDeinit
 * USE:      Delete an instance from the active instance list, cancel the pairing timer and free it
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:    This MUST be called only by smFixedChState when the state is L2C_STATE_CLOSED to avoid
 *           potential race and freeing already-freed instance
 */
static void smInstDeinit(struct smInstance* inst)
{
    pthread_mutex_lock(&mInstLock);
    if (!inst)
        return;

    if (inst->next)
        inst->next->prev = inst->prev;
    if (inst->prev)
        inst->prev->next = inst->next;
    else
        mActInstList = inst->next;

    if (inst->stallTimer)
        timerCancel(inst->stallTimer);

    free(inst);

    pthread_mutex_unlock(&mInstLock);
}

/*
 * FUNCTION: smInstInit
 * USE:      Set default values for repeated attempt fields of the instance, add it to the
 *           active instance list and keep start the timer for pairing process if an instance is
 *           not yet in the active instance list
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
 */
static void smInstInit(struct smInstance *inst)
{
    if (!inst)
        return;

    pthread_mutex_lock(&mInstLock);

    if (!smInstFindByAddr(&inst->peerAddr)) {
        inst->prev = NULL;
        inst->next = mActInstList;
        if (mActInstList)
            mActInstList->prev = inst;
        mActInstList = inst;
    }

    /* TODO(mcchou): fix stall timer according to Volume 3, Part H, 3.4 SMP TIMEOUT */
    inst->stallTimer = timerSet(SM_STALL_INTVL, smStallTimerCb, inst->conn);

    pthread_mutex_unlock(&mInstLock);
}

/*
 * FUNCTION: smFixedChState
 * USE:      Connection event happened
 * PARAMS:   userData - unused
 *           instance - per-connection instance allocated by smFixedChAlloc()
 *           evt - what happened
 *           data - the pertinent data
 *           len - length of said data
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static void smFixedChState(void *userData, void *instance, uint8_t evt, const void *data,
                           uint32_t len)
{
    struct smInstance *inst = (struct smInstance*)instance;
    struct l2cEncrState *encrState = NULL;
    struct l2cKeyReqState *keyReqState = NULL;
    struct bt_addr peerAddr;
    struct bt_addr myAddr;
    smPairErr pairErr = SM_PAIR_ERR_UNKNOWN;;
    l2c_handle_t conn;
    sg s;
    uint16_t ediv = 0, myEdiv = 0;
    uint64_t myRand = 0;
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    uint8_t rawKey[HCI_LE_KEY_LEN] = {0,};
    uint8_t phase = inst->phase;

    switch (evt) {
    case L2C_STATE_OPEN:
        if (len != sizeof(l2c_handle_t)) {
            loge("invalid length for open event\n");
            goto fail;
        }

        conn = *(l2c_handle_t*)data;

        /* when we support outbound connections, this code will need to evolve */
        if (!inst->isInitiator && inst->conn != conn) {
            loge("Handle change!!!\n");
            pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
            goto reset;
        }

        /* assign the connection, our address and peer address if smPair was called to start the
         * pairing as an initiator
         */
        if (inst->isInitiator) {
            if (inst->phase != SM_PHASE_START) {
                pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
                goto reset;
            }

            inst->conn = conn;

            if (!l2cApiGetBtAddr(inst->conn, &peerAddr) ||
                !l2cApiGetSelfBtAddr(inst->conn, &myAddr)) {
                loge("Failed to get peer address or local address\n");
                goto reset;
            }
            if (memcmp(&inst->peerAddr, &peerAddr, sizeof(struct bt_addr))) {
                loge("Peer address mismatched\n");
                goto reset;
            }
            inst->myAddr = myAddr;
        }

        smInstInit(inst);

        if (inst->isInitiator) {
            if (!smSendPairReq(inst)) {
                pairErr = SM_PAIR_ERR_SEND_SM_CMD;
                goto reset;
            }
            smTransitPhase(inst, SM_PHASE_REQ_SENT);
        }

        break;

    case L2C_STATE_ENCR:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encryption event\n");
            goto fail;
        }

        encrState = (struct l2cEncrState *)data;

        if (inst->phase != SM_PHASE_LTK_SENT && inst->phase != SM_PHASE_STK_SENT) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
            goto reset;
        }

        if (!smIsEncryptExpected(encrState, inst)) {
            logw("Failed to encrypt the connection %llu\n", inst->conn);
            pairErr = SM_PAIR_ERR_ENCR_CONN;
            goto reset;
        }

        /* if the encryption of re-establishment went well, no further action is needed for either
         * initiator or responder
         */
        if (inst->phase == SM_PHASE_LTK_SENT) {
            smTransitPhase(inst, SM_PHASE_LTK_ENCRYPTED);
            break;
        }

        if (inst->phase == SM_PHASE_STK_SENT)
            smTransitPhase(inst, SM_PHASE_STK_ENCRYPTED);

        /* if we are the initiator, we wait for key distribution from the peer if any */
        if (inst->isInitiator &&
            (inst->peerKeyDistr & (SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK)))
            break;

        if (!smDistributeKeys(inst)) {
            pairErr = SM_PAIR_ERR_SEND_SM_CMD;
            goto reset;
        }

        /* if we are responder, we wait for key distribution from the peer if any */
        if (!inst->isInitiator &&
            !!(inst->peerKeyDistr & (SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK)))
            break;

        smTransitPhase(inst, SM_PHASE_DONE);

        break;

    case L2C_STATE_RX:
        if (len != sizeof(sg)) {
            loge("invalid length for RX event\n");
            goto fail;
        }
        s = *(sg*)data;
        smRx(inst, s);
        break;

    case L2C_STATE_KEY_REQ:
        if (len != sizeof(struct l2cKeyReqState)) {
            loge("invalid length for key request event\n");
            goto fail;
        }
        keyReqState = (struct l2cKeyReqState *)data;

        if (inst->isInitiator) {
            logw("received key request when we are the initiator\n");
            pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
            goto reset;
        }

        /* TODO(mcchou): finish connection re-establishment mechanism using
         * l2cApiLeSetDefaultSecurityManager and remove the phase check of SM_PHASE_LTK_ENCRYPTED
         */
        /* Handle the case where the peer device try to re-establish security after exchanging LTK,
         * by checking if the EDIV and random number in key request state match with the numbers
         * exchanged in previous pairing session. In this case, we are the responder where the peer
         * will use our LTK for security re-establishment.
         */
        if (inst->phase == SM_PHASE_DONE || inst->phase == SM_PHASE_LTK_ENCRYPTED) {
            logd("Received key request for connection re-establishment\n");
            if (!persistGetDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_MY_RANDOM, &myRand)) {
                logw("Failed to retrieve random number to verify connection re-establishment\n");
                goto fail;
            } else if (!smGenEdiv(&myEdiv, inst, myRand, true) || myEdiv != keyReqState->ediv) {
                logw("Failed to verify EDIV for connection re-establishment\n");
                goto fail;
            } else if (!persistGetDevKey(&inst->peerAddr, KEY_TYPE_MY_LTK, key) ||
                       !smReadReversedBytes(rawKey, sizeof(rawKey), key, sizeof(key))) {
                logw("Failed to retrieve LTK for connection re-establishment\n");
                goto fail;
            } else {
                inst->isInitiator = false; /* for re-establishment, we are the responder for sure */
                l2cApiLeProvideLtk(inst->conn, rawKey);
                smTransitPhase(inst, SM_PHASE_LTK_SENT);
            }
            break;
        }

        if (inst->phase != SM_PHASE_RAND_RESP) {
            l2cApiLeProvideLtk(inst->conn, NULL); /* reply with no key */
            pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
            goto reset;
        }

        /* the controller takes the key in little endian order, so reverse it before sending */
        if (!smReadReversedBytes(rawKey, sizeof(rawKey), inst->stk.key, sizeof(inst->stk.key)) ||
            !l2cApiLeProvideLtk(inst->conn, rawKey)) {
            pairErr = SM_PAIR_ERR_ENCR_CONN;
            goto reset;
        }

        smTransitPhase(inst, SM_PHASE_STK_SENT);

        break;

    case L2C_STATE_ENCR_KEY_REF:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encryption event\n");
            goto fail;
        }
        encrState = (struct l2cEncrState *)data;

        if (inst->phase != SM_PHASE_LTK_SENT) {
            pairErr = SM_PAIR_ERR_UNEXPECTED_L2C_EVT;
            goto reset;
        }

        if (!smIsEncryptExpected(encrState, inst)) {
            logw("Failed to encrypt the connection %llu\n", inst->conn);
            pairErr = SM_PAIR_ERR_ENCR_CONN;
            goto reset;
        }

        /* if the encryption of key refreshing went well, no further action is needed for either
         * initiator or responder
         */
        smTransitPhase(inst, SM_PHASE_LTK_ENCRYPTED);

        break;

    case L2C_STATE_CLOSED:
        smInstDeinit(inst);
        break;

    default:
        logd("unknown L2C event %u\n", evt);
        goto fail;
    }
    return;
reset:
    smTransitPhase(inst, SM_PHASE_START);
fail:
    logd("Got l2cap event 0x%02X in phase %u when we are the %s\n", evt, phase,
         inst->isInitiator ? "initiator" : "responder");
    smNotifyPairObserver(&inst->peerAddr, SM_PAIR_STATE_FAILED, pairErr);
}

/*
 * FUNCTION: smFixedChAlloc
 * USE:      Connection request: if accepted, alloc a per-connection instance of the SM connection
 *           structure
 * PARAMS:   userData - unused
 *           conn - l2c conn handle
 *           instanceP - we store instance here if we allocate one
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t smFixedChAlloc(void *userData, l2c_handle_t conn, void **instanceP)
{
    struct smInstance *inst;
    struct bt_addr peerAddr;
    struct bt_addr myAddr;
    struct smPairSecurityRequirements secReqs;

    if (!l2cApiGetBtAddr(conn, &peerAddr)) {
        loge("Failed to get peer address\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    if(!l2cApiGetSelfBtAddr(conn, &myAddr)) {
        logd("Failed to get local address\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    if (!BT_ADDR_IS_LE(peerAddr)) {
        logd("Refusing SM connection for EDR\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    if (smIsDevBanned(&peerAddr)) {
        logd("Refusing SM connection for banned device\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    inst = (struct smInstance*)calloc(1, sizeof(struct smInstance));
    if (!inst)
        return SVC_ALLOC_FAIL_OTHER;

    inst->peerAddr = peerAddr;
    inst->myAddr = myAddr;
    inst->conn = conn;

    inst->phase = SM_PHASE_START;
    /* if the peer is previously paired, set the phase to done */
    if (smIsPaired(NULL, NULL, &inst->peerAddr))
        inst->phase = SM_PHASE_DONE;

    *instanceP = inst;

    l2cApiLeSetSecurityManagerForAclConn(conn);

    logd("%s() sm channel assigned\n", __func__);

    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: smInit
 * USE:      Init the security manager
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool smInit(uint8_t ioCapability)
{
    uint8_t key[HCI_LE_KEY_LEN] = {0,};
    uint64_t num = 0;

    static const struct l2cServiceFixedChDescriptor descr = {
        .serviceFixedChAlloc = smFixedChAlloc,
        .serviceFixedChStateCbk = smFixedChState,
    };

    if (ioCapability != HCI_DISP_CAP_DISP_ONLY && ioCapability != HCI_DISP_CAP_DISP_YES_NO &&
        ioCapability != HCI_DISP_CAP_KBD_ONLY && ioCapability != HCI_DISP_CAP_NONE &&
        ioCapability != HCI_DISP_CAP_KBD_DISP) {
        return false;
    }
    mIoCap = ioCapability;

    /* TODO(mcchou): this needs to be changed accordingly for the OOB support */
    mHasOob = false;

    if (!(mSmSendQ = sendQueueAlloc(MAX_PACKETS_SM_SEND_QUEUE))) {
        logw("%s(): Error allocating SM sendQ.\n", __func__);
        return false;
    }

    if(!l2cApiServiceFixedChRegister(L2C_FIXED_CH_NUM_SM, &descr)) {
        logw("%s(): Error registering L2CAP service.\n", __func__);
        sendQueueFree(mSmSendQ);
        return false;
    }

    mPairObservers = multiNotifCreate();
    if (!mPairObservers) {
       logw("%s(): Error creating pairing observer list\n", __func__);
       return false;
    }

    persistLoad();

    logd("%s() smInit finished correctly\n", __func__);

    return true;
}

/*
 * FUNCTION: smDeinit
 * USE:      Deinit the security manager
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void smDeinit(void)
{
    multiNotifDestroy(mPairObservers);

    persistStore();

    smBannedDevFreeAll();
    sendQueueFree(mSmSendQ);
    l2cApiServiceFixedChUnregister(L2C_FIXED_CH_NUM_SM, NULL, NULL);
}

/*
 * FUNCTION: smRegisterPairObserver
 * USE:      Register as an observer if interested in receiving notifications about pairing state
 *           changes with remote LE devices
 * PARAMS:   observerData - observer's own data passed when the callback is invoked
 *           cbk - the callback invoked if there is any pairing state change
 * RETURN:   a none-zero unique observer ID in success case; 0 otherwise
 * NOTES:
 */
uniq_t smRegisterPairObserver(void *observerData, smPairStateChangeCbk cbk)
{
    if (!cbk)
        return 0;
    return multiNotifRegister(mPairObservers, cbk, observerData);
}

/*
 * FUNCTION: smUnregisterPairObserver
 * USE:      Unregister as an observer if no longer interested in receiving notifications about
 *           pairing state changes with remote LE devices
 * PARAMS:   observerId - an unique observer ID
 * RETURN:   NONE
 * NOTES:
 */
void smUnregisterPairObserver(uniq_t observerId)
{
    multiNotifUnregister(mPairObservers, observerId);
}

/*
 * FUNCTION: smNotifyPairObserver
 * USE:      Notify the observers about the state change of a device
 * PARAMS:   addr - the address of a device
 *           state - the new pairing state
 * RETURN:   NONE
 * NOTES:
 */
static void smNotifyPairObserver(const struct bt_addr *addr, smPairState state, smPairErr err)
{
    struct smPairStateChange change;
    bool notifyNotPaired = false;

    switch(state) {
    case SM_PAIR_STATE_NOT_PAIRED:
    case SM_PAIR_STATE_START:
    case SM_PAIR_STATE_PAIRED:
    case SM_PAIR_STATE_CANCELED:
    case SM_PAIR_STATE_FAILED:
        break;
    default:
        logd("Drop unknown pair state %u with err %u\n", state, err);
        return;
    }

    switch(err) {
    case SM_PAIR_ERR_NONE:
        if (state == SM_PAIR_STATE_FAILED) {
            logd("Drop unexpected pair state %u with err %u\n", state, err);
            return;
        }
        break;
    case SM_PAIR_ERR_ALREADY_PAIRED:
    case SM_PAIR_ERR_IN_PROGRESS:
    case SM_PAIR_ERR_NO_SUCH_DEVICE:
    case SM_PAIR_ERR_INVALID_PAIR_REQ:
    case SM_PAIR_ERR_PASSKEY_FAILED:
    case SM_PAIR_ERR_OOB_NOT_AVAILABLE:
    case SM_PAIR_ERR_AUTH_REQ_INFEASIBLE:
    case SM_PAIR_ERR_CONF_VALUE_MISMATCHED:
    case SM_PAIR_ERR_PAIRING_NOT_SUPPORTED:
    case SM_PAIR_ERR_ENCR_KEY_SIZE:
    case SM_PAIR_ERR_REPEATED_ATTEMPT:
    case SM_PAIR_ERR_INVALID_PARAM:
    case SM_PAIR_ERR_MEMORY:
    case SM_PAIR_ERR_L2C_CONN:
    case SM_PAIR_ERR_UNEXPECTED_SM_CMD:
    case SM_PAIR_ERR_SEND_SM_CMD:
    case SM_PAIR_ERR_ENCR_CONN:
    case SM_PAIR_ERR_UNEXPECTED_L2C_EVT:
    case SM_PAIR_ERR_STALLED:
    case SM_PAIR_ERR_UNKNOWN:
        if (state != SM_PAIR_STATE_FAILED) {
            logd("Drop unexpected pair state %u with err %u\n", state, err);
            return;
        }
        break;
    default:
        logd("Drop unknown pair error %u with state %u\n", err, state);
        return;
    }

    change.peerAddr = *addr;
    change.pairState = state;
    change.pairErr = err;
    multiNotifNotify(mPairObservers, &change);
}

/*
 * FUNCTION: smPair
 * USE:      Pair with a peer device with security requirements enforced
 * PARAMS:   addr - the address of the peer device
 *           secReqs - the security requirements of the pairing
 * RETURN:   NONE
 * NOTES:    The outcome will be notified via smPairStateChangeCbk once a client registered as an
 *           observer of pairing state changes
 */
void smPair(const struct bt_addr *addr, const struct smPairSecurityRequirements *secReqs)
{
    bool mitm = false, bonded = false;
    struct smInstance *inst = NULL;
    smPairErr pairErr = SM_PAIR_ERR_UNKNOWN;

    if (!addr || !secReqs) {
        logd("%s(): Invalid peer address or security requirements\n", __func__);
        pairErr = SM_PAIR_ERR_INVALID_PAIR_REQ;
        goto fail;
    }

    logd("%s(): "ADDRFMT"\n", __func__, ADDRCONV(*addr));

    /* check if there is an existing instance with the same peer device */
    pthread_mutex_lock(&mInstLock);
    inst = smInstFindByAddr(addr);
    pthread_mutex_unlock(&mInstLock);
    if (inst) {
        logd("%s(): Pairing in progress with "ADDRFMT"\n", __func__, ADDRCONV(*addr));
        pairErr = SM_PAIR_ERR_IN_PROGRESS;
        goto fail;
    }

    /* check if the device was previously paired with the security requirements met */
    if (smIsPaired(&mitm, &bonded, addr) && (secReqs->bond ? bonded : true) &&
        (secReqs->mitm ? mitm : true)) {
        logd("%s(): Already paired with "ADDRFMT"\n", __func__, ADDRCONV(*addr));
        pairErr = SM_PAIR_ERR_ALREADY_PAIRED;
        goto fail;
    }

    /* clear the previously pairing info and start the new pairing */
    logi("%s(): Clear pairing info if any to start pairing with higher security requirements "
         ADDRFMT"\n", __func__, ADDRCONV(*addr));
    persistDelDevKeys(addr);
    persistDelDevNumbers(addr);

    inst = (struct smInstance*)calloc(1, sizeof(struct smInstance));
    if (!inst) {
        logd("%s(): Failed to create instance for pairing with "ADDRFMT"\n", __func__,
             ADDRCONV(*addr));
        pairErr = SM_PAIR_ERR_MEMORY;
        goto fail;
    }

    inst->peerAddr = *addr;

    inst->isInitiator = true; /* we are the initiator of pairing */
    inst->phase = SM_PHASE_START;
    inst->myAuthReq = (secReqs->bond ? SM_AUTH_REQ_BOND_BOND : 0x00) |
                      (secReqs->mitm ? SM_AUTH_REQ_MITM_FLAG : 0x00);
    inst->myMaxKeySz = SM_MAX_KEY_LEN;
    inst->myKeyDistr = SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK;

    if (!l2cApiCreateFixedChConnection(L2C_FIXED_CH_NUM_SM, addr, smFixedChState, NULL, inst)) {
        logw("%s(): Error creating a fixed channel l2cap connection\n", __func__);
        pairErr = SM_PAIR_ERR_L2C_CONN;
        goto fail;
    }

    smNotifyPairObserver(&inst->peerAddr, SM_PAIR_STATE_START, SM_PAIR_ERR_NONE);
    return;
fail:
    smNotifyPairObserver(addr, SM_PAIR_STATE_FAILED, pairErr);
}

/*
 * FUNCTION: smUnpair
 * USE:      Unpair with a peer device by removing all stored pairing info from the database OR
 *           cancel the pairing with a peer device
 * PARAMS:   addr - the address of the peer device
 * RETURN:   None
 * NOTES:    The outcome will be notified via smPairStateChangeCbk once a client registered as an
 *           observer of pairing state changes
 */
void smUnpair(const struct bt_addr *addr)
{
    struct smInstance *inst;
    bool cancelled = false;

    if (!addr || !persistGetKnownDev(addr, NULL, NULL, NULL)) {
        smNotifyPairObserver(addr, SM_PAIR_STATE_FAILED, SM_PAIR_ERR_NO_SUCH_DEVICE);
        return;
    }

    logd("%s(): "ADDRFMT"\n", __func__, ADDRCONV(*addr));

    /* check if there is an existing instance with the peer device */
    pthread_mutex_lock(&mInstLock);
    inst = smInstFindByAddr(addr);
    pthread_mutex_unlock(&mInstLock);
    if (inst) {
        /* Let L2CAP handle the removal of the instance to avoid race condition */
        if (inst->conn)
            l2cApiDisconnect(inst->conn);
        cancelled = inst->phase != SM_PHASE_DONE && inst->phase != SM_PHASE_LTK_ENCRYPTED;
    }

    persistDelDevKeys(addr);
    persistDelDevNumbers(addr);

    logd("%s(): Pairing with "ADDRFMT" is %s\n", __func__, ADDRCONV(*addr),
         cancelled ? "cancelled" : "forgotten");
    smNotifyPairObserver(addr, cancelled ? SM_PAIR_STATE_CANCELED : SM_PAIR_STATE_NOT_PAIRED,
                         SM_PAIR_ERR_NONE);
}

/*
 * FUNCTION: smCmacDo
 * USE:      Perform the CMAC calculation
 * PARAMS:   dst - where the store the result
 *           macLen - how long of a digest do we want (in bytes)
 *           m - the message to digest
 *           k - the key to use
 *           k1 - subkey 1 (from smCmacPrepare)
 *           k2 - subkey 2 (from smCmacPrepare)
 * RETURN:   NONE
 * NOTES:
 */
static void smCmacDo(uint8_t *dst, uint8_t macLen, sg m, const uint8_t *k, const uint8_t *k1,
                     const uint8_t *k2)
{
    uint32_t len = sgLength(m), pos = 0;
    uint32_t i, nblocks = len ? (len + SM_BLOCK_LEN - 1) / SM_BLOCK_LEN : 1;
    uint8_t tmp1[SM_BLOCK_LEN] = {0,}, tmp2[SM_BLOCK_LEN], j, *in = tmp1, *out = tmp2, *swap;

    for (i = 0; i < nblocks; i++) {
        for(j = 0; j < len && j < SM_BLOCK_LEN; j++) {
            uint8_t byte;

            sgSerialize(m, pos++, sizeof(uint8_t),&byte);
            in[j] ^= byte;
        }
        len -= j;
        if (!len) {
            if (j == SM_BLOCK_LEN) {
                for(j = 0; j < SM_BLOCK_LEN; j++)
                    in[j] ^= *k1++;
            } else {
                in[j] ^= 0x80;
                for(j = 0; j < SM_BLOCK_LEN; j++)
                    in[j] ^= *k2++;
            }
        }
        smEncrypt(out, in, k);
        swap = in;
        in = out;
        out = swap;
    }
    while (macLen--)
        *dst++ = *in++;
}

/*
 * FUNCTION: smCmacPrepare
 * USE:      Perform the pre-calculation of K1 & K2 for the CMAC
 * PARAMS:   k1 - where to store k1
 *           k2 - where to store k2
 *           k - the k input value
 * RETURN:   NONE
 * NOTES:
 */
static void smCmacPrepare(uint8_t *k1, uint8_t *k2, const uint8_t *k)
{
    uint8_t tmp[SM_BLOCK_LEN] = {0,}, *out = k1, i, j;

    smEncrypt(k1, tmp, k);
    for (i = 0; i < 2; i++) {
        bool shiftOut;
        shiftOut = !!(k1[0] & 0x80);
        for (j = 0; j < 15; j++)
            out[j] = (k1[j] << 1) | (k1[j + 1] >> 7);
        out[15] = k1[15] << 1;
        if (shiftOut)
            out[15] ^= 0x87;
        out = k2;
    }
}

/*
 * FUNCTION: smSignatureCalc
 * USE:      Perform a CMAC calculation in its entirety
 * PARAMS:   data - the data to checksum
 *           len - length of said data
 *           key - the key
 *           sig - sig gets stored here
 * RETURN:   NONE
 * NOTES:
 */
void smSignatureCalc(sg m, const uint8_t *key, uint8_t *sig)
{
    uint8_t k1[SM_BLOCK_LEN], k2[SM_BLOCK_LEN];

    smCmacPrepare(k1, k2, key);
    smCmacDo(sig, SM_SIG_LEN, m, key, k1, k2);
}

/*
 * FUNCTION: smCalcKey
 * USE:      Perform the calculation of a key
 * PARAMS:   out - store the result here
 *           k - the k value as per spec
 *           p1 - the p1 value as per spec
 *           p2 - the p2 calue as per spec
 * RETURN:   NONE
 * NOTES:    The SM docs call this the s1() function
 */
static_or_test void smCalcKey(uint8_t *out, const uint8_t *k, const uint8_t *r1, const uint8_t *r2)
{
    uint8_t t[SM_BLOCK_LEN];
    uint8_t i;

    for (i = 0; i < 8; i++) {
        t[i + 0] = r1[i + 8];
        t[i + 8] = r2[i + 8];
    }
    smEncrypt(out, t, k);
}

/*
 * FUNCTION: smCalcConfVal
 * USE:      Perform the calculation of the confirmation value
 * PARAMS:   out - store the result here
 *           k - the k value as per spec
 *           r - the r value as per spec
 *           pres - the pres value as per spec, only bottom 56 bit used
 *           preq - the pres value as per spec, only bottom 56 bit used
 *           iat - the iat value as per spec
 *           ia - the ia value as per spec, only bottom 48 bits used
 *           rat - the rat value as per spec
 *           ra - the ra value as per spec, only bottom 48 bits used
 * RETURN:   NONE
 * NOTES:    The SM docs call this the c1() function
 */
static_or_test void smCalcConfVal(uint8_t* out, const uint8_t *k, const uint8_t *r, uint64_t pres,
                                  uint64_t preq, bool iat, uint64_t ia, bool rat, uint64_t ra)
{
    uint8_t p1[SM_BLOCK_LEN];
    uint8_t p2[SM_BLOCK_LEN] = {0,};
    uint8_t i;

    /* create p1 */
    for (i = 0; i < 7; i++) {
        p1[6 - i] = pres;
        p1[13 - i] = preq;
        pres >>= 8;
        preq >>= 8;
    }
    p1[14] = rat ? 1 : 0;
    p1[15] = iat ? 1 : 0;

    /* create p2 */
    for (i = 0; i < 6; i++) {
        p2[9 - i] = ia;
        p2[15 - i] = ra;
        ia >>= 8;
        ra >>= 8;
    }

    /* p1 ^= r */
    for (i = 0; i < SM_BLOCK_LEN; i++)
        p1[i] ^= r[i];

    /* out = e(k, p1) */
    smEncrypt(out, p1, k);

    /* p2 ^= out */
    for (i = 0; i < SM_BLOCK_LEN; i++)
        p2[i] ^= out[i];

    /* out = e(k, p2) */
    smEncrypt(out, p2, k);
}

/*
 * FUNCTION: smCalcDivMask
 * USE:      Perform the calculation of the DIV mask value
 * PARAMS:   mask - a 16-bit DIV mask value
 *           k - the 128-bit DHK, k value as per spec
 *           r - the 64-bit Rand, r value as per spec
 * RETURN:   NONE
 * NOTES:    The SM docs call this the dm() function
 */
static void smCalcDivMask(uint16_t *mask, const uint8_t *k, uint64_t r)
{
    uint8_t out[SM_BLOCK_LEN] = {0,};
    uint8_t paddingR[SM_BLOCK_LEN] = {0,};
    uint8_t i;

    if (!mask || !k)
        return;

    for (i = SM_BLOCK_LEN - 1; i >= SM_BLOCK_LEN - sizeof(r); --i) {
        paddingR[i] = r;
        r >>= 8;
    }

    smEncrypt(out, k, paddingR);

    /* truncate to 16 bits by taking the least significant 16 bits of the output of e() */
    *mask = 0;
    *mask = out[SM_BLOCK_LEN - 2];
    *mask <<= 8;
    *mask |= out[SM_BLOCK_LEN - 1];
}

/*
 * FUNCTION: smPrepPreqPrsp
 * USE:      Prepare the pairing request command or the pairing response
 *           command by reconstructing fields into a uint64_t value
 * PARAS:    cmd - output
 *           reqRsp - command type
 *           ioCap - IO capability
 *           hasOob - OOB data flag
 *           authReq - security properties
 *           maxKeySz - maximum key size
 *           initKeyDistr - initiator key distribution
 *           rspKeyDistr - responder key distribution
 * RETURN:   success
 * NOTES:
 */
static bool smPrepPreqPrsp(
    uint64_t *cmd, uint8_t reqRsp, uint8_t ioCap, bool hasOob, uint8_t authReq, uint8_t maxKeySz,
    uint8_t initKeyDistr, uint8_t rspKeyDistr)
{
    uint8_t oob = hasOob;

    if (!cmd)
        return false;

    if (reqRsp != SM_PAIRING_REQ && reqRsp != SM_PAIRING_RSP)
        return false;

    *cmd = utilGetBE8(&rspKeyDistr);
    *cmd = *cmd << 8 | utilGetBE8(&initKeyDistr);
    *cmd = *cmd << 8 | utilGetBE8(&maxKeySz);
    *cmd = *cmd << 8 | utilGetBE8(&authReq);
    *cmd = *cmd << 8 | utilGetBE8(&oob);
    *cmd = *cmd << 8 | utilGetBE8(&ioCap);
    *cmd = *cmd << 8 | reqRsp;

    return true;
}

/*
 * FUNCTION: smPrepInitRspAddr
 * USE:      Prepare the initiator device address or the responding device
 *           address by reconstructing a struct bt_addr into an uint64_t value
 * PARAS:    a - output uint64_t
 *           at - output address types (public or random)
 *           addr - address
 * RETURN:   success
 * NOTES:
 */
static bool smPrepInitRspAddr(uint64_t *a, uint8_t *at, const struct bt_addr *addr)
{
    int i;
    uint8_t type;

    if (!addr)
        return false;

    *at = (addr->type == BT_ADDR_TYPE_LE_RANDOM) ? 1 : 0;
    for (i = sizeof(addr->addr) - 1, *a = 0; i >= 0; --i)
        *a = ((*a) << 8) | addr->addr[i]; /* addr is stored in [0]->[5] : LSO -> MSO */

    return true;
}

/*
 * FUNCTION: smGenSTK
 * USE:      Generate a 128-bit Short Term Key, mask it with the negotiated key size and store it
 *           in the given instance
 * PARAS:    inst - instance
 * RETURN:   success
 * NOTES:
 */
static bool smGenSTK(struct smInstance *inst)
{
    uint8_t i, maskLen;

    if (!inst) {
        logw("%s(): Invalid instance\n", __func__);
        return false;
    }

    smCalcKey((uint8_t *)inst->stk.key, (uint8_t *)inst->tk.key,
              inst->isInitiator ? inst->peerRandNum.rand : inst->myRandNum.rand,
              inst->isInitiator ? inst->myRandNum.rand : inst->peerRandNum.rand);

    maskLen = SM_MAX_KEY_LEN -
        (inst->myMaxKeySz > inst->peerMaxKeySz ? inst->peerMaxKeySz : inst->myMaxKeySz);
    for (i = 0; i < maskLen; ++i)
        inst->stk.key[i] = 0x00;

    return true;
}

/*
 * FUNCTION: smGenTK
 * USE:      Generate a 128-bit Temporary Key and store it in the given instance
 * PARAS:    inst - instance
 * RETURN:   success
 * NOTES:
 */
static bool smGenTK(struct smInstance *inst)
{
    if (!inst) {
        logw("%s(): Invalid instance\n", __func__);
        return false;
    }

    if ((inst->alg = smSelPairAlg(inst)) == SM_ALG_UNKNOWN)
        return false;

    logd("Pairing method chosen: 0x%02X\n", inst->alg);

    /* generate TK */
    switch(inst->alg) {
    case SM_ALG_JUST_WORK:
        memset(inst->tk.key, 0, sizeof(inst->tk.key));
        inst->isMitmSafe = false;
        break;
    case SM_ALG_PASS_KEY: /* TODO(mcchou): retrieve passkey from user */
    case SM_ALG_OOB:      /* TODO(mcchou): retrieve OOB data from OOB source */
        logw("%s(): Pairing method %d not implemented\n", __func__, inst->alg);
        inst->isMitmSafe = true;
        return false;
    default:
        logw("%s(): Unknown pairing algorithm 0x%02X", __func__, inst->alg);
      return false;
    }

    if(!persistAddDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_MITM_PROTECT,
                            inst->isMitmSafe ? SM_AUTH_MITM_PROTECT : SM_UNAUTH_NO_MITM_PROTECT)) {
        logw("%s(): Failed to store MITM security property\n", __func__);
        return false;
    }

    return true;
}

/*
 * FUNCTION: smGenConfVal
 * USE:      Generate a 128-bit random number and store it in the given instance. This should be
 *           called after calling smGenTK.
 * PARAS:    rand - the output random number
 *           inst - instance
 *           check - indicate whether this is confirm value check or not, if true, peer random
 *                   value will be used as the input of confirm value calculation; otherwise the
 *                   local random value will be used.
 * RETURN:   success
 * NOTES:
 */
static bool smGenConfVal(struct smPairConf *conf, const struct smInstance *inst, bool check)
{
    uint64_t pReqCmd = 0, pRspCmd = 0, ia = 0, ra = 0;
    uint8_t iat = 0, rat = 0;
    int i;

    if (!inst) {
        logw("%s(): Invalid instance\n", __func__);
        return false;
    }

    /* prepare params for calculation of confirm value */
    if (inst->isInitiator) {
        if (!smPrepPreqPrsp(&pReqCmd, SM_PAIRING_REQ, mIoCap, mHasOob, inst->myAuthReq,
                            inst->myMaxKeySz, inst->peerKeyDistr, inst->myKeyDistr) ||
            !smPrepPreqPrsp(&pRspCmd, SM_PAIRING_RSP, inst->peerIoCap, inst->peerHasOob,
                            inst->peerAuthReq, inst->peerMaxKeySz, inst->peerKeyDistr,
                            inst->myKeyDistr) ||
            !smPrepInitRspAddr(&ia, &iat, &inst->myAddr) ||
            !smPrepInitRspAddr(&ra, &rat, &inst->peerAddr))
            goto fail;
    } else {
        if (!smPrepPreqPrsp(&pReqCmd, SM_PAIRING_REQ, inst->peerIoCap, inst->peerHasOob,
                            inst->peerAuthReq, inst->peerMaxKeySz, inst->myKeyDistr,
                            inst->peerKeyDistr) ||
            !smPrepPreqPrsp(&pRspCmd, SM_PAIRING_RSP, mIoCap, mHasOob, inst->myAuthReq,
                            inst->myMaxKeySz, inst->myKeyDistr, inst->peerKeyDistr) ||
            !smPrepInitRspAddr(&ia, &iat, &inst->peerAddr) ||
            !smPrepInitRspAddr(&ra, &rat, &inst->myAddr))
            goto fail;
    }

    smCalcConfVal(conf->confirmVal, (uint8_t *)inst->tk.key,
                  check ? inst->peerRandNum.rand : inst->myRandNum.rand,
                  pRspCmd, pReqCmd, iat, ia, rat, ra);

    return true;

fail:
    logw("%s(): Preparation error\n", __func__);
    return false;
}

/*
 * FUNCTION: smGenRandNum
 * USE:      Generate a random number
 * PARAS:    out - the output random number
 *           numLen - the wanted byte length of the random number where the max length is 16
 * RETURN:   success
 * NOTES:
 */
static bool smGenRandNum(uint8_t *out, uint8_t numLen)
{
    int r = 0;
    uint16_t i, rSz, cp, rem;
    struct timespec ts;

    if (!out) {
        logw("%s(): Invalid destination for random number\n", __func__);
        return false;
    }

    if (!timespec_get(&ts, TIME_UTC)) {
        logd("%s(): Filed to retrieve time\n", __func__);
        return false;
    }

    srandom(ts.tv_nsec ^ ts.tv_sec);

    rem = numLen > SM_BLOCK_LEN ? SM_BLOCK_LEN : numLen;
    for (i = 0, rSz = sizeof(r); rem > 0; i += cp, rem -= cp) {
        r = random();
        cp = rem > rSz ? rSz : rem;
        memcpy(&out[i], &r, cp);
    }

    return true;
}

/*
 * FUNCTION: smGenLtk
 * USE:      Generate a 128-bit Long Term Key and store it in database
 * PARAS:    inst - the instance
 * RETURN:   success if LTK is valid and stored
 * NOTES:    We use Database Lookup approach for key management.
 */
static bool smGenLtk(struct smInstance *inst)
{
    uint8_t key[HCI_LE_KEY_LEN] = {0,};

    if (!inst || !smGenRandNum(key, HCI_LE_KEY_LEN))
        return false;

    return persistAddDevKey(&inst->peerAddr, KEY_TYPE_MY_LTK, key);
}

/*
 * FUNCTION: smGenEdiv
 * USE:      Generate a 16-bit EDIV
 * PARAS:    out - where EDIV is stored
 *           inst - the instance
 *           rand - random number
 *           check - indicate whether this is ediv value check or not. if true, we use previously
 *                   store DIV value to calculate EDIV; otherwise a random DIV value is generated
 *                   and stored.
 * RETURN:   true if EDIV is valid; false otherwise
 * NOTES:    We use Database Lookup approach for key management.
 */
static bool smGenEdiv(uint16_t *out, const struct smInstance* inst, uint64_t rand, bool check)
{
    uint8_t dhk[SM_BLOCK_LEN] = {0,};
    uint16_t div = 0, mask = 0, ediv = 0;
    uint64_t tmp = 0;

    if (check) {
        if (!persistGetDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_MY_DIV, &tmp))
            return false;
        div = tmp;
    } else {
        /* generate a 16-bit DIV value */
        if (!smGenRandNum((uint8_t *)&div, sizeof(div)) ||
            !persistAddDevNumber(&inst->peerAddr, PERSIST_NUM_TYPE_SM_MY_DIV, div))
            return false;
    }

    /* generate a 16-bit DIV mask value */
    if (!persistGetDevKey(NULL, KEY_TYPE_DHK, dhk))
        return false;
    smCalcDivMask(&mask, dhk, rand);

    *out = mask^div;

    return true;
}

/*
 * FUNCTION: smAddressHash
 * USE:      Perform 24bit->24bit hash for address resolution
 * PARAMS:   r - the 24 bits to hash
 *           key - the key
 * RETURN:   the 24-bit hash
 * NOTES:    The SM docs call this the ah() function
 */
static uint32_t smAddressHash(uint32_t r, const uint8_t *key)
{
    uint8_t in[SM_BLOCK_LEN] = {0,}, out[SM_BLOCK_LEN];

    utilSetBE24(in + 13, r);
    smEncrypt(out, in, key);
    return utilGetBE24(out + 13);
}

/* a simple AES-128 implementation - we use it rarely enough that even a nonoptimal version will do */

static void smEncryptSubBytes(uint8_t *bytes, uint8_t num)
{
    static const uint8_t sbox[] = {
        0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
        0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
        0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
        0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
        0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
        0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
        0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
        0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
        0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
        0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
        0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
        0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
        0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
        0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
        0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
        0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
    };
    uint8_t i;

    for (i = 0; i < num; i++)
        bytes[i] = sbox[bytes[i]];
}

static void smEncryptShiftRows(uint8_t *state)
{
    uint8_t i, j;
    uint8_t tmp[4];

    for (i = 0; i < 4; i++) {
        for(j = 0; j < 4; j++)
            tmp[j] = state[i * 4 + j];

        for(j = 0; j < 4; j++)
            state[i * 4 + j] = tmp[(i + j) & 3];
    }
}

static uint8_t smEncryptMul(uint8_t vin, uint8_t by)
{
    uint16_t v = vin;

    switch(by){
    case 1:
        break;
    case 2:
        v <<= 1;
        break;
    case 3:
        v = (v << 1) ^ v;
        break;
    }
    if (v & 0x100)
        v ^= 0x1B;

    return v;
}

static void smEncryptMixColumns(uint8_t *state)
{
    uint8_t i, j, k, ret[SM_BLOCK_LEN];
    static const uint8_t matrix[] = {2,3,1,1,1,2,3,1,1,1,2,3,3,1,1,2};

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            ret[i * 4 + j] = 0;
            for(k = 0; k < 4; k++)
                ret[i * 4 + j] ^= smEncryptMul(state[k * 4 + j], matrix[i * 4 + k]);
        }
    }

    for (i = 0; i <SM_BLOCK_LEN; i++)
        state[i] = ret[i];
}

static void smEncryptAddRoundKey(uint8_t *state, const uint8_t *keyPos)
{
    uint8_t i, j;

    for (i = 0; i < 4; i++, keyPos += 40)
        for(j = 0; j < 4; j++)
            *state++ ^= *keyPos++;
}

static void smEncryptExpandKey(uint8_t *expanded, const uint8_t *key)
{
    static const uint8_t rcon[] = {1,0,0,0, 2,0,0,0, 4,0,0,0, 8,0,0,0, 16,0,0,0, 32,0,0,0, 64,0,0,0,
                                   128,0,0,0, 27,0,0,0, 54,0,0,0};
    uint8_t i, j, temp[4];

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            expanded[i + 44 * j] = key[i * 4 + j];

    for (i = 4; i < 44; i++) {
        for(j = 0; j < 4; j++)
            temp[j] = expanded[i - 1 + 44 * j];

        if (!(i & 3)) {
            j = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = j;
            smEncryptSubBytes(temp, 4);
            for (j = 0; j < 4; j++)
                temp[j] ^= rcon[(i &~ 3) - 4 + j];
        }

        for(j = 0; j < 4; j++)
            expanded[i + 44 * j] = expanded[i - 4 + 44 * j] ^ temp[j];
    }
}

/*
 * FUNCTION: smEncrypt
 * USE:      Perform an AES-128 encryption
 * PARAMS:   dst - place to store result (SM_ENCRYPT_DATA_LEN bytes)
 *           src - the plain text (SM_ENCRYPT_DATA_LEN bytes)
 *           key - the key (SM_ENCRYPT_DATA_LEN bytes)
 * RETURN:   NONE
 * NOTES:    The SM docs call this the e() function
 */
static_or_test void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key)
{
    uint8_t expandedKey[176];
    uint8_t state[SM_BLOCK_LEN];
    uint8_t i, j;

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            state[i + 4 * j] = *src++;

    smEncryptExpandKey(expandedKey, key);


    smEncryptAddRoundKey(state, expandedKey + 0);

    for (i = 1; i < 10; i++) {
        smEncryptSubBytes(state, SM_BLOCK_LEN);
        smEncryptShiftRows(state);
        smEncryptMixColumns(state);
        smEncryptAddRoundKey(state, expandedKey + i * 4);
    }

    smEncryptSubBytes(state, SM_BLOCK_LEN);
    smEncryptShiftRows(state);
    smEncryptAddRoundKey(state, expandedKey + i * 4);

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            *dst++ = state[i + 4 * j];
}
