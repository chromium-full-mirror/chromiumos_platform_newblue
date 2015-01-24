#ifndef _AAPI_H_
#define _AAPI_H_

#include "types.h"
#include "bt.h"

#ifdef ANDROID
#include <hardware/bluetooth.h>
#else
typedef int bt_status_t;
typedef int bt_property_t;

/** Bluetooth SSP Bonding Variant */
typedef enum {
    BT_SSP_VARIANT_PASSKEY_CONFIRMATION,
    BT_SSP_VARIANT_PASSKEY_ENTRY,
    BT_SSP_VARIANT_CONSENT,
    BT_SSP_VARIANT_PASSKEY_NOTIFICATION
} bt_ssp_variant_t;

/** Bluetooth Address */
typedef struct {
    uint8_t address[6];
} __attribute__((packed))bt_bdaddr_t;
#endif

#define AAPI_BOND_STATE_FAILED     0
#define AAPI_BOND_STATE_INPROGRESS 1
#define AAPI_BOND_STATE_SUCCESS    2

void aapiAdapterStateChanged(bool on);
void aapiAdapterProperties(bt_status_t status, int num_properties, const bt_property_t *properties);
void aapiRemoteDevProperties(bt_status_t status, const bt_bdaddr_t *bd_addr, int num_properties, const bt_property_t *properties);
void aapiDevDiscoveredCbk(int num_properties, const bt_property_t *properties);
void aapiDiscoveryStateChanged(bool on);
void aapiPinReqCbk(const struct bt_addr *peer);
void aapiSspReqCbk(const struct bt_addr *peer, bt_ssp_variant_t pairing_variant, uint32_t pass_key);
void aapiBondStateChangedCbk(const struct bt_addr *peer, uint8_t state); /* AAPI_BOND_STATE_* */
void aapiAclStateChanged(bt_status_t status, const struct bt_addr *peer, bool up);
void aapiDutModeEventCbk(uint16_t opcode, const uint8_t *buf, uint8_t len);
void aapiLeTestModeEventCbk(bt_status_t status, uint16_t num_packets);



bool aapiIsStackUp(void);


#endif

