#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>

/* if your platform lacks defines for uint8_t .. uint64_t, define them here */

#ifndef __cplusplus
#ifndef bool /* yes someone actually used #define on a bool somewhere in hardware/ includes */
typedef uint8_t bool;
#endif
#endif

#ifndef false
# define false (0)
#endif
#ifndef true
# define true  (1)
#endif

/* for printing 64-bit values */
#define FMT64      "%016ll"
#define CNV64(_v)  ((long long unsigned)(_v))


#endif

