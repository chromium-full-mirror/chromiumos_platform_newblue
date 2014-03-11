#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>
#include "types.h"
#include "uniq.h"

typedef void (*timerCbk)(uniq_t which,  uint64_t cbkData);

/* allocate a timer - data copy will be made */
uniq_t timerSet(uint64_t msec, timerCbk cbk, uint64_t cbkData);

/* cancel a timer. returns true if it was cancelled and callback has not run yet. If you need to know for sure when it is safe to destroy callback's state, use timerSync() */
bool timerCancel(uniq_t timer);

/* make sure that all cancelled timers before now have either fired or run */
void timerSync(void);

bool timersInit(void);
void timersDeinit(void);

#endif

