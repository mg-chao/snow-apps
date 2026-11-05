use super::*;

pub(super) struct CanvasElement {
    pub view: Entity<CanvasView>,
}
impl IntoElement for CanvasElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for CanvasElement {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static core::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.size.width = relative(1.0).into();
        style.size.height = relative(1.0).into();
        (window.request_layout(style, [], cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Hitbox {
        let hitbox = window.insert_hitbox(bounds, HitboxBehavior::Normal);
        self.view.update(cx, |this, cx| {
            let resized = this.bounds.size != bounds.size || this.scale != window.scale_factor();
            this.bounds = bounds;
            this.scale = window.scale_factor();
            this.update_crosshair(this.scale, cx);
            if resized && bounds.size.width > px(0.0) && bounds.size.height > px(0.0) {
                match this.session.set_surface(
                    &mut this.document,
                    f32::from(bounds.size.width).round().max(1.0) as u32,
                    f32::from(bounds.size.height).round().max(1.0) as u32,
                ) {
                    Ok(_) => {
                        if this.generation == 0 {
                            this.fit(window, cx);
                        } else {
                            this.changed(window, cx);
                        }
                    }
                    Err(error) => this.fail(error, cx),
                }
            }
            if this.document.engine().has_pointer_capture()
                || this.pan.is_some()
                || this.selecting_text
            {
                window.capture_pointer(hitbox.id);
            }
            this.prepare_display_frame(window, cx);
        });
        hitbox
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        cx: &mut App,
    ) {
        let (image, image_frame, focus, editing, cursor, scene, options, crosshair, dirty) =
            self.view.read_with(cx, |this, _| {
                (
                    this.gpu_frame.clone(),
                    this.image_frame,
                    this.focus.clone(),
                    this.draft.is_some(),
                    this.effective_cursor(),
                    this.display_scene().clone(),
                    crate::CanvasViewOptions {
                        presentation: this.display_presentation().clone(),
                        ..this.options.clone()
                    },
                    this.crosshair_cache.and_then(|(_, value)| value),
                    this.show_dirty_regions,
                )
            });
        window.set_cursor_style(cursor, hitbox);
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            let clear = scene.frame().clear_color;
            if options.presentation.clear_background && image.is_none() {
                window.paint_quad(fill(bounds, gpui_color(clear)));
            }
            if let Some(frame) = image
                && let Some(reference) = image_frame
            {
                let live = scene.frame();
                let ratio = live.camera.zoom / reference.camera.zoom;
                let density = frame.scale;
                let top_left = draw::view_to_canvas(
                    draw::Point::new(0.0, 0.0),
                    &reference.camera,
                    reference.surface,
                );
                let origin = draw::canvas_to_view(top_left, &live.camera, live.surface);
                for tile in frame.tiles.iter() {
                    let r = tile.region;
                    let tile_bounds = Bounds::from_corners(
                        bounds.origin
                            + point(
                                px(origin.x as f32 + r.x as f32 / density * ratio as f32),
                                px(origin.y as f32 + r.y as f32 / density * ratio as f32),
                            ),
                        bounds.origin
                            + point(
                                px(origin.x as f32
                                    + (r.x + r.width) as f32 / density * ratio as f32),
                                px(origin.y as f32
                                    + (r.y + r.height) as f32 / density * ratio as f32),
                            ),
                    );
                    let _ = window.paint_gpu_texture(
                        tile_bounds,
                        tile.texture.clone(),
                        gpui::GpuTextureOptions {
                            revision: tile.revision,
                            linear_filtering: false,
                            ..Default::default()
                        },
                    );
                }
            }
            if options.presentation.content_visible {
                paint_overlays(scene.overlays(), scene.frame(), bounds, window, cx);
            }
            if editing {
                window.handle_input(
                    &focus,
                    ElementInputHandler::new(bounds, self.view.clone()),
                    cx,
                );
            }
            self.view
                .update(cx, |this, cx| paint_text_selection(this, window, cx));
            if dirty {
                for (regions, color) in [
                    (scene.dirty_regions(), rgba(0xe34b4be0)),
                    (scene.overlay_dirty_regions(), rgba(0x387bd1e0)),
                ] {
                    for r in regions {
                        let rect = Bounds::from_corners(
                            bounds.origin + point(px(r.min_x as f32), px(r.min_y as f32)),
                            bounds.origin + point(px(r.max_x as f32), px(r.max_y as f32)),
                        );
                        window.paint_quad(gpui::outline(rect, color, Default::default()));
                    }
                }
            }
            if cursor == CursorStyle::Crosshair
                && !editing
                && (hitbox.is_hovered(window) || window.captured_hitbox() == Some(hitbox.id))
                && let Some(crosshair) = crosshair
            {
                host_controls::paint_crosshair(crosshair, window.mouse_position(), window);
            }
        });
        let entity = self.view.downgrade();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase.bubble() && hit.is_hovered_at(event.position, window) {
                let _ = entity.update(cx, |this, cx| this.mouse_down(event, &hit, window, cx));
                cx.stop_propagation();
            }
        });
        let entity = self.view.downgrade();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase.bubble()
                && (hit.is_hovered_at(event.position, window)
                    || window.captured_hitbox() == Some(hit.id))
            {
                let _ = entity.update(cx, |this, cx| this.mouse_move(event, &hit, window, cx));
            }
        });
        let entity = self.view.downgrade();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
            if phase.bubble()
                && (hit.is_hovered_at(event.position, window)
                    || window.captured_hitbox() == Some(hit.id))
            {
                let _ = entity.update(cx, |this, cx| this.mouse_up(event, &hit, window, cx));
            }
        });
        let entity = self.view.downgrade();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &ScrollEvent, phase, window, cx| {
            if phase.bubble() && hit.should_handle_scroll(window) {
                let _ = entity.update(cx, |this, cx| {
                    if !this.options.interaction_enabled || !this.options.wheel_zoom_enabled {
                        return;
                    }
                    let (x, y, kind) = match event.delta {
                        ScrollDelta::Pixels(delta) => (
                            f64::from(f32::from(delta.x)),
                            f64::from(f32::from(delta.y)),
                            draw::WheelDeltaKind::Pixel,
                        ),
                        ScrollDelta::Lines(delta) => (
                            f64::from(delta.x) * 120.0,
                            f64::from(delta.y) * 120.0,
                            draw::WheelDeltaKind::Angle,
                        ),
                    };
                    let position = this.local(event.position);
                    this.input(
                        draw::InputEvent::Wheel(draw::WheelEvent {
                            position,
                            delta: draw::Vector2 { x: -x, y: -y },
                            delta_kind: kind,
                            modifiers: modifiers(event.modifiers, cx),
                        }),
                        &hit,
                        window,
                        cx,
                    );
                });
                cx.stop_propagation();
            }
        });
    }
}

impl CanvasView {
    fn local(&self, point: Point<Pixels>) -> draw::Point<f64> {
        let local = point - self.bounds.origin;
        draw::Point::new(f64::from(f32::from(local.x)), f64::from(f32::from(local.y)))
    }
    fn world(&self, point: Point<Pixels>) -> draw::Point<f64> {
        let frame = self.session.scene().frame();
        draw::view_to_canvas(self.local(point), &frame.camera, frame.surface)
    }
    fn pointer(
        &self,
        kind: draw::PointerEventType,
        position: Point<Pixels>,
        button: Option<MouseButton>,
        modifier: Modifiers,
        cx: &App,
    ) -> draw::InputEvent {
        draw::InputEvent::Pointer(draw::PointerEvent {
            pointer_id: 1,
            event_type: kind,
            device: draw::PointerDevice::Mouse,
            position: self.local(position),
            button: button.map(pointer_button),
            buttons: draw::PointerButtons(if kind == draw::PointerEventType::Up {
                0
            } else {
                match button {
                    Some(MouseButton::Left) => draw::PointerButtons::PRIMARY,
                    Some(MouseButton::Right) => draw::PointerButtons::SECONDARY,
                    _ => 0,
                }
            }),
            modifiers: modifiers(modifier, cx),
            pressure: None,
            timestamp_ms: 0,
        })
    }
    fn mouse_down(
        &mut self,
        event: &MouseDownEvent,
        hitbox: &Hitbox,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.options.interaction_enabled {
            return;
        }
        self.focus.focus(window, cx);
        if event.button == MouseButton::Middle || self.space_down {
            let space = self.space_down;
            self.finish_text(true, window, cx);
            self.cancel_gesture(window, cx);
            self.space_down = space;
            self.pan = Some((event.position, self.session.scene().frame().camera));
            self.cursor = CursorStyle::ClosedHand;
            window.capture_pointer(hitbox.id);
            cx.notify();
            return;
        }
        let finishing_text = self.draft.is_some();
        if let Some(draft) = self.draft.as_ref() {
            let world = self.world(event.position);
            let (sin, cos) = draft.rotation.sin_cos();
            let dx = world.x - draft.center.x;
            let dy = world.y - draft.center.y;
            let width = if draft.auto_resize {
                draft.layout.width
            } else {
                draft.width
            };
            if event.button == MouseButton::Left
                && (dx * cos + dy * sin).abs() <= width / 2.0 + 8.0
                && (-dx * sin + dy * cos).abs() <= draft.layout.height / 2.0 + 8.0
                && let Some(index) = self.text_index_at(event.position)
            {
                let draft = self.draft.as_mut().unwrap();
                draft.buffer.select(index, event.modifiers.shift);
                self.selecting_text = true;
                window.capture_pointer(hitbox.id);
                self.reset_text_caret(window, cx);
                return;
            }
            // An active text editor owns double-clicks even outside its ink.
            // Committing here would let the same press complete its host.
            if event.button == MouseButton::Left && event.click_count >= 2 {
                return;
            }
            self.finish_text(true, window, cx);
        }
        if self
            .options
            .presentation
            .content_clip
            .is_some_and(|area| !area.contains(self.world(event.position)))
            && (event.button != MouseButton::Left || !self.hits_existing_selection(event.position))
        {
            return;
        }
        // A blank primary press belongs to the editor it just closed. Forward
        // presses on existing targets so another label or handle can take over.
        if finishing_text
            && event.button == MouseButton::Left
            && self.primary_element_at(event.position).is_none()
            && !self.hits_existing_selection(event.position)
        {
            if let Err(error) = self.session.mutate(
                &mut self.document,
                draw::Engine::clear_selection_with_viewport_changes,
            ) {
                self.fail(error, cx);
            }
            return;
        }
        let kind = if event.click_count >= 2 {
            draw::PointerEventType::DoubleClick
        } else {
            draw::PointerEventType::Down
        };
        if kind == draw::PointerEventType::DoubleClick
            && event.button == MouseButton::Left
            && event.modifiers == Modifiers::default()
            && self.begin_bound_label_at(self.world(event.position), window, cx)
        {
            return;
        }
        let consumed = self.input_with_consumption(
            self.pointer(
                kind,
                event.position,
                Some(event.button),
                event.modifiers,
                cx,
            ),
            hitbox,
            window,
            cx,
        );
        // A press that finishes editing may open another existing label, but it
        // must never spend the same press creating a new empty draft.
        self.check_text_request(event.position, !finishing_text, window, cx);
        if self.draft.is_some() && event.button == MouseButton::Left {
            self.selecting_text = true;
            window.capture_pointer(hitbox.id);
        }
        if event.click_count == 2
            && event.button == MouseButton::Left
            && !consumed
            && self.draft.is_none()
            && self.error.is_none()
        {
            cx.emit(CanvasEvent::UnhandledDoubleClick {
                position: event.position,
            });
        }
    }
    fn primary_element_at(&self, position: Point<Pixels>) -> Option<draw::ElementId> {
        self.document
            .engine()
            .hit_quick_selection_at(
                self.session.viewport(),
                self.world(position),
                draw::PointerButton::Primary,
            )
            .ok()
            .flatten()
    }
    pub(super) fn hits_existing_selection(&self, position: Point<Pixels>) -> bool {
        let world = self.world(position);
        let selected = self.document.engine().selected_ids();
        if selected.is_empty() {
            return false;
        }
        let frame = self.session.scene().frame();
        if self
            .document
            .engine()
            .viewport_active_tool(self.session.viewport())
            .ok()
            == Some(draw::ActiveTool::Select)
            && self
                .primary_element_at(position)
                .is_some_and(|id| selected.contains(&id))
        {
            return true;
        }
        selection_input::hits_selection_overlay(
            self.session.scene().overlays(),
            world,
            frame.camera.zoom,
        )
    }
    fn mouse_move(
        &mut self,
        event: &MouseMoveEvent,
        hitbox: &Hitbox,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if let Some((origin, camera)) = self.pan {
            let delta = event.position - origin;
            let next = draw::Camera {
                center: draw::Point::new(
                    camera.center.x - f64::from(f32::from(delta.x)) / camera.zoom,
                    camera.center.y - f64::from(f32::from(delta.y)) / camera.zoom,
                ),
                ..camera
            };
            if let Err(error) = self.session.set_camera(&mut self.document, next) {
                self.fail(error, cx);
            }
            self.changed(window, cx);
            return;
        }
        if self.selecting_text {
            if let Some(index) = self.text_index_at(event.position) {
                self.draft.as_mut().unwrap().buffer.select(index, true);
                self.reset_text_caret(window, cx);
            }
            return;
        }
        if self.draft.is_some() {
            return;
        }
        self.input(
            self.pointer(
                draw::PointerEventType::Move,
                event.position,
                event.pressed_button,
                event.modifiers,
                cx,
            ),
            hitbox,
            window,
            cx,
        );
    }
    fn mouse_up(
        &mut self,
        event: &MouseUpEvent,
        hitbox: &Hitbox,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.pan.take().is_some() {
            window.release_pointer();
            self.cursor = CursorStyle::OpenHand;
            cx.notify();
            return;
        }
        if self.selecting_text {
            self.selecting_text = false;
            window.release_pointer();
            return;
        }
        if self.draft.is_some() {
            return;
        }
        self.input(
            self.pointer(
                draw::PointerEventType::Up,
                event.position,
                Some(event.button),
                event.modifiers,
                cx,
            ),
            hitbox,
            window,
            cx,
        );
        self.check_text_request(event.position, true, window, cx);
    }
    fn check_text_request(
        &mut self,
        position: Point<Pixels>,
        allow_new: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let viewport = self.session.viewport();
        let existing = self
            .document
            .command(|engine| engine.take_text_edit_request(viewport))
            .ok()
            .flatten();
        let bubble = self
            .document
            .command(|engine| engine.take_speech_bubble_text_edit_request(viewport))
            .ok()
            .flatten();
        let new = self
            .document
            .command(|engine| engine.take_new_text_draft_request(viewport))
            .unwrap_or(false);
        if existing.is_some() || bubble.is_some() || (new && allow_new) {
            self.begin_text(self.world(position), existing, window, cx);
            if let Some(index) = self.text_index_at(position) {
                self.draft.as_mut().unwrap().buffer.select(index, false);
                self.reset_text_caret(window, cx);
            }
        }
    }
}

fn pointer_button(button: MouseButton) -> draw::PointerButton {
    match button {
        MouseButton::Right => draw::PointerButton::Secondary,
        MouseButton::Middle => draw::PointerButton::Middle,
        _ => draw::PointerButton::Primary,
    }
}
fn modifiers(modifiers: Modifiers, cx: &App) -> draw::Modifiers {
    draw::Modifiers {
        ctrl: modifiers.secondary(cx),
        shift: modifiers.shift,
        alt: modifiers.alt,
        meta: modifiers.platform,
    }
}
pub(super) fn gpui_color(color: draw::ColorRgba8) -> Hsla {
    rgba(
        (u32::from(color.r) << 24)
            | (u32::from(color.g) << 16)
            | (u32::from(color.b) << 8)
            | u32::from(color.a),
    )
    .into()
}

fn paint_overlays(
    overlays: &[draw::OverlayDisplayItem],
    frame: draw::FrameView,
    bounds: Bounds<Pixels>,
    window: &mut Window,
    cx: &mut App,
) {
    let map = |x: f64, y: f64| {
        let p = draw::canvas_to_view(draw::Point::new(x, y), &frame.camera, frame.surface);
        bounds.origin + point(px(p.x as f32), px(p.y as f32))
    };
    for overlay in overlays {
        match overlay {
            draw::OverlayDisplayItem::Rectangle(item) => {
                selection_overlay::paint_rectangle(item, frame, bounds, window);
            }
            draw::OverlayDisplayItem::SnapGuide(item) => {
                for path in selection_guides::guide_paths(item, frame, bounds) {
                    window.paint_path(path, gpui_color(item.color));
                }
                if item.kind == draw::SnapGuideKind::Gap
                    && let Some(label) = item.label
                {
                    let text: SharedString = format!("{label:.0}").into();
                    let line = window.text_system().shape_line(
                        text.clone(),
                        px(12.0),
                        &[gpui::TextRun {
                            len: text.len(),
                            font: gpui::font("Segoe UI"),
                            color: gpui_color(item.color),
                            ..Default::default()
                        }],
                        None,
                    );
                    let midpoint = map(
                        (item.start.x + item.end.x) / 2.0,
                        (item.start.y + item.end.y) / 2.0,
                    );
                    let origin = if item.axis == draw::SnapGuideAxis::Horizontal {
                        midpoint - point(line.width / 2.0, px(20.0))
                    } else {
                        midpoint + point(px(6.0), px(-8.0))
                    };
                    let _ = line.paint(origin, px(16.0), gpui::TextAlign::Left, None, window, cx);
                }
            }
            draw::OverlayDisplayItem::FocusConnection(item)
            | draw::OverlayDisplayItem::PenFilterContour(item) => {
                connection_overlay::paint(
                    item,
                    matches!(overlay, draw::OverlayDisplayItem::PenFilterContour(_)),
                    frame,
                    bounds,
                    window,
                );
            }
        }
    }
}

fn paint_text_selection(this: &mut CanvasView, window: &mut Window, cx: &mut Context<CanvasView>) {
    let caret_visible = this.text_caret_visible(window, cx);
    if let Err(error) =
        this.text_selection_overlay
            .paint_published(this.bounds, caret_visible, window)
    {
        this.fail(error, cx);
    }
}
