#ifndef _VENDOR_LIB_H_
#define _VENDOR_LIB_H_

#include "types.h"

#define VENDOR_DATA_TYP_CMD 0
#define VENDOR_DATA_TYP_EVT 1
#define VENDOR_DATA_TYP_ACL 2
#define VENDOR_DATA_TYP_SCO 3

/* callback for chip up, first fw=false for hw up, then fw=true for fw up. If first is false, do not expect second */
typedef void (*vendorCbkChipHwUp)(void* cbkData, bool fw, bool up);

/* callback for data rx. do not block int this for long */
typedef void (*vendorCbkRx)(void* cbkData, uint8_t typ, const void *data, uint32_t len);

/* open the lib itself - done once ever */
bool vendorOpen(void);

/* bring up the chip*/
bool vendorUp(const uint8_t *addr, vendorCbkChipHwUp chipUpCbk, vendorCbkRx dataRxCbk, void *chipCbkData);

/* send - may block */
bool vendorTx(uint8_t typ, const void *data, uint32_t len);

/* bring down the chip */
void vendorDown(void);

/* close the lib itself */
void vendorClose(void);


#endif

