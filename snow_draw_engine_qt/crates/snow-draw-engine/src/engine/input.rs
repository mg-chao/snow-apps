use super::*;

impl Engine {
    pub fn hit_quick_selection_at(
        &self,
        id: ViewportId,
        point: Point<f64>,
        button: snow_draw_engine_interaction::PointerButton,
    ) -> Result<Option<ElementId>, ErrorCode> {
        let view = &self.viewport_slot(id)?.view;
        Ok(self
            .editor
            .hit_quick_selection_at(&self.model, view, point, button))
    }

    pub fn process_pointer_move_batch_with_viewport_changes(
        &mut self,
        id: ViewportId,
        events: &[InputEvent],
    ) -> Result<InputUpdate, ErrorCode> {
        if events.is_empty()
            || events.iter().any(|event| {
                !matches!(
                    event,
                    InputEvent::Pointer(pointer)
                        if pointer.event_type
                            == snow_draw_engine_interaction::PointerEventType::Move
                )
            })
        {
            return Err(ErrorCode::InvalidArgument);
        }

        let before_scene_revision = self.editor.scene_input_revision();
        let before_overlay_revision = self.editor.overlay_input_revision();
        let before_view = self.viewport_slot(id)?.view;
        let mut interaction = InteractionOutput::default();
        for event in events.iter().copied() {
            let update = {
                let model = &self.model;
                let slot = self.viewports.get_mut(&id).ok_or(ErrorCode::NotFound)?;
                self.editor.process_input(model, &mut slot.view, event)?
            };
            if update.command.is_some() {
                return Err(ErrorCode::InvalidArgument);
            }
            interaction = update.interaction;
        }

        let changed_viewports = if self.editor.scene_input_revision() != before_scene_revision
            || self.editor.overlay_input_revision() != before_overlay_revision
        {
            self.refresh_all_viewports()?.changed_viewports
        } else if self.viewport_slot(id)?.view != before_view {
            self.refresh_single_viewport(id)?.changed_viewports
        } else {
            Vec::new()
        };
        Ok(InputUpdate {
            interaction,
            changed_viewports,
        })
    }

    pub fn process_input(
        &mut self,
        id: ViewportId,
        event: InputEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        self.process_input_with_viewport_changes(id, event)
            .map(|update| update.interaction)
    }

    pub fn process_input_with_viewport_changes(
        &mut self,
        id: ViewportId,
        event: InputEvent,
    ) -> Result<InputUpdate, ErrorCode> {
        let before_scene_revision = self.editor.scene_input_revision();
        let before_overlay_revision = self.editor.overlay_input_revision();
        let before_view = self.viewport_slot(id)?.view;
        if matches!(event, InputEvent::Pointer(pointer) if matches!(pointer.event_type, snow_draw_engine_interaction::PointerEventType::Down | snow_draw_engine_interaction::PointerEventType::DoubleClick))
            || matches!(event, InputEvent::Key(_) | InputEvent::FocusLost)
        {
            self.history.break_angle_wheel_coalescing();
        }
        let update = {
            let model = &self.model;
            let slot = self.viewports.get_mut(&id).ok_or(ErrorCode::NotFound)?;
            self.editor.process_input(model, &mut slot.view, event)?
        };
        if let Some(command) = update.command {
            let result = self.apply_editor_command(id, command)?;
            Ok(InputUpdate {
                interaction: update.interaction,
                changed_viewports: result.changed_viewports,
            })
        } else if self.editor.scene_input_revision() != before_scene_revision
            || self.editor.overlay_input_revision() != before_overlay_revision
        {
            let result = self.refresh_all_viewports()?;
            Ok(InputUpdate {
                interaction: update.interaction,
                changed_viewports: result.changed_viewports,
            })
        } else if self.viewport_slot(id)?.view != before_view {
            let result = self.refresh_single_viewport(id)?;
            Ok(InputUpdate {
                interaction: update.interaction,
                changed_viewports: result.changed_viewports,
            })
        } else {
            Ok(InputUpdate {
                interaction: update.interaction,
                changed_viewports: Vec::new(),
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_interaction::{
        Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
    };

    fn pointer(event_type: PointerEventType, x: f64, y: f64) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type,
            device: PointerDevice::Mouse,
            position: Point::new(x, y),
            button: (event_type == PointerEventType::Down).then_some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    }

    #[test]
    fn right_quick_selection_query_uses_requested_viewport_zoom_without_mutation() {
        let mut engine = Engine::default();
        let first = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine.set_viewport_surface_size(first, 200, 200).unwrap();
        engine
            .set_viewport_active_tool(first, ActiveTool::Shape)
            .unwrap();
        for (kind, x, y) in [
            (PointerEventType::Down, 80.0, 80.0),
            (PointerEventType::Move, 120.0, 120.0),
            (PointerEventType::Up, 120.0, 120.0),
        ] {
            engine.process_input(first, pointer(kind, x, y)).unwrap();
        }
        let second = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine.set_viewport_surface_size(second, 200, 200).unwrap();
        engine
            .set_viewport_camera(
                second,
                Camera {
                    zoom: 4.0,
                    ..Camera::default()
                },
            )
            .unwrap();
        engine
            .set_quick_selection_disabled_tools(ActiveTool::Shape.policy_bit())
            .unwrap();
        let selection = engine.selected_ids();
        let point = Point::new(0.0, -24.0);
        assert!(
            engine
                .hit_quick_selection_at(first, point, PointerButton::Secondary)
                .unwrap()
                .is_some()
        );
        assert!(
            engine
                .hit_quick_selection_at(second, point, PointerButton::Secondary)
                .unwrap()
                .is_none()
        );
        assert!(
            engine
                .hit_quick_selection_at(first, point, PointerButton::Primary)
                .unwrap()
                .is_none()
        );
        assert_eq!(engine.selected_ids(), selection);
    }

    #[test]
    fn pointer_move_batch_refreshes_the_composer_once() {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine
            .set_viewport_surface_size(viewport, 800, 600)
            .unwrap();
        engine
            .set_viewport_active_tool(viewport, ActiveTool::FreeDraw)
            .unwrap();
        engine
            .process_input_with_viewport_changes(
                viewport,
                pointer(PointerEventType::Down, 100.0, 100.0),
            )
            .unwrap();
        let before = engine
            .viewport_slot(viewport)
            .unwrap()
            .composer
            .current_cursor();
        let moves = (1..=32)
            .map(|index| pointer(PointerEventType::Move, 100.0 + index as f64, 120.0))
            .collect::<Vec<_>>();

        let update = engine
            .process_pointer_move_batch_with_viewport_changes(viewport, &moves)
            .unwrap();
        let after = engine
            .viewport_slot(viewport)
            .unwrap()
            .composer
            .current_cursor();

        assert_eq!(update.changed_viewports, vec![viewport]);
        assert_eq!(after.scene_revision.0, before.scene_revision.0 + 1);
    }

    #[test]
    fn pen_filter_batch_refreshes_once_and_commits_simplified_geometry() {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine
            .set_viewport_surface_size(viewport, 800, 600)
            .unwrap();
        engine
            .set_viewport_active_tool(viewport, ActiveTool::PenFilter)
            .unwrap();
        engine
            .process_input_with_viewport_changes(
                viewport,
                pointer(PointerEventType::Down, 100.0, 100.0),
            )
            .unwrap();
        let before = engine
            .viewport_slot(viewport)
            .unwrap()
            .composer
            .current_cursor();
        let moves = (1..=128)
            .map(|index| pointer(PointerEventType::Move, 100.0 + index as f64, 120.0))
            .collect::<Vec<_>>();

        engine
            .process_pointer_move_batch_with_viewport_changes(viewport, &moves)
            .unwrap();
        let after = engine
            .viewport_slot(viewport)
            .unwrap()
            .composer
            .current_cursor();
        assert_eq!(after.scene_revision.0, before.scene_revision.0 + 1);

        engine
            .process_input_with_viewport_changes(
                viewport,
                pointer(PointerEventType::Up, 240.0, 120.0),
            )
            .unwrap();
        let id = engine.model.paint_order()[0];
        let points = engine.model.pen_filter(id).unwrap().global_points();
        assert_eq!(points[0], Point::new(-300.0, -200.0));
        assert_eq!(points.last(), Some(&Point::new(-160.0, -180.0)));
        assert!(
            points.len() <= 4,
            "straight input should collapse to its turns"
        );
    }

    #[test]
    fn pointer_move_batch_rejects_other_events_before_mutation() {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        let before = engine.editor.snapshot();

        assert_eq!(
            engine.process_pointer_move_batch_with_viewport_changes(
                viewport,
                &[pointer(PointerEventType::Down, 10.0, 10.0)],
            ),
            Err(ErrorCode::InvalidArgument)
        );
        assert_eq!(engine.editor.snapshot(), before);
    }
}
