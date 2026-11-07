# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independent native and original Ant Design Menu captures.

Surfaces, root layouts and default-chevron bounds have a one-logical-pixel
geometry budget. Text, chevrons and shadows also appear in paired contact sheets
and unmasked pixel-error metrics.
"""
import argparse
import colorsys
import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def css_color(value):
    values = [float(v) for v in re.findall(r"[\d.]+", value)]
    return tuple(v / 255 for v in values[:3]) + (values[3] if len(values) == 4 else 1.,)


def native_color(value):
    if not value.startswith("Solid(Hsla"):
        return None
    values = dict((key, float(value)) for key, value in re.findall(r"([hsla]): ([\d.eE+-]+)", value))
    return colorsys.hls_to_rgb(values["h"], values["l"], values["s"]) + (values["a"],)


def chevron_bounds(pixels, box, background, threshold, scale):
    x0, y0, x1, y1 = box
    crop = pixels[y0:y1, x0:x1]
    foreground = np.max(np.abs(crop - background), axis=2) > threshold
    coordinates = np.argwhere(foreground)
    if not len(coordinates):
        return None
    return dict(left=float(x0 + coordinates[:, 1].min()) / scale,
                right=float(x0 + coordinates[:, 1].max() + 1) / scale,
                top=float(y0 + coordinates[:, 0].min()) / scale,
                bottom=float(y0 + coordinates[:, 0].max() + 1) / scale)


def targets(case):
    for element in case["metrics"]["elements"]:
        selector = element["selector"]
        color = css_color(element["background"])
        if color[3] > 0 and element["w"] > 0 and element["h"] > 0:
            yield dict(element, color=color)
        if element.get("outlineStyle") == "solid":
            width = float(element["outlineWidth"].removesuffix("px"))
            offset = float(element["outlineOffset"].removesuffix("px"))
            extent = width + offset
            if width > 0:
                yield dict(element, selector=selector + " focus outline", color=css_color(element["outlineColor"]),
                           stroke=True, border_width=width, x=element["x"] - extent,
                           y=element["y"] - extent, w=element["w"] + 2 * extent, h=element["h"] + 2 * extent)
        if selector == ".ant-menu-item" and case["scenario"].get("mode") == "horizontal":
            after = element["after"]
            widths = [float(value.removesuffix("px")) for value in after["borderWidth"].split()]
            bottom = widths[2] if len(widths) == 4 else widths[0]
            colors = re.findall(r"rgba?\([^)]+\)", after["borderColor"])
            bar_color = css_color(colors[2] if len(colors) == 4 else colors[0])
            if bottom > 0 and bar_color[3] > 0:
                inset = float(after["insetInline"].split()[0].removesuffix("px"))
                yield dict(element, selector=selector + " active bar", x=element["x"] + inset,
                           y=element["y"] + element["h"] - bottom, w=element["w"] - 2 * inset,
                           h=bottom, color=bar_color)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8-sig"))
    reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8-sig"))
    browser = {case["scenario"]["name"]: case for case in reference["cases"]}
    if {case["scenario"]["name"] for case in native} != browser.keys():
        raise ValueError("Native and browser case sets differ")
    results = []
    for case in native:
        name, scale = case["scenario"]["name"], case["scale"]
        other = browser[name]
        if case["scenario"] != other["scenario"] or other["metrics"]["scale"] != scale:
            raise ValueError(f"{name}: scenario or scale differs")
        quads = [dict(quad, color=native_color(quad["background"]), stroke=native_color(quad["border_color"])) for quad in case["quads"]]
        checks = []
        if case.get("root_bounds") is not None:
            root = next(element for element in other["metrics"]["elements"] if element["selector"] == ".ant-menu-root")
            delta = {key:case["root_bounds"][key] - root[key] for key in ("x", "y", "w", "h")}
            checks.append(dict(selector=".ant-menu-root layout",kind="layout",text=root["text"],geometry_delta=delta,
                               passed=max(map(abs,delta.values())) <= 1.0001))
        for target in targets(other):
            channel = "stroke" if target.get("stroke") else "color"
            matching = [quad for quad in quads if quad[channel] and
                        max(abs(quad[channel][i] - target["color"][i]) for i in range(3)) <= 1 / 255 + 1e-6 and
                        abs(quad[channel][3] - target["color"][3]) <= .001]
            candidates = matching or [quad for quad in quads if quad[channel]]
            if not candidates:
                checks.append(dict(selector=target["selector"], passed=False, reason="No native painted surface"))
                continue
            closest = min(candidates, key=lambda quad: sum(abs(quad[key] - target[key]) for key in ("x", "y", "w", "h")))
            delta = {key: closest[key] - target[key] for key in ("x", "y", "w", "h")}
            if target.get("stroke"):
                delta["border_width"] = max(abs(width - target["border_width"]) for width in closest["border_widths"])
            checks.append(dict(selector=target["selector"], text=target["text"], geometry_delta=delta,
                               color_matches=bool(matching), passed=bool(matching) and max(map(abs, delta.values())) <= 1.0001))
        foregrounds = [dict(sprite, resolved=native_color(sprite["color"])) for sprite in case.get("foregrounds", [])]
        for element in other["metrics"]["elements"]:
            if element["selector"] not in (".ant-menu-item", ".ant-menu-submenu-title"):
                continue
            # SVG-only indicators have no text glyphs. Their paint is assessed
            # in screenshots; monochrome text color checks require a text slot.
            if not element["text"].strip():
                continue
            expected = css_color(element["color"])
            candidates = [sprite for sprite in foregrounds if sprite["resolved"] and
                          element["x"] <= sprite["x"] + sprite["w"] / 2 <= element["x"] + element["w"] and
                          element["y"] <= sprite["y"] + sprite["h"] / 2 <= element["y"] + element["h"]]
            matching = [sprite for sprite in candidates if
                        max(abs(sprite["resolved"][i] - expected[i]) for i in range(3)) <= 1 / 255 + 1e-6 and
                        # SVG icon tints pass through an 8-bit RGBA texture;
                        # compare the same alpha quantization as its CSS paint.
                        round(sprite["resolved"][3] * 255) == round(expected[3] * 255)]
            checks.append(dict(selector=element["selector"] + " foreground", kind="foreground", text=element["text"],
                               color_matches=bool(matching), passed=bool(matching)))
        native_image = Image.open(args.native / f"{name}.png").convert("RGB")
        browser_image = Image.open(args.reference / f"{name}.png").convert("RGB")
        a, b = np.asarray(native_image, dtype=np.int16), np.asarray(browser_image, dtype=np.int16)
        if a.shape != b.shape:
            raise ValueError(f"{name}: image sizes differ")
        for element in other["metrics"]["elements"] + other["metrics"].get("customExpandSlots", []):
            arrow = element.get("arrow") or element.get("customExpand")
            if not arrow:
                continue
            kind = "custom_expand" if element.get("customExpand") else "chevron"
            # Source CSS arrow slots have zero height. Keep a generous ROI
            # around that measured slot, clear of the separately padded label.
            radius = max(4., arrow["w"] * 1.3)
            center_x, center_y = arrow["x"] + arrow["w"] / 2, arrow["y"] + arrow["h"] / 2
            box = (max(0, math.floor((center_x - radius) * scale)),
                   max(0, math.floor((center_y - radius) * scale)),
                   min(b.shape[1], math.ceil((center_x + radius) * scale)),
                   min(b.shape[0], math.ceil((center_y + radius) * scale)))
            x0, y0, x1, y1 = box
            reference_crop = b[y0:y1, x0:x1]
            colors, counts = np.unique(reference_crop.reshape(-1, 3), axis=0, return_counts=True)
            background = colors[counts.argmax()]
            contrast = np.max(np.abs(reference_crop - background))
            if contrast < 16:
                checks.append(dict(selector=element["selector"] + " painted bounds", kind=kind,
                                   text=element["text"], passed=False,
                                   reason="Visible reference indicator slot contains no painted stroke; inspect or recapture the reference frame"))
                continue
            # Include real antialiased stroke support. At 100% DPI one valid
            # capsule edge has 42% coverage in GPUI versus 61% in Chromium;
            # a 45% cutoff moved that measured edge by an extra pixel.
            # The geometry budget remains one logical pixel.
            threshold = max(12., contrast * .30)
            expected = chevron_bounds(b, box, background, threshold, scale)
            actual = chevron_bounds(a, box, background, threshold, scale)
            delta = None if actual is None else {edge: actual[edge] - expected[edge] for edge in expected}
            checks.append(dict(selector=element["selector"] + " painted bounds",kind=kind,text=element["text"],
                               reference_bounds=expected,native_bounds=actual,geometry_delta=delta,
                               passed=delta is not None and max(map(abs,delta.values())) <= 1.0001))
        # Fixed component region; no masking text, arrows, shadows or differences.
        roi = (slice(int(32 * scale), int(480 * scale)), slice(int(48 * scale), int(780 * scale)))
        difference = np.abs(a[roi] - b[roi])
        results.append(dict(name=name, checks=checks, pixel_mean_absolute_channel_error=float(difference.mean()),
                            pixel_fraction_over_16=float((difference.max(axis=2) > 16).mean())))
    checks = [check for result in results for check in result["checks"]]
    surfaces = [check for check in checks if check.get("kind") not in ("foreground", "layout", "chevron", "custom_expand")]
    foregrounds = [check for check in checks if check.get("kind") == "foreground"]
    layouts = [check for check in checks if check.get("kind") == "layout"]
    chevrons = [check for check in checks if check.get("kind") == "chevron"]
    custom_expand = [check for check in checks if check.get("kind") == "custom_expand"]
    report = dict(cases=len(results), scale_factors=sorted({case["scale"] for case in native}),
                  renderer=reference["renderer"], surface_checks=len(surfaces),
                  logical_pixel_geometry_budget=1, chevron_foreground_contrast_fraction=.30,
                  chevron_minimum_channel_contrast=12,
                  passed_surface_checks=sum(check["passed"] for check in surfaces),
                  failed_surface_checks=sum(not check["passed"] for check in surfaces),
                  foreground_color_checks=len(foregrounds), passed_foreground_color_checks=sum(check["passed"] for check in foregrounds),
                  failed_foreground_color_checks=sum(not check["passed"] for check in foregrounds),
                  root_layout_checks=len(layouts),passed_root_layout_checks=sum(check["passed"] for check in layouts),
                  failed_root_layout_checks=sum(not check["passed"] for check in layouts),
                  chevron_bounds_checks=len(chevrons),passed_chevron_bounds_checks=sum(check["passed"] for check in chevrons),
                  failed_chevron_bounds_checks=sum(not check["passed"] for check in chevrons),
                  custom_expand_bounds_checks=len(custom_expand),
                  passed_custom_expand_bounds_checks=sum(check["passed"] for check in custom_expand),
                  failed_custom_expand_bounds_checks=sum(not check["passed"] for check in custom_expand),
                  excluded_from_surface_pass=["text rasterization", "chevron antialiasing", "shadows", "motion", "transparent layout elements"], results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    # Paired full-menu crops for human review, avoiding a whole-page thumbnail.
    for start in range(0, len(results), 8):
        group = results[start:start + 8]
        sheet = Image.new("RGB", (1000, 280 * len(group)), "#eeeeee")
        draw = ImageDraw.Draw(sheet)
        for index, result in enumerate(group):
            name = result["name"]
            draw.text((8, index * 280 + 4), name + "   native | original", fill="black")
            for column, directory in enumerate([args.native, args.reference]):
                image = Image.open(directory / f"{name}.png").convert("RGB")
                image = image.crop((48 * scale, 32 * scale, 780 * scale, 480 * scale))
                image.thumbnail((490, 254), Image.Resampling.LANCZOS)
                sheet.paste(image, (column * 500 + 5, index * 280 + 24))
        sheet.save(args.output.parent / f"pairs-{start // 8 + 1}.png")
    print(json.dumps({key: value for key, value in report.items() if key != "results"}, indent=2))
    if report["failed_surface_checks"] or report["failed_foreground_color_checks"] or report["failed_root_layout_checks"] or report["failed_chevron_bounds_checks"] or report["failed_custom_expand_bounds_checks"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
