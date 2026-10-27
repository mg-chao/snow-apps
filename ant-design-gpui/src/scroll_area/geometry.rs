// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::ScrollAxis;
use crate::Direction;
use gpui::{Bounds, Pixels, Point, point, px, size};

/// All values use the viewport's physical coordinate system. GPUI offsets are
/// negative as content moves toward the top/left; scrollbar distances are positive.
#[derive(Clone, Copy, Debug)]
pub(super) struct BarGeometry {
    pub track: Bounds<Pixels>,
    pub thumb: Bounds<Pixels>,
    pub vertical: bool,
    pub maximum: Pixels,
}

impl BarGeometry {
    pub fn new(
        track: Bounds<Pixels>,
        vertical: bool,
        viewport: Pixels,
        maximum: Pixels,
        offset: Pixels,
        minimum_thumb: Pixels,
    ) -> Option<Self> {
        let length = if vertical {
            track.size.height
        } else {
            track.size.width
        };
        if length <= px(0.) || viewport <= px(0.) || maximum <= px(0.) {
            return None;
        }
        let thumb_length = (length * (viewport / (viewport + maximum)))
            .max(minimum_thumb.min(length))
            .min(length);
        let travel = length - thumb_length;
        let distance = travel * (-offset / maximum).clamp(0., 1.);
        let thumb = if vertical {
            Bounds::new(
                track.origin + point(px(0.), distance),
                size(track.size.width, thumb_length),
            )
        } else {
            Bounds::new(
                track.origin + point(distance, px(0.)),
                size(thumb_length, track.size.height),
            )
        };
        Some(Self {
            track,
            thumb,
            vertical,
            maximum,
        })
    }

    pub fn coordinate(self, position: Point<Pixels>) -> Pixels {
        if self.vertical {
            position.y
        } else {
            position.x
        }
    }

    pub fn thumb_start(self) -> Pixels {
        self.coordinate(self.thumb.origin)
    }

    /// Keep the visible thumb against the viewport's outer edge, matching the
    /// Qt overlay scrollbar. Hover expansion grows inward without moving that
    /// edge or the larger pointer target. Clamp thickness for tiny viewports.
    pub fn visual_thumb(self, thickness: Pixels, direction: Direction) -> Bounds<Pixels> {
        let cross = if self.vertical {
            self.track.size.width
        } else {
            self.track.size.height
        };
        let thickness = thickness.clamp(px(0.), cross);
        if self.vertical {
            Bounds::new(
                point(
                    if direction == Direction::Rtl {
                        self.thumb.left()
                    } else {
                        self.thumb.right() - thickness
                    },
                    self.thumb.top(),
                ),
                size(thickness, self.thumb.size.height),
            )
        } else {
            Bounds::new(
                point(self.thumb.left(), self.thumb.bottom() - thickness),
                size(self.thumb.size.width, thickness),
            )
        }
    }

    /// Convert a pointer coordinate into a native negative scroll offset. The
    /// original grab distance prevents the thumb from jumping when first pressed.
    pub fn offset_for_pointer(self, position: Point<Pixels>, grab: Pixels) -> Pixels {
        let travel = if self.vertical {
            self.track.size.height - self.thumb.size.height
        } else {
            self.track.size.width - self.thumb.size.width
        };
        if travel <= px(0.) {
            return px(0.);
        }
        let fraction = ((self.coordinate(position) - self.coordinate(self.track.origin) - grab)
            / travel)
            .clamp(0., 1.);
        -self.maximum * fraction
    }
}

pub(super) fn clamp_offset(
    offset: Point<Pixels>,
    maximum: Point<Pixels>,
    axis: ScrollAxis,
) -> Point<Pixels> {
    let clamp = |value: Pixels, max: Pixels, enabled: bool| {
        if enabled && f32::from(value).is_finite() {
            value.clamp(-max.max(px(0.)), px(0.))
        } else {
            px(0.)
        }
    };
    point(
        clamp(offset.x, maximum.x, axis.horizontal()),
        clamp(offset.y, maximum.y, axis.vertical()),
    )
}

/// Preserve a real horizontal gesture. Shift converts a vertical wheel only when
/// horizontal overflow exists, including at its endpoints (where it must bubble).
pub(super) fn wheel_delta(
    delta: Point<Pixels>,
    shift: bool,
    axis: ScrollAxis,
    maximum: Point<Pixels>,
) -> Point<Pixels> {
    if axis.horizontal() && maximum.x > px(0.) && (shift || axis == ScrollAxis::Horizontal) {
        point(if delta.x != px(0.) { delta.x } else { delta.y }, px(0.))
    } else {
        point(
            if axis.horizontal() { delta.x } else { px(0.) },
            if axis.vertical() { delta.y } else { px(0.) },
        )
    }
}
