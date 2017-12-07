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
#define HCI_DISP_CAP_KBD_DISP    4 /* LE only! */


#define HCI_EIR_DATA_LEN        240
#define HCI_RSSI_UNKNOWN        127

#define HCI_DEV_NAME_LEN        248

/* eir data types */
#define HCI_EIR_TYPE_FLAGS                0x01
#define HCI_EIR_TYPE_INCOMPL_LIST_UUID16  0x02
#define HCI_EIR_TYPE_COMPL_LIST_UUID16    0x03
#define HCI_EIR_TYPE_INCOMPL_LIST_UUID32  0x04
#define HCI_EIR_TYPE_COMPL_LIST_UUID32    0x05
#define HCI_EIR_TYPE_INCOMPL_LIST_UUID128 0x06
#define HCI_EIR_TYPE_COMPL_LIST_UUID128   0x07
#define HCI_EIR_TYPE_SHORTENED_NAME       0x08
#define HCI_EIR_TYPE_FULL_NAME            0x09
#define HCI_EIR_TX_POWER_LEVEL            0x0A
#define HCI_EIR_DEV_CLS                   0x0D
#define HCI_EIR_SSP_C                     0x0E
#define HCI_EIR_SSP_R                     0x0F
#define HCI_EIR_SM_OOB_FLAGS              0x11
#define HCI_EIR_FLAVE_CONN_INTS           0x12
#define HCI_EIR_SVC_SOLICITS_UUID16       0x14
#define HCI_EIR_SVC_SOLICITS_UUID128      0x15
#define HCI_EIR_SVC_DATA_UUID16           0x16
#define HCI_EIR_PUBLIC_TARGET_ADDR        0x17
#define HCI_EIR_RANDOM_TARGET_ADDR        0x18
#define HCI_EIR_APPEARANCE                0x19
#define HCI_EIR_ADV_INT                   0x1A
#define HCI_EIR_LE_BT_DEV_ADDR            0x1B
#define HCI_EIR_LE_ROLE                   0x1C
#define HCI_EIR_SSP_C_256                 0x1D
#define HCI_EIR_SSP_R_256                 0x1E
#define HCI_EIR_SVC_SOLICITS_UUID32       0x1F
#define HCI_EIR_SVC_DATA_UUID32           0x20
#define HCI_EIR_SVC_DATA_UUID128          0x21
#define HCI_EIR_3D_INFO_DATA              0x3D
#define HCI_EIR_MANUF_DATA                0xFF



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
#define HCI_ADV_TYPE_ADV_DIRECT_IND_LOW_DUTY                     0x04 /* connectable directed, low duty cycle */
#define HCI_ADV_TYPE_SCAN_RSP                                    0x04 /* scan response */

#define HCI_ADV_OWN_ADDR_TYPE_PUBLIC                             0x00 /* public address */
#define HCI_ADV_OWN_ADDR_TYPE_RANDOM                             0x01 /* random address */
#define HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_PUBLIC            0x02 /* generate resolvable from local IRK, if none, use public address */
#define HCI_ADV_OWN_ADDR_TYPE_AUTO_RESOLV_ELSE_RANDOM            0x03 /* generate resolvable from local IRK, if none, use random address */

#define HCI_ADV_CHAN_MAP_USE_CHAN_37                             0x01 /* use channel 37 */
#define HCI_ADV_CHAN_MAP_USE_CHAN_38                             0x02 /* use channel 37 */
#define HCI_ADV_CHAN_MAP_USE_CHAN_39                             0x04 /* use channel 37 */

#define HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_ALL                  0x00 /* scan: from all, connect: from all */
#define HCI_ADV_FILTER_POL_SCAN_LIST_CONNECT_ALL                 0x01 /* scan: from whitelisted, connect: from all */
#define HCI_ADV_FILTER_POL_SCAN_ALL_CONNECT_LIST                 0x02 /* scan: from all, connect: from whitelisted */
#define HCI_ADV_FILTER_POL_SCAN_LIST_CONNECT_LIST                0x03 /* scan: from whitelisted, connect: from whitelisted */


/* for printing addrs and macs */
#define MACFMT                "%02X:%02X:%02X:%02X:%02X:%02X"
#define MACCONV(x)            (x)[5],(x)[4],(x)[3],(x)[2],(x)[1],(x)[0]
#define ADDRFMT               "%c-"MACFMT
#define ADDRCONV(x)           (((x).type == BT_ADDR_TYPE_EDR) ? 'E' : ((x).type == BT_ADDR_TYPE_LE_PUBLIC ? 'P' : ((x).type == BT_ADDR_TYPE_LE_RANDOM ? 'R' : '?'))) , MACCONV((x).addr)
#define CLSFMT                "0x%02X%02X%02X"
#define CLSCONV(x)            (x)[2],(x)[1],(x)[0]

typedef uniq_t hci_conn_t;
#define HCI_CONN_FMT      UNIQ_FMT
#define HCI_CONN_CONV(x)  UNIQ_CONV(x)

#define HCI_NAME_REQ_STATUS_IN_PROGRESS  0
#define HCI_NAME_REQ_STATUS_ERROR        1
#define HCI_NAME_REQ_STATUS_COMPLETE     2

/* various callbacks - it is universally a bad idea to call back into hci from these, except otherwise noted funcs */
/* rssi = 127 -> no RSSI */
typedef void (*hciDeviceDiscoveredLeCbk)(void *cbkData, const struct bt_addr *addr, int8_t rssi, uint8_t replyType, const void *eir, uint8_t eirLen);

typedef void (*hciOpDoneCbk)(void *cbkData, uint8_t status /* 0 = good */);

/* bring chip communications up or down (also query)*/
bool hciUp(const uint8_t *addr);
void hciDown(void);

/* test if stack is up or needs bringup */
bool hciIsUp(void);

/* info api */
bool hciInfoSharedBuffers(void); //true if EDR & LE share buffers
bool hciInfoAclBufSizeLe(uint16_t *bufSz, uint16_t *bufNum); //false if no LE support
void hciGetLocalAddress(uint8_t *addr);

/* misc api. False on error, true if expect callback */
bool hciSetAdvertiseEnable(bool on, hciOpDoneCbk cbkDone, void *cbkData);
bool hciSetAdvData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData); /* EIR for LE adv */
bool hciSetRspData(const uint8_t *data, uint8_t dlen, hciOpDoneCbk cbkDone, void *cbkData); /* EIR for LE rsp */
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
bool hciDemandEncr(hci_conn_t aclConn, bool demandMitmSafe);
/* LE connection info */
bool hciLeConnGetInfo(hci_conn_t aclConn, bool *masterP, uint64_t *peerFtrsP, uint16_t *intP, uint16_t *latP, uint16_t *toP, uint8_t *mcaP);
/* LE adv */
bool hciLeSetAdvParams(uint16_t advIntervalMin, uint16_t advIntervalMax, uint8_t advType, uint8_t ownAddressType, uint8_t directRandomAddress, const uint8_t *directAddr, uint8_t advChannelMap, uint8_t advFilterPolicy);

/* misc */
void hciInquiryUpdatePersistDeviceDb(const struct bt_addr *addr, const uint32_t *devClsP, const void *eir, uint8_t eirLen);

#endif

