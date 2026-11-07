// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Collapsed table borders split at cumulative device-pixel boundaries.
//!
//! Keeping the phase cumulative prevents a half-device-pixel rounding error
//! from being repeated independently in every row or physical column.

use gpui::{Edges, Pixels, px};

use crate::Direction;

fn valid(width: f32, scale: f32) -> bool {
    width.is_finite()
        && width > 0.
        && scale.is_finite()
        && scale > 0.
        && (width * scale).is_finite()
}

/// Match `Window::pixel_snap`: exact half-device pixels round toward zero.
fn snap_device(device: f64) -> f64 {
    (device.abs() - 0.5).ceil().copysign(device)
}

/// The first half belongs to the cell before the boundary; the second belongs
/// to the cell after it. Their sum is the snapped width at this boundary's phase.
pub(super) fn boundary_parts(index: usize, width: f32, scale: f32) -> (Pixels, Pixels) {
    if !valid(width, scale) {
        return (px(0.), px(0.));
    }
    let device_width = f64::from(width) * f64::from(scale);
    let start = index as f64 * device_width;
    let middle = (index as f64 + 0.5) * device_width;
    let end = (index as f64 + 1.) * device_width;
    let [start, middle, end] = [start, middle, end].map(snap_device);
    // Subtract integer device coordinates before converting back to logical
    // space. Subtracting large f32 logical coordinates loses local pixel pieces.
    (
        px(((middle - start).max(0.) / f64::from(scale)) as f32),
        px(((end - middle).max(0.) / f64::from(scale)) as f32),
    )
}

/// The separate enclosing view owns these edges. Cell edges at the outside of
/// the table stay zero. Opposite outer edges sum to `snap(2 * width)`.
pub(super) fn outer_borders(width: f32, scale: f32) -> Edges<Pixels> {
    if !valid(width, scale) || !(width * 2. * scale).is_finite() {
        return Edges::default();
    }
    let device_width = f64::from(width) * f64::from(scale);
    let start_device = snap_device(device_width);
    let end_device = snap_device(device_width * 2.);
    let start = px((start_device / f64::from(scale)) as f32);
    let end = px(((end_device - start_device).max(0.) / f64::from(scale)) as f32);
    Edges {
        top: start,
        right: end,
        bottom: end,
        left: start,
    }
}

/// Row positions address physical rendered rows; column positions address
/// physical tracks, including separate label/value tracks in horizontal mode.
/// `column_end` is exclusive, so spanning cells consume only their two outside
/// interior boundaries. Logical start/end columns are mirrored in RTL.
#[allow(clippy::too_many_arguments)]
pub(super) fn cell_borders(
    row: usize,
    rows: usize,
    column_start: usize,
    column_end: usize,
    columns: usize,
    width: f32,
    scale: f32,
    direction: Direction,
) -> Edges<Pixels> {
    if rows == 0
        || columns == 0
        || row >= rows
        || column_start >= column_end
        || column_end > columns
        || !valid(width, scale)
    {
        return Edges::default();
    }
    let top = if row > 0 {
        boundary_parts(row - 1, width, scale).1
    } else {
        px(0.)
    };
    let bottom = if row + 1 < rows {
        boundary_parts(row, width, scale).0
    } else {
        px(0.)
    };
    let start = if column_start > 0 {
        boundary_parts(column_start - 1, width, scale).1
    } else {
        px(0.)
    };
    let end = if column_end < columns {
        boundary_parts(column_end - 1, width, scale).0
    } else {
        px(0.)
    };
    let (left, right) = match direction {
        Direction::Ltr => (start, end),
        Direction::Rtl => (end, start),
    };
    Edges {
        top,
        right,
        bottom,
        left,
    }
}

/// Convert a native intrinsic border-box width back to the source's continuous
/// half-border model. Apply this once to its minimum/preferred width constraints.
pub(super) fn horizontal_intrinsic_adjustment(
    column_start: usize,
    column_end: usize,
    columns: usize,
    width: f32,
    insets: Edges<Pixels>,
) -> f32 {
    if !width.is_finite()
        || width <= 0.
        || columns == 0
        || column_start >= column_end
        || column_end > columns
    {
        return 0.;
    }
    let halves = usize::from(column_start > 0) + usize::from(column_end < columns);
    halves as f32 * width / 2. - f32::from(insets.left + insets.right)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn snap(value: f32, scale: f32) -> f32 {
        (snap_device(f64::from(value) * f64::from(scale)) / f64::from(scale)) as f32
    }

    fn near(actual: f32, expected: f32) {
        assert!((actual - expected).abs() < 0.0002, "{actual} != {expected}");
    }

    #[test]
    fn exact_half_device_pixels_round_toward_zero() {
        near(snap(0.5, 1.), 0.);
        near(snap(1.5, 1.), 1.);
        near(snap(2.5, 1.), 2.);
        near(snap(-1.5, 1.), -1.);
        assert_eq!(boundary_parts(0, 1., 1.), (px(0.), px(1.)));
        assert_eq!(boundary_parts(0, 1., 2.), (px(0.5), px(0.5)));
        let (before, after) = boundary_parts(0, 1., 1.5);
        near(f32::from(before), 2. / 3.);
        near(f32::from(after), 0.);
    }

    #[test]
    fn cumulative_border_width_stays_within_one_device_pixel_across_many_rows() {
        for scale in [1., 1.5, 2.] {
            for width in [0.5, 1., 1.25, 2., 3.] {
                let mut sum = 0f64;
                for index in 0..1024 {
                    let (before, after) = boundary_parts(index, width, scale);
                    assert!(before >= px(0.) && after >= px(0.));
                    for half in [before, after] {
                        let device = f32::from(half) * scale;
                        near(device, device.round());
                    }
                    sum += f64::from(f32::from(before + after));
                    let raw = (index + 1) as f64 * f64::from(width);
                    assert!((sum - raw).abs() <= 1. / f64::from(scale) + 0.001);
                }
            }
        }
    }

    #[test]
    fn large_boundary_indices_preserve_integer_device_pixel_parts() {
        for scale in [1., 1.5, 2.] {
            for width in [0.5, 1., 1.25, 2., 3.] {
                for index in [1023, 65_535, 1_000_003, 16_777_215, 16_777_216] {
                    let (before, after) = boundary_parts(index, width, scale);
                    for half in [before, after] {
                        let device = f32::from(half) * scale;
                        near(device, device.round());
                    }
                    let device_width = f64::from(width) * f64::from(scale);
                    let expected = snap_device((index as f64 + 1.) * device_width)
                        - snap_device(index as f64 * device_width);
                    near(f32::from(before + after) * scale, expected as f32);
                }
            }
        }
    }

    #[test]
    fn neighbors_share_each_boundary_and_outward_cell_edges_stay_zero() {
        for scale in [1., 1.5, 2.] {
            for width in [1., 2., 3.] {
                for row in 0..127 {
                    let upper = cell_borders(row, 128, 0, 1, 4, width, scale, Direction::Ltr);
                    let lower = cell_borders(row + 1, 128, 1, 4, 4, width, scale, Direction::Ltr);
                    let expected =
                        snap((row + 1) as f32 * width, scale) - snap(row as f32 * width, scale);
                    near(f32::from(upper.bottom + lower.top), expected);
                    assert_eq!(upper.left, px(0.));
                    assert_eq!(lower.right, px(0.));
                }
                let first = cell_borders(0, 128, 0, 4, 4, width, scale, Direction::Ltr);
                let last = cell_borders(127, 128, 0, 4, 4, width, scale, Direction::Ltr);
                assert_eq!(first.top, px(0.));
                assert_eq!(last.bottom, px(0.));
                let outside = outer_borders(width, scale);
                near(
                    f32::from(outside.top + outside.bottom),
                    snap(width * 2., scale),
                );
                near(
                    f32::from(outside.left + outside.right),
                    snap(width * 2., scale),
                );
            }
        }
    }

    #[test]
    fn rtl_mirrors_track_edges_and_spans_use_only_their_outside_boundaries() {
        let ltr = cell_borders(2, 8, 1, 5, 6, 3., 1.5, Direction::Ltr);
        let rtl = cell_borders(2, 8, 1, 5, 6, 3., 1.5, Direction::Rtl);
        assert_eq!(ltr.left, rtl.right);
        assert_eq!(ltr.right, rtl.left);
        assert_eq!(ltr.top, rtl.top);
        assert_eq!(ltr.bottom, rtl.bottom);
        assert_eq!(ltr.left, boundary_parts(0, 3., 1.5).1);
        assert_eq!(ltr.right, boundary_parts(4, 3., 1.5).0);
        near(
            horizontal_intrinsic_adjustment(1, 5, 6, 3., ltr),
            3. - f32::from(ltr.left + ltr.right),
        );
        near(
            horizontal_intrinsic_adjustment(1, 5, 6, 3., rtl),
            horizontal_intrinsic_adjustment(1, 5, 6, 3., ltr),
        );
    }

    #[test]
    fn zero_or_invalid_geometry_has_no_borders_or_intrinsic_adjustment() {
        for (width, scale) in [
            (0., 1.),
            (-1., 1.),
            (f32::NAN, 1.),
            (1., 0.),
            (1., f32::INFINITY),
        ] {
            assert_eq!(boundary_parts(0, width, scale), (px(0.), px(0.)));
            assert_eq!(outer_borders(width, scale), Edges::default());
            assert_eq!(
                cell_borders(0, 1, 0, 1, 1, width, scale, Direction::Ltr),
                Edges::default()
            );
        }
        assert_eq!(
            cell_borders(0, 0, 0, 1, 1, 1., 1., Direction::Ltr),
            Edges::default()
        );
        assert_eq!(
            cell_borders(0, 1, 1, 1, 1, 1., 1., Direction::Ltr),
            Edges::default()
        );
        assert_eq!(
            horizontal_intrinsic_adjustment(0, 1, 1, 0., Edges::default()),
            0.
        );
    }
}
