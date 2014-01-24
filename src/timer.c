#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "timer.h"
#include "uniq.h"
#include "log.h"



struct timer {
    struct timer *next;
    uniq_t        handle;
    uint64_t      fireTime;
    timerCbk      cbk;
    void         *cbkData;
};

static pthread_mutex_t mTimersLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mTimersCond = PTHREAD_COND_INITIALIZER;
static pthread_t mTimersThread = 0;
static struct timer *mTimers = NULL;
static clockid_t mClock;

/*
 * FUNCTION: timerCurTime
 * USE:      Get current time in milliseconds
 * PARAMS:   unused - unused
 * RETURN:   the time
 * NOTES:    Time returned here need only be monotonicly increating,
 *           it need not be base don any particular base time, nor
 *           is it required to be the same base every reboot.
 */
static uint64_t timerCurTime(void)
{
    struct timespec ts;
    uint64_t ret;
    int err;

    err = clock_gettime(mClock, &ts);
    if (err)
        loge("Unexpected error %d getting clock\n", err);

    ret = ts.tv_sec;
    ret *= 1000ULL;
    ret += ts.tv_nsec / 1000000ULL;

    return ret;
}

/*
 * FUNCTION: timersThread
 * USE:      Worker thread for timers code
 * PARAMS:   unused - unused
 * RETURN:   void* - unused
 * NOTES:
 */
static void* timersThread(void *unused)
{
    struct timer *t, *toFire;

    do {

        pthread_mutex_lock(&mTimersLock);
        do {
            struct timer *p;
            uint64_t curTime, nextTime;

            /* grab the part of the list that is due, disconnect it, save head to "toFire" */
            
            curTime = timerCurTime();
            nextTime = 0;

            t = mTimers;
            p = NULL;
            while (t && t->fireTime <= curTime) {
                p = t;
                t = t->next;
            }

            toFire = mTimers;
            mTimers = t;
            if (p)
                p->next = NULL;
            else
                toFire = NULL;

            if (toFire)
                break;

            if (mTimers) {
                struct timespec waitTime;
                nextTime = mTimers->fireTime;
                waitTime.tv_sec = (nextTime - curTime) / 1000ULL;
                waitTime.tv_nsec = ((nextTime - curTime) % 1000ULL) * 1000000ULL;

                pthread_cond_timedwait(&mTimersCond, &mTimersLock, &waitTime);
            } else
                pthread_cond_wait(&mTimersCond, &mTimersLock);

        } while(1);

        pthread_mutex_unlock(&mTimersLock);

        while (toFire) {
            t = toFire;
            toFire = toFire->next;
            t->cbk(t->handle, t->cbkData);
            free(t);
        }

    } while(1);
    return unused;
}

/*
 * FUNCTION: timersActualInit
 * USE:      Perform actual one-time init of timer code
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    Guaranteed to be called exactly once
 */
static void timersActualInit(void)
{
    struct timespec dummy;
    int ret;

    /* find which clock we'll use */
    ret = clock_gettime(mClock = CLOCK_BOOTTIME, &dummy);
    if (ret) {
        logw("CLOCK_BOOTTIME not available on this system. Trying fallbacks\n");
        if (!clock_gettime(mClock = CLOCK_MONOTONIC, &dummy))
            logw("Clock fallback: CLOCK_MONOTONIC\n");
        else if(!clock_gettime(mClock = CLOCK_REALTIME, &dummy))
            logw("Clock fallback: CLOCK_REALTIME\n");
        else
            loge("No clock fallback found!\n");
    }

    /* get going on the worker thread */
    ret = pthread_create(&mTimersThread, NULL, timersThread, NULL);
    if (ret)
        loge("Timer thread creatin error: %d\n", ret);
}

/*
 * FUNCTION: timersInit
 * USE:      Perform one-time init of timer code
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    Uses pthread_once to call timersActualInit() exactly once.
 *           Do not add any more code to this function due to
 *           thread-safety reasons.
 */
static void timersInit(void)
{
    static pthread_once_t mOnceInit = PTHREAD_ONCE_INIT;

    pthread_once(&mOnceInit, timersActualInit); 
}

/*
 * FUNCTION: timerSet
 * USE:      Create a timer
 * PARAMS:   msec - how many msce from now to fire in
 *           cbk - callback to call when timer fires
 *           cbkData - data to pass to said callback
 * RETURN:   uniq_t handle of the timer or 0 for failure
 * NOTES:
 */
uniq_t timerSet(uint64_t msec, timerCbk cbk, void *cbkData)
{
    struct timer *t, *c, *p;
    bool wake = false;
    uniq_t ret = 0;

    /* mandatory one-time init */
    timersInit();

    /* prepare the struct */
    t = (struct timer*)malloc(sizeof(struct timer));
    if (!t)
        return 0;
    t->handle = uniqGetNext();
    t->cbk = cbk;
    t->cbkData = cbkData;
    t->fireTime = timerCurTime() + msec;

    pthread_mutex_lock(&mTimersLock);

    /* find where it belongs */
    c = mTimers;
    p = NULL;
    while (c && c->fireTime < t->fireTime) {
        p = c;
        c = c->next;
    }

    /* link it in */
    t->next = c;
    if (p)
        p->next = t;
    else {
        mTimers = t;
        wake = true;
    }

    /* wake the timer thread if we added a new first */
    pthread_cond_signal(&mTimersCond);
    pthread_mutex_unlock(&mTimersLock);
}

/*
 * FUNCTION: timerCancel
 * USE:      Cancel a timer
 * PARAMS:   timer the handle for a timer
 * RETURN:   true if we cancelled the timer before it had a
 *           chance to fire. False in all other cases like:
 *           handle not found, already fired, etc...
 * NOTES:
 */
bool timerCancel(uniq_t timer)
{
    timersInit();
    pthread_mutex_lock(&mTimersLock);

    pthread_mutex_unlock(&mTimersLock);
}



