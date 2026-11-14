// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Result, ResultStatus,
    behavior::focus_handle,
    button::ButtonType,
    icons::{IconName, IconRenderOptions, icon},
    result::{ResultMargin, ResultStyles, ResultTheme, ResultTokenPatch},
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, Context, FocusHandle, FontWeight, IntoElement, Render, Role, ScrollAnchor,
    StyleRefinement, Subscription, Window, div, prelude::*, px, relative, size,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy)]
pub(super) enum Preview {
    Success,
    Info,
    Warning,
    Forbidden,
    NotFound,
    InternalServerError,
    Error,
    CustomIcon,
    SemanticStyles,
    ComponentToken,
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub preview: Preview,
}

// Keep the public examples in components/result/index.en-US.md order.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "result-success",
        title: "Success",
        description: "Show successful results.",
        preview: Preview::Success,
    },
    Demo {
        id: "result-info",
        title: "Info",
        description: "Show processing results.",
        preview: Preview::Info,
    },
    Demo {
        id: "result-warning",
        title: "Warning",
        description: "The result of the warning.",
        preview: Preview::Warning,
    },
    Demo {
        id: "result-403",
        title: "403",
        description: "You are not authorized to access this page.",
        preview: Preview::Forbidden,
    },
    Demo {
        id: "result-404",
        title: "404",
        description: "The page you visited does not exist.",
        preview: Preview::NotFound,
    },
    Demo {
        id: "result-500",
        title: "500",
        description: "Something went wrong on server.",
        preview: Preview::InternalServerError,
    },
    Demo {
        id: "result-error",
        title: "Error",
        description: "Complex error feedback.",
        preview: Preview::Error,
    },
    Demo {
        id: "result-customIcon",
        title: "Custom icon",
        description: "Custom icon.",
        preview: Preview::CustomIcon,
    },
    Demo {
        id: "result-style-class",
        title: "Custom semantic dom styling",
        description: "Customize Result's semantic slots with native styles or a resolver based on its properties.",
        preview: Preview::SemanticStyles,
    },
    Demo {
        id: "result-component-token",
        title: "Component Token",
        description: "Component Token Debug.",
        preview: Preview::ComponentToken,
    },
];

const SUCCESS_TITLE: &str = "Successfully Purchased Cloud Server ECS!";
const SUCCESS_SUBTITLE: &str =
    "Order number: 2017182818828182881 Cloud server configuration takes 1-5 minutes, please wait.";

pub(super) struct ResultPage {
    pub scope: ConfigScope,
    token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub last_action: Option<&'static str>,
    pub action_count: usize,
    pub console_focus: FocusHandle,
    _config: Subscription,
}

impl ResultPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<ResultTheme>(ResultTokenPatch {
            title_font_size: Some(18.),
            subtitle_font_size: Some(14.),
            icon_font_size: Some(48.),
            extra_margin: Some(ResultMargin::new(12., 0., 0., 0.)),
        });
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid Result component theme");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            anchors,
            last_action: None,
            action_count: 0,
            console_focus: focus_handle(cx),
        }
    }

    fn result(&self, id: &'static str) -> Result {
        Result::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .w_full()
            .min_w(px(0.))
    }

    fn action(
        &self,
        id: &'static str,
        label: &'static str,
        primary: bool,
        cx: &Context<Self>,
    ) -> AnyElement {
        self.action_with_type(
            id,
            label,
            if primary {
                ButtonType::Primary
            } else {
                ButtonType::Default
            },
            cx,
        )
    }

    fn action_with_type(
        &self,
        id: &'static str,
        label: &'static str,
        button_type: ButtonType,
        cx: &Context<Self>,
    ) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let line_height = config.theme.tokens.font_size * config.theme.tokens.line_height;
        let button = Button::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .label(label)
            .when(id == "result.success.console", |button| {
                button.focus_handle(&self.console_focus)
            })
            .button_type(button_type)
            .when(button_type == ButtonType::Link, |button| {
                button.px(px(0.)).h(px(line_height as f32))
            })
            .on_click(cx.listener(move |this, _, _, cx| {
                this.last_action = Some(label);
                this.action_count += 1;
                cx.notify();
            }));
        div()
            .id(id)
            .debug_selector(move || id.into())
            .flex()
            .flex_shrink_0()
            .child(button)
            .into_any_element()
    }

    fn success(&self, id: &'static str, token: bool, cx: &Context<Self>) -> Result {
        self.result(id)
            .scope(if token {
                self.token_scope.clone()
            } else {
                self.scope.clone()
            })
            .status(ResultStatus::Success)
            .title(SUCCESS_TITLE)
            .sub_title(SUCCESS_SUBTITLE)
            .extra(self.action(
                if token {
                    "result.token.console"
                } else {
                    "result.success.console"
                },
                "Go Console",
                true,
                cx,
            ))
            .extra(self.action(
                if token {
                    "result.token.buy"
                } else {
                    "result.success.buy"
                },
                "Buy Again",
                false,
                cx,
            ))
    }

    fn error_body(&self, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let error_icon = || {
            let frame = px(g.font_size as f32);
            let artwork = frame * (8. / 7.);
            let offset = (frame - artwork) / 2.;
            div()
                .relative()
                .w(frame)
                .h(frame)
                .flex_shrink_0()
                .text_color(g.color_error)
                .child(
                    icon(IconName::CloseCircleOutlined)
                        .render_options(IconRenderOptions {
                            size: size(artwork, artwork),
                            colors: config.icon_palette,
                            ..Default::default()
                        })
                        .absolute()
                        .left(offset)
                        .top(offset)
                        .inherit_color(),
                )
        };
        let row = |message: &'static str, action: AnyElement| {
            div()
                .w_full()
                .flex()
                .flex_wrap()
                .items_center()
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse())
                .gap(px(g.margin_xs as f32))
                .child(error_icon())
                .child(message)
                .child(action)
        };
        div()
            .id("result.error.details")
            .debug_selector(|| "result.error.details".into())
            .w_full()
            .flex()
            .flex_col()
            .gap(px(g.font_size as f32))
            .pb(px(g.font_size as f32))
            .child(
                div()
                    .text_size(px(16.))
                    .font_weight(FontWeight::SEMIBOLD)
                    .child("The content you submitted has the following error:"),
            )
            .child(row(
                "Your account has been frozen.",
                self.action_with_type(
                    "result.error.thaw",
                    "Thaw immediately >",
                    ButtonType::Link,
                    cx,
                ),
            ))
            .child(row(
                "Your account is not yet eligible to apply.",
                self.action_with_type(
                    "result.error.unlock",
                    "Apply Unlock >",
                    ButtonType::Link,
                    cx,
                ),
            ))
            .into_any_element()
    }

    fn preview(&self, preview: Preview, cx: &Context<Self>) -> AnyElement {
        match preview {
            Preview::Success => self.success("result.success", false, cx).into_any_element(),
            Preview::Info => self
                .result("result.info")
                .title("Your operation has been executed")
                .extra(self.action("result.info.console", "Go Console", true, cx))
                .into_any_element(),
            Preview::Warning => self
                .result("result.warning")
                .status(ResultStatus::Warning)
                .title("There are some problems with your operation.")
                .extra(self.action("result.warning.console", "Go Console", true, cx))
                .into_any_element(),
            Preview::Forbidden | Preview::NotFound | Preview::InternalServerError => {
                let (id, status, title, subtitle, action) = match preview {
                    Preview::Forbidden => (
                        "result.403",
                        ResultStatus::Forbidden,
                        "403",
                        "Sorry, you are not authorized to access this page.",
                        "result.403.home",
                    ),
                    Preview::NotFound => (
                        "result.404",
                        ResultStatus::NotFound,
                        "404",
                        "Sorry, the page you visited does not exist.",
                        "result.404.home",
                    ),
                    _ => (
                        "result.500",
                        ResultStatus::InternalServerError,
                        "500",
                        "Sorry, something went wrong.",
                        "result.500.home",
                    ),
                };
                self.result(id)
                    .status(status)
                    .title(title)
                    .sub_title(subtitle)
                    .extra(self.action(action, "Back Home", true, cx))
                    .into_any_element()
            }
            Preview::Error => self
                .result("result.error")
                .status(ResultStatus::Error)
                .title("Submission Failed")
                .sub_title("Please check and modify the following information before resubmitting.")
                .extra(self.action("result.error.console", "Go Console", true, cx))
                .extra(self.action("result.error.buy", "Buy Again", false, cx))
                .child(self.error_body(cx))
                .into_any_element(),
            Preview::CustomIcon => self
                .result("result.customIcon")
                .icon(IconName::SmileOutlined)
                .title("Great, we have done all the operations!")
                .extra(self.action("result.custom.next", "Next", true, cx))
                .into_any_element(),
            Preview::ComponentToken => self.success("result.token", true, cx).into_any_element(),
            Preview::SemanticStyles => {
                let root = StyleRefinement::default()
                    .border_2()
                    .border_dashed()
                    .p(px(16.));
                div()
                    .w_full()
                    .min_w(px(0.))
                    .flex()
                    .flex_col()
                    .gap(px(24.))
                    .child(
                        self.result("result.semantic.object")
                            .title("classNames Object")
                            .sub_title("This is a subtitle")
                            .styles(ResultStyles {
                                root: root.clone(),
                                title: StyleRefinement::default()
                                    .italic()
                                    .text_color(Color::rgb(0x1890ff)),
                                sub_title: StyleRefinement::default().font_weight(FontWeight::BOLD),
                                icon: StyleRefinement::default().opacity(0.8),
                                extra: StyleRefinement::default()
                                    .bg(Color::rgb(0xf0f0f0))
                                    .p(px(8.)),
                                body: StyleRefinement::default()
                                    .bg(Color::rgb(0xfafafa))
                                    .p(px(12.)),
                            })
                            .extra(self.action("result.semantic.object.action", "Action", true, cx))
                            .child("Content area"),
                    )
                    .child(
                        self.result("result.semantic.function")
                            .status(ResultStatus::Success)
                            .title("classNames Function")
                            .sub_title("Dynamic class names")
                            .styles(ResultStyles {
                                root,
                                ..Default::default()
                            })
                            .styles_fn(|context, _, _| {
                                let (background, color) = if context.status == ResultStatus::Error {
                                    (0xfff2f0, 0xff4d4f)
                                } else {
                                    (0xf6ffed, 0x52c41a)
                                };
                                ResultStyles {
                                    root: StyleRefinement::default()
                                        .bg(Color::rgb(background))
                                        .border_color(Color::rgb(color)),
                                    title: StyleRefinement::default().text_color(Color::rgb(color)),
                                    ..Default::default()
                                }
                            })
                            .extra(self.action(
                                "result.semantic.function.action",
                                "Action",
                                false,
                                cx,
                            )),
                    )
                    .into_any_element()
            }
        }
    }
}

impl Render for ResultPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.result.examples")
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
                            .min_w(px(0.))
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
