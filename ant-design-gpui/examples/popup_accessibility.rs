// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native UI Automation fixture; see scripts/test-popup-accessibility.ps1.

use ant_design_gpui::{
    Button, ConfigScope, Popconfirm, Popover, Tooltip, UiRoot, popup::PopupTriggers,
};
use gpui::{
    App, AppContext, Bounds, Context, Focusable, IntoElement, Render, SharedString, Window,
    WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

struct Fixture {
    scope: ConfigScope,
    output: PathBuf,
    trigger_clicks: usize,
    confirms: usize,
    cancels: usize,
    errors: usize,
    popover_open: bool,
    disabled: bool,
}

impl Fixture {
    fn record(&self) {
        std::fs::write(
            &self.output,
            serde_json::to_vec(&serde_json::json!({
                "trigger_clicks": self.trigger_clicks, "confirms": self.confirms,
                "cancels": self.cancels, "errors": self.errors,
            }))
            .unwrap(),
        )
        .expect("write native accessibility evidence");
    }
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let scope = self.scope.clone();
        let weak = cx.weak_entity();
        div()
            .size_full()
            .bg(gpui::white())
            .p(px(80.))
            .flex()
            .flex_col()
            .gap(px(24.))
            .child(
                Tooltip::new("hint")
                    .scope(scope.clone())
                    .title("Explains the existing control")
                    .automation_id("tooltip-surface")
                    .child(
                        Button::new("hint-button")
                            .scope(scope.clone())
                            .label("Hint")
                            .automation_id("hint-trigger")
                            .accessibility_description("Existing description"),
                    ),
            )
            .child(
                Popover::new("information")
                    .scope(scope.clone())
                    .title("Information")
                    .text("Popup details")
                    .triggers(PopupTriggers::CLICK)
                    .open(self.popover_open)
                    .automation_id("popover-surface")
                    .on_open_change(cx.listener(
                        |state, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                            state.popover_open = change.open;
                            cx.notify();
                        },
                    ))
                    .child(
                        Button::new("information-button")
                            .scope(scope.clone())
                            .label("Information")
                            .disabled(self.disabled)
                            .automation_id("popover-trigger")
                            .on_click(cx.listener(|state, _, _, _| {
                                state.trigger_clicks += 1;
                                state.record();
                            })),
                    ),
            )
            .child(
                Popconfirm::new("confirmation")
                    .scope(scope.clone())
                    .title("Delete the item?")
                    .description("This operation cannot be undone.")
                    .automation_id("confirm-surface")
                    .on_confirm_async(move |_, _, cx| {
                        let attempt = weak
                            .update(cx, |state, _| {
                                state.confirms += 1;
                                state.record();
                                state.confirms
                            })
                            .unwrap();
                        let timer = cx.background_executor().timer(Duration::from_secs(2));
                        cx.background_executor().spawn(async move {
                            timer.await;
                            if attempt == 1 {
                                Err(SharedString::from("Please retry"))
                            } else {
                                Ok(())
                            }
                        })
                    })
                    .on_cancel(cx.listener(|state, _, _, _| {
                        state.cancels += 1;
                        state.record();
                    }))
                    .on_confirm_error(cx.listener(|state, _, _, _| {
                        state.errors += 1;
                        state.record();
                    }))
                    .child(
                        Button::new("confirmation-button")
                            .scope(scope.clone())
                            .label("Delete")
                            .automation_id("confirm-trigger"),
                    ),
            )
            .child(
                Button::new("availability")
                    .scope(scope.clone())
                    .label("Toggle disabled")
                    .automation_id("toggle-disabled")
                    .on_click(cx.listener(|state, _, _, cx| {
                        state.disabled = !state.disabled;
                        cx.notify();
                    })),
            )
            .child(
                Button::new("outside")
                    .scope(scope)
                    .label("Outside")
                    .automation_id("outside"),
            )
    }
}

fn main() {
    let output = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("popup_accessibility <state.json>"),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        let evidence = output.with_extension("tree.json");
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(720.), px(600.)),
                    cx,
                ))),
                ..Default::default()
            },
            move |window, cx| {
                window.set_window_title("Popup accessibility fixture");
                let scope = ConfigScope::new_window(cx);
                scope
                    .set_patch(
                        ant_design_gpui::ConfigPatch {
                            motion: Some(false),
                            ..Default::default()
                        },
                        cx,
                    )
                    .unwrap();
                let view = cx.new(|_| Fixture {
                    scope: scope.clone(),
                    output,
                    trigger_clicks: 0,
                    confirms: 0,
                    cancels: 0,
                    errors: 0,
                    popover_open: false,
                    disabled: false,
                });
                view.read(cx).record();
                let root = cx.new(|cx| UiRoot::new(scope, view, window, cx));
                window.focus(&root.focus_handle(cx), cx);
                window.activate_window();
                window
                    .spawn(cx, async move |cx| {
                        for _ in 0..40 {
                            cx.background_executor()
                                .timer(Duration::from_millis(200))
                                .await;
                            if cx
                                .update(|window, _| {
                                    if let Some(tree) = window.debug_a11y_tree_json() {
                                        std::fs::write(&evidence, tree)
                                            .expect("write native accessibility tree");
                                    }
                                })
                                .is_err()
                            {
                                break;
                            }
                        }
                    })
                    .detach();
                root
            },
        )
        .unwrap();
        cx.activate(true);
    });
}
