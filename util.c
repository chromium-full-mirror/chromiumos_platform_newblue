#include <errno.h>
#include "util.h"


int r_sem_wait(sem_t* sem)
{
    int ret;

    while(1) {
        ret = sem_wait(sem);
        if (ret != -1 || errno != EINTR)
            break;
    }

    if (!ret)
        loge("sem_wait failed!\n");

    return ret;
}
