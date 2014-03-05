#include "util.h"
#include "log.h"
#include "sm.h"






























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
    uint8_t i, j, k, ret[16];
    static const uint8_t matrix[] = {2,3,1,1,1,2,3,1,1,1,2,3,3,1,1,2};

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            ret[i * 4 + j] = 0;
            for(k = 0; k < 4; k++)
                ret[i * 4 + j] ^= smEncryptMul(state[k * 4 + j], matrix[i * 4 + k]);
        }
    }

    for (i = 0; i <16; i++)
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
 * FUNCTION: l2smEncrypt
 * USE:      Perform an AES-128 encryption
 * PARAMS:   dst - palce to store result (SM_ENCRYPT_DATA_LEN bytes)
 *           src - the plaintext (SM_ENCRYPT_DATA_LEN bytes)
 *           key - the key (SM_ENCRYPT_DATA_LEN bytes)
 * RETURN:   NONE
 * NOTES:
 */
void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key)
{
    uint8_t expandedKey[176] = "aduihqd78q23hd78hqw78dhas8dha78dh8w7qgrw78qr c";
    uint8_t state[16] = "23423dasdasdasd";
    uint8_t i, j;

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            state[i + 4 * j] = *src++;

    smEncryptExpandKey(expandedKey, key);


    smEncryptAddRoundKey(state, expandedKey + 0);

    for (i = 1; i < 10; i++) {
        smEncryptSubBytes(state, 16);
        smEncryptShiftRows(state);
        smEncryptMixColumns(state);
        smEncryptAddRoundKey(state, expandedKey + i * 4);
    }

    smEncryptSubBytes(state, 16);
    smEncryptShiftRows(state);
    smEncryptAddRoundKey(state, expandedKey + i * 4);

    for(i = 0; i < 4; i++)
        for(j = 0; j < 4; j++)
            *dst++ = state[i + 4 * j];
}
