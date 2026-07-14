#include "utils/init.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "diagnostics.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <pthread.h>
#include <stdatomic.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/asset_manager.h"
#include "reimpl/controls.h"
#include "reimpl/egl.h"
#include "reimpl/pthr.h"

#define BTD5_SCREEN_WIDTH 960
#define BTD5_SCREEN_HEIGHT 544
/* MainActivity passes android.view.Display.getRotation(), not the
 * Configuration orientation.  A landscape phone display normally reports
 * Surface.ROTATION_90. */
#define ANDROID_SURFACE_ROTATION_90 1

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

typedef void (*BTD5NativeLoad)(JNIEnv *, jobject, jobject, jobject);
typedef void (*BTD5NativeSurfaceCreated)(JNIEnv *, jobject, jobject, jint, jint);
typedef void (*BTD5NativeResize)(JNIEnv *, jobject, jint, jint);
typedef void (*BTD5NativeTick)(JNIEnv *, jobject);
typedef void (*BTD5NativeTouch)(JNIEnv *, jobject, jfloat, jfloat, jint);
typedef void (*BTD5NativeTouchHeld)(JNIEnv *, jobject, jfloat, jfloat, jint, jboolean);
typedef void (*BTD5NativeKey)(JNIEnv *, jobject, jint, jint);
typedef void (*BTD5NativeVoid)(JNIEnv *, jobject);
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
static BTD5NativeVoid native_resume;
static BTD5NativeVoid native_gained_audio_focus;
static BTD5NativeOrientation native_orientation_changed;

static atomic_bool native_tick_active = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t native_tick_started_us = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_started = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t native_ticks_completed = ATOMIC_VAR_INIT(0);
static atomic_int main_thread_id = ATOMIC_VAR_INIT(-1);

static void *tick_watchdog(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelDelayThread(10 * 1000 * 1000);
        uint64_t started = atomic_load_explicit(&native_ticks_started,
                                                memory_order_relaxed);
        uint64_t completed = atomic_load_explicit(&native_ticks_completed,
                                                  memory_order_relaxed);
        uint64_t swaps = egl_swap_count();
        if (atomic_load_explicit(&native_tick_active, memory_order_relaxed)) {
            uint64_t begin = atomic_load_explicit(&native_tick_started_us,
                                                  memory_order_relaxed);
            uint64_t elapsed = (sceKernelGetProcessTimeWide() - begin) /
                               1000000ULL;
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
        } else {
            l_info("Watchdog: %llu native ticks completed; %llu EGL swaps.",
                   (unsigned long long)completed,
                   (unsigned long long)swaps);
        }
        log_flush();
    }
    return NULL;
}

static void validate_game_data(void) {
    if (!file_exists(SO_PATH)) {
        fatal_error("Missing BTD5 executable:\n%s", SO_PATH);
    }
    if (!file_exists(APK_PATH)) {
        fatal_error("Missing source APK:\n%s", APK_PATH);
    }
    if (!file_exists(DATA_PATH "assets/Assets/BTD5.jet")) {
        fatal_error("Missing BTD5 assets:\n%sassets/Assets/BTD5.jet", DATA_PATH);
    }
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
    native_resume = (BTD5NativeVoid)game_symbol("_Z25MainActivity_nativeResumeP7_JNIEnvP8_jobject");
    native_gained_audio_focus = (BTD5NativeVoid)game_symbol("_Z35MainActivity_nativeGainedAudioFocusP7_JNIEnvP8_jobject");
    native_orientation_changed = (BTD5NativeOrientation)game_symbol("_Z37MainActivity_nativeOrientationChangedP7_JNIEnvP8_jobjecti");
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

int main(void) {
    log_start_session();
    l_success("BTD5 loader 01.22 started.");
    log_flush();

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
    native_surface_created(&jni, NULL, NULL, BTD5_SCREEN_WIDTH, BTD5_SCREEN_HEIGHT);
    l_success("nativeSurfaceCreated returned.");
    native_resize(&jni, NULL, BTD5_SCREEN_WIDTH, BTD5_SCREEN_HEIGHT);
    native_orientation_changed(&jni, NULL, ANDROID_SURFACE_ROTATION_90);
    native_gained_audio_focus(&jni, NULL);
    native_resume(&jni, NULL);
    l_success("Lifecycle setup complete; entering tick loop.");
    log_flush();

    atomic_store_explicit(&main_thread_id, sceKernelGetThreadId(),
                          memory_order_release);
    btd5_diag_bind_current_thread();

    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, tick_watchdog, NULL) == 0) {
        pthread_detach(watchdog);
    } else {
        l_warn("Could not start nativeTick watchdog thread.");
    }

    uint64_t ticks = 0;
    uint64_t last_heartbeat = sceKernelGetProcessTimeWide();
    while (1) {
        controls_poll();
        uint64_t started = atomic_fetch_add_explicit(&native_ticks_started, 1,
                                                     memory_order_relaxed) + 1;
        atomic_store_explicit(&native_tick_started_us,
                              sceKernelGetProcessTimeWide(),
                              memory_order_relaxed);
        atomic_store_explicit(&native_tick_active, true, memory_order_release);
        if (started == 1) {
            l_info("Entering first nativeTick call.");
            log_flush();
        }
        btd5_diag_set_stage(BTD5_STAGE_TICK_ENTER);
        native_tick(&jni, NULL);
        btd5_diag_set_stage(BTD5_STAGE_TICK_RETURNED);
        atomic_store_explicit(&native_tick_active, false, memory_order_release);
        atomic_fetch_add_explicit(&native_ticks_completed, 1,
                                  memory_order_relaxed);
        ticks++;

        if (ticks == 1) {
            l_success("First nativeTick call returned.");
            log_flush();
        }

        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - last_heartbeat >= 10ULL * 1000ULL * 1000ULL) {
            l_info("Tick loop alive: %llu calls.", (unsigned long long)ticks);
            log_flush();
            last_heartbeat = now;
        }
    }

    return sceKernelExitDeleteThread(0);
}
