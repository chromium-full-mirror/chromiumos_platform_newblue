#ifndef _VENDOR_LIB_H_
#define _VENDOR_LIB_H_

#include "types.h"
#include "sg.h"


/* callback for chip up, first fw=false for hw up, then fw=true for fw up. If first is false, do not expect second */
typedef void (*vendorCbkChipUp)(void* cbkData, bool fw, bool up);

/* callback for data rx. do not block in this for long */
typedef void (*vendorCbkRx)(void* cbkData, uint8_t typ, sg data);

/* open the lib itself - done once ever */
bool vendorOpen(void);

/* enable/disable logging */
void vendorLogEnable(bool on);

/* bring up the chip*/
bool vendorUp(const uint8_t *addr, vendorCbkChipUp chipUpCbk, vendorCbkRx dataRxCbk, void *chipCbkData);

/* send - may block */
bool vendorTx(uint8_t typ, sg data, bool expectReply);

/* notify vendor lib that we aren't waiting for any events */
void vendorTxDoneWithEvts(void);

/* bring down the chip */
void vendorDown(void);

/* close the lib itself */
void vendorClose(void);


#endif

