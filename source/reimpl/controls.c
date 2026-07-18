/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "reimpl/ui_context.h"
#include "utils/logger.h"
#include "utils/settings.h"

#include <math.h>
#include <stdbool.h>
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f
#define LOW_GRAPHICS_TILE_X   700.0f
#define LOW_GRAPHICS_TILE_Y   360.0f
#define LOW_GRAPHICS_TILE_W   250.0f
#define LOW_GRAPHICS_TILE_H   165.0f

static void reset_cursor_for_surface(void);

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

static unsigned int touch_id_slot(int id) {
    return (unsigned int)id & 0xffU;
}

void poll_touch() {
    SceTouchData sampled_touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &sampled_touch, 1) <= 0) {
        /* Preserve the previous successful sample. Treating a failed read as
         * zero contacts would emit a false UP and place the dragged tower. */
        return;
    }
    sceClibMemcpy(&touch, &sampled_touch, sizeof(touch));

    for (int i = 0; i < touch.reportNum; i++) {
        float x = (float)touch.report[i].x *
                  (float)settings_render_width() / 1920.0f;
        float y = (float)touch.report[i].y *
                  (float)settings_render_height() / 1088.0f;

        // Check if the finger was down before to distinguish between the Move and Down events
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
        if (old_finger_index < 0) {
            if (low_graphics_tile_hit(x, y)) {
                cancelled_touch_ids[slot] = 1;
                toggle_low_graphics();
            } else {
                cancelled_touch_ids[slot] = 0;
                controls_handler_touch(touch.report[i].id, x, y,
                                       CONTROLS_ACTION_DOWN);
            }
        } else if (!cancelled_touch_ids[slot] &&
                   (touch.report[i].x != touch_old.report[old_finger_index].x ||
                    touch.report[i].y != touch_old.report[old_finger_index].y)) {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_MOVE);
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = (float)touch_old.report[i].x *
                      (float)settings_render_width() / 1920.0f;
            float y = (float)touch_old.report[i].y *
                      (float)settings_render_height() / 1088.0f;

            unsigned int slot = touch_id_slot(touch_old.report[i].id);
            if (!cancelled_touch_ids[slot]) {
                controls_handler_touch(touch_old.report[i].id, x, y,
                                       CONTROLS_ACTION_UP);
            }
            cancelled_touch_ids[slot] = 0;
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
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
    if (touch.reportNum <= 0) {
        return;
    }
    if (cursor_down) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_CANCEL);
        cursor_down = false;
        cursor_buttons_suppressed = true;
    }
}

/* A native screen change is a touch epoch boundary. Cancel at the OLD
 * coordinates. Held buttons remain suppressed until released so they cannot
 * immediately start a second contact on the newly opened popup. */
static void handle_ui_screen_change(void) {
    (void)cancel_active_touches();
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
