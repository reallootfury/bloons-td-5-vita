#include "utils/init.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "diagnostics.h"
#include "game.h"
#include "java.h"

#include <psp2/kernel/threadmgr.h>
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

static atomic_bool native_tick_active = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t native_tick_started_us = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_started = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_completed = ATOMIC_VAR_INIT(0);
static atomic_int main_thread_id = ATOMIC_VAR_INIT(-1);
static atomic_bool native_surface_active = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t native_surface_started_us = ATOMIC_VAR_INIT(0);
static atomic_bool clean_exit_requested = ATOMIC_VAR_INIT(false);
static uint32_t committed_profile_generation = 0;
static uint64_t profile_sync_retry_after_us = 0;

static void sync_profile_storage(bool force, const char *reason) {
    uint32_t pending = profile_save_generation();
    if (!force && pending == committed_profile_generation) {
        return;
    }

    uint64_t now = sceKernelGetProcessTimeWide();
    if (!force && now < profile_sync_retry_after_us) {
        return;
    }

    int result = sceIoSync("ux0:", 0);
    if (result < 0) {
        l_warn("Could not commit BTD5 profile storage (%s): 0x%08x.",
               reason, (unsigned int)result);
        profile_sync_retry_after_us = now + 1000ULL * 1000ULL;
        return;
    }

    profile_sync_retry_after_us = 0;
    committed_profile_generation = pending;
    if (pending != 0) {
        l_success("Autosave checkpoint %u committed (%s).", pending, reason);
    } else {
        l_info("BTD5 storage committed (%s).", reason);
    }
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
    l_success("BTD5 loader 01.00 started.");
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

    atomic_store_explicit(&main_thread_id, sceKernelGetThreadId(),
                          memory_order_release);
    btd5_diag_bind_current_thread();
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, tick_watchdog, NULL) == 0) {
        pthread_detach(watchdog);
    } else {
        l_warn("Could not start startup/nativeTick watchdog thread.");
    }

    int render_width = settings_render_width();
    int render_height = settings_render_height();
    l_info("Render surface: %dx%d (Low Graphics %s).", render_width,
           render_height, settings_low_graphics_applied() ? "ON" : "OFF");
    l_info("Calling nativeSurfaceCreated.");
    log_flush();
    atomic_store_explicit(&native_surface_started_us,
                          sceKernelGetProcessTimeWide(), memory_order_relaxed);
    atomic_store_explicit(&native_surface_active, true, memory_order_release);
    native_surface_created(&jni, NULL, NULL, render_width, render_height);
    atomic_store_explicit(&native_surface_active, false, memory_order_release);
    l_success("nativeSurfaceCreated returned.");
    native_resize(&jni, NULL, render_width, render_height);
    native_orientation_changed(&jni, NULL, ANDROID_SURFACE_ROTATION_90);
    native_gained_audio_focus(&jni, NULL);
    native_resume(&jni, NULL);
    l_info("Audio port state after lifecycle setup: MAIN=%d BGM=%d.",
           sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_MAIN),
           sceAudioOutGetAdopt(SCE_AUDIO_OUT_PORT_TYPE_BGM));
    l_success("Lifecycle setup complete; entering tick loop.");
    log_flush();

    uint64_t ticks = 0;
    uint64_t last_heartbeat = sceKernelGetProcessTimeWide();
    uint64_t timing_samples = 0;
    uint64_t timing_total_us = 0;
    uint64_t timing_max_us = 0;
    uint32_t timing_over_33ms = 0;
    uint32_t timing_over_50ms = 0;
    uint32_t timing_over_100ms = 0;
    bool lifecycle_paused = false;
    while (1) {
        lifecycle_paused = update_lifecycle(lifecycle_paused);
        if (lifecycle_paused) {
            sync_profile_storage(false, "completed game save");
            sceKernelDelayThread(16 * 1000);
            continue;
        }
        controls_poll();
        if (atomic_exchange_explicit(&clean_exit_requested, false,
                                     memory_order_acq_rel)) {
            perform_clean_exit();
        }
        uint64_t started = atomic_fetch_add_explicit(&native_ticks_started, 1,
                                                     memory_order_relaxed) + 1;
        uint64_t tick_started_us = sceKernelGetProcessTimeWide();
        atomic_store_explicit(&native_tick_started_us,
                              tick_started_us,
                              memory_order_relaxed);
        atomic_store_explicit(&native_tick_active, true, memory_order_release);
        if (started == 1) {
            l_info("Entering first nativeTick call.");
            log_flush();
        }
        btd5_diag_set_stage(BTD5_STAGE_TICK_ENTER);
        native_tick(&jni, NULL);
        btd5_diag_set_stage(BTD5_STAGE_TICK_RETURNED);
        uint64_t tick_elapsed_us = sceKernelGetProcessTimeWide() -
                                   tick_started_us;
        atomic_store_explicit(&native_tick_active, false, memory_order_release);
        atomic_fetch_add_explicit(&native_ticks_completed, 1,
                                  memory_order_relaxed);
        ticks++;
        timing_samples++;
        timing_total_us += tick_elapsed_us;
        if (tick_elapsed_us > timing_max_us) {
            timing_max_us = tick_elapsed_us;
        }
        if (tick_elapsed_us >= 33333ULL) timing_over_33ms++;
        if (tick_elapsed_us >= 50000ULL) timing_over_50ms++;
        if (tick_elapsed_us >= 100000ULL) timing_over_100ms++;

        sync_profile_storage(false, "completed game save");

        if (ticks == 1) {
            l_success("First nativeTick call returned.");
            log_flush();
        }

        if (ticks % 60 == 0) {
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
                GLDrawStats draw_stats = {0};
                gl_take_draw_stats(&draw_stats);
                l_info("GL draw load: arrays=%llu (%llu vertices), "
                       "elements=%llu (%llu indices) over %llu frames.",
                       draw_stats.array_calls, draw_stats.array_vertices,
                       draw_stats.element_calls, draw_stats.element_indices,
                       (unsigned long long)timing_samples);
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
    }

    return sceKernelExitDeleteThread(0);
}
