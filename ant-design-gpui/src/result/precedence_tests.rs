// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    rc::Rc,
    sync::{Arc, Mutex},
};

use gpui::{
    AppContext, Background, Bounds, Context, Entity, IntoElement, Pixels, Render, StyleRefinement,
    TestAppContext, Window, WindowHandle, div, point, prelude::*, px, rgb, size,
};

use super::{Result, ResultStatus, ResultStyleContext, ResultStyleResolver, ResultStyles};
use crate::{ConfigPatch, ConfigScope, Direction, UiRoot};

const ICON_MARKER: u32 = 0x5a1729;
const SCOPE: u32 = 0x183951;
const SCOPE_RESOLVER: u32 = 0x294a62;
const INSTANCE: u32 = 0x3a5b73;
const INSTANCE_RESOLVER: u32 = 0x4b6c84;
const NATIVE: u32 = 0x5c7d95;

struct Probe {
    scope: ConfigScope,
    status: ResultStatus,
    instance_top: Option<f32>,
    instance_resolver: bool,
    native_top: Option<f32>,
    show_icon: bool,
    content: bool,
    calls: Rc<RefCell<Vec<ResultStyleContext>>>,
}

impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut result = Result::new("result-precedence")
            .scope(self.scope.clone())
            .status(self.status)
            .show_icon(self.show_icon)
            .styles(ResultStyles {
                root: self
                    .instance_top
                    .map_or_else(StyleRefinement::default, |top| {
                        StyleRefinement::default().pt(px(top)).bg(rgb(INSTANCE))
                    }),
                icon: StyleRefinement::default().bg(rgb(ICON_MARKER)),
                ..Default::default()
            });
        if self.content {
            result = result
                .title("Result title")
                .sub_title("Result description")
                .extra(div().w(px(40.)).h(px(32.)))
                .child(div().h(px(12.)));
        }
        if self.instance_resolver {
            let calls = self.calls.clone();
            result = result.styles_fn(move |context, _, _| {
                calls.borrow_mut().push(*context);
                let top = if context.status == ResultStatus::Success {
                    41.
                } else {
                    40.
                };
                ResultStyles {
                    root: StyleRefinement::default()
                        .pt(px(top))
                        .bg(rgb(INSTANCE_RESOLVER)),
                    ..Default::default()
                }
            });
        }
        if let Some(top) = self.native_top {
            result = result.pt(px(top)).bg(rgb(NATIVE));
        }
        div().w(px(700.)).child(result)
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    scope: ConfigScope,
    scoped_calls: Arc<Mutex<Vec<ResultStyleContext>>>,
    instance_calls: Rc<RefCell<Vec<ResultStyleContext>>>,
}

impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let scoped_calls = Arc::new(Mutex::new(Vec::new()));
        let instance_calls = Rc::new(RefCell::new(Vec::new()));
        let mut scope = None;
        let mut probe = None;
        let calls = scoped_calls.clone();
        let local_calls = instance_calls.clone();
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let component_scope = root_scope.child(ConfigPatch::default(), cx).unwrap();
            component_scope
                .update_patch(cx, |patch| {
                    patch.result.styles.root = StyleRefinement::default().p(px(10.)).bg(rgb(SCOPE));
                    patch.result.styles_fn =
                        Some(ResultStyleResolver::new(move |context, _, _| {
                            calls.lock().unwrap().push(*context);
                            let top = if context.status == ResultStatus::Success {
                                21.
                            } else {
                                20.
                            };
                            ResultStyles {
                                root: StyleRefinement::default()
                                    .pt(px(top))
                                    .bg(rgb(SCOPE_RESOLVER)),
                                ..Default::default()
                            }
                        }));
                })
                .unwrap();
            let view = cx.new(|_| Probe {
                scope: component_scope.clone(),
                status: ResultStatus::Info,
                instance_top: Some(30.),
                instance_resolver: true,
                native_top: Some(50.),
                show_icon: true,
                content: true,
                calls: local_calls,
            });
            scope = Some(component_scope);
            probe = Some(view.clone());
            UiRoot::new(root_scope, view, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            scope: scope.unwrap(),
            probe: probe.unwrap(),
            scoped_calls,
            instance_calls,
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

    fn edit(&mut self, edit: impl FnOnce(&mut Probe)) {
        self.probe.update(&mut self.cx, |probe, cx| {
            edit(probe);
            cx.notify();
        });
        self.draw();
    }

    fn patch(&mut self, edit: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, edit).unwrap());
        self.draw();
    }

    fn bounds(&mut self, color: u32) -> Bounds<Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let background: Background = rgb(color).into();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background == background)
                    .unwrap_or_else(|| panic!("rendered marker {color:06x}"));
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
            .unwrap()
    }

    fn assert_top(&mut self, top: f32, color: u32) {
        assert_eq!(self.bounds(ICON_MARKER).top(), px(top));
        let root = self.bounds(color);
        assert_eq!(root.top(), px(0.));
        assert_eq!(root.size.width, px(700.));
        assert_eq!(
            self.bounds(ICON_MARKER).left(),
            px(10.),
            "unrelated scoped padding survives"
        );
    }
}

#[test]
fn result_rendered_semantic_styles_follow_all_five_layers_and_preserve_explicit_zero() {
    let mut f = Fixture::new();
    f.assert_top(50., NATIVE);
    f.edit(|probe| probe.native_top = Some(0.));
    f.assert_top(0., NATIVE);
    f.edit(|probe| probe.native_top = None);
    f.assert_top(40., INSTANCE_RESOLVER);
    f.edit(|probe| probe.instance_resolver = false);
    f.assert_top(30., INSTANCE);
    f.edit(|probe| probe.instance_top = Some(0.));
    f.assert_top(0., INSTANCE);
    f.edit(|probe| probe.instance_top = None);
    f.assert_top(20., SCOPE_RESOLVER);
    f.patch(|patch| patch.result.styles_fn = None);
    f.assert_top(10., SCOPE);
    f.patch(|patch| {
        patch.result.styles.root = StyleRefinement::default()
            .p(px(10.))
            .pt(px(0.))
            .bg(rgb(SCOPE))
    });
    f.assert_top(0., SCOPE);
    f.patch(|patch| patch.result.styles = ResultStyles::default());
    assert_eq!(f.bounds(ICON_MARKER).top(), px(48.));
    assert_eq!(f.bounds(ICON_MARKER).left(), px(32.));
}

#[test]
fn result_rendered_resolvers_receive_effective_status_direction_and_slot_presence() {
    let mut f = Fixture::new();
    f.edit(|probe| {
        probe.native_top = None;
        probe.instance_top = None;
    });
    f.assert_top(40., INSTANCE_RESOLVER);
    f.edit(|probe| probe.status = ResultStatus::Success);
    f.assert_top(41., INSTANCE_RESOLVER);
    let scoped = *f.scoped_calls.lock().unwrap().last().unwrap();
    let instance = *f.instance_calls.borrow().last().unwrap();
    assert_eq!(scoped, instance);
    assert_eq!(instance.status, ResultStatus::Success);
    assert!(
        instance.has_title
            && instance.has_sub_title
            && instance.has_extra
            && instance.has_body
            && instance.show_icon
    );
    f.edit(|probe| probe.instance_resolver = false);
    f.assert_top(21., SCOPE_RESOLVER);
    f.patch(|patch| patch.direction = Some(Direction::Rtl));
    f.edit(|probe| {
        probe.instance_resolver = true;
        probe.status = ResultStatus::Forbidden;
        probe.show_icon = false;
        probe.content = false;
    });
    assert_eq!(f.bounds(ICON_MARKER).top(), px(40.));
    assert_eq!(f.bounds(INSTANCE_RESOLVER).size.width, px(700.));
    let scoped = *f.scoped_calls.lock().unwrap().last().unwrap();
    let instance = *f.instance_calls.borrow().last().unwrap();
    assert_eq!(scoped, instance);
    assert_eq!(instance.status, ResultStatus::Forbidden);
    assert_eq!(instance.direction, Direction::Rtl);
    assert!(
        !instance.has_title && !instance.has_sub_title && !instance.has_extra && !instance.has_body
    );
    assert!(
        instance.show_icon,
        "exception illustration overrides icon hiding in resolved context"
    );
}
