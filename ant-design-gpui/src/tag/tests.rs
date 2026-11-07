// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Bounds, Context, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput,
    Point, Render, StyleRefinement, TestAppContext, Window, WindowHandle, point, size,
};

use super::*;
use crate::{Button, ConfigPatch, UiRoot};

const ROOT_COLOR: Color = Color::rgb(0x13579b);
const CLOSE_COLOR: Color = Color::rgb(0xfedcba);

struct Consumer {
    scope: ConfigScope,
    focus: FocusHandle,
    close_focus: FocusHandle,
    next_focus: FocusHandle,
    clicks: Rc<RefCell<Vec<Activation>>>,
    closes: Rc<RefCell<Vec<Activation>>>,
    changes: Rc<RefCell<Vec<TagChange>>>,
    visible: bool,
    actionable: bool,
    closable: bool,
    close_disabled: bool,
    disabled: Option<bool>,
    prevent_close: bool,
    href: bool,
    checked: Option<bool>,
    external_focus: bool,
    label: &'static str,
    custom_close: bool,
    icon: bool,
    style: StyleRefinement,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: focus_handle(cx),
            close_focus: focus_handle(cx),
            next_focus: focus_handle(cx),
            clicks: Rc::default(),
            closes: Rc::default(),
            changes: Rc::default(),
            visible: true,
            actionable: false,
            closable: false,
            close_disabled: false,
            disabled: None,
            prevent_close: false,
            href: false,
            checked: None,
            external_focus: true,
            label: "Tag",
            custom_close: false,
            icon: false,
            style: StyleRefinement::default().bg(ROOT_COLOR),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut row = div()
            .flex()
            .items_start()
            .p(px(12.))
            .gap(px(20.))
            .w(px(400.));
        if self.visible {
            let clicks = self.clicks.clone();
            let closes = self.closes.clone();
            let changes = self.changes.clone();
            let prevent = self.prevent_close;
            let mut tag = if let Some(checked) = self.checked {
                CheckableTag::new("tag-test-first")
                    .checked(checked)
                    .on_change(move |change, _, _| changes.borrow_mut().push(*change))
                    .0
            } else {
                Tag::new("tag-test-first")
            }
            .scope(self.scope.clone())
            .label(self.label)
            .close_disabled(self.close_disabled)
            .close_style(StyleRefinement::default().bg(CLOSE_COLOR))
            .automation_id("tag-test.first")
            .close_automation_id("tag-test.close")
            .on_close(move |event, _, _| {
                closes.borrow_mut().push(event.activation);
                if prevent {
                    event.prevent_default();
                }
            });
            if self.custom_close {
                tag = tag.close_icon(IconName::CloseCircleOutlined);
            } else {
                tag = tag.closable(self.closable);
            }
            if self.actionable {
                tag = tag.on_click(move |activation, _, _| clicks.borrow_mut().push(*activation));
            }
            if self.href {
                tag = tag.href("https://example.com/tag");
            }
            if self.icon {
                tag = tag.icon(IconName::CheckCircleOutlined);
            }
            if let Some(disabled) = self.disabled {
                tag = tag.disabled(disabled);
            }
            if self.external_focus {
                tag = tag
                    .focus_handle(&self.focus)
                    .close_focus_handle(&self.close_focus);
            }
            tag.style().refine(&self.style);
            // Render once inside the consumer, like the component's normal immediate builder.
            row = row.child(tag.render(window, cx));
        }
        row.child(
            Button::new("tag-test-next")
                .scope(self.scope.clone())
                .label("Next")
                .focus_handle(&self.next_focus),
        )
    }
}

struct CachedPanel(Entity<Consumer>);

impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(400.)).h(px(200.))),
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
    fn new(configure: impl FnOnce(&mut Consumer)) -> Self {
        Self::with_motion(false, configure)
    }

    fn with_motion(motion: bool, configure: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let tag_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut consumer = Consumer::new(tag_scope.clone(), cx);
                configure(&mut consumer);
                consumer
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(tag_scope);
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

    fn bounds(&mut self, color: Color) -> Option<Bounds<Pixels>> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background.as_solid() == Some(color.to_hsla()))
                    .map(|quad| {
                        Bounds::new(
                            point(
                                px(quad.bounds.origin.x.0 / scale),
                                px(quad.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(quad.bounds.size.width.0 / scale),
                                px(quad.bounds.size.height.0 / scale),
                            ),
                        )
                    })
            })
            .unwrap()
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

    fn pointer(&mut self, down: bool, position: Point<Pixels>) {
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

    fn click(&mut self, close: bool) {
        let bounds = self
            .bounds(if close { CLOSE_COLOR } else { ROOT_COLOR })
            .expect("visible Tag slot");
        let position = if close {
            bounds.center()
        } else {
            point(bounds.left() + px(3.), bounds.center().y)
        };
        self.pointer(true, position);
        self.pointer(false, position);
        self.draw();
    }

    fn hover(&mut self, inside: bool) {
        let position = if inside {
            self.bounds(ROOT_COLOR).unwrap().center()
        } else {
            point(px(390.), px(190.))
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseMove(MouseMoveEvent {
                        position,
                        ..Default::default()
                    }),
                    cx,
                )
            })
            .unwrap();
        self.draw();
    }

    fn focus(&mut self, close: bool) {
        let focus = self.consumer.read_with(&self.cx, |consumer, _| {
            if close {
                consumer.close_focus.clone()
            } else {
                consumer.focus.clone()
            }
        });
        self.cx
            .update_window(self.window.into(), |_, window, cx| window.focus(&focus, cx))
            .unwrap();
        self.draw();
    }

    fn focused(&mut self, index: usize) -> bool {
        let focus = self.consumer.read_with(&self.cx, |consumer, _| {
            [
                consumer.focus.clone(),
                consumer.close_focus.clone(),
                consumer.next_focus.clone(),
            ][index]
                .clone()
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }

    fn clicks(&self) -> Vec<Activation> {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.clicks.borrow().clone())
    }

    fn closes(&self) -> Vec<Activation> {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.closes.borrow().clone())
    }

    fn changes(&self) -> Vec<TagChange> {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.changes.borrow().clone())
    }

    fn frame(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        let count = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        count
    }
}

#[test]
fn tag_close_pointer_never_activates_its_root_or_link() {
    let mut f = Fixture::new(|c| {
        c.actionable = true;
        c.closable = true;
        c.href = true;
    });
    f.click(true);
    assert_eq!(f.closes(), [Activation::Pointer]);
    assert!(f.clicks().is_empty());
    assert!(f.cx.opened_url().is_none());
    assert!(f.bounds(ROOT_COLOR).is_none());
}

#[test]
fn tag_close_can_be_prevented_and_dismissal_survives_rerenders_until_remount() {
    let mut f = Fixture::new(|c| {
        c.closable = true;
        c.prevent_close = true;
    });
    f.click(true);
    assert!(f.bounds(ROOT_COLOR).is_some());
    f.edit(|c| c.prevent_close = false);
    f.click(true);
    assert_eq!(f.closes().len(), 2);
    assert!(f.bounds(ROOT_COLOR).is_none());
    f.edit(|c| c.label = "Different text");
    assert!(f.bounds(ROOT_COLOR).is_none());
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    assert!(f.bounds(ROOT_COLOR).is_some());
}

#[test]
fn tag_disabled_close_is_isolated_from_an_enabled_root() {
    let mut f = Fixture::new(|c| {
        c.actionable = true;
        c.closable = true;
        c.close_disabled = true;
    });
    f.click(true);
    assert!(f.closes().is_empty());
    assert!(f.clicks().is_empty());
    assert!(f.bounds(ROOT_COLOR).is_some());
    f.click(false);
    assert_eq!(f.clicks(), [Activation::Pointer]);
    f.focus(false);
    f.key("tab");
    assert!(f.focused(2), "disabled close leaves the tab order");
}

#[test]
fn tag_root_close_and_next_have_distinct_keyboard_focus_and_activation() {
    let mut f = Fixture::new(|c| {
        c.actionable = true;
        c.closable = true;
        c.prevent_close = true;
    });
    f.key("tab");
    assert!(f.focused(0));
    f.key("enter");
    assert_eq!(f.clicks(), [Activation::Keyboard(KeyboardButton::Enter)]);
    f.key("tab");
    assert!(f.focused(1));
    f.key_event("space", true, false);
    f.key_event("space", true, true);
    f.key_event("space", false, false);
    f.draw();
    assert_eq!(f.closes(), [Activation::Keyboard(KeyboardButton::Space)]);
    assert_eq!(
        f.clicks().len(),
        1,
        "close keyboard input does not bubble to root"
    );
    f.key("enter");
    assert_eq!(f.closes().len(), 2);
    f.key("tab");
    assert!(f.focused(2));
}

#[test]
fn tag_static_content_is_not_a_tab_stop_and_custom_close_enables_closing() {
    let mut f = Fixture::new(|_| {});
    f.key("tab");
    assert!(f.focused(2));
    let mut f = Fixture::new(|c| c.custom_close = true);
    f.key("tab");
    assert!(f.focused(1));
    f.key("enter");
    assert!(f.bounds(ROOT_COLOR).is_none());
}

#[test]
fn tag_controlled_checkable_uses_space_and_waits_for_its_owner() {
    let mut f = Fixture::new(|c| c.checked = Some(false));
    f.key("tab");
    f.key("enter");
    f.key("ctrl-space");
    assert!(f.changes().is_empty());
    f.key_event("space", true, false);
    f.key_event("space", true, true);
    f.key_event("space", false, false);
    f.draw();
    f.key("space");
    assert_eq!(f.changes().len(), 2);
    assert!(f.changes().iter().all(|change| change.checked));
    f.edit(|c| c.checked = Some(true));
    f.click(false);
    assert_eq!(
        f.changes().last().unwrap(),
        &TagChange {
            checked: false,
            activation: Activation::Pointer
        }
    );
}

#[test]
fn tag_links_activate_on_enter_and_pointer_with_disabled_provider_override() {
    let mut f = Fixture::new(|c| c.href = true);
    f.key("tab");
    assert!(f.focused(0));
    f.key("space");
    assert!(f.cx.opened_url().is_none());
    f.key("enter");
    assert_eq!(
        f.cx.opened_url().as_deref(),
        Some("https://example.com/tag")
    );

    let mut disabled = Fixture::new(|c| {
        c.href = true;
        c.closable = true;
        c.disabled = Some(true);
    });
    disabled.click(false);
    disabled.click(true);
    assert!(disabled.cx.opened_url().is_none());
    assert!(disabled.closes().is_empty());
    disabled.key("tab");
    assert!(disabled.focused(2));
    let scope = disabled.scope.clone();
    disabled
        .cx
        .update(|cx| scope.update_patch(cx, |p| p.disabled = Some(true)).unwrap());
    disabled.edit(|c| c.disabled = Some(false));
    disabled.click(false);
    assert_eq!(
        disabled.cx.opened_url().as_deref(),
        Some("https://example.com/tag")
    );
}

#[test]
fn tag_cached_provider_disable_roundtrip_cancels_pending_root_and_close_keys() {
    let mut f = Fixture::new(|c| {
        c.actionable = true;
        c.closable = true;
    });
    for close in [false, true] {
        f.focus(close);
        f.key_event("space", true, false);
        let scope = f.scope.clone();
        for disabled in [true, false] {
            f.cx.update(|cx| {
                scope
                    .update_patch(cx, |p| p.disabled = Some(disabled))
                    .unwrap()
            });
            f.cx.run_until_parked();
        }
        f.key_event("space", false, false);
        f.draw();
    }
    assert!(f.clicks().is_empty());
    assert!(f.closes().is_empty());
    assert!(f.bounds(ROOT_COLOR).is_some());
}

#[test]
fn tag_native_bounds_follow_reference_geometry_styled_size_and_rtl_close_order() {
    let mut f = Fixture::new(|c| {
        c.closable = true;
        c.icon = true;
    });
    let root = f.bounds(ROOT_COLOR).unwrap();
    let close_ltr = f.bounds(CLOSE_COLOR).unwrap();
    assert_eq!(root.origin, point(px(12.), px(12.)));
    assert_eq!(root.size.height, px(22.));
    assert_eq!(close_ltr.size, size(px(10.), px(10.)));
    assert_eq!(close_ltr.center().y, root.center().y);
    let scope = f.scope.clone();
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    let rtl = f.bounds(ROOT_COLOR).unwrap();
    let close_rtl = f.bounds(CLOSE_COLOR).unwrap();
    assert_eq!(rtl.size, root.size);
    assert!(close_rtl.left() < close_ltr.left());
    f.edit(|c| {
        c.style = StyleRefinement::default()
            .bg(ROOT_COLOR)
            .w(px(180.))
            .h(px(36.))
    });
    assert_eq!(f.bounds(ROOT_COLOR).unwrap().size, size(px(180.), px(36.)));
}

#[test]
fn tag_fixed_line_box_survives_font_size_refinements_and_empty_content_collapses() {
    let mut f = Fixture::new(|c| {
        c.style = StyleRefinement::default().bg(ROOT_COLOR).text_size(px(18.));
    });
    assert_eq!(f.bounds(ROOT_COLOR).unwrap().size.height, px(22.));
    f.edit(|c| c.label = "");
    assert_eq!(f.bounds(ROOT_COLOR).unwrap().size, size(px(16.), px(2.)));
    f.edit(|c| c.icon = true);
    assert_eq!(f.bounds(ROOT_COLOR).unwrap().size.height, px(22.));
    f.edit(|c| {
        c.icon = false;
        c.closable = true;
    });
    assert_eq!(f.bounds(ROOT_COLOR).unwrap().size.height, px(22.));
}

#[test]
fn tag_internal_focus_survives_label_changes_and_disabled_state_removes_focus() {
    let mut f = Fixture::new(|c| {
        c.actionable = true;
        c.external_focus = false;
    });
    f.key("tab");
    let focus =
        f.cx.update_window(f.window.into(), |_, window, cx| window.focused(cx).unwrap())
            .unwrap();
    f.edit(|c| c.label = "Updated label");
    assert!(
        f.cx.update_window(f.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    );
    f.edit(|c| c.disabled = Some(true));
    assert!(
        !f.cx
            .update_window(f.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    );
    f.key("tab");
    assert!(f.focused(2));
}

#[test]
fn tag_idle_and_reduced_motion_do_not_schedule_animation_frames() {
    let mut static_tag = Fixture::with_motion(true, |_| {});
    assert_eq!(static_tag.frame(16), 0);
    let mut link = Fixture::with_motion(true, |c| c.href = true);
    link.click(false);
    assert_eq!(
        link.frame(16),
        0,
        "a link alone does not opt into a click wave"
    );
    let mut f = Fixture::new(|c| c.actionable = true);
    assert_eq!(f.frame(16), 0);
    f.click(false);
    assert_eq!(f.frame(16), 0);
    let mut animated = Fixture::with_motion(true, |c| c.actionable = true);
    assert_eq!(animated.frame(16), 0);
    animated.click(false);
    assert_eq!(animated.frame(16), 1);
    animated.frame(2100);
    assert_eq!(animated.frame(16), 0);
    animated.cx.update(|cx| cx.set_reduce_motion(true));
    animated.click(false);
    assert_eq!(animated.frame(16), 0);
}

#[test]
fn tag_checkable_color_transitions_settle_and_snap_under_reduced_motion() {
    let mut f = Fixture::with_motion(true, |c| c.checked = Some(false));
    f.hover(true);
    assert_eq!(f.frame(16), 1);
    f.frame(250);
    assert_eq!(f.frame(16), 0);
    f.edit(|c| c.checked = Some(true));
    assert_eq!(f.frame(16), 1);
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0);
}

#[test]
fn tag_visual_retargeting_is_continuous_and_transparency_uses_premultiplied_srgb() {
    let style = TagStyle::resolve(
        &crate::theme::ResolvedTheme::default(),
        TagColor::Default,
        TagVariant::Filled,
        Some(false),
        false,
    );
    let mut visual = VisualTransition::default();
    let motion = MotionPolicy::new(true, false);
    let duration = Duration::from_millis(200);
    visual.resolve(style.rest, Duration::ZERO, duration, motion);
    visual.resolve(style.hover, Duration::ZERO, duration, motion);
    let halfway = visual.sample(Duration::from_millis(100));
    let (retargeted, pending) =
        visual.resolve(style.rest, Duration::from_millis(100), duration, motion);
    assert_eq!(retargeted, halfway);
    assert!(pending);
    let (snapped, pending) = visual.resolve(
        style.rest,
        Duration::from_millis(110),
        duration,
        MotionPolicy::new(true, true),
    );
    assert_eq!(snapped, style.rest);
    assert!(!pending);
    let primary = Color::rgb(0x1677ff).to_hsla();
    let mixed = mix(primary, Color::TRANSPARENT.to_hsla(), 0.5).to_rgb();
    assert!((mixed.a - 0.5).abs() < 1e-6);
    assert!((mixed.r - primary.to_rgb().r).abs() < 1e-6);
    assert!((mixed.g - primary.to_rgb().g).abs() < 1e-6);
    assert!((mixed.b - primary.to_rgb().b).abs() < 1e-6);
}

#[test]
fn tag_close_request_prevention_and_explicit_closability_are_independent() {
    let event = TagClose {
        activation: Activation::Accessibility,
        prevented: Cell::new(false),
    };
    assert!(!event.default_prevented());
    event.prevent_default();
    event.prevent_default();
    assert!(event.default_prevented());
    assert!(!Tag::new("plain").can_close());
    assert!(
        Tag::new("custom")
            .close_icon(IconName::CloseCircleOutlined)
            .can_close()
    );
    assert!(
        !Tag::new("explicit")
            .close_icon(IconName::CloseCircleOutlined)
            .closable(false)
            .can_close()
    );
}
