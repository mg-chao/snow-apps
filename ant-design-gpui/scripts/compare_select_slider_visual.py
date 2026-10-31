# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare independently captured native primitives with browser geometry/colors.

Non-text surfaces have a one-logical-pixel geometry budget. Color comparison
allows one byte of RGB rounding and 0.001 alpha rounding. Pixel differences are
reported separately, including text/shadows; they are not hidden by a whole-page
average or used as a substitute for inspecting the paired images.
"""
import argparse
import colorsys
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image


def css_color(value):
    values = [float(v) for v in re.findall(r"[\d.]+", value)]
    return tuple(v / 255 for v in values[:3]) + (values[3] if len(values) == 4 else 1.,)


def native_color(value):
    if not value.startswith("Solid(Hsla"):
        return None
    values = dict((key, float(value)) for key, value in re.findall(r"([hsla]): ([\d.eE+-]+)", value))
    return colorsys.hls_to_rgb(values["h"], values["l"], values["s"]) + (values["a"],)


def edges(values):
    if len(values) == 1:
        return values * 4
    if len(values) == 2:
        return values * 2
    if len(values) == 3:
        return values + [values[1]]
    return values


def targets(case):
    elements = case["metrics"]["elements"]
    # The HTML host creates an inline formatting strut around small Selects.
    # GPUI uses block/flex layout. Compare the component and its popup relative
    # to the common fixture anchor, preserving all internal geometry/dimensions.
    root = next((e for e in elements if e["selector"] == ".ant-select"), None)
    offset_x = root["x"] - case.get("scenario", {}).get("anchor_x", 64) if root else 0
    offset_y = root["y"] - case.get("scenario", {}).get("anchor_y", 48) if root else 0
    for element in elements:
        element = dict(element, x=element["x"] - offset_x, y=element["y"] - offset_y)
        selector = element["selector"]
        if selector in [".ant-select", ".ant-select-dropdown", ".ant-select-item-option",
                        ".ant-slider-rail", ".ant-slider-track", ".ant-slider-dot"]:
            color = css_color(element["background"])
            if color[3] > 0:
                target = dict(element, color=color)
                if selector == ".ant-slider-track":
                    # Draggable tracks use transparent borders to enlarge the
                    # hit target; background-clip paints only the content box.
                    borders = [float(v.removesuffix("px")) for v in element["borderWidth"].split()]
                    if len(borders) == 2:
                        top, right = borders
                        target.update(x=target["x"] + right, y=target["y"] + top,
                                      w=target["w"] - 2 * right, h=target["h"] - 2 * top)
                yield target
        elif selector == ".ant-slider-handle":
            after = element["after"]
            width, height = float(after["w"].removesuffix("px")), float(after["h"].removesuffix("px"))
            yield dict(element, x=element["x"] + (element["w"] - width) / 2,
                       y=element["y"] + (element["h"] - height) / 2, w=width, h=height,
                       color=css_color(after["background"]))
            shadow = re.fullmatch(r"(rgba?\([^)]+\)) 0px 0px 0px ([\d.]+)px", after["boxShadow"])
            if shadow:
                spread = float(shadow[2])
                yield dict(element, selector=selector + " ring", x=element["x"] + (element["w"] - width) / 2 - spread,
                           y=element["y"] + (element["h"] - height) / 2 - spread,
                           w=width + 2 * spread, h=height + 2 * spread, color=css_color(shadow[1]))
        if selector in [".ant-select", ".ant-slider-dot"]:
            widths = edges([float(v.removesuffix("px")) for v in element["borderWidth"].split()])
            colors = edges([css_color(v) for v in re.findall(r"rgba?\([^)]+\)", element["border"])])
            visible = [color for color, width in zip(colors, widths) if width > 0 and color[3] > 0]
            if visible:
                yield dict(element, selector=selector + " border", color=visible[0], border_widths=widths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8-sig"))
    reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8-sig"))
    browser = {c["scenario"]["name"]: c for c in reference["cases"]}
    native_names = {c["scenario"]["name"] for c in native}
    if not native or len(native_names) != len(native) or len(browser) != len(reference["cases"]):
        raise ValueError("Capture sets must be nonempty and have unique scenario names")
    if native_names != browser.keys():
        raise ValueError("Native and browser capture sets differ")
    results = []
    for case in native:
        name, scale = case["scenario"]["name"], case["scale"]
        other = browser[name]
        if case["scenario"] != other["scenario"]:
            raise ValueError(f"{name}: scenario configurations differ")
        select_root = next((e for e in other["metrics"]["elements"] if e["selector"] == ".ant-select"), None)
        if other["metrics"]["scale"] != scale:
            raise ValueError(f"{name}: scale factors differ")
        quads = [dict(q, color=native_color(q["background"]), stroke=native_color(q.get("border_color", ""))) for q in case["quads"]]
        checks = []
        for target in targets(other):
            # Match the same resolved fill first; this avoids mistaking a handle
            # ring for its smaller white face, or a track for the full-width rail.
            channel = "stroke" if "border_widths" in target else "color"
            matching = [q for q in quads if q[channel] and
                        max(abs(q[channel][i] - target["color"][i]) for i in range(3)) <= 1 / 255 + 1e-6 and
                        abs(q[channel][3] - target["color"][3]) <= .001]
            candidates = matching or [q for q in quads if q["color"]]
            if not candidates:
                checks.append(dict(selector=target["selector"], passed=False, reason="No painted surface"))
                continue
            closest = min(candidates, key=lambda q: sum(abs(q[k] - target[k]) for k in ("x", "y", "w", "h")))
            delta = {k: closest[k] - target[k] for k in ("x", "y", "w", "h")}
            if "border_widths" in target:
                delta["border_width"] = max(abs(a - b) for a, b in zip(closest.get("border_widths", [0] * 4), target["border_widths"]))
            checks.append(dict(selector=target["selector"], text=target["text"],
                               geometry_delta=delta, color_matches=bool(matching),
                               passed=bool(matching) and max(map(abs, delta.values())) <= 1.0001))
        a = np.array(Image.open(args.native / f"{name}.png").convert("RGB"), dtype=np.int16)
        b = np.array(Image.open(args.reference / f"{name}.png").convert("RGB"), dtype=np.int16)
        if a.shape != b.shape:
            raise ValueError(f"{name}: image sizes differ: {a.shape}, {b.shape}")
        # A fixed component-sized region includes popup shadows and tooltips but
        # excludes the reference harness's event log below the capture area.
        roi_a = a[:int(390 * scale), :int(420 * scale)]
        roi_b = b[:int(390 * scale), :int(420 * scale)]
        difference = np.abs(roi_a - roi_b)
        results.append(dict(name=name, checks=checks,
                            reference_inline_host_offset=None if not select_root else {
                                "x": select_root["x"] - other["scenario"].get("anchor_x", 64),
                                "y": select_root["y"] - other["scenario"].get("anchor_y", 48)},
                            pixel_mean_absolute_channel_error=float(difference.mean()),
                            pixel_fraction_over_16=float((difference.max(axis=2) > 16).mean())))
    checks = [check for result in results for check in result["checks"]]
    report = dict(cases=len(results), scale_factors=sorted({c["scale"] for c in native}),
                  renderer=reference["renderer"], surface_checks=len(checks),
                  passed_surface_checks=sum(c["passed"] for c in checks),
                  failed_surface_checks=sum(not c["passed"] for c in checks),
                  excluded_from_surface_pass=["text rasterization", "text-dependent tag widths", "shadows", "motion", "transparent layout-only elements"], results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "results"}, indent=2))
    if report["failed_surface_checks"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
