"""Deterministic checks for recording-level comparison and invalid samples."""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "recording_results", Path(__file__).with_name("compare-recording-results.py"))
results = importlib.util.module_from_spec(spec)
spec.loader.exec_module(results)


class RecordingComparisonTests(unittest.TestCase):
    def test_paired_direction_absolute_latency_and_invalid_measurements(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index in range(1, 11):
                for variant, fps, latency in [("baseline", 20, 40), ("candidate", 25, 50)]:
                    directory = root / f"{variant}-{index}"
                    directory.mkdir()
                    row = dict(useful_fps=fps, cpu_percent=100, private_peak_mib=300,
                               setup_ms=10, stop_ms=10, working_set_delta_mib=200,
                               unreadable_ids=0, dropped_capture_frames=3,
                               input_failures=0, workload_failures=int(index == 10 and variant == "candidate"))
                    with (directory / "realtime-recording-benchmark.csv").open("w", newline="") as stream:
                        writer = csv.DictWriter(stream, fieldnames=row)
                        writer.writeheader()
                        writer.writerow(row)
                    with (directory / "realtime-recording-stages.csv").open("w", newline="") as stream:
                        writer = csv.writer(stream)
                        writer.writerow(["stage", "count", "p50_ms", "p95_ms", "max_ms"])
                        writer.writerow(["pipeline.capture_to_packet", 100, latency, latency, latency])
            results.compare(root)
            report = json.loads((root / "comparison.json").read_text())
            self.assertFalse(report["measurement_valid"])
            self.assertEqual(report["invalid_samples"], ["candidate-10"])
            self.assertAlmostEqual(report["metrics"]["useful_fps"]["improvement_percent"], 25)
            latency = report["metrics"]["pipeline.capture_to_packet.p95_ms"]
            self.assertEqual(latency["delta_ci95_high"], 10)
            self.assertNotIn("improvement_percent", report["metrics"]["pipeline.capture_to_packet.count"])

    def test_incomplete_comparison_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                results.compare(Path(temporary))


if __name__ == "__main__":
    unittest.main()
