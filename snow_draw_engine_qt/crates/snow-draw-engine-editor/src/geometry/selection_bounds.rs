use super::*;
use snow_draw_engine_core::arrow::ArrowPathCommand;

pub(crate) fn selection_bounds_from_elements(
    elements: &[SelectionRectState],
) -> Option<SelectionBounds> {
    if elements.is_empty() {
        return None;
    }
    if elements.len() == 1 {
        let rect = elements[0].rect;
        return Some(SelectionBounds {
            center: rect.center,
            width: rect.width,
            height: rect.height,
            rotation: rect.rotation,
        });
    }

    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    for element in elements {
        for corner in ALL_CORNERS {
            let point = rect_local_to_canvas(
                element.rect.center,
                element.rect.rotation,
                rect_corner_point(element.rect.width, element.rect.height, corner),
            );
            min_x = min_x.min(point.x);
            min_y = min_y.min(point.y);
            max_x = max_x.max(point.x);
            max_y = max_y.max(point.y);
        }
    }

    Some(SelectionBounds {
        center: Point {
            x: f64::midpoint(min_x, max_x),
            y: f64::midpoint(min_y, max_y),
        },
        width: (max_x - min_x).max(MIN_RECT_SIZE),
        height: (max_y - min_y).max(MIN_RECT_SIZE),
        rotation: 0.0,
    })
}

pub(crate) fn selection_bounds_from_selection(
    elements: &[SelectionRectState],
    arrows: &[SelectionArrowState],
) -> Option<SelectionBounds> {
    if arrows.is_empty() {
        return selection_bounds_from_elements(elements);
    }
    if elements.is_empty() && arrows.len() == 1 {
        return Some(selection_bounds_from_arrow(&arrows[0].arrow));
    }

    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    let mut found_any = false;

    for element in elements {
        found_any = true;
        for corner in ALL_CORNERS {
            let point = rect_local_to_canvas(
                element.rect.center,
                element.rect.rotation,
                rect_corner_point(element.rect.width, element.rect.height, corner),
            );
            min_x = min_x.min(point.x);
            min_y = min_y.min(point.y);
            max_x = max_x.max(point.x);
            max_y = max_y.max(point.y);
        }
    }

    for arrow in arrows {
        found_any = true;
        let bounds = selection_bounds_to_draw_rect(&selection_bounds_from_arrow(&arrow.arrow));
        min_x = min_x.min(bounds.min_x);
        min_y = min_y.min(bounds.min_y);
        max_x = max_x.max(bounds.max_x);
        max_y = max_y.max(bounds.max_y);
    }

    if !found_any {
        return None;
    }

    Some(SelectionBounds {
        center: Point::new(f64::midpoint(min_x, max_x), f64::midpoint(min_y, max_y)),
        width: (max_x - min_x).max(MIN_RECT_SIZE),
        height: (max_y - min_y).max(MIN_RECT_SIZE),
        rotation: 0.0,
    })
}

fn selection_bounds_from_arrow(arrow: &ArrowData) -> SelectionBounds {
    // The reference engine stores arrow points in element-local coordinates and
    // builds a single-selection frame from that rect plus the element rotation.
    // This engine stores transformed points in canvas coordinates, so project
    // the rendered shaft back onto the persisted rotation axes before taking
    // the rect. Arrowheads and stroke expansion are deliberately excluded:
    // they are paint details, not element selection geometry.
    let rotation = arrow.rotation;
    let (sin, cos) = rotation.sin_cos();
    let mut path_bounds = ProjectedPathBounds::new();
    let mut current = None;

    for command in arrow.path_commands() {
        match command {
            ArrowPathCommand::MoveTo { point } => {
                current = Some(project_arrow_point(point, sin, cos));
                if let Some(point) = current {
                    path_bounds.include(point);
                }
            }
            ArrowPathCommand::LineTo { point } => {
                let end = project_arrow_point(point, sin, cos);
                if let Some(start) = current {
                    path_bounds.include_line(start, end);
                } else {
                    path_bounds.include(end);
                }
                current = Some(end);
            }
            ArrowPathCommand::QuadTo { control, end } => {
                let control = project_arrow_point(control, sin, cos);
                let end = project_arrow_point(end, sin, cos);
                if let Some(start) = current {
                    path_bounds.include_quadratic(start, control, end);
                } else {
                    path_bounds.include(control);
                    path_bounds.include(end);
                }
                current = Some(end);
            }
            ArrowPathCommand::CubicTo {
                control_1,
                control_2,
                end,
            } => {
                let control_1 = project_arrow_point(control_1, sin, cos);
                let control_2 = project_arrow_point(control_2, sin, cos);
                let end = project_arrow_point(end, sin, cos);
                if let Some(start) = current {
                    path_bounds.include_cubic(start, control_1, control_2, end);
                } else {
                    path_bounds.include(control_1);
                    path_bounds.include(control_2);
                    path_bounds.include(end);
                }
                current = Some(end);
            }
        }
    }

    path_bounds
        .into_selection_bounds(rotation, sin, cos)
        .unwrap_or(SelectionBounds {
            center: Point::new(arrow.x + arrow.width / 2.0, arrow.y + arrow.height / 2.0),
            width: arrow.width.max(MIN_RECT_SIZE),
            height: arrow.height.max(MIN_RECT_SIZE),
            rotation,
        })
}

fn project_arrow_point(point: [f64; 2], sin: f64, cos: f64) -> Point<f64> {
    Point::new(
        point[0] * cos + point[1] * sin,
        -point[0] * sin + point[1] * cos,
    )
}

struct ProjectedPathBounds {
    min_x: f64,
    min_y: f64,
    max_x: f64,
    max_y: f64,
}

impl ProjectedPathBounds {
    fn new() -> Self {
        Self {
            min_x: f64::INFINITY,
            min_y: f64::INFINITY,
            max_x: f64::NEG_INFINITY,
            max_y: f64::NEG_INFINITY,
        }
    }

    fn include(&mut self, point: Point<f64>) {
        if !point.x.is_finite() || !point.y.is_finite() {
            return;
        }
        self.min_x = self.min_x.min(point.x);
        self.min_y = self.min_y.min(point.y);
        self.max_x = self.max_x.max(point.x);
        self.max_y = self.max_y.max(point.y);
    }

    fn include_line(&mut self, start: Point<f64>, end: Point<f64>) {
        self.include(start);
        self.include(end);
    }

    fn include_quadratic(&mut self, start: Point<f64>, control: Point<f64>, end: Point<f64>) {
        self.include(start);
        self.include(end);
        for t in [
            quadratic_extremum(start.x, control.x, end.x),
            quadratic_extremum(start.y, control.y, end.y),
        ]
        .into_iter()
        .flatten()
        {
            self.include(quadratic_point(start, control, end, t));
        }
    }

    fn include_cubic(
        &mut self,
        start: Point<f64>,
        control_1: Point<f64>,
        control_2: Point<f64>,
        end: Point<f64>,
    ) {
        self.include(start);
        self.include(end);
        for t in cubic_derivative_roots(start.x, control_1.x, control_2.x, end.x)
            .into_iter()
            .chain(cubic_derivative_roots(
                start.y,
                control_1.y,
                control_2.y,
                end.y,
            ))
        {
            self.include(cubic_point(start, control_1, control_2, end, t));
        }
    }

    fn into_selection_bounds(self, rotation: f64, sin: f64, cos: f64) -> Option<SelectionBounds> {
        if !self.min_x.is_finite()
            || !self.min_y.is_finite()
            || !self.max_x.is_finite()
            || !self.max_y.is_finite()
        {
            return None;
        }

        let center = Point::new(
            f64::midpoint(self.min_x, self.max_x),
            f64::midpoint(self.min_y, self.max_y),
        );
        Some(SelectionBounds {
            center: Point::new(
                center.x * cos - center.y * sin,
                center.x * sin + center.y * cos,
            ),
            width: (self.max_x - self.min_x).max(MIN_RECT_SIZE),
            height: (self.max_y - self.min_y).max(MIN_RECT_SIZE),
            rotation,
        })
    }
}

fn quadratic_extremum(start: f64, control: f64, end: f64) -> Option<f64> {
    let denominator = start - 2.0 * control + end;
    let t = (denominator.abs() > f64::EPSILON).then(|| (start - control) / denominator)?;
    (0.0 < t && t < 1.0).then_some(t)
}

fn quadratic_point(start: Point<f64>, control: Point<f64>, end: Point<f64>, t: f64) -> Point<f64> {
    let one_minus_t = 1.0 - t;
    Point::new(
        one_minus_t * one_minus_t * start.x + 2.0 * one_minus_t * t * control.x + t * t * end.x,
        one_minus_t * one_minus_t * start.y + 2.0 * one_minus_t * t * control.y + t * t * end.y,
    )
}

fn cubic_derivative_roots(start: f64, control_1: f64, control_2: f64, end: f64) -> Vec<f64> {
    const EPSILON: f64 = 1e-9;
    let a = -start + 3.0 * control_1 - 3.0 * control_2 + end;
    let b = 3.0 * start - 6.0 * control_1 + 3.0 * control_2;
    let c = -3.0 * start + 3.0 * control_1;

    if a.abs() < EPSILON {
        if b.abs() < EPSILON {
            return Vec::new();
        }
        let t = -c / (2.0 * b);
        return (0.0 < t && t < 1.0).then_some(t).into_iter().collect();
    }

    let quadratic_a = 3.0 * a;
    let quadratic_b = 2.0 * b;
    let discriminant = quadratic_b * quadratic_b - 4.0 * quadratic_a * c;
    if discriminant < 0.0 {
        return Vec::new();
    }

    let denominator = 2.0 * quadratic_a;
    if denominator.abs() < EPSILON {
        return Vec::new();
    }
    let discriminant = discriminant.sqrt();
    [
        (-quadratic_b + discriminant) / denominator,
        (-quadratic_b - discriminant) / denominator,
    ]
    .into_iter()
    .filter(|t| 0.0 < *t && *t < 1.0)
    .collect()
}

fn cubic_point(
    start: Point<f64>,
    control_1: Point<f64>,
    control_2: Point<f64>,
    end: Point<f64>,
    t: f64,
) -> Point<f64> {
    let one_minus_t = 1.0 - t;
    Point::new(
        one_minus_t.powi(3) * start.x
            + 3.0 * one_minus_t.powi(2) * t * control_1.x
            + 3.0 * one_minus_t * t.powi(2) * control_2.x
            + t.powi(3) * end.x,
        one_minus_t.powi(3) * start.y
            + 3.0 * one_minus_t.powi(2) * t * control_1.y
            + 3.0 * one_minus_t * t.powi(2) * control_2.y
            + t.powi(3) * end.y,
    )
}

pub(crate) fn axis_aligned_bounds(start: Point<f64>, end: Point<f64>) -> Option<AxisAlignedBounds> {
    let left = start.x.min(end.x);
    let top = start.y.min(end.y);
    let right = start.x.max(end.x);
    let bottom = start.y.max(end.y);
    if right <= left || bottom <= top {
        return None;
    }
    Some(AxisAlignedBounds {
        left,
        top,
        right,
        bottom,
    })
}

pub(crate) fn axis_aligned_bounds_corners(bounds: AxisAlignedBounds) -> [Point<f64>; 4] {
    [
        Point {
            x: bounds.left,
            y: bounds.top,
        },
        Point {
            x: bounds.right,
            y: bounds.top,
        },
        Point {
            x: bounds.right,
            y: bounds.bottom,
        },
        Point {
            x: bounds.left,
            y: bounds.bottom,
        },
    ]
}

pub(crate) fn rectangle_corners(rect: &RectangleData) -> [Point<f64>; 4] {
    [
        rect_local_to_canvas(
            rect.center,
            rect.rotation,
            rect_corner_point(rect.width, rect.height, RectCorner::TopLeft),
        ),
        rect_local_to_canvas(
            rect.center,
            rect.rotation,
            rect_corner_point(rect.width, rect.height, RectCorner::TopRight),
        ),
        rect_local_to_canvas(
            rect.center,
            rect.rotation,
            rect_corner_point(rect.width, rect.height, RectCorner::BottomRight),
        ),
        rect_local_to_canvas(
            rect.center,
            rect.rotation,
            rect_corner_point(rect.width, rect.height, RectCorner::BottomLeft),
        ),
    ]
}

pub(crate) fn project_points_onto_axis(points: &[Point<f64>], axis: Point<f64>) -> (f64, f64) {
    let first = points[0].x * axis.x + points[0].y * axis.y;
    points
        .iter()
        .skip(1)
        .fold((first, first), |(min, max), point| {
            let value = point.x * axis.x + point.y * axis.y;
            (min.min(value), max.max(value))
        })
}

pub(crate) fn projected_ranges_overlap(left: (f64, f64), right: (f64, f64)) -> bool {
    left.1 >= right.0 && right.1 >= left.0
}

pub(crate) fn rectangle_intersects_axis_aligned_bounds(
    rect: &RectangleData,
    bounds: AxisAlignedBounds,
) -> bool {
    if rect.width <= 0.0 || rect.height <= 0.0 {
        return false;
    }

    let rect_points = rectangle_corners(rect);
    let bounds_points = axis_aligned_bounds_corners(bounds);
    let axes = [
        Point { x: 1.0, y: 0.0 },
        Point { x: 0.0, y: 1.0 },
        rotate_vector(Point { x: 1.0, y: 0.0 }, rect.rotation),
        rotate_vector(Point { x: 0.0, y: 1.0 }, rect.rotation),
    ];

    axes.iter().all(|axis| {
        projected_ranges_overlap(
            project_points_onto_axis(&rect_points, *axis),
            project_points_onto_axis(&bounds_points, *axis),
        )
    })
}

pub(crate) fn draw_rect_intersects_axis_aligned_bounds(
    rect: DrawRect,
    bounds: AxisAlignedBounds,
) -> bool {
    rect.max_x >= bounds.left
        && rect.min_x <= bounds.right
        && rect.max_y >= bounds.top
        && rect.min_y <= bounds.bottom
}

pub(crate) fn selection_hit_target(
    bounds: &SelectionBounds,
    single_rect: Option<&RectangleData>,
    single_text_rect: Option<&RectangleData>,
    frame_padding: f64,
    corner_handle_outset: f64,
    zoom: f64,
    canvas_point: Point<f64>,
) -> Option<SelectionHitTarget> {
    let hit_size = selection_handle_hit_size(zoom);
    let hit_radius = hit_size / 2.0;
    let edge_inner_tolerance = if frame_padding > 0.0 || single_rect.is_some() {
        hit_radius
    } else {
        SELECTION_TIGHT_EDGE_INNER_HIT_PX / zoom.max(0.0001)
    };

    let rotation_center = selection_rotation_handle_center(bounds, frame_padding, zoom);
    if point_in_circle(rotation_center, hit_radius, canvas_point) {
        return Some(SelectionHitTarget::Rotate);
    }

    if let Some(text_rect) = single_text_rect
        && text_selection_move_ring_contains_point(bounds, text_rect, frame_padding, canvas_point)
    {
        return Some(SelectionHitTarget::Move);
    }

    for corner in ALL_CORNERS {
        let center =
            selection_resize_handle_center(bounds, frame_padding, corner_handle_outset, corner);
        if point_in_rotated_rect(center, hit_size, hit_size, bounds.rotation, canvas_point) {
            return Some(SelectionHitTarget::Resize(ResizeHandle::from_corner(
                corner,
            )));
        }
    }

    if let Some(handle) = selection_resize_edge_hit_target(
        bounds,
        frame_padding,
        hit_radius,
        edge_inner_tolerance,
        canvas_point,
    ) {
        return Some(SelectionHitTarget::Resize(handle));
    }

    if let Some(rect) = single_rect.filter(|rect| rect.supports_corner_radius()) {
        for corner in ALL_CORNERS {
            let center = rect_local_to_canvas(
                rect.center,
                rect.rotation,
                corner_radius_handle_local_point(rect, zoom, corner),
            );
            if point_in_circle(center, hit_radius, canvas_point) {
                return Some(SelectionHitTarget::CornerRadius(corner));
            }
        }
    }

    if rectangle_contains_point(
        bounds.center,
        bounds.width,
        bounds.height,
        bounds.rotation,
        canvas_point,
    ) {
        return Some(SelectionHitTarget::Move);
    }

    None
}

fn text_selection_move_ring_contains_point(
    bounds: &SelectionBounds,
    text_rect: &RectangleData,
    frame_padding: f64,
    canvas_point: Point<f64>,
) -> bool {
    if frame_padding <= 0.0 {
        return false;
    }
    point_in_rotated_rect_exclusive(
        bounds.center,
        bounds.width + frame_padding * 2.0,
        bounds.height + frame_padding * 2.0,
        bounds.rotation,
        canvas_point,
    ) && !rectangle_contains_point(
        text_rect.center,
        text_rect.width,
        text_rect.height,
        text_rect.rotation,
        canvas_point,
    )
}

fn point_in_rotated_rect_exclusive(
    center: Point<f64>,
    width: f64,
    height: f64,
    rotation: f64,
    point: Point<f64>,
) -> bool {
    if width <= 0.0 || height <= 0.0 {
        return false;
    }
    let local = canvas_to_rect_local(center, rotation, point);
    local.x.abs() < width / 2.0 && local.y.abs() < height / 2.0
}

pub(crate) fn selection_resize_edge_hit_target(
    bounds: &SelectionBounds,
    frame_padding: f64,
    outer_tolerance: f64,
    inner_tolerance: f64,
    canvas_point: Point<f64>,
) -> Option<ResizeHandle> {
    let local_point = canvas_to_rect_local(bounds.center, bounds.rotation, canvas_point);
    let padded_width = bounds.width + frame_padding * 2.0;
    let padded_height = bounds.height + frame_padding * 2.0;
    let min_x = -padded_width / 2.0;
    let max_x = padded_width / 2.0;
    let min_y = -padded_height / 2.0;
    let max_y = padded_height / 2.0;
    let span_tolerance = outer_tolerance.max(inner_tolerance);

    if test_horizontal_edge(EdgeHitTest {
        local_point,
        span_min: min_x,
        span_max: max_x,
        edge: min_y,
        inward_sign: 1.0,
        outer_tolerance,
        inner_tolerance,
        span_tolerance,
    }) {
        return Some(ResizeHandle::Top);
    }
    if test_vertical_edge(EdgeHitTest {
        local_point,
        span_min: min_y,
        span_max: max_y,
        edge: max_x,
        inward_sign: -1.0,
        outer_tolerance,
        inner_tolerance,
        span_tolerance,
    }) {
        return Some(ResizeHandle::Right);
    }
    if test_horizontal_edge(EdgeHitTest {
        local_point,
        span_min: min_x,
        span_max: max_x,
        edge: max_y,
        inward_sign: -1.0,
        outer_tolerance,
        inner_tolerance,
        span_tolerance,
    }) {
        return Some(ResizeHandle::Bottom);
    }
    if test_vertical_edge(EdgeHitTest {
        local_point,
        span_min: min_y,
        span_max: max_y,
        edge: min_x,
        inward_sign: 1.0,
        outer_tolerance,
        inner_tolerance,
        span_tolerance,
    }) {
        return Some(ResizeHandle::Left);
    }
    None
}

pub(crate) struct EdgeHitTest {
    pub(crate) local_point: Point<f64>,
    pub(crate) span_min: f64,
    pub(crate) span_max: f64,
    pub(crate) edge: f64,
    pub(crate) inward_sign: f64,
    pub(crate) outer_tolerance: f64,
    pub(crate) inner_tolerance: f64,
    pub(crate) span_tolerance: f64,
}

pub(crate) fn test_horizontal_edge(request: EdgeHitTest) -> bool {
    is_near_edge(
        request.local_point.y,
        request.edge,
        request.inward_sign,
        request.outer_tolerance,
        request.inner_tolerance,
    ) && is_inside_edge_span(
        request.local_point.x,
        request.span_min,
        request.span_max,
        request.span_tolerance,
    )
}

pub(crate) fn test_vertical_edge(request: EdgeHitTest) -> bool {
    is_near_edge(
        request.local_point.x,
        request.edge,
        request.inward_sign,
        request.outer_tolerance,
        request.inner_tolerance,
    ) && is_inside_edge_span(
        request.local_point.y,
        request.span_min,
        request.span_max,
        request.span_tolerance,
    )
}

pub(crate) fn is_inside_edge_span(value: f64, min: f64, max: f64, tolerance: f64) -> bool {
    value > min + tolerance && value < max - tolerance
}

pub(crate) fn is_near_edge(
    value: f64,
    edge: f64,
    inward_sign: f64,
    outer_tolerance: f64,
    inner_tolerance: f64,
) -> bool {
    let distance = (value - edge) * inward_sign;
    distance >= -outer_tolerance && distance <= inner_tolerance
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8,
        arrow::{ArrowStrokeStyle, ArrowType, Arrowhead},
    };

    fn assert_close(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() <= 1e-9,
            "expected {actual} to equal {expected}"
        );
    }

    fn selection_bounds() -> SelectionBounds {
        SelectionBounds {
            center: Point::new(0.0, 0.0),
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
        }
    }

    fn text_rect(bounds: SelectionBounds) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: bounds.center,
            width: bounds.width,
            height: bounds.height,
            rotation: bounds.rotation,
            fill: ColorRgba8::default(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    #[test]
    fn text_selection_ring_hits_move() {
        let bounds = selection_bounds();
        let text_rect = text_rect(bounds);

        let target = selection_hit_target(
            &bounds,
            None,
            Some(&text_rect),
            10.0,
            0.0,
            1.0,
            Point::new(0.0, -25.0),
        );

        assert_eq!(target, Some(SelectionHitTarget::Move));
    }

    #[test]
    fn text_selection_ring_prioritizes_move_over_edge_resize_tolerance() {
        let bounds = selection_bounds();
        let text_rect = text_rect(bounds);

        let target = selection_hit_target(
            &bounds,
            None,
            Some(&text_rect),
            10.0,
            0.0,
            1.0,
            Point::new(0.0, -29.0),
        );

        assert_eq!(target, Some(SelectionHitTarget::Move));
    }

    #[test]
    fn text_selection_ring_prioritizes_move_over_corner_resize_handle_overlap() {
        let bounds = selection_bounds();
        let text_rect = text_rect(bounds);

        let target = selection_hit_target(
            &bounds,
            None,
            Some(&text_rect),
            10.0,
            0.0,
            1.0,
            Point::new(-58.0, -28.0),
        );

        assert_eq!(target, Some(SelectionHitTarget::Move));
    }

    #[test]
    fn text_selection_ring_does_not_add_outer_move_tolerance() {
        let bounds = selection_bounds();
        let text_rect = text_rect(bounds);

        let target = selection_hit_target(
            &bounds,
            None,
            Some(&text_rect),
            10.0,
            0.0,
            1.0,
            Point::new(0.0, -30.1),
        );

        assert_ne!(target, Some(SelectionHitTarget::Move));
    }

    #[test]
    fn rectangle_highlight_corner_radius_controls_are_not_hittable() {
        let bounds = selection_bounds();
        let highlight =
            text_rect(bounds).into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
        let handle = rect_local_to_canvas(
            highlight.center,
            highlight.rotation,
            corner_radius_handle_local_point(&highlight, 1.0, RectCorner::TopLeft),
        );

        let target = selection_hit_target(&bounds, Some(&highlight), None, 4.0, 0.0, 1.0, handle);

        assert_ne!(
            target,
            Some(SelectionHitTarget::CornerRadius(RectCorner::TopLeft))
        );
    }

    #[test]
    fn spotlight_corner_radius_controls_are_not_hittable() {
        let bounds = selection_bounds();
        let spotlight = text_rect(bounds).into_spotlight();
        let handle = rect_local_to_canvas(
            spotlight.center,
            spotlight.rotation,
            corner_radius_handle_local_point(&spotlight, 1.0, RectCorner::TopLeft),
        );

        let target = selection_hit_target(&bounds, Some(&spotlight), None, 4.0, 0.0, 1.0, handle);

        assert_ne!(
            target,
            Some(SelectionHitTarget::CornerRadius(RectCorner::TopLeft))
        );
    }

    #[test]
    fn ellipse_and_diamond_corner_radius_controls_are_not_hittable() {
        let bounds = selection_bounds();
        for shape in [
            snow_draw_engine_document::HighlightShape::Ellipse,
            snow_draw_engine_document::HighlightShape::Diamond,
        ] {
            let mut selected_rect = text_rect(bounds);
            selected_rect.highlight_shape = shape;
            let handle = rect_local_to_canvas(
                selected_rect.center,
                selected_rect.rotation,
                corner_radius_handle_local_point(&selected_rect, 1.0, RectCorner::TopLeft),
            );

            let target =
                selection_hit_target(&bounds, Some(&selected_rect), None, 4.0, 0.0, 1.0, handle);

            assert_ne!(
                target,
                Some(SelectionHitTarget::CornerRadius(RectCorner::TopLeft))
            );
        }
    }

    #[test]
    fn arrow_selection_uses_path_geometry_without_arrowhead_or_stroke_expansion() {
        let arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(100.0, 0.0)],
            ColorRgba8::default(),
            24.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            Some(Arrowhead::Circle),
            Some(Arrowhead::Arrow),
        )
        .expect("two points should create an arrow");

        let bounds = selection_bounds_from_selection(
            &[],
            &[SelectionArrowState {
                id: ElementId::default(),
                arrow,
            }],
        )
        .expect("arrow should have selection bounds");

        assert_close(bounds.center.x, 50.0);
        assert_close(bounds.center.y, 0.0);
        assert_close(bounds.width, 100.0);
        assert_close(bounds.height, MIN_RECT_SIZE);
        assert_close(bounds.rotation, 0.0);
    }

    #[test]
    fn projected_cubic_path_bounds_include_internal_extrema() {
        let mut bounds = ProjectedPathBounds::new();
        bounds.include_cubic(
            Point::new(0.0, 0.0),
            Point::new(0.0, 100.0),
            Point::new(100.0, 100.0),
            Point::new(100.0, 0.0),
        );

        let bounds = bounds
            .into_selection_bounds(0.0, 0.0, 1.0)
            .expect("cubic path should have bounds");

        assert_close(bounds.center.x, 50.0);
        assert_close(bounds.center.y, 37.5);
        assert_close(bounds.width, 100.0);
        assert_close(bounds.height, 75.0);
    }
}
