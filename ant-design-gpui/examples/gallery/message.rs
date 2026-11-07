// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, InputNumber, InputNumberState, Switch,
    SwitchChange,
    button::ButtonType,
    input_number::{ExactDecimal, InputNumberEvent, NumberOptions},
    message::{
        Message, MessageConfigPatch, MessageHost, MessageOptions, MessageStack, MessageState,
        MessageStyles, MessageTheme, MessageTokenPatch, MessageType,
    },
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, BoxShadow, Context, Edges, Entity, FontWeight, IntoElement, Render,
    Role, ScrollAnchor, StyleRefinement, Subscription, Task, Window, div, point, prelude::*, px,
    relative,
};

use super::shell::{page_padding, sidebar_width};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "message-hooks",
        title: "Hooks usage (recommended)",
        description: "A retained native API keeps messages connected to this page's theme and direction.",
    },
    Demo {
        id: "message-other",
        title: "Other types of message",
        description: "Display success, error, and warning feedback without interrupting the current task.",
    },
    Demo {
        id: "message-duration",
        title: "Customize duration",
        description: "This success message remains visible for ten seconds.",
    },
    Demo {
        id: "message-stack",
        title: "Stack",
        description: "Collapse a growing collection into a stack. Hover over the stack to expand it.",
    },
    Demo {
        id: "message-loading",
        title: "Message with loading indicator",
        description: "Show persistent loading feedback, then dismiss it manually when the operation completes.",
    },
    Demo {
        id: "message-thenable",
        title: "Promise interface",
        description: "Await native completion to display loading, success, and information messages in sequence.",
    },
    Demo {
        id: "message-style-class",
        title: "Custom semantic styles",
        description: "Customize the message surface, icon, and title with native styles or a properties resolver.",
    },
    Demo {
        id: "message-update",
        title: "Update Message Content",
        description: "Use the same key to replace a loading message with the result of the operation.",
    },
    Demo {
        id: "message-info",
        title: "Window API",
        description: "The window's Message API provides feedback from callbacks while retaining its configuration.",
    },
    Demo {
        id: "message-component-token",
        title: "Component Token",
        description: "A separate scoped host demonstrates customized message padding and background.",
    },
];

pub(super) struct MessagePage {
    pub scope: ConfigScope,
    pub messages: Entity<MessageState>,
    pub stack_messages: Entity<MessageState>,
    pub token_messages: Entity<MessageState>,
    pub stack_enabled: bool,
    pub threshold: usize,
    pub opened: usize,
    pub updates: usize,
    pub sequence_step: usize,
    pub loading: bool,
    number: Entity<InputNumberState>,
    anchors: Vec<ScrollAnchor>,
    loading_task: Option<Task<()>>,
    update_task: Option<Task<()>>,
    sequence_task: Option<Task<()>>,
    _config: Subscription,
    _number: Subscription,
}

impl MessagePage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), DEMOS.len());
        let messages = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
        let stack_messages = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
        Message::new(&stack_messages).set_config(
            MessageConfigPatch {
                stack: Some(MessageStack::Enabled { threshold: 3 }),
                ..Default::default()
            },
            window,
            cx,
        );
        let mut token_patch = ConfigPatch::default();
        token_patch
            .theme
            .get_or_insert_default()
            .components
            .set::<MessageTheme>(MessageTokenPatch {
                content_bg: Some(Color::rgb(0xe6f4ff)),
                content_padding: Some(Edges {
                    top: 40.,
                    right: 40.,
                    bottom: 40.,
                    left: 40.,
                }),
                ..Default::default()
            });
        let token_scope = scope
            .child(token_patch, cx)
            .expect("valid Message token customization");
        let token_messages = cx.new(|cx| MessageState::new(token_scope, window, cx));
        let options = NumberOptions {
            minimum: Some(ExactDecimal::from(1)),
            maximum: Some(ExactDecimal::from(10)),
            ..Default::default()
        };
        let number = cx.new(|cx| {
            let mut state = InputNumberState::with_options(options, window, cx).unwrap();
            state
                .set_value(Some(ExactDecimal::from(3).into()), window, cx)
                .unwrap();
            state
        });
        let number_subscription = cx.subscribe_in(&number, window, |this, _, event, window, cx| {
            if let InputNumberEvent::Change(change) = event {
                this.threshold = change
                    .value
                    .as_ref()
                    .and_then(|value| value.to_string().parse().ok())
                    .unwrap_or(1);
                this.configure_stack(window, cx);
            }
        });
        Self {
            _config: scope.observe(cx),
            _number: number_subscription,
            scope,
            messages,
            stack_messages,
            token_messages,
            stack_enabled: true,
            threshold: 3,
            opened: 0,
            updates: 0,
            sequence_step: 0,
            loading: false,
            number,
            anchors,
            loading_task: None,
            update_task: None,
            sequence_task: None,
        }
    }

    fn configure_stack(&self, window: &mut Window, cx: &mut Context<Self>) {
        Message::new(&self.stack_messages).set_config(
            MessageConfigPatch {
                stack: Some(if self.stack_enabled {
                    MessageStack::Enabled {
                        threshold: self.threshold,
                    }
                } else {
                    MessageStack::Disabled
                }),
                ..Default::default()
            },
            window,
            cx,
        );
        cx.notify();
    }

    fn button(&self, id: &'static str, label: &'static str) -> Button {
        Button::new(id)
            .scope(self.scope.clone())
            .label(label)
            .automation_id(id)
    }

    fn observed(id: &'static str, element: impl IntoElement) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .child(element)
            .into_any_element()
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .w_full()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(8.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn open(
        &mut self,
        kind: MessageType,
        content: &'static str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.opened += 1;
        Message::new(&self.messages).open(MessageOptions::new(content).kind(kind), window, cx);
        cx.notify();
    }

    pub fn start_loading(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.loading_task = None;
        self.loading = true;
        let handle = Message::new(&self.messages).open(
            MessageOptions::new("Action in progress..")
                .kind(MessageType::Loading)
                .key("gallery-loading")
                .duration(Duration::ZERO),
            window,
            cx,
        );
        self.loading_task = Some(cx.spawn_in(window, async move |page, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(2500))
                .await;
            let _ = page.update_in(cx, |page, window, cx| {
                handle.close(window, cx);
                page.loading = false;
                cx.notify();
            });
        }));
        cx.notify();
    }

    pub fn update_message(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.update_task = None;
        Message::new(&self.messages).open(
            MessageOptions::new("Loading...")
                .kind(MessageType::Loading)
                .key("updatable"),
            window,
            cx,
        );
        self.update_task = Some(cx.spawn_in(window, async move |page, cx| {
            cx.background_executor().timer(Duration::from_secs(1)).await;
            let _ = page.update_in(cx, |page, window, cx| {
                Message::new(&page.messages).open(
                    MessageOptions::new("Loaded!")
                        .kind(MessageType::Success)
                        .key("updatable")
                        .duration(Duration::from_secs(2)),
                    window,
                    cx,
                );
                page.updates += 1;
                cx.notify();
            });
        }));
        cx.notify();
    }

    pub fn start_sequence(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.sequence_task = None;
        Message::new(&self.messages).destroy("gallery-sequence", window, cx);
        self.sequence_step = 1;
        let first = Message::new(&self.messages).open(
            MessageOptions::new("Action in progress..")
                .kind(MessageType::Loading)
                .key("gallery-sequence")
                .duration(Duration::from_millis(2500)),
            window,
            cx,
        );
        self.sequence_task = Some(cx.spawn_in(window, async move |page, cx| {
            first.completion().await;
            let Ok(second) = page.update_in(cx, |page, window, cx| {
                page.sequence_step = 2;
                let handle = Message::new(&page.messages).open(
                    MessageOptions::new("Loading finished")
                        .kind(MessageType::Success)
                        .key("gallery-sequence")
                        .duration(Duration::from_millis(2500)),
                    window,
                    cx,
                );
                cx.notify();
                handle
            }) else {
                return;
            };
            second.completion().await;
            let Ok(third) = page.update_in(cx, |page, window, cx| {
                page.sequence_step = 3;
                let handle = Message::new(&page.messages).open(
                    MessageOptions::new("Loading finished")
                        .kind(MessageType::Info)
                        .key("gallery-sequence")
                        .duration(Duration::from_millis(2500)),
                    window,
                    cx,
                );
                cx.notify();
                handle
            }) else {
                return;
            };
            third.completion().await;
            let _ = page.update_in(cx, |page, _, cx| {
                page.sequence_step = 0;
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn semantic(error: bool) -> MessageStyles {
        let color = Color::rgb(if error { 0xcf1322 } else { 0x237804 });
        MessageStyles {
            root: StyleRefinement::default()
                .bg(Color::rgb(if error { 0xfff2f0 } else { 0xf6ffed }))
                .border_2()
                .border_color(Color::rgb(if error { 0xffccc7 } else { 0x95de64 }))
                .rounded(px(16.))
                .shadow(vec![BoxShadow {
                    color: Color::rgb(if error { 0xffccc7 } else { 0xd9f7be }).into(),
                    offset: point(px(4.), px(4.)),
                    blur_radius: px(0.),
                    spread_radius: px(0.),
                    inset: false,
                }]),
            icon: StyleRefinement::default().text_color(color),
            title: StyleRefinement::default()
                .text_color(color)
                .font_weight(FontWeight::SEMIBOLD),
            ..Default::default()
        }
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "message-hooks" => Self::observed("message.normal", self.button("message.normal", "Display normal message").button_type(ButtonType::Primary).on_click(cx.listener(|this, _, window, cx| this.open(MessageType::Info, "Hello, Ant Design!", window, cx)))),
            "message-other" => self.row(cx).children([
                ("message.success", "Success", MessageType::Success, "This is a success message"),
                ("message.error", "Error", MessageType::Error, "This is an error message"),
                ("message.warning", "Warning", MessageType::Warning, "This is a warning message"),
            ].map(|(id, label, kind, content)| Self::observed(id, self.button(id, label).on_click(cx.listener(move |this, _, window, cx| this.open(kind, content, window, cx)))))).into_any_element(),
            "message-duration" => Self::observed("message.duration", self.button("message.duration", "Customized display duration").on_click(cx.listener(|this, _, window, cx| {
                Message::new(&this.messages).open(MessageOptions::new("This is a prompt message for success, and it will disappear in 10 seconds").kind(MessageType::Success).duration(Duration::from_secs(10)), window, cx);
            }))),
            "message-stack" => {
                let controls = self.row(cx).child("Enabled:").child(Self::observed(
                    "message.stack.enabled",
                    Switch::new("message.stack.enabled").scope(self.scope.clone())
                        .checked(self.stack_enabled).accessibility_label("Enable message stack")
                        .on_change(cx.listener(|this, event: &SwitchChange, window, cx| {
                            this.stack_enabled = event.checked;
                            this.configure_stack(window, cx);
                        })),
                )).child("Threshold:").child(Self::observed(
                    "message.stack.threshold",
                    InputNumber::new("message.stack.threshold", &self.number, cx)
                        .scope(self.scope.clone()).disabled(!self.stack_enabled)
                        .accessibility_label("Stack threshold").w(px(72.)),
                ));
                let open = self.button("message.stack.open", "Open the message box")
                    .button_type(ButtonType::Primary)
                    .on_click(cx.listener(|this, _, window, cx| {
                        this.opened += 1;
                        let content = if this.opened % 2 == 1 {
                            format!("Message {}: This is a stacked message.", this.opened)
                        } else {
                            format!("Message {}: This is a slightly longer stacked message.", this.opened)
                        };
                        Message::new(&this.stack_messages).open(
                            MessageOptions::new(content).kind(MessageType::Info).duration(Duration::ZERO), window, cx,
                        );
                        cx.notify();
                    }));
                let destroy = self.button("message.stack.destroy", "Destroy all")
                    .on_click(cx.listener(|this, _, window, cx| {
                        Message::new(&this.stack_messages).destroy_all(window, cx);
                    }));
                div().w_full().flex().flex_col().gap(px(16.)).child(controls)
                    .child(self.row(cx).child(Self::observed("message.stack.open", open))
                        .child(Self::observed("message.stack.destroy", destroy)))
                    .into_any_element()
            },
            "message-loading" => Self::observed("message.loading", self.button("message.loading", "Display a loading indicator").on_click(cx.listener(|this, _, window, cx| this.start_loading(window, cx)))),
            "message-thenable" => Self::observed("message.sequence", self.button("message.sequence", "Display sequential messages").on_click(cx.listener(|this, _, window, cx| this.start_sequence(window, cx)))),
            "message-style-class" => self.row(cx)
                .child(Self::observed("message.semantic.object", self.button("message.semantic.object", "Object style").on_click(cx.listener(|this, _, window, cx| {
                    Message::new(&this.messages).open(MessageOptions::new("This is a message with object styles").kind(MessageType::Success).styles(Self::semantic(false)), window, cx);
                }))))
                .child(Self::observed("message.semantic.function", self.button("message.semantic.function", "Function style").button_type(ButtonType::Primary).on_click(cx.listener(|this, _, window, cx| {
                    Message::new(&this.messages).open(MessageOptions::new("This is a message with function styles").kind(MessageType::Error).styles_fn(|context, _, _| Self::semantic(context.kind == Some(MessageType::Error))), window, cx);
                })))).into_any_element(),
            "message-update" => Self::observed("message.update", self.button("message.update", "Open the message box").button_type(ButtonType::Primary).on_click(cx.listener(|this, _, window, cx| this.update_message(window, cx)))),
            "message-info" => Self::observed("message.window", self.button("message.window", "Display window message").button_type(ButtonType::Primary).on_click(|_, window, cx| {
                if let Some(api) = Message::for_window(window, cx) { api.info("This is a normal message", window, cx); }
            })),
            "message-component-token" => Self::observed("message.tokens", self.button("message.tokens", "Display customized message").on_click(cx.listener(|this, _, window, cx| {
                Message::new(&this.token_messages).open(MessageOptions::new("Hello World!").kind(MessageType::Error), window, cx);
            }))),
            _ => unreachable!("registered Message example"),
        }
    }
}

impl Render for MessagePage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        let content = div()
            .id("gallery.message.examples")
            .w_full()
            .grid()
            .grid_cols(if content_width >= px(840.) { 2 } else { 1 })
            .items_start()
            .gap(px(tokens.margin_lg as f32))
            .when(config.direction == Direction::Rtl, |page| page.text_right())
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
            }));
        MessageHost::new(
            "gallery-message-host",
            &self.messages,
            MessageHost::new(
                "gallery-message-stack-host",
                &self.stack_messages,
                MessageHost::new("gallery-message-token-host", &self.token_messages, content),
            ),
        )
    }
}
