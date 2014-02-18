#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include "l2cap.h"
#include "util.h"
#include "sdp.h"
#include "log.h"
#include "sg.h"

#define SDP_SDP_SERVER_UUID  0x1000

#define SDP_PSM              0x0001

#define SDP_HANDLE_SELF      0
#define SDP_HANDLE_FIRST     0x00010000
#define SDP_HANDLE_LAST      0xFFFFFFFF



struct sdpService {
    struct sdpService     *prev;
    struct sdpService     *next;
    uint32_t               handle; /* will match what's in descr, but quicker to read it here */
    uint32_t               len;
    uint8_t                descr[];
};

struct sdpInst {
    l2c_handle_t           conn;
    uint16_t               mtu;
};


/* our global state */

static pthread_rwlock_t mSvcsLock = PTHREAD_RWLOCK_INITIALIZER;
static struct sdpService *mSvcsHead = NULL;
static struct sdpService *mSvcsTail = NULL;
static uint32_t mNextHandle = SDP_HANDLE_FIRST;

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
static bool sdpServiceDescriptorAddInt(const uint8_t *svc, uint32_t len, uint32_t handle)
{
    static const unsigned handle_attr_len = 8;
    struct sdpService *ss;

    ss = malloc(sizeof(struct sdpService) + len + handle_attr_len);
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
    bool ret = false;

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
 * FUNCTION: sdpRx
 * USE:      Handle an incoming request
 * PARAMS:   inst - the instance
 *           req - the request
 * RETURN:   NONE
 * NOTES:    call with mSvcsLock held for read
 */
static void sdpRx(struct sdpInst *inst, sg req)
{
    //TODO: this
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
            sdpRx(instance, payload);
            pthread_rwlock_unlock(&mSvcsLock);
            sgFree(payload);
        }
        break;
    case L2C_STATE_CLOSED:
        logi("SDP closed\n");
        //TODO: free the instance if needed
        free(instance);
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
 * FUNCTION: sdpRegisterProfile
 * USE:      Called to init SDP and register it with L2CAP
 * PARAMS:   NONE
 * RETURN:   success
 * NOTES:
 */
bool sdpRegisterProfile(void)
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

    if (!sdpServiceDescriptorAddInt(sdpDescrSdp, sizeof(sdpDescrSdp), SDP_HANDLE_SELF)) {
        loge("SDP failed to register with itself!\n");
        return false;
    }

    if (!l2cApiServicePsmRegister(SDP_PSM, &sdpSvc)) {
        loge("Failed to register SDP service\n");
        sdpServiceDescriptorDelInt(mSvcsHead); /* we are our only record - this is safe */
        return false;
    }

    return true;
}

