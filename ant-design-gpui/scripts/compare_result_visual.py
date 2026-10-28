# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare genuine browser Result captures with hidden native GPU captures.

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
    presence_errors = []
    for case in browser["cases"]:
        name = case["name"]
        native_case = native[name]
        assert len(case["rows"]) == len(native_case["rows"])
        geometry = []
        for index, (expected, actual) in enumerate(zip(case["rows"], native_case["rows"])):
            differences = {key: actual[key] - expected[key] for key in ("x", "y", "width", "height")}
            max_geometry_error = max(max_geometry_error, *(abs(value) for value in differences.values()))
            max_control_size_error = max(max_control_size_error, abs(differences["width"]), abs(differences["height"]))
            parts = {}
            unpainted_zero_area_slots = []
            for part in ("icon", "title", "subTitle", "extra", "body", "image"):
                wanted, found = expected.get(part), actual.get(part)
                if wanted is not None and wanted["height"] == 0 and found is None:
                    # Scene::insert_primitive discards empty-area quads. Root
                    # geometry remains measured; semantic presence is covered
                    # by the component's deterministic offscreen context test.
                    unpainted_zero_area_slots.append(part)
                    continue
                if wanted is None or found is None:
                    if not (wanted is None and found is None):
                        presence_errors.append({"name":name,"row":index,"part":part,"browser":wanted,"native":found})
                else:
                    delta = {key: found[key] - wanted[key] for key in ("x", "y", "width", "height")}
                    max_geometry_error = max(max_geometry_error, *(abs(value) for value in delta.values()))
                    max_control_size_error = max(max_control_size_error, abs(delta["width"]), abs(delta["height"]))
                    parts[part] = delta
            if len(expected["actions"]) != len(actual["actions"]):
                presence_errors.append({"name":name,"row":index,"part":"actions","browser":len(expected["actions"]),"native":len(actual["actions"])})
            for action, (wanted, found) in enumerate(zip(expected["actions"], actual["actions"])):
                delta = {key: found[key] - wanted[key] for key in ("x", "y", "width", "height")}
                max_geometry_error = max(max_geometry_error, *(abs(value) for value in delta.values()))
                max_control_size_error = max(max_control_size_error, abs(delta["width"]), abs(delta["height"]))
                parts[f"action-{action}"] = delta
            geometry.append({"index": index, "delta": differences, "parts": parts,
                             "unpainted_zero_area_slots": unpainted_zero_area_slots})
        expected = Image.open(args.browser / (name + ".png")).convert("RGB")
        actual = Image.open(args.native / (name + ".png")).convert("RGB")
        size_match = expected.size == actual.size
        width = min(expected.width, actual.width)
        height = min(expected.height, actual.height)
        diff = ImageChops.difference(expected.crop((0, 0, width, height)), actual.crop((0, 0, width, height)))
        means = ImageStat.Stat(diff).mean
        threshold = diff.convert("L").point(lambda value: 255 if value > 16 else 0)
        changed = threshold.histogram()[255] / (width * height)
        # A second diagnostic aligns each measured Result origin and uses its
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
        "compared_result_rows": sum(len(case["geometry"]) for case in results),
        "max_absolute_geometry_error_logical_px": max_geometry_error,
        "max_absolute_control_size_error_logical_px": max_control_size_error,
        "all_image_dimensions_match": all(case["size_match"] for case in results),
        "geometry_threshold_logical_px": 1.0,
        "geometry_gate_passed": max_geometry_error <= 1.0,
        "unpainted_zero_area_slots": sum(len(row["unpainted_zero_area_slots"]) for case in results for row in case["geometry"]),
        "presence_gate_passed": not presence_errors,
        "presence_errors": presence_errors,
        "action_probes": {"widths_logical_px": [72, 68], "native_measurement": "Original child prepaint bounds through a layout-neutral Element delegate", "unconstrained_host_metrics_record": "fixtures/result/host-action-metrics-windows.json"},
        "known_discrepancies_record": "fixtures/result/known-discrepancies-windows.json",
        "notes": [
            "Browser uses the real upstream React Result and upstream SVG icon components.",
            "Font shaping and GPU versus browser antialiasing produce pixel differences; inspect them alongside geometry.",
            "Geometry deltas are signed native minus browser in logical pixels, before any image cropping.",
            "Pixel comparison covers the common image area and explicitly records unequal image dimensions.",
            "The separate control crop metric aligns measured Result origins and crops common rectangles without resampling; full-page errors retain all flow drift.",
            "GPUI drops empty-area paint primitives. Zero-height browser slots without native quads are recorded separately; no rectangle is fabricated. Component context tests verify their semantic presence.",
        ],
        "cases": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Compared {len(results)} images / {report['compared_result_rows']} Result rows; maximum geometry delta {max_geometry_error:.5f}px")
    if args.summary:
        crate = Path(__file__).resolve().parent.parent
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sources = [
            "examples/result_visual.rs", "fixtures/result/visual-cases.json",
            "scripts/compare_result_visual.py", "scripts/capture_result_reference.mjs",
            "src/result/mod.rs", "src/result/view.rs", "src/result/style.rs",
            "src/result/config.rs", "src/behavior/paragraph.rs", "src/behavior/text.rs",
            "src/config.rs", "src/root.rs", "src/icons/mod.rs",
            "src/result/resources/403.svg", "src/result/resources/404.svg",
            "src/result/resources/500.svg",
            "fixtures/result/known-discrepancies-windows.json",
            "fixtures/result/host-action-metrics-windows.json",
        ]
        summary = {
            "schema_version": 1,
            "upstream": browser["provenance"]["upstream"],
            "browser": browser["renderer"],
            "source_sha256": {source: digest(crate / source) for source in sources},
            "reference_generator": browser["provenance"]["generator"],
            "comparisons": report["comparisons"],
            "compared_result_rows": report["compared_result_rows"],
            "all_image_dimensions_match": report["all_image_dimensions_match"],
            "geometry_threshold_logical_px": 1.0,
            "geometry_gate_passed": report["geometry_gate_passed"],
            "unpainted_zero_area_slots": report["unpainted_zero_area_slots"],
            "presence_gate_passed": report["presence_gate_passed"],
            "presence_errors": presence_errors,
            "action_probes": report["action_probes"],
            "known_discrepancies_record": report["known_discrepancies_record"],
            "max_absolute_geometry_error_logical_px": max_geometry_error,
            "max_absolute_control_size_error_logical_px": max_control_size_error,
            "pixel_measurement": "Full unaligned, unmasked canvases; no resizing, interpolation, font masking, or origin alignment is applied to these reported pixel errors.",
            "notes": [
                "Source PNGs, complete child geometry, browser package/source provenance, and diagnostic origin-aligned crop errors are retained in the ignored comparison artifacts.",
                "Reported full-page errors include actual vertical flow rounding, SVG rasterization, and browser versus GPUI font antialiasing differences.",
                "The capture harness verifies the native platform DPI and exact physical surface size for every frame.",
                "Authentic screenshots are saved before a separate marker-only render pass measures native semantic slot backgrounds; marker pixels never enter image comparisons.",
            ],
            "cases": [{
                "name": case["name"], "scale": case["scale"],
                "browser_size": case["browser_size"], "native_size": case["native_size"],
                "native_scale": native[case["name"]]["scale"],
                "mean_absolute_channel_error": case["mean_absolute_channel_error"],
                "changed_fraction_above_16": case["changed_fraction_above_16"],
                "max_absolute_bounds_error_logical_px": max(abs(value) for row in case["geometry"] for delta in [row["delta"], *row["parts"].values()] for value in delta.values()),
                "max_absolute_control_size_error_logical_px": max(abs(delta[key]) for row in case["geometry"] for delta in [row["delta"], *row["parts"].values()] for key in ("width", "height")),
                "browser_png_sha256": digest(args.browser / (case["name"] + ".png")),
                "native_png_sha256": digest(args.native / (case["name"] + ".png")),
            } for case in results],
        }
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8", newline="\n")
    if not report["all_image_dimensions_match"]:
        raise SystemExit("Invalid capture dimensions: inspect the report and correct DPI/surface sizing before assessing visual parity")
    if presence_errors:
        raise SystemExit(f"Result semantic slot presence differs in {len(presence_errors)} places; inspect the report")
    if not report["geometry_gate_passed"]:
        raise SystemExit(f"Result geometry exceeds the 1 logical pixel limit: {max_geometry_error:.5f}px; inspect signed slot deltas in the report")


if __name__ == "__main__":
    main()
