#!/usr/bin/env python3
"""Verify a supported BTD5 ARMv7 libnative.so and extract nativeTick evidence."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path


NATIVE_TICK = "_Z23MainActivity_nativeTickP7_JNIEnvP8_jobject"
SUPPORTED = {
    (9_851_888, 0x37AFBB66): "3.37",
    (8_683_424, 0x53BEA993): "4.7",
}
SYMBOL_RE = re.compile(
    rf"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+\S+\s+{re.escape(NATIVE_TICK)}$"
)
CALL_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):.*\bblx?\s+([0-9a-fA-Fx]+)(?:\s+<([^>]+)>)?"
)
INDIRECT_CALL_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):.*\bblx?\s+(r(?:1[0-5]|[0-9])|ip|lr)\b",
    re.IGNORECASE,
)
ENGINE_CALLSITE = {
    0x00500994: "ldr r1, [r0]",
    0x00500996: "ldr r1, [r1, #0x18]",
    0x00500998: "blx r1",
    0x0050099A: "ldr r0, [r5, #0x20]",
}


@dataclass(frozen=True)
class Toolchain:
    readelf: str
    objdump: str
    objdump_kind: str


def fingerprint(path: Path) -> tuple[int, int]:
    crc = 0
    size = 0
    with path.open("rb") as handle:
        while block := handle.read(1024 * 1024):
            size += len(block)
            crc = zlib.crc32(block, crc)
    return size, crc & 0xFFFFFFFF


def find_tool(names: list[str]) -> str:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise RuntimeError("missing tool: tried " + ", ".join(names))


def find_toolchain() -> Toolchain:
    readelf = find_tool([
        "arm-vita-eabi-readelf",
        "arm-linux-gnueabihf-readelf",
        "llvm-readelf",
        "readelf",
    ])

    # Prefer LLVM over a generic host objdump. Debian/Ubuntu's /usr/bin/objdump
    # is frequently built without ARM support, while LLVM can decode this
    # stripped Android Thumb-2 binary when given an explicit target triple.
    llvm_objdump = shutil.which("llvm-objdump")
    if llvm_objdump:
        return Toolchain(readelf, llvm_objdump, "llvm")

    gnu_objdump = find_tool([
        "arm-vita-eabi-objdump",
        "arm-linux-gnueabihf-objdump",
    ])
    return Toolchain(readelf, gnu_objdump, "gnu-arm")


def run(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}"
        )
    return result.stdout


def locate_native_tick(readelf_output: str) -> tuple[int, int]:
    matches: list[tuple[int, int]] = []
    for line in readelf_output.splitlines():
        match = SYMBOL_RE.match(line)
        if match:
            matches.append((int(match.group(1), 16), int(match.group(2))))
    if not matches:
        raise RuntimeError(
            f"exported symbol {NATIVE_TICK} was not found; verify this is the "
            "supported ARMv7 libnative.so rather than an ARM64 or stripped "
            "replacement"
        )
    return max(matches, key=lambda item: item[1])


def disassemble_native_tick(
    tools: Toolchain, library: Path, address: int, symbol_size: int
) -> str:
    # ELF function symbols mark Thumb entry points with bit zero set. objdump
    # expects the actual even byte address. Keep the symbol's reported size.
    start = address & ~1
    end = start + max(symbol_size, 4)

    if tools.objdump_kind == "llvm":
        command = [
            tools.objdump,
            "--triple=thumbv7-none-linux-gnueabihf",
            "--disassemble",
            "--demangle",
            f"--start-address=0x{start:x}",
            f"--stop-address=0x{end:x}",
            str(library),
        ]
    else:
        command = [
            tools.objdump,
            "-d",
            "-M",
            "force-thumb",
            "-C",
            f"--start-address=0x{start:x}",
            f"--stop-address=0x{end:x}",
            str(library),
        ]
    return run(command)


def summarize_calls(disassembly: str) -> str:
    counts: dict[str, int] = {}
    records: list[str] = []
    indirect: list[str] = []
    callsite_lines: dict[int, str] = {}
    for line in disassembly.splitlines():
        address_match = re.match(r"^\s*([0-9a-fA-F]+):\s+(.*)$", line)
        if address_match:
            address = int(address_match.group(1), 16)
            if address in ENGINE_CALLSITE:
                callsite_lines[address] = address_match.group(2).strip()

        match = CALL_RE.match(line)
        if match:
            caller = match.group(1)
            target = match.group(2).lower()
            if not target.startswith("0x"):
                target = "0x" + target
            name = match.group(3) or "unknown"
            key = f"{target} <{name}>"
            counts[key] = counts.get(key, 0) + 1
            records.append(f"0x{caller.lower()} -> {key}")
            continue

        indirect_match = INDIRECT_CALL_RE.match(line)
        if indirect_match:
            indirect.append(
                f"0x{indirect_match.group(1).lower()} -> indirect "
                f"{indirect_match.group(2).lower()}"
            )

    output = ["nativeTick direct call sites", ""]
    output.extend(records or ["No direct BL/BLX call sites were parsed."])
    output.extend(["", "nativeTick indirect call sites", ""])
    output.extend(indirect or ["No register-indirect BLX call sites were parsed."])
    output.extend(["", "Direct target frequency", ""])
    for target, count in sorted(
        counts.items(), key=lambda item: (-item[1], item[0])
    ):
        output.append(f"{count:4d}  {target}")

    output.extend(["", "BTD5 4.7 engine call-site verification", ""])
    verified = True
    for address, expected in ENGINE_CALLSITE.items():
        actual = callsite_lines.get(address, "missing")
        normalized = re.sub(r"\s+", " ", actual.lower())
        expected_normalized = re.sub(r"\s+", " ", expected.lower())
        ok = expected_normalized in normalized
        verified = verified and ok
        output.append(
            f"0x{address:08x}: {'OK' if ok else 'MISMATCH'} "
            f"expected={expected!r} actual={actual!r}"
        )
    output.append(f"engine_callsite_verified={int(verified)}")
    return "\n".join(output) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Verify BTD5 libnative.so and extract the exported nativeTick "
            "disassembly/call list for version-specific optimization work"
        )
    )
    parser.add_argument("library", type=Path, help="path to libnative.so")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("native-tick-analysis"),
        help="directory for reports (default: native-tick-analysis)",
    )
    args = parser.parse_args()

    if not args.library.is_file():
        parser.error(f"not a file: {args.library}")

    size, crc = fingerprint(args.library)
    version = SUPPORTED.get((size, crc))
    if not version:
        supported = ", ".join(
            f"{name}: {known_size} bytes CRC32 {known_crc:08X}"
            for (known_size, known_crc), name in SUPPORTED.items()
        )
        parser.error(
            f"unsupported native fingerprint: {size} bytes CRC32 {crc:08X}; "
            f"expected {supported}"
        )

    try:
        tools = find_toolchain()
        symbols = run([tools.readelf, "-Ws", str(args.library)])
        address, symbol_size = locate_native_tick(symbols)
        disassembly = disassemble_native_tick(
            tools, args.library, address, symbol_size
        )
    except RuntimeError as error:
        parser.error(str(error))

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "nativeTick-disassembly.txt").write_text(
        disassembly, encoding="utf-8"
    )
    (args.output_dir / "nativeTick-call-targets.txt").write_text(
        summarize_calls(disassembly), encoding="utf-8"
    )
    (args.output_dir / "fingerprint.txt").write_text(
        f"BTD5 {version}\n"
        f"size={size}\n"
        f"crc32={crc:08X}\n"
        f"nativeTick=0x{address:08X}\n"
        f"nativeTick_size={symbol_size}\n",
        encoding="utf-8",
    )

    print(f"BTD5 {version} native executable verified.")
    print(f"nativeTick: 0x{address:08X}, symbol size {symbol_size} bytes")
    print(f"Reports: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
