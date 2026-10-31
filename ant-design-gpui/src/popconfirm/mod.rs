// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Confirmation dialogs with native buttons and guarded asynchronous completion.
mod style;
pub use style::{PopconfirmStyle, PopconfirmTheme, PopconfirmTokenPatch, PopconfirmTokens};

use crate::{
    Button, Direction, Size,
    behavior::{Activation, focus_handle},
    button::ButtonType,
    icons::{IconName, IconPalette, IconRef, IconRenderOptions, icon},
    popover::{PopoverStyle, PopoverTheme},
    popup::{
        Popup, PopupHandle, PopupOpenReason,
        component::{Slot, popup_methods},
    },
};
use gpui::{
    App, Context, ElementId, Entity, FocusHandle, FontWeight, IntoElement, Refineable, RenderOnce,
    Role, SharedString, StyleRefinement, Task, WeakEntity, Window, div, prelude::*, px, relative,
    size,
};
use std::rc::Rc;

/// The result of a confirmation callback. Rejections leave the dialog open.
pub enum Confirmation {
    Close,
    KeepOpen,
    /// A task already started by the application. Dismissing/unmounting the
    /// dialog does not cancel it; stale completions cannot affect a new dialog.
    Pending(Task<Result<(), SharedString>>),
}

#[derive(Clone, Debug, Default)]
pub struct PopconfirmButtonOptions {
    pub button_type: Option<ButtonType>,
    pub disabled: bool,
    pub loading: bool,
    pub danger: bool,
    pub style: StyleRefinement,
}
impl PopconfirmButtonOptions {
    fn apply(&self, mut button: Button, default_type: ButtonType, pending: bool) -> Button {
        button = button
            .button_type(self.button_type.unwrap_or(default_type))
            .disabled(self.disabled)
            .loading(self.loading || pending)
            .danger(self.danger);
        button.style().refine(&self.style);
        button
    }
}

type ConfirmHandler = Rc<dyn Fn(&Activation, &mut Window, &mut App) -> Confirmation>;
type CancelHandler = Rc<dyn Fn(&Activation, &mut Window, &mut App)>;
type ErrorHandler = Rc<dyn Fn(&SharedString, &mut Window, &mut App)>;

#[derive(IntoElement)]
pub struct Popconfirm {
    popup: Popup,
    title: Option<Slot>,
    description: Option<Slot>,
    icon: Option<IconRef>,
    ok_text: Option<SharedString>,
    cancel_text: Option<SharedString>,
    show_cancel: bool,
    ok_button: PopconfirmButtonOptions,
    cancel_button: PopconfirmButtonOptions,
    on_confirm: Option<ConfirmHandler>,
    on_cancel: Option<CancelHandler>,
    on_error: Option<ErrorHandler>,
    title_style: StyleRefinement,
    description_style: StyleRefinement,
    icon_style: StyleRefinement,
    buttons_style: StyleRefinement,
}
impl Popconfirm {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            popup: Popup::new(id).accessibility_role(Role::AlertDialog),
            title: None,
            description: None,
            icon: Some(IconName::ExclamationCircleFilled.into()),
            ok_text: None,
            cancel_text: None,
            show_cancel: true,
            ok_button: PopconfirmButtonOptions::default(),
            cancel_button: PopconfirmButtonOptions::default(),
            on_confirm: None,
            on_cancel: None,
            on_error: None,
            title_style: StyleRefinement::default(),
            description_style: StyleRefinement::default(),
            icon_style: StyleRefinement::default(),
            buttons_style: StyleRefinement::default(),
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
    pub fn description(mut self, description: impl Into<SharedString>) -> Self {
        self.description = Slot::text(description);
        self
    }
    pub fn description_content<E: IntoElement>(
        mut self,
        description: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.description = Some(Slot::content(description));
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }
    pub fn show_icon(mut self, show: bool) -> Self {
        if !show {
            self.icon = None;
        } else if self.icon.is_none() {
            self.icon = Some(IconName::ExclamationCircleFilled.into());
        }
        self
    }
    pub fn ok_text(mut self, text: impl Into<SharedString>) -> Self {
        self.ok_text = Some(text.into());
        self
    }
    pub fn cancel_text(mut self, text: impl Into<SharedString>) -> Self {
        self.cancel_text = Some(text.into());
        self
    }
    pub fn show_cancel(mut self, show: bool) -> Self {
        self.show_cancel = show;
        self
    }
    pub fn ok_button(mut self, options: PopconfirmButtonOptions) -> Self {
        self.ok_button = options;
        self
    }
    pub fn cancel_button(mut self, options: PopconfirmButtonOptions) -> Self {
        self.cancel_button = options;
        self
    }
    pub fn on_confirm(
        mut self,
        callback: impl Fn(&Activation, &mut Window, &mut App) -> Confirmation + 'static,
    ) -> Self {
        self.on_confirm = Some(Rc::new(callback));
        self
    }
    pub fn on_confirm_async(
        self,
        callback: impl Fn(&Activation, &mut Window, &mut App) -> Task<Result<(), SharedString>>
        + 'static,
    ) -> Self {
        self.on_confirm(move |activation, window, cx| {
            Confirmation::Pending(callback(activation, window, cx))
        })
    }
    pub fn on_cancel(
        mut self,
        callback: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_cancel = Some(Rc::new(callback));
        self
    }
    pub fn on_confirm_error(
        mut self,
        callback: impl Fn(&SharedString, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_error = Some(Rc::new(callback));
        self
    }
    pub fn title_style(mut self, style: StyleRefinement) -> Self {
        self.title_style = style;
        self
    }
    pub fn description_style(mut self, style: StyleRefinement) -> Self {
        self.description_style = style;
        self
    }
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = style;
        self
    }
    pub fn buttons_style(mut self, style: StyleRefinement) -> Self {
        self.buttons_style = style;
        self
    }
}

struct ConfirmState {
    handle: PopupHandle,
    confirm_focus: FocusHandle,
    cancel_focus: FocusHandle,
    request: u64,
    pending_session: Option<crate::popup::PopupSession>,
    on_confirm: Option<ConfirmHandler>,
    on_error: Option<ErrorHandler>,
}
impl ConfirmState {
    fn new(cx: &mut Context<Self>) -> Self {
        Self {
            handle: PopupHandle::new(),
            confirm_focus: focus_handle(cx),
            cancel_focus: focus_handle(cx),
            request: 0,
            pending_session: None,
            on_confirm: None,
            on_error: None,
        }
    }
    fn is_pending(&self, cx: &App) -> bool {
        self.pending_session.is_some()
            && self.pending_session == self.handle.session_id(cx)
            && self.handle.is_open(cx)
    }
}

fn confirm(
    state: &WeakEntity<ConfirmState>,
    activation: &Activation,
    window: &mut Window,
    cx: &mut App,
) {
    let Some(entity) = state.upgrade() else {
        return;
    };
    let Some((request, callback)) = entity.update(cx, |state, cx| {
        if state.is_pending(cx) || !state.handle.is_open(cx) {
            return None;
        }
        state.request = state.request.wrapping_add(1);
        state.pending_session = state.handle.session_id(cx);
        cx.notify();
        Some((state.request, state.on_confirm.clone()))
    }) else {
        return;
    };
    // User code can synchronously close or unmount this control; no entity lease
    // is held across this call or across the application task's lifetime.
    match callback.map_or(Confirmation::Close, |callback| {
        callback(activation, window, cx)
    }) {
        Confirmation::Close => finish(state, request, Ok(()), true, window, cx),
        Confirmation::KeepOpen => finish(state, request, Ok(()), false, window, cx),
        Confirmation::Pending(task) => {
            entity.update(cx, |_, cx| {
                cx.spawn_in(window, async move |state, cx| {
                    let result = task.await;
                    let _ =
                        cx.update(|window, cx| finish(&state, request, result, true, window, cx));
                })
                .detach();
            });
        }
    }
}

fn finish(
    state: &WeakEntity<ConfirmState>,
    request: u64,
    result: Result<(), SharedString>,
    close: bool,
    window: &mut Window,
    cx: &mut App,
) {
    let Ok(Some((handle, error_handler))) = state.update(cx, |state, cx| {
        if state.request != request || !state.is_pending(cx) {
            return None;
        }
        state.pending_session = None;
        cx.notify();
        Some((state.handle.clone(), state.on_error.clone()))
    }) else {
        return;
    };
    match result {
        Ok(()) if close => handle.request(false, PopupOpenReason::Confirm, window, cx),
        Ok(()) => {}
        Err(error) => {
            if let Some(callback) = error_handler {
                callback(&error, window, cx);
            }
        }
    }
}

impl RenderOnce for Popconfirm {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.popup.resolve_scope(cx);
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<PopconfirmTheme>();
        let surface_tokens = config.theme.component::<PopoverTheme>();
        let style = PopconfirmStyle::from_tokens(&tokens, self.description.is_some());
        let surface_style = PopoverStyle::from_tokens(&surface_tokens, None);
        let state: Entity<ConfirmState> =
            window.use_keyed_state(self.popup.id.clone(), cx, |_, cx| ConfirmState::new(cx));
        state.update(cx, |state, cx| {
            if let Some(handle) = self.popup.handle_value() {
                state.handle = handle;
            }
            state.on_confirm = self.on_confirm.clone();
            state.on_error = self.on_error.clone();
            if !state.is_pending(cx) {
                state.pending_session = None;
            }
        });
        let retained = state.read(cx);
        let handle = retained.handle.clone();
        let confirm_focus = retained.confirm_focus.clone();
        let cancel_focus = retained.cancel_focus.clone();
        let initial_focus = if self.show_cancel && !self.cancel_button.disabled {
            Some(&cancel_focus)
        } else if !self.ok_button.disabled {
            Some(&confirm_focus)
        } else {
            None
        };
        let mut surface = div()
            .p(px(surface_style.padding as f32))
            .font(tokens.font().clone())
            .text_size(px(style.font_size as f32))
            .line_height(relative(style.line_height as f32))
            .text_color(style.description_color);
        let mut popup = self
            .popup
            .defaults(config.popconfirm, tokens.z_index_popup)
            .handle(&handle)
            .label_default(self.title.as_ref().and_then(|title| title.text.clone()))
            .description_default(
                self.description
                    .as_ref()
                    .and_then(|description| description.text.clone()),
            )
            .accessibility_busy(retained.is_pending(cx))
            .appearance(
                surface.style().clone(),
                surface_style.background,
                px(surface_style.radius as f32),
                surface_tokens.globals(),
                surface_tokens.globals().border_radius_outer,
            );
        if let Some(focus) = initial_focus {
            popup = popup.initial_focus(focus);
        }
        let ok_text = self
            .ok_text
            .unwrap_or_else(|| config.locale.popconfirm.ok_text.clone());
        let cancel_text = self
            .cancel_text
            .unwrap_or_else(|| config.locale.popconfirm.cancel_text.clone());
        let rtl = config.direction == Direction::Rtl;
        let title = self.title;
        let description = self.description;
        let weak = state.downgrade();
        popup.content(move |handle, window, cx| {
            // The factory can remain cached while closed. Read live pending state
            // when it runs so a task completion never leaves a stale spinner.
            let pending = weak
                .upgrade()
                .is_some_and(|state| state.read(cx).is_pending(cx));
            let mut texts = div().flex().flex_col().min_w(px(0.));
            if let Some(title) = &title {
                let mut element = div()
                    .font_weight(FontWeight(style.title_weight as f32))
                    .text_color(style.title_color)
                    .child(title.build(handle, window, cx));
                element.style().refine(&self.title_style);
                texts = texts.child(element);
            }
            if let Some(description) = &description {
                let mut element = div()
                    .mt(px(style.description_margin_top as f32))
                    .text_color(style.description_color)
                    .child(description.build(handle, window, cx));
                element.style().refine(&self.description_style);
                texts = texts.child(element);
            }
            let mut message = div()
                .flex()
                .items_start()
                .mb(px(style.message_margin_bottom as f32))
                .when(rtl, |element| element.flex_row_reverse());
            if let Some(reference) = &self.icon {
                let icon_size = self
                    .icon_style
                    .text
                    .font_size
                    .map_or(px(style.font_size as f32), |size| {
                        size.to_pixels(window.rem_size())
                    });
                let options = IconRenderOptions {
                    size: size(icon_size, icon_size),
                    colors: IconPalette {
                        text: self
                            .icon_style
                            .text
                            .color
                            .unwrap_or(style.icon_color)
                            .into(),
                        ..Default::default()
                    },
                    ..Default::default()
                };
                let mut element = div()
                    .flex()
                    .items_center()
                    .h(px((style.font_size * style.line_height) as f32).max(icon_size))
                    .flex_shrink_0()
                    .when(rtl, |element| element.ml(px(style.icon_gap as f32)))
                    .when(!rtl, |element| element.mr(px(style.icon_gap as f32)))
                    .child(icon(reference.clone()).render_options(options));
                element.style().refine(&self.icon_style);
                message = message.child(element);
            }
            message = message.child(texts);
            let mut buttons = div()
                .flex()
                .justify_end()
                .when(rtl, |element| element.flex_row_reverse().justify_start());
            if self.show_cancel {
                let handle = handle.clone();
                let callback = self.on_cancel.clone();
                let button = self.cancel_button.apply(
                    Button::new("cancel")
                        .scope(scope.clone())
                        .size(Size::Small)
                        .label(cancel_text.clone())
                        .focus_handle(&cancel_focus)
                        .on_click(move |activation, window, cx| {
                            handle.request(false, PopupOpenReason::Cancel, window, cx);
                            if let Some(callback) = &callback {
                                callback(activation, window, cx);
                            }
                        }),
                    ButtonType::Default,
                    false,
                );
                buttons = buttons.child(
                    div()
                        .when(rtl, |element| {
                            element.mr(px(style.button_margin_start as f32))
                        })
                        .when(!rtl, |element| {
                            element.ml(px(style.button_margin_start as f32))
                        })
                        .child(button),
                );
            }
            let weak = weak.clone();
            let button = self.ok_button.apply(
                Button::new("confirm")
                    .scope(scope.clone())
                    .size(Size::Small)
                    .label(ok_text.clone())
                    .focus_handle(&confirm_focus)
                    .on_click(move |activation, window, cx| confirm(&weak, activation, window, cx)),
                ButtonType::Primary,
                pending,
            );
            buttons = buttons.child(
                div()
                    .when(rtl, |element| {
                        element.mr(px(style.button_margin_start as f32))
                    })
                    .when(!rtl, |element| {
                        element.ml(px(style.button_margin_start as f32))
                    })
                    .child(button),
            );
            buttons.style().refine(&self.buttons_style);
            div()
                .flex()
                .flex_col()
                .px(px(surface_style.content_padding_inline as f32))
                .py(px(surface_style.content_padding_block as f32))
                .child(message)
                .child(buttons)
        })
    }
}
