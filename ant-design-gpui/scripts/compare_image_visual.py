# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare complete unaligned browser/native Image frames and measured geometry."""
import argparse
import hashlib
import json
import re
from functools import lru_cache
from math import ceil, hypot
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@lru_cache(maxsize=24)
def _distance_offsets(width, height):
    return sorted((hypot(x, y), x, y)
                  for y in range(1 - height, height)
                  for x in range(1 - width, width))


def glyph_raster_metrics(reference, native, reference_background, native_background):
    """Compare original pixel support; neither crop is shifted or resampled.

    This candidate is diagnostic until its policy is independently reviewed.
    Pixel distances use physical device pixels, independent of logical DPI.
    """
    reference_difference = ImageChops.difference(reference, Image.new("RGB", reference.size, reference_background))
    reference_peak = max(high for _, high in reference_difference.getextrema())
    threshold = max(1, min(13, ceil(reference_peak * .2)))

    def ink(frame, background):
        points, channels, total_weight = [], [0., 0., 0.], 0
        for y in range(frame.height):
            for x in range(frame.width):
                pixel = frame.getpixel((x, y))
                contrast = tuple(abs(value - base) for value, base in zip(pixel, background))
                strength = max(contrast)
                total_weight += strength
                if strength >= threshold:
                    points.append((x, y, strength))
                    for channel in range(3):
                        channels[channel] += pixel[channel] - background[channel]
        weight = sum(point[2] for point in points)
        center = (sum(x * value for x, _, value in points) / weight,
                  sum(y * value for _, y, value in points) / weight) if weight else (0., 0.)
        return points, weight, center, max((point[2] for point in points), default=0), channels, total_weight

    expected, expected_weight, expected_center, expected_peak, expected_color, expected_total = ink(reference, reference_background)
    actual, actual_weight, actual_center, actual_peak, actual_color, actual_total = ink(native, native_background)
    if not expected or not actual:
        return {"candidate_acceptable": False, "reason": "missing_ink", "contrast_threshold": threshold}

    offsets = _distance_offsets(reference.width, reference.height)
    def distances(source, target):
        support = {(x, y) for x, y, _ in target}
        values = []
        for x, y, weight in source:
            distance = next(distance for distance, dx, dy in offsets if (x + dx, y + dy) in support)
            values.append((distance, weight))
        values.sort()
        total = sum(weight for _, weight in values)
        cumulative, percentile, p99 = 0, None, 0.
        for distance, weight in values:
            cumulative += weight
            if cumulative >= total * .95 and percentile is None:
                percentile = distance
            if cumulative >= total * .99:
                p99 = distance
                break
        strong_maximum = max((distance for distance, weight in values if weight >= reference_peak * .5), default=0.)
        return sum(distance * weight for distance, weight in values) / total, percentile, values[-1][0], p99, strong_maximum

    forward, backward = distances(expected, actual), distances(actual, expected)
    result = {
        "contrast_threshold": threshold,
        "centroid_distance_physical_px": hypot(expected_center[0] - actual_center[0], expected_center[1] - actual_center[1]),
        # Integrate all 8-bit contrast for coverage. A one-level rounded fringe
        # crossing the support cutoff must not appear as newly created ink.
        "weighted_ink_ratio": actual_total / expected_total,
        "thresholded_ink_ratio": actual_weight / expected_weight,
        "support_pixel_ratio": len(actual) / len(expected),
        "peak_contrast_ratio": actual_peak / expected_peak,
        "normalized_rgb_contrast_delta": max(abs(a / actual_weight - b / expected_weight) for a, b in zip(actual_color, expected_color)),
        "maximum_directional_mean_distance_physical_px": max(forward[0], backward[0]),
        "maximum_directional_p95_distance_physical_px": max(forward[1], backward[1]),
        "maximum_support_distance_physical_px": max(forward[2], backward[2]),
        "maximum_directional_p99_distance_physical_px": max(forward[3], backward[3]),
        # Weak background islands may occupy a percentile tail; strong outliers
        # cannot hide in that percentile tail, even for a large high-DPI glyph.
        "maximum_strong_support_distance_physical_px": max(forward[4], backward[4]),
    }
    checks = {
        "centroid": result["centroid_distance_physical_px"] <= 1.,
        "ink_weight": .75 <= result["weighted_ink_ratio"] <= 1.33,
        "contrast": .75 <= result["peak_contrast_ratio"] <= 1.25,
        "color": result["normalized_rgb_contrast_delta"] <= .12,
        "mean_shape_distance": result["maximum_directional_mean_distance_physical_px"] <= .5,
        "p95_shape_distance": result["maximum_directional_p95_distance_physical_px"] <= 1.5,
        "strong_shape_distance": result["maximum_strong_support_distance_physical_px"] <= 1.5,
    }
    return {**result, "checks": checks, "candidate_acceptable": all(checks.values())}


def canonical_cases():
    inputs = json.loads((Path(__file__).resolve().parent.parent / "fixtures/image/visual-cases.json").read_text(encoding="utf-8"))
    return {f"{scene}-{theme}-{direction}-{round(scale * 100)}":
            (scene, theme, direction, scale, sample["width"], sample["height"])
            for scene, sample in inputs.items()
            for theme in ["default", "dark", "compact", "dark-compact"]
            for direction in ["ltr", "rtl"] for scale in [1, 1.5, 2]}


def required_native_sources():
    root = Path(__file__).resolve().parent.parent
    paths = {"Cargo.toml", "Cargo.lock", "rust-toolchain.toml", ".cargo/config.toml",
             "examples/image_visual.rs", "scripts/capture-image-native.ps1", "scripts/image_build_support.ps1",
             "fixtures/image/visual-cases.json", "src/lib.rs", "src/config.rs", "src/locale.rs", "src/root.rs",
             "resources/image/manifest.json"}
    for directory in ["src/image", "src/modal", "src/icons", "src/behavior", "src/theme"]:
        paths.update(str(path.relative_to(root)).replace("\\", "/")
                     for path in (root / directory).rglob("*.rs") if not re.search(r"(?:^|_)tests\.rs$", path.name))
    paths.update(str(path.relative_to(root)).replace("\\", "/") for path in (root / "resources/image").glob("*.png"))
    return paths


def compare(browser_path, native_path, output, summary=None, geometry_limit=1.0, filters=None, glyph_raster_policy="strict"):
    comparator_hash = digest(Path(__file__))
    if glyph_raster_policy not in ["strict", "shape"]:
        raise ValueError("Unknown glyph rasterization policy")
    browser = json.loads((browser_path / "browser-metrics.json").read_text(encoding="utf-8"))
    native_report = json.loads((native_path / "native-metrics.json").read_text(encoding="utf-8"))
    filters = filters or {}
    if summary and any(value is not None for value in filters.values()):
        raise AssertionError("Published parity summaries require the complete unfiltered matrix")
    for label, records in [("browser", browser["cases"]), ("native", native_report)]:
        names = [case["name"] for case in records]
        if len(names) != len(set(names)):
            raise AssertionError(f"Duplicate {label} capture names")
    if summary:
        contract = canonical_cases()
        supplied = {case["name"] for case in browser["cases"]}
        if supplied != set(contract):
            raise AssertionError({"canonical_matrix_missing": sorted(set(contract) - supplied),
                                  "canonical_matrix_unexpected": sorted(supplied - set(contract))})
        for case in browser["cases"]:
            values = tuple(case.get(key) for key in ["scene", "theme", "direction", "scale", "width", "height"])
            if values != contract[case["name"]]:
                raise AssertionError((case["name"], "canonical_metadata", values, contract[case["name"]]))
    def selected(case):
        return all(value is None or case[key] == value for key, value in filters.items())
    browser["cases"] = [case for case in browser["cases"] if selected(case)]
    native = {case["name"]: case for case in native_report if selected(case)}
    if not browser["cases"]:
        raise AssertionError("No selected browser cases")
    expected_names = {case["name"] for case in browser["cases"]}
    if set(native) != expected_names:
        raise AssertionError({"missing_native": sorted(expected_names - set(native)), "unexpected_native": sorted(set(native) - expected_names)})
    cases, failures, raw_glyph_exceedances = [], [], []
    for expected in browser["cases"]:
        name, scale = expected["name"], expected["scale"]
        actual = native[name]
        for field in ["scene", "theme", "direction"]:
            if actual.get(field) != expected.get(field):
                raise AssertionError((name, "native_metadata", field, actual.get(field), expected.get(field)))
        bpath, npath = browser_path / f"{name}.png", native_path / f"{name}.png"
        before, after = Image.open(bpath).convert("RGB"), Image.open(npath).convert("RGB")
        dimensions = (round(expected["width"] * scale), round(expected["height"] * scale))
        if before.size != after.size or before.size != dimensions:
            raise AssertionError((name, "physical_size", before.size, after.size, dimensions))
        if actual["scale"] != scale:
            raise AssertionError((name, "scale", actual["scale"], scale))
        differences = []

        def geometry(slot, reference, measured):
            if reference is None or measured is None:
                if reference != measured:
                    failures.append({"name": name, "slot": slot, "error": "presence"})
                return
            deltas = {key: measured[key] - reference[key] for key in ["x", "y", "width", "height"]}
            error = max(abs(value) for value in deltas.values())
            differences.append({"slot": slot, "delta": deltas, "maximum_error": error})
            if error > geometry_limit:
                failures.append({"name": name, "slot": slot, "error": error})

        if len(expected["rows"]) != len(actual["rows"]):
            failures.append({"name": name, "error": "thumbnail_count", "browser": len(expected["rows"]), "native": len(actual["rows"])})
        for reference, measured in zip(expected["rows"], actual["rows"]):
            geometry(reference["key"] + ".root", reference["root"], measured["root"])
        if bool(expected["preview"]) != bool(actual["preview"]):
            failures.append({"name": name, "error": "preview_presence"})
        if expected["preview"] and actual["preview"]:
            for slot in ["image", "actions"]:
                geometry("preview." + slot, expected["preview"][slot], actual["preview"][slot])
            for key, value in expected["preview"]["transform"].items():
                measured = actual["preview"]["transform"][key]
                if abs(measured - value) > .001:
                    failures.append({"name": name, "error": "transform." + key, "browser": value, "native": measured})
        diff = ImageChops.difference(before, after)
        channel_means = ImageStat.Stat(diff).mean
        red, green, blue = diff.split()
        frame_histogram = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()
        pixel_count = before.width * before.height
        p99_threshold = pixel_count * .99
        cumulative = 0
        p99 = 0
        for value, count in enumerate(frame_histogram):
            cumulative += count
            if cumulative >= p99_threshold:
                p99 = value
                break
        probes = []
        for label, x, y in [("background", 3, 3), ("bottom-background", 3, expected["height"] - 3)]:
            position = (round(x * scale), round(y * scale))
            b, n = before.getpixel(position), after.getpixel(position)
            delta = max(abs(a - z) for a, z in zip(b, n))
            probes.append({"slot": label, "browser_rgb": b, "native_rgb": n, "max_channel_delta": delta})
            if delta > 1:
                failures.append({"name": name, "error": "solid_color", "slot": label, "delta": delta})
        image_crops = []
        rectangles = [row["root"] for row in expected["rows"]]
        if expected["preview"]:
            rectangles.append(expected["preview"]["image"])
        for index, rectangle in enumerate(rectangles):
            box = (max(0, round(rectangle["x"] * scale)), max(0, round(rectangle["y"] * scale)),
                   min(before.width, round((rectangle["x"] + rectangle["width"]) * scale)),
                   min(before.height, round((rectangle["y"] + rectangle["height"]) * scale)))
            if box[2] > box[0] and box[3] > box[1]:
                crop_error = sum(ImageStat.Stat(diff.crop(box)).mean) / 3
                image_crops.append({"index": index, "box_physical": box,
                    "mean_absolute_channel_error": crop_error})
                # A correctly positioned but missing/incorrectly transformed image
                # must fail too. The crop retains all text and antialiased edges.
                if crop_error > 8:
                    failures.append({"name": name, "error": "image_crop_pixels", "index": index,
                                     "mean_absolute_channel_error": crop_error, "limit": 8})
        glyphs = []
        count_presence = None
        if expected["preview"]:
            preview = expected["preview"]
            controls = [(control["name"], control["bounds"]) for control in preview["controls"]]
            controls.extend((slot, preview[slot]) for slot in ["close", "previous", "next"] if preview.get(slot))
            glyph_size = float(preview["styles"]["operationSize"].removesuffix("px"))
            for label, bounds in controls:
                center_x = bounds["x"] + bounds["width"] / 2
                center_y = bounds["y"] + bounds["height"] / 2
                box = tuple(round(value * scale) for value in (center_x - glyph_size / 2, center_y - glyph_size / 2, center_x + glyph_size / 2, center_y + glyph_size / 2))
                background_point = (round(center_x * scale), round((bounds["y"] + bounds["height"] - 4) * scale))
                counts = []
                for frame in [before, after]:
                    background = frame.getpixel(background_point)
                    crop = frame.crop(box)
                    solid = Image.new("RGB", crop.size, background)
                    red, green, blue = ImageChops.difference(crop, solid).split()
                    histogram = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()
                    counts.append(sum(histogram[13:]))
                glyph_error = sum(ImageStat.Stat(diff.crop(box)).mean) / 3
                raster_metrics = glyph_raster_metrics(before.crop(box), after.crop(box),
                                                      before.getpixel(background_point), after.getpixel(background_point))
                glyphs.append({"control": label, "reference_ink_pixels": counts[0], "native_ink_pixels": counts[1],
                               "mean_absolute_channel_error": glyph_error, "raster_diagnostics": raster_metrics})
                if counts[0] >= 8 and counts[1] < counts[0] * .2:
                    failures.append({"name": name, "error": "missing_control_glyph", "control": label, "reference": counts[0], "native": counts[1]})
                if glyph_error > 12:
                    exceedance = {"name": name, "error": "control_glyph_pixels", "control": label,
                                  "mean_absolute_channel_error": glyph_error, "limit": 12}
                    raw_glyph_exceedances.append(exceedance)
                    if glyph_raster_policy == "strict":
                        failures.append(exceedance)
                if glyph_raster_policy == "shape" and not raster_metrics["candidate_acceptable"]:
                    failures.append({"name": name, "error": "control_glyph_shape", "control": label,
                                     "raster_diagnostics": raster_metrics})
            footer = preview.get("footer")
            image = preview["image"]
            if preview.get("count") and footer and image["y"] + image["height"] <= footer["y"]:
                # In the one-item and token scenes the count lies over the solid
                # mask, so actual painted ink can prove the label is present.
                box = tuple(round(value * scale) for value in (footer["x"], footer["y"],
                    footer["x"] + footer["width"], preview["actions"]["y"]))
                background_point = (round((footer["x"] + 2) * scale), round((footer["y"] + 2) * scale))
                counts = []
                for frame in [before, after]:
                    crop = frame.crop(box)
                    solid = Image.new("RGB", crop.size, frame.getpixel(background_point))
                    red, green, blue = ImageChops.difference(crop, solid).split()
                    histogram = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()
                    counts.append(sum(histogram[13:]))
                count_presence = {"text": preview["count"], "reference_ink_pixels": counts[0], "native_ink_pixels": counts[1]}
                if counts[0] >= 8 and counts[1] < counts[0] * .2:
                    failures.append({"name": name, "error": "missing_count_label", "reference": counts[0], "native": counts[1]})
        cases.append({"name": name, "scale": scale, "scene": expected["scene"],
            "geometry": differences, "mean_absolute_channel_error": sum(channel_means) / 3,
            "p99_max_channel_error": p99,
            "fraction_pixels_with_channel_error_above_32": sum(frame_histogram[33:]) / pixel_count,
            "solid_probes": probes, "image_crops": image_crops, "control_glyph_presence": glyphs, "count_label_presence": count_presence,
            "browser_sha256": digest(bpath), "native_sha256": digest(npath)})
    root = Path(__file__).resolve().parent.parent
    sources = [root / "examples/image_visual.rs", root / "fixtures/image/visual-cases.json", Path(__file__)]
    sources.extend(sorted((root / "src/image").rglob("*.rs")))
    native_provenance_path = native_path / "native-provenance.json"
    native_provenance = json.loads(native_provenance_path.read_text(encoding="utf-8-sig")) if native_provenance_path.exists() else None
    if summary and not native_provenance:
        raise AssertionError("A published summary requires verified native build/capture provenance")
    if native_provenance and not native_provenance["sources_unchanged_during_run"]:
        failures.append({"error": "native_sources_changed_during_capture"})
    if summary and native_provenance:
        if native_provenance["status"] != "passed" or not native_provenance.get("build_verified"):
            raise AssertionError("Native capture does not have verified source/binary provenance")
        if native_provenance["metrics_sha256"] != digest(native_path / "native-metrics.json"):
            raise AssertionError("Native metrics changed after capture")
        missing_sources = required_native_sources() - set(native_provenance.get("source_sha256", {}))
        if missing_sources:
            raise AssertionError({"native_provenance_missing_source_hashes": sorted(missing_sources)})
        executable = Path(native_provenance["executable"])
        if not executable.exists() or digest(executable) != native_provenance["executable_sha256"]:
            raise AssertionError("Native executable no longer matches captured build identity")
        for filename, captured_hash in native_provenance["source_sha256"].items():
            source = root / filename
            if not source.exists() or digest(source) != captured_hash:
                raise AssertionError(f"Current source differs from captured native build: {filename}")
        linked = native_provenance.get("linked_library")
        if not linked:
            raise AssertionError("Native provenance does not identify the actually linked library")
        dependency_info = Path(linked["dependency_info"])
        if not dependency_info.exists() or digest(dependency_info) != linked["dependency_info_sha256"]:
            raise AssertionError("Linked library compiler dependency info changed")
        recorded = re.search(r"^# env-dep:CARGO_MANIFEST_DIR=(.+)$", dependency_info.read_text(encoding="utf-8"), re.MULTILINE)
        if not recorded or Path(recorded.group(1).strip().replace("\\\\", "\\")).resolve() != root:
            raise AssertionError("Actually linked library was not compiled from this source checkout")
    if comparator_hash != digest(Path(__file__)):
        raise AssertionError("Comparator source changed during comparison; repeat against stable tooling")
    report = {"status": "passed" if not failures else "failed", "comparisons": len(cases),
        "glyph_raster_policy": glyph_raster_policy, "comparator_sha256": comparator_hash,
        "raw_control_pixel_status": "failed" if raw_glyph_exceedances else "passed",
        "raw_control_pixel_exceedances": raw_glyph_exceedances,
        "filters": filters,
        "browser": browser["renderer"], "upstream_revision": browser["provenance"]["revision"],
        "image_version": browser["provenance"]["imageVersion"], "all_image_dimensions_match": True,
        "maximum_geometry_error_logical_px": max((item["maximum_error"] for case in cases for item in case["geometry"]), default=0),
        "mean_full_frame_channel_error": sum(case["mean_absolute_channel_error"] for case in cases) / len(cases),
        "maximum_image_crop_channel_error": max((crop["mean_absolute_channel_error"] for case in cases for crop in case["image_crops"]), default=0),
        "maximum_control_glyph_channel_error": max((glyph["mean_absolute_channel_error"] for case in cases for glyph in case["control_glyph_presence"]), default=0),
        "checked_image_crops": sum(len(case["image_crops"]) for case in cases),
        "checked_geometry_rects": sum(len(case["geometry"]) for case in cases),
        "checked_control_glyphs": sum(len(case["control_glyph_presence"]) for case in cases),
        "checked_count_labels": sum(case["count_label_presence"] is not None for case in cases),
        "measurement": "Complete unaligned unmasked RGB frames. No resizing, interpolation, image shifting, font masking, or color correction. Native geometry comes from actual layout-delegate bounds and native transform state. Gates: <=1 logical px measured geometry, <=1 solid channel delta, exact transform state, <=8 mean channel error in each complete image crop. " + ("Strict glyph gate: <=12 mean channel error in each crop. " if glyph_raster_policy == "strict" else "Every glyph must satisfy physical-pixel shape/centroid, signed-color, contrast and coverage gates. Raw12/255 glyph errors remain separately reported, including all exceedances. ") + "Full-frame and p99 errors remain diagnostic.",
        "source_sha256": {str(file.relative_to(root)).replace('\\', '/'): digest(file) for file in sources if file.exists()},
        "native_provenance": native_provenance,
        "failures": failures, "cases": cases}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if summary:
        summary.parent.mkdir(parents=True, exist_ok=True)
        summary.write_text(json.dumps({key: value for key, value in report.items() if key != "cases"}, indent=2) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--scene")
    parser.add_argument("--theme")
    parser.add_argument("--direction")
    parser.add_argument("--scale", type=float)
    parser.add_argument("--glyph-raster-policy", choices=["strict", "shape"], default="strict")
    args = parser.parse_args()
    result = compare(args.browser, args.native, args.output, args.summary,
                     filters={key: getattr(args, key) for key in ["scene", "theme", "direction", "scale"]},
                     glyph_raster_policy=args.glyph_raster_policy)
    print(json.dumps({key: value for key, value in result.items() if key not in ["cases", "source_sha256", "failures", "native_provenance"]}, indent=2))
    if result["failures"]:
        print(json.dumps(result["failures"][:12], indent=2))
        raise SystemExit(1)
