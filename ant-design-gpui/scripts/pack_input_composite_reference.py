# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Pack original-browser composite measurements without deriving expectations."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--measurements", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
styles, identifiers, cases, hashes = [], {}, [], {}
options = scale = profile = None
for theme in ["default", "dark", "compact", "dark-compact"]:
    for state in ["rest", "hover", "focus"]:
        path = args.measurements / f"{theme}-{state}.json"
        raw = path.read_bytes()
        measured = json.loads(raw)
        hashes[path.name] = hashlib.sha256(raw).hexdigest()
        assert (measured["theme"], measured["state"]) == (theme, state)
        current = [control["options"] for control in measured["controls"]]
        if options is None:
            options, scale, profile = current, measured["scale"], measured["profile"]
        assert current == options and measured["scale"] == scale and measured["profile"] == profile
        assert len(options) == 840
        controls = []
        for control in measured["controls"]:
            parts = {}
            for name, style in control["parts"].items():
                # Absolute placement is retained in the hashed raw measurements.
                # These portable fixtures compare local dimensions and CSS styles.
                style = {key: value for key, value in style.items() if key not in ("x", "y")}
                key = json.dumps(style, sort_keys=True, separators=(",", ":"))
                if key not in identifiers:
                    identifiers[key] = len(styles)
                    styles.append(style)
                parts[name] = identifiers[key]
            controls.append(parts)
        cases.append({"theme": theme, "state": state, "controls": controls})
args.output.mkdir(parents=True, exist_ok=True)
result = {"scale": scale, "profile": profile, "options": options, "styles": styles, "cases": cases}
(args.output / "reference.json").write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
provenance = json.loads((args.measurements / "provenance.json").read_text(encoding="utf-8"))
provenance["measurementsSha256"] = hashes
provenance["markupAudit"] = {
    "package": "@rc-component/input@1.3.1",
    "source": "https://registry.npmjs.org/@rc-component/input/-/input-1.3.1.tgz",
    "sha256": {
        "es/BaseInput.js": "001b0c6bab4ebf024315fc903f9419344fee734844c452a55e9661396986e0e8",
        "es/Input.js": "9991443962f988eb8cbeaac51a53194f11908fd17c9f750f94a7f4cae5140155",
        "es/TextArea.js": "20c29ffd5dab0ef91a8a7d70e0c6c033311bc864d9db4d8362a2e27b979b15c3",
    },
    "notes": "Static markup was checked against these published package sources. Circular SVG placeholders measure icon boxes only; they do not establish icon pixel fidelity."
}
(args.output / "provenance.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
print(f"Packed {profile}: {len(cases) * len(options)} composite states, {len(styles)} unique measured parts")
