// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigScope, Direction, Segmented, SegmentedOption, Size,
    icons::{IconName, IconPalette, icon},
    segmented::SegmentedShape,
    theme::Color,
};
use gpui::{
    AnyElement, Context, FontWeight, IntoElement, Render, Role, ScrollAnchor, StyleRefinement,
    Subscription, Window, div, prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

// Preserve the public order in components/segmented/index.en-US.md.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "segmented-basic",
        title: "Basic",
        description: "The most basic usage.",
    },
    Demo {
        id: "segmented-vertical",
        title: "Vertical Direction",
        description: "Make it vertical.",
    },
    Demo {
        id: "segmented-block",
        title: "Block Segmented",
        description: "The block property makes Segmented fit its parent's width. Long labels truncate inside equal-width items.",
    },
    Demo {
        id: "segmented-shape",
        title: "Round shape",
        description: "Round shape of Segmented. Choose a size to compare the rounded controls.",
    },
    Demo {
        id: "segmented-disabled",
        title: "Disabled",
        description: "Disable the entire Segmented or individual options.",
    },
    Demo {
        id: "segmented-controlled",
        title: "Controlled mode",
        description: "Controlled Segmented. Selection is stored by the containing view.",
    },
    Demo {
        id: "segmented-custom",
        title: "Custom Render",
        description: "Customize each Segmented item with native GPUI content.",
    },
    Demo {
        id: "segmented-dynamic",
        title: "Dynamic",
        description: "Load options dynamically while preserving the selection.",
    },
    Demo {
        id: "segmented-size",
        title: "Three sizes of Segmented",
        description: "Large, medium and small controls are 40px, 32px and 24px high in the default theme.",
    },
    Demo {
        id: "segmented-with-icon",
        title: "With Icon",
        description: "Add an icon alongside each Segmented label.",
    },
    Demo {
        id: "segmented-icon-only",
        title: "With Icon only",
        description: "Set an icon without a visible label. Accessible names identify each option.",
    },
    Demo {
        id: "segmented-with-name",
        title: "With name",
        description: "Native radio-group semantics keep keyboard navigation within this Segmented. Tab enters the group; arrow keys change the selected option.",
    },
    Demo {
        id: "segmented-style-class",
        title: "Custom semantic dom styling",
        description: "Customize the root, item, icon and label with native Styled refinements and semantic styles.",
    },
];

pub(super) struct SegmentedPage {
    pub scope: ConfigScope,
    pub value: &'static str,
    pub round_size: Size,
    pub more_loaded: bool,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl SegmentedPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        Self {
            _config: scope.observe(cx),
            scope,
            value: "Map",
            round_size: Size::Medium,
            more_loaded: false,
            anchors,
        }
    }

    fn segmented(&self, id: &'static str, labels: &[&'static str]) -> Segmented<&'static str> {
        Segmented::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .accessibility_label(id.trim_start_matches("segmented.").replace('.', " "))
            .options(
                labels
                    .iter()
                    .map(|&label| SegmentedOption::new(label, label, label)),
            )
    }

    fn column(&self, cx: &Context<Self>) -> gpui::Div {
        let gap = self.scope.snapshot(cx).theme.tokens.margin_sm;
        div()
            .w_full()
            .flex()
            .flex_col()
            .items_start()
            .gap(px(gap as f32))
    }

    fn icon_options() -> [SegmentedOption<&'static str>; 2] {
        [
            SegmentedOption::new("list", "List", "")
                .icon(IconName::BarsOutlined)
                .accessibility_label("List"),
            SegmentedOption::new("kanban", "Kanban", "")
                .icon(IconName::AppstoreOutlined)
                .accessibility_label("Kanban"),
        ]
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        const PERIODS: &[&str] = &["Daily", "Weekly", "Monthly", "Quarterly", "Yearly"];
        match id {
            "segmented-basic" => self
                .segmented("segmented.basic", PERIODS)
                .into_any_element(),
            "segmented-vertical" => self
                .segmented("segmented.vertical", &[])
                .options(Self::icon_options())
                .vertical(true)
                .into_any_element(),
            "segmented-block" => self
                .segmented(
                    "segmented.block",
                    &["123", "456", "longtext-longtext-longtext-longtext"],
                )
                .block(true)
                .into_any_element(),
            "segmented-shape" => self
                .column(cx)
                .child(
                    Segmented::new("segmented.shape.size")
                        .scope(self.scope.clone())
                        .accessibility_label("Round control size")
                        .options([
                            SegmentedOption::new("small", Size::Small, "small"),
                            SegmentedOption::new("medium", Size::Medium, "medium"),
                            SegmentedOption::new("large", Size::Large, "large"),
                        ])
                        .value(self.round_size)
                        .on_change(cx.listener(|this, value: &Size, _, cx| {
                            this.round_size = *value;
                            cx.notify();
                        })),
                )
                .child(
                    self.segmented("segmented.shape.round", &[])
                        .options([
                            SegmentedOption::new("light", "light", "")
                                .icon(IconName::SunOutlined)
                                .accessibility_label("Light"),
                            SegmentedOption::new("dark", "dark", "")
                                .icon(IconName::MoonOutlined)
                                .accessibility_label("Dark"),
                        ])
                        .shape(SegmentedShape::Round)
                        .size(self.round_size),
                )
                .into_any_element(),
            "segmented-disabled" => self
                .column(cx)
                .child(
                    self.segmented("segmented.disabled.all", &["Map", "Transit", "Satellite"])
                        .disabled(true),
                )
                .child(self.segmented("segmented.disabled.options", &[]).options([
                    SegmentedOption::new("daily", "Daily", "Daily"),
                    SegmentedOption::new("weekly", "Weekly", "Weekly").disabled(true),
                    SegmentedOption::new("monthly", "Monthly", "Monthly"),
                    SegmentedOption::new("quarterly", "Quarterly", "Quarterly").disabled(true),
                    SegmentedOption::new("yearly", "Yearly", "Yearly"),
                ]))
                .into_any_element(),
            "segmented-controlled" => self
                .column(cx)
                .child(
                    div()
                        .id("segmented.controlled")
                        .debug_selector(|| "segmented.controlled".into())
                        .child(
                            self.segmented(
                                "segmented.controlled.group",
                                &["Map", "Transit", "Satellite"],
                            )
                            .value(self.value)
                            .on_change(cx.listener(
                                |this, value: &&'static str, _, cx| {
                                    this.value = *value;
                                    cx.notify();
                                },
                            )),
                        ),
                )
                .child(
                    div()
                        .text_color(self.scope.snapshot(cx).theme.tokens.color_text_secondary)
                        .child(format!("Selected: {}", self.value)),
                )
                .into_any_element(),
            "segmented-custom" => {
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                self.column(cx)
                    .child(
                        self.segmented("segmented.custom.users", &[]).options(
                            [
                                ("user1", "User 1", Color::rgb(0x1677ff), Some("A")),
                                ("user2", "User 2", Color::rgb(0xf56a00), Some("K")),
                                ("user3", "User 3", Color::rgb(0x87d068), None),
                            ]
                            .map(|(id, label, color, initial)| {
                                let avatar = div()
                                    .size(px(32.))
                                    .rounded_full()
                                    .bg(color)
                                    .text_color(Color::rgb(0xffffff))
                                    .flex()
                                    .items_center()
                                    .justify_center();
                                SegmentedOption::new(id, id, label)
                                    .child(
                                        div()
                                            .p(px(4.))
                                            .flex()
                                            .flex_col()
                                            .items_center()
                                            .child(match initial {
                                                Some(initial) => {
                                                    avatar.child(initial).into_any_element()
                                                }
                                                None => avatar
                                                    .child(
                                                        icon(IconName::UserOutlined)
                                                            .size(px(18.))
                                                            .palette(IconPalette {
                                                                text: Color::rgb(0xffffff)
                                                                    .to_rgba(),
                                                                ..self
                                                                    .scope
                                                                    .snapshot(cx)
                                                                    .icon_palette
                                                            }),
                                                    )
                                                    .into_any_element(),
                                            })
                                            .child(label),
                                    )
                                    .accessibility_label(label)
                                    .tooltip(format!("Hello {label}"))
                            }),
                        ),
                    )
                    .child(
                        self.segmented("segmented.custom.seasons", &[]).options(
                            [
                                ("spring", "Spring", "Jan-Mar"),
                                ("summer", "Summer", "Apr-Jun"),
                                ("autumn", "Autumn", "Jul-Sept"),
                                ("winter", "Winter", "Oct-Dec"),
                            ]
                            .map(|(id, label, detail)| {
                                SegmentedOption::new(id, id, label)
                                    .child(
                                        div()
                                            .p(px(4.))
                                            .flex()
                                            .flex_col()
                                            .items_center()
                                            .child(label)
                                            .child(
                                                div()
                                                    .text_size(px(tokens.font_size_sm as f32))
                                                    .child(detail),
                                            ),
                                    )
                                    .accessibility_label(format!("{label}, {detail}"))
                            }),
                        ),
                    )
                    .into_any_element()
            }
            "segmented-dynamic" => self
                .column(cx)
                .child(self.segmented(
                    "segmented.dynamic.options",
                    if self.more_loaded {
                        PERIODS
                    } else {
                        &PERIODS[..3]
                    },
                ))
                .child(
                    div()
                        .id("segmented.dynamic.load")
                        .debug_selector(|| "segmented.dynamic.load".into())
                        .child(
                            Button::new("segmented.dynamic.load.button")
                                .scope(self.scope.clone())
                                .primary()
                                .label("Load more options")
                                .disabled(self.more_loaded)
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.more_loaded = true;
                                    cx.notify();
                                })),
                        ),
                )
                .into_any_element(),
            "segmented-size" => self
                .column(cx)
                .child(
                    self.segmented("segmented.size.large", PERIODS)
                        .size(Size::Large),
                )
                .child(
                    self.segmented("segmented.size.medium", PERIODS)
                        .size(Size::Medium),
                )
                .child(
                    self.segmented("segmented.size.small", PERIODS)
                        .size(Size::Small),
                )
                .into_any_element(),
            "segmented-with-icon" => self
                .segmented("segmented.with-icon", &[])
                .options([
                    SegmentedOption::new("list", "List", "List").icon(IconName::BarsOutlined),
                    SegmentedOption::new("kanban", "Kanban", "Kanban")
                        .icon(IconName::AppstoreOutlined),
                    SegmentedOption::new("calendar", "Calendar", "Calendar")
                        .icon(IconName::CalendarOutlined),
                ])
                .into_any_element(),
            "segmented-icon-only" => self
                .segmented("segmented.icon-only", &[])
                .options(Self::icon_options())
                .into_any_element(),
            "segmented-with-name" => self
                .segmented("segmented.with-name", PERIODS)
                .accessibility_label("Period choices")
                .into_any_element(),
            "segmented-style-class" => {
                let options = || {
                    [
                        SegmentedOption::new("boost", "boost", "Boost")
                            .icon(IconName::RocketOutlined),
                        SegmentedOption::new("stream", "stream", "Stream")
                            .icon(IconName::ThunderboltOutlined),
                        SegmentedOption::new("cloud", "cloud", "Cloud")
                            .icon(IconName::CloudOutlined),
                    ]
                };
                self.column(cx)
                    .child(
                        self.segmented("segmented.styles.object", &[])
                            .options(options())
                            .p(px(4.))
                            .w(px(260.)),
                    )
                    .child(
                        self.segmented("segmented.styles.function", &[])
                            .options(options())
                            .vertical(true)
                            .p(px(4.))
                            .w(px(100.))
                            .border_1()
                            .border_color(Color::rgb(0x77bef0))
                            .item_style(StyleRefinement::default().justify_start())
                            .label_style(StyleRefinement::default().justify_start())
                            .icon_style(
                                StyleRefinement::default().text_color(Color::rgb(0x77bef0)),
                            ),
                    )
                    .into_any_element()
            }
            _ => unreachable!("registered Segmented demo"),
        }
    }
}

impl Render for SegmentedPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let viewport = window.viewport_size().width;
        let content = viewport - sidebar_width(viewport) - page_padding(viewport) * 2.;
        div()
            .id("gallery.segmented.examples")
            .w_full()
            .grid()
            .grid_cols(if content >= px(840.) { 2 } else { 1 })
            .items_start()
            .when(config.direction == Direction::Rtl, |page| page.text_right())
            .gap(px(tokens.margin_lg as f32))
            .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                div()
                    .id(demo.id)
                    .anchor_scroll(Some(self.anchors[index].clone()))
                    .role(Role::Group)
                    .aria_label(demo.title)
                    .debug_selector(|| format!("gallery.segmented.card.{}", demo.id))
                    .min_w(px(0.))
                    .overflow_hidden()
                    .border_1()
                    .border_color(tokens.color_border_secondary)
                    .rounded(px(tokens.border_radius_lg as f32))
                    .bg(tokens.color_bg_container)
                    .child(
                        div()
                            .p(px(tokens.padding as f32))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(demo.title),
                    )
                    .child(
                        div()
                            .id("preview")
                            .role(Role::Group)
                            .aria_label(format!("{} preview", demo.title))
                            .debug_selector(|| format!("gallery.segmented.preview.{}", demo.id))
                            .min_w(px(0.))
                            .min_h(px(120.))
                            .p(px(tokens.padding_lg as f32))
                            .flex()
                            .items_center()
                            .child(self.preview(demo.id, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(tokens.color_border_secondary)
                            .p(px(tokens.padding as f32))
                            .text_size(px(tokens.font_size_sm as f32))
                            .line_height(relative(tokens.line_height as f32))
                            .text_color(tokens.color_text_secondary)
                            .child(demo.description),
                    )
            }))
    }
}
