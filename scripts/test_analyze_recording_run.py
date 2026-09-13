"""Offscreen checks for endurance diagnostics; windows are not statistical trials."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "recording_analysis", Path(__file__).with_name("analyze-recording-run.py"))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class RecordingDiagnosticsTests(unittest.TestCase):
    def test_static_frame_across_windows_is_not_counted_as_fresh_again(self):
        frames = [dict(pts=pts, identifier=identifier, valid=valid)
                  for pts, identifier, valid in [(0, 1, 1), (5, 1, 1), (10, 1, 1),
                                                  (11, 2, 1), (19, 3, 0), (20, 4, 1)]]
        windows, regressions = analysis.cadence_windows(frames, 1, 20, 10)
        self.assertEqual([window["fresh_frames"] for window in windows], [1, 1])
        self.assertEqual([window["unreadable_ids"] for window in windows], [0, 1])
        self.assertEqual(regressions, 0)

    def test_partial_final_window_and_pts_regression(self):
        frames = [dict(pts=pts, identifier=pts, valid=1) for pts in [0, 20, 19, 21]]
        windows, regressions = analysis.cadence_windows(frames, 1, 22, 10)
        self.assertEqual(windows[-1]["fresh_fps"], 1)
        self.assertEqual(regressions, 1)

    def test_memory_slope_excludes_initialization_and_reports_growth(self):
        samples = [dict(seconds=0, private_bytes=999 * 1048576)]
        samples += [dict(seconds=second, private_bytes=(100 + second) * 1048576)
                    for second in [10, 20, 30, 40]]
        report = analysis.memory_diagnostics(samples, 40)
        self.assertEqual(report["private_slope_mib_per_minute"], 60)
        self.assertEqual(report["private_max_mib"], 130)


if __name__ == "__main__":
    unittest.main()
