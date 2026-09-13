use super::*;

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct AutoFilterInteraction {
    start: Option<(u32, Point<f64>, Point<f64>, u64)>,
    hover: Option<Point<f64>>,
    hits: Vec<u64>,
}

impl Editor {
    pub(crate) fn process_auto_filter_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let mut output = InteractionOutput {
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
            ..InteractionOutput::default()
        };
        match event.event_type {
            PointerEventType::Down if event.button == Some(PointerButton::Primary) => {
                self.state.auto_filter.start = Some((
                    event.pointer_id,
                    event.position,
                    point,
                    document.auto_filter_generation(),
                ));
                self.state.auto_filter.hover = Some(point);
                self.state.auto_filter.hits.clear();
                output.capture = PointerCaptureCommand::Capture(event.pointer_id);
                output.consumed = true;
            }
            PointerEventType::Move | PointerEventType::Enter | PointerEventType::Up => {
                self.state.auto_filter.hover = Some(point);
                if let Some((pointer, start_view, start, generation)) = self.state.auto_filter.start
                {
                    if pointer != event.pointer_id {
                        return Ok(output);
                    }
                    if generation != document.auto_filter_generation() {
                        self.cancel_interaction();
                        output.capture = PointerCaptureCommand::Release;
                        return Ok(output);
                    }
                    let dragging = (event.position.x - start_view.x)
                        .hypot(event.position.y - start_view.y)
                        >= POINTER_DRAG_THRESHOLD;
                    self.state.auto_filter.hits =
                        if let Some(record) = document.auto_filter_regions() {
                            if dragging {
                                let bounds = DrawRect::new(
                                    start.x.min(point.x),
                                    start.y.min(point.y),
                                    start.x.max(point.x),
                                    start.y.max(point.y),
                                );
                                record
                                    .regions
                                    .iter()
                                    .filter(|r| {
                                        r.bounds.min_x < bounds.max_x
                                            && r.bounds.max_x > bounds.min_x
                                            && r.bounds.min_y < bounds.max_y
                                            && r.bounds.max_y > bounds.min_y
                                    })
                                    .map(|r| r.id)
                                    .collect()
                            } else {
                                record
                                    .region_at(point)
                                    .map(|r| vec![r.id])
                                    .unwrap_or_default()
                            }
                        } else {
                            Vec::new()
                        };
                    self.set_marquee(if dragging {
                        selection_marquee_rectangle(start, point, self.camera().zoom)
                    } else {
                        None
                    });
                    if event.event_type == PointerEventType::Up {
                        let style = self.filter_style(document);
                        let transaction = document.auto_filter_fill_transaction(
                            &self.state.auto_filter.hits,
                            style.filter_type,
                            style.strength,
                            true,
                        );
                        self.cancel_interaction();
                        self.state.auto_filter.hover = Some(point);
                        if !transaction.is_empty() {
                            self.pending_command = Some(EditorCommand::ApplyTransaction(
                                ApplyTransactionCommand::new(transaction),
                            ));
                        }
                        output.capture = PointerCaptureCommand::Release;
                    }
                    output.consumed = true;
                }
            }
            PointerEventType::Leave => {
                self.state.auto_filter.hover = None;
            }
            PointerEventType::Cancel => {
                self.cancel_interaction();
                output.capture = PointerCaptureCommand::Release;
            }
            _ => {}
        }
        self.bump_overlay_state_revision();
        Ok(output)
    }

    pub(crate) fn invalidate_auto_filter_gesture(&mut self, document: &DocumentModel) {
        if self
            .state
            .auto_filter
            .start
            .is_some_and(|(_, _, _, generation)| generation != document.auto_filter_generation())
        {
            // Retain the pointer until the next event can release capture, but remove
            // stale previews immediately when a record transaction is committed.
            self.state.auto_filter.hits.clear();
            self.state.auto_filter.hover = None;
            self.set_marquee(None);
        }
    }

    pub(crate) fn auto_filter_highlights(&self, document: &DocumentModel) -> Vec<RectangleData> {
        if self.active_tool() != ActiveTool::AutoFilter {
            return Vec::new();
        }
        let Some(record) = document.auto_filter_regions() else {
            return Vec::new();
        };
        let hits = if self.state.auto_filter.start.is_some() {
            self.state.auto_filter.hits.clone()
        } else {
            self.state
                .auto_filter
                .hover
                .and_then(|p| record.region_at(p))
                .map(|r| vec![r.id])
                .unwrap_or_default()
        };
        let style = self.filter_style(document);
        record
            .regions
            .iter()
            .filter(|r| hits.contains(&r.id))
            .filter_map(|region| {
                let mut rect = selection_marquee_rectangle(
                    Point::new(region.bounds.min_x, region.bounds.min_y),
                    Point::new(region.bounds.max_x, region.bounds.max_y),
                    self.camera().zoom,
                )?;
                let success = document
                    .auto_filter_fill(region.id)
                    .is_some_and(|(_, fill)| fill.filter_type == style.filter_type);
                let color = if success {
                    snow_draw_engine_core::ColorRgba8 {
                        r: 0x52,
                        g: 0xc4,
                        b: 0x1a,
                        a: 255,
                    }
                } else {
                    snow_draw_engine_core::ColorRgba8 {
                        r: 0xff,
                        g: 0x4d,
                        b: 0x4f,
                        a: 255,
                    }
                };
                rect.stroke = color;
                rect.fill = snow_draw_engine_core::ColorRgba8 {
                    a: rect.fill.a,
                    ..color
                };
                Some(rect)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_document::{AutoFilterRegion, AutoFilterRegionRecord, CanvasFilterType};
    use snow_draw_engine_interaction::{PointerButtons, PointerDevice};

    fn record() -> AutoFilterRegionRecord {
        AutoFilterRegionRecord {
            source_bounds: DrawRect::new(0.0, 0.0, 100.0, 100.0),
            regions: vec![
                AutoFilterRegion {
                    id: 1,
                    bounds: DrawRect::new(0.0, 0.0, 100.0, 100.0),
                    category: "image".into(),
                },
                AutoFilterRegion {
                    id: 2,
                    bounds: DrawRect::new(20.0, 20.0, 40.0, 40.0),
                    category: "text".into(),
                },
            ],
        }
    }
    fn pointer(kind: PointerEventType, x: f64, y: f64) -> InputEvent {
        InputEvent::Pointer(PointerEvent {
            pointer_id: 1,
            event_type: kind,
            device: PointerDevice::Mouse,
            position: Point::new(x, y),
            button: Some(PointerButton::Primary),
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    }
    fn setup() -> (Editor, DocumentModel) {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::AutoFilter).unwrap();
        // A zero-sized view leaves canvas and view coordinates identical.
        editor.set_surface_size(0, 0).unwrap();
        let mut model = DocumentModel::new();
        let transaction = model
            .auto_filter_record_transaction(Some(record()))
            .unwrap();
        model.apply_transaction(transaction).unwrap();
        (editor, model)
    }
    fn commit(model: &mut DocumentModel, update: EditorUpdate) {
        if let Some(EditorCommand::ApplyTransaction(command)) = update.command {
            model.apply_transaction(command.transaction).unwrap();
        }
    }
    #[test]
    fn auto_filter_hover_child_priority_and_marquee_intersection() {
        let (mut e, mut m) = setup();
        e.process_input(&m, pointer(PointerEventType::Move, 30.0, 30.0))
            .unwrap();
        let preview = e.auto_filter_highlights(&m);
        assert_eq!(preview.len(), 1);
        assert_eq!(preview[0].width, 20.0);
        assert_eq!(
            (
                preview[0].fill.r,
                preview[0].fill.g,
                preview[0].fill.b,
                preview[0].fill.a
            ),
            (255, 77, 79, 51)
        );
        e.process_input(&m, pointer(PointerEventType::Down, 30.0, 30.0))
            .unwrap();
        let up = e
            .process_input(&m, pointer(PointerEventType::Up, 30.0, 30.0))
            .unwrap();
        commit(&mut m, up);
        assert!(m.auto_filter_fill(2).is_some());
        assert!(m.auto_filter_fill(1).is_none());
        assert_eq!(e.auto_filter_highlights(&m)[0].stroke.g, 196);
        e.process_input(&m, pointer(PointerEventType::Down, 25.0, 25.0))
            .unwrap();
        e.process_input(&m, pointer(PointerEventType::Move, 45.0, 45.0))
            .unwrap();
        let highlights = e.auto_filter_highlights(&m);
        assert_eq!(highlights.len(), 2);
        assert_eq!(highlights[0].stroke.r, 255);
        assert_eq!(highlights[1].stroke.g, 196);
        assert_eq!(e.state.ui.marquee.unwrap().stroke.b, 255);
        let up = e
            .process_input(&m, pointer(PointerEventType::Up, 45.0, 45.0))
            .unwrap();
        let Some(EditorCommand::ApplyTransaction(ref command)) = up.command else {
            panic!("one marquee transaction");
        };
        assert_eq!(command.transaction.operations().len(), 2);
        commit(&mut m, up);
        assert!(m.auto_filter_fill(1).is_some());
        assert!(m.auto_filter_fill(2).is_none());
    }
    #[test]
    fn auto_filter_fills_ignore_selection_hover_click_and_drag() {
        for tool in [ActiveTool::Select, ActiveTool::RectangleFilter] {
            for shift in [false, true] {
                let (mut e, mut m) = setup();
                let fill = m.auto_filter_fill_transaction(
                    &[2],
                    CanvasFilterType::GaussianBlur,
                    0.3,
                    false,
                );
                m.apply_transaction(fill).unwrap();
                let fill_id = m.auto_filter_fill(2).unwrap().0;
                let original = *m.auto_filter_fill(2).unwrap().1;
                e.set_active_tool(tool).unwrap();
                for (kind, x, y) in [
                    (PointerEventType::Move, 30.0, 30.0),
                    (PointerEventType::Down, 30.0, 30.0),
                    (PointerEventType::Move, 35.0, 35.0),
                    (PointerEventType::Up, 35.0, 35.0),
                ] {
                    let InputEvent::Pointer(mut event) = pointer(kind, x, y) else {
                        unreachable!();
                    };
                    event.modifiers.shift = shift;
                    if kind == PointerEventType::Move && x == 30.0 {
                        event.button = None;
                        event.buttons = PointerButtons::default();
                    }
                    let update = e.process_input(&m, InputEvent::Pointer(event)).unwrap();
                    commit(&mut m, update);
                    assert!(
                        !e.selected_ids().contains(&fill_id),
                        "{tool:?}, shift={shift}"
                    );
                    assert_ne!(e.state.ui.hovered_element, Some(fill_id), "{tool:?}");
                    if kind == PointerEventType::Move && x == 30.0 {
                        assert!(e.presentation_state(&m).hovered_rect.is_none(), "{tool:?}");
                    }
                    assert!(e.auto_filter_highlights(&m).is_empty());
                    assert_eq!(*m.auto_filter_fill(2).unwrap().1, original);
                }
                e.set_active_tool(ActiveTool::AutoFilter).unwrap();
                e.process_input(&m, pointer(PointerEventType::Move, 30.0, 30.0))
                    .unwrap();
                assert_eq!(e.auto_filter_highlights(&m).len(), 1);
                e.process_input(&m, pointer(PointerEventType::Down, 30.0, 30.0))
                    .unwrap();
                let up = e
                    .process_input(&m, pointer(PointerEventType::Up, 30.0, 30.0))
                    .unwrap();
                commit(&mut m, up);
                assert_ne!(*m.auto_filter_fill(2).unwrap().1, original);
                e.process_input(&m, pointer(PointerEventType::Down, 30.0, 30.0))
                    .unwrap();
                let up = e
                    .process_input(&m, pointer(PointerEventType::Up, 30.0, 30.0))
                    .unwrap();
                commit(&mut m, up);
                assert!(m.auto_filter_fill(2).is_none());
            }
        }
    }

    #[test]
    fn auto_filter_gesture_does_not_apply_new_detection_and_eraser_preserves_fill() {
        let (mut e, mut m) = setup();
        let reset = m.auto_filter_record_transaction(None).unwrap();
        m.apply_transaction(reset).unwrap();
        e.process_input(&m, pointer(PointerEventType::Down, 30.0, 30.0))
            .unwrap();
        let identify = m.auto_filter_record_transaction(Some(record())).unwrap();
        m.apply_transaction(identify).unwrap();
        assert!(
            e.process_input(&m, pointer(PointerEventType::Up, 30.0, 30.0))
                .unwrap()
                .command
                .is_none()
        );
        let fill = m.auto_filter_fill_transaction(&[2], CanvasFilterType::GaussianBlur, 0.3, false);
        m.apply_transaction(fill).unwrap();
        e.set_active_tool(ActiveTool::Eraser).unwrap();
        e.process_input(&m, pointer(PointerEventType::Down, 30.0, 30.0))
            .unwrap();
        let up = e
            .process_input(&m, pointer(PointerEventType::Up, 30.0, 30.0))
            .unwrap();
        commit(&mut m, up);
        assert!(m.auto_filter_fill(2).is_some());
        assert!(m.auto_filter_regions().is_some());
    }
    #[test]
    fn auto_filter_record_change_clears_marquee_before_next_pointer_event() {
        let (mut e, mut m) = setup();
        e.process_input(&m, pointer(PointerEventType::Down, 25.0, 25.0))
            .unwrap();
        e.process_input(&m, pointer(PointerEventType::Move, 45.0, 45.0))
            .unwrap();
        let snapshot = e.capture_document_sync_snapshot(&m);
        let transaction = m.auto_filter_record_transaction(Some(record())).unwrap();
        m.apply_transaction(transaction).unwrap();
        e.sync_after_document_change(&m, &snapshot);
        assert!(e.state.ui.marquee.is_none());
        assert!(e.auto_filter_highlights(&m).is_empty());
        let up = e
            .process_input(&m, pointer(PointerEventType::Up, 45.0, 45.0))
            .unwrap();
        assert!(up.command.is_none());
    }
}
