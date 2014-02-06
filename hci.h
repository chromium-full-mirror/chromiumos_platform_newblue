#ifndef _HCI_H_
#define _HCI_H_

#include "types.h"
#include "uniq.h"
#include "bt.h"

#define HCI_CONN_ID_MASK	0x0FFF

#define HCI_PKT_TYP_CMD         0x01
#define HCI_PKT_TYP_ACL         0x02
#define HCI_PKT_TYP_SCO         0x03
#define HCI_PKT_TYP_EVT         0x04


#define HCI_EIR_DATA_LEN        240
#define HCI_RSSI_UNKNOWN        127

#define HCI_ADV_TYPE_ADV_IND                                     0x00 /* connectable undirected */
#define HCI_ADV_TYPE_ADV_DIRECT_IND                              0x01 /* connectable directed */
#define HCI_ADV_TYPE_ADV_SCAN_IND                                0x02 /* scannable undirected */
#define HCI_ADV_TYPE_ADV_NONCONN_IND                             0x03 /* non-connectable undirected */
#define HCI_ADV_TYPE_SCAN_RSP                                    0x04 /* scan response */

struct hciOpaqueDiscId;

/* various callbacks */
/* "devCls" is devic class "forNameGetting" is only valid in callback - do not save it. rssi = 127 -> no RSSI */
typedef void (*hciDeviceDiscoveredEdrCbk)(void *cbkData, const struct bt_addr *addr, uint32_t devCls, const struct hciOpaqueDiscId *forNameGetting, int8_t rssi, const void *eir, uint8_t eirLen);
typedef void (*hciDeviceDiscoveredEdrNameCbk)(void *cbkData, const char *name);
typedef void (*hciDeviceDiscoveredLeCbk)(void *cbkData, const struct bt_addr *addr, int8_t rssi, uint8_t replyType, const void *eir, uint8_t eirLen);

typedef void (*hciOpDoneCbk)(void *cbkData, uint8_t status /* 0 = good */);
typedef void (*hciAclRxCbk)(uint16_t aclConn, const void *data, uint32_t len, bool first);
typedef void (*hciScoRxCbk)(uint16_t aclConn, const void *data, uint8_t len, uint8_t lossAmount);
#define SCO_LOSS_AMOUNT_NONE    0
#define SCO_LOSS_AMOUNT_SOME    1
#define SCO_LOSS_AMOUNT_UNKNOWN 2
#define SCO_LOSS_AMOUNT_ALL     3

/* bring chip communications up or down */
bool hciUp(const uint8_t *addr, hciAclRxCbk aclRxF, hciScoRxCbk scoRxF);
void hciDown(void);


/* misc api. False on error, true if expect callback */
bool hciSetLocalName(const char *name, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetDeviceClass(uint32_t cls, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetEirData(const uint8_t *data, uint8_t dlen, bool fecRequired, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetDiscoverableConnectable(const bool *discoverableStateP, const bool *connectableStateP, hciOpDoneCbk cbkDone, void *cbkData);/* pass null to keep as is */

uniq_t hciDiscoverEdrStart(hciDeviceDiscoveredEdrCbk cbk, void *cbkData, uint32_t msec);
bool hciDiscoverEdrStop(uniq_t handle);
uniq_t hciDiscoverEdrGetName(hciDeviceDiscoveredEdrNameCbk cbk, void *cbkData, const struct hciOpaqueDiscId *forNameGetting);
bool hciDiscoverEdrGetNameCancel(uniq_t handle);

uniq_t hciDiscoverLeStart(hciDeviceDiscoveredLeCbk cbk, void *cbkData, bool active, bool useRandomAddr);
bool hciDiscoverLeStop(uniq_t handle);


#endif

