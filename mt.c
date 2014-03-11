#undef MT_DEBUG
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include "types.h"
#include "log.h"
#include "mt.h"

/* the utility things */

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

bool r_write(int fd, const void *data, size_t count)
{
    size_t countOriginal = count;
    const uint8_t *buf = (const uint8_t*)data;
    int ret;

    while (count) {

        ret = write(fd, buf, count);
        if (ret > 0) {
            buf += ret;
            count -= ret;
            continue;
        }
        if (ret == -1 && errno == EINTR)
            continue;

        return false;
    }
    return true;
}



/* the debug things */

#define MT_MAX_MUTEX_TAKE_TIME   10000 /* 10000 uS */
#define MT_MAX_MUTEX_HOLD_TIME   5000  /* 5000 uS */


struct mtID {
    char *name;
    char *file;
    int line;
    unsigned tid;
};

struct mtObject {
    struct mtObject *prev;
    struct mtObject *next;
    void *obj;
    int oType;

    struct mtID firstSeen;
    struct mtID lastLocked;

    uint64_t lockedAt;
};

#define MT_TYPE_MUTEX   1
#define MT_TYPE_THREAD  2


static pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
static struct mtObject* mObjects = NULL;
static pthread_once_t mInit = PTHREAD_ONCE_INIT;



static uint64_t mt_get_time(void)
{
    struct timeval tv;
    uint64_t t;

    gettimeofday(&tv, NULL);
    t = tv.tv_sec * 1000000UL;
    t += tv.tv_usec;

    return t;
}

static void mt_log(const struct mtID* id, const char* action, void* ptr)
{
    logd("**["FMT64"u %5u] %s:%d: %s['%s' = 0x%p]\n", CNV64(mt_get_time()), id->tid, id->file, id->line, action, id->name, ptr);
}

static void mt_ctrl_c_handler(int sig)
{
    struct mtObject *objs, *t;


    loge("sig caught, our lock = %d\n", pthread_mutex_trylock(&mLock));

    objs = mObjects;
    mObjects = NULL;

    for(t = objs; t; t = t->next) {
        if (t->oType == MT_TYPE_THREAD) {
            logd("**["FMT64"u] atexit thread %p\n", CNV64(mt_get_time()), t->obj);
        }
    }

    for(t = objs; t; t = t->next) {
        if (t->oType == MT_TYPE_MUTEX) {
            logd("**["FMT64"u] atexit mutex %p\n", CNV64(mt_get_time()), t->obj);
            mt_log(&t->lastLocked, "last locked here", t->obj);
            mt_log(&t->firstSeen, "first seen here", t->obj);
        }
    }

    abort();
}

static void mt_init_guts(void)
{
    signal(SIGINT, mt_ctrl_c_handler);
}

static void mt_init(void)
{
    pthread_once(&mInit, mt_init_guts);
}

static void mt_id_free(struct mtID* id)
{
    if (id->name)
        free(id->name);
    if (id->file)
        free(id->file);

    id->name = NULL;
    id->file =NULL;
    id->line = -1;
    id->tid = 0;
}

static void mt_id_fill(struct mtID* id, const char *file, int line, const char *name, unsigned tid)
{
    mt_init(); // why not here?

    mt_id_free(id);
    id->name = strdup(name);
    id->file = strdup(file);
    id->line = line;
    id->tid = tid;
}

//call with lock
static struct mtObject *mt_find_object(pthread_mutex_t *m)
{
    struct mtObject *mm;

    for (mm = mObjects; mm && mm->obj != (void*)m; mm = mm->next);

    return mm;
}

//call with lock
static struct mtObject* mt_evt_mutex_seen(const char *file, int line, const char *name, unsigned tid, pthread_mutex_t *m)
{
    struct mtObject *mm;

    if(!pthread_mutex_trylock(&mLock)) {
        loge("not locked!\n");
        abort();
    }

    mm = mt_find_object(m);
    if (!mm) {
        mm = (struct mtObject*)calloc(1, sizeof(struct mtObject));
        mm->oType = MT_TYPE_MUTEX;
        mm->obj = m;
        mt_id_fill(&mm->firstSeen, file, line, name, tid);
        mm->next = mObjects;
        mObjects = mm;
        if (mm->next)
            mm->next->prev = mm;
        mt_log(&mm->firstSeen, "mutex first seen", m);
    }
    return mm;
}

//call with lock
static struct mtObject* mt_evt_thread_seen(const char *file, int line, const char *name, unsigned tid, void *th)
{
    struct mtObject *mm;

    if(!pthread_mutex_trylock(&mLock)) {
        loge("not locked!\n");
        abort();
    }


    mm = mt_find_object(th);
    if (!mm) {
        mm = (struct mtObject*)calloc(1, sizeof(struct mtObject));
        mm->oType = MT_TYPE_THREAD;
        mm->obj = th;
        mt_id_fill(&mm->firstSeen, file, line, name, tid);
        mm->next = mObjects;
        mObjects = mm;
        if (mm->next)
            mm->next->prev = mm;
        mt_log(&mm->firstSeen, "thread first seen", th);
    }
    return mm;
}

static int mt_pthread_mutex_lock_int(const char *file, int line, const char *name, pthread_mutex_t *m, bool forCond, bool justTry)
{
    struct mtID id = {0,};
    unsigned tid = gettid();
    struct mtObject *mm;
    uint64_t tm;
    int ret;

    mt_id_fill(&id, file, line, name, tid);

    pthread_mutex_lock(&mLock);
    mm = mt_evt_mutex_seen(file, line, name, tid, m);
    tm = mt_get_time();
    mt_log(&id, forCond ? "mutex cond lock wait" : (justTry ? "mutex try wait" : "mutex lock wait"), m);
    pthread_mutex_unlock(&mLock);
    ret = forCond ? 0 : (justTry ? pthread_mutex_trylock(m) : pthread_mutex_lock(m));
    pthread_mutex_lock(&mLock);
    mm = mt_evt_mutex_seen(file, line, name, tid, m);
    if (!ret) {
        mm->lockedAt = mt_get_time();
        tm = mt_get_time() - tm;
        if (tm > MT_MAX_MUTEX_TAKE_TIME) {
            struct mtID id = {0,};
            mt_id_fill(&id, file, line, name, tid);
            logd("MUTEX lock took "FMT64"uuS!\n", CNV64(tm));
            mt_log(&id, "lock here", m);
            mt_log(&mm->lastLocked, "last locked here", m);
            mt_log(&mm->firstSeen, "first seen here", m);
            mt_id_free(&id);
        }
        mt_id_fill(&mm->lastLocked, file, line, name, tid);
        mt_log(&mm->lastLocked, forCond ? "mutex cond locked" : "mutex locked", m);
    } else {

        mt_log(&id, forCond ? "mutex cond lock failed" : (justTry ? "mutex trylock failed" : "mutex lock failed"), m);
    }
    pthread_mutex_unlock(&mLock);
    mt_id_free(&id);


    return ret;
}

int mt_pthread_mutex_lock(const char *file, int line, const char *name, pthread_mutex_t *m)
{
    return mt_pthread_mutex_lock_int(file, line, name, m, false, false);
}

int mt_pthread_mutex_trylock(const char *file, int line, const char *name, pthread_mutex_t *m)
{
    return mt_pthread_mutex_lock_int(file, line, name, m, false, true);
}

static int mt_pthread_mutex_unlock_int(const char *file, int line, const char *name, pthread_mutex_t *m, bool forCond)
{
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    uint64_t tm;
    int ret;

    pthread_mutex_lock(&mLock);
    mt_id_fill(&id, file, line, name, tid);
    mm = mt_evt_mutex_seen(file, line, name, tid, m);
    if (!mm->lockedAt) {
        logd("Unlocking a non-locked mutex\n");
        mt_log(&id, "unlock here", m);
        mt_log(&mm->firstSeen, "first seen here", m);
    }
    ret = forCond ? 0 : pthread_mutex_unlock(m);
    tm = mt_get_time() - mm->lockedAt;
    mm->lockedAt = 0;
    if (tm > MT_MAX_MUTEX_HOLD_TIME) {
        logd("MUTEX hold was "FMT64"uuS!\n", CNV64(tm));
        mt_log(&id, "unlock here", m);
        mt_log(&mm->lastLocked, "last locked here", m);
        mt_log(&mm->firstSeen, "first seen here", m);
    }
    mt_log(&id, forCond ?"mutex cond unlocked" : "mutex unlocked", m);
    mt_id_free(&id);
    pthread_mutex_unlock(&mLock);

    return ret;
}

int mt_pthread_mutex_unlock(const char *file, int line, const char *name, pthread_mutex_t *m)
{
    return mt_pthread_mutex_unlock_int(file, line, name, m, false);
}

static int mt_pthread_cond_wait_stub(pthread_cond_t *c, pthread_mutex_t *m, unsigned msec)
{
    return pthread_cond_wait(c, m);
}

static int mt_pthread_cond_timeout_np_stub(pthread_cond_t *c, pthread_mutex_t *m, unsigned msec)
{
    return pthread_cond_timeout_np(c, m, msec);
}

static int mt_pthread_cond_wait_generic(const char* file, int line, const char* name, pthread_cond_t *c, pthread_mutex_t *m, unsigned msec, int(*f)(pthread_cond_t*, pthread_mutex_t*, unsigned))
{
    int ret;
    mt_pthread_mutex_unlock_int(file, line, name, m, true);
    ret = f(c, m, msec);
    mt_pthread_mutex_lock_int(file, line, name, m, true, false);
    return ret;
}

int mt_pthread_cond_wait(const char* file, int line, const char* name, pthread_cond_t *c, pthread_mutex_t *m)
{
    return mt_pthread_cond_wait_generic(file, line, name, c, m, 0, mt_pthread_cond_wait_stub);
}

int mt_pthread_cond_timeout_np(const char* file, int line, const char* name, pthread_cond_t *c, pthread_mutex_t *m, unsigned msec)
{
    return mt_pthread_cond_wait_generic(file, line, name, c, m, msec, mt_pthread_cond_timeout_np_stub);
}


int mt_pthread_mutex_init(const char *file, int line, const char *name, pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    uint64_t tm;
    int ret;

    pthread_mutex_lock(&mLock);
    mt_id_fill(&id, file, line, name, tid);
    mm = mt_evt_mutex_seen(file, line, name, tid, m);
    ret = pthread_mutex_init(m, a);
    mt_log(&id, "mutex created", m);
    mt_id_free(&id);
    pthread_mutex_unlock(&mLock);

    return ret;
}

int mt_pthread_mutex_destroy(const char *file, int line, const char *name, pthread_mutex_t *m)
{
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    uint64_t tm;
    int ret;

    pthread_mutex_lock(&mLock);
    mt_id_fill(&id, file, line, name, tid);
    mm = mt_evt_mutex_seen(file, line, name, tid, m);
    if (mm->lockedAt) {
        logd("Destroying a locked mutex\n");
        mt_log(&id, "destroy here", m);
        mt_log(&mm->lastLocked, "last locked here", m);
        mt_log(&mm->firstSeen, "first seen here", m);
    }
    ret = pthread_mutex_destroy(m);
    mt_log(&id, "mutex destroyed", m);
    mt_id_free(&id);

    mt_id_free(&mm->lastLocked);
    mt_id_free(&mm->firstSeen);
    if (mm->next)
        mm->next->prev = mm->prev;
    if (mm->prev)
        mm->prev->next = mm->next;
    else
        mObjects = mm->next;
    free(mm);
    pthread_mutex_unlock(&mLock);

    return ret;
}

int mt_sem_destroy(const char *file, int line, const char *name, sem_t *s)
{
    int ret;

    ret = sem_destroy(s);

    return ret;
}

int mt_sem_init(const char *file, int line, const char *name, sem_t *s, int shared, unsigned val)
{
    int ret;

    ret = sem_init(s, shared, val);

    return ret;
}

int mt_sem_post(const char *file, int line, const char *name, sem_t *s)
{
    int ret;

    ret = sem_post(s);

    return ret;
}

int mt_r_sem_trywait(const char *file, int line, const char *name, sem_t *s)
{
    int ret;

    ret = r_sem_trywait(s);

    return ret;
}

int mt_r_sem_wait(const char *file, int line, const char *name, sem_t *s)
{
    int ret;

    ret = r_sem_wait(s);

    return ret;
}

struct mtThreadData {
    void *(*proc)(void *);
    void *param;
};

static void *mt_thread_stub(void *param)
{
    struct mtThreadData *td = (struct mtThreadData*)param;
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    void *ret;


    mt_id_fill(&id, __FILE__, __LINE__, "mt_thread_stub", tid);
    mt_log(&id, "thread started", (void*)pthread_self());
    ret = td->proc(td->param);
    mt_log(&id, "thread ended", (void*)pthread_self());
    free(td);

    pthread_mutex_lock(&mLock);
    mm = mt_evt_thread_seen(__FILE__, __LINE__, "mt_thread_stub", tid, (void*)pthread_self());
    mt_id_free(&mm->lastLocked);
    mt_id_free(&mm->firstSeen);
    if (mm->next)
        mm->next->prev = mm->prev;
    if (mm->prev)
        mm->prev->next = mm->next;
    else
        mObjects = mm->next;
    free(mm);
    pthread_mutex_unlock(&mLock);

    mt_id_free(&id);
    return ret;
}

int mt_pthread_create(const char *file, int line, const char *name, pthread_t *th, const pthread_attr_t *attr, void *(*proc)(void *), void *param)
{
    struct mtThreadData *td;
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    int ret;


    td = malloc(sizeof(struct mtThreadData));
    td->proc = proc;
    td->param = param;


    pthread_mutex_lock(&mLock);
    mt_id_fill(&id, file, line, name, tid);
    ret = pthread_create(th, attr, mt_thread_stub, td);
    mm = mt_evt_thread_seen(file, line, name, tid, (void*)*th);
    mt_log(&id, "thread created", (void*)*th);
    mt_id_free(&id);
    pthread_mutex_unlock(&mLock);

    return ret;
}

int mt_pthread_setname_np(const char *file, int line, const char *name, pthread_t thread, const char *threadName)
{
    struct mtThreadData *td;
    unsigned tid = gettid();
    struct mtID id = {0,};
    struct mtObject *mm;
    int ret;


    pthread_mutex_lock(&mLock);
    mt_id_fill(&id, file, line, threadName, tid);
    mm = mt_evt_thread_seen(file, line, threadName, tid, (void*)thread);
    ret = pthread_setname_np(thread, threadName);
    mt_log(&id, "thread renamed", (void*)thread);
    mt_id_free(&id);
    pthread_mutex_unlock(&mLock);

    return ret;
}




