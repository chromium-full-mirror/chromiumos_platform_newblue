#ifndef _HCI_H_
#define _HCI_H_

#define HCI_CONN_ID_MASK	0x0FFF

#define HCI_PKT_TYP_CMD         0x01
#define HCI_PKT_TYP_ACL         0x02
#define HCI_PKT_TYP_SCO         0x03
#define HCI_PKT_TYP_EVT         0x04


#define HCI_EIR_DATA_LEN        240

/* various callbacks */
typedef void (*hciOpDoneCbk)(void *cbkData, bool success);

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

#endif

