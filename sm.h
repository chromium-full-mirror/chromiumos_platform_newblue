#ifndef _SM_H_
#define _SM_H_

#include "types.h"


#define SM_ENCRYPT_DATA_LEN      16


void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);






#endif

