/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "newblue-macros.h"
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
    struct persistPropOnDisk info;
};

struct persistFileHdr {
    uint32_t magic;
    uint32_t version;
} __packed;

struct persistKey {
    uint8_t keyType;
    uint8_t key[HCI_LINK_KEY_LEN];
} __packed;

#define PERSIST_MAGIC                    0x374940ED

#define PERSIST_VERSION_1                0x00000001
#define PERSIST_VERSION_CUR              PERSIST_VERSION_1




/* names */
#define PERS_NAME_DEVICE_NAME            0x00000001 /* type: PERS_TYPE_BYTE_ARRAY, size: 0..HCI_DEV_NAME_LEN */
#define PERS_NAME_DISC_TIMEOUT           0x00000002 /* type: PERS_TYPE_UINT, size: 1 */
#define PERS_NAME_DEV_CLS                0x00000003 /* type: PERS_TYPE_UINT, size: 4 */
#define PERS_NAME_KEYS                   0x00000004 /* type: PERS_TYPE_ARRAY of {PERS_NAME_KEY} */
#define PERS_NAME_KEY                    0x00000005 /* type: PERS_TYPE_BYTE_ARRAY, size: sizeof(struct persistKey) */
#define PERS_NAME_DEVICE                 0x00000006 /* type: PERS_TYPE_ARRAY of {PERS_NAME_DEVICE_NAME, PERS_NAME_BT_ADDR, PERS_NAME_DEV_CLS, PERS_NAME_KEYS, PERS_NAME_LAST_SEEN} */
#define PERS_NAME_KNOWN_DEVICES          0x00000007 /* type: PERS_TYPE_ARRAY of PERS_NAME_KNOWN_DEVICES */
#define PERS_NAME_BT_ADDR                0x00000008 /* type: PERS_TYPE_BT_ADDR */
#define PERS_NAME_LAST_SEEN              0x00000009 /* type: PERS_TYPE_UINT, size: 8, milliseconds since epoch */
#define PERS_NAME_NUMBERS                0x0000000a /* type: PERS_TYPE_ARRAY of {PERS_NAME_NUM} */
#define PERS_NAME_NUM                    0x0000000b /* type: PERS_TYPE_ARRAY of 2 elems {PERS_NAME_NUM_NAME, PERS_NAME_NUM_VAL} */
#define PERS_NAME_NUM_NAME               0x0000000c /* type: PERS_TYPE_UINT, size: 1 */
#define PERS_NAME_NUM_VAL                0x0000000d /* type: PERS_TYPE_UINT, size: 8 */
#define PERS_NAME_IDENTITY_ADDR          0x0000000e /* type: PERS_TYPE_BT_ADDR */
#define PERS_NAME_LATEST_ADDR            0x0000000f /* type: PERS_TYPE_BT_ADDR */

/* types */
#define PERS_TYPE_UINT                   0x00000001
#define PERS_TYPE_SINT                   0x00000002
#define PERS_TYPE_BYTE_ARRAY             0x00000003
#define PERS_TYPE_BT_ADDR                0x00000004
#define PERS_TYPE_ARRAY                  0x00000005

#define MAX_DEVICE_AGE                   5 * 60 * 1000 /* Maximum device age before getting garbage collected (5 minutes) */

#define IDENTITY_ADDR_TYPE_PUBLIC        0
#define IDENTITY_ADDR_TYPE_STATIC_RANDOM 1

static pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
static struct persistProp *mPropsHead = NULL;
static struct persistProp *mPropsTail = NULL;



/* fwd decls */
static bool persistPropListLoad(struct persistProp **headP, struct persistProp **tailP, int fd, uint64_t len);
static bool persistSetPropInList(struct persistProp **headP, struct persistProp **tailP, uint32_t name, uint32_t type, const void *buf, uint32_t len, bool replaceExisting);
static bool persistAddNumber(struct persistProp **headP, struct persistProp **tailP, uint8_t numType, uint64_t num);
static struct persistProp* persistFindNum(struct persistProp *head, uint8_t numType);

/*
 * FUNCTION: persistGetRealtime
 * USE:      Get clock time at a uint64_t
 * PARAMS:   NONE
 * RETURN:   time in milliseconds as a uint64
 * NOTES:
 */
static uint64_t persistGetRealtime(void)
{
    struct timespec ts;
    uint64_t time;

    clock_gettime(CLOCK_REALTIME, &ts);
    time = ts.tv_sec;
    time *= 1000;
    time += ts.tv_nsec / 1000000UL;

    return time;
}

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
        t->info.name = name;
        t->info.type = type;
        t->info.len = len;
    }
    return t;
}

/*
 * FUNCTION: persistPropFree
 * USE:      Free a property struct
 * PARAMS:   p - the object
 * RETURN:   NONE
 * NOTES:
 */
static void persistPropFree(struct persistProp *p)
{
    if (p->info.type == PERS_TYPE_ARRAY) {
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
 * FUNCTION: persistPropDelete
 * USE:      Delete a property struct from the list it is in & then free it
 * PARAMS:   headP - list head stored here
 *           tailP - lsit tail stored here
 *           p - the object
 * RETURN:   NONE
 * NOTES:
 */
static void persistPropDelete(struct persistProp **headP, struct persistProp **tailP, struct persistProp *p)
{
    if (p->next)
        p->next->prev = p->prev;
    else
        (*tailP) = p->prev;

    if (p->prev)
        p->prev->next = p->next;
    else
        (*headP) = p->next;

    persistPropFree(p);
}

/*
 * FUNCTION: persistFree
 * USE:      Free all property structs
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:    call with mLock held
 */
void persistFree(void)
{
    while (mPropsHead) {
        mPropsTail = mPropsHead;
        mPropsHead = mPropsHead->next;
        persistPropFree(mPropsTail);
    }
    mPropsTail = mPropsHead = NULL;
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
    while (head && (head->info.type != type || head->info.name != name))
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
    uint8_t buf[sizeof(uint64_t)];
    struct persistArrayInRam *arr;
    struct persistPropOnDisk p;
    struct persistProp *prop;
    uint32_t type, inRamLen;
    struct bt_addr *addr;
    uint64_t onDiskLen;

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
            *(uint16_t*)(prop + 1) = utilGetBE16(buf);
            return prop;
        case sizeof(uint32_t):
            *(uint32_t*)(prop + 1) = utilGetBE32(buf);
            return prop;
        case sizeof(uint64_t):
            *(uint64_t*)(prop + 1) = utilGetBE64(buf);
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
        addr = (struct bt_addr*)(prop + 1);
        if (!r_read(fd, addr, inRamLen))
            break;
        if (addr->type >= BT_ADDR_TYPE_NUM) {
            loge("Invalid device addr type\n");
            break;
        }
        return prop;
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
 * FUNCTION: persistFindKey
 * USE:      Find a given key in a given property list
 * PARAMS:   head - list head
 *           keyType - key type to find
 * RETURN:   key's property structure or NULL on error
 * NOTES:
 */
static struct persistProp* persistFindKey(struct persistProp *head, uint8_t keyType)
{
    while (head) {

        if (head->info.name == PERS_NAME_KEY && head->info.type == PERS_TYPE_BYTE_ARRAY && head->info.len == sizeof(struct persistKey)) {
            struct persistKey *key = (struct persistKey*)(head + 1);
            if (key->keyType == keyType)
                break;
        }

        head = head->next;
    }
    return head;
}

/*
 * FUNCTION: persistGenRandomBytes
 * USE:      Generate random bytes
 * PARAMS:   out - where the random bytes are stored
 *           len - the size of desired length of random bytes
 * RETURN:   success with the valid output
 * NOTES:
 */
static bool persistGenRandomBytes(uint8_t *out, uint8_t len)
{
    int fd;
    bool ret;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1) {
        logw("Failed to open urandom\n");
        return false;
    }

    ret = r_read(fd, out, len);
    close(fd);

    if (!ret) {
        logw("Failed to read urandom\n");
        return false;
    }
    return true;
}

/*
 * FUNCTION: persistAddKey
 * USE:      Add a key of the given type and add it to the given list (and optionally generate it)
 * PARAMS:   headP - list head stored here
 *           tailP - lsit tail stored here
 *           keyType - key type to generate
 *           key - key or NULL if you want it auto-generated
 * RETURN:   success
 * NOTES:
 */
static bool persistAddKey(struct persistProp **headP, struct persistProp **tailP, uint8_t keyType, const uint8_t *key)
{
    struct persistKey pkey;

    pkey.keyType = keyType;

    if (key)
        memcpy(pkey.key, key, sizeof(pkey.key));
    else {
        if (!persistGenRandomBytes(pkey.key, HCI_LE_KEY_LEN))
            return false;
    }

    return persistSetPropInList(headP, tailP, PERS_NAME_KEY, PERS_TYPE_BYTE_ARRAY, &pkey, sizeof(pkey), false);
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
    struct persistProp *keylist, *key;

    if (!persistPropFind(mPropsHead, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY))
        if (!persistPropAddByteArr(&mPropsHead, &mPropsTail, PERS_NAME_DEVICE_NAME, strlen(defaultName), defaultName))
            loge("Failed to add device name default prop\n");

    if (!persistPropFind(mPropsHead, PERS_NAME_DISC_TIMEOUT, PERS_TYPE_UINT))
        if (!persistPropAddInt(&mPropsHead, &mPropsTail, PERS_NAME_DISC_TIMEOUT, false, sizeof(uint8_t), 60) /* 60 seconds seems reasonable */)
            loge("Failed to add discovery length default prop\n");

    if (!persistPropFind(mPropsHead, PERS_NAME_KNOWN_DEVICES, PERS_TYPE_ARRAY))
        if (!persistPropAddEmptyArray(&mPropsHead, &mPropsTail, PERS_NAME_KNOWN_DEVICES))
            loge("Failed to add known devices default prop\n");

    keylist = persistPropFind(mPropsHead, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
    if (!keylist)
        keylist = persistPropAddEmptyArray(&mPropsHead, &mPropsTail, PERS_NAME_KEYS);
    if (!keylist)
        loge("Failed to add device keys default prop\n");
    else {
        struct persistArrayInRam *keyArr = (struct persistArrayInRam*)(keylist + 1);
        key = keyArr->head;

        /* delete all keys that are invalid */
        while ((key = persistPropFind(key, PERS_NAME_KEY, PERS_TYPE_ARRAY))) {
            if (key->info.len != sizeof(struct persistKey)) {
                persistPropDelete(&keyArr->head, &keyArr->tail, key);
                key = keyArr->head;
            }
        }

        if (!persistFindKey(keyArr->head, KEY_TYPE_CSRK))
            if (!persistAddKey(&keyArr->head, &keyArr->tail, KEY_TYPE_CSRK, NULL))
                loge("failed to create & store CSRK\n");

        if (!persistFindKey(keyArr->head, KEY_TYPE_IRK))
            if (!persistAddKey(&keyArr->head, &keyArr->tail, KEY_TYPE_IRK, NULL))
                loge("failed to create & store IRK\n");

        if (!persistFindKey(keyArr->head, KEY_TYPE_DHK))
            if (!persistAddKey(&keyArr->head, &keyArr->tail, KEY_TYPE_DHK, NULL))
                loge("failed to create & store DHK\n");
    }
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

        if (head->info.type == PERS_TYPE_ARRAY)
            len += persistPropCalcListSize(arr->head);
        else
            len += head->info.len;

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

    utilSetBE32(&p.name, prop->info.name);
    utilSetBE32(&p.type, prop->info.type);

    if (prop->info.type == PERS_TYPE_ARRAY)
        utilSetBE32(&p.len, persistPropCalcListSize(arr->head));
    else
        utilSetBE32(&p.len, prop->info.len);

    if (!r_write(fd, &p, sizeof(p))) {
        loge("Failed to write prop header\n");
        return false;
    }

    switch (prop->info.type) {
    case PERS_TYPE_UINT:
    case PERS_TYPE_SINT:
        switch (prop->info.len) {
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
            loge("Unknown integer size %u\n", prop->info.len);
            return false;
        }
        return r_write(fd, buf, prop->info.len);

    case PERS_TYPE_BYTE_ARRAY:
        return r_write(fd, prop + 1, prop->info.len);

    case PERS_TYPE_BT_ADDR:
        if (prop->info.len != sizeof(struct bt_addr)) {
            loge("Bad bt_addr size %u\n", prop->info.len);
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
        loge("Unknown property type %u\n", prop->info.type);
        return false;
    }
}

/*
 * FUNCTION: persistStore
 * USE:      Store persistent preferences to the preferences file
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void persistStore(void)
{
    struct persistFileHdr hdr;
    struct persistProp *t;
    int fd;
    char *temp_path;

    pthread_mutex_lock(&mLock);
    utilSetBE32(&hdr.magic, PERSIST_MAGIC);
    utilSetBE32(&hdr.version, PERSIST_VERSION_CUR);

    if (asprintf(&temp_path, "%s.XXXXXX", pref_path) < 0)
        goto unlock;

    fd = mkstemp(temp_path);
    if (fd < 0) {
        loge("failed creating temp file %s\n", temp_path);
        goto fail;
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
    if (rename(temp_path, pref_path) < 0)
        loge("failed renaming %s to %s\n", temp_path, pref_path);

    unlink(temp_path);
    free(temp_path);
    pthread_mutex_unlock(&mLock);
    return;

fail:
    close(fd);
    unlink(temp_path);
    free(temp_path);
unlock:
    pthread_mutex_unlock(&mLock);
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
 * NOTES:
 */
void persistLoad(void)
{
    struct persistFileHdr hdr;
    uint64_t fileLen;
    int fd;


    pthread_mutex_lock(&mLock);

    mPropsHead = NULL;
    mPropsTail = NULL;

    fd = open(pref_path, O_RDONLY);
    if (fd == -1) {
        logi("Preferences not found\n");
        goto out;
    }

    fileLen = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
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

    fileLen -= sizeof(struct persistFileHdr);
    if (!persistPropListLoad(&mPropsHead, &mPropsTail, fd, fileLen))
        loge("Failed to read preference file\n");

out_close:
    close(fd);

out:
    logi("Adding default preferences if needed\n");
    persistLoadRequiredProps();
    pthread_mutex_unlock(&mLock);
    persistStore();
}

/*
 * FUNCTION: persistGetDeviceName
 * USE:      Get stored device name for our device
 * PARAMS:   buf - name gets stored here. Should fit at least HCI_DEV_NAME_LEN bytes
 * RETURN:   length of name returned
 * NOTES:    NONE
 */
uint32_t persistGetDeviceName(void* buf)
{
    struct persistProp *t;
    uint32_t ret = 0;

    pthread_mutex_lock(&mLock);
    t = persistPropFind(mPropsHead, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY);
    if (!t)
        logw("Device name property not found\n");
    else {
        ret = t->info.len;
        if (t->info.len > HCI_DEV_NAME_LEN) {
            logw("Device name property too long - truncating\n");
            ret = HCI_DEV_NAME_LEN;
        }
        memcpy(buf, t + 1, ret);
    }
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistSetPropInList
 * USE:      Set a property in a given property list
 * PARAMS:   headP - head of list stored here
 *           tailP- tail of list stored here
 *           name - name of the property
 *           type - type of the property
 *           buf - the data to save
 *           len - length of said data
 *           replaceExisting - if not set can create keys with identical names/types
 * RETURN:   success
 * NOTES:    call with mLock held
 */
static bool persistSetPropInList(struct persistProp **headP, struct persistProp **tailP, uint32_t name, uint32_t type, const void *buf, uint32_t len, bool replaceExisting)
{
    struct persistProp *t;
    bool ret = false;


    if (replaceExisting) {
        t = persistPropFind(*headP, name, type);
        if (t) {
            if (t->info.len < len) {
                persistPropDelete(headP, tailP, t);
                t = NULL;
            } else {
                t->info.len = len;
                memcpy(t + 1, buf, len);
                ret = true;
                goto done;
            }
        }
    }

    ret = !!persistPropAdd(headP, tailP, name, type, len, buf);

done:
    return ret;
}

/*
 * FUNCTION: persistSetDeviceName
 * USE:      Set stored device name for our device
 * PARAMS:   buf - the name to save
 *           len - length of said name (at most HCI_DEV_NAME_LEN bytes)
 * RETURN:   success
 * NOTES:    NONE
 */
bool persistSetDeviceName(const void *buf, uint32_t len)
{
    bool ret;

    if (len > HCI_DEV_NAME_LEN) {
        logw("Name to long. Truncating\n");
        len = HCI_DEV_NAME_LEN;
    }

    pthread_mutex_lock(&mLock);
    ret = persistSetPropInList(&mPropsHead, &mPropsTail, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY, buf, len, true);
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistGetDiscoveryLength
 * USE:      Get stored discovery length limit
 * PARAMS:   NONE
 * RETURN:   stored discovery length in seconds
 * NOTES:    NONE
 */
uint8_t persistGetDiscoveryLength(void)
{
    struct persistProp *t;
    uint8_t ret = 60;

    pthread_mutex_lock(&mLock);
    t = persistPropFind(mPropsHead, PERS_NAME_DISC_TIMEOUT, PERS_TYPE_UINT);
    if (!t)
        logw("Discovery length property not found\n");
    else {
        if (t->info.len != sizeof(uint8_t))
            logw("Discovery length property wrong size: %u\n", t->info.len);
        else
            ret = *(uint8_t*)(t + 1);
    }
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistSetDiscoveryLength
 * USE:      Set stored discovery length limit
 * PARAMS:   discLen - the length of discovery
 * RETURN:   success
 * NOTES:    NONE
 */
bool persistSetDiscoveryLength(uint8_t discLen)
{
    struct persistProp *t;
    bool ret = false;

    pthread_mutex_lock(&mLock);
    t = persistPropFind(mPropsHead, PERS_NAME_DISC_TIMEOUT, PERS_TYPE_UINT);
    if (t) {
        if (t->info.len < sizeof(discLen)) {
            persistPropDelete(&mPropsHead, &mPropsTail, t);
            t = NULL;
        } else {
            t->info.len = sizeof(discLen);
            memcpy(t + 1, &discLen, sizeof(discLen));
            ret = true;
            goto done;
        }
    }

    ret = !!persistPropAddInt(&mPropsHead, &mPropsTail,PERS_NAME_DISC_TIMEOUT, false, sizeof(discLen), discLen);

done:
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistEnumKnownDevs
 * USE:      Enumerate all known devices
 * PARAMS:   enumF - callback to call
 *           cbkData - data to pass to said callback
 *           wantedKeyType - if not null, only call callback for devices for which we have a key of this type
 *           wantedNumType - if not null, only call callback for devices for which we have a number of this type
 * RETURN:   true if enumeration finished by itself, false if by request of callback
 * NOTES:    NONE
 */
bool persistEnumKnownDevs(persistKnownDevEnumeratorF enumF, void *cbkData, const uint8_t *wantedKeyType, const uint8_t *wantedNumType)
{
    struct persistProp *t;
    bool ret = true;

    pthread_mutex_lock(&mLock);
    t = persistPropFind(mPropsHead, PERS_NAME_KNOWN_DEVICES, PERS_TYPE_ARRAY);
    if (!t)
        logw("Known device list not found\n");
    else {
        struct persistArrayInRam *arr = (struct persistArrayInRam*)(t + 1);
        struct persistProp *dev;

        /* iterate over all devices in the "known devices" list */
        for (dev = arr->head; dev; dev = dev->next) {
            struct persistProp *devItem, *dAddr = NULL, *dKeys = NULL, *dNums = NULL;
            struct persistArrayInRam *subArr = (struct persistArrayInRam*)(dev + 1);
            uint32_t nameLen = 0, haveKeys = 0, devCls = 0, haveNums = 0;
            const uint8_t *wantedKey = NULL;
            uint64_t *wantedNum = NULL;
            uint64_t lastSeen = 0;
            const void* name;

            if (dev->info.type != PERS_TYPE_ARRAY) {
                logw("Unexpected child type %u in known devices array\n", dev->info.type);
                continue;
            }
            if (dev->info.name != PERS_NAME_DEVICE) {
                logw("Unexpected child name %u in known devices array\n", dev->info.name);
                continue;
            }

            /* iterate over all items for this device and find which we care about */
            for (devItem = subArr->head; devItem; devItem = devItem->next) {
                if (devItem->info.name == PERS_NAME_DEVICE_NAME && devItem->info.type == PERS_TYPE_BYTE_ARRAY && devItem->info.len <= HCI_DEV_NAME_LEN) {
                    name = devItem + 1;
                    nameLen = devItem->info.len;
                } else if (devItem->info.name == PERS_NAME_BT_ADDR && devItem->info.type == PERS_TYPE_BT_ADDR && devItem->info.len == sizeof(struct bt_addr))
                    dAddr = devItem;
                else if (devItem->info.name == PERS_NAME_DEV_CLS && devItem->info.type == PERS_TYPE_UINT && devItem->info.len == sizeof(uint32_t))
                    devCls = *(uint32_t*)(devItem + 1);
                else if (devItem->info.name == PERS_NAME_KEYS && devItem->info.type == PERS_TYPE_ARRAY)
                    dKeys = devItem;
                else if (devItem->info.name == PERS_NAME_NUMBERS && devItem->info.type == PERS_TYPE_ARRAY)
                    dNums = devItem;
                else if (devItem->info.name == PERS_NAME_LAST_SEEN && devItem->info.type == PERS_TYPE_UINT && devItem->info.len == sizeof(uint64_t))
                    lastSeen = *(uint64_t*)(devItem + 1);
            }

            if (!dAddr) {
                logw("Device with no address in seen device list - ignoring\n");
                continue;
            }

            /* iterate over all the keys for this device to collect the proper value for "haveKeys" also find the wanted one if asked */
            if (dKeys) {
                struct persistArrayInRam *keyArr = (struct persistArrayInRam*)(dKeys + 1);
                struct persistKey *keyStruct;
                struct persistProp *keyProp;

                for (keyProp = keyArr->head; keyProp; keyProp = keyProp->next) {
                    if (keyProp->info.name != PERS_NAME_KEY || keyProp->info.type != PERS_TYPE_BYTE_ARRAY || keyProp->info.len != sizeof(struct persistKey)) {
                        logw("Unexpected item in keys array of name %u, type %u, and len %u\n", keyProp->info.name, keyProp->info.type, keyProp->info.len);
                        continue;
                    }
                    keyStruct = (struct persistKey*)(keyProp + 1);
                    haveKeys |= 1UL << keyStruct->keyType;
                    if (wantedKeyType && keyStruct->keyType == *wantedKeyType)
                        wantedKey = keyStruct->key;
                }
            }

            /* iterate over all numbers of this device to collect the proper value for haveNums also find the wanted one if asked */
            if (dNums) {
                struct persistArrayInRam *numsArr = (struct persistArrayInRam*)(dNums + 1);
                struct persistArrayInRam *numArr;
                struct persistProp *numProp;

                for (numProp = numsArr->head; numProp; numProp = numProp->next) {
                    struct persistProp *inner;
                    bool nameSeen = false, valSeen = false;
                    uint8_t lastSeenName = 0;
                    uint64_t *lastSeenVal = NULL;

                    if (numProp->info.name != PERS_NAME_NUM || numProp->info.type != PERS_TYPE_ARRAY) {
                        logw("Unexpected item in numbers array of name %u, type %u\n", numProp->info.name, numProp->info.type);
                        continue;
                    }

                    numArr = (struct persistArrayInRam*)(numProp + 1);
                    for (inner = numArr->head; inner; inner = inner->next) {
                        if (inner->info.name == PERS_NAME_NUM_NAME && inner->info.type == PERS_TYPE_UINT && inner->info.len == sizeof(uint8_t)) {
                            lastSeenName = *(uint8_t*)(inner + 1);
                            nameSeen = true;
                        }
                        if (inner->info.name == PERS_NAME_NUM_VAL && inner->info.type == PERS_TYPE_UINT && inner->info.len == sizeof(uint64_t)) {
                            lastSeenVal = (uint64_t*)(inner + 1);
                            valSeen = true;
                        }
                    }
                    if (nameSeen && valSeen) {
                        haveNums |= 1UL << lastSeenName;
                        if (wantedNumType && lastSeenName == *wantedNumType)
                            wantedNum = lastSeenVal;
                    }
                }
            }

            /* call the callback with all this info */
            if (((!wantedKeyType || wantedKey) || (!wantedNumType || wantedNum))
                && !enumF(cbkData, (const struct bt_addr*)(dAddr + 1), name, nameLen, devCls, haveKeys, wantedKey, haveNums, wantedNum)) {
                ret = false;
                break;
            }
        }
    }
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistFindKnownDev
 * USE:      Find a known device's property in known devices list
 * PARAMS:   addr - the device address we're looking for
 * RETURN:   the device property or NULL if none
 * NOTES:    call wuth mLock held
 */
static struct persistProp* persistFindKnownDev(const struct bt_addr *addr)
{
    struct persistArrayInRam *arr, *itemsArr;
    struct persistProp *t, *dev, *devItem;
    struct bt_addr *devAddr;

    //XXX: TODO: private address resolution


    t = persistPropFind(mPropsHead, PERS_NAME_KNOWN_DEVICES, PERS_TYPE_ARRAY);
    if (!t) {
        logw("Known device list not found\n");
        return NULL;
    }
    arr = (struct persistArrayInRam*)(t + 1);
    for (dev = arr->head; dev; dev = dev->next) {
        if (dev->info.type != PERS_TYPE_ARRAY)
            continue;
        if (dev->info.name != PERS_NAME_DEVICE)
            continue;
        itemsArr = (struct persistArrayInRam*)(dev + 1);
        for (devItem = itemsArr->head; devItem; devItem = devItem->next) {
            if (devItem->info.name != PERS_NAME_BT_ADDR)
                continue;
            if (devItem->info.type != PERS_TYPE_BT_ADDR)
                continue;
            if (devItem->info.len != sizeof(struct bt_addr))
                continue;
            devAddr = (struct bt_addr*)(devItem + 1);
            if (memcmp(devAddr, addr, sizeof(struct bt_addr)))
                continue;
            return dev;
        }
    }
    return NULL;
}

/*
 * FUNCTION: persistGetKnownDev
 * USE:      Find info given a mac address
 * PARAMS:   addr - the address to look up
 *           name - name gets stored here (should fit at least HCI_DEV_NAME_LEN bytes). may be null
 *           nameLen - name's length gets stored here. may be null
 *           devCls - device class gets stored here. may be null
 * RETURN:   true if device was found (not a promise that any data was returned)
 * NOTES:    NONE
 */
bool persistGetKnownDev(const struct bt_addr *addr, void *name, uint32_t *nameLen, uint32_t *devCls)
{
    struct persistArrayInRam *devItems;
    struct persistProp *dev, *t;
    uint32_t len;

    pthread_mutex_lock(&mLock);
    dev = persistFindKnownDev(addr);
    if (dev) {
        devItems = (struct persistArrayInRam*)(dev + 1);

        t = persistPropFind(devItems->head, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY);
        if (t) {
            len = t->info.len;
            if (len > HCI_DEV_NAME_LEN)
                len = HCI_DEV_NAME_LEN;
            if (name)
                memcpy(name, t + 1, len);
            if (nameLen)
                *nameLen = len;
        } else if (nameLen)
            *nameLen = 0;

        if (devCls) {
             t = persistPropFind(devItems->head, PERS_NAME_DEV_CLS, PERS_TYPE_UINT);
             if (t && t->info.len == sizeof(uint32_t))
                 *devCls = *(uint32_t*)(t + 1);
             else
                 *devCls = 0;
        }
    }
    pthread_mutex_unlock(&mLock);
    return !!dev;
}

/*
 * FUNCTION: persistDelKnownDev
 * USE:      Delete device info from known device list
 * PARAMS:   addr - the address
 * RETURN:   NONE
 * NOTES:
 */
void persistDelKnownDev(const struct bt_addr *addr)
{
    struct persistArrayInRam *allDevsArr;
    struct persistProp *dev, *allDevs;

    pthread_mutex_lock(&mLock);
    allDevs = persistPropFind(mPropsHead, PERS_NAME_KNOWN_DEVICES, PERS_TYPE_ARRAY);
    allDevsArr = (struct persistArrayInRam*)(allDevs + 1);
    dev = persistFindKnownDev(addr);
    if (allDevs && dev)
        persistPropDelete(&allDevsArr->head, &allDevsArr->tail, dev);
    pthread_mutex_unlock(&mLock);
    persistStore();
}

/*
 * FUNCTION: persistAddKnownDevInt
 * USE:      Add a new known device or info about current one. "Last seen" timestamp is also updated
 * PARAMS:   addr - the address
 *           name - device name if seen
 *           nameLenP - device name length or NULL if no name known
 *           nameIsFull - set if name is complete. Clear if it is shortened
 *           devCls - device class or NULL if not known
 * RETURN:   the device property or NULL on error
 * NOTES:    call with mLock held
 */
static struct persistProp* persistAddKnownDevInt(const struct bt_addr *addr, const void *name, const uint32_t *nameLenP, bool nameIsFull, const uint32_t *devCls)
{
    struct persistArrayInRam *allDevsArr, *devArr;
    struct persistProp *dev = NULL, *allDevs, *t;
    bool haveName = !!nameLenP;
    uint32_t nameLen;
    uint64_t time;

    if (haveName)
        nameLen = *nameLenP;

    /* shorten name if it has a NULL */
    if (haveName) {
        uint32_t bytesLeft = nameLen;
        const uint8_t *buf = (const uint8_t*)name;

        while(bytesLeft && *buf++)
            bytesLeft--;

        nameLen -= bytesLeft;
    }

    time = persistGetRealtime();

    allDevs = persistPropFind(mPropsHead, PERS_NAME_KNOWN_DEVICES, PERS_TYPE_ARRAY);
    if (!allDevs)
        goto out;

    allDevsArr = (struct persistArrayInRam*)(allDevs + 1);

    // Garbage collection of old devices.
    struct persistProp *devIter = allDevsArr->head;
    while (devIter) {
        struct persistArrayInRam *devItems = (struct persistArrayInRam*)(devIter + 1);
        struct persistProp *keys = persistPropFind(devItems->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
        struct persistProp *nums = persistPropFind(devItems->head, PERS_NAME_NUMBERS, PERS_TYPE_ARRAY);
        struct persistProp *lastSeenProp = persistPropFind(devItems->head, PERS_NAME_LAST_SEEN, PERS_TYPE_UINT);
        uint64_t lastSeen = lastSeenProp ? *(uint64_t*)(lastSeenProp + 1) : 0;

        /* The device list is sorted by last seen, so we can conclude that we don't
         * need to continue iterating once we find the first recent device. */
        if (time - lastSeen < MAX_DEVICE_AGE)
            break;

        bool paired = false;
        if (keys) {
            struct persistArrayInRam* keyArr = (struct persistArrayInRam*)(keys + 1);
            struct persistArrayInRam* numArr = (struct persistArrayInRam*)(nums + 1);

            paired = keyArr && numArr &&
                     ((persistFindKey(keyArr->head, KEY_TYPE_LTK) &&
                     persistFindNum(numArr->head, PERSIST_NUM_TYPE_SM_EDIV) &&
                     persistFindNum(numArr->head, PERSIST_NUM_TYPE_SM_RANDOM)) ||
                     (persistFindKey(keyArr->head, KEY_TYPE_MY_LTK) &&
                     persistFindNum(numArr->head, PERSIST_NUM_TYPE_SM_MY_RANDOM)));
        }

        if (paired) {
            devIter = devIter->next;
            continue;
        }

        /* devIter is old and unpaired, remove it from the list. */
        if (devIter->prev)
            devIter->prev->next = devIter->next;
        else
            allDevsArr->head = devIter->next;

        if (devIter->next)
            devIter->next->prev = devIter->prev;
        else
            allDevsArr->tail = devIter->prev;

        struct persistProp *temp = devIter;
        devIter = devIter->next;
        persistPropFree(temp);
    }

    dev = persistFindKnownDev(addr);
    if (!dev)
        dev = persistPropAddEmptyArray(&allDevsArr->head, &allDevsArr->tail, PERS_NAME_DEVICE);
    if (!dev)
        goto out;

    /* Move the discovered device to the tail to keep the list sorted by "last seen".
     * This helps in optimizing garbage collection of old devices. */
    if (dev->next) {
        dev->next->prev = dev->prev;
        if (dev->prev)
            dev->prev->next = dev->next;
        else
            allDevsArr->head = dev->next;

        dev->next = NULL;
        allDevsArr->tail->next = dev;
        dev->prev = allDevsArr->tail;
        allDevsArr->tail = dev;
    }

    devArr = (struct persistArrayInRam*)(dev + 1);
    if (!persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_BT_ADDR, PERS_TYPE_BT_ADDR, addr, sizeof(struct bt_addr), true))
        logw("Failed to save device name\n");

    if (haveName) {
        /* never replace an existing name with a shortened one. Always replace with a full one */
        struct persistArrayInRam *devItems;

        devItems = (struct persistArrayInRam*)(dev + 1);
        t = persistPropFind(devItems->head, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY);
        if ((!t || nameIsFull) && !persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY, name, nameLen, true))
            logw("Failed to save device name\n");
    }

    if (devCls && !persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_DEV_CLS, PERS_TYPE_UINT, devCls, sizeof(*devCls), true))
        logw("Failed to save device class\n");

    if (!persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_LAST_SEEN, PERS_TYPE_UINT, &time, sizeof(time), true))
        logw("Failed to save device last seen\n");

    if (!persistPropFind(devArr->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY))
        if (!persistPropAddEmptyArray(&devArr->head, &devArr->tail, PERS_NAME_KEYS))
            logw("Failed to add the missing keys container to device\n");

    if (!persistPropFind(devArr->head, PERS_NAME_NUMBERS, PERS_TYPE_ARRAY))
        if (!persistPropAddEmptyArray(&devArr->head, &devArr->tail, PERS_NAME_NUMBERS))
            logw("Failed to add the missing numbers container to device\n");

out:
    return dev;
}

/*
 * FUNCTION: persistAddKnownDev
 * USE:      Add a new known device or info about current one. "Last seen" timestamp is also updated
 * PARAMS:   addr - the address
 *           name - device name if seen
 *           nameLen - device name length or NULL if no name known
 *           nameIsFull - set if name is complete. Clear if it is shortened
 *           devCls - device class or NULL if not known
 * RETURN:   NONE
 * NOTES:
 */
void persistAddKnownDev(const struct bt_addr *addr, const void *name, const uint32_t *nameLen, bool nameIsFull, const uint32_t *devCls)
{
    pthread_mutex_lock(&mLock);
    persistAddKnownDevInt(addr, name, nameLen, nameIsFull, devCls);
    pthread_mutex_unlock(&mLock);
}

/*
 * FUNCTION: persistFindNamedContainerInDevNodeForAddrOrSelf
 * USE:      Find a named array container in device node for a given addr or self (if addr is null)
 * PARAMS:   addr - the address or NULL for ourselves
 *           containerName - container name to find
 * RETURN:   the node or NULL
 * NOTES:    call with mLock held
 */
static struct persistProp* persistFindNamedContainerInDevNodeForAddrOrSelf(const struct bt_addr *addr, uint32_t containerName)
{
    struct persistProp *parent;

    //self?
    if (!addr)
        return persistPropFind(mPropsHead, containerName, PERS_TYPE_ARRAY);

    //someone else?
    parent = persistAddKnownDevInt(addr, NULL, NULL, false, NULL); /* if we're adding a key for it means we saw it and/or are talking to it */
    if (parent)
        return persistPropFind(((struct persistArrayInRam*)(parent + 1))->head, containerName, PERS_TYPE_ARRAY);

    return NULL;
}

/*
 * FUNCTION: persistAddDevKey
 * USE:      Add a new key to a given device (or ourselves)
 * PARAMS:   addr - the address or NULL for ourselves
 *           keyType - type of key being added
 *           key - the key to add (HCI_LINK_KEY_LEN bytes)
 * RETURN:   success
 * NOTES:    any existing keys of same type are overwritten
 */
bool persistAddDevKey(const struct bt_addr *addr, uint8_t keyType, const uint8_t *key) //addr = NULL for our own keys, all keys are HCI_LINK_KEY_LEN bytes long
{
    struct persistArrayInRam *parArr;
    struct persistProp *parent;
    bool ret = false;


    pthread_mutex_lock(&mLock);
    parent = persistFindNamedContainerInDevNodeForAddrOrSelf(addr, PERS_NAME_KEYS);

    if (!parent) {
        logw("Failed to find key list parent for given addess -> abandonning key addition\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);

    ret = persistAddKey(&parArr->head, &parArr->tail, keyType, key);

out:
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistDelDevPropsByName
 * USE:      Delete all properties of a given name for a given device
 * PARAMS:   addr - the address
 *           name - properties name
 * RETURN:   success
 * NOTES:    recursive
 */
static bool persistDelDevPropsByName(const struct bt_addr *addr, uint32_t name)
{
    struct persistArrayInRam *propsArr;
    struct persistProp *props, *k;
    bool ret = true;

    pthread_mutex_lock(&mLock);
    props = persistFindNamedContainerInDevNodeForAddrOrSelf(addr, name);
    if (!props) {
        logd("Keys/Numbers property not found probably means we have no keys/numbers stored for the device\n");
        goto out;
    }

    propsArr = (struct persistArrayInRam*)(props + 1);
    propsArr->tail = NULL;
    while (propsArr->head) {
        k = propsArr->head;
        propsArr->head = propsArr->head->next;
        persistPropFree(k);
    }

out:
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistDelDevKeys
 * USE:      Delete all keys for a given device
 * PARAMS:   addr - the address
 * RETURN:   success
 * NOTES:
 */
bool persistDelDevKeys(const struct bt_addr *addr)
{
    return persistDelDevPropsByName(addr, PERS_NAME_KEYS);
}

/*
 * FUNCTION: persistGetDevKey
 * USE:      Get a key (ours or another device's)
 * PARAMS:   addr - the address for which to retreive the key (NULL for our own)
 *           keyType - key type to find
 *           key - key is returned here (HCI_LINK_KEY_LEN bytes)
 * RETURN:   true if key was found and returned
 * NOTES:
 */
bool persistGetDevKey(const struct bt_addr *addr, uint8_t keyType, uint8_t *key)
{
    struct persistArrayInRam *parArr;
    struct persistProp *parent, *keyProp;
    struct persistKey *keyStruct;
    bool ret = false;


    pthread_mutex_lock(&mLock);
    parent = persistFindNamedContainerInDevNodeForAddrOrSelf(addr, PERS_NAME_KEYS);
    if (!parent) {
        logw("Failed to find key list parent for given addess -> abandonning key search\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);

    keyProp = persistFindKey(parArr->head, keyType);
    if (!keyProp)
        goto out;

    keyStruct = (struct persistKey*)(keyProp + 1);
    memcpy(key, keyStruct->key, HCI_LINK_KEY_LEN);
    ret = true;

out:
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistFindNum
 * USE:      find a named number in the parent "all numbers" array
 * PARAMS:   head - the hgead of the parent array
 *           numType - type of number being looked for
 * RETURN:   the prop containing key VALUE or NULL
 * NOTES:    call with mLock held
 */
static struct persistProp* persistFindNum(struct persistProp *head, uint8_t numType)
{
    for (;head;head = head->next) {
        struct persistArrayInRam *arr = (struct persistArrayInRam*)(head + 1);
        struct persistProp *inner, *seenNameValProp = NULL;
        bool nameSeen = false, valSeen = false;
        uint8_t lastSeenName = 0;

        if (head->info.name != PERS_NAME_NUM || head->info.type != PERS_TYPE_ARRAY)
            continue;

        for (inner = arr->head; inner; inner = inner->next) {
            if (inner->info.name == PERS_NAME_NUM_NAME && inner->info.type == PERS_TYPE_UINT && inner->info.len == sizeof(uint8_t)) {
                if (nameSeen) //only one name expected - this is weird - bail
                    return NULL;
                lastSeenName = *(uint8_t*)(inner + 1);
                nameSeen = true;
            }
            if (inner->info.name == PERS_NAME_NUM_VAL && inner->info.type == PERS_TYPE_UINT && inner->info.len == sizeof(uint64_t)) {
                if (valSeen) //only one val expected - this is weird - bail
                    return NULL;
                seenNameValProp = inner;
                valSeen = true;
            }
        }
        if (nameSeen && valSeen && lastSeenName == numType)
            return seenNameValProp;
    }
    return NULL;
}

/*
 * FUNCTION: persistAddNumber
 * USE:      Add a number of the given type and add it to the given list
 * PARAMS:   headP - list head stored here
 *           tailP - lsit tail stored here
 *           numType - type of number being added
 *           num - the number
 * RETURN:   success
 * NOTES:    call with mLock held
 */
static bool persistAddNumber(struct persistProp **headP, struct persistProp **tailP, uint8_t numType, uint64_t num)
{
    struct persistProp *valProp, *nameProp, *parentProp;
    struct persistArrayInRam *parentPropArr;

    //if it already exists, replace it
    valProp = persistFindNum(*headP, numType);
    if (valProp) {
        *(uint64_t*)(valProp + 1) = num;
        return true;
    }

    //1. construct a new value-holding array
    parentProp = persistPropAddEmptyArray(headP, tailP, PERS_NAME_NUM);
    if (!parentProp)
        return NULL;

    parentPropArr = (struct persistArrayInRam*)(parentProp + 1);

    //2. add name & val
    nameProp = persistPropAddInt(&parentPropArr->head, &parentPropArr->tail, PERS_NAME_NUM_NAME, false, sizeof(numType), numType);
    valProp = persistPropAddInt(&parentPropArr->head, &parentPropArr->tail, PERS_NAME_NUM_VAL, false, sizeof(num), num);

    //if all is ok - return
    if (nameProp && valProp)
        return true;

    //else cleanup and fail
    persistPropDelete(headP, tailP, parentProp);
    return false;
}

/*
 * FUNCTION: persistAddDevNumber
 * USE:      Add a new "number" to a given device (or ourselves)
 * PARAMS:   addr - the address or NULL for ourselves
 *           numType - type of number being added
 *           num - the number (64 bits)
 * RETURN:   success
 * NOTES:    any existing numbers of same type are overwritten
 */
bool persistAddDevNumber(const struct bt_addr *addr, uint8_t numType, uint64_t num)
{
    struct persistArrayInRam *parArr;
    struct persistProp *parent;
    bool ret = false;

    pthread_mutex_lock(&mLock);
    parent = persistFindNamedContainerInDevNodeForAddrOrSelf(addr, PERS_NAME_NUMBERS);
    if (!parent) {
        logw("Failed to find key list parent for given addess -> abandonning number addition\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);

    ret = persistAddNumber(&parArr->head, &parArr->tail, numType, num);

out:
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistGetDevNumber
 * USE:      Get a number (ours or another device's)
 * PARAMS:   addr - the address for which to retreive the number (NULL for our own)
 *           numType - type of number being added
 *           num - the number is returned here (always 64 bits)
 * RETURN:   true if key was found and returned
 * NOTES:
 */
bool persistGetDevNumber(const struct bt_addr *addr, uint8_t numType, uint64_t *num)
{
    struct persistArrayInRam *parArr;
    struct persistProp *parent, *numProp;
    bool ret = false;

    pthread_mutex_lock(&mLock);
    parent = persistFindNamedContainerInDevNodeForAddrOrSelf(addr, PERS_NAME_NUMBERS);
    if (!parent) {
        logw("Failed to find key list parent for given addess - >abandonning key search\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);
    numProp = persistFindNum(parArr->head, numType);
    if (!numProp)
        goto out;

    *num = *(uint64_t*)(numProp + 1);
    ret = true;

out:
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistDelDevNumbers
 * USE:      Delete all numbers for a given device
 * PARAMS:   addr - the address
 * RETURN:   success
 * NOTES:
 */
bool persistDelDevNumbers(const struct bt_addr *addr)
{
    return persistDelDevPropsByName(addr, PERS_NAME_NUMBERS);
}

/*
 * FUNCTION: getAddrPersistenceNameFromType
 * USE:      get the persistence name for an address from it's type
 * PARAMS:   addrType - the address's type
 *           persistName - the persistence name returned
 * RETURN:   success
 * NOTES:
 */
static bool getAddrPersistenceNameFromType(uint8_t addrType, uint8_t *persistName) {
    switch(addrType) {
        case PERSIST_ADDRESS_TYPE_IDENTITY:
            *persistName = PERS_NAME_IDENTITY_ADDR;
            break;
        case PERSIST_ADDRESS_TYPE_LATEST:
            *persistName = PERS_NAME_LATEST_ADDR;
            break;
        default:
            return false;
    }

    return true;
}

/*
 * FUNCTION: isAddressPersistenceValid
 * USE:      Check whether the persistence of the address type is valid
 * PARAMS:   addr - the address of the device or NULL for ourselves
 *           addrType - address type
 *           persistName - the address persistence name returned if valid
 * RETURN:   success
 * NOTES:
 */
static bool isAddressPersistenceValid(const struct bt_addr *addr, uint8_t addrType, uint8_t *persistName) {
    if (!addr && addrType != PERSIST_ADDRESS_TYPE_IDENTITY) {
        logw("Only identity address is supported for local device\n");
        return false;
    }
    if (!getAddrPersistenceNameFromType(addrType, persistName)) {
        logw("Failed to find address persistence name for the given device -> abandonning address operation\n");
        return false;
    }

    return true;
}

/*
 * FUNCTION: persistAddDevOtherAddr
 * USE:      Add the address of the given type
 * PARAMS:   addr - the address of the device or NULL for ourselves
 *           addrType - address type
 *           otherAddr - the address to add
 * RETURN:   success
 * NOTES:    the existing address is overwritten
 */
bool persistAddDevOtherAddr(const struct bt_addr *addr, uint8_t addrType, const struct bt_addr *otherAddr)
{
    struct persistProp **head;
    struct persistProp **tail;
    bool ret = false;
    uint8_t persistName;

    if (!isAddressPersistenceValid(addr, addrType, &persistName)) {
        return false;
    }
    pthread_mutex_lock(&mLock);

    if (!addr) { //self?
        head = &mPropsHead;
        tail = &mPropsTail;
    } else { //someone else?
        struct persistProp *parent = persistAddKnownDevInt(addr, NULL, NULL, false, NULL);
        if (!parent) {
            logw("Failed to find address parent for the given device -> abandonning address addition\n");
            goto out;
        }
        head = &((struct persistArrayInRam*)(parent + 1))->head;
        tail = &((struct persistArrayInRam*)(parent + 1))->tail;
    }

    ret = persistSetPropInList(head, tail, persistName, PERS_TYPE_BT_ADDR, otherAddr, sizeof(struct bt_addr), true);

out:
    pthread_mutex_unlock(&mLock);
    persistStore();
    return ret;
}

/*
 * FUNCTION: persistGetDevOtherAddr
 * USE:      Get the address of the given type
 * PARAMS:   addr - the address of the device or NULL for ourselves
 *           addrType - address type
 *           otherAddr - the address to return
 * RETURN:   success
 * NOTES:
 */
bool persistGetDevOtherAddr(const struct bt_addr *addr, uint8_t addrType, struct bt_addr *otherAddr)
{
    struct persistProp *head;
    bool ret = false;
    uint8_t persistName;

    if (!isAddressPersistenceValid(addr, addrType, &persistName)) {
        return false;
    }

    pthread_mutex_lock(&mLock);

    if (!addr) { //self?
        head = mPropsHead;
    } else { //someone else?
        struct persistProp *parent = persistAddKnownDevInt(addr, NULL, NULL, false, NULL);
        if (!parent) {
            logw("Failed to find address parent for the given device -> abandonning address search\n");
            goto out;
        }
        head = ((struct persistArrayInRam*)(parent + 1))->head;
    }

    struct persistProp *addrProp = persistPropFind(head, persistName, PERS_TYPE_BT_ADDR);
    if (!addrProp)
        goto out;

    memcpy(otherAddr, (struct bt_addr*)(addrProp + 1), sizeof(struct bt_addr));
    ret = true;
out:
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistDelDevOtherAddr
 * USE:      Delete the address of the given type
 * PARAMS:   addr - the address of the device
 *           addrType - address type
 * RETURN:   success
 * NOTES:
 */
bool persistDelDevOtherAddr(const struct bt_addr *addr, uint8_t addrType)
{
    struct persistProp **head;
    struct persistProp **tail;
    uint8_t persistName;
    bool ret = false;

    if (!isAddressPersistenceValid(addr, addrType, &persistName)) {
        return false;
    }

    pthread_mutex_lock(&mLock);

    if (!addr) { //self?
        head = &mPropsHead;
        tail = &mPropsTail;
    } else { //someone else?
        struct persistProp *parent = persistAddKnownDevInt(addr, NULL, NULL, false, NULL);
        if (!parent) {
            logw("Failed to find address parent for the given device -> abandonning address deletion\n");
            goto out;
        }
        head = &((struct persistArrayInRam*)(parent + 1))->head;
        tail = &((struct persistArrayInRam*)(parent + 1))->tail;
    }
    struct persistProp *addrProp = persistPropFind(*head, persistName, PERS_TYPE_BT_ADDR);
    if (!addrProp)
        goto out;
    persistPropDelete(head, tail, addrProp);
    ret = true;

out:
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistPropNameToString
 * USE:      Return the string representation of a property name
 * PARAMS:   name - the property name enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistPropNameToString(uint32_t name)
{
    switch (name) {
        case PERS_NAME_DEVICE_NAME:
            return "Device Name";
        case PERS_NAME_DISC_TIMEOUT:
            return "Discovery Timeout";
        case PERS_NAME_DEV_CLS:
            return "Device Class";
        case PERS_NAME_KEYS:
            return "Keys";
        case PERS_NAME_KEY:
            return "Key";
        case PERS_NAME_DEVICE:
            return "Device";
        case PERS_NAME_KNOWN_DEVICES:
            return "Known Devices";
        case PERS_NAME_BT_ADDR:
            return "BT Addr";
        case PERS_NAME_LAST_SEEN:
            return "Last Seen";
        case PERS_NAME_NUMBERS:
            return "Numbers";
        case PERS_NAME_NUM:
            return "Number";
        case PERS_NAME_NUM_NAME:
            return "Number Name";
        case PERS_NAME_NUM_VAL:
            return "Number Value";
        case PERS_NAME_IDENTITY_ADDR:
            return "Identity Addr";
    }
    return "Unknown Property";
}

/*
 * FUNCTION: persistTypeToString
 * USE:      Return the string representation of a property type
 * PARAMS:   type - the property type enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistTypeToString(uint32_t type)
{
    switch (type) {
        case PERS_TYPE_UINT:
            return "unsigned int";
        case PERS_TYPE_SINT:
            return "signed int";
        case PERS_TYPE_BYTE_ARRAY:
            return "byte array";
        case PERS_TYPE_BT_ADDR:
            return "address";
        case PERS_TYPE_ARRAY:
            return "array";
    }
    return "unknown type";
}

/*
 * FUNCTION: persistKeyTypeToString
 * USE:      Return the string representation of a key type
 * PARAMS:   type - the key type enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistKeyTypeToString(uint8_t type)
{
    switch (type) {
        case KEY_TYPE_MITM_PROTECTED:
            return "MITM protected";
        case KEY_TYPE_MITM_UNPROTECTED:
            return "MITM unprotected";
        case KEY_TYPE_CSRK:
            return "CSRK";
        case KEY_TYPE_IRK:
            return "IRK";
        case KEY_TYPE_DHK:
            return "DHK";
        case KEY_TYPE_LTK:
            return "LTK";
        case KEY_TYPE_MY_LTK:
            return "my LTK";
    }
    return "unknown key type";
}

/*
 * FUNCTION: persistBtAddrTypeToString
 * USE:      Return the string representation of a bt_addr type
 * PARAMS:   type - the bt_addr type enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistBtAddrTypeToString(uint8_t type)
{
    switch (type) {
        case BT_ADDR_TYPE_EDR:
            return "EDR";
        case BT_ADDR_TYPE_LE_PUBLIC:
            return "LE public";
        case BT_ADDR_TYPE_LE_RANDOM:
            return "LE random";
    }
    return "unknown address type";
}

/*
 * FUNCTION: persistIdentityAddrTypeToString
 * USE:      Return the string representation of a identity address type
 * PARAMS:   type - the identity address type enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistIdentityAddrTypeToString(uint8_t type)
{
    switch (type) {
        case IDENTITY_ADDR_TYPE_PUBLIC:
            return "public";
        case IDENTITY_ADDR_TYPE_STATIC_RANDOM:
            return "static random";
    }
    return "unknown identity address type";
}

/*
 * FUNCTION: persistNumberTypeToString
 * USE:      Return the string representation of a number type
 * PARAMS:   type - the number type enum
 * RETURN:   the string representation
 * NOTES:
 */
char* persistNumberTypeToString(uint8_t type)
{
    switch (type) {
        case PERSIST_NUM_TYPE_SM_RANDOM:
            return "random";
        case PERSIST_NUM_TYPE_SM_MY_RANDOM:
            return "my random";
        case PERSIST_NUM_TYPE_SM_EDIV:
            return "EDIV";
        case PERSIST_NUM_TYPE_SM_MY_DIV:
            return "my DIV";
        case PERSIST_NUM_TYPE_SM_MITM_PROTECT:
            return "MITM protect";
        case PERSIST_NUM_TYPE_SM_BOND:
            return "bond";
    }
    return "unknown number type";
}

/*
 * FUNCTION: persistDumpPrintWithIndent
 * USE:      Print an info log with leading spaces for indent
 * PARAMS:   indent - the number of leading spaces
 *           format - a printf format
 * RETURN:   NONE
 * NOTES:
 */
__attribute__((format(printf, 2, 3)))
void persistDumpPrintWithIndent(int indent, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char s[1000];
    for (int i = 0; i < indent; i++) {
        s[i] = ' ';
    }
    vsnprintf(s + indent, sizeof(s) - indent, format, args);
    logi("%s\n", s);
    va_end(args);
}

/*
 * FUNCTION: persistDumpPrintPropValueNumberName
 * USE:      Print the value of a property Number Name
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueNumberName(int indent, struct persistProp *p)
{
    uint8_t type = *(uint8_t*)(p + 1);
    persistDumpPrintWithIndent(
            indent, "0x%02x (%s)", type, persistNumberTypeToString(type));
}

/*
 * FUNCTION: persistDumpPrintPropValueUint32
 * USE:      Print the value of a property of type uint32
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueUint32(int indent, struct persistProp *p)
{
    uint32_t val = *(uint32_t*)(p + 1);
    persistDumpPrintWithIndent(indent, "0x%08x (%d)", val, val);
}

/*
 * FUNCTION: persistDumpPrintPropValueUint64
 * USE:      Print the value of a property of type uint64
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueUint64(int indent, struct persistProp *p)
{
    uint64_t val = *(uint64_t*)(p + 1);
    persistDumpPrintWithIndent(indent, "0x%016" PRIx64 " (%" PRIu64 ")", val, val);
}

/*
 * FUNCTION: persistDumpPrintPropValueBtAddr
 * USE:      Print the value of a property of type bt_addr
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 *           isIdentityAddress - if this is an identity address
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueBtAddr(int indent, struct persistProp *p, bool isIdentityAddress)
{
    struct bt_addr *addr = (struct bt_addr*)(p + 1);
    char s[18] = "";
    for (int i = 0; i < 6; i++) {
        sprintf(s + strlen(s), i ? ":%02X" : "%02X", addr->addr[6 - i - 1]);
    }
    persistDumpPrintWithIndent(
            indent, "%s (%s)", s, isIdentityAddress ? persistIdentityAddrTypeToString(addr->type) :
            persistBtAddrTypeToString(addr->type));
}

/*
 * FUNCTION: persistDumpPrintPropValueDeviceName
 * USE:      Print the value of a property Device Name
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueDeviceName(int indent, struct persistProp *p)
{
    char name[p->info.len + 1];
    memcpy(name, p + 1, p->info.len);
    name[p->info.len] = 0;

    persistDumpPrintWithIndent(indent, "%s", name);
}

/*
 * FUNCTION: persistDumpPrintPropValueKey
 * USE:      Print the value of a property Key
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValueKey(int indent, struct persistProp *p)
{
    char s[HCI_LINK_KEY_LEN * 2 + 1];
    struct persistKey *key = (struct persistKey*)(p + 1);

    for (int i = 0; i < HCI_LINK_KEY_LEN; i++) {
        sprintf(s + i * 2, "%02x", key->key[i]);
    }

    persistDumpPrintWithIndent(
            indent, "%s (%s)", s, persistKeyTypeToString(key->keyType));
}

/* forward declaration */
void persistDumpPropList(int indent, struct persistProp *p);

/*
 * FUNCTION: persistDumpPrintPropValue
 * USE:      Print the value of a property
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPrintPropValue(int indent, struct persistProp *p)
{
    /* Recurse if the value contains more properties. */
    if (p->info.type == PERS_TYPE_ARRAY) {
        struct persistArrayInRam *arr = (struct persistArrayInRam*)(p + 1);
        persistDumpPropList(indent, arr->head);
        return;
    }

    /* If the value is a leaf in the tree, print it based on property name. */
    switch (p->info.name) {
        case PERS_NAME_DEVICE_NAME:
            persistDumpPrintPropValueDeviceName(indent, p);
            break;
        case PERS_NAME_DISC_TIMEOUT:
            persistDumpPrintPropValueUint32(indent, p);
            break;
        case PERS_NAME_DEV_CLS:
            persistDumpPrintPropValueUint32(indent, p);
            break;
        case PERS_NAME_KEY:
            persistDumpPrintPropValueKey(indent, p);
            break;
        case PERS_NAME_BT_ADDR:
            persistDumpPrintPropValueBtAddr(indent, p, false);
            break;
        case PERS_NAME_LAST_SEEN:
            persistDumpPrintPropValueUint64(indent, p);
            break;
        case PERS_NAME_NUM_NAME:
            persistDumpPrintPropValueNumberName(indent, p);
            break;
        case PERS_NAME_NUM_VAL:
            persistDumpPrintPropValueUint64(indent, p);
            break;
        case PERS_NAME_IDENTITY_ADDR:
            persistDumpPrintPropValueBtAddr(indent, p, true);
            break;
    }
}

/*
 * FUNCTION: persistDumpPrintPropList
 * USE:      Print a property list
 * PARAMS:   indent - the number of leading spaces
 *           p - the property
 * RETURN:   NONE
 * NOTES:
 */
void persistDumpPropList(int indent, struct persistProp *p)
{
    while (p) {
        persistDumpPrintWithIndent(
                indent,
                "%s (%s): ",
                persistPropNameToString(p->info.name),
                persistTypeToString(p->info.type));
        persistDumpPrintPropValue(indent + 2, p);
        p = p->next;
    }
}

/*
 * FUNCTION: persistDump
 * USE:      Dump persistent preferences file content in human-readable format
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
void persistDump()
{
    persistLoad();
    persistDumpPropList(0, mPropsHead);
}
