#include "utils/init.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "diagnostics.h"
#include "patch.h"
#include "game.h"
#include "java.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/audioout.h>

#include <pthread.h>
#include <stdatomic.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/asset_manager.h"
#include "reimpl/controls.h"
#include "reimpl/egl.h"
#include "reimpl/io.h"
#include "reimpl/pthr.h"

/* MainActivity passes android.view.Display.getRotation(), not the
 * Configuration orientation.  A landscape phone display normally reports
 * Surface.ROTATION_90. */
#define ANDROID_SURFACE_ROTATION_90 1

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

/* The Vita OpenSLES backend uses this weak override for its output mixer.
 * Keep sound effects on the 48 kHz MAIN port so Java MP3 music can own the
 * independent BGM port. */
int _opensles_user_freq = 48000;

extern volatile uint32_t opensles_vita_player_count;
extern volatile uint32_t opensles_vita_player_failures;
extern volatile uint32_t opensles_vita_enqueue_count;
extern volatile uint32_t opensles_vita_enqueue_failures;
extern volatile uint32_t opensles_vita_last_source_rate;
extern volatile uint32_t opensles_vita_last_source_channels;
extern volatile uint32_t opensles_vita_last_source_bits;
extern volatile uint32_t opensles_vita_resample_allocations;
extern volatile uint32_t opensles_vita_resample_reuses;
extern volatile uint32_t opensles_vita_resample_grows;
extern volatile uint32_t opensles_vita_resample_max_bytes;
extern volatile uint32_t opensles_vita_output_buffers;
extern volatile uint32_t opensles_vita_nonzero_buffers;
extern volatile uint32_t opensles_vita_output_completions;
extern volatile uint32_t opensles_vita_waited_for_enqueue;
extern volatile int32_t opensles_vita_output_error;

static void log_effects_status(void) {
    l_info("Effects audio: players=%u (failed=%u), enqueues=%u "
           "(failed=%u), mixed=%u/%u buffers, last=%u Hz/%u ch/%u-bit, "
           "completed=%u, delayed_start=%u, output_error=0x%08x, "
           "resample alloc/reuse/grow=%u/%u/%u (max=%u bytes).",
           opensles_vita_player_count, opensles_vita_player_failures,
           opensles_vita_enqueue_count, opensles_vita_enqueue_failures,
           opensles_vita_nonzero_buffers, opensles_vita_output_buffers,
           opensles_vita_last_source_rate / 1000U,
           opensles_vita_last_source_channels,
           opensles_vita_last_source_bits,
           opensles_vita_output_completions,
           opensles_vita_waited_for_enqueue,
           (unsigned int)opensles_vita_output_error,
           opensles_vita_resample_allocations,
           opensles_vita_resample_reuses,
           opensles_vita_resample_grows,
           opensles_vita_resample_max_bytes);
}

typedef void (*BTD5NativeLoad)(JNIEnv *, jobject, jobject, jobject);
typedef void (*BTD5NativeSurfaceCreated)(JNIEnv *, jobject, jobject, jint, jint);
typedef void (*BTD5NativeResize)(JNIEnv *, jobject, jint, jint);
typedef void (*BTD5NativeTick)(JNIEnv *, jobject);
typedef void (*BTD5NativeTouch)(JNIEnv *, jobject, jfloat, jfloat, jint);
typedef void (*BTD5NativeTouchHeld)(JNIEnv *, jobject, jfloat, jfloat, jint, jboolean);
typedef void (*BTD5NativeKey)(JNIEnv *, jobject, jint, jint);
typedef void (*BTD5NativeVoid)(JNIEnv *, jobject);
typedef void (*BTD5NativeBool)(JNIEnv *, jobject, jboolean);
typedef void (*BTD5NativeOrientation)(JNIEnv *, jobject, jint);

static BTD5NativeLoad native_load;
static BTD5NativeSurfaceCreated native_surface_created;
static BTD5NativeResize native_resize;
static BTD5NativeTick native_tick;
static BTD5NativeTouch native_touch_started;
static BTD5NativeTouch native_touch_ended;
static BTD5NativeTouch native_touch_cancelled;
static BTD5NativeTouchHeld native_touch_held;
static BTD5NativeKey native_key_down;
static BTD5NativeKey native_key_up;
static BTD5NativeVoid native_back_pressed;
static BTD5NativeVoid native_pause;
static BTD5NativeVoid native_resume;
static BTD5NativeBool native_lost_audio_focus;
static BTD5NativeVoid native_gained_audio_focus;
static BTD5NativeOrientation native_orientation_changed;
static BTD5NativeLicenseResult native_license_result;
static pthread_mutex_t native_touch_dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;

static atomic_bool native_tick_active = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t native_tick_started_us = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_started = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_completed = ATOMIC_VAR_INIT(0);
static atomic_int main_thread_id = ATOMIC_VAR_INIT(-1);
static atomic_bool native_surface_active = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t native_surface_started_us = ATOMIC_VAR_INIT(0);
static atomic_bool clean_exit_requested = ATOMIC_VAR_INIT(false);
static bool runtime_diagnostics_enabled = false;

#define PROFILE_SYNC_QUIET_US        UINT64_C(3000000)
#define PROFILE_SYNC_MIN_INTERVAL_US UINT64_C(15000000)
#define PROFILE_SYNC_MAX_DIRTY_US    UINT64_C(30000000)
#define PROFILE_SYNC_POLL_US         (250 * 1000)
#define PROFILE_SYNC_THREAD_STACK    (96 * 1024)

static atomic_uint committed_profile_generation = ATOMIC_VAR_INIT(0);
static pthread_mutex_t profile_sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t profile_sync_retry_after_us = 0;
static uint64_t profile_last_sync_us = 0;
static bool profile_sync_worker_available = false;

static void configure_main_thread_affinity(void) {
    /* The previous fix hard-pinned nativeTick to core 0 and pushed every
     * Android worker onto cores 1/2/3. The supplied late-game run regressed
     * under that layout. Let the Vita scheduler balance the main thread and
     * game workers across the three supported user cores instead. CPU3 is
     * intentionally not selected: it is system-reserved and the captured run
     * already showed worse contention when CapUnlocker exposed it. */
    int result = sceKernelChangeThreadCpuAffinityMask(
        sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_ALL);
    if (result < 0) {
        l_warn("Could not restore scheduler-balanced main-thread affinity: "
               "0x%08x.", (unsigned int)result);
    }

    pthr_set_android_worker_cpu_mask(SCE_KERNEL_CPU_MASK_USER_ALL);
    l_info("CPU layout: nativeTick/render and Android workers are "
           "scheduler-balanced across user cores 0/1/2; system CPU3 is "
           "disabled by default.");
}

static bool sync_profile_storage(bool force, const char *reason) {
    uint32_t pending = profile_save_generation();
    uint32_t committed = atomic_load_explicit(
        &committed_profile_generation, memory_order_acquire);
    if (!force && pending == committed) {
        return true;
    }

    pthread_mutex_lock(&profile_sync_mutex);
    pending = profile_save_generation();
    committed = atomic_load_explicit(&committed_profile_generation,
                                     memory_order_acquire);
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!force && pending == committed) {
        pthread_mutex_unlock(&profile_sync_mutex);
        return true;
    }
    if (!force && now < profile_sync_retry_after_us) {
        pthread_mutex_unlock(&profile_sync_mutex);
        return false;
    }

    uint64_t started_us = now;
    int result = sceIoSync("ux0:", 0);
    uint64_t completed_us = sceKernelGetProcessTimeWide();
    uint64_t elapsed_us = completed_us - started_us;
    if (result < 0) {
        l_warn("Could not commit BTD5 profile storage (%s): 0x%08x "
               "after %llu.%01llu ms.", reason, (unsigned int)result,
               (unsigned long long)(elapsed_us / 1000ULL),
               (unsigned long long)((elapsed_us % 1000ULL) / 100ULL));
        profile_sync_retry_after_us = completed_us + UINT64_C(1000000);
        pthread_mutex_unlock(&profile_sync_mutex);
        return false;
    }

    profile_sync_retry_after_us = 0;
    profile_last_sync_us = completed_us;
    atomic_store_explicit(&committed_profile_generation, pending,
                          memory_order_release);
    if (pending != 0) {
        l_success("Autosave checkpoint %u committed (%s, %llu.%01llu ms).",
                  pending, reason,
                  (unsigned long long)(elapsed_us / 1000ULL),
                  (unsigned long long)((elapsed_us % 1000ULL) / 100ULL));
    } else {
        l_info("BTD5 storage committed (%s, %llu.%01llu ms).", reason,
               (unsigned long long)(elapsed_us / 1000ULL),
               (unsigned long long)((elapsed_us % 1000ULL) / 100ULL));
    }
    pthread_mutex_unlock(&profile_sync_mutex);
    return true;
}

static void *profile_sync_worker(void *unused) {
    (void)unused;
    uint64_t dirty_since_us = 0;

    for (;;) {
        sceKernelDelayThread(PROFILE_SYNC_POLL_US);

        uint32_t pending = profile_save_generation();
        uint32_t committed = atomic_load_explicit(
            &committed_profile_generation, memory_order_acquire);
        if (pending == committed) {
            dirty_since_us = 0;
            continue;
        }

        uint64_t now = sceKernelGetProcessTimeWide();
        if (dirty_since_us == 0) {
            dirty_since_us = now;
        }

        uint64_t last_closed_us = profile_save_last_closed_us();
        pthread_mutex_lock(&profile_sync_mutex);
        uint64_t last_sync_us = profile_last_sync_us;
        pthread_mutex_unlock(&profile_sync_mutex);

        bool quiet = last_closed_us != 0 &&
                     now - last_closed_us >= PROFILE_SYNC_QUIET_US;
        bool interval_elapsed = last_sync_us == 0 ||
                                now - last_sync_us >=
                                    PROFILE_SYNC_MIN_INTERVAL_US;
        bool max_age_reached = now - dirty_since_us >=
                               PROFILE_SYNC_MAX_DIRTY_US;

        if ((quiet && interval_elapsed) || max_age_reached) {
            const char *reason = max_age_reached
                ? "write-back cache maximum age"
                : "write-back cache quiet period";
            (void)sync_profile_storage(false, reason);
            if (profile_save_generation() == atomic_load_explicit(
                    &committed_profile_generation, memory_order_acquire)) {
                dirty_since_us = 0;
            }
        }
    }
    return NULL;
}

static void start_profile_sync_worker(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int stack_result = pthread_attr_setstacksize(
        &attr, PROFILE_SYNC_THREAD_STACK);
    if (stack_result != 0) {
        l_warn("Could not set profile write-back thread stack to %u KiB "
               "(pthread %d); using the default.",
               PROFILE_SYNC_THREAD_STACK / 1024U, stack_result);
    }

    pthread_t thread;
    int result = pthread_create(&thread, &attr, profile_sync_worker, NULL);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        l_warn("Could not start profile write-back cache thread (pthread %d); "
               "falling back to synchronous save commits.", result);
        return;
    }

    pthread_detach(thread);
    profile_sync_worker_available = true;
    l_info("Profile write-back cache enabled: 3 s quiet period, 15 s "
           "minimum commit interval, 30 s maximum dirty age; pause and "
           "clean exit still force a commit.");
}

static void *tick_watchdog(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelDelayThread(10 * 1000 * 1000);
        uint64_t started = atomic_load_explicit(&native_ticks_started,
                                                memory_order_relaxed);
        uint64_t completed = atomic_load_explicit(&native_ticks_completed,
                                                  memory_order_relaxed);
        uint64_t swaps = egl_swap_count();
        if (atomic_load_explicit(&native_surface_active, memory_order_acquire)) {
            uint64_t begin = atomic_load_explicit(&native_surface_started_us,
                                                  memory_order_relaxed);
            uint64_t elapsed = (sceKernelGetProcessTimeWide() - begin) /
                               1000000ULL;
            l_warn("Watchdog: nativeSurfaceCreated still running after %llu s.",
                   (unsigned long long)elapsed);
            BTD5TickStage stage = btd5_diag_get_stage();
            SceKernelThreadInfo info = {0};
            info.size = sizeof(info);
            int thread_info_result = sceKernelGetThreadInfo(
                atomic_load_explicit(&main_thread_id, memory_order_acquire),
                &info);
            l_warn("Watchdog main thread during surface creation: stage %s.",
                   btd5_diag_stage_name(stage));
            AAssetDiagnostics asset_diag;
            AAsset_getDiagnostics(&asset_diag);
            l_warn("Asset progress: opens=%llu active=%llu reads=%llu, "
                   "bytes=%llu, seeks=%llu, last=%llu/%llu.",
                   asset_diag.opens, asset_diag.active, asset_diag.reads,
                   asset_diag.bytes, asset_diag.seeks,
                   asset_diag.last_position, asset_diag.last_size);
            if (thread_info_result == 0) {
                l_warn("Watchdog main thread: status 0x%x, wait type %u, "
                       "wait id 0x%x, CPU %d.", info.status, info.waitType,
                       info.waitId, info.currentCpuId);
                if (info.waitType == 32 && info.waitId > 0) {
                    SceKernelSemaInfo sema = {0};
                    sema.size = sizeof(sema);
                    int sema_result = sceKernelGetSemaInfo(info.waitId, &sema);
                    if (sema_result == 0) {
                        l_warn("Main wait semaphore: '%s', count %d/%d, "
                               "initial %d, waiters %d.", sema.name,
                               sema.currentCount, sema.maxCount,
                               sema.initCount, sema.numWaitThreads);
                    } else {
                        l_warn("Main wait UID is not a visible semaphore: 0x%x.",
                               sema_result);
                    }
                }
            } else {
                l_warn("Watchdog could not inspect main thread: 0x%x.",
                       thread_info_result);
            }
            pthr_diag_log_threads();
        } else if (atomic_load_explicit(&native_tick_active,
                                        memory_order_acquire)) {
            uint64_t begin = atomic_load_explicit(&native_tick_started_us,
                                                  memory_order_relaxed);
            uint64_t elapsed_us = sceKernelGetProcessTimeWide() - begin;
            if (elapsed_us < 2ULL * 1000ULL * 1000ULL) {
                continue;
            }
            uint64_t elapsed = elapsed_us / 1000000ULL;
            BTD5TickStage stage = btd5_diag_get_stage();
            SceKernelThreadInfo info = {0};
            info.size = sizeof(info);
            int thread_info_result = sceKernelGetThreadInfo(
                atomic_load_explicit(&main_thread_id, memory_order_acquire),
                &info);
            l_warn("Watchdog: nativeTick #%llu still running after %llu s; "
                   "%llu completed, %llu EGL swaps; stage: %s.",
                   (unsigned long long)started,
                   (unsigned long long)elapsed,
                   (unsigned long long)completed,
                   (unsigned long long)swaps,
                   btd5_diag_stage_name(stage));
            if (thread_info_result == 0) {
                l_warn("Watchdog main thread: status 0x%x, wait type %u, "
                       "wait id 0x%x, CPU %d.", info.status, info.waitType,
                       info.waitId, info.currentCpuId);
            } else {
                l_warn("Watchdog could not inspect main thread: 0x%x.",
                       thread_info_result);
            }
            if (stage == BTD5_STAGE_COND_WAIT) {
                pthr_diag_log_threads();
            }
        }
        log_flush();
    }
    return NULL;
}

static void validate_game_data(void) {
    char asset_path[160];
    btd5_path(asset_path, sizeof(asset_path), "assets/Assets/BTD5.jet");
    if (!file_exists(btd5_so_path())) {
        fatal_error("Missing BTD5 %s executable:\n%s",
                    btd5_game_version_name(), btd5_so_path());
    }
    if (!file_exists(asset_path)) {
        fatal_error("Missing BTD5 %s assets:\n%s",
                    btd5_game_version_name(), asset_path);
    }

    uint32_t native_crc = 0;
    uint64_t native_size = 0;
    int fingerprint_result = btd5_verify_native_fingerprint(
        &native_crc, &native_size);
    if (fingerprint_result < 0) {
        fatal_error("Could not read BTD5 %s executable:\n%s\n\nError 0x%08X",
                    btd5_game_version_name(), btd5_so_path(),
                    (unsigned int)fingerprint_result);
    }
    if (fingerprint_result == 0) {
        fatal_error("Wrong BTD5 executable in the %s folder.\n\n"
                    "Found: %llu bytes, CRC32 %08X\n"
                    "Expected: %llu bytes, CRC32 %08X\n\n"
                    "Recreate this folder with the matching game files.",
                    btd5_game_version_name(),
                    (unsigned long long)native_size, native_crc,
                    (unsigned long long)btd5_expected_native_size(),
                    btd5_expected_native_crc());
    }
    l_success("Verified BTD5 %s native fingerprint: %llu bytes, CRC32 %08X.",
              btd5_game_version_name(),
              (unsigned long long)native_size, native_crc);
}

static void *game_symbol(const char *name) {
    void *symbol = (void *)so_symbol(&so_mod, name);
    if (!symbol) {
        l_fatal("BTD5 is missing required export: %s", name);
        fatal_error("Unsupported Bloons TD 5 libnative.so: missing %s.", name);
    }
    return symbol;
}

static void bind_game_api(void) {
    native_load = (BTD5NativeLoad)game_symbol("_Z23MainActivity_nativeLoadP7_JNIEnvP8_jobjectS2_S2_");
    native_surface_created = (BTD5NativeSurfaceCreated)game_symbol("_Z33MainActivity_nativeSurfaceCreatedP7_JNIEnvP8_jobjectS2_ii");
    native_resize = (BTD5NativeResize)game_symbol("_Z25MainActivity_nativeResizeP7_JNIEnvP8_jobjectii");
    native_tick = (BTD5NativeTick)game_symbol("_Z23MainActivity_nativeTickP7_JNIEnvP8_jobject");
    native_touch_started = (BTD5NativeTouch)game_symbol("_Z31MainActivity_nativeTouchStartedP7_JNIEnvP8_jobjectffi");
    native_touch_ended = (BTD5NativeTouch)game_symbol("_Z29MainActivity_nativeTouchEndedP7_JNIEnvP8_jobjectffi");
    native_touch_cancelled = (BTD5NativeTouch)game_symbol("_Z33MainActivity_nativeTouchCancelledP7_JNIEnvP8_jobjectffi");
    native_touch_held = (BTD5NativeTouchHeld)game_symbol("_Z28MainActivity_nativeTouchHeldP7_JNIEnvP8_jobjectffih");
    native_key_down = (BTD5NativeKey)game_symbol("_Z31MainActivity_nativeInputKeyDownP7_JNIEnvP8_jobjectii");
    native_key_up = (BTD5NativeKey)game_symbol("_Z29MainActivity_nativeInputKeyUpP7_JNIEnvP8_jobjectii");
    native_back_pressed = (BTD5NativeVoid)game_symbol("_Z30MainActivity_nativeBackPressedP7_JNIEnvP8_jobject");
    native_pause = (BTD5NativeVoid)game_symbol("_Z24MainActivity_nativePauseP7_JNIEnvP8_jobject");
    native_resume = (BTD5NativeVoid)game_symbol("_Z25MainActivity_nativeResumeP7_JNIEnvP8_jobject");
    native_lost_audio_focus = (BTD5NativeBool)game_symbol("_Z33MainActivity_nativeLostAudioFocusP7_JNIEnvP8_jobjecth");
    native_gained_audio_focus = (BTD5NativeVoid)game_symbol("_Z35MainActivity_nativeGainedAudioFocusP7_JNIEnvP8_jobject");
    native_orientation_changed = (BTD5NativeOrientation)game_symbol("_Z37MainActivity_nativeOrientationChangedP7_JNIEnvP8_jobjecti");
    native_license_result = (BTD5NativeLicenseResult)game_symbol("_Z12ox94jnabaredP7_JNIEnvP8_jobjectii");
    btd5_java_bind_license_result(native_license_result);
}

static bool update_lifecycle(bool paused) {
    SceAppMgrAppState state = {0};
    int result = _sceAppMgrGetAppState(&state, sizeof(state), 0);
    if (result < 0) {
        return paused;
    }

    bool should_pause = state.isSystemUiOverlaid != 0;
    if (should_pause && !paused) {
        l_info("System UI opened; requesting Android pause/save lifecycle.");
        native_lost_audio_focus(&jni, NULL, JNI_TRUE);
        native_pause(&jni, NULL);
        sync_profile_storage(true, "system UI pause");
        log_flush();
    } else if (!should_pause && paused) {
        l_info("System UI closed; resuming Android lifecycle.");
        native_gained_audio_focus(&jni, NULL);
        native_resume(&jni, NULL);
        log_flush();
    }
    return should_pause;
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    if (keycode == AKEYCODE_BACK && action == CONTROLS_ACTION_UP) {
        native_back_pressed(&jni, NULL);
        return;
    }

    if (action == CONTROLS_ACTION_DOWN) {
        native_key_down(&jni, NULL, keycode, 0);
    } else if (action == CONTROLS_ACTION_UP) {
        native_key_up(&jni, NULL, keycode, 0);
    }
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    /* Android delivers MotionEvent callbacks on its UI thread while the game
     * renderer advances on a separate thread. The Vita touch sampler mirrors
     * that model for immediate in-game DOWN delivery. Serialize callbacks so
     * the sampler and frame thread cannot enter the native input bridge at the
     * same time. */
    pthread_mutex_lock(&native_touch_dispatch_mutex);
    switch (action) {
    case CONTROLS_ACTION_DOWN:
        native_touch_started(&jni, NULL, x, y, id);
        break;
    case CONTROLS_ACTION_MOVE:
        /* MainActivity passes false for touchscreen ACTION_MOVE. True denotes
         * generic mouse/hover motion and does not behave as a held drag. */
        native_touch_held(&jni, NULL, x, y, id, JNI_FALSE);
        break;
    case CONTROLS_ACTION_UP:
        native_touch_ended(&jni, NULL, x, y, id);
        break;
    case CONTROLS_ACTION_CANCEL:
        native_touch_cancelled(&jni, NULL, x, y, id);
        break;
    }
    pthread_mutex_unlock(&native_touch_dispatch_mutex);
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    (void)which;
    (void)x;
    (void)y;
    (void)action;
}

void controls_handler_exit_request(void) {
    atomic_store_explicit(&clean_exit_requested, true, memory_order_release);
}

static void *clean_exit_watchdog(void *unused) {
    (void)unused;
    /* A broken Android pause callback must not leave the application stuck on
     * a black screen. Storage is committed before this watchdog starts. */
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitProcess(0);
    return NULL;
}

static void perform_clean_exit(void) {
    l_info("Clean exit requested; committing profile and Android lifecycle.");
    sync_profile_storage(true, "clean exit before pause");
    log_flush();

    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, clean_exit_watchdog, NULL) == 0) {
        pthread_detach(watchdog);
    }

    native_lost_audio_focus(&jni, NULL, JNI_TRUE);
    native_pause(&jni, NULL);
    sync_profile_storage(true, "clean exit after pause");
    l_success("Clean exit lifecycle completed.");
    log_flush();
    sceKernelExitProcess(0);
}

int main(void) {
    log_start_session();
    runtime_diagnostics_enabled = log_is_enabled();
    btd5_diag_set_enabled(runtime_diagnostics_enabled);
    egl_set_diagnostics_enabled(runtime_diagnostics_enabled);
    l_success("BTD5 loader 01.00 started.");
    l_info("Performance v7.2 single-build path: retained the complete v6.9 OpenSLES and stable renderer behavior, enabled fingerprint-gated frame-debt protection, added a conservative vertex-layout state cache, and restricted deep profiling plus periodic timing to the logging VPK. Unstable draw batching, upload-content deduplication, section-GC and LTO remain disabled.");
    log_flush();

    soloader_platform_init();
    btd5_select_game_version();
    validate_game_data();
    soloader_init_all();
    bind_game_api();

    int (*jni_on_load)(void *vm) = (void *)game_symbol("JNI_OnLoad");
    l_info("Calling JNI_OnLoad.");
    jni_on_load(&jvm);
    l_success("JNI_OnLoad returned.");

    /*
     * The APK declares nativeLoad(MainActivity, Object). The second object is
     * its Java AssetManager in the Android build. Use FalsoJNI's reserved fake
     * handles for both Java objects: DeleteGlobalRef explicitly preserves
     * these values, while AAssetManager_fromJava ignores the wrapper and
     * creates our native asset manager on demand.
     */
    jobject activity = (jobject)0x42424242;
    jobject assets = (jobject)0x69696969;
    l_info("Calling nativeLoad.");
    native_load(&jni, NULL, activity, assets);
    l_success("nativeLoad returned.");

    if (runtime_diagnostics_enabled) {
        atomic_store_explicit(&main_thread_id, sceKernelGetThreadId(),
                              memory_order_release);
        btd5_diag_bind_current_thread();
        pthread_t watchdog;
        if (pthread_create(&watchdog, NULL, tick_watchdog, NULL) == 0) {
            pthread_detach(watchdog);
        } else {
            l_warn("Could not start startup/nativeTick watchdog thread.");
        }
    }
    configure_main_thread_affinity();

    int render_width = settings_render_width();
    int render_height = settings_render_height();
    l_info("Render surface: %dx%d (Low Graphics %s).", render_width,
           render_height, settings_low_graphics_applied() ? "ON" : "OFF");
    l_info("Calling nativeSurfaceCreated.");
    log_flush();
    if (runtime_diagnostics_enabled) {
        atomic_store_explicit(&native_surface_started_us,
                              sceKernelGetProcessTimeWide(),
                              memory_order_relaxed);
        atomic_store_explicit(&native_surface_active, true,
                              memory_order_release);
    }
    native_surface_created(&jni, NULL, NULL, render_width, render_height);
    if (runtime_diagnostics_enabled) {
        atomic_store_explicit(&native_surface_active, false,
                              memory_order_release);
    }
    l_success("nativeSurfaceCreated returned.");

    l_info("Calling nativeResize.");
    log_flush();
    native_resize(&jni, NULL, render_width, render_height);
    l_success("nativeResize returned.");

    l_info("Calling nativeOrientationChanged.");
    log_flush();
    native_orientation_changed(&jni, NULL, ANDROID_SURFACE_ROTATION_90);
    l_success("nativeOrientationChanged returned.");

    l_info("Calling nativeGainedAudioFocus.");
    log_flush();
    native_gained_audio_focus(&jni, NULL);
    l_success("nativeGainedAudioFocus returned.");

    l_info("Calling nativeResume.");
    log_flush();
    native_resume(&jni, NULL);
    l_success("nativeResume returned.");

    start_profile_sync_worker();
    controls_start_touch_sampler();
    l_info("Audio port state after lifecycle setup: MAIN=%d BGM=%d.",
           sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_MAIN),
           sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM));
    l_success("Lifecycle setup complete; entering tick loop.");
    log_flush();

    uint64_t ticks = 0;
#ifdef BTD5_PERIODIC_TELEMETRY
    const bool telemetry_enabled = log_is_enabled();
    uint64_t last_heartbeat = telemetry_enabled ?
        sceKernelGetProcessTimeWide() : 0;
    uint64_t timing_samples = 0;
    uint64_t timing_total_us = 0;
    uint64_t timing_max_us = 0;
    uint32_t timing_over_33ms = 0;
    uint32_t timing_over_50ms = 0;
    uint32_t timing_over_100ms = 0;
#endif
    bool lifecycle_paused = false;
#ifdef BTD5_NATIVE_PHASE_PROFILER
    const bool phase_profiler_enabled =
        btd5_native_phase_profiler_enabled();
    uint64_t previous_tick_us = 0;
#endif
    bool tick_timing_enabled = runtime_diagnostics_enabled;
#ifdef BTD5_PERIODIC_TELEMETRY
    tick_timing_enabled = tick_timing_enabled || telemetry_enabled;
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
    tick_timing_enabled = tick_timing_enabled || phase_profiler_enabled;
#endif
    while (1) {
        lifecycle_paused = update_lifecycle(lifecycle_paused);
        if (lifecycle_paused) {
            sceKernelDelayThread(16 * 1000);
            continue;
        }
        controls_poll();
        if (atomic_exchange_explicit(&clean_exit_requested, false,
                                     memory_order_acq_rel)) {
            perform_clean_exit();
        }
        uint64_t started = ticks + 1;
        uint64_t tick_started_us = tick_timing_enabled ?
            sceKernelGetProcessTimeWide() : 0;
        if (runtime_diagnostics_enabled) {
            atomic_fetch_add_explicit(&native_ticks_started, 1,
                                      memory_order_relaxed);
            atomic_store_explicit(&native_tick_started_us,
                                  tick_started_us,
                                  memory_order_relaxed);
            atomic_store_explicit(&native_tick_active, true,
                                  memory_order_release);
        }
        if (started == 1) {
            l_info("Entering first nativeTick call.");
            log_flush();
        }
        if (runtime_diagnostics_enabled) {
            btd5_diag_set_stage(BTD5_STAGE_TICK_ENTER);
        }
#ifdef BTD5_NATIVE_PHASE_PROFILER
        if (phase_profiler_enabled) {
            btd5_native_phase_probe_begin(started, previous_tick_us);
        }
#endif
        native_tick(&jni, NULL);
#ifdef BTD5_NATIVE_PHASE_PROFILER
        if (phase_profiler_enabled) {
            btd5_native_phase_probe_end();
        }
#endif
        if (runtime_diagnostics_enabled) {
            btd5_diag_set_stage(BTD5_STAGE_TICK_RETURNED);
        }
#if defined(BTD5_PERIODIC_TELEMETRY) || defined(BTD5_NATIVE_PHASE_PROFILER)
        uint64_t tick_elapsed_us = tick_timing_enabled ?
            sceKernelGetProcessTimeWide() - tick_started_us : 0;
#endif
        if (runtime_diagnostics_enabled) {
            atomic_store_explicit(&native_tick_active, false,
                                  memory_order_release);
            atomic_fetch_add_explicit(&native_ticks_completed, 1,
                                      memory_order_relaxed);
        }
        ticks++;
#ifdef BTD5_PERIODIC_TELEMETRY
        if (telemetry_enabled) {
            timing_samples++;
            timing_total_us += tick_elapsed_us;
            if (tick_elapsed_us > timing_max_us) {
                timing_max_us = tick_elapsed_us;
            }
            if (tick_elapsed_us >= 33333ULL) timing_over_33ms++;
            if (tick_elapsed_us >= 50000ULL) timing_over_50ms++;
            if (tick_elapsed_us >= 100000ULL) timing_over_100ms++;
        }
#endif
#ifdef BTD5_NATIVE_PHASE_PROFILER
        if (phase_profiler_enabled) {
            previous_tick_us = tick_elapsed_us;
        }
#endif

        if (!profile_sync_worker_available) {
            sync_profile_storage(false, "synchronous fallback save");
        }

        if (ticks == 1) {
            l_success("First nativeTick call returned.");
            log_flush();
        }

#ifdef BTD5_PERIODIC_TELEMETRY
        if (telemetry_enabled && ticks % 60 == 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            if (now - last_heartbeat >= 10ULL * 1000ULL * 1000ULL) {
                l_info("Tick loop alive: %llu calls.",
                       (unsigned long long)ticks);
                EGLTimingStats swap_stats = {0};
                egl_take_timing_stats(&swap_stats);
                uint64_t timing_average_us = timing_samples != 0 ?
                    timing_total_us / timing_samples : 0;
                uint64_t swap_average_us = swap_stats.samples != 0 ?
                    swap_stats.total_us / swap_stats.samples : 0;
                uint32_t frame_debt_clamps =
                    btd5_take_frame_debt_clamp_count();
                l_info("Frame timing: nativeTick avg=%llu.%01llu ms, "
                       "max=%llu.%01llu ms, >=33/50/100ms=%u/%u/%u "
                       "(%llu frames); EGL swap avg=%llu.%01llu ms, "
                       "max=%llu.%01llu ms (%llu swaps).",
                       (unsigned long long)(timing_average_us / 1000ULL),
                       (unsigned long long)((timing_average_us % 1000ULL) /
                                            100ULL),
                       (unsigned long long)(timing_max_us / 1000ULL),
                       (unsigned long long)((timing_max_us % 1000ULL) /
                                            100ULL),
                       timing_over_33ms, timing_over_50ms,
                       timing_over_100ms,
                       (unsigned long long)timing_samples,
                       (unsigned long long)(swap_average_us / 1000ULL),
                       (unsigned long long)((swap_average_us % 1000ULL) /
                                            100ULL),
                       (unsigned long long)(swap_stats.max_us / 1000ULL),
                       (unsigned long long)((swap_stats.max_us % 1000ULL) /
                                            100ULL),
                       (unsigned long long)swap_stats.samples);
                l_info("Frame-debt protection: clamped %u overloaded frame "
                       "delta values during this interval.",
                       frame_debt_clamps);
                BTD5NativePhaseStats phase_stats = {0};
                btd5_take_native_phase_stats(&phase_stats);
                if (phase_stats.sampled_ticks != 0) {
                    uint64_t measured_us = phase_stats.pre_engine_total_us +
                                           phase_stats.engine_total_us;
                    uint64_t post_engine_total_us =
                        phase_stats.sampled_tick_total_us > measured_us ?
                        phase_stats.sampled_tick_total_us - measured_us : 0;
                    uint64_t total_avg_us = phase_stats.sampled_tick_total_us /
                                            phase_stats.sampled_ticks;
                    uint64_t pre_engine_avg_us = phase_stats.pre_engine_samples ?
                        phase_stats.pre_engine_total_us /
                        phase_stats.pre_engine_samples : 0;
                    uint64_t engine_avg_us = phase_stats.engine_samples ?
                        phase_stats.engine_total_us /
                        phase_stats.engine_samples : 0;
                    uint64_t inner_update_avg_us =
                        phase_stats.inner_update_samples ?
                        phase_stats.inner_update_total_us /
                        phase_stats.inner_update_samples : 0;
                    uint64_t outer_exclusive_total_us =
                        phase_stats.engine_total_us >
                            phase_stats.inner_update_total_us ?
                        phase_stats.engine_total_us -
                            phase_stats.inner_update_total_us : 0;
                    uint64_t outer_exclusive_avg_us =
                        phase_stats.engine_samples ?
                        outer_exclusive_total_us /
                            phase_stats.engine_samples : 0;
                    uint64_t post_engine_avg_us = post_engine_total_us /
                                                  phase_stats.sampled_ticks;
                    unsigned int engine_target_offset = 0xffffffffu;
                    if (phase_stats.engine_samples != 0 &&
                        phase_stats.engine_target >= so_mod.text_base &&
                        phase_stats.engine_target <
                            so_mod.text_base + so_mod.exec_size) {
                        engine_target_offset = (unsigned int)(
                            phase_stats.engine_target - so_mod.text_base);
                    }
                    unsigned int inner_update_target_offset = 0xffffffffu;
                    if (phase_stats.inner_update_samples != 0 &&
                        phase_stats.inner_update_target >= so_mod.text_base &&
                        phase_stats.inner_update_target <
                            so_mod.text_base + so_mod.exec_size) {
                        inner_update_target_offset = (unsigned int)(
                            phase_stats.inner_update_target - so_mod.text_base);
                    }
                    l_info("Native engine samples: ticks=%llu "
                           "(periodic=%llu slow=%llu), total avg=%llu.%01llu "
                           "ms max=%llu.%01llu; pre-engine avg=%llu.%01llu "
                           "max=%llu.%01llu (%llu); engine@SO+0x%08x "
                           "avg=%llu.%01llu max=%llu.%01llu "
                           "(%llu, target_changes=%llu); post-engine/residual "
                           "avg=%llu.%01llu ms.",
                           (unsigned long long)phase_stats.sampled_ticks,
                           (unsigned long long)phase_stats.periodic_samples,
                           (unsigned long long)phase_stats.slow_triggered_samples,
                           (unsigned long long)(total_avg_us / 1000ULL),
                           (unsigned long long)((total_avg_us % 1000ULL) / 100ULL),
                           (unsigned long long)(phase_stats.sampled_tick_max_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.sampled_tick_max_us %
                                                1000ULL) / 100ULL),
                           (unsigned long long)(pre_engine_avg_us / 1000ULL),
                           (unsigned long long)((pre_engine_avg_us % 1000ULL) /
                                                100ULL),
                           (unsigned long long)(phase_stats.pre_engine_max_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.pre_engine_max_us %
                                                1000ULL) / 100ULL),
                           (unsigned long long)phase_stats.pre_engine_samples,
                           engine_target_offset,
                           (unsigned long long)(engine_avg_us / 1000ULL),
                           (unsigned long long)((engine_avg_us % 1000ULL) /
                                                100ULL),
                           (unsigned long long)(phase_stats.engine_max_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.engine_max_us %
                                                1000ULL) / 100ULL),
                           (unsigned long long)phase_stats.engine_samples,
                           (unsigned long long)phase_stats.engine_target_changes,
                           (unsigned long long)(post_engine_avg_us / 1000ULL),
                           (unsigned long long)((post_engine_avg_us % 1000ULL) /
                                                100ULL));
                    l_info("Nested engine update: inner@SO+0x%08x "
                           "avg=%llu.%01llu ms max=%llu.%01llu "
                           "(%llu samples, target_changes=%llu); outer "
                           "exclusive/residual avg=%llu.%01llu ms.",
                           inner_update_target_offset,
                           (unsigned long long)(inner_update_avg_us / 1000ULL),
                           (unsigned long long)((inner_update_avg_us % 1000ULL) /
                                                100ULL),
                           (unsigned long long)(phase_stats.inner_update_max_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.inner_update_max_us %
                                                1000ULL) / 100ULL),
                           (unsigned long long)phase_stats.inner_update_samples,
                           (unsigned long long)
                               phase_stats.inner_update_target_changes,
                           (unsigned long long)(outer_exclusive_avg_us / 1000ULL),
                           (unsigned long long)((outer_exclusive_avg_us % 1000ULL) /
                                                100ULL));
                    uint64_t sampled_draw_avg_us = phase_stats.gl_draw_calls ?
                        phase_stats.gl_draw_cpu_us /
                        phase_stats.gl_draw_calls : 0;
                    l_info("Sampled GL inside nativeTick: calls=%llu, "
                           "vertices=%llu, draw CPU total=%llu.%01llu ms, "
                           "avg=%llu us/call, max=%llu.%01llu ms. This is "
                           "nested inside the sampled engine/frame phases, "
                           "not additional frame time.",
                           (unsigned long long)phase_stats.gl_draw_calls,
                           (unsigned long long)phase_stats.gl_draw_vertices,
                           (unsigned long long)(phase_stats.gl_draw_cpu_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.gl_draw_cpu_us %
                                                1000ULL) / 100ULL),
                           (unsigned long long)sampled_draw_avg_us,
                           (unsigned long long)(phase_stats.gl_draw_cpu_max_us /
                                                1000ULL),
                           (unsigned long long)((phase_stats.gl_draw_cpu_max_us %
                                                1000ULL) / 100ULL));
                }
                GLDrawStats draw_stats = {0};
                gl_take_draw_stats(&draw_stats);
                l_info("GL draw load: arrays=%llu (%llu vertices), "
                       "elements=%llu (%llu indices) over %llu frames.",
                       draw_stats.array_calls, draw_stats.array_vertices,
                       draw_stats.element_calls, draw_stats.element_indices,
                       (unsigned long long)timing_samples);
                unsigned long long cache_percent = draw_stats.state_calls != 0 ?
                    draw_stats.state_skipped * 100ULL /
                    draw_stats.state_calls : 0;
                if (draw_stats.detailed_timing) {
                    unsigned long long draw_calls = draw_stats.array_calls +
                                                    draw_stats.element_calls;
                    unsigned long long draw_average_us = draw_calls != 0 ?
                        draw_stats.draw_cpu_us / draw_calls : 0;
                    l_info("GL submit timing: draw CPU total=%llu.%01llu ms, "
                           "avg=%llu us/call, max=%llu.%01llu ms, "
                           ">=1/4ms=%llu/%llu; state cache skipped=%llu/%llu "
                           "(%llu%%).",
                           draw_stats.draw_cpu_us / 1000ULL,
                           (draw_stats.draw_cpu_us % 1000ULL) / 100ULL,
                           draw_average_us,
                           draw_stats.draw_cpu_max_us / 1000ULL,
                           (draw_stats.draw_cpu_max_us % 1000ULL) / 100ULL,
                           draw_stats.draw_over_1ms,
                           draw_stats.draw_over_4ms,
                           draw_stats.state_skipped,
                           draw_stats.state_calls,
                           cache_percent);
                } else {
                    l_info("GL release telemetry: per-draw timers disabled; "
                           "state cache skipped=%llu/%llu (%llu%%).",
                           draw_stats.state_skipped,
                           draw_stats.state_calls,
                           cache_percent);
                }
                log_effects_status();
                log_flush();
                timing_samples = 0;
                timing_total_us = 0;
                timing_max_us = 0;
                timing_over_33ms = 0;
                timing_over_50ms = 0;
                timing_over_100ms = 0;
                last_heartbeat = now;
            }
        }
#endif
    }

    return sceKernelExitDeleteThread(0);
}
