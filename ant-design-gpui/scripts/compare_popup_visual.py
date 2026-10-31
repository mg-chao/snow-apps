# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Report native/browser differences without hiding font rasterization differences.

Requires Pillow and NumPy. Run after popup_visual and capture_popup_visual.ps1.
The report is evidence for review, not a substitute for inspecting screenshots.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def rectangle_mask(shape, box, scale, pad=0):
    yy, xx = np.ogrid[:shape[0], :shape[1]]
    return ((xx >= (box["x"] - pad) * scale)
            & (xx < (box["x"] + box["width"] + pad) * scale)
            & (yy >= (box["y"] - pad) * scale)
            & (yy < (box["y"] + box["height"] + pad) * scale))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    native = json.loads((args.native / "native-metrics.json").read_text(encoding="utf-8"))
    reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8"))
    browser = {case["name"]: case for case in reference["cases"]}
    results = []
    for case in native:
        name = case["name"]
        other = browser[name]
        a = np.asarray(Image.open(args.native / f"{name}.png").convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(args.reference / f"{name}.png").convert("RGB"), dtype=np.int16)
        if a.shape != b.shape:
            raise ValueError(f"{name}: image sizes differ: {a.shape} vs {b.shape}")
        if case["scale"] != other["scale"]:
            raise ValueError(f"{name}: DPI differs")
        scale = case["scale"]
        difference = np.abs(a - b)
        # Limit statistics to the surface and its nearby shadow. Whole-window
        # averages would be diluted by the large, identical blank background.
        roi = rectangle_mask(a.shape, case["body"], scale, 48) | rectangle_mask(a.shape, other["body"], scale, 48)
        surface = rectangle_mask(a.shape, case["body"], scale, 10) | rectangle_mask(a.shape, other["body"], scale, 10)
        anchor = case.get("anchor", [360, 234])
        trigger = rectangle_mask(a.shape, dict(x=anchor[0], y=anchor[1], width=80, height=32), scale, 4)
        shadow = roi & ~surface & ~trigger
        results.append(dict(
            name=name,
            bounds_delta={key: case["body"][key] - other["body"][key] for key in ("x", "y", "width", "height")},
            roi_mean_absolute_channel_error=float(difference[roi].mean()),
            roi_pixels_above_16=float((difference.max(axis=2)[roi] > 16).mean()),
            shadow_mean_absolute_channel_error=float(difference[shadow].mean()),
            shadow_max_channel_error=int(difference[shadow].max()),
        ))
    report = dict(
        cases=len(results),
        scale_factors=sorted({case["scale"] for case in native}),
        renderer=reference["renderer"],
        maximum_absolute_bounds_delta={key: max(abs(case["bounds_delta"][key]) for case in results) for key in ("x", "y", "width", "height")},
        maximum_roi_mean_error=max(case["roi_mean_absolute_channel_error"] for case in results),
        maximum_shadow_mean_error=max(case["shadow_mean_absolute_channel_error"] for case in results),
        results=results,
    )
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "results"}, indent=2))


if __name__ == "__main__":
    main()
