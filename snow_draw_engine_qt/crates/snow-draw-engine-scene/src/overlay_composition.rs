use super::*;

pub(crate) fn compose_overlay_items(
    snap_config: SnapConfig,
    presentation: &EditorPresentationState,
    frame_view: FrameView,
) -> Vec<OverlayDisplayItem> {
    let mut items = Vec::new();
    let zoom = frame_view.camera.zoom;

    if let Some(position) = presentation.eraser_cursor
        && zoom.is_finite()
        && zoom > 0.0
    {
        items.push(OverlayDisplayItem::Rectangle(UiRectangleDisplayItem {
            kind: UiShapeKind::EraserCursor,
            center_x: position.x,
            center_y: position.y,
            width: 16.0 / zoom,
            height: 16.0 / zoom,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 0xff,
                g: 0xff,
                b: 0xff,
                a: 20,
            },
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 140,
            },
            stroke_width: 1.5 / zoom,
            corner_radii: CornerRadii::default(),
        }));
    }

    if let Some(cursor) = presentation.stroke_cursor
        && zoom.is_finite()
        && zoom > 0.0
        && cursor.stroke_width.is_finite()
        && cursor.stroke_width > 0.0
    {
        items.push(OverlayDisplayItem::Rectangle(UiRectangleDisplayItem {
            kind: UiShapeKind::EraserCursor,
            center_x: cursor.position.x,
            center_y: cursor.position.y,
            width: cursor.stroke_width,
            height: cursor.stroke_width,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 0xff,
                g: 0xff,
                b: 0xff,
                a: 20,
            },
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 140,
            },
            stroke_width: 1.5 / zoom,
            corner_radii: CornerRadii::default(),
        }));
    }

    for element in &presentation.marquee_candidate_elements {
        items.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            selection_member_frame_for_rect(element, &presentation.text_rect_ids, zoom),
        )));
    }

    for arrow in &presentation.marquee_candidate_arrows {
        if arrow_is_degenerate(&arrow.arrow) {
            continue;
        }
        items.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            selection_outline_for_arrow_bounds(arrow_bounds(&arrow.arrow), zoom),
        )));
    }

    if let Some(marquee) = presentation.marquee {
        items.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionMarquee,
            marquee,
        )));
    }

    if let Some(rect) = presentation.hovered_rect {
        items.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            rect,
        )));
    }

    if let Some(rect) = presentation.hovered_text_rect {
        items.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::TextHoverUnderline,
            text_hover_underline_for_rect(rect, zoom),
        )));
    }

    if let Some(free_draw) = presentation.hovered_free_draw.as_ref()
        && free_draw.vertices.len() >= 2
    {
        items.push(OverlayDisplayItem::FocusConnection(hover_free_draw_item(
            free_draw, zoom,
        )));
    }

    if let Some(arrow) = presentation.hovered_arrow.as_ref()
        && !arrow_is_degenerate(arrow)
    {
        items.push(OverlayDisplayItem::FocusConnection(hover_arrow_item(
            arrow, zoom,
        )));
    }

    if let Some(pen_filter) = presentation.hovered_pen_filter.as_ref()
        && pen_filter.global_points().len() >= 2
    {
        items.push(OverlayDisplayItem::PenFilterContour(hover_pen_filter_item(
            pen_filter,
        )));
    }

    let selected_item_count =
        presentation.selection_elements.len() + presentation.selection_arrows.len();
    if selection_box_visible_for_members(
        &presentation.selection_elements,
        &presentation.selection_arrows,
    ) {
        if let Some(bounds) = presentation.selection_bounds {
            append_selection_overlay_items(
                &mut items,
                SelectionOverlayRequest {
                    bounds,
                    selected_elements: &presentation.selection_elements,
                    text_rect_ids: &presentation.text_rect_ids,
                    selected_item_count,
                    selected_arrow_count: presentation.selection_arrows.len(),
                    single_rect: presentation.selected_single_rect.as_ref(),
                    single_text_rect: presentation.selected_single_text_rect.as_ref(),
                    zoom,
                },
            );
            if selected_item_count > 1 {
                append_selected_arrow_candidate_overlay_items(
                    &mut items,
                    &presentation.selection_arrows,
                    zoom,
                );
            }
        } else {
            append_selected_arrow_overlay_items(&mut items, &presentation.selection_arrows, zoom);
        }
    }

    for handle in &presentation.arrow_handles {
        if handle.kind == ArrowHandleKind::FocusPoint
            && let Some(anchor) = handle.anchor
        {
            items.push(OverlayDisplayItem::FocusConnection(focus_connection_item(
                anchor,
                handle.center,
                zoom,
            )));
        }
        items.push(OverlayDisplayItem::Rectangle(arrow_handle_item(
            *handle, zoom,
        )));
    }

    for guide in &presentation.snap_guides {
        items.push(OverlayDisplayItem::SnapGuide(snap_guide_display_item(
            guide,
            snap_config,
        )));
    }

    items.retain(|item| overlay_item_visible(item, frame_view));
    items
}

struct SelectionOverlayRequest<'a> {
    bounds: SelectionBounds,
    selected_elements: &'a [SelectionRectState],
    text_rect_ids: &'a [ElementId],
    selected_item_count: usize,
    selected_arrow_count: usize,
    single_rect: Option<&'a RectangleData>,
    single_text_rect: Option<&'a RectangleData>,
    zoom: f64,
}

fn append_selection_overlay_items(
    out: &mut Vec<OverlayDisplayItem>,
    request: SelectionOverlayRequest<'_>,
) {
    let SelectionOverlayRequest {
        bounds,
        selected_elements,
        text_rect_ids,
        selected_item_count,
        selected_arrow_count,
        single_rect,
        single_text_rect,
        zoom,
    } = request;

    if selected_item_count > 1 {
        for element in selected_elements {
            out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
                UiShapeKind::SelectionCandidateFrame,
                selection_member_frame_for_rect(element, text_rect_ids, zoom),
            )));
        }
    }

    let frame_padding = if selected_item_count == 1 && single_text_rect.is_some() {
        text_selection_frame_padding(zoom)
    } else {
        selection_frame_padding_for_members(zoom, selected_elements.len(), selected_arrow_count)
    };
    let corner_handle_outset = selection_corner_handle_outset_for_members(
        zoom,
        selected_elements.len(),
        selected_arrow_count,
    );
    let frame = selection_outline_bounds(bounds, zoom, frame_padding);
    out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
        if selected_item_count > 1 {
            UiShapeKind::SelectionMultiFrame
        } else {
            UiShapeKind::SelectionFrame
        },
        frame,
    )));
    if selected_item_count == 1
        && let Some(text_rect) = single_text_rect
    {
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::TextActualFrame,
            text_actual_frame(*text_rect, zoom),
        )));
    }

    let handle_size = selection_handle_size(zoom);
    for corner in Corner::ALL {
        let center =
            selection_resize_handle_center(bounds, frame_padding, corner_handle_outset, corner);
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionResizeHandle,
            selection_handle_rect(center, handle_size, bounds.rotation, false, zoom),
        )));
    }

    let rotation_center = selection_rotation_handle_center(bounds, zoom, frame_padding);
    out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
        UiShapeKind::SelectionRotationHandle,
        selection_handle_rect(rotation_center, handle_size, 0.0, true, zoom),
    )));

    if let Some(rect) = single_rect.filter(|rect| rect.supports_corner_radius()) {
        for corner in Corner::ALL {
            let center = rect_local_to_canvas(
                rect.center,
                rect.rotation,
                corner_radius_handle_local_point(*rect, zoom, corner),
            );
            out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
                UiShapeKind::SelectionCornerRadiusHandle,
                selection_handle_rect(center, handle_size, 0.0, true, zoom),
            )));
        }
    }
}

fn selection_member_frame_for_rect(
    element: &SelectionRectState,
    text_rect_ids: &[ElementId],
    zoom: f64,
) -> RectangleData {
    if text_rect_ids.contains(&element.id) {
        text_actual_frame(element.rect, zoom)
    } else {
        selection_outline_for_rect(element.rect, zoom)
    }
}

fn append_selected_arrow_candidate_overlay_items(
    out: &mut Vec<OverlayDisplayItem>,
    selected_arrows: &[SelectionArrowState],
    zoom: f64,
) {
    for arrow in selected_arrows {
        if arrow_is_degenerate(&arrow.arrow) {
            continue;
        }
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            selection_outline_for_arrow_bounds(arrow_bounds(&arrow.arrow), zoom),
        )));
    }
}

fn append_selected_arrow_overlay_items(
    out: &mut Vec<OverlayDisplayItem>,
    selected_arrows: &[SelectionArrowState],
    zoom: f64,
) {
    let mut outlined_arrows = Vec::with_capacity(selected_arrows.len());
    let mut multi_bounds: Option<DrawRect> = None;
    for arrow in selected_arrows {
        if arrow_is_degenerate(&arrow.arrow) {
            continue;
        }
        let bounds = arrow_bounds(&arrow.arrow);
        outlined_arrows.push(selection_outline_for_arrow_bounds(bounds, zoom));
        multi_bounds = Some(match multi_bounds {
            Some(current) => DrawRect::new(
                current.min_x.min(bounds.min_x),
                current.min_y.min(bounds.min_y),
                current.max_x.max(bounds.max_x),
                current.max_y.max(bounds.max_y),
            ),
            None => bounds,
        });
    }
    if outlined_arrows.is_empty() {
        return;
    }

    if outlined_arrows.len() == 1 {
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionFrame,
            outlined_arrows[0],
        )));
        return;
    }

    for rect in &outlined_arrows {
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionCandidateFrame,
            *rect,
        )));
    }
    if let Some(multi_bounds) = multi_bounds {
        out.push(OverlayDisplayItem::Rectangle(ui_rect_item(
            UiShapeKind::SelectionMultiFrame,
            selection_outline_for_arrow_bounds(multi_bounds, zoom),
        )));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, SurfaceSize};
    use snow_draw_engine_document::FreeDrawData;

    fn element_id(index: u32) -> ElementId {
        ElementId {
            index,
            generation: 1,
        }
    }

    fn rect(width: f64, height: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(0.0, 0.0),
            width,
            height,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn frame_view() -> FrameView {
        FrameView {
            surface: SurfaceSize {
                width: 1000,
                height: 1000,
            },
            camera: Camera {
                center: Point::new(0.0, 0.0),
                zoom: 1.0,
            },
            clear_color: ColorRgba8::default(),
        }
    }

    fn overlay_rect(items: &[OverlayDisplayItem], index: usize) -> UiRectangleDisplayItem {
        match items[index] {
            OverlayDisplayItem::Rectangle(item) => item,
            _ => panic!("expected rectangle overlay item"),
        }
    }

    #[test]
    fn marquee_text_candidate_uses_actual_text_rect() {
        let text_id = element_id(1);
        let mut text_rect = rect(100.0, 40.0);
        text_rect.corner_radii = CornerRadii::splat(18.0);
        let mut presentation = EditorPresentationState {
            marquee_candidate_elements: vec![SelectionRectState {
                id: text_id,
                rect: text_rect,
            }],
            text_rect_ids: vec![text_id],
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let frame = overlay_rect(&items, 0);

        assert_eq!(frame.kind, UiShapeKind::SelectionCandidateFrame);
        assert_eq!(frame.width, 100.0);
        assert_eq!(frame.height, 40.0);
        assert_eq!(frame.corner_radii, CornerRadii::default());

        presentation.text_rect_ids.clear();
        let padded_items =
            compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let padded_frame = overlay_rect(&padded_items, 0);
        assert_eq!(padded_frame.width, 108.0);
        assert_eq!(padded_frame.height, 48.0);
    }

    #[test]
    fn marquee_rectangle_preserves_the_selection_visual_style() {
        let marquee = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(20.0, 30.0),
            width: 100.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 0x40,
                g: 0x96,
                b: 0xff,
                a: 0x33,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 0x40,
                g: 0x96,
                b: 0xff,
                a: 0xff,
            },
            stroke_width: 1.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let presentation = EditorPresentationState {
            marquee: Some(marquee),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let item = overlay_rect(&items, 0);

        assert_eq!(item.kind, UiShapeKind::SelectionMarquee);
        assert_eq!(item.center_x, marquee.center.x);
        assert_eq!(item.center_y, marquee.center.y);
        assert_eq!(item.width, marquee.width);
        assert_eq!(item.height, marquee.height);
        assert_eq!(item.fill, marquee.fill);
        assert_eq!(item.fill_style, DisplayFillStyle::Solid);
        assert_eq!(item.stroke, marquee.stroke);
        assert_eq!(item.stroke_width, marquee.stroke_width);
    }

    #[test]
    fn hovered_text_uses_an_underline_instead_of_a_selection_frame() {
        let presentation = EditorPresentationState {
            hovered_text_rect: Some(rect(100.0, 40.0)),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let underline = overlay_rect(&items, 0);

        assert_eq!(underline.kind, UiShapeKind::TextHoverUnderline);
        assert_eq!(underline.center_x, 0.0);
        assert_eq!(underline.center_y, 0.0);
        assert_eq!(underline.width, 100.0);
        assert_eq!(underline.height, 40.0);
        assert_eq!(underline.fill, TRANSPARENT);
        assert_eq!(underline.stroke, SELECTION_COLOR);
        assert_eq!(underline.stroke_width, 1.5);
    }

    #[test]
    fn hovered_arrow_uses_a_selection_colored_path_instead_of_a_frame() {
        let arrow = ArrowData::from_global_points(
            &[Point::new(10.0, 20.0), Point::new(110.0, 70.0)],
            ColorRgba8::default(),
            4.0,
            StrokeStyle::Dashed,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("two points should create an arrow");
        let presentation = EditorPresentationState {
            hovered_arrow: Some(arrow),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let OverlayDisplayItem::FocusConnection(hover) = &items[0] else {
            panic!("hovered arrows should be represented by an overlay path");
        };

        assert_eq!(hover.points, vec![[10.0, 20.0], [110.0, 70.0]]);
        assert_eq!(hover.stroke, SELECTION_COLOR);
        assert_eq!(hover.stroke_width, 1.0);
        assert_eq!(hover.stroke_style, StrokeStyle::Solid);
    }

    #[test]
    fn hovered_free_draw_uses_a_thin_selection_colored_path() {
        let free_draw = FreeDrawData::from_global_vertices(
            &[
                Point::new(10.0, 20.0),
                Point::new(60.0, 80.0),
                Point::new(110.0, 30.0),
            ],
            vec![
                snow_draw_engine_core::PathSegmentMode::Curve,
                snow_draw_engine_core::PathSegmentMode::Curve,
            ],
            false,
            snow_draw_engine_document::FreeDrawStyle {
                stroke: ColorRgba8::default(),
                stroke_width: 12.0,
                stroke_style: StrokeStyle::Dashed,
                fill: ColorRgba8::default(),
                fill_style: FillStyle::Solid,
                opacity: 1.0,
            },
        )
        .expect("valid points should create a free draw");
        let expected_commands = free_draw.path_commands();
        let presentation = EditorPresentationState {
            hovered_free_draw: Some(free_draw),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        assert_eq!(items.len(), 1);
        let OverlayDisplayItem::FocusConnection(hover) = &items[0] else {
            panic!("hovered free draw should be represented by an overlay path");
        };

        assert_eq!(
            hover.points,
            vec![[10.0, 20.0], [60.0, 80.0], [110.0, 30.0]]
        );
        assert_eq!(hover.path_commands, expected_commands);
        assert_eq!(hover.arrow_type, ArrowType::Curve);
        assert_eq!(hover.stroke, SELECTION_COLOR);
        assert_eq!(hover.stroke_width, 1.0);
        assert_eq!(hover.stroke_style, StrokeStyle::Solid);
        assert_eq!(hover.start_arrowhead, None);
        assert_eq!(hover.end_arrowhead, None);
    }

    #[test]
    fn hovered_pen_filter_uses_a_selection_colored_contour_instead_of_a_frame() {
        let pen_filter = snow_draw_engine_document::PenFilterData {
            x: 10.0,
            y: 20.0,
            width: 100.0,
            height: 50.0,
            rotation: 0.0,
            points: vec![[0.0, 0.0], [0.4, 0.8], [1.0, 0.25]],
            filter_type: snow_draw_engine_document::CanvasFilterType::Mosaic,
            strength: 0.5,
            stroke_width: 30.0,
            opacity: 1.0,
        };
        let presentation = EditorPresentationState {
            hovered_pen_filter: Some(pen_filter),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let OverlayDisplayItem::PenFilterContour(hover) = &items[0] else {
            panic!("hovered pen filters should be represented by an overlay contour");
        };

        assert_eq!(
            hover.points,
            vec![[10.0, 20.0], [50.0, 60.0], [110.0, 32.5]]
        );
        assert_eq!(hover.stroke, SELECTION_COLOR);
        assert_eq!(hover.stroke_width, 30.0);
        assert_eq!(hover.stroke_style, StrokeStyle::Solid);
        assert_eq!(
            hover.path_commands,
            vec![
                ArrowPathCommand::MoveTo {
                    point: [10.0, 20.0]
                },
                ArrowPathCommand::LineTo {
                    point: [50.0, 60.0]
                },
                ArrowPathCommand::LineTo {
                    point: [110.0, 32.5]
                },
            ]
        );
    }

    #[test]
    fn selected_pen_filter_keeps_its_selection_box_without_a_hover_contour() {
        let mut selection_rect = rect(100.0, 50.0);
        selection_rect.center = Point::new(60.0, 45.0);
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: selection_rect.center,
                width: selection_rect.width,
                height: selection_rect.height,
                rotation: selection_rect.rotation,
            }),
            selection_elements: vec![SelectionRectState {
                id: element_id(1),
                rect: selection_rect,
            }],
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());

        assert!(
            !items
                .iter()
                .any(|item| matches!(item, OverlayDisplayItem::PenFilterContour(_)))
        );
        assert!(items.iter().any(|item| matches!(
            item,
            OverlayDisplayItem::Rectangle(rect)
                if rect.kind == UiShapeKind::SelectionFrame
        )));
    }

    #[test]
    fn eraser_cursor_has_a_stable_sixteen_pixel_view_size() {
        let presentation = EditorPresentationState {
            eraser_cursor: Some(Point::new(12.0, 34.0)),
            ..EditorPresentationState::default()
        };
        let mut view = frame_view();
        view.camera.zoom = 2.0;

        let items = compose_overlay_items(SnapConfig::default(), &presentation, view);
        let cursor = overlay_rect(&items, 0);

        assert_eq!(cursor.kind, UiShapeKind::EraserCursor);
        assert_eq!((cursor.center_x, cursor.center_y), (12.0, 34.0));
        assert_eq!((cursor.width, cursor.height), (8.0, 8.0));
        assert_eq!(cursor.stroke_width, 0.75);
        assert_eq!(cursor.fill.a, 20);
        assert_eq!(cursor.stroke.a, 140);
    }

    #[test]
    fn stroke_cursor_uses_the_canvas_space_stroke_width() {
        let presentation = EditorPresentationState {
            stroke_cursor: Some(snow_draw_engine_editor::EditorStrokeCursor {
                position: Point::new(12.0, 34.0),
                stroke_width: 30.0,
            }),
            ..EditorPresentationState::default()
        };
        let mut view = frame_view();
        view.camera.zoom = 2.0;

        let items = compose_overlay_items(SnapConfig::default(), &presentation, view);
        let cursor = overlay_rect(&items, 0);

        assert_eq!(cursor.kind, UiShapeKind::EraserCursor);
        assert_eq!((cursor.center_x, cursor.center_y), (12.0, 34.0));
        assert_eq!((cursor.width, cursor.height), (30.0, 30.0));
        assert_eq!(cursor.stroke_width, 0.75);
        assert_eq!(cursor.fill.a, 20);
        assert_eq!(cursor.stroke.a, 140);
    }

    #[test]
    fn two_point_selected_arrow_hides_selection_box_and_keeps_endpoint_handles() {
        let arrow = ArrowData::from_global_points(
            &[Point::new(-40.0, -20.0), Point::new(40.0, 20.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("two points should create an arrow");
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: Point::new(0.0, 0.0),
                width: 80.0,
                height: 40.0,
                rotation: 0.0,
            }),
            selection_arrows: vec![SelectionArrowState {
                id: element_id(1),
                arrow,
            }],
            arrow_handles: vec![
                ArrowHandleState {
                    kind: ArrowHandleKind::Endpoint,
                    center: Point::new(-40.0, -20.0),
                    anchor: None,
                    fixed_segment: false,
                },
                ArrowHandleState {
                    kind: ArrowHandleKind::Endpoint,
                    center: Point::new(40.0, 20.0),
                    anchor: None,
                    fixed_segment: false,
                },
            ],
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        assert!(!items.iter().any(|item| matches!(
            item,
            OverlayDisplayItem::Rectangle(rect)
                if matches!(
                    rect.kind,
                    UiShapeKind::SelectionFrame
                        | UiShapeKind::SelectionResizeHandle
                        | UiShapeKind::SelectionRotationHandle
                )
        )));
        assert_eq!(
            items
                .iter()
                .filter(|item| matches!(
                    item,
                    OverlayDisplayItem::Rectangle(rect)
                        if rect.kind == UiShapeKind::ArrowEndpointHandle
                ))
                .count(),
            2
        );
    }

    #[test]
    fn multi_point_selected_arrow_keeps_selection_box() {
        let arrow = ArrowData::from_global_points(
            &[
                Point::new(-40.0, -20.0),
                Point::new(0.0, 60.0),
                Point::new(40.0, 20.0),
            ],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("three points should create an arrow");
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: Point::new(0.0, 20.0),
                width: 80.0,
                height: 80.0,
                rotation: 0.0,
            }),
            selection_arrows: vec![SelectionArrowState {
                id: element_id(1),
                arrow,
            }],
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        assert!(items.iter().any(|item| matches!(
            item,
            OverlayDisplayItem::Rectangle(rect)
                if rect.kind == UiShapeKind::SelectionFrame
        )));
        assert_eq!(
            items
                .iter()
                .filter(|item| matches!(
                    item,
                    OverlayDisplayItem::Rectangle(rect)
                        if rect.kind == UiShapeKind::SelectionResizeHandle
                ))
                .count(),
            4
        );
        assert_eq!(
            items
                .iter()
                .filter(|item| matches!(
                    item,
                    OverlayDisplayItem::Rectangle(rect)
                        if rect.kind == UiShapeKind::SelectionRotationHandle
                ))
                .count(),
            1
        );
    }

    #[test]
    fn selected_rectangle_highlight_hides_corner_radius_handles() {
        let highlight =
            rect(100.0, 40.0).into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: highlight.center,
                width: highlight.width,
                height: highlight.height,
                rotation: highlight.rotation,
            }),
            selection_elements: vec![SelectionRectState {
                id: element_id(1),
                rect: highlight,
            }],
            selected_single_rect: Some(highlight),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());

        assert!(!items.iter().any(|item| matches!(
            item,
            OverlayDisplayItem::Rectangle(rect)
                if rect.kind == UiShapeKind::SelectionCornerRadiusHandle
        )));
    }

    #[test]
    fn selected_spotlight_hides_corner_radius_handles() {
        let spotlight = rect(100.0, 40.0).into_spotlight();
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: spotlight.center,
                width: spotlight.width,
                height: spotlight.height,
                rotation: spotlight.rotation,
            }),
            selection_elements: vec![SelectionRectState {
                id: element_id(1),
                rect: spotlight,
            }],
            selected_single_rect: Some(spotlight),
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());

        assert!(!items.iter().any(|item| matches!(
            item,
            OverlayDisplayItem::Rectangle(rect)
                if rect.kind == UiShapeKind::SelectionCornerRadiusHandle
        )));
    }

    #[test]
    fn selected_ellipse_and_diamond_hide_corner_radius_handles() {
        for shape in [
            snow_draw_engine_document::HighlightShape::Ellipse,
            snow_draw_engine_document::HighlightShape::Diamond,
        ] {
            let mut selected_rect = rect(100.0, 40.0);
            selected_rect.highlight_shape = shape;
            let presentation = EditorPresentationState {
                selection_bounds: Some(SelectionBounds {
                    center: selected_rect.center,
                    width: selected_rect.width,
                    height: selected_rect.height,
                    rotation: selected_rect.rotation,
                }),
                selection_elements: vec![SelectionRectState {
                    id: element_id(1),
                    rect: selected_rect,
                }],
                selected_single_rect: Some(selected_rect),
                ..EditorPresentationState::default()
            };

            let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());

            assert!(!items.iter().any(|item| matches!(
                item,
                OverlayDisplayItem::Rectangle(rect)
                    if rect.kind == UiShapeKind::SelectionCornerRadiusHandle
            )));
        }
    }

    #[test]
    fn multi_selection_text_member_uses_actual_rect() {
        let text_id = element_id(1);
        let rect_id = element_id(2);
        let presentation = EditorPresentationState {
            selection_bounds: Some(SelectionBounds {
                center: Point::new(0.0, 0.0),
                width: 100.0,
                height: 40.0,
                rotation: 0.0,
            }),
            selection_elements: vec![
                SelectionRectState {
                    id: text_id,
                    rect: rect(100.0, 40.0),
                },
                SelectionRectState {
                    id: rect_id,
                    rect: rect(80.0, 20.0),
                },
            ],
            text_rect_ids: vec![text_id],
            ..EditorPresentationState::default()
        };

        let items = compose_overlay_items(SnapConfig::default(), &presentation, frame_view());
        let text_frame = overlay_rect(&items, 0);
        let rect_frame = overlay_rect(&items, 1);

        assert_eq!(text_frame.kind, UiShapeKind::SelectionCandidateFrame);
        assert_eq!(text_frame.width, 100.0);
        assert_eq!(text_frame.height, 40.0);
        assert_eq!(rect_frame.kind, UiShapeKind::SelectionCandidateFrame);
        assert_eq!(rect_frame.width, 88.0);
        assert_eq!(rect_frame.height, 28.0);
    }
}
