#include "util.h"
#include "log.h"
#include "sm.h"
#include "sg.h"


/* fwd decls */
static void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);









/*
 * FUNCTION: smCmacDo
 * USE:      Perform the CMAC calculation
 * PARAMS:   dst - where the store the result
 *           macLen - how long of a digest do we want (in bytes)
 *           m - the message to digest
 *           k - the key to use
 *           k1 - subkey 1 (from smCmacPrepare)
 *           k2 - subkey 2 (from smCmacPrepare)
 * RETURN:   NONE
 * NOTES:
 */
static void smCmacDo(uint8_t *dst, uint8_t macLen, sg m, const uint8_t *k, const uint8_t *k1, const uint8_t *k2)
{
    uint32_t len = sgLength(m), pos = 0;
    uint32_t i, nblocks = len ? (len + SM_BLOCK_LEN - 1) / SM_BLOCK_LEN : 1;
    uint8_t tmp1[SM_BLOCK_LEN] = {0,}, tmp2[SM_BLOCK_LEN], j, *in = tmp1, *out = tmp2, *swap;

    for (i = 0; i < nblocks; i++) {
        for(j = 0; j < len && j < SM_BLOCK_LEN; j++) {
            uint8_t byte;

            sgSerialize(m, pos++, sizeof(uint8_t),&byte);
            in[j] ^= byte;
        }
        len -= j;
        if (!len) {
            if (j == SM_BLOCK_LEN) {
                for(j = 0; j < SM_BLOCK_LEN; j++)
                    in[j] ^= *k1++;
            } else {
                in[j] ^= 0x80;
                for(j = 0; j < SM_BLOCK_LEN; j++)
                    in[j] ^= *k2++;
            }
        }
        smEncrypt(out, in, k);
        swap = in;
        in = out;
        out = swap;
    }
    while (macLen--)
        *dst++ = *in++;
}

/*
 * FUNCTION: smCmacPrepare
 * USE:      Perform the pre-calculation of K1 & K2 for the CMAC
 * PARAMS:   k1 - where to store k1
 *           k2 - where to store k2
 *           k - the k input value
 * RETURN:   NONE
 * NOTES:
 */
static void smCmacPrepare(uint8_t *k1, uint8_t *k2, const uint8_t *k)
{
    uint8_t tmp[SM_BLOCK_LEN] = {0,}, *out = k1, i, j;

    smEncrypt(k1, tmp, k);
    for (i = 0; i < 2; i++) {
        bool shiftOut;
        shiftOut = !!(k1[0] & 0x80);
        for (j = 0; j < 15; j++)
            out[j] = (k1[j] << 1) | (k1[j + 1] >> 7);
        out[15] = k1[15] << 1;
        if (shiftOut)
            out[15] ^= 0x87;
        out = k2;
    }
}

/*
 * FUNCTION: smSignatureCalc
 * USE:      Perform a CMAC calculation in its entirety
 * PARAMS:   data - the data to checksum
 *           len - length of said data
 *           key - the key
 *           sig - sig gets stored here
 * RETURN:   NONE
 * NOTES:
 */
void smSignatureCalc(sg m, const uint8_t *key, uint8_t *sig)
{
    uint8_t k1[SM_BLOCK_LEN], k2[SM_BLOCK_LEN];

    smCmacPrepare(k1, k2, key);
    smCmacDo(sig, SM_SIG_LEN, m, key, k1, k2);
}

/*
 * FUNCTION: smCalcKey
 * USE:      Perform the calculation of a key
 * PARAMS:   out - store the result here
 *           k - the k value as per spec
 *           p1 - the p1 value as per spec
 *           p2 - the p2 calue as per spec
 * RETURN:   NONE
 * NOTES:    The SM docs call this the s1() function
 */
static void smCalcKey(uint8_t *out, const uint8_t *k, const uint8_t *r1, const uint8_t *r2)
{
    uint8_t t[SM_BLOCK_LEN];
    uint8_t i;

    for (i = 0; i < 8; i++) {
        t[i + 0] = r1[i + 8];
        t[i + 8] = r2[i + 8];
    }
    smEncrypt(out, t, k);
}

/*
 * FUNCTION: smCalcConfVal
 * USE:      Perform the calculation of the confirmation value
 * PARAMS:   out - store the result here
 *           k - the k value as per spec
 *           r - the r value as per spec
 *           pres - the pres value as per spec, only bottom 56 bit used
 *           preq - the pres value as per spec, only bottom 56 bit used
 *           iat - the iat value as per spec
 *           ia - the ia value as per spec, only bottom 48 bits used
 *           rat - the rat value as per spec
 *           ra - the ra value as per spec, only bottom 48 bits used
 * RETURN:   NONE
 * NOTES:    The SM docs call this the c1() function
 */
static void smCalcConfVal(uint8_t* out, const uint8_t *k, const uint8_t *r, uint64_t pres, uint64_t preq, bool iat, uint64_t ia, bool rat, uint64_t ra)
{
    uint8_t p1[SM_BLOCK_LEN];
    uint8_t p2[SM_BLOCK_LEN] = {0,};
    uint8_t i;

    /* create p1 */
    for (i = 0; i < 7; i++) {
        p1[6 - i] = pres;
        p1[13 - i] = preq;
        pres >>= 8;
        preq >>= 8;
    }
    p1[14] = rat ? 1 : 0;
    p1[15] = iat ? 1 : 0;

    /* create p2 */
    for (i = 0; i < 6; i++) {
        p2[9 - i] = ia;
        p2[15 - i] = ra;
        ia >>= 8;
        ra >>= 8;
    }

    /* p1 ^= r */
    for (i = 0; i < SM_BLOCK_LEN; i++)
        p1[i] ^= r[i];

    /* out = e(k, p1) */
    smEncrypt(out, p1, k);

    /* p2 ^= out */
    for (i = 0; i < SM_BLOCK_LEN; i++)
        p2[i] ^= out[i];

    /* out = e(k, p2) */
    smEncrypt(out, p2, k);
}

/*
 * FUNCTION: smAddressHash
 * USE:      Perform 24bit->24bit hash for address resolution
 * PARAMS:   r - the 24 bits to hash
 *           key - the key
 * RETURN:   the 24-bit hash
 * NOTES:    The SM docs call this the ah() function
 */
static uint32_t smAddressHash(uint32_t r, const uint8_t *key)
{
    uint8_t in[SM_BLOCK_LEN] = {0,}, out[SM_BLOCK_LEN];

    utilSetBE24(in + 13, r);
    smEncrypt(out, in, key);
    return utilGetBE24(out + 13);
}

/* a simple AES-128 implementation - we use it rarely enough that even an unoptimal version will do */

static void smEncryptSubBytes(uint8_t *bytes, uint8_t num)
{
    static const uint8_t sbox[] = {
        0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
        0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
        0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
        0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
        0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
        0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
        0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
        0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
        0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
        0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
        0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
        0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
        0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
        0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
        0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
        0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
    };
    uint8_t i;

    for (i = 0; i < num; i++)
        bytes[i] = sbox[bytes[i]];
}

static void smEncryptShiftRows(uint8_t *state)
{
    uint8_t i, j;
    uint8_t tmp[4];

    for (i = 0; i < 4; i++) {
        for(j = 0; j < 4; j++)
            tmp[j] = state[i * 4 + j];

        for(j = 0; j < 4; j++)
            state[i * 4 + j] = tmp[(i + j) & 3];
    }
}

static uint8_t smEncryptMul(uint8_t vin, uint8_t by)
{
    uint16_t v = vin;

    switch(by){
    case 1:
        break;
    case 2:
        v <<= 1;
        break;
    case 3:
        v = (v << 1) ^ v;
        break;
    }
    if (v & 0x100)
        v ^= 0x1B;

    return v;
}

static void smEncryptMixColumns(uint8_t *state)
{
    uint8_t i, j, k, ret[SM_BLOCK_LEN];
    static const uint8_t matrix[] = {2,3,1,1,1,2,3,1,1,1,2,3,3,1,1,2};

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            ret[i * 4 + j] = 0;
            for(k = 0; k < 4; k++)
                ret[i * 4 + j] ^= smEncryptMul(state[k * 4 + j], matrix[i * 4 + k]);
        }
    }

    for (i = 0; i <SM_BLOCK_LEN; i++)
        state[i] = ret[i];
}

static void smEncryptAddRoundKey(uint8_t *state, const uint8_t *keyPos)
{
    uint8_t i, j;

    for (i = 0; i < 4; i++, keyPos += 40)
        for(j = 0; j < 4; j++)
            *state++ ^= *keyPos++;
}

static void smEncryptExpandKey(uint8_t *expanded, const uint8_t *key)
{
    static const uint8_t rcon[] = {1,0,0,0, 2,0,0,0, 4,0,0,0, 8,0,0,0, 16,0,0,0, 32,0,0,0, 64,0,0,0, 128,0,0,0, 27,0,0,0, 54,0,0,0};
    uint8_t i, j, temp[4];

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            expanded[i + 44 * j] = key[i * 4 + j];

    for (i = 4; i < 44; i++) {
        for(j = 0; j < 4; j++)
            temp[j] = expanded[i - 1 + 44 * j];

        if (!(i & 3)) {
            j = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = j;
            smEncryptSubBytes(temp, 4);
            for (j = 0; j < 4; j++)
                temp[j] ^= rcon[(i &~ 3) - 4 + j];
        }

        for(j = 0; j < 4; j++)
            expanded[i + 44 * j] = expanded[i - 4 + 44 * j] ^ temp[j];
    }
}

/*
 * FUNCTION: smEncrypt
 * USE:      Perform an AES-128 encryption
 * PARAMS:   dst - palce to store result (SM_ENCRYPT_DATA_LEN bytes)
 *           src - the plaintext (SM_ENCRYPT_DATA_LEN bytes)
 *           key - the key (SM_ENCRYPT_DATA_LEN bytes)
 * RETURN:   NONE
 * NOTES:
 */
static void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key)
{
    uint8_t expandedKey[176];
    uint8_t state[SM_BLOCK_LEN];
    uint8_t i, j;

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            state[i + 4 * j] = *src++;

    smEncryptExpandKey(expandedKey, key);


    smEncryptAddRoundKey(state, expandedKey + 0);

    for (i = 1; i < 10; i++) {
        smEncryptSubBytes(state, SM_BLOCK_LEN);
        smEncryptShiftRows(state);
        smEncryptMixColumns(state);
        smEncryptAddRoundKey(state, expandedKey + i * 4);
    }

    smEncryptSubBytes(state, SM_BLOCK_LEN);
    smEncryptShiftRows(state);
    smEncryptAddRoundKey(state, expandedKey + i * 4);

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            *dst++ = state[i + 4 * j];
}
