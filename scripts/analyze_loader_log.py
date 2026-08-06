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
PROFILE_COMMIT_RE = re.compile(
    r"Autosave checkpoint (\d+) committed \(.*?, ([0-9.]+) ms\)\.")
CPU_COUNT_RE = re.compile(
    r"Android sysconf: reporting (\d+) online Vita user CPUs\.")
TOUCH_OVERFLOW_RE = re.compile(
    r"(?:Touch sampler|Semantic touch) queue overflowed (\d+) times")
KUSER_RE = re.compile(
    r"Patched ARM kuser helpers: (\d+) cmpxchg, (\d+) memory barriers")
FINGERPRINT_RE = re.compile(
    r"Verified BTD5 ([0-9.]+) native fingerprint: (\d+) bytes, "
    r"CRC32 ([0-9A-Fa-f]{8})\.")
FRAME_RE = re.compile(
    r"Frame timing: nativeTick avg=([0-9.]+) ms, max=([0-9.]+) ms, "
    r">=33/50/100ms=(\d+)/(\d+)/(\d+) \((\d+) frames\); "
    r"EGL swap avg=([0-9.]+) ms, max=([0-9.]+) ms \((\d+) swaps\)\.")
NESTED_ENGINE_RE = re.compile(
    r"Nested engine update: inner@SO\+0x([0-9a-fA-F]+) "
    r"avg=([0-9.]+) ms max=([0-9.]+) ms "
    r"\((\d+) samples, target_changes=(\d+)\); outer "
    r"exclusive/residual avg=([0-9.]+) ms\.")
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
GL_SUBMIT_RE = re.compile(
    r"GL submit timing: draw CPU total=([0-9.]+) ms, "
    r"avg=(\d+) us/call, max=([0-9.]+) ms, "
    r">=1/4ms=(\d+)/(\d+); state cache skipped=(\d+)/(\d+) "
    r"\((\d+)%\)\.")

GL_BATCH_RE = re.compile(
    r"GL batching: logical arrays=(\d+), submitted=(\d+), "
    r"merged=(\d+) \((\d+)%, (\d+) vertices\); "
    r"state cache skipped=(\d+)/(\d+) \((\d+)%\)\.")
GL_SAMPLE_RE = re.compile(
    r"GL sampled submit timing: samples=(\d+) \(1/(\d+)\), "
    r"avg=(\d+) us, max=([0-9.]+) ms, >=1/4ms=(\d+)/(\d+)\.")
GL_PHASE_RE = re.compile(
    r"GL frame phases: pre-draw avg=([0-9.]+) ms "
    r"\(max=([0-9.]+)\), render-window avg=([0-9.]+) ms "
    r"\(max=([0-9.]+)\), draw/no-draw=(\d+)/(\d+) frames\.")
GL_UPLOAD_RE = re.compile(
    r"GL uploads: bufferData=(\d+), bufferSubData=(\d+), "
    r"uploaded=(\d+) bytes, skipped unchanged=(\d+) \((\d+) bytes\); "
    r"textures=(\d+) \((\d+) bytes\); uniforms=(\d+), "
    r"attribPointers=(\d+)\.")


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
class NestedEngineInterval:
    target_offset: int
    average_ms: float
    max_ms: float
    samples: int
    target_changes: int
    outer_exclusive_average_ms: float


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
class GLSubmitInterval:
    draw_cpu_total_ms: float
    draw_cpu_average_us: int
    draw_cpu_max_ms: float
    draw_over_1ms: int
    draw_over_4ms: int
    state_skipped: int
    state_calls: int
    state_skip_percent: int


@dataclass
class GLBatchInterval:
    logical_arrays: int
    submitted_arrays: int
    merged_arrays: int
    merged_percent: int
    merged_vertices: int
    state_skipped: int
    state_calls: int
    state_skip_percent: int


@dataclass
class GLSampleInterval:
    samples: int
    stride: int
    average_us: int
    max_ms: float
    over_1ms: int
    over_4ms: int


@dataclass
class GLPhaseInterval:
    pre_draw_average_ms: float
    pre_draw_max_ms: float
    render_window_average_ms: float
    render_window_max_ms: float
    draw_frames: int
    no_draw_frames: int


@dataclass
class GLUploadInterval:
    buffer_data_calls: int
    buffer_sub_data_calls: int
    uploaded_bytes: int
    skipped_uploads: int
    skipped_bytes: int
    texture_uploads: int
    texture_bytes: int
    uniform_calls: int
    attrib_pointer_calls: int


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
    reported_cpu_count: int | None = None
    profile_commit_ms: list[float] = field(default_factory=list)
    touch_queue_overflows: int = 0
    frames: list[FrameInterval] = field(default_factory=list)
    nested_engine: list[NestedEngineInterval] = field(default_factory=list)
    draws: list[DrawInterval] = field(default_factory=list)
    gl_submit: list[GLSubmitInterval] = field(default_factory=list)
    gl_batches: list[GLBatchInterval] = field(default_factory=list)
    gl_samples: list[GLSampleInterval] = field(default_factory=list)
    gl_phases: list[GLPhaseInterval] = field(default_factory=list)
    gl_uploads: list[GLUploadInterval] = field(default_factory=list)
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
        match = PROFILE_COMMIT_RE.search(line)
        if match:
            report.profile_commit_ms.append(float(match.group(2)))
        match = CPU_COUNT_RE.search(line)
        if match:
            report.reported_cpu_count = int(match.group(1))
        match = TOUCH_OVERFLOW_RE.search(line)
        if match:
            report.touch_queue_overflows = max(
                report.touch_queue_overflows, int(match.group(1)))
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

        match = NESTED_ENGINE_RE.search(line)
        if match:
            values = match.groups()
            report.nested_engine.append(NestedEngineInterval(
                target_offset=int(values[0], 16),
                average_ms=float(values[1]),
                max_ms=float(values[2]),
                samples=int(values[3]),
                target_changes=int(values[4]),
                outer_exclusive_average_ms=float(values[5]),
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

        match = GL_SUBMIT_RE.search(line)
        if match:
            values = match.groups()
            report.gl_submit.append(GLSubmitInterval(
                draw_cpu_total_ms=float(values[0]),
                draw_cpu_average_us=int(values[1]),
                draw_cpu_max_ms=float(values[2]),
                draw_over_1ms=int(values[3]),
                draw_over_4ms=int(values[4]),
                state_skipped=int(values[5]),
                state_calls=int(values[6]),
                state_skip_percent=int(values[7]),
            ))

        match = GL_BATCH_RE.search(line)
        if match:
            values = [int(value) for value in match.groups()]
            report.gl_batches.append(GLBatchInterval(
                logical_arrays=values[0],
                submitted_arrays=values[1],
                merged_arrays=values[2],
                merged_percent=values[3],
                merged_vertices=values[4],
                state_skipped=values[5],
                state_calls=values[6],
                state_skip_percent=values[7],
            ))

        match = GL_SAMPLE_RE.search(line)
        if match:
            values = match.groups()
            report.gl_samples.append(GLSampleInterval(
                samples=int(values[0]),
                stride=int(values[1]),
                average_us=int(values[2]),
                max_ms=float(values[3]),
                over_1ms=int(values[4]),
                over_4ms=int(values[5]),
            ))

        match = GL_PHASE_RE.search(line)
        if match:
            values = match.groups()
            report.gl_phases.append(GLPhaseInterval(
                pre_draw_average_ms=float(values[0]),
                pre_draw_max_ms=float(values[1]),
                render_window_average_ms=float(values[2]),
                render_window_max_ms=float(values[3]),
                draw_frames=int(values[4]),
                no_draw_frames=int(values[5]),
            ))

        match = GL_UPLOAD_RE.search(line)
        if match:
            values = [int(value) for value in match.groups()]
            report.gl_uploads.append(GLUploadInterval(
                buffer_data_calls=values[0],
                buffer_sub_data_calls=values[1],
                uploaded_bytes=values[2],
                skipped_uploads=values[3],
                skipped_bytes=values[4],
                texture_uploads=values[5],
                texture_bytes=values[6],
                uniform_calls=values[7],
                attrib_pointer_calls=values[8],
            ))

        if ("Cancelled active tower placement after" in line or
                "Cancelled unclaimed game drag" in line):
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

    slow_native_indices = [
        index for index, item in enumerate(report.frames)
        if item.native_average_ms >= 33.0 and item.swap_average_ms <= 5.0
    ]

    if slow_native_indices:
        worst_index = max(
            slow_native_indices,
            key=lambda index: report.frames[index].native_average_ms)
        worst_interval = report.frames[worst_index]
        matching_submit = (
            report.gl_submit[worst_index]
            if worst_index < len(report.gl_submit) else None)
        matching_phase = (
            report.gl_phases[worst_index]
            if worst_index < len(report.gl_phases) else None)
        matching_sample = (
            report.gl_samples[worst_index]
            if worst_index < len(report.gl_samples) else None)
        matching_batch = (
            report.gl_batches[worst_index]
            if worst_index < len(report.gl_batches) else None)

        worst_draw_ms_per_frame = (
            matching_submit.draw_cpu_total_ms / worst_interval.frames
            if matching_submit and worst_interval.frames else 0.0)
        sampled_submit_ms_per_frame = 0.0
        if (matching_sample and matching_batch and worst_interval.frames and
                matching_sample.samples):
            submitted_calls = (matching_batch.submitted_arrays +
                               (report.draws[worst_index].element_calls
                                if worst_index < len(report.draws) else 0))
            sampled_submit_ms_per_frame = (
                matching_sample.average_us * submitted_calls /
                worst_interval.frames / 1000.0)

        if (matching_phase and matching_phase.pre_draw_average_ms >=
                worst_interval.native_average_ms * 0.45):
            diagnosis = (
                "game/update work before the first draw dominates the worst "
                "interval; pre-draw time averaged "
                f"{matching_phase.pre_draw_average_ms:.1f} ms/frame")
        elif (matching_phase and matching_phase.render_window_average_ms >=
              worst_interval.native_average_ms * 0.60):
            diagnosis = (
                "the render window dominates the worst interval; work from "
                "the first draw to swap averaged "
                f"{matching_phase.render_window_average_ms:.1f} ms/frame")
            if sampled_submit_ms_per_frame > 0:
                diagnosis += (
                    f", while sampled draw calls estimate only "
                    f"{sampled_submit_ms_per_frame:.1f} ms/frame of direct "
                    "submission")
        elif (matching_submit and worst_draw_ms_per_frame >=
              worst_interval.native_average_ms * 0.45):
            diagnosis = (
                "vitaGL draw submission dominates the worst measured "
                "nativeTick interval; draw calls consumed "
                f"{worst_draw_ms_per_frame:.1f} ms/frame")
        elif ((matching_submit and worst_draw_ms_per_frame <=
               worst_interval.native_average_ms * 0.20) or
              (sampled_submit_ms_per_frame > 0 and
               sampled_submit_ms_per_frame <=
               worst_interval.native_average_ms * 0.20)):
            measured = (worst_draw_ms_per_frame if matching_submit
                        else sampled_submit_ms_per_frame)
            diagnosis = (
                "non-draw game/update work dominates the worst interval; "
                "measured or sampled draw submission consumed only "
                f"{measured:.1f} ms/frame")
        else:
            diagnosis = (
                "slow work is inside nativeTick; frame-phase telemetry is "
                "insufficient to isolate simulation from renderer preparation")
        diagnosis += (
            f"; worst interval averaged {worst_interval.native_average_ms:.1f} "
            f"ms with EGL swap at {worst_interval.swap_average_ms:.1f} ms")
    elif native_average < 20.0 and over_100ms == 0:
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


def nested_engine_summary(report: LoaderReport) -> list[str]:
    if not report.nested_engine:
        return ["Nested engine telemetry: absent from this log."]
    latest = report.nested_engine[-1]
    total = latest.average_ms + latest.outer_exclusive_average_ms
    share = (latest.average_ms / total * 100.0) if total > 0.0 else 0.0
    return [
        "Nested engine target: "
        f"SO+0x{latest.target_offset:08x}, avg={latest.average_ms:.1f} ms, "
        f"max={latest.max_ms:.1f} ms, samples={latest.samples}, "
        f"target changes={latest.target_changes}.",
        "Latest sampled outer-engine split: "
        f"inner={share:.1f}%, outer-exclusive="
        f"{latest.outer_exclusive_average_ms:.1f} ms.",
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
    result = [
        f"GL draw load: {(array_calls + element_calls) / divisor:.1f} "
        f"calls/frame ({array_calls} arrays, {element_calls} indexed); "
        f"{(vertices + indices) / divisor:.1f} vertices+indices/frame",
    ]

    if report.gl_batches:
        logical = sum(item.logical_arrays for item in report.gl_batches)
        submitted = sum(item.submitted_arrays for item in report.gl_batches)
        merged = sum(item.merged_arrays for item in report.gl_batches)
        merged_vertices = sum(
            item.merged_vertices for item in report.gl_batches)
        saved_percent = 100.0 * merged / logical if logical else 0.0
        total_state_calls = sum(
            item.state_calls for item in report.gl_batches)
        total_state_skipped = sum(
            item.state_skipped for item in report.gl_batches)
        skip_percent = (100.0 * total_state_skipped / total_state_calls
                        if total_state_calls else 0.0)
        result.append(
            f"GL batching: logical/submitted arrays={logical}/{submitted}, "
            f"merged={merged} ({saved_percent:.1f}%), "
            f"merged vertices={merged_vertices}")
        result.append(
            f"GL state cache: skipped={total_state_skipped}/"
            f"{total_state_calls} ({skip_percent:.1f}%)")
    elif report.gl_submit:
        total_state_calls = sum(
            item.state_calls for item in report.gl_submit)
        total_state_skipped = sum(
            item.state_skipped for item in report.gl_submit)
        skip_percent = (100.0 * total_state_skipped / total_state_calls
                        if total_state_calls else 0.0)
        result.append(
            f"GL state cache: skipped={total_state_skipped}/"
            f"{total_state_calls} ({skip_percent:.1f}%)")

    if report.gl_samples:
        samples = sum(item.samples for item in report.gl_samples)
        weighted_us = sum(
            item.average_us * item.samples for item in report.gl_samples)
        average_us = weighted_us / samples if samples else 0.0
        max_ms = max(item.max_ms for item in report.gl_samples)
        slow_1ms = sum(item.over_1ms for item in report.gl_samples)
        slow_4ms = sum(item.over_4ms for item in report.gl_samples)
        stride = report.gl_samples[-1].stride
        result.append(
            f"GL sampled submit: samples={samples} at 1/{stride}, "
            f"avg={average_us:.1f} us, max={max_ms:.1f} ms, "
            f">=1/4 ms={slow_1ms}/{slow_4ms}")
    elif report.gl_submit:
        total_draw_ms = sum(
            item.draw_cpu_total_ms for item in report.gl_submit)
        max_draw_ms = max(
            item.draw_cpu_max_ms for item in report.gl_submit)
        slow_1ms = sum(item.draw_over_1ms for item in report.gl_submit)
        slow_4ms = sum(item.draw_over_4ms for item in report.gl_submit)
        draw_ms_per_frame = total_draw_ms / divisor
        result.append(
            f"GL submit CPU: {draw_ms_per_frame:.2f} ms/frame, "
            f"max call={max_draw_ms:.1f} ms, >=1/4 ms calls="
            f"{slow_1ms}/{slow_4ms}")
    else:
        result.append(
            "GL submit timing: absent; low CPU percentages alone cannot "
            "prove a GPU bottleneck because vitaGL work may block inside "
            "nativeTick/draw calls.")

    if report.gl_phases:
        draw_frames = sum(item.draw_frames for item in report.gl_phases)
        pre_draw = (
            sum(item.pre_draw_average_ms * item.draw_frames
                for item in report.gl_phases) / draw_frames
            if draw_frames else 0.0)
        render_window = (
            sum(item.render_window_average_ms * item.draw_frames
                for item in report.gl_phases) / draw_frames
            if draw_frames else 0.0)
        pre_draw_max = max(item.pre_draw_max_ms for item in report.gl_phases)
        render_max = max(
            item.render_window_max_ms for item in report.gl_phases)
        no_draw = sum(item.no_draw_frames for item in report.gl_phases)
        result.append(
            f"GL frame phases: pre-draw={pre_draw:.2f} ms "
            f"(max {pre_draw_max:.1f}), render-window={render_window:.2f} ms "
            f"(max {render_max:.1f}), no-draw frames={no_draw}")

    if report.gl_uploads:
        buffer_data = sum(
            item.buffer_data_calls for item in report.gl_uploads)
        buffer_sub = sum(
            item.buffer_sub_data_calls for item in report.gl_uploads)
        uploaded = sum(item.uploaded_bytes for item in report.gl_uploads)
        skipped = sum(item.skipped_uploads for item in report.gl_uploads)
        skipped_bytes = sum(item.skipped_bytes for item in report.gl_uploads)
        textures = sum(item.texture_uploads for item in report.gl_uploads)
        texture_bytes = sum(item.texture_bytes for item in report.gl_uploads)
        uniforms = sum(item.uniform_calls for item in report.gl_uploads)
        attribs = sum(item.attrib_pointer_calls for item in report.gl_uploads)
        result.append(
            f"GL uploads: bufferData/subData={buffer_data}/{buffer_sub}, "
            f"bytes={uploaded}, unchanged skips={skipped} "
            f"({skipped_bytes} bytes), textures={textures} "
            f"({texture_bytes} bytes)")
        result.append(
            f"GL call pressure: uniforms={uniforms}, "
            f"attribPointers={attribs}")

    return result


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
        f"Android CPU count: "
        f"{report.reported_cpu_count if report.reported_cpu_count is not None else 'not logged'}",
        f"Placement safety cancellations: {report.cancelled_drags}; "
        f"touch queue overflows={report.touch_queue_overflows}",
    ]
    if report.profile_commit_ms:
        lines.append(
            "Profile write-back commits: "
            f"count={len(report.profile_commit_ms)}, "
            f"avg={sum(report.profile_commit_ms) / len(report.profile_commit_ms):.1f} ms, "
            f"max={max(report.profile_commit_ms):.1f} ms")
    else:
        lines.append("Profile write-back commit timing: absent from this log.")
    lines.extend(performance_summary(report))
    lines.extend(nested_engine_summary(report))
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
