// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use super::shell::{page_padding, sidebar_width};
use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Progress, Segmented, SegmentedOption, Slider,
    SliderOptions, SliderState, Tooltip, icons::IconName, progress::*, theme::Color,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FontWeight, IntoElement, Render, Role, ScrollAnchor,
    StyleRefinement, Subscription, Window, div, prelude::*, px, relative,
};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}
const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
    }
}
pub(super) const DEMOS: &[Demo] = &[
    demo(
        "progress-line",
        "Progress bar",
        "A standard progress bar with normal, active, exception, and success states.",
    ),
    demo(
        "progress-circle",
        "Circular progress bar",
        "Display progress in a circle.",
    ),
    demo(
        "progress-line-mini",
        "Mini size progress bar",
        "A smaller progress bar for compact layouts.",
    ),
    demo(
        "progress-circle-micro",
        "Responsive circular progress bar",
        "At 20 pixels or less, the percentage appears in a tooltip.",
    ),
    demo(
        "progress-circle-mini",
        "Mini size circular progress bar",
        "An 80-pixel circular progress indicator.",
    ),
    demo(
        "progress-dynamic",
        "Dynamic",
        "Increase or decrease progress in ten-percent increments.",
    ),
    demo(
        "progress-format",
        "Custom text format",
        "Use application-specific text in the indicator.",
    ),
    demo(
        "progress-dashboard",
        "Dashboard",
        "Change the degree and logical placement of the gap.",
    ),
    demo(
        "progress-segment",
        "Progress bar with success segment",
        "Show completed work separately from work in progress.",
    ),
    demo(
        "progress-linecap",
        "Stroke Linecap",
        "Use flat ends on lines, circles, and dashboards.",
    ),
    demo(
        "progress-gradient-line",
        "Custom line gradient",
        "Two-color and multi-stop gradients, including circular and dashboard indicators.",
    ),
    demo(
        "progress-steps",
        "Progress bar with steps",
        "Discrete steps with optional individual colors.",
    ),
    demo(
        "progress-circle-steps",
        "Circular progress bar with steps",
        "Adjust the number of steps and the gap between them.",
    ),
    demo(
        "progress-size",
        "Progress size",
        "Default, small, and custom dimensions for every shape.",
    ),
    demo(
        "progress-info-position",
        "Change progress value position",
        "Place values inside or outside the track and align them logically.",
    ),
    demo(
        "progress-style-class",
        "Custom semantic dom styling",
        "Resolve native track and rail styles from the current percentage.",
    ),
    demo(
        "progress-tokens",
        "Component tokens",
        "Scoped Progress tokens customize colors, rounding, and circular text.",
    ),
];

pub(super) struct ProgressPage {
    pub scope: ConfigScope,
    pub percent: f64,
    pub gap: f32,
    pub placement: ProgressGapPlacement,
    pub count: usize,
    pub step_gap: f32,
    count_slider: Entity<SliderState>,
    gap_slider: Entity<SliderState>,
    token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}
impl ProgressPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), DEMOS.len());
        let mut patch = ConfigPatch::default();
        patch
            .theme
            .get_or_insert_default()
            .components
            .set::<ProgressTheme>(ProgressTokenPatch {
                default_color: Some(Color::rgb(0x722ed1)),
                remaining_color: Some(Color::rgb(0xf9f0ff)),
                line_border_radius: Some(2.),
                circle_text_color: Some(Color::rgb(0x722ed1)),
                circle_text_font_size: Some(0.9),
                circle_icon_font_size: Some(1.4),
            });
        Self {
            token_scope: scope.child(patch, cx).unwrap(),
            _config: scope.observe(cx),
            scope,
            anchors,
            percent: 0.,
            gap: 50.,
            placement: ProgressGapPlacement::Bottom,
            count: 5,
            step_gap: 7.,
            count_slider: cx.new(|cx| {
                SliderState::with_options(
                    SliderOptions {
                        min: 2.,
                        max: 10.,
                        ..Default::default()
                    },
                    window,
                    cx,
                )
                .unwrap()
            }),
            gap_slider: cx.new(|cx| {
                SliderState::with_options(
                    SliderOptions {
                        max: 40.,
                        step: Some(4.),
                        ..Default::default()
                    },
                    window,
                    cx,
                )
                .unwrap()
            }),
        }
    }
    fn progress(&self, id: &'static str, percent: f64) -> Progress {
        Progress::new(id)
            .scope(self.scope.clone())
            .percent(percent)
            .automation_id(id)
            .accessibility_label("Progress")
    }
    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(16.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |d| {
                d.flex_row_reverse()
            })
    }
    fn column() -> gpui::Div {
        div().w_full().flex().flex_col().items_start().gap(px(16.))
    }
    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        use ProgressKind::{Circle, Dashboard};
        use ProgressStatus::{Active, Exception};
        match id {
            "progress-line" | "progress-line-mini" => {
                let small = id == "progress-line-mini";
                Self::column()
                    .when(small, |d| d.w(px(180.)))
                    .children(
                        [
                            self.progress("progress.line.30", 30.),
                            self.progress("progress.line.active", 50.).status(Active),
                            self.progress("progress.line.exception", 70.)
                                .status(Exception),
                            self.progress("progress.line.done", 100.),
                            self.progress("progress.line.no-info", 50.).show_info(false),
                        ]
                        .map(|p| {
                            if small {
                                p.size(ProgressSize::Small)
                            } else {
                                p
                            }
                        }),
                    )
                    .into_any_element()
            }
            "progress-circle" | "progress-circle-mini" => {
                let small = id == "progress-circle-mini";
                self.row(cx)
                    .children(
                        [
                            self.progress("progress.circle.normal", if small { 30. } else { 75. }),
                            self.progress("progress.circle.exception", 70.)
                                .status(Exception),
                            self.progress("progress.circle.done", 100.),
                        ]
                        .map(|p| p.kind(Circle).size(if small { 80 } else { 120 })),
                    )
                    .into_any_element()
            }
            "progress-circle-micro" => self
                .row(cx)
                .child(
                    self.progress("progress.micro", 60.)
                        .kind(Circle)
                        .size(14)
                        .stroke_width(20.)
                        .rail_color(Color::rgb(0xe6f4ff))
                        .format(|p, _| format!("In progress, {p}% complete").into()),
                )
                .child("Code release")
                .into_any_element(),
            "progress-dynamic" => Self::column()
                .child(self.progress("progress.dynamic.line", self.percent))
                .child(
                    self.progress("progress.dynamic.circle", self.percent)
                        .kind(Circle),
                )
                .child(
                    self.row(cx).gap_0().children(
                        [
                            (
                                "progress.dynamic.decrease",
                                IconName::MinusOutlined,
                                -10.,
                                "Decrease progress",
                            ),
                            (
                                "progress.dynamic.increase",
                                IconName::PlusOutlined,
                                10.,
                                "Increase progress",
                            ),
                        ]
                        .map(|(id, icon, delta, label)| {
                            div().id(id).debug_selector(move || id.into()).child(
                                Button::new(id)
                                    .scope(self.scope.clone())
                                    .icon(icon)
                                    .accessibility_label(label)
                                    .automation_id(id)
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.percent = (this.percent + delta).clamp(0., 100.);
                                        cx.notify();
                                    })),
                            )
                        }),
                    ),
                )
                .into_any_element(),
            "progress-format" => self
                .row(cx)
                .child(
                    self.progress("progress.format.days", 75.)
                        .kind(Circle)
                        .format(|p, _| format!("{p} Days").into()),
                )
                .child(
                    self.progress("progress.format.done", 100.)
                        .kind(Circle)
                        .format(|_, _| "Done".into()),
                )
                .into_any_element(),
            "progress-dashboard" => Self::column()
                .child("gapDegree:")
                .child(
                    Segmented::new("progress.dashboard.degree")
                        .scope(self.scope.clone())
                        .options([
                            SegmentedOption::new("50", 50_u32, "50"),
                            SegmentedOption::new("100", 100, "100"),
                        ])
                        .value(self.gap as u32)
                        .accessibility_label("Gap degree")
                        .on_change(cx.listener(|this, value: &u32, _, cx| {
                            this.gap = *value as f32;
                            cx.notify();
                        })),
                )
                .child("gapPlacement:")
                .child(
                    Segmented::new("progress.dashboard.placement")
                        .scope(self.scope.clone())
                        .options(
                            [
                                ("start", ProgressGapPlacement::Start),
                                ("end", ProgressGapPlacement::End),
                                ("top", ProgressGapPlacement::Top),
                                ("bottom", ProgressGapPlacement::Bottom),
                            ]
                            .map(|(label, value)| SegmentedOption::new(label, value, label)),
                        )
                        .value(self.placement)
                        .accessibility_label("Gap placement")
                        .on_change(cx.listener(|this, value: &ProgressGapPlacement, _, cx| {
                            this.placement = *value;
                            cx.notify();
                        })),
                )
                .child(
                    self.progress("progress.dashboard", 30.)
                        .kind(Dashboard)
                        .gap_degree(self.gap)
                        .gap_placement(self.placement),
                )
                .into_any_element(),
            "progress-segment" => Self::column()
                .child(
                    Tooltip::new("progress.success.tip")
                        .scope(self.scope.clone())
                        .title("3 done / 3 in progress / 4 to do")
                        .child(self.progress("progress.success.line", 60.).success(30.)),
                )
                .child(self.row(cx).children([Circle, Dashboard].map(|kind| {
                    self.progress(
                        if kind == Circle {
                            "progress.success.circle"
                        } else {
                            "progress.success.dashboard"
                        },
                        60.,
                    )
                    .kind(kind)
                    .success(30.)
                })))
                .into_any_element(),
            "progress-linecap" => Self::column()
                .child(
                    self.progress("progress.cap.line", 75.)
                        .stroke_linecap(ProgressLinecap::Butt),
                )
                .child(self.row(cx).children([Circle, Dashboard].map(|kind| {
                    self.progress(
                        if kind == Circle {
                            "progress.cap.circle"
                        } else {
                            "progress.cap.dashboard"
                        },
                        75.,
                    )
                    .kind(kind)
                    .stroke_linecap(ProgressLinecap::Butt)
                })))
                .into_any_element(),
            "progress-gradient-line" => {
                let two = ProgressGradient::between(Color::rgb(0x108ee9), Color::rgb(0x87d068));
                let three = ProgressGradient::new([
                    (0., Color::rgb(0x87d068)),
                    (0.5, Color::rgb(0xffe58f)),
                    (1., Color::rgb(0xffccc7)),
                ]);
                Self::column()
                    .child(
                        self.progress("progress.gradient.line", 99.9)
                            .stroke_color(two.clone()),
                    )
                    .child(
                        self.progress("progress.gradient.active", 50.)
                            .status(Active)
                            .stroke_color(two.clone()),
                    )
                    .children(
                        [Circle, Dashboard]
                            .into_iter()
                            .enumerate()
                            .map(|(row, kind)| {
                                self.row(cx).children(
                                    [
                                        (90., two.clone()),
                                        (100., two.clone()),
                                        (93., three.clone()),
                                    ]
                                    .into_iter()
                                    .enumerate()
                                    .map(|(i, (p, g))| {
                                        Progress::new(("gradient", row * 3 + i))
                                            .scope(self.scope.clone())
                                            .kind(kind)
                                            .percent(p)
                                            .stroke_color(g)
                                    }),
                                )
                            }),
                    )
                    .into_any_element()
            }
            "progress-steps" => Self::column()
                .children([
                    self.progress("progress.steps.three", 50.).steps(3),
                    self.progress("progress.steps.five", 30.).steps(5),
                    self.progress("progress.steps.small", 100.)
                        .steps(5)
                        .size(ProgressSize::Small)
                        .stroke_color(Color::rgb(0x389e0d)),
                    self.progress("progress.steps.colors", 60.)
                        .steps(5)
                        .stroke_color(ProgressStroke::steps([
                            Color::rgb(0x389e0d),
                            Color::rgb(0x389e0d),
                            Color::rgb(0xf5222d),
                        ])),
                ])
                .into_any_element(),
            "progress-circle-steps" => Self::column()
                .child("Custom count:")
                .child(
                    Slider::new("progress.steps.count", &self.count_slider)
                        .scope(self.scope.clone())
                        .value(self.count as f64)
                        .accessibility_label("Step count")
                        .on_change(cx.listener(
                            |this, e: &ant_design_gpui::SliderChange, _, cx| {
                                this.count = e.value.as_slice()[0] as usize;
                                cx.notify();
                            },
                        )),
                )
                .child("Custom gap:")
                .child(
                    Slider::new("progress.steps.gap", &self.gap_slider)
                        .scope(self.scope.clone())
                        .value(self.step_gap as f64)
                        .accessibility_label("Step gap")
                        .on_change(cx.listener(
                            |this, e: &ant_design_gpui::SliderChange, _, cx| {
                                this.step_gap = e.value.as_slice()[0] as f32;
                                cx.notify();
                            },
                        )),
                )
                .child(
                    self.row(cx).children(
                        [
                            self.progress("progress.steps.dashboard", 50.)
                                .kind(Dashboard)
                                .steps(8),
                            self.progress("progress.steps.circle", 100.)
                                .kind(Circle)
                                .steps(ProgressSteps {
                                    count: self.count,
                                    gap: self.step_gap,
                                }),
                        ]
                        .map(|p| {
                            p.stroke_width(20.)
                                .rail_color(Color::BLACK.with_alpha(0.06))
                        }),
                    ),
                )
                .into_any_element(),
            "progress-size" => Self::column()
                .child(
                    Self::column().w(px(300.)).children([
                        self.progress("progress.size.line", 50.),
                        self.progress("progress.size.small", 50.)
                            .size(ProgressSize::Small),
                        self.progress("progress.size.custom", 50.).size([300., 20.]),
                    ]),
                )
                .children(
                    [Circle, Dashboard]
                        .into_iter()
                        .enumerate()
                        .map(|(row, kind)| {
                            self.row(cx).children(
                                [
                                    ProgressSize::Medium,
                                    ProgressSize::Small,
                                    ProgressSize::Pixels(20.),
                                ]
                                .into_iter()
                                .enumerate()
                                .map(|(i, size)| {
                                    Progress::new(("size", row * 3 + i))
                                        .scope(self.scope.clone())
                                        .kind(kind)
                                        .percent(50.)
                                        .size(size)
                                }),
                            )
                        }),
                )
                .child(
                    self.row(cx).children([
                        self.progress("progress.size.steps", 50.).steps(3),
                        self.progress("progress.size.steps.small", 50.)
                            .steps(3)
                            .size(ProgressSize::Small),
                        self.progress("progress.size.steps.square", 50.)
                            .steps(3)
                            .size(20),
                        self.progress("progress.size.steps.custom", 50.)
                            .steps(3)
                            .size([20., 30.]),
                    ]),
                )
                .into_any_element(),
            "progress-info-position" => Self::column()
                .children(
                    [
                        (0., ProgressAlign::Center, Some(0xe6f4ff)),
                        (10., ProgressAlign::Center, None),
                        (50., ProgressAlign::Start, Some(0xb7eb8f)),
                        (60., ProgressAlign::End, Some(0x001342)),
                        (100., ProgressAlign::Center, None),
                    ]
                    .into_iter()
                    .enumerate()
                    .map(|(i, (value, align, color))| {
                        let p = Progress::new(("inner", i))
                            .scope(self.scope.clone())
                            .percent(value)
                            .size([if i == 0 { 200. } else { 300. }, 20.])
                            .percent_position(ProgressPercentPosition {
                                align,
                                position: ProgressInfoPosition::Inner,
                            });
                        if let Some(color) = color {
                            p.stroke_color(Color::rgb(color))
                        } else {
                            p
                        }
                    }),
                )
                .children(
                    [
                        (60., ProgressAlign::Start),
                        (100., ProgressAlign::Start),
                        (60., ProgressAlign::Center),
                        (100., ProgressAlign::Center),
                    ]
                    .into_iter()
                    .enumerate()
                    .map(|(i, (value, align))| {
                        Progress::new(("outer", i))
                            .scope(self.scope.clone())
                            .percent(value)
                            .percent_position(ProgressPercentPosition {
                                align,
                                position: ProgressInfoPosition::Outer,
                            })
                    }),
                )
                .into_any_element(),
            "progress-style-class" => Self::column()
                .children([10., 20., 40., 60., 80., 99.].into_iter().enumerate().map(
                    |(i, percent)| {
                        Progress::new(("semantic", i))
                            .scope(self.scope.clone())
                            .percent(percent)
                            .styles_fn(|context, _, _| {
                                let hue = (200. - 2. * context.percent) as f32 / 360.;
                                ProgressStyles {
                                    track: StyleRefinement::default()
                                        .bg(gpui::linear_gradient(
                                            90.,
                                            gpui::linear_color_stop(
                                                gpui::hsla(hue, 0.85, 0.65, 1.),
                                                0.,
                                            ),
                                            gpui::linear_color_stop(
                                                gpui::hsla(hue + 30. / 360., 0.9, 0.55, 0.95),
                                                1.,
                                            ),
                                        ))
                                        .rounded(px(8.)),
                                    rail: StyleRefinement::default()
                                        .bg(Color::BLACK.with_alpha(0.1))
                                        .rounded(px(8.)),
                                    ..Default::default()
                                }
                            })
                    },
                ))
                .into_any_element(),
            "progress-tokens" => Self::column()
                .child(
                    self.progress("progress.token.line", 60.)
                        .scope(self.token_scope.clone()),
                )
                .child(
                    self.row(cx)
                        .children([60., 100.].into_iter().enumerate().map(|(i, p)| {
                            Progress::new(("token-circle", i))
                                .scope(self.token_scope.clone())
                                .percent(p)
                                .kind(Circle)
                        })),
                )
                .into_any_element(),
            _ => unreachable!("registered Progress example"),
        }
    }
}

impl Render for ProgressPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.progress.examples")
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
