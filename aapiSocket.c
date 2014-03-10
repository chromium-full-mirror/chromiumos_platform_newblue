#include <hardware/bluetooth.h>
#include <hardware/bt_sock.h>
#include "aapiSocket.h"


/*
 * FUNCTION: btSockListen
 * USE:      Listen for connections from a peer on a socket
 * PARAMS:   type - tocket type
 *           service_name - service name to advertise
 *           service_uuid - service uuid to advertise
 *           channel - channel number
 *           sock_fd - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t btSockListen(btsock_type_t type, const char* service_name, const uint8_t* service_uuid, int channel, int* sock_fd, int flags)
{
    //TODO
    return 0;
}

/*
 * FUNCTION: btSockConnect
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - tocket type
 *           uuid - service uuid
 *           channel - channel number
 *           sock_fd - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t btSockConnect(const bt_bdaddr_t *bd_addr, btsock_type_t type, const uint8_t* uuid, int channel, int* sock_fd, int flags)
{
    //TODO
    return 0;
}

const void* aapiGetProfileIfaceSockets(void)
{
    static const btsock_interface_t pSockets = {
        .size = sizeof(btsock_interface_t),
        .listen = btSockListen,
        .connect = btSockConnect,
    };

    return &pSockets;
}
