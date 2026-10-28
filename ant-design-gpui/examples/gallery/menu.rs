// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::collections::BTreeMap;

use ant_design_gpui::{
    Button, ConfigScope, Divider, Switch,
    icons::IconName,
    menu::{
        Menu, MenuColorTheme, MenuDivider, MenuEntry, MenuEvent, MenuGroup, MenuItem, MenuItems,
        MenuMode, MenuState, MenuStyles, SubMenu,
    },
    popup::PopupPlacement,
    switch::SwitchChange,
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FontWeight, IntoElement, Render, ScrollAnchor,
    SharedString, StyleRefinement, Subscription, Window, div, prelude::*, px,
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

// Public examples follow components/menu/index.en-US.md in the pinned checkout.
pub(super) const DEMOS: &[Demo] = &[
    demo(
        "horizontal",
        "Top Navigation",
        "Horizontal navigation supports icons, disabled items, grouped submenus and links. Resize the window to reveal the overflow menu.",
    ),
    demo(
        "inline",
        "Inline menu",
        "Nested navigation expands in place. Groups, dividers and selected items retain their identity.",
    ),
    demo(
        "inline-collapsed",
        "Collapsed inline menu",
        "Collapse the inline menu into an icon rail. Expanding restores its open branches.",
    ),
    demo(
        "tooltip",
        "Menu tooltip",
        "Configure the tooltip in inline collapsed mode, or disable it.",
    ),
    demo(
        "sider-current",
        "Open current submenu only",
        "Opening a branch closes other branches at the same level and their descendants.",
    ),
    demo(
        "vertical",
        "Vertical menu",
        "Vertical navigation opens submenus beside their parent. Arrow keys navigate between levels.",
    ),
    demo(
        "theme",
        "Menu Themes",
        "Switch between the menu's light and dark palettes while preserving the current selection.",
    ),
    demo(
        "submenu-theme",
        "Sub-menu theme",
        "Use an independent submenu palette. The root is dark and the submenu starts light.",
    ),
    demo(
        "switch-mode",
        "Switch the menu type",
        "Switch between inline and vertical navigation and change the palette without losing selection.",
    ),
    demo(
        "style-class",
        "Custom semantic dom styling",
        "Native GPUI refinements customize the root, items and submenu surfaces independently.",
    ),
    demo(
        "custom-popup-render",
        "Custom Submenu Render",
        "A custom submenu surface combines a heading with rich navigation content while retaining keyboard and selection behavior.",
    ),
];

fn items(entries: impl IntoIterator<Item = MenuEntry>) -> MenuItems {
    MenuItems::new(entries).expect("unique gallery Menu keys")
}

fn options(first: usize, last: usize) -> MenuItems {
    items(
        (first..=last)
            .map(|index| MenuItem::new(index.to_string(), format!("Option {index}")).into()),
    )
}

fn grouped_options(prefix: &'static str) -> MenuItems {
    items([
        MenuGroup::new(format!("{prefix}-g1"), "Item 1", options(1, 2)).into(),
        MenuGroup::new(format!("{prefix}-g2"), "Item 2", options(3, 4)).into(),
    ])
}

fn navigation(with_groups: bool, with_divider: bool) -> MenuItems {
    let mut entries = vec![
        SubMenu::new(
            "sub1",
            "Navigation One",
            if with_groups {
                grouped_options("inline")
            } else {
                options(1, 4)
            },
        )
        .icon(IconName::MailOutlined)
        .into(),
        SubMenu::new(
            "sub2",
            "Navigation Two",
            items([
                MenuItem::new("5", "Option 5").into(),
                MenuItem::new("6", "Option 6").into(),
                SubMenu::new("sub3", "Submenu", options(7, 8)).into(),
            ]),
        )
        .icon(IconName::AppstoreOutlined)
        .into(),
    ];
    if with_divider {
        entries.push(MenuDivider::new("divider").into());
    }
    entries.push(
        SubMenu::new("sub4", "Navigation Three", options(9, 12))
            .icon(IconName::SettingOutlined)
            .into(),
    );
    if with_divider {
        entries.push(MenuGroup::new("grp", "Group", options(13, 14)).into());
    }
    items(entries)
}

fn horizontal_items() -> MenuItems {
    let grouped = items([
        MenuGroup::new(
            "g1",
            "Item 1",
            items([
                MenuItem::new("setting:1", "Option 1").into(),
                MenuItem::new("setting:2", "Option 2").into(),
            ]),
        )
        .into(),
        MenuGroup::new(
            "g2",
            "Item 2",
            items([
                MenuItem::new("setting:3", "Option 3").into(),
                MenuItem::new("setting:4", "Option 4").into(),
            ]),
        )
        .into(),
    ]);
    items([
        MenuItem::new("mail", "Navigation One")
            .icon(IconName::MailOutlined)
            .into(),
        MenuItem::new("app", "Navigation Two")
            .icon(IconName::AppstoreOutlined)
            .disabled(true)
            .into(),
        SubMenu::new("SubMenu", "Navigation Three - Submenu", grouped)
            .icon(IconName::SettingOutlined)
            .into(),
        MenuItem::new("alipay", "Navigation Four - Link").into(),
    ])
}

fn collapsed_items() -> MenuItems {
    items([
        MenuItem::new("1", "Option 1")
            .icon(IconName::PieChartOutlined)
            .into(),
        MenuItem::new("2", "Option 2")
            .icon(IconName::DesktopOutlined)
            .into(),
        MenuItem::new("3", "Option 3")
            .icon(IconName::ContainerOutlined)
            .into(),
        SubMenu::new("sub1", "Navigation One", options(5, 8))
            .icon(IconName::MailOutlined)
            .into(),
        SubMenu::new(
            "sub2",
            "Navigation Two",
            items([
                MenuItem::new("9", "Option 9").into(),
                MenuItem::new("10", "Option 10").into(),
                SubMenu::new("sub3", "Submenu", options(11, 12)).into(),
            ]),
        )
        .icon(IconName::AppstoreOutlined)
        .into(),
    ])
}

fn accordion_items() -> MenuItems {
    let option = |key: &'static str, label: &'static str| MenuItem::new(key, label).into();
    items([
        SubMenu::new(
            "1",
            "Navigation One",
            items([
                option("11", "Option 1"),
                option("12", "Option 2"),
                option("13", "Option 3"),
                option("14", "Option 4"),
            ]),
        )
        .icon(IconName::MailOutlined)
        .into(),
        SubMenu::new(
            "2",
            "Navigation Two",
            items([
                option("21", "Option 1"),
                option("22", "Option 2"),
                SubMenu::new(
                    "23",
                    "Submenu",
                    items([
                        option("231", "Option 1"),
                        option("232", "Option 2"),
                        option("233", "Option 3"),
                    ]),
                )
                .into(),
                SubMenu::new(
                    "24",
                    "Submenu 2",
                    items([
                        option("241", "Option 1"),
                        option("242", "Option 2"),
                        option("243", "Option 3"),
                    ]),
                )
                .into(),
            ]),
        )
        .icon(IconName::AppstoreOutlined)
        .into(),
        SubMenu::new(
            "3",
            "Navigation Three",
            items([
                option("31", "Option 1"),
                option("32", "Option 2"),
                option("33", "Option 3"),
                option("34", "Option 4"),
            ]),
        )
        .icon(IconName::SettingOutlined)
        .into(),
    ])
}

fn submenu_theme_items(dark: bool) -> MenuItems {
    items([
        SubMenu::new("sub1", "Navigation One", options(1, 3))
            .icon(IconName::MailOutlined)
            .theme(if dark {
                MenuColorTheme::Dark
            } else {
                MenuColorTheme::Light
            })
            .into(),
        MenuItem::new("5", "Option 5").into(),
        MenuItem::new("6", "Option 6").into(),
    ])
}

fn switch_items() -> MenuItems {
    items([
        MenuItem::new("1", "Navigation One")
            .icon(IconName::MailOutlined)
            .into(),
        MenuItem::new("2", "Navigation Two")
            .icon(IconName::CalendarOutlined)
            .into(),
        SubMenu::new(
            "sub1",
            "Navigation Two",
            items([
                MenuItem::new("3", "Option 3").into(),
                MenuItem::new("4", "Option 4").into(),
                SubMenu::new("sub1-2", "Submenu", options(5, 6)).into(),
            ]),
        )
        .icon(IconName::AppstoreOutlined)
        .into(),
        SubMenu::new("sub2", "Navigation Three", options(7, 10))
            .icon(IconName::SettingOutlined)
            .into(),
        MenuItem::new("link", "Ant Design")
            .icon(IconName::LinkOutlined)
            .into(),
    ])
}

fn semantic_items() -> MenuItems {
    items([
        SubMenu::new(
            "SubMenu",
            "Navigation One",
            items([MenuGroup::new("g1", "Item 1", options(1, 2)).into()]),
        )
        .into(),
        MenuItem::new("mail", "Navigation Two").into(),
    ])
}

fn rich_item(
    scope: ConfigScope,
    key: &'static str,
    title: &'static str,
    description: &'static str,
) -> MenuEntry {
    MenuItem::new(key, title)
        .style(StyleRefinement::default().h_auto())
        .content(move |_, _, cx| {
            let config = scope.snapshot(cx);
            let tokens = &config.theme.tokens;
            div()
                .flex()
                .flex_col()
                .gap(px(4.))
                .py(px(12.))
                .child(
                    div()
                        .text_size(px(16.))
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(title),
                )
                .child(
                    div()
                        .text_size(px(tokens.font_size as f32))
                        .text_color(tokens.color_text_secondary)
                        .whitespace_normal()
                        .child(description),
                )
        })
        .into()
}

fn custom_items(scope: &ConfigScope) -> MenuItems {
    items([
        MenuItem::new("home", "Home").into(),
        SubMenu::new(
            "features",
            "Features",
            items([
                rich_item(
                    scope.clone(),
                    "getting-started",
                    "Getting Started",
                    "Quick start guide and learn the basics.",
                ),
                rich_item(
                    scope.clone(),
                    "components",
                    "Components",
                    "Explore our component library.",
                ),
                rich_item(
                    scope.clone(),
                    "templates",
                    "Templates",
                    "Ready-to-use template designs.",
                ),
            ]),
        )
        .into(),
        SubMenu::new(
            "resources",
            "Resources",
            items([
                rich_item(
                    scope.clone(),
                    "blog",
                    "Blog",
                    "Latest updates and articles.",
                ),
                rich_item(
                    scope.clone(),
                    "community",
                    "Community",
                    "Join our developer community.",
                ),
            ]),
        )
        .into(),
    ])
}

pub(super) struct MenuPage {
    scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub states: BTreeMap<&'static str, Entity<MenuState>>,
    pub collapsed: bool,
    pub tooltip_collapsed: bool,
    pub tooltip_enabled: bool,
    pub theme_dark: bool,
    pub submenu_dark: bool,
    pub switch_vertical: bool,
    pub switch_dark: bool,
    pub accordion_open: Vec<SharedString>,
    _config: Subscription,
}

impl MenuPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let models = [
            ("horizontal", horizontal_items()),
            ("inline", navigation(true, true)),
            ("inline-collapsed", collapsed_items()),
            ("tooltip", collapsed_items()),
            ("sider-current", accordion_items()),
            ("vertical", navigation(true, false)),
            ("theme", navigation(false, false)),
            ("submenu-theme", submenu_theme_items(false)),
            ("switch-mode", switch_items()),
            ("style-object", semantic_items()),
            ("style-function", semantic_items()),
            ("custom-popup-render", custom_items(&scope)),
        ];
        Self {
            states: models
                .into_iter()
                .map(|(id, items)| (id, cx.new(|cx| MenuState::new(items, window, cx))))
                .collect(),
            anchors,
            collapsed: false,
            tooltip_collapsed: false,
            tooltip_enabled: true,
            theme_dark: true,
            submenu_dark: false,
            switch_vertical: false,
            switch_dark: false,
            accordion_open: vec!["2".into(), "23".into()],
            _config: scope.observe(cx),
            scope,
        }
    }

    fn menu(&self, id: &'static str, cx: &Context<Self>) -> Menu {
        Menu::new(format!("menu.{id}"), &self.states[id])
            .scope(self.scope.clone())
            .automation_id(format!("menu.{id}"))
            .on_event(cx.listener(move |this, event, _, cx| {
                if let MenuEvent::Activate(activation) = event
                    && (activation.key == "alipay" || activation.key == "link")
                {
                    cx.open_url("https://ant.design");
                }
                if id == "sider-current"
                    && let MenuEvent::OpenChange { open_keys, .. } = event
                {
                    let new_key = open_keys
                        .iter()
                        .find(|key| !this.accordion_open.contains(key));
                    this.accordion_open = if let Some(new_key) = new_key {
                        let level = if new_key == "23" || new_key == "24" {
                            2
                        } else {
                            1
                        };
                        open_keys
                            .iter()
                            .filter(|key| {
                                let candidate_level =
                                    if *key == "23" || *key == "24" { 2 } else { 1 };
                                candidate_level < level || *key == new_key
                            })
                            .cloned()
                            .collect()
                    } else {
                        open_keys.clone()
                    };
                    cx.notify();
                }
            }))
    }

    fn theme_switch(&self, id: &'static str, checked: bool, cx: &Context<Self>) -> AnyElement {
        examples::control(
            id,
            Switch::new(id)
                .scope(self.scope.clone())
                .checked(checked)
                .checked_label("Dark")
                .unchecked_label("Light")
                .accessibility_label("Menu color theme")
                .on_change(cx.listener(move |this, event: &SwitchChange, _, cx| {
                    match id {
                        "menu.theme.toggle" => this.theme_dark = event.checked,
                        "menu.submenu-theme.toggle" => {
                            this.submenu_dark = event.checked;
                            this.states["submenu-theme"].update(cx, |state, cx| {
                                state.set_items(submenu_theme_items(event.checked), cx)
                            });
                        }
                        _ => this.switch_dark = event.checked,
                    }
                    cx.notify();
                })),
        )
    }

    fn collapse_button(&self, tooltip: bool, cx: &Context<Self>) -> AnyElement {
        let collapsed = if tooltip {
            self.tooltip_collapsed
        } else {
            self.collapsed
        };
        let id = if tooltip {
            "menu.tooltip.collapse"
        } else {
            "menu.inline-collapsed.collapse"
        };
        examples::control(
            id,
            Button::new(id)
                .scope(self.scope.clone())
                .primary()
                .icon(if collapsed {
                    IconName::MenuUnfoldOutlined
                } else {
                    IconName::MenuFoldOutlined
                })
                .accessibility_label(if collapsed {
                    "Expand menu"
                } else {
                    "Collapse menu"
                })
                .on_click(cx.listener(move |this, _, _, cx| {
                    if tooltip {
                        this.tooltip_collapsed = !this.tooltip_collapsed;
                    } else {
                        this.collapsed = !this.collapsed;
                    }
                    cx.notify();
                })),
        )
    }

    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        let color_theme = |dark| {
            if dark {
                MenuColorTheme::Dark
            } else {
                MenuColorTheme::Light
            }
        };
        let column = || examples::column().items_start();
        let control = match id {
            "horizontal" => self
                .menu(id, cx)
                .mode(MenuMode::Horizontal)
                .default_selected_keys(["mail"])
                .w_full()
                .into_any_element(),
            "inline" => self
                .menu(id, cx)
                .mode(MenuMode::Inline)
                .default_selected_keys(["1"])
                .default_open_keys(["sub1"])
                .w(px(256.))
                .into_any_element(),
            "inline-collapsed" => column()
                .child(self.collapse_button(false, cx))
                .child(
                    self.menu(id, cx)
                        .mode(MenuMode::Inline)
                        .color_theme(MenuColorTheme::Dark)
                        .inline_collapsed(self.collapsed)
                        .default_selected_keys(["1"])
                        .default_open_keys(["sub1"])
                        .when(!self.collapsed, |menu| menu.w(px(256.))),
                )
                .into_any_element(),
            "tooltip" => column()
                .child(
                    examples::row(&self.scope, cx)
                        .child(self.collapse_button(true, cx))
                        .child(examples::control(
                            "menu.tooltip.enabled",
                            Switch::new("menu.tooltip.enabled")
                                .scope(self.scope.clone())
                                .checked(self.tooltip_enabled)
                                .checked_label("Tooltip On")
                                .unchecked_label("Tooltip Off")
                                .accessibility_label("Collapsed menu tooltips")
                                .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                    this.tooltip_enabled = event.checked;
                                    cx.notify();
                                })),
                        )),
                )
                .child(
                    self.menu(id, cx)
                        .mode(MenuMode::Inline)
                        .color_theme(MenuColorTheme::Dark)
                        .inline_collapsed(self.tooltip_collapsed)
                        .tooltip(self.tooltip_enabled)
                        .tooltip_placement(PopupPlacement::Left)
                        .default_selected_keys(["1"])
                        .default_open_keys(["sub1"])
                        .when(!self.tooltip_collapsed, |menu| menu.w(px(256.))),
                )
                .into_any_element(),
            "sider-current" => self
                .menu(id, cx)
                .mode(MenuMode::Inline)
                .default_selected_keys(["231"])
                .open_keys(self.accordion_open.clone())
                .w(px(256.))
                .into_any_element(),
            "vertical" => self
                .menu(id, cx)
                .mode(MenuMode::Vertical)
                .w(px(256.))
                .into_any_element(),
            "theme" => column()
                .child(self.theme_switch("menu.theme.toggle", self.theme_dark, cx))
                .child(
                    self.menu(id, cx)
                        .mode(MenuMode::Inline)
                        .color_theme(color_theme(self.theme_dark))
                        .default_selected_keys(["1"])
                        .default_open_keys(["sub1"])
                        .w(px(256.)),
                )
                .into_any_element(),
            "submenu-theme" => column()
                .child(self.theme_switch("menu.submenu-theme.toggle", self.submenu_dark, cx))
                .child(
                    self.menu(id, cx)
                        .mode(MenuMode::Vertical)
                        .color_theme(MenuColorTheme::Dark)
                        .default_selected_keys(["1"])
                        .open_keys(["sub1"])
                        .w(px(256.)),
                )
                .into_any_element(),
            "switch-mode" => column()
                .child(
                    examples::row(&self.scope, cx)
                        .child(examples::control(
                            "menu.switch-mode.mode",
                            Switch::new("menu.switch-mode.mode")
                                .scope(self.scope.clone())
                                .checked(self.switch_vertical)
                                .accessibility_label("Vertical menu mode")
                                .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                    this.switch_vertical = event.checked;
                                    cx.notify();
                                })),
                        ))
                        .child("Change Mode")
                        .child(
                            Divider::new("menu.switch-mode.divider")
                                .scope(self.scope.clone())
                                .vertical(true),
                        )
                        .child(examples::control(
                            "menu.switch-mode.theme",
                            Switch::new("menu.switch-mode.theme")
                                .scope(self.scope.clone())
                                .checked(self.switch_dark)
                                .accessibility_label("Dark menu style")
                                .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                    this.switch_dark = event.checked;
                                    cx.notify();
                                })),
                        ))
                        .child("Change Style"),
                )
                .child(
                    self.menu(id, cx)
                        .mode(if self.switch_vertical {
                            MenuMode::Vertical
                        } else {
                            MenuMode::Inline
                        })
                        .color_theme(color_theme(self.switch_dark))
                        .default_selected_keys(["1"])
                        .default_open_keys(["sub1"])
                        .w(px(256.)),
                )
                .into_any_element(),
            "style-class" => column()
                .child(
                    self.menu("style-object", cx).w_full().styles(MenuStyles {
                        root: StyleRefinement::default()
                            .border_1()
                            .border_color(Color::rgb(0xf0f0f0))
                            .p(px(8.))
                            .rounded(px(4.)),
                        item: StyleRefinement::default().text_color(Color::rgb(0x1677ff)),
                        submenu_list: StyleRefinement::default().text_color(Color::rgb(0xfa541c)),
                        ..Default::default()
                    }),
                )
                .child(
                    self.menu("style-function", cx)
                        .mode(MenuMode::Inline)
                        .w_full()
                        .styles_fn(|info, _, _| MenuStyles {
                            root: StyleRefinement::default()
                                .w_full()
                                .border_1()
                                .border_color(Color::rgb(0xf0f0f0))
                                .p(px(8.))
                                .rounded(px(4.))
                                .bg(if info.mode == MenuMode::Inline {
                                    Color::rgba(0xf0f9ff99)
                                } else {
                                    Color::rgb(0xffffff)
                                }),
                            item: StyleRefinement::default().text_color(Color::rgb(0x1677ff)),
                            ..Default::default()
                        }),
                )
                .into_any_element(),
            "custom-popup-render" => {
                let scope = self.scope.clone();
                self.menu(id, cx)
                    .mode(MenuMode::Horizontal)
                    .w_full()
                    .styles(MenuStyles {
                        submenu_list: StyleRefinement::default()
                            .w_full()
                            .grid()
                            .grid_cols(2)
                            .gap(px(16.)),
                        ..Default::default()
                    })
                    .popup_render(move |content, context, _, cx| {
                        let config = scope.snapshot(cx);
                        let tokens = &config.theme.tokens;
                        div()
                            .w(px(480.))
                            .max_w_full()
                            .p(px(tokens.padding as f32))
                            .flex()
                            .flex_col()
                            .gap(px(16.))
                            .child(
                                div()
                                    .pb(px(8.))
                                    .border_b_1()
                                    .border_color(tokens.color_split)
                                    .text_size(px(24.))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(if context.key == "features" {
                                        "Features"
                                    } else {
                                        "Resources"
                                    }),
                            )
                            .child(content)
                            .into_any_element()
                    })
                    .into_any_element()
            }
            _ => unreachable!("registered Menu demo"),
        };
        div()
            .id(format!("menu.preview.{id}"))
            .debug_selector(move || format!("gallery.menu.preview.{id}"))
            .min_w_0()
            .w_full()
            .child(control)
            .into_any_element()
    }
}

impl Render for MenuPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.menu.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "menu",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
