#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def symbol(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in re.findall(r"[A-Za-z0-9]+", name))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    asset_root = root / "src" / "presentation" / "components" / "icons" / "resources"
    entries = []
    for asset in sorted(asset_root.glob("*.svg"), key=lambda item: item.name):
        name = asset.stem
        variant = "outlined"
        model = "monochrome"
        defaults = None
        if name == "screenshot-feature":
            variant, model = "twotone", "twoTone"
            defaults = {"secondary": "#9254DE"}
        elif name == "snow-shot-logo":
            variant = "brand"
        entry = {
            "variant": variant,
            "symbol": symbol(name),
            "name": name,
            "source": f"../src/presentation/components/icons/resources/{asset.name}",
            "colorModel": model,
            "fit": "contain",
        }
        if defaults:
            entry["defaultColors"] = defaults
        entries.append(entry)
    entries.append({
        "variant": "app",
        "symbol": "ApplicationIcon",
        "name": "application-icon",
        "source": "app-icon.svg",
        "colorModel": "fullColor",
        "fit": "contain",
        "allowEmbeddedDataImages": True,
    })
    entries.sort(key=lambda item: (item["variant"], item["name"]))
    manifest = {
        "schemaVersion": 1,
        "pack": "snow-shot",
        "cppNamespace": "snow_shot::presentation::icons::custom",
        "headerInclude": "snow_shot/presentation/components/icons/snowshoticons.h",
        "source": "Snow Shot project-owned static SVG assets",
        "entries": entries,
    }
    expected = json.dumps(manifest, indent=2) + "\n"
    output = root / "resources" / "icons.manifest.json"
    if args.check:
        actual = output.read_text(encoding="utf-8") if output.exists() else ""
        if actual != expected:
            raise SystemExit(f"stale manifest: {output}")
    else:
        output.write_text(expected, encoding="utf-8", newline="\n")
    print(f"snow-shot manifest entries={len(entries)} check={args.check}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
