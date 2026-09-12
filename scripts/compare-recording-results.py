"""Paired relative changes and two-sided Student-t 95% confidence intervals.

Positive numbers mean improvement. Samples are paired by run index; intervals
are computed on log ratios, not by treating per-frame observations as trials.
"""
import csv
import json
import math
import statistics
import sys
from pathlib import Path


def read_sample(directory):
    with (directory / "realtime-recording-benchmark.csv").open() as stream:
        row = next(csv.DictReader(stream))
    values = {key: float(row[key]) for key in (
        "useful_fps", "cpu_percent", "private_peak_mib", "setup_ms", "stop_ms",
        "working_set_delta_mib", "unreadable_ids", "dropped_capture_frames")}
    for key in ("private_plateau_mib", "first_packet_ms", "time_to_first_handoff_ms",
                "encoder_copied_bytes", "superseded_capture_frames", "missed_output_slots",
                "input_failures"):
        if row.get(key):
            values[key] = float(row[key])
    stages = directory / "realtime-recording-stages.csv"
    if stages.exists():
        with stages.open() as stream:
            for stage in csv.DictReader(stream):
                for percentile in ("count", "p50_ms", "p95_ms", "max_ms"):
                    values[f"{stage['stage']}.{percentile}"] = float(stage[percentile])
    return values


def compare(root):
    pairs = []
    for index in range(1, 11):
        a, b = root / f"baseline-{index}", root / f"candidate-{index}"
        if not a.is_dir() or not b.is_dir():
            break
        pairs.append((read_sample(a), read_sample(b)))
    critical = {5: 2.776445105, 10: 2.262157163}[len(pairs)]
    invalid = [f"{variant}-{i}" for i, pair in enumerate(pairs, 1)
               for variant, values in zip(("baseline", "candidate"), pair)
               if values["unreadable_ids"] > 0 or values.get("input_failures", 0) > 0]
    result = {"pairs": len(pairs), "confidence": 0.95,
              "measurement_valid": not invalid, "invalid_samples": invalid, "metrics": {}}
    for metric in sorted(set.intersection(*(set(a) & set(b) for a, b in pairs))):
        baseline = [a[metric] for a, _ in pairs]
        candidate = [b[metric] for _, b in pairs]
        entry = {"baseline_mean": statistics.mean(baseline),
                 "candidate_mean": statistics.mean(candidate)}
        if min(baseline + candidate) > 0:
            sign = 1 if metric == "useful_fps" else -1
            changes = [sign * math.log(b / a) for a, b in zip(baseline, candidate)]
            center = statistics.mean(changes)
            radius = critical * statistics.stdev(changes) / math.sqrt(len(changes))
            benefit = (lambda x: 100 * math.expm1(x)) if sign == 1 else (lambda x: -100 * math.expm1(-x))
            entry.update(improvement_percent=benefit(center),
                         ci95_low_percent=benefit(center - radius),
                         ci95_high_percent=benefit(center + radius))
        result["metrics"][metric] = entry
    (root / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    for metric in ("useful_fps", "cpu_percent", "private_peak_mib", "pipeline.end_to_end.p95_ms"):
        if metric in result["metrics"]:
            print(metric, json.dumps(result["metrics"][metric]))


if __name__ == "__main__":
    compare(Path(sys.argv[1]))
