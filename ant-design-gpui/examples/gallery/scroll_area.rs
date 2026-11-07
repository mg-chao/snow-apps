// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::collections::BTreeMap;

use ant_design_gpui::{
    Button, ConfigScope, ScrollArea, ScrollAreaState, ScrollAxis, ScrollbarVisibility,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, Subscription,
    Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column, row};

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}

pub(super) const DEMOS: &[Demo] = &[
    demo(
        "vertical",
        "Vertical",
        "Wheel, drag the thumb, or focus the region and use Arrow, Page, Home and End keys. Content keeps its natural height.",
    ),
    demo(
        "horizontal",
        "Horizontal",
        "Use a trackpad, drag the horizontal thumb, or hold Shift while using the wheel.",
    ),
    demo(
        "both",
        "Both axes",
        "Explore a wide data sheet. Overlay scrollbars share the same native offset and keep their corner clear.",
    ),
    demo(
        "hover",
        "Reveal on hover",
        "Move into the region to reveal its scrollbars. Hover over a thumb to expand its visual width.",
    ),
    demo(
        "hidden",
        "Hidden scrollbars",
        "The scrollbar presentation is optional. Wheel, trackpad and keyboard scrolling remain available.",
    ),
    demo(
        "programmatic",
        "Programmatic scrolling",
        "Retain the state or its native ScrollHandle to scroll from application controls. Appearance changes preserve position.",
    ),
];

pub(super) struct ScrollAreaPage {
    scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub states: BTreeMap<&'static str, Entity<ScrollAreaState>>,
    _config: Subscription,
}

impl ScrollAreaPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self {
            states: DEMOS
                .iter()
                .map(|demo| (demo.id, cx.new(|cx| ScrollAreaState::new(window, cx))))
                .collect(),
            anchors,
            _config: scope.observe(cx),
            scope,
        }
    }

    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        let state = &self.states[id];
        let config = self.scope.snapshot(cx);
        let t = &config.theme.tokens;
        let area = ScrollArea::new(id, state)
            .scope(self.scope.clone())
            .automation_id(format!("scroll-area.{id}"))
            .accessibility_label(format!("{id} example content"))
            .w_full()
            .h(px(216.))
            .border_1()
            .border_color(t.color_border_secondary)
            .rounded(px(t.border_radius as f32))
            .bg(t.color_bg_container);
        match id {
            "horizontal" => area
                .axis(ScrollAxis::Horizontal)
                .child(
                    div()
                        .flex()
                        .gap(px(12.))
                        .p(px(16.))
                        .children((0..10).map(|index| {
                            div()
                                .w(px(148.))
                                .h(px(172.))
                                .flex_shrink_0()
                                .rounded(px(t.border_radius as f32))
                                .bg(if index % 2 == 0 {
                                    t.color_primary_bg
                                } else {
                                    t.color_fill_quaternary
                                })
                                .flex()
                                .flex_col()
                                .justify_between()
                                .p(px(16.))
                                .child(format!("Collection {:02}", index + 1))
                                .child(
                                    div()
                                        .text_color(t.color_text_secondary)
                                        .child(format!("{} files", 12 + index * 7)),
                                )
                        })),
                )
                .into_any_element(),
            "both" => area
                .axis(ScrollAxis::Both)
                .child(
                    div()
                        .w(px(820.))
                        .flex()
                        .flex_col()
                        .children((0..36).map(|index| {
                            div()
                                .flex()
                                .h(px(36.))
                                .flex_shrink_0()
                                .bg(if index % 2 == 0 {
                                    t.color_fill_quaternary
                                } else {
                                    t.color_bg_container
                                })
                                .children([
                                    div().w(px(260.)).px(px(16.)).child(if index == 0 {
                                        "Document".to_string()
                                    } else {
                                        format!("Design specification {index:02}")
                                    }),
                                    div().w(px(180.)).child(if index == 0 {
                                        "Owner"
                                    } else {
                                        "Design team"
                                    }),
                                    div().w(px(180.)).child(if index == 0 {
                                        "Status"
                                    } else {
                                        "In review"
                                    }),
                                    div().w(px(200.)).child(if index == 0 {
                                        "Last updated"
                                    } else {
                                        "October 27"
                                    }),
                                ])
                        })),
                )
                .into_any_element(),
            _ => {
                let area = area
                    .scrollbar_visibility(match id {
                        "hover" => ScrollbarVisibility::Hover,
                        "hidden" => ScrollbarVisibility::Hidden,
                        _ => ScrollbarVisibility::Always,
                    })
                    .child(
                        div()
                            .w_full()
                            .flex()
                            .flex_col()
                            .children((0..40).map(|index| {
                                div()
                                    .px(px(16.))
                                    .py(px(10.))
                                    .flex_shrink_0()
                                    .border_b_1()
                                    .border_color(t.color_border_secondary)
                                    .child(format!(
                                        "{} {:02}",
                                        if id == "programmatic" {
                                            "Activity"
                                        } else {
                                            "Document"
                                        },
                                        index + 1
                                    ))
                            })),
                    );
                if id != "programmatic" {
                    return area.into_any_element();
                }
                let top = state.clone();
                let bottom = state.clone();
                column()
                    .child(
                        row(&self.scope, cx)
                            .child(
                                Button::new("scroll.top")
                                    .scope(self.scope.clone())
                                    .label("Back to top")
                                    .on_click(move |_, _, cx| {
                                        top.update(cx, |state, cx| state.scroll_to_top(cx))
                                    }),
                            )
                            .child(
                                Button::new("scroll.bottom")
                                    .scope(self.scope.clone())
                                    .label("Latest activity")
                                    .on_click(move |_, _, cx| {
                                        bottom.update(cx, |state, cx| state.scroll_to_bottom(cx))
                                    }),
                            ),
                    )
                    .child(area)
                    .into_any_element()
            }
        }
    }
}

impl Render for ScrollAreaPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.scroll-area.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "scroll-area",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
