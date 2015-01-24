#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>
#include <stdbool.h>

/* if your platform lacks defines for uint8_t .. uint64_t, define them here */



/* for printing 64-bit values */
#define FMT64      "%016ll"
#define CNV64(_v)  ((long long unsigned)(_v))


#endif

