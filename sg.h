#ifndef _SG_H_
#define _SG_H_

#include "types.h"

struct sgbuf;
typedef struct sgbuf *sg;



//create a new empty sg -> null on error
sg sgNew(void);

//create a new sg with given data,  a copy is made -> null on error
sg sgNewWithCopyData(const void* data, uint32_t len);

//create a new sg with given data. sg takes ownership of data buf and will free it -> null on error
sg sgNewWithAllocedData(void *data, uint32_t len);

//delete an sg
void sgFree(sg s);

//query length
uint32_t sgLength(sg s);

//concat. back is freed, front gets its data
void sgConcat(sg front, sg back);

//convenient permutations of concat
bool sgConcatFrontCopy(sg s, const void *data, uint32_t len);
bool sgConcatFrontAlloced(sg s, void *data, uint32_t len);
bool sgConcatBackCopy(sg s, const void *data, uint32_t len);
bool sgConcatBackAlloced(sg s, void *data, uint32_t len);

//truncation (delete end)
void sgTruncBack(sg s, uint32_t bytes_to_delete);

//truncation (delete front)
void sgTruncFront(sg s, uint32_t bytes_to_delete);

//cuts the last few bytes off into a new sg. param is how many bytes stay with the first sg
sg sgSplit(sg s, uint32_t firstLen);

//serialization
uint32_t sgSerialize(sg s, uint32_t offset, uint32_t len, void* dstBuf);

//swap the data in a and b
void sgSwap(sg a, sg b);

//combo
bool sgSerializeCutFront(sg s, void *buf, uint32_t len);

//iter start
void* sgIterStart(sg s);               /* null means iteration is over */
uint32_t sgIterCurLen(void *iter);     /* get "current" length */
const void* sgIterCurData(void *iter); /* get "current" data */
void* sgIterAdvance(void *iter);       /* null means iteration is over */


#endif

