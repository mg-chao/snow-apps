# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Detect stale/tiled browser raster output outside measured Pagination bounds."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = []
    for label in (100, 150, 200):
        directory = args.reference / f"visual-{label}"
        captures = json.loads((directory / "browser-metrics.json").read_text(encoding="utf-8-sig"))
        for case in captures["cases"]:
            scale = case["metrics"]["scale"]
            name = case["scenario"]["name"]
            image = np.array(Image.open(directory / f"{name}.png").convert("RGB"), dtype=np.int16)
            foreground = np.max(np.abs(image - image[round(5 * scale), round(5 * scale)]), axis=2) > 24
            allowed = np.zeros(foreground.shape, dtype=bool)
            # Twenty-four logical pixels covers ordinary popup/focus shadows,
            # antialiasing, and inline font overflow. It does not change any
            # component geometry/color comparison budget.
            for element in case["metrics"]["elements"]:
                if element["selector"] not in (".ant-pagination", ".ant-select-dropdown") or element["display"] == "none" or not element["w"]:
                    continue
                x0, y0 = [max(0, int((element[key] - 24) * scale)) for key in ("x", "y")]
                x1 = min(image.shape[1], round((element["x"] + element["w"] + 24) * scale))
                y1 = min(image.shape[0], round((element["y"] + element["h"] + 24) * scale))
                allowed[y0:y1, x0:x1] = True
            unexpected = foreground & ~allowed
            rows, columns = np.nonzero(unexpected)
            results.append(dict(name=name, scale=scale, unexpected_foreground_pixels=len(rows),
                                unexpected_bounds=None if not len(rows) else dict(
                                    x=float(columns.min() / scale), y=float(rows.min() / scale),
                                    right=float((columns.max() + 1) / scale), bottom=float((rows.max() + 1) / scale))))
    report = dict(cases=len(results), shadow_and_inline_overflow_allowance_logical_px=24,
                  corrupt_cases=sum(bool(result["unexpected_foreground_pixels"]) for result in results), results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "results"}, indent=2))
    if report["corrupt_cases"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
