#ifndef _AAPI_GATT_H_
#define _AAPI_GATT_H_

#include "uuid.h"
#include <hardware/bluetooth.h>

void *aapiGetProfileIfaceGatt(void);
void aapiGattNotifStackState(bool up);


void aapiGattUuidFromAndroidUuid(struct uuid *dst, const bt_uuid_t *src);

#endif

