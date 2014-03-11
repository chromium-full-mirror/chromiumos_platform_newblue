#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <android/log.h>
#include <utils/Log.h>

#define LOG_STRINGIFYX(x) #x
#define LOG_STRINGIFY(x) LOG_STRINGIFYX(x)

#ifdef EXECUTABLE
# ifndef LOG_PRINTF_CALL
#  define LOG_PRINTF_CALL(...) fprintf(stderr, __VA_ARGS__)
# endif


# define log(level, ...) LOG_PRINTF_CALL(level "@ " __FILE__ ":" LOG_STRINGIFY(__LINE__) ": " __VA_ARGS__)

# define loge(...)	log("E", __VA_ARGS__)
# define logw(...)	log("W", __VA_ARGS__)
# define logi(...)	log("I", __VA_ARGS__)
# define logd(...)	log("D", __VA_ARGS__)
#else
# undef LOG_TAG
# define LOG_TAG        "NB.BT" __FILE__ ":" LOG_STRINGIFY(__LINE__)
# define loge           ALOGE
# define logw           ALOGW
# define logi           ALOGI
# define logd           ALOGD
#endif



#endif

