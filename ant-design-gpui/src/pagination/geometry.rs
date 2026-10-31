// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

/// Carries fractional pixels across consecutive widths and gaps. Snapping each
/// absolute edge preserves the row's total span instead of accumulating the
/// rounding error of independently snapped controls.
#[derive(Clone, Copy, Default)]
pub(super) struct PixelCursor {
    logical: f64,
}

impl PixelCursor {
    pub fn span(&mut self, length: f64, snap: impl Fn(f64) -> f64) -> f64 {
        let start = snap(self.logical);
        self.logical += length;
        snap(self.logical) - start
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pagination_pixel_cursor_keeps_physical_edges_and_bounded_cumulative_error() {
        for scale in [1., 1.5, 2.] {
            let snap = |value: f64| (value * scale).round() / scale;
            for width in [21., 35., 32., 73.708_333_333_333_33] {
                for gap in [0., 2., 8.] {
                    for count in [1, 2, 3, 7, 11, 31, 1000] {
                        let mut cursor = PixelCursor::default();
                        let (mut logical, mut painted) = (0., 0.);
                        for index in 0..count {
                            for length in [width, if index + 1 < count { gap } else { 0. }] {
                                let span = cursor.span(length, snap);
                                assert!(span >= 0.);
                                assert!((span * scale - (span * scale).round()).abs() < 1e-8);
                                assert!((span - length).abs() <= 1. / scale + 1e-8);
                                logical += length;
                                painted += span;
                                assert!(
                                    (painted - logical).abs() <= 0.5 / scale + 1e-8,
                                    "scale={scale}, width={width}, gap={gap}, count={count}, index={index}"
                                );
                            }
                        }
                        assert!((painted - snap(logical)).abs() < 1e-8);
                    }
                }
            }
        }
    }

    #[test]
    fn pagination_pixel_cursor_carries_compact_half_pixels_instead_of_truncating_each_item() {
        // GPUI's layout grid sends positive half ties toward zero. Both compact
        // sizes must alternate physical spans, rather than lose half a pixel
        // on every control in a long pagination row.
        let snap = |value: f64| {
            let physical = value * 1.5;
            if physical.fract() == 0.5 {
                physical.trunc() / 1.5
            } else {
                physical.round() / 1.5
            }
        };
        for width in [21., 35.] {
            let mut cursor = PixelCursor::default();
            let first = cursor.span(width, snap);
            let second = cursor.span(width, snap);
            assert!((first - (width - 1. / 3.)).abs() < 1e-12);
            assert!((second - (width + 1. / 3.)).abs() < 1e-12);
            assert_eq!(first + second, 2. * width);
        }
    }

    #[test]
    fn pagination_pixel_cursor_preserves_zero_spans_and_mixed_intrinsic_widths() {
        let snap = |value: f64| (value * 1.5).round() / 1.5;
        let mut cursor = PixelCursor::default();
        let lengths = [32., 8., 73.708_333_333_333_33, 8., 21., 2., 35., 0., 32.];
        let spans = lengths.map(|length| cursor.span(length, snap));
        assert_eq!(spans[7], 0.);
        assert!((spans.iter().sum::<f64>() - snap(lengths.iter().sum())).abs() < 1e-12);
    }
}
