#ifndef _UNIQ_H_
#define _UNIQ_H_

#include "types.h"

/*
 * UNIQ_T:
 *  - uniformly-increasing unique ID
 *  - is safe to compare to numeric 0, and will be equal if uniq_t is invalid
 *  - will not overflow in any sane timeframe
 */
typedef uint64_t uniq_t;

#define UNIQ_MARKER   ((uniq_t)1) /* non-zero (aka: valid) uniq_t guaranteed to be never returned by uniqGetNext() */

#define UNIQ_FMT      FMT64
#define UNIQ_CONV(x)  CNV64(x)

/* get another uniq_t */
uniq_t uniqGetNext(void);

#endif
