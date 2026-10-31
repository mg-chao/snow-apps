// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Paint circular radio dots and final group button decorations.

use std::{cell::RefCell, rc::Rc};

use gpui::{
    Bounds, Corners, Edges, FocusHandle, Hsla, IntoElement, PathBuilder, Pixels, Styled, Window,
    canvas, outline, point, px,
};

/// Paint the animated dot at the center of the ring as a vector circle.
/// GPUI rounds quad edges independently to device pixels, which shifts an
/// odd-pixel dot by half a pixel inside an even-pixel ring at some DPI scales.
pub(crate) fn paint_dot(
    bounds: Bounds<Pixels>,
    diameter: Pixels,
    color: Hsla,
    window: &mut Window,
) {
    if diameter <= px(0.) || color.a <= 0. {
        return;
    }
    let center = point(
        (window.pixel_snap(bounds.left()) + window.pixel_snap(bounds.right())) / 2.,
        (window.pixel_snap(bounds.top()) + window.pixel_snap(bounds.bottom())) / 2.,
    );
    let radius = diameter / 2.;
    let mut path = PathBuilder::fill();
    path.move_to(point(center.x - radius, center.y));
    path.arc_to(
        point(radius, radius),
        px(0.),
        false,
        true,
        point(center.x + radius, center.y),
    );
    path.arc_to(
        point(radius, radius),
        px(0.),
        false,
        true,
        point(center.x - radius, center.y),
    );
    path.close();
    if let Ok(path) = path.build() {
        window.paint_path(path, color);
    }
}

/// One button's effective paint data, captured after native layout.
///
/// `opacity` is the button's own opacity. The group and its ancestors are still
/// active while painting the final canvas and must not be included here.
pub(crate) struct ButtonPaint {
    pub bounds: Bounds<Pixels>,
    pub borders: Edges<Pixels>,
    pub radii: Corners<Pixels>,
    pub color: Hsla,
    pub checked: bool,
    pub focus: FocusHandle,
    pub focus_color: Hsla,
    pub focus_width: Pixels,
    pub opacity: f32,
}

/// A bounded, retained set of records in display order.
///
/// The canvas stays in the group's ordinary painting pass, preserving native
/// clipping, scrolling and opacity. It owns no hitbox, focus stop or animation.
#[derive(Clone, Default)]
pub(crate) struct ButtonPaintQueue(Rc<RefCell<Vec<Option<ButtonPaint>>>>);

impl ButtonPaintQueue {
    /// Clear prior-frame geometry while reusing the existing option allocation.
    pub fn reset(&self, count: usize) {
        let mut records = self.0.borrow_mut();
        records.resize_with(count, || None);
        records.iter_mut().for_each(|record| *record = None);
    }

    pub fn record(&self, index: usize, paint: ButtonPaint) {
        let mut records = self.0.borrow_mut();
        debug_assert!(index < records.len(), "button paint index is mounted");
        if let Some(record) = records.get_mut(index) {
            *record = Some(paint);
        }
    }

    /// Append this element after every option in a relatively positioned group.
    pub fn element(&self) -> impl IntoElement {
        let records = self.0.clone();
        canvas(
            |_, _, _| (),
            move |_, (), window, _| {
                let records = records.borrow();
                for record in records.iter().flatten().filter(|record| record.checked) {
                    paint_selected_border(record, window);
                }
                if window.last_input_was_keyboard() {
                    for record in records.iter().flatten() {
                        paint_focus_ring(record, window);
                    }
                }
            },
        )
        .absolute()
        .top(px(0.))
        .left(px(0.))
        .size_full()
    }
}

fn paint_selected_border(record: &ButtonPaint, window: &mut Window) {
    if record.bounds.is_empty() || record.color.a <= 0. || record.opacity <= 0. {
        return;
    }
    window.paint_quad(
        outline(
            record.bounds,
            record.color.opacity(record.opacity),
            Default::default(),
        )
        .border_widths(record.borders)
        .corner_radii(record.radii.clamp_radii_for_quad_size(record.bounds.size)),
    );
}

fn paint_focus_ring(record: &ButtonPaint, window: &mut Window) {
    if !record.focus.is_focused(window)
        || record.bounds.is_empty()
        || !f32::from(record.focus_width).is_finite()
        || record.focus_width <= px(0.)
        || record.opacity <= 0.
    {
        return;
    }
    // Match FocusRing: a transparent 1px gap, with rounded corners clamped to
    // the button before expanding. Adjacent fills cannot cover this last pass.
    let extent = record.focus_width + px(1.);
    let corners = record
        .radii
        .map(|radius| finite_nonnegative(*radius))
        .clamp_radii_for_quad_size(record.bounds.size)
        .map(|radius| *radius + extent);
    window.paint_quad(
        outline(
            record.bounds.dilate(extent),
            record.focus_color.opacity(record.opacity),
            Default::default(),
        )
        .border_widths(record.focus_width)
        .corner_radii(corners),
    );
}

fn finite_nonnegative(value: Pixels) -> Pixels {
    if f32::from(value).is_finite() && value > px(0.) {
        value
    } else {
        px(0.)
    }
}
