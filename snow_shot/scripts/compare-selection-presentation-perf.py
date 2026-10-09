#!/usr/bin/env python3
"""Compare matched smart-selection benchmark reports without conflating timer work."""

import argparse
import json
import math
import sys
from pathlib import Path


CONFIG_FIELDS = (
    "benchmark",
    "platform",
    "logical_width",
    "logical_height",
    "physical_width",
    "physical_height",
    "device_pixel_ratio",
    "warmup",
    "screen_refresh_hz",
    "animation_sample_interval_ms",
)
CASE_FIELDS = (
    "iterations",
    "requests_per_frame",
    "request_count",
    "clock",
    "frame_commit",
    "input_interval_ms",
    "maximum_requested_animation_idle_wait_ms",
)
COUNTERS = (
    "semantic_notifications",
    "hint_translation_requests",
    "canvas_paints",
    "ui_paints",
    "displayed_geometry_changes",
)
DISTRIBUTIONS = ("preparation_ms", "event_processing_ms", "total_ms")


def numeric(value, description):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{description} must be numeric")
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{description} must be finite and nonnegative")
    return value


def integer(value, description, positive=False):
    numeric(value, description)
    if value != int(value) or (positive and value == 0):
        raise ValueError(f"{description} must be a {'positive' if positive else 'nonnegative'} integer")
    return int(value)


def required(mapping, key, description):
    if key not in mapping:
        raise ValueError(f"{description} is missing {key}")
    return mapping[key]


def load_report(path, implementation):
    with path.open(encoding="utf-8-sig") as stream:
        report = json.load(stream)
    if not isinstance(report, dict):
        raise ValueError(f"{path}: expected a report object")
    if required(report, "benchmark", str(path)) != "screenshot_selection_presentation":
        raise ValueError(f"{path}: expected screenshot_selection_presentation benchmark")
    if required(report, "implementation", str(path)) != implementation:
        raise ValueError(f"{path}: expected implementation={implementation}")
    for key in CONFIG_FIELDS:
        required(report, key, str(path))
    for key in ("logical_width", "logical_height", "physical_width", "physical_height",
                "warmup", "animation_sample_interval_ms"):
        integer(report[key], f"{path}: {key}", positive=True)
    if numeric(report["device_pixel_ratio"], f"{path}: device_pixel_ratio") == 0:
        raise ValueError(f"{path}: device_pixel_ratio must be positive")
    numeric(report["screen_refresh_hz"], f"{path}: screen_refresh_hz")
    for axis in ("width", "height"):
        expected = math.ceil(report[f"logical_{axis}"] * report["device_pixel_ratio"])
        if report[f"physical_{axis}"] != expected:
            raise ValueError(f"{path}: physical_{axis} disagrees with logical size and DPR")
    raw_cases = required(report, "results", str(path))
    if not isinstance(raw_cases, list) or not raw_cases:
        raise ValueError(f"{path}: results must contain at least one scenario")
    cases = {}
    for case in raw_cases:
        if not isinstance(case, dict):
            raise ValueError(f"{path}: each scenario must be an object")
        name = required(case, "scenario", str(path))
        if not isinstance(name, str) or not name or name in cases:
            raise ValueError(f"{path}: invalid or duplicate scenario {name!r}")
        description = f"{path}: {name}"
        for key in CASE_FIELDS:
            required(case, key, description)
        for key in ("iterations", "requests_per_frame", "input_interval_ms"):
            integer(case[key], f"{description}: {key}", positive=True)
        integer(case["request_count"], f"{description}: request_count")
        integer(case["maximum_requested_animation_idle_wait_ms"],
                f"{description}: maximum_requested_animation_idle_wait_ms")
        for key in COUNTERS:
            integer(required(case, key, description), f"{description}: {key}")
        for key in DISTRIBUTIONS:
            distribution = required(case, key, description)
            for percentile in ("median", "p95", "mean"):
                numeric(required(distribution, percentile, f"{description}: {key}"),
                        f"{description}: {key}.{percentile}")
        numeric(required(case, "elapsed_ms", description), f"{description}: elapsed_ms")
        numeric(required(case, "mean_canvas_damage_ratio", description),
                f"{description}: mean_canvas_damage_ratio")
        cases[name] = case
    return report, cases


def delta(baseline, optimized):
    return {
        "baseline": baseline,
        "optimized": optimized,
        "reduction": baseline - optimized,
        "reduction_percent": 100 * (baseline - optimized) / baseline if baseline > 0 else None,
        "speedup": baseline / optimized if baseline > 0 and optimized > 0 else None,
    }


def optional_delta(baseline, optimized):
    return delta(baseline, optimized) if baseline is not None and optimized is not None else {
        "baseline": baseline, "optimized": optimized, "reduction": None,
        "reduction_percent": None, "speedup": None,
    }


def normalized_work(case, count_key, supplied_key):
    count = case[count_key]
    calculated = case["total_ms"]["mean"] * case["iterations"] / count if count else None
    if supplied_key in case:
        supplied = case[supplied_key]
        if supplied is not None:
            numeric(supplied, f"{case['scenario']}: {supplied_key}")
        if (calculated is None) != (supplied is None) or (
            calculated is not None and not math.isclose(calculated, supplied, rel_tol=1e-6, abs_tol=1e-8)
        ):
            raise ValueError(f"{case['scenario']}: {supplied_key} disagrees with mean measured work and count")
    return calculated


def compare_reports(baseline_path, optimized_path):
    baseline, baseline_cases = load_report(baseline_path, "baseline")
    optimized, optimized_cases = load_report(optimized_path, "optimized")
    mismatches = [key for key in CONFIG_FIELDS if baseline[key] != optimized[key]]
    if mismatches:
        raise ValueError("report configuration mismatch: " + ", ".join(mismatches))
    if baseline_cases.keys() != optimized_cases.keys():
        raise ValueError(
            "scenario mismatch: baseline-only=" + repr(sorted(baseline_cases.keys() - optimized_cases.keys()))
            + "; optimized-only=" + repr(sorted(optimized_cases.keys() - baseline_cases.keys()))
        )

    comparisons = []
    for name, old in baseline_cases.items():
        new = optimized_cases[name]
        mismatches = [key for key in CASE_FIELDS if old[key] != new[key]]
        if mismatches:
            raise ValueError(f"{name}: input/configuration mismatch: " + ", ".join(mismatches))
        if old["clock"] == "controlled_monotonic" and old["frame_commit"] == "explicit_frame":
            mode = "controlled"
        elif old["clock"] == "real_monotonic" and old["frame_commit"] == "scheduled_timer":
            mode = "real_animation"
        else:
            raise ValueError(f"{name}: unknown clock/frame_commit combination")
        idle_wait = old["maximum_requested_animation_idle_wait_ms"]
        if (mode == "controlled" and idle_wait != 0) or (
            mode == "real_animation" and idle_wait == 0
        ):
            raise ValueError(f"{name}: idle-wait metadata does not match the execution mode")
        if old["request_count"] > old["iterations"] * old["requests_per_frame"]:
            raise ValueError(f"{name}: request_count exceeds the supplied frame workload")
        if mode == "controlled" and old["request_count"] != old["iterations"] * old["requests_per_frame"]:
            raise ValueError(f"{name}: controlled request_count does not match the supplied frames")

        work_differences = [key for key in ("displayed_geometry_changes", "canvas_paints", "ui_paints")
                            if old[key] != new[key]]
        visible_work_matches = old["displayed_geometry_changes"] == new["displayed_geometry_changes"]
        canvas_work_matches = old["canvas_paints"] == new["canvas_paints"]
        matched_controlled_work = mode == "controlled" and visible_work_matches and canvas_work_matches
        if matched_controlled_work:
            claim = "Comparable cost per supplied frame with matching observed geometry changes and canvas paint counts."
        elif mode == "controlled":
            claim = "Matched inputs but different committed work; raw cost reductions need the geometry/paint count qualification."
        else:
            claim = (
                "Real timer-driven animation observation, not an equal-commit frame-speedup claim. "
                "Compare measured cost alongside observed geometry/paint cadence and normalized work."
            )
        comparisons.append({
            "scenario": name,
            "mode": mode,
            "matched_inputs": {key: old[key] for key in CASE_FIELDS},
            "costs_ms": {key: {percentile: delta(old[key][percentile], new[key][percentile])
                                for percentile in ("median", "p95", "mean")}
                         for key in DISTRIBUTIONS},
            "counts": {key: delta(old[key], new[key]) for key in COUNTERS},
            "elapsed_ms": delta(old["elapsed_ms"], new["elapsed_ms"]),
            "total_measured_work_ms": delta(old["total_ms"]["mean"] * old["iterations"],
                                            new["total_ms"]["mean"] * new["iterations"]),
            "work_ms_per_observed_canvas_paint": optional_delta(
                normalized_work(old, "canvas_paints", "work_ms_per_canvas_paint"),
                normalized_work(new, "canvas_paints", "work_ms_per_canvas_paint")),
            "work_ms_per_observed_geometry_change": optional_delta(
                normalized_work(old, "displayed_geometry_changes", "work_ms_per_geometry_change"),
                normalized_work(new, "displayed_geometry_changes", "work_ms_per_geometry_change")),
            "mean_canvas_damage_ratio": delta(old["mean_canvas_damage_ratio"], new["mean_canvas_damage_ratio"]),
            "committed_work": {
                "geometry_changes_match": visible_work_matches,
                "canvas_paints_match": canvas_work_matches,
                "ui_paints_match": old["ui_paints"] == new["ui_paints"],
                "mismatched_counts": work_differences,
                "matched_controlled_work": matched_controlled_work,
            },
            "claim_basis": claim,
        })

    summary = {}
    for mode in ("controlled", "real_animation"):
        cases = [case for case in comparisons if case["mode"] == mode]
        summary[mode] = {
            "scenario_count": len(cases),
            "committed_work_mismatches": [case["scenario"] for case in cases
                                          if case["committed_work"]["mismatched_counts"]],
            "counts": {key: delta(sum(case["counts"][key]["baseline"] for case in cases),
                                  sum(case["counts"][key]["optimized"] for case in cases))
                       for key in COUNTERS},
        }
    return {
        "comparison": "screenshot_selection_presentation",
        "sources": {"baseline": str(baseline_path.resolve()), "optimized": str(optimized_path.resolve())},
        "matched_configuration": {key: baseline[key] for key in CONFIG_FIELDS},
        "dpr_from_dimensions": {
            "x": baseline["physical_width"] / baseline["logical_width"],
            "y": baseline["physical_height"] / baseline["logical_height"],
            "note": "Dimensions include ceil rounding; exact reported device_pixel_ratio is matched.",
        },
        "limitations": [
            "Work timings measure elapsed preparation and event-processing calls, including native presentation; they are not isolated renderer or CPU-cycle timings.",
            "Animation event-processing work and counters cover the whole input interval; idle waits are excluded and actual timer work can differ.",
            "Geometry changes are observed after each event-pump call; multiple intermediate commits during one call can be missed.",
            "Requested idle waits may overshoot under OS scheduling; inspect elapsed time and observed geometry/paint counts.",
            "Qt event processing and paint counts do not measure compositor completion or input-to-photon latency.",
            "Selector workers and magnifier are excluded by this benchmark.",
        ],
        "summary": summary,
        "scenarios": comparisons,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="Baseline JSON report")
    parser.add_argument("optimized", type=Path, help="Optimized JSON report with identical inputs")
    parser.add_argument("--output", type=Path, help="Save comparison JSON (otherwise writes to stdout)")
    parser.add_argument("--strict-work", action="store_true",
                        help="Exit 2 if a controlled scenario has different geometry or canvas paint counts")
    arguments = parser.parse_args()
    try:
        report = compare_reports(arguments.baseline, arguments.optimized)
        encoded = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if arguments.output:
            arguments.output.write_text(encoded, encoding="utf-8")
        else:
            sys.stdout.write(encoded)
        for case in report["scenarios"]:
            mismatch = case["committed_work"]["mismatched_counts"]
            if mismatch:
                print(f"{case['scenario']} ({case['mode']}): committed work differs: {', '.join(mismatch)}",
                      file=sys.stderr)
        if arguments.strict_work and any(
            case["mode"] == "controlled" and not case["committed_work"]["matched_controlled_work"]
            for case in report["scenarios"]
        ):
            return 2
        return 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"Comparison rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
