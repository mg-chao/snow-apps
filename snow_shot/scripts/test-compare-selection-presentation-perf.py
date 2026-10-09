#!/usr/bin/env python3
"""Generated-report contract tests for the smart-selection comparison utility."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("compare-selection-presentation-perf.py")
SPEC = importlib.util.spec_from_file_location("selection_presentation_comparison", SCRIPT)
COMPARISON = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARISON)


def distribution(mean):
    return {"min": mean * 0.5, "median": mean * 0.9, "mean": mean,
            "p95": mean * 1.5, "max": mean * 2}


def report(implementation, animation=False, cost_ms=1.5):
    case = {
        "scenario": "animation_frames_1" if animation else "changed_targets_1",
        "iterations": 40,
        "requests_per_frame": 1,
        "request_count": 5 if animation else 40,
        "clock": "real_monotonic" if animation else "controlled_monotonic",
        "frame_commit": "scheduled_timer" if animation else "explicit_frame",
        "input_interval_ms": 17 if animation else 8,
        "maximum_requested_animation_idle_wait_ms": 1 if animation else 0,
        "elapsed_ms": 680 if animation else 100,
        "preparation_ms": distribution(cost_ms * 0.2),
        "event_processing_ms": distribution(cost_ms * 0.8),
        "total_ms": distribution(cost_ms),
        "semantic_notifications": 5 if animation else 40,
        "hint_translation_requests": 0,
        "canvas_paints": 28 if animation else 40,
        "ui_paints": 8 if animation else 80,
        "displayed_geometry_changes": 32 if animation else 40,
        "mean_canvas_damage_ratio": 0.15,
    }
    return {
        "benchmark": "screenshot_selection_presentation",
        "platform": "windows",
        "implementation": implementation,
        "logical_width": 1800,
        "logical_height": 975,
        "physical_width": 3600,
        "physical_height": 1950,
        "device_pixel_ratio": 2.0,
        "warmup": 4,
        "screen_refresh_hz": 120.0,
        "animation_sample_interval_ms": 17,
        "results": [case],
    }


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.baseline_path = self.directory / "baseline.json"
        self.optimized_path = self.directory / "optimized.json"

    def save(self, baseline, optimized):
        self.baseline_path.write_text(json.dumps(baseline, allow_nan=False), encoding="utf-8")
        self.optimized_path.write_text(json.dumps(optimized, allow_nan=False), encoding="utf-8")

    def compare(self, baseline, optimized):
        self.save(baseline, optimized)
        return COMPARISON.compare_reports(self.baseline_path, self.optimized_path)

    def invoke(self, baseline, optimized, *arguments):
        self.save(baseline, optimized)
        return subprocess.run(
            [sys.executable, "-B", str(SCRIPT), str(self.baseline_path),
             str(self.optimized_path), *arguments],
            check=False, capture_output=True, text=True,
        )

    def test_matching_controlled_inputs_and_observed_work(self):
        baseline = report("baseline")
        optimized = report("optimized", cost_ms=0.6)
        comparison = self.compare(baseline, optimized)
        case = comparison["scenarios"][0]
        self.assertEqual(case["mode"], "controlled")
        self.assertTrue(case["committed_work"]["matched_controlled_work"])
        self.assertEqual(comparison["matched_configuration"]["screen_refresh_hz"], 120.0)
        self.assertEqual(comparison["matched_configuration"]["device_pixel_ratio"], 2.0)
        self.assertEqual(comparison["matched_configuration"]["warmup"], 4)
        self.assertAlmostEqual(case["costs_ms"]["total_ms"]["mean"]["reduction_percent"], 60.0)
        self.assertEqual(self.invoke(baseline, optimized, "--strict-work").returncode, 0)

    def test_incompatible_report_configuration_is_rejected(self):
        baseline = report("baseline")
        for key, value in (("platform", "offscreen"), ("warmup", 5),
                           ("screen_refresh_hz", 60.0), ("animation_sample_interval_ms", 8)):
            with self.subTest(field=key):
                optimized = report("optimized")
                optimized[key] = value
                with self.assertRaisesRegex(ValueError, f"configuration mismatch: {key}"):
                    self.compare(baseline, optimized)

    def test_current_metadata_is_required(self):
        for key in ("device_pixel_ratio", "warmup", "screen_refresh_hz"):
            with self.subTest(field=key):
                optimized = report("optimized")
                del optimized[key]
                with self.assertRaisesRegex(ValueError, f"missing {key}"):
                    self.compare(report("baseline"), optimized)
        optimized = report("optimized", animation=True)
        del optimized["results"][0]["maximum_requested_animation_idle_wait_ms"]
        with self.assertRaisesRegex(ValueError, "missing maximum_requested_animation_idle_wait_ms"):
            self.compare(report("baseline", animation=True), optimized)

    def test_reported_dpr_must_match_the_actual_surface_dimensions(self):
        optimized = report("optimized")
        optimized["physical_width"] = 1800
        with self.assertRaisesRegex(ValueError, "physical_width disagrees"):
            self.compare(report("baseline"), optimized)

    def test_incompatible_case_input_and_idle_wait_are_rejected(self):
        baseline = report("baseline", animation=True)
        for key, value in (("request_count", 6), ("input_interval_ms", 8),
                           ("maximum_requested_animation_idle_wait_ms", 2)):
            with self.subTest(field=key):
                optimized = report("optimized", animation=True)
                optimized["results"][0][key] = value
                with self.assertRaisesRegex(ValueError, f"input/configuration mismatch: {key}"):
                    self.compare(baseline, optimized)

    def test_controlled_count_difference_is_qualified_and_strict_mode_fails(self):
        baseline = report("baseline")
        optimized = report("optimized", cost_ms=0.6)
        optimized["results"][0]["canvas_paints"] = 20
        comparison = self.compare(baseline, optimized)
        case = comparison["scenarios"][0]
        self.assertFalse(case["committed_work"]["matched_controlled_work"])
        self.assertIn("different committed work", case["claim_basis"])
        result = self.invoke(baseline, optimized, "--strict-work")
        self.assertEqual(result.returncode, 2)
        self.assertIn("canvas_paints", result.stderr)

    def test_auxiliary_paint_reduction_keeps_renderer_work_comparable(self):
        baseline = report("baseline")
        optimized = report("optimized", cost_ms=0.6)
        optimized["results"][0]["ui_paints"] = 0
        case = self.compare(baseline, optimized)["scenarios"][0]
        self.assertTrue(case["committed_work"]["matched_controlled_work"])
        self.assertFalse(case["committed_work"]["ui_paints_match"])
        self.assertEqual(self.invoke(baseline, optimized, "--strict-work").returncode, 0)

    def test_animation_counts_can_exceed_input_count_without_a_frame_speedup_claim(self):
        baseline = report("baseline", animation=True)
        optimized = report("optimized", animation=True, cost_ms=2.0)
        optimized_case = optimized["results"][0]
        optimized_case["displayed_geometry_changes"] = 64
        optimized_case["canvas_paints"] = 56
        comparison = self.compare(baseline, optimized)
        case = comparison["scenarios"][0]
        self.assertEqual(case["mode"], "real_animation")
        self.assertFalse(case["committed_work"]["matched_controlled_work"])
        self.assertIn("not an equal-commit frame-speedup claim", case["claim_basis"])
        self.assertEqual(case["matched_inputs"]["maximum_requested_animation_idle_wait_ms"], 1)
        self.assertAlmostEqual(case["work_ms_per_observed_geometry_change"]["baseline"], 60 / 32)
        self.assertAlmostEqual(case["work_ms_per_observed_geometry_change"]["optimized"], 80 / 64)
        self.assertEqual(self.invoke(baseline, optimized, "--strict-work").returncode, 0)

    def test_legacy_animation_sampling_metadata_is_rejected(self):
        baseline = report("baseline", animation=True)
        optimized = report("optimized", animation=True)
        for source in (baseline, optimized):
            source["results"][0]["frame_commit"] = "platform_timer"
        with self.assertRaisesRegex(ValueError, "unknown clock/frame_commit combination"):
            self.compare(baseline, optimized)


if __name__ == "__main__":
    unittest.main()
