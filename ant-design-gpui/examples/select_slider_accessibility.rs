// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native UIA fixture. No remote services or changes to system accessibility settings.
use ant_design_gpui::{
    ConfigScope, Select, SelectMode, SelectOption, SelectOptions, SelectSettings, SelectState,
    Slider, SliderOptions, SliderState, UiRoot,
    select::{SelectGroup, SelectSearch},
    slider::SliderRange,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

struct Fixture {
    scope: ConfigScope,
    single: Entity<SelectState<String>>,
    multiple: Entity<SelectState<String>>,
    slider: Entity<SliderState>,
}
impl Render for Fixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .p(px(64.))
            .bg(gpui::white())
            .flex()
            .flex_col()
            .gap(px(32.))
            .child(
                Select::new("single", &self.single)
                    .scope(self.scope.clone())
                    .w(px(320.))
                    .accessibility_label("Choose one")
                    .automation_id("single")
                    .allow_clear(true),
            )
            .child(
                Select::new("multiple", &self.multiple)
                    .scope(self.scope.clone())
                    .w(px(320.))
                    .accessibility_label("Choose several")
                    .automation_id("multiple")
                    .allow_clear(true),
            )
            .child(
                Slider::new("slider", &self.slider)
                    .scope(self.scope.clone())
                    .w(px(320.))
                    .handle_labels(["Minimum", "Middle", "Maximum"])
                    .automation_id("slider"),
            )
    }
}
fn main() {
    let output = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("select_slider_accessibility <output.json>"),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| { if cx.windows().is_empty() { cx.quit(); } }).detach();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(720.), px(600.)), cx))),
            ..Default::default()
        }, move |window, cx| {
            window.set_window_title("Select and Slider accessibility fixture");
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
            let data = SelectOptions::from_entries([
                SelectGroup::new("people", "People", [SelectOption::new("jack", "Jack".to_owned(), "Jack"), SelectOption::new("lucy", "Lucy".to_owned(), "Lucy"), SelectOption::new("disabled", "Disabled".to_owned(), "Disabled").disabled(true)]).into(),
                SelectGroup::new("directory", "Directory", (0..10_000).map(|i| SelectOption::new(i as u64, format!("User {i}"), format!("User {i}")))).into(),
            ]).unwrap();
            let single = cx.new(|cx| {
                let mut state = SelectState::new(data.clone(), window, cx);
                state.set_search(SelectSearch { enabled: Some(true), ..SelectSearch::label() }, cx);
                state.set_values(["Jack".to_owned()], cx); state
            });
            let multiple = cx.new(|cx| {
                let mut state = SelectState::new(data, window, cx);
                state.set_settings(SelectSettings { mode: SelectMode::Multiple, ..Default::default() }, cx);
                state.set_values(["Jack".to_owned(), "Lucy".to_owned()], cx); state
            });
            let slider = cx.new(|cx| {
                let mut state = SliderState::with_options(SliderOptions { range: Some(SliderRange::default()), disabled: [true, false, false].into(), ..Default::default() }, window, cx).unwrap();
                state.set_value([20., 50., 80.], window, cx).unwrap(); state
            });
            let fixture = cx.new(|_| Fixture { scope: scope.clone(), single, multiple, slider });
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                for _ in 0..300 {
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window, cx| {
                        let Some(fixture) = observation.upgrade() else { return; };
                        let f = fixture.read(cx);
                        let value = serde_json::json!({
                            "process_id": std::process::id(),
                            "single": f.single.read(cx).values().iter().map(|v| &v.value).collect::<Vec<_>>(),
                            "multiple": f.multiple.read(cx).values().iter().map(|v| &v.value).collect::<Vec<_>>(),
                            "slider": f.slider.read(cx).value().as_slice(),
                            "tree": window.debug_a11y_tree_json().and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok()),
                        });
                        let temporary = output.with_extension("tmp");
                        if let Err(error) = std::fs::write(&temporary, serde_json::to_vec_pretty(&value).unwrap())
                            .and_then(|_| std::fs::rename(temporary, &output)) {
                            // Windows readers may briefly deny rename/delete sharing.
                            // Evidence publication retries on the next tick; it must
                            // never terminate the control's native event loop.
                            eprintln!("Accessibility snapshot publication: {error}");
                        }
                    }).is_err() { break; }
                }
            }).detach();
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).unwrap();
    });
}
