#include "persist.h"
#include "l2cap.h"
#include "util.h"
#include "log.h"
#include "sm.h"
#include "sg.h"
#include "bt.h"
#include "sendQ.h"
#include "config.h"

#include <stdlib.h>

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
    uint8_t confirmVal[16];
} __packed;

struct smPairRand {
    uint8_t rand[16];
} __packed;

struct smPairFailed {
    uint8_t reason; /* SM_ERR_* */
} __packed;

struct smKey {
    uint16_t key[16];
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
#define SM_ERR_PASSKEY_ENTRY_FAILED     0x01 /* user cancelled or faile dto input key */
#define SM_ERR_OOB_NOT_AVAIL            0x02 /* no OOB data */
#define SM_ERR_AUTH_REQMENTS            0x03 /* auth req'ments cannot be met due to IO caps */
#define SM_ERR_CONF_VAL_FAILED          0x04 /* confirm value does not match calculated value */
#define SM_ERR_PAIRING_NOT_SUPPORTED    0x05 /* pairing not supported by this device */
#define SM_ERR_ENCR_KEY_SZ              0x06 /* resulting key size is not long enough */
#define SM_ERR_CMD_NOT_SUPP             0x07 /* received command not supported by this device */
#define SM_ERR_UNSPECCED_REASON         0x08 /* pairing faield due to an unspecified reason */
#define SM_ERR_REPEATED_ATTEMPTS        0x09 /* too rapid an attempt to pair after a failure */
#define SM_ERR_INVALID_PARAMS           0x0A /* unparseable command or invalid params in command */

/* things for authReq fields */
#define SM_AUTH_REQ_BOND_MASK           0x03
#define SM_AUTH_REQ_BOND_NO             0x00
#define SM_AUTH_REQ_BOND_BOND           0x01
#define SM_AUTH_REQ_MITM_FLAG           0x04

/* things for key distr fields */
#define SM_KEY_DISTR_LTK                0x01
#define SM_KEY_DISTR_IRK                0x02
#define SM_KEY_DISTR_CSRK               0x04

/* misc defines */
#define SM_MIN_KEY_LEN                  7
#define SM_MAX_KEY_LEN                  16



/* our structs/defines */

#define SM_PHASE_START                  0 /* nothing happened yet */
#define SM_PHASE_REQ_SENT               1 /* we sent pair req, waiting for pair resp */
#define SM_PHASE_REQ_RESP               2 /* other side has sent us a pair request, we replied, or we sent req and got reply */

struct smInstance {
    struct bt_addr        peer;
    l2c_handle_t          conn;
    bool                  isInitiator;

    uint8_t               phase;

    uint8_t               peerIoCaps;
    bool                  peerHasOob;
    uint8_t               peerAuthReq;
    uint8_t               peerMaxKeySz;
    uint8_t               peerKeyDistr;
    uint8_t               myKeyDistr;
};




/* state */
static uint8_t mIoCap;


/* fwd decls */
static void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);

/* sendQ for packet transmissions */
static struct sendQ *mSmSendQ;






/*
 * FUNCTION: smEncrChanged
 * USE:      Encryption changed - handle it
 * PARAMS:   inst - the instance
 * RETURN:   NONE
 * NOTES:
 */
static void smEncrChanged(struct smInstance *inst)
{
    //TODO
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
static bool smTx(struct smInstance *inst, uint8_t pktTyp, const void *data, uint8_t len)
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
 * FUNCTION: smTxErr
 * USE:      Send an error to the peer
 * PARAMS:   inst - the instance
 *           reason - the error reason
 * RETURN:   success
 * NOTES:
 */
static bool smTxErr(struct smInstance *inst, uint8_t reason)
{
    struct smPairFailed fail;

    utilSetLE8(&fail.reason, reason);

    return smTx(inst, SM_PAIRING_FAIL, &fail, sizeof(fail));
}

/*
 * FUNCTION: smParsePairReqRest
 * USE:      Parse a struct smPairReqResp into relevant instance variables
 * PARAMS:   inst - the instance
 *           pairReqResp - the struct
 * RETURN:   true if all values were read & found valid
 * NOTES:
 */
static bool smParsePairReqRest(struct smInstance *inst, const struct smPairReqResp *pairReqResp)
{
    uint8_t tmp, initKeyDistr, rspKeyDistr;

    tmp = utilGetLE8(&pairReqResp->ioCap);
    if (tmp != HCI_DISP_CAP_DISP_ONLY && tmp != HCI_DISP_CAP_DISP_YES_NO && tmp != HCI_DISP_CAP_KBD_ONLY && tmp != HCI_DISP_CAP_NONE &&
        tmp != HCI_DISP_CAP_KBD_DISP) {
        logd("Invalid io caps RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerIoCaps = tmp;

    tmp = utilGetLE8(&pairReqResp->oobDataFlag);
    if (tmp != 0 && tmp != 1) {
        logd("Invalid oob flag RXed 0x%02X\n", tmp);
        return false;
    }
    inst->peerHasOob = !!tmp;

    tmp = utilGetLE8(&pairReqResp->authReq);
    if ((tmp & ~(SM_AUTH_REQ_BOND_MASK | SM_AUTH_REQ_MITM_FLAG)) || ((tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_NO && (tmp & SM_AUTH_REQ_BOND_MASK) != SM_AUTH_REQ_BOND_BOND)) {
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

    if (inst->isInitiator) { /* must be a response. do not send any keys we did not already commit to send. accept as many as given */
        inst->myKeyDistr &= initKeyDistr;
        inst->peerKeyDistr = rspKeyDistr;
    } else { /* must be a request - agree to send all keys they want, aceept whatever was given */
        inst->myKeyDistr = rspKeyDistr;
        inst->peerKeyDistr = initKeyDistr;
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
    struct smPairReqResp pairReqResp;
    struct smHdr hdr;
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
            logw("Residual data in packet: %ub\n", sgLength(packet));
            smTxErr(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        if (inst->phase != SM_PHASE_START) {
            logd("Got pairing req in phase %u\n", inst->phase);
            break;
        }
        if (!smParsePairReqRest(inst, &pairReqResp)) {
            smTxErr(inst, SM_ERR_INVALID_PARAMS);
            break;
        }
        utilSetLE8(&pairReqResp.ioCap, mIoCap);
        utilSetLE8(&pairReqResp.oobDataFlag, 0) ; /* XXX: if and when we support OOB, this will need to change */
        utilSetLE8(&pairReqResp.authReq, SM_AUTH_REQ_BOND_BOND | SM_AUTH_REQ_MITM_FLAG); /* TODO: we'll need a way to set this externally! */
        utilSetLE8(&pairReqResp.maxKeySz, SM_MAX_KEY_LEN);
        utilSetLE8(&pairReqResp.initiatorKeyDistr, inst->peerKeyDistr); /* accept whatever they promised us */
        utilSetLE8(&pairReqResp.responderKeyDistr, inst->myKeyDistr); /* give up ahwtever they asked for */
        if (smTx(inst, SM_PAIRING_RSP, &pairReqResp, sizeof(pairReqResp)))
            inst->phase = SM_PHASE_REQ_RESP;
        else
            inst->phase = SM_PHASE_START;
        break;
 
    case SM_PAIRING_RSP:
    case SM_PAIRING_CONF:
    case SM_PAIRING_RAND:
    case SM_PAIRING_FAIL:
    case SM_ENCR_INFO:
    case SM_MASTER_INFO:
    case SM_IDENTITY_INFO:
    case SM_IDENTITY_ADDR_INFO:
    case SM_SIGNING_INFO:
    case SM_SECURITY_REQUEST:
    default:
        logw("Unxpected packet type 0x%02X in SM. Dropping\n", typ);
        break;
    }

    sgFree(packet);
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
    free(inst);
}

/*
 * FUNCTION: smFixedChState
 * USE:      Connection event happened
 * PARAMS:   userData - unused
 *           instance - per-connection insdtance allocated by smFixedChAlloc()
 *           evt - what happened
 *           data - the pertinent data
 *           len - length of said data
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static void smFixedChState(void *userData, void *instance, uint8_t evt, const void *data, uint32_t len)
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
        if (inst->conn != conn) /* when we support outbound connections, this code will need to evolve */
            loge("Handle change!!!\n");
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
        smInstFree(inst);
        break;
    default:
        logd("unknown L2C event %u\n", evt);
        break;
    }
}

/*
 * FUNCTION: smFixedChAlloc
 * USE:      Connection request: if accepted, alloc a per-conection instance of the SM connection structure
 * PARAMS:   userData - unused
 *           conn - l2c conn handle
 *           instanceP - we store instance here if we allocate one
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t smFixedChAlloc(void *userData, l2c_handle_t conn, void **instanceP)
{
    struct smInstance *inst;
    struct bt_addr peer;

    if (!l2cApiGetBtAddr(conn, &peer)) {
        loge("Failed to get peer address\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    if (!BT_ADDR_IS_LE(peer)) {
        logd("Refusing SM connection for EDR\n");
        return SVC_ALLOC_FAIL_OTHER;
    }

    inst = (struct smInstance*)calloc(1, sizeof(struct smInstance));
    if (!inst)
        return SVC_ALLOC_FAIL_OTHER;

    memcpy(&inst->peer, &peer, sizeof(struct bt_addr));
    inst->conn = conn;
    *instanceP = inst;

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

    mIoCap = ioCapability;
    if (!(mSmSendQ = sendQueueAlloc(MAX_PACKETS_SM_SEND_QUEUE))) {
        logw("%s(): Error allocating SM sendQ.\n", __func__);
        return false;
    }

    if(!l2cApiServiceFixedChRegister(L2C_FIXED_CH_NUM_SM, &descr)) {
        logw("%s(): Error registering L2CAP service.\n", __func__);
        sendQueueFree(mSmSendQ);
        return false;
    }

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
static void smCmacDo(uint8_t *dst, uint8_t macLen, sg m, const uint8_t *k, const uint8_t *k1, const uint8_t *k2)
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
static void smCalcKey(uint8_t *out, const uint8_t *k, const uint8_t *r1, const uint8_t *r2)
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
static void smCalcConfVal(uint8_t* out, const uint8_t *k, const uint8_t *r, uint64_t pres, uint64_t preq, bool iat, uint64_t ia, bool rat, uint64_t ra)
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

/* a simple AES-128 implementation - we use it rarely enough that even an unoptimal version will do */

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
    static const uint8_t rcon[] = {1,0,0,0, 2,0,0,0, 4,0,0,0, 8,0,0,0, 16,0,0,0, 32,0,0,0, 64,0,0,0, 128,0,0,0, 27,0,0,0, 54,0,0,0};
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
 * PARAMS:   dst - palce to store result (SM_ENCRYPT_DATA_LEN bytes)
 *           src - the plaintext (SM_ENCRYPT_DATA_LEN bytes)
 *           key - the key (SM_ENCRYPT_DATA_LEN bytes)
 * RETURN:   NONE
 * NOTES:
 */
static void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key)
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
