#include <string.h>
#include "aapiGattServer.h"
#include "gatt.h"


/* our state */
static btgatt_server_callbacks_t mCbks;



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
    //TODO. WTF??
    return BT_STATUS_FAIL;
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
    //TODO. WTF??
    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiGattServerConnect
 * USE:      Create a connection to a remote peripheral
 * PARAMS:   server_if - the server's reference number
 *           bd_addr - remote address
 *           is_direct - XXX: TODO: WTF??
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerConnect(int server_if, const bt_bdaddr_t *bd_addr, bool is_direct)
{
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
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiGattServerStartService
 * USE:      Starts a local service
 * PARAMS:   server_if - the server's reference number
 *           service_handle - the handle of the current service
 *           transport - XXX: TODO: WTF??
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerStartService(int server_if, int service_handle, int transport)
{
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
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
    //TODO
    return BT_STATUS_FAIL;
}

/*
 * FUNCTION: aapiGattServerSendResponse
 * USE:      Send a response to a read/write operatio
 * PARAMS:   conn_id - the connection reference number
 *           trans_id - transaction reference number
 *           status - the status to send
 *           response - the response to send
 * RETURN:   status
 * NOTES:    also calls callback with result if successful result here
 */
static bt_status_t aapiGattServerSendResponse(int conn_id, int trans_id, int status, btgatt_response_t *response)
{
    //TODO
    return BT_STATUS_FAIL;
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
    memcpy(&mCbks, cbks, sizeof(mCbks));

    //TODO
    return BT_STATUS_SUCCESS;
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
    //TODO
}


