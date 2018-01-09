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
    uint8_t confirmVal[16]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smPairRand {
    uint8_t rand[16]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smPairFailed {
    uint8_t reason; /* SM_ERR_* */
} __packed;

struct smKey {
    uint8_t key[16]; /* [0]->[15] : MSO->LSO */
} __packed;

struct smMasterInfo {
    uint16_t ediv;
    uint64_t rand;
} __packed;

struct smIdentityAddrInfo {
    uint8_t addrType;
    uint8_t mac[6];
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
#define SM_ERR_INVALID_PARAMS           0x0A /* unparseable command or invalid params in command */

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
#define SM_MIN_KEY_LEN                  7
#define SM_MAX_KEY_LEN                  16


/* definition of structs and macros based on our implementation */
#define SM_PHASE_START                  0 /* nothing happened yet. */
#define SM_PHASE_REQ_SENT               1 /* we sent pair req. waiting for pair resp. */
#define SM_PHASE_REQ_RESP               2 /* the peer has sent us a pair request, we replied. waiting for Mconfim. OR we sent req and got resp */
#define SM_PHASE_CONF_SENT              3 /* we sent Mconfirm. waiting for Sconfirm. */
#define SM_PHASE_CONF_RESP              4 /* the peer has sent us Mconfirm, we replied with Sconfirm. waiting for Mrand. OR we sent Mconfirm and got Sconfim. */
#define SM_PHASE_RAND_SENT              5 /* we sent Mrand. wainting for Srand. */
#define SM_PHASE_RAND_RESP              6 /* other peer has sent us Mrand, we replied with Srand. OR we sent Mrand and got Srand.*/

#define SM_WAIT_INTVL_FACTOR            2     /* the wait interval grows exponentially with base 2 */
#define SM_DEF_WAIT_INTVL               0     /* 0 sec in msec - default wait interval */
#define SM_MIN_WAIT_INTVL               4000  /* 4 sec in msec - minimum wait interval */
#define SM_MAX_WAIT_INTVL               64000 /* 64 sec in msec - maximum wait interval */
#define SM_STALL_INTVL                  30000 /* 30 sec in msec - timeout of a pairing process */

struct smInstance {
    struct smInstance*    prev;
    struct smInstance*    next;

    struct bt_addr        peerAddr;
    struct bt_addr        myAddr;
    l2c_handle_t          conn;
    bool                  isInitiator; /* whether we are the initiator */

    uint8_t               phase;
    uniq_t                stallTimer;  /* the timer tracking whether a pairing session is stalled */

    struct smKey          tk;
    struct smKey          stk;

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

/* fwd decls */
static bool smGenSTK(struct smInstance *inst);
static bool smGenTK(struct smInstance *inst);
static bool smGenConfVal(struct smPairConf *conf, const struct smInstance *inst, bool check);
static bool smGenRandNum(struct smInstance *inst);
static_or_test void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);

/* sendQ for packet transmissions */
static struct sendQ *mSmSendQ;

/* pairing algorithms */
#define SM_ALG_JUST_WORK                0x00
#define SM_ALG_PASS_KEY                 0x01
#define SM_ALG_OOB                      0x02
#define SM_ALG_UNKNOWN                  0xFF

/* table of pairing algorithms -mPairAlgs[responder][initiator] */
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
 * FUNCTION: smEncrChanged
 * USE:      Encryption changed - handle it
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
 */
static void smEncrChanged(struct smInstance *inst)
{
    /* TODO(mcchou) */
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
 * PARAMS:   inst - the instance
 *           pairReqResp - the struct
 * RETURN:   true if all values were read & found valid
 * NOTES:
 */
static bool smParsePairReqResp(struct smInstance *inst, const struct smPairReqResp *pairReqResp)
{
    uint8_t tmp, initKeyDistr, rspKeyDistr;
    tmp = utilGetLE8(&pairReqResp->ioCap);
    if (tmp != HCI_DISP_CAP_DISP_ONLY && tmp != HCI_DISP_CAP_DISP_YES_NO &&
        tmp != HCI_DISP_CAP_KBD_ONLY && tmp != HCI_DISP_CAP_NONE &&
        tmp != HCI_DISP_CAP_KBD_DISP) {
        logd("Invalid io caps RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerIoCap = tmp;

    tmp = utilGetLE8(&pairReqResp->oobDataFlag);
    if (tmp != 0 && tmp != 1) {
        logd("Invalid oob flag RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerHasOob = !!tmp;

    tmp = utilGetLE8(&pairReqResp->authReq);
    if ((tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_NO &&
         (tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_BOND) {
        logd("Invalid authReq RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerAuthReq = tmp;

    tmp = utilGetLE8(&pairReqResp->maxKeySz);
    if (tmp < SM_MIN_KEY_LEN || tmp > SM_MAX_KEY_LEN) {
        logd("Invalid maxKeySize RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerMaxKeySz = tmp;

    tmp = utilGetLE8(&pairReqResp->initiatorKeyDistr);
    if (tmp &~ (SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK)) {
        logd("Invalid keyDistr.i RXed 0x%02X\n", tmp);
        return false;
    }
    initKeyDistr = tmp;

    tmp = utilGetLE8(&pairReqResp->responderKeyDistr);
    if (tmp &~ (SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK)) {
        logd("Invalid keyDistr.r RXed 0x%02X\n", tmp);
        return false;
    }
    rspKeyDistr = tmp;

    if (inst->isInitiator) { /* must be a response. accept as many as given */
        /* TODO(mcchou): do not send any keys we did not already commit to send inst->myKeyDistr &= initKeyDistr; */
        inst->myKeyDistr = initKeyDistr;
        inst->peerKeyDistr = rspKeyDistr;
    } else { /* must be a request - agree to send all keys they want, accept whatever was given */
        inst->myKeyDistr = rspKeyDistr;
        inst->peerKeyDistr = initKeyDistr;
    }

    return true;
}

/*
 * FUNCTION: smReverseBytes
 * USE:      Reverse the byte stream field of a pairing struct and store bytes
 *           in MSB first order
 * PARAMS:   out - the output byte array
 *           outLen - the length of out
 *           in - the input byte array
 *           InLen - the length of in
 * RETURN:   success
 * NOTES:    Used for Pairing Confirm CMD, Pairing Random CMD
 */
static bool smReverseBytes(uint8_t *out, int outLen, const uint8_t *in, int inLen)
{
    int i;

    if (outLen <= 0 || inLen <= 0 || outLen != inLen)
        return false;

    for (i = 0; i < inLen; ++i)
        out[i] = utilGetLE8(&in[inLen - i - 1]);
    return true;
}

/*
 * FUNCTION: smSendPairReq
 * USE:      Send pairing request
 * PARAMS:   inst - the instance of the initiator
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairReq(struct smInstance *inst)
{
    struct smPairReqResp req;

    inst->isInitiator = true;

    utilSetLE8(&req.ioCap, mIoCap);
    /* TODO(mcchou): this needs to be changed accordingly for OOB support */
    utilSetLE8(&req.oobDataFlag, mHasOob);
    utilSetLE8(&req.authReq, inst->myAuthReq);
    utilSetLE8(&req.maxKeySz, inst->myMaxKeySz);
    utilSetLE8(&req.initiatorKeyDistr, inst->myKeyDistr);
    /* TODO(mcchou): this needs to be changed */
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
 * PARAMS:   inst - the instance of the initiator
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
 * PARAMS:   inst - the instance of the initiator
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairConf(const struct smInstance *inst)
{
    uint16_t i, confSz;
    struct smPairConf conf;

    for (i = 0, confSz = sizeof(inst->myConfVal.confirmVal); i < confSz ; ++i)
        utilSetLE8(&conf.confirmVal[i], inst->myConfVal.confirmVal[confSz - i - 1]);

    if (!smTx(inst, SM_PAIRING_CONF, &conf, sizeof(conf))) {
        logw("%s(): Failed to send pairing confirm command\n", __func__);
        return false;
    }
    return true;
}

/*
 * FUNCTION: smSendPairRand
 * USE:      Send pairing Random number
 * PARAMS:   inst - the instance of the initiator
 * RETURN:   success
 * NOTES:
 */
static bool smSendPairRand(const struct smInstance *inst)
{
    uint16_t i, randSz;
    struct smPairRand rand;

    for (i = 0, randSz = sizeof(inst->myRandNum.rand); i < randSz ; ++i)
        utilSetLE8(&rand.rand[i], inst->myRandNum.rand[randSz - i - 1]);

    if (!smTx(inst, SM_PAIRING_RAND, &rand, sizeof(rand))) {
        logw("%s(): Failed to send pairing random command\n", __func__);
        return false;
    }
    return true;
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

    smBanDeviceForTime(&inst->peerAddr);
    utilSetLE8(&fail.reason, reason);

    return smTx(inst, SM_PAIRING_FAIL, &fail, sizeof(fail));
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
    if (phase != SM_PHASE_START && phase != SM_PHASE_REQ_SENT &&
                                 phase != SM_PHASE_REQ_RESP && phase != SM_PHASE_CONF_SENT &&
                                 phase != SM_PHASE_CONF_RESP && phase != SM_PHASE_RAND_SENT &&
                                 phase != SM_PHASE_RAND_RESP) {
        logd("Invalid phase to transit, phase 0x%02X\n", phase);
        return;
    }
    if (inst->phase == phase) {
        logd("Phase transition skipped, phase 0x%02X\n", phase);
        return;
    }

    logd("Transit from 0x%02X to 0x%02X\n", inst->phase, phase);
    inst->phase = phase;
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
    uint8_t typ, tmp;

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
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        if (inst->phase != SM_PHASE_START) {
            logd("Got pairing req in phase %u\n", inst->phase);
            break;
        }

        inst->isInitiator = false;
        if (!smParsePairReqResp(inst, &pairReqResp)) {
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }

        if (!smGenRandNum(inst))
            break;
        if (!smGenTK(inst))
            break;
        if (!smGenConfVal(&inst->myConfVal, inst, false))
            break;

        if (!smSendPairResp(inst))
            break;
        smTransitPhase(inst, SM_PHASE_REQ_RESP);

        goto done;

    case SM_PAIRING_RSP:
        if (!sgSerializeCutFront(packet, &pairReqResp, sizeof(pairReqResp))) {
            logw("Incoming SM packet data extraction failed\n");
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        if (inst->phase != SM_PHASE_REQ_SENT|| !inst->isInitiator) {
            logd("Got pairing resp in phase %u when we %s the initiator\n", inst->phase,
                 inst->isInitiator ? "are" : "are not");
            break;
        }

        if (!smParsePairReqResp(inst, &pairReqResp)) {
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }

        if (!smGenRandNum(inst))
            break;
        if (!smGenTK(inst))
            break;
        if (!smGenConfVal(&inst->myConfVal, inst, false))
            break;

        if (!smSendPairConf(inst))
            break;
        smTransitPhase(inst, SM_PHASE_CONF_SENT);

        goto done;

    case SM_PAIRING_CONF:
        if (!sgSerializeCutFront(packet, &pairConf, sizeof(pairConf))) {
            logw("Incoming SM packet data extraction failed\n");
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        if ((inst->isInitiator && inst->phase != SM_PHASE_CONF_SENT) ||
            (!inst->isInitiator && inst->phase != SM_PHASE_REQ_RESP)) {
            logd("Got pairing conf in phase %u when we %s the initiator \n", inst->phase,
                 inst->isInitiator ? "are" : "are not");
            break;
        }

        if (!smReverseBytes(inst->peerConfVal.confirmVal, sizeof(inst->peerConfVal.confirmVal),
                            pairConf.confirmVal, sizeof(pairConf.confirmVal)))
            break;

        if (inst->isInitiator) {
            if (!smSendPairRand(inst))
                break;
            smTransitPhase(inst, SM_PHASE_RAND_SENT);
        } else {
            if (!smSendPairConf(inst))
                break;
            smTransitPhase(inst, SM_PHASE_CONF_RESP);
        }

        goto done;

    case SM_PAIRING_RAND:
        if (!sgSerializeCutFront(packet, &pairRand, sizeof(pairRand))) {
            logw("Incoming SM packet data extraction failed\n");
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        if ((inst->isInitiator && inst->phase != SM_PHASE_RAND_SENT) ||
            (!inst->isInitiator && inst->phase != SM_PHASE_CONF_RESP)) {
            logd("Got pairing rand in phase %u when we %s the initiator \n", inst->phase,
                 inst->isInitiator ? "are" : "are not");
            break;
        }

        if (!smReverseBytes(inst->peerRandNum.rand, sizeof(inst->peerRandNum.rand),
                            pairRand.rand, sizeof(pairRand.rand)))
            break;

        /* check if the peer confirm value generated from the random value matches */
        smGenConfVal(&pairConf, inst, true);
        if (memcmp(pairConf.confirmVal, inst->peerConfVal.confirmVal,
                   sizeof(pairConf.confirmVal)) != 0) {
            smSendPairFail(inst, SM_ERR_CONF_VAL_FAILED);
            break;
        }
        if (!inst->isInitiator && !smSendPairRand(inst))
            break;
        smTransitPhase(inst, SM_PHASE_RAND_RESP);

        if (!smGenSTK(inst))
            break;

        /* TODO(mcchou): if we are the initiator, encrypt the link using the STK */

        goto done;

    case SM_PAIRING_FAIL:
        if (!sgSerializeCutFront(packet, &pairFailed, sizeof(pairFailed))) {
            logw("Incoming SM packet data extraction failed\n");
            break;
        }
        if (sgLength(packet)) {
            logw("Residual data in packet: %u\n", sgLength(packet));
            smSendPairFail(inst, SM_ERR_INVALID_PARAMS);
            break;
        }

        smTransitPhase(inst, SM_PHASE_START);
        smBanDeviceForTime(&inst->peerAddr);

        goto done;

    case SM_ENCR_INFO:
    case SM_MASTER_INFO:
    case SM_IDENTITY_INFO:
    case SM_IDENTITY_ADDR_INFO:
    case SM_SIGNING_INFO:
    case SM_SECURITY_REQUEST:
        logw("Unhandled packet type 0x%02X in SM. Dropping\n", typ);
        goto done;
    default:
        logw("Unxpected packet type 0x%02X in SM. Dropping\n", typ);
        break;
    }

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
}

/*
 * FUNCTION: smInstFree
 * USE:      Free a per-connection instance struct
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
 */
static void smInstFree(struct smInstance *inst)
{
    if (inst->stallTimer)
        timerCancel(inst->stallTimer);
    free(inst);
}

/*
 * FUNCTION: smInstDeinit
 * USE:      Delete an instance from the active instance list and free it
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
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

    smInstFree(inst);
    pthread_mutex_unlock(&mInstLock);
}

/*
 * FUNCTION: smInstInit
 * USE:      Set some default values for pairing phase and repeated attempt field of an instance
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
 */
static void smInstInit(struct smInstance *inst)
{
    pthread_mutex_lock(&mInstLock);

    inst->prev = NULL;
    inst->next = mActInstList;
    if (mActInstList)
        mActInstList->prev = inst;
    mActInstList = inst;

    inst->phase = SM_PHASE_START;
    /* TODO(mcchou): the stall timer needs to be canceled when the pairing finishes correctly */
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
    l2c_handle_t conn;
    sg s;

    switch (evt) {
    case L2C_STATE_OPEN:
        if (len != sizeof(l2c_handle_t)) {
            loge("invalid length for open event\n");
            break;
        }
        conn = *(l2c_handle_t*)data;
        /* when we support outbound connections, this code will need to evolve */
        if (inst->conn != conn)
            loge("Handle change!!!\n");

        smInstInit(inst);

        break;

    case L2C_STATE_ENCR:
        if (len != sizeof(struct l2cEncrState)) {
            loge("invalid length for encr event\n");
            break;
        }
        smEncrChanged(inst);
        break;

    case L2C_STATE_RX:
        if (len != sizeof(sg)) {
            loge("invalid length for RX event\n");
            break;
        }
        s = *(sg*)data;
        smRx(inst, s);
        break;

    case L2C_STATE_CLOSED:
        smInstDeinit(inst);
        break;
    default:
        logd("unknown L2C event %u\n", evt);
        break;
    }
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
        smBanDeviceForTime(&peerAddr);
        return SVC_ALLOC_FAIL_OTHER;
    }

    inst = (struct smInstance*)calloc(1, sizeof(struct smInstance));
    if (!inst)
        return SVC_ALLOC_FAIL_OTHER;

    memcpy(&inst->peerAddr, &peerAddr, sizeof(struct bt_addr));
    memcpy(&inst->myAddr, &myAddr, sizeof(struct bt_addr));

    /* TODO(mcchou): default settings. we need to provide ATT a way to set authentication requirement for a certain instance */
    inst->myAuthReq = SM_AUTH_REQ_BOND_BOND | SM_AUTH_REQ_MITM_FLAG;
    inst->myMaxKeySz = SM_MAX_KEY_LEN;
    inst->myKeyDistr = SM_KEY_DISTR_LTK | SM_KEY_DISTR_IRK | SM_KEY_DISTR_CSRK;

    inst->conn = conn;
    *instanceP = inst;

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
    smBannedDevFreeAll();
    sendQueueFree(mSmSendQ);
    l2cApiServiceFixedChUnregister(L2C_FIXED_CH_NUM_SM, NULL, NULL);
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
static bool smPrepInitRspAddr(uint64_t *a, bool *at, const struct bt_addr *addr)
{
    int i;
    uint8_t type;

    if (!addr)
        return false;

    *at = addr->type != BT_ADDR_TYPE_EDR && addr->type != BT_ADDR_TYPE_LE_PUBLIC;
    for (i = sizeof(addr->addr) - 1, *a = 0; i >= 0; --i)
        *a = (*a << 8) | addr->addr[i]; /* addr is stored in [0]->[5] : LSO -> MSO */

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
    uint8_t pairAlg;

    if ((pairAlg = smSelPairAlg(inst)) == SM_ALG_UNKNOWN)
        return false;

    logd("Pairing method chosen: 0x%02X\n", pairAlg);

    /* generate TK */
    switch(pairAlg) {
    case SM_ALG_JUST_WORK:
        memset(inst->tk.key, 0, sizeof(inst->tk.key));
        break;
    case SM_ALG_PASS_KEY: /* TODO(mcchou): retrieve passkey from user */
    case SM_ALG_OOB:      /* TODO(mcchou): retrieve OOB data from OOB source */
        logw("%s(): Not implemented\n", __func__);
        return false;
    default:
        logw("%s(): Unknown pairing algorithm %d", __func__, pairAlg);
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
    bool iat = false, rat = false;
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
 * USE:      Generate a 128-bit random number and store it in the given instance
 * PARAS:    inst - instance
 * RETURN:   success
 * NOTES:
 */
static bool smGenRandNum(struct smInstance *inst)
{
    int r = 0;
    uint16_t i, rSz, cp, rem;
    struct timespec ts;

    if (!inst) {
        logw("%s(): Invalid instance\n", __func__);
        return false;
    }

    if (!timespec_get(&ts, TIME_UTC)) {
        logd("%s(): Filed to retrieve time\n", __func__);
        return false;
    }

    srandom(ts.tv_nsec ^ ts.tv_sec);

    for (i = 0, rSz = sizeof(r), rem = sizeof(inst->myRandNum); rem > 0; i += cp, rem -= cp) {
        r = random();
        cp = rem > rSz ? rSz : rem;
        memcpy(&inst->myRandNum.rand[i], &r, cp);
    }

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
 * NOTES:
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
