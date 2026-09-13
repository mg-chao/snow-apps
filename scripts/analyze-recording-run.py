"""Inspect one recording's media endpoints and cadence/memory by time window.

Run only after timed recordings have finished. The frozen benchmark executable
provides media inspection using the installed FFmpeg and offline audio decoding
using Windows Media Foundation (the deployed FFmpeg has no AAC decoder).
Raw inspection output is preserved alongside the JSON diagnostics. Time-window
observations are diagnostics, not independent trials for confidence intervals.
"""
import argparse
import csv
import io
import json
import math
import os
from pathlib import Path
import statistics
import subprocess


def read_csv(path):
    with path.open(encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def percentile(values, fraction):
    return sorted(values)[math.floor((len(values) - 1) * fraction + 0.5)] if values else None


def cadence_windows(identifiers, time_base, duration, seconds):
    windows = [dict(start_seconds=start, end_seconds=min(start + seconds, duration),
                    decoded_frames=0, fresh_frames=0, unreadable_ids=0)
               for start in range(0, math.ceil(duration), seconds)]
    previous = None
    regressions = 0
    previous_pts = None
    for frame in identifiers:
        pts = int(frame["pts"])
        if previous_pts is not None and pts < previous_pts:
            regressions += 1
        previous_pts = pts
        at = pts * time_base
        if not 0 <= at < duration:
            continue
        window = windows[min(int(at // seconds), len(windows) - 1)]
        window["decoded_frames"] += 1
        if int(frame["valid"]) != 1:
            window["unreadable_ids"] += 1
            continue
        identifier = int(frame["identifier"])
        if identifier != previous:
            window["fresh_frames"] += 1
        previous = identifier
    for window in windows:
        window["fresh_fps"] = window["fresh_frames"] / (window["end_seconds"] - window["start_seconds"])
    return windows, regressions


def memory_diagnostics(samples, duration):
    # Exclude initialization and the final sampler interval, which can contain
    # encoder teardown. Report the slope and range rather than imposing
    # an arbitrary leak threshold on noisy allocator/working-set observations.
    steady = [(float(row["seconds"]), int(row["private_bytes"]) / 1048576)
              for row in samples if max(5, duration * 0.2) <= float(row["seconds"]) < duration - 1]
    if len(steady) < 2:
        return {"steady_samples": len(steady)}
    center_x = statistics.mean(x for x, _ in steady)
    center_y = statistics.mean(y for _, y in steady)
    variance = sum((x - center_x) ** 2 for x, _ in steady)
    return dict(steady_samples=len(steady), private_min_mib=min(y for _, y in steady),
                private_max_mib=max(y for _, y in steady),
                private_slope_mib_per_minute=(60 * sum((x - center_x) * (y - center_y)
                                                     for x, y in steady) / variance) if variance else None)


def analyze(directory, executable, seconds):
    rows = read_csv(directory / "realtime-recording-benchmark.csv")
    if len(rows) != 1:
        raise ValueError("Select a directory containing exactly one measured recording")
    row = rows[0]
    media = directory / f"{row['scenario']}-sample-{row['sample']}.mp4"
    environment = os.environ.copy()
    root = Path(__file__).resolve().parents[1]
    environment["PATH"] = str(root / ".tools/vcpkg/installed/dynamic/x64-windows/bin") + os.pathsep + environment["PATH"]

    def inspect(mode, suffix):
        result = subprocess.run([str(executable), mode, str(media)], capture_output=True,
                                text=True, env=environment, check=False)
        media.with_suffix(suffix).write_text(result.stdout, encoding="utf-8")
        media.with_suffix(suffix + ".stderr.txt").write_text(result.stderr, encoding="utf-8")
        result.check_returncode()
        return list(csv.DictReader(io.StringIO(result.stdout)))

    streams = inspect("--inspect-media", ".streams.csv")
    video = next(stream for stream in streams if stream["kind"] == "Video")
    time_base = int(video["time_base_numerator"]) / int(video["time_base_denominator"])
    duration = float(video["last_packet_end_seconds"])
    windows, regressions = cadence_windows(read_csv(media.with_suffix(".identifiers.csv")),
                                          time_base, duration, seconds)
    latency_file = directory / f"{row['scenario']}-{row['sample']}-packet-latencies.csv"
    if latency_file.exists():
        latencies = read_csv(latency_file)
        for window in windows:
            values = [int(item["capture_to_first_packet_ns"]) / 1e6 for item in latencies
                      if window["start_seconds"] <= int(item["output_pts"]) / float(row["fps"]) < window["end_seconds"]]
            window.update(packet_samples=len(values), packet_latency_p95_ms=percentile(values, 0.95),
                          packet_latency_max_ms=max(values) if values else None)
    diagnostics = dict(schema_version=1, window_seconds=seconds, windows=windows,
                       decoded_pts_regressions=regressions, streams=streams,
                       dropped_audio_frames=int(row["dropped_audio_frames"]),
                       queued_video_replacements=int(row.get("queued_video_replacements", 0)),
                       memory=memory_diagnostics(read_csv(media.with_suffix(".usage.csv")), duration))
    audio_streams = [stream for stream in streams if stream["kind"] == "Audio"]
    if audio_streams:
        diagnostics["audio"] = inspect("--inspect-audio", ".audio.csv")[0]
        diagnostics["audio_minus_video_endpoint_seconds"] = float(audio_streams[0]["last_packet_end_seconds"]) - duration
    (directory / "recording-diagnostics.json").write_text(json.dumps(diagnostics, indent=2) + "\n")
    return diagnostics


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--window-seconds", type=int, default=30)
    options = parser.parse_args()
    if options.window_seconds <= 0:
        parser.error("Window size must be positive")
    print(json.dumps(analyze(options.directory.resolve(), options.executable.resolve(),
                             options.window_seconds), indent=2))
