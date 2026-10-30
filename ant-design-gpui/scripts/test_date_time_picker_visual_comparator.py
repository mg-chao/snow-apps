# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Focused negative controls for the picker visual evidence comparator."""
import copy
import json
import unittest

import numpy as np
from compare_date_time_picker_visual import (
    compare_case, covered_ink_pixels, painted_extent_checks, scrollbar_paint_checks,
)


SCALES = (1, 1.25, 1.5, 2)


def analytical_rectangle(scale, x, y, gray=132):
    """Exact area coverage of a 6x10 logical rectangle against physical pixels."""
    yy, xx = np.indices((round(48*scale), round(48*scale)))
    overlap_x = np.maximum(0, np.minimum(xx+1, (x+6)*scale)-np.maximum(xx, x*scale))
    overlap_y = np.maximum(0, np.minimum(yy+1, (y+10)*scale)-np.maximum(yy, y*scale))
    intensity = np.rint(255-(255-gray)*overlap_x*overlap_y).astype(np.uint8)
    return np.repeat(intensity[:, :, None], 3, axis=2)


def analytical_ink_checks(scale, source, target):
    # Keep the authored slot fixed and fractional. Only rasterized paint varies.
    slot = {"x": 10.1, "y": 10.2, "width": 20, "height": 24}
    return scrollbar_paint_checks("analytical mark", source, target, slot, slot,
                                  scale, "#ffffff", part_label="remove mark",
                                  compare_color=False)


def fixture():
    rect = {"x": 10, "y": 10, "width": 24, "height": 24}
    case = {"name": "date-default-ltr-100", "scene": "date", "theme": "default", "direction": "ltr", "scale": 1,
            "geometry": {"trigger": {"x": 0, "y": 0, "width": 80, "height": 32},
                         "cells": [{"key": "2024-02-15", "text": "15", "selected": False, "today": True,
                                    "inRange": False, "rangeStart": False, "rangeEnd": False, "rangeHover": False,
                                    "inView": True, "disabled": False, "rect": rect.copy(), "inner": rect.copy()}]}}
    image = np.full((100, 100, 3), 255, dtype=np.uint8)
    image[10, 10:34] = [22, 119, 255]
    return case, image


def scrollbar_fixture():
    case, image = fixture()
    case["tokens"] = {"colorBgElevated": "#ffffff"}
    case["geometry"]["scrollbars"] = [{"column": 0, "track": {"x": 50, "y": 10, "width": 10, "height": 70},
                                        "side": "right", "scrollTop": 20, "scrollHeight": 200, "clientHeight": 70}]
    case["geometry"]["timeViewports"] = [{"x": 10, "y": 10, "width": 40, "height": 70}]
    image[12:15, 53:57] = 140
    image[30:50, 52:58] = 140
    image[75:78, 53:57] = 140
    return case, image


def tag_fixture():
    case, image = fixture()
    case["options"] = {"multiple": True}
    case["geometry"]["tags"] = [{"key": 0, "label": "2024-02-15", "rect": {"x": 10, "y": 50, "width": 70, "height": 24},
                                 "content": {"x": 18, "y": 51, "width": 40, "height": 22},
                                 "remove": {"x": 62, "y": 57, "width": 10, "height": 10}}]
    image[50:74, 10:80] = 240
    for at in range(8):
        image[58+at, 63+at] = 132
        image[58+at, 70-at] = 132
    return case, image


def custom_border_fixture():
    case, image = fixture()
    inner = case["geometry"]["cells"][0]["inner"]
    inner.update(borderWidth="1px", borderColor="rgb(22, 119, 255)")
    actual = copy.deepcopy(case)
    actual["geometry"]["cells"][0]["paintBorderCandidates"] = [{
        "rect": {key: inner[key] for key in ("x", "y", "width", "height")},
        "clip": {"x": 0, "y": 0, "width": 100, "height": 100},
        "rgba": [22/255, 119/255, 1, 1], "widths": [1, 1, 1, 1],
    }]
    return case, actual, image


def fractional_outline_fixture():
    case, _ = fixture()
    case["scale"] = 1.5
    cell = case["geometry"]["cells"][0]
    cell["rect"]["y"] = cell["inner"]["y"] = 10.125
    cell["innerBefore"] = {"borderColor": "rgb(22, 119, 255)"}
    source = np.full((150, 150, 3), 255, dtype=np.uint8)
    target = source.copy()
    # Exact full-color source and native borders straddle the nominal sample
    # row because their raster phases differ by one physical pixel.
    source[16, 15:51] = [22, 119, 255]
    target[15, 15:51] = [22, 119, 255]
    return case, source, target


class PickerComparatorControls(unittest.TestCase):
    def assert_fails(self, expected, actual, source=None, target=None):
        self.assertTrue(any(not c["passed"] for c in compare_case(expected, actual, source, target)))

    def test_identical_geometry_and_today_ink_pass(self):
        case, image = fixture()
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), image, image.copy())))

    def test_subpixel_geometry_within_tolerance_passes(self):
        case, _ = fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["trigger"]["width"] += 0.75
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual)))

    def test_displaced_trigger_fails(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                case, _ = fixture()
                case["scale"] = scale
                actual = copy.deepcopy(case)
                actual["geometry"]["trigger"]["x"] += 1.01
                checks = compare_case(case, actual)
                self.assertTrue(any(not c["passed"] for c in checks if c["target"] == "trigger"))

    def test_missing_today_outline_pixels_fail(self):
        case, source = fixture()
        self.assert_fails(case, copy.deepcopy(case), source, np.full_like(source, 255))

    def test_wrong_today_metadata_fails(self):
        case, _ = fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"][0]["today"] = False
        self.assert_fails(case, actual)

    def test_missing_selected_ink_fails(self):
        case, source = fixture()
        case["geometry"]["cells"][0].update(selected=True, today=False)
        source[10:34, 10:34] = [22, 119, 255]
        self.assert_fails(case, copy.deepcopy(case), source, np.full_like(source, 255))

    def test_wrong_selection_metadata_fails(self):
        case, _ = fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"][0]["selected"] = True
        self.assert_fails(case, actual)

    def test_missing_range_band_pixels_fail(self):
        case, source = fixture()
        case["geometry"]["cells"][0].update(today=False, inRange=True)
        source[10:34, 10:34] = [230, 244, 255]
        self.assert_fails(case, copy.deepcopy(case), source, np.full_like(source, 255))

    def test_wrong_range_endpoint_fails(self):
        case, _ = fixture()
        case["geometry"]["cells"][0]["rangeStart"] = True
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"][0].update(rangeStart=False, rangeEnd=True)
        self.assert_fails(case, actual)

    def test_missing_cell_fails(self):
        case, _ = fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"] = []
        self.assert_fails(case, actual)

    def test_wrong_scale_fails(self):
        case, _ = fixture()
        actual = copy.deepcopy(case)
        actual["scale"] = 1.5
        self.assert_fails(case, actual)

    def test_explicit_unsupported_scene_fails(self):
        case, _ = fixture()
        self.assert_fails(case, {"unsupported": "pending native implementation"})

    def test_missing_week_row_band_pixels_fail(self):
        case, source = fixture()
        case["geometry"]["cells"][0].update(today=False, weekSelected=True)
        source[10:34, 10:34] = [22, 119, 255]
        self.assert_fails(case, copy.deepcopy(case), source, np.full_like(source, 255))

    def test_wrong_week_row_selection_metadata_fails(self):
        case, _ = fixture()
        case["geometry"]["cells"][0].update(today=False, weekSelected=True)
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"][0]["weekSelected"] = False
        self.assert_fails(case, actual)

    def test_separate_selected_squares_do_not_pass_as_continuous_week_band(self):
        case, source = fixture()
        cell = case["geometry"]["cells"][0]
        cell.update(today=False, weekSelected=True)
        cell["rect"]["width"] = 36
        cell["inner"]["x"] = 16
        source[10:34, 10:46] = [22, 119, 255]
        target = np.full_like(source, 255)
        target[10:34, 16:40] = [22, 119, 255]
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_selected_week_hover_alias_does_not_require_same_internal_state(self):
        case, image = fixture()
        case["geometry"]["cells"][0].update(today=False, weekSelected=True, weekHover=True)
        actual = copy.deepcopy(case)
        actual["geometry"]["cells"][0]["weekHover"] = False
        image[10:34, 10:34] = [22, 119, 255]
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, image, image.copy())))

    def test_empty_html_multiple_focus_helper_does_not_require_dummy_native_input(self):
        case, _ = fixture()
        case["options"] = {"multiple": True}
        case["geometry"]["inputs"] = [{"x": 0, "y": 0, "width": 4, "height": 22, "value": ""}]
        actual = copy.deepcopy(case)
        actual["geometry"]["inputs"] = []
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual)))

    def test_nonempty_multiple_editor_cannot_be_silently_ignored(self):
        case, _ = fixture()
        case["options"] = {"multiple": True}
        case["geometry"]["inputs"] = [{"x": 0, "y": 0, "width": 30, "height": 22, "value": "draft"}]
        actual = copy.deepcopy(case)
        actual["geometry"]["inputs"] = []
        self.assert_fails(case, actual)

    def test_identical_scrollbar_arrows_thumb_and_extent_pass(self):
        case, source = scrollbar_fixture()
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, source.copy())))

    def test_missing_scrollbar_arrow_fails(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[12:15, 53:57] = 255
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_missing_scrollbar_thumb_fails(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[30:50, 52:58] = 255
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_displaced_scrollbar_thumb_fails(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[30:50, 52:58] = 255
        target[34:54, 52:58] = 140
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_wrong_scrollbar_ink_fails(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[30:50, 52:58] = 180
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_wrong_scrollbar_extent_fails(self):
        case, source = scrollbar_fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["scrollbars"][0]["scrollHeight"] = 208
        self.assert_fails(case, actual, source, source.copy())

    def test_neighboring_selected_fill_is_not_a_scrollbar_part(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[10:80, 59] = [230,244,255]
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, target)))

    def test_neighboring_separator_is_not_a_scrollbar_thumb(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[10:80, 50] = 225
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, target)))

    def test_hollow_thumb_with_correct_bbox_and_color_fails(self):
        case, source = scrollbar_fixture()
        target = source.copy()
        target[31:49, 53:57] = 255
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_wrong_primary_footer_fill_fails(self):
        case, source = fixture()
        case["geometry"]["footerActions"] = [{"key": "ok", "label": "OK", "rect": {"x": 60, "y": 60, "width": 24, "height": 24}}]
        source[60:84, 60:84] = [22, 119, 255]
        target = source.copy()
        target[60:84, 60:84] = [245, 34, 45]
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_wrong_scrollbar_client_viewport_fails(self):
        case, source = scrollbar_fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["timeViewports"][0]["width"] += 10
        self.assert_fails(case, actual, source, source.copy())

    def test_painted_scrollbar_report_serializes_as_portable_json(self):
        case, source = scrollbar_fixture()
        report = compare_case(case, copy.deepcopy(case), source, source.copy())
        self.assertEqual(json.loads(json.dumps(report)), report)

    def test_hidden_nonempty_multiple_helper_does_not_require_native_dummy_editor(self):
        case, _ = fixture()
        case["options"] = {"multiple": True}
        case["geometry"]["inputs"] = [{"x": 0, "y": 0, "width": 4, "height": 2, "value": "2024-02-15",
                                        "auxiliary": True, "visibility": "hidden"}]
        actual = copy.deepcopy(case)
        actual["geometry"]["inputs"] = []
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual)))

    def test_zero_width_active_bar_does_not_require_native_paint(self):
        case, _ = fixture()
        case["geometry"]["activeBar"] = {"x": 0, "y": 0, "width": 0, "height": 2}
        actual = copy.deepcopy(case)
        actual["geometry"]["activeBar"] = None
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual)))

    def test_identical_visible_chips_and_remove_ink_pass(self):
        case, source = tag_fixture()
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, source.copy())))

    def test_missing_visible_chip_fails(self):
        case, source = tag_fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["tags"] = []
        self.assert_fails(case, actual, source, source.copy())

    def test_displaced_chip_content_fails(self):
        case, source = tag_fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["tags"][0]["content"]["x"] += 3
        self.assert_fails(case, actual, source, source.copy())

    def test_wrong_chip_background_fails(self):
        case, source = tag_fixture()
        target = source.copy()
        target[50:74, 10:80] = 220
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_chip_fill_contrast_survives_overlapping_popup_shadow(self):
        case, source = tag_fixture()
        target = source.copy()
        target[50:74, 5:85] = np.maximum(target[50:74, 5:85].astype(int) - 3, 0)
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(all(c['passed'] for c in checks if c['target'].endswith('background')))

    def test_independent_range_arrow_fill_passes(self):
        case, source = fixture()
        case["geometry"]["popup"] = {"x": 50,"y":66,"width":40,"height":30}
        case["geometry"]["rangeArrow"] = {"x":60,"y":50,"width":24,"height":16}
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, source.copy())))

    def test_clipped_range_arrow_above_popup_fails(self):
        case, source = fixture()
        case["geometry"]["popup"] = {"x":50,"y":66,"width":40,"height":30}
        case["geometry"]["rangeArrow"] = {"x":60,"y":50,"width":24,"height":16}
        target = source.copy()
        target[58:66,60:84] = 220
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_wrong_trigger_flat_fill_still_fails(self):
        case, source = fixture()
        target = source.copy()
        target[1:8, 35:45] = 220
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(any(not c["passed"] for c in checks if c["target"] == "trigger surface"))

    def test_popup_shadow_does_not_masquerade_as_wrong_trigger_fill(self):
        case, source = fixture()
        target = source.copy()
        target[16, 2] = 250
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, target)))

    def test_wrong_popup_flat_fill_fails(self):
        case, source = fixture()
        case["geometry"]["popup"] = {"x": 50, "y": 40, "width": 40, "height": 50}
        target = source.copy()
        target[41:46, 62:78] = 220
        checks = compare_case(case, copy.deepcopy(case), source, target)
        popup = [c for c in checks if c["target"] == "popup surface"]
        self.assertEqual(len(popup), 2)
        self.assertTrue(all(not c["passed"] for c in popup))

    def test_popup_flat_fill_probes_stay_clear_of_edge_rail(self):
        case, source = fixture()
        case["geometry"]["popup"] = {"x": 50, "y": 40, "width": 40, "height": 50}
        target = source.copy()
        # The former left-edge/mid-height fill sample intersects these valid
        # rail pixels. The independent top clear-band samples remain white.
        source[40:90, 50:52] = 140
        target[40:90, 51:53] = 140
        self.assertTrue(all(c["passed"] for c in compare_case(case, copy.deepcopy(case), source, target)))

    def test_custom_cell_border_scene_proof_passes(self):
        case, actual, source = custom_border_fixture()
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, source, source.copy())))

    def test_missing_custom_cell_border_scene_proof_fails(self):
        case, actual, source = custom_border_fixture()
        del actual["geometry"]["cells"][0]["paintBorderCandidates"]
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_wrong_custom_cell_border_rgba_fails(self):
        for rgba in ([1, 0, 0, 1], [22/255, 119/255, 1, 0.5]):
            with self.subTest(rgba=rgba):
                case, actual, source = custom_border_fixture()
                actual["geometry"]["cells"][0]["paintBorderCandidates"][0]["rgba"] = rgba
                checks = compare_case(case, actual, source, source.copy())
                self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_shifted_custom_cell_border_scene_proof_fails(self):
        case, actual, source = custom_border_fixture()
        actual["geometry"]["cells"][0]["paintBorderCandidates"][0]["rect"]["x"] += 1.01
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_thick_custom_cell_border_scene_proof_fails(self):
        case, actual, source = custom_border_fixture()
        actual["geometry"]["cells"][0]["paintBorderCandidates"][0]["widths"] = [1, 1, 3, 1]
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_clipped_custom_cell_border_scene_proof_fails(self):
        case, actual, source = custom_border_fixture()
        actual["geometry"]["cells"][0]["paintBorderCandidates"][0]["clip"]["width"] = 22
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_red_overpaint_cannot_hide_behind_correct_custom_border(self):
        case, actual, source = custom_border_fixture()
        candidates = actual["geometry"]["cells"][0]["paintBorderCandidates"]
        candidates.append({**copy.deepcopy(candidates[0]), "rgba": [1, 0, 0, 1]})
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_custom_border_uses_paint_order_instead_of_candidate_list_order(self):
        case, actual, source = custom_border_fixture()
        candidates = actual["geometry"]["cells"][0]["paintBorderCandidates"]
        candidates[0]["order"] = 20
        candidates.append({**copy.deepcopy(candidates[0]), "rgba": [1, 0, 0, 1], "order": 10})
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, source, source.copy())))

    def test_rounded_custom_border_four_clip_fragment_union_passes(self):
        case, actual, source = custom_border_fixture()
        border = actual["geometry"]["cells"][0]["paintBorderCandidates"][0]
        clips = ((10, 10, 24, 5), (10, 29, 24, 5), (10, 15, 2, 14), (32, 15, 2, 14))
        actual["geometry"]["cells"][0]["paintBorderCandidates"] = [
            {**copy.deepcopy(border), "order": 5,
             "clip": dict(zip(("x", "y", "width", "height"), clip))}
            for clip in clips
        ]
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, source, source.copy())))

    def test_rounded_custom_border_missing_straight_side_fragment_fails(self):
        case, actual, source = custom_border_fixture()
        border = actual["geometry"]["cells"][0]["paintBorderCandidates"][0]
        # Top/bottom clips retain both corners. Without the left fragment,
        # the straight left side is missing even though the union bbox fits.
        clips = ((10, 10, 24, 5), (10, 29, 24, 5), (32, 15, 2, 14))
        actual["geometry"]["cells"][0]["paintBorderCandidates"] = [
            {**copy.deepcopy(border), "order": 5,
             "clip": dict(zip(("x", "y", "width", "height"), clip))}
            for clip in clips
        ]
        checks = compare_case(case, actual, source, source.copy())
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("authored custom border")))

    def test_rounded_custom_border_mixed_paint_order_fragment_union_passes(self):
        case, actual, source = custom_border_fixture()
        border = actual["geometry"]["cells"][0]["paintBorderCandidates"][0]
        fragments = ((11, (10, 10, 24, 5)), (11, (10, 29, 24, 5)),
                     (9, (10, 15, 2, 14)), (9, (32, 15, 2, 14)))
        actual["geometry"]["cells"][0]["paintBorderCandidates"] = [
            {**copy.deepcopy(border), "order": order,
             "clip": dict(zip(("x", "y", "width", "height"), clip))}
            for order, clip in fragments
        ]
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, source, source.copy())))

    def test_fractional_y_today_outline_finds_independent_full_color_pixels(self):
        case, source, target = fractional_outline_fixture()
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(all(c["passed"] for c in checks), checks)
        outline = next(c for c in checks if c["target"].endswith("today outline"))
        self.assertEqual(outline["reference"]["physical_pixel"], [33, 16])
        self.assertEqual(outline["native"]["physical_pixel"], [33, 15])

    def test_missing_top_outline_cannot_pass_using_other_blue_border(self):
        case, source, target = fractional_outline_fixture()
        target[15, 15:51] = 255
        target[50, 15:51] = [22, 119, 255]
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("today outline")))

    def test_wrong_color_fractional_today_outline_fails(self):
        case, source, target = fractional_outline_fixture()
        target[15, 15:51] = [255, 0, 0]
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("today outline")))

    def test_wrong_visible_chip_value_fails(self):
        case, source = tag_fixture()
        actual = copy.deepcopy(case)
        actual["geometry"]["tags"][0]["label"] = "2024-02-16"
        self.assert_fails(case, actual, source, source.copy())

    def test_missing_remove_mark_with_correct_box_fails(self):
        case, source = tag_fixture()
        target = source.copy()
        target[57:67, 62:72] = 240
        self.assert_fails(case, copy.deepcopy(case), source, target)

    def test_correct_shape_red_remove_mark_fails_hue(self):
        case, source = tag_fixture()
        target = source.copy()
        for at in range(8):
            target[58+at, 63+at] = [200,0,0]
            target[58+at, 70-at] = [200,0,0]
        checks = compare_case(case, copy.deepcopy(case), source, target)
        self.assertTrue(any(not c["passed"] for c in checks if c["target"].endswith("achromatic ink")))

    def test_exact_native_remove_sprite_tint_passes(self):
        case, source = tag_fixture()
        case["geometry"]["tags"][0]["remove"]["color"] = "rgba(0, 0, 0, 0.45)"
        actual = copy.deepcopy(case)
        actual["geometry"]["tags"][0]["remove"]["paintRgbaCandidates"] = [[0, 0, 0, 0.45]]
        self.assertTrue(all(c["passed"] for c in compare_case(case, actual, source, source.copy())))

    def test_wrong_native_remove_opacity_fails_with_identical_raster(self):
        case, source = tag_fixture()
        case["geometry"]["tags"][0]["remove"]["color"] = "rgba(0, 0, 0, 0.45)"
        actual = copy.deepcopy(case)
        actual["geometry"]["tags"][0]["remove"]["paintRgbaCandidates"] = [[0, 0, 0, 0.25]]
        checks = compare_case(case, actual, source, source.copy())
        tint = next(c for c in checks if c["target"].endswith("remove native scene tint"))
        self.assertFalse(tint["passed"])
        self.assertTrue(all(c["passed"] for c in checks if c is not tint))

    def test_missing_or_ambiguous_native_remove_sprite_tint_fails(self):
        for candidates in (None, [], [[0, 0, 0, 0.45], [0, 0, 0, 0.45]]):
            with self.subTest(candidates=candidates):
                case, source = tag_fixture()
                case["geometry"]["tags"][0]["remove"]["color"] = "rgba(0, 0, 0, 0.45)"
                actual = copy.deepcopy(case)
                if candidates is not None:
                    actual["geometry"]["tags"][0]["remove"]["paintRgbaCandidates"] = candidates
                checks = compare_case(case, actual, source, source.copy())
                tint = next(c for c in checks if c["target"].endswith("remove native scene tint"))
                self.assertFalse(tint["passed"])
                self.assertTrue(all(c["passed"] for c in checks if c is not tint))

    def test_fractional_ink_edges_use_distance_not_summed_width(self):
        expected = {"x":10,"y":10,"width":10,"height":10}
        actual = {"x":10.667,"y":10,"width":8.666,"height":10}
        self.assertTrue(painted_extent_checks("ink", expected, actual)[0]["passed"])

    def test_true_painted_edge_displacement_still_fails(self):
        expected = {"x":10,"y":10,"width":10,"height":10}
        actual = {"x":11.334,"y":10,"width":10,"height":10}
        self.assertFalse(painted_extent_checks("ink", expected, actual)[0]["passed"])

    def test_pixel_square_coverage_matches_analytical_distance_at_all_scales(self):
        mask = np.zeros((11, 11), dtype=bool)
        mask[5, 5] = True
        # The occupied physical pixel is the closed square [5,6] x [5,6].
        # An independent closest-point calculation tests every nearby center,
        # including diagonal corners that a square dilation would over-accept.
        for scale in SCALES:
            with self.subTest(scale=scale):
                actual = covered_ink_pixels(mask, scale)
                for y in range(11):
                    for x in range(11):
                        center = np.array([x+0.5, y+0.5])
                        closest = np.clip(center, [5, 5], [6, 6])
                        expected = float(np.dot(center-closest, center-closest)) <= scale*scale
                        self.assertEqual(bool(actual[y, x]), expected, (scale, x, y))

    def test_fractional_phase_analytical_paint_passes_at_all_scales(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                source = analytical_rectangle(scale, 17.125, 17.375)
                target = analytical_rectangle(scale, 17.625, 17.625)
                checks = analytical_ink_checks(scale, source, target)
                self.assertTrue(all(c["passed"] for c in checks), checks)

    def test_paint_only_three_logical_pixel_shift_fails_at_all_scales(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                source = analytical_rectangle(scale, 17.125, 17.375)
                target = analytical_rectangle(scale, 20.125, 17.375)
                checks = analytical_ink_checks(scale, source, target)
                silhouette = next(c for c in checks if c["target"].endswith("solid ink silhouette"))
                self.assertFalse(silhouette["passed"])
                self.assertGreater(silhouette["reference_core_pixels"], 0)
                self.assertGreater(silhouette["native_core_pixels"], 0)

    def test_faded_gray_without_native_strong_core_fails_at_all_scales(self):
        for scale in SCALES:
            with self.subTest(scale=scale):
                source = analytical_rectangle(scale, 17.125, 17.375)
                target = analytical_rectangle(scale, 17.125, 17.375, gray=220)
                checks = analytical_ink_checks(scale, source, target)
                silhouette = next(c for c in checks if c["target"].endswith("solid ink silhouette"))
                # The same connected gray mark and achromatic hue remain. It
                # must fail because faint AA support cannot replace real ink.
                self.assertTrue(all(c["passed"] for c in checks if c is not silhouette))
                self.assertEqual(silhouette["native_core_pixels"], 0)
                self.assertGreater(silhouette["reference_core_pixels"], 0)
                self.assertFalse(silhouette["passed"])


if __name__ == "__main__":
    unittest.main()
