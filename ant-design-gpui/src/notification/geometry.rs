// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::NotificationPlacement;
use gpui::{Bounds, Pixels, Size, point, px};

pub(super) fn card_bounds(
    placement: NotificationPlacement,
    viewport: Size<Pixels>,
    measured: Size<Pixels>,
    edge: f32,
    inset: f32,
    offset: f32,
    scroll: f32,
) -> Bounds<Pixels> {
    let x = if placement.center() {
        (viewport.width - measured.width) / 2.
    } else if placement.right() {
        viewport.width - measured.width - px(edge)
    } else {
        px(edge)
    };
    let y = if placement.bottom() {
        viewport.height - px(inset + offset - scroll) - measured.height
    } else {
        px(inset + offset - scroll)
    };
    Bounds::new(point(x, y), measured)
}

/// Newest card is at the configured edge in both top and bottom placements.
pub(super) fn offsets(heights: &[f32], gap: f32, collapsed: bool) -> Vec<f32> {
    let mut y = 0.;
    heights
        .iter()
        .enumerate()
        .map(|(index, height)| {
            let offset = if collapsed && index > 0 {
                y + 8. - height
            } else {
                y
            };
            y = if collapsed {
                offset + height
            } else {
                offset + height + gap
            };
            offset
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn measured_heights_drive_expanded_and_collapsed_positions() {
        assert_eq!(offsets(&[100., 80., 120.], 16., false), [0., 116., 212.]);
        assert_eq!(offsets(&[100., 80., 120.], 16., true), [0., 28., -4.]);
    }
    #[test]
    fn all_placements_anchor_to_physical_edges() {
        use gpui::size;
        let viewport = size(px(800.), px(600.));
        let card = size(px(384.), px(100.));
        for (placement, x, y) in [
            (NotificationPlacement::Top, 208., 24.),
            (NotificationPlacement::TopLeft, 24., 24.),
            (NotificationPlacement::TopRight, 392., 24.),
            (NotificationPlacement::Bottom, 208., 476.),
            (NotificationPlacement::BottomLeft, 24., 476.),
            (NotificationPlacement::BottomRight, 392., 476.),
        ] {
            assert_eq!(
                card_bounds(placement, viewport, card, 24., 24., 0., 0.).origin,
                point(px(x), px(y))
            );
        }
    }
}
