// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native GPU captures of menus and scrolling surfaces, without an interactive window.

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, ContextMenu, ContextMenuState, Direction, MenuEntry, MenuItem, MenuItems,
    ScrollArea, ScrollAreaState, ScrollAxis, UiRoot, icons::IconName, theme::AlgorithmChain,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, Keystroke, PlatformInput,
    Render, Window, WindowBounds, WindowOptions, div, point, prelude::*, px, size,
};

struct Capture {
    scope: ConfigScope,
    menu: Entity<ContextMenuState>,
    scroll: Entity<ScrollAreaState>,
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let t = &config.theme.tokens;
        div()
            .size_full()
            .p(px(32.))
            .flex()
            .flex_col()
            .gap(px(24.))
            .child(div().text_size(px(24.)).child("Context Menu · Scroll Area"))
            .child(
                div()
                    .text_color(t.color_text_secondary)
                    .child("Native GPUI surfaces · Ant Design tokens · Retained interaction state"),
            )
            .child(
                div()
                    .flex()
                    .gap(px(32.))
                    .items_start()
                    .child(
                        ScrollArea::new("scroll", &self.scroll)
                            .scope(self.scope.clone())
                            .axis(ScrollAxis::Both)
                            .accessibility_label("Documents")
                            .w(px(400.))
                            .h(px(340.))
                            .border_1()
                            .border_color(t.color_border_secondary)
                            .rounded(px(t.border_radius_lg as f32))
                            .bg(t.color_bg_container)
                            .child(div().w(px(700.)).flex().flex_col().children((0..60).map(
                                |index| {
                                    div()
                                        .flex_shrink_0()
                                        .h(px(36.))
                                        .flex()
                                        .items_center()
                                        .px(px(16.))
                                        .bg(if index % 2 == 0 {
                                            t.color_fill_quaternary
                                        } else {
                                            t.color_bg_container
                                        })
                                        .child(
                                            div()
                                                .w(px(260.))
                                                .child(format!("Design document {index:02}")),
                                        )
                                        .child(
                                            div()
                                                .w(px(160.))
                                                .text_color(t.color_text_secondary)
                                                .child("Design team"),
                                        )
                                        .child(
                                            div()
                                                .w(px(180.))
                                                .text_color(t.color_text_secondary)
                                                .child("Ready for review"),
                                        )
                                },
                            ))),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(12.))
                            .child(
                                ContextMenu::new("menu", &self.menu)
                                    .scope(self.scope.clone())
                                    .accessibility_label("Document actions")
                                    .child(
                                        Button::new("trigger")
                                            .scope(self.scope.clone())
                                            .label("Document actions"),
                                    ),
                            )
                            .child(
                                div()
                                    .text_color(t.color_text_tertiary)
                                    .child("Right-click or Shift+F10"),
                            ),
                    ),
            )
    }
}

fn items(entries: impl IntoIterator<Item = MenuEntry>) -> MenuItems {
    MenuItems::new(entries).expect("valid visual fixture")
}

fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(
        args.next().as_deref(),
        Some("--output"),
        "Usage: context_scroll_visual --output <directory>"
    );
    let output = PathBuf::from(args.next().expect("output directory"));
    assert!(args.next().is_none(), "unexpected argument");
    std::fs::create_dir_all(&output).expect("create capture directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(880.), px(560.)),
                    cx,
                ))),
                show: false,
                focus: false,
                ..Default::default()
            },
            |window, cx| {
                window.resize(size(px(880.), px(560.)));
                let menu_items = items([
                    MenuEntry::group("Document"),
                    MenuItem::new("share", "Share with")
                        .icon(IconName::ShareAltOutlined)
                        .submenu(items([
                            MenuItem::new("team", "Design team").badge("8").into(),
                            MenuItem::new("link", "Copy link")
                                .icon(IconName::LinkOutlined)
                                .shortcut("Ctrl+L")
                                .into(),
                            MenuItem::new("invite", "Invite collaborators").into(),
                        ]))
                        .into(),
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
                    MenuEntry::Separator,
                    MenuItem::new("hidden", "Show hidden layers")
                        .checked(true)
                        .into(),
                    MenuItem::new("list", "List view")
                        .radio("view", true)
                        .into(),
                    MenuItem::new("grid", "Grid view")
                        .radio("view", false)
                        .into(),
                    MenuEntry::Separator,
                    MenuItem::new("delete", "Delete permanently")
                        .icon(IconName::DeleteOutlined)
                        .danger(true)
                        .into(),
                ]);
                let menu = cx.new(|cx| ContextMenuState::new(menu_items.clone(), window, cx));
                let long_menu_items = items((0_usize..1000).map(|index| {
                    MenuItem::new(("document", index), format!("Document {index:04}")).into()
                }));
                let scroll = cx.new(|cx| ScrollAreaState::new(window, cx));
                let capture = cx.new(|_| Capture {
                    scope: scope.clone(),
                    menu: menu.clone(),
                    scroll: scroll.clone(),
                });
                let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
                window
                    .spawn(cx, async move |cx| {
                        cx.background_executor()
                            .timer(Duration::from_millis(150))
                            .await;
                        for (name, algorithm) in [
                            ("light", AlgorithmChain::default()),
                            ("dark", AlgorithmChain::dark()),
                            ("compact", AlgorithmChain::compact()),
                            ("dark-compact", AlgorithmChain::dark_compact()),
                        ] {
                            for rtl in [false, true] {
                                cx.update(|window, cx| {
                                    scope
                                        .update_patch(cx, |patch| {
                                            patch.direction = Some(if rtl {
                                                Direction::Rtl
                                            } else {
                                                Direction::Ltr
                                            });
                                            patch.theme.get_or_insert_default().algorithms =
                                                Some(algorithm.clone());
                                        })
                                        .unwrap();
                                    capture.update(cx, |_, cx| cx.notify());
                                    window.refresh();
                                    window.draw(cx).clear(cx);
                                    scroll.update(cx, |state, cx| {
                                        state.scroll_to(point(px(0.), px(-180.)), cx)
                                    });
                                })
                                .unwrap();
                                for case in ["root", "submenu", "edge", "long"] {
                                    cx.update(|window, cx| {
                                        menu.update(cx, |state, cx| state.close(window, cx));
                                    })
                                    .unwrap();
                                    cx.background_executor()
                                        .timer(Duration::from_millis(20))
                                        .await;
                                    cx.update(|window, cx| {
                                        let position = if case == "edge" {
                                            point(px(870.), px(550.))
                                        } else {
                                            point(px(490.), px(186.))
                                        };
                                        menu.update(cx, |state, cx| {
                                            state.set_items(
                                                if case == "long" {
                                                    long_menu_items.clone()
                                                } else {
                                                    menu_items.clone()
                                                },
                                                cx,
                                            );
                                            state.open_at(position, window, cx)
                                        });
                                    })
                                    .unwrap();
                                    cx.background_executor()
                                        .timer(Duration::from_millis(30))
                                        .await;
                                    if case == "submenu" || case == "long" {
                                        cx.update(|window, cx| {
                                            window.dispatch_event(
                                                PlatformInput::KeyDown(KeyDownEvent {
                                                    keystroke: Keystroke::parse(
                                                        if case == "long" {
                                                            "end"
                                                        } else if rtl {
                                                            "left"
                                                        } else {
                                                            "right"
                                                        },
                                                    )
                                                    .unwrap(),
                                                    is_held: false,
                                                    prefer_character_input: false,
                                                }),
                                                cx,
                                            );
                                        })
                                        .unwrap();
                                    }
                                    cx.background_executor()
                                        .timer(Duration::from_millis(30))
                                        .await;
                                    let file = format!(
                                        "{name}-{}-{case}.png",
                                        if rtl { "rtl" } else { "ltr" }
                                    );
                                    cx.update(|window, cx| {
                                        window.refresh();
                                        window.draw(cx).clear(cx);
                                        assert!(
                                            menu.read(cx).is_open(),
                                            "menu is present in capture"
                                        );
                                        if case == "submenu" {
                                            assert!(
                                                menu.read(cx).submenu_depth() > 0,
                                                "submenu key opened a branch"
                                            );
                                        }
                                        if case == "long" {
                                            assert_eq!(
                                                menu.read(cx)
                                                    .active_item()
                                                    .map(|item| item.label.as_ref()),
                                                Some("Document 0999")
                                            );
                                        }
                                        let image =
                                            window.render_to_image().expect("native GPU frame");
                                        assert!(
                                            image.width() > 1 && image.height() > 1,
                                            "native drawable has a size"
                                        );
                                        image.save(output.join(&file)).expect("save capture");
                                    })
                                    .unwrap();
                                    println!("{}", output.join(file).display());
                                }
                            }
                        }
                        cx.update(|_, cx| cx.quit()).unwrap();
                    })
                    .detach();
                root
            },
        )
        .expect("native capture window");
    });
}
