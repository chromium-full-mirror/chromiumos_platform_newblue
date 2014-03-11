#ifndef _AAPI_GATT_SERVER_H_
#define _AAPI_GATT_SERVER_H_

#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_gatt_server.h>

void *aapiGattServerGetProfileIface(void);
bt_status_t aapiGattServerInit(const btgatt_server_callbacks_t *cbks);
void aapiGattServerDeinit(void);

#endif


