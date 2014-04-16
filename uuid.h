#ifndef _UUID_H_
#define _UUID_H_

#include "types.h"

#define BT_UUID_BASE                  {0x800000805F9B34FBULL, 0x0000000000001000ULL}

#define UUIDFMT      "{%08lx-%04x-%04x-%04x-%04x%08lx}"
#define UUIDCONV(u)  (unsigned long)(((u).hi) >> 32),(unsigned)((((u).hi) >> 16) & 0xFFFF),(unsigned)(((u).hi) & 0xFFFF),\
                     (unsigned)(((u).lo) >> 48),(unsigned)((((u).lo) >> 32) & 0xFFFF),(unsigned long)(((u).lo) & 0xFFFFFFFF)


struct uuid {
    uint64_t lo;
    uint64_t hi;
} __packed;



void uuidFromUuid16(struct uuid *dst, uint16_t uuid16);
bool uuidToUuid16(uint16_t *dst, const struct uuid *uuid128);
void uuidFromUuid32(struct uuid *dst, uint32_t uuid32);
bool uuidToUuid32(uint32_t *dst, const struct uuid *uuid128);
bool uuidCmp(const struct uuid *a, const struct uuid *b);
void uuidWriteLE(void *dst, const struct uuid *src);
void uuidReadLE(struct uuid *dst, const void *src);
void uuidWriteBE(void *dst, const struct uuid *src);
void uuidReadBE(struct uuid *dst, const void *src);


#endif

