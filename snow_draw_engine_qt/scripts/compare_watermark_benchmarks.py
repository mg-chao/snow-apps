#!/usr/bin/env python3
"""Compare a captured watermark baseline with a current-format candidate."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


KEY_FIELDS = ("suite", "scenario", "operation")
COMMON_FIELDS = {
    "format_version",
    "suite",
    "scenario",
    "operation",
    "samples",
    "p50_ms",
    "render_calls_per_sample",
}

STRATEGY_FIELDS = {
    "selected_strategy",
    "cache_bytes",
    "fragment_coverage",
    "dense_fills_per_sample",
    "sparse_batches_per_sample",
    "fallback_glyph_draws_per_sample",
    "shape_ms_per_sample",
    "raster_ms_per_sample",
    "tint_ms_per_sample",
    "placement_ms_per_sample",
    "composition_ms_per_sample",
}


def read_results(
    path: Path,
    expected_versions: set[str],
    require_strategy_fields: bool = False,
) -> dict[tuple[str, str, str], dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or ())
        required = COMMON_FIELDS | (STRATEGY_FIELDS if require_strategy_fields else set())
        missing = required - fields
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")
        rows = list(reader)

    if not rows:
        raise ValueError(f"{path}: CSV contains no benchmark rows")
    versions = {row["format_version"] for row in rows}
    if not versions.issubset(expected_versions):
        raise ValueError(
            f"{path}: expected format_version in {sorted(expected_versions)}, "
            f"found {sorted(versions)}"
        )

    results: dict[tuple[str, str, str], dict[str, str]] = {}
    for row in rows:
        key = tuple(row[field] for field in KEY_FIELDS)
        if key in results:
            raise ValueError(f"{path}: duplicate result row: {'/'.join(key)}")
        results[key] = row
    return results


def number(row: dict[str, str], field: str) -> float:
    try:
        return float(row[field])
    except (KeyError, ValueError) as error:
        raise ValueError(
            f"{row.get('scenario', '<unknown>')}: invalid {field}={row.get(field)!r}"
        ) from error


def validate_structure(results: dict[tuple[str, str, str], dict[str, str]]) -> list[str]:
    errors: list[str] = []
    for key, row in sorted(results.items()):
        label = "/".join(key)
        render_calls = number(row, "render_calls_per_sample")
        dense_fills = number(row, "dense_fills_per_sample")
        sparse_batches = number(row, "sparse_batches_per_sample")
        fallback_draws = number(row, "fallback_glyph_draws_per_sample")
        scenario_is_multi_canvas = row["scenario"].startswith("workflow_multi_canvas")
        expected_render_calls = 2.0 if scenario_is_multi_canvas else 1.0
        is_render_sample = row["suite"] == "renderer" or row["operation"] in {
            "widget_paint",
            "preview_paint",
            "render_area_move",
            "camera_move",
            "multi_canvas_paint",
            "export",
        }

        if is_render_sample and render_calls != expected_render_calls:
            per_widget = " per canvas" if scenario_is_multi_canvas else ""
            errors.append(
                f"{label}: expected exactly one decoration render{per_widget}, "
                f"got {render_calls:g}"
            )
        if is_render_sample and dense_fills > expected_render_calls:
            errors.append(
                f"{label}: expected at most one dense fill per canvas, "
                f"got {dense_fills:g}"
            )
        visible = "hidden" not in row["scenario"]
        if is_render_sample and visible and dense_fills + sparse_batches + fallback_draws <= 0:
            errors.append(f"{label}: visible frame did not record a composition strategy")
        if is_render_sample and not visible and row["selected_strategy"] != "none":
            errors.append(
                f"{label}: hidden frame selected {row['selected_strategy']!r}"
            )

    export_rows = {
        row["scenario"]: row
        for row in results.values()
        if row["operation"] == "export"
    }
    for scenario, visible_row in sorted(export_rows.items()):
        prefix = "workflow_export_watermark_"
        if not scenario.startswith(prefix):
            continue
        suffix = scenario[len(prefix) :]
        hidden_name = "workflow_export_hidden_" + suffix
        hidden_row = export_rows.get(hidden_name)
        if hidden_row is None:
            errors.append(f"{scenario}: matching hidden export row is missing")
        elif number(visible_row, "samples") != number(hidden_row, "samples"):
            errors.append(
                f"{scenario}: visible/hidden export sample counts are not matched"
            )

    return errors


def validate_timing_gates(
    baseline: dict[tuple[str, str, str], dict[str, str]],
    candidate: dict[tuple[str, str, str], dict[str, str]],
) -> list[str]:
    errors: list[str] = []

    def require_pair(
        label: str,
        baseline_key: tuple[str, str, str],
        candidate_key: tuple[str, str, str],
        maximum_fraction: float,
    ) -> None:
        if baseline_key not in baseline or candidate_key not in candidate:
            errors.append(f"{label}: required baseline/candidate row is missing")
            return
        before = number(baseline[baseline_key], "p50_ms")
        after = number(candidate[candidate_key], "p50_ms")
        if before <= 0.0 or after > before * maximum_fraction:
            errors.append(
                f"{label}: candidate p50 {after:.4f} ms is above "
                f"the {maximum_fraction:.0%} baseline gate {before * maximum_fraction:.4f} ms"
            )

    require_pair(
        "warm renderer p50",
        ("renderer", "renderer_warm_short_1920x1080", "paint"),
        ("renderer", "renderer_warm_short_1920x1080", "paint"),
        0.40,
    )
    require_pair(
        "full 1080p preview paint p50",
        ("workflow", "workflow_preview_paint_1920x1080", "preview_paint"),
        ("workflow", "workflow_preview_paint_1920x1080", "preview_paint"),
        0.50,
    )

    for suffix, maximum_fraction in (("1920x1080", 0.35), ("3840x2160", 0.40)):
        hidden_key = ("workflow", f"workflow_export_hidden_{suffix}", "export")
        visible_key = ("workflow", f"workflow_export_watermark_{suffix}", "export")
        if hidden_key not in baseline or visible_key not in baseline:
            errors.append(f"export {suffix}: required baseline rows are missing")
            continue
        if hidden_key not in candidate or visible_key not in candidate:
            errors.append(f"export {suffix}: required candidate rows are missing")
            continue
        baseline_delta = number(baseline[visible_key], "p50_ms") - number(
            baseline[hidden_key], "p50_ms"
        )
        candidate_delta = number(candidate[visible_key], "p50_ms") - number(
            candidate[hidden_key], "p50_ms"
        )
        if baseline_delta <= 0.0 or candidate_delta > baseline_delta * maximum_fraction:
            errors.append(
                f"export {suffix} watermark overhead: candidate {candidate_delta:.4f} ms is above "
                f"the {maximum_fraction:.0%} baseline gate {baseline_delta * maximum_fraction:.4f} ms"
            )

    fallback_key = ("renderer", "renderer_segmented_maximum_text_1920x1080", "paint")
    if fallback_key not in candidate or number(candidate[fallback_key], "p50_ms") >= 5.0:
        value = number(candidate[fallback_key], "p50_ms") if fallback_key in candidate else float("nan")
        errors.append(f"maximum-text segmented p50: expected below 5 ms, got {value:.4f} ms")
    return errors


def print_comparison(
    baseline: dict[tuple[str, str, str], dict[str, str]],
    candidate: dict[tuple[str, str, str], dict[str, str]],
) -> None:
    shared = sorted(set(baseline) & set(candidate))
    missing_baseline = sorted(set(candidate) - set(baseline))
    missing_candidate = sorted(set(baseline) - set(candidate))

    if missing_baseline:
        print("Candidate-only rows:")
        for key in missing_baseline:
            print(f"  {'/'.join(key)}")
    if missing_candidate:
        print("Baseline-only rows:")
        for key in missing_candidate:
            print(f"  {'/'.join(key)}")

    print("p50 comparison (candidate - baseline):")
    for key in shared:
        before = number(baseline[key], "p50_ms")
        after = number(candidate[key], "p50_ms")
        delta = after - before
        percent = delta / before * 100.0 if before else 0.0
        print(f"  {'/'.join(key)}: {before:.4f} -> {after:.4f} ms ({delta:+.4f}, {percent:+.2f}%)")

    print("Visible-minus-hidden export incremental p50:")
    hidden_prefix = "workflow_export_hidden_"
    visible_prefix = "workflow_export_watermark_"
    suffixes = sorted(
        {
            key[1][len(hidden_prefix) :]
            for key in shared
            if key[1].startswith(hidden_prefix)
        }
        & {
            key[1][len(visible_prefix) :]
            for key in shared
            if key[1].startswith(visible_prefix)
        }
    )
    for suffix in suffixes:
        hidden_name = hidden_prefix + suffix
        visible_name = visible_prefix + suffix
        hidden_key = next(key for key in shared if key[1] == hidden_name)
        visible_key = next(key for key in shared if key[1] == visible_name)
        baseline_delta = number(baseline[visible_key], "p50_ms") - number(
            baseline[hidden_key], "p50_ms"
        )
        candidate_delta = number(candidate[visible_key], "p50_ms") - number(
            candidate[hidden_key], "p50_ms"
        )
        print(
            f"  {suffix}: {baseline_delta:.4f} -> {candidate_delta:.4f} ms "
            f"({candidate_delta - baseline_delta:+.4f})"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "baseline", type=Path, help="captured watermark baseline CSV"
    )
    parser.add_argument("candidate", type=Path, help="candidate watermark CSV")
    parser.add_argument(
        "--allow-structural-errors",
        action="store_true",
        help="print structural errors without failing",
    )
    parser.add_argument(
        "--enforce-timing-gates",
        action="store_true",
        help="enforce the recorded refactor timing thresholds",
    )
    args = parser.parse_args()

    try:
        baseline = read_results(args.baseline, {"1"})
        candidate = read_results(args.candidate, {"1"}, require_strategy_fields=True)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    structural_errors = validate_structure(candidate)
    if structural_errors:
        print("Structural diagnostics:", file=sys.stderr)
        for error in structural_errors:
            print(f"  {error}", file=sys.stderr)
        if not args.allow_structural_errors:
            return 1

    if args.enforce_timing_gates:
        timing_errors = validate_timing_gates(baseline, candidate)
        if timing_errors:
            print("Timing gates:", file=sys.stderr)
            for error in timing_errors:
                print(f"  {error}", file=sys.stderr)
            return 1

    print_comparison(baseline, candidate)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
