/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
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

/* Exposed only for unittest purpose. */
#ifdef _UNITTEST_
void smEncrypt(uint8_t* dst, const uint8_t *src, const uint8_t *key);
void smCalcConfVal(uint8_t* out, const uint8_t *k, const uint8_t *r, uint64_t pres, uint64_t preq,
                   bool iat, uint64_t ia, bool rat, uint64_t ra);
void smCalcKey(uint8_t *out, const uint8_t *k, const uint8_t *r1, const uint8_t *r2);
#endif

#endif /* _SM_H_ */
