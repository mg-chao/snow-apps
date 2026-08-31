use crate::dirty_regions::clip_dirty_region;
use snow_draw_engine_core::{
    ColorRgba8, DrawRect, Point, SnapGuideAxis, SnapGuideKind,
    arrow::{StrokeStyle, ArrowType, Arrowhead},
    rotated_rect_extents,
};
use snow_draw_engine_display::{
    DirtyRegion, DisplayRectangleShape, FrameView, OverlayDisplayItem, SceneDisplayItem,
    SerialNumberConnectorDisplayItem, SnapGuideDisplayItem, TextDisplayItem,
};
use snow_draw_engine_document::{
    ArrowData, FillStyle, LinearElementKind, RectangleData, arrow_bounds, arrow_is_degenerate,
};

pub(crate) fn rect_bounds(rect: RectangleData) -> DrawRect {
    let (extent_x, extent_y) =
        rotated_rect_extents(rect.width, rect.height, rect.rotation, rect.stroke_width);
    DrawRect::new(
        rect.center.x - extent_x,
        rect.center.y - extent_y,
        rect.center.x + extent_x,
        rect.center.y + extent_y,
    )
}

pub(crate) fn bounds_visible(bounds: DrawRect, viewport: (f64, f64, f64, f64)) -> bool {
    bounds.max_x >= viewport.0
        && bounds.min_x <= viewport.2
        && bounds.max_y >= viewport.1
        && bounds.min_y <= viewport.3
}

pub(crate) fn overlay_item_visible(item: &OverlayDisplayItem, frame_view: FrameView) -> bool {
    overlay_display_item_bounds(item, frame_view)
        .and_then(|bounds| clip_dirty_region(bounds, frame_view.surface))
        .is_some()
}

pub(crate) fn scene_display_item_bounds(
    item: &SceneDisplayItem,
    frame_view: FrameView,
) -> Option<DirtyRegion> {
    match item {
        SceneDisplayItem::Rectangle(item) => match item.shape {
            DisplayRectangleShape::Diamond => draw_diamond_bounds(
                frame_view,
                item.center_x,
                item.center_y,
                item.width,
                item.height,
                item.rotation,
                item.stroke_width,
            ),
            DisplayRectangleShape::Rectangle | DisplayRectangleShape::Ellipse => draw_rect_bounds(
                frame_view,
                item.center_x,
                item.center_y,
                item.width,
                item.height,
                item.rotation,
                item.stroke_width,
            ),
        },
        SceneDisplayItem::Filter(item) => draw_rect_bounds(
            frame_view,
            item.center_x,
            item.center_y,
            item.width,
            item.height,
            item.rotation,
            item.stroke_width,
        ),
        SceneDisplayItem::Arrow(item) => draw_path_item_bounds(frame_view, item),
        SceneDisplayItem::Text(item) => draw_text_bounds(frame_view, item),
        SceneDisplayItem::SerialNumber(item) => draw_rect_bounds(
            frame_view,
            item.center_x,
            item.center_y,
            item.diameter,
            item.diameter,
            item.rotation,
            item.stroke_width,
        ),
        SceneDisplayItem::SerialNumberConnector(item) => {
            draw_serial_number_connector_bounds(frame_view, *item)
        }
        SceneDisplayItem::Stroke | SceneDisplayItem::Image => None,
    }
}

fn draw_path_item_bounds(
    frame_view: FrameView,
    item: &snow_draw_engine_display::ArrowDisplayItem,
) -> Option<DirtyRegion> {
    let bounds = item.geometry.canvas_bounds;
    if bounds.iter().any(|value| !value.is_finite()) || item.stroke_width < 0.0 {
        return None;
    }
    let outset = item.stroke_width * 0.5;
    let mut min_x = bounds[0] - outset;
    let mut min_y = bounds[1] - outset;
    let mut max_x = bounds[2] + outset;
    let mut max_y = bounds[3] + outset;
    for primitive in &item.arrowhead_primitives {
        for point in &primitive.points {
            min_x = min_x.min(point[0] - outset);
            min_y = min_y.min(point[1] - outset);
            max_x = max_x.max(point[0] + outset);
            max_y = max_y.max(point[1] + outset);
        }
        if primitive.diameter > 0.0 {
            let radius = primitive.diameter * 0.5 + outset;
            min_x = min_x.min(primitive.center[0] - radius);
            min_y = min_y.min(primitive.center[1] - radius);
            max_x = max_x.max(primitive.center[0] + radius);
            max_y = max_y.max(primitive.center[1] + radius);
        }
    }
    let top_left = canvas_point_to_surface(frame_view, Point::new(min_x, min_y));
    let bottom_right = canvas_point_to_surface(frame_view, Point::new(max_x, max_y));
    Some(DirtyRegion::new(
        top_left.x.min(bottom_right.x),
        top_left.y.min(bottom_right.y),
        top_left.x.max(bottom_right.x),
        top_left.y.max(bottom_right.y),
    ))
}

fn draw_text_bounds(frame_view: FrameView, item: &TextDisplayItem) -> Option<DirtyRegion> {
    const DIRTY_FILL_LINE_HEIGHT_PER_FONT_SIZE: f64 = 1.5;
    const HORIZONTAL_FILL_PADDING_PER_FONT_SIZE: f64 = DIRTY_FILL_LINE_HEIGHT_PER_FONT_SIZE * 0.32;
    const VERTICAL_FILL_PADDING_PER_FONT_SIZE: f64 = DIRTY_FILL_LINE_HEIGHT_PER_FONT_SIZE * 0.1;

    let can_paint_text = !item.text.is_empty() && item.font_size > 0.0;
    let stroke_outset = if can_paint_text && item.stroke.a != 0 {
        item.stroke_width.max(0.0) / 2.0
    } else {
        0.0
    };
    let (fill_outset_x, fill_outset_y) = if item.fill.a != 0 && item.font_size > 0.0 {
        (
            item.font_size * HORIZONTAL_FILL_PADDING_PER_FONT_SIZE,
            item.font_size * VERTICAL_FILL_PADDING_PER_FONT_SIZE,
        )
    } else {
        (0.0, 0.0)
    };
    let outset_x = stroke_outset.max(fill_outset_x);
    let outset_y = stroke_outset.max(fill_outset_y);
    draw_rect_bounds(
        frame_view,
        item.center_x,
        item.center_y,
        item.width + outset_x * 2.0,
        item.height + outset_y * 2.0,
        item.rotation,
        0.0,
    )
}

pub(crate) fn overlay_display_item_bounds(
    item: &OverlayDisplayItem,
    frame_view: FrameView,
) -> Option<DirtyRegion> {
    match item {
        OverlayDisplayItem::Rectangle(item) => draw_rect_bounds(
            frame_view,
            item.center_x,
            item.center_y,
            item.width,
            item.height,
            item.rotation,
            item.stroke_width,
        ),
        OverlayDisplayItem::FocusConnection(item) => draw_arrow_bounds(
            frame_view,
            display_arrow_to_document_arrow(
                &item.points,
                item.arrow_type,
                item.start_arrowhead,
                item.end_arrowhead,
                item.stroke,
                item.stroke_width,
                item.stroke_style,
            ),
        ),
        OverlayDisplayItem::PenFilterContour(item) => draw_arrow_bounds(
            frame_view,
            display_arrow_to_document_arrow(
                &item.points,
                item.arrow_type,
                item.start_arrowhead,
                item.end_arrowhead,
                item.stroke,
                item.stroke_width + 1.0 / frame_view.camera.zoom.max(0.0001),
                item.stroke_style,
            ),
        ),
        OverlayDisplayItem::SnapGuide(item) => snap_guide_bounds(*item, frame_view),
    }
}

pub(crate) fn draw_rect_bounds(
    frame_view: FrameView,
    center_x: f64,
    center_y: f64,
    width: f64,
    height: f64,
    rotation: f64,
    stroke_width: f64,
) -> Option<DirtyRegion> {
    if !center_x.is_finite()
        || !center_y.is_finite()
        || !width.is_finite()
        || !height.is_finite()
        || !rotation.is_finite()
        || !stroke_width.is_finite()
    {
        return None;
    }

    let center = canvas_point_to_surface(frame_view, Point::new(center_x, center_y));
    let zoom = frame_view.camera.zoom.max(0.0);
    let (extent_x, extent_y) = rotated_rect_extents(
        width.max(0.0) * zoom,
        height.max(0.0) * zoom,
        rotation,
        stroke_width.max(0.0) * zoom,
    );
    Some(DirtyRegion::new(
        center.x - extent_x,
        center.y - extent_y,
        center.x + extent_x,
        center.y + extent_y,
    ))
}

fn draw_diamond_bounds(
    frame_view: FrameView,
    center_x: f64,
    center_y: f64,
    width: f64,
    height: f64,
    rotation: f64,
    stroke_width: f64,
) -> Option<DirtyRegion> {
    if !center_x.is_finite()
        || !center_y.is_finite()
        || !width.is_finite()
        || !height.is_finite()
        || !rotation.is_finite()
        || !stroke_width.is_finite()
    {
        return None;
    }

    let center = canvas_point_to_surface(frame_view, Point::new(center_x, center_y));
    let zoom = frame_view.camera.zoom.max(0.0);
    let half_width = width.max(0.0) * zoom / 2.0;
    let half_height = height.max(0.0) * zoom / 2.0;
    let half_stroke = stroke_width.max(0.0) * zoom / 2.0;
    let diagonal = half_width.hypot(half_height);

    // The renderer uses Qt::MiterJoin. At a diamond tip the miter extends
    // farther than half the stroke width, unlike a rectangular or elliptic
    // outline. Match QPen's default miter limit so very thin diamonds whose
    // joins are bevelled do not produce disproportionately large dirty areas.
    const QT_DEFAULT_MITER_LIMIT: f64 = 2.0;
    let max_miter = stroke_width.max(0.0) * zoom * QT_DEFAULT_MITER_LIMIT;
    let miter_x = if half_height > 0.0 {
        (half_stroke * diagonal / half_height).min(max_miter)
    } else {
        half_stroke
    };
    let miter_y = if half_width > 0.0 {
        (half_stroke * diagonal / half_width).min(max_miter)
    } else {
        half_stroke
    };
    let local_extent_x = half_width + miter_x;
    let local_extent_y = half_height + miter_y;
    let cos_theta = rotation.cos().abs();
    let sin_theta = rotation.sin().abs();
    let extent_x = cos_theta * local_extent_x + sin_theta * local_extent_y;
    let extent_y = sin_theta * local_extent_x + cos_theta * local_extent_y;

    Some(DirtyRegion::new(
        center.x - extent_x,
        center.y - extent_y,
        center.x + extent_x,
        center.y + extent_y,
    ))
}

fn draw_arrow_bounds(frame_view: FrameView, arrow: ArrowData) -> Option<DirtyRegion> {
    if arrow_is_degenerate(&arrow) || !arrow.stroke_width.is_finite() || arrow.stroke_width < 0.0 {
        return None;
    }

    let bounds = arrow_bounds(&arrow);
    let min = canvas_point_to_surface(frame_view, Point::new(bounds.min_x, bounds.min_y));
    let max = canvas_point_to_surface(frame_view, Point::new(bounds.max_x, bounds.max_y));
    Some(DirtyRegion::new(min.x, min.y, max.x, max.y))
}

fn draw_serial_number_connector_bounds(
    frame_view: FrameView,
    item: SerialNumberConnectorDisplayItem,
) -> Option<DirtyRegion> {
    if !item.start_x.is_finite()
        || !item.start_y.is_finite()
        || !item.end_x.is_finite()
        || !item.end_y.is_finite()
        || !item.stroke_width.is_finite()
        || item.stroke_width <= 0.0
    {
        return None;
    }

    let start = canvas_point_to_surface(frame_view, Point::new(item.start_x, item.start_y));
    let end = canvas_point_to_surface(frame_view, Point::new(item.end_x, item.end_y));
    let mut bounds = DirtyRegion::new(
        start.x.min(end.x),
        start.y.min(end.y),
        start.x.max(end.x),
        start.y.max(end.y),
    );
    if item.has_baseline {
        if !item.baseline_start_x.is_finite()
            || !item.baseline_start_y.is_finite()
            || !item.baseline_end_x.is_finite()
            || !item.baseline_end_y.is_finite()
        {
            return None;
        }
        let baseline_start = canvas_point_to_surface(
            frame_view,
            Point::new(item.baseline_start_x, item.baseline_start_y),
        );
        let baseline_end = canvas_point_to_surface(
            frame_view,
            Point::new(item.baseline_end_x, item.baseline_end_y),
        );
        bounds = bounds.union(DirtyRegion::new(
            baseline_start.x.min(baseline_end.x),
            baseline_start.y.min(baseline_end.y),
            baseline_start.x.max(baseline_end.x),
            baseline_start.y.max(baseline_end.y),
        ));
    }

    let padding = (item.stroke_width * frame_view.camera.zoom.max(0.0)).max(1.0);
    Some(DirtyRegion::new(
        bounds.min_x - padding,
        bounds.min_y - padding,
        bounds.max_x + padding,
        bounds.max_y + padding,
    ))
}

fn snap_guide_bounds(item: SnapGuideDisplayItem, frame_view: FrameView) -> Option<DirtyRegion> {
    if !item.line_width.is_finite()
        || !item.marker_size.is_finite()
        || item.line_width < 0.0
        || item.marker_size < 0.0
    {
        return None;
    }

    let start = canvas_point_to_surface(frame_view, item.start);
    let end = canvas_point_to_surface(frame_view, item.end);
    let half_line_width = item.line_width / 2.0;
    let mut bounds = DirtyRegion::new(
        start.x.min(end.x) - half_line_width,
        start.y.min(end.y) - half_line_width,
        start.x.max(end.x) + half_line_width,
        start.y.max(end.y) + half_line_width,
    );

    let marker_radius = match item.kind {
        SnapGuideKind::Gap => item.marker_size * 0.75,
        SnapGuideKind::Point => item.marker_size * 0.5,
    };
    if item.marker_count == 0 {
        bounds = bounds.union(point_bounds(start, marker_radius));
        bounds = bounds.union(point_bounds(end, marker_radius));
    } else {
        for marker in item.markers.iter().take(item.marker_count as usize) {
            bounds = bounds.union(point_bounds(
                canvas_point_to_surface(frame_view, *marker),
                marker_radius,
            ));
        }
    }

    if let Some(label) = item.label {
        bounds = bounds.union(snap_guide_label_bounds(
            item.axis,
            start,
            end,
            format!("{label:.0}").chars().count(),
        ));
    }

    Some(bounds)
}

fn point_bounds(point: Point<f64>, radius: f64) -> DirtyRegion {
    DirtyRegion::new(
        point.x - radius,
        point.y - radius,
        point.x + radius,
        point.y + radius,
    )
}

pub(crate) const SNAP_GUIDE_LABEL_CHAR_WIDTH: f64 = 12.0;
pub(crate) const SNAP_GUIDE_LABEL_BASE_WIDTH: f64 = 20.0;
pub(crate) const SNAP_GUIDE_LABEL_HEIGHT: f64 = 24.0;
pub(crate) const SNAP_GUIDE_LABEL_OFFSET: f64 = 6.0;
pub(crate) const SNAP_GUIDE_LABEL_SIDE_PADDING: f64 = 4.0;
pub(crate) const SNAP_GUIDE_LABEL_VERTICAL_PADDING: f64 = 2.0;

pub(crate) fn snap_guide_label_bounds(
    axis: SnapGuideAxis,
    start: Point<f64>,
    end: Point<f64>,
    text_len: usize,
) -> DirtyRegion {
    let midpoint = Point::new(f64::midpoint(start.x, end.x), f64::midpoint(start.y, end.y));
    let width = SNAP_GUIDE_LABEL_BASE_WIDTH + text_len as f64 * SNAP_GUIDE_LABEL_CHAR_WIDTH;
    match axis {
        SnapGuideAxis::Horizontal => DirtyRegion::new(
            midpoint.x - width / 2.0 - SNAP_GUIDE_LABEL_SIDE_PADDING,
            midpoint.y
                - SNAP_GUIDE_LABEL_HEIGHT
                - SNAP_GUIDE_LABEL_OFFSET
                - SNAP_GUIDE_LABEL_VERTICAL_PADDING,
            midpoint.x + width / 2.0 + SNAP_GUIDE_LABEL_SIDE_PADDING,
            midpoint.y + SNAP_GUIDE_LABEL_HEIGHT / 2.0 + SNAP_GUIDE_LABEL_VERTICAL_PADDING,
        ),
        SnapGuideAxis::Vertical => DirtyRegion::new(
            midpoint.x + SNAP_GUIDE_LABEL_OFFSET - SNAP_GUIDE_LABEL_SIDE_PADDING,
            midpoint.y - SNAP_GUIDE_LABEL_HEIGHT / 2.0 - SNAP_GUIDE_LABEL_VERTICAL_PADDING,
            midpoint.x + SNAP_GUIDE_LABEL_OFFSET + width + SNAP_GUIDE_LABEL_SIDE_PADDING,
            midpoint.y + SNAP_GUIDE_LABEL_HEIGHT / 2.0 + SNAP_GUIDE_LABEL_VERTICAL_PADDING,
        ),
    }
}

pub(crate) fn canvas_point_to_surface(frame_view: FrameView, point: Point<f64>) -> Point<f64> {
    Point::new(
        (point.x - frame_view.camera.center.x) * frame_view.camera.zoom
            + frame_view.surface.width as f64 / 2.0,
        (point.y - frame_view.camera.center.y) * frame_view.camera.zoom
            + frame_view.surface.height as f64 / 2.0,
    )
}

fn display_arrow_to_document_arrow(
    points: &[[f64; 2]],
    arrow_type: ArrowType,
    start_arrowhead: Option<Arrowhead>,
    end_arrowhead: Option<Arrowhead>,
    stroke: ColorRgba8,
    stroke_width: f64,
    stroke_style: StrokeStyle,
) -> ArrowData {
    let global_points = points
        .iter()
        .map(|point| Point::new(point[0], point[1]))
        .collect::<Vec<_>>();
    let mut arrow = ArrowData::from_global_points(
        &global_points,
        stroke,
        stroke_width,
        stroke_style,
        arrow_type,
        start_arrowhead,
        end_arrowhead,
    )
    .unwrap_or_else(|| ArrowData {
        linear_kind: LinearElementKind::Arrow,
        x: 0.0,
        y: 0.0,
        width: 0.0,
        height: 0.0,
        rotation: 0.0,
        points: Vec::new(),
        start_binding: None,
        end_binding: None,
        start_arrowhead,
        end_arrowhead,
        arrow_type,
        fixed_segments: None,
        start_is_special: None,
        end_is_special: None,
        stroke,
        stroke_width,
        stroke_style,
        fill: ColorRgba8::default(),
        fill_style: FillStyle::Solid,
        opacity: 1.0,
    });
    arrow.start_arrowhead = start_arrowhead;
    arrow.end_arrowhead = end_arrowhead;
    arrow.arrow_type = arrow_type;
    arrow.stroke = stroke;
    arrow.stroke_width = stroke_width;
    arrow.stroke_style = stroke_style;
    arrow
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, CornerRadii, SurfaceSize};
    use snow_draw_engine_display::{
        DisplayFillStyle, DisplayItemId, DisplayTextHorizontalAlign, DisplayTextVerticalAlign,
        RectangleDisplayItem,
    };

    fn frame_view() -> FrameView {
        FrameView {
            surface: SurfaceSize {
                width: 1000,
                height: 1000,
            },
            camera: Camera {
                center: Point::default(),
                zoom: 1.0,
            },
            clear_color: ColorRgba8::default(),
        }
    }

    fn text_item() -> TextDisplayItem {
        TextDisplayItem {
            id: DisplayItemId::default(),
            center_x: 0.0,
            center_y: 0.0,
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
            text: "editing".to_owned(),
            color: ColorRgba8::default(),
            font_size: 20.0,
            font_family: None,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
            horizontal_align: DisplayTextHorizontalAlign::Left,
            vertical_align: DisplayTextVerticalAlign::Center,
            opacity: 1.0,
        }
    }

    #[test]
    fn active_text_scene_bounds_include_fill_and_stroke_paint() {
        let mut item = text_item();
        item.fill = ColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        let fill_bounds = draw_text_bounds(frame_view(), &item).unwrap();
        assert!((fill_bounds.min_x - 440.4).abs() < 1e-9);
        assert!((fill_bounds.max_x - 559.6).abs() < 1e-9);
        assert!((fill_bounds.min_y - 477.0).abs() < 1e-9);
        assert!((fill_bounds.max_y - 523.0).abs() < 1e-9);

        item.fill = ColorRgba8::default();
        item.stroke = ColorRgba8 {
            r: 0,
            g: 0,
            b: 0,
            a: 0xff,
        };
        item.stroke_width = 10.0;
        let stroke_bounds = draw_text_bounds(frame_view(), &item).unwrap();
        assert_eq!(stroke_bounds, DirtyRegion::new(445.0, 475.0, 555.0, 525.0));
    }

    #[test]
    fn large_text_scene_bounds_keep_proportional_fill_safety() {
        let mut item = text_item();
        item.font_size = 400.0;
        item.fill = ColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };

        let fill_bounds = draw_text_bounds(frame_view(), &item).unwrap();

        assert_eq!(fill_bounds, DirtyRegion::new(258.0, 420.0, 742.0, 580.0));
    }

    #[test]
    fn diamond_scene_bounds_include_mitered_corners() {
        let item = RectangleDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 100.0,
            height: 100.0,
            stroke_width: 10.0,
            shape: DisplayRectangleShape::Diamond,
            ..RectangleDisplayItem::default()
        };

        let bounds = scene_display_item_bounds(&SceneDisplayItem::Rectangle(item), frame_view())
            .expect("diamond should have dirty bounds");
        let expected_extent = 50.0 + 5.0 * 2.0_f64.sqrt();

        assert!((bounds.min_x - (500.0 - expected_extent)).abs() < 1e-9);
        assert!((bounds.max_x - (500.0 + expected_extent)).abs() < 1e-9);
        assert!((bounds.min_y - (500.0 - expected_extent)).abs() < 1e-9);
        assert!((bounds.max_y - (500.0 + expected_extent)).abs() < 1e-9);
    }

    #[test]
    fn diamond_scene_bounds_account_for_rotation() {
        let item = RectangleDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 100.0,
            height: 100.0,
            rotation: std::f64::consts::FRAC_PI_4,
            stroke_width: 10.0,
            shape: DisplayRectangleShape::Diamond,
            ..RectangleDisplayItem::default()
        };

        let bounds = scene_display_item_bounds(&SceneDisplayItem::Rectangle(item), frame_view())
            .expect("rotated diamond should have dirty bounds");
        let expected_extent = 50.0 * 2.0_f64.sqrt() + 10.0;

        assert!((bounds.min_x - (500.0 - expected_extent)).abs() < 1e-9);
        assert!((bounds.max_x - (500.0 + expected_extent)).abs() < 1e-9);
        assert!((bounds.min_y - (500.0 - expected_extent)).abs() < 1e-9);
        assert!((bounds.max_y - (500.0 + expected_extent)).abs() < 1e-9);
    }
}
