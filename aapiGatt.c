#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include "aapiGatt.h"
#include "aapiGattClient.h"
#include "aapiGattServer.h"
#include "log.h"



/*
 * FUNCTION: aapiGattInit
 * USE:      Called by java to init GATT
 * PARAMS:   callbacks - callbacks back into java
 * RETURN:   status
 * NOTES:
 */
static bt_status_t aapiGattInit(const btgatt_callbacks_t* callbacks)
{
    bt_status_t ret;

    ret = aapiGattClientInit(callbacks->client);
    if (ret != BT_STATUS_SUCCESS)
        return ret;

    ret = aapiGattServerInit(callbacks->server);
    if (ret == BT_STATUS_SUCCESS)
        return ret;

    aapiGattClientDeinit();
    return ret;
}

/*
 * FUNCTION: aapiGattCleanup
 * USE:      Called by java to deinit GATT
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void aapiGattCleanup(void)
{
    aapiGattServerDeinit();
    aapiGattClientDeinit();
}

/*
 * FUNCTION: aapiGetProfileIfaceGatt
 * USE:      Called by java to get GATT profile
 * PARAMS:   NONE
 * RETURN:   pointer to the profile struct
 * NOTES:
 */
void *aapiGetProfileIfaceGatt(void)
{
    static btgatt_interface_t gatt = {
        .size = sizeof(btgatt_interface_t),
        .init = aapiGattInit,
        .cleanup = aapiGattCleanup,
    };

    gatt.client = aapiGattClientGetProfileIface();
    gatt.server = aapiGattServerGetProfileIface();

    return &gatt;
}
