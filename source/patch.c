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
#include "game.h"

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
static so_hook tick_update_hook;
static so_hook tick_post_frame_hook;
static so_hook present_hook;
static so_hook render_core_hook;
static so_hook render_overlay_hook;

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

static void install_version_337_hooks(void) {
#ifdef DEBUG_SOLOADER
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
    l_info("Installed nativeTick internal stage diagnostics.");
#endif
}

static void patch_kuser_helpers(void) {
    const uintptr_t cmpxchg = (uintptr_t)&__atomic_cmpxchg;
    const uintptr_t barrier = (uintptr_t)&bionic_memory_barrier;
    unsigned int cmpxchg_count = 0;
    unsigned int barrier_count = 0;

    for (size_t offset = 0; offset + sizeof(uint32_t) <= so_mod.exec_size;
         offset += sizeof(uint32_t)) {
        uintptr_t address = so_mod.exec_base + offset;
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
    if (btd5_game_version() == BTD5_VERSION_337) {
        patch_kuser_helpers();
        install_version_337_hooks();
    } else {
        /* 4.7 was rebuilt with modern atomics and has no Linux kuser literal
         * calls. Keep it on named exports/imports only. */
        l_info("BTD5 4.7 detected: no version-specific patches required.");
    }
}
