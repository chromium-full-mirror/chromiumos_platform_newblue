#ifndef _SM_H_
#define _SM_H_

#include "types.h"
#include "hci.h"
#include "sg.h"

#define L2C_FIXED_CH_NUM_SM    6

#define SM_BLOCK_LEN      16
#define SM_SIG_LEN        12

void smSignatureCalc(sg m, const uint8_t *key, uint8_t *sig);



bool smInit(uint8_t ioCapability);
void smDeinit(void);


#endif

