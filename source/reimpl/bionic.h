#ifndef BTD5_BIONIC_H
#define BTD5_BIONIC_H

#include <stdint.h>

int bionic_fpclassifyd(double value);
int bionic_isfinite(double value);
int bionic_signbit(double value);
int bionic_dladdr(const void *address, void *info);
int bionic_epoll_create(int size);
int bionic_epoll_ctl(int epfd, int operation, int fd, void *event);
int bionic_epoll_wait(int epfd, void *events, int maxevents, int timeout);
int bionic_getpwuid_r(unsigned int uid, void *pwd, char *buffer, unsigned int size,
                      void **result);
unsigned int bionic_if_nametoindex(const char *name);
char *bionic_if_indextoname(unsigned int index, char *name);
int bionic_kill(int pid, int signal);
void *bionic_popen(const char *command, const char *mode);
int bionic_pclose(void *stream);
int bionic_execl(const char *path, const char *arg0, ...);
unsigned int bionic_getuid(void);
unsigned int bionic_geteuid(void);
unsigned int bionic_getgid(void);
unsigned int bionic_getegid(void);
int bionic_pthread_attr_getdetachstate(void *attr, int *detach_state);
int bionic_dup2(int oldfd, int newfd);
void bionic_flockfile(void *stream);
void bionic_funlockfile(void *stream);

#endif
