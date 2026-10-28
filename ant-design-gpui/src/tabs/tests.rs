// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use super::{
    geometry::{Span, reveal},
    model::TabsModel,
};
use crate::{
    ConfigScope, Direction, UiRoot,
    theme::{ComponentTheme, ThemeConfig},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, KeyDownEvent, Keystroke, MouseButton, MouseDownEvent,
    MouseUpEvent, PlatformInput, Render, SharedString, Subscription, TestAppContext, Window,
    WindowHandle, div, prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

fn items() -> TabsItems {
    TabsItems::new([
        TabItem::new("one", "Tab 1").text("Content 1"),
        TabItem::new("disabled", "Disabled")
            .disabled(true)
            .text("Disabled content"),
        TabItem::new("two", "Tab 2").text("Content 2"),
        TabItem::new("three", "Tab 3")
            .closable(false)
            .text("Content 3"),
    ])
    .unwrap()
}

#[test]
fn tabs_items_reject_duplicate_keys_but_allow_duplicate_labels() {
    assert!(TabsItems::new([TabItem::new("a", "Same"), TabItem::new("b", "Same")]).is_ok());
    assert!(matches!(
        TabsItems::new([TabItem::new("a", "One"), TabItem::new("a", "Two")]),
        Err(TabsItemsError::DuplicateKey(_))
    ));
}
#[test]
fn tabs_navigation_skips_disabled_wraps_and_handles_empty_groups() {
    let model = TabsModel::new(items());
    assert_eq!(model.next_enabled(Some("one"), 1).as_deref(), Some("two"));
    assert_eq!(
        model.next_enabled(Some("one"), -1).as_deref(),
        Some("three")
    );
    assert_eq!(model.next_enabled(None, 1).as_deref(), Some("one"));
    assert_eq!(model.next_enabled(None, -1).as_deref(), Some("three"));
    assert_eq!(
        TabsModel::new(TabsItems::default()).next_enabled(None, 1),
        None
    );
    let disabled = TabsItems::new([TabItem::new("a", "A").disabled(true)]).unwrap();
    assert_eq!(TabsModel::new(disabled).next_enabled(None, 1), None);
}
#[test]
fn tabs_selection_survives_reorder_and_recovers_when_removed() {
    let mut model = TabsModel::new(items());
    model.active = Some("two".into());
    let mut entries = model.items.entries().to_vec();
    entries.reverse();
    model.set_items(TabsItems::new(entries).unwrap());
    assert_eq!(model.active.as_deref(), Some("two"));
    model.set_items(TabsItems::new([TabItem::new("three", "Three")]).unwrap());
    assert_eq!(model.active.as_deref(), Some("three"));
    model.set_items(TabsItems::default());
    assert_eq!(model.active, None);
}
#[test]
fn tabs_controlled_item_changes_do_not_replace_the_owner_selection() {
    let mut model = TabsModel::new(items());
    model.controlled = true;
    model.active = Some("two".into());
    model.set_items(TabsItems::default());
    assert_eq!(model.active.as_deref(), Some("two"));
}
#[test]
fn tabs_reveal_is_minimal_bounded_and_handles_oversized_items() {
    assert_eq!(
        reveal(
            Span {
                start: 20.,
                length: 30.
            },
            0.,
            100.,
            400.
        ),
        0.
    );
    assert_eq!(
        reveal(
            Span {
                start: 120.,
                length: 30.
            },
            0.,
            100.,
            400.
        ),
        50.
    );
    assert_eq!(
        reveal(
            Span {
                start: 20.,
                length: 30.
            },
            100.,
            100.,
            400.
        ),
        20.
    );
    assert_eq!(
        reveal(
            Span {
                start: 80.,
                length: 150.
            },
            0.,
            100.,
            400.
        ),
        80.
    );
    assert_eq!(
        reveal(
            Span {
                start: 0.,
                length: 30.
            },
            0.,
            100.,
            30.
        ),
        0.
    );
}
#[test]
fn tabs_placements_resolve_logical_edges() {
    assert!(!TabPlacement::Start.trailing(Direction::Ltr));
    assert!(TabPlacement::Start.trailing(Direction::Rtl));
    assert!(TabPlacement::End.trailing(Direction::Ltr));
    assert!(!TabPlacement::End.trailing(Direction::Rtl));
    assert!(!TabPlacement::Top.vertical());
    assert!(TabPlacement::End.vertical());
}
#[test]
fn tabs_card_height_overrides_recompute_padding_unless_explicit() {
    let theme = ThemeConfig::default().resolve();
    let mut tokens = TabsTheme::default_tokens(&theme.tokens);
    TabsTheme::apply(
        &mut tokens,
        &TabsTokenPatch {
            card_height: Some(64.),
            ..Default::default()
        },
    );
    assert_eq!(
        tokens.card_padding.top * 2. + theme.tokens.font_height + theme.tokens.line_width * 2.,
        64.
    );
    TabsTheme::apply(
        &mut tokens,
        &TabsTokenPatch {
            card_height: Some(80.),
            card_padding: Some(TabsSpacing::symmetric(2., 10.)),
            ..Default::default()
        },
    );
    assert_eq!(tokens.card_padding, TabsSpacing::symmetric(2., 10.));
}

struct Consumer {
    state: Entity<TabsState>,
    scope: ConfigScope,
    controlled: Option<SharedString>,
    kind: TabsType,
    placement: TabPlacement,
    destroy: bool,
    shown: bool,
    width: f32,
    animation: TabsAnimation,
    indicator: TabsIndicator,
    centered: bool,
    popup_width: Option<f32>,
    _subscription: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().w(px(self.width)).h(px(220.)).p(px(10.));
        if self.shown {
            let mut tabs = Tabs::new("tabs", &self.state)
                .scope(self.scope.clone())
                .kind(self.kind)
                .placement(self.placement)
                .centered(self.centered)
                .destroy_on_hidden(self.destroy)
                .h_full()
                .w_full();
            tabs = tabs
                .animated(self.animation)
                .indicator(self.indicator.clone());
            if let Some(width) = self.popup_width {
                tabs = tabs.styles(TabsStyles {
                    popup: div().w(px(width)).style().clone(),
                    ..Default::default()
                });
            }
            if let Some(key) = &self.controlled {
                tabs = tabs.active_key(key.clone());
            }
            root = root.child(tabs);
        }
        root
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<TabsState>,
    events: Rc<RefCell<Vec<TabsEvent>>>,
}
impl Fixture {
    fn new(items: TabsItems) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| TabsState::new(items, window, cx));
            let consumer = cx.new(|cx| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                controlled: None,
                kind: TabsType::Line,
                placement: TabPlacement::Top,
                destroy: false,
                shown: true,
                width: 420.,
                animation: TabsAnimation::default(),
                indicator: TabsIndicator::default(),
                centered: false,
                popup_width: None,
                _subscription: cx.subscribe(&state, move |_, _, event, _| {
                    output.borrow_mut().push(event.clone())
                }),
            });
            result = Some((consumer.clone(), state));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state) = result.unwrap();
        let mut f = Self {
            cx,
            window,
            consumer,
            state,
            events,
        };
        f.draw();
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
    fn update(
        &mut self,
        callback: impl FnOnce(&mut TabsState, &mut Window, &mut Context<TabsState>),
    ) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                self.state
                    .update(cx, |state, cx| callback(state, window, cx))
            })
            .unwrap();
        self.draw();
    }
    fn active(&self) -> Option<SharedString> {
        self.state
            .read_with(&self.cx, |s, _| s.active_key().cloned())
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }
    fn click(&mut self, key: &str) {
        let position = self
            .state
            .read_with(&self.cx, |s, _| s.items[key].bounds.get().unwrap().center());
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
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

    fn open_overflow(&mut self) -> gpui::Bounds<gpui::Pixels> {
        let popup = self.state.read_with(&self.cx, |s, _| s.popup.clone());
        self.cx
            .update_window(self.window.into(), |_, w, cx| popup.set_open(true, w, cx))
            .unwrap();
        self.draw();
        self.draw();
        self.cx.read(|cx| popup.layout(cx).unwrap().bounds)
    }

    fn click_at(&mut self, position: gpui::Point<gpui::Pixels>) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
    }
}

#[test]
fn tabs_native_click_and_keyboard_separate_focus_from_selection() {
    let mut f = Fixture::new(items());
    f.click("two");
    assert_eq!(f.active().as_deref(), Some("two"));
    f.key("right");
    assert_eq!(f.active().as_deref(), Some("two"));
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.focused_key.clone())
            .as_deref(),
        Some("three")
    );
    f.key("enter");
    assert_eq!(f.active().as_deref(), Some("three"));
    f.key("home");
    f.key("space");
    assert_eq!(f.active().as_deref(), Some("one"));
}
#[test]
fn tabs_disabled_clicks_and_repeated_selection_do_not_emit_changes() {
    let mut f = Fixture::new(items());
    f.click("disabled");
    assert!(f.events.borrow().is_empty());
    f.click("one");
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|e| matches!(e, TabsEvent::Change(_)))
            .count(),
        0
    );
    assert!(matches!(
        f.events.borrow().last(),
        Some(TabsEvent::Activate(_))
    ));
}
#[test]
fn tabs_controlled_selection_waits_for_owner_acceptance() {
    let mut f = Fixture::new(items());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.controlled = Some("one".into());
        cx.notify();
    });
    f.draw();
    f.click("two");
    assert_eq!(f.active().as_deref(), Some("one"));
    assert!(
        f.events
            .borrow()
            .iter()
            .any(|e| matches!(e, TabsEvent::Change(c) if c.key.as_ref() == "two"))
    );
    f.consumer.update(&mut f.cx, |c, cx| {
        c.controlled = Some("two".into());
        cx.notify();
    });
    f.draw();
    assert_eq!(f.active().as_deref(), Some("two"));
}
#[test]
fn tabs_edit_requests_respect_closable_and_leave_items_owned_by_caller() {
    let mut f = Fixture::new(items());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.kind = TabsType::EditableCard;
        cx.notify();
    });
    f.draw();
    f.update(|s, w, cx| {
        s.edit(
            TabsEdit::Remove("three".into()),
            TabsSource::Keyboard,
            w,
            cx,
        )
    });
    assert!(f.events.borrow().is_empty());
    f.update(|s, w, cx| s.edit(TabsEdit::Remove("two".into()), TabsSource::Keyboard, w, cx));
    assert!(matches!(
        f.events.borrow().last(),
        Some(TabsEvent::Edit { .. })
    ));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.items().len()), 4);
}
#[test]
fn tabs_panes_are_lazy_retained_and_recreated_on_destroy() {
    let created = Rc::new(Cell::new(0));
    let make = |key: &'static str| {
        let count = created.clone();
        TabItem::new(key, key).view(move |_, cx| {
            count.set(count.get() + 1);
            cx.new(|_| Pane).into()
        })
    };
    struct Pane;
    impl Render for Pane {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().child("Pane")
        }
    }
    let mut f = Fixture::new(TabsItems::new([make("one"), make("two")]).unwrap());
    assert_eq!(created.get(), 1);
    f.click("two");
    assert_eq!(created.get(), 2);
    f.click("one");
    assert_eq!(created.get(), 2);
    f.consumer.update(&mut f.cx, |c, cx| {
        c.destroy = true;
        cx.notify();
    });
    f.draw();
    f.click("two");
    assert_eq!(created.get(), 3);
    f.click("one");
    assert_eq!(created.get(), 4);
}
#[test]
fn tabs_overflow_reveals_selected_item_and_clamps_after_resize() {
    let items =
        TabsItems::new((0..30).map(|i| TabItem::new(format!("tab-{i}"), format!("Tab {i}"))))
            .unwrap();
    let mut f = Fixture::new(items);
    assert!(f.state.read_with(&f.cx, |s, _| s.overflow));
    f.update(|s, _, cx| s.set_active_key(Some("tab-29".into()), cx));
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.offset > 0.));
    assert!(!f.state.read_with(&f.cx, |s, _| {
        s.hidden.iter().any(|key| key.as_ref() == "tab-29")
    }));
    f.update(|s, w, cx| {
        s.set_items(
            TabsItems::new([TabItem::new("tab-29", "Last")]).unwrap(),
            w,
            cx,
        )
    });
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.offset), 0.);
    assert!(!f.state.read_with(&f.cx, |s, _| s.overflow));
}
#[test]
fn tabs_empty_all_disabled_and_unmount_are_safe() {
    let mut f = Fixture::new(TabsItems::default());
    f.update(|s, w, cx| s.focus(w, cx));
    f.key("right");
    assert_eq!(f.active(), None);
    f.update(|s, w, cx| {
        s.set_items(
            TabsItems::new([TabItem::new("a", "Disabled").disabled(true)]).unwrap(),
            w,
            cx,
        )
    });
    f.update(|s, w, cx| s.focus(w, cx));
    f.key("enter");
    assert!(f.events.borrow().is_empty());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.shown = false;
        cx.notify();
    });
    f.draw();
    f.draw();
    assert!(!f.state.read_with(&f.cx, |s, _| s.mounted));
    assert!(!f.state.read_with(&f.cx, |s, _| s.frame_pending));
}

#[test]
fn tabs_rtl_and_vertical_navigation_follow_the_visible_axis() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for placement in [
            TabPlacement::Top,
            TabPlacement::Bottom,
            TabPlacement::Start,
            TabPlacement::End,
        ] {
            let mut f = Fixture::new(items());
            f.consumer.update(&mut f.cx, |c, cx| {
                c.placement = placement;
                c.scope
                    .update_patch(cx, |p| p.direction = Some(direction))
                    .unwrap();
                cx.notify();
            });
            f.draw();
            f.click("one");
            f.key(if placement.vertical() {
                "down"
            } else if direction == Direction::Ltr {
                "right"
            } else {
                "left"
            });
            assert_eq!(f.active().as_deref(), Some("one"));
            f.key("space");
            assert_eq!(f.active().as_deref(), Some("two"));
            f.key("end");
            f.key("enter");
            assert_eq!(f.active().as_deref(), Some("three"));
        }
    }
}

#[test]
fn tabs_removing_focused_tab_recovers_to_a_live_enabled_handle() {
    let mut f = Fixture::new(items());
    f.click("two");
    f.update(|s, w, cx| {
        s.set_items(
            TabsItems::new([TabItem::new("one", "One"), TabItem::new("three", "Three")]).unwrap(),
            w,
            cx,
        )
    });
    assert!(
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let s = f.state.read(cx);
            s.items.values().any(|i| i.focus.is_focused(w))
        })
        .unwrap()
    );
    f.key("home");
    f.key("space");
    assert_eq!(f.active().as_deref(), Some("one"));
}

#[test]
fn tabs_overflow_short_labels_use_ant_design_minimum_width() {
    for algorithms in [
        crate::theme::AlgorithmChain::default(),
        crate::theme::AlgorithmChain::dark(),
        crate::theme::AlgorithmChain::compact(),
        crate::theme::AlgorithmChain::dark_compact(),
    ] {
        let mut f = Fixture::new(
            TabsItems::new((0..20).map(|i| TabItem::new(i.to_string(), "Tab"))).unwrap(),
        );
        f.consumer.update(&mut f.cx, |c, cx| {
            c.scope
                .update_patch(cx, |p| {
                    p.theme.get_or_insert_default().algorithms = Some(algorithms);
                })
                .unwrap();
            cx.notify();
        });
        f.draw();
        let bounds = f.open_overflow();
        assert_eq!(bounds.size.width, px(120.));
        assert_eq!(bounds.size.height, px(200.));
    }
}

#[test]
fn tabs_overflow_row_hitbox_and_hover_fill_the_menu_in_both_directions() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        let mut f = Fixture::new(
            TabsItems::new((0..10).map(|i| TabItem::new(i.to_string(), "Tab"))).unwrap(),
        );
        f.consumer.update(&mut f.cx, |c, cx| {
            c.scope
                .update_patch(cx, |p| p.direction = Some(direction))
                .unwrap();
            cx.notify();
        });
        f.draw();
        let bounds = f.open_overflow();
        let key = f.state.read_with(&f.cx, |s, _| s.hidden[0].clone());
        let position = gpui::point(bounds.right() - px(2.), bounds.top() + px(10.));
        f.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
            position,
            ..Default::default()
        }));
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let background: gpui::Background = f
                .consumer
                .read(cx)
                .scope
                .snapshot(cx)
                .theme
                .tokens
                .control_item_bg_hover
                .to_hsla()
                .into();
            let scale = window.scale_factor();
            assert!(
                window.rendered_scene().quads.iter().any(|quad| {
                    quad.background == background
                        && (quad.bounds.origin.x.0 / scale - f32::from(bounds.left())).abs() < 0.01
                        && (quad.bounds.size.width.0 / scale - f32::from(bounds.size.width)).abs()
                            < 0.01
                }),
                "overflow hover must cover the entire row, {direction:?}"
            );
        })
        .unwrap();
        f.click_at(position);
        assert_eq!(f.active(), Some(key));
        assert!(!f.state.read_with(&f.cx, |s, cx| s.popup.is_open(cx)));
    }
}

#[test]
fn tabs_overflow_measures_offscreen_custom_content_and_reacts_to_replacement() {
    let mut entries: Vec<_> = (0..40)
        .map(|i| TabItem::new(i.to_string(), "Tab"))
        .collect();
    entries.push(
        TabItem::new("wide", "Short accessible name").label_render(|_, _| {
            div()
                .w(px(260.))
                .h(px(20.))
                .child("Custom label")
                .into_any_element()
        }),
    );
    let mut f = Fixture::new(TabsItems::new(entries).unwrap());
    let bounds = f.open_overflow();
    // The widest option is outside the virtualized viewport. Include its native
    // content width and Ant Design's 12px padding on both sides.
    assert_eq!(bounds.size.width, px(284.));
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_popup_rows < 20));
    f.update(|s, w, cx| {
        s.set_items(
            TabsItems::new((0..40).map(|i| TabItem::new(i.to_string(), "Tab"))).unwrap(),
            w,
            cx,
        )
    });
    assert_eq!(f.open_overflow().size.width, px(120.));
}

#[test]
fn tabs_overflow_explicit_width_fills_rows_and_disabled_rows_do_not_activate() {
    let mut f = Fixture::new(
        TabsItems::new((0..20).map(|i| TabItem::new(i.to_string(), "Tab").disabled(i > 0)))
            .unwrap(),
    );
    f.consumer.update(&mut f.cx, |c, cx| {
        c.popup_width = Some(300.);
        cx.notify();
    });
    f.draw();
    let bounds = f.open_overflow();
    assert_eq!(bounds.size.width, px(300.));
    let position = gpui::point(bounds.right() - px(2.), bounds.top() + px(10.));
    f.click_at(position);
    assert_eq!(f.active().as_deref(), Some("0"));
    assert!(f.state.read_with(&f.cx, |s, cx| s.popup.is_open(cx)));
    // Re-enable the same keyed options while the popup is open.
    f.update(|s, w, cx| {
        s.set_items(
            TabsItems::new((0..20).map(|i| TabItem::new(i.to_string(), "Tab"))).unwrap(),
            w,
            cx,
        )
    });
    let expected = f.state.read_with(&f.cx, |s, _| s.hidden[0].clone());
    f.click_at(position);
    assert_eq!(f.active(), Some(expected));
}

#[test]
fn tabs_overflow_editable_long_labels_reserve_the_trailing_remove_button() {
    let mut f = Fixture::new(
        TabsItems::new((0..20).map(|i| {
            TabItem::new(
                i.to_string(),
                "A long tab label that must fit next to its close button",
            )
            .icon(crate::icons::IconName::HeartOutlined)
        }))
        .unwrap(),
    );
    f.consumer.update(&mut f.cx, |c, cx| {
        c.kind = TabsType::EditableCard;
        cx.notify();
    });
    f.draw();
    let bounds = f.open_overflow();
    assert!(bounds.size.width > px(300.));
    let expected = f.state.read_with(&f.cx, |s, _| s.hidden[0].clone());
    f.events.borrow_mut().clear();
    f.click_at(gpui::point(
        bounds.right() - px(24.),
        bounds.top() + px(19.),
    ));
    assert!(f.events.borrow().iter().any(|event| matches!(event,
        TabsEvent::Edit { action: TabsEdit::Remove(key), source: TabsSource::Overflow } if key == &expected
    )));
    assert_eq!(f.active().as_deref(), Some("0"));
}

#[test]
fn tabs_overflow_virtualizes_long_lists_and_keyboard_reveals_the_last_row() {
    let mut f = Fixture::new(
        TabsItems::new(
            (0..1_000).map(|i| TabItem::new(i.to_string(), format!("Tab {i}")).disabled(i == 999)),
        )
        .unwrap(),
    );
    let popup = f.state.read_with(&f.cx, |s, _| s.popup.clone());
    f.cx.update_window(f.window.into(), |_, w, cx| popup.set_open(true, w, cx))
        .unwrap();
    f.draw();
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_popup_rows < 20));
    f.update(|s, w, cx| s.popup_focus.focus(w, cx));
    f.key("end");
    f.draw();
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.popup_active.clone())
            .as_deref(),
        Some("998")
    );
    // ScrollHandle's logical child index is local to a rendered virtual slice.
    // The native offset, rather than that local index, verifies distant reveal.
    assert!(f.state.read_with(
        &f.cx,
        |s, _| s.popup_scroll.0.borrow().base_handle.offset().y < px(-27_000.)
    ));
    f.key("enter");
    f.draw();
    assert_eq!(f.active().as_deref(), Some("998"));
    assert!(!f.cx.read(|cx| popup.is_open(cx)));
}

#[test]
fn tabs_first_visit_fades_in_and_reduced_motion_finishes_without_idle_frames() {
    use std::time::Duration;
    let mut f = Fixture::new(items());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.animation = true.into();
        c.scope.update_patch(cx, |p| p.motion = Some(true)).unwrap();
        cx.notify();
    });
    f.draw();
    f.click("two");
    assert!(f.state.read_with(
        &f.cx,
        |s, cx| s.panes["two"].opacity.sample(s.now(cx)).value < 1.
    ));
    f.cx.executor().advance_clock(Duration::from_millis(150));
    f.draw();
    let middle = f.state.read_with(&f.cx, |s, cx| {
        s.panes["two"].opacity.sample(s.now(cx)).value
    });
    assert!(middle > 0. && middle < 1.);
    f.click("one");
    f.consumer.update(&mut f.cx, |c, cx| {
        c.scope
            .update_patch(cx, |p| p.motion = Some(false))
            .unwrap();
        cx.notify();
    });
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        w.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    assert_eq!(
        f.state.read_with(&f.cx, |s, cx| s.panes["one"]
            .opacity
            .sample(s.now(cx))
            .value),
        1.
    );
    assert_eq!(
        f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap(),
        0
    );
}

#[test]
fn tabs_force_render_and_item_destroy_override_have_independent_lifetimes() {
    let mut f = Fixture::new(
        TabsItems::new([
            TabItem::new("one", "One").text("One"),
            TabItem::new("two", "Two").force_render(true).text("Two"),
            TabItem::new("three", "Three")
                .destroy_on_hidden(false)
                .text("Three"),
        ])
        .unwrap(),
    );
    assert!(f.state.read_with(&f.cx, |s, _| s.pane("two").is_some()));
    assert!(f.state.read_with(&f.cx, |s, _| s.pane("three").is_none()));
    f.click("three");
    let id = f
        .state
        .read_with(&f.cx, |s, _| s.pane("three").unwrap().entity_id());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.destroy = true;
        cx.notify();
    });
    f.draw();
    f.click("one");
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.pane("three").unwrap().entity_id()),
        id
    );
    let forced = f
        .state
        .read_with(&f.cx, |s, _| s.pane("two").unwrap().entity_id());
    f.draw();
    f.draw();
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, _| s.pane("two").unwrap().entity_id()),
        forced
    );
}

#[test]
fn tabs_indicator_alignment_tracks_native_bounds_on_both_axes() {
    for placement in [TabPlacement::Top, TabPlacement::Start] {
        for (align, fraction) in [
            (TabsIndicatorAlign::Start, 0.),
            (TabsIndicatorAlign::Center, 0.5),
            (TabsIndicatorAlign::End, 1.),
        ] {
            let mut f = Fixture::new(items());
            f.consumer.update(&mut f.cx, |c, cx| {
                c.placement = placement;
                c.indicator = TabsIndicator::fixed(10.).align(align);
                cx.notify();
            });
            f.draw();
            f.click("two");
            f.draw();
            f.state.read_with(&f.cx, |s, _| {
                let span = s.spans["two"];
                assert!(
                    (s.indicator_start.target()
                        - (span.start + (span.length - 10.) * fraction) as f64)
                        .abs()
                        < 0.01
                );
                assert_eq!(s.indicator_length.target(), 10.);
            });
        }
    }
}

#[test]
fn tabs_centered_indicator_paints_at_the_selected_tab() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        for placement in [
            TabPlacement::Top,
            TabPlacement::Bottom,
            TabPlacement::Start,
            TabPlacement::End,
        ] {
            let mut f = Fixture::new(
                TabsItems::new([TabItem::new("one", "One"), TabItem::new("two", "Two")]).unwrap(),
            );
            f.consumer.update(&mut f.cx, |c, cx| {
                c.placement = placement;
                c.centered = true;
                c.scope
                    .update_patch(cx, |p| p.direction = Some(direction))
                    .unwrap();
                cx.notify();
            });
            for width in [420., 70., 360.] {
                f.consumer.update(&mut f.cx, |c, cx| {
                    c.width = width;
                    cx.notify();
                });
                for key in ["one", "two"] {
                    f.update(|s, _, cx| s.set_active_key(Some(key.into()), cx));
                    f.draw();
                    f.draw();
                    let expected = f.state.read_with(&f.cx, |s, _| s.tab_bounds(key).unwrap());
                    f.cx.update_window(f.window.into(), |_, window, _| {
                        let background: gpui::Background = gpui::rgb(0x1677ff).into();
                        let quad = window.rendered_scene().quads.iter()
                            .find(|quad| quad.background == background).expect("painted indicator");
                        let scale = window.scale_factor();
                        let (position, extent, expected_position, expected_extent) = if placement.vertical() {
                            (quad.bounds.origin.y.0 / scale, quad.bounds.size.height.0 / scale,
                             f32::from(expected.top()), f32::from(expected.size.height))
                        } else {
                            (quad.bounds.origin.x.0 / scale, quad.bounds.size.width.0 / scale,
                             f32::from(expected.left()), f32::from(expected.size.width))
                        };
                        assert!((position - expected_position).abs() < 0.01,
                            "{placement:?}/{direction:?}/{key}/{width}: ink {position}, tab {expected_position}");
                        assert!((extent - expected_extent).abs() < 0.01);
                    }).unwrap();
                }
            }
        }
    }
}

#[test]
fn tabs_native_add_and_remove_buttons_activate_from_keyboard() {
    let mut f = Fixture::new(TabsItems::new([TabItem::new("one", "One")]).unwrap());
    f.consumer.update(&mut f.cx, |c, cx| {
        c.kind = TabsType::EditableCard;
        cx.notify();
    });
    f.draw();
    f.update(|s, w, cx| s.focus_tab("one", w, cx));
    f.events.borrow_mut().clear();
    f.key("tab");
    f.key("enter");
    assert!(f.events.borrow().iter().any(|event|matches!(event,TabsEvent::Edit {action:TabsEdit::Remove(key),source:TabsSource::Keyboard} if key.as_ref()=="one")), "events {:?}",f.events.borrow());
    f.events.borrow_mut().clear();
    f.key("tab");
    f.key("space");
    assert!(f.events.borrow().iter().any(|event| matches!(
        event,
        TabsEvent::Edit {
            action: TabsEdit::Add,
            source: TabsSource::Keyboard
        }
    )));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.items().len()), 1);
}

#[test]
fn tabs_scoped_disabling_rejects_stale_selection_and_edit_requests() {
    let mut f = Fixture::new(items());
    f.click("one");
    f.consumer.update(&mut f.cx, |c, cx| {
        c.kind = TabsType::EditableCard;
        c.scope
            .update_patch(cx, |p| p.disabled = Some(true))
            .unwrap();
        cx.notify();
    });
    f.draw();
    f.events.borrow_mut().clear();
    f.click("two");
    f.key("enter");
    f.key("delete");
    f.update(|s, w, cx| s.edit(TabsEdit::Add, TabsSource::Pointer, w, cx));
    assert!(f.events.borrow().is_empty());
    assert_eq!(f.active().as_deref(), Some("one"));
}
