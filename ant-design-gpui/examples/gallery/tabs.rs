// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::input_examples::{self as examples, Coverage, Demo, column, row};
use ant_design_gpui::{
    Button, Checkbox, ConfigPatch, ConfigScope, Input, InputState, ScrollArea, ScrollAreaState,
    Segmented, SegmentedOption, Size, Switch, TabItem, TabPlacement, Tabs, TabsItems, TabsState,
    TabsType,
    icons::IconName,
    popup::PopupTriggers,
    tabs::{
        TabsAnimation, TabsEdit, TabsIndicator, TabsIndicatorAlign, TabsSpacing, TabsStyles,
        TabsTheme, TabsTokenPatch,
    },
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, Focusable, IntoElement, Render, ScrollAnchor,
    SharedString, Subscription, Window, div, prelude::*, px,
};
use std::collections::BTreeMap;

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}
pub(super) const DEMOS: &[Demo] = &[
    demo("basic", "Basic", "Default activate first tab."),
    demo("disabled", "Disabled", "Disabled a tab."),
    demo("centered", "Centered", "Centered tabs."),
    demo("icon", "Icon", "The Tab with Icon."),
    demo(
        "custom-indicator",
        "Indicator",
        "Customize the size and alignment of the indicator.",
    ),
    demo(
        "slide",
        "Slide",
        "In order to fit more tabs, they can slide left and right (or up and down).",
    ),
    demo(
        "extra",
        "Extra content",
        "You can add extra actions to the right or left or even both sides of Tabs.",
    ),
    demo("size", "Size", "Large, medium and small size."),
    demo(
        "placement",
        "Placement",
        "Tabs can be placed at the top, bottom, start or end.",
    ),
    demo(
        "popupRender-Search",
        "Custom Popup Search",
        "Search tabs in a customized overflow popup.",
    ),
    demo(
        "card",
        "Card type tab",
        "Another type of Tabs, which does not support vertical mode.",
    ),
    demo(
        "editable-card",
        "Add & close tab",
        "Only card type Tabs support adding and closing tabs.",
    ),
    demo(
        "custom-add-trigger",
        "Customized trigger of new tab",
        "Hide the default add button and use a custom trigger.",
    ),
    demo(
        "custom-tab-bar",
        "Customized bar of tab",
        "A sticky tab bar inside a native scroll viewport, with a 64px top offset and 20px bottom clearance.",
    ),
    demo(
        "custom-tab-bar-node",
        "Draggable Tabs",
        "Drag a tab onto another tab to reorder it.",
    ),
    demo(
        "style-class",
        "Custom semantic dom styling",
        "Customize the styles of semantic parts.",
    ),
    demo(
        "animated",
        "Debug: animated panes",
        "Toggle indicator and pane animation independently.",
    ),
    demo(
        "nest",
        "Debug: nested tabs",
        "Nested native tab groups retain independent selection and keyboard focus.",
    ),
    demo(
        "card-top",
        "Debug: card container",
        "Card tabs above a padded content surface.",
    ),
    demo(
        "component-token",
        "Debug: component tokens",
        "Scoped tokens customize spacing, typography and colors without affecting other examples.",
    ),
];

pub(super) struct TabsPage {
    scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub states: BTreeMap<&'static str, Entity<TabsState>>,
    pub(super) search: Entity<InputState>,
    next_id: usize,
    size: Size,
    placement: TabPlacement,
    search_nav: Entity<SearchNavigation>,
    slide_vertical: bool,
    align: TabsIndicatorAlign,
    extra_left: bool,
    extra_right: bool,
    animation: TabsAnimation,
    pub(super) sticky_scroll: Entity<ScrollAreaState>,
    token_scope: ConfigScope,
    nested: Entity<NestedTabs>,
    _subscriptions: Vec<Subscription>,
}
impl TabsPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut states: BTreeMap<_, _> = DEMOS
            .iter()
            .map(|demo| {
                let count = if matches!(demo.id, "slide" | "popupRender-Search") {
                    30
                } else {
                    3
                };
                let items = TabsItems::new((0..count).map(|index| {
                    let ordinal = if count == 30 { index } else { index + 1 };
                    let mut item = TabItem::new(
                        ordinal.to_string(),
                        if count == 30 {
                            format!("Tab-{ordinal}")
                        } else {
                            format!("Tab {ordinal}")
                        },
                    )
                    .text(format!("Content of Tab Pane {ordinal}"));
                    if (demo.id == "disabled" && index == 1) || (count == 30 && index == 28) {
                        item = item.disabled(true);
                    }
                    if demo.id == "icon" {
                        item = item
                            .icon(
                                [
                                    IconName::AppleOutlined,
                                    IconName::AndroidOutlined,
                                    IconName::HeartFilled,
                                ][index],
                            )
                            .text(format!("Tab {ordinal}"));
                    }
                    if demo.id == "editable-card" && index == 2 {
                        item = item.closable(false);
                    }
                    item
                }))
                .unwrap();
                (demo.id, cx.new(|cx| TabsState::new(items, window, cx)))
            })
            .collect();
        for id in [
            "extra-default",
            "size-card",
            "size-editable",
            "style-card",
            "token-small",
            "token-large",
            "token-vertical",
            "token-card",
            "token-card-small",
            "token-card-large",
        ] {
            let items = TabsItems::new((1..=3).map(|i| {
                TabItem::new(
                    i.to_string(),
                    if id == "size-card" {
                        format!("Card Tab {i}")
                    } else {
                        format!("Tab {i}")
                    },
                )
                .text(if id == "size-editable" {
                    format!("Content of editable tab {i}")
                } else {
                    format!("Content of Tab Pane {i}")
                })
            }))
            .unwrap();
            states.insert(id, cx.new(|cx| TabsState::new(items, window, cx)));
        }
        let animated = TabsItems::new(
            [
                ("Bamboo", "Hello Bamboo!", 200., 0xff0000),
                ("Little", "Hi Little!", 300., 0x00ff00),
                ("Light", "Welcome Light!", 100., 0x0000ff),
            ]
            .into_iter()
            .enumerate()
            .map(|(i, (label, text, height, color))| {
                TabItem::new((i + 1).to_string(), label).content(move |_, _| {
                    div()
                        .h(px(height))
                        .border_1()
                        .border_color(gpui::rgb(color))
                        .child(text)
                        .into_any_element()
                })
            }),
        )
        .unwrap();
        states["animated"].update(cx, |s, cx| s.set_items(animated, window, cx));
        let sticky_scroll = cx.new(|cx| ScrollAreaState::new(window, cx));
        let nested = cx.new(|cx| NestedTabs::new(scope.clone(), window, cx));
        let mut theme = ThemeConfig::default();
        theme.components.set::<TabsTheme>(TabsTokenPatch {
            card_bg: Some(Color::rgb(0xf6ffed)),
            card_height: Some(60.),
            card_padding: Some(TabsSpacing::symmetric(20., 20.)),
            card_padding_sm: Some(TabsSpacing::symmetric(20., 20.)),
            card_padding_lg: Some(TabsSpacing::symmetric(20., 20.)),
            title_font_size: Some(20.),
            title_font_size_sm: Some(20.),
            title_font_size_lg: Some(20.),
            ink_bar_color: Some(Color::rgb(0x52c41a)),
            horizontal_margin: Some(TabsSpacing::new(0., 0., 12., 0.)),
            horizontal_item_gutter: Some(12.),
            horizontal_item_padding: Some(TabsSpacing::symmetric(20., 20.)),
            horizontal_item_padding_sm: Some(TabsSpacing::symmetric(20., 20.)),
            horizontal_item_padding_lg: Some(TabsSpacing::symmetric(20., 20.)),
            vertical_item_padding: Some(TabsSpacing::symmetric(8., 8.)),
            vertical_item_margin: Some(TabsSpacing::new(4., 0., 0., 0.)),
            item_color: Some(Color {
                r: 0,
                g: 0,
                b: 0,
                a: 0.85,
            }),
            item_selected_color: Some(Color::rgb(0x389e0d)),
            item_hover_color: Some(Color::rgb(0xd9f7be)),
            item_active_color: Some(Color::rgb(0xb7eb8f)),
            card_gutter: Some(12.),
            ..Default::default()
        });
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        let search = cx.new(|cx| InputState::new(window, cx));
        let search_nav = cx.new(|cx| SearchNavigation {
            focus: cx.focus_handle(),
            active: None,
            scroll: gpui::UniformListScrollHandle::default(),
        });
        let subscriptions = vec![
            scope.observe(cx),
            cx.subscribe(&search, |_, _, _, cx| cx.notify()),
            cx.observe(&search_nav, |_, _, cx| cx.notify()),
        ];
        Self {
            scope,
            anchors,
            states,
            search,
            next_id: 0,
            size: Size::Small,
            placement: TabPlacement::Start,
            slide_vertical: false,
            align: TabsIndicatorAlign::Center,
            extra_left: true,
            extra_right: true,
            animation: TabsAnimation {
                indicator: true,
                panes: true,
            },
            sticky_scroll,
            token_scope,
            nested,
            search_nav,
            _subscriptions: subscriptions,
        }
    }
    pub(super) fn edit(
        &mut self,
        id: &'static str,
        edit: &TabsEdit,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let state = self.states[id].clone();
        let mut items = state.read(cx).items().entries().to_vec();
        let mut active = state.read(cx).active_key().cloned();
        match edit {
            TabsEdit::Add => {
                let key: SharedString = format!("newTab{}", self.next_id).into();
                self.next_id += 1;
                items.push(TabItem::new(key.clone(), "New Tab").text("Content of new Tab"));
                active = Some(key);
            }
            TabsEdit::Remove(key) => {
                if let Some(index) = items.iter().position(|item| &item.key == key) {
                    items.remove(index);
                    if active.as_ref() == Some(key) {
                        active = items
                            .get(index.saturating_sub(1))
                            .or_else(|| items.first())
                            .map(|item| item.key.clone());
                    }
                }
            }
        }
        state.update(cx, |state, cx| {
            state.set_items(TabsItems::new(items).unwrap(), window, cx);
            state.set_active_key(active, cx);
        });
        cx.notify();
    }
    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        let state = &self.states[id];
        let mut tabs = Tabs::new(id, state)
            .scope(self.scope.clone())
            .automation_id(format!("tabs.{id}"))
            .w_full()
            .on_edit(cx.listener(move |this, edit, window, cx| this.edit(id, edit, window, cx)));
        match id {
            "icon" => tabs = tabs.default_active_key("2"),
            "centered" => tabs = tabs.centered(true),
            "card" => tabs = tabs.kind(TabsType::Card),
            "editable-card" | "custom-add-trigger" => tabs = tabs.kind(TabsType::EditableCard),
            "extra" => {
                if self.extra_left {
                    tabs = tabs.extra_start(
                        div().mr(px(16.)).child(
                            Button::new("extra-left")
                                .scope(self.scope.clone())
                                .label("Left Extra Action"),
                        ),
                    );
                }
                if self.extra_right {
                    tabs = tabs.extra_end(
                        div().ml(px(16.)).child(
                            Button::new("extra-right")
                                .scope(self.scope.clone())
                                .label("Right Extra Action"),
                        ),
                    );
                }
            }
            "custom-indicator" => {
                tabs = tabs.indicator(
                    TabsIndicator::with_size(|original| (original - 20.).max(0.)).align(self.align),
                )
            }
            "slide" => {
                tabs = tabs
                    .default_active_key("1")
                    .placement(if self.slide_vertical {
                        TabPlacement::Start
                    } else {
                        TabPlacement::Top
                    })
                    .h(px(220.))
            }
            "size" => tabs = tabs.size(self.size),
            "placement" => tabs = tabs.placement(self.placement).h(px(180.)),
            "popupRender-Search" => {
                let editor = self.search.clone();
                let scope = self.scope.clone();
                let navigation = self.search_nav.clone();
                tabs = tabs
                    .default_active_key("1")
                    .more_triggers(PopupTriggers::CLICK)
                    .more_placement(ant_design_gpui::popup::PopupPlacement::BottomLeft)
                    .popup_focus(&editor.read(cx).focus_handle(cx))
                    .popup_render(move |_, context, _, cx| {
                        search_popup(context, &editor, &navigation, &scope, cx)
                    });
            }
            "custom-tab-bar" => {
                let scroll = self.sticky_scroll.read(cx).handle().clone();
                let scope = self.scope.clone();
                tabs = tabs
                    .header_render(move |header, _, _, cx| {
                        let displacement = (-f32::from(scroll.offset().y)).clamp(0., 134.);
                        gpui::deferred(
                            div()
                                .relative()
                                .top(px(displacement))
                                .occlude()
                                .bg(scope.snapshot(cx).theme.tokens.color_bg_container.to_hsla())
                                .child(header),
                        )
                        .with_priority(1)
                        .into_any_element()
                    })
                    .h(px(260.));
            }
            "animated" => tabs = tabs.animated(self.animation),
            "card-top" => {
                let g = &self.scope.snapshot(cx).theme.tokens;
                let styles = TabsStyles {
                    header: div().mb_0().style().clone(),
                    body: div()
                        .p(px(g.padding as f32))
                        .bg(g.color_bg_container)
                        .style()
                        .clone(),
                    ..Default::default()
                };
                tabs = tabs.kind(TabsType::Card).styles(styles);
            }
            "component-token" => tabs = tabs.scope(self.token_scope.clone()),
            "custom-tab-bar-node" => {
                let scope = self.scope.clone();
                let controller = state.clone();
                tabs = tabs.tab_render(move |node, tab, _, _| {
                    let key = tab.item.key.clone();
                    let drop_key = key.clone();
                    let controller = controller.clone();
                    let scope = scope.clone();
                    div()
                        .id(key.clone())
                        .cursor_move()
                        .child(node)
                        .on_drag(TabDrag(key), move |drag, _, _, cx| {
                            cx.new(|_| DragPreview {
                                label: drag.0.clone(),
                                scope: scope.clone(),
                            })
                        })
                        .on_drop(move |drag: &TabDrag, window, cx| {
                            controller.update(cx, |state, cx| {
                                let mut items = state.items().entries().to_vec();
                                if let (Some(from), Some(to)) = (
                                    items.iter().position(|item| item.key == drag.0),
                                    items.iter().position(|item| item.key == drop_key),
                                ) {
                                    let item = items.remove(from);
                                    items.insert(to, item);
                                    state.set_items(TabsItems::new(items).unwrap(), window, cx);
                                }
                            });
                        })
                        .into_any_element()
                });
            }
            "style-class" => {
                let mut styles = TabsStyles {
                    root: div().border_2().border_dashed().style().clone(),
                    ..Default::default()
                };
                styles.root.border_color = Some(gpui::rgb(0x1890ff).into());
                styles.header.background = Some(gpui::rgba(0xf5f5f580).into());
                styles.body.background = Some(gpui::rgba(0xe6f7ffcc).into());
                styles.content = div().p(px(16.)).style().clone();
                styles.item = div()
                    .font_weight(gpui::FontWeight::BOLD)
                    .text_color(gpui::rgb(0x1890ff))
                    .py(px(6.))
                    .px(px(10.))
                    .style()
                    .clone();
                styles.indicator = div().h(px(4.)).bg(gpui::rgba(0xff4d4f4d)).style().clone();
                tabs = tabs.styles(styles).p(px(16.));
            }
            _ => {}
        }
        let controls: Option<AnyElement> = match id {
            "animated" => Some(
                row(&self.scope, cx)
                    .child(
                        Switch::new("inkBar")
                            .scope(self.scope.clone())
                            .checked(self.animation.indicator)
                            .checked_label("inkBar")
                            .unchecked_label("inkBar")
                            .on_change(cx.listener(
                                |this, value: &ant_design_gpui::switch::SwitchChange, _, cx| {
                                    this.animation.indicator = value.checked;
                                    cx.notify();
                                },
                            )),
                    )
                    .child(
                        Switch::new("tabPane")
                            .scope(self.scope.clone())
                            .checked(self.animation.panes)
                            .checked_label("tabPane")
                            .unchecked_label("tabPane")
                            .on_change(cx.listener(
                                |this, value: &ant_design_gpui::switch::SwitchChange, _, cx| {
                                    this.animation.panes = value.checked;
                                    cx.notify();
                                },
                            )),
                    )
                    .into_any_element(),
            ),
            "custom-indicator" => Some(
                Segmented::new("indicator-align")
                    .scope(self.scope.clone())
                    .value(self.align)
                    .options([
                        SegmentedOption::new("start", TabsIndicatorAlign::Start, "start"),
                        SegmentedOption::new("center", TabsIndicatorAlign::Center, "center"),
                        SegmentedOption::new("end", TabsIndicatorAlign::End, "end"),
                    ])
                    .on_change(cx.listener(|this, value, _, cx| {
                        this.align = *value;
                        cx.notify();
                    }))
                    .into_any_element(),
            ),
            "slide" => Some(
                Segmented::new("slide-mode")
                    .scope(self.scope.clone())
                    .value(self.slide_vertical)
                    .options([
                        SegmentedOption::new("horizontal", false, "Horizontal"),
                        SegmentedOption::new("vertical", true, "Vertical"),
                    ])
                    .on_change(cx.listener(|this, value, _, cx| {
                        this.slide_vertical = *value;
                        cx.notify();
                    }))
                    .into_any_element(),
            ),
            "placement" => Some(
                Segmented::new("placement")
                    .scope(self.scope.clone())
                    .value(self.placement)
                    .options([
                        SegmentedOption::new("top", TabPlacement::Top, "top"),
                        SegmentedOption::new("bottom", TabPlacement::Bottom, "bottom"),
                        SegmentedOption::new("start", TabPlacement::Start, "start"),
                        SegmentedOption::new("end", TabPlacement::End, "end"),
                    ])
                    .on_change(cx.listener(|this, value, _, cx| {
                        this.placement = *value;
                        cx.notify();
                    }))
                    .into_any_element(),
            ),
            "size" => Some(
                Segmented::new("size")
                    .scope(self.scope.clone())
                    .value(self.size)
                    .options([
                        SegmentedOption::new("small", Size::Small, "Small"),
                        SegmentedOption::new("medium", Size::Medium, "Medium"),
                        SegmentedOption::new("large", Size::Large, "Large"),
                    ])
                    .on_change(cx.listener(|this, value, _, cx| {
                        this.size = *value;
                        cx.notify();
                    }))
                    .into_any_element(),
            ),
            "custom-add-trigger" => {
                tabs = tabs.hide_add(true);
                Some(
                    Button::new("custom-add")
                        .scope(self.scope.clone())
                        .label("ADD")
                        .on_click(cx.listener(move |this, _, window, cx| {
                            this.edit(id, &TabsEdit::Add, window, cx)
                        }))
                        .into_any_element(),
                )
            }
            _ => None,
        };
        if id == "nest" {
            return self.nested.clone().into_any_element();
        }
        if id == "custom-tab-bar" {
            return ScrollArea::new("sticky-viewport", &self.sticky_scroll)
                .scope(self.scope.clone())
                .h(px(300.))
                .w_full()
                .on_scroll(cx.listener(|_, _, _, cx| cx.notify()))
                .child(div().pt(px(64.)).pb(px(300.)).child(tabs))
                .into_any_element();
        }
        let mut preview = column().when_some(controls, |column, control| {
            column.child(row(&self.scope, cx).child(control))
        });
        if id == "extra" {
            preview = preview
                .child(
                    Tabs::new("extra-default", &self.states["extra-default"])
                        .scope(self.scope.clone())
                        .w_full()
                        .extra_end(
                            Button::new("extra-default-action")
                                .scope(self.scope.clone())
                                .label("Extra Action"),
                        ),
                )
                .child("You can also specify its direction or both side")
                .child(
                    row(&self.scope, cx)
                        .child(
                            Checkbox::new("extra-left-toggle")
                                .scope(self.scope.clone())
                                .label("left")
                                .checked(self.extra_left)
                                .on_change(cx.listener(
                                    |this,
                                     value: &ant_design_gpui::checkbox::CheckboxChange,
                                     _,
                                     cx| {
                                        this.extra_left = value.checked;
                                        cx.notify();
                                    },
                                )),
                        )
                        .child(
                            Checkbox::new("extra-right-toggle")
                                .scope(self.scope.clone())
                                .label("right")
                                .checked(self.extra_right)
                                .on_change(cx.listener(
                                    |this,
                                     value: &ant_design_gpui::checkbox::CheckboxChange,
                                     _,
                                     cx| {
                                        this.extra_right = value.checked;
                                        cx.notify();
                                    },
                                )),
                        ),
                );
        }
        preview = preview.child(tabs);
        if id == "size" {
            for (id, kind) in [
                ("size-card", TabsType::Card),
                ("size-editable", TabsType::EditableCard),
            ] {
                preview = preview.child(
                    Tabs::new(id, &self.states[id])
                        .scope(self.scope.clone())
                        .kind(kind)
                        .size(self.size)
                        .w_full()
                        .on_edit(cx.listener(move |this, edit, w, cx| this.edit(id, edit, w, cx))),
                );
            }
        }
        if id == "style-class" {
            preview = preview.child(
                Tabs::new("style-card", &self.states["style-card"])
                    .scope(self.scope.clone())
                    .kind(TabsType::Card)
                    .placement(TabPlacement::Start)
                    .w_full()
                    .styles_fn(|info, _, _| TabsStyles {
                        root: if info.kind == TabsType::Card {
                            div()
                                .bg(gpui::rgba(0xfafafacc))
                                .border_2()
                                .border_color(gpui::rgb(0xd9d9d9))
                                .p(px(16.))
                                .style()
                                .clone()
                        } else {
                            Default::default()
                        },
                        ..Default::default()
                    }),
            );
        }
        if id == "component-token" {
            for (id, kind, size, placement) in [
                (
                    "token-vertical",
                    TabsType::Line,
                    Size::Medium,
                    TabPlacement::Start,
                ),
                (
                    "token-small",
                    TabsType::Line,
                    Size::Small,
                    TabPlacement::Top,
                ),
                (
                    "token-large",
                    TabsType::Line,
                    Size::Large,
                    TabPlacement::Top,
                ),
                (
                    "token-card",
                    TabsType::Card,
                    Size::Medium,
                    TabPlacement::Top,
                ),
                (
                    "token-card-small",
                    TabsType::Card,
                    Size::Small,
                    TabPlacement::Top,
                ),
                (
                    "token-card-large",
                    TabsType::Card,
                    Size::Large,
                    TabPlacement::Top,
                ),
            ] {
                preview = preview.child(
                    Tabs::new(id, &self.states[id])
                        .scope(self.token_scope.clone())
                        .kind(kind)
                        .size(size)
                        .placement(placement)
                        .centered(kind == TabsType::Card)
                        .w_full(),
                );
            }
        }
        preview.into_any_element()
    }
}
impl Render for TabsPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.tabs.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "tabs",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
#[derive(Clone)]
struct TabDrag(SharedString);
struct SearchNavigation {
    focus: gpui::FocusHandle,
    active: Option<SharedString>,
    scroll: gpui::UniformListScrollHandle,
}
fn search_popup(
    context: &ant_design_gpui::tabs::TabsPopupRender,
    editor: &Entity<InputState>,
    navigation: &Entity<SearchNavigation>,
    scope: &ConfigScope,
    cx: &mut gpui::App,
) -> AnyElement {
    let query = editor.read(cx).value().to_lowercase();
    let filtered = std::rc::Rc::new(
        context
            .rest_tabs
            .iter()
            .filter(|item| item.label.to_lowercase().contains(&query))
            .cloned()
            .collect::<Vec<_>>(),
    );
    let g = scope.snapshot(cx).theme.tokens.clone();
    let key_items = filtered.clone();
    let nav = navigation.clone();
    let selection = context.clone();
    let key_editor = editor.clone();
    let focus = navigation.read(cx).focus.clone();
    let scroll = navigation.read(cx).scroll.clone();
    let active = navigation.read(cx).active.clone();
    let mut popup = div()
        .id("search-popup")
        .w(px(200.))
        .flex()
        .flex_col()
        .capture_key_down(move |event, w, cx| {
            let key = event.keystroke.key.as_str();
            let menu_focused = nav.read(cx).focus.is_focused(w);
            if !matches!(key, "up" | "down") && !(menu_focused && key == "enter") {
                return;
            }
            let enabled = key_items
                .iter()
                .filter(|item| !item.disabled)
                .collect::<Vec<_>>();
            if enabled.is_empty() {
                return;
            }
            let current = nav
                .read(cx)
                .active
                .as_ref()
                .and_then(|active| enabled.iter().position(|item| &item.key == active));
            if key == "enter" {
                if let Some(index) = current {
                    selection.select(&enabled[index].key, w, cx);
                    key_editor.update(cx, |s, cx| s.set_value("", w, cx));
                }
            } else {
                let index = if menu_focused {
                    if key == "up" {
                        current.map_or(enabled.len() - 1, |i| {
                            (i + enabled.len() - 1) % enabled.len()
                        })
                    } else {
                        current.map_or(0, |i| (i + 1) % enabled.len())
                    }
                } else if key == "up" {
                    enabled.len() - 1
                } else {
                    0
                };
                nav.update(cx, |s, cx| {
                    s.active = Some(enabled[index].key.clone());
                    s.focus.focus(w, cx);
                    s.scroll.scroll_to_item(
                        key_items
                            .iter()
                            .position(|i| i.key == enabled[index].key)
                            .unwrap(),
                        gpui::ScrollStrategy::Nearest,
                    );
                    cx.notify();
                });
            }
            w.prevent_default();
            cx.stop_propagation();
        })
        .child(
            div()
                .py(px(g.padding_xs as f32))
                .px(px(g.padding_sm as f32))
                .border_b_1()
                .border_color(g.color_border)
                .child(
                    Input::new("tab-search", editor)
                        .scope(scope.clone())
                        .placeholder("Search tabs...")
                        .prefix(ant_design_gpui::icons::icon(IconName::SearchOutlined))
                        .allow_clear(true)
                        .w_full(),
                ),
        );
    if filtered.is_empty() {
        popup = popup.child(
            div()
                .p(px(g.padding_sm as f32))
                .text_center()
                .text_color(g.color_text_disabled)
                .child("No matching tabs"),
        );
    } else {
        let selection = context.clone();
        let editor = editor.clone();
        let count = filtered.len();
        popup = popup.child(
            div()
                .id("search-results")
                .track_focus(&focus)
                .role(gpui::Role::Menu)
                .child(
                    gpui::uniform_list("search-rows", count, move |range, _, _| {
                        range
                            .map(|i| {
                                let item = &filtered[i];
                                let selection = selection.clone();
                                let key = item.key.clone();
                                let editor = editor.clone();
                                let disabled = item.disabled;
                                div()
                                    .id(key.clone())
                                    .debug_selector(|| format!("gallery.tabs.search-result.{key}"))
                                    .h(px(40.))
                                    .mx(px(4.))
                                    .px(px(12.))
                                    .flex()
                                    .items_center()
                                    .rounded(px(g.border_radius as f32))
                                    .when(item.disabled, |d| {
                                        d.text_color(g.color_text_disabled).cursor_not_allowed()
                                    })
                                    .when(!item.disabled, |d| {
                                        d.cursor_pointer().hover(|d| d.bg(g.control_item_bg_hover))
                                    })
                                    .when(active.as_ref() == Some(&key), |d| {
                                        d.bg(g.control_item_bg_active)
                                    })
                                    .role(gpui::Role::MenuItem)
                                    .aria_label(item.label.clone())
                                    .on_click(move |_, w, cx| {
                                        if disabled {
                                            return;
                                        }
                                        selection.select(&key, w, cx);
                                        editor.update(cx, |s, cx| s.set_value("", w, cx));
                                    })
                                    .child(item.label.clone())
                                    .into_any_element()
                            })
                            .collect::<Vec<_>>()
                    })
                    .w_full()
                    .h(px((count as f32 * 40.).min(300.)))
                    .track_scroll(&scroll),
                ),
        );
    }
    popup.into_any_element()
}
struct DragPreview {
    label: SharedString,
    scope: ConfigScope,
}
impl Render for DragPreview {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        Button::new("tab-drag-preview")
            .scope(self.scope.clone())
            .label(self.label.clone())
    }
}

struct NestedChild {
    scope: ConfigScope,
    state: Entity<TabsState>,
    placement: TabPlacement,
    kind: TabsType,
}
impl Render for NestedChild {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        Tabs::new("nested-child", &self.state)
            .scope(self.scope.clone())
            .placement(self.placement)
            .kind(self.kind)
            .h(px(300.))
            .w_full()
    }
}
struct NestedTabs {
    scope: ConfigScope,
    parent: Entity<TabsState>,
    child: Entity<NestedChild>,
    placement: TabPlacement,
    kind: TabsType,
}
impl NestedTabs {
    fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let state = cx.new(|cx| {
            TabsState::new(
                TabsItems::new((0..20).map(|i| {
                    TabItem::new(i.to_string(), format!("Tab {i}")).text(format!("TTTT {i}"))
                }))
                .unwrap(),
                window,
                cx,
            )
        });
        let child = cx.new(|_| NestedChild {
            scope: scope.clone(),
            state,
            placement: TabPlacement::Top,
            kind: TabsType::Line,
        });
        let view = child.clone();
        let parent = cx.new(|cx| {
            TabsState::new(
                TabsItems::new([
                    TabItem::new("1", "Tab 1").view(move |_, _| view.clone().into()),
                    TabItem::new("2", "Tab 2").text("Content of Tab Pane 2"),
                ])
                .unwrap(),
                window,
                cx,
            )
        });
        Self {
            scope,
            parent,
            child,
            placement: TabPlacement::Top,
            kind: TabsType::Line,
        }
    }
}
impl Render for NestedTabs {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut controls = column();
        for child in [false, true] {
            let placement = if child {
                self.child.read(cx).placement
            } else {
                self.placement
            };
            let kind = if child {
                self.child.read(cx).kind
            } else {
                self.kind
            };
            controls = controls
                .child(if child { "Child" } else { "Parent" })
                .child(
                    Segmented::new(if child {
                        "child-placement"
                    } else {
                        "parent-placement"
                    })
                    .scope(self.scope.clone())
                    .value(placement)
                    .options(
                        [
                            (TabPlacement::Top, "top"),
                            (TabPlacement::Bottom, "bottom"),
                            (TabPlacement::Start, "start"),
                            (TabPlacement::End, "end"),
                        ]
                        .map(|(value, label)| SegmentedOption::new(label, value, label)),
                    )
                    .on_change(cx.listener(move |this, value, _, cx| {
                        if child {
                            this.child.update(cx, |c, cx| {
                                c.placement = *value;
                                cx.notify();
                            });
                        } else {
                            this.placement = *value;
                        }
                        cx.notify();
                    })),
                )
                .child(
                    Segmented::new(if child { "child-kind" } else { "parent-kind" })
                        .scope(self.scope.clone())
                        .value(kind)
                        .options(
                            [
                                (TabsType::Line, "line"),
                                (TabsType::Card, "card"),
                                (TabsType::EditableCard, "editable-card"),
                            ]
                            .map(|(value, label)| SegmentedOption::new(label, value, label)),
                        )
                        .on_change(cx.listener(move |this, value, _, cx| {
                            if child {
                                this.child.update(cx, |c, cx| {
                                    c.kind = *value;
                                    cx.notify();
                                });
                            } else {
                                this.kind = *value;
                            }
                            cx.notify();
                        })),
                );
        }
        controls.child(
            Tabs::new("nested-parent", &self.parent)
                .scope(self.scope.clone())
                .kind(self.kind)
                .placement(self.placement)
                .w_full(),
        )
    }
}
