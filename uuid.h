#ifndef _UUID_H_
#define _UUID_H_

#include "types.h"

#define BT_UUID_BASE                  {0x800000805F9B34FBULL, 0x0000000000001000ULL}

struct uuid {
    uint64_t lo;
    uint64_t hi;
} __packed;



void uuidFromUuid16(struct uuid *dst, uint16_t uuid16);
bool uuidCmp(const struct uuid *a, const struct uuid *b);
bool uuidToUuid16(uint16_t *dst, const struct uuid *uuid128);
void uuidWriteLE(struct uuid *dst, const struct uuid *src);
void uuidReadLE(struct uuid *dst, const struct uuid *src);
void uuidWriteBE(struct uuid *dst, const struct uuid *src);
void uuidReadBE(struct uuid *dst, const struct uuid *src);


#endif

