/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"
#include "diagnostics.h"
#include "reimpl/errno.h"
#include "reimpl/bits/_errno_bionic.h"

#include <so_util/so_util.h>

#define PTHR_MAX_OBJECTS 1024
#define PTHR_MAX_THREADS 32
#define BTD5_ANDROID_WORKER_STACK (256U * 1024U)

extern so_module so_mod;

static atomic_uintptr_t watched_cond = ATOMIC_VAR_INIT(0);
static atomic_uint condition_poll_wakeups = ATOMIC_VAR_INIT(0);
static atomic_uint android_threads_created = ATOMIC_VAR_INIT(0);
static atomic_uint android_worker_affinity_count = ATOMIC_VAR_INIT(0);

/* Let the Vita scheduler distribute Android-created workers over all three
 * supported user cores. Hard-pinning the supplied late-game run to a single
 * main core plus cores 1/2/3 for workers increased contention and stutter. */
#define BTD5_ANDROID_WORKER_CPU_MASK SCE_KERNEL_CPU_MASK_USER_ALL
static atomic_int android_worker_cpu_mask =
    ATOMIC_VAR_INIT(BTD5_ANDROID_WORKER_CPU_MASK);
static atomic_uint android_semaphores_created = ATOMIC_VAR_INIT(0);
static atomic_int android_thread_ids[PTHR_MAX_THREADS];
static atomic_uintptr_t android_thread_starts[PTHR_MAX_THREADS];
static atomic_uintptr_t android_thread_tasks[PTHR_MAX_THREADS];
static atomic_uintptr_t android_thread_invokes[PTHR_MAX_THREADS];
static atomic_uintptr_t android_thread_sync_callers[PTHR_MAX_THREADS];
static atomic_int android_thread_sync_ops[PTHR_MAX_THREADS];
static _Thread_local int android_thread_slot = -1;
static _Thread_local bool barrier_wait_logged = false;

enum {
    PTHR_SYNC_NONE = 0,
    PTHR_SYNC_MUTEX_LOCK,
    PTHR_SYNC_COND_WAIT,
    PTHR_SYNC_COND_TIMEDWAIT,
    PTHR_SYNC_JOIN,
    PTHR_SYNC_SEM_WAIT,
    PTHR_SYNC_SEM_TIMEDWAIT,
};

typedef struct PthrStartContext {
    void *(*start)(void *);
    void *param;
} PthrStartContext;

static unsigned int game_address_offset(const void *address) {
    uintptr_t value = (uintptr_t)address;
    if (value >= so_mod.text_base &&
        value < so_mod.text_base + so_mod.text_size) {
        return (unsigned int)(value - so_mod.text_base);
    }
    return 0xffffffffu;
}

static uintptr_t boost_thread_task(void *start, void *param,
                                   uintptr_t *invoke) {
    *invoke = 0;
    unsigned int start_offset = game_address_offset(start);
    if (!param || (start_offset != 0x007ba561u &&
                   start_offset != 0x007408ddu)) {
        return 0;
    }

    /* Both pinned executables use Boost 1.x's pthread entry wrapper. Its
     * argument stores thread_data_base at +4; the virtual run() target is the
     * third vtable entry.  Recording it turns the otherwise generic Boost
     * address into the game's actual loading task in watchdog logs. */
    void *thread_data = *(void **)((char *)param + 4);
    if (!thread_data) return 0;
    void **vtable = *(void ***)thread_data;
    if (!vtable) return 0;
    uintptr_t task = (uintptr_t)vtable[2];

    /* BTD5 4.7's CApp Boost bind wrapper dispatches the actual loading
     * function stored at +104. Keep both addresses: task describes the Boost
     * type, while invoke identifies the game subsystem doing the work. */
    if (game_address_offset((void *)task) == 0x004de153u) {
        *invoke = *(uintptr_t *)((char *)thread_data + 104);
    }
    return task;
}

static void worker_sync_begin(int op, const void *caller) {
    if (android_thread_slot < 0) return;
    atomic_store_explicit(&android_thread_sync_callers[android_thread_slot],
                          (uintptr_t)caller, memory_order_relaxed);
    atomic_store_explicit(&android_thread_sync_ops[android_thread_slot], op,
                          memory_order_release);
}

static void worker_sync_end(void) {
    if (android_thread_slot < 0) return;
    atomic_store_explicit(&android_thread_sync_ops[android_thread_slot],
                          PTHR_SYNC_NONE, memory_order_release);
}

static const char *worker_sync_name(int op) {
    switch (op) {
    case PTHR_SYNC_MUTEX_LOCK: return "mutex_lock";
    case PTHR_SYNC_COND_WAIT: return "cond_wait";
    case PTHR_SYNC_COND_TIMEDWAIT: return "cond_timedwait";
    case PTHR_SYNC_JOIN: return "join";
    case PTHR_SYNC_SEM_WAIT: return "sem_wait";
    case PTHR_SYNC_SEM_TIMEDWAIT: return "sem_timedwait";
    default: return "none";
    }
}

void pthr_set_android_worker_cpu_mask(int mask) {
    if ((mask & SCE_KERNEL_CPU_MASK_USER_ALL) == 0) {
        mask = BTD5_ANDROID_WORKER_CPU_MASK;
    }
    atomic_store_explicit(&android_worker_cpu_mask, mask, memory_order_release);
}

static void *pthread_start_trampoline(void *opaque) {
    PthrStartContext context = *(PthrStartContext *)opaque;
    free(opaque);

    uintptr_t invoke = 0;
    uintptr_t task = boost_thread_task((void *)context.start, context.param,
                                       &invoke);

    int thread_id = sceKernelGetThreadId();
    int worker_cpu_mask = atomic_load_explicit(
        &android_worker_cpu_mask, memory_order_acquire);
    int affinity_result = sceKernelChangeThreadCpuAffinityMask(
        thread_id, worker_cpu_mask);
    unsigned int affinity_sequence = atomic_fetch_add_explicit(
        &android_worker_affinity_count, 1, memory_order_relaxed) + 1;
    if (affinity_sequence <= 16 || affinity_result < 0) {
        if (affinity_result < 0) {
            l_warn("Could not assign Android worker 0x%x to CPU mask "
                   "0x%08x: 0x%08x.", thread_id,
                   (unsigned int)worker_cpu_mask,
                   (unsigned int)affinity_result);
        } else {
            l_info("Android worker 0x%x assigned to CPU mask 0x%08x.",
                   thread_id, (unsigned int)worker_cpu_mask);
        }
    }
    int slot = -1;
    for (int i = 0; i < PTHR_MAX_THREADS; ++i) {
        int empty = 0;
        if (atomic_compare_exchange_strong_explicit(
                &android_thread_ids[i], &empty, thread_id,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&android_thread_starts[i],
                                  (uintptr_t)context.start,
                                  memory_order_release);
            atomic_store_explicit(&android_thread_tasks[i], task,
                                  memory_order_release);
            atomic_store_explicit(&android_thread_invokes[i], invoke,
                                  memory_order_release);
            slot = i;
            android_thread_slot = i;
            break;
        }
    }

    void *result = context.start(context.param);
    if (slot >= 0) {
        atomic_store_explicit(&android_thread_starts[slot], 0,
                              memory_order_relaxed);
        atomic_store_explicit(&android_thread_tasks[slot], 0,
                              memory_order_relaxed);
        atomic_store_explicit(&android_thread_invokes[slot], 0,
                              memory_order_relaxed);
        atomic_store_explicit(&android_thread_sync_ops[slot], PTHR_SYNC_NONE,
                              memory_order_relaxed);
        atomic_store_explicit(&android_thread_ids[slot], 0,
                              memory_order_release);
    }
    return result;
}

void pthr_diag_log_threads(void) {
    for (int i = 0; i < PTHR_MAX_THREADS; ++i) {
        int thread_id = atomic_load_explicit(&android_thread_ids[i],
                                             memory_order_acquire);
        if (thread_id <= 0) {
            continue;
        }

        SceKernelThreadInfo info = {0};
        info.size = sizeof(info);
        int ret = sceKernelGetThreadInfo(thread_id, &info);
        unsigned int start_offset = game_address_offset((void *)
            atomic_load_explicit(&android_thread_starts[i],
                                 memory_order_acquire));
        unsigned int task_offset = game_address_offset((void *)
            atomic_load_explicit(&android_thread_tasks[i],
                                 memory_order_acquire));
        unsigned int invoke_offset = game_address_offset((void *)
            atomic_load_explicit(&android_thread_invokes[i],
                                 memory_order_acquire));
        int sync_op = atomic_load_explicit(&android_thread_sync_ops[i],
                                           memory_order_acquire);
        unsigned int sync_caller = game_address_offset((void *)
            atomic_load_explicit(&android_thread_sync_callers[i],
                                 memory_order_relaxed));
        if (ret == 0) {
            l_warn("Android worker %s (0x%x, start SO+0x%08x, task "
                   "SO+0x%08x, invoke SO+0x%08x): status "
                   "0x%x, wait type %u, wait id 0x%x, CPU %d.",
                   info.name, thread_id, start_offset, task_offset,
                   invoke_offset, info.status, info.waitType, info.waitId,
                   info.currentCpuId);
            if (sync_op != PTHR_SYNC_NONE) {
                l_warn("Android worker active sync: %s from SO+0x%08x.",
                       worker_sync_name(sync_op), sync_caller);
            }
        } else {
            l_warn("Could not inspect Android worker 0x%x (start "
                   "SO+0x%08x, task SO+0x%08x, invoke SO+0x%08x): 0x%x.",
                   thread_id, start_offset, task_offset, invoke_offset, ret);
        }
    }
}

static bool cond_wait_watch_begin(pthread_cond_t_bionic *cond,
                                  pthread_mutex_t_bionic *mutex,
                                  const void *caller, bool timed) {
    if (!btd5_diag_is_current_thread()) {
        return false;
    }

    atomic_store_explicit(&watched_cond, (uintptr_t)cond,
                          memory_order_release);
#ifdef DEBUG_COND_VERBOSE
    l_warn("Main thread entering pthread_cond_%swait from SO+0x%08x "
           "(cond %p -> %p, mutex %p -> %p).",
           timed ? "timed" : "", game_address_offset(caller), cond,
           cond->real_ptr, mutex, mutex->real_ptr);
#endif
    return true;
}

static void cond_wait_watch_end(bool watched, pthread_cond_t_bionic *cond,
                                const void *caller, int ret) {
    if (!watched) {
        return;
    }
#ifdef DEBUG_COND_VERBOSE
    l_warn("Main thread pthread_cond_wait from SO+0x%08x returned %d.",
           game_address_offset(caller), ret);
#else
    (void)caller;
    (void)ret;
#endif
    uintptr_t expected = (uintptr_t)cond;
    atomic_compare_exchange_strong_explicit(
        &watched_cond, &expected, 0, memory_order_acq_rel,
        memory_order_relaxed);
}

static bool cond_is_watched(const pthread_cond_t_bionic *cond) {
    return atomic_load_explicit(&watched_cond, memory_order_acquire) ==
           (uintptr_t)cond;
}

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

void * initializedObjects[PTHR_MAX_OBJECTS] = {0};
static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

int pthr_bridge_init(void) {
    if (pthr_mutex_inited) {
        return 0;
    }
    int ret = sceKernelCreateLwMutex(&pthr_mutex, "bionic_pthr_registry",
                                     0, 0, NULL);
    if (ret < 0) {
        l_error("Failed to create pthread bridge mutex: 0x%x", ret);
        return ret;
    }
    pthr_mutex_inited = 1;
    return 0;
}

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = pthr_bridge_init(); \
        if (ret < 0) { \
            return 0; \
        } \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

static int isObjectInitializedUnlocked(const void *mut) {
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) {
            return 1;
        }
    }
    return 0;
}

static int rememberObjectUnlocked(void *mut) {
    if (isObjectInitializedUnlocked(mut)) {
        return 1;
    }
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == NULL) {
            initializedObjects[i] = mut;
            return 1;
        }
    }
    return 0;
}

static int forgetObjectUnlocked(const void *mut) {
    int found = 0;
    for (int i = 0; i < PTHR_MAX_OBJECTS; ++i) {
        if (initializedObjects[i] == mut) {
            initializedObjects[i] = NULL;
            found = 1;
        }
    }
    return found;
}

int isObjectInitialized(const void * mut) {
    PTHR_LOCK
    int result = isObjectInitializedUnlocked(mut);
    PTHR_UNLOCK
    return result;
}

int rememberObject(void * mut) {
    PTHR_LOCK
    int result = rememberObjectUnlocked(mut);
    PTHR_UNLOCK
    return result;
}

int forgetObject(const void * mut) {
    PTHR_LOCK
    int result = forgetObjectUnlocked(mut);
    PTHR_UNLOCK
    return result;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->stack_size = 0;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    /* Serialize lookup, publication, native initialization and registration.
     * Publishing mutex->real_ptr before pthread_mutex_init completes lets a
     * second Android thread observe a wrapper whose inner pointer is NULL. */
    PTHR_LOCK
    if (isObjectInitializedUnlocked(mutex)) {
        PTHR_UNLOCK
        return ret;
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    /* pthread_mutex_t is a pointer in Vita's pthreads-embedded. It must start
     * as NULL; copying an uninitialized local pointer here can make the first
     * lock perform LDREX on an arbitrary (and possibly unaligned) address. */
    pthread_mutex_t *real_ptr = calloc(1, sizeof(pthread_mutex_t));
    if (!real_ptr) {
        PTHR_UNLOCK
        return ENOMEM;
    }

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(real_ptr, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        if (!rememberObjectUnlocked(mutex)) {
            pthread_mutex_destroy(real_ptr);
            free(real_ptr);
            ret = ENOMEM;
        } else {
            /* Publish only after native initialization is complete. */
            mutex->real_ptr = real_ptr;
        }
    } else {
        l_error("mutex initialization for %p has failed", mutex);
        free(real_ptr);
    }

    PTHR_UNLOCK
    return ret;
}

/* Once a Bionic mutex has been bridged, its four-byte storage contains a real
 * Vita pthread pointer. Avoid taking the global object-registry lock for every
 * operation. Besides being unnecessary, that extra lock can deadlock the
 * render thread while it releases the graphics mutex after presenting a
 * frame. Small values are Bionic's three supported static initializers. */
PTHR_INLINE int _mutex_t_has_real_ptr(const pthread_mutex_t_bionic *mutex) {
    uintptr_t value = (uintptr_t)mutex->real_ptr;
    return value != BIONIC_PTHREAD_MUTEX_INITIALIZER &&
           value != BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER &&
           value != BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    PTHR_LOCK
    if (isObjectInitializedUnlocked(cond)) {
        PTHR_UNLOCK
        return ret;
    }

    /* pthread_cond_t has the same pointer representation on Vita. */
    pthread_cond_t *real_ptr = calloc(1, sizeof(pthread_cond_t));
    if (!real_ptr) {
        PTHR_UNLOCK
        return ENOMEM;
    }

    ret = pthread_cond_init(real_ptr, attr);

    if (ret == 0) {
        if (!rememberObjectUnlocked(cond)) {
            pthread_cond_destroy(real_ptr);
            free(real_ptr);
            ret = ENOMEM;
        } else {
            cond->real_ptr = real_ptr;
        }
    } else {
        l_error("cond initialization for %p has failed", cond);
        free(real_ptr);
    }

    PTHR_UNLOCK
    return ret;
}

PTHR_INLINE int _cond_t_has_real_ptr(const pthread_cond_t_bionic *cond) {
    return cond->real_ptr != NULL;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    PthrStartContext *context = malloc(sizeof(*context));
    if (!context) {
        return ENOMEM;
    }
    context->start = start;
    context->param = param;

    size_t effective_stack_size = BTD5_ANDROID_WORKER_STACK;
    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, effective_stack_size);
        ret = pthread_create(thread, &a, pthread_start_trampoline, context);
        pthread_attr_destroy(&a);
    } else {
        pthread_attr_t_bionic *mutable_attr = (pthread_attr_t_bionic *)attr;
        _attr_t_static_init(mutable_attr);
        /* BTD5 normally leaves the Android attribute at its default. Use a
         * smaller safe default there, but never shrink an explicit request
         * larger than 256 KiB. */
        if (mutable_attr->stack_size > effective_stack_size) {
            effective_stack_size = mutable_attr->stack_size;
        }
        pthread_attr_setstacksize(mutable_attr->real_ptr,
                                  effective_stack_size);
        ret = pthread_create(thread, mutable_attr->real_ptr,
                             pthread_start_trampoline, context);
    }

    if (ret != 0) {
        free(context);
    }

    unsigned int sequence = atomic_fetch_add_explicit(
        &android_threads_created, 1, memory_order_relaxed) + 1;
    if (sequence <= 16 || ret != 0) {
        l_info("Android pthread_create #%u: start SO+0x%08x, stack=%u KiB, "
               "result %d.", sequence, game_address_offset((void *)start),
               (unsigned int)(effective_stack_size / 1024U), ret);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    if (!isObjectInitialized(mutex)) {
        mutex->real_ptr = NULL;
        return 0;
    }
    forgetObject(mutex);
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!_mutex_t_has_real_ptr(mutex)) {
        int ret = _mutex_t_static_init(mutex, NULL);
        if (ret != 0 || !mutex->real_ptr) return ret ? ret : EINVAL;
    }
#ifdef DEBUG_SOLOADER
    worker_sync_begin(PTHR_SYNC_MUTEX_LOCK, __builtin_return_address(0));
    btd5_diag_set_stage(BTD5_STAGE_MUTEX_LOCK);
#endif
    int ret = pthread_mutex_lock(mutex->real_ptr);
#ifdef DEBUG_SOLOADER
    btd5_diag_set_stage(BTD5_STAGE_MUTEX_LOCK_RETURNED);
    worker_sync_end();
#endif
    return ret;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!_mutex_t_has_real_ptr(mutex)) {
        int ret = _mutex_t_static_init(mutex, NULL);
        if (ret != 0 || !mutex->real_ptr) return ret ? ret : EINVAL;
    }
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!_mutex_t_has_real_ptr(mutex)) return EINVAL;
#ifdef DEBUG_SOLOADER
    btd5_diag_set_stage(BTD5_STAGE_MUTEX_UNLOCK);
#endif
    int ret = pthread_mutex_unlock(mutex->real_ptr);
#ifdef DEBUG_SOLOADER
    btd5_diag_set_stage(BTD5_STAGE_MUTEX_UNLOCK_RETURNED);
#endif
    return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    if (btd5_diag_is_current_thread()) {
        l_warn("Vita main entering Android pthread_join from SO+0x%08x.",
               game_address_offset(__builtin_return_address(0)));
    }
    worker_sync_begin(PTHR_SYNC_JOIN, __builtin_return_address(0));
    int ret = pthread_join(thread, value_ptr);
    worker_sync_end();
    return ret;
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    if (!isObjectInitialized(cond)) {
        cond->real_ptr = NULL;
        return 0;
    }
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    if (!_cond_t_has_real_ptr(cond)) {
        int ret = _cond_t_static_init(cond, NULL);
        if (ret != 0 || !cond->real_ptr) return ret ? ret : EINVAL;
    }

    bool watched = cond_is_watched(cond);
    const void *caller = __builtin_return_address(0);
    if (watched) {
#ifdef DEBUG_COND_VERBOSE
        l_warn("Android worker signaling watched condition from "
               "SO+0x%08x (cond %p -> %p).",
               game_address_offset(caller), cond, cond->real_ptr);
#else
        (void)caller;
#endif
    }
    int ret = pthread_cond_signal(cond->real_ptr);
#ifdef DEBUG_COND_VERBOSE
    if (watched) {
        l_warn("pthread_cond_signal for watched condition returned %d.", ret);
    }
#endif
    return ret;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    int ret = 0;
    if (!_cond_t_has_real_ptr(cond)) {
        ret = _cond_t_static_init(cond, NULL);
        if (ret != 0 || !cond->real_ptr) return ret ? ret : EINVAL;
    }
    if (!_mutex_t_has_real_ptr(mutex)) {
        ret = _mutex_t_static_init(mutex, NULL);
        if (ret != 0 || !mutex->real_ptr) return ret ? ret : EINVAL;
    }

    /* pthreads-embedded reports Vita/newlib errno values, while the Android
     * caller consumes Linux/Bionic values directly. In particular, newlib's
     * ETIMEDOUT is 116, which Bionic interprets as ESTALE. Boost then throws a
     * condition_error instead of treating an ordinary timeout as a timeout. */
    const void *caller = __builtin_return_address(0);
    bool watched = cond_wait_watch_begin(cond, mutex, caller, true);
    btd5_diag_set_stage(BTD5_STAGE_COND_WAIT);
    worker_sync_begin(PTHR_SYNC_COND_TIMEDWAIT,
                      __builtin_return_address(0));
    ret = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
    worker_sync_end();
    btd5_diag_set_stage(BTD5_STAGE_COND_WAIT_RETURNED);
    int bionic_ret = errno_newlib_to_bionic(ret);
    if (bionic_ret != 0 && bionic_ret != ETIMEDOUT_BIONIC) {
        l_warn("pthread_cond_timedwait(%p, %p) returned %d (Bionic %d)",
               cond, mutex, ret, bionic_ret);
    }
    cond_wait_watch_end(watched, cond, caller, bionic_ret);
    return bionic_ret;
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    if (!_cond_t_has_real_ptr(cond)) {
        int ret = _cond_t_static_init(cond, NULL);
        if (ret != 0 || !cond->real_ptr) return ret ? ret : EINVAL;
    }
    if (!_mutex_t_has_real_ptr(mutex)) {
        int ret = _mutex_t_static_init(mutex, NULL);
        if (ret != 0 || !mutex->real_ptr) return ret ? ret : EINVAL;
    }

    const void *caller = __builtin_return_address(0);
    unsigned int caller_offset = game_address_offset(caller);
    if (!barrier_wait_logged && caller_offset == 0x004dd815u) {
        /* In BTD5 4.7 this import is reached from boost::barrier::wait().
         * The condition and mutex occupy barrier+8 and barrier+4; the live
         * count/generation at +12/+16 tell us whether both participants are
         * using the same Android object and actually serializing on it. */
        const uint32_t *barrier = (const uint32_t *)((const char *)cond - 8);
        l_warn("BTD5 barrier entry: thread 0x%x (%s), barrier %p, count=%u, "
               "generation=%u, cond %p -> %p, mutex %p -> %p.",
               sceKernelGetThreadId(),
               android_thread_slot >= 0 ? "Android worker" : "Vita main",
               barrier, barrier[3], barrier[4], cond, cond->real_ptr,
               mutex, mutex->real_ptr);
        barrier_wait_logged = true;
    }
    bool watched = cond_wait_watch_begin(cond, mutex, caller, false);
    btd5_diag_set_stage(BTD5_STAGE_COND_WAIT);

    /* pthreads-embedded occasionally loses a broadcast at BTD5 4.7's
     * Boost startup barrier. Only that fingerprinted call site needs a bounded
     * wait. Applying a 250 ms timeout to every game condition causes otherwise
     * idle workers to wake and churn during gameplay. */
    struct timespec deadline;
    int ret;
    if (caller_offset == 0x004dd815u &&
        clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
        deadline.tv_nsec += 250 * 1000 * 1000L;
        if (deadline.tv_nsec >= 1000 * 1000 * 1000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000 * 1000 * 1000L;
        }
        worker_sync_begin(PTHR_SYNC_COND_WAIT, caller);
        ret = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr,
                                     &deadline);
        worker_sync_end();
        if (ret == ETIMEDOUT) {
            unsigned int count = atomic_fetch_add_explicit(
                &condition_poll_wakeups, 1, memory_order_relaxed) + 1;
            if (count <= 5 || count % 20 == 0) {
                l_warn("Recovered BTD5 startup barrier wait at SO+0x%08x "
                       "with a POSIX spurious wake (poll #%u).",
                       game_address_offset(caller), count);
            }
            ret = 0;
        } else if (ret != 0) {
            ret = errno_newlib_to_bionic(ret);
        }
    } else {
        worker_sync_begin(PTHR_SYNC_COND_WAIT, caller);
        ret = pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
        worker_sync_end();
    }
    btd5_diag_set_stage(BTD5_STAGE_COND_WAIT_RETURNED);
    cond_wait_watch_end(watched, cond, caller, ret);
    return ret;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    if (!_cond_t_has_real_ptr(cond)) {
        int ret = _cond_t_static_init(cond, NULL);
        if (ret != 0 || !cond->real_ptr) return ret ? ret : EINVAL;
    }

    bool watched = cond_is_watched(cond);
    const void *caller = __builtin_return_address(0);
    if (watched) {
#ifdef DEBUG_COND_VERBOSE
        l_warn("Android worker broadcasting watched condition from "
               "SO+0x%08x (cond %p -> %p).",
               game_address_offset(caller), cond, cond->real_ptr);
#else
        (void)caller;
#endif
    }
    int ret = pthread_cond_broadcast(cond->real_ptr);
#ifdef DEBUG_COND_VERBOSE
    if (watched) {
        l_warn("pthread_cond_broadcast for watched condition returned %d.",
               ret);
    }
#endif
    return ret;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->real_ptr = NULL;
    attr->magic = 0x0;
    attr->stack_size = 0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    int ret = pthread_attr_setstacksize(attr->real_ptr, stacksize);
    if (ret == 0) {
        attr->stack_size = stacksize;
    }
    return ret;
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    unsigned int sequence = atomic_fetch_add_explicit(
        &android_semaphores_created, 1, memory_order_relaxed) + 1;
    if (sequence <= 24 || *uid < 0) {
        l_info("Android sem_init #%u: uid 0x%x, value %u, caller "
               "SO+0x%08x.", sequence, *uid, value,
               game_address_offset(__builtin_return_address(0)));
    }
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    worker_sync_begin(PTHR_SYNC_SEM_TIMEDWAIT, __builtin_return_address(0));
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0) {
        worker_sync_end();
        return 0;
    }
    if (!abstime) {
        worker_sync_end();
        return -1;
    }
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) {
        worker_sync_end();
        return -1;
    }
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0) {
        worker_sync_end();
        return -1;
    }
    worker_sync_end();
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (btd5_diag_is_current_thread()) {
        SceKernelSemaInfo info = {0};
        info.size = sizeof(info);
        int info_result = sceKernelGetSemaInfo(*uid, &info);
        l_warn("Vita main entering Android sem_wait from SO+0x%08x: uid "
               "0x%x, current count %d (info 0x%x).",
               game_address_offset(__builtin_return_address(0)), *uid,
               info_result >= 0 ? info.currentCount : -1, info_result);
        log_flush();
    }
    worker_sync_begin(PTHR_SYNC_SEM_WAIT, __builtin_return_address(0));
    if (sceKernelWaitSema(*uid, 1, NULL) < 0) {
        worker_sync_end();
        return -1;
    }
    worker_sync_end();
    return 0;
}
