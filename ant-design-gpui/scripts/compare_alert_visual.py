# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare genuine browser Alert captures with hidden native GPU captures.

Pillow is developer-only. No rendered images or font rasterization enter Cargo tests.
"""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()
    browser = json.loads((args.browser / "browser-metrics.json").read_text(encoding="utf-8"))
    native = {
        case["name"]: case
        for case in json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8"))
    }
    results = []
    max_geometry_error = 0.0
    max_control_size_error = 0.0
    for case in browser["cases"]:
        name = case["name"]
        native_case = native[name]
        assert len(case["rows"]) == len(native_case["rows"])
        geometry = []
        for index, (expected, actual) in enumerate(zip(case["rows"], native_case["rows"])):
            differences = {key: actual[key] - expected[key] for key in ("x", "y", "width", "height")}
            max_geometry_error = max(max_geometry_error, *(abs(value) for value in differences.values()))
            max_control_size_error = max(max_control_size_error, abs(differences["width"]), abs(differences["height"]))
            geometry.append({"index": index, "delta": differences})
        expected = Image.open(args.browser / (name + ".png")).convert("RGB")
        actual = Image.open(args.native / (name + ".png")).convert("RGB")
        size_match = expected.size == actual.size
        width = min(expected.width, actual.width)
        height = min(expected.height, actual.height)
        diff = ImageChops.difference(expected.crop((0, 0, width, height)), actual.crop((0, 0, width, height)))
        means = ImageStat.Stat(diff).mean
        threshold = diff.convert("L").point(lambda value: 255 if value > 16 else 0)
        changed = threshold.histogram()[255] / (width * height)
        # A second diagnostic aligns each measured Alert origin and uses its
        # common unscaled pixel rectangle. No pixels are resized/interpolated.
        # This isolates component painting from cumulative vertical flow drift.
        row_channel_error = row_area = 0.0
        scale = case["scale"]
        for expected_row, actual_row in zip(case["rows"], native_case["rows"]):
            box_width = round(min(expected_row["width"], actual_row["width"]) * scale)
            box_height = round(min(expected_row["height"], actual_row["height"]) * scale)
            if box_width < 1 or box_height < 1:
                continue
            ex, ey = round(expected_row["x"] * scale), round(expected_row["y"] * scale)
            ax, ay = round(actual_row["x"] * scale), round(actual_row["y"] * scale)
            row_diff = ImageChops.difference(
                expected.crop((ex, ey, ex + box_width, ey + box_height)),
                actual.crop((ax, ay, ax + box_width, ay + box_height)),
            )
            area = box_width * box_height
            row_channel_error += sum(ImageStat.Stat(row_diff).mean) / 3 * area
            row_area += area
        results.append({
            "name": name,
            "scale": case["scale"],
            "browser_size": expected.size,
            "native_size": actual.size,
            "size_match": size_match,
            "mean_absolute_channel_error": sum(means) / 3,
            "changed_fraction_above_16": changed,
            "origin_aligned_control_mean_absolute_channel_error": row_channel_error / row_area,
            "geometry": geometry,
        })
    report = {
        "browser_provenance": browser["provenance"],
        "renderer": browser["renderer"],
        "comparisons": len(results),
        "compared_alert_rows": sum(len(case["geometry"]) for case in results),
        "max_absolute_geometry_error_logical_px": max_geometry_error,
        "max_absolute_control_size_error_logical_px": max_control_size_error,
        "all_image_dimensions_match": all(case["size_match"] for case in results),
        "notes": [
            "Browser uses the real upstream React Alert and upstream SVG icon components.",
            "Font shaping and GPU versus browser antialiasing produce pixel differences; inspect them alongside geometry.",
            "Geometry deltas are signed native minus browser in logical pixels, before any image cropping.",
            "Pixel comparison covers the common image area and explicitly records unequal image dimensions.",
            "The separate control crop metric aligns measured Alert origins and crops common rectangles without resampling; full-page errors retain all flow drift.",
        ],
        "cases": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Compared {len(results)} images / {report['compared_alert_rows']} Alert rows; maximum geometry delta {max_geometry_error:.5f}px")
    if not report["all_image_dimensions_match"]:
        raise SystemExit("Invalid capture dimensions: inspect the report and correct DPI/surface sizing before assessing visual parity")
    if args.summary:
        crate = Path(__file__).resolve().parent.parent
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sources = [
            "examples/alert_visual.rs", "fixtures/alert/visual-cases.json",
            "scripts/compare_alert_visual.py", "scripts/capture_alert_reference.ps1",
            "src/alert/mod.rs", "src/alert/view.rs", "src/alert/style.rs",
            "src/alert/state.rs", "src/alert/element.rs", "src/alert/text.rs",
            "src/alert/config.rs", "src/behavior/paragraph.rs", "src/icons/mod.rs",
        ]
        summary = {
            "schema_version": 1,
            "upstream": browser["provenance"]["upstream"],
            "browser": browser["renderer"],
            "source_sha256": {source: digest(crate / source) for source in sources},
            "reference_generator": browser["provenance"]["generator"],
            "comparisons": report["comparisons"],
            "compared_alert_rows": report["compared_alert_rows"],
            "all_image_dimensions_match": True,
            "max_absolute_geometry_error_logical_px": max_geometry_error,
            "max_absolute_control_size_error_logical_px": max_control_size_error,
            "pixel_measurement": "Full unaligned, unmasked canvases; no resizing, interpolation, font masking, or origin alignment is applied to these reported pixel errors.",
            "notes": [
                "Source PNGs, complete child geometry, browser package/source provenance, and diagnostic origin-aligned crop errors are retained in the ignored comparison artifacts.",
                "Reported full-page errors include actual vertical flow rounding, SVG rasterization, and browser versus GPUI font antialiasing differences.",
                "The capture harness verifies the native platform DPI and exact physical surface size for every frame.",
            ],
            "cases": [{
                "name": case["name"], "scale": case["scale"],
                "browser_size": case["browser_size"], "native_size": case["native_size"],
                "native_scale": native[case["name"]]["scale"],
                "mean_absolute_channel_error": case["mean_absolute_channel_error"],
                "changed_fraction_above_16": case["changed_fraction_above_16"],
                "max_absolute_bounds_error_logical_px": max(abs(value) for row in case["geometry"] for value in row["delta"].values()),
                "max_absolute_control_size_error_logical_px": max(abs(row["delta"][key]) for row in case["geometry"] for key in ("width", "height")),
                "browser_png_sha256": digest(args.browser / (case["name"] + ".png")),
                "native_png_sha256": digest(args.native / (case["name"] + ".png")),
            } for case in results],
        }
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
