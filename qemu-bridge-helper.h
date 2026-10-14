/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-FileCopyrightText: 2026 Skye Soss
 */
#ifndef QEMU_BRIDGE_HELPER_H
#define QEMU_BRIDGE_HELPER_H

#include <errno.h>
#include <unistd.h>

#ifndef SOCKET_PATH
#define SOCKET_PATH "/run/qemu-bridge-helper/qemu-bridge-helper.sock"
#endif

#define INTERFACE_NAME "website.soss.QemuBridgeHelper"
#define METHOD_CREATE_TAP INTERFACE_NAME ".CreateTap"
#define ERROR_BRIDGE_NOT_ALLOWED INTERFACE_NAME ".BridgeNotAllowed"
#define ERROR_NO_SUCH_BRIDGE INTERFACE_NAME ".NoSuchBridge"

static inline void closep(int *fd) {
    if (*fd >= 0)
        close(*fd);
}

#define _cleanup_(f) __attribute__((cleanup(f)))
#define _cleanup_close_ _cleanup_(closep)
#define TAKE_FD(var) __extension__ ({ \
        int *_pvar_ = &(var); \
        int _var_ = *_pvar_; \
        *_pvar_ = -EBADF; \
        _var_; })

#endif
