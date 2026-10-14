/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-CopyrightText: 2026 Skye Soss
 */

/*
 * The unprivileged half of the qemu-bridge-helper replacement, and what qemu
 * runs as its bridge helper
 * ("qemu-bridge-helper [--use-vnet] --br=bridge --fd=unixfd"). It calls
 * CreateTap() on qemu-bridge-helper-server over Varlink, and passes the tap
 * fd it gets back on to qemu.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "qemu-bridge-helper.h"

/* Sends fd to qemu with the single zero byte it expects alongside it */
static int send_fd(int c, int fd) {
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    char data[1] = { 0x00 };
    struct iovec iov = {
        .iov_base = data,
        .iov_len = sizeof data,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.buf,
        .msg_controllen = sizeof control.buf,
    };
    struct cmsghdr *cmsg;

    memset(&control, 0, sizeof control);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (sendmsg(c, &msg, MSG_NOSIGNAL) < 0)
        return -errno;
    return 0;
}

int main(int argc, char *argv[]) {
    _cleanup_(sd_varlink_unrefp) sd_varlink *link = NULL;
    _cleanup_close_ int tapfd = -EBADF;
    sd_json_variant *reply = NULL;
    const char *error_id = NULL;
    const char *bridge = NULL;
    const char *path;
    bool use_vnet = false;
    int unixfd = -1;
    int r;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--use-vnet") == 0) {
            use_vnet = true;
        } else if (strncmp(argv[i], "--br=", 5) == 0) {
            bridge = &argv[i][5];
        } else if (strncmp(argv[i], "--fd=", 5) == 0) {
            char *end;
            long fd = strtol(&argv[i][5], &end, 10);
            unixfd = end != &argv[i][5] && *end == 0 && fd >= 0 && fd <= INT_MAX
                     ? (int)fd : -1;
        } else {
            bridge = NULL;
            break;
        }
    }

    if (bridge == NULL || unixfd < 0) {
        fprintf(stderr,
                "Usage: qemu-bridge-helper [--use-vnet] --br=bridge --fd=unixfd\n");
        return EXIT_FAILURE;
    }

    /* The client is unprivileged, so letting the environment pick the
     * socket is harmless */
    path = getenv("QEMU_BRIDGE_HELPER_SOCKET");
    if (path == NULL || *path == 0)
        path = SOCKET_PATH;

    r = sd_varlink_connect_address(&link, path);
    if (r >= 0)
        r = sd_varlink_set_allow_fd_passing_input(link, true);
    if (r < 0) {
        fprintf(stderr, "failed to connect to `%s': %s\n", path, strerror(-r));
        return EXIT_FAILURE;
    }

    r = sd_varlink_callbo(link, METHOD_CREATE_TAP, &reply, &error_id,
                          SD_JSON_BUILD_PAIR_STRING("bridge", bridge),
                          SD_JSON_BUILD_PAIR_BOOLEAN("vnetHeader", use_vnet));
    if (r < 0) {
        fprintf(stderr, "failed to call %s: %s\n", METHOD_CREATE_TAP, strerror(-r));
        return EXIT_FAILURE;
    }
    if (error_id != NULL) {
        /* Errors without an errno equivalent map to -EBADR */
        r = sd_varlink_error_to_errno(error_id, reply);
        fprintf(stderr, "failed to create tap device on bridge `%s': %s\n",
                bridge, r == -EBADR ? error_id : strerror(-r));
        return EXIT_FAILURE;
    }

    tapfd = sd_varlink_take_fd(link, sd_json_variant_unsigned(
                    sd_json_variant_by_key(reply, "fileDescriptor")));
    if (tapfd < 0) {
        fprintf(stderr, "reply has no tap device attached: %s\n", strerror(-tapfd));
        return EXIT_FAILURE;
    }

    r = send_fd(unixfd, tapfd);
    if (r < 0) {
        fprintf(stderr, "failed to write fd to unix socket: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
