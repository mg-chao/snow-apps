# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Preserve measured Chromium form-control appearance beside computed styles."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from compare_pagination_visual import ua_targets

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
reference = json.loads((args.reference / "browser-metrics.json").read_text(encoding="utf-8"))
measurements = {}
for case in reference["cases"]:
    if not any(element["selector"] == ".ant-pagination-options-quick-jumper-button" for element in case["metrics"]["elements"]):
        continue
    name = case["scenario"]["name"]
    source = args.reference / f"{name}.png"
    pixels = np.array(Image.open(source).convert("RGB"), dtype=np.int16)
    targets = list(ua_targets(case, pixels, case["metrics"]["scale"]))
    fill = next(target for target in targets if target["channel"] == "color")
    stroke = next(target for target in targets if target["channel"] == "stroke")
    measurements[name] = {
        "fill_rgb": [round(value * 255) for value in fill["color"][:3]],
        "outline_rgb": [round(value * 255) for value in stroke["color"][:3]],
        "outline_width": stroke["border_widths"][0],
        "outline_bands": stroke["outline_bands"],
        "state": case["metrics"].get("native_button_state", {}),
        "source_png_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "evidence": "Neutral fill and outline measured from original browser pixels; computed CSS is retained separately",
    }
args.output.write_text(json.dumps(measurements, indent=2) + "\n", encoding="utf-8")
print(f"Preserved {len(measurements)} original browser-painted control measurements")
