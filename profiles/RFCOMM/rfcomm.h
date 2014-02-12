#ifndef _BT_RFCOMM_H_
#define _BT_RFCOMM_H_

#include "types.h"

#define NUM_DLCIs                       62
#define RFCOMM_DLCI_PREFERENCE_NONE     0x80        /*use this param to the below */
#define RFCOMM_DLCI_NEED_EVEN           0x81
#define RFCOMM_DLCI_NEED_ODD            0x82


typedef void (*rfcommPortOpenF)(void* port, uint8_t dlci);
typedef void (*rfcommPortCloseF)(void* port, uint8_t dlci);
typedef void (*rfcommPortRxF)(void* port, uint8_t dlci, const uint8_t* buf, uint16_t sz);


void rfcommRegisterPort(uint8_t dlci, rfcommPortOpenF oF, rfcommPortCloseF cF, rfcommPortRxF rF);
void rfcommPortTx(void* port, uint8_t dlci, const uint8_t* data, uint16_t size); //makes a copy of your buffer


uint8_t rfcommReserveDlci(uint8_t preference);        //return dlci if success, zero if fail
void rfcommReleaseDlci(uint8_t dlci);


void rfcommRegisterProfile(void);


#endif

