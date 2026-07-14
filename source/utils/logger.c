/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);
static SceUID _log_file = -2;

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

static void log_file_write_unlocked(const char *message) {
    if (_log_file == -2) {
        _log_file = sceIoOpen(DATA_PATH "loader.log",
                              SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                              0666);
        if (_log_file < 0) {
            /* Some I/O layers accept the device-root slash form only. */
            _log_file = sceIoOpen("ux0:/data/btd5/loader.log",
                                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                                  0666);
        }
        if (_log_file >= 0) {
            sceIoSyncByFd(_log_file, 0);
        } else {
            sceClibPrintf("Persistent loader log open failed: 0x%x\n", _log_file);
        }
    }
    if (_log_file >= 0 && message) {
        sceIoWrite(_log_file, message, strlen(message));
    }
}

void log_start_session(void) {
    log_write_raw("\n=== Bloons TD 5 Vita loader session ===\n");
    log_flush();
}

void log_flush(void) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        return;
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    if (_log_file >= 0) {
        sceIoSyncByFd(_log_file, 0);
    }
    sceKernelUnlockLwMutex(&_log_mutex, 1);
}

void log_write_raw(const char *message) {
    if (!message) {
        return;
    }
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    log_file_write_unlocked(message);
    sceKernelUnlockLwMutex(&_log_mutex, 1);
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf(buffer_b);

    // VitaShell's debug output is not always available on a first test run.
    // Keep a persistent copy next to the user-supplied data so crash reports
    // can be retrieved over USB/FTP afterwards.
    log_file_write_unlocked(buffer_b);

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
