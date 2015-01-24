#ifndef _AAPI_GATT_CLIENT_H_
#define _AAPI_GATT_CLIENT_H_

#include <hardware/bluetooth.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_gatt_client.h>

void *aapiGattClientGetProfileIface(void);
bt_status_t aapiGattClientInit(const btgatt_client_callbacks_t *cbks);
void aapiGattClientDeinit(void);

#endif


