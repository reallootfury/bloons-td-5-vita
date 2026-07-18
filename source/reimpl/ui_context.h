/* Read-only BTD5 screen-context detection used by Vita-owned overlays and
 * touch-transition safety. It never invokes game methods or navigates UI. */

#ifndef BTD5_UI_CONTEXT_H
#define BTD5_UI_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum UiContext {
    UI_CONTEXT_UNKNOWN = 0,
    UI_CONTEXT_MAIN,
    UI_CONTEXT_MENU,
    UI_CONTEXT_GAME,
    UI_CONTEXT_PAUSE,
    UI_CONTEXT_SETTINGS,
    UI_CONTEXT_POPUP,
} UiContext;

/* Reset all cached native pointers. Call after libnative.so is initialized. */
void ui_context_init(void);

/*
 * Check active-screen identity. Returns true only when the deepest active
 * screen changed. controls.c uses this as a touch epoch boundary so an old
 * DOWN cannot confirm a new screen.
 */
bool ui_context_update(void);

UiContext ui_context_current(void);
const char *ui_context_name(UiContext context);

#ifdef __cplusplus
}
#endif

#endif /* BTD5_UI_CONTEXT_H */
