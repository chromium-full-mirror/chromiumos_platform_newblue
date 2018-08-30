/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef _SG_H_
#define _SG_H_

#include "newblue-macros.h"
#include "types.h"

NEWBLUE_BEGIN_DECLS

struct sgbuf;
typedef struct sgbuf *sg;


/*
 * THE USUAL SEMANTICS OF SG BUFFERS:
 * - if a function has no return type, it is assumed to take over the ownership of any and all SGs passed to it
 * - if a function returns a bool signifying success, it shall retain ovwenership of any SGs passed to it in case of success, and NOT own them in case of failure
 * - if a function returns an integer-like value to indicate error, it shall retain ownership of the SG if it is successful, and not otherwise
 * - in all other cases the function shall have comments explaining its interactions with any SGs passed ot it (avoid this case please!)
 */



//create a new empty sg -> null on error
sg sgNew(void) NEWBLUE_EXPORT;

//create a new sg with given data, a copy is made -> null on error
sg sgNewWithCopyData(const void* data, uint32_t len) NEWBLUE_EXPORT;

//create a new sg with given data. sg takes ownership of data buf and will free it -> null on error
sg sgNewWithAllocedData(void *data, uint32_t len) NEWBLUE_EXPORT;

//delete an sg
void sgFree(sg s) NEWBLUE_EXPORT;

//query length
uint32_t sgLength(sg s) NEWBLUE_EXPORT;

//concat. back is freed, front gets its data
void sgConcat(sg front, sg back) NEWBLUE_EXPORT;

//convenient permutations of concat
bool sgConcatFrontCopy(sg s, const void *data, uint32_t len) NEWBLUE_EXPORT;
bool sgConcatFrontAlloced(sg s, void *data, uint32_t len) NEWBLUE_EXPORT;
bool sgConcatBackCopy(sg s, const void *data, uint32_t len) NEWBLUE_EXPORT;
bool sgConcatBackAlloced(sg s, void *data, uint32_t len) NEWBLUE_EXPORT;

//truncation (delete end)
void sgTruncBack(sg s, uint32_t bytes_to_delete) NEWBLUE_EXPORT;

//truncation (delete front)
void sgTruncFront(sg s, uint32_t bytes_to_delete) NEWBLUE_EXPORT;

//cuts the last few bytes off into a new sg. param is how many bytes stay with the first sg
sg sgSplit(sg s, uint32_t firstLen) NEWBLUE_EXPORT;

//serialization
uint32_t sgSerialize(sg s, uint32_t offset, uint32_t len, void* dstBuf) NEWBLUE_EXPORT;

//swap the data in a and b
void sgSwap(sg a, sg b) NEWBLUE_EXPORT;

//combo
bool sgSerializeCutFront(sg s, void *buf, uint32_t len) NEWBLUE_EXPORT;

//duplication
sg sgDup(sg data) NEWBLUE_EXPORT;

//iter start
void* sgIterStart(sg s) NEWBLUE_EXPORT;               /* null means iteration is over */
uint32_t sgIterCurLen(void *iter) NEWBLUE_EXPORT;     /* get "current" length */
const void* sgIterCurData(void *iter) NEWBLUE_EXPORT; /* get "current" data */
void* sgIterAdvance(void *iter) NEWBLUE_EXPORT;       /* null means iteration is over */


NEWBLUE_END_DECLS

#endif

