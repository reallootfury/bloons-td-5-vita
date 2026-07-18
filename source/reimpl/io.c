/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "game.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#define PROFILE_WRITE_STREAM_SLOTS 8

static atomic_uintptr_t profile_write_streams[PROFILE_WRITE_STREAM_SLOTS];
static atomic_uint profile_generation = ATOMIC_VAR_INIT(0);

static bool profile_mode_is_writable(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') ||
                    strchr(mode, '+'));
}

static bool is_primary_profile_path(const char *filename) {
    if (!filename) return false;
    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;
    return strcmp(basename, "Profile.save") == 0;
}

static void track_profile_write_stream(FILE *stream) {
    if (!stream) return;

    for (size_t i = 0; i < PROFILE_WRITE_STREAM_SLOTS; ++i) {
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &profile_write_streams[i], &expected, (uintptr_t)stream,
                memory_order_release, memory_order_relaxed)) {
            return;
        }
    }

    l_warn("Profile write stream table is full; this save will use the next "
           "system-UI storage sync.");
}

static bool untrack_profile_write_stream(FILE *stream) {
    if (!stream) return false;

    for (size_t i = 0; i < PROFILE_WRITE_STREAM_SLOTS; ++i) {
        uintptr_t expected = (uintptr_t)stream;
        if (atomic_compare_exchange_strong_explicit(
                &profile_write_streams[i], &expected, 0,
                memory_order_acq_rel, memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

uint32_t profile_save_generation(void) {
    return atomic_load_explicit(&profile_generation, memory_order_acquire);
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    bool profile_file = strstr(filename, "Profile.save") != NULL;
    struct stat profile_stat;
    int profile_stat_result = profile_file ? stat(filename, &profile_stat) : -1;

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret && is_primary_profile_path(filename) &&
        profile_mode_is_writable(mode)) {
        track_profile_write_stream(ret);
    }

    if (profile_file) {
        if (ret) {
            l_info("Profile file opened: %s (%s, previous_size=%lld).",
                   filename, mode,
                   profile_stat_result == 0
                       ? (long long)profile_stat.st_size : -1LL);
        } else {
            l_error("Profile file could not be opened: %s (%s).",
                    filename, mode);
        }
        log_flush();
    }

#ifdef DEBUG_IO_VERBOSE
    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);
#endif

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    int bionic_oflag = oflag;
    bool profile_file = strstr(path, "Profile.save") != NULL;
    struct stat profile_stat;
    int profile_stat_result = profile_file ? stat(path, &profile_stat) : -1;
    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (profile_file) {
        if (ret >= 0) {
            l_info("Profile file descriptor opened: %s "
                   "(flags=0x%x, previous_size=%lld).",
                   path, bionic_oflag,
                   profile_stat_result == 0
                       ? (long long)profile_stat.st_size : -1LL);
        } else {
            l_error("Profile file descriptor failed: %s (flags=0x%x).",
                    path, bionic_oflag);
        }
        log_flush();
    }
#ifdef DEBUG_IO_VERBOSE
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
#endif
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

#ifdef DEBUG_IO_VERBOSE
    l_debug("fstat(%i): %i", fd, res);
#endif
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

#ifdef DEBUG_IO_VERBOSE
    l_debug("stat(%s): %i", path, res);
#endif
    return res;
}

int fclose_soloader(FILE * f) {
    bool completed_profile_write = untrack_profile_write_stream(f);
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    if (completed_profile_write && ret == 0) {
        atomic_fetch_add_explicit(&profile_generation, 1,
                                  memory_order_release);
    }

#ifdef DEBUG_IO_VERBOSE
    l_debug("fclose(%p): %i", f, ret);
#endif
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
#ifdef DEBUG_IO_VERBOSE
    l_debug("close(%i): %i", fd, ret);
#endif
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
#ifdef DEBUG_IO_VERBOSE
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
#endif
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
#ifdef DEBUG_IO_VERBOSE
    l_debug("readdir(%p): %p", dir, ret);
#endif

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

#ifdef DEBUG_IO_VERBOSE
    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
#endif
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
#ifdef DEBUG_IO_VERBOSE
    l_debug("closedir(%p): %i", dir, ret);
#endif
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
#ifdef DEBUG_IO_VERBOSE
    l_debug("fsync(%i): %i", fd, ret);
#endif
    return ret;
}
