import sys
import unittest
from pathlib import Path
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import analyze_native_tick as analyzer  # noqa: E402


class NativeTickAnalyzerTests(unittest.TestCase):
    def test_thumb_symbol_address_is_normalized_for_llvm(self) -> None:
        tools = analyzer.Toolchain("readelf", "llvm-objdump", "llvm")
        with patch.object(analyzer, "run", return_value="ok") as run_mock:
            output = analyzer.disassemble_native_tick(
                tools, Path("libnative.so"), 0x00500591, 1196
            )
        self.assertEqual(output, "ok")
        command = run_mock.call_args.args[0]
        self.assertIn("--triple=thumbv7-none-linux-gnueabihf", command)
        self.assertIn("--start-address=0x500590", command)
        self.assertIn("--stop-address=0x500a3c", command)

    def test_gnu_arm_command_forces_thumb(self) -> None:
        tools = analyzer.Toolchain("readelf", "arm-vita-eabi-objdump", "gnu-arm")
        with patch.object(analyzer, "run", return_value="ok") as run_mock:
            analyzer.disassemble_native_tick(
                tools, Path("libnative.so"), 0x00500591, 1196
            )
        command = run_mock.call_args.args[0]
        self.assertEqual(command[1:5], ["-d", "-M", "force-thumb", "-C"])


    def test_indirect_engine_callsite_is_reported_and_verified(self) -> None:
        disassembly = """
  500994: 6801          ldr r1, [r0]
  500996: 6989          ldr r1, [r1, #0x18]
  500998: 4788          blx r1
  50099a: 6a28          ldr r0, [r5, #0x20]
  50099c: f7db f880     bl 0x4dbaa0 <post>
"""
        summary = analyzer.summarize_calls(disassembly)
        self.assertIn("0x500998 -> indirect r1", summary)
        self.assertIn("engine_callsite_verified=1", summary)
        self.assertIn("0x4dbaa0 <post>", summary)

    def test_generic_objdump_is_not_selected_as_arm_fallback(self) -> None:
        lookup = {
            "readelf": "/usr/bin/readelf",
            "llvm-objdump": "/usr/bin/llvm-objdump",
        }
        with patch.object(
            analyzer.shutil, "which", side_effect=lambda name: lookup.get(name)
        ):
            tools = analyzer.find_toolchain()
        self.assertEqual(tools.objdump_kind, "llvm")
        self.assertEqual(tools.objdump, "/usr/bin/llvm-objdump")


if __name__ == "__main__":
    unittest.main()
