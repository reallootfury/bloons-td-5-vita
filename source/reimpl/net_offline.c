/*
 * Offline network posture for BTD5 on PS Vita: networking and trackers are
 * stripped at the import boundary.
 *
 * The port declares posture (a): offline, local saves only. Without this
 * file the engine's statically linked curl reaches the Vita's real network
 * stack for clock sync (time.ninjakiwi.com), live config
 * (static-api.nkstatic.com) and analytics (analytics.ninjakiwi.com). On
 * hardware with a live Wi-Fi route the boot then stalls at the NinjaKiwi
 * logo; in Vita3K, where every socket fails at once, the same build reaches
 * the main menu.
 *
 * Every socket-family and resolver import resolves here instead of to
 * VitaSDK's SceNet-backed implementations, so the loader imports no SceNet
 * function at all. No descriptor is ever created: socket() and the resolvers
 * refuse, every per-descriptor call fails with EBADF, and select()/poll()
 * with no descriptors keep their documented role as a timed sleep, which
 * curl relies on. Attempts are logged with their caller offset, up to a
 * fixed limit, so the engine's network dependency stays visible.
 *
 * MIT license, same terms as the surrounding project.
 */

#include "reimpl/net_offline.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

/* bionic <netdb.h> / <poll.h> / <sys/select.h> values and layouts. The
 * game passes and compares bionic structures and constants. */
#define BIONIC_EAI_FAIL 4
#define BIONIC_POLLNVAL 0x20
#define BIONIC_FD_SETSIZE 1024
#define NET_OFFLINE_LOG_LIMIT 32u
#define NET_OFFLINE_MAX_SLEEP_MS 60000

typedef struct BionicPollFd {
    int fd;
    short events;
    short revents;
} BionicPollFd;

typedef struct BionicTimeval {
    long tv_sec;
    long tv_usec;
} BionicTimeval;

static atomic_uint net_offline_count = ATOMIC_VAR_INIT(0);

static unsigned int net_caller_offset(const void *address) {
    uintptr_t value = (uintptr_t)address;
    if (value >= so_mod.text_base &&
        value < so_mod.text_base + so_mod.text_size) {
        return (unsigned int)(value - so_mod.text_base);
    }
    return 0xffffffffu;
}

static int net_offline_should_log(void) {
    unsigned int seen = atomic_fetch_add(&net_offline_count, 1u) + 1u;
    if (seen == NET_OFFLINE_LOG_LIMIT + 1u) {
        l_warn("Offline posture: further network attempts are refused "
               "silently (%u logged).", NET_OFFLINE_LOG_LIMIT);
    }
    return seen <= NET_OFFLINE_LOG_LIMIT;
}

static void net_sleep_ms(long ms) {
    if (ms <= 0) {
        return;
    }
    if (ms > NET_OFFLINE_MAX_SLEEP_MS) {
        ms = NET_OFFLINE_MAX_SLEEP_MS;
    }
    sceKernelDelayThread((SceUInt)ms * 1000u);
}

static int net_ebadf(void) {
    errno = EBADF;
    return -1;
}

unsigned int net_offline_attempts(void) {
    return atomic_load(&net_offline_count);
}

/* ---- refusal points: the only ways a network descriptor could exist ---- */

int socket_offline(int domain, int type, int protocol) {
    if (net_offline_should_log()) {
        l_warn("Offline posture: socket(%d, %d, %d) from SO+0x%08x refused.",
               domain, type, protocol,
               net_caller_offset(__builtin_return_address(0)));
    }
    errno = EACCES;
    return -1;
}

int getaddrinfo_offline(const char *node, const char *service,
                        const void *hints, void **res) {
    (void)hints;
    if (res != NULL) {
        *res = NULL;
    }
    if (net_offline_should_log()) {
        l_warn("Offline posture: getaddrinfo(%s, %s) from SO+0x%08x refused.",
               node != NULL ? node : "(null)",
               service != NULL ? service : "(null)",
               net_caller_offset(__builtin_return_address(0)));
    }
    return BIONIC_EAI_FAIL;
}

void freeaddrinfo_offline(void *res) {
    /* getaddrinfo_offline never allocates, so there is nothing to free. */
    (void)res;
}

void *gethostbyname_offline(const char *name) {
    if (net_offline_should_log()) {
        l_warn("Offline posture: gethostbyname(%s) from SO+0x%08x refused.",
               name != NULL ? name : "(null)",
               net_caller_offset(__builtin_return_address(0)));
    }
    return NULL;
}

void *gethostbyaddr_offline(const void *addr, int len, int type) {
    (void)addr;
    if (net_offline_should_log()) {
        l_warn("Offline posture: gethostbyaddr(len=%d, type=%d) from "
               "SO+0x%08x refused.", len, type,
               net_caller_offset(__builtin_return_address(0)));
    }
    return NULL;
}

void *getservbyname_offline(const char *name, const char *proto) {
    (void)name;
    (void)proto;
    return NULL;
}

int gethostname_offline(char *name, size_t len) {
    static const char host[] = "localhost";
    if (name == NULL || len < sizeof(host)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, host, sizeof(host));
    return 0;
}

/* ---- per-descriptor calls: no network descriptor exists, so EBADF ---- */

int connect_offline(int fd, const void *addr, unsigned int len) {
    (void)fd; (void)addr; (void)len;
    return net_ebadf();
}

int bind_offline(int fd, const void *addr, unsigned int len) {
    (void)fd; (void)addr; (void)len;
    return net_ebadf();
}

int listen_offline(int fd, int backlog) {
    (void)fd; (void)backlog;
    return net_ebadf();
}

int accept_offline(int fd, void *addr, unsigned int *len) {
    (void)fd; (void)addr; (void)len;
    return net_ebadf();
}

int shutdown_offline(int fd, int how) {
    (void)fd; (void)how;
    return net_ebadf();
}

int getsockopt_offline(int fd, int level, int name, void *value,
                       unsigned int *len) {
    (void)fd; (void)level; (void)name; (void)value; (void)len;
    return net_ebadf();
}

int setsockopt_offline(int fd, int level, int name, const void *value,
                       unsigned int len) {
    (void)fd; (void)level; (void)name; (void)value; (void)len;
    return net_ebadf();
}

int getpeername_offline(int fd, void *addr, unsigned int *len) {
    (void)fd; (void)addr; (void)len;
    return net_ebadf();
}

int getsockname_offline(int fd, void *addr, unsigned int *len) {
    (void)fd; (void)addr; (void)len;
    return net_ebadf();
}

long send_offline(int fd, const void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return net_ebadf();
}

long sendto_offline(int fd, const void *buf, size_t len, int flags,
                    const void *addr, unsigned int addr_len) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addr_len;
    return net_ebadf();
}

long sendmsg_offline(int fd, const void *msg, int flags) {
    (void)fd; (void)msg; (void)flags;
    return net_ebadf();
}

long recv_offline(int fd, void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    return net_ebadf();
}

long recvfrom_offline(int fd, void *buf, size_t len, int flags, void *addr,
                      unsigned int *addr_len) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addr_len;
    return net_ebadf();
}

long recvmsg_offline(int fd, void *msg, int flags) {
    (void)fd; (void)msg; (void)flags;
    return net_ebadf();
}

/* ---- readiness waits: with no descriptors they are timed sleeps ---- */

int poll_offline(void *fds, unsigned int nfds, int timeout_ms) {
    BionicPollFd *entries = (BionicPollFd *)fds;
    if (nfds == 0u) {
        if (timeout_ms < 0) {
            /* An untimed wait on nothing would never return. */
            errno = EINVAL;
            return -1;
        }
        net_sleep_ms(timeout_ms);
        return 0;
    }
    if (entries == NULL || nfds > BIONIC_FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned int i = 0; i < nfds; ++i) {
        entries[i].revents = entries[i].fd < 0 ? 0 : BIONIC_POLLNVAL;
    }
    return (int)nfds;
}

static int fd_set_is_empty(const void *set, int nfds) {
    const uint8_t *bits = (const uint8_t *)set;
    if (bits == NULL) {
        return 1;
    }
    for (int fd = 0; fd < nfds; ++fd) {
        if (bits[fd / 8] & (uint8_t)(1u << (fd % 8))) {
            return 0;
        }
    }
    return 1;
}

int select_offline(int nfds, void *readfds, void *writefds, void *exceptfds,
                   void *timeout) {
    const BionicTimeval *tv = (const BionicTimeval *)timeout;
    if (nfds < 0 || nfds > BIONIC_FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    if (!fd_set_is_empty(readfds, nfds) || !fd_set_is_empty(writefds, nfds) ||
        !fd_set_is_empty(exceptfds, nfds)) {
        return net_ebadf();
    }
    if (tv == NULL) {
        errno = EINVAL;
        return -1;
    }
    net_sleep_ms(tv->tv_sec * 1000L + tv->tv_usec / 1000L);
    return 0;
}
