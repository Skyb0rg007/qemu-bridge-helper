/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-CopyrightText: 2026 Skye Soss
 */

/*
 * The privileged half of the qemu-bridge-helper replacement. It's socket
 * activated (Accept=yes, one process per connection) with CAP_NET_ADMIN and
 * serves the website.soss.QemuBridgeHelper Varlink interface on that
 * connection. CreateTap() checks the bridge against the same ACL file as
 * upstream, creates and configures the tap device, and returns its fd.
 */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <systemd/sd-daemon.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>
#include <systemd/sd-varlink-idl.h>

#include "qemu-bridge-helper.h"

#ifndef ACL_FILE
#define ACL_FILE "/etc/qemu/bridge.conf"
#endif

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (p == NULL)
        abort();
    return p;
}

/* QEMU uses glib and their own version of queue.h */
#define g_malloc(N) xmalloc(N)
#define g_ascii_isspace(C) (strchr(" \r\t\n\v", (C)) != NULL)
#define g_str_equal(S1, S2) (strcmp((S1), (S2)) == 0)
#define QSIMPLEQ_ENTRY(T) STAILQ_ENTRY(T)
#define QSIMPLEQ_HEAD(N, T) STAILQ_HEAD(N, T)
#define QSIMPLEQ_INSERT_TAIL(H, E, F) STAILQ_INSERT_TAIL(H, E, F)

/* SPDX-SnippetBegin
 * SPDX-License-Identifier: GPL-2.0-or-later
 * SPDX-SnippetCopyrightText: Copyright IBM, Corp. 2011
 * SPDX-SnippetComment: This copies the exact parsing logic from upstream
 */
enum {
    ACL_ALLOW = 0,
    ACL_ALLOW_ALL,
    ACL_DENY,
    ACL_DENY_ALL,
};

typedef struct ACLRule {
    int type;
    char iface[IFNAMSIZ];
    QSIMPLEQ_ENTRY(ACLRule) entry;
} ACLRule;

typedef QSIMPLEQ_HEAD(ACLList, ACLRule) ACLList;

static int parse_acl_file(const char *filename, ACLList *acl_list)
{
    FILE *f;
    char line[4096];
    ACLRule *acl_rule;

    f = fopen(filename, "r");
    if (f == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        char *ptr = line;
        char *cmd, *arg, *argend;

        while (g_ascii_isspace(*ptr)) {
            ptr++;
        }

        /* skip comments and empty lines */
        if (*ptr == '#' || *ptr == 0) {
            continue;
        }

        cmd = ptr;
        arg = strchr(cmd, ' ');
        if (arg == NULL) {
            arg = strchr(cmd, '\t');
        }

        if (arg == NULL) {
            fprintf(stderr, "Invalid config line:\n  %s\n", line);
            goto err;
        }

        *arg = 0;
        arg++;
        while (g_ascii_isspace(*arg)) {
            arg++;
        }

        argend = arg + strlen(arg);
        while (arg != argend && g_ascii_isspace(*(argend - 1))) {
            argend--;
        }
        *argend = 0;

        if (!g_str_equal(cmd, "include") && strlen(arg) >= IFNAMSIZ) {
            fprintf(stderr, "name `%s' too long: %zu\n", arg, strlen(arg));
            goto err;
        }

        if (strcmp(cmd, "deny") == 0) {
            acl_rule = g_malloc(sizeof(*acl_rule));
            if (strcmp(arg, "all") == 0) {
                acl_rule->type = ACL_DENY_ALL;
            } else {
                acl_rule->type = ACL_DENY;
                snprintf(acl_rule->iface, IFNAMSIZ, "%s", arg);
            }
            QSIMPLEQ_INSERT_TAIL(acl_list, acl_rule, entry);
        } else if (strcmp(cmd, "allow") == 0) {
            acl_rule = g_malloc(sizeof(*acl_rule));
            if (strcmp(arg, "all") == 0) {
                acl_rule->type = ACL_ALLOW_ALL;
            } else {
                acl_rule->type = ACL_ALLOW;
                snprintf(acl_rule->iface, IFNAMSIZ, "%s", arg);
            }
            QSIMPLEQ_INSERT_TAIL(acl_list, acl_rule, entry);
        } else if (strcmp(cmd, "include") == 0) {
            /* ignore errors */
            parse_acl_file(arg, acl_list);
        } else {
            fprintf(stderr, "Unknown command `%s'\n", cmd);
            goto err;
        }
    }

    fclose(f);
    return 0;

err:
    fclose(f);
    errno = EINVAL;
    return -1;

}
/* SPDX-SnippetEnd */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

static SD_VARLINK_DEFINE_METHOD(
        CreateTap,
        SD_VARLINK_FIELD_COMMENT("The bridge to add the tap device to. It must be allowed by " ACL_FILE "."),
        SD_VARLINK_DEFINE_INPUT(bridge, SD_VARLINK_STRING, 0),
        SD_VARLINK_FIELD_COMMENT("Whether to enable virtio-net headers (IFF_VNET_HDR). Defaults to false."),
        SD_VARLINK_DEFINE_INPUT(vnetHeader, SD_VARLINK_BOOL, SD_VARLINK_NULLABLE),
        SD_VARLINK_FIELD_COMMENT("The name of the tap device."),
        SD_VARLINK_DEFINE_OUTPUT(interfaceName, SD_VARLINK_STRING, 0),
        SD_VARLINK_FIELD_COMMENT("Index of the tap device's file descriptor among the fds sent with the reply. The device goes away once every copy of it is closed."),
        SD_VARLINK_DEFINE_OUTPUT(fileDescriptor, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_ERROR(
        BridgeNotAllowed,
        SD_VARLINK_DEFINE_FIELD(bridge, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
        NoSuchBridge,
        SD_VARLINK_DEFINE_FIELD(bridge, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_INTERFACE(
        website_soss_QemuBridgeHelper,
        INTERFACE_NAME,
        SD_VARLINK_INTERFACE_COMMENT("Creates tap devices on bridges for unprivileged qemu, as qemu-bridge-helper does."),
        SD_VARLINK_SYMBOL_COMMENT("Creates a tap device, adds it to a bridge, and brings it up."),
        &vl_method_CreateTap,
        SD_VARLINK_SYMBOL_COMMENT("The ACL file doesn't allow the bridge."),
        &vl_error_BridgeNotAllowed,
        SD_VARLINK_SYMBOL_COMMENT("No network interface with the bridge's name exists."),
        &vl_error_NoSuchBridge);

#pragma GCC diagnostic pop

/* Logs "<message>: <strerror(errno)>" and returns -errno */
__attribute__((format(printf, 1, 2)))
static int log_errno(const char *fmt, ...) {
    int err = errno;
    va_list ap;

    fputs(SD_WARNING, stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(err));
    return -err;
}

static int check_acl(const char *bridge) {
    ACLList acl_list;
    ACLRule *acl_rule;
    bool allowed = false, denied = false;
    int r = 0;

    STAILQ_INIT(&acl_list);
    if (parse_acl_file(ACL_FILE, &acl_list) < 0) {
        r = log_errno("failed to parse acl file `%s'", ACL_FILE);
        goto cleanup;
    }

    STAILQ_FOREACH(acl_rule, &acl_list, entry) {
        switch (acl_rule->type) {
            case ACL_ALLOW_ALL:
                allowed = true;
                break;
            case ACL_ALLOW:
                if (strcmp(bridge, acl_rule->iface) == 0)
                    allowed = true;
                break;
            case ACL_DENY_ALL:
                denied = true;
                break;
            case ACL_DENY:
                if (strcmp(bridge, acl_rule->iface) == 0)
                    denied = true;
                break;
        }
    }

    if (!allowed || denied) {
        fprintf(stderr, SD_WARNING "access to bridge `%s' denied by acl file\n", bridge);
        r = -EACCES;
    }

cleanup:
    while ((acl_rule = STAILQ_FIRST(&acl_list)) != NULL) {
        STAILQ_REMOVE_HEAD(&acl_list, entry);
        free(acl_rule);
    }
    return r;
}

/* Creates a tap device on the bridge and returns its fd. */
static int create_tap(const char *bridge, bool use_vnet, char iface[IFNAMSIZ]) {
    _cleanup_close_ int ctlfd = -EBADF;
    _cleanup_close_ int fd = -EBADF;
    struct ifreq tap = {
        .ifr_name = "tap%d",
        .ifr_flags = IFF_TAP | IFF_NO_PI | (use_vnet ? IFF_VNET_HDR : 0),
    };
    struct ifreq br = { 0 };

    snprintf(br.ifr_name, IFNAMSIZ, "%s", bridge);

    ctlfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctlfd < 0)
        return log_errno("failed to open control socket");

    fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return log_errno("failed to open /dev/net/tun");

    if (ioctl(fd, TUNSETIFF, &tap) < 0)
        return log_errno("failed to create tap device");

    snprintf(iface, IFNAMSIZ, "%s", tap.ifr_name);

    /* Set the MTU of the interface based on the bridge */
    if (ioctl(ctlfd, SIOCGIFMTU, &br) < 0)
        return log_errno("failed to get mtu of bridge `%s'", bridge);

    tap.ifr_mtu = br.ifr_mtu;
    if (ioctl(ctlfd, SIOCSIFMTU, &tap) < 0)
        return log_errno("failed to set mtu of device `%s' to %d", iface, tap.ifr_mtu);

    /* Linux uses the lowest enslaved MAC address as the MAC address of the
     * bridge. Set the MAC address to a high value so that it doesn't affect
     * the MAC address of the bridge. */
    if (ioctl(ctlfd, SIOCGIFHWADDR, &tap) < 0)
        return log_errno("failed to get MAC address of device `%s'", iface);

    tap.ifr_hwaddr.sa_data[0] = (char)0xFE;
    if (ioctl(ctlfd, SIOCSIFHWADDR, &tap) < 0)
        return log_errno("failed to set MAC address of device `%s'", iface);

    /* Add the interface to the bridge */
    if (ioctl(ctlfd, SIOCGIFINDEX, &tap) < 0)
        return log_errno("failed to get index of device `%s'", iface);

    br.ifr_ifindex = tap.ifr_ifindex;
    if (ioctl(ctlfd, SIOCBRADDIF, &br) < 0)
        return log_errno("failed to add interface `%s' to bridge `%s'", iface, bridge);

    /* Bring the interface up */
    if (ioctl(ctlfd, SIOCGIFFLAGS, &tap) < 0)
        return log_errno("failed to get interface flags for `%s'", iface);

    tap.ifr_flags |= IFF_UP;
    if (ioctl(ctlfd, SIOCSIFFLAGS, &tap) < 0)
        return log_errno("failed to bring up interface `%s'", iface);

    return TAKE_FD(fd);
}

typedef struct CreateTapParameters {
    const char *bridge;
    bool vnet_header;
} CreateTapParameters;

/* Errors returned as negative errnos are sent as io.systemd.System errors */
static int vl_method_create_tap(sd_varlink *link, sd_json_variant *parameters,
                                sd_varlink_method_flags_t flags, void *userdata) {
    static const sd_json_dispatch_field dispatch_table[] = {
        { "bridge",     SD_JSON_VARIANT_STRING,  sd_json_dispatch_const_string, offsetof(CreateTapParameters, bridge),      SD_JSON_MANDATORY },
        { "vnetHeader", SD_JSON_VARIANT_BOOLEAN, sd_json_dispatch_stdbool,      offsetof(CreateTapParameters, vnet_header), 0                 },
        { NULL }
    };
    _cleanup_close_ int fd = -EBADF;
    CreateTapParameters p = { 0 };
    char iface[IFNAMSIZ];
    uid_t uid;
    int idx, r;
    (void)flags;
    (void)userdata;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    if (p.bridge[0] == 0 || strlen(p.bridge) >= IFNAMSIZ)
        return sd_varlink_error_invalid_parameter_name(link, "bridge");

    r = sd_varlink_get_peer_uid(link, &uid);
    if (r < 0)
        return r;

    fprintf(stderr, SD_INFO "uid %u requested a tap device on bridge %s\n",
            (unsigned)uid, p.bridge);

    r = check_acl(p.bridge);
    if (r == -EACCES)
        return sd_varlink_errorbo(link, ERROR_BRIDGE_NOT_ALLOWED,
                                  SD_JSON_BUILD_PAIR_STRING("bridge", p.bridge));
    if (r < 0)
        return r;

    if (if_nametoindex(p.bridge) == 0) {
        fprintf(stderr, SD_WARNING "bridge `%s' does not exist\n", p.bridge);
        return sd_varlink_errorbo(link, ERROR_NO_SUCH_BRIDGE,
                                  SD_JSON_BUILD_PAIR_STRING("bridge", p.bridge));
    }

    fd = create_tap(p.bridge, p.vnet_header, iface);
    if (fd < 0)
        return fd;

    idx = sd_varlink_push_fd(link, fd);
    if (idx < 0)
        return idx;
    TAKE_FD(fd);

    fprintf(stderr, SD_INFO "added %s to bridge %s\n", iface, p.bridge);

    return sd_varlink_replybo(link,
                              SD_JSON_BUILD_PAIR_STRING("interfaceName", iface),
                              SD_JSON_BUILD_PAIR_INTEGER("fileDescriptor", idx));
}

int main(void) {
    _cleanup_(sd_varlink_server_unrefp) sd_varlink_server *server = NULL;
    int r;

    r = sd_varlink_invocation(SD_VARLINK_ALLOW_ACCEPT);
    if (r <= 0) {
        fprintf(stderr, SD_ERR "must be socket activated with Accept=yes and "
                "FileDescriptorName=varlink%s%s\n",
                r < 0 ? ": " : "", r < 0 ? strerror(-r) : "");
        return EXIT_FAILURE;
    }

    r = sd_varlink_server_new(&server, SD_VARLINK_SERVER_ALLOW_FD_PASSING_OUTPUT);
    if (r >= 0)
        r = sd_varlink_server_add_interface(server, &vl_interface_website_soss_QemuBridgeHelper);
    if (r >= 0)
        r = sd_varlink_server_bind_method(server, METHOD_CREATE_TAP, vl_method_create_tap);
    if (r >= 0)
        r = sd_varlink_server_loop_auto(server);
    if (r < 0) {
        fprintf(stderr, SD_ERR "failed to run varlink server: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
