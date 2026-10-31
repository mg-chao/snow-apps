// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::UiRoot;
use gpui::{
    AppContext, Context, MouseMoveEvent, PlatformInput, Render, TestAppContext, WindowHandle,
};
use std::time::Duration;

struct Probe {
    scope: ConfigScope,
    state: Entity<ColorPickerState>,
    shown: bool,
    styles: ColorPickerStyles,
    disabled: bool,
}
struct OtherWindow;
impl Render for OtherWindow {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
    }
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().size_full().p(px(40.));
        if self.shown {
            root = root.child(
                ColorPicker::new("motion-picker", &self.state)
                    .scope(self.scope.clone())
                    .disabled(self.disabled)
                    .styles(self.styles.clone()),
            );
        }
        root
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    state: Entity<ColorPickerState>,
}
impl Fixture {
    fn new(motion: bool, reduced: bool, styles: ColorPickerStyles) -> Self {
        let mut cx = TestAppContext::single();
        cx.update(|cx| cx.set_reduce_motion(reduced));
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(motion)).unwrap();
            let state = cx.new(|cx| ColorPickerState::new(window, cx));
            let probe = cx.new(|_| Probe {
                scope: scope.clone(),
                state: state.clone(),
                shown: true,
                styles,
                disabled: false,
            });
            retained = Some((probe.clone(), state));
            UiRoot::new(scope, probe, window, cx)
        });
        let (probe, state) = retained.unwrap();
        let mut fixture = Self {
            cx,
            window,
            probe,
            state,
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
    fn frame(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        let callbacks = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        callbacks
    }
    fn hover(&mut self, hovered: bool) {
        let bounds = self
            .state
            .read_with(&self.cx, |s, _| s.geometry().trigger.unwrap());
        let position = if hovered {
            bounds.center()
        } else {
            point(px(500.), px(500.))
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseMove(MouseMoveEvent {
                        position,
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn border(&mut self) -> gpui::Hsla {
        let bounds = self
            .state
            .read_with(&self.cx, |s, _| s.geometry().trigger.unwrap());
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|q| {
                        q.border_widths.left.0 > 0.
                            && (q.bounds.origin.x.0 / scale - f32::from(bounds.left())).abs() < 0.1
                            && (q.bounds.origin.y.0 / scale - f32::from(bounds.top())).abs() < 0.1
                    })
                    .expect("painted trigger border")
                    .border_color
            })
            .unwrap()
    }
}

#[test]
fn color_picker_trigger_hover_motion_is_interruptible_and_has_no_idle_frames() {
    let mut f = Fixture::new(true, false, Default::default());
    let initial = f.border();
    assert_eq!(f.frame(16), 0, "mount is static");
    f.hover(true);
    assert_eq!(
        f.border(),
        initial,
        "transition starts from the visible color"
    );
    assert_eq!(f.frame(80), 1);
    let middle = f.border();
    assert_ne!(middle, initial);
    f.hover(false);
    assert_eq!(f.border(), middle, "reversal has no jump");
    f.frame(300);
    assert_eq!(f.border(), initial);
    assert_eq!(f.frame(16), 0);
}

#[test]
fn color_picker_trigger_motion_respects_reduced_motion_and_scoped_disable() {
    for (motion, reduced) in [(false, false), (true, true)] {
        let mut f = Fixture::new(motion, reduced, Default::default());
        let initial = f.border();
        f.hover(true);
        assert_ne!(f.border(), initial);
        assert_eq!(f.frame(16), 0);
    }
    let mut f = Fixture::new(true, false, Default::default());
    f.hover(true);
    f.frame(40);
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0);
}

#[test]
fn color_picker_disabling_hovered_trigger_returns_to_disabled_border() {
    let mut f = Fixture::new(true, false, Default::default());
    let border = f.border();
    f.hover(true);
    f.frame(300);
    assert_ne!(f.border(), border);
    f.probe.update(&mut f.cx, |p, cx| {
        p.disabled = true;
        cx.notify();
    });
    f.draw();
    f.frame(300);
    assert_eq!(f.border(), border);
    assert_eq!(f.frame(16), 0);
}

#[test]
fn color_picker_trigger_authored_color_and_gradient_overrides_are_preserved() {
    let gradient = gpui::linear_gradient(
        90.,
        gpui::linear_color_stop(gpui::rgb(0xff0000), 0.),
        gpui::linear_color_stop(gpui::rgb(0x0000ff), 1.),
    );
    let mut f = Fixture::new(
        true,
        false,
        ColorPickerStyles {
            root: div()
                .border_color(gpui::rgb(0x008000))
                .bg(gradient)
                .style()
                .clone(),
            ..Default::default()
        },
    );
    let initial = f.border();
    assert_eq!(initial, gpui::Hsla::from(gpui::rgb(0x008000)));
    f.hover(true);
    assert_eq!(f.border(), initial);
    assert_eq!(f.frame(16), 0);
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(
            window
                .rendered_scene()
                .quads
                .iter()
                .any(|q| q.background == gradient)
        )
    })
    .unwrap();
}

#[test]
fn color_picker_trigger_animation_cancels_on_hide_and_unmount() {
    let mut f = Fixture::new(true, false, Default::default());
    f.hover(true);
    f.frame(40);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0);
    let mut f = Fixture::new(true, false, Default::default());
    f.hover(true);
    f.frame(40);
    let _other = f.cx.add_window(|window, _| {
        window.activate_window();
        OtherWindow
    });
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0, "inactive windows do not keep scheduling");
    let mut f = Fixture::new(true, false, Default::default());
    f.hover(true);
    f.frame(40);
    f.probe.update(&mut f.cx, |p, cx| {
        p.shown = false;
        cx.notify();
    });
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0);
    assert!(
        f.state
            .read_with(&f.cx, |s, _| s.geometry().trigger.is_none())
    );
}
