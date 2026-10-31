// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, Direction, UiRoot};
use gpui::{
    AppContext, Bounds, Context, Entity, Focusable, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, Modifiers, Pixels, PlatformInput, Render, StyleRefinement, TestAppContext,
    VisualTestContext, Window, WindowHandle, div, prelude::*, px, size,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

fn data() -> MenuItems {
    MenuItems::new([
        MenuItem::new("first", "First").into(),
        MenuItem::new("disabled", "Disabled").disabled(true).into(),
        SubMenu::new(
            "branch",
            "Branch",
            MenuItems::new([
                MenuItem::new("child", "Child").into(),
                SubMenu::new(
                    "nested",
                    "Nested",
                    MenuItems::new([MenuItem::new("deep", "Deep").into()]).unwrap(),
                )
                .into(),
            ])
            .unwrap(),
        )
        .into(),
        MenuItem::new("last", "Last").danger(true).into(),
    ])
    .unwrap()
}
struct Harness {
    state: Entity<MenuState>,
    scope: ConfigScope,
    options: MenuOptions,
    width: f32,
    max_height: Option<Pixels>,
    controlled_open: Option<Vec<MenuKey>>,
    echo_open: bool,
    popup_calls: Option<Rc<Cell<usize>>>,
    custom_expand: bool,
    shown: bool,
    events: Rc<RefCell<Vec<MenuEvent>>>,
}
impl Render for Harness {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let events = self.events.clone();
        let host = cx.weak_entity();
        let echo_open = self.echo_open;
        div()
            .size_full()
            .p(px(20.))
            .when(self.shown, |root| {
                root.child(
                    div().debug_selector(|| "menu-frame".into()).child(
                        Menu::new("native-menu", &self.state)
                            .scope(self.scope.clone())
                            .options(self.options.clone())
                            .when_some(self.max_height, |menu, height| menu.max_height(height))
                            .when_some(self.controlled_open.clone(), |menu, keys| {
                                menu.open_keys(keys)
                            })
                            .when_some(self.popup_calls.clone(), |menu, calls| {
                                menu.popup_render(move |content, _, _, _| {
                                    calls.set(calls.get() + 1);
                                    content
                                })
                            })
                            .when(self.custom_expand, |menu| {
                                menu.expand_icon(|context, _, _| {
                                    let key = context.key.clone();
                                    div()
                                        .debug_selector(move || format!("custom-expand-{key}"))
                                        .line_height(px(22.))
                                        .child(if context.expanded { "−" } else { "+" })
                                })
                            })
                            .automation_id("native-menu")
                            .styles(MenuStyles {
                                root: if self.options.collapsed() {
                                    StyleRefinement::default()
                                } else {
                                    StyleRefinement::default().w(px(self.width))
                                },
                                ..Default::default()
                            })
                            .on_event(move |event, _, cx| {
                                events.borrow_mut().push(event.clone());
                                if echo_open && let MenuEvent::OpenChange { open_keys, .. } = event
                                {
                                    let _ = host.update(cx, |h, cx| {
                                        h.controlled_open = Some(open_keys.clone());
                                        cx.notify();
                                    });
                                }
                            }),
                    ),
                )
            })
            .child(crate::Button::new("after-menu").label("After"))
    }
}
struct Fixture {
    cx: VisualTestContext,
    state: Entity<MenuState>,
    host: Entity<Harness>,
    events: Rc<RefCell<Vec<MenuEvent>>>,
    _window: WindowHandle<UiRoot>,
}
impl Fixture {
    fn new(mode: MenuMode) -> Self {
        let mut app = TestAppContext::single();
        let mut output = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let recorded = events.clone();
        let window = app.add_window(|window, cx| {
            crate::init(cx);
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| MenuState::new(data(), window, cx));
            let host = cx.new(|_| Harness {
                state: state.clone(),
                scope: scope.clone(),
                options: MenuOptions {
                    mode,
                    ..Default::default()
                },
                width: 256.,
                max_height: None,
                controlled_open: None,
                echo_open: false,
                popup_calls: None,
                custom_expand: false,
                shown: true,
                events: recorded,
            });
            output = Some((state, host.clone()));
            let root = UiRoot::new(scope, host, window, cx);
            window.focus(&root.focus_handle(cx), cx);
            root
        });
        let (state, host) = output.unwrap();
        let cx = VisualTestContext::from_window(window.into(), &app);
        let mut f = Self {
            cx,
            state,
            host,
            events,
            _window: window,
        };
        f.cx.simulate_resize(size(px(800.), px(600.)));
        f.draw();
        f
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx.update(|w, c| w.draw(c).clear(c));
        self.cx.run_until_parked();
    }
    fn bounds(&mut self, key: &'static str) -> Bounds<Pixels> {
        self.cx
            .debug_bounds(key)
            .unwrap_or_else(|| panic!("missing Menu row {key}"))
    }
    fn click(&mut self, key: &'static str) {
        let position = self.bounds(key).center();
        self.cx.simulate_click(position, Modifiers::none());
        self.draw();
    }
    fn key(&mut self, stroke: &'static str) {
        let key = Keystroke::parse(stroke).unwrap();
        self.cx.update(|window, cx| {
            window.dispatch_event(
                PlatformInput::KeyDown(KeyDownEvent {
                    keystroke: key.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke: key }), cx);
        });
        self.draw();
    }
    fn focus(&mut self, key: &str) {
        let key = key.to_owned().into();
        let state = self.state.clone();
        self.cx.update(|w, c| state::focus_key(&state, key, w, c));
        self.draw();
    }
    fn active(&self) -> Option<MenuKey> {
        self.state.read_with(&self.cx, |s, _| s.active.clone())
    }
    fn open(&self) -> Vec<MenuKey> {
        self.state.read_with(&self.cx, |s, _| s.open.clone())
    }
    fn selected(&self) -> Vec<MenuKey> {
        self.state.read_with(&self.cx, |s, _| s.selected.clone())
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Harness)) {
        self.host.update(&mut self.cx, |h, cx| {
            edit(h);
            cx.notify();
        });
        self.draw();
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.update(|window, cx| window.simulate_next_frame(cx));
        self.draw();
    }
    fn motion(&mut self, enabled: bool) {
        let scope = self.host.read_with(&self.cx, |h, _| h.scope.clone());
        self.cx.update(|_, cx| {
            scope
                .update_patch(cx, |p| p.motion = Some(enabled))
                .unwrap()
        });
        self.draw();
    }
}
#[test]
fn menu_native_pointer_activation_and_reference_row_stride() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let first = f.bounds("first");
    let disabled = f.bounds("disabled");
    assert_eq!(first.size.height, px(40.));
    assert_eq!(disabled.top() - first.top(), px(44.));
    f.click("first");
    assert_eq!(f.selected(), vec!["first"]);
    f.click("disabled");
    assert_eq!(f.selected(), vec!["first"]);
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, MenuEvent::Activate(_)))
            .count(),
        1
    );
}
#[test]
fn menu_native_inline_keyboard_traverses_visible_tree_and_skips_disabled() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.focus("first");
    f.key("down");
    assert_eq!(f.active(), Some("branch".into()));
    f.key("enter");
    assert_eq!(f.open(), vec!["branch"]);
    assert!(f.cx.debug_bounds("child").is_some());
    f.key("down");
    assert_eq!(f.active(), Some("child".into()));
    f.key("space");
    assert_eq!(f.selected(), vec!["child"]);
    f.key("end");
    assert_eq!(f.active(), Some("last".into()));
    f.key("down");
    assert_eq!(f.active(), Some("first".into()));
    f.key("l");
    assert_eq!(f.active(), Some("last".into()));
}
#[test]
fn menu_native_flyout_keyboard_open_nested_escape_and_activation() {
    let mut f = Fixture::new(MenuMode::Vertical);
    f.edit(|h| h.custom_expand = true);
    let assert_custom_centered = |f: &mut Fixture, key: &'static str, selector: &'static str| {
        let row = f.bounds(key);
        let content = f.bounds(selector);
        assert_eq!(content.size.height, px(22.));
        assert!(
            (content.center().y - row.center().y).abs() <= px(1.),
            "custom expand content stays vertically centered in {key}"
        );
    };
    assert_custom_centered(&mut f, "branch", "custom-expand-branch");
    f.focus("branch");
    f.key("right");
    assert_eq!(f.open(), vec!["branch"]);
    assert!(f.cx.debug_bounds("child").is_some());
    assert_custom_centered(&mut f, "nested", "custom-expand-nested");
    f.key("down");
    assert_eq!(f.active(), Some("nested".into()));
    f.key("right");
    assert!(f.open().contains(&"nested".into()));
    f.key("escape");
    assert_eq!(f.active(), Some("nested".into()));
    assert_eq!(f.open(), vec!["branch"]);
    f.key("home");
    f.key("enter");
    assert_eq!(f.selected(), vec!["child"]);
    assert!(f.open().is_empty());
}
#[test]
fn menu_native_collapse_restores_open_keys_and_hides_inline_children() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.click("branch");
    assert_eq!(f.open(), vec!["branch"]);
    f.edit(|h| h.options.inline_collapsed = true);
    assert!(f.open().is_empty());
    assert!(f.cx.debug_bounds("child").is_none());
    assert_eq!(f.bounds("first").size.width, px(71.));
    let first = f.bounds("first");
    let scope = f.host.read_with(&f.cx, |h, _| h.scope.clone());
    let (tooltip_background, label_width) = f.cx.update(|window, cx| {
        let config = scope.snapshot(cx);
        let style = crate::tooltip::TooltipStyle::resolve(
            &config.theme,
            None,
            crate::popup::PopupPlacement::Right,
        );
        let title: gpui::SharedString = "First".into();
        let run = gpui::TextRun {
            len: title.len(),
            font: config.font.clone(),
            color: style.foreground,
            background_color: None,
            underline: None,
            strikethrough: None,
        };
        let width = window
            .text_system()
            .shape_line(title, px(style.font_size as f32), &[run], None)
            .width
            + px((style.padding_inline * 2.) as f32);
        (style.background, width)
    });
    let painted_tooltip = |fixture: &mut Fixture| {
        fixture.cx.update(|window, _| {
            let scale = window.scale_factor();
            window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| quad.background.as_solid() == Some(tooltip_background))
                .map(|quad| {
                    Bounds::new(
                        gpui::point(
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
    };
    f.cx.simulate_event(gpui::MouseMoveEvent {
        position: first.center(),
        pressed_button: None,
        modifiers: Modifiers::none(),
    });
    f.draw();
    f.advance(120);
    let tooltip = painted_tooltip(&mut f).expect("collapsed leaf paints its tooltip");
    assert!(
        tooltip.left() >= first.right(),
        "LTR tooltip appears beside the leaf"
    );
    assert!((tooltip.center().y - first.center().y).abs() <= px(1.));
    assert!(
        tooltip.size.width + px(1.) >= label_width,
        "tooltip fits the full label fallback rather than the collapsed initial"
    );
    f.edit(|h| h.options.tooltip = false);
    f.advance(120);
    assert!(
        painted_tooltip(&mut f).is_none(),
        "tooltip(false) removes its surface"
    );
    f.edit(|h| h.options.inline_collapsed = false);
    assert_eq!(f.open(), vec!["branch"]);
    assert!(f.cx.debug_bounds("child").is_some());
}
#[test]
fn menu_native_horizontal_overflow_resizes_without_losing_selection() {
    let mut f = Fixture::new(MenuMode::Horizontal);
    f.edit(|h| h.width = 110.);
    let omitted = f.state.read_with(&f.cx, |s, _| s.overflow_start);
    assert!(omitted < 4);
    assert!(f.cx.debug_bounds("last").is_none());
    f.state
        .update(&mut f.cx, |s, cx| s.set_selected_keys(["last"], cx));
    f.draw();
    f.edit(|h| h.width = 700.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.overflow_start), 4);
    assert!(f.cx.debug_bounds("last").is_some());
    assert_eq!(f.selected(), vec!["last"]);
}
#[test]
fn menu_native_rtl_reverses_horizontal_navigation_and_flyout_direction() {
    let mut f = Fixture::new(MenuMode::Horizontal);
    f.edit(|h| h.width = 700.);
    let scope = f.host.read_with(&f.cx, |h, _| h.scope.clone());
    f.cx.update(|_, cx| {
        scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    f.focus("first");
    f.key("left");
    assert_eq!(f.active(), Some("branch".into()));
    f.key("down");
    assert!(f.open().contains(&"branch".into()));
}
#[test]
fn menu_native_unmount_cancels_delayed_hover_work() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let target = f.bounds("branch").center();
    f.cx.simulate_event(gpui::MouseMoveEvent {
        position: target,
        pressed_button: None,
        modifiers: Modifiers::none(),
    });
    f.draw();
    f.edit(|h| h.shown = false);
    f.cx.executor().advance_clock(Duration::from_millis(150));
    f.draw();
    assert!(!f.state.read_with(&f.cx, |s, _| s.mounted));
    assert!(f.open().is_empty());
}

#[test]
fn menu_native_hover_delay_bridge_and_click_trigger() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let target = f.bounds("branch").center();
    f.cx.simulate_event(gpui::MouseMoveEvent {
        position: target,
        pressed_button: None,
        modifiers: Modifiers::none(),
    });
    f.draw();
    assert!(f.open().is_empty());
    f.cx.executor().advance_clock(Duration::from_millis(99));
    f.draw();
    assert!(f.open().is_empty());
    f.cx.executor().advance_clock(Duration::from_millis(2));
    f.draw();
    assert_eq!(f.open(), vec!["branch"]);
    let child = f.bounds("child").center();
    f.cx.simulate_event(gpui::MouseMoveEvent {
        position: child,
        pressed_button: None,
        modifiers: Modifiers::none(),
    });
    f.draw();
    f.cx.executor().advance_clock(Duration::from_millis(150));
    f.draw();
    assert_eq!(f.open(), vec!["branch"]);
    f.edit(|h| h.options.trigger = MenuTrigger::Click);
    f.state.update(&mut f.cx, |s, cx| {
        s.set_open_keys(Vec::<MenuKey>::new(), cx)
    });
    f.draw();
    f.cx.simulate_event(gpui::MouseMoveEvent {
        position: target,
        pressed_button: None,
        modifiers: Modifiers::none(),
    });
    f.draw();
    f.cx.executor().advance_clock(Duration::from_millis(150));
    f.draw();
    assert!(f.open().is_empty());
    f.click("branch");
    assert_eq!(f.open(), vec!["branch"]);
    let child = f.bounds("child").center();
    for position in [child, gpui::point(px(700.), px(500.))] {
        f.cx.simulate_event(gpui::MouseMoveEvent {
            position,
            pressed_button: None,
            modifiers: Modifiers::none(),
        });
        f.draw();
    }
    f.advance(150);
    assert_eq!(
        f.open(),
        vec!["branch"],
        "click-triggered popups remain open on pointer leave"
    );
}

#[test]
fn menu_native_overflow_keyboard_visits_indicator_and_hidden_items() {
    let mut f = Fixture::new(MenuMode::Horizontal);
    f.edit(|h| h.width = 110.);
    let state = f.state.clone();
    f.cx.update(|window, cx| state.update(cx, |s, cx| s.focus(window, cx)));
    f.draw();
    f.key("end");
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| state.read(cx).overflow_focus.is_focused(window)));
    f.key("down");
    assert!(f.state.read_with(&f.cx, |s, _| s.overflow_open));
    f.key("end");
    assert_eq!(f.active(), Some("last".into()));
    f.key("enter");
    assert_eq!(f.selected(), vec!["last"]);
    assert!(!f.state.read_with(&f.cx, |s, _| s.overflow_open));
}

#[test]
fn menu_native_disabled_root_does_not_accept_keyboard_or_pointer_activation() {
    let mut f = Fixture::new(MenuMode::Inline);
    let scope = f.host.read_with(&f.cx, |h, _| h.scope.clone());
    f.cx.update(|_, cx| scope.update_patch(cx, |p| p.disabled = Some(true)).unwrap());
    f.draw();
    f.click("first");
    f.key("enter");
    assert!(f.selected().is_empty());
    assert!(f.events.borrow().is_empty());
}

#[test]
fn menu_native_bounded_root_materializes_visible_rows_and_reveals_end() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let rows = Rc::new(RefCell::new(std::collections::HashSet::new()));
    let items = MenuItems::new((0..1000).map(|index| {
        let rows = rows.clone();
        MenuItem::new(format!("row-{index}"), format!("Item {index}"))
            .content(move |_, _, _| {
                rows.borrow_mut().insert(index);
                div().child(format!("Item {index}"))
            })
            .into()
    }))
    .unwrap();
    f.edit(|h| h.max_height = Some(px(240.)));
    f.state.update(&mut f.cx, |s, cx| s.set_items(items, cx));
    f.draw();
    assert!(
        !rows.borrow().is_empty() && rows.borrow().len() < 30,
        "only viewport rows rendered: {}",
        rows.borrow().len()
    );
    f.focus("row-0");
    rows.borrow_mut().clear();
    f.key("end");
    assert_eq!(f.active(), Some("row-999".into()));
    assert!(f.cx.debug_bounds("row-999").is_some());
    assert!(
        rows.borrow().len() < 30,
        "jump materialized a bounded range: {}",
        rows.borrow().len()
    );
}

#[test]
fn menu_native_force_render_materializes_closed_content_only_when_requested() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let calls = Rc::new(Cell::new(0));
    let counter = calls.clone();
    let items = MenuItems::new([SubMenu::new(
        "branch",
        "Branch",
        MenuItems::new([MenuItem::new("prepared", "Prepared")
            .content(move |_, _, _| {
                counter.set(counter.get() + 1);
                div().child("Prepared")
            })
            .into()])
        .unwrap(),
    )
    .into()])
    .unwrap();
    f.state.update(&mut f.cx, |s, cx| s.set_items(items, cx));
    f.draw();
    assert_eq!(calls.get(), 0);
    let popup_calls = Rc::new(Cell::new(0));
    f.edit(|h| {
        h.options.force_submenu_render = true;
        h.popup_calls = Some(popup_calls.clone());
    });
    assert!(calls.get() > 0);
    assert!(
        popup_calls.get() > 0,
        "eager preparation also calls the custom popup wrapper"
    );
    assert!(f.open().is_empty());
    assert!(f.cx.debug_bounds("prepared").is_none());
    f.edit(|h| h.options.mode = MenuMode::Inline);
    assert_eq!(
        f.bounds("menu-frame").size.height,
        px(48.),
        "eager hidden inline content occupies no extra height"
    );
    f.click("branch");
    assert_eq!(f.bounds("menu-frame").size.height, px(96.));
}

#[test]
fn menu_native_data_removal_repairs_owned_focus_to_an_available_row() {
    let mut f = Fixture::new(MenuMode::Vertical);
    f.focus("last");
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(
            MenuItems::new([MenuItem::new("replacement", "Replacement").into()]).unwrap(),
            cx,
        )
    });
    f.draw();
    assert_eq!(f.active(), Some("replacement".into()));
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| {
        state
            .read(cx)
            .row_focus
            .get("replacement")
            .unwrap()
            .is_focused(window)
    }));
}

#[test]
fn menu_native_inline_motion_interruption_and_reduced_motion_preserve_geometry() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.motion(true);
    let closed = f.bounds("menu-frame").size.height;
    f.click("branch");
    f.advance(100);
    let middle = f.bounds("menu-frame").size.height;
    assert!(middle > closed, "opening height must advance");
    f.click("branch");
    let reversed = f.bounds("menu-frame").size.height;
    assert!(
        (reversed - middle).abs() <= px(1.),
        "interruption must start at painted geometry"
    );
    f.advance(50);
    let closing = f.bounds("menu-frame").size.height;
    assert!(closing < middle && closing > closed);
    f.motion(false);
    assert_eq!(f.bounds("menu-frame").size.height, closed);
    f.click("branch");
    let expanded = f.bounds("menu-frame").size.height;
    assert!(expanded > middle);
}

#[test]
fn menu_native_collapse_width_animates_then_reduced_motion_settles() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.motion(true);
    let expanded = f.bounds("first").size.width;
    f.edit(|h| h.options.inline_collapsed = true);
    f.advance(150);
    let middle = f.bounds("first").size.width;
    assert!(
        middle < expanded && middle > px(71.),
        "collapse width should interpolate: {middle:?}"
    );
    f.motion(false);
    assert_eq!(f.bounds("first").size.width, px(71.));
}

#[test]
fn menu_native_held_enter_does_not_toggle_a_submenu_repeatedly() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.focus("branch");
    f.key("enter");
    let repeated = Keystroke::parse("enter").unwrap();
    f.cx.update(|window, cx| {
        for _ in 0..3 {
            window.dispatch_event(
                PlatformInput::KeyDown(KeyDownEvent {
                    keystroke: repeated.clone(),
                    is_held: true,
                    prefer_character_input: false,
                }),
                cx,
            );
        }
    });
    f.draw();
    assert_eq!(f.open(), vec!["branch"]);
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, MenuEvent::TitleClick(_)))
            .count(),
        1
    );
}

#[test]
fn menu_native_popup_measures_custom_content_intrinsic_width() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let items = MenuItems::new([SubMenu::new(
        "branch",
        "Branch",
        MenuItems::new([MenuItem::new("wide", "Short")
            .content(|_, _, _| div().w(px(240.)).child("Custom content"))
            .into()])
        .unwrap(),
    )
    .into()])
    .unwrap();
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(items, cx);
        s.set_open_keys(["branch"], cx);
    });
    f.draw();
    assert!(
        f.bounds("wide").size.width >= px(272.),
        "custom content and both16px row paddings fit"
    );
}

#[test]
fn menu_native_unmount_releases_an_open_popup_and_its_retained_factories() {
    let mut f = Fixture::new(MenuMode::Vertical);
    f.focus("branch");
    f.key("right");
    assert_eq!(f.open(), vec!["branch"]);
    let popup = f
        .state
        .read_with(&f.cx, |s, _| s.popups.get("branch").unwrap().clone());
    f.edit(|h| h.shown = false);
    assert!(!f.state.read_with(&f.cx, |s, _| s.mounted));
    assert!(!f.cx.update(|_, cx| popup.is_open(cx)));
}

#[test]
fn menu_native_controlled_keyboard_open_moves_focus_only_after_owner_accepts() {
    let mut f = Fixture::new(MenuMode::Vertical);
    f.edit(|h| h.controlled_open = Some(Vec::new()));
    f.focus("branch");
    f.key("right");
    assert!(f.open().is_empty());
    assert_eq!(f.active(), Some("branch".into()));
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| {
        state
            .read(cx)
            .row_focus
            .get("branch")
            .unwrap()
            .is_focused(window)
    }));
    f.edit(|h| h.echo_open = true);
    f.key("right");
    assert_eq!(f.open(), vec!["branch"]);
    assert_eq!(f.active(), Some("child".into()));
    f.edit(|h| {
        h.echo_open = false;
        h.controlled_open = Some(Vec::new());
    });
    assert_eq!(f.active(), Some("branch".into()));
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| {
        state
            .read(cx)
            .row_focus
            .get("branch")
            .unwrap()
            .is_focused(window)
    }));
}

#[test]
fn menu_native_tab_preserves_persistent_inline_expansion() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.focus("branch");
    f.key("enter");
    f.key("down");
    assert_eq!(f.active(), Some("child".into()));
    f.key("tab");
    assert_eq!(f.open(), vec!["branch"]);
    let state = f.state.clone();
    assert!(!f.cx.update(|window, cx| {
        state
            .read(cx)
            .row_focus
            .values()
            .any(|focus| focus.is_focused(window))
    }));
}

#[test]
fn menu_native_queued_keyboard_event_cannot_activate_a_closed_popup_child() {
    let mut f = Fixture::new(MenuMode::Vertical);
    f.focus("branch");
    f.key("right");
    assert_eq!(f.active(), Some("child".into()));
    // Exercise an old popup callback directly: GPUI's dispatch_key_event redraws
    // dirty windows before routing real keys, repairing focus to the title first.
    let state = f.state.clone();
    f.cx.update(|window, cx| {
        state.update(cx, |s, cx| s.set_open_keys(Vec::<MenuKey>::new(), cx));
        let keystroke = Keystroke::parse("enter").unwrap();
        state::key(
            &state,
            Some(&MenuKey::from("branch")),
            &KeyDownEvent {
                keystroke: keystroke.clone(),
                is_held: false,
                prefer_character_input: false,
            },
            window,
            cx,
        );
    });
    f.draw();
    assert!(f.selected().is_empty());
    assert!(f.open().is_empty());
    f.key("enter");
    assert_eq!(
        f.open(),
        vec!["branch"],
        "the next real key correctly activates the restored submenu title"
    );
}

#[test]
fn menu_native_grouped_popup_virtualizes_and_reveals_the_last_leaf() {
    for count in [20, 1000] {
        let mut f = Fixture::new(MenuMode::Vertical);
        let rows = Rc::new(RefCell::new(std::collections::HashSet::new()));
        let children = MenuItems::new((0..count).map(|index| {
            let rows = rows.clone();
            MenuItem::new(format!("group-row-{index}"), format!("Item {index}"))
                .content(move |_, _, _| {
                    rows.borrow_mut().insert(index);
                    div().child(format!("Item {index}"))
                })
                .into()
        }))
        .unwrap();
        let data = MenuItems::new([SubMenu::new(
            "branch",
            "Branch",
            MenuItems::new([MenuGroup::new("group", "Group", children).into()]).unwrap(),
        )
        .into()])
        .unwrap();
        f.state.update(&mut f.cx, |s, cx| {
            s.set_items(data, cx);
            s.set_open_keys(["branch"], cx);
        });
        f.draw();
        assert!(
            !rows.borrow().is_empty() && rows.borrow().len() < 40,
            "grouped popup materializes onlyviewport rows: {}",
            rows.borrow().len()
        );
        f.focus("group-row-0");
        rows.borrow_mut().clear();
        f.key("end");
        let last = if count == 20 {
            "group-row-19"
        } else {
            "group-row-999"
        };
        assert_eq!(f.active(), Some(last.into()));
        let bounds = f.bounds(last);
        assert!(
            bounds.bottom() <= px(592.),
            "keyboard target remains in the popup viewport, count={count}"
        );
        assert!(rows.borrow().len() < 40);
    }
}

#[test]
fn menu_native_virtual_popup_measures_custom_content_after_the_sixty_four_item_boundary() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let children = MenuItems::new((0..65).map(|index| {
        MenuItem::new(format!("wide-{index}"), "Short")
            .content(move |_, _, _| {
                div()
                    .w(px(if index == 64 { 320. } else { 240. }))
                    .child("Custom content")
            })
            .into()
    }))
    .unwrap();
    let data = MenuItems::new([SubMenu::new("branch", "Branch", children).into()]).unwrap();
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(data, cx);
        s.set_open_keys(["branch"], cx);
    });
    f.draw();
    assert!(f.bounds("wide-0").size.width >= px(272.));
    f.focus("wide-0");
    f.key("end");
    assert!(
        f.bounds("wide-64").size.width >= px(352.),
        "newly active wide custom row remains unclipped"
    );
}

#[test]
fn menu_native_model_replacement_releases_removed_popup_content_captures() {
    let mut f = Fixture::new(MenuMode::Vertical);
    let lease = Rc::new(());
    let weak = Rc::downgrade(&lease);
    let data = MenuItems::new([SubMenu::new(
        "old-branch",
        "Old branch",
        MenuItems::new([MenuItem::new("old-child", "Old child")
            .content(move |_, _, _| {
                let _keep_alive = &lease;
                div().child("Old content")
            })
            .into()])
        .unwrap(),
    )
    .into()])
    .unwrap();
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(data, cx);
        s.set_open_keys(["old-branch"], cx);
    });
    f.draw();
    assert!(weak.upgrade().is_some());
    f.state.update(&mut f.cx, |s, cx| {
        s.set_items(
            MenuItems::new([MenuItem::new("replacement", "Replacement").into()]).unwrap(),
            cx,
        )
    });
    f.draw();
    f.draw();
    assert!(
        weak.upgrade().is_none(),
        "removed renderclosures must not remain in historical branch caches"
    );
}

#[test]
fn menu_native_first_arrow_after_tab_entry_focuses_the_first_enabled_item() {
    for mode in [MenuMode::Vertical, MenuMode::Inline, MenuMode::Horizontal] {
        let mut f = Fixture::new(mode);
        if mode == MenuMode::Horizontal {
            f.edit(|h| h.width = 700.);
        }
        f.key("tab");
        let state = f.state.clone();
        assert!(f.cx.update(|window, cx| state.read(cx).focus.is_focused(window)));
        f.key(if mode == MenuMode::Horizontal {
            "right"
        } else {
            "down"
        });
        assert_eq!(f.active(), Some("first".into()), "{mode:?}");
        let state = f.state.clone();
        assert!(f.cx.update(|window, cx| {
            state
                .read(cx)
                .row_focus
                .get("first")
                .unwrap()
                .is_focused(window)
        }));
    }
}

#[test]
fn menu_native_horizontal_resize_transfers_owned_focus_through_the_overflow_indicator() {
    let mut f = Fixture::new(MenuMode::Horizontal);
    f.edit(|h| h.width = 700.);
    f.focus("last");
    f.edit(|h| h.width = 110.);
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| state.read(cx).overflow_focus.is_focused(window)));
    f.edit(|h| h.width = 700.);
    let state = f.state.clone();
    assert!(f.cx.update(|window, cx| {
        state
            .read(cx)
            .row_focus
            .get("last")
            .unwrap()
            .is_focused(window)
    }));
}

#[test]
fn menu_native_component_local_motion_override_settles_without_global_motion_changes() {
    let mut f = Fixture::new(MenuMode::Inline);
    f.motion(true);
    let scope = f.host.read_with(&f.cx, |h, _| h.scope.clone());
    f.cx.update(|_, cx| {
        scope
            .update_patch(cx, |patch| {
                patch.theme.get_or_insert_default().components.set_theme(
                    crate::theme::ComponentThemeConfig::<MenuTheme>::default().with_tokens(
                        crate::theme::TokenPatch {
                            motion: Some(false),
                            ..Default::default()
                        },
                    ),
                );
            })
            .unwrap()
    });
    f.draw();
    f.edit(|h| h.options.inline_collapsed = true);
    assert_eq!(f.bounds("first").size.width, px(71.));
}

#[test]
fn menu_native_empty_group_and_divider_heights_match_independent_browser_measurements() {
    for mode in [MenuMode::Vertical, MenuMode::Inline] {
        let mut f = Fixture::new(mode);
        for (data, height) in [
            (MenuItems::default(), 0.),
            (
                MenuItems::new([
                    MenuGroup::new("empty-group", "Group", MenuItems::default()).into()
                ])
                .unwrap(),
                38.,
            ),
            (
                MenuItems::new([MenuDivider::new("only-divider").into()]).unwrap(),
                3.,
            ),
        ] {
            f.state.update(&mut f.cx, |s, cx| s.set_items(data, cx));
            f.draw();
            assert_eq!(
                f.bounds("menu-frame").size.height,
                px(height),
                "{mode:?} height {height}"
            );
        }
    }
}

#[test]
fn menu_native_single_submenu_outer_margin_height_matches_browser() {
    let mut f = Fixture::new(MenuMode::Inline);
    let data = MenuItems::new([SubMenu::new(
        "only-branch",
        "Branch",
        MenuItems::new([MenuItem::new("only-child", "Child").into()]).unwrap(),
    )
    .into()])
    .unwrap();
    f.state.update(&mut f.cx, |s, cx| s.set_items(data, cx));
    f.draw();
    assert_eq!(f.bounds("menu-frame").size.height, px(48.));
    f.state
        .update(&mut f.cx, |s, cx| s.set_open_keys(["only-branch"], cx));
    f.draw();
    assert_eq!(f.bounds("menu-frame").size.height, px(96.));
}

#[test]
fn menu_native_inline_custom_item_heights_are_preserved_in_natural_and_bounded_layouts() {
    for bounded in [false, true] {
        let mut f = Fixture::new(MenuMode::Inline);
        if bounded {
            f.edit(|h| h.max_height = Some(px(400.)));
        }
        let data = MenuItems::new([SubMenu::new(
            "branch",
            "Branch",
            MenuItems::new([
                MenuItem::new("tall", "Tall row")
                    .style(StyleRefinement::default().h(px(80.)))
                    .into(),
                MenuItem::new("normal", "Normal row").into(),
            ])
            .unwrap(),
        )
        .into()])
        .unwrap();
        f.state.update(&mut f.cx, |s, cx| {
            s.set_items(data, cx);
            s.set_open_keys(["branch"], cx);
        });
        f.draw();
        let tall = f.bounds("tall");
        let normal = f.bounds("normal");
        let frame = f.bounds("menu-frame");
        assert_eq!(tall.size.height, px(80.));
        assert!(
            normal.top() >= tall.bottom() + px(4.),
            "custom height retains row spacing, bounded={bounded}"
        );
        assert!(
            frame.bottom() >= normal.bottom(),
            "expanded content is not clipped, bounded={bounded}"
        );
    }
}
