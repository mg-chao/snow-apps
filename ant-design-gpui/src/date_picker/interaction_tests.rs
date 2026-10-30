// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use chrono::{Datelike, Timelike};
use gpui::{
    AppContext, Context, Entity, EntityInputHandler, Focusable, IntoElement, KeyDownEvent,
    KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, Pixels, PlatformInput, Point,
    Render, SharedString, Subscription, TestAppContext, Window, WindowHandle, div, point,
    prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::Arc,
    time::Duration,
};

fn date(day: u32) -> NaiveDate {
    NaiveDate::from_ymd_opt(2024, 2, day).unwrap()
}
fn now() -> NaiveDateTime {
    date(20).and_hms_milli_opt(13, 24, 35, 123).unwrap()
}
type TestCellRender = Rc<
    dyn Fn(&PickerCellContext, gpui::AnyElement, &mut Window, &mut gpui::App) -> gpui::AnyElement,
>;

struct Consumer<S: PickerSelection> {
    state: Entity<PickerState<S>>,
    scope: ConfigScope,
    options: PickerOptions,
    format: PickerFormat,
    controlled: Option<S::Value>,
    controlled_open: Option<bool>,
    picker_value: Option<NaiveDate>,
    panel_mode: Option<PickerPanelMode>,
    disabled_date: Option<super::state::DisabledDate>,
    disabled_time: Option<super::state::DisabledTimeFactory>,
    cell_render: Option<TestCellRender>,
    presets: Vec<PickerPreset<S>>,
    outside_focus: gpui::FocusHandle,
    accept: bool,
    reentrant: bool,
    disabled: bool,
    shown: bool,
    initial_open: bool,
    inline: bool,
}
impl<S: PickerSelection> Render for Consumer<S> {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().size_full().p(px(48.));
        if self.shown {
            let mut picker = Picker::<S>::new("picker", &self.state)
                .scope(self.scope.clone())
                .options(self.options.clone())
                .format(self.format.clone())
                .disabled(self.disabled)
                .default_open(self.initial_open)
                .clock(now)
                .allow_clear(true)
                .presets(self.presets.clone())
                .automation_id("test.picker")
                .on_change(
                    cx.listener(|this, change: &PickerChange<S::Value>, window, cx| {
                        if this.accept && this.controlled.is_some() {
                            this.controlled = Some(change.value.clone());
                            cx.notify();
                        }
                        if this.reentrant {
                            this.state
                                .update(cx, |s, c| s.set_value(change.value.clone(), window, c));
                        }
                    }),
                );
            if let Some(value) = &self.controlled {
                picker = picker.value(value.clone());
            }
            if let Some(open) = self.controlled_open {
                picker = picker.open(open);
            }
            if let Some(value) = self.picker_value {
                picker = picker.picker_value(value);
            }
            if let Some(mode) = self.panel_mode {
                picker = picker.panel_mode(mode);
            }
            if let Some(disabled_date) = &self.disabled_date {
                let predicate = disabled_date.clone();
                picker = picker.disabled_date(move |date, info| predicate(date, info));
            }
            if let Some(disabled_time) = &self.disabled_time {
                let factory = disabled_time.clone();
                picker = picker.disabled_time(move |value, endpoint| factory(value, endpoint));
            }
            if let Some(render) = &self.cell_render {
                let render = render.clone();
                picker = picker.cell_render(move |context, content, window, cx| {
                    render(context, content, window, cx)
                });
            }
            root = root.child(if self.inline {
                picker.into_panel().into_any_element()
            } else {
                picker.into_any_element()
            });
        }
        root.child(
            div()
                .id("outside")
                .track_focus(&self.outside_focus)
                .absolute()
                .left(px(500.))
                .top(px(600.))
                .size(px(40.)),
        )
    }
}

struct Fixture<S: PickerSelection> {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    state: Entity<PickerState<S>>,
    consumer: Entity<Consumer<S>>,
    events: Rc<RefCell<Vec<PickerEvent<S::Value>>>>,
    _subscription: Subscription,
}
impl<S: PickerSelection> Fixture<S> {
    fn new(value: S::Value) -> Self {
        Self::with_open(value, true)
    }
    fn with_open(value: S::Value, open: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            crate::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| {
                let mut s = PickerState::<S>::new(window, cx);
                s.set_value(value, window, cx);
                s
            });
            let subscription = cx.subscribe(&state, move |_, _, event, _| {
                output.borrow_mut().push(event.clone())
            });
            let consumer = cx.new(|cx| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                options: PickerOptions::default(),
                format: PickerFormat::default(),
                controlled: None,
                controlled_open: None,
                picker_value: None,
                panel_mode: None,
                disabled_date: None,
                disabled_time: None,
                cell_render: None,
                presets: Vec::new(),
                outside_focus: cx.focus_handle().tab_stop(false),
                accept: false,
                reentrant: false,
                disabled: false,
                shown: true,
                initial_open: open,
                inline: false,
            });
            retained = Some((state, consumer.clone(), subscription));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (state, consumer, subscription) = retained.unwrap();
        let mut result = Self {
            cx,
            window,
            state,
            consumer,
            events,
            _subscription: subscription,
        };
        result.draw();
        result.draw();
        result
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, c| w.draw(c).clear(c))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn update<R>(
        &mut self,
        f: impl FnOnce(&mut PickerState<S>, &mut Window, &mut Context<PickerState<S>>) -> R,
    ) -> R {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                state.update(c, |s, c| f(s, w, c))
            })
            .unwrap()
    }
    fn configure(&mut self, f: impl FnOnce(&mut Consumer<S>)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            f(consumer);
            cx.notify();
        });
        self.draw();
    }
    fn compact_at_scale(&mut self, scale: f32) {
        let scope = self
            .consumer
            .read_with(&self.cx, |consumer, _| consumer.scope.clone());
        self.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.theme = Some(crate::theme::ThemeConfig {
                        algorithms: Some(crate::theme::AlgorithmChain::compact()),
                        ..Default::default()
                    });
                })
                .unwrap();
        });
        self.cx
            .simulate_window_scale_factor_change(self.window.into(), scale);
        self.draw();
    }
    fn compact_case(&mut self, dark: bool, direction: crate::Direction, scale: f32) {
        self.compact_at_scale(scale);
        let scope = self.consumer.read_with(&self.cx, |v, _| v.scope.clone());
        self.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(direction);
                    if dark {
                        patch.theme = Some(crate::theme::ThemeConfig {
                            algorithms: Some(crate::theme::AlgorithmChain::dark_compact()),
                            ..Default::default()
                        });
                    }
                })
                .unwrap()
        });
        self.draw();
    }
    fn value(&self) -> S::Value {
        self.state.read_with(&self.cx, |s, _| s.value().clone())
    }
    fn draft(&self) -> S::Value {
        self.state.read_with(&self.cx, |s, _| s.draft_value())
    }
    fn open(&self) -> bool {
        self.state.read_with(&self.cx, |s, _| s.is_open())
    }
    fn text(&self) -> Vec<SharedString> {
        self.state.read_with(&self.cx, |s, c| s.input_values(c))
    }
    fn changes(&self) -> Vec<PickerChange<S::Value>> {
        self.events
            .borrow()
            .iter()
            .filter_map(|event| {
                if let PickerEvent::Change(change) = event {
                    Some(change.clone())
                } else {
                    None
                }
            })
            .collect()
    }
    fn observe_invalidations(&mut self) -> (Rc<Cell<usize>>, Subscription) {
        let count = Rc::new(Cell::new(0));
        let output = count.clone();
        let consumer = self.consumer.clone();
        let subscription = self
            .cx
            .update(|cx| cx.observe(&consumer, move |_, _| output.set(output.get() + 1)));
        (count, subscription)
    }
    fn assert_editor_focused(&mut self, index: usize) {
        self.update(|state, window, cx| {
            assert!(
                state.editors[index]
                    .read(cx)
                    .focus_handle(cx)
                    .is_focused(window)
            );
        });
    }
    fn cursor_ring_bounds(&mut self) -> gpui::Bounds<Pixels> {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                let state = state.read(cx);
                let config = state.scope.as_ref().unwrap().snapshot(cx);
                let g = &config.theme.tokens;
                let width = g.line_width_focus as f32;
                let color = g.color_primary_border.to_hsla();
                let scale = window.scale_factor();
                let inner = state
                    .geometry
                    .cells
                    .values()
                    .find(|cell| cell.in_view && cell.date == Some(state.cursor.date()))
                    .expect("the keyboard cursor has a visible calendar cell")
                    .inner;
                let outlines: Vec<_> = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| {
                        quad.border_color == color
                            && (quad.border_widths.left.0 / scale - width).abs() < 0.01
                    })
                    .collect();
                let outline = outlines
                    .first()
                    .expect("the active calendar cell paints a focus ring");
                // GPUI splits a transparent outline into clipped border strips.
                // Every strip must describe the same single active cell box.
                assert!(outlines.iter().all(|part| part.bounds == outline.bounds));
                let actual = gpui::Bounds::new(
                    point(
                        outline.bounds.origin.x.0 / scale,
                        outline.bounds.origin.y.0 / scale,
                    )
                    .map(px),
                    gpui::size(
                        outline.bounds.size.width.0 / scale,
                        outline.bounds.size.height.0 / scale,
                    )
                    .map(px),
                );
                let expected = inner.dilate(px(width + 1.));
                assert!((actual.left() - expected.left()).abs() < px(0.01));
                assert!((actual.top() - expected.top()).abs() < px(0.01));
                assert!((actual.size.width - expected.size.width).abs() < px(0.01));
                assert!((actual.size.height - expected.size.height).abs() < px(0.01));
                actual
            })
            .unwrap()
    }
    fn select(&mut self, day: u32) {
        self.update(|s, w, c| s.select_date(date(day), PickerChangeSource::Calendar, w, c));
        self.draw();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                w.dispatch_event(event, c);
            })
            .unwrap();
        self.draw();
    }
    fn date_position(&self, panel: usize, date: NaiveDate) -> Point<Pixels> {
        self.state.read_with(&self.cx, |s, _| {
            s.geometry
                .cells
                .values()
                .find(|cell| cell.panel == panel && cell.date == Some(date))
                .unwrap()
                .inner
                .center()
        })
    }
    fn move_to_date(&mut self, panel: usize, date: NaiveDate) {
        let position = self.date_position(panel, date);
        self.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
            position,
            ..Default::default()
        }));
    }
    fn click(&mut self, position: Point<Pixels>) {
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
        let key = Keystroke::parse(key).unwrap();
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: key.clone(),
            is_held: false,
            prefer_character_input: false,
        }));
        self.event(PlatformInput::KeyUp(KeyUpEvent { keystroke: key }));
    }
    fn edit(&mut self, index: usize, text: &str) {
        self.update(|s, w, c| s.focus_input(index, w, c));
        self.draw();
        let editor = self
            .state
            .read_with(&self.cx, |s, _| s.editors[index].clone());
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                editor.update(c, |e, c| {
                    e.select_all(w, c);
                    e.replace_text_in_range(None, text, w, c);
                })
            })
            .unwrap();
        self.draw();
    }
    fn wheel_time(&mut self, column: usize, delta: f32) {
        let position = self
            .state
            .read_with(&self.cx, |s, _| s.geometry.time_columns[column].center());
        self.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
            position,
            ..Default::default()
        }));
        self.event(PlatformInput::Scroll(gpui::ScrollEvent {
            position,
            delta: gpui::ScrollDelta::Pixels(point(px(0.), px(delta))),
            ..Default::default()
        }));
    }
    fn click_header_navigation(&mut self, forward: bool, large: bool) {
        self.click_panel_header_navigation(0, forward, large);
    }
    fn click_panel_header_navigation(&mut self, panel: usize, forward: bool, large: bool) {
        let position = self.state.read_with(&self.cx, |state, cx| {
            let header = state.geometry.headers[panel];
            let config = state.scope.as_ref().unwrap().snapshot(cx);
            let g = &config.theme.tokens;
            let inset =
                px((g.padding_xs + g.control_height_sm * if large { 0.5 } else { 1.5 }) as f32);
            point(
                if forward {
                    header.right() - inset
                } else {
                    header.left() + inset
                },
                header.center().y,
            )
        });
        self.click(position);
    }
}

#[test]
fn date_picker_native_calendar_click_commits_once_and_closes() {
    let mut f = Fixture::<DateSelection>::new(None);
    let bounds = f.state.read_with(&f.cx, |s, _| {
        s.geometry
            .cells
            .values()
            .find(|cell| cell.date == Some(date(21)) && cell.in_view)
            .unwrap()
            .bounds
    });
    f.click(bounds.center());
    assert_eq!(f.value(), Some(date(21)));
    assert!(!f.open());
    assert_eq!(f.changes().len(), 1);
    assert!(f.state.read_with(&f.cx, |s, _| s.geometry.popup.is_none()));
    f.assert_editor_focused(0);
}

#[test]
fn date_picker_native_keyboard_selection_returns_focus_to_an_enabled_editor() {
    let mut single = Fixture::<DateSelection>::new(Some(date(20)));
    single.update(|s, window, cx| s.panel_focus.focus(window, cx));
    single.key("right");
    single.key("enter");
    assert_eq!(single.value(), Some(date(21)));
    assert!(!single.open());
    single.assert_editor_focused(0);

    let mut range = Fixture::<DateRangeSelection>::new(PickerRange::default());
    range.update(|s, window, cx| s.panel_focus.focus(window, cx));
    range.key("right");
    range.key("enter");
    assert!(range.open());
    range.assert_editor_focused(1);
    range.update(|s, window, cx| s.panel_focus.focus(window, cx));
    range.key("right");
    range.key("enter");
    assert_eq!(
        range.value(),
        PickerRange::new(Some(date(21)), Some(date(22)))
    );
    assert!(!range.open());
    range.assert_editor_focused(1);

    range.update(|s, window, cx| s.set_open(true, window, cx));
    range.configure(|v| v.options.disabled_endpoints = [false, true]);
    range.update(|s, window, cx| {
        s.active = 1;
        s.panel_focus.focus(window, cx);
        s.clear(window, cx);
    });
    range.draw();
    assert_eq!(range.value(), PickerRange::new(None, Some(date(22))));
    range.assert_editor_focused(0);
}

#[test]
fn date_picker_native_preset_today_and_ok_restore_focus_while_inline_actions_keep_it() {
    let mut preset = Fixture::<DateSelection>::new(Some(date(20)));
    preset
        .configure(|v| v.presets = vec![PickerPreset::new("tomorrow", "Tomorrow", Some(date(21)))]);
    let sidebar = preset
        .state
        .read_with(&preset.cx, |s, _| s.geometry.presets.unwrap());
    preset.click(point(sidebar.center().x, sidebar.top() + px(24.)));
    assert_eq!(preset.value(), Some(date(21)));
    assert!(!preset.open());
    preset.assert_editor_focused(0);

    let mut today = Fixture::<DateSelection>::new(Some(date(21)));
    let footer = today
        .state
        .read_with(&today.cx, |s, _| s.geometry.footer.unwrap());
    today.click(footer.center());
    assert_eq!(today.value(), Some(date(20)));
    assert!(!today.open());
    today.assert_editor_focused(0);

    let mut time = Fixture::<TimeSelection>::new(None);
    time.update(|s, window, cx| {
        s.select_time(TimeUnit::Hour, 8, PickerChangeSource::Time, window, cx)
    });
    time.draw();
    let ok = time.state.read_with(&time.cx, |s, _| {
        s.geometry
            .footer_actions
            .iter()
            .find(|action| action.key == "ok")
            .unwrap()
            .bounds
    });
    time.click(ok.center());
    assert_eq!(time.value().unwrap().hour(), 8);
    assert!(!time.open());
    time.assert_editor_focused(0);

    let mut inline = Fixture::<DateSelection>::new(Some(date(20)));
    inline.configure(|v| v.inline = true);
    inline.update(|s, window, cx| {
        s.panel_focus.focus(window, cx);
        s.preset(Some(date(21)), window, cx);
        assert!(s.panel_focus.is_focused(window));
        s.now(window, cx);
        assert!(s.panel_focus.is_focused(window));
        s.clear(window, cx);
        assert!(s.panel_focus.is_focused(window));
    });
    assert_eq!(inline.value(), None);
}

#[test]
fn date_picker_controlled_close_restores_focus_on_acceptance_without_stealing_outside_focus() {
    let mut f = Fixture::<DateSelection>::new(Some(date(20)));
    f.configure(|v| v.controlled_open = Some(true));
    f.update(|s, window, cx| s.panel_focus.focus(window, cx));
    let cell = f.state.read_with(&f.cx, |s, _| {
        s.geometry
            .cells
            .values()
            .find(|cell| cell.in_view && cell.date == Some(date(21)))
            .unwrap()
            .bounds
    });
    f.click(cell.center());
    assert!(f.open());
    f.update(|s, window, cx| {
        assert!(s.panel_focus.is_focused(window));
        assert!(!s.editors[0].read(cx).focus_handle(cx).is_focused(window));
    });
    f.configure(|v| v.controlled_open = Some(false));
    assert!(!f.open());
    f.assert_editor_focused(0);

    f.configure(|v| v.controlled_open = Some(true));
    f.update(|s, window, cx| {
        s.panel_focus.focus(window, cx);
        s.clear(window, cx);
        assert!(s.panel_focus.is_focused(window));
    });
    f.draw();
    assert!(f.open());
    f.click(point(px(520.), px(620.)));
    let outside = f
        .consumer
        .read_with(&f.cx, |consumer, _| consumer.outside_focus.clone());
    f.update(|_, window, _| assert!(outside.is_focused(window)));
    f.configure(|v| v.controlled_open = Some(false));
    f.update(|_, window, _| assert!(outside.is_focused(window)));
    f.configure(|v| v.disabled = true);
    f.update(|s, window, cx| {
        s.close_and_restore_focus(window, cx);
        assert!(outside.is_focused(window));
    });
    f.configure(|v| {
        v.disabled = false;
        v.shown = false;
    });
    f.update(|s, window, cx| {
        s.close_and_restore_focus(window, cx);
        assert!(outside.is_focused(window));
    });
}

#[test]
fn date_picker_rtl_time_columns_keep_physical_order_and_column_keyboard_navigation() {
    fn check<S: PickerSelection>(mut f: Fixture<S>) {
        let accepted = f.value();
        let scope = f
            .consumer
            .read_with(&f.cx, |consumer, _| consumer.scope.clone());
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
                .unwrap()
        });
        f.draw();
        f.state.read_with(&f.cx, |s, _| {
            assert_eq!(s.geometry.time_columns.len(), 3);
            assert!(
                s.geometry
                    .time_columns
                    .windows(2)
                    .all(|columns| columns[0].left() < columns[1].left())
            );
        });
        f.update(|s, window, cx| s.column_focus[0].focus(window, cx));
        f.key("right");
        f.update(|s, window, _| assert!(s.column_focus[1].is_focused(window)));
        f.key("right");
        f.update(|s, window, _| assert!(s.column_focus[2].is_focused(window)));
        f.key("left");
        f.update(|s, window, _| assert!(s.column_focus[1].is_focused(window)));
        f.key("down");
        assert_eq!(
            f.state
                .read_with(&f.cx, |s, _| s.draft[0].unwrap().minute()),
            25
        );
        f.key("left");
        f.update(|s, window, _| assert!(s.column_focus[0].is_focused(window)));
        f.key("down");
        assert_eq!(
            f.state.read_with(&f.cx, |s, _| s.draft[0].unwrap().hour()),
            14
        );
        assert!(f.value() == accepted);
        assert!(f.changes().is_empty());
    }
    check(Fixture::<TimeSelection>::new(Some(
        NaiveTime::from_hms_opt(13, 24, 35).unwrap(),
    )));
    check(Fixture::<DateTimeSelection>::new(Some(now())));
}

#[test]
fn date_picker_compact_fractional_dpi_calendar_rows_preserve_the_authored_table_extent() {
    let scale = 1.5;
    // Independent browser values from style-reference.json: seven 33px rows,
    // a 35px week heading, four 57.75px period rows, or one 49px quarter row.
    for (mode, expected_height, heading_height, expected_cells) in [
        (PickerMode::Date, 231., 33., 42),
        (PickerMode::Week, 233., 35., 42),
        (PickerMode::Month, 231., 0., 12),
        (PickerMode::Quarter, 49., 0., 4),
        (PickerMode::Year, 231., 0., 12),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| v.options.mode = mode);
        f.compact_at_scale(scale);
        f.state.read_with(&f.cx, |state, _| {
            let table = state.geometry.tables[0];
            let cells: Vec<_> = state
                .geometry
                .cells
                .values()
                .filter(|cell| cell.panel == 0 && cell.date.is_some())
                .collect();
            assert_eq!(cells.len(), expected_cells);
            assert!(
                (f32::from(table.size.height) - expected_height).abs() * scale <= 1.001,
                "{mode:?}: table height {} != {expected_height}",
                table.size.height
            );
            let first_top = cells.iter().map(|cell| cell.bounds.top()).min().unwrap();
            let last_bottom = cells.iter().map(|cell| cell.bounds.bottom()).max().unwrap();
            assert!(
                (f32::from(first_top - table.top()) - heading_height).abs() * scale <= 1.001,
                "{mode:?}: first real row follows the authored heading"
            );
            assert!(
                f32::from(last_bottom - table.bottom()).abs() * scale <= 1.001,
                "{mode:?}: real cell rows fill the authored table without accumulated snapping gaps"
            );
            if matches!(
                mode,
                PickerMode::Month | PickerMode::Quarter | PickerMode::Year
            ) {
                assert!(
                    cells
                        .iter()
                        .all(
                            |cell| (f32::from(cell.inner.size.width) - 52.5).abs() * scale <= 1.001
                        ),
                    "compact period content uses the browser's 52.5px native width"
                );
            }
        });
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_compact_fractional_dpi_time_columns_share_the_authored_total_width() {
    fn check<S: PickerSelection>(mut f: Fixture<S>, column_count: usize) {
        let scale = 1.5;
        let accepted = f.value();
        f.configure(|v| v.options.time.use_12_hours = column_count == 4);
        f.compact_at_scale(scale);
        f.state.read_with(&f.cx, |state, _| {
            let table = state.geometry.tables[usize::from(S::HAS_DATE)];
            let columns = &state.geometry.time_columns;
            let authored_width = column_count as f32 * 49.;
            let last = column_count - 1;
            assert_eq!(columns.len(), column_count, "hour, minute, second and optional meridiem are real native columns");
            assert!((f32::from(table.size.width) - authored_width).abs() <= 0.34);
            assert!(columns.iter().all(|column| (f32::from(column.size.width) - 49.).abs() * scale <= 1.001));
            assert!(columns.windows(2).all(|pair| f32::from(pair[0].right() - pair[1].left()).abs() * scale <= 1.001));
            let occupied = columns[last].right() - columns[0].left();
            assert!((f32::from(occupied) - authored_width).abs() <= 0.34, "columns preserve the authored total instead of accumulating independent 49px rounding");
            assert!(f32::from(columns[0].left() - table.left()).abs() * scale <= 1.001);
            assert!(f32::from(columns[last].right() - table.right()).abs() * scale <= 1.001);
            assert_eq!(state.geometry.time_viewports.len(), column_count);
            let authored_height = if S::HAS_DATE { 231. } else { 216. };
            for (column, viewport) in columns.iter().zip(&state.geometry.time_viewports) {
                assert!((f32::from(viewport.size.height) - authored_height).abs() <= 0.34);
                assert!(viewport.size.width > px(0.));
                assert!(viewport.left() >= column.left() - px(1. / scale));
                assert!(viewport.right() <= column.right() + px(1. / scale));
                assert!(f32::from(viewport.top() - column.top()).abs() * scale <= 1.001);
                assert!(f32::from(viewport.bottom() - column.bottom()).abs() * scale <= 1.001);
            }
        });
        assert!(f.value() == accepted);
        assert!(f.changes().is_empty());
    }
    for columns in [3, 4] {
        check(
            Fixture::<TimeSelection>::new(Some(NaiveTime::from_hms_opt(13, 24, 35).unwrap())),
            columns,
        );
        check(Fixture::<DateTimeSelection>::new(Some(now())), columns);
    }
}

#[test]
fn date_picker_compact_range_separator_and_active_bar_follow_reference_tokens() {
    for dark in [false, true] {
        for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
            for scale in [1., 1.5, 2.] {
                let value = PickerRange::new(Some(date(8)), Some(date(21)));
                let mut f = Fixture::<DateRangeSelection>::new(value);
                f.compact_case(dark, direction, scale);
                f.state.read_with(&f.cx, |s, _| {
                    let mut inputs = s.geometry.inputs.clone();
                    assert_eq!(inputs.len(), 2);
                    assert!(
                        inputs
                            .iter()
                            .all(|r| (f32::from(r.size.width) - 97.).abs() <= 0.34)
                    );
                    inputs.sort_by(|a, b| a.left().partial_cmp(&b.left()).unwrap());
                    assert!(
                        (f32::from(inputs[1].left() - inputs[0].right()) - 22.).abs() * scale
                            <= 1.001
                    );
                    let bar = s.geometry.active_bar.unwrap();
                    assert!((f32::from(bar.size.width) - 97.).abs() <= 0.34);
                    // This fixture is inset48px; reference captures use32px.
                    // The start bar follows the first input plus root border.
                    let source_input_left = if direction == crate::Direction::Rtl {
                        189.
                    } else {
                        56.
                    };
                    assert!((f32::from(bar.left()) - (source_input_left + 1.)).abs() <= 1.001);
                });
                assert_eq!(f.value(), value);
                assert!(f.changes().is_empty());
            }
        }
    }
}

#[test]
fn date_picker_compact_dual_calendar_cells_keep_fractional_group_width() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        for scale in [1., 1.5, 2.] {
            let mut f =
                Fixture::<DateRangeSelection>::new(PickerRange::new(Some(date(8)), Some(date(21))));
            f.compact_case(true, direction, scale);
            f.state.read_with(&f.cx, |s, _| {
                assert_eq!(s.geometry.panels.len(), 2);
                let left = s.geometry.panels.iter().map(|r| r.left()).min().unwrap();
                let right = s.geometry.panels.iter().map(|r| r.right()).max().unwrap();
                assert!((f32::from(right - left) - 481.).abs() <= 0.34);
                for panel in 0..2 {
                    let mut cells: Vec<_> = s.geometry.cells.values().filter(|cell| cell.panel == panel && cell.date.is_some()).collect();
                    cells.sort_by_key(|cell| cell.date);
                    assert_eq!(cells.len(), 42);
                    let physical_panel = if direction == crate::Direction::Rtl { 1 - panel } else { panel };
                    for (index, cell) in cells.iter().enumerate() {
                        let column = if direction == crate::Direction::Rtl { 6 - index % 7 } else { index % 7 };
                        let expected_left = f32::from(left) + physical_panel as f32 * 240.5 + 10.
                            + (column as f32 + 0.5) * 31.5 - 10.5;
                        assert!((f32::from(cell.inner.left()) - expected_left).abs() <= 1.001,
                            "{direction:?} scale{scale} panel{panel} {} inner{} expected{expected_left}", cell.text, cell.inner.left());
                    }
                }
            });
        }
    }
}

#[test]
fn date_picker_compact_decade_labels_preserve_natural_width_before_centering() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        for scale in [1., 1.5, 2.] {
            let mut f = Fixture::<DateSelection>::new(None);
            f.configure(|v| {
                v.options.mode = PickerMode::Year;
                v.panel_mode = Some(PickerPanelMode::Decade);
            });
            f.compact_case(false, direction, scale);
            let state = f.state.clone();
            let widths =
                f.cx.update_window(f.window.into(), |_, window, cx| {
                    let s = state.read(cx);
                    let config = s.scope.as_ref().unwrap().snapshot(cx);
                    let g = &config.theme.tokens;
                    s.geometry
                        .cells
                        .values()
                        .map(|cell| {
                            let run = gpui::TextRun {
                                len: cell.text.len(),
                                font: config.font.clone(),
                                color: g.color_text.to_hsla(),
                                background_color: None,
                                underline: None,
                                strikethrough: None,
                            };
                            let width = window
                                .text_system()
                                .shape_line(cell.text.clone(), px(g.font_size as f32), &[run], None)
                                .width;
                            (cell.text.clone(), f32::from(width) + g.padding_xs as f32)
                        })
                        .collect::<std::collections::HashMap<_, _>>()
                })
                .unwrap();
            f.state.read_with(&f.cx, |s, _| {
                let left = f32::from(s.geometry.panels[0].left());
                let mut cells: Vec<_> = s
                    .geometry
                    .cells
                    .values()
                    .filter(|cell| cell.date.is_some())
                    .collect();
                cells.sort_by_key(|cell| cell.date);
                assert_eq!(cells.len(), 12);
                for (index, cell) in cells.iter().enumerate() {
                    let column = if direction == crate::Direction::Rtl {
                        2 - index % 3
                    } else {
                        index % 3
                    };
                    let natural_width = widths[&cell.text];
                    assert!(
                        natural_width.fract() > 0.1 && natural_width.fract() < 0.9,
                        "the regression must exercise a fractional native text advance"
                    );
                    let expected_left =
                        left + (column as f32 + 0.5) * 240.5 / 3. - natural_width / 2.;
                    assert!(
                        (f32::from(cell.inner.left()) - expected_left).abs() <= 1.001,
                        "{direction:?} scale{scale} {} inner{} expected{expected_left}",
                        cell.text,
                        cell.inner.left()
                    );
                    assert!(
                        (f32::from(cell.inner.size.width) - natural_width).abs() * scale <= 1.001
                    );
                }
            });
        }
    }
}

#[test]
fn date_picker_today_outline_preserves_custom_content_box_and_disabled_tint() {
    for scale in [1., 1.5] {
        let measured = Rc::new(RefCell::new(std::collections::HashMap::new()));
        let output = measured.clone();
        let mut f = Fixture::<DateSelection>::new(None);
        f.configure(|v| {
            v.cell_render = Some(Rc::new(move |context, content, _, _| {
                let date = context.date;
                let output = output.clone();
                div()
                    .relative()
                    .size_full()
                    .border(px(1.5))
                    .border_color(gpui::rgb(0x123456))
                    .child(content)
                    .child(
                        gpui::canvas(
                            move |bounds, _, _| {
                                if let Some(date) = date {
                                    output.borrow_mut().insert(date, bounds);
                                }
                            },
                            |_, (), _, _| (),
                        )
                        .absolute()
                        .inset_0(),
                    )
                    .into_any_element()
            }));
        });
        f.compact_case(false, crate::Direction::Ltr, scale);
        let cells = measured.borrow();
        assert_eq!(
            cells[&date(20)].size,
            cells[&date(6)].size,
            "Today decoration cannot contract caller-owned content"
        );
        drop(cells);
        for selected in [false, true] {
            let mut disabled = Fixture::<DateSelection>::new(selected.then(|| date(20)));
            disabled.configure(|v| v.disabled_date = Some(Rc::new(|value, _| value == date(20))));
            disabled.compact_case(true, crate::Direction::Ltr, scale);
            let state = disabled.state.clone();
            disabled
                .cx
                .update_window(disabled.window.into(), |_, window, cx| {
                    let s = state.read(cx);
                    let cell = s
                        .geometry
                        .cells
                        .values()
                        .find(|cell| cell.date == Some(date(20)) && cell.in_view)
                        .unwrap();
                    let color = s
                        .scope
                        .as_ref()
                        .unwrap()
                        .snapshot(cx)
                        .theme
                        .tokens
                        .color_text_disabled
                        .to_hsla();
                    let quads: Vec<_> = window
                        .rendered_scene()
                        .quads
                        .iter()
                        .filter(|q| q.border_color == color)
                        .collect();
                    assert!(
                        !quads.is_empty(),
                        "disabled Today keeps the reference gray outline, including selected Today"
                    );
                    assert!(quads.iter().any(|q| {
                        (q.bounds.origin.x.0 / scale - f32::from(cell.inner.left())).abs() < 0.01
                            && (q.bounds.origin.y.0 / scale - f32::from(cell.inner.top())).abs()
                                < 0.01
                            && (q.bounds.size.width.0 / scale - f32::from(cell.inner.size.width))
                                .abs()
                                < 0.01
                    }));
                })
                .unwrap();
        }
    }
}

#[test]
fn date_picker_compact_datetime_ok_uses_natural_label_advance_and_footer_insets() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        for scale in [1., 1.5, 2.] {
            let mut f = Fixture::<DateTimeSelection>::new(Some(now()));
            f.compact_case(true, direction, scale);
            let state = f.state.clone();
            let expected_width =
                f.cx.update_window(f.window.into(), |_, window, cx| {
                    let s = state.read(cx);
                    let config = s.scope.as_ref().unwrap().snapshot(cx);
                    let tokens = config.theme.component::<crate::button::ButtonTheme>();
                    let mut font = tokens.font().clone();
                    font.weight = gpui::FontWeight(tokens.font_weight as f32);
                    let text: SharedString = "OK".into();
                    let run = gpui::TextRun {
                        len: text.len(),
                        font,
                        color: config.theme.tokens.color_text.to_hsla(),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    };
                    let width = window
                        .text_system()
                        .shape_line(text, px(tokens.content_font_size_sm as f32), &[run], None)
                        .width;
                    f32::from(width) + 16.
                })
                .unwrap();
            assert!(
                expected_width.ceil() - expected_width > 0.51,
                "the old logical-pixel ceiling must exceed this regression's geometry budget"
            );
            f.state.read_with(&f.cx, |s, _| {
                let footer = s.geometry.footer.unwrap();
                let button = s
                    .geometry
                    .footer_actions
                    .iter()
                    .find(|a| a.key == "ok")
                    .unwrap()
                    .bounds;
                assert!(
                    (f32::from(button.size.width) - expected_width).abs() <= 0.51,
                    "{direction:?} scale{scale} button{} expected{expected_width}",
                    button.size.width
                );
                let inset = if direction == crate::Direction::Rtl {
                    button.left() - footer.left()
                } else {
                    footer.right() - button.right()
                };
                assert!((f32::from(inset) - 8.).abs() * scale <= 1.001);
            });
            assert_eq!(f.value(), Some(now()));
            assert!(f.changes().is_empty());
        }
    }
}

#[test]
fn date_picker_dual_calendar_omits_a_checked_missing_right_period_at_chrono_boundary() {
    let mut f = Fixture::<DateRangeSelection>::new(PickerRange::default());
    f.configure(|v| v.picker_value = Some(NaiveDate::MAX));
    f.compact_at_scale(1.);
    f.state.read_with(&f.cx, |s, _| {
        assert_eq!(s.geometry.panels.len(), 1);
        assert!((f32::from(s.geometry.popup.unwrap().size.width) - 240.5).abs() <= 0.51);
        assert!(s.geometry.cells.values().all(|cell| cell.panel == 0));
    });
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_native_cell_press_can_be_canceled_before_release() {
    let mut date_picker = Fixture::<DateSelection>::new(None);
    let bounds = date_picker.state.read_with(&date_picker.cx, |s, _| {
        s.geometry
            .cells
            .values()
            .find(|cell| cell.date == Some(date(21)) && cell.in_view)
            .unwrap()
            .bounds
    });
    let outside = point(px(530.), px(630.));
    date_picker.event(PlatformInput::MouseDown(MouseDownEvent {
        position: bounds.center(),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(date_picker.draft(), None);
    assert_eq!(date_picker.value(), None);
    date_picker.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: outside,
        ..Default::default()
    }));
    date_picker.event(PlatformInput::MouseUp(MouseUpEvent {
        position: outside,
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(date_picker.value(), None);
    assert!(date_picker.changes().is_empty());
    date_picker.click(bounds.center());
    assert_eq!(date_picker.value(), Some(date(21)));

    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let mut time_picker = Fixture::<TimeSelection>::new(Some(initial));
    let bounds = time_picker.state.read_with(&time_picker.cx, |s, _| {
        s.geometry
            .cells
            .values()
            .find(|cell| cell.unit == Some(TimeUnit::Hour) && cell.value == Some(7))
            .unwrap()
            .bounds
    });
    time_picker.event(PlatformInput::MouseDown(MouseDownEvent {
        position: bounds.center(),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(time_picker.draft(), Some(initial));
    time_picker.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: outside,
        ..Default::default()
    }));
    time_picker.event(PlatformInput::MouseUp(MouseUpEvent {
        position: outside,
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(time_picker.draft(), Some(initial));
    assert!(time_picker.events.borrow().is_empty());
    time_picker.click(bounds.center());
    assert_eq!(time_picker.draft().unwrap().hour(), 7);
    assert_eq!(time_picker.value(), Some(initial));

    let mut clear_picker = Fixture::<DateSelection>::with_open(Some(date(20)), false);
    let trigger = clear_picker
        .state
        .read_with(&clear_picker.cx, |s, _| s.geometry.trigger.unwrap());
    let clear_inset = clear_picker.state.read_with(&clear_picker.cx, |s, cx| {
        let tokens = s
            .scope
            .as_ref()
            .unwrap()
            .snapshot(cx)
            .theme
            .component::<DatePickerTheme>();
        px((tokens.trigger_metrics(crate::Size::Medium).padding_inline
            + tokens.globals().line_width
            + tokens.globals().font_size / 2.) as f32)
    });
    let clear_position = point(trigger.right() - clear_inset, trigger.center().y);
    clear_picker.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: clear_position,
        ..Default::default()
    }));
    clear_picker.event(PlatformInput::MouseDown(MouseDownEvent {
        position: clear_position,
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(clear_picker.value(), Some(date(20)));
    assert!(clear_picker.changes().is_empty());
    clear_picker.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: outside,
        ..Default::default()
    }));
    clear_picker.event(PlatformInput::MouseUp(MouseUpEvent {
        position: outside,
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(clear_picker.value(), Some(date(20)));
    assert!(clear_picker.changes().is_empty());
    clear_picker.update(|s, window, cx| {
        s.set_open(true, window, cx);
        s.panel_focus.focus(window, cx);
    });
    clear_picker.draw();
    clear_picker.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: clear_position,
        ..Default::default()
    }));
    clear_picker.click(clear_position);
    assert_eq!(clear_picker.value(), None);
    assert_eq!(clear_picker.changes().len(), 1);
    assert_eq!(clear_picker.changes()[0].source, PickerChangeSource::Clear);
    assert!(!clear_picker.open());
    clear_picker.assert_editor_focused(0);
}

#[test]
fn date_picker_native_disabled_date_and_time_cells_reject_selection() {
    let mut date_picker = Fixture::<DateSelection>::new(None);
    date_picker.configure(|v| v.options.min_date = Some(date(15)));
    let bounds = date_picker.state.read_with(&date_picker.cx, |s, _| {
        let cell = s
            .geometry
            .cells
            .values()
            .find(|cell| cell.date == Some(date(10)) && cell.in_view)
            .unwrap();
        assert!(cell.disabled);
        cell.bounds
    });
    date_picker.click(bounds.center());
    assert_eq!(date_picker.value(), None);
    assert_eq!(date_picker.draft(), None);
    assert!(date_picker.changes().is_empty());

    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let mut time_picker = Fixture::<TimeSelection>::new(Some(initial));
    time_picker.configure(|v| {
        v.disabled_time = Some(Rc::new(|_, _| DisabledTime::new().hours(|| vec![7])));
    });
    let bounds = time_picker.state.read_with(&time_picker.cx, |s, _| {
        let cell = s
            .geometry
            .cells
            .values()
            .find(|cell| cell.unit == Some(TimeUnit::Hour) && cell.value == Some(7))
            .unwrap();
        assert!(cell.disabled);
        cell.bounds
    });
    time_picker.click(bounds.center());
    assert_eq!(time_picker.value(), Some(initial));
    assert_eq!(time_picker.draft(), Some(initial));
    assert!(time_picker.events.borrow().is_empty());
}

#[test]
fn date_picker_native_coarse_disabled_cells_share_pointer_and_keyboard_representatives() {
    let make_date = |year, month, day| NaiveDate::from_ymd_opt(year, month, day).unwrap();
    for (mode, panel, representative, end) in [
        (
            PickerMode::Month,
            None,
            make_date(2024, 3, 1),
            make_date(2024, 3, 31),
        ),
        (
            PickerMode::Quarter,
            None,
            make_date(2024, 4, 1),
            make_date(2024, 6, 30),
        ),
        (
            PickerMode::Year,
            None,
            make_date(2025, 1, 1),
            make_date(2025, 12, 31),
        ),
        (
            PickerMode::Year,
            Some(PickerPanelMode::Decade),
            make_date(2030, 1, 1),
            make_date(2039, 12, 31),
        ),
    ] {
        let panel = panel.unwrap_or(match mode {
            PickerMode::Month => PickerPanelMode::Month,
            PickerMode::Quarter => PickerPanelMode::Quarter,
            _ => PickerPanelMode::Year,
        });
        let flags = Rc::new(Cell::new((true, true)));
        let disabled = flags.clone();
        let predicate: super::state::DisabledDate = Rc::new(move |value, info| {
            assert!(
                info.from.is_none(),
                "single pickers never supply a range origin"
            );
            assert_eq!(info.mode, panel);
            (value == representative && disabled.get().0) || (value == end && disabled.get().1)
        });
        let mut keyboard = Fixture::<DateSelection>::new(None);
        keyboard.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = Some(panel);
            v.disabled_date = Some(predicate.clone());
        });
        keyboard.update(|s, w, c| s.panel_focus.focus(w, c));
        keyboard.key("right");
        let before = keyboard
            .state
            .read_with(&keyboard.cx, |s, _| s.panel_date());
        keyboard.key("enter");
        assert_eq!(keyboard.value(), None);
        assert_eq!(
            keyboard
                .state
                .read_with(&keyboard.cx, |s, _| s.panel_date()),
            before
        );
        assert!(keyboard.events.borrow().is_empty());
        // The quarter panel calls disabledDate on its representative directly;
        // the other coarse panels require both endpoints to be disabled.
        flags.set(if mode == PickerMode::Quarter {
            (false, true)
        } else {
            (true, false)
        });
        keyboard.configure(|_| {});
        let enabled = keyboard.state.read_with(&keyboard.cx, |s, _| {
            s.geometry
                .cells
                .values()
                .find(|cell| cell.date == Some(representative))
                .unwrap()
                .disabled
        });
        assert!(
            !enabled,
            "partially available {panel:?} cell must be interactive"
        );
        keyboard.key("enter");
        assert_eq!(
            keyboard
                .state
                .read_with(&keyboard.cx, |s, _| s.cursor.date()),
            representative
        );
        if panel == PickerPanelMode::Decade {
            assert_eq!(keyboard.value(), None);
            assert!(keyboard.events.borrow().iter().any(|event| matches!(
                event, PickerEvent::PanelChange { date, mode: PickerPanelMode::Year } if *date == representative
            )));
        } else {
            assert_eq!(keyboard.value(), Some(representative));
            assert_eq!(keyboard.changes().len(), 1);
        }
        let mut pointer = Fixture::<DateSelection>::new(None);
        pointer.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = Some(panel);
            v.disabled_date = Some(predicate);
        });
        let position = pointer.state.read_with(&pointer.cx, |s, _| {
            s.geometry
                .cells
                .values()
                .find(|cell| cell.date == Some(representative))
                .unwrap()
                .bounds
                .center()
        });
        pointer.click(position);
        assert_eq!(pointer.value(), keyboard.value());
        assert_eq!(
            pointer.state.read_with(&pointer.cx, |s, _| s.cursor.date()),
            representative
        );
        assert_eq!(
            pointer.state.read_with(&pointer.cx, |s, _| s.panel_date()),
            keyboard
                .state
                .read_with(&keyboard.cx, |s, _| s.panel_date())
        );
    }
}

#[test]
fn date_picker_native_coarse_disabled_callbacks_keep_range_origin_and_panel_context() {
    let from = date(7);
    for (mode, panel) in [
        (PickerMode::Month, PickerPanelMode::Month),
        (PickerMode::Quarter, PickerPanelMode::Quarter),
        (PickerMode::Year, PickerPanelMode::Year),
        (PickerMode::Year, PickerPanelMode::Decade),
    ] {
        let mut f = Fixture::<DateRangeSelection>::new(PickerRange::new(Some(from), None));
        let calls = Rc::new(RefCell::new(Vec::new()));
        let output = calls.clone();
        f.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = Some(panel);
            v.disabled_date = Some(Rc::new(move |value, info| {
                assert_eq!(info.from, Some(from));
                assert_eq!(info.mode, panel);
                output.borrow_mut().push(value);
                true
            }));
        });
        f.update(|s, w, c| {
            s.focus_input(1, w, c);
            s.panel_focus.focus(w, c);
        });
        f.key("right");
        let cell = f.state.read_with(&f.cx, |s, _| {
            super::calendar::cell_for_date(s.cursor.date(), panel, s.format_context.week_rules)
        });
        calls.borrow_mut().clear();
        // Dispatch to the actual panel handler; a disabled coarse cell must
        // preserve both range endpoints and the selection origin.
        f.key("enter");
        let calls = calls.borrow();
        assert!(!calls.is_empty());
        assert_eq!(calls[0], cell.period_start);
        if panel != PickerPanelMode::Quarter {
            assert_eq!(calls[1], cell.period_end);
        }
        assert_eq!(f.draft(), PickerRange::new(Some(from), None));
        assert_eq!(f.value(), PickerRange::new(Some(from), None));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_native_decade_keyboard_bounds_keep_partially_available_cells_reachable() {
    let make_date = |year, month, day| NaiveDate::from_ymd_opt(year, month, day).unwrap();
    for (initial, min, max, key) in [
        (
            make_date(2024, 2, 20),
            Some(make_date(2038, 1, 1)),
            None,
            "right",
        ),
        (
            make_date(2044, 2, 20),
            None,
            Some(make_date(2031, 12, 31)),
            "left",
        ),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(initial));
        f.configure(|v| {
            v.options.mode = PickerMode::Year;
            v.panel_mode = Some(PickerPanelMode::Decade);
            v.options.min_date = min;
            v.options.max_date = max;
        });
        let representative = make_date(2030, 1, 1);
        f.state.read_with(&f.cx, |s, _| {
            let cell = s
                .geometry
                .cells
                .values()
                .find(|cell| cell.date == Some(representative))
                .unwrap();
            assert!(!cell.disabled, "the decade intersects the authored bounds");
        });
        f.update(|s, w, c| s.panel_focus.focus(w, c));
        f.key(key);
        assert_eq!(
            f.state.read_with(&f.cx, |s, _| s.cursor.date()),
            make_date(2034, 2, 20)
        );
        f.key("enter");
        assert_eq!(
            f.state.read_with(&f.cx, |s, _| s.cursor.date()),
            representative
        );
        assert!(f.events.borrow().iter().any(|event| matches!(
            event, PickerEvent::PanelChange { date, mode: PickerPanelMode::Year } if *date == representative
        )));
        assert_eq!(f.value(), Some(initial));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_time_draft_requires_confirmation_and_uses_fixed_anchor() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.update(|s, w, c| {
        assert_eq!(s.cursor.date(), TIME_ANCHOR);
        s.select_time(TimeUnit::Hour, 8, PickerChangeSource::Time, w, c);
        s.select_time(TimeUnit::Minute, 15, PickerChangeSource::Time, w, c);
    });
    f.draw();
    assert_eq!(f.value(), None);
    assert_eq!(f.draft().unwrap().hour(), 8);
    assert!(f.changes().is_empty());
    f.update(|s, w, c| assert!(s.submit(PickerChangeSource::Keyboard, w, c)));
    f.draw();
    assert_eq!(f.value().unwrap().minute(), 15);
    assert_eq!(f.changes().len(), 1);
    assert!(!f.open());
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|e| matches!(e, PickerEvent::Ok(_)))
            .count(),
        1
    );
}

#[test]
fn date_picker_empty_time_cannot_confirm_an_absent_value() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.update(|s, w, c| {
        assert!(!s.can_commit());
        assert!(!s.submit(PickerChangeSource::Keyboard, w, c));
    });
    f.draw();
    assert!(f.open());
    assert_eq!(f.value(), None);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_empty_time_explicit_ok_accepts_valid_default_open_time_only() {
    let template = NaiveTime::from_hms_opt(12, 8, 30).unwrap();
    let mut f = Fixture::<TimeSelection>::with_open(None, false);
    f.configure(|v| {
        v.options.time.default_open_value = Some(template);
        v.options.time.minute_step = 15;
    });
    f.update(|s, w, c| s.set_open(true, w, c));
    f.draw();
    f.update(|s, _, _| {
        assert_eq!(s.cursor.time(), template);
        assert!(!s.can_commit());
        assert!(s.can_submit());
    });
    assert_eq!(f.value(), None);
    let footer = f.state.read_with(&f.cx, |s, _| s.geometry.footer.unwrap());
    f.click(point(footer.right() - px(20.), footer.center().y));
    assert_eq!(f.value(), Some(template));
    assert_eq!(f.changes().len(), 1);
    let mut datetime = Fixture::<DateTimeSelection>::new(None);
    datetime.configure(|v| v.options.time.default_open_value = Some(template));
    datetime.update(|s, w, c| {
        assert!(!s.can_submit());
        assert!(!s.submit(PickerChangeSource::Keyboard, w, c));
    });
    assert_eq!(datetime.value(), None);
}

#[test]
fn date_picker_range_orders_final_values_and_keeps_partial_draft_private() {
    let mut f = Fixture::<DateRangeSelection>::new(PickerRange::default());
    f.select(25);
    assert_eq!(f.value(), PickerRange::default());
    assert_eq!(f.draft().start, Some(date(25)));
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.active_endpoint()),
        PickerEndpoint::End
    );
    f.select(10);
    assert_eq!(f.value(), (date(10), date(25)).into());
    assert_eq!(f.changes().len(), 1);
    assert!(!f.open());
}

#[test]
fn date_picker_datetime_range_ok_confirms_each_endpoint_through_native_buttons() {
    let mut f = Fixture::<DateTimeRangeSelection>::new(PickerRange::default());
    f.select(20);
    assert!(f.state.read_with(&f.cx, |s, _| s.can_submit()));
    let footer = f.state.read_with(&f.cx, |s, _| s.geometry.footer.unwrap());
    f.click(point(footer.right() - px(20.), footer.center().y));
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.active_endpoint()),
        PickerEndpoint::End
    );
    assert_eq!(f.value(), PickerRange::default());
    assert!(f.open());
    f.select(23);
    let footer = f.state.read_with(&f.cx, |s, _| s.geometry.footer.unwrap());
    f.click(point(footer.right() - px(20.), footer.center().y));
    assert_eq!(f.value().start.unwrap().date(), date(20));
    assert_eq!(f.value().end.unwrap().date(), date(23));
    assert!(!f.open());
    assert_eq!(f.changes().len(), 1);
}

#[test]
fn date_picker_range_allow_empty_and_disabled_endpoint_clear() {
    let mut f = Fixture::<DateRangeSelection>::new((date(10), date(25)).into());
    f.configure(|v| v.options.disabled_endpoints = [true, false]);
    f.update(|s, w, c| s.clear(w, c));
    f.draw();
    assert_eq!(f.value(), PickerRange::new(Some(date(10)), None));
    let mut open = Fixture::<DateRangeSelection>::new(PickerRange::default());
    open.configure(|v| v.options.allow_empty = [false, true]);
    open.select(12);
    assert_eq!(open.value(), PickerRange::new(Some(date(12)), None));
    assert!(!open.open());
}

#[test]
fn date_picker_multiple_toggles_many_values_without_indexing_endpoint_flags() {
    let mut f = Fixture::<MultipleDateSelection>::new(Arc::from([]));
    for day in [25, 10, 20, 12] {
        f.select(day);
    }
    assert_eq!(&*f.value(), &[date(10), date(12), date(20), date(25)]);
    assert!(f.open());
    f.select(20);
    assert_eq!(&*f.value(), &[date(10), date(12), date(25)]);
    assert_eq!(f.changes().len(), 5);
}

#[test]
fn date_picker_tag_removal_uses_value_identity_and_keeps_popup_open() {
    let mut f = Fixture::<MultipleDateSelection>::new(Arc::from([date(10), date(20), date(25)]));
    f.configure(|v| v.options.need_confirm = Some(true));
    f.update(|s, w, c| s.remove_date(date(20), w, c));
    f.draw();
    assert_eq!(&*f.value(), &[date(10), date(25)]);
    assert!(f.open());
    assert_eq!(f.changes()[0].source, PickerChangeSource::Tag);
    f.update(|s, w, c| s.remove_date(date(20), w, c));
    f.draw();
    assert_eq!(f.changes().len(), 1);
}

#[test]
fn date_picker_multiple_combobox_opens_from_native_tab_enter_and_space() {
    let mut f = Fixture::<MultipleDateSelection>::with_open(Arc::from([date(10)]), false);
    f.key("tab");
    f.update(|s, w, c| assert!(s.focus_handle(c).is_focused(w)));
    f.key("enter");
    assert!(f.open());
    f.key("escape");
    assert!(!f.open());
    f.key("space");
    assert!(f.open());
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_controlled_rejection_restores_draft_and_acceptance_is_reentrant() {
    let initial = Some(NaiveTime::from_hms_opt(9, 10, 0).unwrap());
    let mut f = Fixture::<TimeSelection>::new(initial);
    f.configure(|v| {
        v.controlled = Some(initial);
        v.options.need_confirm = Some(false);
    });
    f.update(|s, w, c| s.select_time(TimeUnit::Hour, 11, PickerChangeSource::Time, w, c));
    f.draw();
    assert_eq!(f.value(), initial);
    assert_eq!(f.draft(), initial);
    assert_eq!(f.changes().len(), 1);
    f.configure(|v| {
        v.accept = true;
        v.reentrant = true;
    });
    f.update(|s, w, c| s.select_time(TimeUnit::Hour, 12, PickerChangeSource::Time, w, c));
    f.draw();
    assert_eq!(f.value().unwrap().hour(), 12);
    assert_eq!(f.draft(), f.value());
    assert_eq!(f.changes().len(), 2);
}

#[test]
fn date_picker_controlled_open_rejection_does_not_discard_current_draft() {
    let mut f = Fixture::<DateTimeSelection>::new(None);
    f.configure(|v| v.controlled_open = Some(true));
    f.select(22);
    let draft = f.draft();
    f.update(|s, w, c| s.request_open(false, true, w, c));
    f.draw();
    assert!(f.open());
    assert_eq!(f.draft(), draft);
    f.configure(|v| v.controlled_open = Some(false));
    assert!(!f.open());
    assert_eq!(f.draft(), None);
}

#[test]
fn date_picker_native_text_enter_commits_once_and_invalid_text_reverts() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.edit(0, "2024-02-29");
    assert_eq!(f.draft(), Some(date(29)));
    assert!(f.changes().is_empty());
    f.key("enter");
    assert_eq!(f.value(), Some(date(29)));
    assert_eq!(f.changes().len(), 1);
    assert!(!f.open());
    f.update(|s, w, c| s.set_open(true, w, c));
    f.draw();
    f.edit(0, "2023-02-29");
    f.key("escape");
    assert_eq!(f.value(), Some(date(29)));
    assert_eq!(f.text()[0].as_ref(), "2024-02-29");
    assert_eq!(f.changes().len(), 1);
}

#[test]
fn date_picker_closed_popup_typing_submits_without_losing_the_edit() {
    let mut f = Fixture::<DateSelection>::with_open(None, false);
    f.edit(0, "2024-02-29");
    assert!(!f.open());
    f.key("enter");
    assert_eq!(f.value(), Some(date(29)));
    assert_eq!(f.changes().len(), 1);
    f.edit(0, "");
    f.key("enter");
    assert_eq!(f.value(), None);
    assert_eq!(f.changes().len(), 2);
    let mut range = Fixture::<DateRangeSelection>::with_open(PickerRange::default(), false);
    range.edit(0, "2024-02-10");
    range.key("enter");
    assert_eq!(range.draft().start, Some(date(10)));
    assert!(range.open());
    range.edit(1, "2024-02-20");
    range.key("enter");
    assert_eq!(range.value(), (date(10), date(20)).into());
}

#[test]
fn date_picker_nonconfirm_external_close_flushes_a_valid_user_draft() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.edit(0, "2024-02-29");
    f.update(|s, w, c| s.set_open(false, w, c));
    f.draw();
    assert_eq!(f.value(), Some(date(29)));
    assert!(!f.open());
    assert_eq!(f.changes().len(), 1);
    let mut confirmation = Fixture::<DateTimeSelection>::new(None);
    confirmation.edit(0, "2024-02-29 12:08:00");
    confirmation.update(|s, w, c| s.set_open(false, w, c));
    confirmation.draw();
    assert_eq!(confirmation.value(), None);
    assert!(confirmation.changes().is_empty());
}

#[test]
fn date_picker_ime_preedit_does_not_become_a_calendar_value() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.update(|s, w, c| s.focus_input(0, w, c));
    f.draw();
    let editor = f.state.read_with(&f.cx, |s, _| s.editors[0].clone());
    f.cx.update_window(f.window.into(), |_, w, c| {
        editor.update(c, |e, c| {
            e.replace_and_mark_text_in_range(None, "2024-02-29", Some(10..10), w, c)
        })
    })
    .unwrap();
    f.draw();
    assert_eq!(f.draft(), None);
    assert!(f.changes().is_empty());
    f.cx.update_window(f.window.into(), |_, w, c| {
        editor.update(c, |e, c| e.replace_text_in_range(None, "2024-02-29", w, c))
    })
    .unwrap();
    f.draw();
    assert_eq!(f.draft(), Some(date(29)));
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_preserve_invalid_on_blur_keeps_text_without_accepting_it() {
    let mut f = Fixture::<DateSelection>::new(Some(date(10)));
    f.configure(|v| v.options.preserve_invalid_on_blur = true);
    f.edit(0, "2024-02-31");
    f.update(|s, w, c| s.request_open(false, false, w, c));
    f.draw();
    assert_eq!(f.value(), Some(date(10)));
    assert_eq!(f.text()[0].as_ref(), "2024-02-31");
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_hover_preview_never_commits_and_restores_text() {
    let mut f = Fixture::<DateSelection>::new(Some(date(10)));
    f.update(|s, w, c| s.hover_date(Some(date(20)), w, c));
    f.draw();
    assert_eq!(f.text()[0].as_ref(), "2024-02-20");
    assert_eq!(f.value(), Some(date(10)));
    f.update(|s, w, c| s.hover_date(None, w, c));
    f.draw();
    assert_eq!(f.text()[0].as_ref(), "2024-02-10");
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_panel_cursor_uses_calendar_months_and_leap_years() {
    let mut f = Fixture::<DateSelection>::new(Some(date(29)));
    f.update(|s, w, c| {
        s.change_panel(PickerPanelMode::Year, w, c);
        assert!(s.panel_key("right", false, w, c));
        assert_eq!(
            s.cursor.date(),
            NaiveDate::from_ymd_opt(2025, 2, 28).unwrap()
        );
        s.change_panel(PickerPanelMode::Month, w, c);
        s.cursor = NaiveDate::from_ymd_opt(2024, 1, 31)
            .unwrap()
            .and_time(NaiveTime::MIN);
        s.panel_key("right", false, w, c);
        assert_eq!(s.cursor.date(), date(29));
    });
    assert_eq!(f.value(), Some(date(29)));
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_navigation_and_panel_drilling_do_not_emit_selection() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.configure(|v| {
        v.options.min_date = Some(date(15));
        v.options.max_date = Some(NaiveDate::from_ymd_opt(2024, 3, 10).unwrap());
    });
    f.update(|s, w, c| {
        s.navigate(-1, false, w, c);
        assert_eq!(s.view_date.month(), 2);
        assert!(!s.can_navigate(-1, false));
        s.change_panel(PickerPanelMode::Year, w, c);
        s.select_date(
            NaiveDate::from_ymd_opt(2024, 1, 1).unwrap(),
            PickerChangeSource::Calendar,
            w,
            c,
        );
        assert_eq!(s.panel_mode, PickerPanelMode::Month);
        s.change_panel(PickerPanelMode::Decade, w, c);
        s.select_date(
            NaiveDate::from_ymd_opt(2020, 1, 1).unwrap(),
            PickerChangeSource::Calendar,
            w,
            c,
        );
        assert_eq!(s.panel_mode, PickerPanelMode::Year);
    });
    assert!(f.changes().is_empty());
    assert_eq!(f.value(), None);
}

#[test]
fn date_picker_native_header_navigation_uses_each_panels_reference_interval() {
    for mode in [PickerMode::Date, PickerMode::Week] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| v.options.mode = mode);
        for (forward, large, expected) in [
            (true, false, NaiveDate::from_ymd_opt(2024, 3, 20).unwrap()),
            (false, false, date(20)),
            (true, true, NaiveDate::from_ymd_opt(2025, 2, 20).unwrap()),
            (false, true, date(20)),
        ] {
            f.click_header_navigation(forward, large);
            assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), expected);
        }
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
    for (mode, panel, years) in [
        (PickerMode::Month, None, 1),
        (PickerMode::Quarter, None, 1),
        (PickerMode::Year, None, 10),
        (PickerMode::Year, Some(PickerPanelMode::Decade), 100),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = panel;
        });
        let expected = date(20).with_year(2024 + years).unwrap();
        f.click_header_navigation(true, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), expected);
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == expected)
        );
        f.click_header_navigation(false, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_native_controlled_header_navigation_proposes_the_same_intervals() {
    for (mode, panel, years) in [
        (PickerMode::Date, None, 1),
        (PickerMode::Month, None, 1),
        (PickerMode::Quarter, None, 1),
        (PickerMode::Year, None, 10),
        (PickerMode::Year, Some(PickerPanelMode::Decade), 100),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = panel;
            v.picker_value = Some(date(20));
        });
        let proposed = date(20).with_year(2024 + years).unwrap();
        f.click_header_navigation(true, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == proposed)
        );
        f.configure(|v| v.picker_value = Some(proposed));
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposed);
        f.click_header_navigation(false, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposed);
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date: proposed, .. }) if *proposed == date(20))
        );
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_native_month_and_quarter_year_buttons_respect_date_bounds() {
    for mode in [PickerMode::Month, PickerMode::Quarter] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| {
            v.options.mode = mode;
            v.options.min_date = Some(NaiveDate::from_ymd_opt(2024, 1, 1).unwrap());
            v.options.max_date = Some(NaiveDate::from_ymd_opt(2025, 12, 31).unwrap());
        });
        assert!(f.state.read_with(&f.cx, |s, _| s.can_navigate(1, true)));
        assert!(!f.state.read_with(&f.cx, |s, _| s.can_navigate(-1, true)));
        f.click_header_navigation(false, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert!(f.events.borrow().is_empty());
        f.click_header_navigation(true, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date().year()), 2025);
        let count = f.events.borrow().len();
        assert!(!f.state.read_with(&f.cx, |s, _| s.can_navigate(1, true)));
        f.click_header_navigation(true, true);
        assert_eq!(f.events.borrow().len(), count);
        f.click_header_navigation(false, true);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_header_navigation_keeps_calendar_keys_in_the_visible_period() {
    for (mode, panel, large, visible, after_key) in [
        (
            PickerMode::Date,
            None,
            false,
            NaiveDate::from_ymd_opt(2024, 3, 20).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 21).unwrap(),
        ),
        (
            PickerMode::Month,
            None,
            true,
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 3, 20).unwrap(),
        ),
        (
            PickerMode::Quarter,
            None,
            true,
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 5, 20).unwrap(),
        ),
        (
            PickerMode::Year,
            None,
            true,
            NaiveDate::from_ymd_opt(2034, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2035, 2, 20).unwrap(),
        ),
        (
            PickerMode::Year,
            Some(PickerPanelMode::Decade),
            true,
            NaiveDate::from_ymd_opt(2124, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2134, 2, 20).unwrap(),
        ),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(date(20)));
        f.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = panel;
        });
        f.click_header_navigation(true, large);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), visible);
        f.update(|s, window, cx| s.panel_focus.focus(window, cx));
        f.key("right");
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), after_key);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), visible);
        assert_eq!(f.value(), Some(date(20)));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_controlled_keyboard_crossing_waits_for_visible_period_acceptance() {
    for (mode, panel, initial, proposal, after_key) in [
        (
            PickerMode::Date,
            None,
            date(29),
            NaiveDate::from_ymd_opt(2024, 3, 1).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 2).unwrap(),
        ),
        (
            PickerMode::Month,
            None,
            NaiveDate::from_ymd_opt(2024, 12, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 1, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
        ),
        (
            PickerMode::Quarter,
            None,
            NaiveDate::from_ymd_opt(2024, 11, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 5, 20).unwrap(),
        ),
        (
            PickerMode::Year,
            None,
            NaiveDate::from_ymd_opt(2029, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2030, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2031, 2, 20).unwrap(),
        ),
        (
            PickerMode::Year,
            Some(PickerPanelMode::Decade),
            NaiveDate::from_ymd_opt(2094, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2104, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2114, 2, 20).unwrap(),
        ),
    ] {
        let mut f = Fixture::<DateSelection>::new(Some(initial));
        f.configure(|v| {
            v.options.mode = mode;
            v.panel_mode = panel;
            v.picker_value = Some(date(20));
        });
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), initial);
        f.update(|s, window, cx| s.panel_focus.focus(window, cx));
        f.key("right");
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), initial);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == proposal)
        );
        let count = f.events.borrow().len();
        f.key("right");
        assert_eq!(f.events.borrow().len(), count + 1);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), initial);
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == proposal)
        );
        f.configure(|v| v.picker_value = Some(proposal));
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposal);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), proposal);
        let count = f.events.borrow().len();
        f.key("right");
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), after_key);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposal);
        assert_eq!(f.events.borrow().len(), count);
        assert_eq!(f.value(), Some(initial));
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_uncontrolled_keyboard_emits_only_for_visible_period_changes() {
    let mut f = Fixture::<DateSelection>::new(Some(date(28)));
    f.update(|s, window, cx| s.panel_focus.focus(window, cx));
    f.key("right");
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), date(29));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(28));
    assert!(f.events.borrow().is_empty());
    f.key("right");
    let march = NaiveDate::from_ymd_opt(2024, 3, 1).unwrap();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), march);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), march);
    assert!(
        matches!(f.events.borrow().as_slice(), [PickerEvent::PanelChange { date, mode:PickerPanelMode::Date }] if *date == march)
    );
    f.key("right");
    assert_eq!(f.events.borrow().len(), 1);
    f.key("left");
    assert_eq!(f.events.borrow().len(), 1);
    f.key("left");
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), date(29));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(29));
    assert_eq!(f.events.borrow().len(), 2);
    assert!(
        matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date: proposed, .. }) if *proposed == date(29))
    );
    assert_eq!(f.value(), Some(date(28)));
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_native_keyboard_focus_ring_moves_with_one_visible_cursor_cell() {
    let mut f = Fixture::<DateSelection>::new(Some(date(20)));
    f.click_header_navigation(true, false);
    f.update(|s, window, cx| s.panel_focus.focus(window, cx));
    f.key("right");
    let first = f.cursor_ring_bounds();
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.cursor.date()),
        NaiveDate::from_ymd_opt(2024, 3, 21).unwrap()
    );
    f.key("right");
    let next = f.cursor_ring_bounds();
    assert!(next.left() > first.left());
    assert_eq!(first.size, next.size);
    assert_eq!(f.value(), Some(date(20)));
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_range_keyboard_enters_the_already_visible_second_grid_without_scrolling() {
    for controlled in [false, true] {
        let initial = PickerRange::new(Some(date(29)), None);
        let mut f = Fixture::<DateRangeSelection>::new(initial);
        if controlled {
            f.configure(|v| v.picker_value = Some(date(20)));
        }
        let anchor = f.state.read_with(&f.cx, |s, _| s.panel_date());
        f.update(|s, window, cx| s.panel_focus.focus(window, cx));
        f.key("right");
        let march = NaiveDate::from_ymd_opt(2024, 3, 1).unwrap();
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), march);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), anchor);
        assert!(f.events.borrow().is_empty());
        f.cursor_ring_bounds();
        f.key("right");
        assert_eq!(
            f.state.read_with(&f.cx, |s, _| s.cursor.date()),
            NaiveDate::from_ymd_opt(2024, 3, 2).unwrap()
        );
        assert!(f.events.borrow().is_empty());
        assert_eq!(f.value(), initial);
        f.key("pagedown");
        let proposal = NaiveDate::from_ymd_opt(2024, 4, 2).unwrap();
        assert!(
            matches!(f.events.borrow().as_slice(), [PickerEvent::PanelChange { date, .. }] if *date == proposal)
        );
        if controlled {
            assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), anchor);
            assert_eq!(
                f.state.read_with(&f.cx, |s, _| s.cursor.date()),
                NaiveDate::from_ymd_opt(2024, 3, 2).unwrap()
            );
        } else {
            assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposal);
            assert_eq!(f.state.read_with(&f.cx, |s, _| s.cursor.date()), proposal);
        }
        assert_eq!(f.value(), initial);
    }
}

#[test]
fn date_picker_native_dual_headers_validate_the_period_they_display() {
    for (mode, maximum, extended, right, expected, minimum) in [
        (
            PickerMode::Date,
            NaiveDate::from_ymd_opt(2024, 3, 31).unwrap(),
            NaiveDate::from_ymd_opt(2024, 4, 30).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 20).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 20).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 1).unwrap(),
        ),
        (
            PickerMode::Week,
            NaiveDate::from_ymd_opt(2024, 3, 31).unwrap(),
            NaiveDate::from_ymd_opt(2024, 4, 30).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 20).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 20).unwrap(),
            NaiveDate::from_ymd_opt(2024, 3, 1).unwrap(),
        ),
        (
            PickerMode::Month,
            NaiveDate::from_ymd_opt(2025, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2026, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 1, 1).unwrap(),
        ),
        (
            PickerMode::Quarter,
            NaiveDate::from_ymd_opt(2025, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2026, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2025, 1, 1).unwrap(),
        ),
        (
            PickerMode::Year,
            NaiveDate::from_ymd_opt(2039, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2049, 12, 31).unwrap(),
            NaiveDate::from_ymd_opt(2034, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2034, 2, 20).unwrap(),
            NaiveDate::from_ymd_opt(2030, 1, 1).unwrap(),
        ),
    ] {
        let initial = PickerRange::new(Some(date(20)), Some(date(21)));
        let mut f = Fixture::<DateRangeSelection>::new(initial);
        f.configure(|v| {
            v.options.mode = mode;
            v.options.max_date = Some(maximum);
        });
        let large = !matches!(mode, PickerMode::Date | PickerMode::Week);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.geometry.headers.len()), 2);
        assert!(f.state.read_with(&f.cx, |s, _| s.can_navigate(1, large)));
        assert!(
            !f.state
                .read_with(&f.cx, |s, _| s.can_navigate_from(right, 1, large))
        );
        f.click_panel_header_navigation(1, true, large);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), date(20));
        assert!(f.events.borrow().is_empty());
        f.configure(|v| v.options.max_date = Some(extended));
        f.click_panel_header_navigation(1, true, large);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), expected);
        assert!(
            matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == expected)
        );
        f.configure(|v| v.options.min_date = Some(minimum));
        let count = f.events.borrow().len();
        f.click_panel_header_navigation(0, false, large);
        assert_eq!(f.events.borrow().len(), count);
        assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), expected);
        assert_eq!(f.value(), initial);
        assert!(f.changes().is_empty());
    }
}

#[test]
fn date_picker_controlled_right_header_normalizes_after_month_end_clamping() {
    let january = NaiveDate::from_ymd_opt(2024, 1, 31).unwrap();
    let leap_february = NaiveDate::from_ymd_opt(2024, 2, 29).unwrap();
    let initial = PickerRange::new(Some(january), Some(leap_february));
    let mut f = Fixture::<DateRangeSelection>::new(initial);
    f.configure(|v| v.picker_value = Some(january));
    f.click_panel_header_navigation(1, true, true);
    let proposal = NaiveDate::from_ymd_opt(2025, 1, 28).unwrap();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), january);
    assert!(
        matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == proposal)
    );
    f.configure(|v| v.picker_value = Some(proposal));
    f.click_panel_header_navigation(1, true, false);
    let february = NaiveDate::from_ymd_opt(2025, 2, 28).unwrap();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.panel_date()), proposal);
    assert!(
        matches!(f.events.borrow().last(), Some(PickerEvent::PanelChange { date, .. }) if *date == february)
    );
    assert_eq!(f.value(), initial);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_native_header_keyboard_switch_retains_panel_focus() {
    let mut f = Fixture::<DateSelection>::new(Some(date(20)));
    f.update(|s, w, c| s.panel_focus.focus(w, c));
    f.draw();
    for _ in 0..3 {
        f.key("tab");
    }
    f.key("enter");
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.panel_mode()),
        PickerPanelMode::Month
    );
    f.update(|s, w, _| assert!(s.panel_focus.is_focused(w)));
    assert!(f.open());
    f.key("enter");
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.panel_mode()),
        PickerPanelMode::Date
    );
    f.update(|s, w, _| assert!(s.panel_focus.is_focused(w)));
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_geometry_frame_clear_preserves_warm_capacity_and_removes_old_surfaces() {
    let f = Fixture::<DateTimeRangeSelection>::new(PickerRange::default());
    let mut geometry = f.state.read_with(&f.cx, |s, _| s.geometry().clone());
    assert!(!geometry.cells.is_empty());
    assert!(!geometry.inputs.is_empty());
    let capacities = [
        geometry.inputs.capacity(),
        geometry.panels.capacity(),
        geometry.headers.capacity(),
        geometry.tables.capacity(),
        geometry.cells.capacity(),
        geometry.time_columns.capacity(),
        geometry.time_viewports.capacity(),
        geometry.scrollbars.capacity(),
        geometry.footer_actions.capacity(),
        geometry.tags.capacity(),
    ];
    geometry.clear_frame();
    assert!(
        geometry.trigger.is_none()
            && geometry.popup.is_none()
            && geometry.footer.is_none()
            && geometry.active_bar.is_none()
            && geometry.presets.is_none()
    );
    assert!(
        geometry.inputs.is_empty()
            && geometry.panels.is_empty()
            && geometry.headers.is_empty()
            && geometry.tables.is_empty()
            && geometry.cells.is_empty()
            && geometry.time_columns.is_empty()
            && geometry.time_viewports.is_empty()
            && geometry.scrollbars.is_empty()
            && geometry.footer_actions.is_empty()
            && geometry.tags.is_empty()
    );
    assert_eq!(
        capacities,
        [
            geometry.inputs.capacity(),
            geometry.panels.capacity(),
            geometry.headers.capacity(),
            geometry.tables.capacity(),
            geometry.cells.capacity(),
            geometry.time_columns.capacity(),
            geometry.time_viewports.capacity(),
            geometry.scrollbars.capacity(),
            geometry.footer_actions.capacity(),
            geometry.tags.capacity()
        ]
    );
}

#[test]
fn date_picker_now_aligns_popup_steps_while_typed_input_remains_exact() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.configure(|v| v.options.time.minute_step = 15);
    f.update(|s, w, c| s.now(w, c));
    f.draw();
    assert_eq!(f.value().unwrap().minute(), 15);
    f.update(|s, w, c| s.set_open(true, w, c));
    f.draw();
    f.edit(0, "12:24:35");
    f.update(|s, w, c| assert!(s.submit(PickerChangeSource::Input, w, c)));
    f.draw();
    assert_eq!(f.value().unwrap().minute(), 24);
}

#[test]
fn date_picker_disabled_constraints_prevent_now_and_invalid_typed_commits() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.update(|s, w, c| {
        s.disabled_time = Some(Rc::new(|_, _| {
            DisabledTime::new().hours(|| (0..24).collect())
        }));
        s.now(w, c);
        assert_eq!(s.value(), &None);
    });
    f.configure(|v| v.disabled = true);
    f.update(|s, w, c| s.clear(w, c));
    f.draw();
    assert_eq!(f.value(), None);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_mask_native_digits_and_blur_repair_share_the_editor() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.configure(|v| v.format = PickerFormat::mask("%Y-%m-%d").unwrap());
    f.update(|s, w, c| s.focus_input(0, w, c));
    f.draw();
    for digit in ["2", "0", "2", "4", "0", "2", "3", "1"] {
        f.update(|s, w, c| assert!(s.mask_key(0, digit, w, c)));
    }
    f.draw();
    assert_eq!(f.text()[0].as_ref(), "2024-02-31");
    assert!(f.changes().is_empty());
    f.update(|_, w, c| w.blur(c));
    f.draw();
    assert_eq!(f.value(), Some(date(29)));
}

#[test]
fn date_picker_mask_native_key_dispatch_advances_segments_exactly_once() {
    let mut f = Fixture::<DateSelection>::new(None);
    f.configure(|v| v.format = PickerFormat::mask("%Y-%m-%d").unwrap());
    f.update(|s, w, c| s.focus_input(0, w, c));
    f.draw();
    for key in ["2", "0", "2", "4", "0", "2", "2", "9"] {
        f.key(key);
    }
    assert_eq!(f.text()[0].as_ref(), "2024-02-29");
    f.key("left");
    f.key("up");
    assert_eq!(f.text()[0].as_ref(), "2024-03-29");
    f.key("enter");
    assert_eq!(
        f.value(),
        Some(NaiveDate::from_ymd_opt(2024, 3, 29).unwrap())
    );
    assert_eq!(f.changes().len(), 1);
}

#[test]
fn date_picker_mask_native_clipboard_and_undo_preserve_the_template() {
    let mut f = Fixture::<DateSelection>::new(Some(date(10)));
    f.configure(|v| v.format = PickerFormat::mask("%Y-%m-%d").unwrap());
    f.update(|s, w, c| s.focus_input(0, w, c));
    f.draw();
    f.key("right");
    f.key("up");
    assert_eq!(f.text()[0].as_ref(), "2024-03-10");
    f.key("ctrl-z");
    assert_eq!(f.text()[0].as_ref(), "2024-02-10");
    f.key("ctrl-y");
    assert_eq!(f.text()[0].as_ref(), "2024-03-10");
    f.key("a");
    f.key("space");
    assert_eq!(f.text()[0].as_ref(), "2024-03-10");
    f.update(|_, _, c| c.write_to_clipboard(gpui::ClipboardItem::new_string("2024-02-29".into())));
    f.key("ctrl-v");
    assert_eq!(f.text()[0].as_ref(), "2024-02-29");
    f.key("ctrl-a");
    f.key("ctrl-x");
    assert_eq!(f.text()[0].as_ref(), "YYYY-MM-DD");
    assert_eq!(
        f.update(|_, _, c| c.read_from_clipboard().unwrap().text())
            .as_deref(),
        Some("2024-02-29")
    );
    f.key("ctrl-z");
    assert_eq!(f.text()[0].as_ref(), "2024-02-29");
    f.update(|_, _, c| c.write_to_clipboard(gpui::ClipboardItem::new_string("2024-02-31".into())));
    f.key("ctrl-v");
    assert_eq!(f.text()[0].as_ref(), "2024-02-29");
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_scoped_week_suffix_controls_default_display_and_parsing() {
    let mut f = Fixture::<DateSelection>::new(Some(date(20)));
    f.configure(|v| v.options.mode = PickerMode::Week);
    let scope = f.consumer.read_with(&f.cx, |v, _| v.scope.clone());
    f.cx.update_window(f.window.into(), |_, _, c| {
        scope.update_patch(c, |p| {
            p.locale.picker.week_ordinal =
                Some(crate::locale::PickerWeekOrdinal::Suffix("wk".into()));
        })
    })
    .unwrap()
    .unwrap();
    f.draw();
    assert_eq!(f.text()[0].as_ref(), "2024-8wk");
    f.edit(0, "2024-9wk");
    f.key("enter");
    assert_eq!(f.value(), Some(date(25)));
}

#[test]
fn date_picker_debounced_scroll_is_canceled_by_close_and_unmount() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.configure(|v| v.options.time.change_on_scroll = true);
    f.update(|s, w, c| {
        s.schedule_scroll(0, TimeUnit::Hour, 28., w, c);
        s.set_open(false, w, c);
    });
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), None);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_native_scroll_owns_one_timer_and_releases_it_on_completion() {
    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let mut f = Fixture::<TimeSelection>::new(Some(initial));
    f.configure(|v| {
        v.options.time.change_on_scroll = true;
        v.options.need_confirm = Some(false);
    });
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    assert_eq!(f.value(), Some(initial));
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    assert_eq!(f.value().unwrap().hour(), 8);
    assert_eq!(f.changes().len(), 1);
}

#[test]
fn date_picker_pending_scroll_uses_the_current_hidden_time_rows() {
    let initial = NaiveTime::from_hms_opt(1, 0, 0).unwrap();
    let hidden_zero = Rc::new(Cell::new(true));
    let dynamic = hidden_zero.clone();
    let mut f = Fixture::<TimeSelection>::new(Some(initial));
    f.configure(|v| {
        v.options.time.change_on_scroll = true;
        v.options.time.hide_disabled_options = true;
        v.options.need_confirm = Some(false);
        v.disabled_time = Some(Rc::new(move |_, _| {
            let dynamic = dynamic.clone();
            DisabledTime::new().hours(move || if dynamic.get() { vec![0] } else { vec![] })
        }));
    });
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    hidden_zero.set(false);
    f.configure(|_| {});
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), Some(initial));
    assert_eq!(f.draft(), Some(initial));
    assert!(f.changes().is_empty());
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
}

#[test]
fn date_picker_parent_value_replacement_cancels_pending_scroll() {
    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let replacement = NaiveTime::from_hms_opt(12, 30, 0).unwrap();
    let mut f = Fixture::<TimeSelection>::new(Some(initial));
    f.configure(|v| {
        v.controlled = Some(Some(initial));
        v.options.time.change_on_scroll = true;
        v.options.need_confirm = Some(false);
    });
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.configure(|v| v.controlled = Some(Some(replacement)));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    let event_count = f.events.borrow().len();
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), Some(replacement));
    assert_eq!(f.draft(), Some(replacement));
    assert_eq!(f.events.borrow().len(), event_count);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_time_row_height_change_rejects_an_old_scroll_timer() {
    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let mut f = Fixture::<TimeSelection>::new(Some(initial));
    f.configure(|v| {
        v.options.time.change_on_scroll = true;
        v.options.need_confirm = Some(false);
    });
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    let scope = f
        .consumer
        .read_with(&f.cx, |consumer, _| consumer.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch
                    .theme
                    .get_or_insert_default()
                    .components
                    .set::<DatePickerTheme>(DatePickerTokenPatch {
                        time_cell_height: Some(40.),
                        ..Default::default()
                    });
            })
            .unwrap();
    });
    f.draw();
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    assert_eq!(f.value(), Some(initial));
    assert_eq!(f.draft(), Some(initial));
    assert!(f.events.borrow().is_empty());
}

#[test]
fn date_picker_native_scroll_resources_drop_on_close_disable_and_unmount() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.configure(|v| v.options.time.change_on_scroll = true);
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.update(|s, w, c| s.set_open(false, w, c));
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.draft(), None);
    f.update(|s, w, c| s.set_open(true, w, c));
    f.draw();
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.configure(|v| v.disabled = true);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    f.configure(|v| v.disabled = false);
    f.update(|s, w, c| s.set_open(true, w, c));
    f.draw();
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.configure(|v| v.shown = false);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), None);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_same_id_state_replacement_rebinds_observation_and_releases_each_owner() {
    let initial = NaiveTime::from_hms_opt(6, 0, 0).unwrap();
    let replacement = NaiveTime::from_hms_opt(8, 0, 0).unwrap();
    let mut f = Fixture::<TimeSelection>::new(Some(initial));
    f.configure(|v| {
        v.options.time.change_on_scroll = true;
        v.options.need_confirm = Some(false);
    });
    f.wheel_time(0, -28.);
    let previous = f.state.clone();
    assert_eq!(
        previous.read_with(&f.cx, |s, _| s.pending_scroll_tasks()),
        1
    );
    let next = f.update(|_, window, cx| {
        cx.new(|cx| {
            let mut state = PickerState::<TimeSelection>::new(window, cx);
            state.set_value(Some(replacement), window, cx);
            state
        })
    });
    f.configure(|v| {
        v.state = next.clone();
        v.initial_open = false;
    });
    f.state = next;
    previous.read_with(&f.cx, |state, _| {
        assert!(!state.mounted);
        assert!(!state.is_open());
        assert_eq!(state.pending_scroll_tasks(), 0);
    });
    assert!(f.state.read_with(&f.cx, |s, _| s.mounted));
    let (invalidations, _subscription) = f.observe_invalidations();
    previous.update(&mut f.cx, |_, cx| cx.notify());
    f.cx.run_until_parked();
    assert_eq!(
        invalidations.get(),
        0,
        "the detached state no longer owns invalidation"
    );
    f.state.update(&mut f.cx, |_, cx| cx.notify());
    f.cx.run_until_parked();
    assert!(
        invalidations.get() > 0,
        "the current state invalidates its retained view"
    );
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(previous.read_with(&f.cx, |s, _| *s.value()), Some(initial));
    assert!(f.events.borrow().is_empty());
    f.update(|s, window, cx| s.set_open(true, window, cx));
    f.draw();
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.configure(|v| v.shown = false);
    f.state.read_with(&f.cx, |state, _| {
        assert!(!state.mounted);
        assert!(!state.is_open());
        assert_eq!(state.pending_scroll_tasks(), 0);
    });
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), Some(replacement));
}

#[test]
fn date_picker_inline_time_panel_rebinds_a_replacement_scope() {
    let mut f = Fixture::<TimeSelection>::new(Some(NaiveTime::MIN));
    let replacement_scope = f.cx.update(|cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        scope
    });
    f.configure(|v| {
        v.inline = true;
        v.options.need_confirm = Some(false);
        v.options.show_now = Some(false);
        v.scope = replacement_scope.clone();
    });
    let (invalidations, _subscription) = f.observe_invalidations();
    f.cx.update(|cx| {
        replacement_scope
            .update_patch(cx, |patch| {
                patch.locale.picker.hour = Some("Replacement scope hours".into());
            })
            .unwrap();
    });
    f.cx.run_until_parked();
    assert!(
        invalidations.get() > 0,
        "a patch on the current scope invalidates the inline panel"
    );
    f.draw();
    assert_eq!(
        f.state
            .read_with(&f.cx, |state, _| state.locale.hour.clone())
            .as_ref(),
        "Replacement scope hours"
    );
}

#[test]
fn date_picker_native_scroll_resources_drop_when_window_hides_or_deactivates() {
    let mut f = Fixture::<TimeSelection>::new(None);
    f.configure(|v| v.options.time.change_on_scroll = true);
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.draft(), None);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    f.wheel_time(0, -28.);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 1);
    struct AnotherWindow;
    impl Render for AnotherWindow {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
        }
    }
    let _other = f.cx.add_window(|window, _| {
        window.activate_window();
        AnotherWindow
    });
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.pending_scroll_tasks()), 0);
    f.cx.background_executor
        .advance_clock(Duration::from_millis(301));
    f.draw();
    assert_eq!(f.value(), None);
    assert!(f.changes().is_empty());
}

#[test]
fn date_picker_deferred_consumer_callback_is_canceled_after_unmount() {
    let mut f = Fixture::<DateSelection>::new(None);
    let calls = Rc::new(Cell::new(0));
    let output = calls.clone();
    f.update(|s, w, c| {
        s.on_change = Some(Rc::new(move |_, _, _| output.set(output.get() + 1)));
        s.select_date(date(21), PickerChangeSource::Calendar, w, c);
        s.release_mount();
    });
    f.draw();
    assert_eq!(calls.get(), 0);
}

#[test]
fn date_picker_popup_pointer_open_escape_and_focus_return() {
    let mut f = Fixture::<DateSelection>::with_open(None, false);
    let bounds = f.state.read_with(&f.cx, |s, _| s.geometry.trigger.unwrap());
    f.click(bounds.center());
    assert!(f.open());
    f.key("escape");
    assert!(!f.open());
    assert!(f.changes().is_empty());
    f.update(|s, w, c| assert!(s.editors[0].read(c).focus_handle(c).is_focused(w)));
    f.click(point(px(520.), px(620.)));
    assert!(!f.open());
}

#[test]
fn date_picker_range_pointer_preview_tracks_consecutive_calendar_cells() {
    let initial = PickerRange::new(Some(date(12)), None);
    let mut f = Fixture::<DateRangeSelection>::new(initial);
    f.configure(|v| v.disabled_date = Some(Rc::new(|d, _| d == date(21))));
    let start = f.date_position(0, date(12));
    f.click(start);

    for day in [19, 20, 19, 26, 27, 20] {
        f.move_to_date(0, date(day));
        f.state.read_with(&f.cx, |s, _| {
            let end = s
                .geometry
                .cells
                .values()
                .find(|cell| cell.date == Some(date(day)) && cell.in_view)
                .unwrap();
            assert!(
                end.range_end,
                "moving to day {day} must preview that endpoint"
            );
            assert!(s.geometry.cells.values().any(|cell| cell.in_range));
        });
        assert_eq!(f.text()[1].as_ref(), format!("2024-02-{day:02}"));
        assert_eq!(f.value(), initial);
        assert_eq!(f.draft(), initial);
        assert!(f.changes().is_empty());
    }

    let header = f
        .state
        .read_with(&f.cx, |s, _| s.geometry.headers[0].center());
    for position in [None, Some(header)] {
        f.move_to_date(0, date(20));
        if let Some(position) = position {
            f.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
                position,
                ..Default::default()
            }));
        } else {
            f.move_to_date(0, date(21));
        }
        assert!(f.state.read_with(&f.cx, |s, _| s.hover.is_none()));
        assert!(f.state.read_with(&f.cx, |s, _| {
            s.geometry.cells.values().all(|cell| !cell.in_range)
        }));
        assert!(f.text()[1].is_empty());
        assert_eq!(f.draft(), initial);
        assert_eq!(f.value(), initial);
        assert!(f.changes().is_empty());
    }

    f.move_to_date(0, date(20));
    let position = f.date_position(0, date(20));
    f.click(position);
    assert_eq!(f.value(), PickerRange::new(Some(date(12)), Some(date(20))));
    assert_eq!(f.changes().len(), 1);
    assert!(!f.open());
}

#[test]
fn date_picker_range_pointer_preview_distinguishes_matching_dates_in_adjacent_panels() {
    let initial = PickerRange::new(Some(date(12)), None);
    let mut f = Fixture::<DateRangeSelection>::new(initial);
    f.update(|s, w, c| s.focus_input(1, w, c));
    f.draw();

    // February 29 is also a leading cell in the March panel.
    for panel in [0, 1, 0, 1] {
        f.move_to_date(panel, date(29));
        f.state.read_with(&f.cx, |s, _| {
            let end = s
                .geometry
                .cells
                .values()
                .find(|cell| cell.date == Some(date(29)) && cell.in_view)
                .unwrap();
            assert!(end.range_end, "panel {panel} must retain the date preview");
        });
        assert_eq!(f.text()[1].as_ref(), "2024-02-29");
        assert_eq!(f.value(), initial);
        assert_eq!(f.draft(), initial);
        assert!(f.changes().is_empty());
    }

    // MouseExited alone does not update GPUI's retained pointer position.
    let outside = point(px(-1.), px(-1.));
    f.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: outside,
        ..Default::default()
    }));
    f.event(PlatformInput::MouseExited(gpui::MouseExitEvent {
        position: outside,
        ..Default::default()
    }));
    assert!(f.state.read_with(&f.cx, |s, _| s.hover.is_none()));
    assert!(f.text()[1].is_empty());
    f.move_to_date(1, date(29));
    assert_eq!(f.text()[1].as_ref(), "2024-02-29");
}

#[test]
fn date_picker_single_pointer_preview_tracks_consecutive_calendar_cells() {
    let initial = Some(date(10));
    let mut f = Fixture::<DateSelection>::new(initial);
    for day in [19, 20, 19, 26, 27, 20] {
        f.move_to_date(0, date(day));
        assert_eq!(f.text()[0].as_ref(), format!("2024-02-{day:02}"));
        assert_eq!(f.value(), initial);
        assert_eq!(f.draft(), initial);
        assert!(f.changes().is_empty());
    }
    let outside = point(px(-1.), px(-1.));
    f.event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
        position: outside,
        ..Default::default()
    }));
    assert_eq!(f.text()[0].as_ref(), "2024-02-10");
}

#[test]
fn date_picker_range_hover_renders_a_solid_preview_without_changing_the_draft() {
    let initial = PickerRange::new(Some(date(12)), None);
    let mut f = Fixture::<DateRangeSelection>::new(initial);
    f.update(|s, w, c| {
        s.focus_input(1, w, c);
        s.hover_date(Some(date(19)), w, c);
    });
    f.draw();
    let cells = f.state.read_with(&f.cx, |s, _| s.geometry.cells.clone());
    let start = cells
        .values()
        .find(|c| c.date == Some(date(12)) && c.in_view)
        .unwrap();
    let middle = cells
        .values()
        .find(|c| c.date == Some(date(15)) && c.in_view)
        .unwrap();
    let end = cells
        .values()
        .find(|c| c.date == Some(date(19)) && c.in_view)
        .unwrap();
    assert!(start.range_start && end.range_end && middle.in_range);
    assert!(!middle.range_hover);
    assert_eq!(f.value(), initial);
    assert_eq!(f.draft(), initial);
    assert!(f.changes().is_empty());
    f.update(|s, w, c| s.hover_date(None, w, c));
    f.draw();
    assert!(
        f.state
            .read_with(&f.cx, |s, _| s.geometry.cells.values().all(|c| !c.in_range))
    );
}

#[test]
fn date_picker_multiple_weeks_highlight_every_selected_row() {
    let values: Arc<[NaiveDate]> = Arc::from([date(12), date(19)]);
    let mut f = Fixture::<MultipleDateSelection>::new(values.clone());
    f.configure(|v| v.options.mode = PickerMode::Week);
    f.state.read_with(&f.cx, |s, _| {
        for day in [12, 19] {
            let cell = s
                .geometry
                .cells
                .values()
                .find(|c| c.date == Some(date(day)))
                .unwrap();
            assert!(
                cell.week_hover,
                "selected week {day} must paint its whole row"
            );
        }
        assert!(
            s.geometry
                .cells
                .values()
                .filter(|c| c.week_selected)
                .count()
                >= 7
        );
    });
    assert_eq!(f.value(), values);
    assert!(f.changes().is_empty());
}
