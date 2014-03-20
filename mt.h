#ifndef _MT_H_
#define _MT_H_

#ifdef MT_DEBUG
# define pthread_mutex_lock       mt_old_pthread_mutex_lock
# define pthread_mutex_trylock    mt_old_pthread_mutex_trylock
# define pthread_mutex_unlock     mt_old_pthread_mutex_unlock
# define pthread_mutex_init       mt_old_pthread_mutex_init
# define pthread_mutex_destroy    mt_old_pthread_mutex_destroy
# define sem_destroy              mt_old_sem_destroy
# define sem_init                 mt_old_sem_init
# define sem_post                 mt_old_sem_post
# define r_sem_wait               mt_old_r_sem_wait
# define r_sem_trywait            mt_old_r_sem_trywait
# define pthread_create           mt_old_pthread_create
# define pthread_setname_np       mt_old_pthread_setname_np
# define pthread_cond_wait        mt_old_pthread_cond_wait
# define pthread_cond_timeout_np  mt_old_pthread_cond_timeout_np
#endif

#include <semaphore.h>
#include <pthread.h>


/* These handle EINTR */
int r_sem_wait(sem_t* sem);
int r_sem_trywait(sem_t* sem);
int r_sem_timedwait(sem_t* sem, const struct timespec *abs_timeout);

bool r_write(int fd, const void *data, size_t count); /* true if all writen, else false */
bool r_read(int fd, void *data, size_t count); /* true if all writen, else false */

#ifdef MT_DEBUG
# undef pthread_mutex_lock
# undef pthread_mutex_trylock
# undef pthread_mutex_unlock
# undef pthread_mutex_init
# undef pthread_mutex_destroy
# undef sem_destroy
# undef sem_init
# undef sem_post
# undef r_sem_wait
# undef r_sem_trywait
# undef pthread_create
# undef pthread_setname_np
# undef pthread_cond_wait
# undef pthread_cond_timeout_np
# define pthread_mutex_lock(m)          mt_pthread_mutex_lock(__FILE__, __LINE__, #m, m)
# define pthread_mutex_trylock(m)       mt_pthread_mutex_trylock(__FILE__, __LINE__, #m, m)
# define pthread_mutex_unlock(m)        mt_pthread_mutex_unlock(__FILE__, __LINE__, #m, m)
# define pthread_mutex_init(m,a)        mt_pthread_mutex_init(__FILE__, __LINE__, #m, m, a)
# define pthread_mutex_destroy(m)       mt_pthread_mutex_destroy(__FILE__, __LINE__, #m, m)
# define sem_destroy(s)                 mt_sem_destroy(__FILE__, __LINE__, #s, s)
# define sem_init(s,shr,val)            mt_sem_init(__FILE__, __LINE__, #s, s, shr, val)
# define sem_post(s)                    mt_sem_post(__FILE__, __LINE__, #s, s)
# define r_sem_wait(s)                  mt_r_sem_wait(__FILE__, __LINE__, #s, s)
# define r_sem_trywait(s)               mt_r_sem_trywait(__FILE__, __LINE__, #s, s)
# define pthread_create(t,a,f,p)        mt_pthread_create(__FILE__, __LINE__, #f, t, a, f, p)
# define pthread_setname_np(t,n)        mt_pthread_setname_np(__FILE__, __LINE__, #t, t, n)
# define pthread_cond_wait(c,m)         mt_pthread_cond_wait(__FILE__, __LINE__, #m, c, m)
# define pthread_cond_timeout_np(c,m,t) mt_pthread_cond_timeout_np(__FILE__, __LINE__, #m, c, m, t)
#define mt_analize_death mt_do_analize_death
#else
#define mt_analize_death
#endif


void mt_do_analize_death();
int mt_pthread_mutex_lock(const char* file, int line, const char* name, pthread_mutex_t *m);
int mt_pthread_mutex_trylock(const char* file, int line, const char* name, pthread_mutex_t *m);
int mt_pthread_mutex_unlock(const char* file, int line, const char* name, pthread_mutex_t *m);
int mt_pthread_mutex_init(const char* file, int line, const char* name, pthread_mutex_t *m, const pthread_mutexattr_t *a);
int mt_pthread_mutex_destroy(const char* file, int line, const char* name, pthread_mutex_t *m);
int mt_sem_destroy(const char* file, int line, const char* name, sem_t *s);
int mt_sem_init(const char* file, int line, const char* name, sem_t *s, int shared, unsigned val);
int mt_sem_post(const char* file, int line, const char* name, sem_t *s);
int mt_r_sem_trywait(const char* file, int line, const char* name, sem_t *s);
int mt_r_sem_wait(const char* file, int line, const char* name, sem_t *s);
int mt_pthread_create(const char* file, int line, const char* name, pthread_t *th, const pthread_attr_t *attr, void *(*proc)(void *), void *param);
int mt_pthread_setname_np(const char* file, int line, const char* name, pthread_t thread, const char* threadName);
int mt_pthread_cond_wait(const char* file, int line, const char* name, pthread_cond_t *c, pthread_mutex_t *m);
int mt_pthread_cond_timeout_np(const char* file, int line, const char* name, pthread_cond_t *c, pthread_mutex_t *m, unsigned msec);


#endif


