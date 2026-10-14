/*
 * SPDX-License-Identifier: GPL-2.0-or-later OR MIT
 * SPDX-CopyrightText: 2026 Skye Soss
 */

/*
 * Shared between the qemu-bridge-helper client, which qemu runs as its bridge
 * helper, and qemu-bridge-helper-server, which creates the tap devices.
 */
#ifndef QEMU_BRIDGE_HELPER_H
#define QEMU_BRIDGE_HELPER_H

#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifndef SOCKET_PATH
#define SOCKET_PATH "/run/qemu-bridge-helper.sock"
#endif

/* How long either side waits for the other before giving up */
#define TIMEOUT_SEC 10

/* systemd-style helpers */
static inline void closep(int *fd) {
    if (*fd >= 0)
        close(*fd);
}

#define _cleanup_close_ __attribute__((cleanup(closep)))
#define TAKE_FD(var) __extension__ ({ \
        int *_pvar_ = &(var); \
        int _var_ = *_pvar_; \
        *_pvar_ = -EBADF; \
        _var_; })

/*
 * Wire protocol: one request and one reply per SOCK_SEQPACKET connection,
 * each a single fixed-size packet. The client and server are built from the
 * same source, so host byte order and struct layout are fine.
 */
#define PROTO_MAGIC 0x31524251 /* "QBR1" */

#define REQUEST_USE_VNET (UINT32_C(1) << 0)

struct request {
    uint32_t magic;
    uint32_t flags;
    char bridge[IFNAMSIZ];
};

/* On success error is 0 and the tap fd is attached with SCM_RIGHTS */
struct reply {
    uint32_t magic;
    int32_t error;      /* 0 or a positive errno */
    char ifname[IFNAMSIZ];
    char message[232];  /* why the request failed, for the client to print */
};

/* Sends a single packet, with fd attached if it's valid */
static inline int send_packet(int c, const void *data, size_t size, int fd) {
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct iovec iov = {
        .iov_base = (void *)data,
        .iov_len = size,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    if (fd >= 0) {
        struct cmsghdr *cmsg;

        memset(&control, 0, sizeof control);
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof control.buf;

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }

    if (sendmsg(c, &msg, MSG_NOSIGNAL) < 0)
        return -errno;
    return 0;
}

static inline int set_timeout(int fd) {
    struct timeval tv = { .tv_sec = TIMEOUT_SEC };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0
            || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
        return -errno;
    return 0;
}

#endif
