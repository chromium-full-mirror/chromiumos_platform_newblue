#include <string.h>
#include <stdlib.h>
#include "l2cap.h"
#include "sendQ.h"
#include "uniq.h"
#include "util.h"
#include "sdp.h"
#include "log.h"
#include "sg.h"
#include "mt.h"

#define SDP_SDP_SERVER_UUID       0x1000

#define SDP_PSM                   0x0001

#define SDP_HANDLE_SELF           0
#define SDP_HANDLE_FIRST          0x00010000
#define SDP_HANDLE_LAST           0xFFFFFFFF
#define SDP_MAX_UUIDS_IN_SEARCH   12

#define SDP_PDU_Error_Response                      1
#define SDP_PDU_Service_Search_Request              2
#define SDP_PDU_Service_Search_Response             3
#define SDP_PDU_Service_Attribute_Request           4
#define SDP_PDU_Service_Attribute_Response          5
#define SDP_PDU_Service_Search_Attribute_Request    6
#define SDP_PDU_Service_Search_Attribute_Response   7

#define SDP_ERR_Invalid_SDP_Version                 0x0001
#define SDP_ERR_Invalid_Service_Record_Handle       0x0002
#define SDP_ERR_Invalid_Request_Syntax              0x0003
#define SDP_ERR_Invalid_PDU_Size                    0x0004
#define SDP_ERR_Invalid_Continuation_State          0x0005
#define SDP_ERR_Insufficient_Resources              0x0006



struct sdpService {
    struct sdpService          *prev;
    struct sdpService          *next;
    uint32_t                    handle; /* will match what's in descr, but quicker to read it here */
    uint32_t                    len;
    uint8_t                     descr[];
};

struct sdpSearchResultNode {
    struct sdpSearchResultNode *next;
    struct sdpService          *ss;
};

struct sdpInst {
    l2c_handle_t                conn;
    uint16_t                    mtu;

    uniq_t                      contVal;
    sg                          contResult;
    union {
        uint16_t                numHandles;
    };
};

struct uuid {
    uint64_t hi;
    uint64_t lo;
};

struct sdpPdu {
    uint8_t  pduType;
    uint16_t transactionID;
    uint16_t paramLen;
} __packed;

struct sdpPduSSRepl {
    uint16_t totalHandles;
    uint16_t numHandles;
} __packed;

struct sdpCont {
    uint8_t  len;
    uniq_t   contVal;
} __packed;

static const struct uuid sdpBaseUuid = {0x0000000000001000ULL, 0x800000805F9B34FBULL};

/* our global state */

static pthread_rwlock_t mSvcsLock = PTHREAD_RWLOCK_INITIALIZER;
static struct sdpService *mSvcsHead = NULL;
static struct sdpService *mSvcsTail = NULL;
static uint32_t mNextHandle;
static struct sendQ *mSendQ;


/*
 * FUNCTION: sdpUuidEq
 * USE:      Check two uuids to see if they are equal
 * PARAMS:   lhs - one of the UUIDs to compare
 *           rhs - another UUID to compare
 * RETURN:   true if they are equal
 * NOTES:
 */
static bool sdpUuidEq(const struct uuid *lhs, const struct uuid *rhs)
{
    return lhs->hi == rhs->hi && lhs->lo == rhs->lo;
}

/*
 * FUNCTION: sdpIntToUUID
 * USE:      Converts a SDP short UUID to a full UUID
 * PARAMS:   dst - where to store the result
 *           intVal - the SDP short uuid (16 or 32 bit)
 * RETURN:   NONE
 * NOTES:
 */
static void sdpIntToUUID(struct uuid* dst, uint32_t intVal)
{
    dst->hi = sdpBaseUuid.hi + (((uint64_t)intVal) << 32);
    dst->lo = sdpBaseUuid.lo;
}

/*
 * FUNCTION: sdpServiceFindByHandle
 * USE:      Find a service struct by handle
 * PARAMS:   handle - the handle
 * RETURN:   service struct or NULL
 * NOTES:    call with mSvcsLock held for read
 */
static struct sdpService* sdpServiceFindByHandle(uint32_t handle)
{
    struct sdpService *ss;

    for (ss = mSvcsHead; ss; ss = ss->next)
        if (ss->handle == handle)
            return ss;
    return NULL;
}

/*
 * FUNCTION: sdpServiceDescriptorDelInt
 * USE:      Delete a service struct
 * PARAMS:   ss - the service struct to delete
 * RETURN:   NONE
 * NOTES:    call with mSvcsLock held for write
 */
static void sdpServiceDescriptorDelInt(struct sdpService *ss)
{
    if (ss->prev)
        ss->prev->next = ss->next;
    else
        mSvcsHead = ss->next;

    if (ss->next)
        ss->next->prev = ss->prev;
    else
        mSvcsTail = ss->prev;

    free(ss);
}

/*
 * FUNCTION: sdpServiceDescriptorAddInt
 * USE:      Called to register a service with SDP with a given handle
 * PARAMS:   svc - the service descriptor
 *           len - the descriptor length
 *           handle - the handle to use
 * RETURN:   success
 * NOTES:    call with mSvcsLock held for write
 */
static bool sdpServiceDescriptorAddInt(const void *svc, uint32_t len, uint32_t handle)
{
    static const unsigned handle_attr_len = 8;
    struct sdpService *ss;

    ss = (struct sdpService*)malloc(sizeof(struct sdpService) + len + handle_attr_len);
    if (!ss)
        return false;

    memcpy(ss->descr + handle_attr_len, svc, len);
    ss->len = len + handle_attr_len;
    utilSetBE8(ss->descr + 0, SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2));
    utilSetBE16(ss->descr + 1, SDP_ATTR_HANDLE);
    utilSetBE8(ss->descr + 3, SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_4));
    utilSetBE32(ss->descr + 4, handle);

    ss->handle = handle;
    ss->next = NULL;
    ss->prev = mSvcsTail;
    if (mSvcsTail)
        mSvcsTail->next = ss;
    else
        mSvcsHead = ss;

    mSvcsTail = ss;

    return true;
}

/*
 * FUNCTION: sdpFindHandle
 * USE:      Find a free SDP handle
 * PARAMS:   NONE
 * RETURN:   handle ot 0 on failure
 * NOTES:    call with mSvcsLock held for read
 */
static uint32_t sdpFindHandle(void)
{
    uint32_t handle, origNextHandleVal = mNextHandle;
    struct sdpService *t;

    /* repeatedly guess a good handle value and check it for uniqueness */
    do {
        handle = mNextHandle;
        mNextHandle = (mNextHandle == SDP_HANDLE_LAST) ? SDP_HANDLE_FIRST : mNextHandle + 1;
        for (t = mSvcsHead; t && t->handle != handle; t = t->next);
        /* until success or we come a full circle */
    } while (t && mNextHandle != origNextHandleVal);

    return t ? 0 : handle;
}

/*
 * FUNCTION: sdpServiceDescriptorAdd
 * USE:      Called to register a service with SDP
 * PARAMS:   svc - the service descriptor
 *           len - its length
 * RETURN:   success
 * NOTES:
 */
uint32_t sdpServiceDescriptorAdd(const void *svc, uint32_t len)
{
    uint32_t handle;

    pthread_rwlock_wrlock(&mSvcsLock);
    handle = sdpFindHandle();
    if (handle && !sdpServiceDescriptorAddInt(svc, len, handle)) {
        mNextHandle = handle; /* do not let the search go to waste */
        handle = 0;
    }
    pthread_rwlock_unlock(&mSvcsLock);
    return handle;
}

/*
 * FUNCTION: sdpServiceDescriptorDel
 * USE:      Called to unregister a service with SDP
 * PARAMS:   handle - the service handle from sdpServiceDescriptorAdd()
 * RETURN:   success
 * NOTES:
 */
bool sdpServiceDescriptorDel(uint32_t handle)
{
    struct sdpService *ss;
    bool ret;

    if (!handle) {
        loge("Attempt to remove SDP's SDP descriptor externally\n");
        return false;
    }

    pthread_rwlock_wrlock(&mSvcsLock);
    ss = sdpServiceFindByHandle(handle);
    if (ss){
        sdpServiceDescriptorDelInt(ss);
        ret = true;
    }
    pthread_rwlock_unlock(&mSvcsLock);
    return ret;
}

/*
 * FUNCTION: sdpSend
 * USE:      Enqueue a packet to be sent to the other side
 * PARAMS:   inst - the instance
 *           s - the packet
 * RETURN:   false on error
 * NOTES:
 */
static bool sdpSend(struct sdpInst *inst, sg s)
{
#ifdef SDP_DBG
    char line[128] = "SDP TX <ZLP>";
    uint32_t i;
    uint8_t v;

    for(i = 0; i < sgLength(s); i++) {
        sgSerialize(s, i, 1, &v);
        if (i & 15)
            sprintf(line + strlen(line), " %02X", v);
        else {
            if (i)
                logd("%s\n", line);
            sprintf(line, "SDP TX [%03X] %02X", i, v);
        }
    }
    logd("%s\n", line);
#endif
    return sendQueueTx(mSendQ, inst->conn, s);
}

/*
 * FUNCTION: sdpSendError
 * USE:      Send an error packet if at all possible
 * PARAMS:   inst - the instance
 *           transact - the transaction ID
 *           err - the error
 * RETURN:   false on error
 * NOTES:
 */
static bool sdpSendError(struct sdpInst *inst, uint16_t transact, uint16_t err)
{
    struct sdpPdu pduHdr;
    sg rsg;

    utilSetBE8(&pduHdr.pduType, SDP_PDU_Error_Response);
    utilSetBE16(&pduHdr.transactionID, transact);
    utilSetBE16(&pduHdr.paramLen, sizeof(uint16_t));
    utilSetBE16(&err, err);

    rsg = sgNew();
    if (!rsg)
        return false;

    if (sgConcatBackCopy(rsg, &pduHdr, sizeof(pduHdr))  && sgConcatBackCopy(rsg, &err, sizeof(err))  && sdpSend(inst, rsg))
        return true;

    sgFree(rsg);
    return false;
}

/*
 * FUNCTION: sdpGetSearchReq
 * USE:      Read a list of UUIDs from an SG
 * PARAMS:   req - the SG to read from
 *           uuids - where to store UUIDs (array of at least SDP_MAX_UUIDS_IN_SEARCH)
 *           numUuidsP - where to stor enumber of UUIDs
 * RETURN:   error code to return or 0 for success
 * NOTES:
 */
static uint16_t sdpGetSearchReq(sg req, struct uuid *uuids, uint8_t *numUuidsP)
{
    uint8_t buf[16];
    uint8_t t, numUuids = 0;
    uint32_t len;

    if (!sgSerializeCutFront(req, buf, 1)) /* must have at least a container header */
        return SDP_ERR_Invalid_Request_Syntax;
    t = utilGetBE8(buf);

    if (SDP_ITEM_GET_TYPE(t) != SDP_TYPE_ARRAY)
        return SDP_ERR_Invalid_Request_Syntax;

    switch (SDP_ITEM_GET_SIZE(t)) {
    case SDP_SZ_u8:
        if (!sgSerializeCutFront(req, buf, 1))
            return SDP_ERR_Invalid_Request_Syntax;
        len = utilGetBE8(buf);
        break;
    case SDP_SZ_u16:
        if (!sgSerializeCutFront(req, buf, 2))
            return SDP_ERR_Invalid_Request_Syntax;
        len = utilGetBE16(buf);
        break;
    case SDP_SZ_u32:
        if (!sgSerializeCutFront(req, buf, 4))
            return SDP_ERR_Invalid_Request_Syntax;
        len = utilGetBE32(buf);
        break;
    default:
        return SDP_ERR_Invalid_Request_Syntax;
    }
    if (sgLength(req) < len)
        return SDP_ERR_Invalid_Request_Syntax;

    len = sgLength(req) - len; /* how many bytes we expect to leave */

    while (sgLength(req) > len && numUuids < SDP_MAX_UUIDS_IN_SEARCH) {
        if (!sgSerializeCutFront(req, buf, 1))
            return SDP_ERR_Invalid_Request_Syntax;
        t = utilGetBE8(buf);

        if (SDP_ITEM_GET_TYPE(t) != SDP_TYPE_UUID)
            return SDP_ERR_Invalid_Request_Syntax;
        switch (SDP_ITEM_GET_SIZE(t)) {
        case SDP_SZ_2:
            if (!sgSerializeCutFront(req, buf, 2))
                return SDP_ERR_Invalid_Request_Syntax;
            sdpIntToUUID(uuids + numUuids++, utilGetBE16(buf));
            break;
        case SDP_SZ_4:
            if (!sgSerializeCutFront(req, buf, 4))
                return SDP_ERR_Invalid_Request_Syntax;
            sdpIntToUUID(uuids + numUuids++, utilGetBE32(buf));
            break;
        case SDP_SZ_16:
            if (!sgSerializeCutFront(req, buf, 16))
                return SDP_ERR_Invalid_Request_Syntax;
            uuids[numUuids].hi = utilGetBE64(buf + 0);
            uuids[numUuids].lo = utilGetBE64(buf + 8);
            numUuids++;
            break;
        default:
            return SDP_ERR_Invalid_Request_Syntax;
        }
    }

    if (sgLength(req) != len)
        return SDP_ERR_Invalid_Request_Syntax;

    *numUuidsP = numUuids;
    return 0;
}

/*
 * FUNCTION: sdpGetCont
 * USE:      Read a continuation state in our format from an SG
 * PARAMS:   req - the SG to read from
 *           ofst - offset in the SG to read from
 *           trunc - truncate the cont away? (only available if ofst == 0)
 *           contP - where to store the state (or 0 if none is present in a valid way)
 * RETURN:   error code to return or 0 for success
 * NOTES:
 */
static uint16_t sdpGetCont(sg req, uint32_t ofst, bool trunc, uniq_t *contP)
{
    uint8_t len;

    if (ofst && trunc) {
        loge("Cannot truncate cont not at start of req\n");
        trunc = false;
    }

    if (sizeof(uint8_t) != sgSerialize(req, ofst++, sizeof(uint8_t), &len))
        return SDP_ERR_Invalid_Request_Syntax;
    len = utilGetBE8(&len);

    if (trunc)
        sgTruncFront(req, sizeof(uint8_t));

    if (!len) {
        *contP = 0;
        return 0;
    }

    if (len != sizeof(uniq_t))
        return SDP_ERR_Invalid_Request_Syntax;

    /* we use our endianness here - other side assumes cont state is opaque */
    if (sizeof(uniq_t) != sgSerialize(req, ofst, sizeof(uniq_t), contP))
        return SDP_ERR_Invalid_Request_Syntax;

    if (trunc)
        sgTruncFront(req, sizeof(uniq_t));

    return 0;
}

/*
 * FUNCTION: sdpAppendCont
 * USE:      Append a continuation state in our format to an SG
 * PARAMS:   inst - the instance
 *           req - the SG to append it to
 * RETURN:   false on error
 * NOTES:
 */
static bool sdpAppendCont(struct sdpInst *inst, sg req)
{
    uint8_t buf[sizeof(uniq_t) + 1] = {0,};

    if (!inst->contResult)
        return sgConcatBackCopy(req, buf, 1);

    buf[0] = sizeof(uniq_t);
    inst->contVal = uniqGetNext();
    /* we use our endianness here - other side assumes cont state is opaque */
    memcpy(buf + 1, &inst->contVal, sizeof(uniq_t));
    return sgConcatBackCopy(req, buf, sizeof(buf));
}

/*
 * FUNCTION: sdpDoSearch
 * USE:      Perform a UUID-based search
 * PARAMS:   inst - the instance
 *           uuids - the uuids we want to find
 *           numUuids - how mnay there are
 *           maxRes - do not return more than this many results (negative for no maximum)
 *           resP - put head of result linked list here
 * RETURN:   error to return to other size or 0 if none
 * NOTES:    call with mSvcsLock held for read
 */
static uint16_t sdpDoSearch(struct sdpInst *inst, const struct uuid *uuids, uint8_t numUuids, int32_t maxRes, struct sdpSearchResultNode **resP)
{
    const uint16_t wantMask = (1 << numUuids) - 1;
    struct sdpSearchResultNode *res = NULL;
    struct sdpService *ss;
    uint8_t i;

    for (ss = mSvcsTail; ss; ss = ss->prev) { /* in reverse since we also create th elinked list in reverse  - they balance out */
        const uint8_t *ptr = ss->descr, *end = ptr + ss->len;
        uint16_t haveMask = 0;
        struct uuid uuid = {0,};
        uint32_t sz;
        uint8_t t;

        while (ptr < end && haveMask != wantMask) {
            t = *ptr++;

            /* handle NIL gracefully */
            if (SDP_ITEM_GET_TYPE(t) == SDP_TYPE_NIL && SDP_ITEM_GET_SIZE(t) == SDP_SZ_NIL)
                continue;

            switch (SDP_ITEM_GET_SIZE(t)) {
            case SDP_SZ_1:
                sz = 1;
                break;
            case SDP_SZ_2:
                sz = 2;
                break;
            case SDP_SZ_4:
                sz = 4;
                break;
            case SDP_SZ_8:
                sz = 8;
                break;
            case SDP_SZ_16:
                sz = 16;
                break;
            case SDP_SZ_u8:
                sz = utilGetBE8(ptr);
                ptr += 1;
                break;
            case SDP_SZ_u16:
                sz = utilGetBE16(ptr);
                ptr += 2;
                break;
            case SDP_SZ_u32:
                sz = utilGetBE32(ptr);
                ptr += 4;
                break;
            default:
                sz = 0;
                loge("unknown size mark in 0x%02X\n", t);
                break;
            }

            if (SDP_ITEM_GET_TYPE(t) == SDP_TYPE_UUID) { /* uuid? - check for match */
                if (sz == 2)
                    sdpIntToUUID(&uuid, utilGetBE16(ptr));
                else if (sz == 4)
                    sdpIntToUUID(&uuid, utilGetBE32(ptr));
                else if (sz == 16) {
                    uuid.hi = utilGetBE64(ptr + 0);
                    uuid.lo = utilGetBE64(ptr + 8);
                } else
                    loge("weird uuid sz %ud\n", sz);

                for (i = 0; i < numUuids; i++)
                    if (sdpUuidEq(&uuid, uuids + i))
                        haveMask |= 1 << i;
            } else if (SDP_ITEM_GET_TYPE(t) == SDP_TYPE_ARRAY || SDP_ITEM_GET_TYPE(t) == SDP_TYPE_OR_LIST) /* list? - look inside */
                sz = 0;

            ptr += sz;
        }

        if (haveMask == wantMask) { /* we have a match */
            if (!maxRes--)
                break;

            struct sdpSearchResultNode *rn = (struct sdpSearchResultNode*)malloc(sizeof(struct sdpSearchResultNode));
            if (rn) {
                rn->next = res;
                res = rn;
                rn->ss = ss;
            }
        }
    }
    *resP = res;
    return 0;
}

/*
 * FUNCTION: sdpSSR
 * USE:      Handle an incoming service search request
 * PARAMS:   inst - the instance
 *           transact - the transaction ID
 *           req - the request
 * RETURN:   false on error
 * NOTES:    call with mSvcsLock held for read
 */
static bool sdpSSR(struct sdpInst *inst, uint16_t transact, sg req)
{
    struct sdpSearchResultNode *searchRes = NULL, *t;
    struct uuid searchReq[SDP_MAX_UUIDS_IN_SEARCH];
    uint16_t err, maxHandles;
    uint32_t sendSz;
    struct sdpPduSSRepl repl;
    struct sdpPdu pduHdr;
    uint8_t searchReqNum;
    uniq_t cont;
    sg rsg, end;

    err = sdpGetSearchReq(req, searchReq, &searchReqNum);
    if (err)
        return sdpSendError(inst, transact, err);

    logd("SSR query has %u UUIDS\n", searchReqNum);

    if (!searchReqNum)
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    if (!sgSerializeCutFront(req, &maxHandles, sizeof(maxHandles)))
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    maxHandles = utilGetBE16(&maxHandles);

    logd("SSR max handles: %u\n", maxHandles);

    if (!maxHandles)
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    err = sdpGetCont(req, 0, true, &cont);
    if (err)
        return sdpSendError(inst, transact, err);

    if (sgLength(req))
        logw("SDP SSR had %u extra bytes\n", sgLength(req));

    if (cont && cont != inst->contVal) {
        logw("SDP SSR had cont we didn't expect\n");
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
    }
    if (!cont && inst->contVal) {
        logi("dropping previous SDP cont\n");
        sgFree(inst->contResult);
        inst->contVal = 0;
    }

    if (!cont) { /* new request - perform the actual search */

        err = sdpDoSearch(inst, searchReq, searchReqNum, (uint32_t)maxHandles, &searchRes);
        if (err)
            return sdpSendError(inst, transact, err);

        inst->contResult = sgNew();
        if (!inst->contResult)
            return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);

        err = 0;
        while (searchRes) {
            uint32_t handle;

            t = searchRes;
            searchRes = searchRes->next;
            handle = t->ss->handle;
            utilSetBE32(&handle, handle);
            if (!sgConcatBackCopy(inst->contResult, &handle, sizeof(handle)))
                err = SDP_ERR_Insufficient_Resources;
            free(t);
        }
        if (err)
            return sdpSendError(inst, transact, err);
    }

    /* see how much data we can send */
    sendSz = inst->mtu;
    if (inst->mtu < sizeof(struct sdpPdu) + sizeof(struct sdpPduSSRepl) + sizeof(struct sdpCont) + sizeof(uint32_t)) /* too small an MTU */
        return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);
    sendSz = inst->mtu - (sizeof(struct sdpPdu) + sizeof(struct sdpPduSSRepl) + sizeof(struct sdpCont));

    /* decide how much to actually send */
    sendSz = (sendSz / sizeof(uint32_t)); /* no half-handles can be sent */
    if (sgLength(inst->contResult) < sendSz)
        sendSz = sgLength(inst->contResult);

    /* craft the headers */
    utilSetBE16(&repl.totalHandles, inst->numHandles);
    utilSetBE16(&repl.numHandles, sendSz / sizeof(uint32_t));

    utilSetBE8(&pduHdr.pduType, SDP_PDU_Service_Search_Response);
    utilSetBE16(&pduHdr.transactionID, transact);
    utilSetBE16(&pduHdr.paramLen, sizeof(struct sdpPduSSRepl) + sizeof(struct sdpCont) + sendSz);

    /* create the reply SG */
    rsg = sgNew();
    if (!rsg)
        return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);

    end = sgSplit(inst->contResult, sendSz);
    if (!end || !sgConcatBackCopy(rsg, &pduHdr, sizeof(pduHdr)) || !sgConcatBackCopy(rsg, &repl, sizeof(repl)))
        goto out_nomem;

    /* add the data to reply SG */
    sgConcat(rsg, inst->contResult);
    if (!sgLength(end)) {
        sgFree(end);
        end = NULL;
    }
    inst->contResult = end;

    /* add continuation state to repy SG */
    if (!sdpAppendCont(inst, rsg))
        goto out_nomem;

    /* send it */
    if (sdpSend(inst, rsg))
        return true;

    sgFree(rsg);
    return false;

out_nomem:
    sgFree(rsg);
    return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);
}

/*
 * FUNCTION: sdpSendResultsAR
 * USE:      Send a chunk of the AR response, and create & send & record a new cont value
 * PARAMS:   inst - the instance
 *           pduType - the PDU type to send
 *           transact - the transaction ID
 *           maxRetBytes - max bytes to return
 * RETURN:   success
 * NOTES:    inst->contVal is laready the new cont - we just package it up and send it
 */
static bool sdpSendResultsAR(struct sdpInst *inst, uint8_t pduType, uint16_t transact, uint16_t maxRetBytes)
{
    uint16_t sendBytes = sgLength(inst->contResult);
    uint8_t buf[sizeof(uint16_t)];
    struct sdpPdu pdu;
    sg s, t;

    /*
     * There are a few limitations on our send size in play here. We have: the MTU, the
     * requested max reply size, the representation limits of various "size" fields, and
     * the amount of data we have. The repreentatin limit does not need to be checked
     * against since the L2C MTU itself is also 16-bits long anyways and thus protects
     * us. We will first see if we can send all the data at once. If not, we'll then
     * decide how to segment it.
     */
    if (sgLength(inst->contResult) > maxRetBytes || inst->mtu > sgLength(inst->contResult) + sizeof(struct sdpPdu) + sizeof(uint16_t)/* size */ + sizeof(uint8_t)/*no-cont header */) {
        const uint16_t maxAllowedByMtu = inst->mtu - sizeof(struct sdpPdu) - sizeof(struct sdpCont) - sizeof(uint16_t)/* size */;

        /* limit to requested size */
        if (sendBytes > maxRetBytes)
            sendBytes = maxRetBytes;

        /* limit to mtu */
        if (sendBytes > maxAllowedByMtu)
            sendBytes = maxAllowedByMtu;
    }

    /* grab the data to send into s */
    t = sgSplit(inst->contResult, sendBytes);
    if (!t)
        return false;
    s = inst->contResult;
    if (!sgLength(t)) {
        sgFree(t);
        t= NULL;
    }
    inst->contResult = t;

    /* add the first part of the reply (length) */
    utilSetBE16(buf, sgLength(s));
    if (!sgConcatFrontCopy(s, buf, sizeof(uint16_t)))
        goto fail;

    /* add cont */
    if (!sdpAppendCont(inst, s))
        goto fail;

    /* append the PDU header */
    utilSetBE8(&pdu.pduType, pduType);
    utilSetBE16(&pdu.transactionID, transact);
    utilSetBE16(&pdu.paramLen, sgLength(s));

    if (!sgConcatFrontCopy(s, &pdu, sizeof(pdu)))
        goto fail;

    /* will free sg on failure */
    if (sdpSend(inst, s))
        return true;

fail:
    sgFree(s);
    if (inst->contResult)
        sgFree(inst->contResult);
    inst->contResult = NULL;
    return false;
}

/*
 * FUNCTION: sdpCheckContAR
 * USE:      See if an incoming SAR or SSAR request is a continuation of a previous one
 * PARAMS:   inst - the instance
 *           pduType - the PDU type to send
 *           transact - the transaction ID
 *           req - the request
 *           maxRetBytes - max bytes to return
 *           retP - store return codo here
 * RETURN:   true if cont was found and we handled it. false to treat this as a new request
 * NOTES:
 */
static bool sdpCheckContAR(struct sdpInst *inst, uint8_t pduType, uint16_t transact, sg req, uint16_t maxRetBytes, bool *retP)
{
    uint8_t buf[4];
    uint32_t ofst = 0;
    uniq_t cont;

    /* we need to first skip the data_sequence attr_id_list */
    if (sgLength(req) <= ofst)
        return false;

    /* read data sequence header, sanity check it, skip it */
    sgSerialize(req, ofst++, 1, buf);
    buf[0] = utilGetBE8(buf + 0);
    if (SDP_ITEM_GET_TYPE(buf[0]) != SDP_TYPE_ARRAY)
        return false;
    switch (SDP_ITEM_GET_SIZE(buf[0])) {
    case SDP_SZ_u8:
        if (sgLength(req) < ofst + sizeof(uint8_t))
            return false;
        sgSerialize(req, ofst, sizeof(uint8_t), buf);
        ofst += sizeof(uint8_t) + utilGetBE8(buf);
        break;
    case SDP_SZ_u16:
        if (sgLength(req) < ofst + sizeof(uint16_t))
            return false;
        sgSerialize(req, ofst, sizeof(uint16_t), buf);
        ofst += sizeof(uint16_t) + utilGetBE16(buf);
        break;
    case SDP_SZ_u32:
        if (sgLength(req) < ofst + sizeof(uint32_t))
            return false;
        sgSerialize(req, ofst, sizeof(uint32_t), buf);
        ofst += sizeof(uint32_t) + utilGetBE32(buf);
        break;
    default:
        return false;
    }

    if (sgLength(req) <= ofst)
        return false;

    /* now read cont state */
    if (sdpGetCont(req, ofst, false, &cont))
        return false;

    if (cont && cont == inst->contVal) {
        *retP = sdpSendResultsAR(inst, pduType, transact, maxRetBytes);
        return true;
    }
    if (inst->contVal) {
        logw("dropping existing cont\n");
        sgFree(inst->contResult);
        inst->contVal = 0;
    }

    if (cont)
        logw("Invalid cont seen - ignoring\n");

    return false;
}

/*
 * FUNCTION: sdpGetNextAttrRange
 * USE:      Grab the next wanted attr range from the request
 * PARAMS:   req - the request
 *           ofstP - where to read from and store to the current offsent in the req
 *           len - lengs of req to consider
 *           firstP - where to store first attr id in range
 *           lastP - where to store last attr id in range
 * RETURN:   success
 * NOTES:
 */
static bool sdpGetNextAttrRange(sg req, uint32_t *ofstP, uint32_t len, uint16_t *firstP, uint16_t *lastP)
{
    uint8_t buf[4];

    if (*ofstP >= len)
        return false;

    sgSerialize(req, (*ofstP)++, sizeof(uint8_t), buf);
    buf[0] = utilGetBE8(buf);

    if (SDP_ITEM_GET_TYPE(buf[0]) != SDP_TYPE_UINT)
        return false;

    switch (SDP_ITEM_GET_SIZE(buf[0])) {
    case SDP_SZ_2:
        if (*ofstP + sizeof(uint16_t) > len)
            return 0;
        sgSerialize(req, *ofstP, sizeof(uint16_t), buf);
        *ofstP += sizeof(uint16_t);
        *firstP = *lastP = utilGetBE16(buf);
        return true;
    case SDP_SZ_4:
        if (*ofstP + sizeof(uint32_t) > len)
            return 0;
        sgSerialize(req, *ofstP, sizeof(uint32_t), buf);
        *ofstP += sizeof(uint32_t);
        *firstP = utilGetBE32(buf) >> 16;
        *lastP = utilGetBE32(buf);
        return true;
    default:
        return false;
    }
}

/*
 * FUNCTION: sdpWrap
 * USE:      Wrap the entire SG into an SDP "data sequence" wrapper
 * PARAMS:   s - the sg
 * RETURN:   success
 * NOTES:
 */
static bool sdpWrap(sg s)
{
    uint8_t hdr[5], hlen = 0;
    uint32_t len = sgLength(s);

    if (len < 0xff) {
        hdr[hlen++] = SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8);
        utilSetBE8(hdr + hlen, len);
        hlen += sizeof(uint8_t);
    } else if (len < 0xffff) {
        hdr[hlen++] = SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u16);
        utilSetBE16(hdr + hlen, len);
        hlen += sizeof(uint16_t);
    } else {
        hdr[hlen++] = SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u32);
        utilSetBE32(hdr + hlen, len);
        hlen += sizeof(uint32_t);
    }

    return sgConcatFrontCopy(s, hdr, hlen);
}

/*
 * FUNCTION: sdpDoAR
 * USE:      Handle the "AR" part of the SAR or SSAR request
 * PARAMS:   inst - the instance
 *           pduType - the PDU type to send
 *           transact - the transaction ID
 *           req - the request
 *           maxRetBytes - how many attr bytes max to return
 *           res - the list of services (list to be freed by this func)
 *           doWrap - set to wrap each service's reply into an array
 * RETURN:   false on error
 * NOTES:    call with mSvcsLock held for read
 */
static bool sdpDoAR(struct sdpInst *inst, uint8_t pduType, uint16_t transact, sg req, uint16_t maxRetBytes, struct sdpSearchResultNode *res, bool doWrap)
{
    sg ret;
    uint32_t attrListLen = 0;
    uint8_t buf[4];

    /* get & check collection header, find size */
    if (!sgSerializeCutFront(req, buf, sizeof(uint8_t)))
        goto syntax_err;
    buf[0] = utilGetBE8(buf + 0);

    if (SDP_ITEM_GET_TYPE(buf[0]) != SDP_TYPE_ARRAY)
        goto syntax_err;

    switch (SDP_ITEM_GET_SIZE(buf[0])) {
    case SDP_SZ_u8:
        if (!sgSerializeCutFront(req, buf, sizeof(uint8_t)))
            goto syntax_err;
        attrListLen = utilGetBE8(buf);
        break;
    case SDP_SZ_u16:
        if (!sgSerializeCutFront(req, buf, sizeof(uint16_t)))
            goto syntax_err;
        attrListLen = utilGetBE16(buf);
        break;
    case SDP_SZ_u32:
        if (!sgSerializeCutFront(req, buf, sizeof(uint32_t)))
            goto syntax_err;
        attrListLen = utilGetBE32(buf);
        break;
    default:
        goto syntax_err;
    }

    if (sgLength(req) < attrListLen)
        goto syntax_err;

    logd("AR attr list len %ub\n", attrListLen);

    ret = sgNew();
    if (!ret)
        goto oom_ret;

    while (res) {
        struct sdpSearchResultNode *t = res;
        struct sdpService *ss = t->ss;
        const uint8_t *ptr = ss->descr;
        const uint8_t *end = ptr + ss->len;
        bool needNewAttr = true;
        bool needNewRange = true;
        uint16_t first = 0, last = 0;
        uint32_t ofst = 0;
        uint16_t attrid = 0;
        const void* attrPtr = NULL;
        uint32_t attrLen = 0, hdrLen = 0;

        const uint8_t *_start = ptr;

        sg part = sgNew();

        if (!part)
            goto oom;

        res = res->next;
        free(t);

        /* both attrs in the service description and in request are sorted - this helps us to do this in O(n) */

        while (1) {
            
            if (needNewRange && !sdpGetNextAttrRange(req, &ofst, attrListLen, &first, &last))
                break;
            needNewRange = false;

            if (needNewAttr) {

                if (ptr >= end)
                    break;

                needNewAttr = false;
                hdrLen = 4 /* at least this much always */;

                attrPtr = ptr;
                if (SDP_ITEM_GET_TYPE(*ptr) != SDP_TYPE_UINT) {
                    logw("attrID not uint in SDP record\n");
                    break;
                }
                if (SDP_ITEM_GET_SIZE(*ptr) != SDP_SZ_2) {
                    logw("attrID not uint16 in SDP record\n");
                    break;
                }
                attrid = utilGetBE16(ptr + 1);
                ptr += 3;

                if (SDP_ITEM_GET_TYPE(*ptr) == SDP_TYPE_NIL) { /* we do not care about nils */
                    needNewAttr = true;
                    continue;
                }
                switch(SDP_ITEM_GET_SIZE(*ptr)){
                case SDP_SZ_1:
                    attrLen = 1;
                    ptr++;
                    break;
                case SDP_SZ_2:
                    attrLen = 2;
                    ptr++;
                    break;
                case SDP_SZ_4:
                    attrLen = 4;
                    ptr++;
                    break;
                case SDP_SZ_8:
                    attrLen = 8;
                    ptr++;
                    break;
                case SDP_SZ_16:
                    attrLen = 16;
                    ptr++;
                    break;
                case SDP_SZ_u8:
                    attrLen = utilGetBE8(ptr + 1);
                    hdrLen += sizeof(uint8_t);
                    ptr += sizeof(uint8_t) + 1;
                    break;
                case SDP_SZ_u16:
                    attrLen = utilGetBE16(ptr + 1);
                    hdrLen += sizeof(uint16_t);
                    ptr += sizeof(uint16_t) + 1;
                    break;
                case SDP_SZ_u32:
                    attrLen = utilGetBE32(ptr + 1);
                    hdrLen += sizeof(uint32_t);
                    ptr += sizeof(uint32_t) + 1;
                    break;
                }
                ptr += attrLen;
            }

            needNewAttr = attrid <= last;
            needNewRange = attrid >= last;

            if (attrid >= first && attrid <= last && !sgConcatBackCopy(part, attrPtr, attrLen + hdrLen))
                logw("failed to concat %ub to AR part\n", attrLen + hdrLen);
        }

        if (sgLength(part) && (!doWrap || sdpWrap(part)))
            sgConcat(ret, part);
        else
            sgFree(part);
    }

    if (!sdpWrap(ret))
        goto oom;

    inst->contResult = ret;
    return sdpSendResultsAR(inst, pduType, transact, maxRetBytes);

oom:
    while (res) {
        struct sdpSearchResultNode *t = res;
        res = res->next;
        free(t);
    }
    sgFree(ret);

oom_ret:
    return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);

syntax_err:
    return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
}

/*
 * FUNCTION: sdpSAR
 * USE:      Handle an incoming service attribute request
 * PARAMS:   inst - the instance
 *           transact - the transaction ID
 *           req - the request
 * RETURN:   false on error
 * NOTES:    call with mSvcsLock held for read
 */
static bool sdpSAR(struct sdpInst *inst, uint16_t transact, sg req)
{
    struct sdpSearchResultNode *res;
    struct sdpService *ss;
    uint16_t maxRetBytes;
    uint32_t handle;
    bool ret;

    /* get handle */
    if (!sgSerializeCutFront(req, &handle, sizeof(handle)))
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
    handle = utilGetBE32(&handle);

    /* get the service */
    ss = sdpServiceFindByHandle(handle);
    if (!ss)
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Service_Record_Handle);

    /* get wanted max reply len */
    if (!sgSerializeCutFront(req, &maxRetBytes, sizeof(maxRetBytes)))
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
    maxRetBytes = utilGetBE16(&maxRetBytes);

    if (maxRetBytes < 7) /* as per spec */
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    logd("SAR max reply len: %u\n", maxRetBytes);

    /* check if continued request */
    if (sdpCheckContAR(inst, SDP_PDU_Service_Attribute_Response, transact, req, maxRetBytes, &ret))
        return ret;

    logd("SAR not cont. Proceeding with search\n");

    /* create a node containing it */
    res = (struct sdpSearchResultNode*)malloc(sizeof(struct sdpSearchResultNode));
    if (res)
        return sdpSendError(inst, transact, SDP_ERR_Insufficient_Resources);
    res->ss = ss;
    res->next = NULL;

    /* grab the wanted attributes */
    return sdpDoAR(inst, SDP_PDU_Service_Attribute_Response, transact, req, maxRetBytes, res, false);
}

/*
 * FUNCTION: sdpSSAR
 * USE:      Handle an incoming service search attribute request
 * PARAMS:   inst - the instance
 *           req - the request
 * RETURN:   false on error
 * NOTES:    call with mSvcsLock held for read
 */
static bool sdpSSAR(struct sdpInst *inst, uint16_t transact, sg req)
{
    struct uuid searchReq[SDP_MAX_UUIDS_IN_SEARCH];
    struct sdpSearchResultNode *searchRes;
    uint16_t maxRetBytes;
    uint8_t i, searchReqNum;
    uint16_t err;
    bool ret;

    err = sdpGetSearchReq(req, searchReq, &searchReqNum);
    if (err)
        return sdpSendError(inst, transact, err);

    logd("SSAR query has %u UUIDS\n", searchReqNum);
    for (i = 0; i < searchReqNum; i++)
        logd("SSAR uuid[%u] = "FMT64"x"FMT64"x\n", i, CNV64(searchReq[i].hi), CNV64(searchReq[i].lo));

    if (!searchReqNum)
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    /* get wanted max reply len */
    if (!sgSerializeCutFront(req, &maxRetBytes, sizeof(maxRetBytes)))
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
    maxRetBytes = utilGetBE16(&maxRetBytes);

    logd("SSAR max reply len: %u\n", maxRetBytes);

    if (maxRetBytes < 7) /* as per spec */
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);

    /* check if continued request */
    if (sdpCheckContAR(inst, SDP_PDU_Service_Search_Attribute_Response, transact, req, maxRetBytes, &ret))
        return ret;

    logd("SSAR not cont. Proceeding with search\n");

    err = sdpDoSearch(inst, searchReq, searchReqNum, -1, &searchRes);
    if (err)
        return sdpSendError(inst, transact, err);

    /* grab the wanted attributes */
    return sdpDoAR(inst, SDP_PDU_Service_Search_Attribute_Response, transact, req, maxRetBytes, searchRes, true);
}

/*
 * FUNCTION: sdpRx
 * USE:      Handle an incoming request
 * PARAMS:   inst - the instance
 *           transact - the transaction ID
 *           req - the request
 * RETURN:   false on error
 * NOTES:    call with mSvcsLock held for read
 */
static bool sdpRx(struct sdpInst *inst, sg req)
{
    struct sdpPdu reqPdu;
    uint8_t pduType;
    uint16_t transact, paramLen;

#ifdef SDP_DBG
    char line[128] = "SDP RX <ZLP>";
    uint32_t i;
    uint8_t v;

    for(i = 0; i < sgLength(req); i++) {
        sgSerialize(req, i, 1, &v);
        if (i & 15)
            sprintf(line + strlen(line), " %02X", v);
        else {
            if (i)
                logd("%s\n", line);
            sprintf(line, "SDP RX [%03X] %02X", i, v);
        }
    }
    logd("%s\n", line);
#endif

    if (!sgSerializeCutFront(req, &reqPdu, sizeof(reqPdu)))
        return sdpSendError(inst, 0, SDP_ERR_Invalid_PDU_Size);

    pduType = utilGetBE8(&reqPdu.pduType);
    transact = utilGetBE16(&reqPdu.transactionID);
    paramLen = utilGetBE16(&reqPdu.paramLen);

    if (paramLen != sgLength(req))
        return sdpSendError(inst, transact, SDP_ERR_Invalid_PDU_Size);

    logd("SDP req %u trans %u with %ub of params\n", pduType, transact, paramLen);

    switch (pduType) {
    case SDP_PDU_Service_Search_Request:
        return sdpSSR(inst, transact, req);
    case SDP_PDU_Service_Attribute_Request:
        return sdpSAR(inst, transact, req);
    case SDP_PDU_Service_Search_Attribute_Request:
        return sdpSSAR(inst, transact, req);
    default:
        return sdpSendError(inst, transact, SDP_ERR_Invalid_Request_Syntax);
    }
}

/*
 * FUNCTION: sdpInstState
 * USE:      Handle an event happening to the SDP instance
 * PARAMS:   userData - unused
 *           instance - the instance
 *           state - what happened
 *           data - data pertinent to the state
 *           len - length of said data
 * RETURN:   NONE
 * NOTES:
 */
static void sdpInstState(void *userData, void *instance, uint8_t state, const void *data, uint32_t len)
{
    struct sdpInst *inst = (struct sdpInst*)instance;
    sg payload;

    switch(state){
    case L2C_STATE_OPEN:
        logd("SDP open\n");
        break;
    case L2C_STATE_MTU:
        if (len != sizeof(uint16_t))
            loge("unexpected length for MTU\n");
        else {
            inst->mtu = *(uint16_t*)data;
            logd("SDP MTU: %d\n", inst->mtu);
        }
        break;
    case L2C_STATE_ENCR:
        logd("SDP ENCR\n");
        break;
    case L2C_STATE_RX:
        if (len != sizeof(sg))
            loge("unexpected length for RX\n");
        else {
            payload = *(sg*)data;
            pthread_rwlock_rdlock(&mSvcsLock);
            if (!sdpRx(inst, payload))
                loge("SDP failed to process a request\n");
            pthread_rwlock_unlock(&mSvcsLock);
            sgFree(payload);
        }
        break;
    case L2C_STATE_CLOSED:
        logi("SDP closed\n");
        if (inst->contResult)
            sgFree(inst->contResult);
        sendQueueDelPackets(mSendQ, inst->conn);
        free(inst);
        break;
    default:
        logw("SDP state call %u unsupported\n", state);
        break;
    }
}

/*
 * FUNCTION: sdpInstAlloc
 * USE:      Create a state struct for a connection to SDP
 * PARAMS:   userData - unused
 *           handle - the l2c connection handle
 *           instanceP - save the pointer to the instance here
 * RETURN:   result (SVC_ALLOC_*)
 * NOTES:
 */
static uint8_t sdpInstAlloc(void *userData, l2c_handle_t handle, void **instanceP)
{
    struct sdpInst *inst = (struct sdpInst*)calloc(1, sizeof(struct sdpInst));

    if (!inst)
    return SVC_ALLOC_FAIL_OTHER;

    inst->conn = handle;
    *instanceP = inst;
    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: sdpInit
 * USE:      Called to init SDP and register it with L2CAP
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool sdpInit(void)
{
    static const uint8_t sdpDescrSdp[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_SDP_SERVER_UUID),

        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 8,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_PSM),

        //browse group list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),

        //magic data
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(0xDDDD),
        SDP_ITEM_DESC(SDP_TYPE_TEXT, SDP_SZ_u8), 18,
            0x42, 0x79, 0x20, 0x44, 0x6D, 0x69, 0x74, 0x72, 0x79,
            0x20, 0x47, 0x72, 0x69, 0x6e, 0x62, 0x65, 0x72, 0x67,
    };
    static const struct l2cServicePsmDescriptor sdpSvc = {
        .userData = NULL,
        .serviceInstanceAlloc = sdpInstAlloc,
        .serviceInstanceStateCbk = sdpInstState,
        .serviceConnectionlessRx = NULL,
        .mtu = 32768,
    };

    mNextHandle = SDP_HANDLE_FIRST;

    mSendQ = sendQueueAlloc(1);
    if (!mSendQ) {
        loge("Failed to alloc send queue\n");
        return false;
    }

    if (!sdpServiceDescriptorAddInt(sdpDescrSdp, sizeof(sdpDescrSdp), SDP_HANDLE_SELF)) {
        loge("SDP failed to register with itself!\n");
        sendQueueFree(mSendQ);
        return false;
    }

    if (!l2cApiServicePsmRegister(SDP_PSM, &sdpSvc)) {
        loge("Failed to register SDP service\n");
        sdpServiceDescriptorDelInt(mSvcsHead); /* we are our only record - this is safe */
        sendQueueFree(mSendQ);
        return false;
    }

    return true;
}

/*
 * FUNCTION: sdpDeinit
 * USE:      Clean up SDP
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
void sdpDeinit(void)
{
    l2cApiServicePsmUnregister(SDP_PSM, NULL, NULL);
    pthread_rwlock_wrlock(&mSvcsLock);
    while (mSvcsHead) {
        mSvcsTail = mSvcsHead;
        mSvcsHead = mSvcsHead->next;
        free(mSvcsTail);
    }
    mSvcsTail = NULL;
    pthread_rwlock_unlock(&mSvcsLock);
}

