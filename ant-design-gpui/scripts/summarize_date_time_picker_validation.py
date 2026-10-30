# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Report per-scene evidence completeness; absence never counts as passing."""
import argparse
import json
import hashlib
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig")) if path and path.exists() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--comparison", type=Path)
    parser.add_argument("--output", type=Path, default=Path("fixtures/date_time_picker/coverage-matrix.json"))
    args = parser.parse_args()
    manifest = read(Path(__file__).resolve().parent.parent / "fixtures/date_time_picker/visual-cases.json")
    reference, native, comparison = read(args.reference), read(args.native), read(args.comparison)
    count = len(manifest["themes"]) * len(manifest["directions"]) * len(manifest["scales"])
    rows = []
    for scene, options in manifest["scenes"].items():
        source = [c for c in (reference or {}).get("cases", []) if c["scene"] == scene]
        target = [c for c in (native or {}).get("cases", []) if c["scene"] == scene and not c.get("unsupported")]
        compared = [c for c in (comparison or {}).get("results", []) if c["name"].startswith(scene + "-")]
        # Match exact scenario names to avoid prefix collisions such as date-range/date-range-hover.
        valid_names = {f"{scene}-{theme}-{direction}-{scale * 100:g}" for theme in manifest["themes"]
                       for direction in manifest["directions"] for scale in manifest["scales"]}
        compared = [c for c in compared if c["name"] in valid_names]
        passed = sum(c["passed"] for c in compared)
        rows.append({"scene": scene, "component": options["component"], "mode": options.get("picker", "time" if options["component"] == "time" else "date"),
                     "range": options.get("range", False), "expected_cases": count,
                     "reference_captured": len(source), "native_captured": len(target), "comparison_passed": passed,
                     "reference_status": "complete" if len(source) == count else "partial" if source else "pending",
                     "native_status": "complete" if len(target) == count else "partial" if target else "pending",
                     "visual_status": "passed" if passed == count else "failed" if compared else "pending"})
    report = {"schema_version": 1, "clock": manifest["clock"], "scenes": len(rows),
              "expected_cases": len(rows) * count, "reference_cases": sum(r["reference_captured"] for r in rows),
              "native_cases": sum(r["native_captured"] for r in rows), "visual_passed_cases": sum(r["comparison_passed"] for r in rows),
              "matrix": rows}
    report["evidence"] = {"comparison_status":(comparison or {}).get("status","pending"),
                          "native_attestation":(comparison or {}).get("native_attestation",{}).get("status","pending")}
    for key,path in (("reference_metrics_sha256",args.reference),("native_metrics_sha256",args.native),
                     ("comparison_sha256",args.comparison)):
        if path and path.exists():
            with path.open("rb") as stream:
                report["evidence"][key] = hashlib.file_digest(stream,"sha256").hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Picker evidence: reference {report['reference_cases']}/{report['expected_cases']}; "
          f"native {report['native_cases']}/{report['expected_cases']}; strict visual pass {report['visual_passed_cases']}/{report['expected_cases']}")


if __name__ == "__main__":
    main()
