#ifndef _MT_H_
#define _MT_H_


#include <semaphore.h>
#include <pthread.h>



/* handles EINTR */
int r_sem_wait(sem_t* sem);
int r_sem_trywait(sem_t* sem);


#endif


