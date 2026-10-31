// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Intrinsic widths are measured before native parent layout. The original
//! child nodes then participate in flex layout, so rich descendants determine
//! row heights, hitboxes and focus without a second render or a notify loop.

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, ParentElement, Pixels, Styled, Window, div, px,
    size,
};

use super::geometry::{Constraint, Intrinsic, solve_tracks, span_widths};
use crate::{Direction, behavior::text::TextIntrinsicHandle};
use std::{cell::Cell as ValueCell, rc::Rc};

pub(super) type ColonHandle = Rc<ValueCell<Option<(f32, f32)>>>;

pub(super) struct TextPart {
    pub metrics: TextIntrinsicHandle,
    pub minimum_em: bool,
}

#[derive(Default)]
pub(super) struct CellPrecision {
    pub label: Option<TextPart>,
    pub content: Option<TextPart>,
    pub colon: Option<ColonHandle>,
    /// A horizontal inline pair must advertise the preferred width its native
    /// flex descendants actually consume. Correcting only the table track to
    /// raw glyph advances would underallocate those still-rounded descendants
    /// and wrap a value even when its parent is sized to max-content.
    pub preserve_native_preferred: bool,
}

impl CellPrecision {
    fn corrections(&self) -> (f32, f32) {
        let mut minimum = 0.;
        let mut preferred = 0.;
        for part in [&self.label, &self.content].into_iter().flatten() {
            if let Some(metrics) = part.metrics.get() {
                let raw_min = f32::from(metrics.minimum);
                let raw_max = f32::from(metrics.preferred);
                let ceil = |value: f32| (value * metrics.scale).ceil() / metrics.scale;
                let floor = if part.minimum_em {
                    f32::from(metrics.font_size)
                } else {
                    0.
                };
                let snapped_floor = (floor * metrics.scale - 0.5).ceil() / metrics.scale;
                minimum += raw_min.max(floor) - ceil(raw_min).max(snapped_floor);
                preferred += raw_max - ceil(raw_max);
            }
        }
        if let Some((raw, snapped)) = self.colon.as_ref().and_then(|handle| handle.get()) {
            minimum += raw - snapped;
            preferred += raw - snapped;
        }
        (
            minimum,
            if self.preserve_native_preferred {
                0.
            } else {
                preferred
            },
        )
    }
}

pub(super) struct Cell {
    pub start: usize,
    pub span: usize,
    pub constrained: bool,
    pub width_adjustment: f32,
    pub precision: CellPrecision,
    pub element: AnyElement,
}

pub(super) struct Table {
    pub rows: Vec<Vec<Cell>>,
    pub tracks: usize,
    pub direction: Direction,
    pub border_queue: Option<super::paint::BorderQueue>,
}

impl IntoElement for Table {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

/// Reuses a native child's previously requested layout node. Anonymous wrappers
/// preserve the element-ID namespace across measuring and final prepainting.
struct Measured {
    child: AnyElement,
    layout: LayoutId,
}

impl IntoElement for Measured {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for Measured {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: &mut Window,
        _: &mut App,
    ) -> (LayoutId, ()) {
        (self.layout, ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.child.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.child.paint(window, cx);
    }
}

impl Element for Table {
    type RequestLayoutState = AnyElement;
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some("view".into())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, AnyElement) {
        let min_space = size(AvailableSpace::MinContent, AvailableSpace::MaxContent);
        let max_space = size(AvailableSpace::MaxContent, AvailableSpace::MaxContent);
        let mut measured = Vec::new();
        let mut constraints = Vec::new();
        for row in std::mem::take(&mut self.rows) {
            let mut cells = Vec::with_capacity(row.len());
            for mut cell in row {
                let layout = cell.element.request_layout(window, cx);
                let adjustment = if cell.constrained {
                    0.
                } else {
                    cell.width_adjustment
                };
                let native_minimum =
                    f32::from(cell.element.layout_as_root(min_space, window, cx).width);
                let native_preferred =
                    f32::from(cell.element.layout_as_root(max_space, window, cx).width);
                let (minimum_correction, preferred_correction) = if cell.constrained {
                    (0., 0.)
                } else {
                    cell.precision.corrections()
                };
                let minimum = (native_minimum + adjustment + minimum_correction).max(0.);
                let preferred_adjustment = if cell.precision.preserve_native_preferred {
                    // Inline descendants consume native text and padding
                    // advances. Keep their complete preferred contribution,
                    // including padding rounded at fractional display scales.
                    0.
                } else {
                    adjustment + preferred_correction
                };
                let preferred = (native_preferred + preferred_adjustment).max(minimum);
                // compute_layout clears cached absolute coordinates when this
                // root has been computed before. Do not read bounds afterwards:
                // final parent layout must establish all descendant positions.
                window.compute_layout(layout, max_space, cx);
                constraints.push(Constraint {
                    start: cell.start,
                    span: cell.span,
                    widths: Intrinsic {
                        minimum,
                        preferred,
                        constrained: cell.constrained,
                    },
                });
                cells.push((
                    cell.start,
                    cell.span,
                    Measured {
                        child: cell.element,
                        layout,
                    },
                ));
            }
            measured.push(cells);
        }
        let tracks = solve_tracks(self.tracks, &mut constraints);
        let minimum = tracks.iter().map(|track| track.minimum).sum::<f32>();
        let has_auto = tracks
            .iter()
            .any(|track| !track.constrained && track.preferred > 0.);
        let preferred = tracks.iter().map(|track| track.preferred).sum::<f32>();
        let mut body = div()
            .flex()
            .flex_col()
            .w_full()
            .min_w(px(minimum))
            .child(IntrinsicWidth { minimum, preferred });
        for row in measured {
            let shrink_sum = row
                .iter()
                .map(|(start, span, _)| {
                    let widths = span_widths(&tracks, *start, *span);
                    let basis = widths.preferred.max(0.01);
                    (basis - widths.minimum).max(0.) / basis
                })
                .sum::<f32>();
            // Flex deliberately leaves some negative free space undistributed
            // when shrink factors sum to less than one. Table interpolation
            // must consume all of it, including rows with a large colspan.
            let shrink_scale = if shrink_sum > 0. && shrink_sum < 1. {
                1. / shrink_sum
            } else {
                1.
            };
            let mut native_row = div().flex().w_full().min_w(px(minimum));
            if self.direction == Direction::Rtl {
                native_row = native_row.flex_row_reverse();
            }
            for (start, span, child) in row {
                let widths = span_widths(&tracks, start, span);
                let basis = widths.preferred.max(0.01);
                let mut wrapper = div()
                    .flex()
                    .flex_col()
                    .min_w(px(widths.minimum))
                    .child(child);
                wrapper.style().flex_basis = Some(px(basis).into());
                let growth = if has_auto {
                    tracks[start..start + span]
                        .iter()
                        .filter(|track| !track.constrained)
                        .map(|track| track.preferred)
                        .sum()
                } else {
                    basis
                };
                wrapper.style().flex_grow = Some(growth);
                wrapper.style().flex_shrink =
                    Some((basis - widths.minimum).max(0.) / basis * shrink_scale);
                native_row = native_row.child(wrapper);
            }
            body = body.child(native_row);
        }
        let mut body = body.into_any_element();
        let layout = body.request_layout(window, cx);
        (layout, body)
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        body: &mut AnyElement,
        window: &mut Window,
        cx: &mut App,
    ) {
        body.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        body: &mut AnyElement,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        body.paint(window, cx);
        if let Some(queue) = &self.border_queue {
            queue.flush(window);
        }
    }
}

/// Preserve the unrounded table max-content contribution until GPUI performs
/// its single measured-leaf device-pixel ceiling. Rounding each column's basis
/// independently can otherwise make a shrink-to-fit table wrap at max-content.
struct IntrinsicWidth {
    minimum: f32,
    preferred: f32,
}
impl IntoElement for IntrinsicWidth {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for IntrinsicWidth {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        _: &mut App,
    ) -> (LayoutId, ()) {
        let (minimum, preferred) = (self.minimum, self.preferred);
        let layout = window.request_measured_layout(
            gpui::Style::default(),
            move |known, available, _, _| {
                let width = known.width.unwrap_or(match available.width {
                    AvailableSpace::MinContent => px(minimum),
                    AvailableSpace::MaxContent => px(preferred),
                    AvailableSpace::Definite(width) => width.min(px(preferred)),
                });
                size(width, px(0.))
            },
        );
        (layout, ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut Window,
        _: &mut App,
    ) {
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        _: &mut Window,
        _: &mut App,
    ) {
    }
}
