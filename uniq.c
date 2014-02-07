#include <pthread.h>
#include <stdlib.h>
#include "uniq.h"
#include "log.h"

static pthread_mutex_t mUniqLock = PTHREAD_MUTEX_INITIALIZER;
static uniq_t mUniq = UNIQ_MARKER + 1;

uniq_t uniqGetNext(void)
{
    uniq_t ret;

    pthread_mutex_lock(&mUniqLock);
    ret = mUniq++;
    if (!mUniq) {
        /*
         * This would never happen in real life, due to slow processors and
         * large UINT64_MAX, but it is better to be careful
         */
        loge("out of uniq values\n");
        abort();
    }
    pthread_mutex_unlock(&mUniqLock);
    return ret;
}

