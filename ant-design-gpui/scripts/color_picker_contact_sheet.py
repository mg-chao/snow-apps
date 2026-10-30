# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Make an unmasked reference/native/difference review sheet from capture results."""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="*")
    args = parser.parse_args()
    selected = args.cases or [
        "solid-hex-default-ltr-100", "solid-hex-compact-rtl-150",
        "solid-hsb-dark-ltr-200", "gradient-alpha-default-ltr-100",
        "presets-dark-rtl-150", "split-panel-default-ltr-100",
        "gradient-text-open-default-ltr-100", "bottom-right-no-arrow-compact-ltr-200",
    ]
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    cases = {case["name"]: case for case in report["results"]}
    selected = [name for name in selected if name in cases]
    if not selected:
        raise ValueError("No requested captured cases exist")
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    font = ImageFont.truetype(str(font_path), 14) if font_path.exists() else ImageFont.load_default()
    cell_width, cell_height, label_height = 440, 430, 44
    sheet = Image.new("RGB", (cell_width * 3, (cell_height + label_height) * len(selected)), "#e8e8e8")
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(selected):
        rectangle = cases[name]["component_crop_physical_px"]
        box = tuple(rectangle[key] for key in ("left", "top", "right", "bottom"))
        reference = Image.open(args.reference / f"{name}.png").convert("RGB").crop(box)
        native = Image.open(args.native / f"{name}.png").convert("RGB").crop(box)
        difference = ImageChops.difference(reference, native).point(lambda value: min(255, value * 4))
        top = index * (cell_height + label_height)
        for column, (label, source) in enumerate((("Reference", reference), ("Native", native), ("Absolute difference ×4", difference))):
            draw.text((column * cell_width + 8, top + 3), label, font=font, fill="black")
            if column == 0:
                draw.text((8, top + 22), name, font=font, fill="black")
            source.thumbnail((cell_width - 16, cell_height - 8), Image.Resampling.LANCZOS)
            sheet.paste(source, (column * cell_width + 8, top + label_height))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output)
    print(f"Saved {len(selected)} unmasked component pairs to {args.output}")


if __name__ == "__main__":
    main()
