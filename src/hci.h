#ifndef _HCI_H_
#define _HCI_H_

#define HCI_CONN_ID_MASK	0x0FFF

#define HCI_PKT_TYP_CMD         0x01
#define HCI_PKT_TYP_ACL         0x02
#define HCI_PKT_TYP_SCO         0x03
#define HCI_PKT_TYP_EVT         0x04


/* bring chip communications up or down */
bool hciUp(void);
void hciDown(void);


#endif

