// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    ConfigScope, UiRoot,
    behavior::focus_handle,
    popup::{Popup, PopupTriggers},
};
use gpui::{
    AppContext, Context, Entity, FocusHandle, ParentElement, Render, RenderOnce, Role, Styled,
    TestAppContext, WindowHandle, canvas, div, point, prelude::*,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

#[derive(Clone, Copy, Debug)]
enum Flow {
    Block,
    Row,
    Column,
}

#[derive(IntoElement)]
struct RealTrigger {
    builds: Rc<Cell<usize>>,
    bounds: Rc<RefCell<Option<Bounds<Pixels>>>>,
    focus: FocusHandle,
    widths: [Option<Length>; 3],
}
impl RenderOnce for RealTrigger {
    fn render(self, _: &mut Window, _: &mut App) -> impl IntoElement {
        self.builds.set(self.builds.get() + 1);
        let measure = self.bounds;
        let focus = self.focus.clone();
        let mut trigger = div()
            .id("real-anchor-child")
            .relative()
            .min_w(px(48.))
            .h(px(32.))
            .track_focus(&self.focus)
            .role(Role::Button)
            .aria_label("Choose a color")
            .on_mouse_down(gpui::MouseButton::Left, move |_, window, cx| {
                focus.focus(window, cx)
            })
            .child(
                canvas(
                    move |bounds, _, _| *measure.borrow_mut() = Some(bounds),
                    |_, (), _, _| (),
                )
                .absolute()
                .inset_0(),
            );
        if let Some(width) = self.widths[0] {
            trigger.style().size.width = Some(width);
        }
        if let Some(min) = self.widths[1] {
            trigger.style().min_size.width = Some(min);
        }
        if let Some(max) = self.widths[2] {
            trigger.style().max_size.width = Some(max);
        }
        trigger
    }
}
struct Probe {
    scope: ConfigScope,
    flow: Flow,
    preferred: Option<Pixels>,
    correction: Pixels,
    widths: [Option<Length>; 3],
    builds: Rc<Cell<usize>>,
    bounds: Rc<RefCell<Option<Bounds<Pixels>>>>,
    focus: FocusHandle,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let child = RealTrigger {
            builds: self.builds.clone(),
            bounds: self.bounds.clone(),
            focus: self.focus.clone(),
            widths: self.widths,
        };
        let popup = Popup::new("intrinsic-popup")
            .scope(self.scope.clone())
            .open(true)
            .triggers(PopupTriggers::NONE)
            .child(
                IntrinsicAnchor::new(child, self.preferred, self.correction).with_widths(
                    self.widths[0],
                    self.widths[1],
                    self.widths[2],
                ),
            )
            .content(|_, _, _| div().w(px(80.)).h(px(40.)));
        let root = div().w(px(800.)).h(px(600.)).p(px(40.));
        match self.flow {
            Flow::Block => root.child(popup),
            Flow::Row => root.flex().items_start().child(popup),
            Flow::Column => root.flex().flex_col().child(popup),
        }
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    _probe: Entity<Probe>,
    builds: Rc<Cell<usize>>,
    bounds: Rc<RefCell<Option<Bounds<Pixels>>>>,
    focus: FocusHandle,
}
impl Fixture {
    fn new(flow: Flow, preferred: Option<Pixels>, correction: Pixels, scale: f32) -> Self {
        Self::with_widths(flow, preferred, correction, [None; 3], scale)
    }
    fn with_widths(
        flow: Flow,
        preferred: Option<Pixels>,
        correction: Pixels,
        widths: [Option<Length>; 3],
        scale: f32,
    ) -> Self {
        let mut cx = TestAppContext::single();
        let builds = Rc::new(Cell::new(0));
        let bounds = Rc::new(RefCell::new(None));
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let focus = focus_handle(cx);
            let probe = cx.new(|_| Probe {
                scope: scope.clone(),
                flow,
                preferred,
                correction,
                widths,
                builds: builds.clone(),
                bounds: bounds.clone(),
                focus: focus.clone(),
            });
            retained = Some((probe.clone(), focus));
            UiRoot::new(scope, probe, window, cx)
        });
        cx.simulate_window_scale_factor_change(window.into(), scale);
        cx.run_until_parked();
        builds.set(0);
        cx.update_window(window.into(), |_, window, cx| {
            window.refresh();
            window.draw(cx).clear(cx);
        })
        .unwrap();
        let (probe, focus) = retained.unwrap();
        Self {
            cx,
            window,
            _probe: probe,
            builds,
            bounds,
            focus,
        }
    }
}

#[test]
fn intrinsic_color_picker_anchor_resolves_authored_full_width_in_parent() {
    for flow in [Flow::Block, Flow::Row, Flow::Column] {
        for scale in [1., 1.5] {
            let f = Fixture::with_widths(
                flow,
                None,
                px(0.),
                [Some(gpui::relative(1.).into()), None, None],
                scale,
            );
            let bounds = f.bounds.borrow().unwrap();
            assert_eq!(bounds.size.width, px(720.), "flow={flow:?},scale={scale}");
            assert_eq!(bounds.size.height, px(32.));
            assert_eq!(f.builds.get(), 1);
        }
    }
}

#[test]
fn intrinsic_color_picker_anchor_preserves_authored_fixed_minimum_and_maximum_widths() {
    let cases = [
        ([Some(px(83.).into()), None, None], None, 83.),
        ([None, Some(px(96.).into()), None], None, 96.),
        ([None, None, Some(px(64.).into())], Some(px(97.140625)), 64.),
        (
            [Some(gpui::relative(1.).into()), None, Some(px(128.).into())],
            None,
            128.,
        ),
    ];
    for flow in [Flow::Block, Flow::Row, Flow::Column] {
        for scale in [1., 1.5] {
            for (widths, preferred, expected) in cases {
                let f = Fixture::with_widths(flow, preferred, px(0.), widths, scale);
                let bounds = f.bounds.borrow().unwrap();
                assert!(
                    (f32::from(bounds.size.width) - expected).abs() <= 1. / scale + 0.01,
                    "flow={flow:?},scale={scale},widths={widths:?},bounds={bounds:?}"
                );
                assert_eq!(bounds.size.height, px(32.));
                assert_eq!(f.builds.get(), 1);
            }
        }
    }
}

#[test]
fn intrinsic_color_picker_anchor_keeps_preferred_width_in_block_row_and_column() {
    for flow in [Flow::Block, Flow::Row, Flow::Column] {
        for scale in [1., 1.5, 2.] {
            let f = Fixture::new(flow, Some(px(97.140625)), px(0.), scale);
            let bounds = f.bounds.borrow().unwrap();
            let rounding_error = (f32::from(bounds.size.width) - 97.140625).abs();
            assert!(
                rounding_error <= 0.5 / scale + 0.01 && rounding_error <= 1.,
                "flow={flow:?},scale={scale},bounds={bounds:?}"
            );
            assert_eq!(bounds.size.height, px(32.));
            assert_eq!(
                f.builds.get(),
                1,
                "real child built once despite intrinsic/definite layout passes"
            );
        }
    }
}

#[test]
fn intrinsic_color_picker_anchor_correction_reaches_real_focus_box() {
    for flow in [Flow::Block, Flow::Row, Flow::Column] {
        let mut f = Fixture::new(flow, None, px(4. / 3.), 1.5);
        let bounds = f.bounds.borrow().unwrap();
        assert!(
            (f32::from(bounds.size.width) - (48. + 4. / 3.)).abs() < 0.02,
            "{bounds:?}"
        );
        let position = point(bounds.right() - px(0.2), bounds.center().y);
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.dispatch_event(
                gpui::PlatformInput::MouseDown(gpui::MouseDownEvent {
                    position,
                    button: gpui::MouseButton::Left,
                    click_count: 1,
                    ..Default::default()
                }),
                cx,
            );
        })
        .unwrap();
        f.cx.run_until_parked();
        assert!(
            f.cx.update_window(f.window.into(), |_, window, _| f.focus.is_focused(window))
                .unwrap()
        );
    }
}
