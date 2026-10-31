// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Placement rules adapted from Ant Design 6.6.7 (MIT).

use gpui::{Bounds, Pixels, Point, Size, point, px};

use super::PopupArrow;

/// Physical placements, independent of text direction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PopupPlacement {
    #[default]
    Top,
    TopLeft,
    TopRight,
    Bottom,
    BottomLeft,
    BottomRight,
    Left,
    LeftTop,
    LeftBottom,
    Right,
    RightTop,
    RightBottom,
}

impl PopupPlacement {
    pub const ALL: [Self; 12] = [
        Self::Top,
        Self::TopLeft,
        Self::TopRight,
        Self::Bottom,
        Self::BottomLeft,
        Self::BottomRight,
        Self::Left,
        Self::LeftTop,
        Self::LeftBottom,
        Self::Right,
        Self::RightTop,
        Self::RightBottom,
    ];

    pub fn is_vertical(self) -> bool {
        matches!(
            self,
            Self::Top
                | Self::TopLeft
                | Self::TopRight
                | Self::Bottom
                | Self::BottomLeft
                | Self::BottomRight
        )
    }

    pub fn is_centered(self) -> bool {
        matches!(self, Self::Top | Self::Bottom | Self::Left | Self::Right)
    }

    pub fn opposite(self) -> Self {
        match self {
            Self::Top => Self::Bottom,
            Self::Bottom => Self::Top,
            Self::TopLeft => Self::BottomLeft,
            Self::BottomLeft => Self::TopLeft,
            Self::TopRight => Self::BottomRight,
            Self::BottomRight => Self::TopRight,
            Self::Left => Self::Right,
            Self::Right => Self::Left,
            Self::LeftTop => Self::RightTop,
            Self::RightTop => Self::LeftTop,
            Self::LeftBottom => Self::RightBottom,
            Self::RightBottom => Self::LeftBottom,
        }
    }

    fn opposite_alignment(self) -> Self {
        match self {
            Self::TopLeft => Self::TopRight,
            Self::TopRight => Self::TopLeft,
            Self::BottomLeft => Self::BottomRight,
            Self::BottomRight => Self::BottomLeft,
            Self::LeftTop => Self::LeftBottom,
            Self::LeftBottom => Self::LeftTop,
            Self::RightTop => Self::RightBottom,
            Self::RightBottom => Self::RightTop,
            other => other,
        }
    }

    fn leading(self) -> bool {
        matches!(
            self,
            Self::TopLeft | Self::BottomLeft | Self::LeftTop | Self::RightTop
        )
    }

    pub(crate) fn before(self) -> bool {
        matches!(
            self,
            Self::Top
                | Self::TopLeft
                | Self::TopRight
                | Self::Left
                | Self::LeftTop
                | Self::LeftBottom
        )
    }
}

/// Input to the allocation-free placement solver. Distances are logical pixels.
/// `popup_size` is the measured body, excluding its arrow and shadow.
#[derive(Clone, Copy, Debug)]
pub struct PopupGeometry {
    pub anchor: Bounds<Pixels>,
    pub popup_size: Size<Pixels>,
    pub viewport: Bounds<Pixels>,
    pub placement: PopupPlacement,
    pub arrow: PopupArrow,
    pub arrow_size: Pixels,
    pub gap: Pixels,
    pub radius: Pixels,
    pub offset: Point<Pixels>,
    pub auto_adjust_overflow: bool,
}

/// Resolved body and arrow coordinates. `arrow` is the attachment point on the
/// body edge, in local coordinates, and also defines the motion origin.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PopupLayout {
    pub placement: PopupPlacement,
    pub bounds: Bounds<Pixels>,
    pub arrow: Option<Point<Pixels>>,
}

/// One-dimensional alignment keeps placement independent of rendering, text
/// direction and device scale. A flip transforms the original alignment: it is
/// not a fresh placement at the opposite edge (notably for point-at-center).
#[derive(Clone, Copy, PartialEq, Eq)]
enum AlignPoint {
    Start,
    Center,
    End,
}

impl AlignPoint {
    fn position(self, extent: Pixels) -> Pixels {
        match self {
            Self::Start => px(0.),
            Self::Center => extent / 2.,
            Self::End => extent,
        }
    }
}

#[derive(Clone, Copy)]
struct AxisAlignment {
    popup: AlignPoint,
    target: AlignPoint,
    offset: Pixels,
    shift: Option<Pixels>,
}

impl AxisAlignment {
    fn origin(self, anchor_start: Pixels, anchor_extent: Pixels, extent: Pixels) -> Pixels {
        anchor_start + self.target.position(anchor_extent) - self.popup.position(extent)
            + self.offset
    }

    fn flipped_origin(
        self,
        origin: Pixels,
        anchor_start: Pixels,
        anchor_extent: Pixels,
        extent: Pixels,
        low: Pixels,
        high: Pixels,
    ) -> Option<Pixels> {
        let tolerance = roundoff_tolerance([
            origin,
            anchor_start,
            anchor_extent,
            extent,
            self.offset,
            low,
            high,
        ]);
        match self.popup {
            AlignPoint::Start if origin + extent - high > tolerance => {
                Some(if self.popup == self.target {
                    origin - extent + anchor_extent
                } else {
                    anchor_start - extent - self.offset
                })
            }
            AlignPoint::End if low - origin > tolerance => Some(if self.popup == self.target {
                origin + extent - anchor_extent
            } else {
                anchor_start + anchor_extent - self.offset
            }),
            _ => None,
        }
    }

    fn shift_origin(
        self,
        origin: Pixels,
        anchor_start: Pixels,
        anchor_extent: Pixels,
        extent: Pixels,
        low: Pixels,
        high: Pixels,
    ) -> Pixels {
        let Some(buffer) = self.shift else {
            return origin;
        };
        let tolerance = roundoff_tolerance([
            origin,
            anchor_start,
            anchor_extent,
            extent,
            self.offset,
            low,
            high,
        ]);
        let mut shifted = origin;
        // Retain the offset along the main axis and stop following a trigger
        // once it leaves the visible region. Oversized bodies consider both edges.
        if low - origin > tolerance {
            shifted += low - origin + self.offset;
            if low + buffer - (anchor_start + anchor_extent) > tolerance {
                shifted += anchor_start + anchor_extent - low - buffer;
            }
        }
        if origin + extent - high > tolerance {
            shifted -= origin + extent - high - self.offset;
            if anchor_start - (high - buffer) > tolerance {
                shifted += anchor_start - high + buffer;
            }
        }
        shifted
    }
}

fn roundoff_tolerance(values: [Pixels; 7]) -> Pixels {
    let magnitude = values
        .into_iter()
        .map(f32::from)
        .filter(|value| value.is_finite())
        .map(f32::abs)
        .fold(1., f32::max);
    let spacing = magnitude.next_up() - magnitude;
    // An aligned edge uses up to four additions/subtractions. Budget their
    // representational error at the largest axis operand, rather than treating
    // cancellation around a viewport edge as real overflow. Positions themselves
    // remain unchanged; native layout still owns their eventual pixel snapping.
    px(4.
        * if spacing.is_finite() {
            spacing
        } else {
            magnitude * f32::EPSILON
        })
}

impl PopupGeometry {
    pub fn resolve(self) -> PopupLayout {
        let mut input = self;
        input.popup_size = input.popup_size.map(nonnegative);
        input.arrow_size = nonnegative(input.arrow_size);
        input.radius = nonnegative(input.radius);
        input.gap = finite(input.gap);
        input.offset = input.offset.map(finite);
        let mut placement = input.placement;
        let [mut x, mut y] = input.alignments();
        let mut bounds = Bounds::new(
            point(
                x.origin(
                    input.anchor.left(),
                    input.anchor.size.width,
                    input.popup_size.width,
                ),
                y.origin(
                    input.anchor.top(),
                    input.anchor.size.height,
                    input.popup_size.height,
                ),
            ),
            input.popup_size,
        );
        if input.auto_adjust_overflow {
            // Evaluate vertical, then horizontal reflection against the original
            // visible area, as the reference does. Ties favor the reflected edge.
            let original_area = visible_area(bounds, input.viewport);
            if let Some(top) = y.flipped_origin(
                bounds.top(),
                input.anchor.top(),
                input.anchor.size.height,
                bounds.size.height,
                input.viewport.top(),
                input.viewport.bottom(),
            ) {
                let candidate = Bounds::new(point(bounds.left(), top), bounds.size);
                if visible_area(candidate, input.viewport) >= original_area {
                    bounds = candidate;
                    y.offset = -y.offset;
                    placement = if placement.is_vertical() {
                        placement.opposite()
                    } else {
                        placement.opposite_alignment()
                    };
                }
            }
            if let Some(left) = x.flipped_origin(
                bounds.left(),
                input.anchor.left(),
                input.anchor.size.width,
                bounds.size.width,
                input.viewport.left(),
                input.viewport.right(),
            ) {
                let candidate = Bounds::new(point(left, bounds.top()), bounds.size);
                if visible_area(candidate, input.viewport) >= original_area {
                    bounds = candidate;
                    x.offset = -x.offset;
                    placement = if placement.is_vertical() {
                        placement.opposite_alignment()
                    } else {
                        placement.opposite()
                    };
                }
            }
            bounds.origin = point(
                x.shift_origin(
                    bounds.left(),
                    input.anchor.left(),
                    input.anchor.size.width,
                    bounds.size.width,
                    input.viewport.left(),
                    input.viewport.right(),
                ),
                y.shift_origin(
                    bounds.top(),
                    input.anchor.top(),
                    input.anchor.size.height,
                    bounds.size.height,
                    input.viewport.top(),
                    input.viewport.bottom(),
                ),
            );
        }
        let arrow = input
            .arrow
            .visible
            .then(|| input.arrow_at(placement, bounds));
        PopupLayout {
            placement,
            bounds,
            arrow,
        }
    }

    fn horizontal_arrow_inset(self) -> Pixels {
        if self.radius > px(12.) {
            self.radius + px(2.)
        } else {
            px(12.)
        }
    }

    fn edge_inset(self, placement: PopupPlacement) -> Pixels {
        if placement.is_vertical() {
            self.horizontal_arrow_inset()
        } else {
            px(8.)
        }
    }

    fn alignments(self) -> [AxisAlignment; 2] {
        use AlignPoint::{Center, End, Start};
        let placement = self.placement;
        let arrow_width = if self.arrow.visible {
            self.arrow_size
        } else {
            px(0.)
        };
        let distance = self.gap + arrow_width / 2.;
        let main = AxisAlignment {
            popup: if placement.before() { End } else { Start },
            target: if placement.before() { Start } else { End },
            offset: if placement.before() {
                -distance
            } else {
                distance
            },
            shift: placement.is_centered().then_some(px(0.)),
        };
        let cross = if placement.is_centered() {
            AxisAlignment {
                popup: Center,
                target: Center,
                offset: px(0.),
                shift: Some(self.edge_inset(placement) * 2. + arrow_width),
            }
        } else {
            let edge = if placement.leading() { Start } else { End };
            let centered = self.arrow.visible && self.arrow.point_at_center;
            // Side placements deliberately use the horizontal token twice for
            // alignment; their arrow's CSS inset still uses the vertical token.
            let inset = if placement.is_vertical() {
                self.horizontal_arrow_inset() + arrow_width / 2.
            } else {
                self.horizontal_arrow_inset() * 2. - arrow_width / 2.
            };
            AxisAlignment {
                popup: edge,
                target: if centered { Center } else { edge },
                offset: if !centered {
                    px(0.)
                } else if placement.leading() {
                    -inset
                } else {
                    inset
                },
                shift: None,
            }
        };
        let [mut x, mut y] = if placement.is_vertical() {
            [cross, main]
        } else {
            [main, cross]
        };
        x.offset += self.offset.x;
        y.offset += self.offset.y;
        [x, y]
    }

    fn arrow_at(self, placement: PopupPlacement, bounds: Bounds<Pixels>) -> Point<Pixels> {
        let vertical = placement.is_vertical();
        let extent = if vertical {
            bounds.size.width
        } else {
            bounds.size.height
        };
        let desired = if placement.is_centered() {
            // Aim at the midpoint of the overlapping intervals, including when
            // the trigger is wider than the body or only partly visible.
            if vertical {
                (self.anchor.left().max(bounds.left()) + self.anchor.right().min(bounds.right()))
                    / 2.
                    - bounds.left()
            } else {
                (self.anchor.top().max(bounds.top()) + self.anchor.bottom().min(bounds.bottom()))
                    / 2.
                    - bounds.top()
            }
        } else {
            let inset = self.edge_inset(placement) + self.arrow_size / 2.;
            if placement.leading() {
                inset
            } else {
                extent - inset
            }
        };
        // Corner radii do not displace the reference arrow. Component minimum
        // sizes normally provide room; bound only degenerate custom bodies.
        let safe = (self.arrow_size / 2.).min(extent / 2.);
        let cross = desired.clamp(safe, extent - safe);
        if vertical {
            point(
                cross,
                if placement.before() {
                    bounds.size.height
                } else {
                    px(0.)
                },
            )
        } else {
            point(
                if placement.before() {
                    bounds.size.width
                } else {
                    px(0.)
                },
                cross,
            )
        }
    }
}

fn visible_area(bounds: Bounds<Pixels>, viewport: Bounds<Pixels>) -> f32 {
    let width =
        (bounds.right().min(viewport.right()) - bounds.left().max(viewport.left())).max(px(0.));
    let height =
        (bounds.bottom().min(viewport.bottom()) - bounds.top().max(viewport.top())).max(px(0.));
    f32::from(width) * f32::from(height)
}

fn finite(value: Pixels) -> Pixels {
    if f32::from(value).is_finite() {
        value
    } else {
        px(0.)
    }
}

fn nonnegative(value: Pixels) -> Pixels {
    finite(value).max(px(0.))
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::size;

    fn input(placement: PopupPlacement) -> PopupGeometry {
        PopupGeometry {
            anchor: Bounds::new(point(px(200.), px(200.)), size(px(80.), px(32.))),
            popup_size: size(px(120.), px(60.)),
            viewport: Bounds::new(point(px(0.), px(0.)), size(px(640.), px(480.))),
            placement,
            arrow: PopupArrow::default(),
            arrow_size: px(16.),
            gap: px(4.),
            radius: px(6.),
            offset: Point::default(),
            auto_adjust_overflow: true,
        }
    }

    #[test]
    fn all_placements_preserve_requested_alignment_with_room() {
        use PopupPlacement::*;
        for (placement, x, y) in [
            (Top, 180., 128.),
            (TopLeft, 200., 128.),
            (TopRight, 160., 128.),
            (Bottom, 180., 244.),
            (BottomLeft, 200., 244.),
            (BottomRight, 160., 244.),
            (Left, 68., 186.),
            (LeftTop, 68., 200.),
            (LeftBottom, 68., 172.),
            (Right, 292., 186.),
            (RightTop, 292., 200.),
            (RightBottom, 292., 172.),
        ] {
            let result = input(placement).resolve();
            assert_eq!(result.placement, placement);
            assert_eq!(result.bounds.origin, point(px(x), px(y)), "{placement:?}");
        }
    }

    #[test]
    fn all_sides_flip_when_primary_side_overflows() {
        for placement in PopupPlacement::ALL {
            let mut geometry = input(placement);
            if placement.is_vertical() {
                geometry.anchor.origin.y = if placement.before() { px(0.) } else { px(448.) };
            } else {
                geometry.anchor.origin.x = if placement.before() { px(0.) } else { px(560.) };
            }
            assert_eq!(geometry.resolve().placement, placement.opposite());
        }
    }

    #[test]
    fn centered_shift_retains_arrow_buffer_for_a_narrow_edge_trigger() {
        let mut geometry = input(PopupPlacement::Top);
        geometry.anchor.origin.x = px(0.);
        let result = geometry.resolve();
        assert_eq!(result.bounds.left(), px(0.));
        assert_eq!(result.arrow.unwrap().x, px(40.));
        geometry.anchor.size.width = px(2.);
        let result = geometry.resolve();
        assert_eq!(result.bounds.left(), px(-38.));
        assert_eq!(result.bounds.left() + result.arrow.unwrap().x, px(1.));
    }

    #[test]
    fn aligned_placement_flips_instead_of_shifting() {
        let mut geometry = input(PopupPlacement::TopLeft);
        geometry.anchor.origin.x = px(540.);
        let result = geometry.resolve();
        assert_eq!(result.placement, PopupPlacement::TopRight);
        assert_eq!(result.bounds.right(), geometry.anchor.right());
    }

    #[test]
    fn fractional_zero_edge_alignment_does_not_flip_from_f32_cancellation() {
        // A compact 150% native TimePicker: both widths are exact f32 values
        // measured in logical pixels. Their mathematically aligned zero edge
        // acquires one subtraction ulp below zero.
        let mut geometry = input(PopupPlacement::BottomRight);
        geometry.arrow.visible = false;
        geometry.anchor.origin.x = px(48. / 1.5);
        geometry.anchor.size.width = px(172. / 1.5);
        geometry.popup_size.width = px(220. / 1.5);
        let origin =
            geometry.anchor.left() + geometry.anchor.size.width - geometry.popup_size.width;
        assert_eq!(f32::from(origin), -0.000_015_258_789);
        let result = geometry.resolve();
        assert_eq!(result.placement, PopupPlacement::BottomRight);
        assert_eq!(
            result.bounds.left(),
            origin,
            "the solver does not nudge coordinates"
        );

        let mut vertical = input(PopupPlacement::TopLeft);
        vertical.arrow.visible = false;
        vertical.gap = px(0.);
        vertical.anchor.origin.y = geometry.anchor.right();
        vertical.popup_size.height = geometry.popup_size.width;
        let origin = vertical.anchor.top() - vertical.popup_size.height;
        assert!(origin < px(0.));
        let result = vertical.resolve();
        assert_eq!(result.placement, PopupPlacement::TopLeft);
        assert_eq!(result.bounds.top(), origin);
    }

    #[test]
    fn centered_zero_edge_roundoff_does_not_trigger_a_shift() {
        let mut geometry = input(PopupPlacement::Bottom);
        geometry.arrow.visible = false;
        geometry.anchor.origin.x = px(32.);
        geometry.anchor.size.width = px(172. / 1.5);
        geometry.popup_size.width = px(268. / 1.5);
        let origin = geometry.anchor.left() + geometry.anchor.size.width / 2.
            - geometry.popup_size.width / 2.;
        assert!(origin < px(0.));
        let result = geometry.resolve();
        assert_eq!(result.placement, PopupPlacement::Bottom);
        assert_eq!(result.bounds.left(), origin);
    }

    #[test]
    fn fractional_roundoff_budget_still_flips_real_low_and_high_edge_overflow() {
        let mut low = input(PopupPlacement::BottomRight);
        low.arrow.visible = false;
        low.anchor.origin.x = px(31.75);
        low.anchor.size.width = px(172. / 1.5);
        low.popup_size.width = px(220. / 1.5);
        assert_eq!(low.resolve().placement, PopupPlacement::BottomLeft);

        let mut high = input(PopupPlacement::BottomLeft);
        high.arrow.visible = false;
        high.anchor.origin.x = high.viewport.right() - high.popup_size.width + px(0.25);
        assert_eq!(high.resolve().placement, PopupPlacement::BottomRight);
    }

    #[test]
    fn centered_arrows_and_hidden_arrows_adjust_gap_and_alignment() {
        let mut geometry = input(PopupPlacement::TopLeft);
        geometry.arrow.point_at_center = true;
        let result = geometry.resolve();
        assert_eq!(
            result.bounds.left() + result.arrow.unwrap().x,
            geometry.anchor.center().x
        );
        geometry.arrow.visible = false;
        let result = geometry.resolve();
        assert_eq!(result.bounds.origin, point(px(200.), px(136.)));
        assert_eq!(result.arrow, None);
    }

    #[test]
    fn disabling_overflow_preserves_offscreen_position() {
        let mut geometry = input(PopupPlacement::Top);
        geometry.anchor.origin = point(px(0.), px(0.));
        geometry.auto_adjust_overflow = false;
        let result = geometry.resolve();
        assert_eq!(result.placement, PopupPlacement::Top);
        assert_eq!(result.bounds.origin, point(px(-20.), px(-72.)));
    }

    #[test]
    fn tiny_and_oversized_bodies_keep_finite_bounded_arrow_geometry() {
        for extent in [0., 1., 3., 2000.] {
            for placement in PopupPlacement::ALL {
                let mut geometry = input(placement);
                geometry.popup_size = size(px(extent), px(extent));
                let result = geometry.resolve();
                let arrow = result.arrow.unwrap();
                assert!(arrow.x >= px(0.) && arrow.x <= px(extent));
                assert!(arrow.y >= px(0.) && arrow.y <= px(extent));
            }
        }
    }
}
