import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import analyze_loader_log as analyzer  # noqa: E402


CURRENT_LOG = """
\x1b[32m! success\x1b[0m  BTD5 loader 01.00 started.
ℹ info     Selected BTD5 4.7 data at ux0:data/btd5/4.7/.
! success  Verified BTD5 4.7 native fingerprint: 8683424 bytes, CRC32 53BEA993.
ℹ info     Patched ARM kuser helpers: 0 cmpxchg, 0 memory barriers.
ℹ info     Tick loop alive: 6000 calls.
ℹ info     Frame timing: nativeTick avg=40.0 ms, max=140.0 ms, >=33/50/100ms=80/20/3 (250 frames); EGL swap avg=10.0 ms, max=30.0 ms (250 swaps).
ℹ info     Nested engine update: inner@SO+0x00345678 avg=30.0 ms max=95.0 ms (12 samples, target_changes=0); outer exclusive/residual avg=10.0 ms.
ℹ info     GL draw load: arrays=2500 (10000 vertices), elements=5000 (30000 indices) over 250 frames.
ℹ info     Effects audio: players=1 (failed=0), enqueues=5000 (failed=0), mixed=4000/5000 buffers, last=22050 Hz/2 ch/16-bit, completed=4999, delayed_start=0, output_error=0x00000000, resample alloc/reuse/grow=4/4992/4 (max=65536 bytes).
! success  Autosave checkpoint 12 committed (completed game save).
"""


class AnalyzerTests(unittest.TestCase):
    def test_current_log_parsing_and_cpu_diagnosis(self):
        report = analyzer.parse_log(CURRENT_LOG)
        self.assertEqual(report.loader_version, "01.00")
        self.assertEqual(report.game_version, "4.7")
        self.assertEqual(report.final_tick, 6000)
        self.assertEqual(report.fingerprint_crc, "53bea993")
        self.assertEqual(report.autosave_checkpoint, 12)
        self.assertEqual(report.audio[-1].reuses, 4992)
        self.assertEqual(report.draws[-1].element_calls, 5000)
        self.assertEqual(report.nested_engine[-1].target_offset, 0x00345678)
        self.assertEqual(report.nested_engine[-1].average_ms, 30.0)
        summary = "\n".join(analyzer.performance_summary(report))
        self.assertIn("game/audio CPU time outside EGL swap dominates", summary)
        self.assertIn("30.0 calls/frame", analyzer.draw_summary(report)[0])
        nested = "\n".join(analyzer.nested_engine_summary(report))
        self.assertIn("SO+0x00345678", nested)
        self.assertIn("inner=75.0%", nested)
        output, ready = analyzer.format_report(report, "01.00")
        self.assertTrue(ready, output)

    def test_old_or_mismatched_log_is_not_ready(self):
        report = analyzer.parse_log(
            "BTD5 loader 00.43 started.\n"
            "Selected BTD5 3.37 data.\n"
            "Patched ARM kuser helpers: 0 cmpxchg, 0 memory barriers.\n")
        output, ready = analyzer.format_report(report, "01.00")
        self.assertFalse(ready)
        self.assertIn("frame timing records are missing", output)
        self.assertIn("verify the installed 3.37 libnative.so", output)

    def test_swap_dominated_diagnosis(self):
        report = analyzer.parse_log(CURRENT_LOG.replace(
            "EGL swap avg=10.0", "EGL swap avg=35.0"))
        summary = "\n".join(analyzer.performance_summary(report))
        self.assertIn("render/swap-side time dominates", summary)

    def test_only_latest_concatenated_session_is_used(self):
        report = analyzer.parse_log(
            "BTD5 loader 00.43 started.\n"
            "Tick loop alive: 9999 calls.\n"
            "⚠ warning  historical warning\n" + CURRENT_LOG)
        self.assertEqual(report.loader_version, "01.00")
        self.assertEqual(report.final_tick, 6000)
        self.assertEqual(report.warnings, [])


if __name__ == "__main__":
    unittest.main()
