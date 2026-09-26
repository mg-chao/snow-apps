"""Summarize at least three matched Release MCP measurement reports."""
import argparse
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if len(args.reports) < 3:
        parser.error("at least three repeated reports are required")
    reports = [json.loads(path.read_text(encoding="utf-8")) for path in args.reports]
    first = reports[0]
    for report in reports:
        assert report["benchmark"] is True
        assert report["environment"] == first["environment"], "hardware/runtime metadata changed"
        assert report["binaries"] == first["binaries"], "measured binaries changed between repeats"
        for version in ("baseline", "current"):
            for size, entry in report["legacy_comparison"][version].items():
                assert entry["fixture_sha256"] == first["legacy_comparison"][version][size]["fixture_sha256"]
    lines = ["# Repeated MCP Release measurements", "",
             f"{len(reports)} runs; {first['samples']} samples per legacy workflow per run.", "",
             f"Environment: {first['environment']['platform']}; {first['environment']['processor']}; "
             f"{first['environment']['logical_cpus']} logical processors; Python {first['environment']['python']}.", "",
             f"Measured UTC: {', '.join(report['measured_at_utc'] for report in reports)}.", "",
             "Original/current Qt session and transport code use identical deterministic image/provider ports",
             "and the common renderer. Copy/pin are acknowledgments; these are not native desktop benchmarks.", "",
             "| Resolution | Baseline workflow p50 (ms) | Current workflow p50 (ms) | Current / baseline |",
             "| --- | ---: | ---: | ---: |"]
    for size in ("800x600", "1920x1080", "3840x2160"):
        values = {}
        for version in ("baseline", "current"):
            values[version] = statistics.median(report["legacy_comparison"][version][size]["latency"]
                ["legacy_complete_workflow"]["p50_ms"] for report in reports)
        lines.append(f"| {size} | {values['baseline']:.2f} | {values['current']:.2f} | "
                     f"{values['current'] / values['baseline']:.3f} |")
    lines += ["", "Values are medians of per-run p50 values, not pooled percentiles. Raw JSON retains p95/p99,",
              "CPU, output byte counts, binary hashes and every original tool's contract outcomes.", "",
              "| Check | Range across repeated runs |", "| --- | --- |"]
    extractors = {
        "Blocked-reader shutdown (ms)": lambda r: r["slow_reader"]["exit_ms"],
        "Other client's maximum status latency during blocked reader (ms)": lambda r: r["slow_reader"]["control_max_ms"],
        "Document fixture GUI thread CPU utilization (%)": lambda r: 100 * r["gui_thread"]["cpu_utilization"],
        "Document fixture maximum heartbeat lateness (ms)": lambda r: r["gui_thread"]["max_lateness_ms"],
        "Last cleanup-cycle fixture private bytes (MiB)": lambda r: r["cleanup_cycles"][-1]["fixture_after_close"]["private_bytes"] / 1048576,
    }
    for label, extract in extractors.items():
        values = [extract(report) for report in reports]
        lines.append(f"| {label} | {min(values):.2f}–{max(values):.2f} |")
    lines += ["", "GUI CPU uses kernel thread counters; heartbeat lateness measures responsiveness separately.",
              "Memory after close may include allocator caches. Four cleanup-cycle samples are retained per run;",
              "resource-list emptiness is asserted, but RSS/private-byte retention alone does not prove a leak.", "",
              "The expanded 101-tool catalog is larger than the original 28-tool catalog; tool-list timings",
              "must be interpreted together with output bytes, not as equal-size request benchmarks.", ""]
    lines += ["| Measured executable | SHA-256 |", "| --- | --- |"]
    for label, binary in first["binaries"].items():
        lines.append(f"| {label} | `{binary['sha256']}` |")
    for version in ("baseline", "current"):
        digest = first["legacy_comparison"][version]["800x600"]["fixture_sha256"]
        lines.append(f"| {version} legacy Qt fixture | `{digest}` |")
    lines += [""]
    output = "\n".join(lines)
    if args.output:
        args.output.write_text(output, encoding="utf-8", newline="\n")
    print(output)


if __name__ == "__main__":
    main()
