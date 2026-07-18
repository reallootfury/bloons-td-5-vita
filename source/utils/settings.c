/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "game.h"

int  setting_sampleSetting;
bool setting_sampleSetting2;
bool setting_low_graphics;

static bool applied_low_graphics;

enum {
    CONFIG_LINE_SIZE = 128,
    CONFIG_KEY_SIZE = 64,
    CONFIG_VALUE_SIZE = 32,
    RENDER_WIDTH_NORMAL = 960,
    RENDER_HEIGHT_NORMAL = 544,
};

static bool parse_config_line(const char *line, char *key, int *value) {
    char value_string[CONFIG_VALUE_SIZE];
    char trailing;

    /* Both tokens are width-limited, and a third non-whitespace token makes
     * the line invalid instead of being silently accepted. */
    int fields = sscanf(line, " %63s %31s %c", key, value_string, &trailing);
    if (fields != 2 || key[0] == '#') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value_string, &end, 10);
    if (errno == ERANGE || end == value_string || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    *value = (int)parsed;
    return true;
}

void settings_reset(void) {
    setting_sampleSetting  = 1;
    setting_sampleSetting2 = true;
    setting_low_graphics   = false;
}

void settings_load(void) {
    settings_reset();

    char config_path[160];
    btd5_path(config_path, sizeof(config_path), "config.txt");
    FILE *config = fopen(config_path, "r");

    if (config) {
        char line[CONFIG_LINE_SIZE];
        while (fgets(line, sizeof(line), config)) {
            size_t length = strlen(line);
            if (length > 0 && line[length - 1] != '\n' && !feof(config)) {
                /* Discard the remainder so an overlong line cannot be parsed
                 * as several independent settings. */
                int character;
                while ((character = fgetc(config)) != '\n' &&
                       character != EOF) {
                }
                continue;
            }

            char key[CONFIG_KEY_SIZE];
            int value;
            if (!parse_config_line(line, key, &value)) {
                continue;
            }

            if (strcmp("setting_sampleSetting", key) == 0) {
                setting_sampleSetting = value;
            } else if (strcmp("setting_sampleSetting2", key) == 0) {
                setting_sampleSetting2 = value != 0;
            } else if (strcmp("low_graphics", key) == 0) {
                setting_low_graphics = value != 0;
            }
        }
        fclose(config);
    }

    /* Keep the active render size stable for the lifetime of this process.
     * A later menu toggle only changes setting_low_graphics for the next boot. */
    applied_low_graphics = setting_low_graphics;
}

void settings_save(void) {
    char config_path[160];
    btd5_path(config_path, sizeof(config_path), "config.txt");
    FILE *config = fopen(config_path, "w");

    if (config) {
        fprintf(config, "%s %d\n", "setting_sampleSetting", (int)(setting_sampleSetting));
        fprintf(config, "%s %d\n", "setting_sampleSetting2", (int)(setting_sampleSetting2));
        fprintf(config, "%s %d\n", "low_graphics", (int)setting_low_graphics);
        fclose(config);
    }
}

bool settings_low_graphics_applied(void) {
    return applied_low_graphics;
}

int settings_render_width(void) {
    /* Vita display presentation is kept at its native size in both modes.
     * The former 840x476 intermediate surface reached EGL swaps on hardware
     * but presented only black. Low Graphics now changes safe renderer
     * policy in gl_preload() without altering the display contract. */
    return RENDER_WIDTH_NORMAL;
}

int settings_render_height(void) {
    return RENDER_HEIGHT_NORMAL;
}
