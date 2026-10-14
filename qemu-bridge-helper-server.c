/*
 * SPDX-License-Identifier: GPL-2.0-or-later OR MIT
 * SPDX-CopyrightText: 2026 Skye Soss
 */

/*
 * The privileged half of the qemu-bridge-helper replacement. It's socket
 * activated (Accept=yes, one process per connection) with CAP_NET_ADMIN,
 * checks the request against the same ACL file as upstream, creates and
 * configures the tap device, and sends its fd back to the client.
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
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <systemd/sd-daemon.h>

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

static void prep_ifreq(struct ifreq *ifr, const char *ifname) {
    memset(ifr, 0, sizeof(*ifr));
    snprintf(ifr->ifr_name, IFNAMSIZ, "%s", ifname);
}

/* Records why a request failed in the reply and returns -err */
__attribute__((format(printf, 3, 4)))
static int fail(struct reply *reply, int err, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(reply->message, sizeof reply->message, fmt, ap);
    va_end(ap);
    return -err;
}

static int check_acl(const char *bridge, struct reply *reply) {
    ACLList acl_list;
    ACLRule *acl_rule;
    bool allowed = false, denied = false;
    int r = 0;

    /* Re-read on every request, so edits apply without a restart */
    STAILQ_INIT(&acl_list);
    if (parse_acl_file(ACL_FILE, &acl_list) < 0) {
        r = fail(reply, errno, "failed to parse acl file `%s': %s",
                 ACL_FILE, strerror(errno));
        goto cleanup;
    }

    /* Default policy is deny, and deny always wins over allow */
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

    if (!allowed || denied)
        r = fail(reply, EACCES, "access to bridge `%s' denied by acl file", bridge);

cleanup:
    while ((acl_rule = STAILQ_FIRST(&acl_list)) != NULL) {
        STAILQ_REMOVE_HEAD(&acl_list, entry);
        free(acl_rule);
    }
    return r;
}

/* Creates a tap device on the bridge and returns its fd. The tap isn't
 * persistent, so it goes away once every copy of the fd is closed. */
static int create_tap(const char *bridge, bool use_vnet, uid_t owner,
                      struct reply *reply) {
    _cleanup_close_ int ctlfd = -EBADF;
    _cleanup_close_ int fd = -EBADF;
    char *iface = reply->ifname;
    struct ifreq ifr;
    int ifindex;
    int mtu;

    ctlfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctlfd < 0)
        return fail(reply, errno, "failed to open control socket: %s",
                    strerror(errno));

    fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail(reply, errno, "failed to open /dev/net/tun: %s",
                    strerror(errno));

    /* Request a tap device, disable PI, and add vnet header support if
     * requested and it's available */
    prep_ifreq(&ifr, "tap%d");
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (use_vnet) {
        unsigned features = 0;
        if (ioctl(fd, TUNGETFEATURES, &features) >= 0
                && (features & IFF_VNET_HDR)) {
            ifr.ifr_flags |= IFF_VNET_HDR;
        }
    }

    if (ioctl(fd, TUNSETIFF, &ifr) < 0)
        return fail(reply, errno, "failed to create tun device: %s",
                    strerror(errno));

    snprintf(iface, IFNAMSIZ, "%s", ifr.ifr_name);

    /* Record who asked for it, so it shows up in "ip -d link" */
    if (ioctl(fd, TUNSETOWNER, (unsigned long)owner) < 0)
        return fail(reply, errno, "failed to set owner of device `%s': %s",
                    iface, strerror(errno));

    /* Set the MTU of the interface based on the bridge */
    prep_ifreq(&ifr, bridge);
    if (ioctl(ctlfd, SIOCGIFMTU, &ifr) < 0)
        return fail(reply, errno, "failed to get mtu of bridge `%s': %s",
                    bridge, strerror(errno));

    mtu = ifr.ifr_mtu;

    prep_ifreq(&ifr, iface);
    ifr.ifr_mtu = mtu;
    if (ioctl(ctlfd, SIOCSIFMTU, &ifr) < 0)
        return fail(reply, errno, "failed to set mtu of device `%s' to %d: %s",
                    iface, mtu, strerror(errno));

    /* Linux uses the lowest enslaved MAC address as the MAC address of the
     * bridge. Set the MAC address to a high value so that it doesn't affect
     * the MAC address of the bridge. */
    prep_ifreq(&ifr, iface);
    if (ioctl(ctlfd, SIOCGIFHWADDR, &ifr) < 0)
        return fail(reply, errno, "failed to get MAC address of device `%s': %s",
                    iface, strerror(errno));

    ifr.ifr_hwaddr.sa_data[0] = (char)0xFE;
    if (ioctl(ctlfd, SIOCSIFHWADDR, &ifr) < 0)
        return fail(reply, errno, "failed to set MAC address of device `%s': %s",
                    iface, strerror(errno));

    /* Add the interface to the bridge */
    ifindex = if_nametoindex(iface);
    if (ifindex == 0)
        return fail(reply, errno, "failed to get index of device `%s': %s",
                    iface, strerror(errno));

    prep_ifreq(&ifr, bridge);
    ifr.ifr_ifindex = ifindex;
    if (ioctl(ctlfd, SIOCBRADDIF, &ifr) < 0)
        return fail(reply, errno, "failed to add interface `%s' to bridge `%s': %s",
                    iface, bridge, strerror(errno));

    /* Bring the interface up */
    prep_ifreq(&ifr, iface);
    if (ioctl(ctlfd, SIOCGIFFLAGS, &ifr) < 0)
        return fail(reply, errno, "failed to get interface flags for `%s': %s",
                    iface, strerror(errno));

    ifr.ifr_flags |= IFF_UP;
    if (ioctl(ctlfd, SIOCSIFFLAGS, &ifr) < 0)
        return fail(reply, errno, "failed to bring up interface `%s': %s",
                    iface, strerror(errno));

    return TAKE_FD(fd);
}

/* Reads and validates a request, and returns the tap fd for it */
static int handle_request(int c, const struct ucred *peer,
                          struct request *request, struct reply *reply) {
    ssize_t n;
    int r;

    /* MSG_TRUNC makes recv() return the real packet size, so oversized
     * packets are caught too. Any fds the client attached are discarded,
     * since no control buffer is given. */
    n = recv(c, request, sizeof *request, MSG_TRUNC);
    if (n < 0)
        return fail(reply, errno, "failed to read request: %s", strerror(errno));
    if ((size_t)n != sizeof *request || request->magic != PROTO_MAGIC)
        return fail(reply, EBADMSG, "malformed request");
    if (request->flags & ~REQUEST_USE_VNET)
        return fail(reply, EINVAL, "unknown request flags 0x%" PRIx32,
                    request->flags);
    if (memchr(request->bridge, 0, IFNAMSIZ) == NULL || request->bridge[0] == 0)
        return fail(reply, EINVAL, "invalid bridge name");

    r = check_acl(request->bridge, reply);
    if (r < 0)
        return r;

    return create_tap(request->bridge, request->flags & REQUEST_USE_VNET,
                      peer->uid, reply);
}

static int serve(int c) {
    _cleanup_close_ int tapfd = -EBADF;
    struct request request = { 0 };
    struct reply reply = { .magic = PROTO_MAGIC };
    struct ucred peer;
    socklen_t len = sizeof peer;
    int r;

    r = sd_is_socket_unix(c, SOCK_SEQPACKET, /* listening= */ 0, NULL, 0);
    if (r < 0) {
        fprintf(stderr, SD_ERR "failed to check socket: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }
    if (r == 0) {
        fprintf(stderr, SD_ERR "expected a connected AF_UNIX SOCK_SEQPACKET "
                "socket, is the socket unit missing Accept=yes?\n");
        return EXIT_FAILURE;
    }

    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &peer, &len) < 0) {
        fprintf(stderr, SD_ERR "failed to get peer credentials: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    r = set_timeout(c);
    if (r < 0) {
        fprintf(stderr, SD_ERR "failed to set socket timeout: %s\n", strerror(-r));
        return EXIT_FAILURE;
    }

    tapfd = handle_request(c, &peer, &request, &reply);
    if (tapfd < 0) {
        reply.error = -tapfd;
        fprintf(stderr, SD_WARNING "uid %u pid %d: %s\n",
                (unsigned)peer.uid, (int)peer.pid, reply.message);
    } else {
        fprintf(stderr, SD_INFO "uid %u pid %d: added %s to bridge %s\n",
                (unsigned)peer.uid, (int)peer.pid, reply.ifname, request.bridge);
    }

    r = send_packet(c, &reply, sizeof reply, tapfd);
    if (r < 0) {
        fprintf(stderr, SD_ERR "uid %u pid %d: failed to send reply: %s\n",
                (unsigned)peer.uid, (int)peer.pid, strerror(-r));
        return EXIT_FAILURE;
    }

    return tapfd < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(void) {
    int n;

    n = sd_listen_fds(/* unset_environment= */ 1);
    if (n < 0) {
        fprintf(stderr, SD_ERR "failed to get passed fds: %s\n", strerror(-n));
        return EXIT_FAILURE;
    }
    if (n != 1) {
        fprintf(stderr, SD_ERR "expected one connection passed by socket "
                "activation (Accept=yes), got %d fds\n", n);
        return EXIT_FAILURE;
    }

    return serve(SD_LISTEN_FDS_START);
}
