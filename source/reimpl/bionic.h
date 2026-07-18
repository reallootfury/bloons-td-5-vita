#ifndef BTD5_BIONIC_H
#define BTD5_BIONIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

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
void bionic_FD_SET_chk(int fd, void *set, size_t set_size);
size_t bionic_strlen_chk(const char *value, size_t bound);
int bionic_vsnprintf_chk(char *output, size_t output_size, int flags,
                         size_t destination_size, const char *format,
                         va_list args);
int bionic_epoll_create1(int flags);
int bionic_eventfd(unsigned int initial_value, int flags);
int bionic_link(const char *old_path, const char *new_path);
int bionic_posix_memalign(void **result, size_t alignment, size_t size);
ssize_t bionic_readlink(const char *path, char *buffer, size_t size);
int bionic_statfs(const char *path, void *buffer);
long double bionic_strtold_l(const char *value, char **end, void *locale);
long long bionic_strtoll_l(const char *value, char **end, int base, void *locale);
unsigned long long bionic_strtoull_l(const char *value, char **end, int base,
                                     void *locale);
int bionic_symlink(const char *target, const char *link_path);

#endif
