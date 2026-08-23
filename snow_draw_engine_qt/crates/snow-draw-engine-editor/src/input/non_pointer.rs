use super::*;

impl Editor {
    pub(super) fn process_wheel_event(
        &mut self,
        document: &DocumentModel,
        event: WheelEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if self.surface_size().width == 0 || self.surface_size().height == 0 {
            return Ok(InteractionOutput::default());
        }

        let delta_y = match event.delta_kind {
            WheelDeltaKind::Pixel => event.delta.y,
            WheelDeltaKind::Angle => event.delta.y / 120.0 * 100.0,
        };
        if delta_y == 0.0 {
            return Ok(InteractionOutput::default());
        }

        if !event.modifiers.ctrl
            && !event.modifiers.shift
            && matches!(
                self.state.active_tool,
                ActiveTool::RectangleFilter | ActiveTool::PenFilter
            )
        {
            let mut style = self.filter_style(document);
            let properties = if self.state.active_tool == ActiveTool::PenFilter {
                style.stroke_width = (style.stroke_width + delta_y.signum()).clamp(1.0, 72.0);
                FILTER_STYLE_PROPERTY_STROKE_WIDTH
            } else {
                if matches!(
                    style.filter_type,
                    snow_draw_engine_document::CanvasFilterType::Grayscale
                        | snow_draw_engine_document::CanvasFilterType::Inversion
                ) {
                    return Ok(InteractionOutput::default());
                }
                style.strength = (style.strength + delta_y.signum() * 0.01).clamp(0.0, 1.0);
                FILTER_STYLE_PROPERTY_STRENGTH
            };
            if let Some(command) = self.set_filter_style(document, style, properties)? {
                self.queue_command(command);
            }
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::NoChange,
            });
        }

        if !event.modifiers.ctrl
            && !event.modifiers.shift
            && matches!(self.state.active_tool, ActiveTool::Text)
        {
            return Ok(InteractionOutput::default());
        }

        if !event.modifiers.ctrl
            && !event.modifiers.shift
            && matches!(self.state.active_tool, ActiveTool::SerialNumber)
        {
            let increase = delta_y < 0.0;
            if let Some(command) = self.adjust_font_size_step(document, increase, &[])? {
                self.queue_command(command);
            }
            return Ok(InteractionOutput {
                consumed: true,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::NoChange,
            });
        }

        let current = self.camera();
        let scale = (-delta_y * self.config.wheel_zoom_sensitivity).exp();
        let next_zoom = (current.zoom * scale).clamp(self.config.min_zoom, self.config.max_zoom);
        if next_zoom == current.zoom {
            return Ok(InteractionOutput::default());
        }

        let next_center = match self.config.zoom_focus {
            ZoomFocus::Center => current.center,
            ZoomFocus::Pointer => {
                let anchor = view_to_canvas(event.position, &current, self.surface_size());
                Point {
                    x: anchor.x
                        - (event.position.x - self.surface_size().width as f64 / 2.0) / next_zoom,
                    y: anchor.y
                        - (event.position.y - self.surface_size().height as f64 / 2.0) / next_zoom,
                }
            }
        };
        self.set_camera(Camera {
            center: next_center,
            zoom: next_zoom,
        })?;

        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::NoChange,
        })
    }

    pub(super) fn process_key_event(
        &mut self,
        document: &DocumentModel,
        event: KeyEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if event.event_type != KeyEventType::KeyDown || event.repeat {
            return Ok(InteractionOutput::default());
        }

        let handled = match event.key_code {
            KeyCode::Character('z') | KeyCode::Character('Z') if event.modifiers.ctrl => {
                if event.modifiers.shift {
                    self.queue_command(EditorCommand::Redo);
                } else {
                    self.queue_command(EditorCommand::Undo);
                }
                true
            }
            KeyCode::Escape => {
                self.cancel_interaction();
                true
            }
            KeyCode::Backspace | KeyCode::Delete => {
                if let Some(command) = self.delete_selected(document)? {
                    self.queue_command(command);
                }
                true
            }
            _ => false,
        };

        Ok(InteractionOutput {
            consumed: handled,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::NoChange,
        })
    }

    pub(super) fn handle_focus_lost(
        &mut self,
        _document: &DocumentModel,
    ) -> Result<InteractionOutput, ErrorCode> {
        let had_interaction = !matches!(&self.state.interaction, InteractionState::Idle);
        self.cancel_interaction();
        self.clear_stroke_cursor_state();
        self.set_hovered_element(None);
        Ok(InteractionOutput {
            consumed: had_interaction,
            capture: if had_interaction {
                self.release_capture_command()
            } else {
                PointerCaptureCommand::NoChange
            },
            cursor: CursorCommand::Set(CursorStyle::Default),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::Vector2;
    use snow_draw_engine_document::CanvasFilterType;
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn wheel(delta_y: f64) -> InputEvent {
        InputEvent::Wheel(WheelEvent {
            position: Point::new(50.0, 50.0),
            delta: Vector2 { x: 0.0, y: delta_y },
            delta_kind: WheelDeltaKind::Angle,
            modifiers: Modifiers::default(),
        })
    }

    fn editor_for(tool: ActiveTool) -> Editor {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(100, 100).unwrap();
        editor.set_active_tool(tool).unwrap();
        editor
    }

    #[test]
    fn tool_selection_is_owned_by_the_host_application() {
        let document = DocumentModel::new();
        let mut editor = editor_for(ActiveTool::FreeDraw);
        let enter = InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: PointerEventType::Enter,
            device: PointerDevice::Mouse,
            position: Point::new(50.0, 50.0),
            button: None,
            buttons: PointerButtons::default(),
            modifiers: Modifiers::default(),
        });
        let enter_update = editor.process_input(&document, enter).unwrap();
        assert_eq!(
            enter_update.interaction.cursor,
            CursorCommand::Set(CursorStyle::Hidden)
        );

        let character = InputEvent::Key(KeyEvent {
            event_type: KeyEventType::KeyDown,
            key_code: KeyCode::Character('v'),
            modifiers: Modifiers::default(),
            repeat: false,
        });
        let update = editor.process_input(&document, character).unwrap();

        assert_eq!(editor.active_tool(), ActiveTool::FreeDraw);
        assert!(!update.interaction.consumed);
        assert_eq!(update.interaction.cursor, CursorCommand::NoChange);
    }

    #[test]
    fn rectangle_filter_wheel_steps_only_strength_and_skips_disabled_types() {
        let document = DocumentModel::new();
        let mut editor = editor_for(ActiveTool::RectangleFilter);

        let update = editor.process_input(&document, wheel(120.0)).unwrap();
        assert!(update.interaction.consumed);
        assert!(update.command.is_none());
        assert_eq!(editor.filter_style(&document).strength, 0.51);
        assert_eq!(editor.filter_style(&document).stroke_width, 2.0);

        let mut grayscale = editor.filter_style(&document);
        grayscale.filter_type = CanvasFilterType::Grayscale;
        editor
            .set_filter_style(&document, grayscale, FILTER_STYLE_PROPERTY_TYPE)
            .unwrap();
        let before = editor.filter_style(&document);
        let update = editor.process_input(&document, wheel(-120.0)).unwrap();
        assert!(!update.interaction.consumed);
        assert_eq!(editor.filter_style(&document), before);
    }

    #[test]
    fn pen_filter_wheel_steps_only_width_for_every_filter_type_and_clamps() {
        let document = DocumentModel::new();
        let mut editor = editor_for(ActiveTool::PenFilter);
        let original = editor.filter_style(&document);
        assert_eq!(original.stroke_width, 30.0);

        let update = editor.process_input(&document, wheel(120.0)).unwrap();
        assert!(update.interaction.consumed);
        let stepped = editor.filter_style(&document);
        assert_eq!(stepped.stroke_width, 31.0);
        assert_eq!(stepped.filter_type, original.filter_type);
        assert_eq!(stepped.strength, original.strength);

        let mut grayscale = stepped;
        grayscale.filter_type = CanvasFilterType::Grayscale;
        grayscale.stroke_width = 72.0;
        editor
            .set_filter_style(
                &document,
                grayscale,
                FILTER_STYLE_PROPERTY_TYPE | FILTER_STYLE_PROPERTY_STROKE_WIDTH,
            )
            .unwrap();
        let update = editor.process_input(&document, wheel(120.0)).unwrap();
        assert!(update.interaction.consumed);
        assert_eq!(editor.filter_style(&document).stroke_width, 72.0);

        editor.process_input(&document, wheel(-120.0)).unwrap();
        let stepped_down = editor.filter_style(&document);
        assert_eq!(stepped_down.stroke_width, 71.0);
        assert_eq!(stepped_down.filter_type, CanvasFilterType::Grayscale);
        assert_eq!(stepped_down.strength, original.strength);
    }
}
