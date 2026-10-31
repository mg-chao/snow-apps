// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Menu UIA fixture; publishes evidence without changing system settings.

use ant_design_gpui::{
    ConfigScope, UiRoot,
    menu::{Menu, MenuEntry, MenuEvent, MenuItem, MenuItems, MenuMode, MenuState, SubMenu},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

fn items(entries: impl IntoIterator<Item = MenuEntry>) -> MenuItems {
    MenuItems::new(entries).unwrap()
}

struct Fixture {
    scope: ConfigScope,
    inline: Entity<MenuState>,
    popup: Entity<MenuState>,
    horizontal: Entity<MenuState>,
    multiple: Entity<MenuState>,
    last_key: String,
    last_source: String,
    activations: usize,
    selections: usize,
    deselections: usize,
    events: Vec<String>,
}

impl Fixture {
    fn menu(&self, id: &'static str, state: &Entity<MenuState>, cx: &Context<Self>) -> Menu {
        Menu::new(id, state)
            .scope(self.scope.clone())
            .automation_id(id)
            .accessibility_label(match id {
                "inline" => "Workspace navigation",
                "popup" => "File navigation",
                "multiple" => "Independent choices",
                _ => "Main navigation",
            })
            .on_event(cx.listener(|this, event, _, cx| {
                this.events.push(format!("{event:?}"));
                match event {
                    MenuEvent::Select(_) => this.selections += 1,
                    MenuEvent::Deselect(_) => this.deselections += 1,
                    _ => {}
                }
                if let MenuEvent::Activate(activation) = event {
                    this.last_key = activation.key.to_string();
                    this.last_source = format!("{:?}", activation.source);
                    this.activations += 1;
                    cx.notify();
                }
            }))
    }
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .p(px(48.))
            .flex()
            .flex_col()
            .gap(px(32.))
            .child(
                self.menu("horizontal", &self.horizontal, cx)
                    .mode(MenuMode::Horizontal)
                    .default_selected_keys(["top-home"])
                    .w_full(),
            )
            .child(
                div()
                    .flex()
                    .items_start()
                    .gap(px(48.))
                    .child(
                        self.menu("inline", &self.inline, cx)
                            .mode(MenuMode::Inline)
                            .default_selected_keys(["home"])
                            .w(px(256.)),
                    )
                    .child(
                        self.menu("popup", &self.popup, cx)
                            .mode(MenuMode::Vertical)
                            .w(px(256.)),
                    )
                    .child(
                        self.menu("multiple", &self.multiple, cx)
                            .mode(MenuMode::Vertical)
                            .multiple(true)
                            .default_selected_keys(["choice-a", "choice-b"])
                            .w(px(256.)),
                    ),
            )
    }
}

fn main() {
    let output = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("menu_accessibility <output.json>"),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| { if cx.windows().is_empty() { cx.quit(); } }).detach();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(1000.), px(640.)), cx))),
            ..Default::default()
        }, move |window, cx| {
            window.set_window_title("Menu accessibility fixture");
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
            let inline = cx.new(|cx| MenuState::new(items([
                MenuItem::new("home", "Home").into(),
                MenuItem::new("disabled", "Disabled").disabled(true).into(),
                SubMenu::new("projects", "Projects", items([
                    MenuItem::new("alpha", "Alpha project").into(),
                    MenuItem::new("beta", "Beta project").disabled(true).into(),
                    MenuItem::new("gamma", "Gamma project").into(),
                ])).into(),
            ]), window, cx));
            let popup = cx.new(|cx| MenuState::new(items([
                SubMenu::new("more", "More", items([
                    MenuItem::new("archive", "Archive").into(),
                    MenuItem::new("delete", "Delete").danger(true).into(),
                ])).into(),
            ]), window, cx));
            let horizontal = cx.new(|cx| MenuState::new(items([
                MenuItem::new("top-home", "Home").into(),
                SubMenu::new("help", "Help", items([
                    MenuItem::new("docs", "Documentation").into(),
                ])).into(),
            ]), window, cx));
            let multiple = cx.new(|cx| MenuState::new(items([
                MenuItem::new("choice-a", "Choice A").into(),
                MenuItem::new("choice-b", "Choice B").into(),
                MenuItem::new("choice-c", "Choice C").into(),
            ]), window, cx));
            let fixture = cx.new(|_| Fixture { scope: scope.clone(), inline, popup, horizontal, multiple,
                last_key: String::new(), last_source: String::new(), activations: 0,
                selections: 0, deselections: 0, events: Vec::new() });
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                for _ in 0..600 {
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window, cx| {
                        let Some(fixture) = observation.upgrade() else { return; };
                        let f = fixture.read(cx);
                        let value = serde_json::json!({
                            "process_id": std::process::id(),
                            "inline_open": f.inline.read(cx).open_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "inline_selected": f.inline.read(cx).selected_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "popup_open": f.popup.read(cx).open_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "popup_selected": f.popup.read(cx).selected_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "horizontal_selected": f.horizontal.read(cx).selected_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "multiple_selected": f.multiple.read(cx).selected_keys().iter().map(ToString::to_string).collect::<Vec<_>>(),
                            "last_key": f.last_key, "last_source": f.last_source, "activations": f.activations,
                            "selections": f.selections, "deselections": f.deselections,
                            "events": f.events,
                            "tree": window.debug_a11y_tree_json().and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok()),
                        });
                        let temporary = output.with_extension("tmp");
                        if let Err(error) = std::fs::write(&temporary, serde_json::to_vec_pretty(&value).unwrap())
                            .and_then(|_| std::fs::rename(temporary, &output)) {
                            eprintln!("Menu accessibility snapshot publication: {error}");
                        }
                    }).is_err() { break; }
                }
            }).detach();
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).unwrap();
    });
}
