# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Negative controls for the ColorPicker visual comparison gate."""
import copy
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw

from compare_color_picker_visual import compare


class ColorPickerComparatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.reference_dir = Path(self.temporary.name) / "reference"
        self.native_dir = Path(self.temporary.name) / "native"
        self.reference_dir.mkdir()
        self.native_dir.mkdir()
        geometry = {"panel": None, "palette": {"x": 100, "y": 100, "width": 100, "height": 100},
                    "hue": None, "alpha": None, "preview": None, "gradient": None,
                    "editors": [], "presets": []}
        case = {"name": "probe-default-ltr-100", "scene": "probe", "theme": "default",
                "direction": "ltr", "scale": 1, "canvas": {"width": 640, "height": 720},
                "rows": [], "geometry": geometry, "elements": []}
        self.reference = {"cases": [case], "provenance": {"test": True}, "renderer": {}}
        self.native = [copy.deepcopy(case)]
        for directory in (self.reference_dir, self.native_dir):
            Image.new("RGB", (640, 720), (100, 100, 100)).save(directory / f"{case['name']}.png")

    def result(self):
        return compare(self.reference, self.native, self.reference_dir, self.native_dir)

    def test_identical_independent_inputs_pass(self):
        report = self.result()
        self.assertEqual(report["failed_geometry_checks"], 0)
        self.assertEqual(report["failed_surface_samples"], 0)
        self.assertEqual(report["surface_samples"], 20)

    def test_missing_or_duplicate_scenarios_rejected(self):
        for native in ([], self.native * 2):
            with self.assertRaises(ValueError):
                compare(self.reference, native, self.reference_dir, self.native_dir)

    def test_two_pixel_geometry_error_fails(self):
        self.native[0]["geometry"]["palette"]["x"] += 2
        self.assertEqual(self.result()["failed_geometry_checks"], 1)

    def test_missing_surface_fails_presence(self):
        self.native[0]["geometry"]["palette"] = None
        self.assertEqual(self.result()["failed_geometry_checks"], 1)

    def test_gradient_budget_is_enforced(self):
        image_path = self.native_dir / f"{self.native[0]['name']}.png"
        Image.new("RGB", (640, 720), (104, 100, 100)).save(image_path)
        self.assertEqual(self.result()["failed_surface_samples"], 0)
        Image.new("RGB", (640, 720), (105, 100, 100)).save(image_path)
        self.assertEqual(self.result()["failed_surface_samples"], 20)

    def test_declared_dpi_mismatch_rejected(self):
        self.native[0]["scale"] = 1.5
        with self.assertRaises(ValueError):
            self.result()

    def test_inactive_gradient_thumb_size_difference_fails(self):
        expected = {"kind": "gradient", "index": 1, "x": 100, "y": 100, "width": 9.6, "height": 9.6}
        self.reference["cases"][0]["geometry"]["handles"] = [expected]
        self.native[0]["geometry"]["handles"] = [dict(expected, x=98.8, y=98.8, width=12, height=12)]
        self.assertEqual(self.result()["failed_geometry_checks"], 1)

    def test_duplicate_handle_identity_rejected(self):
        handle = {"kind": "hue", "index": 0, "x": 100, "y": 100, "width": 12, "height": 12}
        self.reference["cases"][0]["geometry"]["handles"] = [handle]
        self.native[0]["geometry"]["handles"] = [handle, handle]
        with self.assertRaises(ValueError):
            self.result()

    def focused_source_handle(self):
        rail = {"x": 100, "y": 100, "width": 100, "height": 8}
        handle = {"kind": "hue", "index": 0, "x": 114, "y": 98, "width": 12, "height": 12}
        self.reference["cases"][0]["geometry"].update(hue=rail, handles=[handle, dict(handle, index=1)])
        self.native[0]["geometry"].update(hue=copy.deepcopy(rail), handles=[copy.deepcopy(handle)])
        self.reference["cases"][0]["elements"] = [
            {"className": "ant-slider-handle ant-slider-handle-1 ant-color-picker-slider-handle", "x": 116, "y": 100, "width": 8, "height": 8},
            {"className": "ant-slider-handle ant-color-picker-slider-handle", "x": 116, "y": 100, "width": 8, "height": 8},
        ]

    def test_coincident_source_accessibility_helper_is_reported(self):
        self.focused_source_handle()
        report = self.result()
        self.assertEqual(report["failed_geometry_checks"], 0)
        self.assertEqual(report["reference_helper_overlays"], 1)

    def test_distinct_source_helper_cannot_be_discarded(self):
        self.focused_source_handle()
        self.reference["cases"][0]["geometry"]["handles"][1]["width"] = 14
        with self.assertRaises(ValueError):
            self.result()

    def bordered_editor(self):
        rectangle = {"x": 100, "y": 100, "width": 100, "height": 24}
        self.reference["cases"][0]["geometry"]["editors"] = [dict(rectangle, borderWidth="1px", borderColor="rgb(100, 100, 100)")]
        self.native[0]["geometry"]["editors"] = [rectangle]
        self.native[0]["quads"] = [dict(rectangle, border_color="Hsla { h: 0.0, s: 0.0, l: 0.392156862745, a: 1.0 }", border_widths=[1, 1, 1, 1])]

    def test_resolved_border_color_budget_is_enforced(self):
        self.bordered_editor()
        self.assertEqual(self.result()["failed_border_checks"], 0)
        self.native[0]["quads"][0]["border_color"] = "Hsla { h: 0.0, s: 0.0, l: 0.4, a: 1.0 }"
        self.assertEqual(self.result()["failed_border_checks"], 1)

    def test_missing_native_border_primitive_fails(self):
        self.bordered_editor()
        self.native[0]["quads"] = []
        self.assertEqual(self.result()["failed_border_checks"], 1)

    def selected_mark(self, native_offset=0, native_color=(255, 255, 255), missing=False):
        rectangle = {"x": 250, "y": 250, "width": 24, "height": 24}
        self.reference["cases"][0]["geometry"]["presets"] = [rectangle]
        self.native[0]["geometry"]["presets"] = [copy.deepcopy(rectangle)]
        self.reference["cases"][0]["elements"] = [dict(rectangle,
            className="ant-color-picker-presets-color ant-color-picker-presets-color-checked",
            after={"borderColor": "rgb(255, 255, 255)"})]
        for directory, offset, color in ((self.reference_dir, 0, (255, 255, 255)),
                                         (self.native_dir, native_offset, native_color)):
            path = directory / f"{self.native[0]['name']}.png"
            image = Image.open(path)
            drawing = ImageDraw.Draw(image)
            drawing.rectangle((250, 250, 273, 273), fill=(22, 119, 255))
            if directory == self.reference_dir or not missing:
                drawing.line([(255 + offset, 262), (259 + offset, 266), (268 + offset, 257)], fill=color, width=2)
            image.save(path)

    def test_selected_mark_passes_independent_silhouette_and_color(self):
        self.selected_mark()
        report = self.result()
        self.assertEqual(report["selected_mark_checks"], 1)
        self.assertEqual(report["failed_selected_mark_checks"], 0)
        self.assertEqual(report["failed_surface_samples"], 0)

    def test_missing_selected_mark_fails(self):
        self.selected_mark(missing=True)
        self.assertEqual(self.result()["failed_selected_mark_checks"], 1)

    def test_translated_selected_mark_fails_geometry(self):
        self.selected_mark(native_offset=2)
        self.assertEqual(self.result()["failed_selected_mark_checks"], 1)

    def test_selected_mark_wrong_color_fails_one_byte_budget(self):
        self.selected_mark(native_color=(253, 255, 255))
        self.assertEqual(self.result()["failed_selected_mark_checks"], 1)

    def content_primitive(self):
        body = {"x": 250, "y": 250, "width": 40, "height": 40}
        content = {"x": 267, "y": 253, "width": 20, "height": 20}
        row = {"trigger": None, "swatch": body, "options": {"value": None}}
        self.reference["cases"][0]["rows"] = [row]
        self.native[0]["rows"] = [copy.deepcopy(row)]
        self.reference["cases"][0]["elements"] = [dict(content,
            className="ant-color-picker-color-block-inner", background="rgb(255, 0, 0)")]
        self.native[0]["quads"] = [dict(content, background="Solid(Hsla { h: 0.0, s: 1.0, l: 0.5, a: 1.0 })",
                                           border_widths=[0, 0, 0, 0])]

    def test_rtl_content_position_checked_independently_of_body(self):
        self.content_primitive()
        self.assertEqual(self.result()["failed_geometry_checks"], 0)
        self.native[0]["quads"][0]["x"] -= 14
        self.assertEqual(self.result()["failed_geometry_checks"], 1)

    def test_content_color_budget_is_enforced(self):
        self.content_primitive()
        self.assertEqual(self.result()["failed_content_checks"], 0)
        self.native[0]["quads"][0]["background"] = "Solid(Hsla { h: 0.0, s: 1.0, l: 0.495, a: 1.0 })"
        self.assertEqual(self.result()["failed_content_checks"], 1)

    def test_missing_content_primitive_fails(self):
        self.content_primitive()
        self.native[0]["quads"] = []
        self.assertEqual(self.result()["failed_content_checks"], 1)

    def test_content_alpha_budget_is_one_byte(self):
        self.content_primitive()
        self.reference["cases"][0]["elements"][0]["background"] = "rgba(255, 0, 0, 0.5)"
        self.native[0]["quads"][0]["background"] = "Solid(Hsla { h: 0.0, s: 1.0, l: 0.5, a: 0.5019607843 })"
        self.assertEqual(self.result()["failed_content_checks"], 0)
        self.native[0]["quads"][0]["background"] = "Solid(Hsla { h: 0.0, s: 1.0, l: 0.5, a: 0.51 })"
        self.assertEqual(self.result()["failed_content_checks"], 1)


if __name__ == "__main__":
    unittest.main()
