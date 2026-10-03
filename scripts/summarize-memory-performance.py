#!/usr/bin/env python3
"""Summarize run-memory-performance.py artifacts without pooling unlike cases."""

import argparse
import csv
import io
import json
import math
from pathlib import Path
import re
import statistics
from collections import defaultdict


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--overlay", type=Path, action="append", default=[],
                        help="Prefer repeated job artifacts from this campaign")
    parser.add_argument("--output", type=Path, help="Write summaries to a separate directory")
    args = parser.parse_args()
    root = args.directory
    metadata = json.loads((root / "metadata.json").read_text())
    labels = list(metadata["plan"]["versions"])
    output_root = args.output or root
    output_root.mkdir(parents=True, exist_ok=True)
    values = defaultdict(list)
    checks = defaultdict(set)
    memory = defaultdict(list)

    def add(version, name, median, p95=None, checksum=None):
        values[name, version].append((float(median), None if p95 is None else float(p95)))
        if checksum not in (None, "", "0"):
            checks[name].add(str(checksum))

    def series(version, name, rows, field, scale=1, keys=("scenario", "width", "height")):
        grouped = defaultdict(list)
        for row in rows:
            if row.get(field, "") == "":
                continue
            key = name + ":" + ":".join(row.get(k, "") for k in keys)
            grouped[key].append(float(row[field]) * scale)
            if row.get("checksum") not in (None, "", "0"):
                # A recording sequence can legitimately differ by frame.
                check_key = key + ":sample" + row.get("iteration", row.get("sample", ""))
                checks[check_key].add(row["checksum"])
        for key, samples in grouped.items():
            samples.sort()
            add(version, key, statistics.median(samples), samples[math.ceil(.95 * len(samples)) - 1])

    def csv_rows(path):
        return list(csv.DictReader(io.StringIO(path.read_text())))

    # Prefer complete repeated jobs, including stdout, stderr and CSV sidecars.
    # Keep original campaigns intact rather than rewriting their evidence.
    artifacts = defaultdict(dict)
    sources = [root, *args.overlay]
    for source in sources:
        source_metadata = json.loads((source / "metadata.json").read_text())
        if source_metadata["plan"]["versions"] != metadata["plan"]["versions"]:
            raise SystemExit("Overlay versions do not match the primary campaign")
        records = [json.loads(line) for line in (source / "manifest.jsonl").read_text().splitlines()]
        if any(record["exit_code"] for record in records):
            raise SystemExit("A campaign includes a failed process")
        expected = source_metadata["rounds"] * sum(
            len(job["commands"]) for job in source_metadata["plan"]["jobs"])
        if len(records) != expected:
            raise SystemExit("Campaign is incomplete; wait for every process to finish")
        for run in sorted((source / "runs").glob("round-*/*")):
            files = artifacts[run.parent.name, run.name]
            for path in run.iterdir():
                if path.is_file():
                    files[path.name] = path
    for (_, version), files in sorted(artifacts.items()):
        for path in (p for p in files.values() if p.suffix == ".stderr"):
            match = re.search(r"(\d+)\s+maximum resident set size", path.read_text())
            if match:
                memory[path.stem + ":process_peak_rss_bytes", version].append(int(match.group(1)))
        for path in (p for p in files.values() if p.suffix == ".stdout"):
            name = path.stem
            content = path.read_text().strip()
            if not content:
                continue
            if content.startswith("{"):
                data = json.loads(content)
                for row in data.get("scenarios", []):
                    scenario = row.get("scenario", row.get("name", name))
                    key = name + ":" + scenario
                    for timing in ("create", "destroy", "operation", "release"):
                        distribution = row.get(timing, row.get(timing + "_timing", {}))
                        if isinstance(distribution, dict) and "p50_ms" in distribution:
                            add(version, key + ":" + timing, distribution["p50_ms"],
                                distribution.get("p95_ms"), row.get("output_sha256"))
                    # Preserve JSON memory checkpoint labels exactly; no attempt
                    # to treat footprint and peak RSS as the same resource.
                    for field, value in row.items():
                        if isinstance(value, (int, float)) and field.endswith("bytes"):
                            memory[key + ":" + field, version].append(value)
                continue
            rows = csv_rows(path)
            if not rows:
                continue
            columns = rows[0]
            if "record" in columns and "elapsed_ns" in columns:
                series(version, name, [r for r in rows if r["record"] == "sample"],
                       "elapsed_ns", 1e-6)
                for row in rows:
                    if row["record"] == "sample":
                        continue
                    key = name + ":" + row["record"]
                    if row.get("checksum") not in (None, "", "0"):
                        checks[name + ":output"].add(row["checksum"])
                    for field in ("rss_bytes", "footprint_bytes", "peak_rss_bytes", "logical_bytes"):
                        if row.get(field):
                            memory[key + ":" + field, version].append(float(row[field]))
            elif "ns_per_op" in columns:
                series(version, name, rows, "ns_per_op", 1e-6)
            elif "heap_live_bytes" in columns:
                for row in rows:
                    key = name + ":" + ":".join(row.get(k, "") for k in
                                                   ("scenario", "phase", "width", "height"))
                    for field in columns:
                        if field.endswith("bytes") and row.get(field):
                            memory[key + ":" + field, version].append(float(row[field]))
            elif "elapsed_ms" in columns:
                series(version, name, rows, "elapsed_ms", keys=("scenario", "working_width", "working_height"))
            elif "render_median_ms" in columns:
                for row in rows:
                    key = name + ":" + ":".join(row.get(k, "") for k in
                                                   ("width", "height", "lines", "scenario"))
                    add(version, key + ":render", row["render_median_ms"], row.get("render_p95_ms"))
                    if "estimate_median_ms" in row:
                        add(version, key + ":estimate", row["estimate_median_ms"], row.get("estimate_p95_ms"))
        for path in (p for p in files.values() if p.suffix == ".csv" and
                     p.name.startswith(("filter-", "watermark-"))):
            for row in csv_rows(path):
                add(version, path.stem, row["p50_ms"], row["p95_ms"], row.get("checksum"))
        path = files.get("skin.json")
        if path is not None:
            data = json.loads(path.read_text())
            add(version, "skin:decode-4k", data["decode"]["median_ms"])
            for row in data["preparation"]:
                key = "skin:prepare:" + ":".join(str(row[k]) for k in
                                                    ("logical_width", "logical_height", "dpr", "blur", "mode"))
                add(version, key, row["median_ms"])
            for row in data["cached_paint"]:
                key = f"skin:cached-paint:{row['physical_width']}:{row['physical_height']}"
                add(version, key, row["median_ms"], row["p95_ms"])

    summary = []
    columns = ["workload"]
    for label in labels:
        columns += [label + suffix for suffix in ("_ms", "_p95_ms", "_run_min_ms", "_run_max_ms", "_runs")]
    columns += ["change_percent", "session_ranges_overlap"]
    for name in sorted({name for name, _ in values}):
        record = {"workload": name}
        for label in labels:
            samples = values.get((name, label), [])
            if not samples:
                continue
            medians = [sample[0] for sample in samples]
            tails = [sample[1] for sample in samples if sample[1] is not None]
            record.update({label + "_ms": statistics.median(medians),
                           label + "_p95_ms": statistics.median(tails) if tails else "",
                           label + "_run_min_ms": min(medians),
                           label + "_run_max_ms": max(medians), label + "_runs": len(samples)})
        before, after = labels[0], labels[-1]
        if before + "_ms" in record and after + "_ms" in record:
            record["change_percent"] = (record[after + "_ms"] / record[before + "_ms"] - 1) * 100
            record["session_ranges_overlap"] = not (
                record[before + "_run_max_ms"] < record[after + "_run_min_ms"] or
                record[after + "_run_max_ms"] < record[before + "_run_min_ms"])
        summary.append(record)
    with (output_root / "summary.csv").open("w") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(summary)
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    memory_rows = [{"workload": name, "version": label,
                    "median_bytes": statistics.median(samples), "runs_bytes": samples}
                   for (name, label), samples in sorted(memory.items())]
    (output_root / "memory-summary.json").write_text(json.dumps(memory_rows, indent=2) + "\n")
    mismatches = {name: sorted(samples) for name, samples in checks.items() if len(samples) > 1}
    (output_root / "checksum-mismatches.json").write_text(json.dumps(mismatches, indent=2) + "\n")
    (output_root / "summary-provenance.json").write_text(json.dumps({
        "versions": metadata["plan"]["versions"],
        "campaigns": [str(source.resolve()) for source in sources],
        "selection": "Later campaigns replace artifacts of the same job, round and version",
    }, indent=2) + "\n")
    print(f"{len(summary)} timing cases, {len(memory_rows)} memory metrics, "
          f"{len(mismatches)} checksum mismatches")


if __name__ == "__main__":
    main()
