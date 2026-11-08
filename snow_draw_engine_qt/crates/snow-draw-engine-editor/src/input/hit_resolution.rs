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
        let labels: std::collections::HashMap<_, _> =
            self.arrow_text_previews(document).into_iter().collect();
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

        let selection_target = if include_selection_handles && selection_box_visible {
            self.selection_bounds_snapshot(document).and_then(|bounds| {
                selection_hit_target(
                    &bounds,
                    single_rect.as_ref(),
                    single_text_rect.as_ref(),
                    selection_frame_padding,
                    selection_corner_handle_outset,
                    self.camera().zoom,
                    canvas_point,
                )
            })
        } else {
            None
        };
        if let Some(target) = selection_target
            && (target != SelectionHitTarget::Move
                || !self
                    .state
                    .selection
                    .ids
                    .iter()
                    .any(|id| document.magnifier(*id).is_ok()))
        {
            return CanvasHit::SelectionHandle(target);
        }

        // Magnifier lenses must be resolved before the generic move area. A selected
        // source/body still moves the whole group once the actual topmost hit is known.
        let element_hit = |id, kind| {
            if selection_target == Some(SelectionHitTarget::Move)
                && self.state.selection.ids.len() > 1
                && self.state.selection.contains(id)
            {
                CanvasHit::SelectionHandle(SelectionHitTarget::Move)
            } else {
                CanvasHit::EligibleElement(id, kind)
            }
        };

        if include_selection_handles
            && let Some((arrow_id, arrow)) = single_arrow
            && let Some(target) = self.arrow_hit_target(document, arrow_id, &arrow, canvas_point)
        {
            return CanvasHit::ArrowHandle(target);
        }

        if include_selection_handles {
            for id in &self.state.selection.ids {
                if let Some(value) = self.magnifier_snapshot(document, *id) {
                    let grip = crate::magnifier_move_grip(value, self.camera().zoom);
                    let half = 9.0 / self.camera().zoom.max(0.0001);
                    if (canvas_point.x - grip.x).abs() <= half
                        && (canvas_point.y - grip.y).abs() <= half
                    {
                        return CanvasHit::MagnifierLens(*id);
                    }
                }
            }
        }

        if !policy.quick_selection_enabled {
            if include_selection_handles {
                for id in &self.state.selection.ids {
                    if let Some(value) = self.magnifier_snapshot(document, *id) {
                        let tolerance = element_hit_tolerance(self.camera().zoom);
                        if rectangle_hit_test(&value.source_rect(), canvas_point, tolerance) {
                            return element_hit(*id, ElementKind::Magnifier);
                        }
                        if rectangle_hit_test(&value.magnified_rect(), canvas_point, tolerance) {
                            return CanvasHit::MagnifierLens(*id);
                        }
                    }
                }
            }
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
            if !element.meta.visible || element.data.is_background_restore() {
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
                    ElementData::SpeechBubble(_) => {
                        let Some(value) = self.speech_bubble_display_snapshot(document, *id) else {
                            continue;
                        };
                        if !snow_draw_engine_document::speech_bubble_hit_test(
                            &value,
                            canvas_point,
                            hit_tolerance,
                        ) {
                            continue;
                        }
                        ElementKind::SpeechBubble
                    }
                    ElementData::Magnifier(value) => {
                        if !Self::selection_scope_matches_document(
                            document,
                            policy.selection_scope,
                            *id,
                            ElementKind::Magnifier,
                        ) {
                            continue;
                        }
                        let value = self.magnifier_snapshot(document, *id).unwrap_or(*value);
                        if rectangle_hit_test(&value.source_rect(), canvas_point, hit_tolerance) {
                            return element_hit(*id, ElementKind::Magnifier);
                        }
                        if rectangle_hit_test(&value.magnified_rect(), canvas_point, hit_tolerance)
                        {
                            return if include_selection_handles {
                                CanvasHit::MagnifierLens(*id)
                            } else {
                                CanvasHit::EligibleElement(*id, ElementKind::Magnifier)
                            };
                        }
                        continue;
                    }
                    ElementData::Filter(filter)
                        if filter_hit_test(filter, canvas_point, hit_tolerance) =>
                    {
                        element.data.kind()
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
                    ElementData::Text(text) => {
                        let bubble_text = self.speech_bubble_text_display_snapshot(document, *id);
                        let text = bubble_text
                            .as_ref()
                            .or_else(|| labels.get(id))
                            .unwrap_or(text);
                        if !text_hit_test(text, canvas_point, hit_tolerance) {
                            continue;
                        }
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
            let owner = document
                .arrow_id_for_text(*id)
                .or_else(|| document.speech_bubble_id_for_text(*id));
            let (hit_id, kind) = owner.map_or((*id, kind), |id| {
                (
                    id,
                    document
                        .element(id)
                        .map_or(ElementKind::Arrow, |element| element.data.kind()),
                )
            });
            if Self::selection_scope_matches_document(
                document,
                policy.selection_scope,
                hit_id,
                kind,
            ) {
                return element_hit(hit_id, kind);
            }
        }
        if selection_target == Some(SelectionHitTarget::Move) {
            return CanvasHit::SelectionHandle(SelectionHitTarget::Move);
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
        arrow::{ArrowEndpointEdge, ArrowType, StrokeStyle},
    };
    use snow_draw_engine_document::{
        ArrowData, CanvasFilterType, ElementMeta, FillStyle, FilterData, PenFilterData, Transaction,
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
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
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
            text: "top".to_owned(),
            auto_resize: false,
            layout: snow_draw_engine_document::TextLayoutSize::new(100.0, 100.0),
            ..TextData::default()
        }
    }

    fn test_arrow(points: &[Point<f64>]) -> ArrowData {
        ArrowData::from_global_points(
            points,
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
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
            pressure: None,
            timestamp_ms: 0,
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
            pressure: None,
            timestamp_ms: 0,
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
    fn arrow_control_hover_tracks_visible_handles_without_repainting_the_scene() {
        for arrow_type in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
            for line in [false, true] {
                let mut document = DocumentModel::new();
                let points = if arrow_type == ArrowType::Elbow {
                    vec![
                        Point::new(-160.0, -80.0),
                        Point::new(0.0, -80.0),
                        Point::new(0.0, 80.0),
                        Point::new(160.0, 80.0),
                    ]
                } else {
                    vec![Point::new(-160.0, -80.0), Point::new(160.0, 80.0)]
                };
                let mut arrow = ArrowData::from_global_points(
                    &points,
                    opaque_color(),
                    2.0,
                    StrokeStyle::Solid,
                    arrow_type,
                    None,
                    None,
                )
                .unwrap();
                if line {
                    arrow = arrow.into_line(opaque_color(), FillStyle::Solid);
                }
                let id = insert_arrow(&mut document, arrow);
                let mut editor = selected_arrow_editor(&document, id);
                editor.set_surface_size(800, 600).unwrap();
                for zoom in [0.5, 1.0, 2.0, 4.0] {
                    editor
                        .set_camera(Camera {
                            zoom,
                            ..editor.camera()
                        })
                        .unwrap();
                    let handles = editor.presentation_state(&document).arrow_handles;
                    assert!(
                        handles
                            .iter()
                            .any(|handle| handle.kind == ArrowHandleKind::Segment)
                    );
                    for handle in handles {
                        let view_point = snow_draw_engine_core::canvas_to_view(
                            handle.center,
                            &editor.camera(),
                            editor.surface_size(),
                        );
                        let scene_revision = editor.scene_input_revision();
                        let update = editor
                            .process_input(&document, pointer_hover_at(view_point))
                            .unwrap();
                        assert_eq!(
                            update.interaction.cursor,
                            CursorCommand::Set(CursorStyle::Grab)
                        );
                        let hovered = editor
                            .presentation_state(&document)
                            .arrow_handles
                            .into_iter()
                            .filter(|handle| handle.hovered)
                            .collect::<Vec<_>>();
                        assert_eq!(hovered.len(), 1, "{arrow_type:?}, line={line}, zoom={zoom}");
                        assert_eq!(hovered[0].center, handle.center);
                        assert_eq!(hovered[0].kind, handle.kind);
                        assert_eq!(editor.scene_input_revision(), scene_revision);
                        let overlay_revision = editor.overlay_input_revision();
                        editor
                            .process_input(
                                &document,
                                pointer_hover_at(Point::new(view_point.x + 0.25, view_point.y)),
                            )
                            .unwrap();
                        assert_eq!(editor.overlay_input_revision(), overlay_revision);
                    }
                    editor
                        .process_input(&document, pointer_hover_at(Point::new(-1000.0, -1000.0)))
                        .unwrap();
                    assert!(
                        editor
                            .presentation_state(&document)
                            .arrow_handles
                            .iter()
                            .all(|handle| !handle.hovered)
                    );
                }
            }
        }
    }

    #[test]
    fn arrow_control_hover_clears_on_drag_leave_focus_tool_and_selection_changes() {
        for action in 0..6 {
            let mut document = DocumentModel::new();
            let id = insert_arrow(
                &mut document,
                test_arrow(&[Point::new(-100.0, 0.0), Point::new(100.0, 0.0)]),
            );
            let mut editor = selected_arrow_editor(&document, id);
            editor.set_surface_size(400, 300).unwrap();
            let point = Point::new(200.0, 150.0);
            editor
                .process_input(&document, pointer_hover_at(point))
                .unwrap();
            assert!(
                editor
                    .presentation_state(&document)
                    .arrow_handles
                    .iter()
                    .any(|handle| handle.hovered)
            );
            let revision = editor.overlay_input_revision();
            match action {
                0 => {
                    editor
                        .process_input(&document, pointer_down_at(point))
                        .unwrap();
                }
                1 | 2 => {
                    let InputEvent::Pointer(mut pointer) = pointer_hover_at(point) else {
                        unreachable!()
                    };
                    pointer.event_type = if action == 1 {
                        PointerEventType::Leave
                    } else {
                        PointerEventType::Cancel
                    };
                    editor
                        .process_input(&document, InputEvent::Pointer(pointer))
                        .unwrap();
                }
                3 => {
                    editor
                        .process_input(&document, InputEvent::FocusLost)
                        .unwrap();
                }
                4 => {
                    editor.set_active_tool(ActiveTool::Arrow).unwrap();
                }
                _ => {
                    editor.clear_selection();
                }
            }
            assert!(editor.overlay_input_revision() > revision);
            assert!(
                editor
                    .presentation_state(&document)
                    .arrow_handles
                    .iter()
                    .all(|handle| !handle.hovered)
            );
        }
    }

    #[test]
    fn bent_line_control_hover_highlights_only_the_hit_vertex_or_segment() {
        let mut document = DocumentModel::new();
        let arrow = test_arrow(&[
            Point::new(-150.0, 0.0),
            Point::new(0.0, 100.0),
            Point::new(150.0, 0.0),
        ])
        .into_line(opaque_color(), FillStyle::Solid);
        let id = insert_arrow(&mut document, arrow);
        let mut editor = selected_arrow_editor(&document, id);
        editor.set_surface_size(800, 600).unwrap();
        for center in [
            Point::new(0.0, 100.0),
            Point::new(-75.0, 50.0),
            Point::new(75.0, 50.0),
        ] {
            let point = snow_draw_engine_core::canvas_to_view(
                center,
                &editor.camera(),
                editor.surface_size(),
            );
            editor
                .process_input(&document, pointer_hover_at(point))
                .unwrap();
            let hovered = editor
                .presentation_state(&document)
                .arrow_handles
                .into_iter()
                .filter(|handle| handle.hovered)
                .collect::<Vec<_>>();
            assert_eq!(hovered.len(), 1);
            assert_eq!(hovered[0].center, center);
        }
    }

    #[test]
    fn angle_arc_controls_keep_point_priority_and_separate_arc_drag_from_arm_move() {
        use snow_draw_engine_document::{angle_arc_control_point, angle_geometry};
        let mut document = DocumentModel::new();
        let mut arrow = test_arrow(&[
            Point::new(100.0, 0.0),
            Point::new(0.0, 0.0),
            Point::new(0.0, -100.0),
        ]);
        crate::AngleStyle::default().apply_to_arrow(&mut arrow);
        let id = insert_arrow(&mut document, arrow.clone());
        let mut editor = selected_arrow_editor(&document, id);
        let control = angle_arc_control_point(&arrow).unwrap();
        assert_eq!(
            editor.arrow_hit_target(&document, id, &arrow, control),
            Some(ArrowHitTarget::AngleArc)
        );
        let arc_body = Point::new(
            40.5 * 20.0_f64.to_radians().cos(),
            -40.5 * 20.0_f64.to_radians().sin(),
        );
        assert_eq!(
            editor.arrow_hit_target(&document, id, &arrow, arc_body),
            Some(ArrowHitTarget::AngleArc)
        );
        editor.set_surface_size(800, 600).unwrap();
        for (point, highlighted) in [(control, true), (arc_body, false)] {
            let view_point = snow_draw_engine_core::canvas_to_view(
                point,
                &editor.camera(),
                editor.surface_size(),
            );
            editor
                .process_input(&document, pointer_hover_at(view_point))
                .unwrap();
            assert_eq!(
                editor
                    .presentation_state(&document)
                    .arrow_handles
                    .iter()
                    .any(|handle| handle.hovered),
                highlighted
            );
        }
        assert_eq!(
            editor.arrow_hit_target(&document, id, &arrow, Point::new(80.0, 0.0)),
            Some(ArrowHitTarget::Move)
        );
        arrow.angle.as_mut().unwrap().arc_radius = Some(1.0);
        assert_eq!(
            editor.arrow_hit_target(
                &document,
                id,
                &arrow,
                angle_arc_control_point(&arrow).unwrap()
            ),
            Some(ArrowHitTarget::Point(1)),
            "vertex stays editable when the arc handle overlaps it"
        );
        arrow.angle.as_mut().unwrap().arc_radius = Some(100.0);
        assert_eq!(
            editor.arrow_hit_target(&document, id, &arrow, Point::new(100.0, 0.0)),
            Some(ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start))
        );
        let handles = editor.arrow_handle_states(&document, id, &arrow);
        assert_eq!(
            handles
                .iter()
                .filter(|handle| handle.kind == crate::ArrowHandleKind::AngleArc)
                .count(),
            1
        );
        let mut zero = arrow.clone();
        zero.points[2] = zero.points[0];
        assert!(angle_arc_control_point(&zero).is_none());
        zero.angle.as_mut().unwrap().full_turn = true;
        let geometry = angle_geometry(&zero).unwrap();
        assert!(
            (angle_arc_control_point(&zero).unwrap().x - geometry.vertex.x + geometry.radius).abs()
                < 1e-9
        );
    }

    #[test]
    fn selected_arrow_controls_win_over_an_overlapping_bound_label() {
        let mut document = DocumentModel::new();
        let arrow_id = document.allocate_element_id();
        let text_id = document.allocate_element_id();
        let mut arrow = test_arrow(&[
            Point::new(-100.0, 0.0),
            Point::new(0.0, 0.0),
            Point::new(100.0, 0.0),
        ]);
        arrow.text_element_id = Some(text_id);
        let mut transaction = Transaction::new("arrow with overlapping label");
        transaction.insert_arrow(arrow_id, ElementMeta::default(), arrow);
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            TextData {
                layout: snow_draw_engine_document::TextLayoutSize::new(300.0, 60.0),
                ..test_text(Point::new(0.0, 0.0))
            },
        );
        document.apply_transaction(transaction).unwrap();
        let editor = selected_arrow_editor(&document, arrow_id);
        for (point, expected) in [
            (
                Point::new(-100.0, 0.0),
                ArrowHitTarget::Endpoint(ArrowEndpointEdge::Start),
            ),
            (Point::new(0.0, 0.0), ArrowHitTarget::Point(1)),
            (Point::new(-75.0, 0.0), ArrowHitTarget::Label),
        ] {
            assert_eq!(
                editor.resolve_canvas_hit(&document, editor.tool_policy(), point, true),
                CanvasHit::ArrowHandle(expected),
                "hit at {point:?}"
            );
        }
        let mut editor = selected_arrow_editor(&document, arrow_id);
        editor.set_surface_size(400, 300).unwrap();
        editor
            .process_input(&document, pointer_down_at(Point::new(100.0, 150.0)))
            .unwrap();
        assert!(matches!(
            editor.state.interaction,
            InteractionState::EditingArrow(EditArrowState {
                mode: ArrowEditMode::Endpoint(ArrowEndpointEdge::Start),
                ..
            })
        ));

        let mut segment_document = DocumentModel::new();
        let segment_arrow_id = segment_document.allocate_element_id();
        let segment_text_id = segment_document.allocate_element_id();
        let mut segment_arrow = test_arrow(&[Point::new(-100.0, 0.0), Point::new(100.0, 0.0)]);
        segment_arrow.text_element_id = Some(segment_text_id);
        let mut transaction = Transaction::new("two-point arrow with overlapping label");
        transaction.insert_arrow(segment_arrow_id, ElementMeta::default(), segment_arrow);
        transaction.insert_text(
            segment_text_id,
            ElementMeta::default(),
            TextData {
                layout: snow_draw_engine_document::TextLayoutSize::new(300.0, 60.0),
                ..test_text(Point::new(0.0, 0.0))
            },
        );
        segment_document.apply_transaction(transaction).unwrap();
        let editor = selected_arrow_editor(&segment_document, segment_arrow_id);
        assert_eq!(
            editor.resolve_canvas_hit(
                &segment_document,
                editor.tool_policy(),
                Point::new(0.0, 0.0),
                true
            ),
            CanvasHit::ArrowHandle(ArrowHitTarget::Segment(1))
        );
        assert!(
            editor
                .arrow_handle_states(
                    &segment_document,
                    segment_arrow_id,
                    segment_document.arrow(segment_arrow_id).unwrap()
                )
                .iter()
                .any(|handle| handle.kind == ArrowHandleKind::Segment)
        );
        assert_eq!(
            editor.resolve_canvas_hit(
                &segment_document,
                editor.tool_policy(),
                Point::new(30.0, 0.0),
                true
            ),
            CanvasHit::ArrowHandle(ArrowHitTarget::Label)
        );
        let mut editor = selected_arrow_editor(&segment_document, segment_arrow_id);
        editor.set_surface_size(400, 300).unwrap();
        editor
            .process_input(&segment_document, pointer_down_at(Point::new(200.0, 150.0)))
            .unwrap();
        assert!(matches!(
            editor.state.interaction,
            InteractionState::EditingArrow(EditArrowState {
                mode: ArrowEditMode::Segment(1),
                ..
            })
        ));
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
                brush_mode: Default::default(),
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
                brush_mode: Default::default(),
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
            CursorCommand::Set(CursorStyle::Crosshair)
        );
        assert!(matches!(
            editor.state.interaction,
            InteractionState::CreatingRectangle(_)
        ));
    }
}
