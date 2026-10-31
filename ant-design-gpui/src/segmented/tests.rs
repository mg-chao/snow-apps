// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

use gpui::{
    AppContext, Bounds, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent,
    KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, Pixels, PlatformInput,
    Render, RenderOnce, Role, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit,
    div, point, px,
};

use super::*;
use crate::{Checkbox, ConfigPatch, UiRoot, behavior::focus_handle};

type MeasuredBounds = Rc<Cell<Option<Bounds<Pixels>>>>;

struct Consumer {
    scope: ConfigScope,
    focus: [FocusHandle; 4],
    changes: Rc<RefCell<Vec<u8>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    bounds: MeasuredBounds,
    item_bounds: [MeasuredBounds; 3],
    value: Option<u8>,
    default_value: Option<u8>,
    order: Vec<u8>,
    value_offset: u8,
    disabled: Option<bool>,
    option_disabled: [Option<bool>; 3],
    vertical: bool,
    orientation: Option<SegmentedOrientation>,
    orientation_first: bool,
    block: bool,
    size: Option<Size>,
    shape: SegmentedShape,
    root_style: StyleRefinement,
    item_style: Option<StyleRefinement>,
    custom_height: f32,
    visible: bool,
    renders: Rc<Cell<usize>>,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: std::array::from_fn(|_| focus_handle(cx)),
            changes: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::RadioGroup))),
            bounds: Rc::default(),
            item_bounds: std::array::from_fn(|_| Rc::default()),
            value: None,
            default_value: None,
            order: vec![1, 2, 3],
            value_offset: 0,
            disabled: None,
            option_disabled: [None; 3],
            vertical: false,
            orientation: None,
            orientation_first: false,
            block: false,
            size: None,
            shape: SegmentedShape::Default,
            root_style: StyleRefinement::default(),
            item_style: None,
            custom_height: 12.,
            visible: true,
            renders: Rc::default(),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        let mut column = div()
            .flex()
            .flex_col()
            .items_start()
            .p(px(12.))
            .gap(px(20.))
            .w(px(360.));
        if self.visible {
            let options = self.order.iter().map(|value| {
                let index = usize::from(*value - 1);
                let child = measure::measure(
                    div()
                        .w(px(20. * f32::from(*value)))
                        .h(px(self.custom_height)),
                    self.item_bounds[index].clone(),
                );
                let mut option = SegmentedOption::new(
                    ("option", index),
                    *value + self.value_offset,
                    format!("Option {value}"),
                )
                .focus_handle(&self.focus[index])
                .child(child)
                .automation_id(format!("segmented-test.option-{value}"));
                if let Some(disabled) = self.option_disabled[index] {
                    option = option.disabled(disabled);
                }
                option
            });
            let changes = self.changes.clone();
            let mut segmented = Segmented::new("segmented-test")
                .scope(self.scope.clone())
                .options(options)
                .block(self.block)
                .shape(self.shape)
                .accessibility_label("Display period")
                .accessibility_description("Choose a reporting period")
                .automation_id("segmented-test.group")
                .on_change(move |value, _, _| changes.borrow_mut().push(*value));
            if self.orientation_first
                && let Some(orientation) = self.orientation
            {
                segmented = segmented.orientation(orientation);
            }
            segmented = segmented.vertical(self.vertical);
            if !self.orientation_first
                && let Some(orientation) = self.orientation
            {
                segmented = segmented.orientation(orientation);
            }
            if let Some(value) = self.value {
                segmented = segmented.value(value);
            }
            if let Some(value) = self.default_value {
                segmented = segmented.default_value(value);
            }
            if let Some(disabled) = self.disabled {
                segmented = segmented.disabled(disabled);
            }
            if let Some(size) = self.size {
                segmented = segmented.size(size);
            }
            if let Some(style) = &self.item_style {
                segmented = segmented.item_style(style.clone());
            }
            segmented.style().refine(&self.root_style);
            let element = segmented.render(window, cx).into_element();
            let mut node = accesskit::Node::new(element.a11y_role().expect("radio group role"));
            element.write_a11y_info(&mut node);
            *self.semantics.borrow_mut() = node;
            column = column.child(measure::measure(element, self.bounds.clone()));
        }
        column.child(
            Checkbox::new("segmented-next-control")
                .scope(self.scope.clone())
                .label("Next control")
                .focus_handle(&self.focus[3]),
        )
    }
}

struct CachedPanel(Entity<Consumer>);

impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(420.)).h(px(300.))),
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
            let child_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut content = Consumer::new(child_scope.clone(), cx);
                configure(&mut content);
                content
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(child_scope);
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
                window.draw(cx).clear(cx)
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

    fn patch(&mut self, patch: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, patch).unwrap());
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

    fn pointer(&mut self, down: bool, position: gpui::Point<Pixels>) {
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

    fn click(&mut self, value: u8) {
        let bounds = self.item_bounds(value);
        let position = bounds.center();
        self.pointer(true, position);
        self.pointer(false, position);
        self.draw();
    }

    fn drag(&mut self, position: gpui::Point<Pixels>) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseMove(gpui::MouseMoveEvent {
                        position,
                        pressed_button: Some(MouseButton::Left),
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }

    fn has_background(&mut self, color: gpui::Rgba) -> bool {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .any(|quad| quad.background.as_solid() == Some(color.into()))
            })
            .unwrap()
    }

    fn group_background_count(&mut self, color: gpui::Rgba) -> usize {
        let bounds = self.bounds();
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| {
                        quad.background.as_solid() == Some(color.into())
                            && quad.bounds.left().0 / scale >= f32::from(bounds.left()) - 0.1
                            && quad.bounds.right().0 / scale <= f32::from(bounds.right()) + 0.1
                            && quad.bounds.top().0 / scale >= f32::from(bounds.top()) - 0.1
                            && quad.bounds.bottom().0 / scale <= f32::from(bounds.bottom()) + 0.1
                    })
                    .count()
            })
            .unwrap()
    }

    fn shadow_count(&mut self) -> usize {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window.rendered_scene().shadows.len()
            })
            .unwrap()
    }

    fn focus(&mut self, value: u8) {
        let handle = self
            .consumer
            .read_with(&self.cx, |c, _| c.focus[usize::from(value - 1)].clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| handle.focus(window, cx))
            .unwrap();
        self.draw();
    }

    fn focused(&mut self, value: u8) -> bool {
        let handle = self
            .consumer
            .read_with(&self.cx, |c, _| c.focus[usize::from(value - 1)].clone());
        self.cx
            .update_window(self.window.into(), |_, window, _| handle.is_focused(window))
            .unwrap()
    }

    fn changes(&self) -> Vec<u8> {
        self.consumer
            .read_with(&self.cx, |c, _| c.changes.borrow().clone())
    }

    fn bounds(&self) -> Bounds<Pixels> {
        self.consumer
            .read_with(&self.cx, |c, _| c.bounds.get().expect("group geometry"))
    }

    fn item_bounds(&self, value: u8) -> Bounds<Pixels> {
        self.consumer.read_with(&self.cx, |c, _| {
            c.item_bounds[usize::from(value - 1)]
                .get()
                .expect("custom item geometry")
        })
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
fn first_option_is_selected_by_default_and_group_is_a_single_tab_stop() {
    let mut f = Fixture::new(|_| {});
    f.click(1);
    assert!(
        f.changes().is_empty(),
        "clicking the selected item is a no-op"
    );
    f.key("tab");
    assert!(
        f.focused(4),
        "pointer focus starts traversal after the selected radio"
    );
    f.key("shift-tab");
    assert!(f.focused(1));
    f.key("tab");
    assert!(f.focused(4));
}

#[test]
fn explicit_default_is_initial_only_and_is_restored_on_remount() {
    let mut f = Fixture::new(|c| c.default_value = Some(3));
    f.key("tab");
    assert!(f.focused(3));
    f.click(2);
    assert_eq!(f.changes(), [2]);
    f.edit(|c| c.default_value = Some(1));
    f.click(2);
    assert_eq!(f.changes(), [2]);
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    f.click(1);
    assert_eq!(f.changes(), [2], "remount reads the current default");
}

#[test]
fn controlled_values_are_proposals_until_the_owner_accepts_them() {
    let mut f = Fixture::new(|c| {
        c.value = Some(2);
        c.default_value = Some(3);
    });
    f.click(2);
    assert!(f.changes().is_empty());
    f.click(3);
    f.click(3);
    assert_eq!(f.changes(), [3, 3]);
    f.edit(|c| c.value = Some(3));
    f.click(3);
    assert_eq!(f.changes(), [3, 3]);
    f.click(1);
    assert_eq!(f.changes(), [3, 3, 1]);
}

#[test]
fn unknown_and_temporarily_removed_values_remain_unselected_until_restored() {
    let mut f = Fixture::new(|c| c.default_value = Some(3));
    f.edit(|c| c.order = vec![1, 2]);
    f.edit(|c| c.order = vec![1, 2, 3]);
    f.click(3);
    assert!(
        f.changes().is_empty(),
        "removed selected values are retained"
    );
    f.edit(|c| c.value = Some(99));
    f.click(1);
    assert_eq!(f.changes(), [1]);
    f.edit(|c| c.order.clear());
    f.key("tab");
    assert!(f.focused(4));
}

#[test]
fn empty_mount_does_not_invent_selection_when_options_arrive_later() {
    let mut f = Fixture::new(|c| c.order.clear());
    f.key("tab");
    assert!(f.focused(4));
    f.edit(|c| c.order = vec![1, 2, 3]);
    f.click(1);
    assert_eq!(f.changes(), [1]);
}

#[test]
fn disabled_selected_item_retains_selection_and_tabs_to_the_first_enabled_item() {
    let mut f = Fixture::new(|c| {
        c.default_value = Some(2);
        c.option_disabled[1] = Some(true);
    });
    f.key("tab");
    assert!(f.focused(1));
    f.click(2);
    assert!(f.changes().is_empty());
    // GPUI clears keyboard focus when a pointer press hits a disabled control.
    f.key("tab");
    assert!(f.focused(1));
    f.key("right");
    assert!(f.focused(3));
    assert_eq!(f.changes(), [3]);
}

#[test]
fn arrows_skip_disabled_items_wrap_and_home_end_select_the_edges() {
    let mut f = Fixture::new(|c| c.option_disabled[1] = Some(true));
    f.key("tab");
    assert!(f.focused(1));
    f.key("right");
    assert!(f.focused(3));
    assert_eq!(f.changes(), [3]);
    f.key("right");
    assert!(f.focused(1));
    f.key("left");
    assert!(f.focused(3));
    f.key("home");
    assert!(f.focused(1));
    f.key("end");
    assert!(f.focused(3));
    assert_eq!(f.changes(), [3, 1, 3, 1, 3]);
    f.key("ctrl-left");
    assert_eq!(f.changes(), [3, 1, 3, 1, 3]);
}

#[test]
fn rtl_horizontal_arrows_follow_physical_direction_and_vertical_uses_up_down() {
    let mut f = Fixture::new(|_| {});
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert!(f.item_bounds(1).left() > f.item_bounds(3).left());
    f.key("tab");
    f.key("left");
    assert!(f.focused(2));
    f.key("right");
    assert!(f.focused(1));
    f.edit(|c| c.vertical = true);
    f.key("down");
    assert!(f.focused(2));
    f.key("up");
    assert!(f.focused(1));
    assert!(f.item_bounds(1).top() < f.item_bounds(3).top());
}

#[test]
fn option_focus_survives_reordering_and_selection_uses_current_option_order() {
    let mut f = Fixture::new(|_| {});
    f.key("tab");
    f.key("right");
    assert!(f.focused(2));
    f.edit(|c| c.order = vec![3, 2, 1]);
    assert!(f.focused(2));
    f.key("right");
    assert!(f.focused(1));
    assert_eq!(f.changes(), [2, 1]);
}

#[test]
fn space_selects_focused_radio_once_and_enter_does_not_select() {
    let mut f = Fixture::new(|_| {});
    f.focus(2);
    f.key("enter");
    f.key("ctrl-space");
    assert!(f.changes().is_empty());
    f.key_event("space", true, false);
    f.key_event("space", true, true);
    f.key_event("space", false, false);
    f.draw();
    assert_eq!(f.changes(), [2]);
    f.key("space");
    assert_eq!(f.changes(), [2]);
}

#[test]
fn disabled_group_dominates_option_false_and_explicit_group_false_overrides_provider() {
    let mut f = Fixture::new(|c| {
        c.disabled = Some(true);
        c.option_disabled[1] = Some(false);
    });
    f.click(2);
    f.key("tab");
    assert!(f.focused(4));
    assert!(f.changes().is_empty());
    f.patch(|p| p.disabled = Some(true));
    f.edit(|c| c.disabled = Some(false));
    f.click(2);
    assert_eq!(f.changes(), [2]);
}

#[test]
fn inherited_disable_roundtrip_cancels_pending_space_without_a_replacement_draw() {
    let mut f = Fixture::new(|_| {});
    f.focus(2);
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
    assert!(f.changes().is_empty());
    assert!(!f.focused(2));
}

#[test]
fn pointer_cancellation_does_not_change_selection() {
    let mut f = Fixture::new(|_| {});
    let start = f.item_bounds(2).center();
    f.pointer(true, start);
    f.pointer(false, point(px(410.), px(290.)));
    f.draw();
    assert!(f.changes().is_empty());
}

#[test]
fn pressed_surface_tracks_drag_exit_and_return_until_release_selects() {
    let mut f = Fixture::new(|_| {});
    let scope = f.scope.clone();
    let active = f.cx.update(|cx| {
        scope
            .snapshot(cx)
            .theme
            .component::<SegmentedTheme>()
            .item_active_bg
            .to_rgba()
    });
    let target = f.item_bounds(2).center();
    f.pointer(true, target);
    f.draw();
    assert!(
        f.has_background(active),
        "pressed option displays its active surface"
    );
    assert!(f.changes().is_empty());
    f.drag(point(px(410.), px(290.)));
    assert!(
        !f.has_background(active),
        "dragging out removes the active surface"
    );
    assert!(f.changes().is_empty());
    f.drag(target);
    assert!(
        f.has_background(active),
        "returning restores the pressed surface"
    );
    assert!(f.changes().is_empty());
    f.pointer(false, target);
    f.draw();
    assert_eq!(f.changes(), [2]);
}

#[test]
fn reusing_option_identity_for_a_different_value_cancels_pending_space() {
    let mut f = Fixture::new(|_| {});
    f.focus(2);
    f.key_event("space", true, false);
    f.edit(|c| c.value_offset = 40);
    f.key_event("space", false, false);
    f.draw();
    assert!(f.changes().is_empty());
    f.key("space");
    assert_eq!(f.changes(), [42]);
}

#[test]
fn explicit_orientation_wins_over_vertical_independently_of_setter_order() {
    for orientation_first in [false, true] {
        let mut f = Fixture::new(|c| {
            c.vertical = true;
            c.orientation = Some(SegmentedOrientation::Horizontal);
            c.orientation_first = orientation_first;
        });
        assert_eq!(f.bounds().size.height, px(32.));
        assert_eq!(f.item_bounds(1).top(), f.item_bounds(3).top());
        f.edit(|c| {
            c.vertical = false;
            c.orientation = Some(SegmentedOrientation::Vertical);
        });
        assert!(f.item_bounds(1).top() < f.item_bounds(3).top());
    }
}

#[test]
fn vertical_block_distributes_a_fixed_height_evenly() {
    let f = Fixture::new(|c| {
        c.vertical = true;
        c.block = true;
        c.root_style = StyleRefinement::default().h(px(300.));
    });
    assert_eq!(f.bounds().size.height, px(300.));
    let centers: Vec<_> = (1..=3).map(|v| f.item_bounds(v).center().y).collect();
    // Native layout snaps neighboring edges to device pixels; fractional thirds
    // can differ by one logical pixel while covering the available space exactly.
    assert!(
        (f32::from(centers[1] - centers[0]) - 296. / 3.).abs() <= 1.,
        "vertical centers: {centers:?}, root: {:?}, children: {:?}",
        f.bounds(),
        (1..=3).map(|v| f.item_bounds(v)).collect::<Vec<_>>()
    );
    assert!(
        (f32::from(centers[2] - centers[1]) - 296. / 3.).abs() <= 1.,
        "vertical centers: {centers:?}"
    );
}

#[test]
fn group_semantics_have_native_radio_role_name_identifier_and_orientation() {
    let f = Fixture::new(|_| {});
    let node = f
        .consumer
        .read_with(&f.cx, |c, _| c.semantics.borrow().clone());
    assert_eq!(node.role(), Role::RadioGroup);
    assert_eq!(node.label(), Some("Display period"));
    assert_eq!(node.description(), Some("Choose a reporting period"));
    assert_eq!(node.author_id(), Some("segmented-test.group"));
    assert_eq!(node.orientation(), Some(accesskit::Orientation::Horizontal));
    assert_eq!(node.size_of_set(), Some(3));
}

#[test]
fn sizes_custom_content_and_styled_width_use_native_layout_geometry() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.bounds().size.height, px(32.));
    f.edit(|c| c.size = Some(Size::Small));
    assert_eq!(f.bounds().size.height, px(24.));
    f.edit(|c| c.size = Some(Size::Large));
    assert_eq!(f.bounds().size.height, px(40.));
    f.edit(|c| {
        c.size = Some(Size::Medium);
        c.custom_height = 60.;
    });
    assert_eq!(f.bounds().size.height, px(64.));
    f.edit(|c| {
        c.custom_height = 12.;
        c.block = true;
        c.root_style = StyleRefinement::default().w(px(300.));
    });
    assert_eq!(f.bounds().size.width, px(300.));
    let centers: Vec<_> = (1..=3).map(|v| f.item_bounds(v).center().x).collect();
    // Allow the one-pixel rounding needed for three equal native flex items.
    assert!(
        (f32::from(centers[1] - centers[0]) - 296. / 3.).abs() <= 1.,
        "horizontal centers: {centers:?}, root: {:?}, children: {:?}",
        f.bounds(),
        (1..=3).map(|v| f.item_bounds(v)).collect::<Vec<_>>()
    );
    assert!(
        (f32::from(centers[2] - centers[1]) - 296. / 3.).abs() <= 1.,
        "horizontal centers: {centers:?}"
    );
}

#[test]
fn cached_parent_observes_scope_size_and_direction_changes() {
    let mut f = Fixture::new(|_| {});
    f.patch(|p| {
        p.size = Some(Size::Small);
        p.direction = Some(Direction::Rtl);
    });
    assert_eq!(f.bounds().size.height, px(24.));
    assert!(f.item_bounds(1).left() > f.item_bounds(3).left());
    f.patch(|p| p.disabled = Some(true));
    f.click(2);
    assert!(f.changes().is_empty());
}

#[test]
fn idle_and_reduced_motion_do_not_schedule_continuous_frames() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.frame(16), 0);
    f.click(2);
    assert_eq!(f.frame(16), 0);
    let mut animated = Fixture::with_motion(true, |_| {});
    assert_eq!(animated.frame(16), 0, "initial selection never animates in");
    animated.click(2);
    assert!(animated.frame(16) > 0, "selection moves the retained thumb");
    animated.frame(400);
    assert_eq!(animated.frame(16), 0);
    animated.cx.update(|cx| cx.set_reduce_motion(true));
    animated.click(3);
    assert_eq!(animated.frame(16), 0);
}

#[test]
fn thumb_interruption_is_continuous_and_resizes_snap_without_spurious_animation() {
    let mut thumb = ThumbTransition::default();
    let linear = CubicBezier::new(0., 0., 1., 1.);
    let motion = MotionPolicy::new(true, false);
    let duration = Duration::from_millis(200);
    let rect =
        |x: f32, width: f32| Bounds::new(point(px(x), px(2.)), gpui::size(px(width), px(28.)));
    let first = rect(2., 40.);
    let last = rect(162., 80.);
    assert_eq!(
        thumb.resolve(
            Some(("first".into(), first)),
            Duration::ZERO,
            duration,
            linear,
            motion
        ),
        (Some(first), false),
    );
    let (initial, pending) = thumb.resolve(
        Some(("last".into(), last)),
        Duration::ZERO,
        duration,
        linear,
        motion,
    );
    assert_eq!(initial, Some(first));
    assert!(pending);
    let halfway = thumb.sample(Duration::from_millis(100)).unwrap();
    assert_eq!(halfway, rect(82., 60.));
    let (interrupted, pending) = thumb.resolve(
        Some(("first".into(), first)),
        Duration::from_millis(100),
        duration,
        linear,
        motion,
    );
    assert_eq!(interrupted, Some(halfway));
    assert!(pending);
    let resized = rect(2., 120.);
    assert_eq!(
        thumb.resolve(
            Some(("first".into(), resized)),
            Duration::from_millis(110),
            duration,
            linear,
            motion
        ),
        (Some(resized), false),
    );
    assert_eq!(
        thumb.resolve(
            Some(("last".into(), last)),
            Duration::from_millis(120),
            duration,
            linear,
            MotionPolicy::new(false, false)
        ),
        (Some(last), false),
    );
    assert_eq!(
        thumb.resolve(None, Duration::from_millis(130), duration, linear, motion),
        (None, false)
    );
    assert!(thumb.target.is_none());
}

#[test]
fn colors_preserve_interruption_continuity_and_snap_for_reduced_motion() {
    let mut colors = ColorTransition::default();
    let rest = SegmentedStateStyle {
        background: Color::TRANSPARENT,
        text: Color::rgb(0x333333),
    };
    let hovered = SegmentedStateStyle {
        background: Color::rgb(0x1677ff).with_alpha(0.15),
        text: Color::rgb(0x000000),
    };
    let duration = Duration::from_millis(200);
    let motion = MotionPolicy::new(true, false);
    assert_eq!(
        colors.resolve(rest, Duration::ZERO, duration, motion),
        (rest, false)
    );
    let (initial, pending) = colors.resolve(hovered, Duration::ZERO, duration, motion);
    assert_eq!(initial, rest);
    assert!(pending);
    let halfway = colors.sample(Duration::from_millis(100));
    assert_ne!(halfway, rest);
    assert_ne!(halfway, hovered);
    let (interrupted, pending) = colors.resolve(rest, Duration::from_millis(100), duration, motion);
    assert_eq!(interrupted, halfway);
    assert!(pending);
    assert_eq!(
        colors.resolve(
            hovered,
            Duration::from_millis(110),
            duration,
            MotionPolicy::new(true, true)
        ),
        (hovered, false),
    );
}

#[test]
fn color_interpolation_uses_premultiplied_alpha_without_a_dark_halo() {
    let blue = Color::rgb(0x1677ff);
    let transparent = Color::TRANSPARENT;
    let faded = blue.with_alpha(0.5);
    assert_eq!(interpolate(blue, transparent, 0.5), faded);
    assert_eq!(interpolate(transparent, blue, 0.5), faded);
    assert_eq!(interpolate(blue, transparent, 0.), blue);
    assert_eq!(interpolate(blue, transparent, 1.), transparent);
    assert_eq!(interpolate(blue, blue, 0.5), blue);
}

#[test]
fn selected_surface_retains_corner_refinements_and_reuses_scaled_shadow_storage() {
    let mut surface = SurfaceStyle::default();
    let corners = gpui::Corners {
        top_left: px(0.),
        top_right: px(3.),
        bottom_right: px(9.),
        bottom_left: px(14.),
    };
    let shadows: Arc<[gpui::BoxShadow]> = vec![
        gpui::BoxShadow {
            inset: false,
            color: Color::BLACK.with_alpha(0.8).to_hsla(),
            offset: point(px(1.), px(2.)),
            blur_radius: px(4.),
            spread_radius: px(2.),
        },
        gpui::BoxShadow {
            inset: true,
            color: Color::WHITE.with_alpha(0.2).to_hsla(),
            offset: point(px(-1.), px(-2.)),
            blur_radius: px(3.),
            spread_radius: px(1.),
        },
    ]
    .into();
    surface.update(corners, 0.5, &shadows);
    assert_eq!(surface.corners, corners);
    assert_eq!(surface.opacity, 0.5);
    assert!(!Arc::ptr_eq(&surface.shadows, &shadows));
    for (scaled, source) in surface.shadows.iter().zip(shadows.iter()) {
        assert!((scaled.color.a - source.color.a * 0.5).abs() < f32::EPSILON);
        assert_eq!(scaled.offset, source.offset);
        assert_eq!(scaled.blur_radius, source.blur_radius);
        assert_eq!(scaled.spread_radius, source.spread_radius);
        assert_eq!(scaled.inset, source.inset);
    }
    let cached = surface.shadows.clone();
    surface.update(corners, 0.5, &shadows);
    assert!(Arc::ptr_eq(&surface.shadows, &cached));
    let changed_corners = gpui::Corners::all(px(6.));
    surface.update(changed_corners, 0.5, &shadows);
    assert_eq!(surface.corners, changed_corners);
    assert!(
        Arc::ptr_eq(&surface.shadows, &cached),
        "corner changes need no shadow allocation"
    );

    let changed_shadows: Arc<[gpui::BoxShadow]> = vec![gpui::BoxShadow {
        color: Color::rgb(0x1677ff).with_alpha(0.6).to_hsla(),
        ..shadows[0]
    }]
    .into();
    surface.update(changed_corners, 0.5, &changed_shadows);
    assert!(Arc::ptr_eq(&surface.source_shadows, &changed_shadows));
    assert!(!Arc::ptr_eq(&surface.shadows, &cached));
    assert!((surface.shadows[0].color.a - 0.3).abs() < f32::EPSILON);
    surface.update(changed_corners, 1., &changed_shadows);
    assert!(
        Arc::ptr_eq(&surface.shadows, &changed_shadows),
        "full opacity uses source storage directly"
    );
}

#[test]
fn custom_item_background_and_shadow_replace_selected_defaults_without_duplicate_layers() {
    let mut f = Fixture::new(|_| {});
    let white = Color::WHITE.to_rgba();
    let red = Color::rgb(0xff0000).with_alpha(0.25);
    assert_eq!(f.group_background_count(white), 1);
    let default_shadows = f.shadow_count();
    assert!(default_shadows > 0, "selected item has the token shadow");

    f.edit(|c| c.item_style = Some(StyleRefinement::default().bg(red)));
    assert_eq!(f.group_background_count(red.to_rgba()), 3);
    assert_eq!(
        f.group_background_count(white),
        0,
        "a translucent custom background must blend directly with the track"
    );
    assert_eq!(
        f.shadow_count(),
        default_shadows,
        "a background override preserves the selected token shadow"
    );

    f.edit(|c| {
        let mut style = StyleRefinement::default().bg(red);
        style.box_shadow = Some(Default::default());
        c.item_style = Some(style);
        c.value = Some(2);
    });
    assert_eq!(f.group_background_count(red.to_rgba()), 3);
    assert_eq!(f.group_background_count(white), 0);
    assert_eq!(
        f.shadow_count(),
        0,
        "an explicit empty shadow overrides the token shadow"
    );

    f.edit(|c| c.item_style = None);
    assert_eq!(f.group_background_count(white), 1);
    assert_eq!(f.shadow_count(), default_shadows);
}
