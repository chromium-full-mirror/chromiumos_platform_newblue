#include <semaphore.h>
#include <pthread.h>
#include <stdlib.h>
#include "multiNotif.h"
#include "util.h"
#include "log.h"


struct multiNotifItem {
    uniq_t                 handle;
    struct multiNotifItem *next;
    multiNotifCbk          cbk;
    void                  *cbkData;
    bool                   destroyRecord;
};

struct multiNotifList {
    struct multiNotifItem *items;
    pthread_mutex_t        structLock;
    pthread_t              bcastThread;

    pthread_mutex_t        bcastLock;
};

/*
 * FUNCTION: multiNotifCreate
 * USE:      Create a multi-notif structure
 * PARAMS:   NONE
 * RETURN:   said struct or NULL on error
 * NOTES:
 */
struct multiNotifList *multiNotifCreate(void)
{
    struct multiNotifList* nl;

    nl = (struct multiNotifList*)calloc(1, sizeof(struct multiNotifList));
    if (!nl)
        goto out_alloc;

    if (pthread_mutex_init(&nl->structLock, NULL)) {
        loge("failed to init notif list struct mutex\n");
        goto out_mutex_1;
    }

    if (pthread_mutex_init(&nl->structLock, NULL)) {
        loge("failed to init notif list bcast mutex\n");
        goto out_mutex_2;
    }

    return nl;

out_mutex_2:
    pthread_mutex_destroy(&nl->structLock);

out_mutex_1:
    free(nl);

out_alloc:
    return NULL;
}

/*
 * FUNCTION: multiNotifDestroy
 * USE:      Delete a multi-notif structure
 * PARAMS:   nl - the multi-notif structure
 * RETURN:   NONE
 * NOTES:
 */
void multiNotifDestroy(struct multiNotifList *nl)
{
    pthread_mutex_destroy(&nl->structLock);
    pthread_mutex_destroy(&nl->bcastLock);
    free(nl);
}

/*
 * FUNCTION: notifListFindAndDestroy
 * USE:      Find an item in the lsit with the given handle and delete it
 * PARAMS:   items - the list
 *           handle - the handle
 * RETURN:   true if found
 * NOTES:
 */
static bool notifListFindAndDestroy(struct multiNotifItem **items, uniq_t handle)
{
    struct multiNotifItem *cur = *items, *prev = NULL;

    while (cur && cur->handle != handle) {
        prev = cur;
        cur = cur->next;
    }
    if (!cur)
        return false;

    if (prev)
        prev->next = cur->next;
    else
        *items = cur->next;
    free(cur);

    return true;
}

/*
 * FUNCTION: multiNotifNotify
 * USE:      Perform a notify on a multi-notif structure
 * PARAMS:   nl - the multi-notif structure
 *           notifData - the data to pass to all those notified
 * RETURN:   NONE
 * NOTES:
 */
void multiNotifNotify(struct multiNotifList *nl, const void *notifData)
{
    struct multiNotifItem *notifTodo, *notifDone = NULL, *t, *iter;

    pthread_mutex_lock(&nl->structLock);

    /* assure only one broadcast at a time */
    pthread_mutex_lock(&nl->bcastLock);

    /* grab all items */
    notifTodo = nl->items;
    nl->items = NULL;
    nl->bcastThread = pthread_self();
    pthread_mutex_unlock(&nl->structLock);

    if (!notifTodo)
        return;

    while (notifTodo) {

        /* call the handler */
        notifTodo->cbk(notifTodo->cbkData, notifData, notifTodo->handle);

        /* move the item to the done list */
        t = notifTodo->next;
        notifTodo->next = notifDone;
        notifDone = notifTodo;
        notifTodo = t;

        pthread_mutex_lock(&nl->structLock);

        /* check for new additions & deletions */
        iter = nl->items;
        while (iter) {
            if (iter->destroyRecord) { /* deletion */
                if (!notifListFindAndDestroy(&notifDone, iter->handle))
                    notifListFindAndDestroy(&notifTodo, iter->handle);
                if (iter->cbk)
                    iter->cbk(iter->cbkData, NULL, iter->handle);
            } else { /* aqddition */
                t = iter->next;
                iter->next = notifTodo;
                notifTodo = iter;
                iter = t;
            }
        }

        /* check for end consition */
        if (!notifTodo)
            break;
        pthread_mutex_unlock(&nl->structLock);
    }

    pthread_mutex_unlock(&nl->bcastLock);
    pthread_mutex_unlock(&nl->structLock);
}

/*
 * FUNCTION: multiNotifRegister
 * USE:      Register to be notified
 * PARAMS:   nl - the multi-notif structure
 *           cbk - the notify callback
 *           cbkData - data for said callback
 * RETURN:   handle for the notif object
 * NOTES:    callback may be called before this func returns
 */
uniq_t multiNotifRegister(struct multiNotifList *nl, multiNotifCbk cbk, void *cbkData)
{
    struct multiNotifItem *t;
    uniq_t ret;

    if (!cbk) {
        loge("Refusing to set notification with no callback\n");
        return 0;
    }

    t = (struct multiNotifItem*)calloc(1, sizeof(struct multiNotifItem));
    if (!t)
        return 0;

    t->handle = ret = uniqGetNext();
    t->cbk = cbk;
    t->cbkData = cbkData;

    pthread_mutex_lock(&nl->structLock);
    t->next = nl->items;
    nl->items = t;
    pthread_mutex_unlock(&nl->structLock);

    return ret;
}

/*
 * FUNCTION: multiNotifUnregisterDestoryCbk
 * USE:      notifies us when delete completed
 * PARAMS:   cbkData - pointer to the semaphore
 *           notifData - the nofitication data (we get NULL)
 *           handle - the notif handle
 * RETURN:   NONE
 * NOTES:
 */
static void multiNotifUnregisterDestoryCbk(void *cbkData, const void* notifData, uniq_t handle)
{
    sem_post((sem_t*)cbkData);
}

/*
 * FUNCTION: multiNotifUnregister
 * USE:      Unregister a callback
 * PARAMS:   nl - the multi-notif structure
 *           handle - the notif handle
 * RETURN:   NONE
 * NOTES:    callback may be called while this func is running. It will NOT be
 *           called anytime after it returns
 */
void multiNotifUnregister(struct multiNotifList *nl, uniq_t handle)
{
    struct multiNotifItem *t, *p = NULL;
    sem_t sem;

    /*
     * The idea here is that by the time unregister() returns, we should be sure
     * that the callback is not currently running, nor will it in the future.
     * There are four main cases:
     * 1. A broadcast is not currently in progress. In this case life is simple.
     *    We just find the item we need and remove it. Done
     * 2. A broadcast is currently in progress and its list of callbacks to call
     *    does not include the item we want to remove (it was added during this
     *    broadcast, perhaps). In this case life is simple. We just find the item we
     *    need and remove it. Done
     * 3. A broadcast is in progress, and we're not the thread doing it. This is
     *    also not too hard. We add a "destroyRecord" for the item we want deleted
     *    and when the broadcast thread gets to it, it will do this for us and
     *    call us back, wherein we'll return to our caller.
     * 4. A broadcast is in progress, and we are the thread doing it. In this case
     *    there are two subcases:
     *    a. We are removing the current item (whose callback we're in). In this
     *       case a delete record is added and we return immediately. When the
     *       callback returns, it will be deleted
     *    b. We are removing another item. Here two subcases exist as well.
     *       i ) Its callback has already fired - we add a delete record and
     *           return.
     *       ii) Its callback has not fired - we add a delete record and return
     *    as can be seen, all subcases of (4) are handled by the same action.
     */

    pthread_mutex_lock(&nl->structLock);
    t = nl->items;
    while (t && t->handle != handle && !t->destroyRecord) {
        p = t;
        t = t->next;
    }
    if (t) { /* cases 1 & 2 */
        if (p)
            p->next = t->next;
        else
            nl->items = t->next;
        free(t);
        goto out;
    }

    if (!pthread_mutex_trylock(&nl->bcastLock)) {
        /*
         * Cases 3 and 4 require bcast in progress. If we manage to take
         * the bcast lock here - a broadcast is not in progress.
         */
        pthread_mutex_unlock(&nl->bcastLock);
        goto out;
    }

    t = (struct multiNotifItem*)calloc(1, sizeof(struct multiNotifItem));
    if (!t) {
        loge("destroy record alloc failed\n");
        goto out;
    }

    t->destroyRecord = true;
    t->handle = handle;
    t->next = nl->items;
    nl->items = t;

    if (nl->bcastThread != pthread_self()) { /* case 3 */
        if (sem_init(&sem, 0, 0)) {
            loge("sem init failed\n");
            goto out;
        }
        t->cbkData = &sem;
        pthread_mutex_unlock(&nl->structLock);
        r_sem_wait(&sem);
        sem_destroy(&sem);
        return;
    }

    /* case 4 - nothing to do here for it */
    

out:
    pthread_mutex_unlock(&nl->structLock);
}






