#ifndef _UNIQ_H_
#define _UNIQ_H_

#include <stdint.h>

/*
 * UNIQ_T:
 *  - uniformly-increasing unique ID
 *  - is safe to compare to numeric 0, and will be equal if uniq_t is invalid
 *  - will not overflow in any sane timeframe
 */
typedef uint64_t uniq_t;

/* get another uniq_t */
uniq_t uniqGetNext(void);

#endif
