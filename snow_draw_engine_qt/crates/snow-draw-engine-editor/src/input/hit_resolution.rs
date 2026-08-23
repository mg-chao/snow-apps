use super::*;
use snow_draw_engine_document::{
    ElementData, arrow_hit_test, filter_hit_test, free_draw_hit_test, rectangle_hit_test,
    serial_number_hit_test, text_hit_test,
};

impl Editor {
    pub(crate) fn resolve_canvas_hit(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
        include_selection_handles: bool,
    ) -> CanvasHit {
        let single_rect = self
            .selected_single_rectangle_snapshot(document)
            .map(|(_, rect)| rect);
        let single_text_rect = self
            .selected_single_text_rect_snapshot(document)
            .map(|(_, rect)| rect);
        let single_arrow = self.selected_single_arrow_snapshot(document);
        let prioritize_arrow_handles = single_rect.is_none() && single_arrow.is_some();
        let (selected_elements, selected_arrows) = match &self.state.interaction {
            InteractionState::EditingSelection(state) => {
                (state.preview_elements.clone(), state.preview_arrows.clone())
            }
            _ => (
                self.selected_document_elements_snapshot(document),
                self.selected_arrows_snapshot(document),
            ),
        };
        let selection_frame_padding = self.selection_frame_padding_for_selected_members(
            document,
            &selected_elements,
            &selected_arrows,
        );
        let selection_corner_handle_outset = selection_corner_handle_outset_for_members(
            self.camera().zoom,
            selected_elements.len(),
            selected_arrows.len(),
        );
        let selection_box_visible =
            selection_box_visible_for_members(&selected_elements, &selected_arrows);

        if prioritize_arrow_handles
            && include_selection_handles
            && let Some((arrow_id, arrow)) = single_arrow.clone()
            && let Some(target) = self.arrow_hit_target(document, arrow_id, &arrow, canvas_point)
        {
            return CanvasHit::ArrowHandle(target);
        }

        if include_selection_handles
            && selection_box_visible
            && let Some(bounds) = self.selection_bounds_snapshot(document)
            && let Some(target) = selection_hit_target(
                &bounds,
                single_rect.as_ref(),
                single_text_rect.as_ref(),
                selection_frame_padding,
                selection_corner_handle_outset,
                self.camera().zoom,
                canvas_point,
            )
        {
            return CanvasHit::SelectionHandle(target);
        }

        if include_selection_handles
            && let Some((arrow_id, arrow)) = single_arrow
            && let Some(target) = self.arrow_hit_target(document, arrow_id, &arrow, canvas_point)
        {
            return CanvasHit::ArrowHandle(target);
        }

        if !policy.quick_selection_enabled {
            return CanvasHit::Empty;
        }

        let active_text = self.active_text_draft_existing_id().and_then(|id| {
            self.active_text_draft_text_for_id(id)
                .map(|text| (id, text))
        });
        let hit_tolerance = element_hit_tolerance(self.camera().zoom);
        for id in document.paint_order().iter().rev() {
            let Ok(element) = document.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            let kind = if active_text
                .as_ref()
                .is_some_and(|(active_id, _)| id == active_id)
            {
                let Some((_, text)) = active_text.as_ref() else {
                    continue;
                };
                if text_hit_test(text, canvas_point, hit_tolerance) {
                    ElementKind::Text
                } else {
                    continue;
                }
            } else {
                match &element.data {
                    ElementData::Rectangle(rect)
                        if rectangle_hit_test(rect, canvas_point, hit_tolerance) =>
                    {
                        rect.element_kind()
                    }
                    ElementData::Filter(filter)
                        if filter_hit_test(filter, canvas_point, hit_tolerance) =>
                    {
                        ElementKind::Filter
                    }
                    ElementData::PenFilter(filter)
                        if snow_draw_engine_document::pen_filter_hit_test(
                            filter,
                            canvas_point,
                            hit_tolerance,
                        ) =>
                    {
                        ElementKind::PenFilter
                    }
                    ElementData::Arrow(arrow)
                        if arrow_hit_test(arrow, canvas_point, hit_tolerance) =>
                    {
                        arrow.element_kind()
                    }
                    ElementData::FreeDraw(free_draw)
                        if free_draw_hit_test(free_draw, canvas_point, hit_tolerance) =>
                    {
                        ElementKind::FreeDraw
                    }
                    ElementData::Text(text) if text_hit_test(text, canvas_point, hit_tolerance) => {
                        ElementKind::Text
                    }
                    ElementData::SerialNumber(serial)
                        if serial_number_hit_test(serial, canvas_point, hit_tolerance) =>
                    {
                        ElementKind::SerialNumber
                    }
                    _ => continue,
                }
            };
            if Self::selection_scope_matches_document(document, policy.selection_scope, *id, kind) {
                return CanvasHit::EligibleElement(*id, kind);
            }
        }
        CanvasHit::Empty
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    use snow_draw_engine_core::{
        ColorRgba8, CornerRadii, EngineConfig,
        arrow::{ArrowEndpointEdge, ArrowStrokeStyle, ArrowType},
    };
    use snow_draw_engine_document::{
        ArrowData, CanvasFilterType, ElementMeta, FilterData, PenFilterData, Transaction,
    };
    use snow_draw_engine_interaction::{
        InputEvent, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
    };

    fn opaque_color() -> ColorRgba8 {
        ColorRgba8 {
            r: 1,
            g: 2,
            b: 3,
            a: 255,
        }
    }

    fn test_rectangle(center: Point<f64>) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center,
            width: 100.0,
            height: 100.0,
            rotation: 0.0,
            fill: opaque_color(),
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn test_text(center: Point<f64>) -> TextData {
        TextData {
            center,
            width: 100.0,
            height: 100.0,
            text: "top".to_owned(),
            auto_resize: false,
            ..TextData::default()
        }
    }

    fn test_arrow(points: &[Point<f64>]) -> ArrowData {
        ArrowData::from_global_points(
            points,
            ColorRgba8::default(),
            2.0,
            ArrowStrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("two or more points should create an arrow")
    }

    fn insert_arrow(document: &mut DocumentModel, arrow: ArrowData) -> ElementId {
        let id = document.allocate_element_id();
        let mut transaction = Transaction::new("arrow");
        transaction.insert_arrow(id, ElementMeta::default(), arrow);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn selected_arrow_editor(document: &DocumentModel, arrow_id: ElementId) -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Select).unwrap();
        editor.set_selection_state_with_document(Some(document), vec![arrow_id], Some(arrow_id));
        editor
    }

    fn pointer_down_at(position: Point<f64>) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: PointerEventType::Down,
            device: PointerDevice::Mouse,
            position,
            button: Some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    }

    fn pointer_hover_at(position: Point<f64>) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: PointerEventType::Move,
            device: PointerDevice::Mouse,
            position,
            button: None,
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        })
    }

    #[test]
    fn shape_tool_hit_resolution_passes_through_topmost_text() {
        let mut document = DocumentModel::new();
        let rectangle_id = document.allocate_element_id();
        let text_id = document.allocate_element_id();
        let mut transaction = Transaction::new("overlapping elements");
        transaction.insert_rectangle(
            rectangle_id,
            ElementMeta::default(),
            test_rectangle(Point::new(0.0, 0.0)),
        );
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            test_text(Point::new(0.0, 0.0)),
        );
        document.apply_transaction(transaction).unwrap();

        let editor = Editor::new(EngineConfig::default()).unwrap();
        let hit =
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false);

        assert_eq!(
            hit,
            CanvasHit::EligibleElement(rectangle_id, ElementKind::Rectangle)
        );
    }

    #[test]
    fn rectangle_highlight_tool_resolves_rectangle_highlights() {
        let mut document = DocumentModel::new();
        let highlight_id = document.allocate_element_id();
        let mut transaction = Transaction::new("rectangle highlight");
        transaction.insert_rectangle(
            highlight_id,
            ElementMeta::default(),
            test_rectangle(Point::new(0.0, 0.0))
                .into_highlight(snow_draw_engine_document::HighlightShape::Rectangle),
        );
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor
            .set_active_tool(ActiveTool::RectangleHighlight)
            .unwrap();

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false),
            CanvasHit::EligibleElement(highlight_id, ElementKind::RectangleHighlight)
        );
    }

    #[test]
    fn rectangle_filter_tool_starts_creation_over_an_existing_filter() {
        let mut document = DocumentModel::new();
        let filter_id = document.allocate_element_id();
        let mut transaction = Transaction::new("filter");
        transaction.insert_filter(
            filter_id,
            ElementMeta::default(),
            FilterData {
                center: Point::new(20.0, 30.0),
                width: 100.0,
                height: 60.0,
                ..FilterData::default()
            },
        );
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_quick_selection_disabled_tools(ActiveTool::RectangleFilter.policy_bit());
        editor.set_active_tool(ActiveTool::Filter).unwrap();

        assert_eq!(
            editor.resolve_canvas_hit(
                &document,
                editor.tool_policy(),
                Point::new(20.0, 30.0),
                false
            ),
            CanvasHit::Empty
        );

        editor
            .process_input(&document, pointer_down_at(Point::new(20.0, 30.0)))
            .unwrap();
        assert!(editor.selected_ids().is_empty());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingRectangle(_)
        ));
    }

    #[test]
    fn free_draw_tool_starts_creation_over_an_existing_free_draw() {
        let mut document = DocumentModel::new();
        let free_draw_id = document.allocate_element_id();
        let free_draw = snow_draw_engine_document::FreeDrawData::from_global_vertices(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            vec![snow_draw_engine_core::PathSegmentMode::Curve],
            false,
            snow_draw_engine_document::FreeDrawStyle {
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                fill: ColorRgba8::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                opacity: 1.0,
            },
        )
        .unwrap();
        let mut transaction = Transaction::new("free draw");
        transaction.insert_free_draw(free_draw_id, ElementMeta::default(), free_draw);
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_quick_selection_disabled_tools(ActiveTool::FreeDraw.policy_bit());
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false),
            CanvasHit::Empty
        );

        editor
            .process_input(&document, pointer_down_at(Point::new(0.0, 0.0)))
            .unwrap();
        assert!(editor.selected_ids().is_empty());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingFreeDraw(_)
        ));
    }

    #[test]
    fn select_tool_hover_exposes_free_draw_path_feedback() {
        let mut document = DocumentModel::new();
        let free_draw_id = document.allocate_element_id();
        let free_draw = snow_draw_engine_document::FreeDrawData::from_global_vertices(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            vec![snow_draw_engine_core::PathSegmentMode::Curve],
            false,
            snow_draw_engine_document::FreeDrawStyle {
                stroke: ColorRgba8::default(),
                stroke_width: 2.0,
                stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                fill: ColorRgba8::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                opacity: 1.0,
            },
        )
        .unwrap();
        let mut transaction = Transaction::new("free draw");
        transaction.insert_free_draw(free_draw_id, ElementMeta::default(), free_draw);
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(200, 200).unwrap();
        editor.set_active_tool(ActiveTool::Select).unwrap();

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false),
            CanvasHit::EligibleElement(free_draw_id, ElementKind::FreeDraw)
        );

        editor
            .process_input(&document, pointer_hover_at(Point::new(100.0, 100.0)))
            .unwrap();
        let presentation = editor.presentation_state(&document);
        let hovered_free_draw = presentation
            .hovered_free_draw
            .as_ref()
            .expect("hovered free draw should expose its path geometry");

        assert_eq!(hovered_free_draw.global_vertices().len(), 2);
        assert_eq!(presentation.hovered_rect, None);
    }

    #[test]
    fn pen_filter_tool_starts_creation_over_an_existing_pen_filter() {
        let mut document = DocumentModel::new();
        let pen_filter_id = document.allocate_element_id();
        let pen_filter = PenFilterData::from_global_points(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            CanvasFilterType::Mosaic,
            0.5,
            30.0,
            1.0,
        )
        .unwrap();
        let mut transaction = Transaction::new("pen filter");
        transaction.insert_pen_filter(pen_filter_id, ElementMeta::default(), pen_filter);
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_quick_selection_disabled_tools(ActiveTool::PenFilter.policy_bit());
        editor.set_active_tool(ActiveTool::PenFilter).unwrap();

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false),
            CanvasHit::Empty
        );

        editor
            .process_input(&document, pointer_down_at(Point::new(0.0, 0.0)))
            .unwrap();
        assert!(editor.selected_ids().is_empty());
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingPenFilter(_)
        ));
    }

    #[test]
    fn canvas_hit_uses_active_draft_text_geometry_instead_of_committed_geometry() {
        let mut document = DocumentModel::new();
        let text_id = document.allocate_element_id();
        let mut transaction = Transaction::new("text");
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            test_text(Point::new(0.0, 0.0)),
        );
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::Select).unwrap();
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: test_text(Point::new(200.0, 0.0)),
                },
            )
            .unwrap();

        let draft_hit = editor.resolve_canvas_hit(
            &document,
            editor.tool_policy(),
            Point::new(200.0, 0.0),
            false,
        );
        let committed_hit =
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), false);

        assert_eq!(
            draft_hit,
            CanvasHit::EligibleElement(text_id, ElementKind::Text)
        );
        assert_eq!(committed_hit, CanvasHit::Empty);
    }

    #[test]
    fn two_point_selected_arrow_skips_selection_box_hits_and_keeps_arrow_controls() {
        let mut document = DocumentModel::new();
        let arrow_id = insert_arrow(
            &mut document,
            test_arrow(&[Point::new(-50.0, 0.0), Point::new(50.0, 0.0)]),
        );
        let editor = selected_arrow_editor(&document, arrow_id);
        let bounds = editor.selection_bounds_snapshot(&document).unwrap();
        let zoom = editor.camera().zoom;
        let resize_position = selection_resize_handle_center(
            &bounds,
            0.0,
            selection_corner_handle_outset_for_members(zoom, 0, 1),
            RectCorner::TopLeft,
        );
        let rotation_position = selection_rotation_handle_center(&bounds, 0.0, zoom);

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), resize_position, true),
            CanvasHit::Empty
        );
        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), rotation_position, true),
            CanvasHit::Empty
        );
        assert!(matches!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), Point::new(0.0, 0.0), true,),
            CanvasHit::ArrowHandle(_)
        ));
        assert_eq!(
            editor.resolve_canvas_hit(
                &document,
                editor.tool_policy(),
                Point::new(-50.0, 0.0),
                true,
            ),
            CanvasHit::ArrowHandle(ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start))
        );
    }

    #[test]
    fn multi_point_selected_arrow_keeps_selection_box_hits() {
        let mut document = DocumentModel::new();
        let arrow_id = insert_arrow(
            &mut document,
            test_arrow(&[
                Point::new(-50.0, 0.0),
                Point::new(0.0, 50.0),
                Point::new(50.0, 0.0),
            ]),
        );
        let editor = selected_arrow_editor(&document, arrow_id);
        let bounds = editor.selection_bounds_snapshot(&document).unwrap();
        let resize_position = selection_resize_handle_center(
            &bounds,
            0.0,
            selection_corner_handle_outset_for_members(editor.camera().zoom, 0, 1),
            RectCorner::TopLeft,
        );

        assert_eq!(
            editor.resolve_canvas_hit(&document, editor.tool_policy(), resize_position, true),
            CanvasHit::SelectionHandle(SelectionHitTarget::Resize(ResizeHandle::TopLeft))
        );
    }

    #[test]
    fn shape_tool_treats_unmatched_text_as_empty_canvas() {
        let mut document = DocumentModel::new();
        let text_id = document.allocate_element_id();
        let mut transaction = Transaction::new("text only");
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            test_text(Point::new(0.0, 0.0)),
        );
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let update = editor
            .process_input(&document, pointer_down_at(Point::new(0.0, 0.0)))
            .unwrap();

        assert!(update.interaction.consumed);
        assert_eq!(
            update.interaction.cursor,
            CursorCommand::Set(CursorStyle::Default)
        );
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingRectangle(_)
        ));
    }
}
