#ifndef _HCI_H_
#define _HCI_H_

#include "types.h"
#include "uniq.h"
#include "bt.h"
#include "sg.h"

#define HCI_CONN_ID_MASK	0x0FFF

#define HCI_PKT_TYP_CMD         0x01
#define HCI_PKT_TYP_ACL         0x02
#define HCI_PKT_TYP_SCO         0x03
#define HCI_PKT_TYP_EVT         0x04

#define HCI_PIN_MAX_LEN         16
#define HCI_LINK_KEY_LEN        16
#define HCI_OOB_LEN             16

#define HCI_SSP_NUMERIC_INVALID 1000000UL
#define HCI_SSP_ENTRY_STARTED   0
#define HCI_SSP_ENTRY_DIGIT_ADD 1
#define HCI_SSP_ENTRY_DIGIT_SUB 2
#define HCI_SSP_ENTRY_ERASED    3
#define HCI_SSP_ENTRY_DONE      4

#define HCI_DISP_CAP_DISP_ONLY   0
#define HCI_DISP_CAP_DISP_YES_NO 1
#define HCI_DISP_CAP_KBD_ONLY    2
#define HCI_DISP_CAP_NONE        3


#define HCI_EIR_DATA_LEN        240
#define HCI_RSSI_UNKNOWN        127

#define HCI_AUTH_REQMENT_NO_MITM_PROT_NO_BOND                    0
#define HCI_AUTH_REQMENT_MITM_PROT_NO_BOND                       1
#define HCI_AUTH_REQMENT_NO_MITM_PROT_DEDICATED_BOND             2
#define HCI_AUTH_REQMENT_MITM_PROT_DEDICATED_BOND                3
#define HCI_AUTH_REQMENT_NO_MITM_PROT_GENERAL_BOND               4
#define HCI_AUTH_REQMENT_MITM_PROT_GENERAL_BOND                  5
#define HCI_AUTH_REQMENT_CANCEL_SSP                              0xFF

#define HCI_KEY_TYPE_COMBINATION                                 0x00 /* BT 1.1+ */
#define HCI_KEY_TYPE_LOCAL_UNIT                                  0x01 /* BT 1.1+ */
#define HCI_KEY_TYPE_REMOTE_UNIT                                 0x02 /* BT 1.1+ */
#define HCI_KEY_TYPE_DEBUG_COMBINATION                           0x03 /* BT 2.1+ */
#define HCI_KEY_TYPE_UNAUTHENTICATED_COMBO_P192                  0x04 /* BT 2.1+ */
#define HCI_KEY_TYPE_AUTHENTICATED_COMBO_P192                    0x05 /* BT 2.1+ */
#define HCI_KEY_TYPE_CHANGED_COMBO                               0x06 /* BT 2.1+ */
#define HCI_KEY_TYPE_UNAUTHENTICATED_COMBO_P256                  0x07 /* BT 4.1+ */
#define HCI_KEY_TYPE_AUTHENTICATED_COMBO_P256                    0x08 /* BT 4.1+ */

#define HCI_ADV_TYPE_ADV_IND                                     0x00 /* connectable undirected */
#define HCI_ADV_TYPE_ADV_DIRECT_IND                              0x01 /* connectable directed */
#define HCI_ADV_TYPE_ADV_SCAN_IND                                0x02 /* scannable undirected */
#define HCI_ADV_TYPE_ADV_NONCONN_IND                             0x03 /* non-connectable undirected */
#define HCI_ADV_TYPE_SCAN_RSP                                    0x04 /* scan response */


/* for printing addrs and macs */
#define MACFMT                "%02X:%02X:%02X:%02X:%02X:%02X"
#define MACCONV(x)            (x)[5],(x)[4],(x)[3],(x)[2],(x)[1],(x)[0]
#define ADDRFMT               "%c-"MACFMT
#define ADDRCONV(x)           (((x).type == BT_ADDR_TYPE_EDR) ? 'E' : ((x).type == BT_ADDR_TYPE_LE_PUBLIC ? 'P' : ((x).type == BT_ADDR_TYPE_LE_RANDOM ? 'R' : '?'))) , MACCONV((x).addr)
#define CLSFMT                "0x%02X%02X%02X"
#define CLSCONV(x)            (x)[2],(x)[1],(x)[0]


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
#define SCO_LOSS_AMOUNT_NONE    0
#define SCO_LOSS_AMOUNT_SOME    1
#define SCO_LOSS_AMOUNT_UNKNOWN 2
#define SCO_LOSS_AMOUNT_ALL     3

/* bring chip communications up or down */
bool hciUp(const uint8_t *addr, bool logAll);
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

#define HCI_TX_SEND_OK          0
#define HCI_TX_SEND_NO_CREDITS  1
#define HCI_TX_SEND_NO_CONN     2
#define HCI_TX_SEND_ERROR       3

#define HCI_BOUNDARY_COMPLETE   0
#define HCI_BOUNDARY_START      1
#define HCI_BOUNDARY_CONT       2

/* connect using default settings */
bool hciConnect(const struct bt_addr* addr);
/* disconnect/cancel connection attempt */
bool hciDisconnect(const struct bt_addr* addr);
/* try to send data -> HCI_TX_SEND_*   boundary = HCI_BOUNDARY_* */
uint8_t hciTryToTx(hci_conn_t aclConn, sg data, uint8_t boundary);
/* LE connection update */
bool hciUpdateLeParams(hci_conn_t aclConn, uint16_t minInt, uint16_t maxInt, uint16_t lat, uint16_t to);
/* request encryption */
bool hciDemandEncr(hci_conn_t aclConn);

/* connection request handling */
bool hciConnRequestReplyYes(const struct bt_addr *addr, bool becomeMaster);
bool hciConnRequestReplyNo(const struct bt_addr *addr);


#endif

