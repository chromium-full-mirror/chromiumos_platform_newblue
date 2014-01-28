#ifndef _HCI_H_
#define _HCI_H_

#define HCI_CONN_ID_MASK	0x0FFF


/* bring chip communications up or down */
bool hciUp(void);
void hciDown(void);


#endif

