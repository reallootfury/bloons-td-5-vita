/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"

#include <math.h>
#include <stdbool.h>
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f


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
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    poll_touch();
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
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    for (int i = 0; i < touch.reportNum; i++) {
        float x = (float) touch.report[i].x * 960.f / 1920.0f;
        float y = (float) touch.report[i].y * 544.f / 1088.0f;

        // Check if the finger was down before to distinguish between the Move and Down events
        int finger_down = 0;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                    break;
                }
            }
        }

        unsigned int slot = touch_id_slot(touch.report[i].id);
        if (!finger_down) {
            cancelled_touch_ids[slot] = 0;
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_DOWN);
        } else if (!cancelled_touch_ids[slot]) {
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
            float x = (float) touch_old.report[i].x * 960.f / 1920.0f;
            float y = (float) touch_old.report[i].y * 544.f / 1088.0f;

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

static ButtonMapping mapping[] = {
        /* BTD5 Mobile does not navigate its menus with Android gamepad
         * keycodes. Circle is handled separately as cancel-or-Back. */
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
};

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
static bool debug_modifier_consumed = false;

#define VIRTUAL_CURSOR_TOUCH_ID 0x7fff
#define CURSOR_ANALOG_SPEED 12.0f
#define CURSOR_DPAD_SPEED 8.0f

static float clampf(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static bool cancel_active_touches(void) {
    bool cancelled_any = false;

    for (int i = 0; i < touch.reportNum; i++) {
        unsigned int slot = touch_id_slot(touch.report[i].id);
        if (cancelled_touch_ids[slot]) {
            continue;
        }

        float x = (float)touch.report[i].x * 960.0f / 1920.0f;
        float y = (float)touch.report[i].y * 544.0f / 1088.0f;
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

void poll_pad() {
    SceCtrlData pad;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    // Gamepad buttons
    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    if (!(current_buttons & SCE_CTRL_SELECT)) {
        debug_modifier_consumed = false;
    }

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    /* Circle cancels an in-progress physical or virtual drag. With no active
     * contact it retains Android's normal Back behaviour. Suppressed physical
     * contacts stay ignored until the finger is lifted, preventing a stale UP
     * event from placing a tower after cancellation. */
    if (pressed_buttons & SCE_CTRL_CIRCLE) {
        circle_sent_back = !cancel_active_touches();
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

    // Analog sticks
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);

    /* Treat the pad as a relative touchscreen pointer. The Android edition's
     * UI is built entirely around touch events, so sending BUTTON_A or DPAD
     * keycodes cannot activate its menu widgets. */
    float dx = analog_lx[0] * CURSOR_ANALOG_SPEED;
    float dy = analog_ly[0] * CURSOR_ANALOG_SPEED;
    if (current_buttons & SCE_CTRL_LEFT)  dx -= CURSOR_DPAD_SPEED;
    if (current_buttons & SCE_CTRL_RIGHT) dx += CURSOR_DPAD_SPEED;
    if (current_buttons & SCE_CTRL_UP)    dy -= CURSOR_DPAD_SPEED;
    if (current_buttons & SCE_CTRL_DOWN)  dy += CURSOR_DPAD_SPEED;

    bool moved = dx != 0.0f || dy != 0.0f;
    if (moved) {
        cursor_x = clampf(cursor_x + dx, 8.0f, 951.0f);
        cursor_y = clampf(cursor_y + dy, 8.0f, 535.0f);
        cursor_visible = true;
        if (cursor_down) {
            controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x,
                                   cursor_y, CONTROLS_ACTION_MOVE);
        }
    }

    if (pressed_buttons & SCE_CTRL_CROSS) {
        cursor_visible = true;
        cursor_down = true;
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_DOWN);
    }
    if (released_buttons & SCE_CTRL_CROSS) {
        controls_handler_touch(VIRTUAL_CURSOR_TOUCH_ID, cursor_x, cursor_y,
                               CONTROLS_ACTION_UP);
        cursor_down = false;
    }
}

bool controls_consume_ingame_debug_modifier(void) {
    if (!(current_buttons & SCE_CTRL_SELECT) || debug_modifier_consumed) {
        return false;
    }

    debug_modifier_consumed = true;
    return true;
}

void controls_draw_cursor(void) {
    if (!cursor_visible) {
        return;
    }

    /* Draw a small crosshair without depending on the game's shaders. Keep
     * every piece of GL state touched here intact for the next frame. */
    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint old_scissor[4];
    GLfloat old_clear_color[4];
    GLboolean old_color_mask[4];
    glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, old_color_mask);

    int x = (int)cursor_x;
    int y = 544 - (int)cursor_y;
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (cursor_down) {
        glClearColor(1.0f, 0.2f, 0.1f, 1.0f);
    } else {
        glClearColor(1.0f, 0.85f, 0.1f, 1.0f);
    }
    glScissor(x - 7, y - 1, 15, 3);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(x - 1, y - 7, 3, 15);
    glClear(GL_COLOR_BUFFER_BIT);

    glClearColor(old_clear_color[0], old_clear_color[1],
                 old_clear_color[2], old_clear_color[3]);
    glColorMask(old_color_mask[0], old_color_mask[1], old_color_mask[2],
                old_color_mask[3]);
    glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (!scissor_enabled) {
        glDisable(GL_SCISSOR_TEST);
    }
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
