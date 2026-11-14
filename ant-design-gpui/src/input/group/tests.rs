// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Button, Input, InputNumber, InputNumberState, InputState, SearchInput, Select, SelectState,
    TextArea, UiRoot,
    select::{SelectOption, SelectOptions},
};
use gpui::{
    AppContext, Context, Entity, FocusHandle, Focusable, Hsla, IntoElement, KeyDownEvent,
    Keystroke, MouseMoveEvent, PlatformInput, Render, TestAppContext, Window, WindowHandle, div,
    point, px,
};

fn color(index: usize) -> Hsla {
    gpui::rgb([0xffdddd, 0xddffdd, 0xddddff][index]).into()
}

struct View {
    scope: ConfigScope,
    inputs: [Entity<InputState>; 2],
    number: Entity<InputNumberState>,
    select: Entity<SelectState<u32>>,
    with_select: bool,
    button_focus: FocusHandle,
    size: Size,
    orientation: InputGroupOrientation,
    nested: bool,
    mixed: bool,
    composite: bool,
    clipped: bool,
    opacity: bool,
    local_size: bool,
    dashed: bool,
    disabled: Option<bool>,
    first_enabled: bool,
}
impl Render for View {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let group = InputGroup::new("group")
            .scope(self.scope.clone())
            .size(self.size)
            .orientation(self.orientation)
            .block(true)
            .accessibility_label("Joined fields");
        let group = if let Some(disabled) = self.disabled {
            group.disabled(disabled)
        } else {
            group
        };
        let group = if self.composite {
            group
                .child(
                    SearchInput::new("search", &self.inputs[0])
                        .w(px(240.))
                        .bg(color(0)),
                )
                .child(
                    TextArea::new("area", &self.inputs[1])
                        .rows(1)
                        .show_count(true)
                        .w(px(120.))
                        .bg(color(1)),
                )
        } else {
            let mut first = Input::new("first", &self.inputs[0])
                .w(px(100.))
                .flex_shrink_0()
                .bg(color(0));
            if self.local_size {
                first = first.size(Size::Small);
            }
            if self.first_enabled {
                first = first.disabled(false);
            }
            let group = if self.with_select {
                group.child(first).child(
                    Select::new("select", &self.select)
                        .flex_1()
                        .min_w_0()
                        .bg(color(1)),
                )
            } else if self.mixed {
                group.child(first).child(
                    InputNumber::new("number", &self.number, cx)
                        .controls(false)
                        .flex_1()
                        .min_w_0()
                        .bg(color(1)),
                )
            } else {
                let second = Input::new("second", &self.inputs[1])
                    .flex_1()
                    .min_w_0()
                    .bg(color(1));
                if self.nested {
                    group.child(
                        InputGroup::new("nested")
                            .orientation(self.orientation)
                            .flex_1()
                            .when(self.opacity, |group| group.opacity(0.5))
                            .child(first)
                            .child(second),
                    )
                } else {
                    group.child(first).child(second)
                }
            };
            group.child(
                Button::new("button")
                    .label("Go")
                    .focus_handle(&self.button_focus)
                    .w(px(70.))
                    .when(self.dashed, |button| {
                        button
                            .button_type(crate::button::ButtonType::Dashed)
                            .rounded(px(0.))
                    })
                    .bg(color(2)),
            )
        };
        div()
            .flex()
            .flex_col()
            .items_start()
            .w(px(424.))
            .p(px(12.))
            .when(self.clipped, |root| {
                root.w(px(210.)).h(px(46.)).overflow_hidden()
            })
            .child(group.when(self.opacity, |group| group.opacity(0.5)))
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    view: Entity<View>,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut view = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let inputs = std::array::from_fn(|_| cx.new(|cx| InputState::new(window, cx)));
            let number = cx.new(|cx| InputNumberState::new(window, cx));
            let select = cx.new(|cx| {
                SelectState::new(
                    SelectOptions::new([SelectOption::new("one", 1, "One")]).unwrap(),
                    window,
                    cx,
                )
            });
            let button_focus = crate::behavior::focus_handle(cx);
            let content = cx.new(|_| View {
                scope: scope.clone(),
                inputs,
                number,
                select,
                with_select: false,
                button_focus,
                size: Size::Medium,
                orientation: InputGroupOrientation::Horizontal,
                nested: false,
                mixed: false,
                composite: false,
                clipped: false,
                opacity: false,
                local_size: false,
                dashed: false,
                disabled: None,
                first_enabled: false,
            });
            view = Some(content.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let mut f = Self {
            cx,
            window,
            view: view.unwrap(),
        };
        f.draw();
        f
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
    fn configure(&mut self, change: impl FnOnce(&mut View, &mut Context<View>)) {
        self.view.update(&mut self.cx, |view, cx| {
            change(view, cx);
            cx.notify();
        });
        self.draw();
    }
    fn focus(&mut self, index: usize) {
        let view = self.view.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                let focus = if index == 2 {
                    view.read(cx).button_focus.clone()
                } else if index == 1 && view.read(cx).mixed {
                    view.read(cx).number.read(cx).focus_handle(cx)
                } else {
                    view.read(cx).inputs[index].read(cx).focus_handle(cx)
                };
                window.focus(&focus, cx);
            })
            .unwrap();
        self.draw();
    }
    fn focused(&mut self, index: usize) -> bool {
        let view = self.view.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                let focus = if index == 2 {
                    view.read(cx).button_focus.clone()
                } else if index == 1 && view.read(cx).mixed {
                    view.read(cx).number.read(cx).focus_handle(cx)
                } else {
                    view.read(cx).inputs[index].read(cx).focus_handle(cx)
                };
                focus.is_focused(window)
            })
            .unwrap()
    }
    fn key(&mut self, key: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse(key).unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn surfaces(&mut self) -> Vec<gpui::Quad> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| {
                        quad.background.as_solid().is_some_and(|background| {
                            (0..3).any(|index| {
                                let expected = color(index);
                                background.h == expected.h
                                    && background.s == expected.s
                                    && background.l == expected.l
                            })
                        })
                    })
                    .map(|quad| {
                        let mut quad = *quad;
                        let scale = window.scale_factor();
                        quad.bounds.origin.x.0 /= scale;
                        quad.bounds.origin.y.0 /= scale;
                        quad.bounds.size.width.0 /= scale;
                        quad.bounds.size.height.0 /= scale;
                        quad.corner_radii =
                            quad.corner_radii.map(|r| gpui::ScaledPixels(r.0 / scale));
                        quad.content_mask.bounds.origin.x.0 /= scale;
                        quad.content_mask.bounds.origin.y.0 /= scale;
                        quad.content_mask.bounds.size.width.0 /= scale;
                        quad.content_mask.bounds.size.height.0 /= scale;
                        quad
                    })
                    .collect()
            })
            .unwrap()
    }
    fn ordered(&mut self) -> Vec<gpui::Quad> {
        let surfaces = self.surfaces();
        (0..3)
            .filter_map(|index| {
                surfaces
                    .iter()
                    .find(|quad| {
                        quad.background
                            .as_solid()
                            .is_some_and(|background| background.h == color(index).h)
                    })
                    .copied()
            })
            .collect()
    }
    fn border_orders(&mut self) -> [u32; 2] {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                [12., 111.].map(|x| {
                    window
                        .rendered_scene()
                        .quads
                        .iter()
                        .filter(|quad| {
                            quad.border_color.a > 0.
                                && quad.bounds.size.height.0 / window.scale_factor() > 20.
                                && (quad.bounds.origin.x.0 / window.scale_factor() - x).abs() < 0.1
                        })
                        .map(|quad| quad.order)
                        .max()
                        .unwrap()
                })
            })
            .unwrap()
    }
}
fn near(actual: f32, expected: f32) {
    assert!((actual - expected).abs() < 0.15, "{actual} != {expected}");
}

#[test]
fn joined_geometry_sizes_and_outer_corners_follow_ltr_and_rtl() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for (size, height, radius) in [
            (Size::Small, 24., 4.),
            (Size::Medium, 32., 6.),
            (Size::Large, 40., 8.),
        ] {
            let mut f = Fixture::new();
            f.configure(|view, cx| {
                view.size = size;
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(direction))
                    .unwrap();
            });
            let q = f.ordered();
            assert_eq!(q.len(), 3);
            for quad in &q {
                near(quad.bounds.size.height.0, height);
            }
            let (first, last) = if direction == Direction::Ltr {
                (q[0], q[2])
            } else {
                (q[2], q[0])
            };
            near(first.bounds.left().0, 12.);
            near(last.bounds.right().0, 412.);
            near(first.corner_radii.top_left.0, radius);
            near(first.corner_radii.top_right.0, 0.);
            near(last.corner_radii.top_left.0, 0.);
            near(last.corner_radii.top_right.0, radius);
            near(q[1].corner_radii.top_left.0, 0.);
            near(q[1].corner_radii.top_right.0, 0.);
            near(first.bounds.right().0 - q[1].bounds.left().0, 1.);
            near(q[1].bounds.right().0 - last.bounds.left().0, 1.);
        }
    }
}

#[test]
fn fractional_scale_stretches_auto_height_inputs_to_join_button_edges() {
    let mut f = Fixture::new();
    f.cx.update_window(f.window.into(), |_, window, _| window.set_scale_factor(1.5))
        .unwrap();
    f.draw();
    let q = f.ordered();
    assert_eq!(q.len(), 3);
    for quad in &q {
        near(quad.bounds.size.height.0, 32.);
        near(quad.bounds.top().0, q[2].bounds.top().0);
        near(quad.bounds.bottom().0, q[2].bounds.bottom().0);
    }
}

#[test]
fn select_joins_input_and_button_and_inherits_group_size_direction_and_disabled_state() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for (size, height) in [(Size::Small, 24.), (Size::Medium, 32.), (Size::Large, 40.)] {
            let mut f = Fixture::new();
            f.configure(|view, cx| {
                view.with_select = true;
                view.size = size;
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(direction))
                    .unwrap();
            });
            let quads = f.ordered();
            assert_eq!(quads.len(), 3);
            for quad in &quads {
                near(quad.bounds.size.height.0, height);
            }
            let (first, last) = if direction == Direction::Ltr {
                (quads[0], quads[2])
            } else {
                (quads[2], quads[0])
            };
            near(quads[1].corner_radii.top_left.0, 0.);
            near(quads[1].corner_radii.top_right.0, 0.);
            near(first.bounds.right().0 - quads[1].bounds.left().0, 1.);
            near(quads[1].bounds.right().0 - last.bounds.left().0, 1.);
            let select = f.view.read_with(&f.cx, |view, _| view.select.clone());
            f.cx.update_window(f.window.into(), |_, window, cx| {
                select.update(cx, |state, cx| state.focus(window, cx))
            })
            .unwrap();
            f.draw();
            f.cx.update_window(f.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse("down").unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
            })
            .unwrap();
            f.draw();
            assert!(select.read_with(&f.cx, |state, _| state.is_open()));
            f.configure(|view, _| view.disabled = Some(true));
            assert!(!select.read_with(&f.cx, |state, _| state.is_open()));
        }
    }
}

#[test]
fn nested_group_preserves_only_global_outer_corners_and_one_overlap() {
    let mut f = Fixture::new();
    f.configure(|view, _| view.nested = true);
    let q = f.ordered();
    assert_eq!(q.len(), 3);
    near(q[0].bounds.left().0, 12.);
    near(q[2].bounds.right().0, 412.);
    near(q[0].bounds.right().0 - q[1].bounds.left().0, 1.);
    near(q[1].bounds.right().0 - q[2].bounds.left().0, 1.);
    near(q[1].corner_radii.top_right.0, 0.);
    f.focus(0);
    assert!(f.focused(0));
    f.key("tab");
    assert!(f.focused(1));
    f.key("tab");
    assert!(f.focused(2));
    f.key("shift-tab");
    assert!(f.focused(1));
}

#[test]
fn focused_and_hovered_controls_paint_over_neighbors_without_changing_tab_order() {
    let mut f = Fixture::new();
    f.focus(0);
    let order = f.border_orders();
    assert!(order[0] > order[1]);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position: point(px(200.), px(25.)),
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    f.draw();
    let order = f.border_orders();
    assert!(order[1] > order[0]);
    f.key("tab");
    assert!(f.focused(1));
    f.key("tab");
    assert!(f.focused(2));
}

#[test]
fn fitted_dashed_button_border_is_raised_with_group_focus_and_preserves_opacity() {
    let mut f = Fixture::new();
    f.configure(|view, _| {
        view.dashed = true;
        view.opacity = true;
    });
    f.focus(2);
    let neighbor_order = f.border_orders()[1];
    f.cx.update_window(f.window.into(), |_, window, _| {
        let paths = &window.rendered_scene().paths;
        assert!(!paths.is_empty(), "fitted dashed stroke is retained");
        for path in paths {
            assert!(
                path.order > neighbor_order,
                "focused fitted border must paint above neighboring borders"
            );
            assert!((path.color.as_solid().unwrap().a - 0.5).abs() < 0.001);
        }
    })
    .unwrap();
    f.key("shift-tab");
    assert!(f.focused(1));
}

#[test]
fn mixed_numeric_group_keeps_independent_editing_and_keyboard_stepping() {
    let mut f = Fixture::new();
    f.configure(|view, _| view.mixed = true);
    f.focus(0);
    f.key("tab");
    assert!(f.focused(1));
    f.key("up");
    assert_eq!(
        f.view
            .read_with(&f.cx, |view, cx| view.number.read(cx).text(cx).to_owned()),
        "1"
    );
    f.key("tab");
    assert!(f.focused(2));
    let q = f.ordered();
    near(q[1].bounds.right().0 - q[2].bounds.left().0, 1.);
}

#[test]
fn composite_widths_include_search_action_and_textarea_count() {
    let mut f = Fixture::new();
    f.configure(|view, _| view.composite = true);
    let q = f.ordered();
    assert_eq!(q.len(), 2);
    near(q[0].bounds.left().0, 12.);
    assert!(q[0].bounds.size.width.0 < 240.);
    near(q[1].bounds.left().0, 251.);
    near(q[1].bounds.size.width.0, 120.);
    near(q[0].corner_radii.top_right.0, 0.);
    near(q[1].corner_radii.top_left.0, 0.);
    near(q[1].corner_radii.top_right.0, 6.);
}

#[test]
fn vertical_joining_and_explicit_child_size_override_group_size() {
    let mut f = Fixture::new();
    f.configure(|view, _| view.orientation = InputGroupOrientation::Vertical);
    let q = f.ordered();
    near(q[0].bounds.bottom().0 - q[1].bounds.top().0, 1.);
    near(q[1].bounds.bottom().0 - q[2].bounds.top().0, 1.);
    near(q[0].corner_radii.bottom_left.0, 0.);
    near(q[2].corner_radii.top_left.0, 0.);
    near(q[2].corner_radii.bottom_left.0, 6.);
    f.configure(|view, _| {
        view.orientation = InputGroupOrientation::Horizontal;
        view.size = Size::Large;
        view.local_size = true;
    });
    let q = f.ordered();
    // Compact uses flex stretch: the smaller control retains its font/padding
    // and outer radius while its automatic height joins the taller neighbor.
    near(q[0].bounds.size.height.0, 40.);
    near(q[1].bounds.size.height.0, 40.);
    near(q[0].corner_radii.top_left.0, 4.);
    f.view.read_with(&f.cx, |view, cx| {
        near(
            f32::from(view.inputs[0].read(cx).geometry().unwrap().line_height),
            22.,
        );
        near(
            f32::from(view.inputs[1].read(cx).geometry().unwrap().line_height),
            24.,
        );
    });
}

#[test]
fn raised_paint_remains_clipped_and_inherits_group_opacity() {
    let mut f = Fixture::new();
    f.configure(|view, _| {
        view.clipped = true;
        view.opacity = true;
    });
    f.focus(0);
    for q in f.surfaces() {
        assert!(q.content_mask.bounds.right().0 <= 210.);
        near(q.background.as_solid().unwrap().a, 0.5);
    }
}

#[test]
fn group_disable_releases_focus_and_explicit_child_enable_overrides_it() {
    let mut f = Fixture::new();
    f.focus(0);
    f.configure(|view, _| view.disabled = Some(true));
    assert!(!f.focused(0));
    assert!(f.view.read_with(&f.cx, |view, cx| {
        view.inputs.iter().all(|input| input.read(cx).is_disabled())
    }));
    f.configure(|view, _| view.first_enabled = true);
    f.focus(0);
    assert!(f.focused(0));
    assert!(
        f.view
            .read_with(&f.cx, |view, cx| view.inputs[1].read(cx).is_disabled())
    );
    f.configure(|view, cx| {
        view.disabled = None;
        view.first_enabled = false;
        view.scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap();
    });
    assert!(!f.focused(0));
    f.configure(|view, _| view.disabled = Some(false));
    f.focus(0);
    f.key("tab");
    assert!(f.focused(1));
}

#[test]
fn nested_opacity_applies_once_to_borders_shadows_and_keyboard_outline() {
    let mut f = Fixture::new();
    f.configure(|view, _| {
        view.nested = true;
        view.opacity = true;
    });
    f.focus(0);
    let q = f.ordered();
    near(q[0].background.as_solid().unwrap().a, 0.25);
    near(q[1].background.as_solid().unwrap().a, 0.25);
    near(q[2].background.as_solid().unwrap().a, 0.5);
    f.cx.update_window(f.window.into(), |_, window, _| {
        let quad = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| {
                quad.border_color.a > 0.
                    && (quad.bounds.origin.x.0 / window.scale_factor() - 12.).abs() < 0.1
                    && quad.bounds.size.height.0 / window.scale_factor() > 20.
            })
            .unwrap();
        near(quad.border_color.a, 0.25);
    })
    .unwrap();
    f.key("tab");
    f.key("tab");
    assert!(f.focused(2));
}
