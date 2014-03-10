#ifndef _AAPI_H_
#define _AAPI_H_

#include "types.h"

void aapiAdapterStateChanged(bool on);
void aapiAdapterProperties(bt_status_t status, int num_properties, const bt_property_t *properties);
void aapiRemoteDevProperties(bt_status_t status, const bt_bdaddr_t *bd_addr, int num_properties, const bt_property_t *properties);
void aapiDevDiscoveredCbk(int num_properties, const bt_property_t *properties);
void aapiDiscoveryStateChanged(bool on);
void aapiPinReqCbk(const bt_bdaddr_t *remote_bd_addr, const bt_bdname_t *bd_name, uint32_t devCls);
void aapiSspReqCbk(const bt_bdaddr_t *remote_bd_addr, const bt_bdname_t *bd_name, uint32_t devCls, bt_ssp_variant_t pairing_variant, uint32_t pass_key);
void aapiBondStateChangedCbk(bt_status_t status, const bt_bdaddr_t *remote_bd_addr, bt_bond_state_t state);
void aapiAclStateChanged(bt_status_t status, const bt_bdaddr_t *remote_bd_addr, bt_acl_state_t state);
void aapiDutModeEventCbk(uint16_t opcode, const uint8_t *buf, uint8_t len);
void aapiLeTestModeEventCbk(bt_status_t status, uint16_t num_packets);




#endif

