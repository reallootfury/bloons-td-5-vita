/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "reimpl/ui_context.h"
#include "utils/logger.h"
#include "utils/glutil.h"
#include "utils/settings.h"

#include <math.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f
#define LOW_GRAPHICS_TILE_X   700.0f
#define LOW_GRAPHICS_TILE_Y   360.0f
#define LOW_GRAPHICS_TILE_W   250.0f
#define LOW_GRAPHICS_TILE_H   165.0f
#define TOUCH_SAMPLE_QUEUE_CAPACITY 256U
#define TOUCH_SAMPLER_THREAD_STACK  (64U * 1024U)
#define TOUCH_BATCH_PHASE_LIMIT            2U
#define TOUCH_PICKUP_RETRY_FRAMES          2U
#define TOUCH_QUICK_DRAG_RETRY_FRAMES      8U
#define TOUCH_QUICK_DRAG_RETRY_US          UINT64_C(500000)
#define TOUCH_GAME_SOURCE_STRIP_FRACTION   0.80f

static void reset_cursor_for_surface(void);
static void hide_cursor_for_front_touch(void);

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }

    // normalize
    *x = *x / magnitude;
    *y = *y / magnitude;

    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

void controls_init() {
    // Enable analog sticks and touchscreen
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);

    // Enable accelerometer
    sceMotionStartSampling();

    ui_context_init();
    reset_cursor_for_surface();

    l_info("Vita controls: sticks=free cursor, "
           "Cross=activate/touch, R=drag, Circle=cancel/back, "
           "Start=pause/back (hold=clean exit), L=precision, "
           "Triangle=recenter, Select in Settings=Low Graphics; "
           "cursor hides after 5 seconds idle.");
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

static bool cancel_active_touches(void);
static void cancel_synthetic_touches_for_front_touch(void);
static void handle_ui_screen_change(void);
static void toggle_low_graphics(void);

static bool low_graphics_tile_hit(float x, float y) {
    if (ui_context_current() != UI_CONTEXT_SETTINGS) {
        return false;
    }
    float scale_x = (float)settings_render_width() / 960.0f;
    float scale_y = (float)settings_render_height() / 544.0f;
    float left = LOW_GRAPHICS_TILE_X * scale_x;
    float top = LOW_GRAPHICS_TILE_Y * scale_y;
    return x >= left && x < left + LOW_GRAPHICS_TILE_W * scale_x &&
           y >= top && y < top + LOW_GRAPHICS_TILE_H * scale_y;
}

void controls_poll() {
    if (ui_context_update()) {
        handle_ui_screen_change();
    }
    poll_touch();
    cancel_synthetic_touches_for_front_touch();
    poll_pad();
    //poll_accel();
}

SceTouchData touch;
SceTouchData touch_old;
static unsigned char cancelled_touch_ids[256];

#define TOUCH_DRAG_CANCEL_DISTANCE 14.0f

typedef struct TouchQueuedSample {
    SceTouchData data;
    bool coalescible;
} TouchQueuedSample;

typedef struct TouchGestureState {
    bool active;
    bool started_in_borders;
    bool started_in_game_source_strip;
    bool placement_seen;
    bool moved;
    float start_x;
    float start_y;
} TouchGestureState;

enum TouchProcessFlags {
    TOUCH_PROCESS_NONE = 0,
    TOUCH_PROCESS_DOWN = 1U << 0,
    TOUCH_PROCESS_MOVE = 1U << 1,
    TOUCH_PROCESS_UP = 1U << 2,
};

static TouchQueuedSample touch_sample_queue[TOUCH_SAMPLE_QUEUE_CAPACITY];
static size_t touch_sample_read_index = 0;
static size_t touch_sample_write_index = 0;
static size_t touch_sample_count = 0;
static pthread_mutex_t touch_sample_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool touch_sampler_started = ATOMIC_VAR_INIT(false);
static atomic_uint touch_sample_overflows = ATOMIC_VAR_INIT(0);
static atomic_uint touch_samples_coalesced = ATOMIC_VAR_INIT(0);
static bool front_touch_activity_this_poll = false;
static TouchGestureState touch_gestures[256];
static bool deferred_touch_valid = false;
static SceTouchData deferred_touch;
static unsigned int deferred_release_wait_frames = 0;
static uint64_t deferred_release_queued_us = 0;
static atomic_uint touch_fast_batches = ATOMIC_VAR_INIT(0);
static atomic_uint touch_release_holds = ATOMIC_VAR_INIT(0);
static atomic_bool touch_immediate_down[256];
static atomic_uint touch_immediate_down_count = ATOMIC_VAR_INIT(0);

static unsigned int touch_id_slot(int id) {
    return (unsigned int)id & 0xffU;
}

static float touch_screen_x(const SceTouchReport *report) {
    return (float)report->x * (float)settings_render_width() / 1920.0f;
}

static float touch_screen_y(const SceTouchReport *report) {
    return (float)report->y * (float)settings_render_height() / 1088.0f;
}

static bool touch_is_game_source_strip(float x) {
    return x >= (float)settings_render_width() *
                TOUCH_GAME_SOURCE_STRIP_FRACTION;
}

static bool touch_topology_equal(const SceTouchData *left,
                                 const SceTouchData *right) {
    if (left->reportNum != right->reportNum) {
        return false;
    }
    for (int i = 0; i < left->reportNum; ++i) {
        if (left->report[i].id != right->report[i].id) {
            return false;
        }
    }
    return true;
}

static bool touch_reports_equal(const SceTouchData *left,
                                const SceTouchData *right) {
    if (!touch_topology_equal(left, right)) {
        return false;
    }
    for (int i = 0; i < left->reportNum; ++i) {
        if (left->report[i].x != right->report[i].x ||
            left->report[i].y != right->report[i].y ||
            left->report[i].force != right->report[i].force) {
            return false;
        }
    }
    return true;
}

static void touch_sample_push(const SceTouchData *sample, bool coalescible) {
    pthread_mutex_lock(&touch_sample_mutex);

    if (touch_sample_count != 0) {
        size_t last_index =
            (touch_sample_write_index + TOUCH_SAMPLE_QUEUE_CAPACITY - 1U) %
            TOUCH_SAMPLE_QUEUE_CAPACITY;
        TouchQueuedSample *last = &touch_sample_queue[last_index];

        if (touch_reports_equal(&last->data, sample)) {
            pthread_mutex_unlock(&touch_sample_mutex);
            return;
        }

        /* A drag path does not need every hardware sample. Preserve the first
         * DOWN and the final UP, but continuously replace the queued held
         * sample with the newest coordinates. This prevents a 13 FPS game
         * thread from replaying seconds of stale movement after the finger is
         * already elsewhere. */
        if (coalescible && last->coalescible &&
            touch_topology_equal(&last->data, sample)) {
            sceClibMemcpy(&last->data, sample, sizeof(*sample));
            atomic_fetch_add_explicit(&touch_samples_coalesced, 1,
                                      memory_order_relaxed);
            pthread_mutex_unlock(&touch_sample_mutex);
            return;
        }
    }

    if (touch_sample_count == TOUCH_SAMPLE_QUEUE_CAPACITY) {
        /* This should be unreachable after movement coalescing. Drop the
         * oldest semantic sample rather than delaying current input by
         * several seconds, and expose the event in loader.log. */
        touch_sample_read_index =
            (touch_sample_read_index + 1U) % TOUCH_SAMPLE_QUEUE_CAPACITY;
        touch_sample_count--;
        atomic_fetch_add_explicit(&touch_sample_overflows, 1,
                                  memory_order_relaxed);
    }

    TouchQueuedSample *queued =
        &touch_sample_queue[touch_sample_write_index];
    sceClibMemcpy(&queued->data, sample, sizeof(*sample));
    queued->coalescible = coalescible;
    touch_sample_write_index =
        (touch_sample_write_index + 1U) % TOUCH_SAMPLE_QUEUE_CAPACITY;
    touch_sample_count++;
    pthread_mutex_unlock(&touch_sample_mutex);
}

static bool touch_sample_pop(SceTouchData *sample) {
    bool available = false;
    pthread_mutex_lock(&touch_sample_mutex);
    if (touch_sample_count != 0) {
        sceClibMemcpy(sample,
                      &touch_sample_queue[touch_sample_read_index].data,
                      sizeof(*sample));
        touch_sample_read_index =
            (touch_sample_read_index + 1U) % TOUCH_SAMPLE_QUEUE_CAPACITY;
        touch_sample_count--;
        available = true;
    }
    pthread_mutex_unlock(&touch_sample_mutex);
    return available;
}

static void touch_sample_drop_pending(void) {
    pthread_mutex_lock(&touch_sample_mutex);
    touch_sample_read_index = 0;
    touch_sample_write_index = 0;
    touch_sample_count = 0;
    pthread_mutex_unlock(&touch_sample_mutex);
    deferred_touch_valid = false;
    deferred_release_wait_frames = 0;
    deferred_release_queued_us = 0;
    for (unsigned int i = 0; i < 256U; ++i) {
        atomic_store_explicit(&touch_immediate_down[i], false,
                              memory_order_relaxed);
    }
}

static bool touch_sample_contains_id(const SceTouchData *sample, int id) {
    for (int i = 0; i < sample->reportNum; ++i) {
        if (sample->report[i].id == id) return true;
    }
    return false;
}

static void touch_dispatch_immediate_game_down(const SceTouchData *previous,
                                               bool have_previous,
                                               const SceTouchData *sample) {
    /* A quick spike/tower gesture can begin and end during one 50-80 ms late
     * frame. Deliver only the initial DOWN from the blocking sampler thread,
     * matching Android's separate UI/input thread. MOVE and UP stay in the
     * semantic queue so their ordering and placement-screen safety remain on
     * the game frame thread. Restrict this to the live game border screen. */
    if (!ui_context_is_ingame_borders()) return;

    for (int i = 0; i < sample->reportNum; ++i) {
        const SceTouchReport *report = &sample->report[i];
        if (have_previous && touch_sample_contains_id(previous, report->id)) {
            continue;
        }
        unsigned int slot = touch_id_slot(report->id);
        float x = touch_screen_x(report);
        float y = touch_screen_y(report);
        atomic_store_explicit(&touch_immediate_down[slot], true,
                              memory_order_release);
        controls_handler_touch(report->id, x, y, CONTROLS_ACTION_DOWN);
        atomic_fetch_add_explicit(&touch_immediate_down_count, 1,
                                  memory_order_relaxed);
    }
}

static void *touch_sampler_main(void *unused) {
    (void)unused;
    SceTouchData previous = {0};
    bool have_previous = false;

    for (;;) {
        SceTouchData sample;
        int result = sceTouchRead(SCE_TOUCH_PORT_FRONT, &sample, 1);
        if (result > 0) {
            bool topology_changed = !have_previous ||
                !touch_topology_equal(&previous, &sample);
            if (!have_previous || !touch_reports_equal(&previous, &sample)) {
                touch_dispatch_immediate_game_down(&previous, have_previous,
                                                   &sample);
                touch_sample_push(&sample, !topology_changed);
                sceClibMemcpy(&previous, &sample, sizeof(previous));
                have_previous = true;
            }
        } else {
            sceKernelDelayThread(1000);
        }
    }
    return NULL;
}

void controls_start_touch_sampler(void) {
    if (atomic_load_explicit(&touch_sampler_started, memory_order_acquire)) {
        return;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int stack_result = pthread_attr_setstacksize(
        &attr, TOUCH_SAMPLER_THREAD_STACK);
    if (stack_result != 0) {
        l_warn("Could not set touch sampler stack to %u KiB (pthread %d); "
               "using the default.", TOUCH_SAMPLER_THREAD_STACK / 1024U,
               stack_result);
    }

    pthread_t thread;
    int result = pthread_create(&thread, &attr, touch_sampler_main, NULL);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        l_warn("Could not start blocking touch sampler (pthread %d); "
               "falling back to once-per-frame touch polling.", result);
        return;
    }

    pthread_detach(thread);
    atomic_store_explicit(&touch_sampler_started, true, memory_order_release);
    l_info("Immediate in-game touch pickup enabled: the blocking sampler "
           "delivers DOWN before a stalled nativeTick; newest MOVE remains "
           "coalesced and UP is frame-ordered with %u map retries or "
           "%u/%u ms source-strip retries.",
           TOUCH_PICKUP_RETRY_FRAMES, TOUCH_QUICK_DRAG_RETRY_FRAMES,
           (unsigned int)(TOUCH_QUICK_DRAG_RETRY_US / 1000U));
}

static unsigned int touch_transition_flags(const SceTouchData *sampled_touch) {
    unsigned int flags = TOUCH_PROCESS_NONE;

    for (int i = 0; i < sampled_touch->reportNum; ++i) {
        int old_index = -1;
        for (int j = 0; j < touch_old.reportNum; ++j) {
            if (sampled_touch->report[i].id == touch_old.report[j].id) {
                old_index = j;
                break;
            }
        }
        if (old_index < 0) {
            flags |= TOUCH_PROCESS_DOWN;
        } else if (sampled_touch->report[i].x != touch_old.report[old_index].x ||
                   sampled_touch->report[i].y != touch_old.report[old_index].y) {
            flags |= TOUCH_PROCESS_MOVE;
        }
    }

    for (int i = 0; i < touch_old.reportNum; ++i) {
        bool still_present = false;
        for (int j = 0; j < sampled_touch->reportNum; ++j) {
            if (touch_old.report[i].id == sampled_touch->report[j].id) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            flags |= TOUCH_PROCESS_UP;
        }
    }

    return flags;
}

static bool touch_has_unclaimed_game_drag(bool *has_source_strip_drag) {
    if (has_source_strip_drag) {
        *has_source_strip_drag = false;
    }
    if (!ui_context_is_ingame_borders()) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < touch_old.reportNum; ++i) {
        TouchGestureState *gesture =
            &touch_gestures[touch_id_slot(touch_old.report[i].id)];
        gesture->placement_seen |= ui_context_is_tower_placement();
        if (gesture->active && gesture->started_in_borders &&
            gesture->moved && !gesture->placement_seen) {
            found = true;
            if (has_source_strip_drag &&
                gesture->started_in_game_source_strip) {
                *has_source_strip_drag = true;
            }
        }
    }
    return found;
}

static void touch_replay_unclaimed_game_drag(void) {
    /* BTD5 may need another held event before it changes from InGameBorders to
     * TowerPlacementScreen. A very quick physical DOWN/MOVE/UP can otherwise
     * finish entirely inside one 50-100 ms nativeTick. Replaying only the
     * newest MOVE keeps the original pointer identity and does not invent a
     * second tap. */
    for (int i = 0; i < touch_old.reportNum; ++i) {
        const SceTouchReport *report = &touch_old.report[i];
        TouchGestureState *gesture =
            &touch_gestures[touch_id_slot(report->id)];
        if (!gesture->active || !gesture->started_in_borders ||
            !gesture->moved || gesture->placement_seen) {
            continue;
        }
        controls_handler_touch(report->id, touch_screen_x(report),
                               touch_screen_y(report),
                               CONTROLS_ACTION_MOVE);
    }
}

static unsigned int process_touch_sample(const SceTouchData *sampled_touch) {
    unsigned int flags = TOUCH_PROCESS_NONE;
    front_touch_activity_this_poll |=
        sampled_touch->reportNum > 0 || touch_old.reportNum > 0;
    sceClibMemcpy(&touch, sampled_touch, sizeof(touch));

    for (int i = 0; i < touch.reportNum; i++) {
        float x = touch_screen_x(&touch.report[i]);
        float y = touch_screen_y(&touch.report[i]);

        int old_finger_index = -1;
        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    old_finger_index = j;
                    break;
                }
            }
        }

        unsigned int slot = touch_id_slot(touch.report[i].id);
        TouchGestureState *gesture = &touch_gestures[slot];
        if (old_finger_index < 0) {
            gesture->active = true;
            gesture->started_in_borders = ui_context_is_ingame_borders();
            gesture->started_in_game_source_strip =
                gesture->started_in_borders && touch_is_game_source_strip(x);
            gesture->placement_seen = ui_context_is_tower_placement();
            gesture->moved = false;
            gesture->start_x = x;
            gesture->start_y = y;
            flags |= TOUCH_PROCESS_DOWN;

            bool down_already_dispatched = atomic_exchange_explicit(
                &touch_immediate_down[slot], false, memory_order_acq_rel);
            if (low_graphics_tile_hit(x, y)) {
                cancelled_touch_ids[slot] = 1;
                if (down_already_dispatched) {
                    controls_handler_touch(touch.report[i].id, x, y,
                                           CONTROLS_ACTION_CANCEL);
                }
                toggle_low_graphics();
            } else {
                cancelled_touch_ids[slot] = 0;
                if (!down_already_dispatched) {
                    controls_handler_touch(touch.report[i].id, x, y,
                                           CONTROLS_ACTION_DOWN);
                }
            }
        } else if (!cancelled_touch_ids[slot] &&
                   (touch.report[i].x !=
                        touch_old.report[old_finger_index].x ||
                    touch.report[i].y !=
                        touch_old.report[old_finger_index].y)) {
            float dx = x - gesture->start_x;
            float dy = y - gesture->start_y;
            if (dx * dx + dy * dy >=
                TOUCH_DRAG_CANCEL_DISTANCE * TOUCH_DRAG_CANCEL_DISTANCE) {
                gesture->moved = true;
            }
            gesture->placement_seen |= ui_context_is_tower_placement();
            controls_handler_touch(touch.report[i].id, x, y,
                                   CONTROLS_ACTION_MOVE);
            flags |= TOUCH_PROCESS_MOVE;
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;
        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = touch_screen_x(&touch_old.report[i]);
            float y = touch_screen_y(&touch_old.report[i]);
            unsigned int slot = touch_id_slot(touch_old.report[i].id);
            TouchGestureState *gesture = &touch_gestures[slot];

            if (!cancelled_touch_ids[slot]) {
                gesture->placement_seen |= ui_context_is_tower_placement();
                if (gesture->active && gesture->started_in_borders &&
                    gesture->moved && !gesture->placement_seen &&
                    ui_context_is_ingame_borders()) {
                    if (gesture->started_in_game_source_strip) {
                        /* The gesture began in BTD5's right-side tower/special
                         * source strip. Even if a very slow frame hid the brief
                         * TowerPlacementScreen transition from ui_context, the
                         * correct semantic end is a placement release, not a
                         * cancellation. This is the fast pineapple/spike path. */
                        controls_handler_touch(touch_old.report[i].id, x, y,
                                               CONTROLS_ACTION_UP);
                        l_info("Completed rapid source-strip drag at %.1f,%.1f "
                               "after held-event pickup retries.", x, y);
                    } else {
                        /* A map-origin drag was never claimed as placement.
                         * ACTION_UP can be reinterpreted as a tap and select a
                         * nearby farm/UI item, so preserve the proven safety
                         * cancellation for non-source gestures. */
                        controls_handler_touch(touch_old.report[i].id, x, y,
                                               CONTROLS_ACTION_CANCEL);
                        l_warn("Cancelled unclaimed game drag at %.1f,%.1f; "
                               "prevented release from becoming a farm/UI tap.",
                               x, y);
                    }
                } else {
                    controls_handler_touch(touch_old.report[i].id, x, y,
                                           CONTROLS_ACTION_UP);
                }
            }
            cancelled_touch_ids[slot] = 0;
            sceClibMemset(gesture, 0, sizeof(*gesture));
            flags |= TOUCH_PROCESS_UP;
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
    return flags;
}

void poll_touch() {
    front_touch_activity_this_poll = false;

    if (atomic_load_explicit(&touch_sampler_started, memory_order_acquire)) {
        /* A release captured during a low-FPS frame is never delivered in the
         * same nativeTick as its DOWN/MOVE. Map-origin drags retain the proven
         * two-pass cancellation safety. Drags beginning in BTD5's right-side
         * tower/special strip replay the newest held MOVE for up to eight passes
         * (bounded to 500 ms), giving the closed-source input state time to enter
         * TowerPlacementScreen before the physical UP is delivered. */
        if (deferred_touch_valid) {
            bool has_source_strip_drag = false;
            bool unclaimed = touch_has_unclaimed_game_drag(
                &has_source_strip_drag);
            if (unclaimed) {
                uint64_t now_us = sceKernelGetProcessTimeWide();
                unsigned int retry_limit = has_source_strip_drag ?
                    TOUCH_QUICK_DRAG_RETRY_FRAMES :
                    TOUCH_PICKUP_RETRY_FRAMES;
                bool within_time_limit = !has_source_strip_drag ||
                    deferred_release_queued_us == 0 ||
                    now_us - deferred_release_queued_us <
                        TOUCH_QUICK_DRAG_RETRY_US;
                if (deferred_release_wait_frames < retry_limit &&
                    within_time_limit) {
                    touch_replay_unclaimed_game_drag();
                    ++deferred_release_wait_frames;
                    atomic_fetch_add_explicit(&touch_release_holds, 1,
                                              memory_order_relaxed);
                    return;
                }
            }

            (void)process_touch_sample(&deferred_touch);
            deferred_touch_valid = false;
            deferred_release_wait_frames = 0;
            deferred_release_queued_us = 0;
            return;
        }

        SceTouchData sampled_touch;
        unsigned int delivered_phases = 0;
        while (touch_sample_pop(&sampled_touch)) {
            unsigned int transition = touch_transition_flags(&sampled_touch);

            /* Preserve the release for the next game update.  DOWN followed
             * by the newest MOVE may be batched into this frame, which removes
             * the previous two-frame pickup latency at 10-15 FPS. */
            if (transition & TOUCH_PROCESS_UP) {
                sceClibMemcpy(&deferred_touch, &sampled_touch,
                              sizeof(deferred_touch));
                deferred_touch_valid = true;
                deferred_release_wait_frames = 0;
                deferred_release_queued_us = sceKernelGetProcessTimeWide();
                break;
            }

            unsigned int processed = process_touch_sample(&sampled_touch);
            if (processed != TOUCH_PROCESS_NONE) {
                ++delivered_phases;
                if (delivered_phases >= TOUCH_BATCH_PHASE_LIMIT) {
                    atomic_fetch_add_explicit(&touch_fast_batches, 1,
                                              memory_order_relaxed);
                    break;
                }
            }
        }

        static unsigned int logged_overflows = 0;
        unsigned int overflows = atomic_load_explicit(
            &touch_sample_overflows, memory_order_relaxed);
        if (overflows != logged_overflows) {
            l_warn("Semantic touch queue overflowed %u times; this now means "
                   "more than %u distinct touch transitions accumulated.",
                   overflows, TOUCH_SAMPLE_QUEUE_CAPACITY);
            logged_overflows = overflows;
        }
        return;
    }

    SceTouchData sampled_touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &sampled_touch, 1) <= 0) {
        /* Preserve the previous successful sample. Treating a failed read as
         * zero contacts would emit a false UP and place the dragged tower. */
        return;
    }
    (void)process_touch_sample(&sampled_touch);
}

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

float analog_lx[3] = { 0 };
float analog_ly[3] = { 0 };
float analog_rx[3] = { 0 };
float analog_ry[3] = { 0 };

static float cursor_x = 480.0f;
static float cursor_y = 272.0f;
static bool cursor_visible = true;
static bool cursor_down = false;
static bool circle_sent_back = false;
static bool cursor_buttons_suppressed = false;
static uint64_t last_pad_poll_us = 0;
static uint64_t cursor_last_activity_us = 0;
static uint64_t low_graphics_notice_until_us = 0;
static uint64_t start_pressed_at_us = 0;
static bool start_exit_triggered = false;

static void hide_cursor_for_front_touch(void) {
    /* Direct touch users do not need the loader cursor. Hiding it immediately
     * also removes the final scissor/clear overlay and the resulting full GL
     * state-cache invalidation from touch-heavy gameplay frames. Any later
     * stick movement calls mark_cursor_activity() and shows it again. */
    cursor_visible = false;
    cursor_last_activity_us = 0;
}

#define VIRTUAL_CURSOR_TOUCH_ID 0x7fff
#define CURSOR_ANALOG_SPEED 12.0f
#define CURSOR_RIGHT_ANALOG_SPEED 14.0f
#define CURSOR_PRECISION_SCALE 0.35f
#define CURSOR_TOUCH_BUTTONS (SCE_CTRL_CROSS | SCE_CTRL_R1)
#define CURSOR_IDLE_TIMEOUT_US UINT64_C(5000000)
#define START_EXIT_HOLD_US     UINT64_C(1500000)

static float clampf(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void mark_cursor_activity(uint64_t now_us) {
    cursor_visible = true;
    cursor_last_activity_us = now_us != 0 ? now_us : 1;
}

static void reset_cursor_for_surface(void) {
    cursor_x = (float)settings_render_width() * 0.5f;
    cursor_y = (float)settings_render_height() * 0.5f;
    mark_cursor_activity(sceKernelGetProcessTimeWide());
}

static bool low_graphics_cursor_hit(void) {
    return cursor_visible && low_graphics_tile_hit(cursor_x, cursor_y);
}

static void toggle_low_graphics(void) {
    setting_low_graphics = !setting_low_graphics;
    settings_save();
    low_graphics_notice_until_us =
        sceKernelGetProcessTimeWide() + UINT64_C(4000000);
    l_info("Low Graphics preference: %s (active this launch: %s)%s.",
           setting_low_graphics ? "ON" : "OFF",
           settings_low_graphics_applied() ? "ON" : "OFF",
           setting_low_graphics != settings_low_graphics_applied()
               ? "; restart required" : "");
}

static bool cancel_active_touches(void) {
    bool cancelled_any = false;

    for (int i = 0; i < touch.reportNum; i++) {
        unsigned int slot = touch_id_slot(touch.report[i].id);
        if (cancelled_touch_ids[slot]) {
            continue;
        }

        float x = (float)touch.report[i].x *
                  (float)settings_render_width() / 1920.0f;
        float y = (float)touch.report[i].y *
                  (float)settings_render_height() / 1088.0f;
        controls_handler_touch(touch.report[i].id, x, y,
                               CONTROLS_ACTION_CANCEL);
        cancelled_touch_ids[slot] = 1;
        cancelled_any = true;
    }

    if (cursor_down) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_CANCEL);
        cursor_down = false;
        cancelled_any = true;
    }

    return cancelled_any;
}

/* Direct front-touch input owns the game's touch stream. If a finger lands
 * while a pad gesture is held, cancel only the synthetic contact and leave
 * the newly sampled physical contact active. */
static void cancel_synthetic_touches_for_front_touch(void) {
    if (!front_touch_activity_this_poll && touch.reportNum <= 0) {
        return;
    }
    hide_cursor_for_front_touch();
    if (cursor_down) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_CANCEL);
        cursor_down = false;
        cursor_buttons_suppressed = true;
    }
}

/* Most native screen changes are touch epoch boundaries. Tower selection is
 * different: BTD5 changes InGameBorders to TowerPlacementScreen in response
 * to the original DOWN, then expects MOVE/UP from that same pointer. Cancelling
 * there makes the tower tray blink and aborts every monkey/spike/pineapple
 * placement. ui_context.c identifies only that proven continuation pair. */
static void handle_ui_screen_change(void) {
    if (!ui_context_touch_cancel_required()) {
        if (ui_context_is_tower_placement()) {
            for (int i = 0; i < touch.reportNum; ++i) {
                touch_gestures[touch_id_slot(touch.report[i].id)].placement_seen = true;
            }
        }
        if (touch.reportNum > 0 || cursor_down) {
            l_info("Preserving active touch across BTD5 placement screen transition.");
        }
        return;
    }

    (void)cancel_active_touches();
    /* Events sampled for the previous screen must not be replayed into the
     * newly opened menu or popup. A currently held finger remains suppressed
     * until its physical UP arrives. */
    touch_sample_drop_pending();
    if (current_buttons & CURSOR_TOUCH_BUTTONS) {
        cursor_buttons_suppressed = true;
    }
}

void poll_pad() {
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) <= 0) {
        /* Do not turn a failed controller sample into a Cross release. */
        return;
    }

    // Gamepad buttons
    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    uint64_t now_us = sceKernelGetProcessTimeWide();
    /* Circle cancels an in-progress physical or virtual drag. With no active
     * contact it retains Android's normal Back behaviour. Suppressed physical
     * contacts stay ignored until the finger is lifted, preventing a stale UP
     * event from placing a tower after cancellation. */
    if (pressed_buttons & SCE_CTRL_CIRCLE) {
        circle_sent_back = !cancel_active_touches();
        if (current_buttons & CURSOR_TOUCH_BUTTONS) {
            cursor_buttons_suppressed = true;
        }
        if (circle_sent_back) {
            controls_handler_key(AKEYCODE_BACK, CONTROLS_ACTION_DOWN);
        }
    }
    if (released_buttons & SCE_CTRL_CIRCLE) {
        if (circle_sent_back) {
            controls_handler_key(AKEYCODE_BACK, CONTROLS_ACTION_UP);
        }
        circle_sent_back = false;
    }

    /* A short Start press remains Android Back (pause/resume/menu). Holding it
     * requests a clean application exit without first opening another screen. */
    if (pressed_buttons & SCE_CTRL_START) {
        (void)cancel_active_touches();
        if (current_buttons & CURSOR_TOUCH_BUTTONS) {
            cursor_buttons_suppressed = true;
        }
        start_pressed_at_us = now_us;
        start_exit_triggered = false;
    }
    if ((current_buttons & SCE_CTRL_START) && start_pressed_at_us != 0 &&
        !start_exit_triggered &&
        now_us - start_pressed_at_us >= START_EXIT_HOLD_US) {
        start_exit_triggered = true;
        controls_handler_exit_request();
    }
    if (released_buttons & SCE_CTRL_START) {
        uint64_t held_us = start_pressed_at_us != 0 && now_us >= start_pressed_at_us
            ? now_us - start_pressed_at_us : 0;
        if (!start_exit_triggered && held_us >= START_EXIT_HOLD_US) {
            start_exit_triggered = true;
            controls_handler_exit_request();
        } else if (!start_exit_triggered) {
            controls_handler_key(AKEYCODE_BACK, CONTROLS_ACTION_DOWN);
            controls_handler_key(AKEYCODE_BACK, CONTROLS_ACTION_UP);
        }
        start_pressed_at_us = 0;
    }

    bool touch_button_was_down =
        (old_buttons & CURSOR_TOUCH_BUTTONS) != 0;
    bool touch_button_is_down =
        (current_buttons & CURSOR_TOUCH_BUTTONS) != 0;

    /* Finish synthetic contacts at their old coordinates before applying any
     * movement from this controller sample. During a long frame, the old
     * implementation moved first and could turn a release into an unintended
     * tower placement several cursor-speeds away. */
    if (touch_button_was_down && !touch_button_is_down && cursor_down) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_UP);
        cursor_down = false;
        mark_cursor_activity(now_us);
    }
    if (!touch_button_is_down) {
        cursor_buttons_suppressed = false;
    }
    /* Low Graphics belongs to BTD5's Settings screens. Both the main-menu and
     * in-game Settings panels classify as UI_CONTEXT_SETTINGS; Select is
     * deliberately ignored everywhere else. */
    if ((pressed_buttons & SCE_CTRL_SELECT) &&
        ui_context_current() == UI_CONTEXT_SETTINGS) {
        (void)cancel_active_touches();
        if (current_buttons & CURSOR_TOUCH_BUTTONS) {
            cursor_buttons_suppressed = true;
        }
        toggle_low_graphics();
    }

    // Analog sticks. Either stick can drive the touch cursor.
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);

    float frame_scale = 1.0f;
    if (last_pad_poll_us != 0 && now_us > last_pad_poll_us) {
        frame_scale = clampf((float)(now_us - last_pad_poll_us) / 16667.0f,
                             0.25f, 4.0f);
    }
    last_pad_poll_us = now_us;

    bool analog_active = analog_lx[0] != 0.0f || analog_ly[0] != 0.0f ||
                         analog_rx[0] != 0.0f || analog_ry[0] != 0.0f;

    bool moved = false;
    /* Either analog stick moves the virtual touch pointer. */
    float dx = analog_lx[0] * CURSOR_ANALOG_SPEED +
               analog_rx[0] * CURSOR_RIGHT_ANALOG_SPEED;
    float dy = analog_ly[0] * CURSOR_ANALOG_SPEED +
               analog_ry[0] * CURSOR_RIGHT_ANALOG_SPEED;

    float movement_scale = frame_scale *
        ((float)settings_render_width() / 960.0f);
    if (current_buttons & SCE_CTRL_L1) {
        movement_scale *= CURSOR_PRECISION_SCALE;
    }
    dx *= movement_scale;
    dy *= movement_scale;

    bool pointer_moved = dx != 0.0f || dy != 0.0f;
    if (pointer_moved) {
        cursor_x = clampf(cursor_x + dx, 8.0f,
                          (float)settings_render_width() - 9.0f);
        cursor_y = clampf(cursor_y + dy, 8.0f,
                          (float)settings_render_height() - 9.0f);
        mark_cursor_activity(now_us);
        moved = true;
    }

    if ((pressed_buttons & SCE_CTRL_TRIANGLE) && !cursor_down &&
        !touch_button_is_down) {
        cursor_x = (float)settings_render_width() * 0.5f;
        cursor_y = (float)settings_render_height() * 0.5f;
        mark_cursor_activity(now_us);
    }

    /* Cross and R are aliases for one virtual finger. R remains identical at
     * the touch layer so it can drag the village, map cards, sliders, and
     * towers. */
    if (!touch_button_was_down && touch_button_is_down) {
        mark_cursor_activity(now_us);
        if (low_graphics_cursor_hit()) {
            /* Cross toggles; R is suppressed because the synthetic tile is
             * not a native game hitbox and must never start a drag. */
            if ((pressed_buttons & SCE_CTRL_CROSS) &&
                !cursor_buttons_suppressed && touch.reportNum == 0 &&
                !cursor_down) {
                toggle_low_graphics();
            }
            cursor_buttons_suppressed = true;
        } else if (!cursor_buttons_suppressed && touch.reportNum == 0 &&
            !cursor_down) {
            cursor_down = true;
            controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x,
                                   cursor_y, CONTROLS_ACTION_DOWN);
        } else {
            cursor_buttons_suppressed = true;
        }
    } else if (touch_button_was_down && touch_button_is_down &&
               cursor_down && moved) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_MOVE);
    }

    if (cursor_buttons_suppressed && !touch_button_is_down) {
        cursor_buttons_suppressed = false;
    }
    if (analog_active) {
        mark_cursor_activity(now_us);
    }
}

typedef struct OverlayGlyph {
    char character;
    unsigned char rows[5];
} OverlayGlyph;

/* Compact 3x5 capitals keep the loader-owned setting readable without
 * introducing a font renderer or touching BTD5's own texture state. */
static const OverlayGlyph overlay_glyphs[] = {
    { 'A', { 2, 5, 7, 5, 5 } },
    { 'C', { 7, 4, 4, 4, 7 } },
    { 'E', { 7, 4, 6, 4, 7 } },
    { 'F', { 7, 4, 6, 4, 4 } },
    { 'G', { 7, 4, 5, 5, 7 } },
    { 'H', { 5, 5, 7, 5, 5 } },
    { 'I', { 7, 2, 2, 2, 7 } },
    { 'L', { 4, 4, 4, 4, 7 } },
    { 'N', { 5, 7, 7, 5, 5 } },
    { 'O', { 7, 5, 5, 5, 7 } },
    { 'P', { 6, 5, 6, 4, 4 } },
    { 'R', { 6, 5, 6, 5, 5 } },
    { 'S', { 7, 4, 7, 1, 7 } },
    { 'T', { 7, 2, 2, 2, 2 } },
    { 'W', { 5, 5, 5, 7, 5 } },
};

static const OverlayGlyph *overlay_glyph(char character) {
    for (size_t i = 0;
         i < sizeof(overlay_glyphs) / sizeof(overlay_glyphs[0]); ++i) {
        if (overlay_glyphs[i].character == character) {
            return &overlay_glyphs[i];
        }
    }
    return NULL;
}

static void overlay_rect_top(int x, int y, int width, int height) {
    int surface_width = settings_render_width();
    int surface_height = settings_render_height();
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > surface_width) width = surface_width - x;
    if (y + height > surface_height) height = surface_height - y;
    if (width <= 0 || height <= 0) {
        return;
    }
    glScissor(x, surface_height - y - height, width, height);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void overlay_text(const char *text, int x, int y, int scale) {
    for (const char *cursor = text; *cursor; ++cursor, x += 4 * scale) {
        const OverlayGlyph *glyph = overlay_glyph(*cursor);
        if (!glyph) {
            continue;
        }
        for (int row = 0; row < 5; ++row) {
            int column = 0;
            while (column < 3) {
                if ((glyph->rows[row] & (4U >> column)) == 0) {
                    ++column;
                    continue;
                }
                int first = column;
                while (column < 3 &&
                       (glyph->rows[row] & (4U >> column)) != 0) {
                    ++column;
                }
                overlay_rect_top(x + first * scale, y + row * scale,
                                 (column - first) * scale, scale);
            }
        }
    }
}

static void draw_low_graphics_tile(void) {
    float scale_x = (float)settings_render_width() / 960.0f;
    float scale_y = (float)settings_render_height() / 544.0f;
    int x = (int)(LOW_GRAPHICS_TILE_X * scale_x + 0.5f);
    int y = (int)(LOW_GRAPHICS_TILE_Y * scale_y + 0.5f);
    int width = (int)(LOW_GRAPHICS_TILE_W * scale_x + 0.5f);
    int height = (int)(LOW_GRAPHICS_TILE_H * scale_y + 0.5f);
    int border = settings_render_width() < 960 ? 2 : 3;

    if (low_graphics_cursor_hit()) {
        glClearColor(1.0f, 0.82f, 0.05f, 1.0f);
    } else {
        glClearColor(0.35f, 0.40f, 0.46f, 1.0f);
    }
    overlay_rect_top(x, y, width, height);
    glClearColor(0.04f, 0.09f, 0.14f, 1.0f);
    overlay_rect_top(x + border, y + border, width - border * 2,
                     height - border * 2);

    int text_scale = 2;
    int text_x = x + (int)(14.0f * scale_x + 0.5f);
    int text_y = y + (int)(18.0f * scale_y + 0.5f);
    glClearColor(0.95f, 0.98f, 1.0f, 1.0f);
    overlay_text("LOW GRAPHICS", text_x, text_y, text_scale);

    if (setting_low_graphics) {
        glClearColor(0.15f, 1.0f, 0.32f, 1.0f);
        overlay_text("ON", text_x,
                     y + (int)(62.0f * scale_y + 0.5f), text_scale);
    } else {
        glClearColor(1.0f, 0.28f, 0.18f, 1.0f);
        overlay_text("OFF", text_x,
                     y + (int)(62.0f * scale_y + 0.5f), text_scale);
    }
    if (setting_low_graphics != settings_low_graphics_applied()) {
        glClearColor(1.0f, 0.70f, 0.12f, 1.0f);
        overlay_text("RESTART", text_x,
                     y + (int)(108.0f * scale_y + 0.5f), text_scale);
    }
}

void controls_draw_cursor(void) {
    uint64_t now_us = sceKernelGetProcessTimeWide();
    if (cursor_visible && !cursor_down &&
        cursor_last_activity_us != 0 && now_us >= cursor_last_activity_us &&
        now_us - cursor_last_activity_us >= CURSOR_IDLE_TIMEOUT_US) {
        cursor_visible = false;
    }
    bool draw_setting = ui_context_current() == UI_CONTEXT_SETTINGS ||
        now_us < low_graphics_notice_until_us;
    if (!cursor_visible && !draw_setting) {
        return;
    }

    /* This overlay is the final operation before swap. Avoid synchronous
     * glGet* state reads here: they force a GPU pipeline round-trip every
     * frame and become noticeable during heavy rounds. The game establishes
     * its own draw state at the start of the following frame. */

    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (draw_setting) {
        draw_low_graphics_tile();
    }
    if (cursor_visible) {
        int x = (int)cursor_x;
        int y = settings_render_height() - (int)cursor_y;
        if (cursor_down) {
            glClearColor(1.0f, 0.2f, 0.1f, 1.0f);
        } else {
            glClearColor(1.0f, 0.85f, 0.1f, 1.0f);
        }
        glScissor(x - 7, y - 1, 15, 3);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(x - 1, y - 7, 3, 15);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    /* The overlay calls vitaGL directly, outside the game's import wrappers.
     * Forget cached game state so the next frame cannot skip a restoration
     * that the overlay changed underneath it. */
    gl_state_cache_invalidate();
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 128.0f;
    readings_y[0] = (raw_y - 128.0f) / 128.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);

    // Last two readings are 0, the one before that isn't ==> MOTION_ACTION_UP
    if (
        (readings_x[0] == 0.f && readings_y[0] == 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] != 0.f || readings_y[2] != 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_UP);
    }
    // Current reading isn't 0, the two before are ==> MOTION_ACTION_DOWN
    else if (
        (readings_x[0] != 0.f || readings_y[0] != 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] == 0.f && readings_y[2] == 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_DOWN);
    }
    // Other cases ==> MOTION_ACTION_MOVE
    else {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_MOVE);
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
