// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).
use super::shell::{page_padding, sidebar_width};
use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, InputNumber, InputNumberState, Size, Switch,
    button::ButtonType,
    icons::{IconColors, IconName},
    input_number::{ExactDecimal, InputNumberEvent, NumberOptions},
    notification::{
        Notification, NotificationConfigPatch, NotificationHost, NotificationOptions,
        NotificationPlacement, NotificationStack, NotificationState, NotificationStyles,
        NotificationTheme, NotificationTokenPatch, NotificationType,
    },
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FontWeight, IntoElement, Render, Role, ScrollAnchor,
    Subscription, Task, Window, div, linear_color_stop, linear_gradient, point, prelude::*, px,
    relative,
};
use std::time::Duration;

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "notification-hooks",
        title: "Hooks usage (recommended)",
        description: "Open a notification with this page's theme and direction.",
    },
    Demo {
        id: "notification-duration",
        title: "Duration after which the notification box is closed",
        description: "This notification stays open until you dismiss it.",
    },
    Demo {
        id: "notification-with-icon",
        title: "Notification with icon",
        description: "Show success, information, warning, and error notifications.",
    },
    Demo {
        id: "notification-with-btn",
        title: "Custom close button",
        description: "Add actions to confirm a notification or dismiss the collection.",
    },
    Demo {
        id: "notification-custom-icon",
        title: "Customized Icon",
        description: "Use a custom icon and color.",
    },
    Demo {
        id: "notification-placement",
        title: "Placement",
        description: "Display notifications at any of the six positions.",
    },
    Demo {
        id: "notification-update",
        title: "Update Message Content",
        description: "Update an existing notification by its key.",
    },
    Demo {
        id: "notification-stack",
        title: "Stack",
        description: "Collapse a growing collection. Hover over the stack to expand it.",
    },
    Demo {
        id: "notification-progress",
        title: "Show with progress",
        description: "Show the time remaining and choose whether hover pauses dismissal.",
    },
    Demo {
        id: "notification-window",
        title: "Window API",
        description: "Open a notification from the current window's API.",
    },
    Demo {
        id: "notification-progress-color",
        title: "Customize progress bar color",
        description: "Choose a custom gradient for the countdown bar.",
    },
    Demo {
        id: "notification-component-token",
        title: "Component Token",
        description: "Customize the background for each notification status.",
    },
    Demo {
        id: "notification-style",
        title: "Custom semantic styles",
        description: "Style the surface, icon, title, and description.",
    },
];
pub(super) struct NotificationPage {
    pub scope: ConfigScope,
    pub notifications: Entity<NotificationState>,
    pub stack: Entity<NotificationState>,
    pub tokens: Entity<NotificationState>,
    pub stack_enabled: bool,
    pub threshold: usize,
    pub opened: usize,
    number: Entity<InputNumberState>,
    anchors: Vec<ScrollAnchor>,
    update: Option<Task<()>>,
    _scope: Subscription,
    _number: Subscription,
}
fn gradient(a: u32, b: u32, angle: f32) -> gpui::Background {
    linear_gradient(
        angle,
        linear_color_stop(Color::rgb(a).to_hsla(), 0.),
        linear_color_stop(Color::rgb(b).to_hsla(), 1.),
    )
}
impl NotificationPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let notifications = cx.new(|cx| NotificationState::new(scope.clone(), window, cx));
        let stack = cx.new(|cx| NotificationState::new(scope.clone(), window, cx));
        let mut patch = ConfigPatch::default();
        patch
            .theme
            .get_or_insert_default()
            .components
            .set::<NotificationTheme>(NotificationTokenPatch {
                progress_bg: Some(gradient(0x6253e1, 0x04befe, 135.)),
                color_success_bg: Some(gradient(0xd9f7be, 0xf6ffed, 30.)),
                color_error_bg: Some(gradient(0xffccc7, 0xfff1f0, 30.)),
                color_info_bg: Some(gradient(0xbae0ff, 0xe6f4ff, 30.)),
                color_warning_bg: Some(gradient(0xffffb8, 0xfeffe6, 30.)),
                ..Default::default()
            });
        let token_scope = scope.child(patch, cx).unwrap();
        let tokens = cx.new(|cx| NotificationState::new(token_scope, window, cx));
        let number = cx.new(|cx| {
            let mut state = InputNumberState::with_options(
                NumberOptions {
                    minimum: Some(ExactDecimal::from(1)),
                    maximum: Some(ExactDecimal::from(10)),
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap();
            state
                .set_value(Some(ExactDecimal::from(3).into()), window, cx)
                .unwrap();
            state
        });
        let subscription = cx.subscribe_in(&number, window, |this, _, event, window, cx| {
            if let InputNumberEvent::Change(change) = event
                && let Some(value) = &change.value
            {
                this.threshold = value.to_string().parse().unwrap_or(3);
                this.configure_stack(window, cx);
            }
        });
        let config = scope.observe(cx);
        Self {
            scope,
            notifications,
            stack,
            tokens,
            stack_enabled: true,
            threshold: 3,
            opened: 0,
            number,
            anchors,
            update: None,
            _scope: config,
            _number: subscription,
        }
    }
    fn configure_stack(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        Notification::new(&self.stack).set_config(
            NotificationConfigPatch {
                stack: Some(if self.stack_enabled {
                    NotificationStack::Enabled {
                        threshold: self.threshold,
                    }
                } else {
                    NotificationStack::Disabled
                }),
                ..Default::default()
            },
            window,
            cx,
        );
        cx.notify();
    }
    fn standard() -> NotificationOptions {
        NotificationOptions::new("Notification Title").description("This is the content of the notification. This is the content of the notification. This is the content of the notification.")
    }
    fn button(&self, id: &'static str, label: &'static str) -> Button {
        Button::new(id)
            .scope(self.scope.clone())
            .label(label)
            .automation_id(id)
    }
    fn open_button(
        &self,
        id: &'static str,
        label: &'static str,
        api: Notification,
        options: NotificationOptions,
    ) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .child(
                self.button(id, label)
                    .button_type(ButtonType::Primary)
                    .on_click(move |_, window, cx| {
                        api.open(options.clone(), window, cx);
                    }),
            )
            .into_any_element()
    }
    fn status_buttons(&self, token: bool) -> AnyElement {
        let api = Notification::new(if token {
            &self.tokens
        } else {
            &self.notifications
        });
        div()
            .flex()
            .flex_wrap()
            .gap(px(8.))
            .children(
                [
                    ("notification.success", "Success", NotificationType::Success),
                    ("notification.info", "Info", NotificationType::Info),
                    ("notification.warning", "Warning", NotificationType::Warning),
                    ("notification.error", "Error", NotificationType::Error),
                ]
                .into_iter()
                .map(|(id, label, kind)| {
                    self.open_button(
                        if token {
                            match kind {
                                NotificationType::Success => "notification.tokens.success",
                                NotificationType::Info => "notification.tokens.info",
                                NotificationType::Warning => "notification.tokens.warning",
                                NotificationType::Error => "notification.tokens.error",
                            }
                        } else {
                            id
                        },
                        label,
                        api.clone(),
                        Self::standard().kind(kind).duration(if token {
                            Duration::ZERO
                        } else {
                            Duration::from_millis(4500)
                        }),
                    )
                }),
            )
            .into_any_element()
    }
    fn semantic(error: bool) -> NotificationStyles {
        let mut root = div()
            .bg(Color::rgb(if error { 0xfff2f0 } else { 0xf6ffed }))
            .border_2()
            .border_color(Color::rgb(if error { 0xffccc7 } else { 0x95de64 }))
            .rounded(px(16.))
            .shadow(vec![gpui::BoxShadow {
                color: Color::rgb(if error { 0xffccc7 } else { 0xd9f7be }).to_hsla(),
                offset: point(px(4.), px(4.)),
                blur_radius: px(0.),
                spread_radius: px(0.),
                inset: false,
            }]);
        let mut title = div()
            .text_color(Color::rgb(if error { 0xcf1322 } else { 0x237804 }))
            .font_weight(FontWeight::SEMIBOLD);
        let mut description = div().text_color(Color::rgb(if error { 0x5c0011 } else { 0x3f6600 }));
        let mut icon = div().text_color(Color::rgb(if error { 0xcf1322 } else { 0x237804 }));
        NotificationStyles {
            root: root.style().clone(),
            title: title.style().clone(),
            description: description.style().clone(),
            icon: icon.style().clone(),
            ..Default::default()
        }
    }
    fn preview(&self, id: &str, cx: &mut Context<Self>) -> AnyElement {
        let api = Notification::new(&self.notifications);
        match id {
            "notification-hooks" => self.open_button(
                "notification.open",
                "Open the notification box",
                api,
                Self::standard(),
            ),
            "notification-duration" => self.open_button(
                "notification.persistent",
                "Open the notification box",
                api,
                Self::standard().duration(Duration::ZERO),
            ),
            "notification-with-icon" => self.status_buttons(false),
            "notification-component-token" => self.status_buttons(true),
            "notification-custom-icon" => self.open_button(
                "notification.custom",
                "Open the notification box",
                api,
                Self::standard().icon(
                    IconName::SmileOutlined
                        .icon()
                        .with_colors(IconColors::primary(Color::rgb(0x108ee9).to_rgba())),
                ),
            ),
            "notification-with-btn" => {
                let scope = self.scope.clone();
                let destroy = api.clone();
                self.open_button(
                    "notification.actions",
                    "Open the notification box",
                    api,
                    Self::standard().actions(move |handle, _, cx| {
                        let close = handle.clone();
                        let destroy = destroy.clone();
                        div()
                            .flex()
                            .gap(px(8.))
                            .when(scope.snapshot(cx).direction == Direction::Rtl, |row| {
                                row.flex_row_reverse()
                            })
                            .child(
                                Button::new("destroy-all")
                                    .scope(scope.clone())
                                    .label("Destroy All")
                                    .size(Size::Small)
                                    .button_type(ButtonType::Link)
                                    .on_click(move |_, w, cx| destroy.destroy_all(w, cx)),
                            )
                            .child(
                                Button::new("confirm")
                                    .scope(scope.clone())
                                    .label("Confirm")
                                    .size(Size::Small)
                                    .button_type(ButtonType::Primary)
                                    .on_click(move |_, w, cx| close.close(w, cx)),
                            )
                    }),
                )
            }
            "notification-placement" => div()
                .flex()
                .flex_wrap()
                .gap(px(8.))
                .children(
                    [
                        ("notification.top", "top", NotificationPlacement::Top),
                        (
                            "notification.top-left",
                            "topLeft",
                            NotificationPlacement::TopLeft,
                        ),
                        (
                            "notification.top-right",
                            "topRight",
                            NotificationPlacement::TopRight,
                        ),
                        (
                            "notification.bottom",
                            "bottom",
                            NotificationPlacement::Bottom,
                        ),
                        (
                            "notification.bottom-left",
                            "bottomLeft",
                            NotificationPlacement::BottomLeft,
                        ),
                        (
                            "notification.bottom-right",
                            "bottomRight",
                            NotificationPlacement::BottomRight,
                        ),
                    ]
                    .into_iter()
                    .map(|(id, label, p)| {
                        self.open_button(id, label, api.clone(), Self::standard().placement(p))
                    }),
                )
                .into_any_element(),
            "notification-update" => div()
                .id("notification.update")
                .debug_selector(|| "notification.update".into())
                .child(
                    self.button("notification.update", "Open the notification box")
                        .on_click(cx.listener(|this, _, window, cx| {
                            let api = Notification::new(&this.notifications);
                            api.open(
                                Self::standard()
                                    .key("update")
                                    .title("Notification Title")
                                    .duration(Duration::ZERO),
                                window,
                                cx,
                            );
                            this.update = Some(cx.spawn_in(window, async move |weak, cx| {
                                cx.background_executor().timer(Duration::from_secs(2)).await;
                                let _ = weak.update_in(cx, |this, window, cx| {
                                    Notification::new(&this.notifications).open(
                                        Self::standard()
                                            .key("update")
                                            .title("New Title")
                                            .description("New description."),
                                        window,
                                        cx,
                                    );
                                });
                            }));
                        })),
                )
                .into_any_element(),
            "notification-stack" => div()
                .flex()
                .flex_col()
                .gap(px(16.))
                .child(
                    div()
                        .flex()
                        .flex_wrap()
                        .items_center()
                        .gap(px(8.))
                        .child("Enabled:")
                        .child(
                            div()
                                .debug_selector(|| "notification.stack.enabled".into())
                                .child(
                                    Switch::new("notification.stack.enabled")
                                        .scope(self.scope.clone())
                                        .checked(self.stack_enabled)
                                        .accessibility_label("Enable notification stack")
                                        .on_change(cx.listener(
                                            |this,
                                             change: &ant_design_gpui::switch::SwitchChange,
                                             window,
                                             cx| {
                                                this.stack_enabled = change.checked;
                                                this.configure_stack(window, cx);
                                            },
                                        )),
                                ),
                        )
                        .child("Threshold:")
                        .child(
                            InputNumber::new("notification.stack.threshold", &self.number, cx)
                                .scope(self.scope.clone())
                                .disabled(!self.stack_enabled),
                        ),
                )
                .child(
                    div()
                        .id("notification.stack.open")
                        .debug_selector(|| "notification.stack.open".into())
                        .child(
                            self.button("notification.stack.open", "Open the notification box")
                                .on_click(cx.listener(|this, _, w, cx| {
                                    this.opened += 1;
                                    Notification::new(&this.stack).open(
                                        Self::standard()
                                            .description(
                                                "This is the content of the notification. "
                                                    .repeat(this.opened % 3 + 1),
                                            )
                                            .duration(Duration::ZERO),
                                        w,
                                        cx,
                                    );
                                })),
                        ),
                )
                .child(
                    div()
                        .debug_selector(|| "notification.stack.destroy".into())
                        .child(
                            self.button("notification.stack.destroy", "Destroy all")
                                .on_click(cx.listener(|this, _, w, cx| {
                                    Notification::new(&this.stack).destroy_all(w, cx)
                                })),
                        ),
                )
                .into_any_element(),
            "notification-progress" => div()
                .flex()
                .flex_wrap()
                .gap(px(8.))
                .child(self.open_button(
                    "notification.pause",
                    "Pause on hover",
                    api.clone(),
                    Self::standard().show_progress(true),
                ))
                .child(self.open_button(
                    "notification.no-pause",
                    "Don't pause on hover",
                    api,
                    Self::standard().show_progress(true).pause_on_hover(false),
                ))
                .into_any_element(),
            "notification-window" => div()
                .debug_selector(|| "notification.window".into())
                .child(
                    self.button("notification.window", "Open the notification box")
                        .on_click(|_, w, cx| {
                            if let Some(api) = Notification::for_window(w, cx) {
                                api.open(Self::standard(), w, cx);
                            }
                        }),
                )
                .into_any_element(),
            "notification-progress-color" => self.open_button(
                "notification.progress-color",
                "Show custom progress color",
                Notification::new(&self.tokens),
                Self::standard()
                    .title("Customize progress bar color")
                    .show_progress(true)
                    .duration(Duration::from_secs(20)),
            ),
            "notification-style" => div()
                .flex()
                .flex_wrap()
                .gap(px(8.))
                .child(
                    self.open_button(
                        "notification.style.default",
                        "Default Notification",
                        api.clone(),
                        Self::standard()
                            .kind(NotificationType::Info)
                            .styles(Self::semantic(false)),
                    ),
                )
                .child(
                    self.open_button(
                        "notification.style.error",
                        "Error Notification",
                        api,
                        Self::standard()
                            .kind(NotificationType::Error)
                            .styles_fn(|ctx, _, _| {
                                Self::semantic(ctx.kind == Some(NotificationType::Error))
                            }),
                    ),
                )
                .into_any_element(),
            _ => gpui::Empty.into_any_element(),
        }
    }
}
impl Render for NotificationPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let t = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content = div()
            .id("gallery.notification.examples")
            .w_full()
            .grid()
            .grid_cols(
                if width - sidebar_width(width) - page_padding(width) * 2. >= px(840.) {
                    2
                } else {
                    1
                },
            )
            .items_start()
            .gap(px(t.margin_lg as f32))
            .when(config.direction == Direction::Rtl, |d| d.text_right())
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
                    .border_color(t.color_border_secondary)
                    .rounded(px(t.border_radius_lg as f32))
                    .bg(t.color_bg_container)
                    .child(
                        div()
                            .p(px(t.padding as f32))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(demo.title),
                    )
                    .child(
                        div()
                            .id("preview")
                            .min_h(px(120.))
                            .p(px(t.padding_lg as f32))
                            .flex()
                            .items_center()
                            .child(self.preview(demo.id, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(t.color_border_secondary)
                            .p(px(t.padding as f32))
                            .text_size(px(t.font_size_sm as f32))
                            .line_height(relative(t.line_height as f32))
                            .text_color(t.color_text_secondary)
                            .child(demo.description),
                    )
            }));
        NotificationHost::new(
            "gallery-notification-host",
            &self.notifications,
            NotificationHost::new(
                "gallery-notification-stack-host",
                &self.stack,
                NotificationHost::new("gallery-notification-token-host", &self.tokens, content),
            ),
        )
    }
}
