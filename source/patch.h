#ifndef BTD5_PATCH_H
#define BTD5_PATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BTD5NativePhaseStats {
    uint64_t sampled_ticks;
    uint64_t sampled_tick_total_us;
    uint64_t sampled_tick_max_us;

    uint64_t pre_engine_samples;
    uint64_t pre_engine_total_us;
    uint64_t pre_engine_max_us;

    uint64_t engine_samples;
    uint64_t engine_total_us;
    uint64_t engine_max_us;
    uintptr_t engine_target;
    uint64_t engine_target_changes;

    uint64_t inner_update_samples;
    uint64_t inner_update_total_us;
    uint64_t inner_update_max_us;
    uintptr_t inner_update_target;
    uint64_t inner_update_target_changes;

    uint64_t gl_draw_calls;
    uint64_t gl_draw_vertices;
    uint64_t gl_draw_cpu_us;
    uint64_t gl_draw_cpu_max_us;

    uint64_t periodic_samples;
    uint64_t slow_triggered_samples;
} BTD5NativePhaseStats;

bool btd5_native_phase_profiler_enabled(void);
void btd5_native_phase_probe_begin(uint64_t tick_number,
                                   uint64_t previous_tick_us);
void btd5_native_phase_probe_end(void);
void btd5_take_native_phase_stats(BTD5NativePhaseStats *stats);
uint32_t btd5_take_frame_debt_clamp_count(void);

uint64_t btd5_engine_call_profile_enter(void *engine, uintptr_t target);
void btd5_engine_call_profile_exit(uint64_t started_us, uintptr_t target);
uint64_t btd5_inner_update_profile_enter(void *screen, void *context,
                                         uintptr_t target);
void btd5_inner_update_profile_exit(uint64_t started_us, uintptr_t target);

#ifdef BTD5_NATIVE_PHASE_PROFILER
extern volatile uint32_t btd5_engine_profile_sample_active;
extern uintptr_t btd5_engine_callsite_continue;
extern uintptr_t btd5_inner_update_callsite_continue;
void btd5_engine_callsite_trampoline(void);
void btd5_inner_update_callsite_trampoline(void);
#endif

#ifdef BTD5_FRAME_DEBT_CLAMP
extern uintptr_t btd5_frame_debt_callsite_continue;
extern uintptr_t btd5_frame_debt_prepare_target;
extern volatile uint32_t btd5_frame_debt_clamp_count;
void btd5_frame_debt_callsite_trampoline(void);
#endif

#endif
