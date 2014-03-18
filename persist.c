#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include "persist.h"
#include "config.h"
#include "util.h"
#include "log.h"
#include "hci.h"
#include "mt.h"
#include "bt.h"


struct persistArrayInRam {
    struct persistProp *head;
    struct persistProp *tail;
};

struct persistPropOnDisk {
    uint32_t name;
    uint32_t type;
    uint32_t len; /* in RAM set to sizeof(persistArrayInRam) for arrays. on disk set to sum of actual lengths of children */
    /* data here */
} __packed;

struct persistProp {
    struct persistProp *next;
    struct persistProp *prev;
    struct persistPropOnDisk data;
};

struct persistFileHdr {
    uint32_t magic;
    uint32_t version;
} __packed;


#define PERSIST_MAGIC                 0x374940ED

#define PERSIST_VERSION_1             0x00000001
#define PERSIST_VERSION_CUR           PERSIST_VERSION_1


static pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
static struct persistProp *mPropsHead = NULL;
static struct persistProp *mPropsTail = NULL;



/* fwd decls */
static bool persistPropListLoad(struct persistProp **headP, struct persistProp **tailP, int fd, uint64_t len);

/*
 * FUNCTION: persistPropAlloc
 * USE:      Allocate an property struct
 * PARAMS:   name - the name to give it
 *           type - the type to set
 *           len - number fo extra bytes to allocate
 * RETURN:   the property or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAlloc(uint32_t name, uint32_t type, uint32_t len)
{
    struct persistProp *t = (struct persistProp*)calloc(1, sizeof(struct persistProp) + len);
    if (t) {
        t->data.name = name;
        t->data.type = type;
        t->data.len = len;
    }
    return t;
}

/*
 * FUNCTION: persistPropFree
 * USE:      Free an property struct
 * PARAMS:   p - the onject
 * RETURN:   NONE
 * NOTES:
 */
static void persistPropFree(struct persistProp *p)
{
    if (p->data.type == PERS_TYPE_ARRAY) {
        struct persistArrayInRam *arr = (struct persistArrayInRam*)(p + 1);
        struct persistProp *t = arr->head, *curr;

        while (t) { /* free all children in arrays */
            curr = t;
            t = t->next;
            persistPropFree(curr);
        }
    }
    free(p);
}

/*
 * FUNCTION: persistFree
 * USE:      Free all property structs
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    call with mLock held
 */
static void persistFree(void)
{
    while (mPropsHead) {
        mPropsTail = mPropsTail;
        mPropsHead = mPropsHead->next;
        persistPropFree(mPropsTail);
    }
    mPropsTail = NULL;
}

/*
 * FUNCTION: persistPropFind
 * USE:      Find a property with a given name and type in a given property list
 * PARAMS:   head - the list we're looking in
 *           name - the name we are looking for
 *           type - the type we're looking for
 * RETURN:   prooperty structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropFind(struct persistProp *head, uint32_t name, uint32_t type)
{
    while (head && head->data.type != type && head->data.name != name)
        head = head->next;

    return head;
}

/*
 * FUNCTION: persistPropAdd
 * USE:      Add property to a given property list
 * PARAMS:   headP - where list head is stored
 *           tailP - whwr elist tail is stored
 *           name - the name to use
 *           type - the type to use
 *           len - data length
 *           data - the data or NULL to leav zero-filled
 * RETURN:   the prop structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAdd(struct persistProp **headP, struct persistProp **tailP, uint32_t name, uint32_t type, uint32_t len, const void *data)
{
    struct persistProp *t = persistPropAlloc(name, type, len);

    if (!t)
        return NULL;

    if (data)
        memcpy(t + 1, data, len);

    t->prev = *tailP;
    if (*tailP)
        (*tailP)->next = t;
    else
        (*headP) = t;
    (*tailP) = t;

    return t;
}

/*
 * FUNCTION: persistPropAddByteArr
 * USE:      Add a byte array to a given property list
 * PARAMS:   headP - where list head is stored
 *           tailP - whwr elist tail is stored
 *           name - the name to use
 *           len - data length
 *           data - the data
 * RETURN:   the prop structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAddByteArr(struct persistProp **headP, struct persistProp **tailP, uint32_t name, uint32_t len, const void *data)
{
    return persistPropAdd(headP, tailP, name, PERS_TYPE_BYTE_ARRAY, len, data);
}

/*
 * FUNCTION: persistPropAddBtAddr
 * USE:      Add a bt_addr to a given property list
 * PARAMS:   headP - where list head is stored
 *           tailP - whwr elist tail is stored
 *           name - the name to use
 *           len - data length
 *           data - the data
 * RETURN:   the prop structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAddBtAddr(struct persistProp **headP, struct persistProp **tailP, uint32_t name, const struct bt_addr *addr)
{
    return persistPropAdd(headP, tailP, name, PERS_TYPE_BYTE_ARRAY, sizeof(struct bt_addr), addr);
}

/*
 * FUNCTION: persistPropAddEmptyArray
 * USE:      Add an empty element array to a given property list
 * PARAMS:   headP - where list head is stored
 *           tailP - whwr elist tail is stored
 *           name - the name to use
 * RETURN:   the prop structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAddEmptyArray(struct persistProp **headP, struct persistProp **tailP, uint32_t name)
{
    return persistPropAdd(headP, tailP, name, PERS_TYPE_ARRAY, sizeof(struct persistArrayInRam), NULL);
}

/*
 * FUNCTION: persistPropAddInt
 * USE:      Add a uint to a given property list
 * PARAMS:   headP - where list head is stored
 *           tailP - whwr elist tail is stored
 *           name - the name to use
 *           isSigned - is the integer signed
 *           len - data length
 *           val - the integer
 * RETURN:   the prop structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistPropAddInt(struct persistProp **headP, struct persistProp **tailP, uint32_t name, bool isSigned, uint32_t len, uint64_t val)
{
    uint8_t v8 = val;
    uint16_t v16 = val;
    uint32_t v32 = val;
    uint64_t v64 = val;
    const void *vp;

    switch (len) {
    case sizeof(v8):
        vp = &v8;
        break;
    case sizeof(v16):
        vp = &v16;
        break;
    case sizeof(v32):
        vp = &v32;
        break;
    case sizeof(v64):
        vp = &v64;
        break;
    default:
        loge("Unknown integer size %u\n", len);
        return false;
    }
    return persistPropAdd(headP, tailP, name, isSigned ? PERS_TYPE_SINT : PERS_TYPE_UINT, len, vp);
}

/*
 * FUNCTION: persistPropLoad
 * USE:      Load a property from a file descriptor
 * PARAMS:   fd - the file descriptor
 *           lenP - in/out: number of bytes left to use
 * RETURN:   the prop structure or NULL on error
 * NOTES:    recursive
 */
static struct persistProp* persistPropLoad(int fd, uint64_t *lenP)
{
    struct persistArrayInRam *arr;
    struct persistPropOnDisk p;
    struct persistProp *prop;
    uint32_t type, inRamLen;
    uint64_t onDiskLen;
    uint8_t buf[sizeof(uint64_t)];


    if (*lenP < sizeof(p))
        return NULL;

    if (!r_read(fd, &p, sizeof(p)))
        return NULL;

    (*lenP) -= sizeof(p);
    type = utilGetBE32(&p.type);
    onDiskLen = utilGetBE32(&p.len);
    if (*lenP < onDiskLen)
        return NULL;

    *lenP -= onDiskLen;
    inRamLen = (type == PERS_TYPE_ARRAY) ? sizeof(struct persistArrayInRam) : onDiskLen;

    prop = persistPropAlloc(utilGetBE32(&p.name), type, inRamLen);
    if (!prop)
        return NULL;

    switch (type) {
    case PERS_TYPE_UINT:
    case PERS_TYPE_SINT:
        if (!r_read(fd, buf, inRamLen)) {
            loge("Read error\n");
            break;
        }
        switch (inRamLen) {
        case sizeof(uint8_t):
            *(uint8_t*)(prop + 1) = utilGetBE8(buf);
            return prop;
        case sizeof(uint16_t):
            *(uint8_t*)(prop + 1) = utilGetBE16(buf);
            return prop;
        case sizeof(uint32_t):
            *(uint8_t*)(prop + 1) = utilGetBE32(buf);
            return prop;
        case sizeof(uint64_t):
            *(uint8_t*)(prop + 1) = utilGetBE64(buf);
            return prop;
        default:
            loge("Unknown integer length %u\n", inRamLen);
            break;
        }
        break;
    case PERS_TYPE_BYTE_ARRAY:
        if (r_read(fd, prop + 1, inRamLen))
            return prop;
        loge("Read error\n");
        break;
    case PERS_TYPE_BT_ADDR:
        if (inRamLen != sizeof(struct bt_addr)) {
            loge("Invalid bt_addr length %u\n", inRamLen);
            break;
        }
        if (!r_read(fd, prop + 1, inRamLen))
            return prop;
        break;
    case PERS_TYPE_ARRAY:
        arr = (struct persistArrayInRam*)(prop + 1);
        if (!persistPropListLoad(&arr->head, &arr->tail, fd, onDiskLen)) {
            loge("Sub-load failed\n");
            break;
        }
        return prop;
    default:
        loge("Unknown property type %u\n", type);
        break;
    }

    persistPropFree(prop);
    return NULL;
}

/*
 * FUNCTION: persistLoadRequiredProps
 * USE:      Load default properties into the RAM structure if they are not present
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    call with mLock held
 */
static void persistLoadRequiredProps(void)
{
    static const char defaultName[] = "NB.t";

    if (!persistPropFind(mPropsHead, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY))
        if (!persistPropAddByteArr(&mPropsHead, &mPropsTail, PERS_NAME_DEVICE_NAME, strlen(defaultName), defaultName))
            loge("Failed to add device name default prop\n");

    if (!persistPropFind(mPropsHead, PERS_NAME_DISC_TIMEOUT, PERS_TYPE_UINT))
        if (!persistPropAddInt(&mPropsHead, &mPropsTail, PERS_NAME_DISC_TIMEOUT, false, sizeof(uint8_t), 60) /* 60 seconds seems reasonable */)
            loge("Failed to add discovery length default prop\n");

    if (!persistPropFind(mPropsHead, PERS_NAME_DISC_TIMEOUT, PERS_TYPE_UINT))
        if (!persistPropAddEmptyArray(&mPropsHead, &mPropsTail, PERS_NAME_BONDED_DEVICES))
            loge("Failed to add bonded devices default prop\n");
}

/*
 * FUNCTION: persistPropCalcListSize
 * USE:      Calculate how many bytes the list of properties will take on disk
 * PARAMS:   head - the head of the list
 * RETURN:   number of bytes
 * NOTES:    recursive
 */
static uint32_t persistPropCalcListSize(const struct persistProp *head)
{
    uint32_t len = 0;

    while (head) {
        struct persistArrayInRam *arr = (struct persistArrayInRam*)(head + 1);

        len += sizeof(struct persistPropOnDisk);

        if (head->data.type == PERS_TYPE_ARRAY)
            len += persistPropCalcListSize(arr->head);
        else
            len += head->data.len;

        head = head->next;
    }

    return len;
}

/*
 * FUNCTION: persistPropStore
 * USE:      Write a single property to a given file descriptor
 * PARAMS:   fd - the file descriptor
 *           prop - the property
 * RETURN:   true if all went well
 * NOTES:    recursive, call with mLock held
 */
static bool persistPropStore(int fd, const struct persistProp *prop)
{
    struct persistArrayInRam *arr = (struct persistArrayInRam*)(prop + 1);
    uint8_t buf[sizeof(uint64_t)];
    const struct persistProp *t;
    struct persistPropOnDisk p;

    utilSetBE32(&p.name, prop->data.name);
    utilSetBE32(&p.type, prop->data.type);

    if (prop->data.type == PERS_TYPE_ARRAY)
        utilSetBE32(&p.len, persistPropCalcListSize(arr->head));
    else
        utilSetBE32(&p.len, prop->data.len);

    if (!r_write(fd, &p, sizeof(p))) {
        loge("Failed to write prop header\n");
        return false;
    }

    switch (prop->data.type) {
    case PERS_TYPE_UINT:
    case PERS_TYPE_SINT:
        switch (prop->data.len) {
        case sizeof(uint8_t):
            utilSetBE8(buf, *(uint8_t*)(prop + 1));
            break;
        case sizeof(uint16_t):
            utilSetBE16(buf, *(uint16_t*)(prop + 1));
            break;
        case sizeof(uint32_t):
            utilSetBE32(buf, *(uint32_t*)(prop + 1));
            break;
        case sizeof(uint64_t):
            utilSetBE64(buf, *(uint64_t*)(prop + 1));
            break;
        default:
            loge("Unknown integer size %u\n", prop->data.len);
            return false;
        }
        return r_write(fd, buf, prop->data.len);

    case PERS_TYPE_BYTE_ARRAY:
        return r_write(fd, prop + 1, prop->data.len);

    case PERS_TYPE_BT_ADDR:
        if (prop->data.len != sizeof(struct bt_addr)) {
            loge("Bad bt_addr size %u\n", prop->data.len);
            return false;
        }
        return r_write(fd, prop + 1, sizeof(struct bt_addr));
    case PERS_TYPE_ARRAY:
        for (t = arr->head; t; t = t->next) {
            if (!persistPropStore(fd, t)) {
                loge("Failed to write sub-property\n");
                return false;
            }
        }
        return true;
    default:
        loge("Unknown property type %u\n", prop->data.type);
        return false;
    }
}

/*
 * FUNCTION: persistStore
 * USE:      Store persistent preferences to the preferences file
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    call with mLock held
 */
static void persistStore(void)
{
    struct persistFileHdr hdr;
    struct persistProp *t;
    int fd;


    utilSetBE32(&hdr.magic, PERSIST_MAGIC);
    utilSetBE32(&hdr.version, PERSIST_VERSION_CUR);

    fd = open(PREF_PATH, O_WRONLY);
    if (fd == -1) {
        loge("Preferences not opened\n");
        return;
    }

    if (!r_write(fd, &hdr, sizeof(hdr))) {
        loge("Failed to write pref header\n");
        goto fail;
    }

    for (t = mPropsHead; t; t = t->next) {
        if (!persistPropStore(fd, t)) {
            loge("Failed to write prop\n");
            goto fail;
        }
    }

    close(fd);
    return;

fail:
    close(fd);
    unlink(PREF_PATH); /* better no file than bad file */
}

/*
 * FUNCTION: persistPropListLoad
 * USE:      Load a list of properties from an fd
 * PARAMS:   headP - where list head is stored
 *           tailP - where list tail is stored
 *           fd - the fd to read from
 *           len - how many bytes to consume
 * RETURN:   true on success
 * NOTES:
 */
static bool persistPropListLoad(struct persistProp **headP, struct persistProp **tailP, int fd, uint64_t len)
{
    struct persistProp *t;

    while ((t = persistPropLoad(fd, &len))) {
        t->prev = (*tailP);
        if (*tailP)
            (*tailP)->next = t;
        else
            (*headP) = t;
        (*tailP) = t;
    }

    return !len;
}

/*
 * FUNCTION: persistLoad
 * USE:      Load persistent preferences from the preferences file
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    call with mLock held
 */
static void persistLoad(void)
{
    const struct persistProp *t;
    struct persistFileHdr hdr;
    uint64_t fileLen;
    int fd;

    mPropsHead = NULL;
    mPropsTail = NULL;

    fd = open(PREF_PATH, O_RDONLY);
    if (fd == -1) {
        logi("Preferences not found\n");
        goto out;
    }

    if (!r_read(fd, &hdr, sizeof(hdr))) {
        logw("Failed to read header\n");
        goto out_close;
    }

    if (utilGetBE32(&hdr.magic) != PERSIST_MAGIC) {
        logw("Bad magic\n");
        goto out_close;
    }

    if (utilGetBE32(&hdr.version) > PERSIST_VERSION_CUR) {
        logw("Version too new\n");
        goto out_close;
    }

    if (utilGetBE32(&hdr.version) < PERSIST_VERSION_CUR) {
        /* future code to read old versions may live here, but for now fail */
        goto out_close;
    }

    fileLen = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (!persistPropListLoad(&mPropsHead, &mPropsTail, fd, fileLen))
        loge("Failed to read preference file\n");

out_close:
    close(fd);

out:
    logi("Adding default preferences if needed\n");
    persistLoadRequiredProps();
    persistStore();
}
