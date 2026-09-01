#![allow(clippy::items_after_test_module)]

use super::*;
use snow_draw_engine_display::{DisplayFilterType, FilterDisplayItem, FilterRenderSpec};
use snow_draw_engine_document::{
    CanvasFilterType, FilterData, FreeDrawData, PenFilterData, filter_bounds, pen_filter_bounds,
};
use snow_draw_engine_editor::{FreeDrawPreview, PenFilterPreview};

pub(crate) fn scene_item_from_rect(id: ElementId, rect: RectangleData) -> SceneDisplayItem {
    SceneDisplayItem::Rectangle(RectangleDisplayItem {
        id: display_item_id(id),
        center_x: rect.center.x,
        center_y: rect.center.y,
        width: rect.width,
        height: rect.height,
        rotation: rect.rotation,
        fill: rect.fill,
        fill_style: display_fill_style(rect.fill_style),
        stroke: rect.stroke,
        stroke_width: rect.stroke_width,
        stroke_style: rect.stroke_style,
        corner_radii: rect.corner_radii,
        opacity: rect.opacity,
        shape: match rect.highlight_shape {
            snow_draw_engine_document::HighlightShape::Rectangle => {
                snow_draw_engine_display::DisplayRectangleShape::Rectangle
            }
            snow_draw_engine_document::HighlightShape::Ellipse => {
                snow_draw_engine_display::DisplayRectangleShape::Ellipse
            }
            snow_draw_engine_document::HighlightShape::Diamond => {
                snow_draw_engine_display::DisplayRectangleShape::Diamond
            }
        },
        blend_mode: if rect.is_highlight() {
            snow_draw_engine_display::DisplayBlendMode::Multiply
        } else {
            snow_draw_engine_display::DisplayBlendMode::Normal
        },
    })
}

pub(crate) fn scene_item_from_arrow(id: ElementId, arrow: ArrowData) -> SceneDisplayItem {
    let path_commands = arrow.path_commands();
    let fill_path_is_closed = arrow.fill_path_is_closed();
    let geometry = Arc::new(snow_draw_engine_core::PathGeometry::from_commands(
        0,
        path_commands.clone(),
        fill_path_is_closed,
    ));
    let arrowhead_primitives = arrowhead_display_primitives(&arrow);
    SceneDisplayItem::Arrow(ArrowDisplayItem {
        id: display_item_id(id),
        points: arrow
            .global_points()
            .iter()
            .map(|point| [point.x, point.y])
            .collect(),
        path_commands,
        geometry,
        arrow_type: arrow.arrow_type,
        start_arrowhead: arrow.start_arrowhead,
        end_arrowhead: arrow.end_arrowhead,
        stroke: arrow.stroke,
        stroke_width: arrow.stroke_width,
        stroke_style: arrow.stroke_style,
        fill: arrow.fill,
        fill_style: display_fill_style(arrow.fill_style),
        arrowhead_primitives,
        opacity: arrow.opacity,
        is_free_draw: false,
        blend_mode: if arrow.is_pen_highlight() {
            snow_draw_engine_display::DisplayBlendMode::Multiply
        } else {
            snow_draw_engine_display::DisplayBlendMode::Normal
        },
    })
}

pub(crate) fn scene_item_from_arrow_revision(
    id: ElementId,
    arrow: ArrowData,
    revision: u64,
) -> SceneDisplayItem {
    let mut item = scene_item_from_arrow(id, arrow);
    if let SceneDisplayItem::Arrow(item) = &mut item {
        let closed = item.geometry.closed;
        item.geometry = Arc::new(snow_draw_engine_core::PathGeometry::from_commands(
            revision,
            item.path_commands.clone(),
            closed,
        ));
    }
    item
}

pub(crate) fn scene_item_from_free_draw(
    id: ElementId,
    free_draw: FreeDrawData,
    geometry: Arc<snow_draw_engine_core::PathGeometry>,
) -> SceneDisplayItem {
    SceneDisplayItem::Arrow(ArrowDisplayItem {
        id: display_item_id(id),
        points: free_draw
            .global_vertices()
            .iter()
            .map(|point| [point.x, point.y])
            .collect(),
        path_commands: geometry.flattened_commands(),
        geometry,
        arrow_type: snow_draw_engine_core::arrow::ArrowType::Curve,
        start_arrowhead: None,
        end_arrowhead: None,
        stroke: free_draw.stroke,
        stroke_width: free_draw.stroke_width,
        stroke_style: free_draw.stroke_style,
        fill: free_draw.fill,
        fill_style: display_fill_style(free_draw.fill_style),
        arrowhead_primitives: Vec::new(),
        opacity: free_draw.opacity,
        is_free_draw: true,
        blend_mode: snow_draw_engine_display::DisplayBlendMode::Normal,
    })
}

pub(crate) fn scene_item_from_free_draw_preview(
    id: ElementId,
    preview: &FreeDrawPreview,
) -> SceneDisplayItem {
    SceneDisplayItem::Arrow(ArrowDisplayItem {
        id: display_item_id(id),
        points: Vec::new(),
        path_commands: preview.geometry.flattened_commands(),
        geometry: preview.geometry.clone(),
        arrow_type: snow_draw_engine_core::arrow::ArrowType::Curve,
        start_arrowhead: None,
        end_arrowhead: None,
        stroke: preview.stroke,
        stroke_width: preview.stroke_width,
        stroke_style: preview.stroke_style,
        fill: preview.fill,
        fill_style: display_fill_style(preview.fill_style),
        arrowhead_primitives: Vec::new(),
        opacity: preview.opacity,
        is_free_draw: true,
        blend_mode: snow_draw_engine_display::DisplayBlendMode::Normal,
    })
}

pub(crate) fn free_draw_preview_bounds(preview: &FreeDrawPreview) -> DrawRect {
    let bounds = preview.geometry.canvas_bounds;
    let outset = preview.stroke_width.max(0.0) * 0.5;
    DrawRect::new(
        bounds[0] - outset,
        bounds[1] - outset,
        bounds[2] + outset,
        bounds[3] + outset,
    )
}

fn arrowhead_display_primitives(arrow: &ArrowData) -> Vec<ArrowheadDisplayPrimitive> {
    [ArrowEndpointPosition::Start, ArrowEndpointPosition::End]
        .into_iter()
        .flat_map(|position| arrowhead_render_primitives(arrow, position))
        .map(arrowhead_display_primitive)
        .collect()
}

fn arrowhead_display_primitive(primitive: ArrowheadRenderPrimitive) -> ArrowheadDisplayPrimitive {
    match primitive {
        ArrowheadRenderPrimitive::Line(line) => ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Line,
            points: vec![line.from, line.to],
            center: [0.0, 0.0],
            diameter: 0.0,
            fill_mode: ArrowheadDisplayFillMode::Stroke,
            dash_mode: arrowhead_display_dash_mode(line.dash_mode),
        },
        ArrowheadRenderPrimitive::Polygon(polygon) => ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Polygon,
            points: polygon.points,
            center: [0.0, 0.0],
            diameter: 0.0,
            fill_mode: arrowhead_display_fill_mode(polygon.fill_mode),
            dash_mode: arrowhead_display_dash_mode(polygon.dash_mode),
        },
        ArrowheadRenderPrimitive::Circle(circle) => ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Circle,
            points: Vec::new(),
            center: circle.center,
            diameter: circle.diameter,
            fill_mode: arrowhead_display_fill_mode(circle.fill_mode),
            dash_mode: arrowhead_display_dash_mode(circle.dash_mode),
        },
    }
}

fn arrowhead_display_fill_mode(fill_mode: ArrowheadFillMode) -> ArrowheadDisplayFillMode {
    match fill_mode {
        ArrowheadFillMode::Stroke => ArrowheadDisplayFillMode::Stroke,
        ArrowheadFillMode::Background => ArrowheadDisplayFillMode::Background,
    }
}

fn arrowhead_display_dash_mode(dash_mode: ArrowheadDashMode) -> ArrowheadDisplayDashMode {
    match dash_mode {
        ArrowheadDashMode::Inherit => ArrowheadDisplayDashMode::Inherit,
        ArrowheadDashMode::Solid => ArrowheadDisplayDashMode::Solid,
        ArrowheadDashMode::DottedCap => ArrowheadDisplayDashMode::DottedCap,
    }
}

pub(crate) fn scene_item_from_text(id: ElementId, text: TextData) -> SceneDisplayItem {
    SceneDisplayItem::Text(TextDisplayItem {
        id: display_item_id(id),
        center_x: text.center.x,
        center_y: text.center.y,
        width: text.width,
        height: text.height,
        rotation: text.rotation,
        text: text.text,
        color: text.color,
        font_size: text.font_size,
        font_family: text.font_family,
        fill: text.fill,
        fill_style: display_fill_style(text.fill_style),
        stroke: text.stroke,
        stroke_width: text.stroke_width,
        corner_radii: text.corner_radii,
        horizontal_align: display_text_horizontal_align(text.horizontal_align),
        vertical_align: display_text_vertical_align(text.vertical_align),
        opacity: text.opacity,
    })
}

pub(crate) fn scene_item_from_serial_number(
    id: ElementId,
    serial: SerialNumberData,
    bound_text_id: Option<ElementId>,
) -> SceneDisplayItem {
    let stroke_width = resolve_serial_number_stroke_width(&serial);
    SceneDisplayItem::SerialNumber(SerialNumberDisplayItem {
        id: display_item_id(id),
        center_x: serial.center.x,
        center_y: serial.center.y,
        diameter: serial.diameter,
        rotation: serial.rotation,
        number: serial.number.max(0),
        color: serial.color,
        fill: serial.fill,
        fill_style: display_fill_style(serial.fill_style),
        font_size: serial.font_size,
        font_family: serial.font_family,
        stroke_width,
        stroke_style: serial.stroke_style,
        opacity: serial.opacity,
        bound_text_id: bound_text_id.map(display_item_id),
    })
}

pub(crate) fn scene_item_with_serial_bound_text(
    mut item: SceneDisplayItem,
    bound_text_id: Option<ElementId>,
) -> SceneDisplayItem {
    if let SceneDisplayItem::SerialNumber(serial) = &mut item {
        serial.bound_text_id = bound_text_id.map(display_item_id);
    }
    item
}

pub(crate) fn scene_item_from_serial_connector(
    id: ElementId,
    serial: &SerialNumberData,
    connection: snow_draw_engine_document::SerialNumberTextConnection,
) -> SceneDisplayItem {
    let baseline_start = connection.text_baseline_start.unwrap_or_default();
    let baseline_end = connection.text_baseline_end.unwrap_or_default();
    SceneDisplayItem::SerialNumberConnector(SerialNumberConnectorDisplayItem {
        id: display_item_id(id),
        start_x: connection.start.x,
        start_y: connection.start.y,
        end_x: connection.end.x,
        end_y: connection.end.y,
        baseline_start_x: baseline_start.x,
        baseline_start_y: baseline_start.y,
        baseline_end_x: baseline_end.x,
        baseline_end_y: baseline_end.y,
        has_baseline: connection.text_baseline_start.is_some()
            && connection.text_baseline_end.is_some(),
        stroke: serial.color,
        stroke_width: resolve_serial_number_stroke_width(serial),
        opacity: serial.opacity,
    })
}

pub(crate) fn serial_connector_bounds(
    connection: &snow_draw_engine_document::SerialNumberTextConnection,
    stroke_width: f64,
) -> DrawRect {
    let mut min_x = connection.start.x.min(connection.end.x);
    let mut min_y = connection.start.y.min(connection.end.y);
    let mut max_x = connection.start.x.max(connection.end.x);
    let mut max_y = connection.start.y.max(connection.end.y);
    if let (Some(start), Some(end)) = (connection.text_baseline_start, connection.text_baseline_end)
    {
        min_x = min_x.min(start.x.min(end.x));
        min_y = min_y.min(start.y.min(end.y));
        max_x = max_x.max(start.x.max(end.x));
        max_y = max_y.max(start.y.max(end.y));
    }
    let padding = stroke_width.max(1.0);
    DrawRect::new(
        min_x - padding,
        min_y - padding,
        max_x + padding,
        max_y + padding,
    )
}

pub(crate) fn scene_item_from_selection_preview(
    model: &DocumentModel,
    id: ElementId,
    rect: RectangleData,
    text_font_size: Option<f64>,
) -> Option<(SceneDisplayItem, DrawRect)> {
    match &model.element(id).ok()?.data {
        ElementData::Rectangle(committed) if !committed.is_spotlight() => {
            Some((scene_item_from_rect(id, rect), rect_bounds(rect)))
        }
        ElementData::Rectangle(_) => None,
        ElementData::Filter(filter) => {
            let mut preview = *filter;
            preview.center = rect.center;
            preview.width = rect.width;
            preview.height = rect.height;
            preview.rotation = rect.rotation;
            preview.opacity = rect.opacity;
            Some((scene_item_from_filter(id, preview), filter_bounds(&preview)))
        }
        ElementData::PenFilter(filter) => {
            let mut preview = filter.clone();
            // Selection rectangles describe the painted outer contour, while
            // PenFilterData stores the unpainted centerline rectangle.
            let stroke_outset = filter.stroke_width.max(0.0);
            preview.width = (rect.width - stroke_outset).max(0.0);
            preview.height = (rect.height - stroke_outset).max(0.0);
            preview.x = rect.center.x - preview.width / 2.0;
            preview.y = rect.center.y - preview.height / 2.0;
            preview.rotation = rect.rotation;
            preview.opacity = rect.opacity;
            let bounds = pen_filter_bounds(&preview);
            Some((scene_item_from_pen_filter(id, preview), bounds))
        }
        ElementData::Text(text) => {
            let mut preview = text.clone();
            preview.center = rect.center;
            preview.width = rect.width;
            preview.height = rect.height;
            preview.rotation = rect.rotation;
            preview.corner_radii = rect.corner_radii;
            preview.opacity = rect.opacity;
            if let Some(font_size) = text_font_size {
                preview.font_size = font_size;
            }
            Some((
                scene_item_from_text(id, preview.clone()),
                text_bounds(&preview),
            ))
        }
        ElementData::SerialNumber(serial) => {
            let mut preview = serial_number_with_selection_rect(serial, rect);
            preview.opacity = rect.opacity;
            Some((
                scene_item_from_serial_number(
                    id,
                    preview.clone(),
                    model.bound_text_id_for_serial_number(id),
                ),
                serial_number_bounds(&preview),
            ))
        }
        ElementData::Arrow(_) => None,
        ElementData::FreeDraw(free_draw) => {
            let mut preview = free_draw.clone();
            preview.x = rect.center.x - rect.width / 2.0;
            preview.y = rect.center.y - rect.height / 2.0;
            preview.width = rect.width;
            preview.height = rect.height;
            preview.rotation = rect.rotation;
            preview.opacity = rect.opacity;
            let bounds = snow_draw_engine_document::free_draw_bounds(&preview);
            let geometry = Arc::new(preview.path_geometry(0));
            Some((scene_item_from_free_draw(id, preview, geometry), bounds))
        }
    }
}

fn display_fill_style(style: FillStyle) -> DisplayFillStyle {
    match style {
        FillStyle::Line => DisplayFillStyle::Line,
        FillStyle::CrossLine => DisplayFillStyle::CrossLine,
        FillStyle::Solid => DisplayFillStyle::Solid,
    }
}


fn display_text_horizontal_align(align: TextHorizontalAlign) -> DisplayTextHorizontalAlign {
    match align {
        TextHorizontalAlign::Left => DisplayTextHorizontalAlign::Left,
        TextHorizontalAlign::Center => DisplayTextHorizontalAlign::Center,
        TextHorizontalAlign::Right => DisplayTextHorizontalAlign::Right,
    }
}

fn display_text_vertical_align(align: TextVerticalAlign) -> DisplayTextVerticalAlign {
    match align {
        TextVerticalAlign::Top => DisplayTextVerticalAlign::Top,
        TextVerticalAlign::Center => DisplayTextVerticalAlign::Center,
        TextVerticalAlign::Bottom => DisplayTextVerticalAlign::Bottom,
    }
}

pub(crate) fn ui_rect_item(kind: UiShapeKind, rect: RectangleData) -> UiRectangleDisplayItem {
    UiRectangleDisplayItem {
        kind,
        center_x: rect.center.x,
        center_y: rect.center.y,
        width: rect.width,
        height: rect.height,
        rotation: rect.rotation,
        fill: rect.fill,
        fill_style: display_fill_style(rect.fill_style),
        stroke: rect.stroke,
        stroke_width: rect.stroke_width,
        corner_radii: rect.corner_radii,
    }
}

pub(crate) fn focus_connection_item(
    anchor: Point<f64>,
    focus_point: Point<f64>,
    zoom: f64,
) -> UiFocusConnectionDisplayItem {
    UiFocusConnectionDisplayItem {
        points: vec![[anchor.x, anchor.y], [focus_point.x, focus_point.y]],
        path_commands: vec![
            ArrowPathCommand::MoveTo {
                point: [anchor.x, anchor.y],
            },
            ArrowPathCommand::LineTo {
                point: [focus_point.x, focus_point.y],
            },
        ],
        arrow_type: ArrowType::Straight,
        start_arrowhead: None,
        end_arrowhead: None,
        stroke: SNOW_SHOT_FOCUS_CONNECTION_STROKE,
        stroke_width: 1.0 / zoom.max(0.0001),
        stroke_style: StrokeStyle::Dashed,
        arrowhead_primitives: Vec::new(),
    }
}

pub(crate) fn hover_arrow_item(arrow: &ArrowData, zoom: f64) -> UiFocusConnectionDisplayItem {
    UiFocusConnectionDisplayItem {
        points: arrow
            .global_points()
            .iter()
            .map(|point| [point.x, point.y])
            .collect(),
        path_commands: arrow.path_commands(),
        arrow_type: arrow.arrow_type,
        start_arrowhead: arrow.start_arrowhead,
        end_arrowhead: arrow.end_arrowhead,
        stroke: SELECTION_COLOR,
        stroke_width: 1.0 / zoom.max(0.0001),
        stroke_style: StrokeStyle::Solid,
        arrowhead_primitives: arrowhead_display_primitives(arrow),
    }
}

pub(crate) fn hover_free_draw_item(
    free_draw: &FreeDrawData,
    zoom: f64,
) -> UiFocusConnectionDisplayItem {
    UiFocusConnectionDisplayItem {
        points: free_draw
            .global_vertices()
            .iter()
            .map(|point| [point.x, point.y])
            .collect(),
        path_commands: free_draw.path_commands(),
        arrow_type: ArrowType::Curve,
        start_arrowhead: None,
        end_arrowhead: None,
        stroke: SELECTION_COLOR,
        stroke_width: 1.0 / zoom.max(0.0001),
        stroke_style: StrokeStyle::Solid,
        arrowhead_primitives: Vec::new(),
    }
}

pub(crate) fn hover_pen_filter_item(filter: &PenFilterData) -> UiFocusConnectionDisplayItem {
    let points = filter.global_points();
    let path_commands = points
        .iter()
        .enumerate()
        .map(|(index, point)| {
            if index == 0 {
                ArrowPathCommand::MoveTo {
                    point: [point.x, point.y],
                }
            } else {
                ArrowPathCommand::LineTo {
                    point: [point.x, point.y],
                }
            }
        })
        .collect();
    UiFocusConnectionDisplayItem {
        points: points.iter().map(|point| [point.x, point.y]).collect(),
        path_commands,
        arrow_type: ArrowType::Straight,
        start_arrowhead: None,
        end_arrowhead: None,
        stroke: SELECTION_COLOR,
        stroke_width: filter.stroke_width,
        stroke_style: StrokeStyle::Solid,
        arrowhead_primitives: Vec::new(),
    }
}

pub(crate) fn arrow_handle_item(handle: ArrowHandleState, zoom: f64) -> UiRectangleDisplayItem {
    let (size, fill, stroke, stroke_width) = match handle.kind {
        ArrowHandleKind::Endpoint => (
            arrow_point_handle_size(zoom),
            SNOW_SHOT_CONTROL_FILL,
            SNOW_SHOT_CONTROL_STROKE,
            1.0 / zoom.max(0.0001),
        ),
        ArrowHandleKind::LoopStart => (
            arrow_point_handle_size(zoom),
            SNOW_SHOT_CONTROL_FILL,
            SNOW_SHOT_CONTROL_STROKE,
            1.0 / zoom.max(0.0001),
        ),
        ArrowHandleKind::LoopEnd => (
            arrow_point_handle_size(zoom) * 2.0,
            TRANSPARENT,
            SNOW_SHOT_CONTROL_STROKE,
            1.0 / zoom.max(0.0001),
        ),
        ArrowHandleKind::FocusPoint => (
            focus_point_handle_size(zoom),
            SNOW_SHOT_CONTROL_FILL,
            SNOW_SHOT_CONTROL_STROKE,
            1.0 / zoom.max(0.0001),
        ),
        ArrowHandleKind::Segment => (
            arrow_point_handle_size(zoom),
            if handle.fixed_segment {
                SNOW_SHOT_CONTROL_FILL
            } else {
                SNOW_SHOT_CONTROL_PHANTOM_FILL
            },
            if handle.fixed_segment {
                SNOW_SHOT_CONTROL_STROKE
            } else {
                TRANSPARENT
            },
            if handle.fixed_segment {
                1.0 / zoom.max(0.0001)
            } else {
                0.0
            },
        ),
    };

    ui_rect_item(
        match handle.kind {
            ArrowHandleKind::Endpoint | ArrowHandleKind::LoopStart | ArrowHandleKind::LoopEnd => {
                UiShapeKind::ArrowEndpointHandle
            }
            ArrowHandleKind::FocusPoint => UiShapeKind::ArrowFocusHandle,
            ArrowHandleKind::Segment => UiShapeKind::ArrowSegmentHandle,
        },
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: handle.center,
            width: size,
            height: size,
            rotation: 0.0,
            fill,
            fill_style: FillStyle::Solid,
            stroke,
            stroke_width,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::splat(size * 0.5),
            opacity: 1.0,
        },
    )
}

pub(crate) fn snap_guide_display_item(
    guide: &SnapGuide,
    config: SnapConfig,
) -> SnapGuideDisplayItem {
    let mut markers = [Point::default(), Point::default()];
    let marker_count = guide.markers.len().min(markers.len());
    for (index, marker) in guide.markers.iter().take(marker_count).enumerate() {
        markers[index] = *marker;
    }

    SnapGuideDisplayItem {
        kind: guide.kind,
        axis: guide.axis,
        start: guide.start,
        end: guide.end,
        marker_count: marker_count as u8,
        markers,
        label: if config.show_gap_size {
            guide.label
        } else {
            None
        },
        color: config.line_color,
        line_width: config.line_width,
        marker_size: config.marker_size,
        gap_dash_length: config.gap_dash_length,
        gap_dash_gap: config.gap_dash_gap,
    }
}

fn display_item_id(id: ElementId) -> DisplayItemId {
    DisplayItemId {
        index: id.index,
        generation: id.generation,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_document::{ElementMeta, Transaction, pen_filter_rect_proxy};
    use snow_draw_engine_model::DocumentModel;

    #[test]
    fn pen_filter_selection_preview_consumes_outer_proxy_once() {
        let filter = PenFilterData {
            x: 10.0,
            y: 20.0,
            width: 100.0,
            height: 40.0,
            points: vec![[0.0, 0.0], [1.0, 1.0]],
            stroke_width: 20.0,
            ..PenFilterData::default()
        };
        let mut model = DocumentModel::new();
        let id = model.peek_next_element_id();
        let mut transaction = Transaction::new("insert pen filter");
        transaction.insert_pen_filter(id, ElementMeta::default(), filter.clone());
        model.apply_transaction(transaction).unwrap();

        let proxy = pen_filter_rect_proxy(&filter);
        let Some((SceneDisplayItem::Filter(item), bounds)) =
            scene_item_from_selection_preview(&model, id, proxy, None)
        else {
            panic!("expected a pen filter selection preview");
        };

        assert_eq!((item.width, item.height), (filter.width, filter.height));
        assert_eq!(bounds, pen_filter_bounds(&filter));
    }

    #[test]
    fn rectangle_display_item_preserves_fill_style() {
        let item = scene_item_from_rect(
            ElementId {
                index: 4,
                generation: 2,
            },
            RectangleData {
                rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                center: Point::default(),
                width: 80.0,
                height: 60.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::CrossLine,
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: StrokeStyle::Dotted,
                corner_radii: CornerRadii::default(),
                opacity: 0.6,
            },
        );

        let SceneDisplayItem::Rectangle(rectangle) = item else {
            panic!("expected a rectangle display item");
        };
        assert_eq!(rectangle.fill_style, DisplayFillStyle::CrossLine);
        assert_eq!(rectangle.stroke_style, StrokeStyle::Dotted);
        assert_eq!(rectangle.opacity, 0.6);
    }

    #[test]
    fn ui_rectangle_display_item_preserves_fill_style() {
        let item = ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            RectangleData {
                rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                center: Point::default(),
                width: 80.0,
                height: 60.0,
                rotation: 0.0,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Line,
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: CornerRadii::default(),
                opacity: 1.0,
            },
        );

        assert_eq!(item.fill_style, DisplayFillStyle::Line);
    }

    #[test]
    fn closed_line_display_geometry_is_marked_closed() {
        let closed_line = ArrowData::from_global_points(
            &[
                Point::new(10.0, 10.0),
                Point::new(90.0, 10.0),
                Point::new(50.0, 90.0),
                Point::new(10.0, 10.0),
            ],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(
            ColorRgba8 {
                r: 255,
                g: 0,
                b: 0,
                a: 255,
            },
            FillStyle::Line,
        );

        let SceneDisplayItem::Arrow(item) = scene_item_from_arrow_revision(
            ElementId {
                index: 7,
                generation: 3,
            },
            closed_line,
            41,
        ) else {
            panic!("expected a line display item");
        };

        assert!(item.geometry.closed);
        assert_eq!(item.geometry.revision, 41);
        assert_eq!(item.fill_style, DisplayFillStyle::Line);
    }

    #[test]
    fn rectangle_highlight_display_item_uses_multiply_blending() {
        let highlight = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::default(),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
        .into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
        let SceneDisplayItem::Rectangle(item) = scene_item_from_rect(
            ElementId {
                index: 8,
                generation: 1,
            },
            highlight,
        ) else {
            panic!("expected rectangle-backed highlight");
        };
        assert_eq!(
            item.shape,
            snow_draw_engine_display::DisplayRectangleShape::Rectangle
        );
        assert_eq!(
            item.blend_mode,
            snow_draw_engine_display::DisplayBlendMode::Multiply
        );
    }

    #[test]
    fn pen_highlight_display_item_uses_multiply_blending() {
        let pen = ArrowData::from_global_points(
            &[Point::new(10.0, 20.0), Point::new(50.0, 60.0)],
            ColorRgba8::default(),
            30.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap()
        .into_pen_highlight();
        let SceneDisplayItem::Arrow(item) = scene_item_from_arrow(
            ElementId {
                index: 9,
                generation: 1,
            },
            pen,
        ) else {
            panic!("expected arrow-backed pen highlight");
        };
        assert_eq!(
            item.blend_mode,
            snow_draw_engine_display::DisplayBlendMode::Multiply
        );
    }

    #[test]
    fn filter_display_item_preserves_effect_and_normalizes_strength() {
        let filter = FilterData {
            center: Point::new(12.0, 18.0),
            width: 80.0,
            height: 40.0,
            rotation: 0.25,
            filter_type: CanvasFilterType::GaussianBlur,
            strength: f64::NAN,
            opacity: 0.65,
        };
        let SceneDisplayItem::Filter(item) = scene_item_from_filter(
            ElementId {
                index: 9,
                generation: 3,
            },
            filter,
        ) else {
            panic!("expected a filter display item");
        };

        assert_eq!(item.filter.filter_type, DisplayFilterType::GaussianBlur);
        assert_eq!(item.filter.strength, 1.0);
        assert_eq!(item.filter.blur_sigma, 18.5);
        assert_eq!(item.filter.sampling_radius, 56.5);
        assert_eq!(item.opacity, 0.65);
        assert_eq!((item.center_x, item.center_y), (12.0, 18.0));
        assert_eq!((item.width, item.height), (80.0, 40.0));
        assert_eq!(item.rotation, 0.25);
    }

    #[test]
    fn color_filter_display_items_ignore_strength() {
        for filter_type in [CanvasFilterType::Grayscale, CanvasFilterType::Inversion] {
            let filter = FilterData {
                filter_type,
                strength: 0.0,
                ..FilterData::default()
            };
            let SceneDisplayItem::Filter(item) = scene_item_from_filter(
                ElementId {
                    index: 9,
                    generation: 4,
                },
                filter,
            ) else {
                panic!("expected a filter display item");
            };

            assert_eq!(item.filter.strength, 1.0);
            assert_eq!(item.filter.sampling_radius, 0.0);
        }
    }
}

pub(crate) fn scene_item_from_filter(id: ElementId, filter: FilterData) -> SceneDisplayItem {
    SceneDisplayItem::Filter(FilterDisplayItem {
        id: display_item_id(id),
        center_x: filter.center.x,
        center_y: filter.center.y,
        width: filter.width,
        height: filter.height,
        rotation: filter.rotation,
        points: Vec::new().into(),
        stroke_width: 0.0,
        is_pen_filter: false,
        filter: FilterRenderSpec::resolve(
            match filter.filter_type {
                CanvasFilterType::Mosaic => DisplayFilterType::Mosaic,
                CanvasFilterType::GaussianBlur => DisplayFilterType::GaussianBlur,
                CanvasFilterType::Grayscale => DisplayFilterType::Grayscale,
                CanvasFilterType::Inversion => DisplayFilterType::Inversion,
            },
            FilterData::normalized_strength(filter.strength),
        ),
        opacity: filter.opacity,
    })
}

pub(crate) fn scene_item_from_pen_filter(id: ElementId, filter: PenFilterData) -> SceneDisplayItem {
    SceneDisplayItem::Filter(FilterDisplayItem {
        id: display_item_id(id),
        center_x: filter.center().x,
        center_y: filter.center().y,
        width: filter.width,
        height: filter.height,
        rotation: filter.rotation,
        points: filter
            .global_points()
            .iter()
            .map(|point| [point.x, point.y])
            .collect::<Vec<_>>()
            .into(),
        stroke_width: filter.stroke_width,
        is_pen_filter: true,
        filter: FilterRenderSpec::resolve(
            match filter.filter_type {
                CanvasFilterType::Mosaic => DisplayFilterType::Mosaic,
                CanvasFilterType::GaussianBlur => DisplayFilterType::GaussianBlur,
                CanvasFilterType::Grayscale => DisplayFilterType::Grayscale,
                CanvasFilterType::Inversion => DisplayFilterType::Inversion,
            },
            FilterData::normalized_strength(filter.strength),
        ),
        opacity: filter.opacity,
    })
}

pub(crate) fn scene_item_from_pen_filter_preview(
    id: ElementId,
    preview: &PenFilterPreview,
) -> Option<(SceneDisplayItem, DrawRect)> {
    if preview.global_points.len() < 2
        || !preview.stroke_width.is_finite()
        || preview.stroke_width <= 0.0
        || !preview.opacity.is_finite()
    {
        return None;
    }
    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    let mut points = Vec::with_capacity(preview.global_points.len());
    for point in &preview.global_points {
        if !point.x.is_finite() || !point.y.is_finite() {
            return None;
        }
        min_x = min_x.min(point.x);
        min_y = min_y.min(point.y);
        max_x = max_x.max(point.x);
        max_y = max_y.max(point.y);
        points.push([point.x, point.y]);
    }
    let width = max_x - min_x;
    let height = max_y - min_y;
    let center_x = (min_x + max_x) / 2.0;
    let center_y = (min_y + max_y) / 2.0;
    let half_stroke = preview.stroke_width / 2.0;
    let bounds = DrawRect::new(
        min_x - half_stroke,
        min_y - half_stroke,
        max_x + half_stroke,
        max_y + half_stroke,
    );
    Some((
        SceneDisplayItem::Filter(FilterDisplayItem {
            id: display_item_id(id),
            center_x,
            center_y,
            width,
            height,
            rotation: 0.0,
            points: points.into(),
            stroke_width: preview.stroke_width,
            is_pen_filter: true,
            filter: FilterRenderSpec::resolve(
                match preview.filter_type {
                    CanvasFilterType::Mosaic => DisplayFilterType::Mosaic,
                    CanvasFilterType::GaussianBlur => DisplayFilterType::GaussianBlur,
                    CanvasFilterType::Grayscale => DisplayFilterType::Grayscale,
                    CanvasFilterType::Inversion => DisplayFilterType::Inversion,
                },
                FilterData::normalized_strength(preview.strength),
            ),
            opacity: preview.opacity,
        }),
        bounds,
    ))
}
