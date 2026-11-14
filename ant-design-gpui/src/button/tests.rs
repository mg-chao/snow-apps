// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Quad,
    Render, RenderOnce, Role, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit,
    div, point, px,
};

use super::*;
use crate::{
    ConfigPatch, ConfigScope, Direction, Size, UiRoot,
    behavior::{Activation, focus_handle},
    icons::IconName,
};

struct Consumer {
    scope: ConfigScope,
    focus: FocusHandle,
    second_focus: FocusHandle,
    activations: Rc<RefCell<Vec<Activation>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    disabled: Option<bool>,
    loading: bool,
    custom_loader: Option<IconName>,
    delay: Duration,
    visible: bool,
    second_visible: bool,
    external_focus: bool,
    label: &'static str,
    icon: bool,
    two_tone: bool,
    placement: IconPlacement,
    size: Option<Size>,
    custom_width: Option<f32>,
    button_type: ButtonType,
    shape: ButtonShape,
    block: bool,
    auto_space: bool,
    href: bool,
    wave: bool,
    spin_loader: Option<bool>,
    style: StyleRefinement,
    icon_style: Option<StyleRefinement>,
    content_style: Option<StyleRefinement>,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: focus_handle(cx),
            second_focus: focus_handle(cx),
            activations: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::Button))),
            disabled: None,
            loading: false,
            custom_loader: None,
            delay: Duration::ZERO,
            visible: true,
            second_visible: true,
            external_focus: true,
            label: "Save",
            icon: false,
            two_tone: false,
            placement: IconPlacement::Start,
            size: None,
            custom_width: None,
            button_type: ButtonType::Default,
            shape: ButtonShape::Default,
            block: false,
            auto_space: true,
            href: false,
            wave: true,
            spin_loader: None,
            style: StyleRefinement::default(),
            icon_style: None,
            content_style: None,
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut row = div()
            .id("button-test-row")
            .flex()
            .items_start()
            .gap(px(20.))
            .w(px(300.))
            .p(px(12.));
        if self.visible {
            let activations = self.activations.clone();
            let mut button = Button::new("button-test-first")
                .label(self.label)
                .scope(self.scope.clone())
                .button_type(self.button_type)
                .loading(self.loading)
                .loading_delay(self.delay)
                .icon_placement(self.placement)
                .shape(self.shape)
                .block(self.block)
                .auto_insert_space(self.auto_space)
                .wave(self.wave)
                .automation_id("button-test.first")
                .accessibility_description("Save the current document")
                .on_click(move |activation, _, _| activations.borrow_mut().push(*activation));
            button.style().refine(&self.style);
            if let Some(spin) = self.spin_loader {
                button = button.loading_icon_spin(spin);
            }
            if let Some(style) = &self.icon_style {
                button = button.icon_style(style.clone());
            }
            if let Some(style) = &self.content_style {
                button = button.content_style(style.clone());
            }
            if let Some(disabled) = self.disabled {
                button = button.disabled(disabled);
            }
            if let Some(size) = self.size {
                button = button.size(size);
            }
            if let Some(width) = self.custom_width {
                button = button.w(px(width));
            }
            if self.external_focus {
                button = button.focus_handle(&self.focus);
            }
            if self.icon {
                button = button.icon(if self.two_tone {
                    IconName::HeartTwoTone
                } else {
                    IconName::SearchOutlined
                });
            }
            if self.href {
                button = button.href("https://example.invalid/button-test");
            }
            if let Some(icon) = self.custom_loader {
                button = button.loading_icon(icon);
            }
            // Inspect the very same element that is subsequently laid out and painted.
            let element = button.render(window, cx).into_element();
            let mut node = accesskit::Node::new(element.a11y_role().expect("button role"));
            element.write_a11y_info(&mut node);
            *self.semantics.borrow_mut() = node;
            row = row.child(element);
        }
        if self.second_visible {
            row = row.child(
                Button::new("button-test-second")
                    .label("Next")
                    .scope(self.scope.clone())
                    .focus_handle(&self.second_focus),
            );
        }
        row
    }
}

struct CachedPanel(Entity<Consumer>);

impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(320.)).h(px(160.))),
        )
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    scope: ConfigScope,
}

impl Fixture {
    fn new() -> Self {
        Self::configured(|_| {})
    }

    fn configured(configure: impl FnOnce(&mut Consumer)) -> Self {
        Self::with_motion(false, configure)
    }

    fn with_motion(motion: bool, configure: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let button_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut consumer = Consumer::new(button_scope.clone(), cx);
                configure(&mut consumer);
                consumer
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(button_scope);
            UiRoot::new(root_scope, panel, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
            scope: scope.unwrap(),
        };
        fixture.draw();
        fixture
    }

    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            edit(consumer);
            cx.notify();
        });
        self.draw();
    }

    fn key_event(&mut self, stroke: &'static str, down: bool, held: bool) {
        let keystroke = Keystroke::parse(stroke).unwrap();
        let event = if down {
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke,
                is_held: held,
                prefer_character_input: false,
            })
        } else {
            PlatformInput::KeyUp(KeyUpEvent { keystroke })
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx)
            })
            .unwrap();
    }

    fn key(&mut self, stroke: &'static str) {
        self.key_event(stroke, true, false);
        self.key_event(stroke, false, false);
        self.draw();
    }

    fn pointer(&mut self, down: bool, inside: bool) {
        let position = if inside {
            point(px(20.), px(24.))
        } else {
            point(px(280.), px(140.))
        };
        let event = if down {
            PlatformInput::MouseDown(MouseDownEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            })
        } else {
            PlatformInput::MouseUp(MouseUpEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            })
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx)
            })
            .unwrap();
    }

    fn click(&mut self) {
        self.pointer(true, true);
        self.pointer(false, true);
        self.draw();
    }

    fn mouse_move(&mut self, inside: bool) {
        let position = if inside {
            point(px(20.), px(24.))
        } else {
            point(px(280.), px(140.))
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseMove(gpui::MouseMoveEvent {
                        position,
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
    }

    fn hover(&mut self, inside: bool) {
        self.mouse_move(inside);
        self.draw();
    }

    fn activations(&self) -> Vec<Activation> {
        self.consumer.read_with(&self.cx, |consumer, _| {
            consumer.activations.borrow().clone()
        })
    }

    fn focused(&mut self, second: bool) -> bool {
        let focus = self.consumer.read_with(&self.cx, |consumer, _| {
            if second {
                consumer.second_focus.clone()
            } else {
                consumer.focus.clone()
            }
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }

    fn advance(&mut self, milliseconds: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(milliseconds));
        self.draw();
    }

    fn frame(&mut self, milliseconds: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(milliseconds));
        let count = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        count
    }

    fn body(&mut self) -> (Quad, f32) {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| {
                        (quad.bounds.origin.x.0 / scale - 12.).abs() < 0.01
                            && (quad.bounds.origin.y.0 / scale - 12.).abs() < 0.01
                            && quad.bounds.size.height.0 / scale >= 24.
                            && quad.bounds.size.height.0 / scale <= 40.
                    })
                    .max_by(|left, right| left.border_color.a.total_cmp(&right.border_color.a))
                    .copied()
                    .expect("first button surface");
                (quad, scale)
            })
            .unwrap()
    }

    fn semantics(&self) -> accesskit::Node {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.semantics.borrow().clone())
    }
}

#[test]
fn keyboard_and_pointer_each_activate_once_and_cancel_outside_release() {
    let mut fixture = Fixture::new();
    fixture.key("tab");
    assert!(fixture.focused(false));
    fixture.key_event("space", true, false);
    fixture.key_event("space", true, true);
    fixture.key_event("space", false, false);
    fixture.key("enter");
    fixture.click();
    fixture.pointer(true, true);
    fixture.pointer(false, false);
    assert_eq!(
        fixture.activations(),
        vec![
            Activation::Keyboard(KeyboardButton::Space),
            Activation::Keyboard(KeyboardButton::Enter),
            Activation::Pointer,
        ]
    );
}

#[test]
fn disabled_and_busy_buttons_block_activation_but_only_disabled_skips_tab() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| consumer.disabled = Some(true));
    fixture.click();
    fixture.key("tab");
    assert!(fixture.focused(true));
    assert!(fixture.activations().is_empty());
    assert!(fixture.semantics().is_disabled());
    assert!(
        !fixture
            .semantics()
            .supports_action(accesskit::Action::Click)
    );

    fixture.edit(|consumer| {
        consumer.disabled = Some(false);
        consumer.loading = true;
    });
    fixture.key("shift-tab");
    assert!(fixture.focused(false));
    fixture.key("enter");
    fixture.click();
    assert!(fixture.activations().is_empty());
    assert!(fixture.focused(false));
    assert!(fixture.semantics().is_busy());
    assert!(!fixture.semantics().is_disabled());
    assert!(
        fixture
            .semantics()
            .supports_action(accesskit::Action::Focus)
    );
    assert!(
        !fixture
            .semantics()
            .supports_action(accesskit::Action::Click)
    );
    fixture.key("tab");
    assert!(fixture.focused(true));
}

#[test]
fn loading_delay_remains_interactive_until_expiry_and_cancels_when_cleared() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| {
        consumer.loading = true;
        consumer.delay = Duration::from_millis(200);
    });
    fixture.advance(199);
    assert!(!fixture.semantics().is_busy());
    fixture.click();
    assert_eq!(fixture.activations().len(), 1);
    fixture.advance(1);
    assert!(fixture.semantics().is_busy());
    fixture.click();
    assert_eq!(fixture.activations().len(), 1);

    fixture.edit(|consumer| consumer.loading = false);
    fixture.edit(|consumer| consumer.loading = true);
    fixture.advance(100);
    fixture.edit(|consumer| consumer.loading = false);
    fixture.advance(200);
    assert!(!fixture.semantics().is_busy());
    fixture.click();
    assert_eq!(fixture.activations().len(), 2);
}

#[test]
fn changing_the_loading_delay_while_busy_never_reenables_activation() {
    let mut fixture = Fixture::configured(|consumer| consumer.loading = true);
    assert!(fixture.semantics().is_busy());
    fixture.edit(|consumer| consumer.delay = Duration::from_millis(200));
    assert!(fixture.semantics().is_busy());
    fixture.click();
    fixture.advance(200);
    assert!(fixture.semantics().is_busy());
    assert!(fixture.activations().is_empty());
    fixture.edit(|consumer| consumer.loading = false);
    fixture.click();
    assert_eq!(fixture.activations(), vec![Activation::Pointer]);
}

#[test]
fn inherited_availability_round_trip_cancels_a_press_before_rerender() {
    let mut fixture = Fixture::new();
    fixture.key("tab");
    fixture.key_event("enter", true, false);
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap();
    });
    fixture.cx.run_until_parked();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(false))
            .unwrap();
    });
    fixture.cx.run_until_parked();
    // Neither edge was drawn. A subscribed retained guard must still cancel the press.
    fixture.key_event("enter", false, false);
    assert!(fixture.activations().is_empty());
    fixture.draw();
    let focus = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.focus.clone());
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            window.focus(&focus, cx);
        })
        .unwrap();
    fixture.draw();
    fixture.key("enter");
    assert_eq!(fixture.activations().len(), 1);
}

#[test]
fn explicit_enabled_overrides_inherited_disabled_in_a_cached_parent() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| consumer.disabled = Some(false));
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap();
    });
    fixture.draw();
    assert!(!fixture.semantics().is_disabled());
    fixture.click();
    assert_eq!(fixture.activations(), vec![Activation::Pointer]);
    fixture.key("tab");
    assert!(fixture.focused(false));
}

#[test]
fn an_internal_focus_handle_survives_parent_rerenders_and_removal_restores_root() {
    let mut fixture = Fixture::configured(|consumer| consumer.external_focus = false);
    fixture.key("tab");
    let original = fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            window.focused(cx).unwrap()
        })
        .unwrap();
    fixture.edit(|consumer| consumer.label = "Save changes");
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            assert!(original.is_focused(window));
        })
        .unwrap();
    fixture.key("enter");
    assert_eq!(fixture.activations().len(), 1);
    fixture.edit(|consumer| consumer.visible = false);
    fixture.key("tab");
    assert!(fixture.focused(true));
}

#[test]
fn removing_a_pending_loading_button_cannot_make_a_later_mount_busy() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| {
        consumer.loading = true;
        consumer.delay = Duration::from_millis(200);
    });
    fixture.advance(100);
    fixture.edit(|consumer| consumer.visible = false);
    fixture.draw();
    fixture.edit(|consumer| {
        consumer.visible = true;
        consumer.loading = false;
    });
    fixture.advance(200);
    assert!(!fixture.semantics().is_busy());
    fixture.click();
    assert_eq!(fixture.activations(), vec![Activation::Pointer]);
}

#[test]
fn icon_only_dimensions_and_radii_match_all_three_ant_sizes() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| {
        consumer.label = "";
        consumer.icon = true;
        consumer.second_visible = false;
    });
    for (size, height, radius) in [
        (Size::Small, 24., 4.),
        (Size::Medium, 32., 6.),
        (Size::Large, 40., 8.),
    ] {
        fixture.edit(|consumer| consumer.size = Some(size));
        let (body, scale) = fixture.body();
        assert_eq!(body.bounds.size.height.0 / scale, height);
        assert_eq!(body.bounds.size.width.0 / scale, height);
        assert_eq!(body.corner_radii.top_left.0 / scale, radius);
    }
    fixture.edit(|consumer| consumer.shape = ButtonShape::Circle);
    let (body, scale) = fixture.body();
    assert_eq!(body.corner_radii.top_left.0 / scale, 20.);
}

#[test]
fn inherited_size_updates_invalidate_the_buttons_cached_consumer() {
    let mut fixture = Fixture::new();
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.size = Some(Size::Large))
            .unwrap();
    });
    fixture.draw();
    let (body, scale) = fixture.body();
    assert_eq!(body.bounds.size.height.0 / scale, 40.);
}

#[test]
fn dashed_border_uses_one_rounded_stroke_and_recolors_without_changing_geometry() {
    let mut fixture = Fixture::configured(|consumer| {
        consumer.button_type = ButtonType::Dashed;
        consumer.second_visible = false;
    });
    let (body, _) = fixture.body();
    assert_eq!(body.border_style, gpui::BorderStyle::Solid);
    assert_eq!(body.border_color.a, 0.);
    let (rest, hover) = fixture.cx.update(|cx| {
        let config = fixture.scope.snapshot(cx);
        (
            config.theme.tokens.color_border.to_hsla(),
            config.theme.tokens.color_primary_hover.to_hsla(),
        )
    });
    let vertices = fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let paths = &window.rendered_scene().paths;
            assert_eq!(paths.len(), 1);
            let path = &paths[0];
            assert_eq!(path.color.as_solid(), Some(rest));
            assert!(!path.vertices.is_empty());
            // Stroke tessellation may round a vertex by a few floating-point ulps.
            // This tolerance is far below a physical pixel and cannot hide an inset.
            assert!((path.bounds.origin.x.0 - body.bounds.origin.x.0).abs() < 0.0001);
            assert!((path.bounds.origin.y.0 - body.bounds.origin.y.0).abs() < 0.0001);
            assert!((path.bounds.size.width.0 - body.bounds.size.width.0).abs() < 0.0001);
            assert!((path.bounds.size.height.0 - body.bounds.size.height.0).abs() < 0.0001);
            path.vertices.clone()
        })
        .unwrap();
    fixture.hover(true);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let paths = &window.rendered_scene().paths;
            assert_eq!(paths.len(), 1);
            assert_eq!(paths[0].color.as_solid(), Some(hover));
            assert_eq!(paths[0].vertices, vertices);
        })
        .unwrap();
    assert_eq!(fixture.body().0.bounds, body.bounds);
}

#[test]
fn keyboard_focus_ring_is_three_pixels_with_one_pixel_gap_and_no_layout_shift() {
    let mut fixture = Fixture::new();
    let (body, scale) = fixture.body();
    fixture.key("tab");
    let (focused_body, _) = fixture.body();
    assert_eq!(body.bounds, focused_body.bounds);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| (quad.border_widths.left.0 / scale - 3.).abs() < 0.01)
                .expect("Ant focus-visible ring");
            assert_eq!(ring.bounds.origin.x.0 / scale, 8.);
            assert_eq!(ring.bounds.origin.y.0 / scale, 8.);
            assert_eq!(
                ring.bounds.size.width.0 / scale,
                body.bounds.size.width.0 / scale + 8.
            );
            assert_eq!(ring.bounds.size.height.0 / scale, 40.);
        })
        .unwrap();
}

#[test]
fn round_focus_radius_matches_the_visible_pill_including_controls_narrower_than_height() {
    for (size, width, label, expected_radius) in [
        (Size::Medium, None, "Save", 20.),
        (Size::Large, Some(36.), "i", 22.),
    ] {
        let mut fixture = Fixture::configured(|consumer| {
            consumer.shape = ButtonShape::Round;
            consumer.size = Some(size);
            consumer.custom_width = width;
            consumer.label = label;
        });
        let (body, scale) = fixture.body();
        if width.is_some() {
            assert!(body.bounds.size.width < body.bounds.size.height);
        }
        fixture.key("tab");
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                let ring = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| (quad.border_widths.left.0 / scale - 3.).abs() < 0.01)
                    .expect("pill focus outline");
                assert_eq!(ring.corner_radii.top_left.0 / scale, expected_radius);
                assert_eq!(ring.corner_radii.bottom_right.0 / scale, expected_radius);
            })
            .unwrap();
        assert_eq!(fixture.body().0.bounds, body.bounds);
    }
}

#[test]
fn pointer_hover_press_and_disabled_states_paint_the_reference_border_colors() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| consumer.second_visible = false);
    let colors = fixture.cx.update(|cx| {
        let config = fixture.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        (
            tokens.color_border.to_hsla(),
            tokens.color_primary_hover.to_hsla(),
            tokens.color_primary_active.to_hsla(),
            tokens.color_border_disabled.to_hsla(),
        )
    });
    assert_eq!(fixture.body().0.border_color, colors.0);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            window.dispatch_event(
                PlatformInput::MouseMove(gpui::MouseMoveEvent {
                    position: point(px(20.), px(24.)),
                    ..Default::default()
                }),
                cx,
            );
        })
        .unwrap();
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, colors.1);
    fixture.pointer(true, true);
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, colors.2);
    fixture.pointer(false, true);
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, colors.1);
    fixture.edit(|consumer| consumer.disabled = Some(true));
    assert_eq!(fixture.body().0.border_color, colors.3);
}

#[test]
fn a_pointer_press_leaves_and_reenters_active_state_while_held() {
    let mut fixture = Fixture::new();
    let original = fixture.body().0.border_color;
    fixture.hover(true);
    fixture.pointer(true, true);
    fixture.draw();
    let active = fixture.body().0.border_color;
    assert_ne!(active, original);
    fixture.hover(false);
    assert_eq!(fixture.body().0.border_color, original);
    fixture.hover(true);
    assert_eq!(fixture.body().0.border_color, active);
    fixture.pointer(false, true);
    fixture.draw();
    assert_eq!(fixture.activations(), vec![Activation::Pointer]);
}

#[test]
fn moving_outside_before_the_first_pressed_frame_cannot_leave_active_color_stuck() {
    let mut fixture = Fixture::new();
    let original = fixture.body().0.border_color;
    fixture.hover(true);
    fixture.pointer(true, true);
    fixture.mouse_move(false);
    // Both input events arrived before the first frame with retained press tracking.
    fixture.draw();
    fixture.frame(16);
    assert_eq!(fixture.body().0.border_color, original);
    fixture.pointer(false, false);
    fixture.draw();
    assert!(fixture.activations().is_empty());
}

#[test]
fn a_window_exit_clears_pointer_active_color_and_allows_reentry() {
    let mut fixture = Fixture::new();
    let original = fixture.body().0.border_color;
    fixture.hover(true);
    fixture.pointer(true, true);
    fixture.draw();
    let active = fixture.body().0.border_color;
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            window.dispatch_event(
                PlatformInput::MouseExited(gpui::MouseExitEvent {
                    position: point(px(20.), px(24.)),
                    pressed_button: Some(MouseButton::Left),
                    ..Default::default()
                }),
                cx,
            );
        })
        .unwrap();
    fixture.draw();
    fixture.frame(16);
    assert_eq!(fixture.body().0.border_color, original);
    fixture.hover(true);
    assert_eq!(fixture.body().0.border_color, active);
    fixture.pointer(false, true);
    fixture.draw();
    assert_eq!(fixture.activations(), vec![Activation::Pointer]);
}

#[test]
fn a_keyboard_press_keeps_active_color_when_the_pointer_leaves() {
    let mut fixture = Fixture::new();
    fixture.hover(true);
    fixture.key("tab");
    assert!(fixture.focused(false));
    fixture.key_event("space", true, false);
    fixture.draw();
    let active = fixture.body().0.border_color;
    fixture.hover(false);
    assert_eq!(fixture.body().0.border_color, active);
    fixture.key_event("space", false, false);
    fixture.draw();
    assert_eq!(
        fixture.activations(),
        vec![Activation::Keyboard(KeyboardButton::Space)]
    );
}

#[test]
fn loading_opacity_also_tints_the_keyboard_focus_outline() {
    let mut fixture = Fixture::configured(|consumer| consumer.loading = true);
    fixture.key("tab");
    let expected_alpha = fixture.cx.update(|cx| {
        let config = fixture.scope.snapshot(cx);
        config.theme.tokens.color_primary_border.a * config.theme.tokens.opacity_loading
    });
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| (quad.border_widths.left.0 / window.scale_factor() - 3.).abs() < 0.01)
                .expect("busy button focus ring");
            assert!((f64::from(ring.border_color.a) - expected_alpha).abs() < 1e-6);
        })
        .unwrap();
}

#[test]
fn loading_spinner_rotates_across_frames_through_the_public_element_path() {
    struct LoadingView;

    impl Render for LoadingView {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            Button::new("loading-spinner")
                .label("Loading")
                .loading(true)
        }
    }

    let mut cx = TestAppContext::single();
    let window = cx.add_window(|window, _| {
        window.activate_window();
        LoadingView
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.run_until_parked();
    let (tile, initial) = cx
        .update_window(window.into(), |_, window, _| {
            let sprite = &window.rendered_scene().monochrome_sprites[0];
            (sprite.tile.tile_id, sprite.transformation)
        })
        .unwrap();
    let mut previous = initial;
    // Eight 125 ms frames must complete one full turn, without input or host notifications.
    for frame in 1..=8 {
        cx.executor().advance_clock(Duration::from_millis(125));
        cx.update_window(window.into(), |_, window, cx| {
            assert_eq!(window.simulate_next_frame(cx), 1);
        })
        .unwrap();
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            let sprite = &window.rendered_scene().monochrome_sprites[0];
            let transform = sprite.transformation;
            assert_eq!(sprite.tile.tile_id, tile);
            assert_ne!(
                transform, previous,
                "spinner must rotate on frame {frame} without input events"
            );
            let angle = (frame % 8) as f32 * std::f32::consts::FRAC_PI_4;
            assert!((transform.rotation_scale[0][0] - angle.cos()).abs() < 1e-6);
            assert!((transform.rotation_scale[1][0] - angle.sin()).abs() < 1e-6);
            previous = transform;
        })
        .unwrap();
        cx.run_until_parked();
    }
}

#[test]
fn loading_reduced_motion_and_idle_buttons_do_not_schedule_animation_frames() {
    let mut fixture = Fixture::new();
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            assert_eq!(window.simulate_next_frame(cx), 0);
        })
        .unwrap();
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        cx.set_reduce_motion(true);
        scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
    });
    fixture.edit(|consumer| consumer.loading = true);
    assert!(fixture.semantics().is_busy());
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            assert_eq!(window.simulate_next_frame(cx), 0);
        })
        .unwrap();
}

#[test]
fn inserted_loader_expands_and_shrinks_smoothly_then_stops_scheduling_frames() {
    let mut fixture = Fixture::with_motion(true, |consumer| consumer.second_visible = false);
    let (body, scale) = fixture.body();
    let initial_width = body.bounds.size.width.0 / scale;
    fixture.edit(|consumer| consumer.loading = true);
    assert_eq!(fixture.body().0.bounds.size.width.0 / scale, initial_width);
    assert_eq!(fixture.frame(150), 1);
    let middle_width = fixture.body().0.bounds.size.width.0 / scale;
    assert!(middle_width > initial_width);
    assert_eq!(fixture.frame(150), 1);
    let expanded_width = fixture.body().0.bounds.size.width.0 / scale;
    assert!(expanded_width > middle_width);
    assert_eq!(expanded_width - initial_width, 22.);
    fixture.edit(|consumer| consumer.loading = false);
    assert!(!fixture.semantics().is_busy());
    assert_eq!(fixture.body().0.bounds.size.width.0 / scale, expanded_width);
    assert_eq!(fixture.frame(150), 1);
    let exiting_width = fixture.body().0.bounds.size.width.0 / scale;
    assert!(exiting_width > initial_width && exiting_width < expanded_width);
    assert_eq!(fixture.frame(150), 1);
    assert_eq!(fixture.body().0.bounds.size.width.0 / scale, initial_width);
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn replacing_an_existing_icon_preserves_width_and_a_static_loader_has_no_frame_loop() {
    let mut fixture = Fixture::with_motion(true, |consumer| {
        consumer.loading = true;
        consumer.custom_loader = Some(IconName::HeartTwoTone);
        consumer.icon = true;
        consumer.second_visible = false;
    });
    let busy_width = fixture.body().0.bounds.size.width;
    assert!(fixture.semantics().is_busy());
    assert_eq!(fixture.frame(16), 0);
    fixture.edit(|consumer| consumer.loading = false);
    assert_eq!(fixture.body().0.bounds.size.width, busy_width);
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn interrupting_a_color_transition_starts_from_the_current_painted_color() {
    let mut fixture = Fixture::with_motion(true, |consumer| consumer.second_visible = false);
    let original = fixture.body().0.border_color;
    fixture.hover(true);
    assert_eq!(fixture.frame(100), 1);
    let halfway = fixture.body().0.border_color;
    assert_ne!(halfway, original);
    fixture.pointer(true, true);
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, halfway);
    assert_eq!(fixture.frame(100), 1);
    let pressed = fixture.body().0.border_color;
    assert_ne!(pressed, halfway);
    fixture.pointer(false, false);
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, pressed);
    fixture.frame(200);
    fixture.hover(false);
    fixture.frame(200);
    assert_eq!(fixture.body().0.border_color, original);
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn a_wave_discarded_by_disabled_or_motion_changes_never_reappears() {
    let mut fixture = Fixture::with_motion(true, |consumer| consumer.second_visible = false);
    let wave_visible = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .any(|quad| quad.border_widths.left.0 / window.scale_factor() > 1.)
            })
            .unwrap()
    };
    fixture.click();
    fixture.frame(100);
    assert!(wave_visible(&mut fixture));
    fixture.edit(|consumer| consumer.disabled = Some(true));
    assert!(!wave_visible(&mut fixture));
    fixture.edit(|consumer| consumer.disabled = Some(false));
    fixture.frame(100);
    assert!(!wave_visible(&mut fixture));
    fixture.click();
    fixture.frame(100);
    assert!(wave_visible(&mut fixture));
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
    });
    fixture.draw();
    assert!(!wave_visible(&mut fixture));
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
    });
    fixture.draw();
    fixture.frame(100);
    assert!(!wave_visible(&mut fixture));
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn semantic_name_description_and_actions_belong_to_the_control() {
    let mut fixture = Fixture::new();
    let node = fixture.semantics();
    assert_eq!(node.role(), Role::Button);
    assert_eq!(node.label(), Some("Save"));
    assert_eq!(node.description(), Some("Save the current document"));
    assert_eq!(node.author_id(), Some("button-test.first"));
    assert!(node.supports_action(accesskit::Action::Click));
    assert!(node.supports_action(accesskit::Action::Focus));
    fixture.edit(|consumer| consumer.href = true);
    assert_eq!(fixture.semantics().role(), Role::Link);
    fixture.edit(|consumer| consumer.disabled = Some(true));
    assert!(
        !fixture
            .semantics()
            .supports_action(accesskit::Action::Click)
    );
    assert!(
        !fixture
            .semantics()
            .supports_action(accesskit::Action::Focus)
    );
}

#[test]
fn a_button_with_link_semantics_does_not_activate_on_space() {
    let mut fixture = Fixture::configured(|consumer| consumer.href = true);
    fixture.key("tab");
    assert!(fixture.focused(false));
    fixture.key("space");
    assert!(fixture.activations().is_empty());
    assert_eq!(fixture.semantics().role(), Role::Link);
}

#[test]
fn rtl_reverses_logical_icon_placement_without_changing_button_width() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| {
        consumer.icon = true;
        consumer.two_tone = true;
        consumer.second_visible = false;
    });
    let icon_x = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                let sprite = window
                    .rendered_scene()
                    .polychrome_sprites
                    .first()
                    .expect("button icon sprite");
                sprite.bounds.origin.x.0 / window.scale_factor()
            })
            .unwrap()
    };
    let initial_x = icon_x(&mut fixture);
    let (initial_body, _) = fixture.body();
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
            .unwrap();
    });
    fixture.draw();
    let rtl_x = icon_x(&mut fixture);
    assert!(rtl_x > initial_x);
    assert_eq!(fixture.body().0.bounds.size, initial_body.bounds.size);
    fixture.edit(|consumer| consumer.placement = IconPlacement::End);
    assert_eq!(icon_x(&mut fixture), initial_x);
}

#[test]
fn two_chinese_character_spacing_changes_visual_width_and_preserves_accessible_name() {
    let mut fixture = Fixture::new();
    fixture.edit(|consumer| {
        consumer.label = "保存";
        consumer.second_visible = false;
    });
    let (spaced, scale) = fixture.body();
    assert_eq!(fixture.semantics().label(), Some("保存"));
    fixture.edit(|consumer| consumer.auto_space = false);
    let (unspaced, _) = fixture.body();
    assert!(spaced.bounds.size.width.0 / scale > unspaced.bounds.size.width.0 / scale);
    fixture.edit(|consumer| {
        consumer.auto_space = true;
        consumer.icon = true;
    });
    let with_icon = fixture.body().0.bounds.size.width;
    fixture.edit(|consumer| consumer.auto_space = false);
    assert_eq!(fixture.body().0.bounds.size.width, with_icon);
}

#[test]
fn native_root_refinements_fill_parent_and_keep_focus_and_hit_testing_aligned() {
    let mut fixture = Fixture::configured(|consumer| {
        consumer.second_visible = false;
        consumer.style = StyleRefinement::default().w_full().rounded(px(12.));
    });
    let (body, scale) = fixture.body();
    assert_eq!(body.bounds.size.width.0 / scale, 276.);
    fixture.key("tab");
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| (quad.border_widths.left.0 / scale - 3.).abs() < 0.01)
                .unwrap();
            assert_eq!(ring.bounds.size.width.0 / scale, 284.);
            assert_eq!(ring.corner_radii.top_left.0 / scale, 16.);
            for down in [true, false] {
                let position = point(px(270.), px(24.));
                let event = if down {
                    PlatformInput::MouseDown(MouseDownEvent {
                        button: MouseButton::Left,
                        position,
                        ..Default::default()
                    })
                } else {
                    PlatformInput::MouseUp(MouseUpEvent {
                        button: MouseButton::Left,
                        position,
                        ..Default::default()
                    })
                };
                window.dispatch_event(event, cx);
            }
        })
        .unwrap();
    assert_eq!(fixture.activations(), [Activation::Pointer]);
}

#[test]
fn parent_flex_and_margin_refinements_apply_to_the_interactive_control() {
    let mut fixture = Fixture::configured(|consumer| {
        consumer.style = StyleRefinement::default().flex_1().ml(px(8.));
    });
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let mut surfaces: Vec<_> = window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    quad.border_widths.left.0 > 0. && quad.bounds.size.height.0 / scale == 32.
                })
                .collect();
            // GPUI may split one stroke into four clipped scene quads.
            surfaces.dedup_by_key(|quad| quad.bounds);
            assert_eq!(surfaces.len(), 2);
            assert_eq!(surfaces[0].bounds.origin.x.0 / scale, 20.);
            assert!(surfaces[0].bounds.size.width.0 / scale > 150.);
            assert_eq!(
                (surfaces[1].bounds.origin.x.0 + surfaces[1].bounds.size.width.0) / scale,
                288.
            );
        })
        .unwrap();
}

#[test]
fn refined_dashed_stroke_and_focus_use_the_painted_border_geometry() {
    let mut fixture = Fixture::configured(|consumer| {
        consumer.button_type = ButtonType::Dashed;
        consumer.second_visible = false;
        consumer.style = StyleRefinement::default().border(px(3.)).rounded(px(0.));
    });
    let (body, scale) = fixture.body();
    assert_eq!(body.corner_radii.top_left.0, 0.);
    fixture.key("tab");
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let path = &window.rendered_scene().paths[0];
            assert!((path.bounds.origin.x.0 - body.bounds.origin.x.0).abs() < 0.001);
            assert!((path.bounds.origin.y.0 - body.bounds.origin.y.0).abs() < 0.001);
            assert!(
                path.vertices.iter().any(|vertex| (vertex.xy_position.y.0
                    - (body.bounds.origin.y.0 + 3. * scale))
                    .abs()
                    < 0.001),
                "the fitted stroke must retain the custom three-pixel width"
            );
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.bounds.origin.x.0 < body.bounds.origin.x.0
                        && (quad.border_widths.left.0 / scale - 3.).abs() < 0.01
                })
                .unwrap();
            assert_eq!(ring.corner_radii.top_left.0 / scale, 4.);
        })
        .unwrap();
}

#[test]
fn semantic_icon_and_label_refinements_preserve_control_name_and_activation() {
    let red: gpui::Hsla = gpui::rgb(0xff0000).into();
    let green: gpui::Hsla = gpui::rgb(0x008000).into();
    let mut fixture = Fixture::configured(|consumer| {
        consumer.icon = true;
        consumer.second_visible = false;
        consumer.icon_style = Some(
            StyleRefinement::default()
                .text_color(red)
                .text_size(px(20.)),
        );
        consumer.content_style = Some(
            StyleRefinement::default()
                .text_color(green)
                .bg(green)
                .text_size(px(18.)),
        );
    });
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let sprites = &window.rendered_scene().monochrome_sprites;
            let icon = sprites.iter().find(|sprite| sprite.color == red).unwrap();
            assert_eq!(icon.bounds.size.width.0 / scale, 20.);
            assert!(
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .any(|quad| quad.background.as_solid() == Some(green))
                    || sprites.iter().any(|sprite| sprite.color == green)
                    || window
                        .rendered_scene()
                        .subpixel_sprites
                        .iter()
                        .any(|sprite| sprite.color == green)
            );
        })
        .unwrap();
    assert_eq!(fixture.semantics().label(), Some("Save"));
    fixture.key("tab");
    fixture.key("enter");
    assert_eq!(
        fixture.activations(),
        [Activation::Keyboard(KeyboardButton::Enter)]
    );
}

#[test]
fn inserted_loader_scales_reuses_its_atlas_tile_and_rotates_through_exit() {
    let mut fixture = Fixture::with_motion(true, |consumer| consumer.second_visible = false);
    fixture.edit(|consumer| consumer.loading = true);
    fixture.frame(150);
    let sprite = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                window.rendered_scene().monochrome_sprites[0]
            })
            .unwrap()
    };
    let entering = sprite(&mut fixture);
    let transform_scale = |sprite: gpui::MonochromeSprite| {
        let matrix = sprite.transformation.rotation_scale;
        matrix[0][0].hypot(matrix[1][0])
    };
    assert!(transform_scale(entering) > 0. && transform_scale(entering) < 1.);
    fixture.frame(150);
    let expanded = sprite(&mut fixture);
    assert_eq!(entering.tile.tile_id, expanded.tile.tile_id);
    assert!((transform_scale(expanded) - 1.).abs() < 1e-6);
    fixture.edit(|consumer| consumer.loading = false);
    fixture.frame(100);
    let departing = sprite(&mut fixture);
    assert_eq!(departing.tile.tile_id, expanded.tile.tile_id);
    assert_ne!(departing.transformation, expanded.transformation);
    assert!(transform_scale(departing) > 0. && transform_scale(departing) < 1.);
    let angle = departing.transformation.rotation_scale[1][0]
        .atan2(departing.transformation.rotation_scale[0][0]);
    assert!((angle - 0.8 * std::f32::consts::PI).abs() < 1e-5);
    fixture.frame(200);
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn custom_monochrome_loader_rotation_is_opt_in_and_respects_reduced_motion() {
    let mut fixture = Fixture::with_motion(true, |consumer| {
        consumer.loading = true;
        consumer.custom_loader = Some(IconName::SyncOutlined);
        consumer.spin_loader = Some(false);
        consumer.second_visible = false;
    });
    assert_eq!(fixture.frame(16), 0);
    fixture.edit(|consumer| consumer.spin_loader = Some(true));
    assert_eq!(fixture.frame(125), 1);
    fixture.cx.update(|cx| cx.set_reduce_motion(true));
    fixture.draw();
    fixture.frame(16);
    assert_eq!(fixture.frame(16), 0);
}

#[test]
fn disabling_waves_or_using_href_keeps_clicks_free_of_wave_frames() {
    for href in [false, true] {
        let mut fixture = Fixture::with_motion(true, |consumer| {
            consumer.href = href;
            consumer.wave = href;
            consumer.second_visible = false;
        });
        fixture.key("tab");
        fixture.key("enter");
        assert_eq!(
            fixture.activations(),
            [Activation::Keyboard(KeyboardButton::Enter)]
        );
        // A key press may have one final color frame pending; waves must not
        // keep requesting frames after the normal color transition settles.
        fixture.frame(200);
        assert_eq!(fixture.frame(16), 0);
    }
}

#[test]
fn custom_font_sizes_inherit_proportional_line_height_and_respect_fixed_icon_tokens() {
    let mut fixture = Fixture::configured(|consumer| {
        consumer.icon = true;
        consumer.second_visible = false;
        consumer.style = StyleRefinement::default().text_size(px(24.)).h_auto();
    });
    let (body, scale) = fixture.body();
    let expected = fixture.cx.update(|cx| {
        let line_height = fixture.scope.snapshot(cx).theme.tokens.line_height as f32;
        ((24. * line_height) * scale).round() / scale + 2.
    });
    assert_eq!(body.bounds.size.height.0 / scale, expected);
    fixture.edit(|consumer| consumer.label = "");
    let scope = fixture.scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch
                    .theme
                    .get_or_insert_default()
                    .components
                    .set::<ButtonTheme>(ButtonTokenPatch {
                        only_icon_size: Some(Some(20.)),
                        ..Default::default()
                    });
            })
            .unwrap();
    });
    fixture.draw();
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let sprite = &window.rendered_scene().monochrome_sprites[0];
            assert_eq!(sprite.bounds.size.width.0 / scale, 20.);
        })
        .unwrap();
    fixture
        .edit(|consumer| consumer.icon_style = Some(StyleRefinement::default().text_size(px(18.))));
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let sprite = &window.rendered_scene().monochrome_sprites[0];
            assert_eq!(sprite.bounds.size.width.0 / scale, 18.);
        })
        .unwrap();
}

#[test]
fn focus_and_waves_follow_individual_corners_and_custom_surface_colors() {
    let mut fixture = Fixture::with_motion(true, |consumer| {
        consumer.second_visible = false;
        consumer.style = StyleRefinement::default()
            .rounded_tl(px(0.))
            .rounded_tr(px(4.))
            .rounded_br(px(10.))
            .rounded_bl(px(14.))
            .border_color(gpui::rgb(0xff0000));
    });
    fixture.key("tab");
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| (quad.border_widths.left.0 / scale - 3.).abs() < 0.01)
                .unwrap();
            assert_eq!(ring.corner_radii.top_left.0 / scale, 4.);
            assert_eq!(ring.corner_radii.top_right.0 / scale, 8.);
            assert_eq!(ring.corner_radii.bottom_right.0 / scale, 14.);
            assert_eq!(ring.corner_radii.bottom_left.0 / scale, 18.);
        })
        .unwrap();
    fixture.key("enter");
    fixture.frame(100);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            let wave = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.border_color.h == 0.
                        && quad.border_color.s == 1.
                        && quad.border_color.a > 0.
                        && quad.border_color.a < 0.2
                })
                .unwrap();
            assert!(wave.corner_radii.top_left < wave.corner_radii.bottom_left);
        })
        .unwrap();
}

#[test]
fn subtle_text_button_hover_opacity_interpolates_without_palette_quantization() {
    let mut fixture = Fixture::with_motion(true, |consumer| {
        consumer.button_type = ButtonType::Text;
        consumer.second_visible = false;
    });
    fixture.hover(true);
    fixture.frame(30);
    let first = fixture.body().0.background.as_solid().unwrap().a;
    fixture.frame(10);
    let second = fixture.body().0.background.as_solid().unwrap().a;
    assert!(
        first > 0. && first < second && second < 0.01,
        "{first} -> {second}"
    );
}

#[test]
fn button_transparent_color_transitions_preserve_the_visible_hue() {
    let color = Color::rgb(0x1677ff);
    let visible = ButtonStateStyle {
        background: color,
        border: color,
        text: color,
    };
    let transparent = ButtonStateStyle {
        background: Color::TRANSPARENT,
        border: Color::TRANSPARENT,
        text: Color::TRANSPARENT,
    };
    for (from, to) in [(visible, transparent), (transparent, visible)] {
        let middle = interpolate_style(from, to, 0.5);
        assert_eq!(middle.background, color.with_alpha(0.5));
        assert_eq!(middle.border, color.with_alpha(0.5));
        assert_eq!(middle.text, color.with_alpha(0.5));
        assert_eq!(interpolate_style(from, to, 0.), from);
        assert_eq!(interpolate_style(from, to, 1.), to);
    }
}

#[test]
fn space_does_not_press_a_control_with_link_semantics() {
    let mut fixture = Fixture::configured(|consumer| consumer.href = true);
    fixture.key("tab");
    let rest = fixture.body().0.border_color;
    fixture.key_event("space", true, false);
    fixture.draw();
    assert_eq!(fixture.body().0.border_color, rest);
    fixture.key_event("space", false, false);
    fixture.draw();
    assert!(fixture.activations().is_empty());
}
