# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independently captured ColorPicker geometry and surface pixels.

Flat swatches allow one RGB byte; shader-painted gradients allow four RGB bytes
for the pinned renderer's dithering. Text, shadows and full-image error are
reported separately and never used to conceal a failed surface sample.
"""
import argparse
import colorsys
import hashlib
import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image


RECT_KEYS = ("x", "y", "width", "height")


def rectangle_checks(label, expected, actual):
    if expected is None or actual is None:
        return [{"target": label, "passed": expected is None and actual is None,
                 "reason": "presence", "reference": expected, "native": actual}]
    if any(key not in actual or not math.isfinite(actual[key]) for key in RECT_KEYS):
        raise ValueError(f"{label}: invalid native rectangle")
    delta = {key: actual[key] - expected[key] for key in RECT_KEYS}
    return [{"target": label, "passed": max(map(abs, delta.values())) <= 1.0001,
             "geometry_delta": delta}]


def inside(point, rect, padding=0):
    return (rect["x"] - padding <= point[0] <= rect["x"] + rect["width"] + padding
            and rect["y"] - padding <= point[1] <= rect["y"] + rect["height"] + padding)


def pixel(image, rect, x_fraction, y_fraction, scale):
    x = math.floor((rect["x"] + rect["width"] * x_fraction) * scale)
    y = math.floor((rect["y"] + rect["height"] * y_fraction) * scale)
    if not (0 <= y < image.shape[0] and 0 <= x < image.shape[1]):
        raise ValueError(f"Surface pixel outside capture: ({x}, {y})")
    return image[y, x].astype(int)


def rectangle_mask(shape, rectangles, scale):
    mask = np.zeros(shape[:2], dtype=bool)
    for rect in rectangles:
        if rect is None:
            continue
        left, top = max(0, math.floor(rect["x"] * scale)), max(0, math.floor(rect["y"] * scale))
        right = min(shape[1], math.ceil((rect["x"] + rect["width"]) * scale))
        bottom = min(shape[0], math.ceil((rect["y"] + rect["height"]) * scale))
        mask[top:bottom, left:right] = True
    return mask


def raster_metrics(difference, mask):
    pixels = difference[mask]
    if not len(pixels):
        return None
    return {"pixels": len(pixels), "mean_absolute_channel_error": float(pixels.mean()),
            "max_channel_error": int(pixels.max()), "fraction_over_16": float((pixels.max(axis=1) > 16).mean())}


def css_color(value):
    channels = [float(channel) for channel in re.findall(r"[\d.]+", value)]
    return tuple(channel / 255 for channel in channels[:3]) + (channels[3] if len(channels) == 4 else 1.,)


def native_color(value):
    channels = {key: float(channel) for key, channel in re.findall(r"([hsla]): ([\d.eE+-]+)", value)}
    if channels.keys() != {"h", "s", "l", "a"}:
        return None
    return colorsys.hls_to_rgb(channels["h"], channels["l"], channels["s"]) + (channels["a"],)


def border_token_check(label, expected, actual, quads):
    if expected is None or actual is None or not any(float(width.removesuffix("px")) > 0 for width in expected.get("borderWidth", "0px").split()):
        return []
    expected_color = css_color(expected["borderColor"])
    candidates = [(quad, native_color(quad["border_color"])) for quad in quads if max(quad["border_widths"]) > 0]
    candidates = [(quad, color) for quad, color in candidates if color and color[3] > 0]
    if not candidates:
        return [{"target": label, "passed": False, "reason": "missing native border primitive"}]
    quad, color = min(candidates, key=lambda item: sum(abs(item[0][key] - actual[key]) for key in RECT_KEYS))
    error = max(abs(color[index] - expected_color[index]) for index in range(3))
    geometry_error = max(abs(quad[key] - actual[key]) for key in RECT_KEYS)
    return [{"target": label, "passed": error <= 1 / 255 + 1e-6 and abs(color[3] - expected_color[3]) <= .001 and geometry_error <= 1.0001,
             "reference_rgba": expected_color, "native_rgba": color, "max_rgb_byte_error": error * 255,
             "native_primitive": quad, "primitive_geometry_error": geometry_error}]


def content_primitive_checks(index, expected, actual_swatch, quads):
    expected_color = css_color(expected["background"])
    candidates = []
    for quad in quads:
        if not quad["background"].startswith("Solid(") or max(quad["border_widths"]) > 0:
            continue
        color = native_color(quad["background"])
        if (color and color[3] > 0 and actual_swatch is not None
                and inside((quad["x"], quad["y"]), actual_swatch, .001)
                and inside((quad["x"] + quad["width"], quad["y"] + quad["height"]), actual_swatch, .001)):
            candidates.append((quad, color))
    if not candidates:
        return rectangle_checks(f"trigger content[{index}]", expected, None), [{
            "target": f"trigger content color[{index}]", "passed": False,
            "reason": "missing native content fill primitive"}]
    quad, color = min(candidates, key=lambda item: sum(abs(item[0][key] - expected[key]) for key in RECT_KEYS))
    error = max(abs(color[channel] - expected_color[channel]) for channel in range(3)) * 255
    alpha_error = abs(color[3] - expected_color[3]) * 255
    return rectangle_checks(f"trigger content[{index}]", expected, quad), [{
        "target": f"trigger content color[{index}]", "passed": error <= 1.0001 and alpha_error <= 1.0001,
        "max_rgb_byte_error": error, "reference_rgba": expected_color,
        "alpha_byte_error": alpha_error, "native_rgba": color, "native_primitive": quad}]


def surface_samples(label, reference_image, native_image, expected, actual, scale,
                    tolerance, positions, handles=(), foreground=None):
    if expected is None or actual is None:
        return []
    samples = []
    for x_fraction, y_fraction in positions:
        position = (expected["x"] + expected["width"] * x_fraction,
                    expected["y"] + expected["height"] * y_fraction)
        if any(inside(position, handle, 4) for handle in handles):
            continue
        if foreground is not None:
            x, y = (math.floor(coordinate * scale) for coordinate in position)
            if foreground[y, x]:
                continue
        reference = pixel(reference_image, expected, x_fraction, y_fraction, scale)
        native = pixel(native_image, actual, x_fraction, y_fraction, scale)
        delta = np.abs(reference - native)
        samples.append({"target": label, "fraction": [x_fraction, y_fraction],
                        "reference_rgb": reference.tolist(), "native_rgb": native.tolist(),
                        "max_channel_error": int(delta.max()), "budget": tolerance,
                        "passed": int(delta.max()) <= tolerance})
    if not samples:
        raise ValueError(f"{label}: every surface sample is occluded")
    return samples


def preset_mark_pixels(image, rectangle, color, scale):
    # Rounded corners and the one-pixel inset border are outside this region.
    # The complete CSS check stroke fits inside this fixed two-logical-pixel inset.
    left = math.ceil((rectangle["x"] + 2) * scale)
    top = math.ceil((rectangle["y"] + 2) * scale)
    right = math.floor((rectangle["x"] + rectangle["width"] - 2) * scale)
    bottom = math.floor((rectangle["y"] + rectangle["height"] - 2) * scale)
    crop = image[top:bottom, left:right].astype(float)
    base = np.median(crop.reshape(-1, 3), axis=0)
    direction = color - base
    contrast = float(direction @ direction)
    if contrast < 32 ** 2:
        raise ValueError("Selected-preset mark lacks measurable contrast")
    coverage = ((crop - base) @ direction) / contrast
    residual = np.max(np.abs(crop - (base + coverage[..., None] * direction)), axis=2)
    stroke = (coverage >= .5) & (residual <= 16)
    coordinates = np.argwhere(stroke)
    points = (coordinates[:, ::-1] + [left + .5, top + .5]) / scale
    peak = crop.reshape(-1, 3)[np.argmax(coverage)] if crop.size else None
    foreground = np.zeros(image.shape[:2], dtype=bool)
    # The source contour, including antialiased edge coverage, identifies ink
    # rather than a flat preset fill. Expand by the existing geometry allowance.
    ink = (coverage > .02) & (residual <= 16)
    radius = math.ceil(scale)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            y0, x0 = max(0, top + dy), max(0, left + dx)
            y1, x1 = min(image.shape[0], bottom + dy), min(image.shape[1], right + dx)
            foreground[y0:y1, x0:x1] |= ink[y0 - top - dy:y1 - top - dy, x0 - left - dx:x1 - left - dx]
    return {"points": points, "peak": peak, "foreground": foreground,
            "crop": crop, "coverage": coverage}


def preset_mark_check(index, element, expected, actual, reference_image, native_image, scale):
    color = np.array(css_color(element["after"]["borderColor"])[:3]) * 255
    reference = preset_mark_pixels(reference_image, expected, color, scale)
    native = preset_mark_pixels(native_image, actual, color, scale)
    left, right = reference["points"], native["points"]
    distance = None
    if len(left) and len(right):
        pairwise = np.sqrt(np.sum((left[:, None, :] - right[None, :, :]) ** 2, axis=2))
        distance = float(max(pairwise.min(axis=0).max(), pairwise.min(axis=1).max()))
    color_error = float(np.max(np.abs(native["peak"] - color)))
    reference_color_error = float(np.max(np.abs(reference["peak"] - color)))
    difference = np.abs(reference_image.astype(np.int16) - native_image.astype(np.int16))
    mask = rectangle_mask(reference_image.shape, [expected, actual], scale)
    result = {"target": f"preset[{index}] selected mark", "passed": distance is not None and distance <= 1.0001 and color_error <= 1.0001,
              "silhouette_hausdorff_logical_px": distance, "reference_stroke_pixels": len(left), "native_stroke_pixels": len(right),
              "reference_peak_rgb": reference["peak"].tolist(), "native_peak_rgb": native["peak"].tolist(),
              "expected_rgb": color.tolist(), "max_rgb_byte_error": color_error,
              "reference_color_byte_error": reference_color_error,
              "raw_unmasked_cell_raster": raster_metrics(difference, mask)}
    return result, reference["foreground"]


def compare(reference, native, reference_dir, native_dir):
    reference_cases = reference["cases"]
    expected_by_name = {case["name"]: case for case in reference_cases}
    actual_by_name = {case["name"]: case for case in native}
    if (not expected_by_name or len(expected_by_name) != len(reference_cases)
            or len(actual_by_name) != len(native)):
        raise ValueError("Capture sets must be nonempty with unique names")
    if expected_by_name.keys() != actual_by_name.keys():
        raise ValueError("Reference/native capture scenario sets differ")
    results = []
    for name, expected in expected_by_name.items():
        actual = actual_by_name[name]
        for key in ("scene", "theme", "direction", "scale", "canvas"):
            if expected[key] != actual[key]:
                raise ValueError(f"{name}: scenario {key} differs")
        scale = expected["scale"]
        expected_geometry, actual_geometry = dict(expected["geometry"]), actual["geometry"]
        # The original DOM capture retains every clear element. Select the actual
        # operation-bar swatch inside the popup, excluding a cleared trigger.
        panel = expected_geometry.get("panel")
        clear_elements = [element for element in expected["elements"]
                          if "ant-color-picker-clear" in element["className"].split()
                          and panel is not None and inside((element["x"], element["y"]), panel)]
        if len(clear_elements) > 1:
            raise ValueError(f"{name}: ambiguous panel clear controls")
        expected_geometry["clear"] = clear_elements[0] if clear_elements else None
        checks = []
        border_checks = []
        content_checks = []
        helper_overlays = []
        for target in ("panel", "palette", "hue", "alpha", "preview", "gradient", "clear"):
            checks.extend(rectangle_checks(target, expected_geometry.get(target),
                                           actual_geometry.get(target)))
        for target in ("editors", "presets"):
            left, right = expected_geometry[target], actual_geometry[target]
            checks.append({"target": target + " count", "passed": len(left) == len(right),
                           "reference": len(left), "native": len(right)})
            for index, (expected_rect, actual_rect) in enumerate(zip(left, right)):
                checks.extend(rectangle_checks(f"{target}[{index}]", expected_rect, actual_rect))
        if "handles" in expected_geometry:
            logical_handles = []
            for handle in expected_geometry["handles"]:
                kind, index = handle["kind"], handle["index"]
                rail = expected_geometry.get(kind)
                numbered = [element for element in expected["elements"]
                            if kind != "palette" and rail is not None
                            and re.search(r"(?:^|\s)ant-slider-handle-\d+(?:\s|$)", element["className"])
                            and inside((element["x"] + element["width"] / 2, element["y"] + element["height"] / 2), rail, .001)]
                if numbered and index >= len(numbered):
                    aliases = [other for other in expected_geometry["handles"]
                               if other["kind"] == kind and other["index"] < len(numbered)
                               and max(abs(other[key] - handle[key]) for key in RECT_KEYS) < .0001]
                    if not aliases:
                        raise ValueError(f"{name}: an auxiliary source handle has distinct painted bounds")
                    helper_overlays.append(dict(handle, aliases_logical_index=aliases[0]["index"]))
                else:
                    logical_handles.append(handle)
            expected_geometry["handles"] = logical_handles
            expected_handles = {(handle["kind"], handle["index"]): handle for handle in expected_geometry["handles"]}
            actual_handles = {(handle["kind"], handle["index"]): handle for handle in actual_geometry.get("handles", [])}
            if len(expected_handles) != len(expected_geometry["handles"]) or len(actual_handles) != len(actual_geometry.get("handles", [])):
                raise ValueError(f"{name}: duplicate handle identity")
            checks.append({"target": "handle identities", "passed": expected_handles.keys() == actual_handles.keys(),
                           "reference": list(expected_handles), "native": list(actual_handles)})
            for key, expected_handle in expected_handles.items():
                actual_handle = actual_handles.get(key)
                if actual_handle is None:
                    checks.append({"target": f"{key[0]} thumb[{key[1]}]", "passed": False, "reason": "missing handle"})
                    continue
                center_delta = [actual_handle[axis] + actual_handle[length] / 2 - expected_handle[axis] - expected_handle[length] / 2
                                for axis, length in (("x", "width"), ("y", "height"))]
                size_delta = [actual_handle[length] - expected_handle[length] for length in ("width", "height")]
                checks.append({"target": f"{key[0]} thumb[{key[1]}]", "passed": max(map(abs, center_delta + size_delta)) <= 1.0001,
                               "center_delta": center_delta, "size_delta": size_delta})
        left_rows, right_rows = expected["rows"], actual["rows"]
        checks.append({"target": "trigger count", "passed": len(left_rows) == len(right_rows),
                       "reference": len(left_rows), "native": len(right_rows)})
        for index, (left, right) in enumerate(zip(left_rows, right_rows)):
            checks.extend(rectangle_checks(f"trigger[{index}]", left["trigger"], right["trigger"]))
            checks.extend(rectangle_checks(f"swatch[{index}]", left["swatch"], right["swatch"]))
            border_checks.extend(border_token_check(f"trigger border token[{index}]", left["trigger"], right["trigger"], actual.get("quads", [])))
            border_checks.extend(border_token_check(f"swatch border token[{index}]", left["swatch"], right["swatch"], actual.get("quads", [])))
            contents = [element for element in expected["elements"]
                        if "ant-color-picker-color-block-inner" in element["className"].split()
                        and left["swatch"] is not None
                        and inside((element["x"], element["y"]), left["swatch"], .001)
                        and inside((element["x"] + element["width"], element["y"] + element["height"]), left["swatch"], .001)
                        and css_color(element["background"])[3] > 0]
            if len(contents) > 1:
                raise ValueError(f"{name}: ambiguous trigger content")
            if contents:
                geometry, colors = content_primitive_checks(index, contents[0], right["swatch"], actual.get("quads", []))
                checks.extend(geometry)
                content_checks.extend(colors)
        for index, (left, right) in enumerate(zip(expected_geometry["editors"], actual_geometry["editors"])):
            border_checks.extend(border_token_check(f"editor border token[{index}]", left, right, actual.get("quads", [])))
        reference_image = np.array(Image.open(reference_dir / f"{name}.png").convert("RGB"))
        native_image = np.array(Image.open(native_dir / f"{name}.png").convert("RGB"))
        expected_shape = (round(720 * scale), round(640 * scale), 3)
        if reference_image.shape != expected_shape or native_image.shape != expected_shape:
            raise ValueError(f"{name}: image dimensions differ from declared canvas/DPI")
        samples = []
        mark_checks = []
        composited_border_samples = []
        # Sample the empty top padding of the actual popup surface, away from its arrow/shadow.
        panel_positions = [(0.4, 5 / expected_geometry["panel"]["height"])] if expected_geometry.get("panel") else []
        samples.extend(surface_samples("panel fill", reference_image, native_image,
                                       expected_geometry.get("panel"), actual_geometry.get("panel"),
                                       scale, 1, panel_positions))
        for index, (left, right) in enumerate(zip(left_rows, right_rows)):
            if left["trigger"] is not None:
                height = left["trigger"]["height"]
                samples.extend(surface_samples(f"trigger border[{index}]", reference_image, native_image,
                                               left["trigger"], right["trigger"], scale, 1,
                                               [(0.5, 0.5 / height)]))
                bottom_samples = surface_samples(f"shadow-composited trigger border[{index}]", reference_image, native_image,
                                                 left["trigger"], right["trigger"], scale, 1, [(0.5, (height - 0.5) / height)])
                composited_border_samples.extend({key: value for key, value in sample.items() if key not in ("passed", "budget")} for sample in bottom_samples)
            if left["options"].get("value") is None:
                continue  # The clear icon is a diagonal line, not a flat color swatch.
            samples.extend(surface_samples(f"trigger swatch[{index}]", reference_image, native_image,
                                           left["swatch"], right["swatch"], scale,
                                           4 if isinstance(left["options"].get("value"), list) else 1,
                                           [(0.3125, 0.3125), (0.4375, 0.6875), (0.6875, 0.4375), (0.8125, 0.8125)]))
        handles = [element for element in expected["elements"]
                   if ("ant-color-picker-slider-handle" in element["className"]
                       or element["className"] == "ant-color-picker-handler")]
        samples.extend(surface_samples("palette gradient", reference_image, native_image,
                                       expected_geometry.get("palette"), actual_geometry.get("palette"), scale, 4,
                                       [(x, y) for x in (0.13, 0.31, 0.53, 0.77, 0.91)
                                        for y in (0.13, 0.39, 0.61, 0.87)], handles))
        for target in ("hue", "alpha", "gradient"):
            samples.extend(surface_samples(target + " gradient", reference_image, native_image,
                                           expected_geometry.get(target), actual_geometry.get(target), scale, 4,
                                           [(x, 0.4) for x in (0.08, 0.19, 0.3, 0.41, 0.52, 0.63, 0.74, 0.85, 0.92)], handles))
        samples.extend(surface_samples("preview", reference_image, native_image,
                                       expected_geometry.get("preview"), actual_geometry.get("preview"), scale, 1,
                                       [(0.3125, 0.3125), (0.6875, 0.6875)]))
        for index, (left, right) in enumerate(zip(expected_geometry["presets"], actual_geometry["presets"])):
            selected = [element for element in expected["elements"]
                        if "ant-color-picker-presets-color-checked" in element["className"].split()
                        and max(abs(element[key] - left[key]) for key in RECT_KEYS) < .001]
            if len(selected) > 1:
                raise ValueError(f"{name}: ambiguous selected-preset mark")
            foreground = None
            positions = [(0.3, 0.7), (0.7, 0.3)]
            if selected:
                check, foreground = preset_mark_check(index, selected[0], left, right, reference_image, native_image, scale)
                mark_checks.append(check)
                positions += [(0.2, 0.2), (0.8, 0.8)]
            samples.extend(surface_samples(f"preset[{index}]", reference_image, native_image,
                                           left, right, scale, 4, positions, foreground=foreground))
        for index, (left, right) in enumerate(zip(expected_geometry["editors"], actual_geometry["editors"])):
            samples.extend(surface_samples(f"editor border[{index}]", reference_image, native_image,
                                           left, right, scale, 1, [(0.5, 0.5 / left["height"])]))
            samples.extend(surface_samples(f"editor fill[{index}]", reference_image, native_image,
                                           left, right, scale, 1, [(0.5, 3 / left["height"])]))
        difference = np.abs(reference_image.astype(np.int16) - native_image.astype(np.int16))
        rectangles = [row["trigger"] for row in left_rows if row["trigger"] is not None]
        rectangles.extend(rect for rect in (expected_geometry.get("panel"), expected_geometry.get("palette")) if rect is not None)
        left = max(0, math.floor((min(rect["x"] for rect in rectangles) - 24) * scale))
        top = max(0, math.floor((min(rect["y"] for rect in rectangles) - 24) * scale))
        right = min(reference_image.shape[1], math.ceil((max(rect["x"] + rect["width"] for rect in rectangles) + 24) * scale))
        bottom = min(reference_image.shape[0], math.ceil((max(rect["y"] + rect["height"] for rect in rectangles) + 24) * scale))
        component = difference[top:bottom, left:right]
        raster_regions = expected.get("raster_regions", {})
        text_mask = rectangle_mask(reference_image.shape, raster_regions.get("text_and_input_viewports", []), scale)
        shadow_mask = np.zeros(reference_image.shape[:2], dtype=bool)
        if expected_geometry.get("panel"):
            panel = expected_geometry["panel"]
            expanded = dict(panel, x=panel["x"] - 24, y=panel["y"] - 24,
                            width=panel["width"] + 48, height=panel["height"] + 48)
            shadow_mask = rectangle_mask(reference_image.shape, [expanded], scale)
            shadow_mask &= ~rectangle_mask(reference_image.shape, [panel, raster_regions.get("arrow")] +
                                           [row["trigger"] for row in left_rows], scale)
        results.append({"name": name, "geometry_checks": checks, "surface_samples": samples,
                        "selected_mark_checks": mark_checks,
                        "resolved_content_checks": content_checks,
                        "resolved_border_checks": border_checks, "reference_helper_overlays": helper_overlays,
                        "shadow_composited_border_raster": composited_border_samples,
                        "pixel_mean_absolute_channel_error": float(difference.mean()),
                        "pixel_fraction_over_16": float((difference.max(axis=2) > 16).mean()),
                        "component_crop_physical_px": {"left": left, "top": top, "right": right, "bottom": bottom},
                        "component_pixel_mean_absolute_channel_error": float(component.mean()),
                        "component_pixel_fraction_over_16": float((component.max(axis=2) > 16).mean()),
                        "text_and_input_viewport_raster": raster_metrics(difference, text_mask),
                        "popup_shadow_raster": raster_metrics(difference, shadow_mask)})
    checks = [check for result in results for check in result["geometry_checks"]]
    samples = [sample for result in results for sample in result["surface_samples"]]
    borders = [check for result in results for check in result["resolved_border_checks"]]
    marks = [check for result in results for check in result["selected_mark_checks"]]
    contents = [check for result in results for check in result["resolved_content_checks"]]
    return {"schema_version": 1, "cases": len(results), "provenance": reference["provenance"],
            "renderer": reference["renderer"], "geometry_checks": len(checks),
            "failed_geometry_checks": sum(not check["passed"] for check in checks),
            "resolved_border_checks": len(borders), "failed_border_checks": sum(not check["passed"] for check in borders),
            "selected_mark_checks": len(marks), "failed_selected_mark_checks": sum(not check["passed"] for check in marks),
            "resolved_content_checks": len(contents), "failed_content_checks": sum(not check["passed"] for check in contents),
            "reference_helper_overlays": sum(len(result["reference_helper_overlays"]) for result in results),
            "surface_samples": len(samples), "failed_surface_samples": sum(not sample["passed"] for sample in samples),
            "budgets": {"geometry_logical_px": 1, "solid_rgb_bytes": 1, "gradient_rgb_bytes": 4,
                        "content_primitive_rgba_bytes": 1},
            "separate_raster_assessment": ["text antialiasing", "shadow blur", "complete-image error"],
            "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8-sig"))
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8-sig"))
    report = compare(reference, native, args.reference, args.native)
    report["comparator_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report["browser_metrics_sha256"] = hashlib.sha256((args.reference / "browser-metrics.json").read_bytes()).hexdigest()
    report["native_metrics_sha256"] = hashlib.sha256((args.native / "native-metrics.json").read_bytes()).hexdigest()
    native_provenance = args.native / "native-provenance.json"
    if native_provenance.exists():
        provenance = json.loads(native_provenance.read_text(encoding="utf-8-sig"))
        if provenance["metrics_sha256"] != report["native_metrics_sha256"]:
            raise ValueError("Native metric file differs from its captured binary/source provenance")
        scene_path = "fixtures/color_picker/visual-cases.json"
        source_hash = provenance["source_sha256"][scene_path]
        browser_hashes = [digest for file, digest in reference["provenance"]["inputs"].items()
                          if file.replace("\\", "/").endswith(scene_path)]
        if browser_hashes != [source_hash]:
            raise ValueError("Native and browser capture fixtures were built from different shared cases")
        report["native_provenance"] = provenance
        report["native_provenance_sha256"] = hashlib.sha256(native_provenance.read_bytes()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key not in ("results", "provenance", "native_provenance", "renderer")}, indent=2))
    if any(report[key] for key in ("failed_geometry_checks", "failed_border_checks", "failed_surface_samples", "failed_selected_mark_checks", "failed_content_checks")):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
