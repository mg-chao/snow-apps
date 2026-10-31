// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, Checkbox, ConfigScope, Direction, Size,
    checkbox::{CheckboxChange, CheckboxGroup, CheckboxOption},
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

// Preserve the public example order from components/checkbox/index.en-US.md.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "basic",
        title: "Basic",
        description: "Basic usage of checkbox.",
    },
    Demo {
        id: "disabled",
        title: "Disabled",
        description: "Disabled checkbox.",
    },
    Demo {
        id: "controller",
        title: "Controlled Checkbox",
        description: "Communicated with other components.",
    },
    Demo {
        id: "group",
        title: "Checkbox Group",
        description: "Generate a group of checkboxes from an array.",
    },
    Demo {
        id: "check-all",
        title: "Check all",
        description: "The indeterminate property can help you to achieve a 'check all' effect.",
    },
    Demo {
        id: "layout",
        title: "Use with Grid",
        description: "Use Checkbox.Group with GPUI's native grid to implement complex layout.",
    },
    Demo {
        id: "style-class",
        title: "Custom semantic dom styling",
        description: "Customize the root, icon and label through native Styled refinements and semantic styles.",
    },
];

pub(super) struct CheckboxPage {
    pub scope: ConfigScope,
    pub checked: bool,
    pub disabled: bool,
    pub checked_list: Vec<&'static str>,
    pub semantic_checked: bool,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl CheckboxPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        Self {
            _config: scope.observe(cx),
            scope,
            checked: true,
            disabled: false,
            checked_list: vec!["Apple", "Orange"],
            semantic_checked: true,
            anchors,
        }
    }

    fn checkbox(&self, id: &'static str, label: &'static str) -> Checkbox {
        Checkbox::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .label(label)
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        let config = self.scope.snapshot(cx);
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(config.theme.tokens.margin_xs as f32))
            .when(config.direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn column(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .w_full()
            .flex()
            .flex_col()
            .gap(px(self.scope.snapshot(cx).theme.tokens.margin as f32))
    }

    fn fruit_options() -> [CheckboxOption<&'static str>; 3] {
        ["Apple", "Pear", "Orange"].map(|fruit| CheckboxOption::new(fruit, fruit, fruit))
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "basic" => self
                .checkbox("checkbox.basic", "Checkbox")
                .into_any_element(),
            "disabled" => self
                .row(cx)
                .child(
                    self.checkbox("checkbox.disabled.unchecked", "Checkbox")
                        .disabled(true),
                )
                .child(
                    self.checkbox("checkbox.disabled.checked", "Checkbox")
                        .default_checked(true)
                        .disabled(true),
                )
                .into_any_element(),
            "controller" => self
                .column(cx)
                .child(
                    div()
                        .id("checkbox.controller")
                        .debug_selector(|| "checkbox.controller".into())
                        .child(
                            Checkbox::new("control")
                                .scope(self.scope.clone())
                                .automation_id("checkbox.controller")
                                .label(format!(
                                    "{}-{}",
                                    if self.checked { "Checked" } else { "Unchecked" },
                                    if self.disabled { "Disabled" } else { "Enabled" },
                                ))
                                .checked(self.checked)
                                .disabled(self.disabled)
                                .on_change(cx.listener(|this, event: &CheckboxChange, _, cx| {
                                    this.checked = event.checked;
                                    cx.notify();
                                })),
                        ),
                )
                .child(
                    self.row(cx)
                        .child(
                            div()
                                .id("checkbox.controller.toggle")
                                .debug_selector(|| "checkbox.controller.toggle".into())
                                .child(
                                    Button::new("toggle")
                                        .scope(self.scope.clone())
                                        .primary()
                                        .size(Size::Small)
                                        .label(if self.checked { "Uncheck" } else { "Check" })
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            this.checked = !this.checked;
                                            cx.notify();
                                        })),
                                ),
                        )
                        .child(
                            div()
                                .id("checkbox.controller.disable")
                                .debug_selector(|| "checkbox.controller.disable".into())
                                .child(
                                    Button::new("disable")
                                        .scope(self.scope.clone())
                                        .primary()
                                        .size(Size::Small)
                                        .label(if self.disabled { "Enable" } else { "Disable" })
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            this.disabled = !this.disabled;
                                            cx.notify();
                                        })),
                                ),
                        ),
                )
                .into_any_element(),
            "group" => self
                .column(cx)
                .child(
                    CheckboxGroup::new("checkbox.group.plain")
                        .scope(self.scope.clone())
                        .accessibility_label("Fruit choices")
                        .options(Self::fruit_options())
                        .default_value(["Apple"]),
                )
                .child(
                    CheckboxGroup::new("checkbox.group.options")
                        .scope(self.scope.clone())
                        .accessibility_label("Fruit options")
                        .options(Self::fruit_options())
                        .default_value(["Pear"]),
                )
                .child(
                    CheckboxGroup::new("checkbox.group.disabled")
                        .scope(self.scope.clone())
                        .accessibility_label("Disabled group with enabled Orange option")
                        .options([
                            CheckboxOption::new("apple", "Apple", "Apple"),
                            CheckboxOption::new("pear", "Pear", "Pear"),
                            CheckboxOption::new("orange", "Orange", "Orange").disabled(false),
                        ])
                        .default_value(["Apple"])
                        .disabled(true),
                )
                .into_any_element(),
            "check-all" => {
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                self.column(cx)
                    .child(
                        div()
                            .id("checkbox.check-all")
                            .debug_selector(|| "checkbox.check-all".into())
                            .child(
                                self.checkbox("checkbox.check-all.control", "Check all")
                                    .checked(self.checked_list.len() == 3)
                                    .indeterminate(
                                        !self.checked_list.is_empty()
                                            && self.checked_list.len() < 3,
                                    )
                                    .on_change(cx.listener(
                                        |this, event: &CheckboxChange, _, cx| {
                                            this.checked_list = if event.checked {
                                                vec!["Apple", "Pear", "Orange"]
                                            } else {
                                                Vec::new()
                                            };
                                            cx.notify();
                                        },
                                    )),
                            ),
                    )
                    .child(div().border_t_1().border_color(tokens.color_split))
                    .child(
                        CheckboxGroup::new("checkbox.check-all.group")
                            .scope(self.scope.clone())
                            .accessibility_label("Select fruit")
                            .options(Self::fruit_options())
                            .value(self.checked_list.iter().copied())
                            .on_change(cx.listener(|this, values: &[&'static str], _, cx| {
                                this.checked_list = values.to_vec();
                                cx.notify();
                            })),
                    )
                    .into_any_element()
            }
            "layout" => CheckboxGroup::new("checkbox.layout")
                .scope(self.scope.clone())
                .accessibility_label("Grid choices")
                .options(
                    ["A", "B", "C", "D", "E"].map(|label| CheckboxOption::new(label, label, label)),
                )
                .w_full()
                .grid()
                .grid_cols(3)
                .into_any_element(),
            "style-class" => {
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                let warning = tokens.color_warning;
                let mut icon_style = StyleRefinement::default().border_color(warning);
                if self.semantic_checked {
                    icon_style = icon_style.bg(warning);
                }
                self.column(cx)
                    .child(
                        self.checkbox("checkbox.styles.object", "Object styles")
                            .icon_style(StyleRefinement::default().rounded(px(6.)))
                            .label_style(
                                StyleRefinement::default().text_color(Color::rgb(0x0000ff)),
                            ),
                    )
                    .child(
                        self.checkbox("checkbox.styles.function", "Function styles")
                            .checked(self.semantic_checked)
                            .rounded(px(tokens.border_radius as f32))
                            .bg(tokens.color_bg_container)
                            .icon_style(icon_style)
                            .label_style(
                                StyleRefinement::default()
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(if self.semantic_checked {
                                        warning
                                    } else {
                                        tokens.color_text_disabled
                                    }),
                            )
                            .on_change(cx.listener(|this, event: &CheckboxChange, _, cx| {
                                this.semantic_checked = event.checked;
                                cx.notify();
                            })),
                    )
                    .into_any_element()
            }
            _ => unreachable!("registered checkbox demo"),
        }
    }
}

impl Render for CheckboxPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let viewport = window.viewport_size().width;
        let content = viewport - sidebar_width(viewport) - page_padding(viewport) * 2.;
        div()
            .id("gallery.checkbox.examples")
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
                    .debug_selector(|| format!("gallery.checkbox.card.{}", demo.id))
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
                            .debug_selector(|| format!("gallery.checkbox.preview.{}", demo.id))
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
