#include <hardware/bluetooth.h>
#include <hardware/bt_sock.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include "aapiSocket.h"
#include "rfcomm.h"
#include "config.h"
#include "aapi.h"
#include "hci.h"
#include "sdp.h"
#include "log.h"
#include "sg.h"
#include "mt.h"


struct aapiSock {
    struct aapiSock *prev;
    struct aapiSock *next;
    btsock_type_t type;
    int fd, java_fd;
    struct bt_addr peer;
    union {
        struct {
            union { /* listening sockets have no rfc_conn_t */
                rfc_conn_t conn;
                uint32_t sdpHandle;
            };
            uint8_t state;
            uint8_t chNum;
            sg bufRx;
            sg bufTx;
        } rfc;
    };
};

struct appiSockListener {
    struct appiSockListener *next;
    uint32_t sdpHandle;
    uint8_t uuid[16];
};

#define RFC_SOCK_STATE_LISTENING       1
#define RFC_SOCK_STATE_AUTOACCEPTING   2 /* we allocated an instance but it is not yet openened by rfcomm */
#define RFC_SOCK_STATE_ESTABLISHED     3 /* it is open */


#define WORKER_MSG_SOCK_ADD            0
#define WORKER_MSG_FD_CAN_RX           1 /* our user may now RX data */
#define WORKER_MSG_FD_CAN_TX           2 /* our user may now TX data */
#define WORKER_MSG_FD_CANNOT_RX        3
#define WORKER_MSG_FD_CANNOT_TX        4
#define WORKER_MSG_SOCK_DEL            5 /* delete this socket */
#define WORKER_MSG_QUIT                6 /* TODO: currently unused */

/* our state */
static struct aapiSock *mSocks = NULL;
static pthread_mutex_t mSocksLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mWorker;
static int mWorkerPipe = -1;
static bool mSocksUp;
static bool mSocksWantUp;
static struct appiSockListener *mListeners = NULL;


/* fwd decls */
static uint32_t aapiSockAddSdpRecord(const uint8_t* serviceUuid, uint8_t rfcommCh, const char* name);
static void aapiSockFree(struct aapiSock* sock);
static void aapiSockWorkerMsgSend(uint8_t msg, void *aux);



/*
 * FUNCTION: aapiWorker
 * USE:      The worker thread for aapi.sock
 * PARAMS:   pipe - actually an int - the pipe we listen to to wake up
 * RETURN:   NONE
 * NOTES:
 */
static void *aapiSockWorker(void *pipe)
{
    uint32_t i, arrSz = 16/* a nice starting number */, arrNum = 1;
    struct aapiSock **socks = NULL;
    struct pollfd *fds = NULL;
    bool quit = false;
    uint8_t rdbuf[4096]; /* we read from java using this */

    fds = malloc(sizeof(struct pollfd) * arrSz);
    if (!fds) {
        loge("Failed to alloc fds\n");
        return NULL;
    }

    socks = malloc(sizeof(struct aapiSock*) * arrSz);
    if (!socks) {
        loge("Failed to alloc socks\n");
        free(fds);
        return NULL;
    }
    fds[0].fd = (int) pipe;
    fds[0].events = POLLIN;
    

    while (!quit) {
        int ret;
        for (i = 0; i < arrNum; i++)
            fds[i].revents = 0;

        for (i = 0; i < arrNum; i++)
            logd(" in.fds[%d] = {%3d, 0x%04x, 0x%04x}\n", i, fds[i].fd, fds[i].events, fds[i].revents);

        do {
            ret = poll(fds, arrNum, -1);
        } while (ret == -1 && errno == EINTR);

        for (i = 0; i < arrNum; i++)
            logd("out.fds[%d] = {%3d, 0x%04x, 0x%04x}\n", i, fds[i].fd, fds[i].events, fds[i].revents);

        if (fds[0].revents & POLLIN) {
            uint8_t msg;
            struct aapiSock *sock;

            r_read(fds[0].fd, &msg, sizeof(msg));
            r_read(fds[0].fd, &sock, sizeof(sock));
            logd("msg %u with p %p\n", msg, sock);
            if(sock)
                logd("sock %p has state %u and fd %u with sdp handle 0x%08x\n", sock, sock->rfc.state, sock->fd, sock->rfc.sdpHandle);
            switch (msg) {
            case WORKER_MSG_SOCK_ADD:
                if (arrNum == arrSz) {
                    uint32_t newArrSz = arrSz + 16; /* we grow 16 at a time */
                    struct pollfd *newFds = realloc(fds, sizeof(struct pollfd) * newArrSz);
                    struct aapiSock **newSocks = realloc(socks, sizeof(struct aapiSock*) * newArrSz);

                    if (newFds)
                        fds = newFds;
                    if (newSocks)
                        socks = newSocks;
                    if (!newFds || !newSocks) {
                        logw("Failed to resize structs to add new sock\n");
                        break;
                    }
                    arrSz = newArrSz;
                }
                socks[arrNum] = sock;
                fds[arrNum].fd = sock->fd;
                fds[arrNum].events = POLLERR | POLLNVAL | POLLHUP;
                fds[arrNum].revents = 0;
                arrNum++;
                break;
            case WORKER_MSG_FD_CAN_RX:
            case WORKER_MSG_FD_CAN_TX:
            case WORKER_MSG_FD_CANNOT_RX:
            case WORKER_MSG_FD_CANNOT_TX:
            case WORKER_MSG_SOCK_DEL:
                for (i = 0; i < arrNum && socks[i] != sock; i++);
                if (i == arrNum) {
                    logw("worker(%u) called with unknown sock\n", msg);
                    break;
                }
                if (msg == WORKER_MSG_FD_CAN_RX)
                    fds[i].events |= POLLOUT;
                else if (msg == WORKER_MSG_FD_CAN_TX)
                    fds[i].events |= POLLIN;
                else if (msg == WORKER_MSG_FD_CANNOT_RX)
                    fds[i].events &=~ POLLOUT;
                else if (msg == WORKER_MSG_FD_CANNOT_TX)
                    fds[i].events &=~ POLLIN;
                else {
                    memcpy(fds + i, fds + arrNum - 1, sizeof(struct pollfd));
                    socks[i] = socks[arrNum - 1];
                    arrNum--;
                    aapiSockFree(sock);
                }
                break;
            case WORKER_MSG_QUIT:
                quit = true;
                logd("AAPI.sock worker exiting\n");
                break;
            default:
                loge("Unknown AAPI.worker command %u\n", msg);
                break;
            }
        }
        for (i = 1; i < arrNum; i++) {
            uint32_t done;
            void *iter;

            if (fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                logd("fd %d (sock %d) in state %u failed with error %d -> deleting\n", fds[i].fd, i, socks[i]->rfc.state, fds[i].revents);
                aapiSockWorkerMsgSend(WORKER_MSG_SOCK_DEL, socks[i]);
                continue;
            }

            if (!(fds[i].events & fds[i].revents))
                continue;

            if (fds[i].revents & POLLIN) { /* java has sent us data */
                int32_t txRet;
                while(1) {
                    ssize_t now;

                    if (sgLength(socks[i]->rfc.bufTx) >= AAPI_RFC_MAX_BUF) {
                        fds[i].events &=~ POLLIN; /* no buffer space - stop reading */
                        logd("TX throttled in socks api\n");
                        break;
                    }
                    now = read(fds[i].fd, rdbuf, sizeof(rdbuf));
                    if (now <= 0)
                        break;
                    if (!sgConcatBackCopy(socks[i]->rfc.bufTx, rdbuf, now))
                        loge("Failed to concat read data. TX data will be lost\n");
                    if (now != sizeof(rdbuf))
                        break;
                }
                txRet = rfcTx(socks[i]->rfc.conn, socks[i]->rfc.bufTx);
                if (txRet == RFC_TX_RET_ENTIRE_SENT) {
                    socks[i]->rfc.bufTx = sgNew();
                    if (!socks[i]->rfc.bufTx) {
                        loge("Failed to alloc new TX sg for conn. Closing\n");
                        aapiSockWorkerMsgSend(WORKER_MSG_SOCK_DEL, socks[i]);
                    }
                } else if (txRet == RFC_TX_RET_STOP_SENDING) {
                    fds[i].events &=~ POLLIN; /* no buffer space - stop reading */
                    logd("TX throttled in rfc\n");
                } else if (txRet == RFC_TX_RET_NO_SUCH_CONN || txRet == RFC_TX_RET_ERROR) {
                    loge("Rfc sock error %d. Closing\n", txRet);
                    aapiSockWorkerMsgSend(WORKER_MSG_SOCK_DEL, socks[i]);
                }
            }
            if (fds[i].revents & POLLOUT) { /* java is ready for data */
                pthread_mutex_lock(&mSocksLock);
                for (done = 0, iter = sgIterStart(socks[i]->rfc.bufRx); iter; iter = sgIterAdvance(iter)) {
                    ssize_t now = write(fds[i].fd, sgIterCurData(iter), sgIterCurLen(iter));
                    if (now <= 0)
                        break;
                    done += now;
                    if ((unsigned)now < sgIterCurLen(iter))
                        break;
                }
                sgTruncFront(socks[i]->rfc.bufRx, done);
                if (sgLength(socks[i]->rfc.bufRx) < AAPI_RFC_MAX_BUF)
                    rfcFlow(socks[i]->rfc.conn, false);
                if (!sgLength(socks[i]->rfc.bufRx))
                    fds[i].events &=~ POLLOUT; /* if we have no more to give, do not bother caring if java can read */
                pthread_mutex_unlock(&mSocksLock);
            }
        }
    }

    for (i = 1; i < arrNum; i++)
        aapiSockFree(socks[i]);
    close((int)pipe);    

    free(fds);
    free(socks);
    return NULL;
}

/*
 * FUNCTION: aapiSockDoInit
 * USE:      Init aapi.sock worker thread & pipes
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void aapiSockDoInit(void)
{
    int pipes[2];

    if (pipe(pipes))
        return;

    if (pthread_create(&mWorker, NULL, aapiSockWorker, (void*)pipes[0])) {
        close(pipes[0]);
        close(pipes[1]);
        return;
    }
    mWorkerPipe = pipes[1];
    mSocksUp = true;
    mSocksWantUp = false;
}

/*
 * FUNCTION: aapiSockDoDeinit
 * USE:      Deinit aapi.sock
 * PARAMS:   NONE
 * RETURN:   NONE
 * NOTES:
 */
static void aapiSockDoDeinit(void)
{
    while (mListeners) {
        struct appiSockListener *sl = mListeners;
        mListeners = mListeners->next;
        free(sl);
    }

    aapiSockWorkerMsgSend(WORKER_MSG_QUIT, NULL);
    pthread_join(mWorker, NULL);
    close(mWorkerPipe);
    mSocksUp = false;
}

/*
 * FUNCTION: aapiSockWorkerMsgSend
 * USE:      Wake up aapi.sock worker with a message
 * PARAMS:   msg - the message
 *           aux - additional data for said message
 * RETURN:   NONE
 * NOTES:
 */
static void aapiSockWorkerMsgSend(uint8_t msg, void *aux)
{
    uint8_t t = msg;

    r_write(mWorkerPipe, &t, 1);
    r_write(mWorkerPipe, &aux, sizeof(void*));
}

/*
 * FUNCTION: aapiSockAlloc
 * USE:      Create a sock struct and link it into the list
 * PARAMS:   type - socket type (as per bt iface)
 *           sockType - local socket type (for linux kernel, SOCK_STREAM/SOCK_DGRAM)
 * RETURN:   sock struct or NULL on error
 * NOTES:    call with mSocksLock held
 */
static struct aapiSock* aapiSockAlloc(btsock_type_t type, int sockType)
{
    struct aapiSock *sock = calloc(1, sizeof(struct aapiSock));
    int socks[2], ret;

    if (!sock)
        return NULL;

    ret = socketpair(AF_LOCAL, sockType, 0, socks);
    if (ret) {
        free(sock);
        return NULL;
    }

    fcntl(socks[0], F_SETFL, O_NONBLOCK | fcntl(socks[0], F_GETFL));

    sock->type = type;
    sock->fd = socks[0];
    sock->java_fd = socks[1];
    sock->next = mSocks;
    if (mSocks)
        mSocks->prev = sock;
    mSocks = sock;

    return sock;
}

/*
 * FUNCTION: aapiSockAllocRfc
 * USE:      Create an RFC sock struct and link it into the list
 * PARAMS:   chNum - the chNum to save in the sock
 *           state - the state to put it into
 * RETURN:   sock struct or NULL on error
 * NOTES:    call with mSocksLock held
 */
static struct aapiSock* aapiSockAllocRfc(uint8_t chNum, uint8_t state)
{
    struct aapiSock *sock = NULL;
    sg bufTx = sgNew();
    sg bufRx = sgNew();

    if (!bufTx && ! bufRx) {
        if (bufTx)
            sgFree(bufTx);
        if (bufRx)
            sgFree(bufRx);
        return NULL;
    }

    sock = aapiSockAlloc(BTSOCK_RFCOMM, SOCK_STREAM);
    if (sock) {
        sock->rfc.chNum = chNum;
        sock->rfc.state = state;
        sock->rfc.bufTx = bufTx;
        sock->rfc.bufRx = bufRx;
    } else {
        sgFree(bufTx);
        sgFree(bufRx);
    }

    return sock;
}

/*
 * FUNCTION: aapiSockFree
 * USE:      Free a socket struct
 * PARAMS:   sock - the socket struct
 * RETURN:   NONE
 * NOTES:
 */
static void aapiSockFree(struct aapiSock* sock)
{
    pthread_mutex_lock(&mSocksLock);
    shutdown(sock->fd, SHUT_RDWR);
    close(sock->fd);

    if (sock->java_fd != -1)
        close(sock->java_fd);

    if (sock->prev)
        sock->prev->next = sock->next;
    else
        mSocks = sock->next;
    if (sock->next)
        sock->next->prev = sock->prev;
    switch (sock->type) {
    case BTSOCK_RFCOMM:
        sgFree(sock->rfc.bufTx);
        sgFree(sock->rfc.bufRx);
        if (sock->rfc.state == RFC_SOCK_STATE_LISTENING) {
            struct appiSockListener *sl, *p = NULL;
            for (sl = mListeners; sl && sl->sdpHandle != sock->rfc.sdpHandle; p = sl, sl = sl->next);
            if (!sl)
                logw("Failed to find listener object for closed listening socket\n");
            else {
                if (p)
                    p->next = sl->next;
                else
                    mListeners = sl->next;
                free(sl);
            }
            rfcStopListen(sock->rfc.chNum);
            sdpServiceDescriptorDel(sl->sdpHandle);
        } else if (sock->rfc.conn)
            rfcClose(sock->rfc.conn);
        break;
    default:
        logw("Unknown socket type %u on close\n", sock->type);
        break;
    }
    free(sock);
    pthread_mutex_unlock(&mSocksLock);
}

static bool aapiSockNotifyOfConnReq(struct aapiSock *srvSock, struct aapiSock *newSock)
{
    sock_connect_signal_t sig = {.size = sizeof(sock_connect_signal_t), .channel = srvSock->rfc.chNum, .status = 0, };
    uint8_t *sigBuf  = (uint8_t*)&sig;
    char cmsgBuf[CMSG_SPACE(1)];
    int ret, len = sizeof(sig);
    struct cmsghdr *cmsg;
    struct msghdr msg;

    memcpy(sig.bd_addr.address, newSock->peer.addr, sizeof(sig.bd_addr.address));


    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(newSock->java_fd));
    memcpy(CMSG_DATA(cmsg), &newSock->java_fd, sizeof(newSock->java_fd));

    while (len) {
        struct iovec ovec = {0,};

        ovec.iov_base = sigBuf;
        ovec.iov_len = len;

        msg.msg_iov = &ovec;
        msg.msg_iovlen = 1;

        do {
            ret = sendmsg(srvSock->fd, &msg, MSG_NOSIGNAL);
        } while (ret < 0 && errno == EINTR);

        if (ret == -1)
            return false;

        sigBuf += ret;
        len -= ret;

        memset(&msg, 0, sizeof(msg));
    }
    close(newSock->java_fd);
    newSock->java_fd = -1;

    return true;
}

/*
 * FUNCTION: aapiSockRfcListenAlloc
 * USE:      Called when a remote device tries to connect to our listening RFC socket
 * PARAMS:   userData - our socket
 *           peerAddr - the peer address
 *           chNum - the chNum 
 * RETURN:   bt_status_t
 * NOTES:
 */
static void* aapiSockRfcListenAlloc(void *userData, const struct bt_addr *peerAddr, uint8_t chNum)
{
    /* we have no way to ask user to accept or not, so we accept for the user, and sort it out later */
    struct aapiSock *srvSock = (struct aapiSock*)userData;
    struct aapiSock *newSock;

    pthread_mutex_lock(&mSocksLock);
    newSock = aapiSockAllocRfc(chNum, RFC_SOCK_STATE_AUTOACCEPTING);

    if (!newSock)
        goto out;

    logd("aapi sock alloc ch %u\n", chNum);
    srvSock->rfc.chNum = chNum; /* in case we get here before aapiSockListen() finishes */

    memcpy(&newSock->peer, peerAddr, sizeof(newSock->peer));
    if (!aapiSockNotifyOfConnReq(srvSock, newSock)) {
        loge("Failed to notify of new connection\n");
        aapiSockFree(newSock);
        newSock = NULL;
    }

out:
    pthread_mutex_unlock(&mSocksLock);
    return newSock;
}

/*
 * FUNCTION: aapiSockRfcListenEvt
 * USE:      Called when an even arises on an RFC connection
 * PARAMS:   userData - unused
 *           instance - our socket
 *           evt - what happened
 *           s - the data
 * RETURN:   NONE
 * NOTES:
 */
static void aapiSockRfcListenEvt(void *userData, void *instance, uint8_t evt, sg s)
{
    struct aapiSock *sock = (struct aapiSock*)instance;

    logd("aapi sock evt %u ch %u\n", evt, sock->rfc.chNum);
    pthread_mutex_lock(&mSocksLock);

    switch (evt) {
    case RFC_EVT_OPEN:
        if (sock->rfc.state != RFC_SOCK_STATE_AUTOACCEPTING)
            logw("Weird socket state %u on rfc open\n", sock->rfc.state);
        sock->rfc.state = RFC_SOCK_STATE_ESTABLISHED;
        sgSerialize(s, 0, sizeof(sock->rfc.conn), &sock->rfc.conn);
        sgFree(s);
        aapiSockWorkerMsgSend(WORKER_MSG_SOCK_ADD, sock);
        aapiSockWorkerMsgSend(WORKER_MSG_FD_CAN_TX, sock);
        break;
    case RFC_EVT_RX:
        sgConcat(sock->rfc.bufRx, s);
        if (sgLength(sock->rfc.bufRx) >= AAPI_RFC_MAX_BUF)
            rfcFlow(sock->rfc.conn, true);
        aapiSockWorkerMsgSend(WORKER_MSG_FD_CAN_RX, sock);
        break;
    case RFC_EVT_MAY_SEND:
        aapiSockWorkerMsgSend(WORKER_MSG_FD_CAN_TX, sock);
        break;
    case RFC_EVT_CLOSE:
        logd("Connection to "ADDRFMT"[%u] closed\n", ADDRCONV(sock->peer), sock->rfc.chNum);
        sock->rfc.conn = 0;
        aapiSockWorkerMsgSend(WORKER_MSG_SOCK_DEL, sock);
    }
    pthread_mutex_unlock(&mSocksLock);
}

/*
 * FUNCTION: aapiSockListen
 * USE:      Listen for connections from a peer on a socket
 * PARAMS:   type - socket type
 *           serviceName - service name to advertise
 *           serviceUuid - service uuid to advertise
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t aapiSockListen(btsock_type_t type, const char* serviceName, const uint8_t* serviceUuid, int channel, int* sockFdP, int flags)
{
    bt_status_t ret = BT_STATUS_FAIL;
    struct appiSockListener *sl = NULL, *p;
    struct aapiSock* sock;
    uint8_t ch;


    pthread_mutex_lock(&mSocksLock);
    if (!mSocksUp) {
        logw("Refusing sock listen when socks not up\n");
        goto out;
    }

    //XXX: HACK, was: for (p = NULL, sl = mListeners; sl && memcmp(sl->uuid, serviceUuid, sizeof(sl->uuid)); p = sl, sl = sl->next);
    if (sl) {
        logw("Sock listen when identical listener exists -> deleting old\n");
        for (sock = mSocks; sock; sock = sock->next) {
            if (sock->rfc.state != RFC_SOCK_STATE_LISTENING)
                continue;
            if (sock->rfc.sdpHandle == sl->sdpHandle)
                break;
        }
        if (sock) {
            logd("Previous identical socket found\n");
            aapiSockWorkerMsgSend(WORKER_MSG_SOCK_DEL, sock);
        } else {
            logw("Failed to find previous listening socket. SDP likely to be leaked->auto-freeing\n");
            sdpServiceDescriptorDel(sl->sdpHandle);
            if (p)
                p->next = sl->next;
            else
                mListeners = sl->next;
            free(sl);
        }
    }
    sl = malloc(sizeof(struct appiSockListener));
    if (sl)
        memcpy(sl->uuid, serviceUuid, sizeof(sl->uuid));
    else {
        loge("Failed to alloc a listener record\n");
        goto out;
    }

    logd("AAPI.sock: listen(%u,'%s',%d,%u)\n", type, serviceName, channel, flags);

    if (type != BTSOCK_RFCOMM) {
        logw("Refusing non-RFCOMM sockets for now\n");
        goto out;
    }

    if (channel == -1) /* magic number! */
        ch = RFC_CHNUM_REQ_ANY;
    else if (channel < RFC_FIRST_CHAN || channel >= RFC_NUM_CHANS + RFC_FIRST_CHAN) {
        logw("Refusing RFCOMM listen on invalid channel %d\n", channel);
        goto out;
    } else ch = channel;

    sock = aapiSockAllocRfc(ch, RFC_SOCK_STATE_LISTENING); /* yes, "ch" might be RFC_CHNUM_REQ_ANY here. this is actually ok */
    if (!sock)
        goto out;

    ch = rfcListen(ch, aapiSockRfcListenAlloc, aapiSockRfcListenEvt, sock);
    if (!ch) {
        logw("Failed to RFC listen\n");
        goto out_sockfree;
    }

    sock->rfc.chNum = ch;
    logd("RFC ch %u opened for listening with name '%s'\n", ch, serviceName);

    /* XXX: this makes me feel dirty all over, but sadly andriod api demands this be sent as an int and in native byte order... */
    channel = ch; /* send the channel ID to java */
    if (!sgConcatBackCopy(sock->rfc.bufRx, &channel ,sizeof(channel))) {
        loge("Failed to enqueue channel number for java\n");
        goto out_stoplisten;
    }

    sock->rfc.sdpHandle = aapiSockAddSdpRecord(serviceUuid, ch, serviceName);
    logd("sock %p has atate %u and fd %u with sdp handle 0x%08x\n", sock, sock->rfc.state, sock->fd, sock->rfc.sdpHandle);
    if (!sock->rfc.sdpHandle) {
        loge("Failed to add SDP record\n");
        goto out_stoplisten;
    }


    sl->sdpHandle = sock->rfc.sdpHandle;
    sl->next = mListeners;
    mListeners = sl;
    
    *sockFdP = sock->java_fd;
    sock->java_fd = -1;
    aapiSockWorkerMsgSend(WORKER_MSG_SOCK_ADD, sock);
    aapiSockWorkerMsgSend(WORKER_MSG_FD_CAN_RX, sock);
    ret = BT_STATUS_SUCCESS;
    goto out;

out_stoplisten:
    rfcStopListen(ch);

out_sockfree:
    aapiSockFree(sock);
out:
    pthread_mutex_unlock(&mSocksLock);
    return ret;
}

/*
 * FUNCTION: aapiSockConnect
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - socket type
 *           uuid - service uuid
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
static bt_status_t aapiSockConnect(const bt_bdaddr_t *bd_addr, btsock_type_t type, const uint8_t* uuid, int channel, int* sockFdP, int flags)
{
    bt_status_t ret = BT_STATUS_FAIL;

    logd("AAPI.sock: connect(%02X:%02X:%02X:%02X:%02X:%02X,%u,%d,%u)\n",
         bd_addr->address[5], bd_addr->address[4], bd_addr->address[3], bd_addr->address[2], bd_addr->address[1], bd_addr->address[0],
         type, channel, flags);

    pthread_mutex_lock(&mSocksLock);
    if (!mSocksUp) {
        pthread_mutex_unlock(&mSocksLock);
        logw("Refusing sock connect when socks not up\n");
        goto out;
    }
    pthread_mutex_unlock(&mSocksLock);

    if (type != BTSOCK_RFCOMM) {
        logw("Refusing non-RFCOMM sockets for now\n");
        goto out;
    }

    if (channel != -1 && (channel < RFC_FIRST_CHAN || channel >= RFC_NUM_CHANS + RFC_FIRST_CHAN)) {
        logw("Refusing RFCOMM connect on invalid channel %d\n", channel);
        goto out;
    }

    //TODO

out:
    return ret;
}


/*
 * FUNCTION: aapiSockAddSdpRecord
 * USE:      Add an SDP record for this service
 * PARAMS:   serviceUuid - the uuid
 *           rfcommCh - the rfcomm channel number
 *           name - the name to advertise
 * RETURN:   handle or 0 on error
 * NOTES:
 */
static uint32_t aapiSockAddSdpRecord(const uint8_t* serviceUuid, uint8_t rfcommCh, const char* name)
{
    uint8_t *record, *t;
    uint32_t ret, nameLen = name ? strlen(name) : 0, recLen;
    static const uint8_t preUuidData[] = {
        /* service class ID list */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_CLS_ID_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 17, SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_16)};

    /* here go 16 bbytes of uuid */

    static const uint8_t preChannelData[] = {
        /* ServiceId */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SVC_ID),
        SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_SVC_CLASS_SPP),
        /* ProtocolDescriptorList */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_PROTOCOL_DESCR_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 15,
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 6,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_L2CAP),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(PSM_RFCOMM),
            SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 5,
                SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_PROTO_RFCOMM),
                SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_1)};

    /* here goes one byte channel number */

    static const uint8_t preNameData[] = {
        /* browse group list */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_BROWSE_GRP_LIST),
        SDP_ITEM_DESC(SDP_TYPE_ARRAY, SDP_SZ_u8), 3,
            SDP_ITEM_DESC(SDP_TYPE_UUID, SDP_SZ_2), SDP_U16(SDP_BROWSE_GROUP_ID_PUBLIC),
        /* name */
        SDP_ITEM_DESC(SDP_TYPE_UINT, SDP_SZ_2), SDP_U16(SDP_ATTR_SPP_NAME),
        SDP_ITEM_DESC(SDP_TYPE_TEXT, SDP_SZ_u8)};

    /* here goes one-byte name length and then the name */


    if (nameLen > 255) {
        logw("Truncating RFCOMM advertised name length\n");
        nameLen = 255;
    }
    recLen = sizeof(preUuidData) + sizeof(preChannelData) + sizeof(preNameData) + 16 + 1 + 1 + nameLen;
    record = malloc(recLen);
    if (!record)
        return 0;

    t = record;
    memcpy(t, preUuidData, sizeof(preUuidData));
    t += sizeof(preUuidData);
    memcpy(t, serviceUuid, 16);
    t += 16;
    memcpy(t, preChannelData, sizeof(preChannelData));
    t += sizeof(preChannelData);
    *t++ = rfcommCh;
    memcpy(t, preNameData, sizeof(preNameData));
    t += sizeof(preNameData);
    *t++ = nameLen;
    memcpy(t, name, nameLen);

    ret = sdpServiceDescriptorAdd(record, recLen);
    if (ret)
        return ret;

    free(record);
    return 0;
}



/*
 * FUNCTION: aapiGetProfileIfaceSockets
 * USE:      Try to connect to a peer's matching listening socket
 * PARAMS:   bd_addr - peer address
 *           type - socket type
 *           uuid - service uuid
 *           channel - channel number
 *           sockFdP - store socket's FD here
 *           flags - other relevant flags
 * RETURN:   bt_status_t
 * NOTES:
 */
const void* aapiGetProfileIfaceSockets(void)
{
    static const btsock_interface_t sockIface = {
        .size = sizeof(btsock_interface_t),
        .listen = aapiSockListen,
        .connect = aapiSockConnect,
    };

    pthread_mutex_lock(&mSocksLock);
    if (mSocksUp)
        logd("socks up again\n");
    else if (!aapiIsStackUp())
        mSocksWantUp = true;
    else
        aapiSockDoInit();

    pthread_mutex_unlock(&mSocksLock);

    return &sockIface;
}

/*
 * FUNCTION: aapiSocketNotifStackState
 * USE:      Stack uses this to tell us of stack up/down state changes
 * PARAMS:   up - true if stack is now up, false else
 * RETURN:   NONE
 * NOTES:
 */
void aapiSocketNotifStackState(bool up)
{
    pthread_mutex_lock(&mSocksLock);
    if (up && !mSocksUp && mSocksWantUp) {
        logd("socks coming up now\n");
        aapiSockDoInit();
    } else if (!up) {
        if(mSocksUp)
            aapiSockDoDeinit();
        mSocksWantUp = false;
    }
    pthread_mutex_unlock(&mSocksLock);
}









