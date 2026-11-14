# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independent upstream Message screenshots with native GPU captures."""

import argparse
import hashlib
import json
import math
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def maximum_geometry(cases):
    return max((abs(error) for case in cases for row in case["geometry"] for error in row.get("root", {}).values()), default=0)


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
        name = case["name"]
        actual = native[name]
        assert abs(actual["scale"] - case["scale"]) < 0.001, f"DPI mismatch: {name}"
        actual_rows = {row["key"]: row for row in actual["rows"]}
        expected_rows = {row["key"]: row for row in case["rows"]}
        assert actual_rows.keys() == expected_rows.keys(), f"Notice identities differ: {name}: {actual_rows.keys()} / {expected_rows.keys()}"
        geometry, crop_errors = [], []
        expected_image = Image.open(args.browser / (name + ".png")).convert("RGB")
        actual_image = Image.open(args.native / (name + ".png")).convert("RGB")
        assert expected_image.size == actual_image.size, f"Physical capture dimensions differ: {name}: {expected_image.size} versus {actual_image.size}"
        diff = ImageChops.difference(expected_image, actual_image)
        means = ImageStat.Stat(diff).mean
        changed = diff.convert("L").point(lambda value: 255 if value > 16 else 0).histogram()[255]
        for key, expected_row in expected_rows.items():
            actual_row = actual_rows[key]
            assert actual_row.get("visible", True) == expected_row["visible"], f"Visibility mismatch: {name}/{key}"
            record = {"key": key, "visible": expected_row["visible"]}
            expected_bounds, actual_bounds = expected_row.get("root"), actual_row.get("root")
            if expected_row["visible"] and expected_bounds and actual_bounds:
                record["root"] = {field: actual_bounds[field] - expected_bounds[field] for field in ("x", "y", "width", "height")}
                scale = case["scale"]
                # Unaligned union crop; do not resize, mask, shift, or exclude text/icons.
                box = (max(0, math.floor(min(expected_bounds["x"], actual_bounds["x"]) * scale) - 16),
                    max(0, math.floor(min(expected_bounds["y"], actual_bounds["y"]) * scale) - 16),
                    min(diff.width, math.ceil(max(expected_bounds["x"] + expected_bounds["width"], actual_bounds["x"] + actual_bounds["width"]) * scale) + 16),
                    min(diff.height, math.ceil(max(expected_bounds["y"] + expected_bounds["height"], actual_bounds["y"] + actual_bounds["height"]) * scale) + 32))
                crop_errors.append(sum(ImageStat.Stat(diff.crop(box)).mean) / 3)
            geometry.append(record)
        comparisons.append({"name": name, "scale": case["scale"], "native_scale": actual["scale"],
            "browser_size": expected_image.size, "native_size": actual_image.size,
            "mean_absolute_channel_error": sum(means) / 3,
            "changed_fraction_above_16": changed / (diff.width * diff.height),
            "geometry": geometry, "unaligned_message_crop_mean_channel_errors": crop_errors,
            "browser_png_sha256": digest(args.browser / (name + ".png")),
            "native_png_sha256": digest(args.native / (name + ".png"))})
    report = {"schema_version": 1, "provenance": browser.get("provenance"), "renderer": browser.get("renderer"),
        "comparisons": len(comparisons), "compared_message_rows": sum(len(case["geometry"]) for case in comparisons),
        "all_image_dimensions_match": True,
        "pixel_measurement": "Complete unaligned, unmasked images at original matching physical DPI; no resizing, interpolation, font masking, or origin alignment.",
        "max_absolute_message_geometry_error_logical_px": maximum_geometry(comparisons),
        "max_absolute_root_geometry_error_by_axis_logical_px": {
            field: max((abs(row["root"][field]) for case in comparisons for row in case["geometry"] if row.get("root")), default=0)
            for field in ("x", "y", "width", "height")},
        "max_full_image_mean_absolute_channel_error": max((case["mean_absolute_channel_error"] for case in comparisons), default=0),
        "max_changed_fraction_above_16": max((case["changed_fraction_above_16"] for case in comparisons), default=0),
        "max_unaligned_message_crop_mean_channel_error": max((error for case in comparisons for error in case["unaligned_message_crop_mean_channel_errors"]), default=0),
        "cases": comparisons}
    if args.native_executable:
        report["native_executable"] = {"name": args.native_executable.name, "sha256": digest(args.native_executable),
            "modified_at_utc": datetime.fromtimestamp(args.native_executable.stat().st_mtime, timezone.utc).isoformat()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.summary:
        crate = Path(__file__).resolve().parent.parent
        source_paths = ["examples/message_visual.rs", "fixtures/message/visual-cases.json", "scripts/compare_message_visual.py", "Cargo.toml", "Cargo.lock", "src/root.rs", "src/behavior/text.rs", "src/config.rs", "src/lib.rs"]
        source_paths += [str(path.relative_to(crate)).replace("\\", "/") for path in (crate / "src/message").glob("*.rs")]
        summary = {key: value for key, value in report.items() if key != "cases"}
        summary["source_sha256"] = {source: digest(crate / source) for source in source_paths if (crate / source).exists()}
        summary["cases"] = [{key: value for key, value in case.items() if key != "geometry"} | {
            "max_absolute_message_geometry_error_logical_px": maximum_geometry([case])} for case in comparisons]
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Compared {len(comparisons)} Message images / {report['compared_message_rows']} notices; maximum geometry error {maximum_geometry(comparisons):.6f} logical px")


if __name__ == "__main__":
    main()
