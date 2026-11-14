// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Radio, RadioGroup, RadioOption, Size,
    radio::{RadioButtonStyle, RadioChange, RadioOptionType, RadioTheme, RadioTokenPatch},
    theme::{Color, ThemeConfig},
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

// Public demos follow components/radio/index.en-US.md. Component tokens follow them.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "radio-basic",
        title: "Basic",
        description: "Basic usage of a radio button.",
    },
    Demo {
        id: "radio-disabled",
        title: "Disabled",
        description: "A disabled radio cannot be selected. Toggle availability to try both states.",
    },
    Demo {
        id: "radio-group",
        title: "Radio Group",
        description: "A controlled group maintains a single selection. Selecting another option updates the value.",
    },
    Demo {
        id: "radio-vertical",
        title: "Vertical Radio.Group",
        description: "Stack standard radios or radio buttons vertically using native GPUI layout.",
    },
    Demo {
        id: "radio-block",
        title: "Block Radio.Group",
        description: "Fill the parent width and distribute the options evenly.",
    },
    Demo {
        id: "radio-options",
        title: "Radio.Group group - optional",
        description: "Build groups from typed options, with optional disabled choices and button appearances.",
    },
    Demo {
        id: "radio-button",
        title: "Radio style",
        description: "Radio buttons retain single-selection behavior with connected borders and rounded outer corners.",
    },
    Demo {
        id: "radio-keyboard",
        title: "Radio.Group with name",
        description: "A native radio group provides shared selection semantics. Tab enters the selected option; arrow keys move and select, skipping disabled options.",
    },
    Demo {
        id: "radio-size",
        title: "Size",
        description: "Radio buttons come in large, medium and small sizes. Standard circular radios retain their own token size.",
    },
    Demo {
        id: "radio-solid",
        title: "Solid radio button",
        description: "A solid background emphasizes the selected radio button.",
    },
    Demo {
        id: "radio-style-class",
        title: "Custom semantic dom styling",
        description: "Refine the root, icon and label independently, including styles derived from the controlled selection.",
    },
    Demo {
        id: "radio-component-token",
        title: "Component Token",
        description: "Customize radio and dot sizes, wrapper spacing and button padding in a local configuration scope.",
    },
];

pub(super) struct RadioPage {
    pub scope: ConfigScope,
    pub disabled: bool,
    pub value: u8,
    pub semantic_value: &'static str,
    token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl RadioPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<RadioTheme>(RadioTokenPatch {
            radio_size: Some(20.),
            dot_size: Some(10.),
            button_padding_inline: Some(20.),
            wrapper_margin_inline_end: Some(20.),
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
            .expect("valid Radio component token customization");
        Self {
            _config: scope.observe(cx),
            scope,
            disabled: true,
            value: 1,
            semantic_value: "object",
            token_scope,
            anchors,
        }
    }

    fn radio(&self, id: &'static str, label: &'static str) -> Radio {
        Radio::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .label(label)
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        let config = self.scope.snapshot(cx);
        div()
            .flex()
            .flex_wrap()
            .items_start()
            .gap(px(config.theme.tokens.margin as f32))
            .when(config.direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn column(&self, cx: &Context<Self>) -> gpui::Div {
        div().w_full().min_w(px(0.)).flex().flex_col().gap(px(self
            .scope
            .snapshot(cx)
            .theme
            .tokens
            .margin as f32))
    }

    fn fruit_options(disabled: bool) -> [RadioOption<&'static str>; 3] {
        [
            RadioOption::new("apple", "Apple", "Apple"),
            RadioOption::new("pear", "Pear", "Pear"),
            RadioOption::new("orange", "Orange", "Orange").disabled(disabled),
        ]
    }

    fn cities(&self, id: &'static str) -> RadioGroup<&'static str> {
        RadioGroup::new(id)
            .scope(self.scope.clone())
            .accessibility_label("City")
            .options(
                ["Hangzhou", "Shanghai", "Beijing", "Chengdu"]
                    .map(|city| RadioOption::new(city, city, city)),
            )
            .default_value("Hangzhou")
            .option_type(RadioOptionType::Button)
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "radio-basic" => self.radio("radio.basic", "Radio").into_any_element(),
            "radio-disabled" => self
                .column(cx)
                .child(
                    self.row(cx)
                        .child(
                            self.radio("radio.disabled.empty", "Disabled")
                                .disabled(self.disabled),
                        )
                        .child(
                            self.radio("radio.disabled.checked", "Disabled")
                                .default_checked(true)
                                .disabled(self.disabled),
                        ),
                )
                .child(
                    div()
                        .id("radio.disabled.toggle")
                        .debug_selector(|| "radio.disabled.toggle".into())
                        .flex()
                        .when_else(
                            self.scope.snapshot(cx).direction == Direction::Rtl,
                            |row| row.self_end().flex_row_reverse(),
                            |row| row.self_start(),
                        )
                        .child(
                            Button::new("toggle")
                                .scope(self.scope.clone())
                                .primary()
                                .label("Toggle disabled")
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.disabled = !this.disabled;
                                    cx.notify();
                                })),
                        ),
                )
                .into_any_element(),
            "radio-group" => self
                .column(cx)
                .child(
                    div()
                        .id("radio.controlled")
                        .debug_selector(|| "radio.controlled".into())
                        .child(
                            RadioGroup::new("radio.group")
                                .scope(self.scope.clone())
                                .accessibility_label("Choose an option")
                                .options([1, 2, 3, 4].map(|value| {
                                    RadioOption::new(
                                        ("option", usize::from(value)),
                                        value,
                                        ["A", "B", "C", "D"][usize::from(value - 1)],
                                    )
                                    .automation_id(format!("radio.controlled.{value}"))
                                }))
                                .value(self.value)
                                .on_change(cx.listener(|this, value: &u8, _, cx| {
                                    this.value = *value;
                                    cx.notify();
                                })),
                        ),
                )
                .child(
                    div()
                        .text_color(self.scope.snapshot(cx).theme.tokens.color_text_secondary)
                        .child(format!(
                            "Selected: {}",
                            ["A", "B", "C", "D"][usize::from(self.value - 1)]
                        )),
                )
                .into_any_element(),
            "radio-vertical" => self
                .row(cx)
                .child(
                    RadioGroup::new("radio.vertical.standard")
                        .scope(self.scope.clone())
                        .accessibility_label("Vertical options")
                        .options(["Option A", "Option B", "Option C", "More…"].map(|label| {
                            RadioOption::new(label, label, label)
                                .style(StyleRefinement::default().h(px(32.)))
                        }))
                        .default_value("Option A")
                        .vertical(true),
                )
                .child(
                    RadioGroup::new("radio.vertical.button")
                        .scope(self.scope.clone())
                        .accessibility_label("Vertical fruit")
                        .options(Self::fruit_options(false))
                        .default_value("Apple")
                        .option_type(RadioOptionType::Button)
                        .vertical(true),
                )
                .into_any_element(),
            "radio-block" => self
                .column(cx)
                .children(
                    [
                        (RadioOptionType::Default, RadioButtonStyle::Outline),
                        (RadioOptionType::Button, RadioButtonStyle::Solid),
                        (RadioOptionType::Button, RadioButtonStyle::Outline),
                    ]
                    .into_iter()
                    .enumerate()
                    .map(|(index, (option_type, button_style))| {
                        RadioGroup::new(("radio.block", index))
                            .scope(self.scope.clone())
                            .accessibility_label("Full-width fruit choices")
                            .options(Self::fruit_options(false))
                            .default_value("Apple")
                            .block(true)
                            .option_type(option_type)
                            .button_style(button_style)
                    }),
                )
                .into_any_element(),
            "radio-options" => self
                .column(cx)
                .children(
                    [
                        (RadioOptionType::Default, RadioButtonStyle::Outline, false),
                        (RadioOptionType::Default, RadioButtonStyle::Outline, true),
                        (RadioOptionType::Button, RadioButtonStyle::Outline, false),
                        (RadioOptionType::Button, RadioButtonStyle::Solid, true),
                    ]
                    .into_iter()
                    .enumerate()
                    .map(|(index, (option_type, button_style, disabled))| {
                        RadioGroup::new(("radio.options", index))
                            .scope(self.scope.clone())
                            .accessibility_label("Fruit choices")
                            .options(Self::fruit_options(disabled))
                            .default_value("Apple")
                            .option_type(option_type)
                            .button_style(button_style)
                    }),
                )
                .into_any_element(),
            "radio-button" => self
                .column(cx)
                .child(self.cities("radio.button.normal"))
                .child(
                    RadioGroup::new("radio.button.partial")
                        .scope(self.scope.clone())
                        .accessibility_label("City with Shanghai unavailable")
                        .options(["Hangzhou", "Shanghai", "Beijing", "Chengdu"].map(|city| {
                            RadioOption::new(city, city, city).disabled(city == "Shanghai")
                        }))
                        .default_value("Hangzhou")
                        .option_type(RadioOptionType::Button),
                )
                .child(self.cities("radio.button.disabled").disabled(true))
                .into_any_element(),
            "radio-keyboard" => RadioGroup::new("radio.keyboard")
                .scope(self.scope.clone())
                .accessibility_label("Favorite fruit")
                .options(Self::fruit_options(false))
                .default_value("Apple")
                .into_any_element(),
            "radio-size" => self
                .column(cx)
                .children(
                    [Size::Large, Size::Medium, Size::Small]
                        .into_iter()
                        .enumerate()
                        .map(|(index, size)| {
                            self.cities(
                                ["radio.size.large", "radio.size.medium", "radio.size.small"]
                                    [index],
                            )
                            .size(size)
                        }),
                )
                .into_any_element(),
            "radio-solid" => self
                .column(cx)
                .children(
                    [Size::Large, Size::Medium, Size::Small]
                        .into_iter()
                        .enumerate()
                        .map(|(index, size)| {
                            self.cities(
                                [
                                    "radio.solid.large",
                                    "radio.solid.medium",
                                    "radio.solid.small",
                                ][index],
                            )
                            .size(size)
                            .button_style(RadioButtonStyle::Solid)
                        }),
                )
                .into_any_element(),
            "radio-style-class" => {
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                let checked = self.semantic_value == "function";
                let mut icon_style = StyleRefinement::default().border_color(tokens.color_warning);
                if checked {
                    icon_style = icon_style.bg(tokens.color_warning);
                }
                self.column(cx)
                    .child(
                        self.radio("radio.styles.object", "Object styles")
                            .checked(self.semantic_value == "object")
                            .icon_style(StyleRefinement::default().rounded(px(6.)))
                            .label_style(
                                StyleRefinement::default().text_color(Color::rgb(0x0000ff)),
                            )
                            .on_change(cx.listener(|this, _: &RadioChange, _, cx| {
                                this.semantic_value = "object";
                                cx.notify();
                            })),
                    )
                    .child(
                        self.radio("radio.styles.function", "Function styles")
                            .checked(checked)
                            .rounded(px(tokens.border_radius as f32))
                            .bg(tokens.color_bg_container)
                            .icon_style(icon_style)
                            .label_style(
                                StyleRefinement::default()
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(if checked {
                                        tokens.color_warning
                                    } else {
                                        tokens.color_text_disabled
                                    }),
                            )
                            .on_change(cx.listener(|this, _: &RadioChange, _, cx| {
                                this.semantic_value = "function";
                                cx.notify();
                            })),
                    )
                    .into_any_element()
            }
            "radio-component-token" => self
                .column(cx)
                .child(
                    Radio::new("radio.tokens.checked")
                        .scope(self.token_scope.clone())
                        .label("Custom size")
                        .default_checked(true),
                )
                .child(
                    Radio::new("radio.tokens.disabled")
                        .scope(self.token_scope.clone())
                        .label("Disabled")
                        .default_checked(true)
                        .disabled(true),
                )
                .child(
                    RadioGroup::new("radio.tokens.buttons")
                        .scope(self.token_scope.clone())
                        .accessibility_label("Custom fruit buttons")
                        .options(Self::fruit_options(false))
                        .default_value("Apple")
                        .option_type(RadioOptionType::Button),
                )
                .into_any_element(),
            _ => unreachable!("registered Radio demo"),
        }
    }
}

impl Render for RadioPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let viewport = window.viewport_size().width;
        let content = viewport - sidebar_width(viewport) - page_padding(viewport) * 2.;
        div()
            .id("gallery.radio.examples")
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
                    .debug_selector(|| format!("gallery.radio.card.{}", demo.id))
                    .min_w(px(0.))
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
                            .debug_selector(|| format!("gallery.radio.preview.{}", demo.id))
                            .min_h(px(120.))
                            .p(px(tokens.padding_lg as f32))
                            .flex()
                            .items_center()
                            .when(config.direction == Direction::Rtl, |preview| {
                                preview.flex_row_reverse()
                            })
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
