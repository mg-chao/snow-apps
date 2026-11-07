# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independently captured Pagination surfaces and report pixel differences.

Each visible fill/border has a one-logical-pixel geometry budget, one byte of RGB
rounding, and .001 alpha rounding. Text and shadows remain visible in the pixel
report and paired captures; they are not disguised by a whole-window average.
"""
import argparse
import colorsys
import json
import re
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image


def css_color(value):
    values = [float(number) for number in re.findall(r"[\d.]+", value)]
    return tuple(number / 255 for number in values[:3]) + (values[3] if len(values) == 4 else 1.,)


def native_color(value):
    if not value.startswith("Solid(Hsla"):
        return None
    values = dict((key, float(value)) for key, value in re.findall(r"([hsla]): ([\d.eE+-]+)", value))
    return colorsys.hls_to_rgb(values["h"], values["l"], values["s"]) + (values["a"],)


def expand(values):
    return values * 4 if len(values) == 1 else values * 2 if len(values) == 2 else values + [values[1]] if len(values) == 3 else values


def targets(case):
    selectors = {".ant-pagination-item-link", ".ant-pagination input", ".ant-select",
                 ".ant-select-dropdown", ".ant-select-item-option", ".ant-input-number"}
    for element in case["metrics"]["elements"]:
        if element["w"] == 0 or element["h"] == 0 or element["display"] == "none":
            continue
        page = element["selector"] == ".ant-pagination > li" and "ant-pagination-item" in element["class"].split()
        if not page and element["selector"] not in selectors:
            continue
        fill = css_color(element["background"])
        if fill[3] > 0:
            yield dict(element, channel="color", color=fill)
        widths = expand([float(number.removesuffix("px")) for number in element["borderWidth"].split()])
        colors = expand([css_color(value) for value in re.findall(r"rgba?\([^)]+\)", element["border"])])
        visible = [color for color, width in zip(colors, widths) if color[3] > 0 and width > 0]
        if visible:
            yield dict(element, channel="stroke", color=visible[0], border_widths=widths)


def total_ink(case, image, scale, element=None):
    """Measure actual line bands, independently of surface layout primitives.

    These fixtures leave the column below the total label empty. Scan that column
    to the bottom of the fixture, so an accidental extra wrapped line is visible
    even when the outer total item retains its original single-line height.
    Glyph rasterization is deliberately not treated as a pixel-identity target.
    """
    element = element or next((element for element in case["metrics"]["elements"]
                    if "ant-pagination-total-text" in element["class"].split()
                    and element["w"] > 0 and element["display"] != "none"), None)
    if element is None:
        return None
    x0 = max(0, round((element["x"] - 1) * scale))
    x1 = min(image.shape[1], round((element["x"] + element["w"] + 1) * scale))
    y0, y1 = max(0, round(element["y"] * scale)), min(image.shape[0], round(390 * scale))
    crop = image[y0:y1, x0:x1]
    background = image[round(5 * scale), round(5 * scale)]
    mask = np.max(np.abs(crop - background), axis=2) > 24
    rows = np.flatnonzero(mask.any(axis=1))
    bands = []
    for row in rows:
        if not bands or row - bands[-1][-1] > max(2, round(2 * scale)):
            bands.append([int(row)])
        else:
            bands[-1].append(int(row))
    return [{"x": (x0 + int(np.flatnonzero(mask[band].any(axis=0))[0])) / scale,
             "y": (y0 + band[0]) / scale,
             "w": (int(np.flatnonzero(mask[band].any(axis=0))[-1]) - int(np.flatnonzero(mask[band].any(axis=0))[0]) + 1) / scale,
             "h": (band[-1] - band[0] + 1) / scale} for band in bands]


def bidi_checks(case, source):
    """Compare native-shaper character order with original DOMRange positions.

    This diagnostic supplements the actual component PNGs; it is not a claim of
    identical glyph rasterization. Logical labels retain their accessible order.
    Wrapped paragraphs have a separate foreground-line check.
    """
    checks = []
    targets = []
    for target in source:
        if target["line_count"] == 1:
            targets.append(target)
            continue
        lines, offset = {}, 0
        for character in target["characters"]:
            start = offset
            offset += len(character["character"].encode("utf-8"))
            if character["character"].isspace():
                continue
            key = round(character["y"] * 64)
            line = lines.setdefault(key, dict(start=start, end=offset, characters=[]))
            line["end"] = offset
            line["characters"].append(character)
        for line in lines.values():
            targets.append(dict(target, text=target["text"].encode("utf-8")[line["start"]:line["end"]].decode("utf-8"),
                                characters=line["characters"], line_range=[line["start"], line["end"]]))
    for target in targets:
        digits = [character for character in target["characters"] if character["character"].isdigit()]
        letters = [character for character in target["characters"] if character["character"].isalpha()]
        if not digits or not letters:
            continue
        candidates = case.get("wrapped_bidi_probe" if "line_range" in target else "bidi_probe", [])
        probe = next((probe for probe in candidates if probe["selector"] == target["selector"]
                      and probe.get("range") == target.get("line_range")), None)
        check = dict(selector=target["selector"], evidence="original DOMRange versus native paragraph-shaper character positions", passed=False)
        if "line_range" in target:
            check["original_line_range"] = target["line_range"]
            check["evidence"] = "original DOMRange line range versus production paragraph-context compositor (supplementary)"
        if probe is None or probe["logical_text"] != target["text"]:
            checks.append(dict(check, reason="Native shaping probe missing or logical labels differ"))
            continue
        glyphs = {glyph["index"]: glyph for glyph in probe["glyphs"]}
        # Keep the identity of the first logical number/word when the platform
        # needs visual runs before shaping. E.g. "51-60 of 500 items" must compare
        # the range token 51-60 even when the total 500 is painted farther left.
        numeric_token = re.search(r"\d[\d-]*", probe["logical_text"]).group()
        word_token = re.search(r"[^\W\d_]+", probe["logical_text"]).group()
        def token_glyph(token):
            character_index = probe["shaped_text"].find(token)
            return None if character_index < 0 else glyphs.get(len(probe["shaped_text"][:character_index].encode("utf-8")))
        number, word = token_glyph(numeric_token), token_glyph(word_token)
        if number is None or word is None:
            checks.append(dict(check, reason="Native character cluster unavailable"))
            continue
        source_dx, native_dx = letters[0]["x"] - digits[0]["x"], word["x"] - number["x"]
        checks.append(dict(check, original_word_minus_numeric_x=source_dx,
                           native_word_minus_numeric_x=native_dx,
                           passed=source_dx * native_dx > 0))
    return checks


def ua_targets(case, reference_image, scale):
    """Chromium's native form appearance paints independently of computed CSS.

    Read the actual neutral fill/stroke from the original pixels. For example,
    the default button reports a 2px black border but paints a snapped 1px gray
    outline with rounded corners. Treating computed CSS as painted truth would
    reward the wrong appearance.
    """
    for element in case["metrics"]["elements"]:
        if element["selector"] != ".ant-pagination-options-quick-jumper-button":
            continue
        x, y = round(element["x"] * scale), round(element["y"] * scale)
        width, height = round(element["w"] * scale), round(element["h"] * scale)
        roi = reference_image[y:y + height, x:x + width]
        inset = max(2, round(2 * scale))
        inner = roi[inset:-inset, inset:-inset]
        fill = Counter(map(tuple, inner.reshape(-1, 3))).most_common(1)[0][0]
        outside = tuple(reference_image[max(0, y - 2), x + width // 2])
        edge = np.concatenate((roi[:inset, inset:-inset].reshape(-1, 3),
                               roi[-inset:, inset:-inset].reshape(-1, 3),
                               roi[inset:-inset, :inset].reshape(-1, 3),
                               roi[inset:-inset, -inset:].reshape(-1, 3)))
        candidates = [tuple(pixel) for pixel in edge if tuple(pixel) not in (fill, outside) and pixel[0] == pixel[1] == pixel[2]]
        if not candidates:
            raise ValueError("Unable to measure the original native button outline")
        stroke = Counter(candidates).most_common(1)[0][0]
        top = reference_image[max(0, y - 1):y + inset + 2, x + width // 2]
        physical_width = sum(tuple(pixel) == stroke for pixel in top)
        if not physical_width:
            raise ValueError("Unable to measure the original native button outline width")
        yield dict(element, channel="color", color=tuple(value / 255 for value in fill) + (1.,), source="original painted pixels")
        bands = []
        if element.get("disabled") is True:
            for pixel in reference_image[y:y + inset + 2, x + width // 2]:
                color = tuple(int(value) for value in pixel)
                if color in (fill, outside):
                    continue
                if bands and bands[-1]["rgb"] == list(color):
                    bands[-1]["physical_width"] += 1
                else:
                    bands.append(dict(rgb=list(color), physical_width=1))
        if bands:
            physical_width = sum(band["physical_width"] for band in bands)
        yield dict(element, channel="stroke", color=tuple(value / 255 for value in stroke) + (1.,),
                   border_widths=[physical_width / scale] * 4, outline_bands=bands, source="original painted pixels")


def form_label_contrast(image, bounds, fill, scale):
    # Exclude the rounded outline and retain the glyph-bearing content region.
    x0, y0 = round((bounds["x"] + 3) * scale), round((bounds["y"] + 3) * scale)
    x1, y1 = round((bounds["x"] + bounds["w"] - 3) * scale), round((bounds["y"] + bounds["h"] - 3) * scale)
    pixels = image[y0:y1, x0:x1]
    return float(np.max(np.abs(pixels - np.array(fill[:3]) * 255))) if pixels.size else 0.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-subset", action="store_true", help="Allow a deliberate native smoke subset")
    parser.add_argument("--text-reference", type=Path, help="Original supplemental DOMRange evidence directory")
    args = parser.parse_args()
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8-sig"))
    reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8-sig"))
    text_directory = args.text_reference or args.reference.parent / args.reference.name.replace("visual-", "text-")
    text_evidence = json.loads((text_directory / "text-metrics.json").read_text(encoding="utf-8-sig"))
    text_reference = {case["scenario"]["name"]: case for case in text_evidence["cases"]}
    form_evidence = json.loads((Path(__file__).resolve().parent.parent / "fixtures/pagination/native-form-reference.json").read_text(encoding="utf-8"))
    form_reference = {record["scale"]: record for record in form_evidence["scales"]}
    browser = {case["scenario"]["name"]: case for case in reference["cases"]}
    if not native or len({case["scenario"]["name"] for case in native}) != len(native):
        raise ValueError("Native captures must be nonempty and unique")
    if len(browser) != len(reference["cases"]):
        raise ValueError("Browser scenario names must be unique")
    if not args.allow_subset and {case["scenario"]["name"] for case in native} != browser.keys():
        raise ValueError("Native and browser capture sets differ")
    results = []
    for case in native:
        name, scale = case["scenario"]["name"], case["scale"]
        other = browser[name]
        if case["scenario"] != other["scenario"] or other["metrics"]["scale"] != scale:
            raise ValueError(f"Scenario or scale differs: {name}")
        quads = [dict(quad, color=native_color(quad["background"]), stroke=native_color(quad.get("border_color", ""))) for quad in case["quads"]]
        a = np.array(Image.open(args.native / f"{name}.png").convert("RGB"), dtype=np.int16)
        b = np.array(Image.open(args.reference / f"{name}.png").convert("RGB"), dtype=np.int16)
        if a.shape != b.shape:
            raise ValueError(f"Image sizes differ for {name}: {a.shape} vs {b.shape}")
        checks, visibility_checks, translucency_checks = [], [], []
        for target in [*targets(other), *ua_targets(other, b, scale)]:
            if "source" in target:
                # A native form control can paint translucent disabled layers.
                # Compare actual composited PNG colors on both sides; a raw
                # RGBA scene color is not an opaque browser screenshot color.
                candidates = [quad for quad in quads if quad["stroke"] and quad["stroke"][3] > 0
                              and max(quad.get("border_widths", [0])) > 0]
                matching = []
            else:
                matching = [quad for quad in quads if quad[target["channel"]] and
                            max(abs(quad[target["channel"]][i] - target["color"][i]) for i in range(3)) <= 1 / 255 + 1e-6 and
                            abs(quad[target["channel"]][3] - target["color"][3]) <= .001]
                candidates = matching or [quad for quad in quads if quad[target["channel"]]]
            if not candidates:
                checks.append(dict(selector=target["selector"], passed=False, reason="No painted surface"))
                continue
            closest = min(candidates, key=lambda quad: sum(abs(quad[key] - target[key]) for key in ("x", "y", "w", "h")))
            delta = {key: closest[key] - target[key] for key in ("x", "y", "w", "h")}
            native_paint = None
            if "source" in target:
                proxy = dict(other, metrics=dict(other["metrics"], elements=[dict(target, **{key: closest[key] for key in ("x", "y", "w", "h")})]))
                native_paint = next(paint for paint in ua_targets(proxy, a, scale) if paint["channel"] == target["channel"])
                matching = [closest] if max(abs(native_paint["color"][i] - target["color"][i]) for i in range(3)) <= 1 / 255 + 1e-6 else []
                if target.get("outline_bands"):
                    original_bands, native_bands = target["outline_bands"], native_paint["outline_bands"]
                    matching = [closest] if len(original_bands) == len(native_bands) and all(
                        max(abs(left - right) for left, right in zip(original["rgb"], native["rgb"])) <= 1
                        and abs(original["physical_width"] - native["physical_width"]) / scale <= 1
                        for original, native in zip(original_bands, native_bands)) else []
                if target["channel"] == "color":
                    reference_contrast = form_label_contrast(b, target, target["color"], scale)
                    native_contrast = form_label_contrast(a, closest, native_paint["color"], scale)
                    allowance = max(8., reference_contrast * .25)
                    visibility_checks.append(dict(evidence="actual PNG form-label contrast against its painted fill; font antialiasing tolerance",
                                                  reference_contrast=reference_contrast, native_contrast=native_contrast,
                                                  contrast_allowance=allowance,
                                                  passed=reference_contrast > 0 and abs(native_contrast - reference_contrast) <= allowance))
                    if target.get("disabled") is True:
                        reference_fill = form_reference[scale]["two_point_equivalent_fill"]
                        reference_outline = form_reference[scale]["two_point_equivalent_border"]
                        if scale >= 2:
                            # The separately resolved outer/inner bands expose
                            # the CSS border RGBA directly. Its actual pixels
                            # over five backdrops validate this model; the
                            # quantized two-point fit is not a unique raw alpha.
                            rgba = form_reference[scale]["edge_band_model"]["css_rgba"]
                            reference_outline = dict(gray=rgba[0], alpha=rgba[3])
                        fills = [quad for quad in quads if quad["color"] and 0 < quad["color"][3] < 1
                                 and quad["x"] >= closest["x"] and quad["y"] >= closest["y"]
                                 and quad["x"] + quad["w"] <= closest["x"] + closest["w"] + .001
                                 and quad["y"] + quad["h"] <= closest["y"] + closest["h"] + .001]
                        fill = min(fills, key=lambda quad: abs(quad["color"][3] - reference_fill["alpha"]), default=None)
                        fill_rgb_delta = None if fill is None else max(abs(channel * 255 - reference_fill["gray"]) for channel in fill["color"][:3])
                        fill_alpha_delta = None if fill is None else abs(fill["color"][3] - reference_fill["alpha"])
                        outline_rgb_delta = max(abs(channel * 255 - reference_outline["gray"]) for channel in closest["stroke"][:3])
                        outline_alpha_delta = abs(closest["stroke"][3] - reference_outline["alpha"])
                        translucency_checks.append(dict(evidence="native raw RGBA versus five-background-validated CSS border bands" if scale >= 2 else "native raw RGBA versus actual five-background browser collapsed paint fit",
                                                        fill_rgb_delta_bytes=fill_rgb_delta, fill_alpha_delta=fill_alpha_delta,
                                                        outline_rgb_delta_bytes=outline_rgb_delta, outline_alpha_delta=outline_alpha_delta,
                                                        passed=fill is not None and max(fill_rgb_delta, outline_rgb_delta) <= 1.0001
                                                        and max(fill_alpha_delta, outline_alpha_delta) <= .001))
            if "border_widths" in target:
                widths = native_paint["border_widths"] if native_paint is not None else closest.get("border_widths", [0] * 4)
                delta["border_width"] = max(abs(a - b) for a, b in zip(widths, target["border_widths"]))
            # CSS caps a uniform radius at half the shortest box side. Preserve
            # that same effective geometry for large semantic radii (e.g. 999px).
            # Chromium's UA Go button appearance is pixel-derived separately.
            radius = re.fullmatch(r"([\d.]+)px", target.get("radius", ""))
            if radius and "source" not in target:
                expected_radius = min(float(radius.group(1)), target["w"] / 2, target["h"] / 2)
                native_radii = [min(value, closest["w"] / 2, closest["h"] / 2)
                                for value in closest.get("radii", [0] * 4)]
                delta["corner_radius"] = max(abs(value - expected_radius) for value in native_radii)
            checks.append(dict(selector=target["selector"], text=target["text"], channel=target["channel"],
                               evidence=target.get("source", "computed style and DOM geometry"),
                               color_comparison="ordered actual edge-band colors" if target.get("outline_bands") else "actual composited PNG color" if native_paint is not None else "raw scene RGBA",
                               native_painted_rgb=None if native_paint is None else [round(value * 255) for value in native_paint["color"][:3]],
                               reference_painted_rgb=None if native_paint is None else [round(value * 255) for value in target["color"][:3]],
                               native_outline_bands=None if native_paint is None else native_paint.get("outline_bands"),
                               reference_outline_bands=target.get("outline_bands"),
                               target={key: target[key] for key in ("x", "y", "w", "h")},
                               geometry_delta=delta, color_matches=bool(matching),
                               passed=bool(matching) and max(map(abs, delta.values())) <= 1.0001))
        roi_a = a[int(24 * scale):int(390 * scale), int(32 * scale):-int(32 * scale)]
        roi_b = b[int(24 * scale):int(390 * scale), int(32 * scale):-int(32 * scale)]
        difference = np.abs(roi_a - roi_b)
        # A second residual over painted foreground avoids allowing the blank
        # fixture canvas to dominate the descriptive pixel-error statistic.
        background_a, background_b = a[round(5 * scale), round(5 * scale)], b[round(5 * scale), round(5 * scale)]
        foreground = (np.max(np.abs(roi_a - background_a), axis=2) > 16) | (np.max(np.abs(roi_b - background_b), axis=2) > 16)
        reference_ink, native_ink = total_ink(other, b, scale), total_ink(other, a, scale)
        text_checks = [] if reference_ink is None else [dict(
            selector=".ant-pagination-total-text", evidence="original and native PNG foreground line bands",
            reference_lines=reference_ink, native_lines=native_ink,
            passed=len(reference_ink) == len(native_ink))]
        source_text = text_reference.get(name, {}).get("text", [])
        total = next((target for target in source_text if target["selector"] == ".ant-pagination-total-text"), None)
        if total and text_checks:
            text_checks[0]["original_dom_line_count"] = total["line_count"]
            text_checks[0]["passed"] &= total["line_count"] == len(reference_ink)
        order_checks = bidi_checks(case, source_text)
        custom_checks = []
        if case["scenario"]["kind"] == "custom":
            for element in other["metrics"]["elements"]:
                if element["selector"] == ".ant-pagination > li" and element["text"] in ("Previous", "Next"):
                    original_lines = total_ink(other, b, scale, element)
                    native_lines = total_ink(other, a, scale, element)
                    custom_checks.append(dict(text=element["text"], evidence="actual custom-label PNG foreground line bands",
                                              reference_lines=original_lines, native_lines=native_lines,
                                              passed=len(original_lines) == len(native_lines)))
        results.append(dict(name=name, checks=checks,
                            text_layout_checks=text_checks,
                            bidi_checks=order_checks,
                            custom_label_checks=custom_checks,
                            native_form_label_visibility_checks=visibility_checks,
                            native_form_translucency_checks=translucency_checks,
                            pixel_mean_absolute_channel_error=float(difference.mean()),
                            foreground_mean_absolute_channel_error=float(difference[foreground].mean()) if foreground.any() else 0.,
                            foreground_fraction_over_16=float((difference[foreground].max(axis=1) > 16).mean()) if foreground.any() else 0.,
                            pixel_fraction_over_16=float((difference.max(axis=2) > 16).mean())))
    checks = [check for result in results for check in result["checks"]]
    text_checks = [check for result in results for check in result["text_layout_checks"]]
    order_checks = [check for result in results for check in result["bidi_checks"]]
    custom_checks = [check for result in results for check in result["custom_label_checks"]]
    visibility_checks = [check for result in results for check in result["native_form_label_visibility_checks"]]
    translucency_checks = [check for result in results for check in result["native_form_translucency_checks"]]
    report = dict(cases=len(results), scale_factors=sorted({case["scale"] for case in native}),
                  renderer=reference["renderer"], surface_checks=len(checks),
                  passed_surface_checks=sum(check["passed"] for check in checks),
                  failed_surface_checks=sum(not check["passed"] for check in checks),
                  text_layout_checks=len(text_checks),
                  passed_text_layout_checks=sum(check["passed"] for check in text_checks),
                  failed_text_layout_checks=sum(not check["passed"] for check in text_checks),
                  bidi_checks=len(order_checks),
                  passed_bidi_checks=sum(check["passed"] for check in order_checks),
                  failed_bidi_checks=sum(not check["passed"] for check in order_checks),
                  custom_label_line_checks=len(custom_checks),
                  passed_custom_label_line_checks=sum(check["passed"] for check in custom_checks),
                  failed_custom_label_line_checks=sum(not check["passed"] for check in custom_checks),
                  native_form_label_visibility_checks=len(visibility_checks),
                  passed_native_form_label_visibility_checks=sum(check["passed"] for check in visibility_checks),
                  failed_native_form_label_visibility_checks=sum(not check["passed"] for check in visibility_checks),
                  native_form_translucency_checks=len(translucency_checks),
                  passed_native_form_translucency_checks=sum(check["passed"] for check in translucency_checks),
                  failed_native_form_translucency_checks=sum(not check["passed"] for check in translucency_checks),
                  excluded_from_surface_pass=["text rasterization", "SVG glyph rasterization", "shadows", "motion", "dashed root borders",
                                              "native form button corner radius (manual PNG review)"], results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "results"}, indent=2))
    if report["failed_surface_checks"] or report["failed_text_layout_checks"] or report["failed_bidi_checks"] or report["failed_custom_label_line_checks"] or report["failed_native_form_label_visibility_checks"] or report["failed_native_form_translucency_checks"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
