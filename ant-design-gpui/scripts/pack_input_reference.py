# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Deduplicate browser measurements without calculating native expected values."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--measurements", type=Path, required=True)
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "fixtures/input")
args = parser.parse_args()
fields = ["width", "height", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
          "fontSize", "lineHeight", "borderTopWidth", "borderLeftWidth", "borderBottomWidth",
          "borderTopLeftRadius", "backgroundColor", "borderBottomColor", "color", "boxShadow",
          "outlineWidth", "outlineColor"]
styles, identifiers, cases = [], {}, []
options, scale = None, None
for theme in ["default", "dark", "compact", "dark-compact"]:
    for state in ["rest", "hover", "focus"]:
        measured = json.loads((args.measurements / f"{theme}-{state}.json").read_text(encoding="utf-8"))
        assert (measured["theme"], measured["state"]) == (theme, state)
        current_options = [control["options"] for control in measured["controls"]]
        if options is None:
            options, scale = current_options, measured["scale"]
        assert current_options == options and measured["scale"] == scale
        assert len(options) == 672
        rows = []
        for control in measured["controls"]:
            row = []
            for part in ["root", "input", "actions"]:
                if control[part] is None:
                    row.append(None)
                    continue
                style = {key: control[part][key] for key in fields}
                key = json.dumps(style, sort_keys=True, separators=(",", ":"))
                if key not in identifiers:
                    identifiers[key] = len(styles)
                    styles.append(style)
                row.append(identifiers[key])
            rows.append(row)
        cases.append({"theme": theme, "state": state, "controls": rows})
args.output.mkdir(parents=True, exist_ok=True)
provenance = json.loads((args.measurements / "provenance.json").read_text(encoding="utf-8"))
result = {"scale": scale, "profile": provenance.get("profile", "default"), "options": options, "styles": styles, "cases": cases}
(args.output / "reference.json").write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
(args.output / "provenance.json").write_text((args.measurements / "provenance.json").read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
print(f"Packed {len(cases) * len(options)} measured controls into {len(styles)} unique styles")

motion_files = [args.measurements / f"motion-{theme}.json" for theme in ["default", "dark", "compact", "dark-compact"]]
if any(path.exists() for path in motion_files):
    assert all(path.exists() for path in motion_files), "Capture all four motion pages before packing"
    motion = [json.loads(path.read_text(encoding="utf-8")) for path in motion_files]
    for page in motion:
        assert page["scale"] == scale and len(page["cases"]) == 24
        assert all(len(case["samples"]) == 7 and len(case["reverseSamples"]) == 8 for case in page["cases"])
    (args.output / "motion.json").write_text(json.dumps(motion, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
    print("Packed 1,440 measured motion samples across 96 transitions/reversals")
