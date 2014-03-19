#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
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
#define PERS_NAME_KEYS                   0x00000004 /* type: PERS_TYPE_ARRAY of {PERS_NAME_KEY_TYPE, PERS_NAME_KEY_VAL} */
#define PERS_NAME_KEY                    0x00000005 /* type: PERS_TYPE_BYTE_ARRAY, size: sizeof(struct persistKey) */
#define PERS_NAME_DEVICE                 0x00000006 /* type: PERS_TYPE_ARRAY of {PERS_NAME_DEVICE_NAME, PERS_NAME_BT_ADDR, PERS_NAME_DEV_CLS, PERS_NAME_KEYS, PERS_NAME_LAST_SEEN} */
#define PERS_NAME_KNOWN_DEVICES          0x00000007 /* type: PERS_TYPE_ARRAY of PERS_NAME_KNOWN_DEVICES */
#define PERS_NAME_BT_ADDR                0x00000008 /* type: PERS_TYPE_BT_ADDR */
#define PERS_NAME_LAST_SEEN              0x00000009 /* type: PERS_TYPE_UINT, size: 8, milliseconds since epoch */

/* types */
#define PERS_TYPE_UINT                   0x00000001
#define PERS_TYPE_SINT                   0x00000002
#define PERS_TYPE_BYTE_ARRAY             0x00000003
#define PERS_TYPE_BT_ADDR                0x00000004
#define PERS_TYPE_ARRAY                  0x00000005




static pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
static struct persistProp *mPropsHead = NULL;
static struct persistProp *mPropsTail = NULL;



/* fwd decls */
static bool persistPropListLoad(struct persistProp **headP, struct persistProp **tailP, int fd, uint64_t len);
static bool persistSetPropInList(struct persistProp **headP, struct persistProp **tailP, uint32_t name, uint32_t type, const void *buf, uint32_t len, bool replaceExisting);

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
    bool ret;
    int fd;

    pkey.keyType = keyType;

    if (key)
        memcpy(pkey.key, key, sizeof(pkey.key));
    else {
        fd = open("/dev/urandom", O_RDONLY);
        if (fd == -1) {
            logw("Failed to open urandom\n");
            return false;
        }

        ret = r_read(fd, pkey.key, sizeof(pkey.key));
        close(fd);

        if (!ret) {
            logw("Failed to read urandom\n");
            return false;
        }
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


    pthread_mutex_lock(&mLock);
    utilSetBE32(&hdr.magic, PERSIST_MAGIC);
    utilSetBE32(&hdr.version, PERSIST_VERSION_CUR);

    fd = open(PREF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (fd == -1) {
        loge("Preferences not opened\n");
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
    pthread_mutex_unlock(&mLock);
    return;

fail:
    close(fd);
    pthread_mutex_unlock(&mLock);
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
 * NOTES:
 */
void persistLoad(void)
{
    const struct persistProp *t;
    struct persistFileHdr hdr;
    uint64_t fileLen;
    int fd;


    pthread_mutex_lock(&mLock);

    mPropsHead = NULL;
    mPropsTail = NULL;

    fd = open(PREF_PATH, O_RDONLY);
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
 * PARAMS:   headP - head of lsit stored here
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
    return ret;
}

/*
 * FUNCTION: persistEnumKnownDevs
 * USE:      Enumerate all known devices
 * PARAMS:   enumF - callback to call
 *           cbkData - data to pass to said callback
 *           wantedKeyType - if not null, only call bacllback for devices for which we have a key of this type
 * RETURN:   true if enumeration finished by itself, false if by request of callback
 * NOTES:    NONE
 */
bool persistEnumKnownDevs(persistKnownDevEnumeratorF enumF, void *cbkData, const uint8_t *wantedKeyType)
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
            struct persistProp *devItem, *dAddr = NULL, *dKeys = NULL;
            struct persistArrayInRam *subArr = (struct persistArrayInRam*)(dev + 1);
            uint32_t nameLen = 0, haveKeys = 0, devCls = 0;
            const uint8_t *wantedKey = NULL;
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

            /* call the callback with all this info */
            if ((!wantedKeyType || wantedKey) && !enumF(cbkData, (const struct bt_addr*)(dAddr + 1), name, nameLen, devCls, haveKeys, wantedKey)) {
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
 *           name - name gets stored here (shoudl fit at least HCI_DEV_NAME_LEN bytes). may be null
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
}

/*
 * FUNCTION: persistAddKnownDevInt
 * USE:      Add a new known device or info about current one. "Last seen" timestamp is also updated
 * PARAMS:   addr - the address
 *           name - device name if seen
 *           nameLenP - device name length or NULL if no name known
 *           devCls - device class or NULL if not known
 * RETURN:   the device property or NULL on error
 * NOTES:    call with mLock held
 */
static struct persistProp* persistAddKnownDevInt(const struct bt_addr *addr, const void *name, const uint32_t *nameLenP, const uint32_t *devCls)
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
    dev = persistFindKnownDev(addr);

    if (!dev)
        dev = persistPropAddEmptyArray(&allDevsArr->head, &allDevsArr->tail, PERS_NAME_DEVICE);
    if (!dev)
        goto out;

    devArr = (struct persistArrayInRam*)(dev + 1);
    if (haveName && !persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_DEVICE_NAME, PERS_TYPE_BYTE_ARRAY, name, nameLen, true))
        logw("Failed to save device name\n");

    if (devCls && !persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_DEV_CLS, PERS_TYPE_UINT, devCls, sizeof(*devCls), true))
        logw("Failed to save device class\n");

    if (devCls && !persistSetPropInList(&devArr->head, &devArr->tail, PERS_NAME_LAST_SEEN, PERS_TYPE_UINT, &time, sizeof(time), true))
        logw("Failed to save device last seen\n");

    if (!persistPropFind(devArr->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY))
        if (!persistPropAddEmptyArray(&devArr->head, &devArr->tail, PERS_NAME_KEYS))
            logw("Failed to add the missing keys container to device\n");

out:
    return dev;
}

/*
 * FUNCTION: persistAddKnownDev
 * USE:      Add a new known device or info about current one. "Last seen" timestamp is also updated
 * PARAMS:   addr - the address
 *           name - device name if seen
 *           nameLen - device name length or NULL if no name known
 *           devCls - device class or NULL if not known
 * RETURN:   NONE
 * NOTES:
 */
void persistAddKnownDev(const struct bt_addr *addr, const void *name, const uint32_t *nameLen, const uint32_t *devCls)
{
    pthread_mutex_lock(&mLock);
    persistAddKnownDevInt(addr, name, nameLen, devCls);
    pthread_mutex_unlock(&mLock);
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
    if (addr) {
        parent = persistAddKnownDevInt(addr, NULL, NULL, NULL); /* if we're adding a key for it means we saw it and/or are talking to it */
        if (parent)
            parent = persistPropFind(((struct persistArrayInRam*)(parent + 1))->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
    } else
        parent = persistPropFind(mPropsHead, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
 
    if (!parent) {
        logw("Failed to find key list parent for given addess - >abandonning key addition\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);

    ret = persistAddKey(&parArr->head, &parArr->tail, keyType, key);

out:
    pthread_mutex_unlock(&mLock);
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
    struct persistArrayInRam *keyArr;
    struct persistProp *dev, *keys, *k;
    bool ret = true;

    pthread_mutex_lock(&mLock);
    dev = persistFindKnownDev(addr);
    if (!dev) {
        logd("Device not found probably means we have no keys stored for it either\n");
        goto out;
    }

    keys = persistPropFind(((struct persistArrayInRam*)(dev + 1))->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
    if (!keys) {
        logd("Keys property not found probably means we have no keys stored for the device\n");
        goto out;
    }

    keyArr = (struct persistArrayInRam*)(keys + 1);
    keyArr->tail = NULL;
    while (keyArr->head) {
        k = keyArr->head;
        keyArr->head = keyArr->head->next;
        persistPropFree(k);
    }

out:
    pthread_mutex_unlock(&mLock);
    return ret;
}

/*
 * FUNCTION: persistGetDevKey
 * USE:      Get a key (ours or another device's)
 * PARAMS:   addr - the address for whic hto retreive th ekey (NULL for our own)
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
    if (addr) {
        parent = persistAddKnownDevInt(addr, NULL, NULL, NULL); /* if we're finding a key for it means we saw it and/or are talking to it */
        if (parent)
            parent = persistPropFind(((struct persistArrayInRam*)(parent + 1))->head, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
    } else
        parent = persistPropFind(mPropsHead, PERS_NAME_KEYS, PERS_TYPE_ARRAY);
 
    if (!parent) {
        logw("Failed to find key list parent for given addess - >abandonning key search\n");
        goto out;
    }
    parArr = (struct persistArrayInRam*)(parent + 1);

    keyProp = persistFindKey(parArr->head, keyType);
    if (!keyProp)
        goto out;

    keyStruct = (struct persistKey*)(key + 1);
    memcpy(key, keyStruct->key, HCI_LINK_KEY_LEN);
    ret = true;

out:
    pthread_mutex_unlock(&mLock);
    return ret;
}



