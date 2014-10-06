#include <string.h>
#include "aapiGattServer.h"
#include "gatt-builtin.h"
#include "gatt.h"
#include "log.h"
#include "bt.h"
#include "mt.h"


/* this grouping is not part of the GATT spec and its semantics are thus unclear. */

struct aapiGattServerService {
    int     gattSvc;
    uint8_t inst;
};

struct aapiGattServer {
    struct aapiGattServer         *next;
    struct aapiGattServer         *prev;
    bt_uuid_t                      andrUuid;
    int                            refNum;
    uint32_t                       numServices;
    struct aapiGattServerService  *services;
};




/* these not documented anywhere, for some reason */
#define GATT_TRANSPORT_LE         0
#define GATT_TRANSPORT_BR_EDR     1
#define GATT_TRANSPORT_LE_BR_EDR  2




/* our state */
static btgatt_server_callbacks_t mCbks;

static pthread_mutex_t mAapiGattSrvLock = PTHREAD_MUTEX_INITIALIZER;
static struct aapiGattServer *mServers;
static bool mInited = false;



/*
 * FUNCTION: aapiGattServerFindStructByRef
 * USE:      Find a aapiGattServer struct by reference number
 * PARAMS:   ref - the reference
 * RETURN:   the aapiGattServer struct or NULL if not found
 * NOTES:    call with mAapiGattSrvLock held
 */
static struct aapiGattServer* aapiGattServerFindStructByRef(int ref)
{
    struct aapiGattServer *ret = mServers;

    while (ret && ret->refNum != ref)
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattServerFindStructByUuid
 * USE:      Find a aapiGattServer struct by android-style uuid
 * PARAMS:   andrUuid - the uuid
 * RETURN:   the aapiGattServer struct or NULL if not found
 * NOTES:    call with mAapiGattSrvLock held
 */
static struct aapiGattServer* aapiGattServerFindStructByUuid(const bt_uuid_t *andrUuid)
{
    struct aapiGattServer *ret = mServers;

    while (ret && memcmp(&ret->andrUuid, andrUuid, sizeof(ret->andrUuid)))
        ret = ret->next;

    return ret;
}

/*
 * FUNCTION: aapiGattServerContainsServiceWithUuid
 * USE:      See if a aapiGattServer contains a service with a given UUID
 * PARAMS:   srv - the aapiGattServer
 *           uuid - the uuid in question
 *           instIdP - if non-null, instance Id will be stored here
 * RETURN:   true if found, false else
 * NOTES:    call with mAapiGattSrvLock held
 */
static bool aapiGattServerContainsServiceWithUuid(const struct aapiGattServer *srv, const struct uuid *uuid, uint8_t *instIdP)
{
    struct uuid localUuid;
    uint32_t i;

    for (i = 0; i < srv->numServices; i++) {
        if (gattServiceGetUuid(srv->services[i].gattSvc, &localUuid))
            loge("Failed to get uuid. This is not possible\n");
        else if (uuidCmp(&localUuid, uuid)) {
            if (instIdP)
                *instIdP = srv->services[i].inst;
            return true;
        }
    }

    return false;
}

/*
 * FUNCTION: aapiGattServerUuidFromAndroidUuid
 * USE:      Convert an android-style bt_uuid_t to a struct uuid
 * PARAMS:   dst - destination
 *           src - source
 * RETURN:   NONE
 * NOTES:
 */
static void aapiGattServerUuidFromAndroidUuid(struct uuid *dst, const bt_uuid_t *src)
{
    uuidReadBE(dst, src->uu);
}

/*
 * FUNCTION: aapiGattServerRegisterServer
 * USE:      Registers a GATT server application with the stack
 * PARAMS:   uuid - the uuid
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerRegisterServer(bt_uuid_t *andrUuid)
{
    bt_status_t ret = BT_STATUS_FAIL;
    int ref = 0;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (aapiGattServerFindStructByUuid(andrUuid))
        loge("Refusing to create identical GATT server\n");
    else {
        struct aapiGattServer *srv = (struct aapiGattServer*)calloc(1, sizeof(struct aapiGattServer));
        if (!srv) {
            loge("Failed to allocate GATT server\n");
            ret = BT_STATUS_NOMEM;
        } else {

            /* grab a free reference number */
            srv->refNum = mServers  ? (mServers->refNum + 1) : 1;
            while (aapiGattServerFindStructByRef(srv->refNum))
                srv->refNum++;

            /* link it in */
            srv->next = mServers;
            if (mServers)
                mServers->prev = srv;
            mServers = srv;

            /* save uuid */
            memcpy(&srv->andrUuid, andrUuid, sizeof(srv->andrUuid));

            /* prepare to announce success */
            ret = BT_STATUS_SUCCESS;
            ref = srv->refNum;
        }
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.register_server_cb(ret, ref, andrUuid);
    return ret;
}

/*
 * FUNCTION: aapiGattServerUnregisterServer
 * USE:      Unregister a server application from the stack
 * PARAMS:   server_if - the server's reference number
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerUnregisterServer(int server_if)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;

    pthread_mutex_lock(&mAapiGattSrvLock);
    srv = aapiGattServerFindStructByRef(server_if);
    if (!srv)
        logw("Server not found while attempting to unregister\n");
    else {
        uint32_t i;

        if (srv->next)
            srv->next->prev = srv->prev;
        if (srv->prev)
            srv->prev->next = srv->next;
        else
            mServers = srv->next;

        for (i = 0; i < srv->numServices; i++) {
            if (gattServiceIsRunning(srv->services[i].gattSvc)) {
                logw("Unregistering server with running services!\n");
                gattServiceStop(srv->services[i].gattSvc);
            }
            gattServiceDestroy(srv->services[i].gattSvc);
        }
        if (srv->services)
            free(srv->services);
        free(srv);
        ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    return ret;
}

/*
 * FUNCTION: aapiGattServerConnect
 * USE:      Create a connection to a remote peripheral
 * PARAMS:   server_if - the server's reference number
 *           bd_addr - remote address
 *           is_direct - XXX: TODO: WTF??
 *           transport - (BT_TRANSPORT_*)
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerConnect(int server_if, const bt_bdaddr_t *bd_addr, bool is_direct, int transport)
{
    //TODO

    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiGattServerDisconnect
 * USE:      Disconnect an established connection or cancel a pending one
 * PARAMS:   server_if - the server's reference number
 *           bd_addr - remote address
 *           conn_id - connection reference number
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerDisconnect(int server_if, const bt_bdaddr_t *bd_addr, int conn_id)
{
    //TODO

    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiGattServerReadF
 * USE:      GATT's callback for reading
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - the handle that this pertains to
 *           byteOfst - the offset into the characteristic for the read (in bytes)
 *           reason - reason for the write (ATT_READ_FOR_*)
 *           wantedLen - the length to write
 * RETURN:   NONE
 * NOTES:    call gattSrvCbkReply() with results. returning false will send ATT_ERROR_UNLIKELY_ERROR to the client
 */
static bool aapiGattServerReadF(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len)
{
    struct bt_addr peer;
    bt_bdaddr_t andrPeerAddr;

    if (!l2cApiGetBtAddr(who, &peer)) {
        loge("Failed to get peer addr for read cbk\n");
        return false;
    }

    memcpy(andrPeerAddr.address, peer.addr, sizeof(andrPeerAddr.address));
    mCbks.request_read_cb(cid, transId, &andrPeerAddr, handle, byteOfst, reason == ATT_READ_FOR_READ_BLOB);

    return true;
}

/*
 * FUNCTION: aapiGattServerWriteF
 * USE:      GATT's callback for writing
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           transId - ATT's transaction ID
 *           handle - the handle that this pertains to
 *           byteOfst - the offset into the characteristic for the write (in bytes)
 *           reason - reason for the write (ATT_WRITE_FOR_*)
 *           len - the length to write
 *           data - the data to write
 * RETURN:   NONE
 * NOTES:    call gattSrvCbkReply() with results. returning false will send ATT_ERROR_UNLIKELY_ERROR to the client
 */
static bool aapiGattServerWriteF(int svc, l2c_handle_t who, int cid, int transId, uint16_t handle, uint16_t byteOfst, uint8_t reason, uint16_t len, const void *data)
{
    struct bt_addr peer;
    bt_bdaddr_t andrPeerAddr;

    if (!l2cApiGetBtAddr(who, &peer)) {
        loge("Failed to get peer addr for write cbk\n");
        return false;
    }

    mCbks.request_write_cb(cid, transId, &andrPeerAddr, handle, byteOfst, len, true, reason == ATT_WRITE_FOR_PREPARE, (uint8_t*)data);
    return true;
}

/*
 * FUNCTION: aapiGattServerIndF
 * USE:      GATT's callback for indication/notification progress
 * PARAMS:   svc - service ref
 *           who - connection to whom this pertains to
 *           cid - ATT's connection ID
 *           handle - the handle that this pertains to
 *           evt - what happened (GATT_SRV_EVT_*)
 *           ref - value originally passed to gattServiceSendInd()
 * RETURN:   NONE
 * NOTES:
 */
static void aapiGattServerIndF(int svc, l2c_handle_t who, int cid, uint16_t handle, uint8_t evt, uint64_t ref)
{
    /* XXX: tell java somehow? Current api has no way it seems */
}

/*
 * FUNCTION: aapiGattServerAddService
 * USE:      Create a new service
 * PARAMS:   server_if - the server's reference number
 *           srvc_id - service identifying information
 *           num_handles - how many handles to allocate
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerAddService(int server_if, btgatt_srvc_id_t *srvc_id, int num_handles)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    uint16_t handle = 0;

    pthread_mutex_lock(&mAapiGattSrvLock);
    srv = aapiGattServerFindStructByRef(server_if);
    if (!srv)
        logw("Server not found while attempting to add service\n");
    else {
        struct aapiGattServerService *svcs;
        struct uuid uuid;
        int svc;

        svcs = realloc(srv->services, sizeof(struct aapiGattServerService[srv->numServices + 1]));
	if (!svcs)
            logw("Failed to make space for the service\n");
        else {
            srv->services = svcs;

            aapiGattServerUuidFromAndroidUuid(&uuid, &srvc_id->id.uuid);
            svc = gattServiceCreate(&uuid, srvc_id->is_primary, num_handles, aapiGattServerReadF, aapiGattServerWriteF, aapiGattServerIndF);
            if (!svc)
                logw("Failed to create GATT service\n");
            else {
                srv->services[srv->numServices].gattSvc = svc;
                srv->services[srv->numServices].inst = srvc_id->id.inst_id;
                srv->numServices++;
                handle = gattServiceGetHandleBaseBySvc(svc);
                ret = BT_STATUS_SUCCESS;
            }
        }
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.service_added_cb(ret, server_if, srvc_id, handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerFindService
 * USE:      Find a server and a service given their externally-visible reference numbers
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the service
 *           srvP - if not NULL, we store the struct aapiGattServer* there
 *           svc - if not NULL, we store the service reference number there
 *           svcIdxInTableP - if not NULL, we store service's index in the server's "services" table
 *           firstOnly - if set handle is service_handle, else it is any handle in the range
 * RETURN:   true if all were found
 * NOTES:    call with mAapiGattSrvLock held
 */
static bool aapiGattServerFindService(int server_if, int handle, struct aapiGattServer **srvP, int *svcP, uint32_t *svcIdxInTableP, bool firstOnly)
{
    struct aapiGattServer *srv;
    uint32_t i;
    int svc;

    srv = aapiGattServerFindStructByRef(server_if);
    if (!srv)
        return false;

    svc = gattServiceFindByHandle(handle, firstOnly);
    if (svc < 0)
        return false;

    /* verify proper belonging */
    for (i = 0; i < srv->numServices; i++)
        if (srv->services[i].gattSvc == svc)
            break;

    if (i == srv->numServices) {
        logw("Found service not aprt of found server!\n");
        return false;
    }

    if (srvP)
        *srvP = srv;

    if (svcP)
        *svcP = svc;

    if (svcIdxInTableP)
        *svcIdxInTableP = i;

    return true;
}

/*
 * FUNCTION: aapiGattServerAddIncludedService
 * USE:      Assign an included service to it's parent service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 *           included_handle - the handle of the included service
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerAddIncludedService(int server_if, int service_handle, int included_handle)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    uint16_t handle = 0;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, NULL, true))
        logw("Server/Service not found while attempting to add included service\n");
    else {
        struct uuid uuid;
        int includedSvc;

        if (!aapiGattServerFindService(server_if, included_handle, NULL, &includedSvc, NULL, true) || !gattServiceGetUuid(includedSvc, &uuid))
            logw("Failed to find or get UUID of included service\n");
        else {

            handle = gattServiceAddIncludedService(svc, &uuid);
            if (handle)
                ret = BT_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.included_service_added_cb(ret, server_if, service_handle, handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerAddCharacteristic
 * USE:      Add a characteristic to a service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 *           andrUuid - the characteristic's UUID in android format
 *           properties - characteristic's properties (in gatt terms)
 *           permissions - characteristic's permissions in android terms
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerAddCharacteristic(int server_if, int service_handle, bt_uuid_t *andrUuid, int properties, int permissions)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    uint16_t handle = 0;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, NULL, true))
        logw("Server/Service not found while attempting to add characteristic\n");
    else {
        struct uuid uuid;

        aapiGattServerUuidFromAndroidUuid(&uuid, andrUuid);
        handle = gattServiceAddChar(svc, &uuid, properties, permissions);
        if (handle)
            ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.characteristic_added_cb(ret, server_if, andrUuid, service_handle, handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerAddDescriptor
 * USE:      Add a descriptor to a given service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 *           andrUuid - the descriptor's UUID in android format
 *           permissions - characteristic's permissions in android terms
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerAddDescriptor(int server_if, int service_handle, bt_uuid_t *andrUuid, int permissions)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    uint16_t handle = 0;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, NULL, true))
        logw("Server/Service not found while attempting to add descriptor\n");
    else {
        struct uuid uuid;

        aapiGattServerUuidFromAndroidUuid(&uuid, andrUuid);
        handle = gattServiceAddCharDescr(svc, &uuid, permissions);
        if (handle)
            ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.descriptor_added_cb(ret, server_if, andrUuid, service_handle, handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerStartService
 * USE:      Starts a local service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 *           transport - which transport to use
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerStartService(int server_if, int service_handle, int transport)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, NULL, true))
        logw("Server/Service not found while attempting to start service\n");
    else {
        bool useLE = transport == GATT_TRANSPORT_LE || transport == GATT_TRANSPORT_LE_BR_EDR;
        bool useEDR = transport == GATT_TRANSPORT_BR_EDR || transport == GATT_TRANSPORT_LE_BR_EDR;

        if (!useLE && !useEDR)
            logw("Cannot bring up service given no transports\n");
        else {

            if (gattServiceStart(svc, useLE, useEDR))
                ret = BT_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.service_started_cb(ret, server_if, service_handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerStopService
 * USE:      Stops a local service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerStopService(int server_if, int service_handle)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, NULL, true))
        logw("Server/Service not found while attempting to stop service\n");
    else {
       if (gattServiceStop(svc))
            ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.service_stopped_cb(ret, server_if, service_handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerDeleteService
 * USE:      Delete a local service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerDeleteService(int server_if, int service_handle)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    uint32_t idx;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, service_handle, &srv, &svc, &idx, true))
        logw("Server/Service not found while attempting to delete service\n");
    else {
       memmove(srv->services + idx, srv->services + srv->numServices - 1, sizeof(struct aapiGattServerService));
       srv->numServices--;
       if (gattServiceDestroy(svc))
           ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    mCbks.service_deleted_cb(ret, server_if, service_handle);
    return ret;
}

/*
 * FUNCTION: aapiGattServerSendIndication
 * USE:      Send value indication to a remote device
 * PARAMS:   server_if - the server's reference number
 *           attribute_handle - the handle of the pertinent attribute
 *           conn_id - the connection reference number
 *           len - the length of the data
 *           confirm - need confirmation?
 *           p_value - the value
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerSendIndication(int server_if, int attribute_handle, int conn_id, int len, int confirm, char *p_value)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct aapiGattServer *srv;
    int svc;

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!aapiGattServerFindService(server_if, attribute_handle, &srv, &svc, NULL, false))
        logw("Server/service not found while attempting to send indication\n");
    else {
        if (gattServiceSendInd(svc, conn_id, attribute_handle, p_value, len, !!confirm, 0))
            ret = BT_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);

    /* XXX: callback here? */
    return ret;
}

/*
 * FUNCTION: aapiGattServerSendResponse
 * USE:      Send a response to a read/write operation
 * PARAMS:   conn_id - the connection reference number
 *           trans_id - transaction reference number
 *           status - the status to send
 *           response - the response to send
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerSendResponse(int conn_id, int trans_id, int status, btgatt_response_t *response)
{
    gattSrvCbkReply(conn_id, trans_id, response->handle, status, response->attr_value.value, response->attr_value.len);

    return BT_STATUS_SUCCESS;
}

/*
 * FUNCTION: aapiGattServerGetProfileIface
 * USE:      Called to get profile pointer to the GATT server profile
 * PARAMS:   NONE
 * RETURN:   pointer to profile struct
 * NOTES:
 */
void *aapiGattServerGetProfileIface(void)
{
    static const btgatt_server_interface_t iface = {
        .register_server = aapiGattServerRegisterServer,
        .unregister_server = aapiGattServerUnregisterServer,
        .connect = aapiGattServerConnect,
        .disconnect = aapiGattServerDisconnect,
        .add_service = aapiGattServerAddService,
        .add_included_service = aapiGattServerAddIncludedService,
        .add_characteristic = aapiGattServerAddCharacteristic,
        .add_descriptor = aapiGattServerAddDescriptor,
        .start_service = aapiGattServerStartService,
        .stop_service = aapiGattServerStopService,
        .delete_service = aapiGattServerDeleteService,
        .send_indication = aapiGattServerSendIndication,
        .send_response = aapiGattServerSendResponse,
    };

    return (void*)&iface;
}

/*
 * FUNCTION: aapiGattServerInit
 * USE:      Init the GATT server profile
 * PARAMS:   cbks - callbacks back into java
 * RETURN:   status
 * NOTES:
 */
bt_status_t aapiGattServerInit(const btgatt_server_callbacks_t *cbks)
{
    bt_status_t ret;


    memcpy(&mCbks, cbks, sizeof(mCbks));

    pthread_mutex_lock(&mAapiGattSrvLock);
    if (!mInited) {
        if (!attInit()) {
            loge("ATT init failed\n");
        } else if (!gattProfileInit()) {
            attDeinit();
            loge("GATT init failed\n");
        } else if (!gattBuiltinInit()) {
            gattProfileDeinit();
            attDeinit();
            loge("GATT.GAP init failed\n");
        } else
            mInited = true;
    }
    ret = mInited ? BT_STATUS_SUCCESS : BT_STATUS_FAIL;
    pthread_mutex_unlock(&mAapiGattSrvLock);

    return ret;
}

/*
 * FUNCTION: aapiGattServerDeinit
 * USE:      Deinit the GATT server profile
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void aapiGattServerDeinit(void)
{
    pthread_mutex_lock(&mAapiGattSrvLock);
    if (mInited) {
        mInited = false;
        gattBuiltinDeinit();
        gattProfileDeinit();
        attDeinit();
    }
    pthread_mutex_unlock(&mAapiGattSrvLock);
}


