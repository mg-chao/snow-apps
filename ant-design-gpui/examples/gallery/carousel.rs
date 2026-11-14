// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Direction, RadioGroup, RadioOption,
    carousel::{
        Carousel, CarouselEffect, CarouselEvent, CarouselItems, CarouselSlide, CarouselState,
        CarouselTheme, CarouselTokenPatch, DotPlacement,
    },
    radio::RadioOptionType,
    theme::ThemeConfig,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FocusHandle, FontWeight, IntoElement, Render, Role,
    ScrollAnchor, Subscription, Window, div, prelude::*, px, relative, rgb,
};

use super::shell::{page_padding, sidebar_width};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}

// The six public examples, followed by the reference's component-token example.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "carousel-basic",
        title: "Basic",
        description: "Display one slide at a time and switch slides with the indicators.",
    },
    Demo {
        id: "carousel-placement",
        title: "Position",
        description: "Place the indicators at the top, bottom, start, or end.",
    },
    Demo {
        id: "carousel-autoplay",
        title: "Scroll automatically",
        description: "Scroll automatically to the next slide every three seconds.",
    },
    Demo {
        id: "carousel-fade",
        title: "Fade in",
        description: "Fade between slides instead of scrolling.",
    },
    Demo {
        id: "carousel-arrows",
        title: "Arrows for switching",
        description: "Use arrows to switch slides. Navigation stops at the first and last slides.",
    },
    Demo {
        id: "carousel-dot-duration",
        title: "Progress of dots",
        description: "Show the time until the next slide in the active indicator.",
    },
    Demo {
        id: "carousel-component-token",
        title: "Component Token",
        description: "Customize the width, height, and active width of the indicators.",
    },
];

pub(super) struct CarouselPage {
    pub scope: ConfigScope,
    pub token_scope: ConfigScope,
    pub states: [Entity<CarouselState>; 8],
    pub placement: u8,
    pub last_change: Option<usize>,
    pub placement_focus: [FocusHandle; 4],
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _changes: Subscription,
}

fn slides(scope: ConfigScope) -> CarouselItems {
    CarouselItems::new(["one", "two", "three", "four"].into_iter().enumerate().map(
        |(index, key)| {
            let scope = scope.clone();
            CarouselSlide::new(key).content(move |_, cx| {
                div()
                    .w_full()
                    .h(px(160.))
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(rgb(0x364d79))
                    .text_color(rgb(0xffffff))
                    .text_size(px(scope.snapshot(cx).theme.tokens.font_size as f32 * 1.17))
                    .font_weight(FontWeight::BOLD)
                    .child((index + 1).to_string())
                    .into_any_element()
            })
        },
    ))
    .expect("unique example slide keys")
}

impl CarouselPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<CarouselTheme>(CarouselTokenPatch {
            dot_width: Some(50.),
            dot_height: Some(50.),
            dot_active_width: Some(80.),
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
            .expect("valid Carousel component theme");
        let states = std::array::from_fn(|_| {
            cx.new(|cx| CarouselState::new(slides(scope.clone()), window, cx))
        });
        let changes = cx.subscribe(&states[0], |this, _, event, cx| {
            if let CarouselEvent::AfterChange(change) = event {
                this.last_change = Some(change.to);
                cx.notify();
            }
        });
        Self {
            _config: scope.observe(cx),
            _changes: changes,
            scope,
            token_scope,
            states,
            placement: 0,
            last_change: None,
            placement_focus: std::array::from_fn(|_| cx.focus_handle()),
            anchors,
        }
    }

    fn carousel(&self, id: &'static str, index: usize) -> Carousel {
        Carousel::new(id, &self.states[index])
            .scope(self.scope.clone())
            .w_full()
            .min_w(px(0.))
    }

    fn observed(id: &'static str, element: impl IntoElement) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .w_full()
            .child(element)
            .into_any_element()
    }

    fn placement(&self) -> DotPlacement {
        match self.placement {
            0 => DotPlacement::Top,
            1 => DotPlacement::Bottom,
            2 => DotPlacement::Start,
            _ => DotPlacement::End,
        }
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "carousel-basic" => {
                Self::observed("carousel.basic", self.carousel("carousel.basic.control", 0))
            }
            "carousel-placement" => div()
                .w_full()
                .flex()
                .flex_col()
                .gap(px(8.))
                .child(
                    RadioGroup::new("carousel.placement.options")
                        .scope(self.scope.clone())
                        .accessibility_label("Dot placement")
                        .option_type(RadioOptionType::Button)
                        .options([(0u8, "Top"), (1, "Bottom"), (2, "Start"), (3, "End")].map(
                            |(value, label)| {
                                RadioOption::new(label, value, label)
                                    .automation_id(format!("carousel.placement.{label}"))
                                    .focus_handle(&self.placement_focus[value as usize])
                            },
                        ))
                        .value(self.placement)
                        .on_change(cx.listener(|this, value: &u8, _, cx| {
                            this.placement = *value;
                            cx.notify();
                        })),
                )
                .child(Self::observed(
                    "carousel.placement",
                    self.carousel("carousel.placement.control", 1)
                        .dot_placement(self.placement()),
                ))
                .into_any_element(),
            "carousel-autoplay" => Self::observed(
                "carousel.autoplay",
                self.carousel("carousel.autoplay.control", 2).autoplay(true),
            ),
            "carousel-fade" => Self::observed(
                "carousel.fade",
                self.carousel("carousel.fade.control", 3)
                    .effect(CarouselEffect::Fade),
            ),
            "carousel-arrows" => div()
                .w_full()
                .flex()
                .flex_col()
                .gap(px(22.))
                .child(Self::observed(
                    "carousel.arrows.horizontal",
                    self.carousel("carousel.arrows.horizontal.control", 4)
                        .arrows(true)
                        .infinite(false),
                ))
                .child(Self::observed(
                    "carousel.arrows.vertical",
                    self.carousel("carousel.arrows.vertical.control", 5)
                        .arrows(true)
                        .dot_placement(DotPlacement::Start)
                        .infinite(false),
                ))
                .into_any_element(),
            "carousel-dot-duration" => Self::observed(
                "carousel.duration",
                self.carousel("carousel.duration.control", 6)
                    .autoplay(true)
                    .autoplay_speed(Duration::from_secs(5))
                    .dot_duration(true),
            ),
            "carousel-component-token" => Self::observed(
                "carousel.tokens",
                self.carousel("carousel.tokens.control", 7)
                    .scope(self.token_scope.clone()),
            ),
            _ => unreachable!("registered Carousel example"),
        }
    }
}

impl Render for CarouselPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.carousel.examples")
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
                            .min_w(px(0.))
                            .p(px(tokens.padding_lg as f32))
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
