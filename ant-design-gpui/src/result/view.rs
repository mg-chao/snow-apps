// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    App, IntoElement, Refineable, RenderOnce, Styled, Subscription, Window, canvas, div,
    prelude::*, px, relative,
};

use super::{Result, ResultStatus, ResultStyle, ResultStyleContext, ResultTheme};
use crate::{
    ConfigScope, Direction,
    icons::{IconName, IconPalette, IconRef, IconRenderOptions, IconRenderer, icon},
};

struct ResultScope {
    scope: ConfigScope,
    _subscription: Subscription,
}

impl RenderOnce for Result {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .take()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let observer = window.use_keyed_state(self.id.clone(), cx, |window, cx| ResultScope {
            _subscription: scope.observe_in(window, cx, |_, _, cx| cx.notify()),
            scope: scope.clone(),
        });
        observer.update(cx, |state, cx| {
            if state.scope != scope {
                state._subscription = scope.observe_in(window, cx, |_, _, cx| cx.notify());
                state.scope = scope.clone();
            }
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<ResultTheme>();
        let metrics = ResultStyle::from_tokens(&tokens, self.status);
        let illustration = self.status.illustration();
        let show_icon = illustration.is_some() || self.show_icon;
        let context = ResultStyleContext {
            status: self.status,
            direction: config.direction,
            has_title: self.title.is_some(),
            has_sub_title: self.sub_title.is_some(),
            has_extra: !self.extra.is_empty(),
            has_body: !self.children.is_empty(),
            show_icon,
        };
        let mut styles = config.result.styles.clone();
        if let Some(resolver) = &config.result.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        styles.root.refine(&self.style);

        // Native block layout preserves the reference's collapsing sibling margins.
        let mut root = div()
            .id(self.id)
            .block()
            .w_full()
            .min_w(px(0.))
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .text_color(metrics.text_color)
            .py(px(metrics.padding_vertical as f32))
            .px(px(metrics.padding_horizontal as f32))
            .when(config.direction == Direction::Rtl, |d| d.text_right());
        root.style().refine(&styles.root);

        if show_icon {
            let mut slot = div()
                .id("icon")
                .text_center()
                .text_color(metrics.icon_color)
                .mb(px(metrics.icon_margin_bottom as f32));
            let child = if let Some(illustration) = illustration {
                slot = slot
                    .flex()
                    .flex_shrink_0()
                    .w(px(metrics.image_width as f32))
                    .h(px(metrics.image_height as f32))
                    .mx_auto()
                    .when(config.direction == Direction::Rtl, |d| d.justify_end());
                icon(illustration)
                    .w(px(self
                        .status
                        .illustration_width()
                        .expect("exception width")))
                    .h(px(294.))
                    .into_any_element()
            } else {
                slot = slot.flex().justify_center().items_center();
                if let Some(content) = self.icon_content.take() {
                    content
                } else {
                    let reference = self.icon.take().unwrap_or_else(|| {
                        match self.status {
                            ResultStatus::Success => IconName::CheckCircleFilled,
                            ResultStatus::Error => IconName::CloseCircleFilled,
                            ResultStatus::Warning => IconName::WarningFilled,
                            _ => IconName::ExclamationCircleFilled,
                        }
                        .into()
                    });
                    foreground_icon(reference, tokens.icon_palette())
                        .size(px(metrics.icon_font_size as f32))
                        .into_any_element()
                }
            };
            slot.style().refine(&styles.icon);
            if self.status.illustration_width().is_some() {
                // Auto margins center a fitting illustration. When it overflows,
                // GPUI's native flex row places it at the paragraph's inline start.
                // Keep block margins on that row so sibling collapse is unchanged.
                let top = slot.style().margin.top.take();
                let bottom = slot.style().margin.bottom.take();
                let hidden = slot.style().display == Some(gpui::Display::None);
                let mut row = div()
                    .w_full()
                    .flex()
                    .when(config.direction == Direction::Rtl, |d| d.justify_end())
                    .when(hidden, |d| d.hidden());
                row.style().margin.top = top;
                row.style().margin.bottom = bottom;
                root = root.child(row.child(slot.child(child)));
            } else {
                root = root.child(slot.child(child));
            }
        }

        if let Some(title) = self.title {
            let mut slot = div()
                .id("title")
                .min_w(px(0.))
                .text_center()
                .text_color(metrics.title_color)
                .text_size(px(metrics.title_font_size as f32))
                .line_height(relative(metrics.title_line_height as f32))
                .my(px(metrics.title_margin as f32));
            slot.style().refine(&styles.title);
            #[cfg(test)]
            {
                slot = slot.debug_selector(|| "result-title-slot".into());
            }
            root = root.child(slot.child(title.into_element(config.direction)));
        }
        if let Some(sub_title) = self.sub_title {
            let mut slot = div()
                .id("sub_title")
                .min_w(px(0.))
                .text_center()
                .text_color(metrics.subtitle_color)
                .text_size(px(metrics.subtitle_font_size as f32))
                .line_height(relative(metrics.line_height as f32));
            slot.style().refine(&styles.sub_title);
            root = root.child(slot.child(sub_title.into_element(config.direction)));
        }
        if !self.extra.is_empty() {
            let mut slot = div()
                .id("extra")
                .flex()
                .justify_center()
                .items_center()
                .text_center()
                .flex_wrap()
                .mt(px(metrics.extra_margin.top as f32))
                .mr(px(metrics.extra_margin.right as f32))
                .mb(px(metrics.extra_margin.bottom as f32))
                .ml(px(metrics.extra_margin.left as f32))
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            slot.style().refine(&styles.extra);
            let count = self.extra.len();
            root = root.child(slot.children(self.extra.into_iter().enumerate().map(
                |(index, child)| {
                    let text = matches!(child, super::Content::Text(_));
                    let wrapper =
                        div()
                            .flex_none()
                            .when(index + 1 < count, |d| match config.direction {
                                Direction::Ltr => d.mr(px(metrics.extra_gap as f32)),
                                Direction::Rtl => d.ml(px(metrics.extra_gap as f32)),
                            });
                    if text {
                        wrapper
                            .id(("extra-text", index))
                            .max_w_full()
                            .min_w(px(0.))
                            .child(child.into_element(config.direction))
                            .into_any_element()
                    } else {
                        wrapper
                            .child(child.into_element(config.direction))
                            .into_any_element()
                    }
                },
            )));
        }
        if !self.children.is_empty() {
            let mut slot = div()
                .id("body")
                .mt(px(metrics.body_margin_top as f32))
                .py(px(metrics.body_padding_vertical as f32))
                .px(px(metrics.body_padding_horizontal as f32))
                .bg(metrics.body_background);
            slot.style().refine(&styles.body);
            root = root.child(slot.children(self.children.into_iter().enumerate().map(
                |(index, child)| {
                    if matches!(child, super::Content::Text(_)) {
                        div()
                            .id(("body-text", index))
                            .min_w(px(0.))
                            .child(child.into_element(config.direction))
                            .into_any_element()
                    } else {
                        child.into_element(config.direction)
                    }
                },
            )));
        }
        self.accessibility.apply(root)
    }
}

fn foreground_icon(reference: IconRef, palette: IconPalette) -> gpui::Canvas<()> {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            if cx.try_global::<IconRenderer>().is_none() {
                cx.set_global(IconRenderer::default());
            }
            let options = IconRenderOptions {
                size: bounds.size,
                scale_factor: window.scale_factor(),
                colors: IconPalette {
                    text: window.text_style().color.into(),
                    ..palette
                },
                ..Default::default()
            };
            cx.update_global::<IconRenderer, _>(|renderer, cx| {
                let _ = renderer.paint_with_scale(
                    bounds,
                    &reference,
                    &options,
                    reference.ant_design_viewbox_scale(),
                    window,
                    cx,
                );
            });
        },
    )
    .flex_shrink_0()
}
