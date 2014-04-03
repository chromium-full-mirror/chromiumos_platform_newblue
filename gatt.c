#include <stdlib.h>
#include <string.h>
#include "workQueue.h"
#include "config.h"
#include "l2cap.h"
#include "sendQ.h"
#include "gatt.h"
#include "att.h"
#include "sdp.h"
#include "log.h"
#include "mt.h"
#include "bt.h"



struct gattConnState {
    struct gattConnState *next;
    struct gattConnState *prev;

    l2c_handle_t conn;
    uint32_t     mtu;
    bool         encr;
    /* more here later */
};

struct gattWorkItem {
    uint8_t type;
    /* work data here */
};



/* our state */
static uint32_t mSdpHandle;
static struct attServer* mAttSrv;
static struct sendQ* mSendQ;
static struct workQueue* mWorkQ;
static pthread_t mWorker;

static pthread_mutex_t mConnsLock = PTHREAD_MUTEX_INITIALIZER;
static struct gattConnState *mConns;



/*
 * FUNCTION: gattConnFind
 * USE:      find a per-connection structure for a given l2c handle
 * PARAMS:   conn - the l2c connection handle
 * RETURN:   connection structure or NULL if not found
 * NOTES:    call with mConnsLock held
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
 * NOTES:    call with mConnsLock held
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
 * FUNCTION: gattAttSrvTxCbk
 * USE:      ATT calls this to TX to a client
 * PARAMS:   srv - the pertinent ATT server (unused)
 *           who - whom to send to
 *           s - the data to send
 * RETURN:   ATT_TX_RET_*
 * NOTES:    
 */
static uint8_t gattAttSrvTxCbk(struct attServer* srv, l2c_handle_t who, sg s)
{
    return sendQueueTx(mSendQ, who, s) ? ATT_TX_RET_ACCEPTED : ATT_TX_RET_L2C_ERR;
}

/*
 * FUNCTION: gattAttSrvIndCbk
 * USE:      ATT calls this with results of sending an indication/notification
 * PARAMS:   srv - the pertinent ATT server (unused)
 *           who - whom the indidation/notification was sent to
 *           userData - the notified value's userData
 *           success - did we succeed?
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvIndCbk(struct attServer* srv, l2c_handle_t who, void *userData, bool success)
{
    //TODO
}

/*
 * FUNCTION: gattAttSrvMtuCbk
 * USE:      ATT calls this with results of MTU negotiation
 * PARAMS:   srv - the pertinent ATT server (unused)
 *           who - whom the MTU is to
 *           mtu - the mtu to the client
 * RETURN:   NONE
 * NOTES:
 */
static void gattAttSrvMtuCbk(struct attServer* srv, l2c_handle_t who, uint32_t mtu)
{
    struct gattConnState* state;

    pthread_mutex_lock(&mConnsLock);
    state = gattConnFind(who);
    if (!state)
        loge("Unknown connection for MTU change event\n");
    else if (state->mtu > mtu)
        loge("MTU decrease not allowed\n");
    else
       state->mtu = mtu;
    pthread_mutex_unlock(&mConnsLock);
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
        attSrvSetMtu(mAttSrv, state->conn, mtu);
        gattAttSrvMtuCbk(mAttSrv, state->conn, mtu);
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
        if (attSrvDataRx(mAttSrv, state->conn, s))
            break;
        logw("Incoming GATT message was not accepted by ATT\n");
        sgFree(s);
        break;

    case L2C_STATE_CLOSED:
        attSrvNotifConnClose(mAttSrv, state->conn);
        sendQueueDelPackets(mSendQ, state->conn);
        pthread_mutex_lock(&mConnsLock);
        gattConnStructDelete(state);
        pthread_mutex_unlock(&mConnsLock);
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
    struct gattConnState *state = calloc(1, sizeof(struct gattConnState));

    if (!state)
        return SVC_ALLOC_FAIL_OTHER;

    state->conn = conn;
    state->mtu = isLE ? ATT_MTU_MIN_LE : ATT_MTU_MIN_EDR;
    /* any other init here */

    pthread_mutex_lock(&mConnsLock);
    state->next = mConns;
    if (mConns)
        mConns->prev = state;
    mConns = state;
    pthread_mutex_unlock(&mConnsLock);

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
 * USE:      Called to add GATT to SDP
 * PARAMS:   genericAttrProfileHandle - start handle for generic attribute profile
 *           genericAttrProfileLen - length of generic attribute profile
 * RETURN:   sdp handle or 0 on error
 * NOTES:
 */
static uint32_t gattSdpInit(uint16_t genericAttrProfileHandle, uint16_t genericAttrProfileLen)
{
    uint16_t genericAttrProfileEnd = genericAttrProfileHandle + genericAttrProfileLen - 1;

    uint8_t sdpDescrGatt[] = {
        //service class ID list
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_SVC_CLASS_GATT),

        //ProtocolDescriptorList
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 19,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(GATT_PSM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 9,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_ATT),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(genericAttrProfileHandle),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(genericAttrProfileEnd),

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

    mAttSrv = attSrvCreate(gattAttSrvTxCbk, gattAttSrvMtuCbk, gattAttSrvIndCbk);
    if (!mAttSrv) {
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

    /* todo: create generic attr profile, so we do not pass zeroes here: */
    mSdpHandle = gattSdpInit(0, 0);
    if (!mSdpHandle) {
        loge("Failed to register GATT with SDP\n");
        goto out_sdp;
    }

    return true;

out_sdp:
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
    attSrvDestroy(mAttSrv);

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
    sdpServiceDescriptorDel(mSdpHandle);
    l2cApiServiceFixedChUnregister(GATT_FIXEDCH, NULL, 0);
    l2cApiServicePsmUnregister(GATT_PSM, NULL, 0);
    attSrvDestroy(mAttSrv);
    workQueueWakeAll(mWorkQ, 1);
    pthread_join(mWorker, NULL);
    workQueueFree(mWorkQ, gattWorkQueueFreeItem);
    sendQueueFree(mSendQ);
}





