# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Positive and deliberately broken inputs for the Carousel comparison contract."""
import copy
import unittest

from PIL import Image

from compare_carousel_visual import compare


class CarouselComparatorTests(unittest.TestCase):
    def setUp(self):
        bounds = {"x": 0, "y": 0, "width": 20, "height": 10}
        row = {"carousel": bounds, "viewport": bounds, "dots": [], "arrows": [],
               "slides": [{"index": 0, "active": True}]}
        self.browser = {"name": "sample", "scale": 1, "rows": [row]}
        self.native = {"name": "sample", "scale": 1, "rows": [{**row, "currentIndex": 0}]}
        self.image = Image.new("RGB", (20, 10), "blue")

    def test_matching_capture(self):
        record = compare(self.browser, self.native, self.image, self.image)
        self.assertEqual(record["mean_channel_error"], 0)

    def test_dpi_mismatch_fails(self):
        self.native["scale"] = 1.5
        with self.assertRaisesRegex(ValueError, "DPI"):
            compare(self.browser, self.native, self.image, self.image)

    def test_physical_dimensions_fail(self):
        with self.assertRaisesRegex(ValueError, "dimensions"):
            compare(self.browser, self.native, self.image, Image.new("RGB", (21, 10)))

    def test_missing_control_fails(self):
        self.native["rows"][0]["arrows"] = [{"x": 0, "y": 0, "width": 1, "height": 1}]
        with self.assertRaisesRegex(ValueError, "count"):
            compare(self.browser, self.native, self.image, self.image)

    def test_wrong_slide_fails(self):
        self.native["rows"][0]["currentIndex"] = 1
        with self.assertRaisesRegex(ValueError, "selected slide"):
            compare(self.browser, self.native, self.image, self.image)

    def test_changed_pixels_remain_measured(self):
        record = compare(self.browser, self.native, self.image, Image.new("RGB", (20, 10), "red"))
        self.assertGreater(record["rows"][0]["crop_mean_channel_error"], 100)

    def test_root_offset_fails(self):
        self.native = copy.deepcopy(self.native)
        self.native["rows"][0]["carousel"]["x"] = 2
        with self.assertRaisesRegex(ValueError, "geometry exceeds"):
            compare(self.browser, self.native, self.image, self.image)

    def test_wrong_dot_position_fails(self):
        bounds = {"x": 5, "y": 5, "width": 2, "height": 2}
        self.browser["rows"][0]["dots"] = [bounds]
        self.native["rows"][0]["dots"] = [{**bounds, "x": 8}]
        with self.assertRaisesRegex(ValueError, "geometry exceeds"):
            compare(self.browser, self.native, self.image, self.image)

    def test_wrong_viewport_clipping_fails(self):
        self.native["rows"][0]["viewport"] = {"x": 0, "y": 0, "width": 15, "height": 10}
        with self.assertRaisesRegex(ValueError, "geometry exceeds"):
            compare(self.browser, self.native, self.image, self.image)

    def test_empty_viewport_exception_preserves_raw_geometry(self):
        self.browser["rows"][0].update(options={"count": 0}, slides=[])
        self.browser["rows"][0]["viewport"] = {"x": 0, "y": 0, "width": 20, "height": 0}
        self.native["rows"][0]["currentIndex"] = None
        record = compare(self.browser, self.native, self.image, self.image)["rows"][0]
        self.assertEqual(record["viewport_geometry_error"], 10)
        self.assertEqual(record["known_difference"], "empty_internal_viewport_extent")
        self.assertTrue(record["exception_proof"]["raw_crop_pixels_identical"])

    def test_empty_viewport_exception_rejects_visible_pixel_difference(self):
        self.browser["rows"][0].update(options={"count": 0}, slides=[])
        self.browser["rows"][0]["viewport"] = {"x": 0, "y": 0, "width": 20, "height": 0}
        self.native["rows"][0]["currentIndex"] = None
        with self.assertRaisesRegex(ValueError, "geometry exceeds"):
            compare(self.browser, self.native, self.image, Image.new("RGB", (20, 10), "red"))

    def adaptive_case(self):
        self.browser["direction"] = "rtl"
        self.browser["rows"][0].update(options={"adaptive": True, "count": 4},
            slides=[{"index": 1, "height": 5, "active": True}, {"index": 2, "height": 10, "active": False}])
        self.native["rows"][0].update(currentIndex=1,
            carousel={"x": 0, "y": 0, "width": 20, "height": 5},
            viewport={"x": 0, "y": 0, "width": 20, "height": 5})

    def test_rtl_adaptive_exception_requires_independent_wrong_and_visible_heights(self):
        self.adaptive_case()
        record = compare(self.browser, self.native, self.image, self.image)["rows"][0]
        self.assertEqual(record["root_geometry_error"], 5)
        self.assertEqual(record["exception_proof"]["visible_source_index"], 1)
        self.assertEqual(record["exception_proof"]["wrong_source_index"], 2)

    def test_rtl_adaptive_exception_rejects_wrong_native_visible_height(self):
        self.adaptive_case()
        self.native["rows"][0]["viewport"]["height"] = 2
        with self.assertRaisesRegex(ValueError, "geometry exceeds"):
            compare(self.browser, self.native, self.image, self.image)

    def test_wrong_dot_active_association_fails(self):
        bounds = {"x": 5, "y": 5, "width": 2, "height": 2}
        self.browser["rows"][0]["dots"] = [{**bounds, "active": True}]
        self.native["rows"][0]["dots"] = [{**bounds, "index": 0, "active": False}]
        with self.assertRaisesRegex(ValueError, "dot logical index/active"):
            compare(self.browser, self.native, self.image, self.image)

    def test_wrong_dot_logical_index_fails(self):
        bounds = {"x": 5, "y": 5, "width": 2, "height": 2}
        self.browser["rows"][0]["dots"] = [{**bounds, "active": True}]
        self.native["rows"][0]["dots"] = [{**bounds, "index": 1, "active": True}]
        with self.assertRaisesRegex(ValueError, "dot logical index/active"):
            compare(self.browser, self.native, self.image, self.image)

    def test_rtl_fade_exception_proves_painted_slides_are_outside_viewport(self):
        self.browser["direction"] = "rtl"
        self.browser["rows"][0].update(options={"effect": "fade"}, slides=[{
            "index": 0, "active": True, "opacity": 1, "x": 30, "y": 0, "width": 20, "height": 10}])
        record = compare(self.browser, self.native, self.image, self.image)["rows"][0]
        self.assertEqual(record["known_difference"], "upstream_rtl_fade_blank_content")
        self.assertTrue(record["exception_proof"]["all_positive_opacity_source_slides_outside_viewport"])

    def test_rtl_fade_inside_viewport_cannot_be_classified_as_blank_source_bug(self):
        self.browser["direction"] = "rtl"
        self.browser["rows"][0].update(options={"effect": "fade"}, slides=[{
            "index": 0, "active": True, "opacity": 1, "x": 0, "y": 0, "width": 20, "height": 10}])
        record = compare(self.browser, self.native, self.image, self.image)["rows"][0]
        self.assertIsNone(record["known_difference"])


if __name__ == "__main__":
    unittest.main()
