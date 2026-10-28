// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    Alert, Button, ConfigScope, Direction, Size, Spin, Switch, SwitchChange,
    alert::AlertType,
    icons::IconName,
    spin::{SpinPercent, SpinStyles},
    theme::Color,
};
use gpui::{
    AnyElement, Context, FontWeight, IntoElement, Render, Role, ScrollAnchor, StyleRefinement,
    Subscription, Task, Window, div, prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

// Follow the nine public examples in components/spin/index.en-US.md.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "spin-basic",
        title: "Basic Usage",
        description: "The simplest loading indicator.",
    },
    Demo {
        id: "spin-size",
        title: "Size",
        description: "Choose a small, medium, or large loading indicator.",
    },
    Demo {
        id: "spin-nested",
        title: "Embedded mode",
        description: "Display a loading indicator over a content block while preserving its layout.",
    },
    Demo {
        id: "spin-tip",
        title: "Customized description",
        description: "Add a description below the loading indicator.",
    },
    Demo {
        id: "spin-delayAndDebounce",
        title: "Delay",
        description: "Wait 500 milliseconds before showing the indicator to avoid flashing during short operations.",
    },
    Demo {
        id: "spin-custom-indicator",
        title: "Custom spinning indicator",
        description: "Use a native icon as the loading indicator, including a custom 48-pixel size.",
    },
    Demo {
        id: "spin-percent",
        title: "Progress",
        description: "Display measured progress, or switch to automatic progress when completion is unknown.",
    },
    Demo {
        id: "spin-style-class",
        title: "Custom semantic dom styling",
        description: "Customize semantic slots with native styles or a resolver based on the component's properties.",
    },
    Demo {
        id: "spin-fullscreen",
        title: "Fullscreen",
        description: "Show a window-wide backdrop while an operation is running. This example completes automatically.",
    },
];

pub(super) struct SpinPage {
    pub scope: ConfigScope,
    pub nested_loading: bool,
    pub delayed_loading: bool,
    pub automatic: bool,
    pub percent: f64,
    pub progress_running: bool,
    pub fullscreen: bool,
    pub fullscreen_percent: f64,
    anchors: Vec<ScrollAnchor>,
    progress_task: Option<Task<()>>,
    fullscreen_task: Option<Task<()>>,
    _config: Subscription,
}

impl SpinPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        Self {
            _config: scope.observe(cx),
            scope,
            nested_loading: false,
            delayed_loading: false,
            automatic: false,
            percent: 35.,
            progress_running: false,
            fullscreen: false,
            fullscreen_percent: 0.,
            anchors,
            progress_task: None,
            fullscreen_task: None,
        }
    }

    fn spin(&self, id: &'static str) -> Spin {
        Spin::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .accessibility_label("Loading")
    }

    fn observed(id: &'static str, element: impl IntoElement) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .when(
                id == "spin.nested.content" || id == "spin.delay.content",
                |wrapper| wrapper.w_full(),
            )
            .child(element)
            .into_any_element()
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(16.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn column() -> gpui::Div {
        div().w_full().flex().flex_col().items_start().gap(px(16.))
    }

    fn alert(&self, id: &'static str) -> Alert {
        Alert::new(id)
            .scope(self.scope.clone())
            .alert_type(AlertType::Info)
            .title("Alert message title")
            .description("Further details about the context of this alert.")
            .w_full()
    }

    pub fn run_progress(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.progress_task = None;
        self.automatic = false;
        self.percent = 0.;
        self.progress_running = true;
        self.progress_task = Some(cx.spawn_in(window, async move |page, cx| {
            for step in 1..=20 {
                cx.background_executor()
                    .timer(Duration::from_millis(100))
                    .await;
                if page
                    .update_in(cx, |page, _, cx| {
                        page.percent = f64::from(step * 5);
                        page.progress_running = step < 20;
                        cx.notify();
                    })
                    .is_err()
                {
                    return;
                }
            }
        }));
        cx.notify();
    }

    pub fn show_fullscreen(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.fullscreen_task = None;
        self.fullscreen = true;
        self.fullscreen_percent = -10.;
        self.fullscreen_task = Some(cx.spawn_in(window, async move |page, cx| {
            for step in 1..=28 {
                cx.background_executor()
                    .timer(Duration::from_millis(100))
                    .await;
                if page
                    .update_in(cx, |page, _, cx| {
                        page.fullscreen_percent = -10. + f64::from(step * 5);
                        if step == 28 {
                            page.fullscreen = false;
                            page.fullscreen_percent = 0.;
                        }
                        cx.notify();
                    })
                    .is_err()
                {
                    return;
                }
            }
        }));
        cx.notify();
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "spin-basic" => Self::observed("spin.basic", self.spin("spin.basic")),
            "spin-size" => self
                .row(cx)
                .children([
                    self.spin("spin.size.small").size(Size::Small),
                    self.spin("spin.size.medium"),
                    self.spin("spin.size.large").size(Size::Large),
                ])
                .into_any_element(),
            "spin-nested" => Self::column()
                .child(Self::observed(
                    "spin.nested.content",
                    self.spin("spin.nested")
                        .spinning(self.nested_loading)
                        .w_full()
                        .child(self.alert("spin.nested.alert")),
                ))
                .child(
                    self.row(cx).child("Loading state:").child(Self::observed(
                        "spin.nested.toggle",
                        Switch::new("spin.nested.toggle")
                            .scope(self.scope.clone())
                            .checked(self.nested_loading)
                            .accessibility_label("Embedded loading state")
                            .automation_id("spin.nested.toggle")
                            .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                this.nested_loading = event.checked;
                                cx.notify();
                            })),
                    )),
                )
                .into_any_element(),
            "spin-tip" => Self::column()
                .child(
                    self.row(cx).children(
                        [
                            ("spin.description.small", Size::Small),
                            ("spin.description.medium", Size::Medium),
                            ("spin.description.large", Size::Large),
                        ]
                        .into_iter()
                        .map(|(id, size)| {
                            self.spin(id).size(size).description("Loading").child(
                                div()
                                    .w(px(100.))
                                    .h(px(100.))
                                    .rounded(px(4.))
                                    .bg(Color::BLACK.with_alpha(0.05)),
                            )
                        }),
                    ),
                )
                .child(
                    self.spin("spin.description.alert")
                        .description("Loading...")
                        .w_full()
                        .child(self.alert("spin.description.alert.content")),
                )
                .into_any_element(),
            "spin-delayAndDebounce" => Self::column()
                .child(Self::observed(
                    "spin.delay.content",
                    self.spin("spin.delay")
                        .spinning(self.delayed_loading)
                        .delay(Duration::from_millis(500))
                        .w_full()
                        .child(self.alert("spin.delay.alert")),
                ))
                .child(
                    self.row(cx).child("Loading state:").child(Self::observed(
                        "spin.delay.toggle",
                        Switch::new("spin.delay.toggle")
                            .scope(self.scope.clone())
                            .checked(self.delayed_loading)
                            .accessibility_label("Delayed loading state")
                            .automation_id("spin.delay.toggle")
                            .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                this.delayed_loading = event.checked;
                                cx.notify();
                            })),
                    )),
                )
                .into_any_element(),
            "spin-custom-indicator" => self
                .row(cx)
                .children(
                    [
                        self.spin("spin.custom.small").size(Size::Small),
                        self.spin("spin.custom.medium"),
                        self.spin("spin.custom.large").size(Size::Large),
                        self.spin("spin.custom.48").styles(SpinStyles {
                            indicator: StyleRefinement::default().text_size(px(48.)),
                            ..Default::default()
                        }),
                    ]
                    .map(|spin| {
                        spin.indicator(IconName::LoadingOutlined)
                            .indicator_spin(true)
                    }),
                )
                .into_any_element(),
            "spin-percent" => {
                let percent = if self.automatic {
                    SpinPercent::Auto
                } else {
                    SpinPercent::Value(self.percent)
                };
                Self::column()
                    .child(
                        self.row(cx)
                            .child(Self::observed(
                                "spin.progress.auto",
                                Switch::new("spin.progress.auto")
                                    .scope(self.scope.clone())
                                    .checked(self.automatic)
                                    .checked_label("Auto")
                                    .unchecked_label("Auto")
                                    .accessibility_label("Automatic progress")
                                    .automation_id("spin.progress.auto")
                                    .on_change(cx.listener(|this, event: &SwitchChange, _, cx| {
                                        this.progress_task = None;
                                        this.progress_running = false;
                                        this.automatic = event.checked;
                                        this.percent = 35.;
                                        cx.notify();
                                    })),
                            ))
                            .children([
                                self.spin("spin.progress.small")
                                    .size(Size::Small)
                                    .percent(percent),
                                self.spin("spin.progress.medium").percent(percent),
                                self.spin("spin.progress.large")
                                    .size(Size::Large)
                                    .percent(percent),
                            ]),
                    )
                    .child(Self::observed(
                        "spin.progress.run",
                        Button::new("spin.progress.run")
                            .scope(self.scope.clone())
                            .label("Run progress")
                            .automation_id("spin.progress.run")
                            .on_click(
                                cx.listener(|this, _, window, cx| this.run_progress(window, cx)),
                            ),
                    ))
                    .into_any_element()
            }
            "spin-style-class" => self
                .row(cx)
                .child(
                    self.spin("spin.semantic.object")
                        .percent(0.)
                        .styles(SpinStyles {
                            root: StyleRefinement::default().p(px(8.)),
                            indicator: StyleRefinement::default().text_color(Color::rgb(0x00d4ff)),
                            ..Default::default()
                        }),
                )
                .child(
                    self.spin("spin.semantic.function")
                        .size(Size::Small)
                        .percent(0.)
                        .styles(SpinStyles {
                            root: StyleRefinement::default().p(px(8.)),
                            ..Default::default()
                        })
                        .styles_fn(|context, _, _| {
                            if context.size == Size::Small {
                                SpinStyles {
                                    indicator: StyleRefinement::default()
                                        .text_color(Color::rgb(0x722ed1)),
                                    ..Default::default()
                                }
                            } else {
                                SpinStyles::default()
                            }
                        }),
                )
                .into_any_element(),
            "spin-fullscreen" => Self::column()
                .child(Self::observed(
                    "spin.fullscreen.show",
                    Button::new("spin.fullscreen.show")
                        .scope(self.scope.clone())
                        .label("Show fullscreen")
                        .automation_id("spin.fullscreen.show")
                        .on_click(
                            cx.listener(|this, _, window, cx| this.show_fullscreen(window, cx)),
                        ),
                ))
                .child(
                    self.spin("spin.fullscreen")
                        .spinning(self.fullscreen)
                        .percent(self.fullscreen_percent)
                        .fullscreen(true),
                )
                .into_any_element(),
            _ => unreachable!("registered Spin example"),
        }
    }
}

impl Render for SpinPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.spin.examples")
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
            }))
    }
}
