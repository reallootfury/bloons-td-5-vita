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
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "reimpl/sys.h"
#include "reimpl/controls.h"
#include "diagnostics.h"
#include "patch.h"
#include "utils/logger.h"
#include "utils/glutil.h"
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


#ifdef BTD5_NATIVE_PHASE_PROFILER
/* Exact offsets for the verified BTD5 4.7 native executable:
 * size 8,683,424 bytes, CRC32 53BEA993, ELF build ID 0757dc4d0ad2560d.
 *
 * The v6.7 trace proved that SO+0x0052146C, SO+0x004DB9F0 and
 * SO+0x004DBAA0 do not explain the sustained 50-84 ms late-round residual.
 * v6.8 therefore patches the exact indirect-call sequence at SO+0x00500994
 * and times the virtual engine call at SO+0x00500998 directly. The v6.7
 * direct update/post hooks are intentionally not reinstalled: their result is
 * already known and their code mutation/cache-flush overhead is unnecessary. */
#define BTD5_47_ENGINE_CALLSITE_OFFSET     0x00500994u
#define BTD5_47_ENGINE_CONTINUE_OFFSET     0x0050099cu
#define BTD5_47_INNER_UPDATE_CALLSITE_OFFSET 0x0020ad6cu
#define BTD5_47_INNER_UPDATE_CONTINUE_OFFSET 0x0020ad74u
#define BTD5_PHASE_PERIODIC_SAMPLE_TICKS   UINT64_C(120)
#define BTD5_PHASE_SLOW_SAMPLE_COOLDOWN    UINT64_C(30)
#define BTD5_PHASE_SLOW_TRIGGER_US         UINT64_C(33333)

/* ldr r1,[r0]; ldr r1,[r1,#0x18]; blx r1; ldr r0,[r5,#0x20] */
#define BTD5_47_ENGINE_CALLSITE_WORD0 UINT32_C(0x69896801)
#define BTD5_47_ENGINE_CALLSITE_WORD1 UINT32_C(0x6a284788)
/* ldr r1,[r0]; ldr r2,[r1,#0x18]; mov r1,r5; blx r2 */
#define BTD5_47_INNER_UPDATE_WORD0 UINT32_C(0x698a6801)
#define BTD5_47_INNER_UPDATE_WORD1 UINT32_C(0x47904629)

static so_hook engine_callsite_hook;
static so_hook inner_update_callsite_hook;
static bool phase_probe_active;
static bool phase_probe_disabled;
static bool phase_probe_runtime_enabled;
static bool engine_callsite_installed;
static bool inner_update_callsite_installed;
static bool phase_probe_slow_triggered;
static uint64_t phase_probe_tick_started_us;
static uint64_t phase_probe_last_sample_tick;
static BTD5NativePhaseStats native_phase_stats;

volatile uint32_t btd5_engine_profile_sample_active;
uintptr_t btd5_engine_callsite_continue;
uintptr_t btd5_inner_update_callsite_continue;

static void record_phase(uint64_t elapsed_us, uint64_t *samples,
                         uint64_t *total_us, uint64_t *max_us) {
    (*samples)++;
    *total_us += elapsed_us;
    if (elapsed_us > *max_us) {
        *max_us = elapsed_us;
    }
}

static so_hook install_profile_hook_address(uintptr_t thumb_address,
                                            uintptr_t target) {
    so_hook hook;
    memset(&hook, 0, sizeof(hook));

    uintptr_t patch_address = thumb_address & ~(uintptr_t)1u;
    if ((patch_address & 3u) != 0) {
        return hook;
    }

    hook.thumb_addr = thumb_address | 1u;
    hook.addr = patch_address;
    hook.patch_instr[0] = UINT32_C(0xf000f8df); /* LDR.W PC, [PC] */
    hook.patch_instr[1] = (uint32_t)target;
    kuKernelCpuUnrestrictedMemcpy(hook.orig_instr,
                                  (const void *)patch_address,
                                  sizeof(hook.orig_instr));
    kuKernelCpuUnrestrictedMemcpy((void *)patch_address, hook.patch_instr,
                                  sizeof(hook.patch_instr));
    kuKernelFlushCaches((void *)patch_address, sizeof(hook.patch_instr));
    return hook;
}


static bool install_engine_callsite_bridge(void) {
    uintptr_t address = so_mod.text_base + BTD5_47_ENGINE_CALLSITE_OFFSET;
    const uint32_t *words = (const uint32_t *)address;
    if (words[0] != BTD5_47_ENGINE_CALLSITE_WORD0 ||
        words[1] != BTD5_47_ENGINE_CALLSITE_WORD1) {
        l_error("BTD5 4.7 engine call-site bytes did not match at "
                "SO+0x%08x: got %08x %08x.",
                BTD5_47_ENGINE_CALLSITE_OFFSET,
                (unsigned int)words[0], (unsigned int)words[1]);
        return false;
    }

    btd5_engine_callsite_continue =
        (so_mod.text_base + BTD5_47_ENGINE_CONTINUE_OFFSET) | 1u;
    engine_callsite_hook = install_profile_hook_address(
        address | 1u, ((uintptr_t)&btd5_engine_callsite_trampoline) | 1u);
    if (!engine_callsite_hook.addr) {
        return false;
    }
    engine_callsite_installed = true;
    return true;
}


static bool install_inner_update_callsite_bridge(void) {
    uintptr_t address = so_mod.text_base +
                        BTD5_47_INNER_UPDATE_CALLSITE_OFFSET;
    const uint32_t *words = (const uint32_t *)address;
    if (words[0] != BTD5_47_INNER_UPDATE_WORD0 ||
        words[1] != BTD5_47_INNER_UPDATE_WORD1) {
        l_error("BTD5 4.7 inner update call-site bytes did not match at "
                "SO+0x%08x: got %08x %08x.",
                BTD5_47_INNER_UPDATE_CALLSITE_OFFSET,
                (unsigned int)words[0], (unsigned int)words[1]);
        return false;
    }

    btd5_inner_update_callsite_continue =
        (so_mod.text_base + BTD5_47_INNER_UPDATE_CONTINUE_OFFSET) | 1u;
    inner_update_callsite_hook = install_profile_hook_address(
        address | 1u,
        ((uintptr_t)&btd5_inner_update_callsite_trampoline) | 1u);
    if (!inner_update_callsite_hook.addr) {
        return false;
    }
    inner_update_callsite_installed = true;
    return true;
}


uint64_t btd5_engine_call_profile_enter(void *engine, uintptr_t target) {
    (void)engine;
    if (!btd5_engine_profile_sample_active) {
        return 0;
    }

    uint64_t entered_us = sceKernelGetProcessTimeWide();
    if (entered_us >= phase_probe_tick_started_us) {
        record_phase(entered_us - phase_probe_tick_started_us,
                     &native_phase_stats.pre_engine_samples,
                     &native_phase_stats.pre_engine_total_us,
                     &native_phase_stats.pre_engine_max_us);
    }

    uintptr_t normalized = target & ~(uintptr_t)1u;
    if (native_phase_stats.engine_target != 0 &&
        native_phase_stats.engine_target != normalized) {
        native_phase_stats.engine_target_changes++;
    }
    native_phase_stats.engine_target = normalized;
    return entered_us;
}

void btd5_engine_call_profile_exit(uint64_t started_us, uintptr_t target) {
    (void)target;
    if (!started_us || !btd5_engine_profile_sample_active) {
        return;
    }
    record_phase(sceKernelGetProcessTimeWide() - started_us,
                 &native_phase_stats.engine_samples,
                 &native_phase_stats.engine_total_us,
                 &native_phase_stats.engine_max_us);
}


uint64_t btd5_inner_update_profile_enter(void *screen, void *context,
                                         uintptr_t target) {
    (void)screen;
    (void)context;
    if (!btd5_engine_profile_sample_active) {
        return 0;
    }

    uintptr_t normalized = target & ~(uintptr_t)1u;
    if (native_phase_stats.inner_update_target != 0 &&
        native_phase_stats.inner_update_target != normalized) {
        native_phase_stats.inner_update_target_changes++;
    }
    native_phase_stats.inner_update_target = normalized;
    return sceKernelGetProcessTimeWide();
}

void btd5_inner_update_profile_exit(uint64_t started_us, uintptr_t target) {
    (void)target;
    if (!started_us || !btd5_engine_profile_sample_active) {
        return;
    }
    record_phase(sceKernelGetProcessTimeWide() - started_us,
                 &native_phase_stats.inner_update_samples,
                 &native_phase_stats.inner_update_total_us,
                 &native_phase_stats.inner_update_max_us);
}
#endif

#ifdef BTD5_FRAME_DEBT_CLAMP
/* Fingerprint-gated experimental frame-delta clamp for BTD5 4.7. The patch
 * begins at SO+0x0020AD56 and reproduces the original prepare call before
 * continuing at SO+0x0020AD66. */
#define BTD5_47_FRAME_DEBT_CALLSITE_OFFSET UINT32_C(0x0020ad56)
#define BTD5_47_FRAME_DEBT_CONTINUE_OFFSET UINT32_C(0x0020ad66)
#define BTD5_47_FRAME_DEBT_PREPARE_OFFSET  UINT32_C(0x004e7ffc)
/* The call site is Thumb-halfword aligned at ...56 rather than word aligned.
 * A normal 8-byte LDR.W-PC hook would fetch its literal from ...58, not ...5A.
 * Use a 10-byte NOP + aligned LDR.W-PC + literal relay instead:
 *   ...56: nop
 *   ...58: ldr.w pc, [pc]
 *   ...5C: .word trampoline|1
 * The trampoline recreates the overwritten instructions and resumes at ...66. */
#define BTD5_47_FRAME_DEBT_PATCH_SIZE 10u
static const uint8_t btd5_47_frame_debt_original[
    BTD5_47_FRAME_DEBT_PATCH_SIZE] = {
    0xd4, 0xf8, 0x24, 0x01, /* ldr.w r0, [r4, #0x124] */
    0x35, 0x68,             /* ldr r5, [r6] */
    0xd0, 0xf8, 0xd0, 0x00  /* ldr.w r0, [r0, #0xd0] */
};

static uintptr_t frame_debt_patch_address;
static uint8_t frame_debt_original_bytes[BTD5_47_FRAME_DEBT_PATCH_SIZE];
static uint8_t frame_debt_patch_bytes[BTD5_47_FRAME_DEBT_PATCH_SIZE];
static bool frame_debt_callsite_installed;
uintptr_t btd5_frame_debt_callsite_continue;
uintptr_t btd5_frame_debt_prepare_target;
volatile uint32_t btd5_frame_debt_clamp_count;

static bool install_frame_debt_clamp_bridge(void) {
    uintptr_t address = so_mod.text_base +
                        BTD5_47_FRAME_DEBT_CALLSITE_OFFSET;
    if (memcmp((const void *)address, btd5_47_frame_debt_original,
               sizeof(btd5_47_frame_debt_original)) != 0) {
        const uint32_t *words = (const uint32_t *)address;
        l_error("BTD5 4.7 frame-debt call-site bytes did not match at "
                "SO+0x%08x: got %08x %08x.",
                BTD5_47_FRAME_DEBT_CALLSITE_OFFSET,
                (unsigned int)words[0], (unsigned int)words[1]);
        return false;
    }

    btd5_frame_debt_callsite_continue =
        (so_mod.text_base + BTD5_47_FRAME_DEBT_CONTINUE_OFFSET) | 1u;
    btd5_frame_debt_prepare_target =
        (so_mod.text_base + BTD5_47_FRAME_DEBT_PREPARE_OFFSET) | 1u;

    frame_debt_patch_address = address;
    memcpy(frame_debt_original_bytes, (const void *)address,
           sizeof(frame_debt_original_bytes));

    const uint16_t nop = UINT16_C(0xbf00);
    const uint32_t ldr_pc = UINT32_C(0xf000f8df);
    const uint32_t target =
        (uint32_t)(((uintptr_t)&btd5_frame_debt_callsite_trampoline) | 1u);
    memcpy(frame_debt_patch_bytes + 0, &nop, sizeof(nop));
    memcpy(frame_debt_patch_bytes + 2, &ldr_pc, sizeof(ldr_pc));
    memcpy(frame_debt_patch_bytes + 6, &target, sizeof(target));

    kuKernelCpuUnrestrictedMemcpy((void *)frame_debt_patch_address,
                                  frame_debt_patch_bytes,
                                  sizeof(frame_debt_patch_bytes));
    kuKernelFlushCaches((void *)frame_debt_patch_address,
                        sizeof(frame_debt_patch_bytes));
    frame_debt_callsite_installed = true;
    return true;
}
#endif

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


bool btd5_native_phase_profiler_enabled(void) {
#ifdef BTD5_NATIVE_PHASE_PROFILER
    return phase_probe_runtime_enabled && !phase_probe_disabled;
#else
    return false;
#endif
}

void btd5_native_phase_probe_begin(uint64_t tick_number,
                                   uint64_t previous_tick_us) {
#ifdef BTD5_NATIVE_PHASE_PROFILER
    if (!phase_probe_runtime_enabled || phase_probe_active ||
        phase_probe_disabled ||
        !engine_callsite_installed ||
        !inner_update_callsite_installed ||
        btd5_game_version() != BTD5_VERSION_47) {
        return;
    }

    bool periodic = tick_number % BTD5_PHASE_PERIODIC_SAMPLE_TICKS == 0;
    bool slow = previous_tick_us >= BTD5_PHASE_SLOW_TRIGGER_US &&
                (phase_probe_last_sample_tick == 0 ||
                 tick_number - phase_probe_last_sample_tick >=
                     BTD5_PHASE_SLOW_SAMPLE_COOLDOWN);
    if (!periodic && !slow) {
        return;
    }

    phase_probe_last_sample_tick = tick_number;
    phase_probe_slow_triggered = slow && !periodic;
    phase_probe_tick_started_us = sceKernelGetProcessTimeWide();
    gl_profile_sample_begin();
    btd5_engine_profile_sample_active = 1;
    phase_probe_active = true;
#else
    (void)tick_number;
    (void)previous_tick_us;
#endif
}

void btd5_native_phase_probe_end(void) {
#ifdef BTD5_NATIVE_PHASE_PROFILER
    if (!phase_probe_active) {
        return;
    }

    uint64_t elapsed_us = sceKernelGetProcessTimeWide() -
                          phase_probe_tick_started_us;
    btd5_engine_profile_sample_active = 0;
    GLProfileSample gl_sample = {0};
    gl_profile_sample_end(&gl_sample);
    phase_probe_active = false;

    native_phase_stats.sampled_ticks++;
    native_phase_stats.sampled_tick_total_us += elapsed_us;
    if (elapsed_us > native_phase_stats.sampled_tick_max_us) {
        native_phase_stats.sampled_tick_max_us = elapsed_us;
    }
    if (phase_probe_slow_triggered) {
        native_phase_stats.slow_triggered_samples++;
    } else {
        native_phase_stats.periodic_samples++;
    }
    native_phase_stats.gl_draw_calls += gl_sample.draw_calls;
    native_phase_stats.gl_draw_vertices += gl_sample.draw_vertices;
    native_phase_stats.gl_draw_cpu_us += gl_sample.draw_cpu_us;
    if (gl_sample.draw_cpu_max_us > native_phase_stats.gl_draw_cpu_max_us) {
        native_phase_stats.gl_draw_cpu_max_us = gl_sample.draw_cpu_max_us;
    }
#endif
}

void btd5_take_native_phase_stats(BTD5NativePhaseStats *stats) {
    if (!stats) {
        return;
    }
#ifdef BTD5_NATIVE_PHASE_PROFILER
    uintptr_t engine_target = native_phase_stats.engine_target;
    uintptr_t inner_update_target = native_phase_stats.inner_update_target;
    *stats = native_phase_stats;
    memset(&native_phase_stats, 0, sizeof(native_phase_stats));
    native_phase_stats.engine_target = engine_target;
    native_phase_stats.inner_update_target = inner_update_target;
#else
    memset(stats, 0, sizeof(*stats));
#endif
}


uint32_t btd5_take_frame_debt_clamp_count(void) {
#ifdef BTD5_FRAME_DEBT_CLAMP
    uint32_t count = btd5_frame_debt_clamp_count;
    btd5_frame_debt_clamp_count = 0;
    return count;
#else
    return 0;
#endif
}

void so_patch(void) {
    if (btd5_game_version() == BTD5_VERSION_337) {
        patch_kuser_helpers();
        install_version_337_hooks();
    } else {
        /* 4.7 was rebuilt with modern atomics and has no Linux kuser literal
         * calls. Keep it on named exports/imports only. */
        l_info("BTD5 4.7 detected: no mandatory version-specific patches required.");
#ifdef BTD5_NATIVE_PHASE_PROFILER
        /* The performance and logging VPKs share one eboot. Install the
         * profiler only when loader_logging.cfg enables diagnostics, keeping
         * the normal VPK free of call-site hooks and timer sampling. */
        if (log_is_enabled()) {
            if (install_engine_callsite_bridge() &&
                install_inner_update_callsite_bridge()) {
                phase_probe_runtime_enabled = true;
                l_info("BTD5 4.7 nested native engine profiler enabled for "
                       "the logging VPK: outer call SO+0x00500998 and inner "
                       "update call SO+0x0020ad72, periodic sample every %llu "
                       "ticks plus sustained slow-frame samples with a "
                       "%llu-tick cooldown.",
                       (unsigned long long)BTD5_PHASE_PERIODIC_SAMPLE_TICKS,
                       (unsigned long long)BTD5_PHASE_SLOW_SAMPLE_COOLDOWN);
            } else {
                phase_probe_disabled = true;
                l_error("BTD5 4.7 nested engine call-site bridges could not "
                        "be installed; profiler disabled without changing "
                        "normal gameplay execution.");
            }
        } else {
            l_info("Native phase profiler compiled but not installed in the "
                   "performance VPK.");
        }
#endif
#ifdef BTD5_FRAME_DEBT_CLAMP
        if (install_frame_debt_clamp_bridge()) {
            l_warn("Experimental BTD5 4.7 frame-debt clamp enabled: positive "
                   "frame delta values above 33.33 ms are capped before the "
                   "active screen update. This may trade catch-up speed for "
                   "steadier frame pacing under severe load.");
        } else {
            l_error("BTD5 4.7 frame-debt clamp bridge could not be installed; "
                    "continuing with original timing behavior.");
        }
#endif
    }
}
