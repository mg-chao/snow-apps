// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Alert, Button, ConfigPatch, ConfigScope, Direction, Size, Switch, SwitchChange,
    alert::{
        AlertConfigPatch, AlertPadding, AlertStyles, AlertTheme, AlertTokenPatch, AlertType,
        AlertVariant,
    },
    behavior::focus_handle,
    button::ButtonType,
    icons::IconName,
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, Context, ElementId, FocusHandle, FontWeight, IntoElement, Render, Role,
    ScrollAnchor, StyleRefinement, Subscription, Window, div, prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy)]
pub(super) enum Preview {
    Basic,
    Types,
    Filled,
    Closable,
    Description,
    Icon,
    Banner,
    SmoothClose,
    CustomIcon,
    Action,
    TitleAlignment,
    ComponentToken,
    SemanticStyles,
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub preview: Preview,
}

// Keep the order from components/alert/index.en-US.md. Loop Banner requires the
// external marquee package; React ErrorBoundary has no native GPUI equivalent.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "alert-basic",
        title: "Basic",
        description: "The simplest usage for short messages.",
        preview: Preview::Basic,
    },
    Demo {
        id: "alert-style",
        title: "More types",
        description: "There are four types of Alert: success, info, warning, and error.",
        preview: Preview::Types,
    },
    Demo {
        id: "alert-filled",
        title: "Filled",
        description: "The filled variant hides the border.",
        preview: Preview::Filled,
    },
    Demo {
        id: "alert-closable",
        title: "Closable",
        description: "Show a close button to dismiss the alert.",
        preview: Preview::Closable,
    },
    Demo {
        id: "alert-description",
        title: "Description",
        description: "Additional description for an alert message.",
        preview: Preview::Description,
    },
    Demo {
        id: "alert-icon",
        title: "Icon",
        description: "A relevant icon makes the message's status easier to recognize.",
        preview: Preview::Icon,
    },
    Demo {
        id: "alert-banner",
        title: "Banner",
        description: "Display an alert as a banner. Warning status and an icon are enabled by default.",
        preview: Preview::Banner,
    },
    Demo {
        id: "alert-smooth-closed",
        title: "Smoothly Unmount",
        description: "Smoothly unmount an alert after closing. Turn the switch on to mount a fresh alert.",
        preview: Preview::SmoothClose,
    },
    Demo {
        id: "alert-custom-icon",
        title: "Custom Icon",
        description: "Custom icons follow the status color and description size. showIcon controls their visibility.",
        preview: Preview::CustomIcon,
    },
    Demo {
        id: "alert-action",
        title: "Custom action",
        description: "Add actions beside the message. Action controls and the close button are independent.",
        preview: Preview::Action,
    },
    Demo {
        id: "alert-custom-title-alignment",
        title: "Custom title alignment",
        description: "Semantic styles align the icon, action, and close button with the first line of a wrapping title.",
        preview: Preview::TitleAlignment,
    },
    Demo {
        id: "alert-component-token",
        title: "Component Token",
        description: "A local component theme changes description padding and icon size.",
        preview: Preview::ComponentToken,
    },
    Demo {
        id: "alert-style-class",
        title: "Custom semantic dom styling",
        description: "Apply native styles to semantic slots, or resolve styles from the alert's properties.",
        preview: Preview::SemanticStyles,
    },
];

const TYPES: [(AlertType, &str); 4] = [
    (AlertType::Success, "Success"),
    (AlertType::Info, "Info"),
    (AlertType::Warning, "Warning"),
    (AlertType::Error, "Error"),
];
const ICON_TITLES: [&str; 4] = ["Success Tips", "Informational Notes", "Warning", "Error"];
const ICON_DESCRIPTIONS: [&str; 4] = [
    "Detailed description and advice about successful copywriting.",
    "Additional description and information about copywriting.",
    "This is a warning notice about copywriting.",
    "This is an error message about copywriting.",
];
const WRAPPING_TITLE: &str =
    "Long alert title wraps to multiple lines when the alert container is narrow enough.";

pub(super) struct AlertPage {
    pub scope: ConfigScope,
    token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub smooth_visible: bool,
    pub smooth_epoch: usize,
    pub close_requests: usize,
    pub close_completions: usize,
    pub last_action: Option<&'static str>,
    pub action_count: usize,
    pub smooth_close_focus: FocusHandle,
    _config: Subscription,
}

impl AlertPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<AlertTheme>(AlertTokenPatch {
            with_description_icon_size: Some(32.),
            with_description_padding: Some(AlertPadding::all(16.)),
            ..Default::default()
        });
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    alert: AlertConfigPatch {
                        success_icon: Some(IconName::SmileOutlined.into()),
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .expect("valid Alert component theme");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            anchors,
            smooth_visible: true,
            smooth_epoch: 0,
            close_requests: 0,
            close_completions: 0,
            last_action: None,
            action_count: 0,
            smooth_close_focus: focus_handle(cx),
        }
    }

    fn alert(&self, id: impl Into<ElementId>) -> Alert {
        let id = id.into();
        let automation_id = id.to_string();
        Alert::new(id)
            .scope(self.scope.clone())
            .automation_id(automation_id.clone())
            .close_automation_id(format!("{automation_id}.close"))
    }

    fn observed(&self, id: &'static str, alert: Alert) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .w_full()
            .min_w(px(0.))
            .child(alert)
            .into_any_element()
    }

    fn column(children: impl IntoIterator<Item = AnyElement>) -> AnyElement {
        div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .gap(px(16.))
            .children(children)
            .into_any_element()
    }

    fn action(&self, label: &'static str, cx: &Context<Self>) -> Button {
        Button::new(format!("alert.action.{label}"))
            .scope(self.scope.clone())
            .automation_id(format!("alert.action.{label}"))
            .label(label)
            .size(Size::Small)
            .on_click(cx.listener(move |this, _, _, cx| {
                this.last_action = Some(label);
                this.action_count += 1;
                cx.notify();
            }))
    }

    fn after_close(
        &self,
        smooth: bool,
        cx: &Context<Self>,
    ) -> impl Fn(&mut Window, &mut gpui::App) + 'static {
        let page = cx.weak_entity();
        move |_, cx| {
            let _ = page.update(cx, |this, cx| {
                this.close_completions += 1;
                if smooth {
                    this.smooth_visible = false;
                }
                cx.notify();
            });
        }
    }

    fn icons(&self, custom: bool) -> AnyElement {
        let mut alerts = Vec::with_capacity(if custom { 9 } else { 8 });
        if custom {
            alerts.push(
                self.alert("alert.custom-icon.hidden")
                    .title("showIcon = false")
                    .alert_type(AlertType::Success)
                    .icon(IconName::SmileOutlined)
                    .into_any_element(),
            );
        }
        for description in [false, true] {
            for (index, (kind, _)) in TYPES.into_iter().enumerate() {
                let mut alert = self
                    .alert(format!(
                        "alert.{}.{}.{}",
                        if custom { "custom-icon" } else { "icon" },
                        if description { "description" } else { "title" },
                        index
                    ))
                    .alert_type(kind)
                    .title(ICON_TITLES[index])
                    .show_icon(true)
                    .closable(!custom && kind == AlertType::Warning);
                if description {
                    alert = alert.description(ICON_DESCRIPTIONS[index]);
                }
                if custom {
                    alert = alert.icon(IconName::SmileOutlined);
                }
                alerts.push(alert.into_any_element());
            }
        }
        Self::column(alerts)
    }

    fn smooth_close(&self, cx: &Context<Self>) -> AnyElement {
        let mut column = div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .items_start()
            .gap(px(16.));
        if self.smooth_visible {
            column = column.child(
                self.observed(
                    "alert.smooth.item",
                    self.alert(("alert.smooth.item", self.smooth_epoch))
                        .automation_id("alert.smooth.item")
                        .close_automation_id("alert.smooth.item.close")
                        .title("Alert Message Text")
                        .alert_type(AlertType::Success)
                        .closable(true)
                        .close_focus_handle(&self.smooth_close_focus)
                        .on_close(cx.listener(|this, _, _, cx| {
                            this.close_requests += 1;
                            cx.notify();
                        }))
                        .after_close(self.after_close(true, cx)),
                ),
            );
        }
        column
            .child("click the close button to see the effect")
            .child(
                div()
                    .id("alert.smooth.toggle")
                    .debug_selector(|| "alert.smooth.toggle".into())
                    .flex()
                    .child(
                        Switch::new("alert.smooth.toggle")
                            .scope(self.scope.clone())
                            .automation_id("alert.smooth.toggle")
                            .accessibility_label("Alert visibility")
                            .checked(self.smooth_visible)
                            .disabled(self.smooth_visible)
                            .on_change(cx.listener(|this, change: &SwitchChange, _, cx| {
                                if change.checked {
                                    this.smooth_epoch += 1;
                                }
                                this.smooth_visible = change.checked;
                                cx.notify();
                            })),
                    ),
            )
            .into_any_element()
    }

    fn actions(&self, cx: &Context<Self>) -> AnyElement {
        let mut column = div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .gap(px(16.))
            .child(
                self.observed(
                    "alert.action.success",
                    self.alert("alert.action.success")
                        .title("Success Tips")
                        .alert_type(AlertType::Success)
                        .show_icon(true)
                        .closable(true)
                        .action(
                            div()
                                .id("alert.action.undo")
                                .debug_selector(|| "alert.action.undo".into())
                                .child(self.action("UNDO", cx).button_type(ButtonType::Text)),
                        ),
                ),
            )
            .child(
                self.alert("alert.action.error")
                    .title("Error Text")
                    .description(
                        "Error Description Error Description Error Description Error Description",
                    )
                    .alert_type(AlertType::Error)
                    .show_icon(true)
                    .action(self.action("Detail", cx).danger(true)),
            )
            .child(
                self.alert("alert.action.warning")
                    .title("Warning Text")
                    .alert_type(AlertType::Warning)
                    .closable(true)
                    .action(self.action("Done", cx).button_type(ButtonType::Text)),
            )
            .child(
                self.alert("alert.action.info")
                    .title("Info Text")
                    .description(
                        "Info Description Info Description Info Description Info Description",
                    )
                    .alert_type(AlertType::Info)
                    .closable(true)
                    .action(
                        div()
                            .min_w(px(80.))
                            .flex()
                            .flex_col()
                            .gap(px(8.))
                            .child(self.action("Accept", cx).primary().block(true))
                            .child(
                                self.action("Decline", cx)
                                    .danger(true)
                                    .ghost(true)
                                    .block(true),
                            ),
                    ),
            );
        if let Some(action) = self.last_action {
            column = column.child(
                div()
                    .text_color(self.scope.snapshot(cx).theme.tokens.color_text_secondary)
                    .child(format!("Last action: {action}")),
            );
        }
        column.into_any_element()
    }

    fn preview(&self, preview: Preview, cx: &Context<Self>) -> AnyElement {
        match preview {
            Preview::Basic => self.observed(
                "alert.basic.one",
                self.alert("alert.basic.one")
                    .title("Success Text")
                    .alert_type(AlertType::Success),
            ),
            Preview::Types => Self::column(TYPES.into_iter().map(|(kind, label)| {
                self.alert(format!("alert.type.{label}"))
                    .title(format!("{label} Text"))
                    .alert_type(kind)
                    .into_any_element()
            })),
            Preview::Filled => self
                .alert("alert.filled.one")
                .title("Info Text")
                .alert_type(AlertType::Info)
                .variant(AlertVariant::Filled)
                .into_any_element(),
            Preview::Closable => {
                Self::column([TYPES[2], TYPES[0], TYPES[1], TYPES[3]].into_iter().map(
                    |(kind, label)| {
                        self.alert(format!("alert.closable.{label}"))
                            .title(format!("{label} Title"))
                            .alert_type(kind)
                            .closable(true)
                            .on_close(cx.listener(|this, _, _, cx| {
                                this.close_requests += 1;
                                cx.notify();
                            }))
                            .after_close(self.after_close(false, cx))
                            .into_any_element()
                    },
                ))
            }
            Preview::Description => Self::column(TYPES.into_iter().map(|(kind, label)| {
                self.alert(format!("alert.description.{label}"))
                    .title(format!("{label} Text"))
                    .description(
                        std::iter::repeat_n(
                            format!("{label} Description"),
                            if kind == AlertType::Success { 3 } else { 4 },
                        )
                        .collect::<Vec<_>>()
                        .join(" "),
                    )
                    .alert_type(kind)
                    .into_any_element()
            })),
            Preview::Icon => self.icons(false),
            Preview::Banner => Self::column([
                self.alert("alert.banner.default")
                    .title("Warning text")
                    .banner(true)
                    .into_any_element(),
                self.alert("alert.banner.long")
                    .title("Very long warning text warning text text text text text text text")
                    .banner(true)
                    .closable(true)
                    .into_any_element(),
                self.alert("alert.banner.hidden")
                    .title("Warning text without icon")
                    .banner(true)
                    .show_icon(false)
                    .into_any_element(),
                self.alert("alert.banner.error")
                    .title("Error text")
                    .banner(true)
                    .alert_type(AlertType::Error)
                    .into_any_element(),
            ]),
            Preview::SmoothClose => self.smooth_close(cx),
            Preview::CustomIcon => self.icons(true),
            Preview::Action => self.actions(cx),
            Preview::TitleAlignment => {
                let first_line_styles = AlertStyles {
                    root: StyleRefinement::default().items_start(),
                    icon: StyleRefinement::default().mt(px(4.)),
                    actions: StyleRefinement::default().mt(px(-1.)),
                    close: StyleRefinement::default().mt(px(5.)),
                    ..Default::default()
                };
                div()
                    .w_full()
                    .max_w(px(360.))
                    .flex()
                    .flex_col()
                    .gap(px(16.))
                    .child(
                        self.alert("alert.alignment.info")
                            .title(WRAPPING_TITLE)
                            .alert_type(AlertType::Info)
                            .show_icon(true)
                            .closable(true)
                            .styles(first_line_styles.clone()),
                    )
                    .child(
                        self.alert("alert.alignment.success")
                            .title(WRAPPING_TITLE)
                            .alert_type(AlertType::Success)
                            .show_icon(true)
                            .closable(true)
                            .styles(first_line_styles)
                            .action(self.action("Action", cx).button_type(ButtonType::Text)),
                    )
                    .into_any_element()
            }
            Preview::ComponentToken => self
                .alert("alert.token.one")
                .scope(self.token_scope.clone())
                .title("Success Tips")
                .description(ICON_DESCRIPTIONS[0])
                .alert_type(AlertType::Success)
                .show_icon(true)
                .into_any_element(),
            Preview::SemanticStyles => {
                let root = StyleRefinement::default()
                    .border_2()
                    .border_dashed()
                    .border_color(Color::rgb(0xcccccc))
                    .rounded(px(8.))
                    .p(px(12.));
                Self::column([
                    self.alert("alert.semantic.object")
                        .title("Object styles")
                        .show_icon(true)
                        .styles(AlertStyles {
                            root: root.clone(),
                            icon: StyleRefinement::default().text_size(px(18.)),
                            section: StyleRefinement::default().font_weight(FontWeight::MEDIUM),
                            ..Default::default()
                        })
                        .action(self.action("Action", cx))
                        .into_any_element(),
                    self.alert("alert.semantic.function")
                        .title("Function styles")
                        .alert_type(AlertType::Success)
                        .show_icon(true)
                        .styles(AlertStyles {
                            root,
                            ..Default::default()
                        })
                        .styles_fn(|context, _, _| match context.kind {
                            AlertType::Success => AlertStyles {
                                root: StyleRefinement::default()
                                    .bg(Color::rgb(0x52c41a).with_alpha(0.1))
                                    .border_color(Color::rgb(0xb7eb8f)),
                                icon: StyleRefinement::default().text_color(Color::rgb(0x52c41a)),
                                ..Default::default()
                            },
                            AlertType::Warning => AlertStyles {
                                root: StyleRefinement::default()
                                    .bg(Color::rgb(0xfaad14).with_alpha(0.1))
                                    .border_color(Color::rgb(0xffe58f)),
                                icon: StyleRefinement::default().text_color(Color::rgb(0xfaad14)),
                                ..Default::default()
                            },
                            _ => AlertStyles::default(),
                        })
                        .into_any_element(),
                ])
            }
        }
    }
}

impl Render for AlertPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.alert.examples")
            .w_full()
            .grid()
            .grid_cols(if content_width >= px(840.) { 2 } else { 1 })
            .items_start()
            .when(config.direction == Direction::Rtl, |page| page.text_right())
            .gap(px(tokens.margin_lg as f32))
            .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                div()
                    .id(demo.id)
                    .anchor_scroll(Some(self.anchors[index].clone()))
                    .role(Role::Group)
                    .aria_label(demo.title)
                    .debug_selector(|| format!("gallery.card.{}", demo.id))
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
                            .debug_selector(|| format!("gallery.preview.{}", demo.id))
                            .min_h(px(120.))
                            .p(px(tokens.padding_lg as f32))
                            .flex()
                            .items_center()
                            .child(self.preview(demo.preview, cx)),
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
