#!/usr/bin/env python3
"""Independently decode animated fixtures from keyboard_overlay_benchmark (requires Pillow)."""
import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter, ImageStat


def verify(directory: Path) -> None:
    for extension in ("gif", "apng", "webp"):
        path = directory / f"keyboard.{extension}"
        with Image.open(path) as image:
            assert image.size == (640, 360), (path, image.size)
            first = None
            differences = []
            for index in range(image.n_frames):
                image.seek(index)
                # GIF's palette dithering can change phase between identical input frames.
                # Compare low-frequency content, preserving the much larger keycap shapes.
                frame = image.convert("RGB").filter(ImageFilter.GaussianBlur(2)).crop((390, 210, 640, 360))
                if first is None:
                    first = frame.copy()
                differences.append(sum(ImageStat.Stat(ImageChops.difference(first, frame)).mean) / 3)
                if index in (0, 24, image.n_frames - 1):
                    image.convert("RGB").save(directory / f"{extension}-frame-{index}.png")
            assert len(differences) > 10, f"{path}: animation frames missing"
            assert max(differences) > 2, f"{path}: key overlay missing"
            assert differences[-1] < 1.5, f"{path}: key overlay did not disappear"
            print(f"{extension}: decoded {len(differences)} frames; "
                  f"key-frame difference={max(differences):.2f}; final={differences[-1]:.2f}")
    for path in directory.glob("*.ppm"):
        with Image.open(path) as image:
            image.save(path.with_suffix(".png"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    verify(parser.parse_args().directory)
