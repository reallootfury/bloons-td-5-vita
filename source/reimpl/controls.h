/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  controls.h
 * @brief Implementations for the touch screen, buttons, and other controls.
 */

#ifndef SOLOADER_CONTROLS_H
#define SOLOADER_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#define LEFT_ANALOG_DEADZONE  0.16f

typedef enum ControlsAction {
    CONTROLS_ACTION_UP = 0,
    CONTROLS_ACTION_DOWN = 1,
    CONTROLS_ACTION_MOVE = 2,
    CONTROLS_ACTION_CANCEL = 3
} ControlsAction;

typedef enum ControlsStickId {
    CONTROLS_STICK_LEFT = 0,
    CONTROLS_STICK_RIGHT = 1
} ControlsStickId;

extern void controls_handler_key(int32_t keycode, ControlsAction action);
extern void controls_handler_touch(int32_t id, float x, float y, ControlsAction action);
extern void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action);
extern void controls_handler_exit_request(void);

enum {
    AKEYCODE_BACK = 4,
};

void controls_init();
void controls_poll();
void controls_draw_cursor(void);

#endif // SOLOADER_CONTROLS_H
