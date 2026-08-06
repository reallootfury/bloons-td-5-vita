from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class StutterTouchCacheSourceTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_android_cpu_count_is_not_stubbed(self) -> None:
        dynlib = self.read("source/dynlib.c")
        sys_impl = self.read("source/reimpl/sys.c")
        self.assertIn('{ "sysconf", (uintptr_t)&sysconf_soloader }', dynlib)
        self.assertIn("BIONIC_SC_NPROCESSORS_ONLN     97", sys_impl)
        self.assertIn("BTD5_VITA_USER_CPU_COUNT        3", sys_impl)

    def test_cpu3_is_not_selected_automatically(self) -> None:
        main = self.read("source/main.c")
        pthr = self.read("source/reimpl/pthr.c")
        self.assertIn("SCE_KERNEL_CPU_MASK_USER_ALL", main)
        self.assertIn("SCE_KERNEL_CPU_MASK_USER_ALL", pthr)
        self.assertNotIn("worker_mask |= SCE_KERNEL_CPU_MASK_SYSTEM", main)

    def test_profile_sync_is_background_batched(self) -> None:
        main = self.read("source/main.c")
        self.assertIn("PROFILE_SYNC_QUIET_US", main)
        self.assertIn("profile_sync_worker", main)
        self.assertNotIn(
            'sync_profile_storage(false, "completed game save")', main
        )
        self.assertIn('sync_profile_storage(true, "system UI pause")', main)
        self.assertIn('sync_profile_storage(true, "clean exit after pause")', main)

    def test_touch_history_has_immediate_down_and_safe_release(self) -> None:
        controls = self.read("source/reimpl/controls.c")
        main = self.read("source/main.c")
        self.assertIn("sceTouchRead(SCE_TOUCH_PORT_FRONT", controls)
        self.assertIn("TOUCH_SAMPLE_QUEUE_CAPACITY 256U", controls)
        self.assertIn("Immediate in-game touch pickup enabled", controls)
        self.assertIn("touch_dispatch_immediate_game_down", controls)
        self.assertIn("touch_immediate_down", controls)
        self.assertIn("TOUCH_PICKUP_RETRY_FRAMES", controls)
        self.assertIn("deferred_touch_valid", controls)
        self.assertIn("Cancelled unclaimed game drag", controls)
        self.assertIn("native_touch_dispatch_mutex", main)

    def test_visual_compatibility_keeps_original_high_assets(self) -> None:
        settings = self.read("source/utils/settings.c")
        assets = self.read("source/reimpl/asset_manager.cpp")
        glutil = self.read("source/utils/glutil.c")
        self.assertIn("setting_low_graphics   = false", settings)
        self.assertNotIn("Assets/Textures/Low/", assets)
        self.assertNotIn("Performance asset redirect active", assets)
        self.assertNotIn("vglUseLowPrecision", glutil)
        self.assertIn("original High atlas assets", glutil)
        self.assertIn("Visual compatibility v5", glutil)

    def test_safe_state_cache_includes_conservative_vertex_layout_cache(self) -> None:
        dynlib = self.read("source/dynlib.c")
        glutil = self.read("source/utils/glutil.c")
        self.assertIn("glBindTexture_soloader", dynlib)
        self.assertIn("glUseProgram_soloader", dynlib)
        self.assertIn("gl_state_cache", glutil)
        self.assertIn("glVertexAttribPointer_soloader", dynlib)
        self.assertIn("GLVertexAttribPointerState", glutil)
        self.assertIn("gl_state_cache.array_buffer_valid", glutil)
        self.assertIn("state->array_buffer == gl_state_cache.array_buffer", glutil)
        self.assertNotIn("glBufferData_soloader", dynlib)
        self.assertNotIn("glUniform4fv_soloader", dynlib)
        self.assertNotIn("gl_uniform_skip", glutil)

    def test_release_draw_timers_are_disabled(self) -> None:
        glutil = self.read("source/utils/glutil.c")
        logger_h = self.read("source/utils/logger.h")
        main = self.read("source/main.c")
        self.assertIn("log_is_enabled", logger_h)
        self.assertIn("#ifdef DEBUG_SOLOADER", glutil)
        self.assertIn("two process-time syscalls per draw", glutil)
        self.assertIn("GL release telemetry", main)

    def test_vita_cpu_codegen_is_explicit(self) -> None:
        cmake = self.read("CMakeLists.txt")
        self.assertIn("-mcpu=cortex-a9", cmake)
        self.assertIn("-mtune=cortex-a9", cmake)
        self.assertIn("-mfpu=neon", cmake)
        self.assertIn("-mfloat-abi=softfp", cmake)

    def test_native_phase_profiler_is_exact_and_logging_gated(self) -> None:
        cmake = self.read("CMakeLists.txt")
        patch = self.read("source/patch.c")
        assembly = self.read("source/engine_callsite.S")
        main = self.read("source/main.c")
        header = self.read("source/patch.h")
        glutil = self.read("source/utils/glutil.c")
        self.assertIn("BTD5_NATIVE_PHASE_PROFILER", cmake)
        self.assertIn('"Compile the BTD5 4.7 native phase profiler; it installs only when loader logging is enabled" ON', cmake)
        self.assertIn("BTD5_NATIVE_PATCH_SOURCES", cmake)
        self.assertIn('COMPILE_FLAGS "-mcpu=cortex-a9 -mthumb"', cmake)
        self.assertIn("0x00500994u", patch)
        self.assertIn("0x0050099cu", patch)
        self.assertNotIn("update_47_hook", patch)
        self.assertNotIn("post_frame_47_hook", patch)
        self.assertIn("pre_engine_total_us", patch)
        self.assertIn("0x69896801", patch)
        self.assertIn("0x6a284788", patch)
        self.assertIn("BTD5_PHASE_SLOW_TRIGGER_US", patch)
        self.assertIn("BTD5_PHASE_SLOW_SAMPLE_COOLDOWN", patch)
        self.assertIn("btd5_native_phase_probe_begin(started, previous_tick_us)", main)
        self.assertIn("btd5_native_phase_profiler_enabled", main)
        self.assertIn("phase_probe_runtime_enabled", patch)
        self.assertIn("if (log_is_enabled())", patch)
        self.assertIn("Native engine samples:", main)
        self.assertIn("Sampled GL inside nativeTick:", main)
        self.assertIn("BTD5NativePhaseStats", header)
        self.assertIn("btd5_engine_callsite_trampoline", assembly)
        self.assertIn("ldr     r1, [r1, #0x18]", assembly)
        self.assertIn("mov     r2, r6", assembly)
        self.assertIn("mov     r3, r7", assembly)
        self.assertIn("gl_profile_sample_begin", glutil)

    def test_profiler_does_not_restore_removed_gl_wrappers(self) -> None:
        dynlib = self.read("source/dynlib.c")
        self.assertNotIn("glBufferData_soloader", dynlib)
        self.assertNotIn("glBufferSubData_soloader", dynlib)

    def test_packaged_cpuinfo_lists_three_user_processors(self) -> None:
        cpuinfo = self.read("extras/cpuinfo")
        processors = [
            line for line in cpuinfo.splitlines()
            if line.startswith("processor       :")
        ]
        self.assertEqual(
            processors,
            [
                "processor       : 0",
                "processor       : 1",
                "processor       : 2",
            ],
        )


    def test_v66_stability_rollback_preserves_v5_renderer_semantics(self) -> None:
        cmake = self.read("CMakeLists.txt")
        dynlib = self.read("source/dynlib.c")
        glutil = self.read("source/utils/glutil.c")
        main = self.read("source/main.c")
        self.assertIn("--allow-multiple-definition", cmake)
        self.assertNotIn("BTD5_ENABLE_LTO", cmake)
        self.assertNotIn("GLPendingArrayDraw", glutil)
        self.assertNotIn("gl_upload_shadow_matches", glutil)
        self.assertNotIn("glBufferData_soloader", dynlib)
        self.assertIn("Calling nativeResize.", main)
        self.assertIn("Calling nativeOrientationChanged.", main)
        self.assertIn("Calling nativeGainedAudioFocus.", main)
        self.assertIn("Calling nativeResume.", main)

    def test_v69_quick_source_drag_replays_held_input(self) -> None:
        controls = self.read("source/reimpl/controls.c")
        self.assertIn("TOUCH_QUICK_DRAG_RETRY_FRAMES      8U", controls)
        self.assertIn("TOUCH_QUICK_DRAG_RETRY_US", controls)
        self.assertIn("TOUCH_GAME_SOURCE_STRIP_FRACTION", controls)
        self.assertIn("started_in_game_source_strip", controls)
        self.assertIn("touch_replay_unclaimed_game_drag", controls)
        self.assertIn("Completed rapid source-strip drag", controls)
        self.assertIn("CONTROLS_ACTION_UP", controls)
        self.assertIn("Cancelled unclaimed game drag", controls)

    def test_v69_touch_hides_loader_cursor_fast_path(self) -> None:
        controls = self.read("source/reimpl/controls.c")
        self.assertIn("hide_cursor_for_front_touch", controls)
        self.assertIn("cursor_visible = false", controls)
        self.assertIn("removes the final scissor/clear overlay", controls)

    def test_v72_release_telemetry_is_runtime_gated(self) -> None:
        cmake = self.read("CMakeLists.txt")
        glutil = self.read("source/utils/glutil.c")
        egl = self.read("source/reimpl/egl.c")
        main = self.read("source/main.c")
        build_script = self.read("build.sh")
        self.assertIn("BTD5_RELEASE_TELEMETRY", cmake)
        self.assertIn('"Compile periodic telemetry; runtime collection occurs only in the logging VPK" ON', cmake)
        self.assertIn("BTD5_PERIODIC_TELEMETRY", cmake)
        self.assertIn("#if defined(DEBUG_SOLOADER) || defined(BTD5_RELEASE_TELEMETRY)", glutil)
        self.assertIn("#ifdef BTD5_PERIODIC_TELEMETRY", egl)
        self.assertIn("#ifdef BTD5_PERIODIC_TELEMETRY", main)
        self.assertIn("-DBTD5_RELEASE_TELEMETRY=ON", build_script)
        self.assertIn("const bool telemetry_enabled = log_is_enabled();", main)
        self.assertIn("if (telemetry_enabled && ticks % 60 == 0)", main)
        self.assertNotIn("build-v71-", build_script)

    def test_v69_worker_memory_and_cond_wait_scope(self) -> None:
        pthr = self.read("source/reimpl/pthr.c")
        self.assertIn("BTD5_ANDROID_WORKER_STACK (256U * 1024U)", pthr)
        self.assertNotIn("pthread_attr_setstacksize(&a, 512 * 1024)", pthr)
        self.assertIn("caller_offset == 0x004dd815u", pthr)
        self.assertIn("Only that fingerprinted call site needs a bounded", pthr)


if __name__ == "__main__":
    unittest.main()
