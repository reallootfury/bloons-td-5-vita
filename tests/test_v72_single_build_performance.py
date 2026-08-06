import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class V72SingleBuildPerformanceTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_v70_audio_object_replacement_remains_reverted(self) -> None:
        cmake = self.read("CMakeLists.txt")
        self.assertIn("${SOFTFP_DEPS_DIR}/libOpenSLES.a", cmake)
        self.assertNotIn("libOpenSLES-btd5-fast.a", cmake)
        self.assertNotIn("IBufferQueue_vita_fast.c", cmake)
        self.assertFalse((ROOT / "source/opensles/IBufferQueue_vita_fast.c").exists())

    def test_frame_debt_clamp_is_enabled_and_fingerprint_gated(self) -> None:
        cmake = self.read("CMakeLists.txt")
        patch = self.read("source/patch.c")
        assembly = self.read("source/engine_callsite.S")
        self.assertIn("option(BTD5_FRAME_DEBT_CLAMP", cmake)
        self.assertIn('active update" ON)', cmake)
        self.assertIn("0x0020ad56", patch)
        self.assertIn("0x0020ad66", patch)
        self.assertIn("0x004e7ffc", patch)
        self.assertIn("BTD5_47_FRAME_DEBT_PATCH_SIZE 10u", patch)
        self.assertIn("0xd4, 0xf8, 0x24, 0x01", patch)
        self.assertIn("0xd0, 0xf8, 0xd0, 0x00", patch)
        self.assertIn("frame_debt_patch_bytes + 2", patch)
        self.assertIn("frame_debt_patch_bytes + 6", patch)
        self.assertNotIn(
            "(patch_address & 3u)",
            patch.split("#ifdef BTD5_FRAME_DEBT_CLAMP", 1)[1],
        )
        self.assertIn("btd5_frame_debt_callsite_trampoline", assembly)
        self.assertIn("0x3d088889", assembly)
        self.assertIn("0x3f800000", assembly)
        self.assertIn("btd5_take_frame_debt_clamp_count", patch)

    def test_deep_profiler_is_logging_only_at_runtime(self) -> None:
        patch = self.read("source/patch.c")
        assembly = self.read("source/engine_callsite.S")
        main = self.read("source/main.c")
        self.assertIn("0x0020ad6cu", patch)
        self.assertIn("0x0020ad74u", patch)
        self.assertIn("btd5_inner_update_callsite_trampoline", assembly)
        self.assertIn("phase_probe_runtime_enabled", patch)
        self.assertIn("if (log_is_enabled())", patch)
        self.assertIn("btd5_native_phase_profiler_enabled", main)
        self.assertIn("Nested engine update:", main)
        self.assertIn("logging VPK", patch)
        self.assertNotIn("22k-stereo-fast", main)

    def test_one_command_build_uses_only_build_directory(self) -> None:
        script = self.read("build.sh")
        self.assertEqual(script.count("cmake -S . -B build"), 1)
        self.assertEqual(script.count("cmake --build build"), 1)
        self.assertNotIn("cmake -S . -B build-", script)
        self.assertNotIn("cmake --build build-", script)
        self.assertIn("-name 'build-*'", script)
        self.assertIn("cmake -E remove_directory build", script)
        self.assertIn("BTD5-v7.2-PERFORMANCE.vpk", script)
        self.assertIn("BTD5-v7.2-PERFORMANCE-logging.vpk", script)
        self.assertIn("BTD5-v7.2-PERFORMANCE.elf", script)
        self.assertIn("SHA256SUMS.txt", script)

    def test_one_build_generates_standard_and_logging_packages(self) -> None:
        cmake = self.read("CMakeLists.txt")
        script = self.read("build.sh")
        self.assertIn("btd5-vita-v1.00.vpk", script)
        self.assertIn("btd5-vita-v1.00-logging.vpk", script)
        self.assertIn("loader_logging.cfg", cmake)
        self.assertIn("${LOG_CONFIG}=loader_logging.cfg", cmake)

    def test_standard_runtime_avoids_profiler_and_telemetry_work(self) -> None:
        main = self.read("source/main.c")
        patch = self.read("source/patch.c")
        self.assertIn("const bool telemetry_enabled = log_is_enabled();", main)
        egl = self.read("source/reimpl/egl.c")
        diagnostics = self.read("source/diagnostics.c")
        self.assertIn("bool tick_timing_enabled = runtime_diagnostics_enabled;", main)
        self.assertIn("tick_timing_enabled ?", main)
        self.assertIn("if (telemetry_enabled && ticks % 60 == 0)", main)
        self.assertIn("if (runtime_diagnostics_enabled)", main)
        self.assertIn("if (log_is_enabled())", patch)
        self.assertIn("phase_probe_runtime_enabled = true", patch)
        self.assertIn("egl_diagnostics_enabled ?", egl)
        self.assertIn("if (egl_diagnostics_enabled)", egl)
        self.assertIn("if (!diagnostics_enabled)", diagnostics)

    def test_startup_message_identifies_v72_changes(self) -> None:
        main = self.read("source/main.c")
        self.assertIn("Performance v7.2", main)
        self.assertIn("single-build", main)
        self.assertIn("frame-debt protection", main)
        self.assertIn("vertex-layout state cache", main)


if __name__ == "__main__":
    unittest.main()
