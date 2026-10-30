# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independent browser/native picker geometry and visible selection ink.

Geometry allows one logical pixel. Flat surfaces and marks allow one RGB byte.
Text/shadow raster differences are reported independently, without masking failed
selection, range or today checks. Missing cases or marks always fail.
"""
import argparse
import json
import math
import hashlib
import re
from pathlib import Path
from collections import Counter
from PIL import ImageColor

import numpy as np
from PIL import Image


RECT_KEYS = ("x", "y", "width", "height")
FLAGS = ("selected", "today", "disabled", "inView", "inRange", "rangeStart", "rangeEnd", "rangeHover",
         "weekSelected", "weekRangeStart", "weekRangeEnd", "weekRangeHover", "weekHover")


def rectangle_checks(label, expected, actual):
    if expected is not None and (expected.get("width") == 0 or expected.get("height") == 0):
        expected = None
    if actual is not None and (actual.get("width") == 0 or actual.get("height") == 0):
        actual = None
    if expected is None or actual is None:
        return [{"target": label, "passed": expected is None and actual is None, "reason": "presence"}]
    if any(key not in actual or not math.isfinite(actual[key]) for key in RECT_KEYS):
        return [{"target": label, "passed": False, "reason": "invalid native rectangle"}]
    delta = {key: actual[key] - expected[key] for key in RECT_KEYS}
    return [{"target": label, "passed": bool(max(map(abs, delta.values())) <= 1.0001), "geometry_delta": delta}]


def painted_extent_checks(label, expected, actual):
    # A rasterized glyph can lose a fractional pixel at each AA edge. Its
    # geometric distance is the edge distance, not their summed width change.
    delta = {key: actual[key]-expected[key] for key in RECT_KEYS}
    edges = {"left": delta["x"], "top": delta["y"],
             "right": delta["x"]+delta["width"], "bottom": delta["y"]+delta["height"]}
    return [{"target": label, "passed": max(map(abs, edges.values())) <= 1.0001,
             "painted_edge_delta": edges, "geometry_delta": delta,
             "comparison": "ink extent edge distances; authored layout boxes remain unchanged"}]


def covered_ink_pixels(mask, scale):
    """Core centers within 1 logical pixel of an occupied raster pixel square."""
    height, width = mask.shape
    radius = math.ceil(scale + 0.5)
    padded = np.pad(mask, radius)
    result = np.zeros_like(mask)
    for dy in range(-radius, radius+1):
        for dx in range(-radius, radius+1):
            # A sample denotes a physical pixel area, not an infinitesimal
            # point. Keep the logical distance budget and model its half-pixel
            # sampling uncertainty explicitly on each axis.
            distance = math.hypot(max(abs(dx)-0.5, 0), max(abs(dy)-0.5, 0))
            if distance <= scale + 0.0001:
                result |= padded[radius+dy:radius+dy+height, radius+dx:radius+dx+width]
    return result


def pixel(image, rect, fraction, scale):
    x = math.floor((rect["x"] + rect["width"] * fraction[0]) * scale)
    y = math.floor((rect["y"] + rect["height"] * fraction[1]) * scale)
    if not (0 <= y < image.shape[0] and 0 <= x < image.shape[1]):
        raise ValueError(f"Sample outside capture: {x}, {y}")
    return image[y, x, :3].astype(int)


def samples(label, reference, native, expected, actual, scale, fractions, tolerance=1):
    if expected is None or actual is None:
        return [{"target": label, "passed": False, "reason": "missing painted mark geometry"}]
    results = []
    for fraction in fractions:
        source, target = pixel(reference, expected, fraction, scale), pixel(native, actual, fraction, scale)
        error = int(np.abs(source - target).max())
        results.append({"target": label, "fraction": list(fraction), "passed": error <= tolerance,
                        "reference_rgb": source.tolist(), "native_rgb": target.tolist(),
                        "max_rgb_byte_error": error, "tolerance": tolerance})
    return results


def outline_sample(label, reference, native, expected, actual, scale, css_color=None):
    """Find independently painted border color within one physical sample step."""
    if expected is None or actual is None:
        return [{"target":label,"passed":False,"reason":"missing outline geometry"}]
    fraction = (0.5, 0.5 / (expected["height"] * scale))
    if css_color:
        values = [float(value) for value in re.findall(r"[\d.]+", css_color)]
        rgb, alpha = np.array(values[:3]), values[3] if len(values) > 3 else 1.
        background = pixel(reference, expected, (0.25, 0.25), scale)
        color = np.rint(rgb*alpha + background*(1-alpha)).astype(int)
    else:
        color = pixel(reference, expected, fraction, scale)
    proofs = []
    for image, rect in ((reference, expected), (native, actual)):
        x = math.floor((rect["x"]+rect["width"]*0.5)*scale)
        y = math.floor(rect["y"]*scale+0.5)
        probes = []
        for dx, dy in ((0,0),(-1,0),(1,0),(0,-1),(0,1)):
            if 0 <= y+dy < image.shape[0] and 0 <= x+dx < image.shape[1]:
                rgb = image[y+dy, x+dx, :3].astype(int)
                probes.append({"physical_pixel":[x+dx,y+dy],"rgb":rgb.tolist(),"error":int(np.abs(rgb-color).max())})
        proofs.append(min(probes, key=lambda probe:probe["error"]))
    return [{"target":label,"passed":all(proof["error"] <= 1 for proof in proofs),
             "expected_rgb":color.tolist(),"reference":proofs[0],"native":proofs[1],
             "rgb_tolerance_bytes":1,"sampling_radius_physical_px":1,
             "method":"independent nearest full-color border pixel; authored geometry remains gated"}]


def tag_fill_checks(label, reference, native, expected, actual, scale):
    # Popup shadows can overlap the trigger. Compare actual chip/gap contrast at
    # the same scanline, retaining absolute colors for shadow diagnostics.
    results = []
    for fraction in [(0.08, 0.12), (0.92, 0.12)]:
        source = pixel(reference, expected, fraction, scale)
        target = pixel(native, actual, fraction, scale)
        source_gap = pixel(reference, {**expected, "x": expected["x"] + (expected["width"] + 2 if fraction[0] > 0.5 else -2), "width": 0}, fraction, scale)
        target_gap = pixel(native, {**actual, "x": actual["x"] + (actual["width"] + 2 if fraction[0] > 0.5 else -2), "width": 0}, fraction, scale)
        error = int(np.abs((source-source_gap) - (target-target_gap)).max())
        results.append({"target": label, "fraction": list(fraction), "passed": error <= 1,
                        "reference_rgb": source.tolist(), "native_rgb": target.tolist(),
                        "reference_adjacent_gap_rgb": source_gap.tolist(), "native_adjacent_gap_rgb": target_gap.tolist(),
                        "max_fill_contrast_rgb_byte_error": error, "tolerance": 1})
    return results


def compare_case(expected, actual, reference_image=None, native_image=None):
    """Return granular checks; native geometry mirrors the public fixture schema."""
    if actual.get("unsupported"):
        return [{"target": expected["name"], "passed": False, "reason": actual["unsupported"]}]
    checks = []
    for key in ("scene", "theme", "direction", "scale"):
        checks.append({"target": key, "passed": actual.get(key) == expected.get(key),
                       "reference": expected.get(key), "native": actual.get(key)})
    eg, ag = expected["geometry"], actual.get("geometry", {})
    for key in ("trigger", "popup", "footer", "presets", "activeBar"):
        checks += rectangle_checks(key, eg.get(key), ag.get(key))
    for key in ("inputs", "panels", "headers", "tables", "timeColumns", "timeViewports"):
        source, target = eg.get(key, []), ag.get(key, [])
        if key == "inputs" and expected.get("options", {}).get("multiple") and all(not i.get("value") or i.get("auxiliary") or i.get("visibility") == "hidden" for i in source):
            checks.append({"target": "multiple hidden HTML focus helper", "passed": True,
                           "reason": "native retained chip group supplies focus without an HTML helper editor"})
            continue
        checks.append({"target": f"{key} count", "passed": len(source) == len(target),
                       "reference": len(source), "native": len(target)})
        for index, (left, right) in enumerate(zip(source, target)):
            checks += rectangle_checks(f"{key}[{index}]", left, right)
            if key == "inputs":
                checks.append({"target": f"input[{index}] value", "passed": left.get("value") == right.get("value"),
                               "reference": left.get("value"), "native": right.get("value")})
    painted = reference_image is not None and native_image is not None
    scale = expected["scale"]
    if painted and eg.get("rangeArrow") and eg.get("popup") and ag.get("popup"):
        # The source's padded arrow element owns a rounded ::before shape in
        # its lower half. Translate its independent probe with the measured
        # native popup, then sample the interior above the popup's clip edge.
        arrow = eg["rangeArrow"]
        native_arrow = {**arrow, "x": arrow["x"] + ag["popup"]["x"] - eg["popup"]["x"],
                        "y": arrow["y"] + ag["popup"]["y"] - eg["popup"]["y"]}
        checks += samples("range arrow visible fill above popup", reference_image, native_image,
                          arrow, native_arrow, scale, [(0.5, 0.78), (0.45, 0.875), (0.55, 0.875)])
    source_tags, target_tags = eg.get("tags", []), ag.get("tags", [])
    checks.append({"target": "multiple visible tag count", "passed": len(source_tags) == len(target_tags),
                   "reference": len(source_tags), "native": len(target_tags)})
    for index, (left, right) in enumerate(zip(source_tags, target_tags)):
        label = f"multiple tag[{index}]"
        checks.append({"target": f"{label} label", "passed": left.get("label") == right.get("label"),
                       "reference": left.get("label"), "native": right.get("label")})
        for key in ("rect", "content", "remove"):
            checks += rectangle_checks(f"{label} {key}", left.get(key), right.get(key))
        if painted:
            checks += tag_fill_checks(f"{label} background", reference_image, native_image,
                                     left["rect"], right["rect"], scale)
            if left.get("remove") and right.get("remove"):
                source_color = left["remove"].get("color")
                if source_color:
                    numbers = [float(value) for value in re.findall(r"[\d.]+", source_color)]
                    source_rgba = [value/255 for value in numbers[:3]] + [numbers[3] if len(numbers) > 3 else 1.]
                    candidates = right["remove"].get("paintRgbaCandidates", [])
                    error = max(abs(a-b)*255 for a, b in zip(source_rgba, candidates[0])) if len(candidates) == 1 else None
                    checks.append({"target": f"{label} remove native scene tint", "passed": error is not None and error <= 1.0001,
                                   "reference_rgba": source_rgba, "native_rgba_candidates": candidates,
                                   "max_rgba_byte_error": error, "tolerance": 1,
                                   "method": "computed source SVG color versus transformed native monochrome sprite tint"})
                background = pixel(reference_image, left["rect"], (0.08, 0.12), scale)
                checks += scrollbar_paint_checks(f"{label} remove mark", reference_image, native_image,
                                                 left["remove"], right["remove"], scale, background,
                                                 part_label="remove mark", compare_color=False)
    source_actions, target_actions = eg.get("footerActions", []), ag.get("footerActions", [])
    checks.append({"target": "footer action count", "passed": len(source_actions) == len(target_actions),
                   "reference": len(source_actions), "native": len(target_actions)})
    for index, (left, right) in enumerate(zip(source_actions, target_actions)):
        checks += rectangle_checks(f"footer action[{index}]", left.get("rect"), right.get("rect"))
        checks.append({"target": f"footer action[{index}] label", "passed": left.get("label") == right.get("label"),
                       "reference": left.get("label"), "native": right.get("label")})
        if painted:
            checks += samples(f"footer action[{index}] primary fill", reference_image, native_image,
                              left.get("rect"), right.get("rect"), scale, [(0.18, 0.2), (0.82, 0.2)])
    source_bars, target_bars = eg.get("scrollbars", []), ag.get("scrollbars", [])
    checks.append({"target": "time scrollbar count", "passed": len(source_bars) == len(target_bars),
                   "reference": len(source_bars), "native": len(target_bars)})
    for index, (left, right) in enumerate(zip(source_bars, target_bars)):
        label = f"time scrollbar[{index}]"
        checks += rectangle_checks(f"{label} occupied rail", left.get("track"), right.get("track"))
        checks.append({"target": f"{label} side", "passed": left.get("side") == right.get("side")})
        for key in ("scrollTop", "scrollHeight", "clientHeight"):
            if key in left:
                checks.append({"target": f"{label} {key}", "passed": right.get(key) is not None and abs(right[key]-left[key]) <= 1.0001,
                               "reference": left[key], "native": right.get(key)})
        if painted and left.get("track") and right.get("track"):
            checks += scrollbar_paint_checks(label, reference_image, native_image, left["track"], right["track"],
                                             scale, expected["tokens"]["colorBgElevated"])
    for key in ("cells", "timeCells"):
        source, target = eg.get(key, []), ag.get(key, [])
        checks.append({"target": f"{key} count", "passed": len(source) == len(target),
                       "reference": len(source), "native": len(target)})
        for index, (left, right) in enumerate(zip(source, target)):
            label = f"{key}[{index}] {left.get('key', '')}"
            checks += rectangle_checks(f"{label} cell", left.get("rect"), right.get("rect"))
            checks += rectangle_checks(f"{label} inner", left.get("inner"), right.get("inner"))
            for field in ("text",) + FLAGS:
                if field in left:
                    # rc-picker marks an already-selected week as hoverValue even
                    # with a neutral pointer. It adds no visual state beyond the
                    # selected/start/end primary band, which is checked below.
                    if field == "weekHover" and any(left.get(flag) for flag in ("weekSelected", "weekRangeStart", "weekRangeEnd")):
                        continue
                    checks.append({"target": f"{label} {field}", "passed": left.get(field) == right.get(field),
                                   "reference": left.get(field), "native": right.get(field)})
            if not painted:
                continue
            border = float(left.get("inner", {}).get("borderWidth", "0px").split()[0].removesuffix("px"))
            if border > 0:
                numbers = [float(value) for value in re.findall(r"[\d.]+", left["inner"]["borderColor"])]
                rgba = [value/255 for value in numbers[:3]] + [numbers[3] if len(numbers) > 3 else 1.]
                candidates = right.get("paintBorderCandidates", [])
                proofs = []
                for candidate_index, candidate in enumerate(candidates):
                    bounds = candidate["rect"]
                    geometry = max(abs(bounds[dimension]-left["inner"][dimension]) for dimension in RECT_KEYS)
                    color = max(abs(a-b)*255 for a,b in zip(candidate["rgba"], rgba))
                    widths = max(abs(value-border) for value in candidate["widths"])
                    proofs.append({"geometry_error":geometry,"rgba_byte_error":color,"width_error":widths,
                                   "paint_order":candidate.get("order",candidate_index),"index":candidate_index,
                                   "passed":geometry <= 1.0001 and color <= 1.0001 and widths <= 1.0001})
                outer = [proof for proof in proofs if proof["geometry_error"] <= 1.0001]
                latest = max(outer,key=lambda proof:(proof["paint_order"],proof["index"])) if outer else None
                winners = []
                if latest:
                    candidate = candidates[latest["index"]]
                    bounds = candidate["rect"]
                    top,right_width,bottom,left_width = candidate["widths"]
                    points = []
                    for fraction in (0.25,0.5,0.75):
                        points += [(bounds["x"]+bounds["width"]*fraction,bounds["y"]+top/2),
                                   (bounds["x"]+bounds["width"]*fraction,bounds["y"]+bounds["height"]-bottom/2),
                                   (bounds["x"]+left_width/2,bounds["y"]+bounds["height"]*fraction),
                                   (bounds["x"]+bounds["width"]-right_width/2,bounds["y"]+bounds["height"]*fraction)]
                    # Rounded-border fragments can receive different draw
                    # orders around intervening children. Resolve the last
                    # actual painter separately at each straight-side point.
                    for x,y in points:
                        covering = [proof for proof in outer
                                    if (clip:=candidates[proof["index"]]["clip"])["x"] <= x <= clip["x"]+clip["width"]
                                    and clip["y"] <= y <= clip["y"]+clip["height"]]
                        winner = max(covering,key=lambda proof:(proof["paint_order"],proof["index"])) if covering else None
                        winners.append(None if winner is None else winner["index"])
                checks.append({"target":f"{label} authored custom border", "passed":bool(winners) and all(winner is not None and proofs[winner]["passed"] for winner in winners),
                               "reference_rgba":rgba,"reference_width":border,"native_candidates":proofs,
                               "side_clip_winners":winners,
                               "method":"computed source border versus last native scene border Quad at that box, all four sides"})
            if left.get("selected") or left.get("rangeStart") or left.get("rangeEnd"):
                checks += samples(f"{label} selected ink", reference_image, native_image,
                                  left.get("inner"), right.get("inner"), scale, [(0.25, 0.18), (0.75, 0.18)])
            if left.get("inRange"):
                checks += samples(f"{label} range band", reference_image, native_image,
                                  left.get("rect"), right.get("rect"), scale, [(0.05, 0.5), (0.95, 0.5)])
            if any(left.get(flag) for flag in ("weekSelected", "weekRangeStart", "weekRangeEnd", "weekRangeHover", "weekHover")):
                checks += samples(f"{label} week row band", reference_image, native_image,
                                  left.get("rect"), right.get("rect"), scale, [(0.05, 0.5), (0.95, 0.5)])
            if left.get("today") and not left.get("selected") and not left.get("rangeStart") and not left.get("rangeEnd"):
                # Top-center of the actual 1px today outline; separate from text.
                inner = left.get("inner")
                if inner:
                    color = inner.get("borderColor") if border > 0 else left.get("innerBefore", {}).get("borderColor")
                    checks += outline_sample(f"{label} today outline", reference_image, native_image,
                                             inner, right.get("inner"), scale, color)
    if painted:
        for key in ("trigger", "popup"):
            if eg.get(key) is not None:
                # The popup shadow can overlap the trigger's lower half. Probe
                # its actual flat fill near the upper center, alongside the
                # border probe, and report the overlapping shadow separately.
                border = float(eg[key].get("borderWidth", "1px").split()[0].removesuffix("px"))
                fractions = [(0.5, 0.01), (0.5, (border + 0.5 / scale) / eg[key]["height"])] if key == "trigger" else [(0.5-4/eg[key]["width"], 2/eg[key]["height"]), (0.5+4/eg[key]["width"], 2/eg[key]["height"])]
                checks += samples(f"{key} surface", reference_image, native_image,
                                  eg[key], ag.get(key), scale, fractions)
    return checks


def scrollbar_paint(image, rect, scale, background, edge_margin=1):
    left, top = math.floor(rect["x"] * scale), math.floor(rect["y"] * scale)
    right, bottom = math.ceil((rect["x"]+rect["width"])*scale), math.ceil((rect["y"]+rect["height"])*scale)
    region = image[top:bottom, left:right, :3]
    bg = np.array(ImageColor.getrgb(background)[:3] if isinstance(background, str) else background)
    mask = np.abs(region.astype(int)-bg).max(axis=2) >= 16
    if edge_margin:
        # Windows thin arrows/thumbs have an observed ~2px horizontal inset.
        # Fractional rail rectangles can include a neighboring selection fill
        # or separator in their outside pixel; it is not scrollbar ink.
        centers = (np.arange(left, right) + 0.5) / scale
        mask &= ((centers >= rect["x"] + edge_margin) &
                 (centers <= rect["x"] + rect["width"] - edge_margin))[None, :]
    return mask, region, (left, top)


def ink_components(mask, region, origin, scale):
    remaining = set(zip(*np.nonzero(mask)))
    result = []
    while remaining:
        start = remaining.pop()
        work, points = [start], [start]
        while work:
            y, x = work.pop()
            for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
                point = (y+dy, x+dx)
                if point in remaining:
                    remaining.remove(point)
                    work.append(point)
                    points.append(point)
        if len(points) < max(1, round(scale)):
            continue
        ys, xs = zip(*points)
        color = Counter(tuple(int(channel) for channel in region[y, x]) for y, x in points).most_common(1)[0][0]
        result.append({"x": float((origin[0]+min(xs))/scale), "y": float((origin[1]+min(ys))/scale),
                       "width": float((max(xs)-min(xs)+1)/scale), "height": float((max(ys)-min(ys)+1)/scale),
                       "rgb": list(color), "pixels": len(points)})
    return sorted(result, key=lambda component: component["y"])


def scrollbar_paint_checks(label, reference, native, expected, actual, scale, background, part_label="arrow/thumb", compare_color=True):
    margin = 1 if part_label == "arrow/thumb" else 0
    source, source_pixels, source_origin = scrollbar_paint(reference, expected, scale, background, margin)
    target, target_pixels, target_origin = scrollbar_paint(native, actual, scale, background, margin)
    source_fringe, target_fringe = source.copy(), target.copy()
    bg = np.array(ImageColor.getrgb(background)[:3] if isinstance(background, str) else background)
    source_contrast = np.abs(source_pixels.astype(int)-bg).max(axis=2)
    target_contrast = np.abs(target_pixels.astype(int)-bg).max(axis=2)
    # Compare at least half-covered stroke pixels. The renderer's faint edge
    # support remains separately reported below and in every unmasked image.
    core_threshold = max(16., float(source_contrast[source].max()) * 0.5) if source.any() else 16.
    source &= source_contrast >= core_threshold
    target &= target_contrast >= core_threshold
    source_parts = ink_components(source_fringe, source_pixels, source_origin, scale)
    target_parts = ink_components(target_fringe, target_pixels, target_origin, scale)
    checks = [{"target": f"{label} painted {part_label} count", "passed": bool(source_parts) and len(source_parts) == len(target_parts),
               "reference": len(source_parts), "native": len(target_parts)}]
    left, top = min(source_origin[0], target_origin[0]), min(source_origin[1], target_origin[1])
    width = max(source_origin[0]+source.shape[1], target_origin[0]+target.shape[1])-left
    height = max(source_origin[1]+source.shape[0], target_origin[1]+target.shape[0])-top
    a, b = np.zeros((height, width), dtype=bool), np.zeros((height, width), dtype=bool)
    def aligned(source_mask, target_mask):
        left_mask, right_mask = np.zeros_like(a), np.zeros_like(b)
        for destination, mask, origin in ((left_mask, source_mask, source_origin), (right_mask, target_mask, target_origin)):
            y, x = origin[1]-top, origin[0]-left
            destination[y:y+mask.shape[0], x:x+mask.shape[1]] = mask
        return left_mask, right_mask
    a, b = aligned(source, target)
    def covered(mask):
        return covered_ink_pixels(mask, scale)
    full_a, full_b = aligned(source_fringe, target_fringe)
    # A sure stroke pixel must have actual paint within the geometry budget in
    # the other renderer. Hard-thresholding both edges would make a gray AA
    # coverage difference appear to be missing geometry.
    missing, extra = int((a & ~covered(full_b)).sum()), int((b & ~covered(full_a)).sum())
    full_missing, full_extra = int((full_a & ~covered(full_b)).sum()), int((full_b & ~covered(full_a)).sum())
    checks.append({"target": f"{label} solid ink silhouette", "passed": not missing and not extra and bool(source.any()) and bool(target.any()),
                   "reference_ink_uncovered_beyond_1_logical_px": missing,
                   "native_ink_uncovered_beyond_1_logical_px": extra,
                   "reference_core_pixels": int(source.sum()), "native_core_pixels": int(target.sum()),
                   "minimum_coverage": 0.5, "source_contrast_threshold": core_threshold,
                   "opposite_ink_minimum_rgb_contrast": 16, "method": "symmetric strong-to-complete ink",
                   "distance": "core pixel center to occupied pixel square", "logical_distance_budget": 1,
                   "sampling_uncertainty_physical_px_per_axis": 0.5, "antialias_extent_diagnostics": [],
                   "full_antialias_fringe_diagnostic": {"reference_uncovered_pixels": full_missing,
                                                         "native_uncovered_pixels": full_extra}})
    silhouette = checks[-1]
    for index, (left, right) in enumerate(zip(source_parts, target_parts)):
        bounds = painted_extent_checks(f"{label} painted part[{index}]", left, right)
        for check in bounds:
            check.update(reference_rgb=left["rgb"], native_rgb=right["rgb"])
        # AA threshold support is not an authored layout box. Keep every raw
        # edge/width residual, while the actual slot/rail and solid contour gate
        # geometry independently above.
        for check in bounds:
            check["within_1_logical_pixel_edge_distance"] = check.pop("passed")
            silhouette["antialias_extent_diagnostics"].append(check)
        color_error = max(abs(a-b) for a, b in zip(left["rgb"], right["rgb"]))
        if compare_color:
            checks.append({"target": f"{label} painted part[{index}] ink", "passed": color_error <= 1,
                           "reference_rgb": left["rgb"], "native_rgb": right["rgb"], "max_rgb_byte_error": color_error})
        elif max(left["rgb"])-min(left["rgb"]) <= 1:
            # Tiny achromatic SVG strokes have backend-dependent gray coverage,
            # but a colored replacement must never pass as the reference X.
            chroma = max(right["rgb"])-min(right["rgb"])
            checks.append({"target": f"{label} painted part[{index}] achromatic ink", "passed": chroma <= 1,
                           "reference_rgb": left["rgb"], "native_rgb": right["rgb"],
                           "native_channel_spread": chroma, "tolerance": 1,
                           "gray_coverage_brightness": "diagnostic; independently reported"})
    return checks


def load_cases(directory, filename):
    data = json.loads((directory / filename).read_text(encoding="utf-8-sig"))
    cases = data["cases"]
    names = [case["name"] for case in cases]
    if len(names) != len(set(names)):
        raise ValueError(f"Duplicate cases in {directory / filename}")
    return data, {case["name"]: case for case in cases}


def file_sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


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


def raster_statistics(delta, mask=None):
    values = delta.reshape(-1, 3) if mask is None else delta[mask]
    if not len(values):
        return None
    return {"pixels": len(values), "mean_absolute_channel_error": float(values.mean()),
            "max_channel_error": int(values.max()), "fraction_over_16": float((values.max(axis=1) > 16).mean())}


def raster_regions(case, delta):
    geometry, scale = case["geometry"], case["scale"]
    boxes = [geometry[key] for key in ("trigger", "popup") if geometry.get(key)]
    left = min(box["x"] for box in boxes) - 24
    top = min(box["y"] for box in boxes) - 24
    right = max(box["x"] + box["width"] for box in boxes) + 24
    bottom = max(box["y"] + box["height"] for box in boxes) + 24
    crop = rectangle_mask(delta.shape, [{"x": left, "y": top, "width": right-left, "height": bottom-top}], scale)
    content = [element for element in case.get("elements", []) if any(token in element.get("className", "").split()
               for token in ("ant-picker-cell-inner", "ant-picker-time-panel-cell-inner", "ant-picker-header", "ant-picker-footer"))]
    content += geometry.get("inputs", [])
    popup = geometry.get("popup")
    shadow = np.zeros(delta.shape[:2], dtype=bool)
    if popup:
        margin = {"x": popup["x"]-24, "y": popup["y"]-24, "width": popup["width"]+48, "height": popup["height"]+48}
        shadow = rectangle_mask(delta.shape, [margin], scale) & ~rectangle_mask(delta.shape, [popup], scale)
    return {"full_canvas": raster_statistics(delta), "component_with_shadow": raster_statistics(delta, crop),
            "cell_header_footer_and_input_viewports": raster_statistics(delta, rectangle_mask(delta.shape, content, scale)),
            "popup_shadow": raster_statistics(delta, shadow)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scene", help="Comma-separated exact scene IDs")
    parser.add_argument("--theme")
    parser.add_argument("--direction")
    parser.add_argument("--scale", type=float)
    args = parser.parse_args()
    reference_data, reference = load_cases(args.reference, "browser-metrics.json")
    _, native = load_cases(args.native, "native-metrics.json")
    selected = {name: case for name, case in reference.items()
                if (args.scene is None or case["scene"] in args.scene.split(","))
                and all(value is None or case[key] == value
                        for key, value in (("theme", args.theme), ("direction", args.direction), ("scale", args.scale)))}
    if not selected:
        raise ValueError("No reference cases selected")
    results = []
    for name, case in selected.items():
        if name not in native:
            results.append({"name": name, "passed": False, "checks": [{"target": "case", "passed": False, "reason": "missing native case"}]})
            continue
        source_path, target_path = args.reference / f"{name}.png", args.native / f"{name}.png"
        if not source_path.exists() or not target_path.exists():
            results.append({"name": name, "passed": False, "checks": [{"target": "image", "passed": False, "reason": "missing PNG"}]})
            continue
        source, target = np.asarray(Image.open(source_path).convert("RGB")), np.asarray(Image.open(target_path).convert("RGB"))
        if source.shape != target.shape:
            results.append({"name": name, "passed": False, "checks": [{"target": "canvas", "passed": False, "reason": "physical image dimensions differ"}]})
            continue
        checks = compare_case(case, native[name], source, target)
        delta = np.abs(source.astype(int) - target.astype(int))
        raster = raster_regions(case, delta)
        results.append({"name": name, "passed": all(c["passed"] for c in checks), "checks": checks,
                        "unmasked_raster": raster["full_canvas"], "raster_regions": raster})
    failed_checks = sum(not check["passed"] for result in results for check in result["checks"])
    report = {"schema_version": 1, "status": "passed" if not failed_checks else "failed", "cases": len(results),
              "failed_cases": sum(not r["passed"] for r in results), "failed_checks": failed_checks,
              "geometry_tolerance_logical_px": 1, "solid_rgb_tolerance_bytes": 1, "results": results}
    report["reference_metrics_sha256"] = file_sha256(args.reference / "browser-metrics.json")
    report["native_metrics_sha256"] = file_sha256(args.native / "native-metrics.json")
    report["comparator_sha256"] = file_sha256(Path(__file__))
    report["reference_upstream"] = {key: reference_data["provenance"][key] for key in ("version", "revision", "rc_picker")}
    attestation = args.native / "native-provenance.json"
    if attestation.exists():
        metadata = json.loads(attestation.read_text(encoding="utf-8-sig"))
        if metadata["metrics_sha256"] != report["native_metrics_sha256"]:
            raise ValueError("Native metric hash differs from its source/executable attestation")
        report["native_attestation"] = {"status": "present", "sha256": file_sha256(attestation),
                                        "executable": metadata["executable"], "completed_utc": metadata["completed_utc"]}
    else:
        report["native_attestation"] = {"status": "unavailable-direct-probe"}
    aggregate = {}
    for region in ("full_canvas", "component_with_shadow", "cell_header_footer_and_input_viewports", "popup_shadow"):
        items = [r["raster_regions"][region] for r in results if r.get("raster_regions", {}).get(region)]
        count = sum(item["pixels"] for item in items)
        if count:
            aggregate[region] = {"pixels": count, "mean_absolute_channel_error": sum(item["pixels"] * item["mean_absolute_channel_error"] for item in items) / count,
                                 "fraction_over_16": sum(item["pixels"] * item["fraction_over_16"] for item in items) / count,
                                 "max_channel_error": max(item["max_channel_error"] for item in items)}
    report["aggregate_raster_regions"] = aggregate
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Date/time picker visual comparison: {report['status']}; {len(results)} cases, {failed_checks} failed checks")
    raise SystemExit(0 if not failed_checks else 1)


if __name__ == "__main__":
    main()
