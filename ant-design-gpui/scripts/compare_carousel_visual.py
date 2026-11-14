# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare original Carousel/browser and GPUI frames without registration or masks."""
import argparse
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def geometry(expected, actual, label):
    if expected is None or actual is None:
        if expected != actual:
            raise ValueError(f"{label}: missing geometry")
        return 0.0
    return max(abs(expected[axis] - actual[axis]) for axis in ("x", "y", "width", "height"))


def compare(browser, native, expected_image, actual_image, geometry_tolerance=1.0):
    name = browser["name"]
    if browser["scale"] != native["scale"]:
        raise ValueError(f"{name}: native/browser DPI differs")
    if expected_image.size != actual_image.size:
        raise ValueError(f"{name}: physical image dimensions differ")
    if len(browser["rows"]) != len(native["rows"]):
        raise ValueError(f"{name}: row count differs")
    diff = ImageChops.difference(expected_image.convert("RGB"), actual_image.convert("RGB"))
    scale = browser["scale"]
    rows = []
    for index, (expected, actual) in enumerate(zip(browser["rows"], native["rows"])):
        options = expected.get("options", {})
        count = options.get("count", 4)
        vertical = options.get("vertical", options.get("placement") in ("start", "end"))
        rtl = browser.get("direction") == "rtl" and not vertical
        control_error = 0.0
        paired_controls = {}
        transparent_reference_arrows = sum(arrow.get("opacity", 1) == 0 for arrow in expected["arrows"])
        for part in ("dots", "arrows"):
            # Slick leaves finite-boundary arrows in the DOM at opacity zero;
            # the native component omits their paint/hitbox/accessibility nodes.
            # Preserve the raw count below, and compare every visible control.
            expected_controls = [arrow for arrow in expected[part] if arrow.get("opacity", 1) > 0] if part == "arrows" else [
                {**dot, "logicalIndex": count - position - 1 if rtl else position}
                for position, dot in enumerate(expected[part])]
            if len(expected_controls) != len(actual[part]):
                raise ValueError(f"{name}/{index}: {part} count differs")
            # The browser enumerates Slick's internal indices; native controls
            # retain logical source indices. Compare physical paint order while
            # checking dot index/active associations independently.
            expected_controls = sorted(expected_controls, key=lambda item: (item["x"], item["y"]))
            actual_controls = sorted(actual[part], key=lambda item: (item["x"], item["y"]))
            paired_controls[part] = list(zip(expected_controls, actual_controls))
            for slot, (a, b) in enumerate(paired_controls[part]):
                control_error = max(control_error, geometry(a, b, f"{name}/{index}/{part}/{slot}"))
                if part == "dots" and "active" in a:
                    if b.get("index") != a["logicalIndex"] or b.get("active") != a["active"]:
                        raise ValueError(f"{name}/{index}: dot logical index/active association differs")
        root_error = geometry(expected["carousel"], actual["carousel"], f"{name}/{index}/root")
        viewport_error = geometry(expected["viewport"], actual["viewport"], f"{name}/{index}/viewport")
        a, b = expected["carousel"], actual["carousel"]
        box = (max(0, math.floor(min(a["x"], b["x"]) * scale)),
               max(0, math.floor(min(a["y"], b["y"]) * scale)),
               min(diff.width, math.ceil(max(a["x"] + a["width"], b["x"] + b["width"]) * scale)),
               min(diff.height, math.ceil(max(a["y"] + a["height"], b["y"] + b["height"]) * scale)))
        crop_error = sum(ImageStat.Stat(diff.crop(box)).mean) / 3 if box[2] > box[0] and box[3] > box[1] else 0.0
        selected = [slide["index"] for slide in expected["slides"] if slide["active"]]
        actual_selected = actual.get("currentIndex")
        # Empty Slick still generates internal placeholder markup; native empty
        # content intentionally has no selected slide.
        if count and selected and actual_selected != selected[0]:
            raise ValueError(f"{name}/{index}: selected slide differs ({selected} vs {actual_selected})")
        maximum_error = max(root_error, viewport_error, control_error)
        checked_error = maximum_error
        known_difference = None
        exception_proof = None
        if rtl and options.get("effect") == "fade":
            viewport = expected["viewport"]
            painted_slides = [slide for slide in expected["slides"] if slide.get("opacity", 1) > 0]
            intersects = lambda slide: (min(slide["x"] + slide["width"], viewport["x"] + viewport["width"]) > max(slide["x"], viewport["x"])
                                        and min(slide["y"] + slide["height"], viewport["y"] + viewport["height"]) > max(slide["y"], viewport["y"]))
            if painted_slides and all(not intersects(slide) for slide in painted_slides):
                known_difference = "upstream_rtl_fade_blank_content"
                exception_proof = {"all_positive_opacity_source_slides_outside_viewport": True,
                                   "positive_opacity_source_indices": [slide["index"] for slide in painted_slides]}
        # Empty Slick's internal list collapses although its explicitly sized
        # outer container stays 160px. Require matching outer bounds and exactly
        # matching raw pixels before classifying this structural difference.
        if (count == 0 and actual_selected is None and root_error <= geometry_tolerance
                and not actual["arrows"] and not actual["dots"] and crop_error == 0
                and expected["viewport"]["height"] == 0
                and actual["viewport"]["height"] == actual["carousel"]["height"]
                and all(abs(expected["viewport"][axis] - actual["viewport"][axis]) <= geometry_tolerance for axis in ("x", "y", "width"))):
            known_difference = "empty_internal_viewport_extent"
            checked_error = max(root_error, control_error)
            exception_proof = {"count": 0, "root_bounds_match": True, "raw_crop_pixels_identical": True,
                               "browser_internal_height": expected["viewport"]["height"],
                               "native_internal_height": actual["viewport"]["height"]}
        # Slick's adaptive-height lookup uses its internal RTL index against
        # source-order DOM attributes. Prove both the wrong-source height and
        # the actually visible source height from independent DOM measurements.
        if rtl and options.get("adaptive") and options.get("placement", "bottom") == "bottom" and len(selected) == 1:
            by_index = {slide["index"]: slide for slide in expected["slides"]}
            visible_index = selected[0]
            wrong_index = count - visible_index - 1
            visible_height = by_index.get(visible_index, {}).get("height")
            wrong_height = by_index.get(wrong_index, {}).get("height")
            if (visible_height is not None and wrong_height is not None and abs(visible_height - wrong_height) > geometry_tolerance
                    and abs(expected["viewport"]["height"] - wrong_height) <= geometry_tolerance
                    and abs(actual["viewport"]["height"] - visible_height) <= geometry_tolerance
                    and abs(expected["carousel"]["height"] - wrong_height) <= geometry_tolerance
                    and abs(actual["carousel"]["height"] - visible_height) <= geometry_tolerance):
                residuals = [abs(expected[part][axis] - actual[part][axis])
                             for part in ("carousel", "viewport") for axis in ("x", "y", "width")]
                height_delta = wrong_height - visible_height
                for part, pairs in paired_controls.items():
                    for a, b in pairs:
                        residuals.extend(abs(a[axis] - b[axis]) for axis in ("x", "width", "height"))
                        predicted_delta = height_delta / 2 if part == "arrows" else height_delta
                        residuals.append(abs((a["y"] - b["y"]) - predicted_delta))
                checked_error = max(residuals, default=0)
                known_difference = "upstream_rtl_adaptive_wrong_source_height"
                exception_proof = {"visible_source_index": visible_index, "visible_dom_height": visible_height,
                                   "wrong_source_index": wrong_index, "wrong_dom_height": wrong_height,
                                   "browser_height_matches_wrong_source": True, "native_height_matches_visible_source": True,
                                   "raw_geometry_errors_retained": True}
        if checked_error > geometry_tolerance:
            raise ValueError(f"{name}/{index}: geometry exceeds {geometry_tolerance} logical px ({checked_error}; raw={maximum_error})")
        if not known_difference and options.get("layered") and options.get("effect") == "fade":
            known_difference = "native_primitive_opacity_layered_fade"
        rows.append({"index": index, "root_geometry_error": root_error,
                     "viewport_geometry_error": viewport_error, "control_geometry_error": control_error,
                     "crop_mean_channel_error": crop_error, "known_difference": known_difference,
                     "unpainted_reference_arrows": transparent_reference_arrows,
                     "normal_geometry_error": checked_error, "exception_proof": exception_proof})
    return {"name": name, "scale": scale, "rows": rows,
            "mean_channel_error": sum(ImageStat.Stat(diff).mean) / 3}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--native-executable", type=Path)
    parser.add_argument("--native-source-sha256", type=Path)
    parser.add_argument("--allow-subset", action="store_true")
    parser.add_argument("--geometry-tolerance", type=float, default=1.0)
    args = parser.parse_args()
    if not 0 <= args.geometry_tolerance <= 1:
        raise ValueError("Geometry tolerance may be tightened, but cannot exceed one logical pixel")
    browser = json.loads((args.browser / "browser-metrics.json").read_text(encoding="utf-8"))
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8"))
    expected = {case["name"]: case for case in browser["cases"]}
    actual = {case["name"]: case for case in native}
    if len(expected) != len(browser["cases"]) or len(actual) != len(native):
        raise ValueError("Duplicate capture names")
    if not args.allow_subset and expected.keys() != actual.keys():
        raise ValueError(f"Case sets differ: browser-only={sorted(expected.keys()-actual.keys())}, native-only={sorted(actual.keys()-expected.keys())}")
    names = sorted(expected.keys() & actual.keys())
    if not names:
        raise ValueError("No common capture cases")
    cases = []
    for name in names:
        browser_path = args.browser / (name + ".png")
        native_path = args.native / (name + ".png")
        with Image.open(browser_path) as original, Image.open(native_path) as rendered:
            record = compare(expected[name], actual[name], original, rendered, args.geometry_tolerance)
        record.update(browser_png_sha256=digest(browser_path), native_png_sha256=digest(native_path))
        cases.append(record)
    rows = [row for case in cases for row in case["rows"]]
    report = {"schema_version": 1, "image_pairs": len(cases), "rows": len(rows),
              "comparison": "Complete unaligned, unmasked physical images and row crops; no resizing or registration.",
              "control_matching": "Physical paint order, with independent logical source index and active-dot assertions; raw capture enumeration is retained in the metrics files.",
              "provenance": browser["provenance"], "renderer": browser["renderer"],
              "browser_metrics_sha256": digest(args.browser / "browser-metrics.json"),
              "native_metrics_sha256": digest(args.native / "native-metrics.json"),
              "all_case_sets_match": expected.keys() == actual.keys(),
              "geometry_tolerance_logical_px": args.geometry_tolerance,
              "normal_geometry_within_tolerance": True,
              "all_raw_geometry_within_tolerance": max(max(r["root_geometry_error"], r["viewport_geometry_error"], r["control_geometry_error"]) for r in rows) <= args.geometry_tolerance,
              "maximum_normal_geometry_error": max(r["normal_geometry_error"] for r in rows),
              "maximum_root_geometry_error": max(r["root_geometry_error"] for r in rows),
              "maximum_viewport_geometry_error": max(r["viewport_geometry_error"] for r in rows),
              "maximum_control_geometry_error": max(r["control_geometry_error"] for r in rows),
              "maximum_crop_mean_channel_error": max(r["crop_mean_channel_error"] for r in rows),
              "maximum_full_image_mean_channel_error": max(c["mean_channel_error"] for c in cases),
              "comparison_tool_sha256": digest(Path(__file__)), "cases": cases}
    for label in ("reference_appearance", "upstream_rtl_fade_blank_content", "upstream_rtl_adaptive_wrong_source_height",
                  "empty_internal_viewport_extent", "native_primitive_opacity_layered_fade"):
        members = [r for r in rows if (r["known_difference"] or "reference_appearance") == label]
        report[label] = {"rows": len(members), "maximum_crop_mean_channel_error": max((r["crop_mean_channel_error"] for r in members), default=0),
                         "maximum_raw_geometry_error": max((max(r["root_geometry_error"], r["viewport_geometry_error"], r["control_geometry_error"]) for r in members), default=0)}
    if args.native_executable:
        report["native_executable"] = {"name": args.native_executable.name, "sha256": digest(args.native_executable)}
    if args.native_source_sha256:
        report["native_build_manifest"] = json.loads(args.native_source_sha256.read_text(encoding="utf-8-sig"))
        manifest = report["native_build_manifest"]
        if args.native_executable and manifest["native_executable"]["sha256"] != report["native_executable"]["sha256"]:
            raise ValueError("Native capture executable does not match the verified build manifest")
        native_scene = manifest["source_sha256"]["fixtures/carousel/visual-cases.json"]
        browser_scene = next(value for key, value in browser["provenance"]["inputs"].items()
                             if key.replace("\\", "/").endswith("fixtures/carousel/visual-cases.json"))
        if native_scene != browser_scene:
            raise ValueError("Native and original browser captures used different shared scene bytes")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("cases", "provenance", "native_build_manifest")}, indent=2))


if __name__ == "__main__":
    main()
