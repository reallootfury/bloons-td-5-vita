/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "reimpl/sys.h"
#include "reimpl/controls.h"
#include "diagnostics.h"
#include "utils/logger.h"

extern so_module so_mod;

/*
 * Old Android ARM binaries use Linux's high-vector kuser helpers directly;
 * those calls are literal pointers and therefore never appear in the ELF
 * import table. Vita has no executable page at these Linux-only addresses.
 */
#define ARM_KUSER_MEMORY_BARRIER 0xffff0fa0u
#define ARM_KUSER_CMPXCHG        0xffff0fc0u

/* Pinned BTD5 3.37 offsets used only to expose which nativeTick substage is
 * blocked. These functions bracket the engine update and post-frame calls in
 * MainActivity_nativeTick. */
#define BTD5_TICK_UPDATE_OFFSET     0x004d1d68u
#define BTD5_TICK_POST_FRAME_OFFSET 0x004d1e34u
#define BTD5_PRESENT_OFFSET         0x004fb17cu
#define BTD5_RENDER_CORE_OFFSET     0x004d1d2cu
#define BTD5_RENDER_OVERLAY_OFFSET  0x00238a88u
/* CInGameUI::onActorActivated. Its existing debug branch compares the event
 * actor with this+0x270, looks up "InGameDebug" through the normal screen
 * registry, and toggles the panel's visibility flag. */
#define BTD5_INGAME_UI_EVENT_OFFSET 0x003ba7c8u
#define BTD5_SCREEN_LOOKUP_OFFSET    0x004db3b8u
#define BTD5_CBASESCREEN_RTTI        0x00944540u
#define BTD5_CINGAMEDEBUG_RTTI       0x0093cca0u
#define BTD5_INGAME_DEBUG_VISIBLE    0x14cu

static so_hook tick_update_hook;
static so_hook tick_post_frame_hook;
static so_hook present_hook;
static so_hook render_core_hook;
static so_hook render_overlay_hook;
static so_hook ingame_ui_event_hook;

extern void *__dynamic_cast(const void *object, const void *source_type,
                            const void *target_type, ptrdiff_t source_offset);

/* BTD5 3.37 uses the old 32-bit libc++ long-string representation. The
 * dormant debug branch builds exactly this 12-byte object for
 * "InGameDebug" before asking its screen registry for CBaseScreen*. */
typedef struct BTD5LongString {
    uint32_t capacity;
    uint32_t size;
    const char *data;
} BTD5LongString;

typedef void *(*BTD5ScreenLookup)(void *screen_registry,
                                  const BTD5LongString *name);

static uintptr_t tick_update_diagnostic(void *engine) {
    btd5_diag_set_stage(BTD5_STAGE_UPDATE);
    uintptr_t result = SO_CONTINUE(uintptr_t, tick_update_hook, engine);
    btd5_diag_set_stage(BTD5_STAGE_UPDATE_RETURNED);
    return result;
}

static uintptr_t tick_post_frame_diagnostic(void *engine) {
    btd5_diag_set_stage(BTD5_STAGE_POST_FRAME);
    uintptr_t result = SO_CONTINUE(uintptr_t, tick_post_frame_hook, engine);
    btd5_diag_set_stage(BTD5_STAGE_POST_FRAME_RETURNED);
    return result;
}

static uintptr_t present_diagnostic(void *renderer) {
    uintptr_t result = SO_CONTINUE(uintptr_t, present_hook, renderer);
    btd5_diag_set_stage(BTD5_STAGE_PRESENT_RETURNED);
    return result;
}

static uintptr_t render_core_diagnostic(void *engine) {
    uintptr_t result = SO_CONTINUE(uintptr_t, render_core_hook, engine);
    btd5_diag_set_stage(BTD5_STAGE_RENDER_CORE_RETURNED);
    return result;
}

static uintptr_t render_overlay_diagnostic(void *engine) {
    btd5_diag_set_stage(BTD5_STAGE_RENDER_OVERLAY);
    uintptr_t result = SO_CONTINUE(uintptr_t, render_overlay_hook, engine);
    btd5_diag_set_stage(BTD5_STAGE_RENDER_OVERLAY_RETURNED);
    return result;
}

static int toggle_ingame_debug_panel(void *ingame_ui) {
    static const char debug_name[] = "InGameDebug";
    BTD5LongString name = {
        .capacity = 17,
        .size = sizeof(debug_name) - 1,
        .data = debug_name,
    };

    void *screen_registry = *(void **)((uintptr_t)ingame_ui + 0x1cu);
    if (!screen_registry) {
        l_warn("InGameDebug screen registry is unavailable.");
        return -1;
    }

    BTD5ScreenLookup lookup = (BTD5ScreenLookup)(
        (so_mod.text_base + BTD5_SCREEN_LOOKUP_OFFSET) | 1u);
    void *base_screen = lookup(screen_registry, &name);
    if (!base_screen) {
        l_warn("InGameDebug is not registered in the current match.");
        return -1;
    }

    void *debug_screen = __dynamic_cast(
        base_screen,
        (const void *)(so_mod.text_base + BTD5_CBASESCREEN_RTTI),
        (const void *)(so_mod.text_base + BTD5_CINGAMEDEBUG_RTTI), 0);
    if (!debug_screen) {
        l_warn("InGameDebug screen cast failed.");
        return -1;
    }

    uint8_t *visible = (uint8_t *)debug_screen + BTD5_INGAME_DEBUG_VISIBLE;
    *visible ^= 1u;
    l_info("InGameDebug panel visibility set to %u.", (unsigned)*visible);
    return *visible;
}

static uintptr_t ingame_ui_event_debug_modifier(void *ingame_ui,
                                                 void *event_actor) {
    if (controls_consume_ingame_debug_modifier()) {
        int visible = toggle_ingame_debug_panel(ingame_ui);
        log_flush();
        if (visible >= 0) {
            /* Consume the modified UI activation. The game's original debug
             * branch also returns immediately after toggling this byte. */
            return (uintptr_t)visible;
        }
    }

    return SO_CONTINUE(uintptr_t, ingame_ui_event_hook, ingame_ui,
                       event_actor);
}

static void install_tick_diagnostics(void) {
    tick_update_hook = hook_addr(
        (so_mod.text_base + BTD5_TICK_UPDATE_OFFSET) | 1u,
        (uintptr_t)&tick_update_diagnostic);
    tick_post_frame_hook = hook_addr(
        (so_mod.text_base + BTD5_TICK_POST_FRAME_OFFSET) | 1u,
        (uintptr_t)&tick_post_frame_diagnostic);
    present_hook = hook_addr(
        (so_mod.text_base + BTD5_PRESENT_OFFSET) | 1u,
        (uintptr_t)&present_diagnostic);
    render_core_hook = hook_addr(
        (so_mod.text_base + BTD5_RENDER_CORE_OFFSET) | 1u,
        (uintptr_t)&render_core_diagnostic);
    render_overlay_hook = hook_addr(
        (so_mod.text_base + BTD5_RENDER_OVERLAY_OFFSET) | 1u,
        (uintptr_t)&render_overlay_diagnostic);
    ingame_ui_event_hook = hook_addr(
        (so_mod.text_base + BTD5_INGAME_UI_EVENT_OFFSET) | 1u,
        (uintptr_t)&ingame_ui_event_debug_modifier);
    l_info("Installed nativeTick internal stage diagnostics.");
    l_info("Installed registry-based InGameDebug panel hook.");
}

static void patch_kuser_helpers(void) {
    const uintptr_t cmpxchg = (uintptr_t)&__atomic_cmpxchg;
    const uintptr_t barrier = (uintptr_t)&bionic_memory_barrier;
    unsigned int cmpxchg_count = 0;
    unsigned int barrier_count = 0;

    for (size_t offset = 0; offset + sizeof(uint32_t) <= so_mod.text_size;
         offset += sizeof(uint32_t)) {
        uintptr_t address = so_mod.text_base + offset;
        uint32_t value = *(const uint32_t *)address;
        uintptr_t replacement = 0;

        if (value == ARM_KUSER_CMPXCHG) {
            replacement = cmpxchg;
            cmpxchg_count++;
        } else if (value == ARM_KUSER_MEMORY_BARRIER) {
            replacement = barrier;
            barrier_count++;
        }

        if (replacement) {
            kuKernelCpuUnrestrictedMemcpy((void *)address, &replacement,
                                          sizeof(replacement));
        }
    }

    l_info("Patched ARM kuser helpers: %u cmpxchg, %u memory barriers.",
           cmpxchg_count, barrier_count);
    if (cmpxchg_count != 47 || barrier_count != 10) {
        l_warn("Unexpected kuser helper counts for the pinned BTD5 binary.");
    }
}

void so_patch(void) {
    patch_kuser_helpers();
    install_tick_diagnostics();
}
