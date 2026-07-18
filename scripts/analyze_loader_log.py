#!/usr/bin/env python3
"""Summarize BTD5 Vita loader health and late-round performance telemetry."""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
LOADER_RE = re.compile(r"BTD5 loader ([0-9.]+) started\.")
VERSION_RE = re.compile(r"Selected BTD5 ([0-9.]+) data")
TICK_RE = re.compile(r"Tick loop alive: (\d+) calls\.")
AUTOSAVE_RE = re.compile(r"Autosave checkpoint (\d+) committed")
KUSER_RE = re.compile(
    r"Patched ARM kuser helpers: (\d+) cmpxchg, (\d+) memory barriers")
FINGERPRINT_RE = re.compile(
    r"Verified BTD5 ([0-9.]+) native fingerprint: (\d+) bytes, "
    r"CRC32 ([0-9A-Fa-f]{8})\.")
FRAME_RE = re.compile(
    r"Frame timing: nativeTick avg=([0-9.]+) ms, max=([0-9.]+) ms, "
    r">=33/50/100ms=(\d+)/(\d+)/(\d+) \((\d+) frames\); "
    r"EGL swap avg=([0-9.]+) ms, max=([0-9.]+) ms \((\d+) swaps\)\.")
AUDIO_RE = re.compile(
    r"Effects audio: players=(\d+) \(failed=(\d+)\), enqueues=(\d+) "
    r"\(failed=(\d+)\), mixed=(\d+)/(\d+) buffers, "
    r"last=(\d+) Hz/(\d+) ch/(\d+)-bit, completed=(\d+), "
    r"delayed_start=(\d+), output_error=(0x[0-9a-fA-F]+)"
    r"(?:, resample alloc/reuse/grow=(\d+)/(\d+)/(\d+) "
    r"\(max=(\d+) bytes\))?\.")
GL_DRAW_RE = re.compile(
    r"GL draw load: arrays=(\d+) \((\d+) vertices\), "
    r"elements=(\d+) \((\d+) indices\) over (\d+) frames\.")


@dataclass
class FrameInterval:
    native_average_ms: float
    native_max_ms: float
    over_33ms: int
    over_50ms: int
    over_100ms: int
    frames: int
    swap_average_ms: float
    swap_max_ms: float
    swaps: int


@dataclass
class AudioStatus:
    players: int
    player_failures: int
    enqueues: int
    enqueue_failures: int
    nonzero_buffers: int
    output_buffers: int
    source_rate: int
    source_channels: int
    source_bits: int
    completions: int
    delayed_start: int
    output_error: str
    allocations: int | None
    reuses: int | None
    grows: int | None
    max_bytes: int | None


@dataclass
class DrawInterval:
    array_calls: int
    array_vertices: int
    element_calls: int
    element_indices: int
    frames: int


@dataclass
class LoaderReport:
    loader_version: str | None = None
    game_version: str | None = None
    final_tick: int = 0
    autosave_checkpoint: int = 0
    kuser_cmpxchg: int | None = None
    kuser_barriers: int | None = None
    fingerprint_version: str | None = None
    fingerprint_size: int | None = None
    fingerprint_crc: str | None = None
    cancelled_drags: int = 0
    frames: list[FrameInterval] = field(default_factory=list)
    draws: list[DrawInterval] = field(default_factory=list)
    audio: list[AudioStatus] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    diagnostics: list[str] = field(default_factory=list)


def clean_line(line: str) -> str:
    return ANSI_ESCAPE.sub("", line).strip()


def parse_log(text: str) -> LoaderReport:
    report = LoaderReport()
    for raw_line in text.splitlines():
        line = clean_line(raw_line)
        if not line:
            continue

        match = LOADER_RE.search(line)
        if match:
            # A manually concatenated file can contain historical sessions.
            # The on-device logger replaces its file each boot, but keeping
            # only the final marker makes copied/combined logs unambiguous.
            if report.loader_version is not None:
                report = LoaderReport()
            report.loader_version = match.group(1)
        match = VERSION_RE.search(line)
        if match:
            report.game_version = match.group(1)
        match = TICK_RE.search(line)
        if match:
            report.final_tick = max(report.final_tick, int(match.group(1)))
        match = AUTOSAVE_RE.search(line)
        if match:
            report.autosave_checkpoint = max(
                report.autosave_checkpoint, int(match.group(1)))
        match = KUSER_RE.search(line)
        if match:
            report.kuser_cmpxchg = int(match.group(1))
            report.kuser_barriers = int(match.group(2))
        match = FINGERPRINT_RE.search(line)
        if match:
            report.fingerprint_version = match.group(1)
            report.fingerprint_size = int(match.group(2))
            report.fingerprint_crc = match.group(3).lower()

        match = FRAME_RE.search(line)
        if match:
            values = match.groups()
            report.frames.append(FrameInterval(
                native_average_ms=float(values[0]),
                native_max_ms=float(values[1]),
                over_33ms=int(values[2]),
                over_50ms=int(values[3]),
                over_100ms=int(values[4]),
                frames=int(values[5]),
                swap_average_ms=float(values[6]),
                swap_max_ms=float(values[7]),
                swaps=int(values[8]),
            ))

        match = AUDIO_RE.search(line)
        if match:
            values = match.groups()
            report.audio.append(AudioStatus(
                players=int(values[0]),
                player_failures=int(values[1]),
                enqueues=int(values[2]),
                enqueue_failures=int(values[3]),
                nonzero_buffers=int(values[4]),
                output_buffers=int(values[5]),
                source_rate=int(values[6]),
                source_channels=int(values[7]),
                source_bits=int(values[8]),
                completions=int(values[9]),
                delayed_start=int(values[10]),
                output_error=values[11],
                allocations=int(values[12]) if values[12] else None,
                reuses=int(values[13]) if values[13] else None,
                grows=int(values[14]) if values[14] else None,
                max_bytes=int(values[15]) if values[15] else None,
            ))

        match = GL_DRAW_RE.search(line)
        if match:
            values = [int(value) for value in match.groups()]
            report.draws.append(DrawInterval(
                array_calls=values[0],
                array_vertices=values[1],
                element_calls=values[2],
                element_indices=values[3],
                frames=values[4],
            ))

        if "Cancelled active tower placement after" in line:
            report.cancelled_drags += 1
        if "⚠ warning" in line:
            report.warnings.append(line)
            report.diagnostics.append(line)
        if "⨯ error" in line or "! fatal" in line:
            report.errors.append(line)
            report.diagnostics.append(line)
    return report


def weighted_average(intervals, average_name: str, count_name: str) -> float:
    total_count = sum(getattr(item, count_name) for item in intervals)
    if total_count == 0:
        return 0.0
    return sum(
        getattr(item, average_name) * getattr(item, count_name)
        for item in intervals
    ) / total_count


def performance_summary(report: LoaderReport) -> list[str]:
    if not report.frames:
        return ["Performance telemetry: absent from this log."]

    total_frames = sum(item.frames for item in report.frames)
    total_swaps = sum(item.swaps for item in report.frames)
    native_average = weighted_average(
        report.frames, "native_average_ms", "frames")
    swap_average = weighted_average(
        report.frames, "swap_average_ms", "swaps")
    native_max = max(item.native_max_ms for item in report.frames)
    swap_max = max(item.swap_max_ms for item in report.frames)
    over_33ms = sum(item.over_33ms for item in report.frames)
    over_50ms = sum(item.over_50ms for item in report.frames)
    over_100ms = sum(item.over_100ms for item in report.frames)
    approximate_fps = 1000.0 / native_average if native_average > 0 else 0.0
    outside_swap = max(0.0, native_average - swap_average)

    if native_average < 20.0 and over_100ms == 0:
        diagnosis = "healthy near-60-FPS interval average"
    elif native_average > 0 and swap_average / native_average >= 0.70:
        diagnosis = "render/swap-side time dominates (heuristic)"
    else:
        diagnosis = "game/audio CPU time outside EGL swap dominates (heuristic)"

    return [
        f"Performance intervals: {len(report.frames)}; frames={total_frames}; "
        f"swaps={total_swaps}",
        f"nativeTick: avg={native_average:.2f} ms "
        f"(~{approximate_fps:.1f} FPS), max={native_max:.1f} ms",
        f"EGL swap: avg={swap_average:.2f} ms, max={swap_max:.1f} ms; "
        f"outside swap≈{outside_swap:.2f} ms",
        f"Slow frames >=33/50/100 ms: "
        f"{over_33ms}/{over_50ms}/{over_100ms}",
        f"Performance diagnosis: {diagnosis}.",
    ]


def audio_summary(report: LoaderReport) -> list[str]:
    if not report.audio:
        return ["Effects telemetry: absent."]
    audio = report.audio[-1]
    result = [
        f"Effects: players={audio.players} (failed={audio.player_failures}), "
        f"enqueues={audio.enqueues} (failed={audio.enqueue_failures})",
        f"Mixer: nonzero={audio.nonzero_buffers}/{audio.output_buffers}, "
        f"completed={audio.completions}, output_error={audio.output_error}",
        f"Last source: {audio.source_rate} Hz/{audio.source_channels} ch/"
        f"{audio.source_bits}-bit",
    ]
    if audio.allocations is None:
        result.append("Resample cache telemetry: absent from this log.")
    else:
        operations = audio.allocations + audio.reuses + audio.grows
        reuse_percent = (100.0 * audio.reuses / operations
                         if operations else 0.0)
        result.append(
            f"Resample cache: alloc/reuse/grow="
            f"{audio.allocations}/{audio.reuses}/{audio.grows}, "
            f"reuse={reuse_percent:.1f}%, max={audio.max_bytes} bytes")
    return result


def draw_summary(report: LoaderReport) -> list[str]:
    if not report.draws:
        return ["GL draw telemetry: absent from this log."]
    frames = sum(item.frames for item in report.draws)
    array_calls = sum(item.array_calls for item in report.draws)
    element_calls = sum(item.element_calls for item in report.draws)
    vertices = sum(item.array_vertices for item in report.draws)
    indices = sum(item.element_indices for item in report.draws)
    divisor = frames if frames else 1
    return [
        f"GL draw load: {(array_calls + element_calls) / divisor:.1f} "
        f"calls/frame ({array_calls} arrays, {element_calls} indexed); "
        f"{(vertices + indices) / divisor:.1f} vertices+indices/frame",
    ]


def format_report(report: LoaderReport, required_version: str | None) -> tuple[str, bool]:
    problems = []
    if report.loader_version is None:
        problems.append("loader version marker is missing")
    elif required_version and report.loader_version != required_version:
        problems.append(
            f"loader {report.loader_version} does not match required "
            f"{required_version}")
    if not report.frames:
        problems.append("frame timing records are missing")
    if required_version and report.fingerprint_crc is None:
        problems.append(
            f"{required_version} native fingerprint confirmation is missing")
    if (report.game_version == "3.37" and report.kuser_cmpxchg == 0 and
            report.kuser_barriers == 0):
        problems.append(
            "3.37 selected but no ARM kuser helpers were patched; verify "
            "the installed 3.37 libnative.so")

    lines = [
        "BTD5 Vita loader report",
        f"Session: loader={report.loader_version or 'unknown'}, "
        f"game={report.game_version or 'unknown'}, final_tick={report.final_tick}",
        f"Native fingerprint: "
        f"{report.fingerprint_version or 'unverified'} "
        f"{report.fingerprint_size or 'unknown'} bytes CRC32 "
        f"{report.fingerprint_crc or 'unknown'}",
        f"Saves: highest committed autosave checkpoint="
        f"{report.autosave_checkpoint or 'none'}",
        f"Placement safety cancellations: {report.cancelled_drags}",
    ]
    lines.extend(performance_summary(report))
    lines.extend(draw_summary(report))
    lines.extend(audio_summary(report))
    lines.append(
        f"Diagnostics: warnings={len(report.warnings)}, "
        f"errors/fatals={len(report.errors)}")
    for line in report.diagnostics[-8:]:
        lines.append(f"  {line}")
    if problems:
        lines.append("Readiness: NOT READY for comparison:")
        lines.extend(f"  - {problem}" for problem in problems)
    else:
        lines.append("Readiness: telemetry is suitable for comparison.")
    return "\n".join(lines), not problems


def read_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze BTD5 Vita loader.log timing/audio/save telemetry")
    parser.add_argument("log", help="loader.log path, or - for standard input")
    parser.add_argument(
        "--require-version",
        help="report not-ready unless this exact loader version is present")
    args = parser.parse_args()
    try:
        report = parse_log(read_input(args.log))
    except OSError as error:
        parser.error(str(error))
    output, ready = format_report(report, args.require_version)
    print(output)
    return 0 if ready or not args.require_version else 2


if __name__ == "__main__":
    raise SystemExit(main())
