// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Shared presentation and coverage labels for the two input reference pages.

use ant_design_gpui::{
    ConfigScope, Direction,
    icons::{Icon, IconName, icon},
};
use gpui::{
    AnyElement, App, FontWeight, IntoElement, Role, ScrollAnchor, SharedString, Window, div,
    prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Coverage {
    Complete,
    Partial(&'static str),
    Planned(&'static str),
}

impl Coverage {
    pub fn planned(self) -> bool {
        matches!(self, Self::Planned(_))
    }
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub coverage: Coverage,
}

pub(super) fn summary(demos: &[Demo]) -> String {
    let complete = demos
        .iter()
        .filter(|d| d.coverage == Coverage::Complete)
        .count();
    let planned = demos.iter().filter(|d| d.coverage.planned()).count();
    let partial = demos.len() - complete - planned;
    format!("{complete} complete · {partial} partial · {planned} planned")
}

pub(super) fn column() -> gpui::Div {
    div().w_full().min_w_0().flex().flex_col().gap(px(16.))
}

pub(super) fn decoration(scope: &ConfigScope, name: IconName, cx: &App) -> Icon {
    let config = scope.snapshot(cx);
    icon(name)
        .size(px(config.theme.tokens.font_size as f32))
        .palette(config.icon_palette)
}

pub(super) fn row(scope: &ConfigScope, cx: &App) -> gpui::Div {
    div()
        .min_w_0()
        .flex()
        .flex_wrap()
        .items_center()
        .gap(px(8.))
        .when(scope.snapshot(cx).direction == Direction::Rtl, |row| {
            row.flex_row_reverse()
        })
}

pub(super) fn control(id: impl Into<SharedString>, element: impl IntoElement) -> AnyElement {
    let id = id.into();
    div()
        .id(id.clone())
        .debug_selector(move || id.to_string())
        .min_w_0()
        .child(element)
        .into_any_element()
}

pub(super) fn grid(scope: &ConfigScope, window: &Window, cx: &App) -> gpui::Div {
    let viewport = window.viewport_size().width;
    let content = viewport - sidebar_width(viewport) - page_padding(viewport) * 2.;
    let config = scope.snapshot(cx);
    div()
        .w_full()
        .grid()
        .grid_cols(if content >= px(840.) { 2 } else { 1 })
        .items_start()
        .gap(px(config.theme.tokens.margin_lg as f32))
        .when(config.direction == Direction::Rtl, |page| page.text_right())
}

pub(super) fn card(
    page: &'static str,
    demo: &'static Demo,
    anchor: ScrollAnchor,
    preview: impl IntoElement,
    scope: &ConfigScope,
    cx: &App,
) -> AnyElement {
    let config = scope.snapshot(cx);
    let tokens = &config.theme.tokens;
    let (badge, note) = match demo.coverage {
        Coverage::Complete => ("Complete", None),
        Coverage::Partial(note) => ("Partial", Some(note)),
        Coverage::Planned(note) => ("Planned", Some(note)),
    };
    div()
        .id(demo.id)
        .anchor_scroll(Some(anchor))
        .role(Role::Group)
        .aria_label(demo.title)
        .debug_selector(move || format!("gallery.{page}.card.{}", demo.id))
        .min_w_0()
        .border_1()
        .border_color(tokens.color_border_secondary)
        .rounded(px(tokens.border_radius_lg as f32))
        .bg(tokens.color_bg_container)
        .child(
            div()
                .p(px(tokens.padding as f32))
                .flex()
                .items_center()
                .justify_between()
                .gap(px(8.))
                .child(
                    div()
                        .min_w_0()
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(demo.title),
                )
                .child(
                    div()
                        .flex_shrink_0()
                        .rounded(px(tokens.border_radius_sm as f32))
                        .bg(tokens.color_fill_quaternary)
                        .px(px(6.))
                        .py(px(2.))
                        .text_size(px(tokens.font_size_sm as f32))
                        .text_color(tokens.color_text_secondary)
                        .child(badge),
                ),
        )
        .child(
            div()
                .id("preview")
                .role(Role::Group)
                .aria_label(format!("{} preview", demo.title))
                .debug_selector(move || format!("gallery.{page}.preview.{}", demo.id))
                .min_w_0()
                .p(px(tokens.padding_lg as f32))
                .when(!demo.coverage.planned(), |preview| preview.min_h(px(110.)))
                .flex()
                .flex_col()
                .justify_center()
                .gap(px(12.))
                .when(!demo.coverage.planned(), |container| {
                    container.child(preview)
                })
                .when_some(note, |container, note| {
                    container.child(
                        div()
                            .id("pending")
                            .role(Role::Note)
                            .debug_selector(move || format!("gallery.{page}.pending.{}", demo.id))
                            .text_size(px(tokens.font_size_sm as f32))
                            .line_height(relative(tokens.line_height as f32))
                            .text_color(tokens.color_text_secondary)
                            .child(format!("Pending: {note}")),
                    )
                }),
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
        .into_any_element()
}
