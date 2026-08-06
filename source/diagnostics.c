#include "diagnostics.h"

#include <stdbool.h>
#include <stdatomic.h>

static bool diagnostics_enabled = false;
static atomic_int tick_stage = ATOMIC_VAR_INIT(BTD5_STAGE_BOOT);
static atomic_bool tick_thread_bound = ATOMIC_VAR_INIT(false);
static _Thread_local bool is_tick_thread = false;

void btd5_diag_set_enabled(bool enabled) {
    diagnostics_enabled = enabled;
}

void btd5_diag_bind_current_thread(void) {
    is_tick_thread = true;
    atomic_store_explicit(&tick_thread_bound, true, memory_order_release);
}

int btd5_diag_is_current_thread(void) {
    return is_tick_thread;
}

void btd5_diag_set_stage(BTD5TickStage stage) {
    if (!diagnostics_enabled) {
        return;
    }
    if (atomic_load_explicit(&tick_thread_bound, memory_order_acquire) &&
        !is_tick_thread) {
        return;
    }
    atomic_store_explicit(&tick_stage, (int)stage, memory_order_release);
}

BTD5TickStage btd5_diag_get_stage(void) {
    return (BTD5TickStage)atomic_load_explicit(&tick_stage,
                                               memory_order_acquire);
}

const char *btd5_diag_stage_name(BTD5TickStage stage) {
    switch (stage) {
    case BTD5_STAGE_BOOT: return "boot";
    case BTD5_STAGE_TICK_ENTER: return "tick entry before update";
    case BTD5_STAGE_UPDATE: return "game update";
    case BTD5_STAGE_UPDATE_RETURNED: return "render before EGL swap";
    case BTD5_STAGE_EGL_SWAP: return "inside EGL swap";
    case BTD5_STAGE_EGL_SWAP_RETURNED: return "render after EGL swap";
    case BTD5_STAGE_PRESENT_RETURNED: return "render after present routine";
    case BTD5_STAGE_RENDER_CORE_RETURNED: return "render core returned";
    case BTD5_STAGE_RENDER_OVERLAY: return "inside late render overlay";
    case BTD5_STAGE_RENDER_OVERLAY_RETURNED: return "late render overlay returned";
    case BTD5_STAGE_MUTEX_LOCK: return "inside pthread mutex lock";
    case BTD5_STAGE_MUTEX_LOCK_RETURNED: return "after pthread mutex lock";
    case BTD5_STAGE_MUTEX_UNLOCK: return "inside pthread mutex unlock";
    case BTD5_STAGE_MUTEX_UNLOCK_RETURNED: return "after pthread mutex unlock";
    case BTD5_STAGE_COND_WAIT: return "inside pthread condition wait";
    case BTD5_STAGE_COND_WAIT_RETURNED: return "after pthread condition wait";
    case BTD5_STAGE_POST_FRAME: return "post-frame processing";
    case BTD5_STAGE_POST_FRAME_RETURNED: return "tick epilogue";
    case BTD5_STAGE_TICK_RETURNED: return "between ticks";
    default: return "unknown";
    }
}
