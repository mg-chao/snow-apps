# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Preserve independently observed scrollbar ink for offline native checks."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image
from compare_date_time_picker_visual import ink_components, scrollbar_paint


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("../build/ant-design-gpui/date-time-picker-reference/browser-capture"))
    parser.add_argument("--output", type=Path, default=Path("fixtures/date_time_picker/scrollbar-reference.json"))
    args = parser.parse_args()
    metrics_path = args.input / "browser-metrics.json"
    data = json.loads(metrics_path.read_text(encoding="utf-8-sig"))
    result = []
    for case in data["cases"]:
        bars = case["geometry"].get("scrollbars", [])
        if not bars:
            continue
        image_path = args.input / f"{case['name']}.png"
        image = np.asarray(Image.open(image_path).convert("RGB"))
        columns = []
        for bar in bars:
            mask, pixels, origin = scrollbar_paint(image, bar["track"], case["scale"], case["tokens"]["colorBgElevated"])
            columns.append({**bar, "viewport": case["geometry"]["timeViewports"][bar["column"]],
                            "painted_parts": ink_components(mask, pixels, origin, case["scale"])})
        result.append({"name": case["name"], "scene": case["scene"], "theme": case["theme"], "direction": case["direction"],
                       "scale": case["scale"], "options": case["options"], "image_sha256": digest(image_path), "columns": columns})
    if not result:
        raise ValueError("No independently captured scrollbar cases")
    report = {"schema_version": 1, "clock": data["clock"], "timezone": data["timezone"],
              "upstream": {key: data["provenance"][key] for key in ("version", "revision", "rc_picker")},
              "capture_sha256": digest(metrics_path), "generator_sha256": digest(Path(__file__)),
              "paint_reader_sha256": digest(Path(__file__).with_name("compare_date_time_picker_visual.py")),
              "foreground_rgb_distance_from_background": 16, "captures": data.get("captures"),
              "rail_ink_edge_margin_logical_px": 1,
              "capture_settings": data.get("capture_settings"), "probe_overrides": data.get("probe_overrides"), "cases": result}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Packed {len(result)} actual scrollbar scenes, {sum(len(c['columns']) for c in result)} column captures")


if __name__ == "__main__":
    main()
