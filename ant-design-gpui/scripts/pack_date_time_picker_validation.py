# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Preserve compact Windows evidence from complete attested picker runs."""
import argparse
import hashlib
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity(provenance):
    sources = provenance["source_sha256"]
    return {"source_map_sha256": hashlib.sha256(json.dumps(sources, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
            "source_files": len(sources), "executable_sha256": provenance["executable"]["sha256"],
            "toolchain": provenance["toolchain"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--visual", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--uia", type=Path)
    parser.add_argument("--performance", type=Path)
    parser.add_argument("--output", type=Path, default=Path("fixtures/date_time_picker"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reports = []
    if args.visual:
        if not args.native:
            raise ValueError("Visual evidence requires native attestation")
        report, provenance = read(args.visual), read(args.native / "native-provenance.json")
        metrics = args.native / "native-metrics.json"
        if report["native_metrics_sha256"] != digest(metrics) or provenance["metrics_sha256"] != digest(metrics):
            raise ValueError("Visual metrics differ from native attestation")
        if report["cases"] != 1344 or provenance["capture_context"]["cases"] != 1344:
            raise ValueError("Only a complete 1,344-case run can be packed")
        extents, fringe = [], {"reference_uncovered_pixels": 0, "native_uncovered_pixels": 0}
        for case in report["results"]:
            for check in case["checks"]:
                for extent in check.get("antialias_extent_diagnostics", []):
                    extents.append({"case": case["name"], **extent})
                for key, count in check.get("full_antialias_fringe_diagnostic", {}).items():
                    fringe[key] += count
        worst = sorted(extents, key=lambda e: max(abs(v) for v in e["painted_edge_delta"].values()), reverse=True)[:8]
        result = {key: report[key] for key in ("status", "cases", "failed_cases", "failed_checks", "geometry_tolerance_logical_px",
                   "solid_rgb_tolerance_bytes", "reference_metrics_sha256", "native_metrics_sha256", "comparator_sha256",
                   "reference_upstream", "aggregate_raster_regions")}
        result.update(schema_version=1, platform="Windows x64", completed_utc=provenance["completed_utc"],
                      comparison_sha256=digest(args.visual), attestation_sha256=digest(args.native / "native-provenance.json"),
                      **identity(provenance), capture_context=provenance["capture_context"],
                      comparator_controls=69, full_antialias_fringe_diagnostic=fringe, largest_antialias_extent_diagnostics=worst,
                      failed_targets=[{"case": case["name"], "checks": [check for check in case["checks"] if not check["passed"]]}
                                      for case in report["results"] if not case["passed"]],
                      ink_sampling={"method": "symmetric strong pixel center to occupied pixel square", "logical_distance_budget": 1,
                                    "minimum_core_coverage": 0.5, "sampling_uncertainty_physical_px_per_axis": 0.5,
                                    "authored_geometry": "independently gated", "remove_tint": "computed SVG RGBA versus transformed native scene sprite, one byte"},
                      limitations=["Text, shadows and AA support are not byte-identical; unmasked statistics and PNGs retained.",
                                   "Pixel sampling introduces half a physical pixel of uncertainty per axis.",
                                   "Native captures tested on Windows only."])
        reports.append(("visual-validation-windows.json", result))
    if args.uia:
        source = read(args.uia)
        if source["build_skipped"] or len(source["cases"]) != 6:
            raise ValueError("Final UIA evidence needs a rebuilt six-profile run")
        result = {key: source[key] for key in ("status", "profile", "utc", "actuation", "adapter_limitation", "cases")}
        result.update(schema_version=1, platform="Windows x64", result_sha256=digest(args.uia), **identity(source),
                      checks=sum(len(case["checks"]) for case in source["cases"]))
        reports.append(("accessibility-windows.json", result))
    if args.performance:
        directory = args.performance
        provenance = read(directory / "date-time-picker-provenance.json")
        path = directory / "date-time-picker-results.jsonl"
        if provenance["profile"] != "Release" or provenance["results_sha256"] != digest(path):
            raise ValueError("Performance results differ from Release attestation")
        measurements = [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
        paths = [item for item in measurements if "us" in item]
        if len(paths) != 17:
            raise ValueError("Expected all 17 measured paths")
        result = {key: provenance[key] for key in ("completed_utc", "preset", "profile", "measurement", "cmake_cache_sha256", "results_sha256")}
        result.update(schema_version=1, status="passed", platform="Windows x64", **identity(provenance),
                      measurements=measurements, retained_editor_identity="stable", single_picker_primitive_limit=1200,
                      hundred_picker_primitive_limit=6400, idle_frame_requests=0,
                      limitations=["CPU/allocation distributions use offscreen TestAppContext and its deterministic test text system, with no numeric latency SLA.",
                                   "Production font-shaping performance is not measured by this fixture.",
                                   "No GPU presentation time or physical input latency was measured."])
        reports.append(("performance-windows.json", result))
    if not reports:
        raise ValueError("Select at least one evidence input")
    for name, report in reports:
        report["packer_sha256"] = digest(Path(__file__))
        (args.output / name).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Packed {name}: {report['status']}")


if __name__ == "__main__":
    main()
