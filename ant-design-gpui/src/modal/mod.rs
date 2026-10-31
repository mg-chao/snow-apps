// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained, window-local modal dialogs and scoped confirmation helpers.

mod config;
pub(crate) mod host;
mod manager;
mod skeleton;
mod state;
mod style;
mod view;

pub use config::{ModalConfig, ModalConfigPatch};
pub use host::ModalHost;
pub use manager::{ModalHandle, ModalManager, ModalOutcome, ModalResult};
pub use state::{ModalEvent, ModalPhase, ModalState};
pub use style::{ModalPadding, ModalTheme, ModalTokenPatch, ModalTokens};
pub use view::Modal;

use crate::{
    button::ButtonType,
    icons::{IconName, IconRef},
};
use gpui::{
    AnyElement, AnyView, App, FocusHandle, IntoElement, SharedString, StyleRefinement, Task, Window,
};
use std::rc::Rc;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ModalKind {
    #[default]
    Standard,
    Confirm,
    Info,
    Success,
    Warning,
    Error,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ModalCancelReason {
    Button,
    Close,
    Mask,
    Escape,
    Accessibility,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ModalAutoFocus {
    #[default]
    Dialog,
    Ok,
    Cancel,
    None,
}

/// Native widths use logical pixels or a fraction of the window's content width.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ModalLength {
    Pixels(f32),
    Fraction(f32),
}
impl From<f32> for ModalLength {
    fn from(value: f32) -> Self {
        Self::Pixels(value)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum ModalWidth {
    Fixed(ModalLength),
    /// xs, sm, md, lg, xl, xxl. Missing breakpoints inherit the preceding width.
    Responsive([Option<ModalLength>; 6]),
}
impl From<f32> for ModalWidth {
    fn from(value: f32) -> Self {
        Self::Fixed(value.into())
    }
}

type ContentRenderer = dyn Fn(&ModalHandle, &mut Window, &mut App) -> AnyElement;
type ViewFactory = dyn Fn(&mut Window, &mut App) -> AnyView;

/// A text value, fresh native element renderer, or lazily retained view factory.
#[derive(Clone)]
pub struct ModalContent(ContentSource);
#[derive(Clone)]
enum ContentSource {
    Text(SharedString),
    Render(Rc<ContentRenderer>),
    View(Rc<ViewFactory>),
}
impl ModalContent {
    pub fn text(text: impl Into<SharedString>) -> Self {
        Self(ContentSource::Text(text.into()))
    }
    pub fn render<E: IntoElement>(
        render: impl Fn(&ModalHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        Self(ContentSource::Render(Rc::new(move |h, w, cx| {
            render(h, w, cx).into_any_element()
        })))
    }
    pub fn view(factory: impl Fn(&mut Window, &mut App) -> AnyView + 'static) -> Self {
        Self(ContentSource::View(Rc::new(factory)))
    }
    fn same(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (ContentSource::Text(a), ContentSource::Text(b)) => a == b,
            (ContentSource::Render(a), ContentSource::Render(b)) => Rc::ptr_eq(a, b),
            (ContentSource::View(a), ContentSource::View(b)) => Rc::ptr_eq(a, b),
            _ => false,
        }
    }
    fn label(&self) -> Option<SharedString> {
        match &self.0 {
            ContentSource::Text(s) => Some(s.clone()),
            _ => None,
        }
    }
}

/// Async work belongs to the application. A stale completion never closes a newer opening.
pub enum ModalAction {
    Close,
    KeepOpen,
    Pending(Task<Result<(), SharedString>>),
}
pub type ModalActionHandler = Rc<dyn Fn(&ModalHandle, &mut Window, &mut App) -> ModalAction>;
pub type ModalEventHandler = Rc<dyn Fn(&ModalEvent, &mut Window, &mut App)>;
pub type ModalSurfaceRenderer =
    Rc<dyn Fn(AnyElement, &ModalHandle, &mut Window, &mut App) -> AnyElement>;
pub type ModalStyleResolver = Rc<dyn Fn(&ModalStyleContext, &mut Window, &mut App) -> ModalStyles>;
pub type ModalFooterRenderer =
    Rc<dyn Fn(ModalFooterActions, &ModalHandle, &mut Window, &mut App) -> AnyElement>;

pub struct ModalFooterActions {
    pub ok: AnyElement,
    pub cancel: Option<AnyElement>,
}
#[derive(Clone, Default)]
pub enum ModalFooter {
    #[default]
    Default,
    None,
    Content(ModalContent),
    Render(ModalFooterRenderer),
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ModalButtonOptions {
    pub button_type: Option<ButtonType>,
    pub disabled: Option<bool>,
    pub loading: Option<bool>,
    pub danger: Option<bool>,
    pub style: StyleRefinement,
}
impl ModalButtonOptions {
    /// Omitted fields inherit from the scope; explicit `false` overrides `true`.
    pub fn resolve(&self, parent: &Self) -> Self {
        use gpui::Refineable;
        let mut style = parent.style.clone();
        style.refine(&self.style);
        Self {
            button_type: self.button_type.or(parent.button_type),
            disabled: self.disabled.or(parent.disabled),
            loading: self.loading.or(parent.loading),
            danger: self.danger.or(parent.danger),
            style,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ModalStyles {
    pub root: StyleRefinement,
    pub wrapper: StyleRefinement,
    pub mask: StyleRefinement,
    pub container: StyleRefinement,
    pub header: StyleRefinement,
    pub title: StyleRefinement,
    pub body: StyleRefinement,
    pub footer: StyleRefinement,
    pub close: StyleRefinement,
}
impl ModalStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        use gpui::Refineable;
        self.root.refine(&other.root);
        self.wrapper.refine(&other.wrapper);
        self.mask.refine(&other.mask);
        self.container.refine(&other.container);
        self.header.refine(&other.header);
        self.title.refine(&other.title);
        self.body.refine(&other.body);
        self.footer.refine(&other.footer);
        self.close.refine(&other.close);
    }
}
#[derive(Clone, Copy, Debug)]
pub struct ModalStyleContext {
    pub kind: ModalKind,
    pub loading: bool,
    pub confirm_loading: bool,
    pub direction: crate::Direction,
}

/// Retain expensive content factories; clones share callback and view-factory identity.
#[derive(Clone)]
pub struct ModalOptions {
    pub kind: ModalKind,
    pub title: Option<ModalContent>,
    pub content: Option<ModalContent>,
    pub footer: ModalFooter,
    pub width: Option<ModalWidth>,
    pub top: f32,
    pub centered: Option<bool>,
    pub mask: Option<bool>,
    pub mask_closable: Option<bool>,
    pub keyboard: bool,
    pub scroll_lock: bool,
    pub focus_trap: Option<bool>,
    pub focus_return: Option<bool>,
    pub auto_focus: ModalAutoFocus,
    pub initial_focus: Option<FocusHandle>,
    pub closable: Option<bool>,
    pub close_disabled: bool,
    pub close_icon: IconRef,
    pub icon: Option<IconRef>,
    pub show_icon: bool,
    pub show_cancel: Option<bool>,
    pub ok_text: Option<SharedString>,
    pub cancel_text: Option<SharedString>,
    pub ok_button: ModalButtonOptions,
    pub cancel_button: ModalButtonOptions,
    pub loading: bool,
    pub destroy_on_hidden: bool,
    pub force_render: bool,
    pub z_index: usize,
    pub styles: ModalStyles,
    pub styles_fn: Option<ModalStyleResolver>,
    pub surface_render: Option<ModalSurfaceRenderer>,
    pub accessibility_label: Option<SharedString>,
    pub accessibility_description: Option<SharedString>,
    pub automation_id: Option<SharedString>,
    pub on_ok: Option<ModalActionHandler>,
    pub on_cancel: Option<ModalActionHandler>,
    pub on_event: Option<ModalEventHandler>,
}
impl Default for ModalOptions {
    fn default() -> Self {
        Self {
            kind: ModalKind::Standard,
            title: None,
            content: None,
            footer: ModalFooter::Default,
            width: None,
            top: 100.,
            centered: None,
            mask: None,
            mask_closable: None,
            keyboard: true,
            scroll_lock: true,
            focus_trap: None,
            focus_return: None,
            auto_focus: ModalAutoFocus::Dialog,
            initial_focus: None,
            closable: None,
            close_disabled: false,
            close_icon: IconName::CloseOutlined.into(),
            icon: None,
            show_icon: true,
            show_cancel: None,
            ok_text: None,
            cancel_text: None,
            ok_button: ModalButtonOptions::default(),
            cancel_button: ModalButtonOptions::default(),
            loading: false,
            destroy_on_hidden: false,
            force_render: false,
            z_index: 1000,
            styles: ModalStyles::default(),
            styles_fn: None,
            surface_render: None,
            accessibility_label: None,
            accessibility_description: None,
            automation_id: None,
            on_ok: None,
            on_cancel: None,
            on_event: None,
        }
    }
}

#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod tests;
