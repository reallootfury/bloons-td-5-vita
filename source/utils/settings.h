/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int  setting_sampleSetting;
extern bool setting_sampleSetting2;
extern bool setting_low_graphics;

void settings_load(void);
void settings_save(void);
void settings_reset(void);

/* Low Graphics is applied when settings_load() runs, before VitaGL configures
 * its renderer. The desired value may then be changed and saved while the
 * game is running, but the active renderer policy remains fixed until the
 * next launch. Display dimensions stay at the Vita-native 960x544 in either
 * mode because the tested intermediate framebuffer presented black. */
bool settings_low_graphics_applied(void);
int settings_render_width(void);
int settings_render_height(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
