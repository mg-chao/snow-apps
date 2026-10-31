// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Anchored title and content with independently controlled trigger and surface state.
mod style;
pub use style::{PopoverStyle, PopoverTheme, PopoverTokenPatch, PopoverTokens};

use crate::popup::{
    Popup, PopupColor, PopupHandle,
    component::{Slot, popup_methods},
};
use gpui::{
    App, ElementId, FontWeight, IntoElement, Refineable, RenderOnce, SharedString, StyleRefinement,
    Window, div, prelude::*, px, relative,
};

#[derive(IntoElement)]
pub struct Popover {
    popup: Popup,
    title: Option<Slot>,
    content: Option<Slot>,
    color: Option<PopupColor>,
    title_style: StyleRefinement,
    content_style: StyleRefinement,
}
impl Popover {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            popup: Popup::new(id),
            title: None,
            content: None,
            color: None,
            title_style: StyleRefinement::default(),
            content_style: StyleRefinement::default(),
        }
    }
    popup_methods!();
    pub fn title(mut self, title: impl Into<SharedString>) -> Self {
        self.title = Slot::text(title);
        self
    }
    pub fn title_content<E: IntoElement>(
        mut self,
        title: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.title = Some(Slot::content(title));
        self
    }
    pub fn text(mut self, text: impl Into<SharedString>) -> Self {
        self.content = Slot::text(text);
        self
    }
    pub fn content<E: IntoElement>(
        mut self,
        content: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.content = Some(Slot::content(content));
        self
    }
    pub fn color(mut self, color: impl Into<PopupColor>) -> Self {
        self.color = Some(color.into());
        self
    }
    pub fn title_style(mut self, style: StyleRefinement) -> Self {
        self.title_style = style;
        self
    }
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = style;
        self
    }
    pub fn initial_focus(mut self, focus: &gpui::FocusHandle) -> Self {
        self.popup = self.popup.initial_focus(focus);
        self
    }
}
impl RenderOnce for Popover {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.popup.resolve_scope(cx);
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<PopoverTheme>();
        let style = PopoverStyle::from_tokens(&tokens, self.color);
        let g = tokens.globals();
        let mut surface = div()
            .p(px(style.padding as f32))
            .font(tokens.font().clone())
            .text_size(px(style.font_size as f32))
            .line_height(relative(style.line_height as f32))
            .text_color(style.foreground);
        let popup = self
            .popup
            .defaults(config.popover, tokens.z_index_popup)
            .appearance(
                surface.style().clone(),
                style.background,
                px(style.radius as f32),
                g,
                g.border_radius_outer,
            )
            .label_default(self.title.as_ref().and_then(|title| title.text.clone()))
            .description_default(
                self.content
                    .as_ref()
                    .and_then(|content| content.text.clone()),
            )
            .disabled_when(self.title.is_none() && self.content.is_none());
        let title = self.title;
        let content = self.content;
        popup.content(move |handle, window, cx| {
            let mut column = div().flex().flex_col();
            if let Some(title) = &title {
                let mut element = div()
                    .min_w(px(style.title_min_width as f32))
                    .mb(px(style.title_margin_bottom as f32))
                    .pt(px(style.title_padding_top as f32))
                    .pb(px(style.title_padding_bottom as f32))
                    .px(px(style.title_padding_inline as f32))
                    .border_b(px(style.title_border_width as f32))
                    .border_color(style.title_border_color)
                    .font_weight(FontWeight(style.title_weight as f32))
                    .text_color(style.title_color)
                    .child(title.build(handle, window, cx));
                element.style().refine(&self.title_style);
                column = column.child(element);
            }
            if let Some(content) = &content {
                let mut element = div()
                    .px(px(style.content_padding_inline as f32))
                    .py(px(style.content_padding_block as f32))
                    .text_color(style.foreground)
                    .child(content.build(handle, window, cx));
                element.style().refine(&self.content_style);
                column = column.child(element);
            }
            column
        })
    }
}
