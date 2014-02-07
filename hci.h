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


struct hciNameGetInfo {
    uint8_t  mac[6];
    uint16_t clockOffset; /* if unknown, set all to zeroes except mac */
    uint8_t  PSRM;
    uint8_t  PSM; /* unused in BT 1.2+ */
};

typedef uniq_t hci_conn_t;
#define HCI_CONN_FMT      UNIQ_FMT
#define HCI_CONN_CONV(x)  UNIQ_CONV(x)

#define HCI_NAME_REQ_STATUS_IN_PROGRESS  0
#define HCI_NAME_REQ_STATUS_ERROR        1
#define HCI_NAME_REQ_STATUS_COMPLETE     2

/* various callbacks - it is universally a bad idea to call back into hci from these, except otherwise noted funcs */
/* "devCls" is devic class, rssi = 127 -> no RSSI */
typedef void (*hciDeviceDiscoveredEdrCbk)(void *cbkData, const struct bt_addr *addr, uint32_t devCls, const struct hciNameGetInfo *forNameGetting, int8_t rssi, const void *eir, uint8_t eirLen);
typedef void (*hciDeviceDiscoveredEdrNameCbk)(void *cbkData, uint8_t nameReqStatus, const char *name);
typedef void (*hciDeviceDiscoveredLeCbk)(void *cbkData, const struct bt_addr *addr, int8_t rssi, uint8_t replyType, const void *eir, uint8_t eirLen);

typedef void (*hciOpDoneCbk)(void *cbkData, uint8_t status /* 0 = good */);
typedef void (*hciAclRxCbk)(uint16_t aclConn, const void *data, uint16_t len, bool first);
typedef void (*hciScoRxCbk)(uint16_t aclConn, const void *data, uint8_t len, uint8_t lossAmount);
#define SCO_LOSS_AMOUNT_NONE    0
#define SCO_LOSS_AMOUNT_SOME    1
#define SCO_LOSS_AMOUNT_UNKNOWN 2
#define SCO_LOSS_AMOUNT_ALL     3

/* bring chip communications up or down */
bool hciUp(const uint8_t *addr, hciAclRxCbk aclRxF, hciScoRxCbk scoRxF, bool logAll);
void hciDown(void);

/* info api */
bool hciInfoSharedBuffers(void); //true if EDR & LE share buffers
bool hciInfoScoBufSize(uint16_t *bufSz, uint16_t *bufNum); //false if no SCO support
bool hciInfoAclBufSizeEdr(uint16_t *bufSz, uint16_t *bufNum); //false if no EDR support
bool hciInfoAclBufSizeLe(uint16_t *bufSz, uint16_t *bufNum); //false if no LE support

/* misc api. False on error, true if expect callback */
bool hciSetLocalName(const char *name, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetDeviceClass(uint32_t cls, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetEirData(const uint8_t *data, uint8_t dlen, bool fecRequired, hciOpDoneCbk cbkDone, void *cbkData); /* EIR for EDR */
bool hciSetAdvData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData); /* EIR for LE adv */
bool hciSetRspData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData); /* EIR for LE rsp */
bool hciSetDiscoverableConnectable(const bool *discoverableStateP, const bool *connectableStateP, hciOpDoneCbk cbkDone, void *cbkData);/* pass null to keep as is */
uniq_t hciDiscoverEdrStart(hciDeviceDiscoveredEdrCbk cbk, void *cbkData, uint32_t msec);
bool hciDiscoverEdrStop(uniq_t handle);
uniq_t hciDiscoverEdrGetName(hciDeviceDiscoveredEdrNameCbk cbk, void *cbkData, const struct hciNameGetInfo *forNameGetting); /* safe to call from hci callbacks */
bool hciDiscoverEdrGetNameCancel(uniq_t handle);
uniq_t hciDiscoverLeStart(hciDeviceDiscoveredLeCbk cbk, void *cbkData, bool active, bool useRandomAddr);
bool hciDiscoverLeStop(uniq_t handle);


#endif

