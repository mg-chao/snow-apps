// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Windows UI Automation fixture. The companion script defaults to a hidden window.

use ant_design_gpui::{
    Button, ConfigScope, Direction, Pagination, PaginationEvent, PaginationState, UiRoot,
    locale::Locale,
    pagination::{PaginationMode, QuickJumper},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

struct Fixture {
    scope: ConfigScope,
    main: Entity<PaginationState>,
    simple: Entity<PaginationState>,
    read_only: Entity<PaginationState>,
    disabled: Entity<PaginationState>,
    main_total: u64,
    main_disabled: bool,
    locale_index: usize,
    events: Vec<serde_json::Value>,
}

fn event_record(control: &str, event: &PaginationEvent) -> serde_json::Value {
    match event {
        PaginationEvent::Change(change) => serde_json::json!({
            "control": control, "type": "change", "current": change.current,
            "page_size": change.page_size, "source": format!("{:?}", change.source),
        }),
        PaginationEvent::ShowSizeChange(change) => serde_json::json!({
            "control": control, "type": "size-change", "current": change.current,
            "page_size": change.page_size,
        }),
    }
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .p(px(40.))
            .bg(gpui::white())
            .flex()
            .flex_col()
            .gap(px(24.))
            .child("Pagination accessibility fixture")
            .child(
                Pagination::new("main", &self.main)
                    .scope(self.scope.clone())
                    .total(self.main_total)
                    .default_current(6)
                    .disabled(self.main_disabled)
                    .show_size_changer(true)
                    .quick_jumper(QuickJumper::Blur)
                    .show_total_text(|total, range, _, _| {
                        format!("{}-{} of {total} items", range[0], range[1])
                    })
                    .automation_id("pagination-main")
                    .on_event(cx.listener(|state, event, _, cx| {
                        state.events.push(event_record("main", event));
                        cx.notify();
                    })),
            )
            .child(
                Pagination::new("simple", &self.simple)
                    .scope(self.scope.clone())
                    .total(50)
                    .default_current(2)
                    .mode(PaginationMode::Simple)
                    .automation_id("pagination-simple")
                    .on_event(cx.listener(|state, event, _, cx| {
                        state.events.push(event_record("simple", event));
                        cx.notify();
                    })),
            )
            .child(
                Pagination::new("read-only", &self.read_only)
                    .scope(self.scope.clone())
                    .total(50)
                    .default_current(2)
                    .mode(PaginationMode::SimpleReadOnly)
                    .automation_id("pagination-readonly"),
            )
            .child(
                Pagination::new("disabled", &self.disabled)
                    .scope(self.scope.clone())
                    .total(500)
                    .default_current(6)
                    .disabled(true)
                    .show_size_changer(true)
                    .quick_jumper(QuickJumper::Blur)
                    .automation_id("pagination-disabled")
                    .on_event(cx.listener(|state, event, _, cx| {
                        state.events.push(event_record("disabled", event));
                        cx.notify();
                    })),
            )
            .child(
                div()
                    .flex()
                    .flex_wrap()
                    .gap(px(8.))
                    .child(
                        Button::new("toggle-disabled")
                            .scope(self.scope.clone())
                            .label("Toggle disabled")
                            .automation_id("pagination-toggle-disabled")
                            .on_click(cx.listener(|state, _, _, cx| {
                                state.main_disabled = !state.main_disabled;
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("cycle-locale")
                            .scope(self.scope.clone())
                            .label("Cycle locale")
                            .automation_id("pagination-cycle-locale")
                            .on_click(cx.listener(|state, _, _, cx| {
                                state.locale_index = (state.locale_index + 1) % 3;
                                let locale = match state.locale_index {
                                    1 => Locale::zh_cn(),
                                    2 => Locale::zh_tw(),
                                    _ => Locale::en_us(),
                                };
                                state
                                    .scope
                                    .update_patch(cx, |patch| patch.locale.locale = Some(locale))
                                    .expect("valid fixture locale");
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("toggle-direction")
                            .scope(self.scope.clone())
                            .label("Toggle direction")
                            .automation_id("pagination-toggle-direction")
                            .on_click(cx.listener(|state, _, _, cx| {
                                let direction =
                                    if state.scope.snapshot(cx).direction == Direction::Ltr {
                                        Direction::Rtl
                                    } else {
                                        Direction::Ltr
                                    };
                                state
                                    .scope
                                    .update_patch(cx, |patch| patch.direction = Some(direction))
                                    .expect("valid fixture direction");
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("focus-last")
                            .scope(self.scope.clone())
                            .label("Focus last page")
                            .automation_id("pagination-focus-last")
                            .on_click(cx.listener(|state, _, window, cx| {
                                let last = state.main.read(cx).page_count();
                                state.main.update(cx, |pager, cx| {
                                    pager.set_current(last, window, cx);
                                    pager.focus_page(last, window, cx);
                                });
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("shrink-total")
                            .scope(self.scope.clone())
                            .label("Shrink total")
                            .automation_id("pagination-shrink-total")
                            .on_click(cx.listener(|state, _, _, cx| {
                                state.main_total = 50;
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("reset")
                            .scope(self.scope.clone())
                            .label("Reset fixture")
                            .automation_id("pagination-reset")
                            .on_click(cx.listener(|state, _, window, cx| {
                                state.main_total = 500;
                                state.main_disabled = false;
                                state.main.update(cx, |pager, cx| {
                                    pager.set_page_size(10, window, cx);
                                    pager.set_total(500, window, cx);
                                    pager.set_current(6, window, cx);
                                });
                                cx.notify();
                            })),
                    ),
            )
            .child(
                Button::new("outside")
                    .scope(self.scope.clone())
                    .label("Outside pagination")
                    .automation_id("pagination-outside"),
            )
    }
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    let output = PathBuf::from(
        arguments
            .next()
            .expect("pagination_accessibility <state.json> [--interactive]"),
    );
    let interactive = arguments.any(|argument| argument == "--interactive");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| {
            if cx.windows().is_empty() { cx.quit(); }
        }).detach();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(1100.), px(620.)), cx))),
            show: interactive,
            focus: interactive,
            ..Default::default()
        }, move |window, cx| {
            window.resize(size(px(1100.), px(620.)));
            window.set_window_title("Pagination accessibility fixture");
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).expect("valid fixture config");
            let main = cx.new(|cx| PaginationState::new(window, cx));
            let simple = cx.new(|cx| PaginationState::new(window, cx));
            let read_only = cx.new(|cx| PaginationState::new(window, cx));
            let disabled = cx.new(|cx| PaginationState::new(window, cx));
            let fixture = cx.new(|_| Fixture {
                scope: scope.clone(), main, simple, read_only, disabled,
                main_total: 500, main_disabled: false, locale_index: 0, events: Vec::new(),
            });
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                for _ in 0..1200 {
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window, cx| {
                        window.draw(cx).clear(cx);
                        let Some(fixture) = observation.upgrade() else { return; };
                        let fixture = fixture.read(cx);
                        let pager = |state: &Entity<PaginationState>| {
                            let state = state.read(cx);
                            serde_json::json!({"current": state.current(), "page_size": state.page_size(),
                                "total": state.total(), "page_count": state.page_count()})
                        };
                        let value = serde_json::json!({
                            "process_id": std::process::id(), "interactive": interactive,
                            "main": pager(&fixture.main), "simple": pager(&fixture.simple),
                            "readonly": pager(&fixture.read_only), "disabled": pager(&fixture.disabled),
                            "main_disabled": fixture.main_disabled, "locale_index": fixture.locale_index,
                            "direction": format!("{:?}", fixture.scope.snapshot(cx).direction),
                            "events": fixture.events,
                            "tree": window.debug_a11y_tree_json().and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok()),
                        });
                        let temporary = output.with_extension("tmp");
                        if let Err(error) = std::fs::write(&temporary, serde_json::to_vec_pretty(&value).unwrap())
                            .and_then(|_| std::fs::rename(temporary, &output)) {
                            eprintln!("Pagination accessibility snapshot publication: {error}");
                        }
                    }).is_err() { break; }
                }
            }).detach();
            if interactive { window.activate_window(); }
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).expect("open pagination fixture");
    });
}
