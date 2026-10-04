#!/usr/bin/env python3
"""Deterministic runner/report contract tests; no performance workloads or builds."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_module("memory_runner", "run-memory-performance.py")
REPORT = load_module("memory_report", "summarize-memory-performance.py")
GENERATOR = load_module("memory_plan", "create-memory-performance-plan.py")


def sample(width=10, iteration=0, elapsed_ns=1000000, checksum=42):
    return {"record": "sample", "scenario": "copy", "width": width, "height": 10,
            "iteration": iteration, "elapsed_ns": elapsed_ns, "checksum": checksum}


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    def campaign(self, name="primary", rounds=1, rows=None, sidecars=None):
        root = self.base / name
        root.mkdir()
        metadata = {"schema_version": 2, "completed": True, "rounds": rounds,
                    "plan": {"versions": {"before": {"revision": "old"},
                                           "after": {"revision": "new"}},
                             "jobs": [{"name": "job", "commands": {
                                 "before": ["executable"], "after": ["executable"]}}]}}
        (root / "metadata.json").write_text(json.dumps(metadata))
        records = []
        for round_index in range(1, rounds + 1):
            for version in ("before", "after"):
                directory = root / "runs" / f"round-{round_index}" / version / "job"
                directory.mkdir(parents=True)
                selected = rows(version, round_index) if callable(rows) else rows
                if selected is None:
                    selected = [sample()]
                stdout = selected if isinstance(selected, str) else "\n".join(map(json.dumps, selected))
                (directory / "job.stdout").write_text(stdout)
                (directory / "job.stderr").write_text("123456 maximum resident set size\n")
                for filename, content in (sidecars or {}).items():
                    (directory / filename).write_text(content)
                records.append({"round": round_index, "version": version, "workload": "job",
                                "exit_code": 0, "artifact_directory": str(directory.relative_to(root))})
        (root / "manifest.jsonl").write_text("\n".join(map(json.dumps, records)) + "\n")
        return root

    def summary(self, root, overlays=(), sampled_memory_phases=()):
        REPORT.summarize(root, list(overlays), root, sampled_memory_phases)
        return json.loads((root / "summary.json").read_text())

    def test_process_without_parsed_timing_is_rejected_even_when_outputs_pair(self):
        for name, rows in (("unrecognized", "Unrecognized human-readable report"),
                           ("memory-only", [self.checkpoint(100, "live")])):
            with self.subTest(name=name):
                root = self.campaign(name=name, rows=rows)
                with self.assertRaisesRegex(ValueError, "No parsed timing for process"):
                    self.summary(root)
                self.assertFalse((root / "summary.json").exists())

    def test_missing_timing_in_same_round_of_both_versions_is_rejected(self):
        root = self.campaign(rounds=3, rows=lambda _, index: (
            "Unrecognized human-readable report" if index == 2 else [sample()]))
        with self.assertRaisesRegex(ValueError, "No parsed timing for process: job/after/round-2"):
            self.summary(root)
        self.assertFalse((root / "summary.json").exists())

    def test_dimensions_and_iteration_checksums_are_not_pooled(self):
        rows = [sample(width=10, iteration=0, checksum=11),
                sample(width=10, iteration=1, checksum=12),
                sample(width=20, iteration=0, checksum=21)]
        summary = self.summary(self.campaign(rows=rows))
        self.assertEqual(len(summary), 2)
        self.assertEqual(sorted(row["before_samples"] for row in summary), [1, 2])

    def test_memory_phases_preserve_scenario_and_dimensions(self):
        rows = [sample(width=10), sample(width=20)]
        rows += [{"record": "phase", "phase": "live", "scenario": "copy", "width": width,
                  "height": 10, "heap_live_bytes": width * 100, "rss_bytes": None}
                 for width in (10, 20)]
        root = self.campaign(rows=rows)
        self.summary(root)
        metrics = json.loads((root / "memory-comparison.json").read_text())
        heap = [row for row in metrics if row["workload"].endswith("heap_live_bytes")]
        self.assertEqual(len(heap), 2)
        self.assertEqual(sorted(row["before_bytes"] for row in heap), [1000, 2000])
        self.assertFalse(any(row["workload"].endswith(":rss_bytes") for row in metrics))

    @staticmethod
    def checkpoint(value, phase="encoding"):
        return {"record": "phase", "phase": phase, "scenario": "copy", "width": 10,
                "height": 10, "heap_live_bytes": value, "rss_bytes": value * 10}

    def test_repeated_memory_phase_requires_explicit_opt_in_and_preserves_raw_inputs(self):
        root = self.campaign(rows=[sample(), self.checkpoint(100), self.checkpoint(300)])
        raw_paths = [root / "metadata.json", root / "manifest.jsonl", *root.glob("runs/*/*/*/*")]
        raw = {path: path.read_bytes() for path in raw_paths}
        with self.assertRaisesRegex(ValueError, "Duplicate memory case"):
            self.summary(root)
        self.summary(root, sampled_memory_phases=("encoding",))
        memory = json.loads((root / "memory-comparison.json").read_text())
        heap = next(row for row in memory if row["workload"].endswith("encoding_max:heap_live_bytes"))
        self.assertEqual((heap["before_bytes"], heap["after_bytes"]), (300, 300))
        provenance = json.loads((root / "summary-provenance.json").read_text())
        self.assertEqual(provenance["sampled_memory_phases"], ["encoding"])
        self.assertTrue(all(path.read_bytes() == content for path, content in raw.items()))

    def test_ordinary_memory_duplicates_fail_even_with_sampled_phase_opt_in(self):
        for second in (100, 300):
            with self.subTest(second=second):
                root = self.campaign(name=f"ordinary-{second}", rows=[sample(),
                                     self.checkpoint(100, "live"), self.checkpoint(second, "live")])
                with self.assertRaisesRegex(ValueError, "Duplicate memory case"):
                    self.summary(root, sampled_memory_phases=("encoding",))

    def test_sampled_memory_uses_process_max_before_median_with_unequal_checkpoint_counts(self):
        values = {"before": [[100], [10, 200], [30] * 9 + [300]],
                  "after": [[2, 3], [400], [150] * 10 + [600]]}
        root = self.campaign(rounds=3, rows=lambda version, index: [sample(), *[
            self.checkpoint(value) for value in values[version][index - 1]]])
        self.summary(root, sampled_memory_phases=("encoding",))
        memory = json.loads((root / "memory-summary.json").read_text())
        heap = [row for row in memory if row["workload"].endswith("encoding_max:heap_live_bytes")]
        self.assertEqual([row["runs_bytes"] for row in heap], [[100, 200, 300], [3, 400, 600]])
        self.assertEqual([row["median_bytes"] for row in heap], [200, 400])
        self.assertTrue(all(row["runs"] == 3 for row in heap))

    def test_more_checkpoints_cannot_change_each_process_median_weight(self):
        maxima = {"before": [100, 200, 300], "after": [50, 400, 600]}
        counts = {"before": [100, 1, 1], "after": [1, 1, 100]}
        root = self.campaign(rounds=3, rows=lambda version, index: [sample(), *[
            self.checkpoint(maxima[version][index - 1])
            for _ in range(counts[version][index - 1])]])
        self.summary(root, sampled_memory_phases=("encoding",))
        memory = json.loads((root / "memory-comparison.json").read_text())
        heap = next(row for row in memory if row["workload"].endswith("encoding_max:heap_live_bytes"))
        self.assertEqual((heap["before_bytes"], heap["after_bytes"]), (200, 400))
        self.assertEqual((heap["before_runs"], heap["after_runs"]), (3, 3))

    def test_sampled_phase_max_suffix_cannot_collide_with_an_ordinary_phase(self):
        for phases in (("encoding", "encoding_max"), ("encoding_max", "encoding")):
            with self.subTest(phases=phases):
                root = self.campaign(name="-".join(phases), rows=[sample(), *[
                    self.checkpoint(100, phase) for phase in phases]])
                with self.assertRaisesRegex(ValueError, "Duplicate memory case"):
                    self.summary(root, sampled_memory_phases=("encoding",))

    def test_sampled_memory_phase_cli_option_is_repeatable(self):
        with mock.patch.object(sys, "argv", ["summary", "campaign", "--sampled-memory-phase",
                                            "encoding", "--sampled-memory-phase", "preview"]), \
             mock.patch.object(REPORT, "summarize", return_value=(1, 2)) as summarize, \
             contextlib.redirect_stdout(io.StringIO()):
            REPORT.main()
        self.assertEqual(summarize.call_args.args[-1], ["encoding", "preview"])

    def test_checksum_mismatch_fails_and_preserves_evidence(self):
        root = self.campaign(rows=lambda version, _: [sample(checksum=1 if version == "before" else 2)])
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.summary(root)
        self.assertTrue(json.loads((root / "checksum-mismatches.json").read_text()))
        self.assertFalse((root / "summary.json").exists())

    def test_changed_case_identity_fails_pairing(self):
        root = self.campaign(rows=lambda version, _: [sample(width=10 if version == "before" else 20)])
        with self.assertRaisesRegex(ValueError, "Unpaired"):
            self.summary(root)

    def test_missing_checksum_on_one_version_rejected(self):
        root = self.campaign(rows=lambda version, _: [sample(checksum=42 if version == "before" else 0)])
        with self.assertRaisesRegex(ValueError, "Unpaired output checksum"):
            self.summary(root)

    def test_unequal_samples_rejected_without_silently_pooling(self):
        root = self.campaign(rows=lambda version, _: [
            sample(iteration=index, checksum=0) for index in range(2 if version == "before" else 1)])
        with self.assertRaisesRegex(ValueError, "Unequal within-process sample"):
            self.summary(root)

    def test_duplicate_manifest_cannot_replace_a_missing_process(self):
        root = self.campaign()
        lines = (root / "manifest.jsonl").read_text().splitlines()
        (root / "manifest.jsonl").write_text(lines[0] + "\n" + lines[0] + "\n")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.summary(root)

    def test_failed_process_and_unfinished_integrity_check_rejected(self):
        root = self.campaign()
        path = root / "manifest.jsonl"
        records = [json.loads(line) for line in path.read_text().splitlines()]
        records[0]["exit_code"] = 1
        path.write_text("\n".join(map(json.dumps, records)))
        with self.assertRaisesRegex(ValueError, "failed process"):
            self.summary(root)
        metadata_path = root / "metadata.json"
        metadata = json.loads(metadata_path.read_text())
        metadata["completed"] = False
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "integrity check"):
            self.summary(root)

    def test_missing_artifact_fails(self):
        root = self.campaign()
        next(root.glob("runs/*/*/*/*.stderr")).unlink()
        with self.assertRaisesRegex(ValueError, "Missing stdout/stderr"):
            self.summary(root)

    def test_overlay_replaces_whole_job_including_extra_primary_rounds(self):
        primary = self.campaign(rounds=3, rows=[sample(elapsed_ns=9000000)])
        overlay = self.campaign(name="overlay", rounds=1, rows=[sample(elapsed_ns=2000000)])
        summary = self.summary(primary, [overlay])
        self.assertEqual(summary[0]["before_runs"], 1)
        self.assertEqual(summary[0]["before_ms"], 2)

    def test_named_sidecar_does_not_pool_distinct_filter_rows(self):
        csv_text = ("scenario,width,height,p50_ms,p95_ms,samples,checksum,retained_bytes\n"
                    "gaussian,10,10,1,2,20,101,1000\n"
                    "gaussian,20,10,3,4,30,202,2000\n")
        root = self.campaign(rows="human table", sidecars={"filter.csv": csv_text})
        summary = self.summary(root)
        self.assertEqual(len(summary), 2)
        self.assertEqual(sorted(row["before_samples"] for row in summary), [20, 30])

    def test_json_reports_preserve_dimensions_and_samples(self):
        data = {"samples": 20, "scenarios": [
            {"scenario": "reader", "width": width, "height": 10,
             "output_sha256": str(width), "create": {"p50_ms": width, "p95_ms": width + 1}}
            for width in (10, 20)]}
        summary = self.summary(self.campaign(rows=json.dumps(data)))
        self.assertEqual(len(summary), 2)
        self.assertTrue(all(row["after_samples"] == 20 for row in summary))

    def test_identical_stdout_and_skin_json_are_one_report_per_process(self):
        skin = json.dumps({"decode": {"median_ms": 1},
                           "preparation": [{"logical_width": 10, "logical_height": 10,
                                            "dpr": 1, "blur": 0, "mode": "solid", "median_ms": 2}],
                           "cached_paint": [{"physical_width": 10, "physical_height": 10,
                                             "median_ms": 3, "p95_ms": 4}]})
        root = self.campaign(rows=skin, sidecars={"skin.json": skin})
        summary = self.summary(root)
        self.assertEqual(len(summary), 3)
        self.assertTrue(all(row["before_runs"] == row["after_runs"] == 1 for row in summary))
        provenance = json.loads((root / "summary-provenance.json").read_text())
        self.assertEqual(len(provenance["duplicate_reports"]), 2)
        memory = json.loads((root / "memory-summary.json").read_text())
        self.assertEqual(len(memory), 2)
        self.assertTrue(all(row["median_bytes"] == 123456 for row in memory))

    def test_matching_metrics_in_different_full_reports_are_not_deduplicated(self):
        data = {"samples": 20, "scenarios": [
            {"scenario": "reader", "width": 10, "height": 10,
             "create": {"p50_ms": 1, "p95_ms": 2}}]}
        different = dict(data, provenance="different report")
        root = self.campaign(rows=json.dumps(data), sidecars={"report.json": json.dumps(different)})
        with self.assertRaisesRegex(ValueError, "Duplicate timing"):
            self.summary(root)

    def test_zero_timings_do_not_produce_infinite_ratios(self):
        summary = self.summary(self.campaign(rows=[sample(elapsed_ns=0)]))
        self.assertEqual(summary[0]["change_percent"], "")

    def test_process_p95_values_are_not_pooled(self):
        root = self.campaign(rounds=3, rows=lambda _, index: [
            sample(iteration=0, elapsed_ns=index * 1000000),
            sample(iteration=1, elapsed_ns=index * 2000000)])
        row = self.summary(root)[0]
        self.assertEqual(row["before_ms"], 3)
        self.assertEqual(row["before_p95_ms"], 4)
        self.assertEqual(row["before_run_min_ms"], 1.5)
        self.assertEqual(row["before_run_max_ms"], 4.5)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.plan = {"versions": {label: {"revision": label, "cwd": str(self.root)}
                                  for label in ("before", "after")}, "jobs": [
            {"name": name, "commands": {label: [sys.executable, "--version"]
                                         for label in ("before", "after")}}
            for name in ("first", "second")]}

    def test_invalid_plan_cannot_silently_skip_a_version(self):
        del self.plan["jobs"][0]["commands"]["after"]
        with self.assertRaisesRegex(ValueError, "every version"):
            RUNNER.validate_plan(self.plan)

    def test_different_arguments_need_explicit_reason(self):
        self.plan["jobs"][0]["commands"]["after"].append("different")
        with self.assertRaisesRegex(ValueError, "different workload"):
            RUNNER.validate_plan(self.plan)
        self.plan["jobs"][0]["argument_difference_reason"] = "implementation mode qt/pages"
        RUNNER.validate_plan(self.plan)

    def test_duplicate_and_unsafe_job_names_rejected(self):
        self.plan["jobs"][1]["name"] = "first"
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            RUNNER.validate_plan(self.plan)
        self.plan["jobs"][1]["name"] = "../outside"
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            RUNNER.validate_plan(self.plan)

    def run_mocked(self, effect=None, extra=()):
        plan_path = self.root / "plan.json"
        plan_path.write_text(json.dumps(self.plan))
        output = self.root / "results"
        arguments = ["runner", "--plan", str(plan_path), "--output", str(output), "--rounds", "2", *extra]
        with mock.patch.object(sys, "argv", arguments), \
             mock.patch.object(RUNNER.platform, "platform", return_value="test-platform"), \
             mock.patch.object(RUNNER.platform, "machine", return_value="test-architecture"), \
             mock.patch.object(RUNNER.subprocess, "run", side_effect=effect,
                               return_value=mock.Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            RUNNER.main()
        return output

    def test_jobs_are_adjacent_and_reverse_order_with_isolated_artifacts(self):
        root = self.run_mocked()
        records = [json.loads(line) for line in (root / "manifest.jsonl").read_text().splitlines()]
        self.assertEqual([(r["round"], r["version"], r["workload"]) for r in records], [
            (1, "before", "first"), (1, "after", "first"),
            (1, "before", "second"), (1, "after", "second"),
            (2, "after", "first"), (2, "before", "first"),
            (2, "after", "second"), (2, "before", "second")])
        self.assertEqual(len({r["artifact_directory"] for r in records}), 8)
        self.assertTrue(json.loads((root / "metadata.json").read_text())["completed"])

    def test_only_selection_is_preserved_in_campaign_metadata(self):
        root = self.run_mocked(extra=("--only", "second"))
        metadata = json.loads((root / "metadata.json").read_text())
        self.assertEqual([job["name"] for job in metadata["plan"]["jobs"]], ["second"])

    def test_launch_failure_is_recorded_and_never_marked_complete(self):
        with self.assertRaisesRegex(SystemExit, "failed"):
            self.run_mocked(effect=OSError("launch failed"))
        root = self.root / "results"
        record = json.loads((root / "manifest.jsonl").read_text())
        self.assertEqual(record["exit_code"], 127)
        self.assertIn("launch failed", record["error"])
        self.assertFalse(json.loads((root / "metadata.json").read_text())["completed"])


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.builds = [self.root / label for label in ("before", "after")]
        for directory in self.builds:
            directory.mkdir()
            (directory / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\nSNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\n")

    def generate(self, extra=()):
        output = self.root / "plan.json"
        arguments = ["generator", "--before-build", str(self.builds[0]), "--after-build",
                     str(self.builds[1]), "--before-revision", "old", "--after-revision", "new",
                     "--output", str(output), *extra]
        with mock.patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
            GENERATOR.main()
        return json.loads(output.read_text())

    def test_every_generated_rust_scenario_is_supported_by_its_example(self):
        before, after = self.root / "rust-before", self.root / "rust-after"
        plan = self.generate(("--before-rust", str(before), "--after-rust", str(after)))
        examples = {path.stem: path for path in
                    (ROOT.parent / "snow-crates/crates").glob("*/examples/memory_*benchmark.rs")}
        rust_jobs = [job for job in plan["jobs"] if job["name"].startswith("rust-")]
        self.assertEqual(len(rust_jobs), 19)
        selected = {}
        for job in rust_jobs:
            command = job["commands"]["before"]
            binary = Path(command[0]).name
            self.assertIn(binary, examples)
            supported = set(re.findall(r'"((?:capture|stitch|recording|ocr|scrolling)-[a-z-]+|overwrite|preserve)"',
                                       examples[binary].read_text()))
            if binary == "memory_overwrite_benchmark":
                supported.discard("capture-full-overwrite")
            self.assertIn(command[1], supported)
            selected.setdefault(binary, set()).add(command[1])
        for binary, scenarios in selected.items():
            supported = set(re.findall(r'"((?:capture|stitch|recording|ocr|scrolling)-[a-z-]+|overwrite|preserve)"',
                                       examples[binary].read_text()))
            if binary == "memory_overwrite_benchmark":
                supported.discard("capture-full-overwrite")
            self.assertEqual(scenarios, supported)

    def test_production_overwrite_reference_and_export_paths_are_paired(self):
        plan = self.generate(("--before-rust", str(self.root / "rust-before"),
                              "--after-rust", str(self.root / "rust-after"), "--samples", "23"))
        jobs = {job["name"]: job for job in plan["jobs"]}
        for name, argument, samples in (
            ("rust-capture-full-overwrite", "overwrite", "23"),
            ("rust-capture-full-preserve", "preserve", "23"),
            ("rust-stitch-reference-vertical", "stitch-reference-vertical", "23"),
            ("rust-stitch-reference-horizontal", "stitch-reference-horizontal", "23"),
            ("rust-scrolling-png-export", "scrolling-png-export", "7"),
        ):
            before, after = jobs[name]["commands"].values()
            self.assertEqual(before[1:], [argument, samples])
            self.assertEqual(after[1:], before[1:])

    def test_all_jobs_pair_identical_arguments_and_absolute_executables(self):
        plan = self.generate(("--samples", "23"))
        names = [job["name"] for job in plan["jobs"]]
        self.assertEqual(len(names), len(set(names)))
        for job in plan["jobs"]:
            before, after = job["commands"]["before"], job["commands"]["after"]
            self.assertTrue(Path(before[0]).is_absolute())
            self.assertTrue(Path(after[0]).is_absolute())
            self.assertEqual(before[1:], after[1:])
            self.assertTrue(all(isinstance(argument, str) for argument in before))

    def test_debug_packaging_and_partial_cache_tokens_are_rejected(self):
        for build_type, benchmarks in (("Debug", "ON"), ("Release", "OFF"),
                                       ("ReleaseWithDebInfo", "ON"), ("Release", "ON_INVALID")):
            with self.subTest(build_type=build_type, benchmarks=benchmarks):
                (self.builds[0] / "CMakeCache.txt").write_text(
                    f"CMAKE_BUILD_TYPE:STRING={build_type}\nSNOW_APPS_BUILD_BENCHMARKS:BOOL={benchmarks}\n")
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    self.generate()

    def test_comments_cannot_spoof_performance_cache_validation(self):
        (self.builds[0] / "CMakeCache.txt").write_text(
            "//CMAKE_BUILD_TYPE:STRING=Release\n//SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\nSNOW_APPS_BUILD_BENCHMARKS:BOOL=OFF\n")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.generate()

    def test_missing_cache_and_one_sided_rust_path_have_clear_errors(self):
        (self.builds[0] / "CMakeCache.txt").unlink()
        with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
            self.generate()
        self.assertIn("Cannot read configured build cache", errors.getvalue())
        with contextlib.redirect_stderr(io.StringIO()) as errors, self.assertRaises(SystemExit):
            self.generate(("--before-rust", str(self.root / "rust")))
        self.assertIn("Supply both", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
