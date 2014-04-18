#include <stdlib.h>
#include <string.h>
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "sendQ.h"
#include "uuid.h"
#include "util.h"
#include "gatt.h"
#include "att.h"
#include "sdp.h"
#include "log.h"
#include "mt.h"
#include "bt.h"


#define GATT_ITEM_TYPE_INCLUDE     1
#define GATT_ITEM_TYPE_CHAR_DECL   2
#define GATT_ITEM_TYPE_CHAR_VAL    3
#define GATT_ITEM_TYPE_CHAR_DESCR  4

#define GATT_HDR_HANDLES           1 /* handles required for a service header */

struct gattIncludeDefHdr {
    uint16_t startHandle;
    uint16_t endHandle;
    /* uuid here if it is a uuid-16 */
} __packed;

struct gattCharDefHdr {
    uint8_t charProps;
    uint16_t valHandle;
    /* uuid here */
} __packed;

struct gattConnState {
    struct gattConnState *next;
    struct gattConnState *prev;

    l2c_handle_t conn;
    uint32_t     mtu;
    bool         encr;
    /* more here later */
};

struct gattService;

struct gattItem { /* a list of these exists in a a gett service */
    struct gattItem    *next;
    struct gattItem    *prev;
    uint8_t             type;
    union {
        struct{
            struct gattService *incl;
        } include;
        struct {
            struct uuid         uuid;
            uint8_t             charProps;
        } charDecl;
        struct {
            uint32_t            valPerms;
        } charVal;
        struct {
            struct uuid         uuid;
            uint32_t            descrValPerms;
        } descriptor;
    };
};

struct gattService {
    struct gattService  *next;
    struct gattService  *prev;
    struct uuid          uuid;
    att_range_t          attRange;
    uint16_t             numItems;
    uint16_t             maxItems;
    int                  svcRef;
    uint32_t             sdpHandle; /* zero if no current SDP record exists (not online) */

    gattSrvValueReadCbk  readF;
    gattSrvValueWriteCbk writeF;
    gattSrvIndCbk        indF;

    struct gattItem     *itemsHead;
    struct gattItem     *itemsTail;
    uint16_t             useCount; /* how many services include this one */
    bool                 primary;
    bool                 online;
};

struct gattWorkItem {
    uint8_t type;
    /* work data here */
    union {
        struct {
            l2c_handle_t             who;
            union {
                gattSrvValueReadCbk  readF;
                gattSrvValueWriteCbk writeF;
            };
            int                      svcRef;
            int                      cid;
            int                      transId;
            uint16_t                 handle;
            uint16_t                 byteOfst;
            uint16_t                 len;
            uint8_t                  reason;
            /* data here, if write */
        } rwCall;
        struct {
            uint64_t                 ref;
            l2c_handle_t             who;
            gattSrvIndCbk            indF;
            int                      svcRef;
            int                      cid;
            uint16_t                 handle;
            uint8_t                  evt;
        } indCall;
    };
};

#define GATT_WORK_TYPE_READ_CALL      0
#define GATT_WORK_TYPE_WRITE_CALL     1
#define GATT_WORK_TYPE_IND_CALL       2



/* our state */
static uint32_t mSdpHandleGatt;
static uint32_t mSdpHandleGap;
static struct sendQ* mSendQ;
static struct workQueue* mWorkQ;
static pthread_t mWorker;

static pthread_mutex_t mGattLock = PTHREAD_MUTEX_INITIALIZER;
static struct gattConnState *mConns;
static struct gattService *mSvcs;


/*
 * FUNCTION: gattConnFind
 * USE:      find a per-connection structure for a given l2c handle
 * PARAMS:   conn - the l2c connection handle
 * RETURN:   connection structure or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattConnState* gattConnFind(l2c_handle_t conn)
{
    struct gattConnState *state;

    for (state = mConns; state && state->conn != conn; state = state->next);

    return state;
}


/*
 * FUNCTION: gattConnStructDelete
 * USE:      Delete and unlink a connection struct
 * PARAMS:   state - said struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattConnStructDelete(struct gattConnState *state)
{
    if (state->next)
        state->next->prev = state->prev;
    if (state->prev)
        state->prev->next = state->next;
    else
        mConns = state->next;

    /* future free steps here */
    free(state);
}

/*
 * FUNCTION: gattServiceFindByUuid
 * USE:      Find a gatt service by uuid
 * PARAMS:   uuid - the uuid
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService *gattServiceFindByUuid(const struct uuid *uuid)
{
    struct gattService *svc = mSvcs;

    while (svc && !uuidCmp(uuid, &svc->uuid))
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattServiceFindByHandle
 * USE:      Find a gatt service by handle
 * PARAMS:   svcRef - the service reference
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService *gattServiceFindByHandle(int svcRef)
{
    struct gattService *svc = mSvcs;

    while (svc && svcRef != svc->svcRef)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattServiceFindByRangeRef
 * USE:      Find a gatt service by att range reference
 * PARAMS:   svcRef - the service reference
 * RETURN:   service struct or NULL if not found
 * NOTES:    call with mGattLock held
 */
static struct gattService *gattServiceFindByRangeRef(att_range_t rangeRef)
{
    struct gattService *svc = mSvcs;

    while (svc && rangeRef != svc->attRange)
        svc = svc->next;

    return svc;
}

/*
 * FUNCTION: gattAttSrvTxCbk
 * USE:      ATT calls this to TX to a client
 * PARAMS:   who - whom to send to
 *           s - the data to send
 * RETURN:   ATT_TX_RET_*
 * NOTES:    
 */
static uint8_t gattAttSrvTxCbk(l2c_handle_t who, sg s)
{
    return sendQueueTx(mSendQ, who, s) ? ATT_TX_RET_ACCEPTED : ATT_TX_RET_L2C_ERR;
}

/*
 * FUNCTION: gattEnqueueRwCall
 * USE:      Enqueue a work item to make a call to the higher layer for a read or a write
 * PARAMS:   workType - read or write?
 *           svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle 
 *           byteOfst - byte offset for the read/write
 *           reason - ATT_READ_FOR_* or ATT_WRITE_FOR_*
 *           len - requested length
 *           data - NULL for read, data for write
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueRwCall(uint8_t workType, struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem) + (data ? len : 0));

    if (wi) {
        wi->type = workType;
        wi->rwCall.who = who;
        wi->rwCall.svcRef = svc->svcRef;
        wi->rwCall.cid = cid;
        wi->rwCall.transId = transId;
        wi->rwCall.handle = handle;
        wi->rwCall.byteOfst = byteOfst;
        wi->rwCall.len = len;
        wi->rwCall.reason = reason;
        if (workType == GATT_WORK_TYPE_READ_CALL)
            wi->rwCall.readF = svc->readF;
        else {
            wi->rwCall.writeF = svc->writeF;
            memcpy(wi + 1, data, len);
        }

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattEnqueueReadCall
 * USE:      Enqueue a work item to make a call to the higher layer for a read
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle 
 *           byteOfst - byte offset for the read
 *           reason - ATT_READ_FOR_*
 *           len - requested length
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueReadCall(struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
{
    return gattEnqueueRwCall(GATT_WORK_TYPE_READ_CALL, svc, who, cid, transId, handle, byteOfst, reason, maxLen, NULL);
}

/*
 * FUNCTION: gattEnqueueWriteCall
 * USE:      Enqueue a work item to make a call to the higher layer for a write
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - handle 
 *           byteOfst - byte offset for the write
 *           reason - ATT_WRITE_FOR_*
 *           len - requested length
 *           data - data to write
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueWriteCall(struct gattService *svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    return gattEnqueueRwCall(GATT_WORK_TYPE_WRITE_CALL, svc, who, cid, transId, handle, byteOfst, reason, len, data);
}

/*
 * FUNCTION: gattEnqueueIndCall
 * USE:      Enqueue a work item to make a call to the higher layer for a result of sending an indication/notification
 * PARAMS:   svc - the relevant service
 *           who - whom the connection is to
 *           cid - ATT's connection ID
 *           handle - handle 
 *           evt - what happened
 *           ref - the value to pass to callback
 * RETURN:   false on immediate failure
 * NOTES:    call with mGattLock held
 */
static bool gattEnqueueIndCall(struct gattService *svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)calloc(1, sizeof(struct gattWorkItem));

    if (wi) {
        wi->type = GATT_WORK_TYPE_IND_CALL;
        wi->indCall.who = who;
        wi->indCall.svcRef = svc->svcRef;
        wi->indCall.cid = cid;
        wi->indCall.handle = handle;
        wi->indCall.evt = evt;
        wi->indCall.ref = ref;

        if (workQueuePut(mWorkQ, wi))
            return true;

        free(wi);
    }
    return false;
}

/*
 * FUNCTION: gattAttSrvIndCbk
 * USE:      ATT calls this with results of sending an indication/notification
 * PARAMS:   who - whom the indidation/notification was sent to
 *           cid - the connection Id to pass back to ATT
 *           rangeRef - the range this is related to
 *           offset - the value in the range
 *           evt - ATT_SRV_EVT_NOTIF_*
 *           ref - the reference passed to attSrvValueNotifyChanged() initially
 *           success - did we succeed?
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvIndCbk(l2c_handle_t to, int cid, att_range_t rangeRef, uint16_t offset, uint8_t evt, uint64_t ref)
{
    uint8_t err = ATT_ERROR_NONE;
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByRangeRef(rangeRef);
    if (!svc)
        logw("Failed to find service for read\n");
    else if (!gattEnqueueIndCall(svc, to, cid, offset + attSrvHandleRangeGetBase(svc->attRange), evt, ref))
        loge("Failed to forward notif event up\n");
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattAttSvrReadCbk
 * USE:      ATT calls this to read an attribute
 * PARAMS:   who - whom the indidation/notification was sent to
 *           rangeRef - the range this is related to
 *           cid - the connection Id to pass back to ATT
 *           ofst - the value in the range
 *           transId - transaction ID
 *           byteOfst - how mnay bytes into the characteristic we want to readsrvcre
 *           reason - read reason (ATT_READ_FOR_*)
 *           maxLen - max bytes to return
 * RETURN:   false on immediate failure
 * NOTES:    when read completes, a call to attSrvCbkReply() should be made
 */
static bool gattAttSvrReadCbk(l2c_handle_t who, int cid, att_range_t rangeRef, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t maxLen)
{
    uint8_t err = ATT_ERROR_NONE;
    struct gattConnState *conn;
    struct gattService *svc;
    struct gattItem *gi;
    uint8_t *ptr = NULL;
    uint16_t len = 0;
    uint16_t tmp;
    uint8_t buf[32];

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByRangeRef(rangeRef);
    conn = gattConnFind(who);
    if (!svc) {
        logw("Failed to find service for read\n");
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    }
    if (!conn) {
        logw("Failed to find/allocate client for read\n");
        err = ATT_ERROR_INSUFFICIENT_RSRCS;
        goto out;
    }

    if (!ofst) { /* service header */
        if (uuidToUuid16(&tmp, &svc->uuid)) { /* if it fits in 16 bits, do that */
            utilSetLE16(buf, tmp);
            ptr = buf;
            len = sizeof(uint16_t);
        } else {                                 /* else, send the entire 16-bute uuid */
            uuidWriteLE(buf, &svc->uuid);
            ptr = buf;
            len = sizeof(struct uuid);
        }
    } else if (ofst < GATT_HDR_HANDLES) {
        logw("Unexpected offset %u after header and before handles!\n", ofst);
        err = ATT_ERROR_INVALID_HANDLE;
        goto out;
    } else {
        uint16_t ctr = ofst;
        gi = svc->itemsHead;
        ctr -= GATT_HDR_HANDLES;
        while (gi && ctr--)
            gi = gi->next;

        if (!gi) {
            logw("Handle list ended abruptly with %u handles to go\n", ctr);
            err = ATT_ERROR_INVALID_HANDLE;
            goto out;
        }

        if (gi->type == GATT_ITEM_TYPE_INCLUDE) {
            struct gattIncludeDefHdr *hdr = (struct gattIncludeDefHdr*)buf;
            struct gattService *incl = gi->include.incl;
            uint16_t start;

            start = attSrvHandleRangeGetBase(incl->attRange);

            if (!attSrvValueGetGrpLen(incl->attRange, 0, &tmp)) {
                loge("Failed to get service len for included service\n");
                err = ATT_ERROR_UNLIKELY_ERROR;
                goto out;
            }
            utilSetLE16(&hdr->startHandle, start);
            utilSetLE16(&hdr->endHandle, start + tmp - 1);
            ptr = buf;
            len = sizeof(struct gattIncludeDefHdr);

            if (uuidToUuid16(&tmp, &incl->uuid)) { /* fits into a uuid-16 */
                utilSetLE16(hdr + 1, tmp);
                len += sizeof(uint16_t);
            }

        } else if (gi->type == GATT_ITEM_TYPE_CHAR_DECL) {
            struct gattCharDefHdr *hdr = (struct gattCharDefHdr*)buf;

            utilSetLE8(&hdr->charProps, gi->charDecl.charProps);
            utilSetLE16(&hdr->valHandle, ofst + attSrvHandleRangeGetBase(svc->attRange) + 1); /* always next */
            ptr = buf;
            len = sizeof(struct gattCharDefHdr);

            if (uuidToUuid16(&tmp, &gi->charDecl.uuid)) { /* fits into a uuid-16 */
                utilSetLE16(hdr + 1, tmp);
                len += sizeof(uint16_t);
            } else {
                uuidWriteLE(hdr + 1, &gi->charDecl.uuid);
                len += sizeof(struct uuid);
            }

        } else { /* in this case call to the higher layer for the read */
            if (!gattEnqueueReadCall(svc, who, cid, transId, ofst + attSrvHandleRangeGetBase(svc->attRange), byteOfst, reason, maxLen))
                err = ATT_ERROR_UNLIKELY_ERROR;
            goto out;
        }
    }

    if (!ptr) {
        loge("No data to send at end of read callback\n");
        err = ATT_ERROR_UNLIKELY_ERROR;
    } else if (byteOfst > len)
        err = ATT_ERROR_INVALID_OFFSET;
    else {
        len -= byteOfst;
        ptr += byteOfst;
    }

out:
    pthread_mutex_unlock(&mGattLock);

    if (len || err != ATT_ERROR_NONE)
        attSrvCbkReply(cid, transId, 0, err, ptr, len);
    return true;
}

/*
 * FUNCTION: gattAttSvrWriteCbk
 * USE:      ATT calls this to write an attribute
 * PARAMS:   who - whom the indidation/notification was sent to
 *           rangeRef - the range this is related to
 *           cid - the connection Id to pass back to ATT
 *           ofst - the value in the range
 *           transId - transaction ID
 *           byteOfst - how mnay bytes into the characteristic we want to write
 *           reason - read reason (ATT_WRITE_FOR_*)
 *           len - bytes to write
 *           data - the data
 * RETURN:   false on immediate failure
 * NOTES:    when write completes, a call to attSrvCbkReply() should be made
 */
static bool gattAttSvrWriteCbk(l2c_handle_t who, int cid, att_range_t range, uint16_t ofst, int transId, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    //TODO
    return false;
}

/*
 * FUNCTION: gattAttExecWriteCbk
 * USE:      ATT calls this when client asks us to execute a write
 * PARAMS:   who - whom the indidation/notification was sent to
 *           cid - the connection Id to pass back to ATT
 *           transId - the transcationID
 *           exec - execute or dump data?
 * RETURN:   NONE
 * NOTES:
 */
static bool gattAttExecWriteCbk(l2c_handle_t who, int cid, int transId, bool exec)
{
    //TODO
    return false;
}

/*
 * FUNCTION: gattAttSrvMtuCbk
 * USE:      ATT calls this with results of MTU negotiation
 * PARAMS:   who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvMtuCbk(l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState* state;

    pthread_mutex_lock(&mGattLock);
    state = gattConnFind(who);
    if (!state)
        loge("Unknown connection for MTU change event\n");
    else if (state->mtu > mtu)
        loge("MTU decrease not allowed\n");
    else
       state->mtu = mtu;
    pthread_mutex_unlock(&mGattLock);
}

/*
 * FUNCTION: gattConnEvt
 * USE:      L2C calls this with events about connections
 * PARAMS:   userData - unused
 *           instance - per-connection instance state (struct gattConnState)
 *           evt - what event happened
 *           data - the data pertinent to the event
 *           len - the length of said data
 * RETURN:   NONE
 * NOTES:
 */
static void gattConnEvt(void *userData, void *instance, uint8_t evt, const void *data, uint32_t len)
{
    struct gattConnState *state = (struct gattConnState*)instance;
    l2c_handle_t conn;
    uint16_t mtu;
    sg s;

    switch (evt) {
    case L2C_STATE_OPEN:
        if (len != sizeof(l2c_handle_t)) {
            loge("invalid length for open event\n");
            break;
        }
        conn = *(l2c_handle_t*)data;
        if (state->conn != conn) /* when we support outbound connections, this code will need to evolve */
            loge("Handle change!!!\n");
        break;

    case L2C_STATE_MTU:
        if (len != sizeof(uint16_t)) {
            loge("invalid length for mtu event\n");
            break;
        }
        mtu = *(uint16_t*)data;
        attSrvSetMtu(state->conn, mtu);
        gattAttSrvMtuCbk(state->conn, mtu);
        break;

    case L2C_STATE_ENCR:
        state->encr = true;
        break;

    case L2C_STATE_RX:
        if (len != sizeof(sg)) {
            loge("invalid length for RX event\n");
            break;
        }
        s = *(sg*)data;
        if (attSrvDataRx(state->conn, s))
            break;
        logw("Incoming GATT message was not accepted by ATT\n");
        sgFree(s);
        break;

    case L2C_STATE_CLOSED:
        attSrvNotifConnClose(state->conn);
        sendQueueDelPackets(mSendQ, state->conn);
        pthread_mutex_lock(&mGattLock);
        gattConnStructDelete(state);
        pthread_mutex_unlock(&mGattLock);
        break;
    default:
        logd("unknown L2C event %u\n", evt);
        break;
    }
}

/*
 * FUNCTION: gattStateAlloc
 * USE:      Allocate a GATT state for an L2C link
 * PARAMS:   conn - the connection handle
 *           instanceP - we store per-connection instance here
 *           isLE - what is the underlying link type?
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t gattStateAlloc(l2c_handle_t conn, void **instanceP, bool isLE)
{
    struct gattConnState *state = (struct gattConnState*)calloc(1, sizeof(struct gattConnState));

    if (!state)
        return SVC_ALLOC_FAIL_OTHER;

    state->conn = conn;
    state->mtu = isLE ? ATT_MTU_MIN_LE : ATT_MTU_MIN_EDR;
    /* any other init here */

    pthread_mutex_lock(&mGattLock);
    state->next = mConns;
    if (mConns)
        mConns->prev = state;
    mConns = state;
    pthread_mutex_unlock(&mGattLock);

    *instanceP = state;
    return SVC_ALLOC_SUCCESS;
}

/*
 * FUNCTION: gattPsmSvcAlloc
 * USE:      L2C calls this with remote requests to open a PSM-based channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t gattPsmSvcAlloc(void *userData, l2c_handle_t handle, void **instanceP)
{
    struct bt_addr peer;

    /* if we cannot get peer address, do not bother with anything else */
    if (!l2cApiGetBtAddr(handle, &peer))
        return SVC_ALLOC_FAIL_OTHER;

    /* we do not support PSM access to GATT over LE */
    if (BT_ADDR_IS_LE(peer))
        return SVC_ALLOC_FAIL_OTHER;

    return gattStateAlloc(handle, instanceP, false);
}

/*
 * FUNCTION: gattFixedChSvcAlloc
 * USE:      L2C calls this with requests to open a fixed channel
 * PARAMS:   userData - unused
 *           handle - the connection handle
 *           instanceP - we store per-connection instance here
 * RETURN:   SVC_ALLOC_*
 * NOTES:
 */
static uint8_t gattFixedChSvcAlloc(void *userData, l2c_handle_t handle, void **instanceP)
{
    struct bt_addr peer;

    /* if we cannot get peer address, do not bother with anything else */
    if (!l2cApiGetBtAddr(handle, &peer))
        return SVC_ALLOC_FAIL_OTHER;

    /* we do not support FixedCh access to GATT over EDR */
    if (BT_ADDR_IS_EDR(peer))
        return SVC_ALLOC_FAIL_OTHER;

    return gattStateAlloc(handle, instanceP, true);
}

/*
 * FUNCTION: gattSdpInit
 * USE:      Called to add a GATT profile to SDP
 * PARAMS:   genericAttrProfileHandle - start handle for generic attribute profile
 *           genericAttrProfileLen - length of generic attribute profile
 * RETURN:   sdp handle or 0 on error
 * NOTES:
 */
static uint32_t gattSdpAdd(uint16_t uuid16, uint16_t profileAttHandle, uint16_t profileAttLen)
{
    uint16_t profileAttEnd = profileAttHandle + profileAttLen - 1;

    uint8_t sdpDescrGatt[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(uuid16),

        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 19,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(GATT_PSM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 9,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_ATT),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(profileAttHandle),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(profileAttEnd),

        //browse group list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),
    };

    return sdpServiceDescriptorAdd(sdpDescrGatt, sizeof(sdpDescrGatt));
}

/*
 * FUNCTION: gattWorkQueueFreeItem
 * USE:      Called to free a workQ item at workQ delete time
 * PARAMS:   item - the work item
 * RETURN:   NONE
 * NOTES:
 */
static void gattWorkQueueFreeItem(void *item)
{
    struct gattWorkItem *wi = (struct gattWorkItem*)item;

    /* future free steps go here */
    free(wi);
}

/*
 * FUNCTION: gattSrvCbkReply
 * USE:      Called from higher layer as a reply to us calling for a read/write
 * PARAMS:   cid - conenction ID from ATT
 *           transId - transaction ID from ATT
 *           handle - handle on which an error occured (unused for read & write)
 *           err - ATT_ERROR_*
 *           data - the read data
 *           len - the read len
 * RETURN:   unused
 * NOTES:
 */
void gattSrvCbkReply(int cid, int transId, uint16_t handle, uint8_t err, const void *data, uint16_t len)
{
    attSrvCbkReply(cid, transId, handle, err, data, len);
}

/*
 * FUNCTION: gattWorker
 * USE:      Worker thread for calls to higher layer
 * PARAMS:   param - unused
 * RETURN:   unused
 * NOTES:
 */
static void* gattWorker(void *param)
{
    struct gattWorkItem *wi;
    int status;

    pthread_setname_np(pthread_self(), "gatt_worker");

    while(1) {
        status = workQueueGet(mWorkQ, (void**)&wi);

        /* The only one thing we use the work queue status for - exiting */
        if (status)
            break;

        switch (wi->type) {
        case GATT_WORK_TYPE_READ_CALL:
            if (!wi->rwCall.readF(wi->rwCall.svcRef, wi->rwCall.who, wi->rwCall.cid, wi->rwCall.transId, wi->rwCall.handle, wi->rwCall.byteOfst, wi->rwCall.reason, wi->rwCall.len))
                gattSrvCbkReply(wi->rwCall.cid, wi->rwCall.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;
        case GATT_WORK_TYPE_WRITE_CALL:
            if (!wi->rwCall.writeF(wi->rwCall.svcRef, wi->rwCall.who, wi->rwCall.cid, wi->rwCall.transId, wi->rwCall.handle, wi->rwCall.byteOfst, wi->rwCall.reason, wi->rwCall.len, wi + 1))
                gattSrvCbkReply(wi->rwCall.cid, wi->rwCall.transId, 0, ATT_ERROR_UNLIKELY_ERROR, NULL, 0);
            break;
        case GATT_WORK_TYPE_IND_CALL:
            wi->indCall.indF(wi->indCall.svcRef, wi->indCall.who, wi->indCall.cid, wi->indCall.handle, wi->indCall.evt, wi->indCall.ref);
            break;
        default:
            loge("Unknown work type %d\n", wi->type);
            break;
        }
        free(wi);
    }

    logd("GATT worker exiting\n");
    return NULL;
}

/*
 * FUNCTION: gattProfileInit
 * USE:      Initialize GATT
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool gattProfileInit(void)
{
    static const struct l2cServicePsmDescriptor gattPsm = {
        .serviceInstanceAlloc = gattPsmSvcAlloc,
        .serviceInstanceStateCbk = gattConnEvt,
        .mtu = 0xFFFF,
    };
    static const struct l2cServiceFixedChDescriptor gattFixedCh = {
        .serviceFixedChAlloc = gattFixedChSvcAlloc,
        .serviceFixedChStateCbk = gattConnEvt,
    };

    if (!attSrvCreate(gattAttSrvTxCbk, gattAttSrvMtuCbk, gattAttSrvIndCbk, gattAttExecWriteCbk, gattAttSvrReadCbk, gattAttSvrWriteCbk)) {
        loge("Failed create an ATT server\n");
        goto out_att;
    }

    mWorkQ = workQueueAlloc(GATT_WORK_Q_SIZE);
    if (!mWorkQ) {
        loge("Failed create a workQ\n");
        goto out_workq;
    }

    if (pthread_create(&mWorker, NULL, gattWorker, NULL)) {
        loge("Failed create worker\n");
        goto out_thread;
    }

    mSendQ = sendQueueAlloc(8);
    if (!mSendQ) {
        loge("Failed create a sendQ\n");
        goto out_sendq;
    }

    if (!l2cApiServicePsmRegister(GATT_PSM, &gattPsm)) {
        loge("Failed to register GATT PSM\n");
        goto out_psm;
    }

    if (!l2cApiServiceFixedChRegister(GATT_FIXEDCH, &gattFixedCh)) {
        loge("Failed to register GATT FixedCh\n");
        goto out_fixed;
    }

    /* todo: create the profile, so we do not pass zeroes here  */
    mSdpHandleGatt = gattSdpAdd(SDP_SVC_CLASS_GATT, 0, 0);
    if (!mSdpHandleGatt) {
        loge("Failed to register ATT-GATT with SDP\n");
        goto out_sdp_gatt;
    }

    /* todo: create the profile, so we do not pass zeroes here  */
    mSdpHandleGap = gattSdpAdd(SDP_SVC_CLASS_GAP, 0, 0);
    if (!mSdpHandleGap) {
        loge("Failed to register ATT-GAP with SDP\n");
        goto out_sdp_gap;
    }



    return true;

out_sdp_gap:
    sdpServiceDescriptorDel(mSdpHandleGatt);

out_sdp_gatt:
    l2cApiServiceFixedChUnregister(GATT_FIXEDCH, NULL, 0);

out_fixed:
    l2cApiServicePsmUnregister(GATT_PSM, NULL, 0);

out_psm:
    sendQueueFree(mSendQ);

out_sendq:
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);

out_thread:
    workQueueFree(mWorkQ, gattWorkQueueFreeItem);

out_workq:
    attSrvDestroy();

out_att:
    return false;
}

/*
 * FUNCTION: gattProfileDeinit
 * USE:      Deinitialize GATT
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void gattProfileDeinit(void)
{
    sdpServiceDescriptorDel(mSdpHandleGap);
    sdpServiceDescriptorDel(mSdpHandleGatt);
    l2cApiServiceFixedChUnregister(GATT_FIXEDCH, NULL, 0);
    l2cApiServicePsmUnregister(GATT_PSM, NULL, 0);
    attSrvDestroy();
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);
    workQueueFree(mWorkQ, gattWorkQueueFreeItem);
    sendQueueFree(mSendQ);
}

/*
 * FUNCTION: gattServiceCreate
 * USE:      Create a public-facing GATT service
 * PARAMS:   uuid - the uuid to use
 *           primary - primary or secondary service?
 *           numHandles - how many handles we expect this service to occupy
 *           readF - read callback for the values
 *           writeF - write callback for the values
 *           indF - callback to be notified of confirmations of indications
 * RETURN:   service reference or 0 on error
 * NOTES:
 */
int gattServiceCreate(const struct uuid *uuid, bool primary, uint16_t numHandles, gattSrvValueReadCbk readF, gattSrvValueWriteCbk writeF, gattSrvIndCbk indF)
{
    struct gattService *svc;
    int svcRef = 0;

    if (numHandles < GATT_HDR_HANDLES) {
        loge("Service with %u handles is impossible (%u min)\n", numHandles, GATT_HDR_HANDLES);
        return 0;
    }

    pthread_mutex_lock(&mGattLock);
    if (gattServiceFindByUuid(uuid)) {
        loge("Service ("UUIDFMT") already exists!\n", UUIDCONV(*uuid));
        goto out;
    }
    svc = (struct gattService*)calloc(1, sizeof(struct gattService));
    if (!svc)
        goto out;

    /* get a handle */
    svc->svcRef = mSvcs ? mSvcs->svcRef : 1;
    while (gattServiceFindByHandle(svc->svcRef)) {
        svc->svcRef++;
        if (svc->svcRef <= 0)
            svc->svcRef = 1;
    }

    /* grab a range */
    svc->attRange = attSrvHandleRangeReserve(numHandles);
    if (!svc->attRange) {
        loge("Failed to reserve %u-handle range\n", numHandles);
        goto out_att;
    }

    memcpy(&svc->uuid, uuid, sizeof(svc->uuid));
    svc->readF = readF;
    svc->writeF = writeF;
    svc->indF = indF;

    svc->maxItems = numHandles;
    svc->primary = primary;
    svcRef = svc->svcRef;

    svc->next = mSvcs;
    if (mSvcs)
        mSvcs->prev = svc;
    mSvcs = svc;

    goto out;

out_att:
    free(svc);

out:
    pthread_mutex_unlock(&mGattLock);
    return svcRef;
}

/*
 * FUNCTION: gattPermToAttPerm
 * USE:      Convert our 32-bit permissions mask to something we can pass to att with its simple 2-flag setup
 * PARAMS:   gattPerms - bitmask of GATT_PERM_*
 * RETURN:   bitmask of ATT_PERM_*
 * NOTES:
 */
static uint8_t gattPermToAttPerm(uint32_t gattPerms)
{
    uint8_t ret = 0;

    if (gattPerms & GATT_PERM_ALL_READ)
        ret |= ATT_PERM_READ;
    if (gattPerms & GATT_PERM_ALL_WRITE)
        ret |= ATT_PERM_WRITE;

    return ret;
}

/*
 * FUNCTION: gattServiceOnline
 * USE:      Online a service
 * PARAMS:   svc - the service struct
 * RETURN:   success
 * NOTES:    call with mGattLock held
 */
static bool gattServiceOnline(struct gattService *svc)
{
    struct gattItem *gi;
    uint16_t ofst = 0, charOfst = 0;
    struct uuid uuid;
    uint8_t perms = 0;

    if (svc->online) {
        logw("Cannot online an online service\n");
        goto out;
    }

    uuidFromUuid16(&uuid, svc->primary ? GATT_UUID_SVC_PRIMARY : GATT_UUID_SVC_SECONDARY);
    if (!attSrvValueRegister(svc->attRange, ofst++, &uuid, ATT_PERM_READ)) {
        loge("Failed to add service header\n");
        goto out;
    }

    gi = svc->itemsHead;
    while (gi) {
        bool updateGrp = false;

        switch (gi->type) {
        case GATT_ITEM_TYPE_INCLUDE:
            uuidFromUuid16(&uuid, GATT_UUID_INCLUDE);
            perms = ATT_PERM_READ;
            break;
        case GATT_ITEM_TYPE_CHAR_DECL:
            uuidFromUuid16(&uuid, GATT_UUID_CHARACTERISTIC);
            perms = ATT_PERM_READ;
            charOfst = ofst;
            break;
        case GATT_ITEM_TYPE_CHAR_VAL:
            if (gi->prev && gi->prev->type == GATT_ITEM_TYPE_CHAR_DECL)
                memcpy(&uuid, &gi->prev->charDecl.uuid, sizeof(uuid));
            else {
                loge("Char value not following a char descr\n");
                goto out_dereg;
            }
            perms = gattPermToAttPerm(gi->charVal.valPerms);
            updateGrp = true;
            break;
        case GATT_ITEM_TYPE_CHAR_DESCR:
            memcpy(&uuid, &gi->descriptor.uuid, sizeof(uuid));
            perms = gattPermToAttPerm(gi->descriptor.descrValPerms);
            updateGrp = true;
            break;
        default:
            loge("Unknown item type %u\n", gi->type);
            goto out_dereg;
        }

        if (!attSrvValueRegister(svc->attRange, ofst, &uuid, perms)) {
            loge("Failed to item at offset %u\n", ofst - 1);
            goto out_dereg;
        }
        ofst++;
        if (updateGrp && !attSrvValueSetGrpLen(svc->attRange, charOfst, ofst - charOfst)) {
            loge("Failed to set group length for characteristic\n");
            goto out_dereg;
        }
        gi = gi->next;
    }

    if (!attSrvValueSetGrpLen(svc->attRange, 0, ofst)) {
        loge("Failed to set group length for service\n");
        goto out_dereg;
    }

    svc->online = true;
    return true;

out_dereg:
    while (ofst)
        attSrvValueUnregister(svc->attRange, --ofst);

out:
    return false;
}

/*
 * FUNCTION: gattServiceOffline
 * USE:      Offline a service
 * PARAMS:   svc - the service struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattServiceOffline(struct gattService *svc)
{
    uint16_t i;

    if (!svc->online) {
        loge("Cannot offline an offlined service\n");
        return;
    }
    svc->online = false;

    for (i = 0; i < svc->maxItems; i++)
        if (!attSrvValueUnregister(svc->attRange, i))
            logw("Failed to unredister a value (ofst %u) for offlining!\n", i);
}

/*
 * FUNCTION: gattServiceDelete
 * USE:      Delete a service
 * PARAMS:   svc - the service struct
 * RETURN:   NONE
 * NOTES:    call with mGattLock held
 */
static void gattServiceDelete(struct gattService *svc)
{
    struct gattItem *gi, *t;

    if (svc->online) {
        loge("Cannot delete an online service\n");
        return;
    }

    if (svc->prev)
        svc->prev->next = svc->next;
    else
        mSvcs = svc->next;
    if (svc->next)
        svc->next->prev = svc->prev;

    gi = svc->itemsHead;
    while (gi) {
        t = gi;
        gi = gi->next;

        if (t->type == GATT_ITEM_TYPE_INCLUDE)
            t->include.incl->useCount--;

        free(t);
    }
    free(svc);
}

/*
 * FUNCTION: gattServiceDestroy
 * USE:      Destroy a public-facing GATT service
 * PARAMS:   svcRef - the service reference
 * RETURN:   success
 * NOTES:
 */
bool gattServiceDestroy(int svcRef)
{
    struct gattService *svc;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (svc) {
        if (svc->useCount)
            loge("Refusing to destroy service included elsewhere (%u times)\n", svc->useCount);
        else {
            if (svc->online) {
                logw("Destroying an online service\n");
                gattServiceOffline(svc);
            }
            gattServiceDelete(svc);
        }
    }
    pthread_mutex_unlock(&mGattLock);

    return !!svc;
}

/*
 * FUNCTION: gattServiceStart
 * USE:      Online a service
 * PARAMS:   svcRef - the service reference
 * RETURN:   success
 * NOTES:
 */
bool gattServiceStart(int svcRef)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (svc)
        ret = gattServiceOnline(svc);
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceStop
 * USE:      Offline a service
 * PARAMS:   svcRef - the service reference
 * RETURN:   success
 * NOTES:
 */
bool gattServiceStop(int svcRef)
{
    struct gattService *svc;
    bool ret = false;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (svc) {
        gattServiceOffline(svc);
        ret = true;
    }
    pthread_mutex_unlock(&mGattLock);

    return ret;
}

/*
 * FUNCTION: gattServiceGetHandleBaseByUuid
 * USE:      Get the base handle for a service given its UUID. Only works for online services
 * PARAMS:   uuid - the desired uuid
 * RETURN:   handle value or 0 on error
 * NOTES:
 */
uint16_t gattServiceGetHandleBaseByUuid(const struct uuid *uuid)
{
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByUuid(uuid);
    if (!svc)
        loge("Cannot get base for not-found service\n");
    else if (!svc->online)
        loge("Non-online services have no base\n");
    else
        handle = attSrvHandleRangeGetBase(svc->attRange);
    pthread_mutex_unlock(&mGattLock);

    return handle;
}

/*
 * FUNCTION: gattServiceGetHandleBaseBySvc///aaa
 * USE:      Get the base handle for a service given its service ref
 * PARAMS:   uuid - the desired uuid
 * RETURN:   handle value or 0 on error
 * NOTES:
 */
uint16_t gattServiceGetHandleBaseBySvc(int svcRef)
{
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (!svc)
        loge("Cannot get base for not-found service\n");
    else
        handle = attSrvHandleRangeGetBase(svc->attRange);
    pthread_mutex_unlock(&mGattLock);

    return handle;
}

/*
 * FUNCTION: gattSeerviceItemAppend
 * USE:      Append an item to a service
 * PARAMS:   svc - service struct
 *           gi - the item
 * RETURN:   offset from range start
 * NOTES:    probably want to tall this with the lock held
 */
static uint16_t gattServiceItemAppend(struct gattService *svc, struct gattItem *gi)
{
    gi->prev = svc->itemsTail;
    if (svc->itemsTail)
        svc->itemsTail->next = gi;
    else
        svc->itemsHead = gi;
    svc->itemsTail = gi;
    return svc->numItems++;
}

/*
 * FUNCTION: gattServiceAddIncludedService
 * USE:      Append an "included service" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the service to include
 * RETURN:   handle or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddIncludedService(int svcRef, const struct uuid *uuid)
{
    struct gattService *svc, *included;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
        goto out;
    }
    included = gattServiceFindByUuid(uuid);
    if (!included) {
        loge("Failed to find included service "UUIDFMT"n", UUIDCONV(*uuid));
        goto out;
    }
    if (svc->numItems >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }

    gi = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!gi) {
        loge("Failed to allocate new item for include\n");
        goto out;
    }

    gi->type = GATT_ITEM_TYPE_INCLUDE;
    gi->include.incl = included;
    handle = gattServiceItemAppend(svc, gi) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;
    included->useCount++;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceAddChar
 * USE:      Append a "characteristic" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the uuid of the value
 *           props - the value properties to show
 *           perms - actual permissions for the value
 * RETURN:   handle of the value attribute (not of the descriptor attribute which is first) or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddChar(int svcRef, const struct uuid *uuid, uint8_t props, uint32_t perms)
{
    struct gattItem *giDecl, *giVal;
    struct gattService *svc;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
        goto out;
    }
    if (svc->numItems + 1 >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }

    giDecl = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!giDecl) {
        loge("Failed to allocate new item for chat decl\n");
        goto out;
    }

    giVal = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!giVal) {
        free(giDecl);
        loge("Failed to allocate new item for char val\n");
        goto out;
    }

    giDecl->type = GATT_ITEM_TYPE_CHAR_DECL;
    giDecl->charDecl.charProps = props;
    memcpy(&giDecl->charDecl.uuid, uuid, sizeof(giDecl->charDecl.uuid));

    giVal->type = GATT_ITEM_TYPE_CHAR_VAL;
    giVal->charVal.valPerms = perms;

    gattServiceItemAppend(svc, giDecl);
    handle = gattServiceItemAppend(svc, giVal) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceAddCharDescr
 * USE:      Append a "characteristic descriptor" to a service
 * PARAMS:   svcRef - the service we're operating on
 *           uuid - the uuid of the descriptor
 *           perms - actual permissions for the descriptor's value
 * RETURN:   handle or 0 on error
 * NOTES:
 */
uint16_t gattServiceAddCharDescr(int svcRef, const struct uuid *uuid, uint32_t perms)
{
    struct gattService *svc;
    struct gattItem *gi;
    uint16_t handle = 0;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
        goto out;
    }
    if (svc->numItems >= svc->maxItems - GATT_HDR_HANDLES) {
        loge("No space left in the service\n");
        goto out;
    }
    gi = (struct gattItem*)calloc(1, sizeof(struct gattItem));
    if (!gi) {
        loge("Failed to allocate new item for char descriptor\n");
        goto out;
    }

    gi->type = GATT_ITEM_TYPE_CHAR_DESCR;
    memcpy(&gi->descriptor.uuid, uuid, sizeof(gi->descriptor.uuid));
    gi->descriptor.descrValPerms = perms;
    handle = gattServiceItemAppend(svc, gi) + attSrvHandleRangeGetBase(svc->attRange) + GATT_HDR_HANDLES;

out:
    pthread_mutex_unlock(&mGattLock);
    return handle;
}

/*
 * FUNCTION: gattServiceSendInd
 * USE:      Send an indication or a notification
 * PARAMS:   svcRef - the reference to a service
 *           cid - ATT's connection Id
 *           handle - the handle to notify/indicate on
 *           buf - the data to send
 *           len - length of said data
 *           withConfirm - use an indication?
 *           ref - passed to callback later. unused by GATT
 * RETURN:   fals eon immediate failure
 * NOTES:
 */
bool gattServiceSendInd(int svcRef, int cid, uint16_t handle, const void *buf, uint16_t len, bool withConfirm, uint64_t ref)
{
    uint8_t ret = L2C_TX_ERROR;
    struct gattService *svc;
    uint16_t base;
    sg data;

    pthread_mutex_lock(&mGattLock);
    svc = gattServiceFindByHandle(svcRef);
    if (!svc) {
        loge("Failed to find service %d\n", svcRef);
        goto out;
    }

    base = attSrvHandleRangeGetBase(svc->attRange);
    if (handle < base || handle >= base + svc->maxItems) {
        logd("Refusing to send notif for handle 0x%04X not in range 0x%04X+0x%04X\n", handle, base, svc->maxItems);
        goto out;
    }

    data = sgNewWithCopyData(buf, len);
    if (!data) {
        loge("Failed to contrust an SG\n");
        goto out;
    }

    ret = attSrvValueNotifyChanged(svc->attRange, handle - base, cid, data, !withConfirm, ref);
    if (ret == L2C_TX_ACCEPTED) {
        if (!withConfirm && !gattEnqueueIndCall(svc, attSrvCidResolve(cid), cid, handle, ATT_SRV_EVT_NOTIF_SENT, ref))
            loge("Failed to notify caller of notif TX\n");
    } else
        sgFree(data);

out:
    pthread_mutex_unlock(&mGattLock);
    return ret;
}












