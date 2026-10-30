// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::state::{Control, Surface};
use super::*;
use crate::{Button, ConfigScope, Direction, UiRoot};
use gpui::{
    AppContext, Context, Entity, EntityInputHandler, FocusHandle, Focusable, IntoElement,
    KeyDownEvent, KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent,
    Pixels, PlatformInput, Point, Render, Subscription, TestAppContext, Window, WindowHandle, div,
    point, prelude::*, px,
};
use std::{cell::RefCell, rc::Rc};

fn blue() -> ColorPickerValue {
    PickerColor::parse("#1677ff").unwrap().into()
}
fn gradient() -> ColorPickerValue {
    ColorPickerValue::gradient([
        ColorStop::new(PickerColor::parse("#ff0000").unwrap(), 0.).unwrap(),
        ColorStop::new(PickerColor::parse("#0000ff").unwrap(), 100.).unwrap(),
    ])
    .unwrap()
}

struct Consumer {
    state: Entity<ColorPickerState>,
    scope: ConfigScope,
    shown: bool,
    initial_open: bool,
    disabled: bool,
    disabled_alpha: bool,
    allow_clear: bool,
    controlled: Option<ColorPickerValue>,
    accept: bool,
    accept_complete: bool,
    reentrant: bool,
    replace_on_change: Option<ColorPickerValue>,
    release_control_on_change: bool,
    modes: Vec<ColorPickerMode>,
    controlled_format: Option<ColorPickerFormat>,
    controlled_open: Option<bool>,
    custom_trigger: bool,
    custom_focus: FocusHandle,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().size_full().p(px(48.));
        if self.shown {
            let mut picker = ColorPicker::new("picker", &self.state)
                .scope(self.scope.clone())
                .default_open(self.initial_open)
                .disabled(self.disabled)
                .disabled_alpha(self.disabled_alpha)
                .modes(self.modes.clone())
                .allow_clear(self.allow_clear)
                .automation_id("test.color-picker")
                .on_change(cx.listener(|this, change: &ColorPickerChange, window, cx| {
                    if this.release_control_on_change {
                        this.controlled = None;
                        cx.notify();
                        return;
                    }
                    if let Some(value) = this.replace_on_change.clone() {
                        this.controlled = Some(value.clone());
                        this.state
                            .update(cx, |state, cx| state.set_value(value, window, cx).unwrap());
                        cx.notify();
                        return;
                    }
                    if this.accept && !this.accept_complete && this.controlled.is_some() {
                        this.controlled = Some(change.value.clone());
                        cx.notify();
                    }
                    if this.reentrant {
                        this.state.update(cx, |state, cx| {
                            state.set_value(change.value.clone(), window, cx).unwrap()
                        });
                    }
                }))
                .on_change_complete(cx.listener(|this, change: &ColorPickerChange, window, cx| {
                    if this.accept && this.accept_complete && this.controlled.is_some() {
                        this.controlled = Some(change.value.clone());
                        cx.notify();
                    }
                    if this.reentrant && this.accept_complete {
                        this.state.update(cx, |state, cx| {
                            state.set_value(change.value.clone(), window, cx).unwrap()
                        });
                    }
                }));
            if let Some(value) = &self.controlled {
                picker = picker.value(value.clone());
            }
            if let Some(format) = self.controlled_format {
                picker = picker.format(format);
            }
            if let Some(open) = self.controlled_open {
                picker = picker.open(open);
            }
            if self.custom_trigger {
                picker = picker.child(
                    Button::new("custom-trigger")
                        .focus_handle(&self.custom_focus)
                        .label("Open color"),
                );
            }
            root = root.child(picker);
        }
        root
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<ColorPickerState>,
    events: Rc<RefCell<Vec<ColorPickerEvent>>>,
    _subscription: Subscription,
}
impl Fixture {
    fn new(value: ColorPickerValue) -> Self {
        Self::with_open(value, true)
    }
    fn with_open(value: ColorPickerValue, open: bool) -> Self {
        let mut cx = TestAppContext::single();
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| {
                let mut state = ColorPickerState::new(window, cx);
                state.set_value(value, window, cx).unwrap();
                state
            });
            let subscription = cx.subscribe(&state, move |_, _, event, _| {
                output.borrow_mut().push(event.clone())
            });
            let consumer = cx.new(|cx| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                shown: true,
                initial_open: open,
                disabled: false,
                disabled_alpha: false,
                allow_clear: true,
                controlled: None,
                accept: false,
                accept_complete: false,
                reentrant: false,
                replace_on_change: None,
                release_control_on_change: false,
                modes: vec![ColorPickerMode::Single, ColorPickerMode::Gradient],
                controlled_format: None,
                controlled_open: None,
                custom_trigger: false,
                custom_focus: crate::behavior::focus_handle(cx),
            });
            retained = Some((consumer.clone(), state, subscription));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state, subscription) = retained.unwrap();
        let mut fixture = Self {
            cx,
            window,
            consumer,
            state,
            events,
            _subscription: subscription,
        };
        fixture.draw();
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }
    fn update<R>(
        &mut self,
        f: impl FnOnce(&mut ColorPickerState, &mut Window, &mut Context<ColorPickerState>) -> R,
    ) -> R {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |s, cx| f(s, window, cx))
            })
            .unwrap()
    }
    fn down(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
    }
    fn move_to(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseMove(MouseMoveEvent {
            position,
            pressed_button: Some(MouseButton::Left),
            ..Default::default()
        }));
    }
    fn up(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
    }
    fn click(&mut self, position: Point<Pixels>) {
        self.down(position);
        self.up(position);
    }
    fn key(&mut self, key: &'static str) {
        let keystroke = Keystroke::parse(key).unwrap();
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: keystroke.clone(),
            is_held: false,
            prefer_character_input: false,
        }));
        self.event(PlatformInput::KeyUp(KeyUpEvent { keystroke }));
    }
    fn changes(&self) -> Vec<ColorPickerChange> {
        self.events
            .borrow()
            .iter()
            .filter_map(|e| {
                if let ColorPickerEvent::Change(change) = e {
                    Some(change.clone())
                } else {
                    None
                }
            })
            .collect()
    }
    fn completions(&self) -> Vec<ColorPickerChange> {
        self.events
            .borrow()
            .iter()
            .filter_map(|e| {
                if let ColorPickerEvent::ChangeComplete(change) = e {
                    Some(change.clone())
                } else {
                    None
                }
            })
            .collect()
    }
    fn value(&self) -> ColorPickerValue {
        self.state
            .read_with(&self.cx, |state, _| state.value().clone())
    }
    fn geometry(&self) -> ColorPickerGeometry {
        self.state.read_with(&self.cx, |state, _| state.geometry())
    }
    fn focus(&mut self, control: Control) {
        self.update(|s, window, cx| s.control_focus(control, window, cx).focus(window, cx));
        self.draw();
    }
    fn edit_hex(&mut self, text: &str) {
        let hex = self.state.read_with(&self.cx, |s, _| s.hex.clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                hex.update(cx, |s, cx| {
                    s.select_all(window, cx);
                    s.replace_text_in_range(None, text, window, cx);
                });
            })
            .unwrap();
        self.draw();
    }
}

#[test]
fn color_picker_popup_opens_edits_and_escape_dismisses() {
    let mut f = Fixture::with_open(blue(), false);
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.click(f.geometry().trigger.unwrap().center());
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
    let g = f.geometry();
    assert_eq!(g.palette.unwrap().size, gpui::size(px(234.), px(160.)));
    assert_eq!(g.hue.unwrap().size.height, px(8.));
    f.click(g.palette.unwrap().center());
    assert_ne!(f.value(), blue());
    f.key("escape");
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    assert!(f.geometry().palette.is_none());
}

#[test]
fn color_picker_palette_capture_clamps_and_completes_once() {
    let mut f = Fixture::new(blue());
    let bounds = f.geometry().palette.unwrap();
    f.down(bounds.center());
    f.move_to(point(bounds.right() + px(80.), bounds.top() - px(80.)));
    f.up(point(bounds.right() + px(80.), bounds.top() - px(80.)));
    let (_, s, b, a) = f.value().active_color(0).to_hsb();
    assert_eq!((s, b, a), (1., 1., 1.));
    assert!(!f.changes().is_empty());
    assert_eq!(f.completions().len(), 1);
    assert_eq!(f.completions()[0].value, f.value());
}

#[test]
fn color_picker_hue_and_alpha_preserve_precise_achromatic_state() {
    let black = PickerColor::new(211.125, 0.6, 0., 0.73).unwrap();
    let mut f = Fixture::new(black.into());
    let hue = f.geometry().hue.unwrap();
    f.click(point(hue.left() + hue.size.width * 0.5, hue.center().y));
    let (h, s, b, a) = f.value().active_color(0).to_hsb();
    assert_eq!((h, s, b, a), (180., 0.6, 0., 0.73));
    let alpha = f.geometry().alpha.unwrap();
    f.click(point(
        alpha.left() + alpha.size.width * 0.25,
        alpha.center().y,
    ));
    assert_eq!(f.value().active_color(0).to_hsb(), (180., 0.6, 0., 0.25));
}

#[test]
fn color_picker_controlled_rejection_and_completion_acceptance_are_distinct() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(blue());
        cx.notify();
    });
    f.draw();
    let palette = f.geometry().palette.unwrap();
    f.down(palette.center());
    assert_eq!(f.value(), blue());
    f.up(palette.center());
    assert_eq!(f.value(), blue());
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
        blue()
    );
    f.consumer.update(&mut f.cx, |s, cx| {
        s.accept = true;
        s.accept_complete = true;
        cx.notify();
    });
    f.draw();
    f.down(palette.center());
    assert_eq!(f.value(), blue());
    f.up(palette.center());
    assert_ne!(f.value(), blue());
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
        f.value()
    );
}

#[test]
fn color_picker_callbacks_can_update_the_same_retained_controller() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.reentrant = true;
        cx.notify();
    });
    f.draw();
    f.click(f.geometry().palette.unwrap().center());
    assert_ne!(f.value(), blue());
    assert_eq!(f.completions().len(), 1);
}

#[test]
fn color_picker_reentrant_controlled_echo_keeps_the_drag_active() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(blue());
        s.accept = true;
        s.reentrant = true;
        cx.notify();
    });
    f.draw();
    let bounds = f.geometry().palette.unwrap();
    f.down(bounds.center());
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_some()));
    f.move_to(point(bounds.right() - px(4.), bounds.top() + px(4.)));
    f.up(point(bounds.right() - px(4.), bounds.top() + px(4.)));
    assert_eq!(f.changes().len(), 2);
    assert_eq!(f.completions().len(), 1);
}

#[test]
fn color_picker_keyboard_and_accessibility_edit_values_without_duplicate_completion() {
    let mut f = Fixture::new(blue());
    f.focus(Control::Hue);
    let before = f.value().active_color(0).to_hsb().0;
    f.key("right");
    assert_eq!(f.value().active_color(0).to_hsb().0, before + 1.);
    assert_eq!(f.completions().len(), 1);
    f.update(|s, w, cx| s.accessible_value(Control::Alpha, 45., w, cx));
    f.draw();
    assert_eq!(f.value().active_color(0).to_hsb().3, 0.45);
    assert_eq!(f.completions().len(), 2);
}

#[test]
fn color_picker_hex_drafts_and_format_switch_preserve_editors() {
    let mut f = Fixture::new(blue());
    let editors = f.state.read_with(&f.cx, |s, _| s.retained_editor_ids());
    f.edit_hex("ab");
    assert_eq!(f.value(), blue());
    f.edit_hex("ff0000");
    assert_eq!(f.value().active_color(0).to_hex_string(), "#ff0000");
    f.update(|s, w, cx| s.change_format(ColorPickerFormat::Hsb, w, cx));
    f.draw();
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.format()),
        ColorPickerFormat::Hsb
    );
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.retained_editor_ids()),
        editors
    );
    assert_eq!(f.value().active_color(0).to_hex_string(), "#ff0000");
}

#[test]
fn color_picker_gradient_insertion_drag_crossing_and_deletion_keep_identity() {
    let mut f = Fixture::new(gradient());
    let rail = f.geometry().gradient.unwrap();
    f.click(rail.center());
    let (id, count) = f.state.read_with(&f.cx, |s, _| {
        (
            s.model.stops()[s.model.active_index()].id,
            s.model.stops().len(),
        )
    });
    assert_eq!(count, 3);
    assert_eq!(f.changes().len(), 1);
    assert_eq!(f.completions().len(), 1);
    let start = point(rail.center().x, rail.center().y);
    f.update(|s, w, cx| s.pointer_start(Surface::Gradient, Some(id), start, w, cx));
    f.draw();
    f.move_to(point(rail.right(), rail.center().y));
    f.up(point(rail.right(), rail.center().y));
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.model.stops()[s.model.active_index()].id),
        id
    );
    f.focus(Control::Stop(id));
    f.key("delete");
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.model.stops().len()), 2);
    f.key("delete");
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.model.stops().len()), 2);
}

#[test]
fn color_picker_disable_unmount_and_scope_updates_cancel_capture() {
    let mut f = Fixture::new(blue());
    let rail = f.geometry().hue.unwrap();
    f.down(rail.center());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.disabled = true;
        cx.notify();
    });
    f.draw();
    let value = f.value();
    f.move_to(point(rail.left(), rail.center().y));
    f.up(rail.center());
    assert_eq!(f.value(), value);
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.consumer.update(&mut f.cx, |s, cx| {
        s.shown = false;
        cx.notify();
    });
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert!(f.geometry().palette.is_none());
}

#[test]
fn color_picker_clear_presets_and_disabled_alpha_share_the_color_model() {
    let mut f = Fixture::new(blue());
    f.update(|s, w, cx| s.clear(w, cx));
    f.draw();
    assert!(f.value().is_cleared());
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, ColorPickerEvent::Clear))
    );
    f.consumer.update(&mut f.cx, |s, cx| {
        s.disabled_alpha = true;
        cx.notify();
    });
    f.draw();
    f.update(|s, w, cx| s.choose_preset(PickerColor::parse("#ff000080").unwrap().into(), w, cx));
    f.draw();
    assert_eq!(f.value().active_color(0).to_hsb().3, 1.);
    assert!(f.geometry().alpha.is_none());
    let scope = f.consumer.read_with(&f.cx, |s, _| s.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |p| {
                p.direction = Some(Direction::Rtl);
                p.locale.locale = Some(crate::locale::Locale::zh_tw());
            })
            .unwrap()
    });
    f.draw();
    assert_eq!(f.value().active_color(0).to_hex_string(), "#ff0000");
}

#[test]
fn color_picker_invalid_external_stops_are_rejected_before_mutation() {
    let mut f = Fixture::new(blue());
    let invalid = ColorPickerValue::Gradient(
        vec![ColorStop {
            color: PickerColor::WHITE,
            percent: f64::NAN,
        }]
        .into(),
    );
    assert!(f.update(|s, w, cx| s.set_value(invalid, w, cx)).is_err());
    assert_eq!(f.value(), blue());
    assert!(f.changes().is_empty());
    f.update(|s, w, cx| s.set_value(ColorPickerValue::Gradient(Vec::new().into()), w, cx))
        .unwrap();
    assert_eq!(f.value(), ColorPickerValue::Cleared);
    f.update(|s, w, cx| {
        s.set_value(
            ColorPickerValue::Gradient(
                vec![
                    ColorStop::new(PickerColor::WHITE, 100.).unwrap(),
                    ColorStop::new(PickerColor::BLACK, 0.).unwrap(),
                ]
                .into(),
            ),
            w,
            cx,
        )
    })
    .unwrap();
    assert!(
        matches!(f.value(), ColorPickerValue::Gradient(ref stops) if stops[0].percent == 0. && stops[1].percent == 100.)
    );
    assert!(f.changes().is_empty());
}

#[test]
fn color_picker_dynamic_stops_release_old_focus_identities() {
    let mut f = Fixture::new(gradient());
    for index in 0..48 {
        f.update(|s, w, cx| {
            let c = PickerColor::new(f64::from(index), 1., 1., 1.).unwrap();
            s.set_value(
                ColorPickerValue::gradient([
                    ColorStop::new(c, 0.).unwrap(),
                    ColorStop::new(c, 100.).unwrap(),
                ])
                .unwrap(),
                w,
                cx,
            )
            .unwrap();
            for id in s
                .model
                .stops()
                .iter()
                .map(|stop| stop.id)
                .collect::<Vec<_>>()
            {
                s.control_focus(Control::Stop(id), w, cx);
            }
            s.retain_controls(&[], w, cx);
            assert!(
                s.focuses.len() <= 8,
                "removed stop focus handles must be dropped"
            );
        });
    }
    assert!(f.changes().is_empty());
}

#[test]
fn color_picker_gradient_only_and_cleared_virtual_stops_materialize_on_edit() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.modes = vec![ColorPickerMode::Gradient];
        cx.notify();
    });
    f.draw();
    assert_eq!(f.value(), blue());
    assert!(f.changes().is_empty());
    assert!(f.geometry().gradient.is_some());
    f.update(|s, w, cx| s.clear(w, cx));
    f.draw();
    assert!(f.value().is_cleared());
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.model.stops().len()), 2);
    f.click(f.geometry().gradient.unwrap().center());
    assert!(matches!(f.value(), ColorPickerValue::Gradient(ref stops) if stops.len() == 3));
}

#[test]
fn color_picker_outside_press_closes_without_emitting_a_color_change() {
    let mut f = Fixture::new(blue());
    f.click(point(px(600.), px(500.)));
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    assert_eq!(f.value(), blue());
    assert!(f.changes().is_empty());
}

#[test]
fn color_picker_nested_format_selector_keeps_the_panel_open() {
    let mut f = Fixture::new(blue());
    let selector = f.state.read_with(&f.cx, |s, _| s.format_select.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        let focus = selector.read(cx).focus_handle(cx);
        focus.focus(window, cx);
    })
    .unwrap();
    f.draw();
    f.key("down");
    f.key("down");
    f.key("enter");
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.format()),
        ColorPickerFormat::Rgb
    );
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, ColorPickerEvent::FormatChange(ColorPickerFormat::Rgb)))
    );
    assert_eq!(
        f.geometry()
            .editors
            .iter()
            .filter(|b| b.size.width > px(0.))
            .count(),
        4
    );
}

#[test]
fn color_picker_controlled_format_can_reject_a_user_proposal() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled_format = Some(ColorPickerFormat::Hex);
        cx.notify();
    });
    f.draw();
    f.update(|s, w, cx| s.change_format(ColorPickerFormat::Hsb, w, cx));
    f.draw();
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.format()),
        ColorPickerFormat::Hex
    );
    assert_eq!(f.value(), blue());
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, ColorPickerEvent::FormatChange(ColorPickerFormat::Hsb)))
    );
    let selector = f.state.read_with(&f.cx, |s, _| s.format_select.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        let focus = selector.read(cx).focus_handle(cx);
        focus.focus(window, cx);
    })
    .unwrap();
    f.draw();
    f.key("down");
    f.key("down");
    f.key("enter");
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.format()),
        ColorPickerFormat::Hex
    );
    assert_eq!(
        selector.read_with(&f.cx, |s, _| s.values()[0].value),
        ColorPickerFormat::Hex
    );
}

#[test]
fn color_picker_custom_button_opens_from_enter_and_space_and_restores_focus() {
    let mut f = Fixture::with_open(blue(), false);
    f.consumer.update(&mut f.cx, |s, cx| {
        s.custom_trigger = true;
        cx.notify();
    });
    f.draw();
    let focus = f.consumer.read_with(&f.cx, |s, _| s.custom_focus.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| focus.focus(window, cx))
        .unwrap();
    f.draw();
    f.key("enter");
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.key("escape");
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(focus.is_focused(window))
    })
    .unwrap();
    f.key("space");
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
}

#[test]
fn color_picker_default_trigger_opens_from_enter_and_space() {
    let mut f = Fixture::with_open(blue(), false);
    f.update(|s, w, cx| s.focus(w, cx));
    f.draw();
    f.key("enter");
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.key("escape");
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.key("space");
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
}

#[test]
fn color_picker_cleared_keyboard_and_accessibility_use_the_pointer_fill_policy() {
    let mut f = Fixture::new(ColorPickerValue::Cleared);
    f.focus(Control::Hue);
    f.key("right");
    assert_eq!(f.value().active_color(0).to_hsb(), (1., 1., 1., 1.));
    f.update(|s, w, cx| s.clear(w, cx));
    f.draw();
    f.update(|s, w, cx| s.accessible_value(Control::Hue, 120., w, cx));
    f.draw();
    assert_eq!(f.value().active_color(0).to_hsb(), (120., 1., 1., 1.));
    f.update(|s, w, cx| s.clear(w, cx));
    f.draw();
    f.focus(Control::Alpha);
    f.key("right");
    assert_eq!(f.value().active_color(0).to_hsb(), (0., 1., 1., 0.01));
    f.update(|s, w, cx| s.clear(w, cx));
    f.draw();
    f.update(|s, w, cx| s.accessible_value(Control::Alpha, 25., w, cx));
    f.draw();
    assert_eq!(f.value().active_color(0).to_hsb(), (0., 1., 1., 0.25));
}

#[test]
fn color_picker_disabling_alpha_during_drag_cancels_its_completion() {
    let mut f = Fixture::new(PickerColor::parse("#1677ff80").unwrap().into());
    let bounds = f.geometry().alpha.unwrap();
    f.down(bounds.center());
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_some()));
    f.consumer.update(&mut f.cx, |s, cx| {
        s.disabled_alpha = true;
        cx.notify();
    });
    f.draw();
    f.up(bounds.center());
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert!(f.completions().is_empty());
}

#[test]
fn color_picker_external_open_and_format_updates_are_silent() {
    let mut f = Fixture::with_open(blue(), false);
    f.update(|s, w, cx| s.set_open(true, w, cx));
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.is_open()));
    f.update(|s, w, cx| s.set_format(ColorPickerFormat::Rgb, w, cx));
    f.draw();
    f.update(|s, w, cx| s.set_open(false, w, cx));
    f.draw();
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    assert!(f.events.borrow().is_empty());
}

#[test]
fn color_picker_rejected_gradient_keyboard_edits_keep_stop_identity_and_focus() {
    let mut f = Fixture::new(gradient());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(gradient());
        cx.notify();
    });
    f.draw();
    let ids = f.state.read_with(&f.cx, |s, _| {
        s.model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()
    });
    f.focus(Control::Stop(ids[1]));
    f.key("left");
    assert_eq!(f.value(), gradient());
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s
            .model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()),
        ids
    );
    let focus = f
        .state
        .read_with(&f.cx, |s, _| s.focuses[&Control::Stop(ids[1])].clone());
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(focus.is_focused(window))
    })
    .unwrap();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.accept = true;
        cx.notify();
    });
    f.draw();
    f.key("left");
    assert_ne!(f.value(), gradient());
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s
            .model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()),
        ids
    );
}

#[test]
fn color_picker_controlled_drag_rollback_honors_the_latest_external_value() {
    let mut f = Fixture::new(gradient());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(gradient());
        cx.notify();
    });
    f.draw();
    let rail = f.geometry().gradient.unwrap();
    let id = f.state.read_with(&f.cx, |s, _| s.model.stops()[1].id);
    f.update(|s, w, cx| s.pointer_start(Surface::Gradient, Some(id), rail.center(), w, cx));
    f.draw();
    let external = ColorPickerValue::gradient([
        ColorStop::new(PickerColor::WHITE, 0.).unwrap(),
        ColorStop::new(PickerColor::BLACK, 100.).unwrap(),
    ])
    .unwrap();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(external.clone());
        cx.notify();
    });
    f.draw();
    f.up(rail.center());
    assert_eq!(f.value(), external);
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
        external
    );
}

#[test]
fn color_picker_controlled_unmount_rolls_back_gradient_with_original_ids() {
    let mut f = Fixture::new(gradient());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(gradient());
        cx.notify();
    });
    f.draw();
    let ids = f.state.read_with(&f.cx, |s, _| {
        s.model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()
    });
    let bounds = f.geometry().gradient.unwrap();
    f.update(|s, w, cx| s.pointer_start(Surface::Gradient, Some(ids[1]), bounds.center(), w, cx));
    f.draw();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.shown = false;
        cx.notify();
    });
    f.draw();
    assert_eq!(f.value(), gradient());
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
        gradient()
    );
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s
            .model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()),
        ids
    );
}

#[test]
fn color_picker_empty_numeric_channel_updates_that_color_channel_to_zero() {
    let mut f = Fixture::new(blue());
    f.update(|s, w, cx| s.set_format(ColorPickerFormat::Rgb, w, cx));
    f.draw();
    let editor = f
        .state
        .read_with(&f.cx, |s, cx| s.channels[0].read(cx).editor.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        editor.update(cx, |s, cx| {
            s.select_all(window, cx);
            s.replace_text_in_range(None, "", window, cx);
        })
    })
    .unwrap();
    f.draw();
    assert_eq!(f.value().active_color(0).to_rgb(), (0, 119, 255, 1.));
}

#[test]
fn color_picker_mode_constraints_deduplicate_and_keep_reference_order() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.allow_clear = false;
        s.modes = vec![ColorPickerMode::Single, ColorPickerMode::Single];
        cx.notify();
    });
    f.draw();
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.modes.clone()),
        [ColorPickerMode::Single]
    );
    assert!(f.geometry().clear.is_none());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.modes = vec![
            ColorPickerMode::Gradient,
            ColorPickerMode::Single,
            ColorPickerMode::Gradient,
        ];
        cx.notify();
    });
    f.draw();
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.modes.clone()),
        [ColorPickerMode::Single, ColorPickerMode::Gradient]
    );
    assert!(f.geometry().clear.is_some());
}

#[test]
fn color_picker_owner_close_during_drag_cancels_hidden_capture() {
    let mut f = Fixture::new(blue());
    f.down(f.geometry().palette.unwrap().center());
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_some()));
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled_open = Some(false);
        cx.notify();
    });
    f.draw();
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_open()));
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert!(f.geometry().palette.is_none());
    f.up(point(px(10.), px(10.)));
    assert!(f.completions().is_empty());
}

#[test]
fn color_picker_owner_value_replacement_cancels_an_active_proposal() {
    let mut f = Fixture::new(blue());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(blue());
        cx.notify();
    });
    f.draw();
    f.down(f.geometry().palette.unwrap().center());
    let external: ColorPickerValue = PickerColor::parse("#722ed1").unwrap().into();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(external.clone());
        cx.notify();
    });
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert_eq!(f.value(), external);
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
        external
    );
}

#[test]
fn color_picker_owner_mode_constraints_cancel_drag_and_drop_hidden_stop_focus() {
    let mut f = Fixture::new(gradient());
    let id = f.state.read_with(&f.cx, |s, _| s.model.stops()[1].id);
    let bounds = f.geometry().gradient.unwrap();
    f.update(|s, w, cx| s.pointer_start(Surface::Gradient, Some(id), bounds.center(), w, cx));
    f.draw();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.modes = vec![ColorPickerMode::Single];
        cx.notify();
    });
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.mode()),
        ColorPickerMode::Single
    );
    assert!(f.state.read_with(&f.cx, |s, _| {
        !s.focuses
            .keys()
            .any(|control| matches!(control, Control::Stop(_)))
    }));
    assert!(f.geometry().gradient.is_none());
}

fn discrete_gradient() -> ColorPickerValue {
    ColorPickerValue::gradient([
        ColorStop::new(PickerColor::parse("#ff0000").unwrap(), 0.).unwrap(),
        ColorStop::new(PickerColor::parse("#00ff00").unwrap(), 50.).unwrap(),
        ColorStop::new(PickerColor::parse("#0000ff").unwrap(), 100.).unwrap(),
    ])
    .unwrap()
}

fn discrete_fixture(edit: &str) -> (Fixture, Vec<u64>, FocusHandle) {
    let mut f = Fixture::new(discrete_gradient());
    f.consumer.update(&mut f.cx, |s, cx| {
        s.controlled = Some(discrete_gradient());
        cx.notify();
    });
    f.draw();
    let ids = f.state.read_with(&f.cx, |s, _| {
        s.model
            .stops()
            .iter()
            .map(|stop| stop.id)
            .collect::<Vec<_>>()
    });
    f.focus(Control::Stop(ids[1]));
    if edit == "hex" || edit == "channel" {
        if edit == "channel" {
            f.update(|s, w, cx| s.set_format(ColorPickerFormat::Rgb, w, cx));
        }
        f.update(|s, w, cx| s.focus_input(0, w, cx));
        f.draw();
    } else if edit == "clear" {
        f.focus(Control::Clear);
    }
    let focused =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.focused(cx).expect("discrete edit has focus")
        })
        .unwrap();
    (f, ids, focused)
}

fn perform_discrete_edit(f: &mut Fixture, edit: &str, ids: &[u64]) {
    match edit {
        "hex" => f.edit_hex("22cc66"),
        "channel" => {
            let editor = f
                .state
                .read_with(&f.cx, |s, cx| s.channels[0].read(cx).editor.clone());
            f.cx.update_window(f.window.into(), |_, window, cx| {
                editor.update(cx, |s, cx| {
                    s.select_all(window, cx);
                    s.replace_text_in_range(None, "128", window, cx);
                });
            })
            .unwrap();
            f.draw();
        }
        "clear" => {
            f.update(|s, w, cx| s.clear(w, cx));
            f.draw();
        }
        "mode" => {
            f.update(|s, w, cx| s.choose_mode(ColorPickerMode::Single, w, cx));
            f.draw();
        }
        "preset" => {
            f.update(|s, w, cx| s.choose_preset(blue(), w, cx));
            f.draw();
        }
        "accessibility" => {
            f.update(|s, w, cx| s.accessible_value(Control::Stop(ids[1]), 60., w, cx));
            f.draw();
        }
        "delete" => f.key("delete"),
        _ => panic!("unknown discrete edit"),
    }
}

#[test]
fn color_picker_controlled_discrete_rejection_preserves_gradient_ids_and_focus() {
    for edit in [
        "hex",
        "channel",
        "clear",
        "mode",
        "preset",
        "accessibility",
        "delete",
    ] {
        let (mut f, ids, focused) = discrete_fixture(edit);
        perform_discrete_edit(&mut f, edit, &ids);
        assert_eq!(
            f.value(),
            discrete_gradient(),
            "accepted value after {edit}"
        );
        f.state.read_with(&f.cx, |s, _| {
            assert_eq!(
                s.preview_value(),
                &discrete_gradient(),
                "preview after {edit}"
            );
            assert_eq!(
                s.model
                    .stops()
                    .iter()
                    .map(|stop| stop.id)
                    .collect::<Vec<_>>(),
                ids,
                "stop IDs after {edit}"
            );
            assert_eq!(s.model.active_index(), 1, "active stop after {edit}");
        });
        f.cx.update_window(f.window.into(), |_, window, _| {
            assert!(focused.is_focused(window), "focus after rejected {edit}");
        })
        .unwrap();
        assert_eq!(f.changes().len(), 1, "change count for {edit}");
        assert_eq!(f.completions().len(), 1, "completion count for {edit}");
    }
}

#[test]
fn color_picker_controlled_discrete_acceptance_supports_completion_and_reentry() {
    for (complete, reentrant) in [(false, false), (true, false), (true, true)] {
        for edit in [
            "hex",
            "channel",
            "clear",
            "mode",
            "preset",
            "accessibility",
            "delete",
        ] {
            let (mut f, ids, _) = discrete_fixture(edit);
            f.consumer.update(&mut f.cx, |s, cx| {
                s.accept = true;
                s.accept_complete = complete;
                s.reentrant = reentrant;
                cx.notify();
            });
            f.draw();
            perform_discrete_edit(&mut f, edit, &ids);
            assert_eq!(f.completions().len(), 1, "completion count for {edit}");
            let accepted = f.completions()[0].value.clone();
            assert_ne!(accepted, discrete_gradient(), "proposal for {edit}");
            assert_eq!(f.value(), accepted, "owner acceptance for {edit}");
            f.state.read_with(&f.cx, |s, _| {
                assert_eq!(s.preview_value(), &accepted, "accepted preview for {edit}");
                if matches!(edit, "hex" | "channel" | "accessibility") {
                    assert_eq!(
                        s.model
                            .stops()
                            .iter()
                            .map(|stop| stop.id)
                            .collect::<Vec<_>>(),
                        ids,
                        "accepted IDs for {edit}"
                    );
                } else if edit == "delete" {
                    assert_eq!(
                        s.model
                            .stops()
                            .iter()
                            .map(|stop| stop.id)
                            .collect::<Vec<_>>(),
                        [ids[0], ids[2]],
                        "remaining accepted IDs"
                    );
                }
            });
        }
    }
}

#[test]
fn color_picker_controlled_discrete_rollback_honors_owner_replacement_and_ownership_loss() {
    let replacement = ColorPickerValue::gradient([
        ColorStop::new(PickerColor::WHITE, 0.).unwrap(),
        ColorStop::new(PickerColor::BLACK, 100.).unwrap(),
    ])
    .unwrap();
    for edit in [
        "hex",
        "channel",
        "clear",
        "mode",
        "preset",
        "accessibility",
        "delete",
    ] {
        let (mut f, ids, _) = discrete_fixture(edit);
        f.consumer.update(&mut f.cx, |s, cx| {
            s.replace_on_change = Some(replacement.clone());
            cx.notify();
        });
        f.draw();
        perform_discrete_edit(&mut f, edit, &ids);
        assert_eq!(f.value(), replacement, "owner replacement after {edit}");
        assert_eq!(
            f.state.read_with(&f.cx, |s, _| s.preview_value().clone()),
            replacement,
            "preview replacement after {edit}"
        );
    }
    let (mut f, ids, _) = discrete_fixture("hex");
    f.consumer.update(&mut f.cx, |s, cx| {
        s.release_control_on_change = true;
        cx.notify();
    });
    f.draw();
    perform_discrete_edit(&mut f, "hex", &ids);
    let proposal = f.changes()[0].value.clone();
    assert_eq!(f.value(), discrete_gradient());
    f.update(|s, w, cx| s.set_value(proposal.clone(), w, cx).unwrap());
    f.draw();
    assert_eq!(f.value(), proposal);
    assert_ne!(
        f.state.read_with(&f.cx, |s, _| s.model.stops()[1].id),
        ids[1],
        "ownership loss discards the old completion snapshot before a later independent setter"
    );
}

#[test]
fn color_picker_completion_only_owner_echo_preserves_pointer_and_key_stop_ids() {
    for input in ["pointer", "keyboard"] {
        let (mut f, ids, focused) = discrete_fixture("accessibility");
        f.consumer.update(&mut f.cx, |s, cx| {
            s.accept = true;
            s.accept_complete = true;
            cx.notify();
        });
        f.draw();
        if input == "pointer" {
            let rail = f.geometry().gradient.unwrap();
            let position = point(rail.left() + rail.size.width * 0.6, rail.center().y);
            f.update(|s, w, cx| s.pointer_start(Surface::Gradient, Some(ids[1]), position, w, cx));
            f.draw();
            f.up(position);
        } else {
            f.key("right");
        }
        assert_eq!(f.completions().len(), 1, "completion count for {input}");
        assert_eq!(
            f.value(),
            f.completions()[0].value,
            "owner echo for {input}"
        );
        f.state.read_with(&f.cx, |s, _| {
            assert_eq!(
                s.model
                    .stops()
                    .iter()
                    .map(|stop| stop.id)
                    .collect::<Vec<_>>(),
                ids,
                "completion-only stop IDs for {input}"
            );
            assert_eq!(s.model.active_index(), 1);
        });
        f.cx.update_window(f.window.into(), |_, window, _| {
            assert!(focused.is_focused(window), "completion focus for {input}");
        })
        .unwrap();
    }
}
