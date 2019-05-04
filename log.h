/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <syslog.h>
#ifndef EXECUTABLE
# include <android/log.h>
# include <utils/Log.h>
#endif

#define LOG_STRINGIFYX(x) #x
#define LOG_STRINGIFY(x) LOG_STRINGIFYX(x)

void newblue_log(const char *level, int syslog_pri, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef EXECUTABLE
# ifndef LOG_PRINTF_CALL
#  define LOG_PRINTF_CALL(...) fprintf(stderr, __VA_ARGS__)
# endif
# define log(level, syslog_pri, ...) newblue_log(level, syslog_pri, level "@ " __FILE__ ":" LOG_STRINGIFY(__LINE__) ": " __VA_ARGS__)
# define loge(...)	log("E", LOG_ERR, __VA_ARGS__)
# define logw(...)	log("W", LOG_WARNING, __VA_ARGS__)
# define logi(...)	log("I", LOG_INFO, __VA_ARGS__)
# define logd(...)	log("D", LOG_DEBUG, __VA_ARGS__)
#else
# undef LOG_TAG
# define LOG_TAG        "NB.BT" __FILE__ ":" LOG_STRINGIFY(__LINE__)
# define loge           ALOGE
# define logw           ALOGW
# define logi           ALOGI
# define logd           ALOGD
#endif



#endif

