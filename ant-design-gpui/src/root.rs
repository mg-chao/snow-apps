// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A retained window root providing traversal and theme defaults.

use crate::{
    ConfigScope, Direction, Message, MessageHost, MessageState, behavior, popup::PopupHost,
};
use gpui::{
    AnyView, Context, Entity, FocusHandle, Focusable, IntoElement, Render, Subscription, Window,
    div, prelude::*, px, relative,
};

/// Own one per native window. Child views observe the same scope, or a retained
/// child scope, for configuration-dependent content of their own.
pub struct UiRoot {
    scope: ConfigScope,
    content: AnyView,
    focus: FocusHandle,
    messages: Entity<MessageState>,
    _config_subscription: Subscription,
    _focus_subscription: Subscription,
}

#[cfg(test)]
mod tests;

impl UiRoot {
    pub fn new(
        scope: ConfigScope,
        content: impl Into<AnyView>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        crate::init(cx);
        let subscription = scope.observe(cx);
        let messages = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
        Message::new(&messages).register(window, cx);
        let focus = cx.focus_handle();
        if window.focused(cx).is_none() {
            window.focus(&focus, cx);
        }
        let focus_subscription = cx.on_focus_lost(window, |this, window, cx| {
            // This event means the rendered focus path disappeared. A removed
            // control can retain its handle, so focused() may still return it.
            window.focus(&this.focus, cx);
        });
        Self {
            scope,
            content: content.into(),
            focus,
            messages,
            _config_subscription: subscription,
            _focus_subscription: focus_subscription,
        }
    }

    pub fn config(&self) -> ConfigScope {
        self.scope.clone()
    }

    pub fn messages(&self) -> Message {
        Message::new(&self.messages)
    }
}

impl Focusable for UiRoot {
    fn focus_handle(&self, _: &gpui::App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for UiRoot {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let content = behavior::traversal(div().id("ant-design-root").track_focus(&self.focus))
            .size_full()
            .bg(tokens.color_bg_layout)
            .text_color(tokens.color_text)
            .text_size(px(tokens.font_size as f32))
            .font(config.font.clone())
            .line_height(relative(tokens.line_height as f32))
            .when(config.direction == Direction::Rtl, |root| root.text_right())
            .child(self.content.clone());
        MessageHost::new(
            "ant-design-message-host",
            &self.messages,
            PopupHost::new("ant-design-popup-host", content),
        )
    }
}
