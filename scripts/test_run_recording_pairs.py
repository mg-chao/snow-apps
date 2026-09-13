"""Only resume complete valid pairs; preserve invalid pair artifacts intact."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "recording_pairs", Path(__file__).with_name("run-recording-pairs.py"))
pairs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pairs)


class RecordingPairResumeTests(unittest.TestCase):
    def test_stop_action_cannot_hide_the_observed_interference_or_shift_its_timestamp(self):
        observations = []
        competitors = [{"pid": 123, "name": "cmake.exe"}]

        def stop(active):
            self.assertEqual(active, competitors)
            self.assertEqual(observations, [{"unix_time": 10, "processes": competitors}])

        with patch.object(pairs.time, "time", return_value=10):
            pairs.record_competing_work(observations, competitors, stop)
        self.assertEqual(observations[0]["unix_time"], 10)

    def test_failed_optional_stop_preserves_interference_and_does_not_abandon_recording(self):
        observations = []

        def fail(_):
            raise OSError("process already exited")

        pairs.record_competing_work(observations, [{"pid": 123}], fail)
        self.assertEqual(observations[0]["processes"], [{"pid": 123}])
        self.assertEqual(observations[0]["interference_handler_error"], "process already exited")

    def test_offline_inspection_is_outside_recording_but_setup_and_drain_are_protected(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            observations = [{"unix_time": value} for value in [1, 10, 40, 40.5, 42]]
            self.assertEqual(pairs.recording_interference(output, observations), observations)
            boundary = output / "all-effects-sample-1.recording-window.csv"
            boundary.write_text("started_unix_seconds,stopped_unix_seconds\n10,40\n")
            self.assertEqual(pairs.recording_interference(output, observations), observations[:-1])
            for contents in ["", "started_unix_seconds,stopped_unix_seconds\n40,10\n",
                             "started_unix_seconds,stopped_unix_seconds\n10,nan\n"]:
                boundary.write_text(contents)
                self.assertEqual(pairs.recording_interference(output, observations), observations)

    def test_invalid_or_incomplete_pair_is_never_resumed_as_valid(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            self.assertFalse(pairs.completed_pair(output, 1))
            for name in ("baseline", "candidate"):
                directory = output / f"{name}-1"
                directory.mkdir()
                (directory / "exit-code.txt").write_text("0\n")
                (directory / "realtime-recording-benchmark.csv").write_text("preserved data\n")
            self.assertTrue(pairs.completed_pair(output, 1))
            marker = output / "candidate-1" / "measurement-invalid.json"
            marker.write_text('{"reason":"workload obstruction"}\n')
            self.assertFalse(pairs.completed_pair(output, 1))
            pairs.preserve_incomplete_pair(output, 1)
            self.assertFalse((output / "candidate-1").exists())
            self.assertFalse((output / "baseline-1").exists())
            archived, = (output / "invalid-attempts").iterdir()
            self.assertEqual((archived / "candidate-1" / "measurement-invalid.json").read_text(),
                             '{"reason":"workload obstruction"}\n')
            for name in ("baseline", "candidate"):
                self.assertEqual((archived / f"{name}-1" / "realtime-recording-benchmark.csv").read_text(),
                                 "preserved data\n")

    def test_failed_exit_is_invalid_even_when_a_summary_was_written(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            for name in ("baseline", "candidate"):
                directory = output / f"{name}-1"
                directory.mkdir()
                (directory / "exit-code.txt").write_text("1\n" if name == "candidate" else "0\n")
                (directory / "realtime-recording-benchmark.csv").write_text("data\n")
            self.assertFalse(pairs.completed_pair(output, 1))


if __name__ == "__main__":
    unittest.main()
