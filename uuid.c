#include "uuid.h"
#include "util.h"





/*
 * FUNCTION: uuidCmp
 * USE:      compare two uuid-128 values
 * PARAMS:   a - one value
 *           b - the other value
 * RETURN:   true if they are equal
 * NOTES:
 */
bool uuidCmp(const struct uuid *a, const struct uuid *b)
{
    return a->lo == b->lo && a->hi == b->hi;
}

/*
 * FUNCTION: uuidFromUuid16
 * USE:      convert a uuid-16 into a uuid-128
 * PARAMS:   dst - where to store the resulting uuid-128
 *           uuid16 - the incoming uuid16
 * RETURN:   NONE
 * NOTES:
 */
void uuidFromUuid16(struct uuid *dst, uint16_t uuid16)
{
    const struct uuid base = BT_UUID_BASE;

    dst->lo = base.lo;
    dst->hi = base.hi + (((uint64_t)uuid16) << 32);
}

/*
 * FUNCTION: uuidToUuid16
 * USE:      try to convert a uuid-128 into a uuid-16
 * PARAMS:   dst - where to store the result or NULL if we don't need it
 *           uuid128 - the input
 * RETURN:   true if we succeeded
 * NOTES:
 */
bool uuidToUuid16(uint16_t *dst, const struct uuid *uuid128)
{
    const struct uuid base = BT_UUID_BASE;

    if (uuid128->lo ^ base.lo)
        return false;
    if ((uuid128->hi ^ base.hi) & 0xFFFF0000FFFFFFFFULL)
        return false;
    if (dst)
        *dst = base.hi >> 32;
    return true;
}


/*
 * FUNCTION: uuidFromUuid32
 * USE:      convert a uuid-32 into a uuid-128
 * PARAMS:   dst - where to store the resulting uuid-128
 *           uuid32 - the incoming uuid32
 * RETURN:   NONE
 * NOTES:
 */
void uuidFromUuid32(struct uuid *dst, uint32_t uuid32)
{
    const struct uuid base = BT_UUID_BASE;

    dst->lo = base.lo;
    dst->hi = base.hi + (((uint64_t)uuid32) << 32);
}

/*
 * FUNCTION: uuidToUuid32
 * USE:      try to convert a uuid-128 into a uuid-32
 * PARAMS:   dst - where to store the result or NULL if we don't need it
 *           uuid128 - the input
 * RETURN:   true if we succeeded
 * NOTES:
 */
bool uuidToUuid32(uint32_t *dst, const struct uuid *uuid128)
{
    const struct uuid base = BT_UUID_BASE;

    if (uuid128->lo ^ base.lo)
        return false;
    if ((uuid128->hi ^ base.hi) & 0x00000000FFFFFFFFULL)
        return false;
    if (dst)
        *dst = base.hi >> 32;
    return true;
}

/*
 * FUNCTION: uuidWriteLE
 * USE:      Write a UUID-128-LE (for TX path)
 * PARAMS:   dst - where to write
 *           src - where to get the value
 * RETURN:   NONE
 * NOTES:
 */
void uuidWriteLE(struct uuid *dst, const struct uuid *src)
{
    utilSetLE64(&dst->lo, src->lo);
    utilSetLE64(&dst->hi, src->hi);
}

/*
 * FUNCTION: uuidReadLE
 * USE:      Read a UUID-128-LE (for RX path)
 * PARAMS:   dst - where to read to
 *           src - where to get the value
 * RETURN:   true if they are equal
 * NOTES:
 */
void uuidReadLE(struct uuid *dst, const struct uuid *src)
{
    dst->lo = utilGetLE64(&src->lo);
    dst->hi = utilGetLE64(&src->hi);
}


/*
 * FUNCTION: uuidWriteBE
 * USE:      Write a UUID-128-BE (for TX path)
 * PARAMS:   dst - where to write
 *           src - where to get the value
 * RETURN:   NONE
 * NOTES:
 */
void uuidWriteBE(struct uuid *dst, const struct uuid *src)
{
    utilSetLE64(&dst->lo, src->hi);
    utilSetLE64(&dst->hi, src->lo);
}

/*
 * FUNCTION: uuidReadBE
 * USE:      Read a UUID-128-BE (for RX path)
 * PARAMS:   dst - where to read to
 *           src - where to get the value
 * RETURN:   true if they are equal
 * NOTES:
 */
void uuidReadBE(struct uuid *dst, const struct uuid *src)
{
    dst->hi = utilGetLE64(&src->lo);
    dst->lo = utilGetLE64(&src->hi);
}


