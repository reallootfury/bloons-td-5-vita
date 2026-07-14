#include "reimpl/bionic.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>

int bionic_fpclassifyd(double value) { return fpclassify(value); }
int bionic_isfinite(double value) { return isfinite(value); }
int bionic_signbit(double value) { return signbit(value); }

/*
 * These APIs are reached by bundled networking/telemetry libraries. Vita's
 * loader runs the offline game without spawning processes or opening Android
 * network watchers; returning the conventional unsupported result lets those
 * libraries use their normal fallback path.
 */
int bionic_dladdr(const void *address, void *info) {
    (void)address; (void)info;
    return 0;
}

int bionic_epoll_create(int size) {
    (void)size;
    errno = ENOSYS;
    return -1;
}

int bionic_epoll_ctl(int epfd, int operation, int fd, void *event) {
    (void)epfd; (void)operation; (void)fd; (void)event;
    errno = ENOSYS;
    return -1;
}

int bionic_epoll_wait(int epfd, void *events, int maxevents, int timeout) {
    (void)epfd; (void)events; (void)maxevents; (void)timeout;
    errno = ENOSYS;
    return -1;
}

int bionic_getpwuid_r(unsigned int uid, void *pwd, char *buffer, unsigned int size,
                      void **result) {
    (void)uid; (void)pwd; (void)buffer; (void)size;
    if (result) *result = NULL;
    return ENOENT;
}

unsigned int bionic_if_nametoindex(const char *name) {
    (void)name;
    return 0;
}

char *bionic_if_indextoname(unsigned int index, char *name) {
    (void)index;
    (void)name;
    errno = ENODEV;
    return NULL;
}

int bionic_kill(int pid, int signal) {
    (void)pid; (void)signal;
    errno = ENOSYS;
    return -1;
}

void *bionic_popen(const char *command, const char *mode) {
    (void)command; (void)mode;
    errno = ENOSYS;
    return NULL;
}

int bionic_pclose(void *stream) {
    (void)stream;
    errno = ENOSYS;
    return -1;
}

int bionic_execl(const char *path, const char *arg0, ...) {
    (void)path; (void)arg0;
    errno = ENOSYS;
    return -1;
}

unsigned int bionic_getuid(void) { return 0; }
unsigned int bionic_geteuid(void) { return 0; }
unsigned int bionic_getgid(void) { return 0; }
unsigned int bionic_getegid(void) { return 0; }

int bionic_pthread_attr_getdetachstate(void *attr, int *detach_state) {
    (void)attr;
    if (detach_state) *detach_state = 0; /* PTHREAD_CREATE_JOINABLE */
    return 0;
}

int bionic_dup2(int oldfd, int newfd) {
    (void)oldfd; (void)newfd;
    errno = ENOSYS;
    return -1;
}

void bionic_flockfile(void *stream) { (void)stream; }
void bionic_funlockfile(void *stream) { (void)stream; }
