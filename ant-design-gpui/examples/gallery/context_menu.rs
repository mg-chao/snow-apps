// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::collections::BTreeMap;

use ant_design_gpui::{
    ConfigScope, ContextMenu, ContextMenuState, MenuEntry, MenuEvent, MenuItem, MenuItems,
    icons::IconName,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, Subscription,
    Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo};

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}

pub(super) const DEMOS: &[Demo] = &[
    demo(
        "basic",
        "File actions",
        "Right-click the surface, or focus it and press Shift+F10. Arrow keys navigate; Enter activates; Escape dismisses.",
    ),
    demo(
        "nested",
        "Nested menus",
        "Submenus follow pointer intent and open from the keyboard. Placement flips at the window edges.",
    ),
    demo(
        "selection",
        "Checks and radio groups",
        "Checkboxes toggle independently. Radio options share a group and preserve their value between openings.",
    ),
    demo(
        "overflow",
        "Long menus",
        "A constrained menu scrolls. Keyboard navigation reveals the active row, and typeahead finds an item by its label.",
    ),
    demo(
        "lazy",
        "Lazy submenu",
        "Recent documents are created when their submenu opens. Reopening requests fresh content.",
    ),
    demo(
        "metadata",
        "Labels and metadata",
        "Icons, disabled commands, destructive actions, badges and shortcut hints use independent columns.",
    ),
];

pub(super) struct ContextMenuPage {
    scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub states: BTreeMap<&'static str, Entity<ContextMenuState>>,
    pub last_action: String,
    _config: Subscription,
}

fn items(entries: impl IntoIterator<Item = MenuEntry>) -> MenuItems {
    MenuItems::new(entries).expect("unique example menu identifiers")
}

fn file_actions() -> MenuItems {
    items([
        MenuItem::new("open", "Open")
            .icon(IconName::FolderOpenOutlined)
            .shortcut("Enter")
            .into(),
        MenuItem::new("copy", "Copy")
            .icon(IconName::CopyOutlined)
            .shortcut("Ctrl+C")
            .into(),
        MenuItem::new("rename", "Rename")
            .icon(IconName::EditOutlined)
            .shortcut("F2")
            .into(),
        MenuEntry::Separator,
        MenuItem::new("delete", "Delete")
            .icon(IconName::DeleteOutlined)
            .danger(true)
            .shortcut("Del")
            .into(),
    ])
}

impl ContextMenuPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut menus = BTreeMap::new();
        menus.insert("basic", file_actions());
        menus.insert(
            "nested",
            items([
                MenuItem::new("open", "Open")
                    .icon(IconName::FolderOpenOutlined)
                    .into(),
                MenuItem::new("share", "Share with")
                    .submenu(items([
                        MenuItem::new("team", "Design team").into(),
                        MenuItem::new("link", "Copy link")
                            .icon(IconName::LinkOutlined)
                            .into(),
                        MenuItem::new("export", "Export")
                            .submenu(items([
                                MenuItem::new("png", "PNG image").into(),
                                MenuItem::new("svg", "SVG document").into(),
                                MenuItem::new("pdf", "PDF document").into(),
                            ]))
                            .into(),
                    ]))
                    .into(),
                MenuItem::new("sort", "Sort by")
                    .submenu(items([
                        MenuItem::new("name", "Name")
                            .radio("sort-order", true)
                            .into(),
                        MenuItem::new("modified", "Last modified")
                            .radio("sort-order", false)
                            .into(),
                    ]))
                    .into(),
            ]),
        );
        menus.insert(
            "selection",
            items([
                MenuItem::new("hidden", "Show hidden files")
                    .checked(false)
                    .into(),
                MenuItem::new("extensions", "Show file extensions")
                    .checked(true)
                    .into(),
                MenuEntry::Separator,
                MenuItem::new("list", "List view")
                    .radio("view", true)
                    .into(),
                MenuItem::new("grid", "Grid view")
                    .radio("view", false)
                    .into(),
                MenuItem::new("columns", "Column view")
                    .radio("view", false)
                    .into(),
            ]),
        );
        menus.insert(
            "overflow",
            items((0_usize..80).map(|index| {
                MenuItem::new(("document", index), format!("Document {index:02}"))
                    .disabled(index == 1)
                    .into()
            })),
        );
        menus.insert(
            "lazy",
            items([
                MenuItem::new("open", "Open")
                    .icon(IconName::FolderOpenOutlined)
                    .into(),
                MenuItem::new("recent", "Recent documents")
                    .lazy_submenu(|_, _| {
                        items([
                            MenuItem::new("spec", "Design specification").into(),
                            MenuItem::new("notes", "Meeting notes").into(),
                            MenuItem::new("roadmap", "Product roadmap").into(),
                        ])
                    })
                    .into(),
            ]),
        );
        menus.insert(
            "metadata",
            items([
                MenuItem::new("copy", "Copy")
                    .icon(IconName::CopyOutlined)
                    .shortcut("Ctrl+C")
                    .into(),
                MenuItem::new("paste", "Paste")
                    .icon(IconName::SnippetsOutlined)
                    .shortcut("Ctrl+V")
                    .disabled(true)
                    .into(),
                MenuItem::new("comments", "Comments")
                    .icon(IconName::CommentOutlined)
                    .badge("12")
                    .into(),
                MenuItem::new(
                    "experimental",
                    "An experimental command with a descriptive label",
                )
                .badge("New")
                .shortcut("Ctrl+Shift+E")
                .into(),
                MenuEntry::Separator,
                MenuItem::new("delete", "Delete permanently")
                    .danger(true)
                    .icon(IconName::DeleteOutlined)
                    .into(),
            ]),
        );
        Self {
            states: menus
                .into_iter()
                .map(|(id, items)| (id, cx.new(|cx| ContextMenuState::new(items, window, cx))))
                .collect(),
            anchors,
            last_action: "No action selected".into(),
            _config: scope.observe(cx),
            scope,
        }
    }

    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let t = &config.theme.tokens;
        div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(
                ContextMenu::new(id, &self.states[id])
                    .scope(self.scope.clone())
                    .accessibility_label(format!("{id} context actions"))
                    .on_event(cx.listener(|this, event, _, cx| {
                        if let MenuEvent::Activate(activation) = event {
                            this.last_action = format!("Selected: {}", activation.label);
                            cx.notify();
                        }
                    }))
                    .child(
                        div()
                            .id(format!("context-target-{id}"))
                            .debug_selector(move || format!("context-menu.{id}.target"))
                            .w_full()
                            .h(px(144.))
                            .border_1()
                            .border_dashed()
                            .border_color(t.color_border)
                            .rounded(px(t.border_radius as f32))
                            .bg(t.color_fill_quaternary)
                            .flex()
                            .flex_col()
                            .items_center()
                            .justify_center()
                            .gap(px(8.))
                            .child(if id == "basic" {
                                "Design specification.fig"
                            } else {
                                "Right-click for actions"
                            })
                            .child(
                                div()
                                    .text_size(px(t.font_size_sm as f32))
                                    .text_color(t.color_text_tertiary)
                                    .child("Right-click · Shift+F10"),
                            ),
                    ),
            )
            .child(
                div()
                    .text_size(px(t.font_size_sm as f32))
                    .text_color(t.color_text_secondary)
                    .child(self.last_action.clone()),
            )
            .into_any_element()
    }
}

impl Render for ContextMenuPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.context-menu.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "context-menu",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
