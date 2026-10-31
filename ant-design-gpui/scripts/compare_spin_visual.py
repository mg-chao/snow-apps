# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare actual upstream Spin screenshots against matching native GPU captures."""

import argparse
import hashlib
import json
import math
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(cases):
    return {"comparisons": len(cases), "spin_rows": sum(len(case["geometry"]) for case in cases),
        "max_absolute_spin_geometry_error_logical_px": max((abs(value) for case in cases for row in case["geometry"] for value in row.get("spin", {}).values()), default=0),
        "max_full_image_mean_absolute_channel_error": max((case["mean_absolute_channel_error"] for case in cases), default=0),
        "max_changed_fraction_above_16": max((case["changed_fraction_above_16"] for case in cases), default=0),
        "max_unaligned_spin_crop_mean_channel_error": max((error for case in cases for error in case["unaligned_spin_crop_mean_channel_errors"]), default=0)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--native-executable", type=Path)
    args = parser.parse_args()
    browser = json.loads((args.browser / "browser-metrics.json").read_text(encoding="utf-8"))
    native = {case["name"]: case for case in json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8"))}
    comparisons = []
    for case in browser["cases"]:
        if "phase" in case:
            continue
        name = case["name"]
        actual = native[name]
        assert abs(actual["scale"] - case["scale"]) < 0.001, f"DPI mismatch: {name}"
        assert len(actual["rows"]) == len(case["rows"]), f"Row count mismatch: {name}"
        geometry = []
        for index, (expected_row, actual_row) in enumerate(zip(case["rows"], actual["rows"])):
            record = {"index": index}
            for part in ("row", "spin"):
                expected_bounds = expected_row if part == "row" else expected_row.get(part)
                actual_bounds = actual_row if part == "row" else actual_row.get(part)
                if expected_bounds is not None and actual_bounds is not None:
                    record[part] = {key: actual_bounds[key] - expected_bounds[key] for key in ("x", "y", "width", "height")}
            geometry.append(record)
        expected_image = Image.open(args.browser / (name + ".png")).convert("RGB")
        actual_image = Image.open(args.native / (name + ".png")).convert("RGB")
        assert expected_image.size == actual_image.size, f"Physical capture dimensions differ: {name}: {expected_image.size} versus {actual_image.size}"
        diff = ImageChops.difference(expected_image, actual_image)
        means = ImageStat.Stat(diff).mean
        changed = diff.convert("L").point(lambda value: 255 if value > 16 else 0).histogram()[255]
        spin_errors = []
        for expected_row, actual_row in zip(case["rows"], actual["rows"]):
            expected_bounds, actual_bounds = expected_row.get("spin"), actual_row.get("spin")
            if not expected_bounds or not actual_bounds or not expected_bounds["width"] or not expected_bounds["height"]:
                continue
            scale = case["scale"]
            # An unaligned union crop preserves every placement difference.
            box = (max(0, math.floor(min(expected_bounds["x"], actual_bounds["x"]) * scale) - 2),
                max(0, math.floor(min(expected_bounds["y"], actual_bounds["y"]) * scale) - 2),
                min(diff.width, math.ceil(max(expected_bounds["x"] + expected_bounds["width"], actual_bounds["x"] + actual_bounds["width"]) * scale) + 2),
                min(diff.height, math.ceil(max(expected_bounds["y"] + expected_bounds["height"], actual_bounds["y"] + actual_bounds["height"]) * scale) + 2))
            crop = diff.crop(box)
            spin_errors.append(sum(ImageStat.Stat(crop).mean) / 3)
        comparisons.append({"name": name, "scale": case["scale"], "native_scale": actual["scale"],
            "browser_size": expected_image.size, "native_size": actual_image.size,
            "mean_absolute_channel_error": sum(means) / 3,
            "changed_fraction_above_16": changed / (diff.width * diff.height), "geometry": geometry,
            "unaligned_spin_crop_mean_channel_errors": spin_errors,
            "browser_png_sha256": digest(args.browser / (name + ".png")),
            "native_png_sha256": digest(args.native / (name + ".png"))})
    report = {"schema_version": 1, "provenance": browser.get("provenance"),
        "renderer": browser.get("renderer"), "comparisons": len(comparisons),
        "compared_spin_rows": sum(len(case["geometry"]) for case in comparisons),
        "all_image_dimensions_match": True,
        "browser_animation_phase_diagnostics": sum("phase" in case for case in browser["cases"]),
        "pixel_measurement": "Complete unaligned, unmasked images at matching native DPI; no resizing, interpolation, font masking, or origin alignment.",
        "max_absolute_row_geometry_error_logical_px": max(abs(value) for case in comparisons for row in case["geometry"] for value in row["row"].values()),
        "max_absolute_spin_geometry_error_logical_px": max(abs(value) for case in comparisons for row in case["geometry"] for value in row.get("spin", {}).values()),
        "max_unaligned_spin_crop_mean_channel_error": max(error for case in comparisons for error in case["unaligned_spin_crop_mean_channel_errors"]),
        "case_groups": {
            "reference_surfaces": summarize([case for case in comparisons if not case["name"].startswith("backdrops-")]),
            "contrasting_parent_backdrops": summarize([case for case in comparisons if case["name"].startswith("backdrops-")])},
        "known_difference": "On contrasting or patterned parent surfaces, native nested loading uses a container-colored wash instead of CSS isolated group opacity. The backdrops scene records the resulting color difference without masking it.",
        "cases": comparisons}
    if args.native_executable:
        report["native_executable"] = {"name": args.native_executable.name,
            "sha256": digest(args.native_executable),
            "modified_at_utc": datetime.fromtimestamp(args.native_executable.stat().st_mtime, timezone.utc).isoformat()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.summary:
        crate = Path(__file__).resolve().parent.parent
        source_paths = ["examples/spin_visual.rs", "fixtures/spin/visual-cases.json", "scripts/compare_spin_visual.py",
            "Cargo.toml", "Cargo.lock", "src/behavior/text.rs", "src/root.rs"]
        source_paths += [str(path.relative_to(crate)).replace("\\", "/") for path in (crate / "src/spin").glob("*.rs")]
        summary = {key: value for key, value in report.items() if key != "cases"}
        summary["source_sha256"] = {source: digest(crate / source) for source in source_paths}
        summary["source_modified_at_utc"] = {source: datetime.fromtimestamp((crate / source).stat().st_mtime, timezone.utc).isoformat() for source in source_paths}
        summary["cases"] = [{key: value for key, value in case.items() if key != "geometry"} | {
            "max_absolute_spin_geometry_error_logical_px": max(abs(value) for row in case["geometry"] for value in row.get("spin", {}).values())
        } for case in comparisons]
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Compared {report['comparisons']} Spin images / {report['compared_spin_rows']} rows; maximum Spin geometry error {report['max_absolute_spin_geometry_error_logical_px']:.6f} logical px")


if __name__ == "__main__":
    main()
