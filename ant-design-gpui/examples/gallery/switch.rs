// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Size, Switch,
    icons::{IconName, IconPalette, icon},
    switch::{SwitchChange, SwitchTheme, SwitchTokenPatch},
    theme::{Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
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

// Follow components/switch/index.en-US.md, including its component token example.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "basic",
        title: "Basic",
        description: "Toggle the switch to apply a change immediately.",
    },
    Demo {
        id: "disabled",
        title: "Disabled",
        description: "Disable the switch while preserving its checked state.",
    },
    Demo {
        id: "text",
        title: "Text & icon",
        description: "Use text, numbers, or native icon content to describe each state.",
    },
    Demo {
        id: "size",
        title: "Two sizes",
        description: "Choose the standard switch or its small size.",
    },
    Demo {
        id: "loading",
        title: "Loading",
        description: "The loading indicator prevents changes while an operation is in progress.",
    },
    Demo {
        id: "component-token",
        title: "Custom component token",
        description: "Customize track and handle geometry through a component theme scoped to this example.",
    },
    Demo {
        id: "style-class",
        title: "Custom semantic styling",
        description: "Style the root, content, and indicator independently with native GPUI refinements.",
    },
];

pub(super) struct SwitchPage {
    pub scope: ConfigScope,
    pub token_scope: ConfigScope,
    pub checked: bool,
    pub disabled: bool,
    pub loading: bool,
    pub changes: usize,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl SwitchPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let blue = Color::rgb(0x1976d2);
        let mut theme = ThemeConfig::default();
        theme.components.set_theme(
            ComponentThemeConfig::<SwitchTheme>::new(SwitchTokenPatch {
                track_height: Some(14.),
                track_min_width: Some(32.),
                track_padding: Some(-3.),
                handle_size: Some(20.),
                handle_bg: Some(blue),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                color_primary: Some(blue.with_alpha(0.5)),
                ..Default::default()
            }),
        );
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid Switch example theme");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            checked: true,
            disabled: true,
            loading: true,
            changes: 0,
            anchors,
        }
    }

    fn switch(&self, id: &'static str, label: &'static str) -> Switch {
        Switch::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .accessibility_label(label)
    }

    fn control(&self, id: &'static str, switch: Switch) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .flex()
            .items_center()
            .child(switch)
            .into_any_element()
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        let config = self.scope.snapshot(cx);
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(config.theme.tokens.margin as f32))
            .when(config.direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn column(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .flex()
            .flex_col()
            .items_start()
            .gap(px(self.scope.snapshot(cx).theme.tokens.margin as f32))
    }

    fn state_icon(&self, name: IconName, cx: &Context<Self>) -> ant_design_gpui::icons::Icon {
        let config = self.scope.snapshot(cx);
        icon(name)
            .size(px(config.theme.tokens.font_size_sm as f32))
            .palette(IconPalette {
                text: config.theme.tokens.color_text_light_solid.to_rgba(),
                ..config.icon_palette
            })
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "basic" => self.control(
                "switch.basic",
                self.switch("switch.basic", "Basic switch")
                    .checked(self.checked)
                    .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                        this.checked = event.checked;
                        this.changes += 1;
                        cx.notify();
                    })),
            ),
            "disabled" => self
                .column(cx)
                .child(
                    self.control(
                        "switch.disabled",
                        self.switch("switch.disabled", "Disabled example switch")
                            .default_checked(true)
                            .disabled(self.disabled)
                            .on_change(cx.listener(|this, _: &SwitchChange, _, cx| {
                                this.changes += 1;
                                cx.notify();
                            })),
                    ),
                )
                .child(
                    div()
                        .id("switch.disabled.toggle")
                        .debug_selector(|| "switch.disabled.toggle".into())
                        .child(
                            Button::new("switch.disabled.toggle")
                                .scope(self.scope.clone())
                                .automation_id("switch.disabled.toggle")
                                .primary()
                                .label("Toggle disabled")
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.disabled = !this.disabled;
                                    cx.notify();
                                })),
                        ),
                )
                .into_any_element(),
            "text" => self
                .column(cx)
                .child(
                    self.control(
                        "switch.text.words",
                        self.switch("switch.text.words", "Text switch")
                            .checked_label("On")
                            .unchecked_label("Off")
                            .default_checked(true),
                    ),
                )
                .child(
                    self.control(
                        "switch.text.numbers",
                        self.switch("switch.text.numbers", "Numeric switch")
                            .checked_label("1")
                            .unchecked_label("0")
                            .default_checked(true),
                    ),
                )
                .child(
                    self.control(
                        "switch.text.icons",
                        self.switch("switch.text.icons", "Icon switch")
                            .checked_content(self.state_icon(IconName::CheckOutlined, cx))
                            .unchecked_content(self.state_icon(IconName::CloseOutlined, cx))
                            .default_checked(true),
                    ),
                )
                .child(
                    self.control(
                        "switch.text.combined",
                        self.switch("switch.text.combined", "Mood switch")
                            .checked_content(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(4.))
                                    .child(self.state_icon(IconName::SmileOutlined, cx))
                                    .child("Happy"),
                            )
                            .unchecked_content(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(4.))
                                    .child(self.state_icon(IconName::FrownOutlined, cx))
                                    .child("Sad"),
                            )
                            .default_checked(true),
                    ),
                )
                .into_any_element(),
            "size" => self
                .column(cx)
                .child(
                    self.control(
                        "switch.size.normal",
                        self.switch("switch.size.normal", "Standard switch")
                            .size(Size::Medium)
                            .default_checked(true),
                    ),
                )
                .child(
                    self.control(
                        "switch.size.small",
                        self.switch("switch.size.small", "Small switch")
                            .size(Size::Small)
                            .default_checked(true),
                    ),
                )
                .into_any_element(),
            "loading" => self
                .column(cx)
                .child(
                    self.row(cx)
                        .child(
                            self.control(
                                "switch.loading.normal",
                                self.switch("switch.loading.normal", "Loading standard switch")
                                    .size(Size::Medium)
                                    .default_checked(true)
                                    .loading(self.loading)
                                    .on_change(cx.listener(|this, _: &SwitchChange, _, cx| {
                                        this.changes += 1;
                                        cx.notify();
                                    })),
                            ),
                        )
                        .child(
                            self.control(
                                "switch.loading.small",
                                self.switch("switch.loading.small", "Loading small switch")
                                    .size(Size::Small)
                                    .loading(self.loading)
                                    .on_change(cx.listener(|this, _: &SwitchChange, _, cx| {
                                        this.changes += 1;
                                        cx.notify();
                                    })),
                            ),
                        ),
                )
                .child(
                    div()
                        .id("switch.loading.toggle")
                        .debug_selector(|| "switch.loading.toggle".into())
                        .child(
                            Button::new("switch.loading.toggle")
                                .scope(self.scope.clone())
                                .automation_id("switch.loading.toggle")
                                .label(if self.loading {
                                    "Stop loading"
                                } else {
                                    "Start loading"
                                })
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.loading = !this.loading;
                                    cx.notify();
                                })),
                        ),
                )
                .into_any_element(),
            "component-token" => self.control(
                "switch.component-token",
                self.switch("switch.component-token", "Custom component token switch")
                    .scope(self.token_scope.clone())
                    .default_checked(true),
            ),
            "style-class" => self
                .column(cx)
                .child(
                    self.control(
                        "switch.styles.small",
                        self.switch("switch.styles.small", "Pink small switch")
                            .size(Size::Small)
                            .checked_label("on")
                            .unchecked_label("off")
                            .bg(Color::rgb(0xf5d2d2)),
                    ),
                )
                .child(
                    self.control(
                        "switch.styles.normal",
                        self.switch("switch.styles.normal", "Green standard switch")
                            .size(Size::Medium)
                            .checked_label("on")
                            .unchecked_label("off")
                            .bg(Color::rgb(0xbde3c3))
                            .content_style(
                                StyleRefinement::default()
                                    .text_color(Color::rgb(0x225833))
                                    .font_weight(FontWeight::SEMIBOLD),
                            ),
                    ),
                )
                .child(
                    self.control(
                        "switch.styles.indicator",
                        self.switch("switch.styles.indicator", "Custom indicator switch")
                            .default_checked(true)
                            .indicator_style(
                                StyleRefinement::default()
                                    .bg(Color::rgb(0xe6f4ff))
                                    .border_1()
                                    .border_color(Color::rgb(0x91caff)),
                            ),
                    ),
                )
                .into_any_element(),
            _ => unreachable!("registered Switch demo"),
        }
    }
}

impl Render for SwitchPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let viewport = window.viewport_size().width;
        let content = viewport - sidebar_width(viewport) - page_padding(viewport) * 2.;
        div()
            .id("gallery.switch.examples")
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
                    .debug_selector(|| format!("gallery.switch.card.{}", demo.id))
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
                            .debug_selector(|| format!("gallery.switch.preview.{}", demo.id))
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
