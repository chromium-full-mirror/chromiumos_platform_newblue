#ifndef _UTIL_H_
#define _UTIL_H_

#include <semaphore.h>
#include "types.h"


/* handles EINTR */
int r_sem_wait(sem_t* sem);
int r_sem_trywait(sem_t* sem);

/* big endian getters return value */
static uint8_t utilGetBE8(const void *buf);
static uint16_t utilGetBE16(const void *buf);
static uint32_t utilGetBE24(const void *buf);
static uint32_t utilGetBE32(const void *buf);
static uint64_t utilGetBE64(const void *buf);

/* big endian setters*/
static void utilSetBE8(void *buf, uint8_t val);
static void utilSetBE16(void *buf, uint16_t val);
static void utilSetBE24(void *buf, uint32_t val);
static void utilSetBE32(void *buf, uint32_t val);
static void utilSetBE64(void *buf, uint64_t val);

/* little endian getters return value */
static uint8_t utilGetLE8(const void *buf);
static uint16_t utilGetLE16(const void *buf);
static uint32_t utilGetLE24(const void *buf);
static uint32_t utilGetLE32(const void *buf);
static uint64_t utilGetLE64(const void *buf);

/* little endian setters */
static void utilSetLE8(void *buf, uint8_t val);
static void utilSetLE16(void *buf, uint16_t val);
static void utilSetLE24(void *buf, uint32_t val);
static void utilSetLE32(void *buf, uint32_t val);
static void utilSetLE64(void *buf, uint64_t val);


#ifndef __packed
# define __packed __attribute__ ((__packed__))
#endif


/* internal use only */

static uint64_t utilGetBE(const void *buf, uint8_t bits);
static void utilSetBE(void *buf, uint8_t bits, uint64_t val);
static uint64_t utilGetLE(const void *buf, uint8_t bits);
static void utilSetLE(void *buf, uint8_t bits, uint64_t val);


#define NUM_GETTER(endianness, bits_use, bits_type)                               \
    static uint##bits_type##_t utilGet##endianness##bits_use(const void *buf)     \
    {                                                                             \
        return utilGet ## endianness (buf, bits_use);                             \
    }

#define NUM_SETTER(endianness, bits_use, bits_type)                               \
    static void utilSet##endianness##bits_use(void *buf, uint##bits_type##_t val) \
    {                                                                             \
        utilSet ## endianness (buf, bits_use, val);                               \
    }

/* create big-endian getters */
NUM_GETTER(BE, 8, 8)
NUM_GETTER(BE, 16, 16)
NUM_GETTER(BE, 24, 32)
NUM_GETTER(BE, 32, 32)
NUM_GETTER(BE, 64, 64)

/* create big-endian setters */
NUM_SETTER(BE, 8, 8)
NUM_SETTER(BE, 16, 16)
NUM_SETTER(BE, 24, 32)
NUM_SETTER(BE, 32, 32)
NUM_SETTER(BE, 64, 64)

/* create little-endian getters */
NUM_GETTER(LE, 8, 8)
NUM_GETTER(LE, 16, 16)
NUM_GETTER(LE, 24, 32)
NUM_GETTER(LE, 32, 32)
NUM_GETTER(LE, 64, 64)

/* create little-endian setters */
NUM_SETTER(LE, 8, 8)
NUM_SETTER(LE, 16, 16)
NUM_SETTER(LE, 24, 32)
NUM_SETTER(LE, 32, 32)
NUM_SETTER(LE, 64, 64)


#undef NUM_GETTER
#undef NUM_SETTER



static uint64_t utilGetBE(const void *buf, uint8_t bits)
{
    const uint8_t *ptr = (const uint8_t*)buf;
    uint64_t val = 0;
    uint8_t bytes = bits / 8;


    while (bytes--)
        val = (val << 8) | *ptr++;

    return val;
}

static void utilSetBE(void *buf, uint8_t bits, uint64_t val)
{
    uint8_t *ptr = (uint8_t*)buf;
    uint8_t bytes = bits / 8;

    ptr += bytes;
    while (bytes--) {
        *--ptr = val;
        val >>= 8;
    }
}

static uint64_t utilGetLE(const void *buf, uint8_t bits)
{
    const uint8_t *ptr = (const uint8_t*)buf;
    uint64_t val = 0;
    uint8_t bytes = bits / 8;

    ptr += bytes;

    while (bytes--)
        val = (val << 8) | *--ptr;

    return val;
}

static void utilSetLE(void *buf, uint8_t bits, uint64_t val)
{
    uint8_t *ptr = (uint8_t*)buf;
    uint8_t bytes = bits / 8;

    while (bytes--) {
        *ptr++ = val;
        val >>= 8;
    }
}


#endif

