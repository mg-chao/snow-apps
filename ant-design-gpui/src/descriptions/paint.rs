// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native border layout with a bounded, color-independent dashed/dotted painter.

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Corners, Edges, Element, ElementId, GlobalElementId, Hsla,
    InspectorElementId, IntoElement, LayoutId, Path, PathBuilder, Pixels, Point, Role, Size,
    Window, accesskit, point, px, quad,
};

use crate::theme::LineType;
use std::{cell::RefCell, rc::Rc};

const MAX_MARKS: usize = 2048;
const MAX_PATH_VERTICES: usize = 48_000;

/// Border widths remain reserved by the native child. Make its component-owned
/// border paint transparent before wrapping it; semantic overrides can supply
/// the final widths, radii and color here. Multiply color by the child's own
/// opacity, because its opacity context ends when its paint method returns.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct BorderPaint {
    pub widths: Edges<Pixels>,
    pub radii: Corners<Pixels>,
    pub color: Hsla,
    pub line_type: LineType,
    /// None paints the usual border inside its box. Some paints each owned
    /// edge at its boundary, displaced outward by the corresponding value.
    pub outsets: Option<Edges<Pixels>>,
}

pub(super) fn bordered<E: IntoElement>(element: E, border: BorderPaint) -> Bordered<E::Element> {
    Bordered {
        element: element.into_element(),
        border,
        cache: Default::default(),
        queue: None,
        before: false,
    }
}

/// Paint the enclosing view's border below its cells, as CSS paints a table's
/// enclosing decoration. Native children retain their own layout and identity.
pub(super) fn behind_bordered<E: IntoElement>(
    element: E,
    border: BorderPaint,
) -> Bordered<E::Element> {
    Bordered {
        before: true,
        ..bordered(element, border)
    }
}

pub(super) fn deferred_bordered<E: IntoElement>(
    element: E,
    border: BorderPaint,
    queue: BorderQueue,
) -> Bordered<E::Element> {
    Bordered {
        queue: Some(queue),
        ..bordered(element, border)
    }
}

/// Interior strokes paint after every cell background. Capture the ancestor's
/// clip with each stroke, and drain the queue after the table's native body.
#[derive(Clone, Default)]
pub(super) struct BorderQueue(Rc<RefCell<Vec<QueuedBorder>>>);

type CacheHandle = Rc<RefCell<BorderCache>>;

struct QueuedBorder {
    bounds: Bounds<Pixels>,
    border: BorderPaint,
    cache: CacheHandle,
    mask: gpui::ContentMask<Pixels>,
}

impl BorderQueue {
    pub(super) fn flush(&self, window: &mut Window) {
        let mut records = self.0.borrow_mut();
        for record in records.drain(..) {
            window.with_content_mask(Some(record.mask), |window| {
                record
                    .cache
                    .borrow_mut()
                    .paint(record.bounds, record.border, window);
            });
        }
    }
}

pub(super) struct Bordered<E> {
    element: E,
    border: BorderPaint,
    cache: CacheHandle,
    queue: Option<BorderQueue>,
    before: bool,
}

impl<E: Element> IntoElement for Bordered<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for Bordered<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;

    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }
    fn a11y_role(&self) -> Option<Role> {
        self.element.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(state, builder);
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector, window, cx)
    }
    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.element
            .prepaint(id, inspector, bounds, state, window, cx)
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        let cache = if let Some(id) = id {
            window.with_element_state(id, |cache: Option<CacheHandle>, _| {
                let cache = cache.unwrap_or_default();
                (cache.clone(), cache)
            })
        } else {
            self.cache.clone()
        };
        if self.before {
            cache.borrow_mut().paint(bounds, self.border, window);
        }
        self.element
            .paint(id, inspector, bounds, state, prepaint, window, cx);
        if let Some(queue) = &self.queue {
            queue.0.borrow_mut().push(QueuedBorder {
                bounds,
                border: self.border,
                cache,
                mask: window.content_mask(),
            });
        } else if !self.before {
            cache.borrow_mut().paint(bounds, self.border, window);
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Geometry {
    size: [f32; 2],
    widths: [f32; 4],
    radii: [f32; 4],
    scale: f32,
    line_type: LineType,
    outsets: Option<[f32; 4]>,
}

impl Geometry {
    fn new(size: Size<Pixels>, border: BorderPaint, scale: f32) -> Option<Self> {
        let size = [f32::from(size.width), f32::from(size.height)];
        let widths = [
            border.widths.top,
            border.widths.right,
            border.widths.bottom,
            border.widths.left,
        ]
        .map(f32::from);
        let radii = [
            border.radii.top_left,
            border.radii.top_right,
            border.radii.bottom_right,
            border.radii.bottom_left,
        ]
        .map(f32::from);
        let outsets = border
            .outsets
            .map(|edges| [edges.top, edges.right, edges.bottom, edges.left].map(f32::from));
        if !scale.is_finite()
            || scale <= 0.
            || size.iter().any(|value| !value.is_finite() || *value <= 0.)
            || widths.iter().chain(&radii).any(|value| !value.is_finite())
            || outsets.is_some_and(|edges| edges.iter().any(|value| !value.is_finite()))
        {
            return None;
        }
        let widths = widths.map(|width| if width <= 0. { 0. } else { width });
        let limit = size[0].min(size[1]);
        if widths
            .iter()
            .any(|width| !width.is_finite() || *width >= limit)
            || widths.iter().all(|width| *width <= 0.)
        {
            return None;
        }
        Some(Self {
            size,
            widths,
            radii: radii.map(|radius| radius.clamp(0., limit / 2.)),
            scale,
            line_type: border.line_type,
            outsets,
        })
    }

    fn contours(self) -> Vec<Contour> {
        if let Some([top, right, bottom, left]) = self.outsets {
            // Browser collapsed-border painting starts a new stroke for each
            // grid edge. Preserve this phase independently at every joint.
            let [width, height] = self.size;
            return [
                (
                    [point(px(0.), px(-top)), point(px(width), px(-top))],
                    self.widths[0],
                ),
                (
                    [
                        point(px(width + right), px(0.)),
                        point(px(width + right), px(height)),
                    ],
                    self.widths[1],
                ),
                (
                    [
                        point(px(0.), px(height + bottom)),
                        point(px(width), px(height + bottom)),
                    ],
                    self.widths[2],
                ),
                (
                    [point(px(-left), px(0.)), point(px(-left), px(height))],
                    self.widths[3],
                ),
            ]
            .into_iter()
            .filter(|(_, thickness)| *thickness > 0.)
            .map(|(points, width)| Contour {
                points: points.to_vec(),
                width,
                closed: false,
            })
            .collect();
        }
        let uniform =
            self.widths[0] > 0. && self.widths.iter().all(|width| *width == self.widths[0]);
        if uniform {
            let width = self.widths[0];
            let inset = width / 2.;
            let [outer_width, outer_height] = self.size;
            let [left, top, right, bottom] =
                [inset, inset, outer_width - inset, outer_height - inset];
            let [tl, tr, br, bl] = self.radii.map(|radius| (radius - inset).max(0.));
            let mut points = vec![
                point(px(left + tl), px(top)),
                point(px(right - tr), px(top)),
            ];
            arc(
                &mut points,
                [right - tr, top + tr],
                tr,
                -std::f32::consts::FRAC_PI_2,
                self.scale,
            );
            points.push(point(px(right), px(bottom - br)));
            arc(&mut points, [right - br, bottom - br], br, 0., self.scale);
            points.push(point(px(left + bl), px(bottom)));
            arc(
                &mut points,
                [left + bl, bottom - bl],
                bl,
                std::f32::consts::FRAC_PI_2,
                self.scale,
            );
            points.push(point(px(left), px(top + tl)));
            arc(
                &mut points,
                [left + tl, top + tl],
                tl,
                std::f32::consts::PI,
                self.scale,
            );
            return vec![Contour {
                points,
                width,
                closed: true,
            }];
        }
        // Internal table separators are straight. Join consecutive equal-width
        // edges into one contour, so a bottom/end junction is painted once.
        let mut contours = Vec::with_capacity(4);
        let mut visited = [false; 4];
        for start in 0..4 {
            let width = self.widths[start];
            let previous = (start + 3) % 4;
            if visited[start] || width <= 0. || self.widths[previous] == width {
                continue;
            }
            let inset = width / 2.;
            let corners = [
                point(px(inset), px(inset)),
                point(px(self.size[0] - inset), px(inset)),
                point(px(self.size[0] - inset), px(self.size[1] - inset)),
                point(px(inset), px(self.size[1] - inset)),
            ];
            let mut points = vec![corners[start]];
            let mut edge = start;
            while !visited[edge] && self.widths[edge] == width {
                visited[edge] = true;
                points.push(corners[(edge + 1) % 4]);
                edge = (edge + 1) % 4;
            }
            contours.push(Contour {
                points,
                width,
                closed: false,
            });
        }
        contours
    }

    fn paths(self) -> Vec<Path<Pixels>> {
        self.contours()
            .into_iter()
            .filter_map(|contour| contour.build(self.line_type, self.scale, self.outsets.is_some()))
            .collect()
    }
}

fn arc(points: &mut Vec<Point<Pixels>>, center: [f32; 2], radius: f32, start: f32, scale: f32) {
    // Fixed upper bound also protects tessellation for extreme custom radii.
    // Normal Ant radii have sub-pixel chord error at all supported DPI scales.
    let count = ((radius * scale).sqrt() * 4.).ceil().clamp(2., 32.) as usize;
    for index in 1..=count {
        let angle = start + std::f32::consts::FRAC_PI_2 * index as f32 / count as f32;
        points.push(point(
            px(center[0] + radius * angle.cos()),
            px(center[1] + radius * angle.sin()),
        ));
    }
}

struct Contour {
    points: Vec<Point<Pixels>>,
    width: f32,
    closed: bool,
}

impl Contour {
    fn length(&self) -> f32 {
        self.points
            .windows(2)
            .map(|pair| distance(pair[0], pair[1]))
            .sum()
    }

    fn at(&self, mut offset: f32) -> Point<Pixels> {
        for pair in self.points.windows(2) {
            let length = distance(pair[0], pair[1]);
            if offset <= length && length > 0. {
                return pair[0] + (pair[1] - pair[0]) * (offset / length);
            }
            offset -= length;
        }
        *self.points.last().expect("nonempty border contour")
    }

    fn build(&self, line_type: LineType, scale: f32, thin_rails: bool) -> Option<Path<Pixels>> {
        let length = self.length();
        if !length.is_finite() || length <= 0. {
            return None;
        }
        if line_type == LineType::Dotted {
            if thin_rails && self.width <= 3. && !self.closed {
                let from = self.points[0];
                let along = (self.points[1] - from) * (1. / length);
                let across = point(-along.y, along.x) * (self.width / 2.);
                let mut builder = PathBuilder::fill();
                for (start, end) in thin_dot_marks(length, self.width, scale) {
                    builder.add_polygon(
                        &[
                            from + along * start - across,
                            from + along * end - across,
                            from + along * end + across,
                            from + along * start + across,
                        ],
                        true,
                    );
                }
                return builder
                    .build()
                    .ok()
                    .filter(|path| !path.vertices.is_empty());
            }
            if self.closed && self.width <= 3. {
                // Narrow CSS dots are square marks. Keep nominal spacing on
                // the straight runs; rounded corners follow the same contour.
                let gap = self.width.max(length / MAX_MARKS as f32 - self.width);
                let mut builder =
                    PathBuilder::stroke(px(self.width)).dash_array(&[px(self.width), px(gap)]);
                builder.move_to(self.points[0]);
                for point in self.points.iter().skip(1) {
                    builder.line_to(*point);
                }
                builder.close();
                return builder
                    .build()
                    .ok()
                    .filter(|path| !path.vertices.is_empty());
            }
            let sides = ((self.width * scale / 2.).sqrt() * 12.)
                .ceil()
                .clamp(8., 48.) as usize;
            let maximum = (MAX_PATH_VERTICES / (3 * (sides - 2))).min(MAX_MARKS);
            let (count, first, step) = round_dot_spacing(length, self.width, self.closed, maximum);
            let mut builder = PathBuilder::fill();
            for index in 0..count {
                let center = self.at(first + index as f32 * step);
                let radius = self.width / 2.;
                for side in 0..sides {
                    let angle = std::f32::consts::TAU * side as f32 / sides as f32;
                    let vertex = center + point(px(radius * angle.cos()), px(radius * angle.sin()));
                    if side == 0 {
                        builder.move_to(vertex);
                    } else {
                        builder.line_to(vertex);
                    }
                }
                builder.close();
            }
            return builder
                .build()
                .ok()
                .filter(|path| !path.vertices.is_empty());
        }
        let mut builder = PathBuilder::stroke(px(self.width));
        if line_type == LineType::Dashed {
            let (dash, gap) = dash_pattern(length, self.width, scale, self.closed);
            builder = builder.dash_array(&[px(dash), px(gap)]);
        }
        builder.move_to(self.points[0]);
        for point in self.points.iter().skip(1) {
            builder.line_to(*point);
        }
        if self.closed {
            builder.close();
        }
        builder
            .build()
            .ok()
            .filter(|path| !path.vertices.is_empty())
    }
}

fn distance(first: Point<Pixels>, second: Point<Pixels>) -> f32 {
    let delta = second - first;
    f32::from(delta.x).hypot(f32::from(delta.y))
}

/// Round caps extend half a stroke past their center. Open rails inset the
/// endpoint centers so each complete dot stays inside the authored edge.
fn round_dot_spacing(length: f32, width: f32, closed: bool, maximum: usize) -> (usize, f32, f32) {
    let nominal = width * 2.;
    if closed {
        let count = ((length / nominal).round().max(1.) as usize).min(maximum);
        (count, 0., length / count as f32)
    } else {
        let first = (width / 2.).min(length / 2.);
        let available = (length - first * 2.).max(0.);
        let count = ((available / nominal).round() as usize)
            .saturating_add(1)
            .min(maximum);
        let step = if count > 1 {
            available / (count - 1) as f32
        } else {
            0.
        };
        (count, first, step)
    }
}

/// Complete endpoint marks keep narrow dotted rails crisp. Absorb the small
/// integer packing remainder at the two ends; retain nominal spacing inside.
fn thin_dot_marks(length: f32, width: f32, scale: f32) -> Vec<(f32, f32)> {
    if length <= width * 2. {
        return vec![(0., length)];
    }
    let ideal = (length + width) / (width * 2.);
    // On a tie, even widths close with shorter endpoint gaps; odd widths
    // preserve the gap and grow endpoint marks by whole logical pixels.
    let rounded = if ideal.fract() == 0.5 && width.round() as usize % 2 == 1 {
        ideal.floor()
    } else {
        ideal.round()
    };
    let count = (rounded as usize).clamp(2, MAX_MARKS);
    let free = (length - count as f32 * width).max(0.);
    let mut delta = free - (count - 1) as f32 * width;
    let mut first_growth = 0.;
    let mut last_growth = 0.;
    if delta >= width && width.round() as usize % 2 == 1 && count < MAX_MARKS {
        first_growth = 1.;
        last_growth = if width >= 3. { 1. } else { 0. };
        delta -= first_growth + last_growth;
    }
    let first_delta = if first_growth > 0. {
        0.
    } else {
        (delta.abs() * scale / 2.).ceil().copysign(delta) / scale
    };
    let first_gap = (width + first_delta).max(0.);
    let last_gap = (width + delta - (first_gap - width)).max(0.);
    let mut marks = Vec::with_capacity(count);
    let mut cursor = 0.;
    for index in 0..count {
        let mark_width = width
            + if index == 0 { first_growth } else { 0. }
            + if index + 1 == count { last_growth } else { 0. };
        marks.push((cursor, (cursor + mark_width).min(length)));
        cursor += mark_width;
        if count == 2 {
            cursor += free;
        } else if count == MAX_MARKS {
            cursor += free / (count - 1) as f32;
        } else if index == 0 {
            cursor += first_gap;
        } else if index + 2 == count {
            cursor += last_gap;
        } else {
            cursor += width;
        }
    }
    marks
}

fn dash_pattern(length: f32, width: f32, scale: f32, closed: bool) -> (f32, f32) {
    let (dash, gap) = if width * scale < 3. {
        (width * 3., width * 2.)
    } else {
        (width * 2., width)
    };
    if length <= dash * 2. {
        return (length, length.max(width));
    }
    let count = ((length + if closed { 0. } else { gap }) / (dash + gap))
        .round()
        .clamp(1., MAX_MARKS as f32);
    let fitted = if closed {
        length / count - dash
    } else if count > 1. {
        (length - count * dash) / (count - 1.)
    } else {
        length
    };
    (dash.min(length), fitted.max(width * 0.01))
}

#[derive(Default)]
struct BorderCache {
    geometry: Option<Geometry>,
    paths: Vec<Path<Pixels>>,
    #[cfg(test)]
    builds: usize,
}

impl BorderCache {
    fn resolve(&mut self, geometry: Geometry) {
        if self.geometry != Some(geometry) {
            self.paths = geometry.paths();
            self.geometry = Some(geometry);
            #[cfg(test)]
            {
                self.builds += 1;
            }
        }
    }

    fn paint(&mut self, bounds: Bounds<Pixels>, border: BorderPaint, window: &mut Window) {
        if border.color.a <= 0.
            || [
                border.color.h,
                border.color.s,
                border.color.l,
                border.color.a,
            ]
            .iter()
            .any(|value| !value.is_finite())
        {
            return;
        }
        if border.line_type == LineType::Solid {
            window.paint_quad(quad(
                bounds,
                border.radii,
                gpui::transparent_black(),
                border.widths,
                border.color,
                gpui::BorderStyle::Solid,
            ));
            return;
        }
        let Some(geometry) = Geometry::new(bounds.size, border, window.scale_factor()) else {
            return;
        };
        self.resolve(geometry);
        let origin = bounds.origin;
        for path in &self.paths {
            let mut translated = path.clone();
            translated.bounds.origin += origin;
            for vertex in &mut translated.vertices {
                vertex.xy_position += origin;
            }
            window.paint_path(translated, border.color);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{black, size};

    fn border(line_type: LineType) -> BorderPaint {
        BorderPaint {
            widths: Edges::all(px(1.)),
            radii: Corners::all(px(8.)),
            color: black(),
            line_type,
            outsets: None,
        }
    }

    #[test]
    fn patterns_use_browser_ratios_without_requantizing_raw_source_width() {
        for scale in [1., 1.5, 2.] {
            let geometry =
                Geometry::new(size(px(860.), px(420.)), border(LineType::Dashed), scale).unwrap();
            assert_eq!(geometry.widths[0], 1.);
            let (dash, _) = dash_pattern(1000., geometry.widths[0], scale, false);
            assert_eq!(dash, 3.);
            assert_eq!(geometry.contours().len(), 1);
            assert!(geometry.contours()[0].closed);
        }
        assert_eq!(dash_pattern(1000., 3., 1., false).0, 6.);
    }

    #[test]
    fn end_and_bottom_separators_share_one_contour_without_an_extra_join() {
        let mut params = border(LineType::Dotted);
        params.widths.top = px(0.);
        params.widths.left = px(0.);
        let geometry = Geometry::new(size(px(200.), px(60.)), params, 1.).unwrap();
        let contours = geometry.contours();
        assert_eq!(contours.len(), 1);
        assert_eq!(contours[0].points.len(), 3);
        assert!(!contours[0].closed);
        assert_eq!(contours[0].points[1], point(px(199.5), px(59.5)));
    }

    #[test]
    fn centered_strokes_keep_full_source_thickness_and_signed_boundary_outsets() {
        let mut params = border(LineType::Dashed);
        params.widths = Edges {
            right: px(3.),
            bottom: px(3.),
            ..Default::default()
        };
        params.outsets = Some(Edges {
            right: px(0.5),
            bottom: px(-0.5),
            ..Default::default()
        });
        let geometry = Geometry::new(size(px(200.), px(60.)), params, 1.5).unwrap();
        let contours = geometry.contours();
        assert_eq!(
            contours.len(),
            2,
            "each physical grid edge retains its own dash phase"
        );
        assert_eq!(
            contours[0].width, 3.,
            "layout half-borders must not thin the painted stroke"
        );
        assert_eq!(
            contours[0].points,
            [point(px(200.5), px(0.)), point(px(200.5), px(60.))]
        );
        assert_eq!(
            contours[1].points,
            [point(px(0.), px(59.5)), point(px(200.), px(59.5))]
        );
    }

    #[test]
    fn odd_width_rail_uses_the_owner_half_before_the_snapped_cell_boundary() {
        let (owner, neighbor) = super::super::borders::boundary_parts(0, 3., 1.);
        let mut params = border(LineType::Dotted);
        params.widths = Edges {
            right: px(3.),
            ..Default::default()
        };
        params.outsets = Some(Edges {
            right: (owner - neighbor) / 2.,
            ..Default::default()
        });
        let geometry = Geometry::new(size(px(131.), px(54.)), params, 1.).unwrap();
        let rail = &geometry.contours()[0];
        // With cell origin x23 and snapped end x154, the source rail occupies
        // x152..155 and is centered at153.5, rather than154.5.
        assert_eq!(rail.points[0].x + px(23.), px(153.5));
        assert_eq!(rail.points[0].x + px(23.) - px(rail.width / 2.), px(152.));
    }

    #[test]
    fn narrow_dotted_rails_keep_complete_end_marks_and_integral_inner_spacing() {
        for (length, width) in [
            (100., 1.),
            (100., 2.),
            (101., 2.),
            (103., 2.),
            (100., 3.),
            (101., 3.),
            (54., 3.),
            (55., 3.),
            (56., 3.),
            (57., 3.),
            (58., 3.),
            (59., 3.),
        ] {
            let marks = thin_dot_marks(length, width, 1.);
            assert_eq!(marks[0].0, 0.);
            assert!(marks[0].1 >= width && marks[0].1 <= width + 1.);
            let last = *marks.last().unwrap();
            assert_eq!(last.1, length);
            assert!(last.1 - last.0 >= width && last.1 - last.0 <= width + 1.);
            assert!(marks.windows(2).all(|pair| pair[0].1 <= pair[1].0));
            assert!(
                marks
                    .iter()
                    .all(|(start, end)| start.fract() == 0. && end.fract() == 0.)
            );
        }
        assert_eq!(&thin_dot_marks(100., 1., 1.)[..2], [(0., 2.), (3., 4.)]);
        assert_eq!(&thin_dot_marks(100., 2., 1.)[..2], [(0., 2.), (3., 5.)]);
        assert_eq!(&thin_dot_marks(100., 3., 1.)[..2], [(0., 3.), (7., 10.)]);
        assert_eq!(&thin_dot_marks(54., 3., 1.)[..2], [(0., 4.), (7., 10.)]);
        assert_eq!(thin_dot_marks(1_000_000., 1., 1.).len(), MAX_MARKS);
    }

    #[test]
    fn rounded_paths_and_extreme_rails_keep_tessellation_bounded() {
        for line_type in [LineType::Dashed, LineType::Dotted] {
            for scale in [1., 1.5, 2.] {
                for dimensions in [(860., 420.), (1_000_000., 1000.), (50., 50.)] {
                    let geometry = Geometry::new(
                        size(px(dimensions.0), px(dimensions.1)),
                        border(line_type),
                        scale,
                    )
                    .unwrap();
                    let paths = geometry.paths();
                    assert_eq!(paths.len(), 1);
                    assert!(
                        paths
                            .iter()
                            .all(|path| !path.vertices.is_empty() && path.vertices.len() <= 65_535)
                    );
                }
            }
        }
        assert!(
            Geometry::new(size(px(f32::NAN), px(100.)), border(LineType::Dotted), 1.).is_none()
        );
        assert!(Geometry::new(size(px(100.), px(100.)), border(LineType::Dotted), 0.).is_none());
    }

    #[test]
    fn thick_round_dots_keep_complete_marks_inside_open_rail_endpoints() {
        for scale in [1., 1.5, 2.] {
            for width in [4., 5., 8.] {
                let length = 100.;
                let (count, first, step) = round_dot_spacing(length, width, false, MAX_MARKS);
                assert_eq!(first, width / 2.);
                assert!((first + (count - 1) as f32 * step - (length - width / 2.)).abs() < 0.001);
                let contour = Contour {
                    points: vec![point(px(0.), px(20.)), point(px(length), px(20.))],
                    width,
                    closed: false,
                };
                let path = contour.build(LineType::Dotted, scale, true).unwrap();
                for vertex in &path.vertices {
                    let x = f32::from(vertex.xy_position.x);
                    assert!((-0.001..=length + 0.001).contains(&x));
                }
            }
        }
        assert_eq!(round_dot_spacing(100., 4., false, MAX_MARKS), (13, 2., 8.));
        assert_eq!(round_dot_spacing(2., 4., false, MAX_MARKS), (1, 1., 0.));
        assert_eq!(
            round_dot_spacing(1_000_000., 4., false, MAX_MARKS).0,
            MAX_MARKS
        );
    }

    #[test]
    fn cache_reuses_geometry_across_color_or_position_changes_and_replaces_one_entry() {
        let mut cache = BorderCache::default();
        let geometry =
            Geometry::new(size(px(860.), px(420.)), border(LineType::Dotted), 1.).unwrap();
        cache.resolve(geometry);
        cache.resolve(geometry);
        assert_eq!(cache.builds, 1);
        let changed = Geometry {
            scale: 2.,
            ..geometry
        };
        cache.resolve(changed);
        assert_eq!(cache.builds, 2);
        assert_eq!(cache.geometry, Some(changed));
        assert_eq!(cache.paths.len(), 1);
    }
}
