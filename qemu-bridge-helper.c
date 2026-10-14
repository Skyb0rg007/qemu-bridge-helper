/*
 * SPDX-License-Identifier: GPL-2.0-or-later OR MIT
 * SPDX-CopyrightText: 2026 Skye Soss
 */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <sys/queue.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/if_bridge.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* systemd-style helpers */
static void closep(int *fd) {
    if (*fd >= 0)
        close(*fd);
}

#define _cleanup_close_ __attribute__((cleanup(closep)))
#define TAKE_FD(var) __extension__ ({ \
        int *_pvar_ = &(var); \
        int _var_ = *_pvar_; \
        *_pvar_ = -EBADF; \
        _var_; })

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

struct settings {
    bool use_vnet;
    const char *bridge;
    int unixfd;
};

static int parse_arguments(struct settings *settings, char *args[]) {
    bool use_vnet = 0;
    const char *bridge = NULL;
    int unixfd = -EBADF;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "--use-vnet") == 0) {
            use_vnet = true;
        } else if (strncmp(args[i], "--br=", 5) == 0) {
            bridge = &args[i][5];
        } else if (strncmp(args[i], "--fd=", 5) == 0) {
            unixfd = atoi(&args[i][5]);
        } else {
            return -1;
        }
    }

    if (bridge == NULL || unixfd == -EBADF) {
        return -1;
    }

    fprintf(stderr, "use_vnet: %s\n", use_vnet ? "true" : "false");
    fprintf(stderr, "bridge: %s\n", bridge);
    fprintf(stderr, "unixfd: %d\n", unixfd);

    settings->use_vnet = use_vnet;
    settings->bridge = bridge;
    settings->unixfd = unixfd;
    return 0;
}

static int _main(int argc, char *argv[]) {
    int r;
    (void)argc;

    fprintf(stderr, "Starting\n");

    struct settings settings;
    r = parse_arguments(&settings, &argv[1]);
    if (r < 0) {
        fprintf(stderr, "invalid argument\n");
        return 1;
    }

    ACLRule *acl_rule;
    const char *acl_file = "/etc/qemu/bridge.conf";

    fprintf(stderr, "acl_file: %s\n", acl_file);

    ACLList acl_list;
    STAILQ_INIT(&acl_list);
    r = parse_acl_file(acl_file, &acl_list);
    if (r < 0) {
        fprintf(stderr, "failed to parse %s\n", acl_file);
        return EXIT_FAILURE;
    }

    STAILQ_FOREACH(acl_rule, &acl_list, entry) {
        switch (acl_rule->type) {
            case ACL_ALLOW_ALL:
                fprintf(stderr, "allow all\n");
                break;
            case ACL_ALLOW:
                fprintf(stderr, "allow %s\n", acl_rule->iface);
                break;
            case ACL_DENY_ALL:
                fprintf(stderr, "deny all\n");
                break;
            case ACL_DENY:
                fprintf(stderr, "deny %s\n", acl_rule->iface);
                break;
        }
    }

    while ((acl_rule = STAILQ_FIRST(&acl_list)) != NULL) {
        STAILQ_REMOVE_HEAD(&acl_list, entry);
        free(acl_rule);
    }

    return EXIT_SUCCESS;
}

static int perform(const struct settings *settings) {
    _cleanup_close_ int ctlfd = -EBADF;
    _cleanup_close_ int fd = -EBADF;
    int mtu;
    struct ifreq ifr;
    char iface[IFNAMSIZ];
    int r;

    ctlfd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctlfd < 0) {
        fprintf(stderr, "failed to open control socket: %s\n", strerror(errno));
        return -errno;
    }

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "failed to open /dev/net/tun: %s\n", strerror(errno));
        return -errno;
    }

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", "tap%d");
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (settings->use_vnet) {
        unsigned features = 0;
        if (ioctl(fd, TUNGETFEATURES, &features) >= 0
                && (features & IFF_VNET_HDR)) {
            ifr.ifr_flags |= IFF_VNET_HDR;
        }
    }

    r = ioctl(fd, TUNSETIFF, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to create tun device: %s\n", strerror(errno));
        return -errno;
    }

    snprintf(iface, sizeof iface, "%s", ifr.ifr_name);
    fprintf(stderr, "tap device: %s\n", ifr.ifr_name);

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", settings->bridge);
    r = ioctl(ctlfd, SIOCGIFMTU, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to get mtu of bridge `%s': %s\n",
                settings->bridge, strerror(errno));
        return -errno;
    }

    mtu = ifr.ifr_mtu;
    fprintf(stderr, "mtu: %d\n", mtu);

    /* Set the MTU of the interface based on the bridge */
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", settings->bridge);
    ifr.ifr_mtu = mtu;
    r = ioctl(fd, SIOCSIFMTU, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to set mtu of device `%s': %s\n",
                iface, strerror(errno));
        return -errno;
    }

    /* Set the MAC address to a high address */
    r = ioctl(ctlfd, SIOCGIFHWADDR, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to get MAC address of device `%s': %s\n",
                iface, strerror(errno));
        return -errno;
    }

    ifr.ifr_hwaddr.sa_data[0] = 0xFE;
    r = ioctl(ctlfd, SIOCSIFHWADDR, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to set MAC address of device `%s': %s\n",
                iface, strerror(errno));
        return -errno;
    }

    /* Add interface to the bridge */
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", settings->bridge);
    ifr.ifr_ifindex = if_nametoindex(iface);
    r = ioctl(ctlfd, SIOCBRADDIF, &ifr);
    if (r < 0) {
        fprintf(stderr, "failed to add interface `%s' to bridge `%s': %s\n",
                iface, settings->bridge, strerror(errno));
        return -errno;
    }

    /* Bring interface up */
}

int main(int argc, char *argv[]) {
    return _main(argc, argv);
}
