#include <errno.h>
#include "util.h"
#include "log.h"


int r_sem_wait(sem_t* sem)
{
    int ret;

    while(1) {
        ret = sem_wait(sem);
        if (ret != -1 || errno != EINTR)
            break;
    }

    if (ret)
        loge("sem_wait failed with ret %d and errono %d!\n", ret, errno);

    return ret;
}

int r_sem_trywait(sem_t* sem)
{
    int ret;

    while(1) {
        ret = sem_trywait(sem);
        if (ret != -1 || errno != EINTR)
            break;
    }

    if (ret && errno != EAGAIN)
        loge("sem_trywait failed with ret %d and errono %d!\n", ret, errno);

    return ret;
}
