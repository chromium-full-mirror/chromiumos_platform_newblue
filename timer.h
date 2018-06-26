/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _TIMER_H_
#define _TIMER_H_

#include "newblue-macros.h"
#include "types.h"
#include "uniq.h"

typedef void (*timerCbk)(uniq_t which,  uint64_t cbkData);

/* allocate a timer - data copy will be made */
uniq_t timerSet(uint64_t msec, timerCbk cbk, uint64_t cbkData) NEWBLUE_EXPORT;

/* cancel a timer. returns true if it was cancelled and callback has not run yet. If you need to
 * know for sure when it is safe to destroy callback's state, use timerSync()
 */
bool timerCancel(uniq_t timer) NEWBLUE_EXPORT;

/* make sure that all cancelled timers before now have either fired or run */
void timerSync(void) NEWBLUE_EXPORT;

bool timersInit(void) NEWBLUE_EXPORT;
void timersDeinit(void) NEWBLUE_EXPORT;

#endif

