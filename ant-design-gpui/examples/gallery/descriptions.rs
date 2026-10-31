// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, DescriptionItem, Descriptions, DescriptionsBreakpoints,
    DescriptionsLayout, DescriptionsSpan, Direction, RadioGroup, RadioOption, Size,
    descriptions::{DescriptionsStyles, DescriptionsTheme, DescriptionsTokenPatch},
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, Context, FocusHandle, IntoElement, Render, Role, ScrollAnchor, StyleRefinement,
    Subscription, Window, div, prelude::*, px, relative,
};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

// Public examples follow components/descriptions/index.en-US.md order.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "descriptions-basic",
        title: "Basic",
        description: "Simplest Usage.",
    },
    Demo {
        id: "descriptions-border",
        title: "Border",
        description: "Descriptions with border and background color.",
    },
    Demo {
        id: "descriptions-size",
        title: "Custom size",
        description: "Custom sizes to fit in a variety of containers.",
    },
    Demo {
        id: "descriptions-responsive",
        title: "Responsive",
        description: "Responsive columns and item spans follow the window width.",
    },
    Demo {
        id: "descriptions-vertical",
        title: "Vertical",
        description: "Display the description label above its content.",
    },
    Demo {
        id: "descriptions-vertical-border",
        title: "Vertical border",
        description: "Vertical descriptions with borders and label backgrounds.",
    },
    Demo {
        id: "descriptions-style-class",
        title: "Custom semantic dom styling",
        description: "Customize the semantic slots with native styles or a resolver based on the component properties.",
    },
    Demo {
        id: "descriptions-block",
        title: "Row",
        description: "Fill the remaining columns in the current row.",
    },
    Demo {
        id: "descriptions-component-token",
        title: "Component Token",
        description: "Customize label backgrounds, title spacing, text colors and colon margins through a child theme.",
    },
];

const ADDRESS: &str = "No. 18, Wantang Road, Xihu District, Hangzhou, Zhejiang, China";
const CONFIG: &str = "Data disk type: MongoDB\nDatabase version: 3.4\nPackage: dds.mongo.mid\nStorage space: 10 GB\nReplication factor: 3\nRegion: East China 1";

pub(super) struct DescriptionsPage {
    pub scope: ConfigScope,
    token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub size: Size,
    pub edit_count: usize,
    pub edit_focus: FocusHandle,
    _config: Subscription,
}

impl DescriptionsPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme
            .components
            .set::<DescriptionsTheme>(DescriptionsTokenPatch {
                label_bg: Some(Color::rgb(0xff0000)),
                title_color: Some(Color::rgb(0xff0000)),
                title_margin_bottom: Some(2.),
                item_padding_bottom: Some(8.),
                item_padding_end: Some(8.),
                colon_margin_right: Some(10.),
                colon_margin_left: Some(20.),
                content_color: Some(Color::rgb(0x008000)),
                extra_color: Some(Color::rgb(0x0000ff)),
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
            .expect("valid Descriptions component theme");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            anchors,
            size: Size::Large,
            edit_count: 0,
            edit_focus: ant_design_gpui::behavior::focus_handle(cx),
        }
    }

    fn descriptions(&self, id: &'static str) -> Descriptions {
        Descriptions::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .w_full()
            .min_w(px(0.))
    }

    fn basic_items(vertical: bool) -> Vec<DescriptionItem> {
        let mut items = vec![
            DescriptionItem::new("name")
                .label("UserName")
                .content("Zhou Maomao"),
            DescriptionItem::new("telephone")
                .label("Telephone")
                .content("1810000000"),
            DescriptionItem::new("live")
                .label("Live")
                .content("Hangzhou, Zhejiang"),
        ];
        if vertical {
            items.push(
                DescriptionItem::new("address")
                    .label("Address")
                    .content(ADDRESS)
                    .span(2),
            );
            items.push(
                DescriptionItem::new("remark")
                    .label("Remark")
                    .content("empty"),
            );
        } else {
            items.push(
                DescriptionItem::new("remark")
                    .label("Remark")
                    .content("empty"),
            );
            items.push(
                DescriptionItem::new("address")
                    .label("Address")
                    .content(ADDRESS),
            );
        }
        items
    }

    fn running(&self, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        // Host-owned status content; Badge is an independent component.
        div()
            .flex()
            .items_center()
            .gap(px(8.))
            .when(config.direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
            .child(
                div()
                    .w(px(6.))
                    .h(px(6.))
                    .rounded_full()
                    .bg(config.theme.tokens.color_primary),
            )
            .child("Running")
            .into_any_element()
    }

    fn lines(text: &'static str) -> AnyElement {
        div()
            .flex()
            .flex_col()
            .children(text.split('\n').map(|line| div().child(line)))
            .into_any_element()
    }

    fn bordered_items(&self, cx: &Context<Self>) -> Vec<DescriptionItem> {
        vec![
            DescriptionItem::new("product")
                .label("Product")
                .content("Cloud Database"),
            DescriptionItem::new("billing")
                .label("Billing Mode")
                .content("Prepaid"),
            DescriptionItem::new("renewal")
                .label("Automatic Renewal")
                .content("YES"),
            DescriptionItem::new("order")
                .label("Order time")
                .content("2018-04-24 18:00:00"),
            DescriptionItem::new("usage")
                .label("Usage Time")
                .content("2019-04-24 18:00:00")
                .span(2),
            DescriptionItem::new("status")
                .label("Status")
                .content_element(self.running(cx))
                .span(3),
            DescriptionItem::new("amount")
                .label("Negotiated Amount")
                .content("$80.00"),
            DescriptionItem::new("discount")
                .label("Discount")
                .content("$20.00"),
            DescriptionItem::new("receipts")
                .label("Official Receipts")
                .content("$60.00"),
            DescriptionItem::new("config")
                .label("Config Info")
                .content_element(Self::lines(CONFIG)),
        ]
    }

    fn compact_items(with_config: bool) -> Vec<DescriptionItem> {
        let mut items = [
            ("Product", "Cloud Database"),
            ("Billing", "Prepaid"),
            ("Time", "18:00:00"),
            ("Amount", "$80.00"),
            ("Discount", "$20.00"),
            ("Official", "$60.00"),
        ]
        .into_iter()
        .map(|(label, content)| DescriptionItem::new(label).label(label).content(content))
        .collect::<Vec<_>>();
        if with_config {
            items.push(
                DescriptionItem::new("config")
                    .label("Config Info")
                    .content_element(Self::lines(CONFIG)),
            );
        }
        items
    }

    fn edit(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .flex()
            .flex_shrink_0()
            .child(
                Button::new("edit")
                    .scope(self.scope.clone())
                    .automation_id(id)
                    .label("Edit")
                    .primary()
                    .when(id == "descriptions.size.edit", |button| {
                        button.focus_handle(&self.edit_focus)
                    })
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.edit_count += 1;
                        cx.notify();
                    })),
            )
            .into_any_element()
    }

    fn responsive(&self) -> Descriptions {
        let span = DescriptionsBreakpoints {
            xs: Some(1),
            sm: Some(2),
            md: Some(3),
            lg: Some(3),
            xl: Some(2),
            xxl: Some(2),
            ..Default::default()
        };
        let wide = DescriptionsBreakpoints {
            xl: Some(2),
            xxl: Some(2),
            ..Default::default()
        };
        let mut items = Self::compact_items(false);
        items[4] = DescriptionItem::new("Discount")
            .label("Discount")
            .content("$20.00")
            .span(DescriptionsSpan::Responsive(wide));
        items[5] = DescriptionItem::new("Official")
            .label("Official")
            .content("$60.00")
            .span(DescriptionsSpan::Responsive(wide));
        items.push(
            DescriptionItem::new("config")
                .label("Config Info")
                .span(DescriptionsSpan::Responsive(span))
                .content_element(Self::lines(
                    "Data disk type: MongoDB\nDatabase version: 3.4\nPackage: dds.mongo.mid",
                )),
        );
        items.push(DescriptionItem::new("hardware").label("Hardware Info").span(DescriptionsSpan::Responsive(span)).content_element(Self::lines("CPU: 6 Core 3.5 GHz\nStorage space: 10 GB\nReplication factor: 3\nRegion: East China 1")));
        self.descriptions("descriptions.responsive")
            .title("Responsive Descriptions")
            .bordered(true)
            .columns(
                ant_design_gpui::descriptions::DescriptionsColumns::Responsive(
                    DescriptionsBreakpoints {
                        xs: Some(1),
                        sm: Some(2),
                        md: Some(3),
                        lg: Some(3),
                        xl: Some(4),
                        xxl: Some(4),
                        ..Default::default()
                    },
                ),
            )
            .items(items)
    }

    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        match id {
            "descriptions-basic" => self
                .descriptions("descriptions.basic")
                .title("User Info")
                .items(Self::basic_items(false))
                .into_any_element(),
            "descriptions-border" => self
                .descriptions("descriptions.border")
                .title("User Info")
                .bordered(true)
                .items(self.bordered_items(cx))
                .into_any_element(),
            "descriptions-size" => div()
                .flex()
                .flex_col()
                .gap(px(32.))
                .child(
                    div()
                        .id("descriptions.size.control")
                        .debug_selector(|| "descriptions.size.control".into())
                        .child(
                            RadioGroup::new("size")
                                .scope(self.scope.clone())
                                .accessibility_label("Descriptions size")
                                .options([(0u8, "large"), (1, "medium"), (2, "small")].map(
                                    |(value, label)| {
                                        RadioOption::new(label, value, label)
                                            .automation_id(format!("descriptions.size.{label}"))
                                    },
                                ))
                                .value(match self.size {
                                    Size::Large => 0,
                                    Size::Medium => 1,
                                    Size::Small => 2,
                                })
                                .on_change(cx.listener(|this, value: &u8, _, cx| {
                                    this.size = match value {
                                        0 => Size::Large,
                                        1 => Size::Medium,
                                        _ => Size::Small,
                                    };
                                    cx.notify();
                                })),
                        ),
                )
                .child(
                    self.descriptions("descriptions.size.bordered")
                        .title("Custom Size")
                        .size(self.size)
                        .bordered(true)
                        .extra_content(self.edit("descriptions.size.edit", cx))
                        .items(Self::compact_items(true)),
                )
                .child(
                    self.descriptions("descriptions.size.plain")
                        .title("Custom Size")
                        .size(self.size)
                        .extra_content(self.edit("descriptions.size.plain-edit", cx))
                        .items(Self::compact_items(false)),
                )
                .when(self.edit_count > 0, |column| {
                    column.child(
                        div()
                            .id("descriptions.edit.feedback")
                            .role(Role::Status)
                            .child(format!("Edit requested {} time(s).", self.edit_count)),
                    )
                })
                .into_any_element(),
            "descriptions-responsive" => self.responsive().into_any_element(),
            "descriptions-vertical" => self
                .descriptions("descriptions.vertical")
                .title("User Info")
                .layout(DescriptionsLayout::Vertical)
                .items(Self::basic_items(true))
                .into_any_element(),
            "descriptions-vertical-border" => self
                .descriptions("descriptions.vertical-border")
                .title("User Info")
                .layout(DescriptionsLayout::Vertical)
                .bordered(true)
                .items(self.bordered_items(cx))
                .into_any_element(),
            "descriptions-style-class" => div()
                .flex()
                .flex_col()
                .gap(px(24.))
                .child(
                    self.descriptions("descriptions.semantic.static")
                        .title("User Info")
                        .size(Size::Small)
                        .bordered(true)
                        .items(self.bordered_items(cx).into_iter().take(3))
                        .styles(DescriptionsStyles {
                            root: StyleRefinement::default().p(px(10.)),
                            label: StyleRefinement::default().text_color(Color::rgb(0x000000)),
                            ..Default::default()
                        }),
                )
                .child(
                    self.descriptions("descriptions.semantic.function")
                        .title("User Info")
                        .size(Size::Large)
                        .bordered(true)
                        .items(self.bordered_items(cx).into_iter().take(3))
                        .p(px(10.))
                        .styles_fn(|context, _, _| {
                            if context.size == Size::Large {
                                DescriptionsStyles {
                                    root: StyleRefinement::default()
                                        .rounded(px(8.))
                                        .border_1()
                                        .border_color(Color::rgb(0xcdc1ff)),
                                    label: StyleRefinement::default()
                                        .text_color(Color::rgb(0xa294f9)),
                                    ..Default::default()
                                }
                            } else {
                                DescriptionsStyles::default()
                            }
                        }),
                )
                .into_any_element(),
            "descriptions-block" => self
                .descriptions("descriptions.block")
                .title("User Info")
                .bordered(true)
                .items([
                    DescriptionItem::new("name")
                        .label("UserName")
                        .content("Zhou Maomao"),
                    DescriptionItem::new("live")
                        .label("Live")
                        .content("Hangzhou, Zhejiang")
                        .span(DescriptionsSpan::Filled),
                    DescriptionItem::new("remark")
                        .label("Remark")
                        .content("empty")
                        .span(DescriptionsSpan::Filled),
                    DescriptionItem::new("address")
                        .label("Address")
                        .content(ADDRESS),
                ])
                .into_any_element(),
            "descriptions-component-token" => div()
                .flex()
                .flex_col()
                .gap(px(32.))
                .child(
                    self.descriptions("descriptions.token.bordered")
                        .scope(self.token_scope.clone())
                        .title("Custom Size")
                        .bordered(true)
                        .extra("Extra color: blue")
                        .items(Self::compact_items(true)),
                )
                .child(
                    self.descriptions("descriptions.token.plain")
                        .scope(self.token_scope.clone())
                        .title("Custom Size")
                        .extra_content(self.edit("descriptions.token.edit", cx))
                        .items(Self::compact_items(false)),
                )
                .into_any_element(),
            _ => unreachable!("registered Descriptions preview"),
        }
    }
}

impl Render for DescriptionsPage {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        div()
            .id("gallery.descriptions.examples")
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .gap(px(tokens.margin_lg as f32))
            .when(config.direction == Direction::Rtl, |page| page.text_right())
            .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                div()
                    .id(demo.id)
                    .anchor_scroll(Some(self.anchors[index].clone()))
                    .role(Role::Group)
                    .aria_label(demo.title)
                    .debug_selector(|| format!("gallery.card.{}", demo.id))
                    .w_full()
                    .min_w(px(0.))
                    .overflow_hidden()
                    .border_1()
                    .border_color(tokens.color_border_secondary)
                    .rounded(px(tokens.border_radius_lg as f32))
                    .bg(tokens.color_bg_container)
                    .child(
                        div()
                            .p(px(tokens.padding as f32))
                            .font_weight(gpui::FontWeight::SEMIBOLD)
                            .child(demo.title),
                    )
                    .child(
                        div()
                            .id("preview")
                            .role(Role::Group)
                            .aria_label(format!("{} preview", demo.title))
                            .debug_selector(|| format!("gallery.preview.{}", demo.id))
                            .min_w(px(0.))
                            .p(px(tokens.padding_lg as f32))
                            .child(self.preview(demo.id, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(tokens.color_border_secondary)
                            .p(px(tokens.padding as f32))
                            .text_size(px(tokens.font_size_sm as f32))
                            .line_height(relative(tokens.line_height_sm as f32))
                            .text_color(tokens.color_text_secondary)
                            .child(demo.description),
                    )
            }))
    }
}
