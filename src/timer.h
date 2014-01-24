#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>
#include "types.h"
#include "uniq.h"

typedef void (*timerCbk)(uniq_t which, void *cbkData);

/* allocate a timer */
uniq_t timerSet(uint64_t msec, timerCbk cbk, void *cbkData);

/* cancel a timer. returns true if it was cancelled and callback has not run yet */
bool timerCancel(uniq_t timer);



#endif

