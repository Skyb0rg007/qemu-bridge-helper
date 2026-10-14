/*
 * SPDX-License-Identifier: GPL-2.0-or-later OR MIT
 * SPDX-CopyrightText: 2026 Skye Soss
 */

/*
 * The unprivileged half of the qemu-bridge-helper replacement, and what qemu
 * runs as its bridge helper
 * ("qemu-bridge-helper [--use-vnet] --br=bridge --fd=unixfd"). It asks
 * qemu-bridge-helper-server for a tap device on the bridge, and passes the
 * tap fd it gets back on to qemu.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "qemu-bridge-helper.h"

struct settings {
    bool use_vnet;
    const char *bridge;
    int unixfd;
};

static void usage(void) {
    fprintf(stderr,
            "Usage: qemu-bridge-helper [--use-vnet] --br=bridge --fd=unixfd\n");
}

static int parse_arguments(struct settings *settings, char *args[]) {
    bool use_vnet = false;
    const char *bridge = NULL;
    int unixfd = -EBADF;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "--use-vnet") == 0) {
            use_vnet = true;
        } else if (strncmp(args[i], "--br=", 5) == 0) {
            bridge = &args[i][5];
        } else if (strncmp(args[i], "--fd=", 5) == 0) {
            char *end;
            long fd;

            errno = 0;
            fd = strtol(&args[i][5], &end, 10);
            if (errno != 0 || end == &args[i][5] || *end != 0
                    || fd < 0 || fd > INT32_MAX)
                return -1;
            unixfd = (int)fd;
        } else {
            return -1;
        }
    }

    if (bridge == NULL || unixfd < 0) {
        return -1;
    }

    settings->use_vnet = use_vnet;
    settings->bridge = bridge;
    settings->unixfd = unixfd;
    return 0;
}

static int connect_server(void) {
    _cleanup_close_ int c = -EBADF;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    const char *path;
    int r;

    /* The client is unprivileged, so letting the environment pick the
     * socket is harmless */
    path = getenv("QEMU_BRIDGE_HELPER_SOCKET");
    if (path == NULL || *path == 0)
        path = SOCKET_PATH;

    if (strlen(path) >= sizeof sa.sun_path) {
        fprintf(stderr, "socket path `%s' too long\n", path);
        return -ENAMETOOLONG;
    }
    strcpy(sa.sun_path, path);

    c = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (c < 0) {
        fprintf(stderr, "failed to create socket: %s\n", strerror(errno));
        return -errno;
    }

    r = set_timeout(c);
    if (r < 0) {
        fprintf(stderr, "failed to set socket timeout: %s\n", strerror(-r));
        return r;
    }

    if (connect(c, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "failed to connect to `%s': %s\n", path, strerror(errno));
        return -errno;
    }

    return TAKE_FD(c);
}

/* Receives the reply, and the tap fd if one was attached */
static int recv_reply(int c, struct reply *reply, int *ret_fd) {
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct iovec iov = {
        .iov_base = reply,
        .iov_len = sizeof *reply,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.buf,
        .msg_controllen = sizeof control.buf,
    };
    _cleanup_close_ int fd = -EBADF;
    struct cmsghdr *cmsg;
    ssize_t n;

    n = recvmsg(c, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0)
        return -errno;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS
                && cmsg->cmsg_len == CMSG_LEN(sizeof(int)) && fd < 0)
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    }

    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
            || (size_t)n != sizeof *reply || reply->magic != PROTO_MAGIC)
        return -EBADMSG;

    reply->message[sizeof reply->message - 1] = 0;
    *ret_fd = TAKE_FD(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    _cleanup_close_ int c = -EBADF;
    _cleanup_close_ int tapfd = -EBADF;
    struct settings settings;
    struct request request = { .magic = PROTO_MAGIC };
    struct reply reply;
    int r;
    (void)argc;

    r = parse_arguments(&settings, &argv[1]);
    if (r < 0) {
        usage();
        return EXIT_FAILURE;
    }

    if (strlen(settings.bridge) >= IFNAMSIZ) {
        fprintf(stderr, "name `%s' too long: %zu\n",
                settings.bridge, strlen(settings.bridge));
        return EXIT_FAILURE;
    }

    if (settings.use_vnet)
        request.flags |= REQUEST_USE_VNET;
    strcpy(request.bridge, settings.bridge);

    c = connect_server();
    if (c < 0)
        return EXIT_FAILURE;

    r = send_packet(c, &request, sizeof request, -EBADF);
    if (r < 0) {
        fprintf(stderr, "failed to send request: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    r = recv_reply(c, &reply, &tapfd);
    if (r < 0) {
        fprintf(stderr, "failed to receive reply: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    if (reply.error != 0) {
        fprintf(stderr, "%s\n", reply.message);
        return EXIT_FAILURE;
    }
    if (tapfd < 0) {
        fprintf(stderr, "reply has no tap device attached\n");
        return EXIT_FAILURE;
    }

    /* qemu expects a single zero byte along with the fd */
    r = send_packet(settings.unixfd, "", 1, tapfd);
    if (r < 0) {
        fprintf(stderr, "failed to write fd to unix socket: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
