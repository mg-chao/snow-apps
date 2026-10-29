// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigPatch, ConfigScope, Direction, UiRoot};
use gpui::{
    AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, Keystroke, MouseButton,
    MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point, Render,
    Subscription, TestAppContext, Window, WindowHandle, div, point, prelude::*, px, size,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

struct Consumer {
    scope: ConfigScope,
    state: Entity<CarouselState>,
    options: CarouselOptions,
    height: Option<f32>,
    shown: bool,
    offset_y: f32,
    styles: CarouselStyles,
    _events: Subscription,
}

impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .relative()
            .top(px(self.offset_y))
            .w_full()
            .when(self.shown, |root| {
                root.child(
                    Carousel::new("native-carousel-test", &self.state)
                        .scope(self.scope.clone())
                        .options(self.options.clone())
                        .styles(self.styles.clone())
                        .w(px(400.))
                        .when_some(self.height, |carousel, height| carousel.h(px(height))),
                )
            })
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<CarouselState>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<CarouselEvent>>>,
}

fn items(heights: &[f32]) -> CarouselItems {
    CarouselItems::new(heights.iter().copied().enumerate().map(|(index, height)| {
        CarouselSlide::new(format!("slide-{index}")).content(move |_, _| {
            div()
                .id(("carousel-test-slide", index))
                .w_full()
                .h(px(height))
                .debug_selector(move || format!("carousel-test.slide-{index}"))
                .child(format!("Slide {index}"))
                .into_any_element()
        })
    }))
    .unwrap()
}

impl Fixture {
    fn new(items: CarouselItems, options: CarouselOptions, height: Option<f32>) -> Self {
        let mut cx = TestAppContext::single();
        let mut output = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let recorded = events.clone();
        let window = cx.open_window(size(px(600.), px(600.)), |window, cx| {
            crate::init(cx);
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| CarouselState::new(items, window, cx));
            let consumer = cx.new(|cx| Consumer {
                scope: scope.clone(),
                state: state.clone(),
                options,
                height,
                shown: true,
                offset_y: 0.,
                styles: CarouselStyles::default(),
                _events: cx.subscribe(&state, move |_, _, event, _| {
                    recorded.borrow_mut().push(event.clone());
                }),
            });
            output = Some((consumer.clone(), state, scope.clone()));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state, scope) = output.unwrap();
        let mut fixture = Self {
            cx,
            window,
            consumer,
            state,
            scope,
            events,
        };
        fixture.draw();
        fixture.draw();
        fixture.event(PlatformInput::MouseMove(MouseMoveEvent {
            position: point(px(550.), px(550.)),
            ..Default::default()
        }));
        fixture.events.borrow_mut().clear();
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

    fn edit(&mut self, update: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            update(consumer);
            cx.notify();
        });
        self.draw();
    }

    fn update(
        &mut self,
        update: impl FnOnce(&mut CarouselState, &mut Window, &mut Context<CarouselState>),
    ) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                self.state.update(cx, |state, cx| update(state, window, cx));
            })
            .unwrap();
        self.draw();
    }

    fn patch(&mut self, patch: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, patch).unwrap());
        self.draw();
    }

    fn bounds(&self) -> Bounds<Pixels> {
        self.state
            .read_with(&self.cx, |state, _| state.viewport_bounds().unwrap())
    }

    fn current(&self) -> Option<usize> {
        self.state
            .read_with(&self.cx, |state, _| state.current_index())
    }

    fn dot(&self, index: usize) -> Bounds<Pixels> {
        self.state
            .read_with(&self.cx, |state, _| state.dot_bounds(index).unwrap())
    }

    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }

    fn click(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position,
            click_count: 1,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position,
            click_count: 1,
            ..Default::default()
        }));
    }

    fn key(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke::parse(key).unwrap(),
            is_held: false,
            prefer_character_input: false,
        }));
    }

    fn time(&mut self, millis: u64) {
        self.update(|state, _, cx| state.set_test_time(Some(Duration::from_millis(millis)), cx));
    }

    fn advance(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
        let callbacks = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        callbacks
    }

    fn drag(&mut self, start: Point<Pixels>, end: Point<Pixels>) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position: start,
            click_count: 1,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseMove(MouseMoveEvent {
            position: end,
            pressed_button: Some(MouseButton::Left),
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position: end,
            click_count: 1,
            ..Default::default()
        }));
    }
}

#[test]
fn native_layout_matches_reference_dots_arrows_and_all_placements() {
    for placement in [
        DotPlacement::Top,
        DotPlacement::Bottom,
        DotPlacement::Start,
        DotPlacement::End,
    ] {
        let mut fixture = Fixture::new(
            items(&[160.; 4]),
            CarouselOptions {
                dot_placement: placement,
                arrows: true,
                ..Default::default()
            },
            None,
        );
        let viewport = fixture.bounds();
        assert_eq!(viewport.size, size(px(400.), px(160.)));
        let active = fixture.dot(0);
        let inactive = fixture.dot(1);
        if placement.vertical() {
            assert_eq!(active.size, size(px(3.), px(24.)));
            assert_eq!(inactive.size, size(px(3.), px(16.)));
            assert_eq!(inactive.top() - active.bottom(), px(8.));
        } else {
            assert_eq!(active.size, size(px(24.), px(3.)));
            assert_eq!(inactive.size, size(px(16.), px(3.)));
            assert_eq!(inactive.left() - active.right(), px(8.));
        }
        match placement {
            DotPlacement::Top => assert_eq!(active.top() - viewport.top(), px(12.)),
            DotPlacement::Bottom => assert_eq!(viewport.bottom() - active.bottom(), px(12.)),
            DotPlacement::Start => assert_eq!(active.left() - viewport.left(), px(12.)),
            DotPlacement::End => assert_eq!(viewport.right() - active.right(), px(12.)),
        }
        let previous = fixture.state.read_with(&fixture.cx, |state, _| {
            state.previous_arrow_bounds().unwrap()
        });
        let next = fixture
            .state
            .read_with(&fixture.cx, |state, _| state.next_arrow_bounds().unwrap());
        assert_eq!(previous.size, size(px(16.), px(16.)));
        assert_eq!(next.size, size(px(16.), px(16.)));
        if placement.vertical() {
            assert_eq!(previous.top() - viewport.top(), px(8.));
            assert_eq!(viewport.bottom() - next.bottom(), px(8.));
        } else {
            assert_eq!(previous.left() - viewport.left(), px(8.));
            assert_eq!(viewport.right() - next.right(), px(8.));
        }
        fixture.click(inactive.center());
        assert_eq!(fixture.current(), Some(1));
    }
}

#[test]
fn native_explicit_orientation_keeps_reference_fallback_offsets_for_each_dot_placement() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for vertical in [false, true] {
            for placement in [
                DotPlacement::Top,
                DotPlacement::Bottom,
                DotPlacement::Start,
                DotPlacement::End,
            ] {
                let mut fixture = Fixture::new(
                    items(&[160.; 3]),
                    CarouselOptions {
                        vertical: Some(vertical),
                        dot_placement: placement,
                        ..Default::default()
                    },
                    Some(160.),
                );
                fixture.patch(|patch| patch.direction = Some(direction));
                let bounds = fixture.bounds();
                let first = fixture.dot(0);
                if vertical {
                    assert_eq!(first.size, size(px(3.), px(24.)));
                    match placement {
                        DotPlacement::Start => assert_eq!(first.left() - bounds.left(), px(12.)),
                        DotPlacement::End => assert_eq!(bounds.right() - first.right(), px(12.)),
                        DotPlacement::Top | DotPlacement::Bottom => {
                            assert_eq!(first.left() - bounds.left(), px(0.))
                        }
                    }
                    assert_eq!(
                        (first.top() + fixture.dot(2).bottom()) / 2.,
                        bounds.center().y
                    );
                } else {
                    assert_eq!(first.size, size(px(24.), px(3.)));
                    match placement {
                        DotPlacement::Top => assert_eq!(first.top() - bounds.top(), px(12.)),
                        DotPlacement::Bottom => {
                            assert_eq!(bounds.bottom() - first.bottom(), px(12.))
                        }
                        DotPlacement::Start | DotPlacement::End => {
                            assert_eq!(bounds.bottom() - first.bottom(), px(0.))
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn native_navigation_wraps_clamps_and_emits_one_event_pair_per_change() {
    let mut fixture = Fixture::new(items(&[160.; 3]), CarouselOptions::default(), Some(160.));
    fixture.update(|state, window, cx| state.next(window, cx));
    fixture.update(|state, window, cx| state.prev(window, cx));
    fixture.update(|state, window, cx| state.prev(window, cx));
    assert_eq!(fixture.current(), Some(2));
    assert_eq!(fixture.events.borrow().len(), 6);
    for events in fixture.events.borrow().as_chunks::<2>().0 {
        assert!(matches!((&events[0], &events[1]),
            (CarouselEvent::BeforeChange(before), CarouselEvent::AfterChange(after)) if before == after));
    }
    fixture.update(|state, window, cx| state.go_to(5, true, window, cx));
    assert_eq!(fixture.current(), Some(2));
    assert_eq!(
        fixture.events.borrow().len(),
        6,
        "same destination emits no change"
    );
    fixture.edit(|consumer| {
        consumer.options.infinite = false;
        consumer.options.arrows = true;
    });
    fixture.update(|state, window, cx| state.next(window, cx));
    assert_eq!(fixture.current(), Some(2));
    fixture.update(|state, window, cx| state.go_to(usize::MAX, true, window, cx));
    assert_eq!(fixture.current(), Some(2));
    fixture.update(|state, window, cx| state.go_to(0, true, window, cx));
    fixture.update(|state, window, cx| state.prev(window, cx));
    assert_eq!(fixture.current(), Some(0));
}

#[test]
fn native_keyboard_follows_axis_direction_and_dot_focus_remains_usable() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for vertical in [false, true] {
            let mut fixture = Fixture::new(
                items(&[160.; 3]),
                CarouselOptions {
                    vertical: Some(vertical),
                    ..Default::default()
                },
                Some(160.),
            );
            fixture.patch(|patch| patch.direction = Some(direction));
            fixture.click(fixture.dot(0).center());
            fixture.key(if vertical {
                "down"
            } else if direction == Direction::Rtl {
                "left"
            } else {
                "right"
            });
            assert_eq!(fixture.current(), Some(1));
            fixture.key(if vertical {
                "up"
            } else if direction == Direction::Rtl {
                "right"
            } else {
                "left"
            });
            assert_eq!(fixture.current(), Some(0));
        }
    }
}

#[test]
fn native_keyed_content_survives_reordering_and_recovers_after_removal() {
    let original = items(&[100., 160., 120.]);
    let mut fixture = Fixture::new(original.clone(), CarouselOptions::default(), None);
    fixture.update(|state, window, cx| state.go_to(1, true, window, cx));
    let retained = fixture
        .state
        .read_with(&fixture.cx, |state, _| state.retained_view_count());
    let mut reordered = original.entries().to_vec();
    reordered.reverse();
    fixture.update(|state, window, cx| {
        state.set_items(CarouselItems::new(reordered).unwrap(), window, cx)
    });
    assert_eq!(fixture.current(), Some(1));
    assert_eq!(
        fixture
            .state
            .read_with(&fixture.cx, |state, _| state.retained_view_count()),
        retained
    );
    fixture.update(|state, window, cx| {
        state.set_items(
            CarouselItems::new([original.entries()[2].clone()]).unwrap(),
            window,
            cx,
        )
    });
    assert_eq!(fixture.current(), Some(0));
    fixture.update(|state, window, cx| state.set_items(CarouselItems::default(), window, cx));
    assert_eq!(fixture.current(), None);
    assert_eq!(
        fixture
            .state
            .read_with(&fixture.cx, |state, _| state.retained_view_count()),
        0
    );
    fixture.update(|state, window, cx| state.set_items(original, window, cx));
    assert_eq!(fixture.current(), Some(0));
}

#[test]
fn native_height_matches_maximum_first_adaptive_and_explicit_modes() {
    let mut fixture = Fixture::new(items(&[100., 200., 140.]), CarouselOptions::default(), None);
    assert_eq!(fixture.bounds().size.height, px(200.));
    fixture.update(|state, window, cx| state.go_to(2, true, window, cx));
    assert_eq!(fixture.bounds().size.height, px(200.));
    fixture.edit(|consumer| consumer.options.adaptive_height = true);
    assert_eq!(fixture.bounds().size.height, px(140.));
    fixture.update(|state, window, cx| state.go_to(0, true, window, cx));
    assert_eq!(fixture.bounds().size.height, px(100.));
    fixture.edit(|consumer| {
        consumer.options.adaptive_height = false;
        consumer.options.vertical = Some(true);
    });
    assert_eq!(fixture.bounds().size.height, px(100.));
    fixture.edit(|consumer| consumer.height = Some(240.));
    assert_eq!(fixture.bounds().size.height, px(240.));
}

#[test]
fn native_viewport_geometry_respects_root_padding_and_explicit_auto_height() {
    let mut fixture = Fixture::new(items(&[100., 200.]), CarouselOptions::default(), Some(160.));
    fixture.edit(|consumer| consumer.styles.root = div().p(px(12.)).style().clone());
    assert_eq!(fixture.bounds().origin, point(px(12.), px(12.)));
    assert_eq!(fixture.bounds().size, size(px(376.), px(136.)));
    fixture.edit(|consumer| {
        consumer.height = None;
        consumer.styles.root = div().h_auto().style().clone();
    });
    fixture.draw();
    assert_eq!(fixture.bounds().size.height, px(200.));
}

#[test]
fn native_typed_hidden_view_notifications_invalidate_intrinsic_measurements() {
    struct DynamicHeight(f32);
    impl Render for DynamicHeight {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().w_full().h(px(self.0)).child("Changing height")
        }
    }
    let retained: Rc<RefCell<Option<Entity<DynamicHeight>>>> = Rc::default();
    let captured = retained.clone();
    let collection = CarouselItems::new([
        CarouselSlide::new("first")
            .content(|_, _| div().h(px(100.)).child("First").into_any_element()),
        CarouselSlide::new("dynamic").view(move |_, cx| {
            let entity = cx.new(|_| DynamicHeight(160.));
            *captured.borrow_mut() = Some(entity.clone());
            entity
        }),
    ])
    .unwrap();
    let mut fixture = Fixture::new(collection, CarouselOptions::default(), None);
    assert_eq!(fixture.bounds().size.height, px(160.));
    let dynamic = retained
        .borrow()
        .clone()
        .expect("all intrinsic heights are measured");
    dynamic.update(&mut fixture.cx, |view, cx| {
        view.0 = 280.;
        cx.notify();
    });
    fixture.draw();
    fixture.draw();
    assert_eq!(fixture.current(), Some(0));
    assert_eq!(fixture.bounds().size.height, px(280.));
}

#[test]
fn native_inactive_slide_controls_cannot_receive_pointer_or_retain_keyboard_focus() {
    use crate::Button;
    use gpui::FocusHandle;

    let activations = Rc::new([Cell::new(0usize), Cell::new(0usize)]);
    let handles: Rc<RefCell<Vec<FocusHandle>>> = Rc::default();
    let collection = CarouselItems::new((0..2).map(|index| {
        let activations = activations.clone();
        let handles = handles.clone();
        CarouselSlide::new(format!("interactive-{index}")).view(move |_, cx| {
            struct Interactive {
                index: usize,
                activations: Rc<[Cell<usize>; 2]>,
                focus: FocusHandle,
            }
            impl Render for Interactive {
                fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                    let index = self.index;
                    let activations = self.activations.clone();
                    div().w_full().h(px(160.)).child(
                        Button::new(("interactive-slide", index))
                            .label(format!("Action {index}"))
                            .focus_handle(&self.focus)
                            .w_full()
                            .h(px(100.))
                            .on_click(move |_, _, _| {
                                activations[index].set(activations[index].get() + 1);
                            }),
                    )
                }
            }
            let focus = crate::behavior::focus_handle(cx);
            handles.borrow_mut().push(focus.clone());
            cx.new(|_| Interactive {
                index,
                activations: activations.clone(),
                focus,
            })
        })
    }))
    .unwrap();
    let mut fixture = Fixture::new(collection, CarouselOptions::default(), None);
    let content = fixture.bounds();
    fixture.click(point(content.center().x, content.top() + px(50.)));
    assert_eq!((activations[0].get(), activations[1].get()), (1, 0));
    fixture.update(|state, window, cx| state.next(window, cx));
    let old_focus = handles.borrow()[0].clone();
    assert!(
        !fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| old_focus
                .is_focused(window))
            .unwrap()
    );
    fixture.click(point(content.center().x, content.top() + px(50.)));
    assert_eq!((activations[0].get(), activations[1].get()), (1, 1));
}

#[test]
fn native_empty_single_and_initially_empty_collections_do_not_activate_controls() {
    let mut fixture = Fixture::new(
        CarouselItems::default(),
        CarouselOptions {
            initial_slide: 2,
            autoplay: true,
            arrows: true,
            ..Default::default()
        },
        Some(160.),
    );
    fixture.update(|state, window, cx| state.next(window, cx));
    assert_eq!(fixture.current(), None);
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, _| state.autoplay_running())
    );
    fixture.update(|state, window, cx| state.set_items(items(&[160.; 4]), window, cx));
    assert_eq!(fixture.current(), Some(2));
    fixture.update(|state, window, cx| state.set_items(items(&[160.]), window, cx));
    assert_eq!(fixture.current(), Some(0));
    assert!(
        fixture
            .state
            .read_with(&fixture.cx, |state, _| state.dot_bounds(0).is_none())
    );
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, _| state.autoplay_running())
    );
}

#[test]
fn native_removing_or_disabling_navigation_controls_recovers_focus_to_the_carousel() {
    let mut fixture = Fixture::new(items(&[160.; 3]), CarouselOptions::default(), Some(160.));
    fixture.click(fixture.dot(0).center());
    fixture.edit(|consumer| {
        consumer.options.dots = false;
        consumer.options.arrows = false;
    });
    assert!(
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, cx| fixture
                .state
                .read(cx)
                .focus
                .is_focused(window))
            .unwrap()
    );
    fixture.edit(|consumer| consumer.options.dots = true);
    fixture.click(fixture.dot(0).center());
    fixture.patch(|patch| patch.disabled = Some(true));
    assert!(
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, cx| fixture
                .state
                .read(cx)
                .focus
                .is_focused(window))
            .unwrap()
    );
}

#[test]
fn native_initial_slide_updates_and_disabled_scope_follow_application_intent() {
    let mut fixture = Fixture::new(items(&[160.; 4]), CarouselOptions::default(), Some(160.));
    fixture.edit(|consumer| consumer.options.initial_slide = 2);
    assert_eq!(fixture.current(), Some(2));
    fixture.patch(|patch| patch.disabled = Some(true));
    fixture.click(fixture.dot(0).center());
    assert_eq!(fixture.current(), Some(2));
    fixture.update(|state, window, cx| state.go_to(1, true, window, cx));
    assert_eq!(
        fixture.current(),
        Some(1),
        "the owner can still update disabled controls"
    );
    fixture.patch(|patch| patch.disabled = Some(false));
    fixture.click(fixture.dot(0).center());
    assert_eq!(fixture.current(), Some(0));
}

#[test]
fn native_scroll_interruption_starts_at_sampled_position_and_wait_mode_rejects_retargeting() {
    let mut fixture = Fixture::new(items(&[160.; 5]), CarouselOptions::default(), Some(160.));
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.time(0);
    fixture.update(|state, window, cx| state.go_to(3, false, window, cx));
    fixture.time(200);
    let before = fixture
        .state
        .read_with(&fixture.cx, |state, cx| state.sample(cx).position);
    assert!(before > 0. && before < 3.);
    fixture.update(|state, window, cx| state.go_to(1, false, window, cx));
    let after = fixture
        .state
        .read_with(&fixture.cx, |state, cx| state.sample(cx).position);
    assert!(
        (before - after).abs() < 1e-8,
        "retargeting must preserve the current picture"
    );
    fixture.time(700);
    assert_eq!(fixture.current(), Some(1));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, cx| state.is_animating(cx))
    );
    fixture.edit(|consumer| consumer.options.wait_for_animate = true);
    fixture.update(|state, window, cx| state.go_to(2, false, window, cx));
    fixture.update(|state, window, cx| state.go_to(4, false, window, cx));
    assert_eq!(fixture.current(), Some(2));
    fixture.patch(|patch| patch.motion = Some(false));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, cx| state.is_animating(cx))
    );
}

#[test]
fn native_fade_preserves_retiring_layers_on_interruption_and_settles_without_idle_work() {
    let mut fixture = Fixture::new(
        items(&[160.; 4]),
        CarouselOptions {
            effect: CarouselEffect::Fade,
            ..Default::default()
        },
        Some(160.),
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.time(0);
    fixture.update(|state, window, cx| state.next(window, cx));
    fixture.time(150);
    let before = fixture
        .state
        .read_with(&fixture.cx, |state, cx| state.sample(cx).fade_layers);
    fixture.update(|state, window, cx| state.next(window, cx));
    let after = fixture
        .state
        .read_with(&fixture.cx, |state, cx| state.sample(cx).fade_layers);
    for (index, opacity) in before {
        let retained = after.iter().find(|(layer, _)| *layer == index).unwrap().1;
        assert!((opacity - retained).abs() < 1e-8);
    }
    fixture.time(650);
    assert_eq!(fixture.current(), Some(2));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, cx| state.is_animating(cx))
    );
    assert_eq!(fixture.advance(1000), 0);
}

#[test]
fn native_fade_scene_opacity_changes_between_cached_same_bounds_frames() {
    let collection = CarouselItems::new([0xef1234, 0x12cd56].into_iter().enumerate().map(
        |(index, color)| {
            CarouselSlide::new(format!("colored-{index}")).content(move |_, _| {
                div()
                    .w_full()
                    .h(px(160.))
                    .bg(gpui::rgb(color))
                    .into_any_element()
            })
        },
    ))
    .unwrap();
    let mut fixture = Fixture::new(
        collection,
        CarouselOptions {
            effect: CarouselEffect::Fade,
            ..Default::default()
        },
        Some(160.),
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.time(0);
    fixture.update(|state, window, cx| state.next(window, cx));
    let incoming_alpha = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter_map(|quad| quad.background.as_solid())
                    .map(|color| color.to_rgb())
                    .find(|color| {
                        (color.g - 205. / 255.).abs() < 0.001
                            && (color.r - 18. / 255.).abs() < 0.001
                    })
                    .expect("incoming colored slide")
                    .a
            })
            .unwrap()
    };
    fixture.time(100);
    let early = incoming_alpha(&mut fixture);
    fixture.time(200);
    let later = incoming_alpha(&mut fixture);
    assert!(
        later > early + 0.1,
        "same-bounds subtree caches cannot freeze a fade layer's primitive opacity"
    );
    let colored_layers = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter_map(|quad| quad.background.as_solid())
                    .map(|color| color.to_rgb())
                    .filter(|color| {
                        (color.r - 239. / 255.).abs() < 0.001
                            || (color.g - 205. / 255.).abs() < 0.001
                    })
                    .map(|color| (color.r, color.g, color.b, color.a))
                    .collect::<Vec<_>>()
            })
            .unwrap()
    };
    let before_return = colored_layers(&mut fixture);
    fixture.update(|state, window, cx| state.prev(window, cx));
    assert_eq!(
        colored_layers(&mut fixture),
        before_return,
        "returning to a retiring slide cannot reorder its native color primitives"
    );
    fixture.time(700);
    let final_layers = colored_layers(&mut fixture);
    assert_eq!(final_layers.len(), 1);
    assert!((final_layers[0].0 - 239. / 255.).abs() < 0.001);
    assert!((final_layers[0].3 - 1.).abs() < 0.001);
}

#[test]
fn native_desktop_drag_is_opt_in_axis_aware_and_snaps_at_threshold() {
    let mut fixture = Fixture::new(items(&[160.; 3]), CarouselOptions::default(), Some(160.));
    let center = fixture.bounds().center();
    fixture.drag(center, center - point(px(120.), px(0.)));
    assert_eq!(fixture.current(), Some(0));
    fixture.edit(|consumer| consumer.options.draggable = true);
    fixture.drag(center, center - point(px(40.), px(0.)));
    assert_eq!(fixture.current(), Some(0));
    fixture.drag(center, center - point(px(120.), px(0.)));
    assert_eq!(fixture.current(), Some(1));
    fixture.drag(center, center + point(px(8.), px(60.)));
    assert_eq!(fixture.current(), Some(1));
    fixture.edit(|consumer| {
        consumer.options.vertical = Some(true);
        consumer.options.infinite = false;
    });
    fixture.drag(center, center - point(px(0.), px(50.)));
    assert_eq!(fixture.current(), Some(2));
    fixture.drag(center, center - point(px(0.), px(50.)));
    assert_eq!(fixture.current(), Some(2));
}

#[test]
fn native_autoplay_pauses_for_hover_focus_and_hidden_mount_but_continues_without_motion() {
    let mut fixture = Fixture::new(
        items(&[160.; 3]),
        CarouselOptions {
            autoplay: true,
            autoplay_speed: Duration::from_millis(100),
            speed: Duration::ZERO,
            ..Default::default()
        },
        Some(160.),
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.advance(101);
    assert_eq!(fixture.current(), Some(1));
    fixture.update(|state, window, cx| state.set_hovered(true, window, cx));
    fixture.advance(200);
    assert_eq!(fixture.current(), Some(1));
    fixture.update(|state, window, cx| state.set_hovered(false, window, cx));
    fixture.update(|state, window, cx| state.set_focused(true, window, cx));
    fixture.advance(200);
    assert_eq!(fixture.current(), Some(1));
    fixture.update(|state, window, cx| state.set_focused(false, window, cx));
    fixture.advance(101);
    assert_eq!(fixture.current(), Some(2));
    fixture.patch(|patch| patch.motion = Some(false));
    fixture.advance(101);
    assert_eq!(fixture.current(), Some(0));
    assert!(
        fixture
            .state
            .read_with(&fixture.cx, |state, _| state.autoplay_running())
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.edit(|consumer| consumer.shown = false);
    fixture.draw();
    fixture.advance(500);
    assert_eq!(fixture.current(), Some(0));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, _| state.mounted)
    );
}

#[test]
fn native_reduced_motion_autoplay_uses_only_a_deadline_and_snaps_slide_changes() {
    let mut fixture = Fixture::new(
        items(&[160.; 3]),
        CarouselOptions {
            autoplay: true,
            autoplay_speed: Duration::from_millis(100),
            dot_duration: true,
            speed: Duration::from_millis(500),
            ..Default::default()
        },
        Some(160.),
    );
    assert_eq!(
        fixture.advance(101),
        0,
        "a countdown needs no animation frame"
    );
    assert_eq!(fixture.current(), Some(1));
    fixture.state.read_with(&fixture.cx, |state, cx| {
        assert!(!state.is_animating(cx));
        assert_eq!(state.sample(cx).position, 1.);
        assert!(!state.frame_pending);
    });
    assert_eq!(fixture.events.borrow().len(), 2);
    fixture.update(|state, window, cx| state.pause(window, cx));
    fixture.advance(500);
    assert_eq!(fixture.current(), Some(1));
    fixture.update(|state, window, cx| state.play(window, cx));
    fixture.advance(101);
    assert_eq!(fixture.current(), Some(2));
}

#[test]
fn native_live_autoplay_preserves_remaining_time_while_clipped_and_window_hidden() {
    let mut fixture = Fixture::new(
        items(&[160.; 3]),
        CarouselOptions {
            autoplay: true,
            autoplay_speed: Duration::from_millis(100),
            speed: Duration::ZERO,
            ..Default::default()
        },
        Some(160.),
    );
    fixture.advance(40);
    fixture.edit(|consumer| consumer.offset_y = 1000.);
    fixture.advance(200);
    assert_eq!(fixture.current(), Some(0));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, _| state.autoplay_running())
    );
    fixture.edit(|consumer| consumer.offset_y = 0.);
    fixture.advance(59);
    assert_eq!(fixture.current(), Some(0));
    fixture.advance(2);
    assert_eq!(fixture.current(), Some(1));

    fixture.advance(30);
    fixture
        .cx
        .simulate_window_visibility_change(fixture.window.into(), gpui::WindowVisibility::Hidden);
    fixture.cx.run_until_parked();
    fixture.advance(300);
    assert_eq!(fixture.current(), Some(1));
    assert!(
        !fixture
            .state
            .read_with(&fixture.cx, |state, _| state.autoplay_running())
    );
    fixture
        .cx
        .simulate_window_visibility_change(fixture.window.into(), gpui::WindowVisibility::Visible);
    fixture.draw();
    fixture.advance(68);
    assert_eq!(fixture.current(), Some(1));
    fixture.advance(3);
    assert_eq!(fixture.current(), Some(2));
}

#[test]
fn native_slide_factory_can_update_the_controller_without_a_reentrant_borrow() {
    let mut fixture = Fixture::new(
        CarouselItems::default(),
        CarouselOptions::default(),
        Some(160.),
    );
    let controller = fixture.state.downgrade();
    let collection =
        CarouselItems::new([CarouselSlide::new("original").view(move |window, cx| {
            controller
                .update(cx, |state, cx| {
                    state.set_items(
                        CarouselItems::new([CarouselSlide::new("replacement").text("Replacement")])
                            .unwrap(),
                        window,
                        cx,
                    );
                })
                .unwrap();
            cx.new(|_| {
                struct Replaced;
                impl Render for Replaced {
                    fn render(
                        &mut self,
                        _: &mut Window,
                        _: &mut Context<Self>,
                    ) -> impl IntoElement {
                        div().child("Old content")
                    }
                }
                Replaced
            })
        })])
        .unwrap();
    fixture.update(|state, window, cx| state.set_items(collection, window, cx));
    fixture.draw();
    fixture.state.read_with(&fixture.cx, |state, _| {
        assert_eq!(state.items().entries()[0].key.as_ref(), "replacement");
        assert!(!state.views.contains_key("original"));
        assert_eq!(state.retained_view_count(), 1);
    });
}

#[test]
fn native_before_change_callback_can_replace_items_while_events_keep_original_keys() {
    let mut fixture = Fixture::new(items(&[160.; 3]), CarouselOptions::default(), Some(160.));
    let controller = fixture.state.downgrade();
    let recorded = Rc::new(RefCell::new(Vec::new()));
    let callback_events = recorded.clone();
    fixture.update(|state, window, cx| {
        let handler = Rc::new(
            move |event: &CarouselEvent, window: &mut Window, cx: &mut gpui::App| {
                callback_events.borrow_mut().push(event.clone());
                if matches!(event, CarouselEvent::BeforeChange(_)) {
                    controller
                        .update(cx, |state, cx| {
                            state.set_items(
                                CarouselItems::new([
                                    CarouselSlide::new("replacement").text("New content")
                                ])
                                .unwrap(),
                                window,
                                cx,
                            );
                        })
                        .unwrap();
                }
            },
        );
        state.configure(
            state.options.clone(),
            state.scope.clone().unwrap(),
            Some(handler),
            window,
            cx,
        );
        state.go_to(2, true, window, cx);
    });
    assert_eq!(fixture.current(), Some(0));
    assert_eq!(recorded.borrow().len(), 2);
    for event in recorded.borrow().iter() {
        let change = match event {
            CarouselEvent::BeforeChange(change) | CarouselEvent::AfterChange(change) => change,
        };
        assert_eq!((change.from, change.to), (0, 2));
        assert_eq!(change.from_key.as_ref(), "slide-0");
        assert_eq!(change.to_key.as_ref(), "slide-2");
    }
    fixture.state.read_with(&fixture.cx, |state, _| {
        assert_eq!(state.items().entries()[0].key.as_ref(), "replacement")
    });
}

#[test]
fn native_autoplay_cadence_and_dot_progress_include_transition_time() {
    let mut fixture = Fixture::new(
        items(&[160.; 4]),
        CarouselOptions {
            autoplay: true,
            autoplay_speed: Duration::from_millis(100),
            speed: Duration::from_millis(80),
            ..Default::default()
        },
        Some(160.),
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.advance(101);
    assert_eq!(fixture.current(), Some(1));
    fixture.advance(50);
    assert_eq!(fixture.current(), Some(1));
    fixture.advance(51);
    assert_eq!(
        fixture.current(),
        Some(2),
        "transition duration does not extend the autoplay interval"
    );

    fixture.edit(|consumer| {
        consumer.options.autoplay_speed = Duration::from_secs(1);
        consumer.options.speed = Duration::from_millis(500);
        consumer.options.dot_duration = true;
    });
    fixture.time(0);
    fixture.update(|state, window, cx| state.next(window, cx));
    fixture.time(250);
    let progress = fixture
        .state
        .read_with(&fixture.cx, |state, cx| state.sample(cx).autoplay_progress);
    assert!(
        (progress - 0.25).abs() < 1e-8,
        "progress begins when the new slide becomes active"
    );
}

#[test]
fn native_fixed_height_warm_draw_constructs_a_bounded_number_of_slide_elements() {
    let rendered = Rc::new(Cell::new(0));
    let collection = CarouselItems::new((0..1000).map(|index| {
        let rendered = rendered.clone();
        CarouselSlide::new(format!("large-{index}")).content(move |_, _| {
            rendered.set(rendered.get() + 1);
            div()
                .h(px(160.))
                .child(index.to_string())
                .into_any_element()
        })
    }))
    .unwrap();
    let mut fixture = Fixture::new(
        collection,
        CarouselOptions {
            dots: false,
            ..Default::default()
        },
        Some(160.),
    );
    let before = rendered.get();
    for _ in 0..8 {
        fixture.draw();
    }
    assert!(
        rendered.get() - before <= 16,
        "warm rendering cannot rebuild hidden slide subtrees"
    );
    fixture.patch(|patch| patch.motion = Some(true));
    fixture.time(0);
    fixture.update(|state, window, cx| state.go_to(900, false, window, cx));
    for time in [20, 80, 160, 280, 400] {
        let before = rendered.get();
        fixture.time(time);
        assert!(
            rendered.get() - before <= 2,
            "only visible track slots render during long travel"
        );
    }
}
