#ifndef _AAPI_GATT_H_
#define _AAPI_GATT_H_

#include <hardware/bluetooth.h>
#include "uuid.h"
#include "bt.h"



/* these not documented anywhere, for some reason */
#define GATT_TRANSPORT_BR_EDR     BT_TRANSPORT_BR_EDR
#define GATT_TRANSPORT_LE         BT_TRANSPORT_LE
#define GATT_TRANSPORT_LE_BR_EDR  (BT_TRANSPORT_BR_EDR | BT_TRANSPORT_LE)

void *aapiGetProfileIfaceGatt(void);
void aapiGattNotifStackState(bool up);


void aapiGattUuidFromAndroidUuid(struct uuid *dst, const bt_uuid_t *src);

#endif

