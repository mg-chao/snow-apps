#!/usr/bin/env python3
"""Summarize complete benchmark campaigns, pairing identical cases and outputs.

Timing is the median of process medians. Reported p95 is the median of process
p95 values, not a pooled p95. Session ranges describe observed noise, not a
confidence interval or significance test. RSS, physical footprint, logical
buffer bytes, and allocator-requested heap bytes remain separate.
Explicitly selected sampled memory phases report each process's maximum, then
the median of those maxima; checkpoint counts never weight process observations.
"""

import argparse
from collections import defaultdict
import csv
import io
import json
import math
from pathlib import Path
import re
import statistics


# Implementation selectors (qt/pages, selected_strategy) are provenance, not
# workload identity. Geometric and workload parameters must never be pooled.
IDENTITY_FIELDS = (
    "scenario", "width", "height", "operation", "suite", "effect", "workload",
    "working_width", "working_height", "logical_width", "logical_height",
    "physical_width", "physical_height", "dpr", "strength", "filter_count",
    "exposed_width", "exposed_height", "cache_mode", "render_area_width",
    "render_area_height", "text_bytes", "font_size", "angle", "gap", "batch_size",
    "lines", "blur",
)


def case_name(name, row):
    return name + "".join(f":{field}={row[field]}" for field in IDENTITY_FIELDS
                           if row.get(field) not in (None, ""))


def number(value):
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"Invalid nonnegative measurement: {value}")
    return result


def load_campaign(root):
    metadata = json.loads((root / "metadata.json").read_text(encoding="utf-8"))
    if metadata.get("schema_version", 1) >= 2 and not metadata.get("completed"):
        raise ValueError(f"Campaign has not completed its binary integrity check: {root}")
    rounds = metadata["rounds"]
    if not isinstance(rounds, int) or rounds < 1:
        raise ValueError(f"Invalid round count in {root}")
    expected = {(round_index, label, job["name"])
                for round_index in range(1, rounds + 1)
                for job in metadata["plan"]["jobs"] for label in job["commands"]}
    records = [json.loads(line) for line in
               (root / "manifest.jsonl").read_text(encoding="utf-8").splitlines() if line]
    actual = [(r["round"], r["version"], r["workload"]) for r in records]
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise ValueError(f"Campaign is incomplete or contains duplicate/unknown processes: {root}")
    if any(record["exit_code"] for record in records):
        raise ValueError(f"A campaign includes a failed process: {root}")
    artifacts = {}
    for record in records:
        run = root / "runs" / f"round-{record['round']}" / record["version"]
        directory = root / record.get("artifact_directory", str(run.relative_to(root)))
        if not directory.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"Artifact directory escapes campaign: {directory}")
        name = record["workload"]
        required = [directory / (name + suffix) for suffix in (".stdout", ".stderr")]
        if any(not path.is_file() for path in required):
            raise ValueError(f"Missing stdout/stderr for {name}: {directory}")
        if "artifact_directory" in record:
            files = sorted(path for path in directory.iterdir() if path.is_file())
        else:
            # Legacy runner shared directories: sidecars must belong to this job.
            files = list(required)
            for argument in record.get("command", []):
                candidate = Path(argument.split("=", 1)[-1])
                if candidate.is_file() and candidate.parent.resolve() == directory.resolve():
                    files.append(candidate)
            files.extend(path for path in directory.iterdir()
                         if path.is_file() and path.name.startswith(name + "-"))
            files = sorted(set(files))
        artifacts[record["round"], record["version"], name] = files
    return metadata, artifacts


class Measurements:
    def __init__(self, sampled_memory_phases=()):
        self.timing = defaultdict(dict)
        self.memory = defaultdict(dict)
        self.sampled_memory_phases = frozenset(sampled_memory_phases)
        if any(not phase or phase.strip() != phase for phase in self.sampled_memory_phases):
            raise ValueError("Sampled memory phase names must be nonempty without surrounding whitespace")
        self.sampled_memory_keys = set()
        self.checks = defaultdict(set)
        self.check_versions = defaultdict(set)
        self.modes = defaultdict(set)
        self.ignored = []

    def add_timing(self, version, name, run, median, p95=None, count=None):
        if run in self.timing[name, version]:
            raise ValueError(f"Duplicate timing case in one process: {name}/{version}/{run}")
        count = int(count) if count not in (None, "") else None
        if count is not None and count < 1:
            raise ValueError(f"Invalid sample count: {name}")
        self.timing[name, version][run] = (
            number(median), None if p95 in (None, "") else number(p95), count)

    def add_memory(self, version, name, run, value, aggregate_max=False):
        if value in (None, ""):
            return
        value = number(value)
        identity = (name, version, run)
        if run in self.memory[name, version]:
            if not aggregate_max or identity not in self.sampled_memory_keys:
                raise ValueError(f"Duplicate memory case in one process: {name}/{version}/{run}")
            value = max(self.memory[name, version][run], value)
        if aggregate_max:
            self.sampled_memory_keys.add(identity)
        self.memory[name, version][run] = value

    def checksum(self, version, name, row, suffix=""):
        for field in ("checksum", "output_sha256", "source_sha256"):
            value = row.get(field)
            if value not in (None, "", "0", 0):
                key = name + ":" + field + suffix
                self.checks[key].add(str(value))
                self.check_versions[key].add(version)

    def samples(self, version, name, run, rows, field, scale):
        grouped, memory = defaultdict(list), defaultdict(list)
        for row in rows:
            if row.get(field) in (None, ""):
                continue
            key = case_name(name, row)
            grouped[key].append(number(row[field]) * scale)
            self.checksum(version, key, row, ":sample=" + str(row.get("iteration", row.get("sample", ""))))
            for column, value in row.items():
                if column.endswith("bytes") and value not in (None, ""):
                    memory[key + ":sampled_max:" + column].append(number(value))
        for key, samples in grouped.items():
            samples.sort()
            self.add_timing(version, key, run, statistics.median(samples),
                            samples[math.ceil(.95 * len(samples)) - 1], len(samples))
        for key, values in memory.items():
            self.add_memory(version, key, run, max(values))

    def records(self, version, name, run, rows):
        self.samples(version, name, run, [r for r in rows if r.get("record") == "sample"],
                     "elapsed_ns", 1e-6)
        for row in rows:
            key = case_name(name, row)
            if row.get("mode"):
                self.modes[name, version].add(row["mode"])
            if row.get("record") == "sample":
                continue
            phase = str(row.get("phase", row.get("record", "checkpoint")))
            self.checksum(version, key, row, ":" + phase)
            sampled = phase in self.sampled_memory_phases
            memory_phase = phase + "_max" if sampled else phase
            for field, value in row.items():
                if field.endswith("bytes"):
                    self.add_memory(version, key + ":" + memory_phase + ":" + field, run, value,
                                    aggregate_max=sampled)

    def csv_report(self, version, name, run, rows):
        if not rows:
            return False
        columns = rows[0]
        if "record" in columns and "elapsed_ns" in columns:
            self.records(version, name, run, rows)
        elif "ns_per_op" in columns:
            self.samples(version, name, run, rows, "ns_per_op", 1e-6)
        elif "elapsed_ms" in columns:
            self.samples(version, name, run, rows, "elapsed_ms", 1)
        elif "p50_ms" in columns:
            for row in rows:
                key = case_name(name, row)
                self.add_timing(version, key, run, row["p50_ms"], row.get("p95_ms"), row.get("samples"))
                self.checksum(version, key, row)
                for field, value in row.items():
                    if field.endswith("bytes"):
                        self.add_memory(version, key + ":" + field, run, value)
        elif "heap_live_bytes" in columns:
            self.records(version, name, run, rows)
        elif "render_median_ms" in columns:
            for row in rows:
                key = case_name(name, row)
                self.add_timing(version, key + ":render", run, row["render_median_ms"], row.get("render_p95_ms"))
                if "estimate_median_ms" in row:
                    self.add_timing(version, key + ":estimate", run, row["estimate_median_ms"], row.get("estimate_p95_ms"))
                self.checksum(version, key, row)
        else:
            return False
        return True

    def json_report(self, version, name, run, data):
        for row in data.get("scenarios", []):
            row = dict(row)
            row.setdefault("scenario", row.get("name", name))
            key = case_name(name, row)
            self.checksum(version, key, row)
            for timing in ("create", "destroy", "operation", "release"):
                distribution = row.get(timing, row.get(timing + "_timing", {}))
                if isinstance(distribution, dict) and "p50_ms" in distribution:
                    self.add_timing(version, key + ":" + timing, run, distribution["p50_ms"],
                                    distribution.get("p95_ms"), distribution.get("samples", data.get("samples")))
            for field, value in row.items():
                if isinstance(value, (int, float)) and not isinstance(value, bool) and field.endswith("bytes"):
                    self.add_memory(version, key + ":" + field, run, value)

    def parse(self, version, name, run, path):
        content = path.read_text(encoding="utf-8", errors="replace").strip()
        if path.suffix == ".stderr":
            match = re.search(r"(\d+)\s+maximum resident set size", content)
            if match:
                self.add_memory(version, name + ":process_peak_rss_bytes", run, match.group(1))
            return
        if path.suffix not in (".stdout", ".csv", ".json") or not content:
            return
        if content.startswith("{"):
            try:
                data = json.loads(content)
            except json.JSONDecodeError:
                rows = [json.loads(line) for line in content.splitlines() if line.strip()]
                if not all(isinstance(row, dict) and "record" in row for row in rows):
                    raise ValueError(f"Unrecognized JSONL benchmark: {path}")
                self.records(version, name, run, rows)
                return
            if "record" in data:
                self.records(version, name, run, [data])
                return
            if "scenarios" in data:
                self.json_report(version, name, run, data)
                return
            if all(field in data for field in ("decode", "preparation", "cached_paint")):
                self.add_timing(version, name + ":decode-4k", run, data["decode"]["median_ms"])
                for kind in ("preparation", "cached_paint"):
                    for row in data[kind]:
                        key = case_name(name + ":" + kind, row)
                        if kind == "preparation":
                            key += ":mode=" + str(row.get("mode", ""))
                        self.add_timing(version, key, run, row["median_ms"], row.get("p95_ms"))
                return
        elif content.startswith("["):
            data = json.loads(content)
            if all("median_us" in row and "p95_us" in row for row in data):
                for row in data:
                    self.add_timing(version, case_name(name, row), run, row["median_us"] / 1000,
                                    row["p95_us"] / 1000, len(row.get("samples_us", [])) or None)
                return
        elif self.csv_report(version, name, run, list(csv.DictReader(io.StringIO(content)))):
            return
        self.ignored.append(str(path))


def summarize(root, overlays, output_root, sampled_memory_phases=()):
    metadata, artifacts = load_campaign(root)
    labels = list(metadata["plan"]["versions"])
    if len(labels) < 2:
        raise ValueError("A comparison requires at least two versions")
    sources = [root, *overlays]
    for source in overlays:
        overlay_metadata, replacement = load_campaign(source)
        if overlay_metadata["plan"]["versions"] != metadata["plan"]["versions"]:
            raise ValueError("Overlay versions do not match the primary campaign")
        # Whole jobs replace all primary rounds, avoiding unequal leftovers.
        jobs = {job for _, _, job in replacement}
        artifacts = {key: files for key, files in artifacts.items() if key[2] not in jobs}
        artifacts.update(replacement)
    measurements = Measurements(sampled_memory_phases)
    duplicate_reports = []
    for (round_index, version, name), files in sorted(artifacts.items()):
        seen_reports = {}
        timings_before = sum(len(runs) for runs in measurements.timing.values())
        for path in files:
            # Some benchmarks emit the same complete report to stdout and a
            # sidecar. Count that measurement once, but never deduplicate a
            # stderr resource snapshot or merely matching individual metrics.
            if path.suffix in (".stdout", ".csv", ".json"):
                content = path.read_bytes()
                if content in seen_reports:
                    duplicate_reports.append({"duplicate": str(path),
                                              "original": str(seen_reports[content])})
                    continue
                seen_reports[content] = path
            measurements.parse(version, name, f"round-{round_index}", path)
        # Process completeness is separate from case pairing: if both versions
        # emit an unknown report, their missing cases would otherwise disappear.
        if sum(len(runs) for runs in measurements.timing.values()) == timings_before:
            raise ValueError(f"No parsed timing for process: {name}/{version}/round-{round_index}")
    output_root.mkdir(parents=True, exist_ok=True)

    def write_json(name, value):
        (output_root / name).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    mismatches = {name: sorted(values) for name, values in measurements.checks.items() if len(values) > 1}
    write_json("checksum-mismatches.json", mismatches)
    if mismatches:
        raise ValueError(f"Output/checksum mismatch in {len(mismatches)} comparable cases; "
                         f"see {output_root / 'checksum-mismatches.json'}")
    for name, versions in measurements.check_versions.items():
        if versions != set(labels):
            raise ValueError(f"Unpaired output checksum: {name}")
    for collection in (measurements.timing, measurements.memory):
        for name in {name for name, _ in collection}:
            reference = set(collection.get((name, labels[0]), {}))
            if any(set(collection.get((name, label), {})) != reference for label in labels[1:]):
                raise ValueError(f"Unpaired case or unequal round count: {name}")
    for name, _ in measurements.timing:
        reference = measurements.timing[name, labels[0]]
        for label in labels[1:]:
            for run, (_, _, count) in measurements.timing[name, label].items():
                if count != reference[run][2]:
                    raise ValueError(f"Unequal within-process sample count: {name}/{run}")
    summary, columns = [], ["workload"]
    suffixes = ("_ms", "_p95_ms", "_run_min_ms", "_run_max_ms", "_runs", "_samples")
    for label in labels:
        columns.extend(label + suffix for suffix in suffixes)
    columns += ["change_percent", "session_ranges_overlap"]
    for name in sorted({name for name, _ in measurements.timing}):
        record = {"workload": name}
        for label in labels:
            samples = list(measurements.timing[name, label].values())
            medians = [sample[0] for sample in samples]
            tails = [sample[1] for sample in samples if sample[1] is not None]
            counts = [sample[2] for sample in samples if sample[2] is not None]
            record.update({label + "_ms": statistics.median(medians),
                           label + "_p95_ms": statistics.median(tails) if tails else "",
                           label + "_run_min_ms": min(medians), label + "_run_max_ms": max(medians),
                           label + "_runs": len(samples),
                           label + "_samples": sum(counts) if len(counts) == len(samples) else ""})
        before, after = labels[0], labels[-1]
        denominator = record[before + "_ms"]
        record["change_percent"] = (record[after + "_ms"] / denominator - 1) * 100 if denominator else ""
        record["session_ranges_overlap"] = not (
            record[before + "_run_max_ms"] < record[after + "_run_min_ms"] or
            record[after + "_run_max_ms"] < record[before + "_run_min_ms"])
        summary.append(record)
    with (output_root / "summary.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns, lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary)
    write_json("summary.json", summary)
    memory_rows, comparisons = [], []
    for name in sorted({name for name, _ in measurements.memory}):
        comparison = {"workload": name}
        for label in labels:
            samples = list(measurements.memory[name, label].values())
            median = statistics.median(samples)
            memory_rows.append({"workload": name, "version": label, "median_bytes": median,
                                "runs_bytes": samples, "run_min_bytes": min(samples),
                                "run_max_bytes": max(samples), "runs": len(samples)})
            comparison[label + "_bytes"] = median
            comparison[label + "_runs"] = len(samples)
        before, after = labels[0], labels[-1]
        comparison["change_bytes"] = comparison[after + "_bytes"] - comparison[before + "_bytes"]
        denominator = comparison[before + "_bytes"]
        comparison["change_percent"] = comparison["change_bytes"] / denominator * 100 if denominator else None
        comparisons.append(comparison)
    write_json("memory-summary.json", memory_rows)
    write_json("memory-comparison.json", comparisons)
    write_json("summary-provenance.json", {
        "versions": metadata["plan"]["versions"],
        "campaigns": [str(source.resolve()) for source in sources],
        "selection": "Later complete campaigns replace all rounds of the same job",
        "timing_semantics": __doc__, "comparison": {"before": labels[0], "after": labels[-1]},
        "sampled_memory_phases": sorted(measurements.sampled_memory_phases),
        "sampled_memory_semantics": "Selected phases use the maximum per metric within each process, "
                                    "then the median across processes with one observation per process",
        "implementation_modes": [{"workload": name, "version": label, "modes": sorted(modes)}
                                 for (name, label), modes in sorted(measurements.modes.items())],
        "ignored_artifacts": measurements.ignored,
        "duplicate_reports": duplicate_reports,
    })
    return len(summary), len(memory_rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--overlay", type=Path, action="append", default=[],
                        help="Replace jobs with a complete repeated campaign")
    parser.add_argument("--output", type=Path, help="Write summaries to a separate directory")
    parser.add_argument("--sampled-memory-phase", action="append", default=[], metavar="PHASE",
                        help="Opt in to maximum per process for repeated memory checkpoints in PHASE; "
                             "report PHASE_max and take the median across processes (repeatable)")
    args = parser.parse_args()
    try:
        timing, memory = summarize(args.directory, args.overlay, args.output or args.directory,
                                   args.sampled_memory_phase)
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{timing} paired timing cases, {memory} memory metrics, no checksum mismatches")


if __name__ == "__main__":
    main()
