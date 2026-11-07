# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Deterministic fault injection for the Image visual comparison gates."""
import importlib.util
import json
import tempfile
import unittest
import sys
import argparse
from unittest.mock import patch
from pathlib import Path

from PIL import Image, ImageDraw, ImageChops

spec = importlib.util.spec_from_file_location("image_comparator", Path(__file__).with_name("compare_image_visual.py"))
comparator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparator)

actual_browser = None
raster_report = None


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ant-image-comparator-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.browser, self.native = self.root / "browser", self.root / "native"
        self.browser.mkdir()
        self.native.mkdir()
        box = {"x": 10, "y": 10, "width": 60, "height": 40}
        self.case = {"name": "sample", "scale": 1, "scene": "thumbnails", "width": 100, "height": 80,
                     "theme": "default", "direction": "ltr",
                     "rows": [{"key": "image-0", "root": box}], "preview": None}
        self.matrix_patch = patch.object(comparator, "canonical_cases", return_value={
            "sample": ("thumbnails", "default", "ltr", 1, 100, 80)})
        self.matrix_patch.start()
        self.addCleanup(self.matrix_patch.stop)
        report = {"renderer": "test", "provenance": {"revision": "test", "imageVersion": "test"}, "cases": [self.case]}
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        self.native_case = json.loads(json.dumps(self.case))
        self.write_native()
        image = Image.new("RGB", (100, 80), "white")
        image.paste((22, 119, 255), (10, 10, 70, 50))
        image.save(self.browser / "sample.png")
        image.save(self.native / "sample.png")

    def write_native(self):
        (self.native / "native-metrics.json").write_text(json.dumps([self.native_case]), encoding="utf-8")

    def run_comparison(self):
        return comparator.compare(self.browser, self.native, self.root / "report.json")

    def test_matching_frame_passes(self):
        self.assertEqual(self.run_comparison()["status"], "passed")

    def test_missing_image_pixels_fail_despite_matching_geometry(self):
        Image.new("RGB", (100, 80), "white").save(self.native / "sample.png")
        self.assertIn("image_crop_pixels", {failure["error"] for failure in self.run_comparison()["failures"]})

    def test_shifted_native_geometry_fails(self):
        self.native_case["rows"][0]["root"]["x"] += 2
        self.write_native()
        self.assertEqual(self.run_comparison()["status"], "failed")

    def test_wrong_dpi_is_rejected(self):
        self.native_case["scale"] = 2
        self.write_native()
        with self.assertRaises(AssertionError):
            self.run_comparison()

    def test_mismatching_physical_dimensions_are_rejected(self):
        Image.new("RGB", (101, 80), "white").save(self.native / "sample.png")
        with self.assertRaises(AssertionError):
            self.run_comparison()

    def test_missing_case_is_rejected(self):
        (self.native / "native-metrics.json").write_text("[]", encoding="utf-8")
        with self.assertRaises(AssertionError):
            self.run_comparison()

    def test_duplicate_capture_names_are_rejected(self):
        (self.native / "native-metrics.json").write_text(json.dumps([self.native_case, self.native_case]), encoding="utf-8")
        with self.assertRaises(AssertionError):
            self.run_comparison()

    def test_duplicate_browser_names_are_rejected(self):
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case, self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaises(AssertionError):
            self.run_comparison()

    def test_jointly_truncated_matrix_cannot_publish_summary(self):
        self.matrix_patch.stop()
        provenance = self.provenance()
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_canonical_name_cannot_hide_altered_capture_metadata(self):
        self.case["theme"] = "compact"
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_summary_requires_verified_native_provenance(self):
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def provenance(self):
        executable = self.root / "fixture.exe"
        executable.write_bytes(b"synthetic comparator fixture")
        compiler_dep = self.root / "fixture.d"
        crate_root = Path(comparator.__file__).resolve().parent.parent
        compiler_dep.write_text("# env-dep:CARGO_MANIFEST_DIR=" + str(crate_root).replace("\\", "\\\\") + "\n", encoding="utf-8")
        return {"status": "passed", "build_verified": True, "sources_unchanged_during_run": True,
                "metrics_sha256": comparator.digest(self.native / "native-metrics.json"),
                "executable": str(executable), "executable_sha256": comparator.digest(executable),
                "source_sha256": {name: comparator.digest(crate_root / name) for name in comparator.required_native_sources()},
                "linked_library": {"dependency_info": str(compiler_dep),
                    "dependency_info_sha256": comparator.digest(compiler_dep)}}

    def test_summary_accepts_matching_binary_source_and_compiler_manifest_binding(self):
        provenance = self.provenance()
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        result = comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")
        self.assertEqual(result["status"], "passed")

    def test_summary_rejects_library_compiled_from_another_checkout(self):
        provenance = self.provenance()
        compiler_dep = Path(provenance["linked_library"]["dependency_info"])
        compiler_dep.write_text("# env-dep:CARGO_MANIFEST_DIR=" + str(self.root).replace("\\", "\\\\") + "\n", encoding="utf-8")
        provenance["linked_library"]["dependency_info_sha256"] = comparator.digest(compiler_dep)
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_summary_rejects_changed_implementation_source(self):
        provenance = self.provenance()
        provenance["source_sha256"]["scripts/compare_image_visual.py"] = "0" * 64
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_summary_rejects_missing_implementation_source_map(self):
        provenance = self.provenance()
        provenance["source_sha256"] = {}
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "native_provenance_missing_source_hashes"):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_summary_rejects_changed_binary(self):
        provenance = self.provenance()
        provenance["executable_sha256"] = "0" * 64
        (self.native / "native-provenance.json").write_text(json.dumps(provenance), encoding="utf-8")
        with self.assertRaises(AssertionError):
            comparator.compare(self.browser, self.native, self.root / "report.json", self.root / "summary.json")

    def test_missing_control_glyph_fails_even_when_large_image_crop_is_close(self):
        box = {"x": 10, "y": 10, "width": 60, "height": 40}
        preview = {"image": box, "actions": box, "transform": {"scale": 1},
                   "styles": {"operationSize": "18px"},
                   "controls": [{"name": "close", "bounds": {"x": 30, "y": 25, "width": 26, "height": 26}}]}
        self.case["preview"] = preview
        self.native_case["preview"] = json.loads(json.dumps(preview))
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        self.write_native()
        frame = Image.open(self.browser / "sample.png").convert("RGB")
        draw = ImageDraw.Draw(frame)
        draw.line([(37, 32), (49, 44)], fill="white", width=2)
        draw.line([(49, 32), (37, 44)], fill="white", width=2)
        frame.save(self.browser / "sample.png")
        failures = self.run_comparison()["failures"]
        self.assertIn("missing_control_glyph", {failure["error"] for failure in failures})

    def test_single_item_group_count_requires_actual_painted_label(self):
        preview = {"image": {"x": 10, "y": 10, "width": 60, "height": 40},
                   "actions": {"x": 20, "y": 70, "width": 60, "height": 10},
                   "footer": {"x": 20, "y": 52, "width": 60, "height": 28},
                   "count": "1 / 1", "transform": {"scale": 1},
                   "styles": {"operationSize": "18px"}, "controls": []}
        self.case["preview"] = preview
        self.native_case["preview"] = json.loads(json.dumps(preview))
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        self.write_native()
        frame = Image.open(self.browser / "sample.png").convert("RGB")
        ImageDraw.Draw(frame).text((42, 53), "1 / 1", fill="black")
        frame.save(self.browser / "sample.png")
        self.assertIn("missing_count_label", {failure["error"] for failure in self.run_comparison()["failures"]})

    def test_full_frame_error_fraction_is_not_replaced_by_count_crop_histogram(self):
        preview = {"image": {"x": 10, "y": 10, "width": 60, "height": 40},
                   "actions": {"x": 20, "y": 70, "width": 60, "height": 10},
                   "footer": {"x": 20, "y": 52, "width": 60, "height": 28},
                   "count": "1 / 1", "transform": {"scale": 1},
                   "styles": {"operationSize": "18px"}, "controls": []}
        self.case["preview"] = preview
        self.native_case["preview"] = json.loads(json.dumps(preview))
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        self.write_native()
        frame = Image.open(self.native / "sample.png").convert("RGB")
        frame.paste("black", (80, 10, 90, 20))
        frame.save(self.native / "sample.png")
        result = self.run_comparison()
        self.assertEqual(result["cases"][0]["fraction_pixels_with_channel_error_above_32"], 100 / 8000)

    def test_shape_policy_rejects_faint_wrong_glyph_even_when_raw_error_is_under_twelve(self):
        preview = {"image": {"x": 10, "y": 10, "width": 60, "height": 40},
                   "actions": {"x": 20, "y": 20, "width": 60, "height": 24},
                   "transform": {"scale": 1}, "styles": {"operationSize": "18px"},
                   "controls": [{"name": "zoomIn", "bounds": {"x": 30, "y": 20, "width": 24, "height": 24}}]}
        self.case["preview"] = preview
        self.native_case["preview"] = json.loads(json.dumps(preview))
        report = json.loads((self.browser / "browser-metrics.json").read_text(encoding="utf-8"))
        report["cases"] = [self.case]
        (self.browser / "browser-metrics.json").write_text(json.dumps(report), encoding="utf-8")
        self.write_native()
        for directory, plus in [(self.browser, True), (self.native, False)]:
            frame = Image.open(directory / "sample.png").convert("RGB")
            draw = ImageDraw.Draw(frame)
            draw.line((37, 32, 47, 32), fill=(39, 136, 255), width=1)
            if plus:
                draw.line((42, 27, 42, 37), fill=(39, 136, 255), width=1)
            frame.save(directory / "sample.png")
        strict = self.run_comparison()
        self.assertEqual(strict["status"], "passed")
        self.assertLess(strict["cases"][0]["control_glyph_presence"][0]["mean_absolute_channel_error"], 12)
        candidate = comparator.compare(self.browser, self.native, self.root / "candidate.json", glyph_raster_policy="shape")
        self.assertIn("control_glyph_shape", {failure["error"] for failure in candidate["failures"]})


class RasterDiagnosticTests(unittest.TestCase):
    def glyph(self, kind="plus", color=(180, 180, 180)):
        image = Image.new("RGB", (24, 24), (10, 10, 10))
        draw = ImageDraw.Draw(image)
        draw.line((5, 12, 18, 12), fill=color, width=2)
        if kind == "plus":
            draw.line((12, 5, 12, 18), fill=color, width=2)
        return image

    def metrics(self, expected, native):
        return comparator.glyph_raster_metrics(expected, native, (10, 10, 10), (10, 10, 10))

    def test_identical_shape_passes_candidate(self):
        self.assertTrue(self.metrics(self.glyph(), self.glyph())["candidate_acceptable"])

    def test_one_physical_pixel_phase_passes_candidate(self):
        expected = self.glyph()
        self.assertTrue(self.metrics(expected, ImageChops.offset(expected, 1, 0))["candidate_acceptable"])

    def test_two_physical_pixel_movement_is_rejected(self):
        expected = self.glyph()
        result = self.metrics(expected, ImageChops.offset(expected, 2, 0))
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["centroid"])

    def test_missing_glyph_is_rejected(self):
        self.assertFalse(self.metrics(self.glyph(), Image.new("RGB", (24, 24), (10, 10, 10)))["candidate_acceptable"])

    def test_faint_glyph_identity_passes_but_missing_shape_is_rejected(self):
        faint = self.glyph(color=(16, 16, 16))
        self.assertTrue(self.metrics(faint, faint)["candidate_acceptable"])
        self.assertFalse(self.metrics(faint, Image.new("RGB", (24, 24), (10, 10, 10)))["candidate_acceptable"])

    def test_faint_wrong_glyph_is_rejected(self):
        self.assertFalse(self.metrics(self.glyph(color=(16, 16, 16)), self.glyph("minus", color=(16, 16, 16)))["candidate_acceptable"])

    def test_one_level_fringe_quantization_uses_integrated_ink_not_thresholded_count(self):
        expected = Image.new("RGB", (24, 24), (10, 10, 10))
        ImageDraw.Draw(expected).rectangle((11, 11, 12, 12), fill=(27, 27, 27))
        native = expected.copy()
        for position in [(10, 11), (10, 12), (11, 10), (12, 10), (13, 11), (13, 12), (11, 13), (12, 13)]:
            expected.putpixel(position, (13, 13, 13))
            native.putpixel(position, (14, 14, 14))
        result = self.metrics(expected, native)
        self.assertTrue(result["candidate_acceptable"])
        self.assertGreater(result["thresholded_ink_ratio"], 1.33)
        self.assertAlmostEqual(result["weighted_ink_ratio"], 100 / 92)

    def test_wrong_plus_minus_shape_is_rejected(self):
        result = self.metrics(self.glyph(), self.glyph("minus"))
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["strong_shape_distance"])

    def test_wrong_glyph_color_is_rejected(self):
        result = self.metrics(self.glyph(), self.glyph(color=(180, 10, 10)))
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["color"])

    def test_inverted_glyph_and_background_are_rejected(self):
        expected = self.glyph()
        result = comparator.glyph_raster_metrics(expected, ImageChops.invert(expected), (10, 10, 10), (245, 245, 245))
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["color"])

    def test_faded_glyph_is_rejected(self):
        result = self.metrics(self.glyph(), self.glyph(color=(80, 80, 80)))
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["contrast"])

    def test_extra_ink_is_rejected(self):
        native = self.glyph()
        ImageDraw.Draw(native).rectangle((3, 3, 20, 20), fill=(180, 180, 180))
        self.assertFalse(self.metrics(self.glyph(), native)["candidate_acceptable"])

    def test_weak_background_outlier_is_reported_without_rejecting_shape(self):
        native = self.glyph()
        native.putpixel((2, 2), (24, 24, 24))
        result = self.metrics(self.glyph(), native)
        self.assertTrue(result["candidate_acceptable"])
        self.assertGreater(result["maximum_support_distance_physical_px"], 5)
        self.assertEqual(result["maximum_directional_p99_distance_physical_px"], 0)

    def test_isolated_strong_extra_pixel_is_rejected(self):
        expected = Image.new("RGB", (64, 64), (10, 10, 10))
        draw = ImageDraw.Draw(expected)
        draw.line((8, 32, 55, 32), fill=(180, 180, 180), width=6)
        draw.line((32, 8, 32, 55), fill=(180, 180, 180), width=6)
        native = expected.copy()
        native.putpixel((2, 2), (180, 180, 180))
        result = self.metrics(expected, native)
        self.assertFalse(result["candidate_acceptable"])
        self.assertEqual(result["maximum_directional_p99_distance_physical_px"], 0)
        self.assertFalse(result["checks"]["strong_shape_distance"])

    def test_accumulated_weak_outlier_ink_is_rejected_by_weighted_tail(self):
        native = self.glyph()
        ImageDraw.Draw(native).rectangle((1, 1, 4, 5), fill=(40, 40, 40))
        result = self.metrics(self.glyph(), native)
        self.assertFalse(result["candidate_acceptable"])
        self.assertFalse(result["checks"]["p95_shape_distance"])

    def test_permuted_genuine_browser_toolbar_glyphs_are_rejected(self):
        if actual_browser is None:
            self.skipTest("Pass --browser to validate genuine independently rendered toolbar permutations")
        report = json.loads((actual_browser / "browser-metrics.json").read_text(encoding="utf-8"))
        names = ["preview-landscape-default-ltr-100", "preview-transform-default-ltr-100",
                 "preview-transform-dark-compact-ltr-150", "mask-none-default-ltr-100",
                 "mask-none-default-ltr-150", "mask-none-default-ltr-200"]
        results, image_hashes, identity_count = [], {}, 0
        for name in names:
            case = next(case for case in report["cases"] if case["name"] == name)
            frame_path = actual_browser / (name + ".png")
            frame = Image.open(frame_path).convert("RGB")
            image_hashes[name] = comparator.digest(frame_path)
            preview, scale = case["preview"], case["scale"]
            glyph_size = float(preview["styles"]["operationSize"].removesuffix("px"))
            glyphs = []
            for control in preview["controls"]:
                bounds = control["bounds"]
                x, y = bounds["x"] + bounds["width"] / 2, bounds["y"] + bounds["height"] / 2
                box = tuple(round(value * scale) for value in (x - glyph_size / 2, y - glyph_size / 2,
                                                               x + glyph_size / 2, y + glyph_size / 2))
                background = frame.getpixel((round(x * scale), round((bounds["y"] + bounds["height"] - 4) * scale)))
                glyphs.append((control["name"], frame.crop(box), background))
            for expected_name, expected, expected_background in glyphs:
                for wrong_name, wrong, wrong_background in glyphs:
                    metrics = comparator.glyph_raster_metrics(expected, wrong, expected_background, wrong_background)
                    if expected_name == wrong_name:
                        identity_count += 1
                        self.assertTrue(metrics["candidate_acceptable"], (name, expected_name, metrics))
                    else:
                        results.append({"scene": name, "expected": expected_name, "wrong": wrong_name,
                                        "metrics": metrics})
                        self.assertFalse(metrics["candidate_acceptable"], (name, expected_name, wrong_name, metrics))
        if raster_report is not None:
            raster_report.parent.mkdir(parents=True, exist_ok=True)
            raster_report.write_text(json.dumps({"status": "passed", "policy_activated": False,
                "upstream_revision": report["provenance"]["revision"], "browser": report["renderer"],
                "reference_png_sha256": image_hashes, "correct_glyph_identities": identity_count,
                "wrong_actual_glyph_permutations": len(results), "permutations": results}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(add_help=False)
    arguments.add_argument("--browser", type=Path)
    arguments.add_argument("--raster-report", type=Path)
    custom, remaining = arguments.parse_known_args()
    actual_browser, raster_report = custom.browser, custom.raster_report
    unittest.main(argv=[sys.argv[0], *remaining])
