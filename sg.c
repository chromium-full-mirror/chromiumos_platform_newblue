#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "sg.h"

/* uncomment to check *all* SGs for validity in all cases on input & output */
#define SG_DEBUG

struct sgitem {
    struct sgitem *next;
    uint32_t       len;    /* how much useful data we have (not incl offset) */
    uint32_t       offset; /* how much in front to ignore */
    void          *data;   /* null if data follows and need nto be freed */
};

struct sgbuf {
    uint32_t       len;
    struct sgitem *head;
    struct sgitem *tail;
};

#ifdef SG_DEBUG
    void __attribute__((noinline)) sgVerifyFail()
    {
        loge("VERIFY FAIL!!!!\n");
        abort();
    }

    void __attribute__((noinline)) sgVerify(sg s)
    {
        struct sgitem *c, *p = NULL;
        uint32_t sz = 0;

        for(c = s->head; c; p = c, c = c->next)
            sz += c->len;

        if (s->len != sz)
            loge("VERIFY: %u != %u\n", sz, s->len);
        else if (p != s->tail)
            loge("TAIL: 0x%p last: 0x%p\n", s->tail, p);
        else
            return;

        sgVerifyFail();
    }
#else
    #define sgVerify(s)
#endif

/*
 * FUNCTION: sgitemNewWithCopy
 * USE:      Allocates a new sgitem with a copy of passed data
 * PARAMS:   data - the data
 *           len - length of said data
 * RETURN:   the sgitem or NULL
 * NOTES:
 */
static struct sgitem *sgitemNewWithCopy(const void* data, uint32_t len)
{
    struct sgitem *item = malloc(sizeof(struct sgitem) + len);
    if (!item)
        return NULL;

    memcpy(item + 1, data, len);
    item->next = NULL;
    item->len = len;
    item->offset = 0;
    item->data = NULL;

    return item;
}

/*
 * FUNCTION: sgitemNewWithAllocedBuf
 * USE:      Allocates a new sgitem with a given alloced buffer and takes ownership of it
 * PARAMS:   data - the data
 *           len - length of said data
 * RETURN:   the sgitem or NULL
 * NOTES:
 */
static struct sgitem *sgitemNewWithAllocedBuf(void* data, uint32_t len)
{
    struct sgitem *item = malloc(sizeof(struct sgitem));
    if (!item)
        return NULL;

    item->next = NULL;
    item->len = len;
    item->offset = 0;
    item->data = data;

    return item;
}

/*
 * FUNCTION: sgitemFree
 * USE:      Frees an sgitem
 * PARAMS:   item - the item
 * RETURN:   NONE
 * NOTES:
 */
static void sgitemFree(struct sgitem* item)
{
    if (item->data)
        free(item->data);
    free(item);
}

/*
 * FUNCTION: sgitemGetDataPtr
 * USE:      Gets the data pointer from an sgitem
 * PARAMS:   item - the item
 * RETURN:   item's data
 * NOTES:    returned pointer is the pointer to useful data, not necessarily safe to call free() on
 */
static uint8_t* sgitemGetDataPtr(struct sgitem* item)
{
    uint8_t *buf = (uint8_t*)((item->data) ? item->data : (item + 1));

    return buf + item->offset;
}

/*
 * FUNCTION: sgNew
 * USE:      Allocates a new empty sg
 * PARAMS:   NONE
 * RETURN:   the sg or NULL
 * NOTES:
 */
sg sgNew(void)
{
    return calloc(1, sizeof(struct sgitem));
}

/*
 * FUNCTION: sgFree
 * USE:      Frees an sg
 * PARAMS:   s - the sg
 * RETURN:   NONE
 * NOTES:
 */
void sgFree(sg s)
{
    struct sgitem *item = s->head, *t;

    sgVerify(s);
    while (item) {
        t = item;
        item = item->next;
        sgitemFree(t);
    }
    free(s);
}

/*
 * FUNCTION: sgNewWithCopyData
 * USE:      Allocates a new sg with a copy of passed data
 * PARAMS:   data - the data
 *           len - length of said data
 * RETURN:   the sg or NULL
 * NOTES:
 */
sg sgNewWithCopyData(const void* data, uint32_t len)
{
    struct sgitem *item;
    sg s;

    if (!len)
        return sgNew();

    item = sgitemNewWithCopy(data, len);
    s = sgNew();

    if (s && item) {
        s->len = len;
        s->head = item;
        s->tail = item;

        sgVerify(s);
        return s;
    } else if (s)
        sgFree(s);
    else if (item)
        free(item);

    return NULL;
}

/*
 * FUNCTION: sgNewWithAllocedData
 * USE:      Allocates a new sg with a passed data whose owenership we take
 * PARAMS:   data - the data
 *           len - length of said data
 * RETURN:   the sg or NULL
 * NOTES:
 */
sg sgNewWithAllocedData(void *data, uint32_t len)
{
    struct sgitem *item;
    sg s;

    if (!len)
        return sgNew();

    item = sgitemNewWithAllocedBuf(data, len);
    s = sgNew();

    if (s && item) {
        s->len = len;
        s->head = item;
        s->tail = item;

        sgVerify(s);
        return s;
    } else if (s)
        sgFree(s);
    else if (item)
        free(item);

    return NULL;
}

/*
 * FUNCTION: sgLength
 * USE:      Get sg's length
 * PARAMS:   s - the sg
 * RETURN:   the length
 * NOTES:
 */
uint32_t sgLength(sg s)
{
    sgVerify(s);
    return s->len;
}

/*
 * FUNCTION: sgConcat
 * USE:      Concatenate two sg buffers
 * PARAMS:   front - the one to go first
 *           back - the one to go second
 * RETURN:   NONE
 * NOTES:
 */
void sgConcat(sg front, sg back)
{
    sgVerify(front);
    sgVerify(back);

    if (!back->head) {
        sgFree(back);
    }

    if (front->tail)
        front->tail->next = back->head;
    else
        front->head = back->head;
    front->tail = back->tail;

    front->len += back->len;
    free(back);

    sgVerify(front);
}

/*
 * FUNCTION: sgTruncBack
 * USE:      Truncate from the back (delete rear few bytes)
 * PARAMS:   s - the sg
 *           bytes_to_delete - how many bytes to delete
 * RETURN:   NONE
 * NOTES:    if bytes_to_delete > len, all bytes will be deleted
 */
void sgTruncBack(sg s, uint32_t bytes_to_delete)
{
    struct sgitem *item = s->head, *t = NULL;
    uint32_t bytes_to_keep;

    sgVerify(s);
    if (bytes_to_delete >= s->len)
        bytes_to_keep = 0;
    else if (!bytes_to_delete)
        return;
    else
        bytes_to_keep = s->len - bytes_to_delete;

    /* skip all chunks we're wholly keeping */
    s->len = bytes_to_keep;
    while (item && bytes_to_keep >= item->len) {
        bytes_to_keep -= item->len;
        t = item;
        item = item->next;
    }

    /* split chunk if needed */
    if (bytes_to_keep)  {
        item->len = bytes_to_keep;
        t = item;
        item = item->next;
    }

    /* now item points to first item we want to delete, t is the previous item - adjust pointers appropriately */
    if (t)
        t->next = NULL;
    else
        s->head = NULL;
    s->tail = t;

    item = item->next;
    while (item) {
        t = item;
        item = item->next;
        sgitemFree(t);
    }
    sgVerify(s);
}

/*
 * FUNCTION: sgTruncFront
 * USE:      Truncate from the front (delete front few bytes)
 * PARAMS:   s - the sg
 *           bytes_to_delete - how many bytes to delete
 * RETURN:   NONE
 * NOTES:    if bytes_to_delete > len, all bytes will be deleted
 */
void sgTruncFront(sg s, uint32_t bytes_to_delete)
{
    struct sgitem *item = s->head, *t;

    sgVerify(s);
    if (bytes_to_delete >= s->len)
        bytes_to_delete = s->len;

    /* delete all the chunks we wholly don't need */
    s->len -= bytes_to_delete;
    while (item && bytes_to_delete >= item->len) {
        bytes_to_delete -= item->len;
        t = item;
        item = item->next;
        sgitemFree(t);
    }
    s->head = item;
    if (!item)
        s->tail = item;

    /* split chunk if needed */
    if (bytes_to_delete) {
        item->offset += bytes_to_delete;
        item->len -= bytes_to_delete;
        item = item->next;
    }
    sgVerify(s);
}

/*
 * FUNCTION: sgConcatFrontCopy
 * USE:      Append a copy of given data to the front of an sg
 * PARAMS:   s - the sg
 *           data - the data
 *           len - length of said data
 * RETURN:   success
 * NOTES:
 */
bool sgConcatFrontCopy(sg s, const void *data, uint32_t len)
{
    struct sgitem *item;

    sgVerify(s);
    if (!len)
        return true;

    item = sgitemNewWithCopy(data, len);

    if (!item)
        return false;

    item->next = s->head;
    s->head = item;
    if (!s->tail)
        s->tail = item;
    s->len += len;

    sgVerify(s);
    return true;
}

/*
 * FUNCTION: sgConcatFrontAlloced
 * USE:      Append a now-owned-by-us buffer to the front of an sg
 * PARAMS:   s - the sg
 *           data - the data
 *           len - length of said data
 * RETURN:   success
 * NOTES:
 */
bool sgConcatFrontAlloced(sg s, void *data, uint32_t len)
{
    struct sgitem *item;

    sgVerify(s);
    if (!len)
        return true;

    item = sgitemNewWithAllocedBuf(data, len);

    if (!item)
        return false;

    item->next = s->head;
    s->head = item;
    if (!s->tail)
        s->tail = item;
    s->len += len;

    sgVerify(s);
    return true;
}

/*
 * FUNCTION: sgConcatBackCopy
 * USE:      Append a copy of given data to the back of an sg
 * PARAMS:   s - the sg
 *           data - the data
 *           len - length of said data
 * RETURN:   success
 * NOTES:
 */
bool sgConcatBackCopy(sg s, const void *data, uint32_t len)
{
    struct sgitem *item;

    sgVerify(s);
    if (!len)
        return true;

    item = sgitemNewWithCopy(data, len);

    if (!item)
        return false;

    if (s->tail)
        s->tail->next = item;
    else
        s->head = item;
    s->tail = item;
    s->len += len;

    sgVerify(s);
    return true;
}

/*
 * FUNCTION: sgConcatBackAlloced
 * USE:      Append a now-owned-by-us buffer to the back of an sg
 * PARAMS:   s - the sg
 *           data - the data
 *           len - length of said data
 * RETURN:   success
 * NOTES:
 */
bool sgConcatBackAlloced(sg s, void *data, uint32_t len)
{
    struct sgitem *item;

    sgVerify(s);
    if (!len)
        return true;

    item = sgitemNewWithAllocedBuf(data, len);

    if (!item)
        return false;

    if (s->tail)
        s->tail->next = item;
    else
        s->head = item;
    s->tail = item;
    s->len += len;

    sgVerify(s);
    return true;
}

/*
 * FUNCTION: sgSplit
 * USE:      Split an sg into two
 * PARAMS:   s - the sg
 *           firstLen - how mnay bytes to leave with the first part
 * RETURN:   the sg containing the second part or NULL on error;
 * NOTES:
 */
sg sgSplit(sg s, uint32_t firstPart)
{
    struct sgitem *i = s->head, *p = NULL, *t;
    uint32_t leftToCut = firstPart;
    sg ret = sgNew();

    sgVerify(s);
    if (!ret)
        return NULL;


    /* first, check for stupid inputs (part 1: cut off all that we have) */
    if (!firstPart) {
        /*
         * Ah, the "cut off everything" case. We simply swap the empty sg we
         * just created with s, and s's internal structure into ret. Dirty?
         * Sure. Clever? Definitely!
         */

        sgSwap(s, ret);
        sgVerify(s);
        sgVerify(ret);
        return ret;
    }

    /* first, check for stupid inputs (part 2: cut off nothing) */
    if (firstPart >= s->len) {
        /* nothing to cut off */
        sgVerify(s);
        sgVerify(ret);
        return ret;
    }

    /* if we got here, we have actual splitting we need to do... */
    ret->len = s->len - firstPart;
    s->len = firstPart;
    while (i && i->len <= leftToCut) {
        leftToCut -= i->len;
        p = i;
        i = i->next;
    }

    if (!leftToCut) { /* exact boundary. nice! */
        ret->tail = s->tail;
        p->next = NULL;
        s->tail = p;

        ret->head = i;
    } else { /* this is the messiest case */
        t = sgitemNewWithCopy(sgitemGetDataPtr(i) + leftToCut, i->len - leftToCut);
        if (!t) {
            sgFree(ret);
            sgVerify(s);
            return NULL;
        }
        i->len = leftToCut;

        ret->tail = s->tail == i ? t : s->tail;
        ret->head = t;
        t->next = i->next;
        i->next = NULL;
        s->tail = i;
    }

    sgVerify(s);
    sgVerify(ret);
    return ret;
}

/*
 * FUNCTION: sgSerialize
 * USE:      Serialize a piece of the sg into a buffer
 * PARAMS:   s - the sg
 *           data - the data
 *           len - length of said data
 * RETURN:   number of bytes produced
 * NOTES:
 */
uint32_t sgSerialize(sg s, uint32_t offset, uint32_t len, void* dstBuf)
{
    uint8_t *dst = (uint8_t*)dstBuf;
    struct sgitem *item = s->head;
    uint32_t origlen = len;

    sgVerify(s);
    /* skip all complete items we do not care about */
    while (item && item->len <= offset) {
        offset -= item->len;
        item = item->next;
    }

    while (item && len) {
        uint8_t *data = sgitemGetDataPtr(item) + offset;
        uint32_t avail = item->len - offset;
        uint32_t want = len;

        offset = 0;
        if (want > avail)
            want = avail;

        memcpy(dst, data, want);
        dst += want;
        len -= want;
        item = item->next;
    }

    return origlen - len;
}

/*
 * FUNCTION: sgSwap
 * USE:      Swap the data in a a and b
 * PARAMS:   a - an sg
 *           b - an sg
 * RETURN:   NONE
 * NOTES:
 */
void sgSwap(sg a, sg b)
{
    struct sgbuf t;

    sgVerify(a);
    sgVerify(b);

    memcpy(&t, a, sizeof(struct sgbuf));
    memcpy(a, b, sizeof(struct sgbuf));
    memcpy(b, &t, sizeof(struct sgbuf));

    sgVerify(a);
    sgVerify(b);
}

/*
 * FUNCTION: sgIterStart
 * USE:      Start an iteration over the pieces of an sg
 * PARAMS:   s - the sg
 * RETURN:   an iterator pointer or NULL if no chunks to iterate over
 * NOTES:
 */
void* sgIterStart(sg s)
{
    return s->head;
}

/*
 * FUNCTION: sgIterCurLen
 * USE:      Get "current" length in an iteration ofer an sg
 * PARAMS:   iter - the iterator (secretly an sgitem*)
 * RETURN:   current length
 * NOTES:
 */
uint32_t sgIterCurLen(void *iter)
{
    struct sgitem *item = (struct sgitem*)iter;

    return item->len;
}

/*
 * FUNCTION: sgIterCurData
 * USE:      Get "current" data in an iteration ofer an sg
 * PARAMS:   iter - the iterator (secretly an sgitem*)
 * RETURN:   current data pointer
 * NOTES:
 */
const void* sgIterCurData(void *iter)
{
    struct sgitem *item = (struct sgitem*)iter;

    return sgitemGetDataPtr(item);
}

/*
 * FUNCTION: sgIterAdvance
 * USE:      Move an sg iterator forward one chunk
 * PARAMS:   iter - the iterator (secretly an sgitem*)
 * RETURN:   new iterator value or NULL if no more left to iterate over
 * NOTES:
 */
void* sgIterAdvance(void *iter)
{
    struct sgitem *item = (struct sgitem*)iter;

    return item->next;
}

/*
 * FUNCTION: sgSerializeCutFront
 * USE:      Serialize first ln bytes of sg into a buffer, then cut them of
 * PARAMS:   s - the SG
 *           buf - destination
 *           len - num bytes to cut
 * RETURN:   true if all went well
 * NOTES:
 */
bool sgSerializeCutFront(sg s, void *buf, uint32_t len)
{
    sgVerify(s);
    if (sgLength(s) < len)
        return false;
    sgSerialize(s, 0, len, buf);
    sgTruncFront(s, len);
    return true;
}


