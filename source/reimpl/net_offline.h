/*
 * Offline network posture shims. See net_offline.c.
 */

#ifndef BTD5_NET_OFFLINE_H
#define BTD5_NET_OFFLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int socket_offline(int domain, int type, int protocol);
int getaddrinfo_offline(const char *node, const char *service,
                        const void *hints, void **res);
void freeaddrinfo_offline(void *res);
void *gethostbyname_offline(const char *name);
void *gethostbyaddr_offline(const void *addr, int len, int type);
void *getservbyname_offline(const char *name, const char *proto);
int gethostname_offline(char *name, size_t len);

int connect_offline(int fd, const void *addr, unsigned int len);
int bind_offline(int fd, const void *addr, unsigned int len);
int listen_offline(int fd, int backlog);
int accept_offline(int fd, void *addr, unsigned int *len);
int shutdown_offline(int fd, int how);
int getsockopt_offline(int fd, int level, int name, void *value,
                       unsigned int *len);
int setsockopt_offline(int fd, int level, int name, const void *value,
                       unsigned int len);
int getpeername_offline(int fd, void *addr, unsigned int *len);
int getsockname_offline(int fd, void *addr, unsigned int *len);
long send_offline(int fd, const void *buf, size_t len, int flags);
long sendto_offline(int fd, const void *buf, size_t len, int flags,
                    const void *addr, unsigned int addr_len);
long sendmsg_offline(int fd, const void *msg, int flags);
long recv_offline(int fd, void *buf, size_t len, int flags);
long recvfrom_offline(int fd, void *buf, size_t len, int flags, void *addr,
                      unsigned int *addr_len);
long recvmsg_offline(int fd, void *msg, int flags);

int poll_offline(void *fds, unsigned int nfds, int timeout_ms);
int select_offline(int nfds, void *readfds, void *writefds, void *exceptfds,
                   void *timeout);

unsigned int net_offline_attempts(void);

#ifdef __cplusplus
};
#endif

#endif // BTD5_NET_OFFLINE_H
