/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

void newblue_log(const char *level, int syslog_pri, const char *fmt, ...)
{
    va_list ap;

    // Always print to syslog.
    va_start(ap, fmt);
    vsyslog(syslog_pri, fmt, ap);
    va_end(ap);

    // Also print to stderr if we are in tty.
    if (isatty(0)) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
}
