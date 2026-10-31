// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design alerts. Build with stable IDs; closing is retained until remount.
//!
//! Titles, descriptions and actions belong to the application. The close label uses
//! the configured locale. [`Alert::on_close`] reports dismissal immediately, while
//! [`Alert::after_close`] runs only after the measured leave transition completes.

mod config;
mod element;
mod state;
mod style;
mod view;

pub use config::{AlertConfig, AlertConfigPatch, AlertIcon, AlertStyleResolver};
pub use style::{AlertPadding, AlertStyle, AlertTheme, AlertTokenPatch, AlertTokens};

use std::rc::Rc;

use gpui::{
    AnyElement, App, ElementId, FocusHandle, IntoElement, Refineable, SharedString,
    StyleRefinement, Styled, Window, accesskit,
};

use crate::{ConfigScope, Direction, behavior::Activation, icons::IconRef};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AlertType {
    Success,
    #[default]
    Info,
    Warning,
    Error,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AlertVariant {
    #[default]
    Outlined,
    Filled,
}

/// Native semantic refinements. Absent properties inherit the preceding layer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AlertStyles {
    pub root: StyleRefinement,
    pub icon: StyleRefinement,
    pub section: StyleRefinement,
    pub title: StyleRefinement,
    pub description: StyleRefinement,
    pub actions: StyleRefinement,
    pub close: StyleRefinement,
}

impl AlertStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.icon.refine(&other.icon);
        self.section.refine(&other.section);
        self.title.refine(&other.title);
        self.description.refine(&other.description);
        self.actions.refine(&other.actions);
        self.close.refine(&other.close);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AlertStyleContext {
    pub kind: AlertType,
    pub variant: AlertVariant,
    pub banner: bool,
    pub show_icon: bool,
    pub closable: bool,
    pub has_description: bool,
    pub direction: Direction,
}

enum Content {
    Text(SharedString),
    Element(AnyElement),
}

impl Content {
    fn text(text: impl Into<SharedString>) -> Option<Self> {
        let text = text.into();
        (!text.is_empty()).then_some(Self::Text(text))
    }
    fn into_element(self, direction: Direction) -> AnyElement {
        match self {
            Self::Text(text) => crate::behavior::paragraph_text("alert-text", text, direction),
            Self::Element(element) => element,
        }
    }
}

type CloseHandler = Rc<dyn Fn(&Activation, &mut Window, &mut App)>;
type AfterCloseHandler = Rc<dyn Fn(&mut Window, &mut App)>;
type ClickHandler = Box<dyn Fn(&gpui::ClickEvent, &mut Window, &mut App)>;
type HoverHandler = Box<dyn Fn(bool, &mut Window, &mut App)>;
type StylesHandler = Box<dyn Fn(&AlertStyleContext, &mut Window, &mut App) -> AlertStyles>;

#[derive(IntoElement)]
pub struct Alert {
    id: ElementId,
    scope: Option<ConfigScope>,
    kind: Option<AlertType>,
    variant: Option<AlertVariant>,
    banner: bool,
    show_icon: Option<bool>,
    title: Option<Content>,
    description: Option<Content>,
    icon: Option<IconRef>,
    icon_content: Option<AnyElement>,
    action: Option<AnyElement>,
    closable: Option<bool>,
    close_disabled: bool,
    close_icon: Option<IconRef>,
    close_content: Option<AnyElement>,
    close_focus: Option<FocusHandle>,
    accessibility: crate::behavior::Accessibility,
    live: Option<accesskit::Live>,
    close_label: Option<SharedString>,
    close_automation_id: Option<SharedString>,
    on_close: Option<CloseHandler>,
    after_close: Option<AfterCloseHandler>,
    on_click: Option<ClickHandler>,
    on_hover: Option<HoverHandler>,
    styles: AlertStyles,
    styles_fn: Option<StylesHandler>,
    style: StyleRefinement,
}

impl Alert {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            kind: None,
            variant: None,
            banner: false,
            show_icon: None,
            title: None,
            description: None,
            icon: None,
            icon_content: None,
            action: None,
            closable: None,
            close_disabled: false,
            close_icon: None,
            close_content: None,
            close_focus: None,
            accessibility: crate::behavior::Accessibility::new(gpui::Role::Alert),
            live: None,
            close_label: None,
            close_automation_id: None,
            on_close: None,
            after_close: None,
            on_click: None,
            on_hover: None,
            styles: AlertStyles::default(),
            styles_fn: None,
            style: StyleRefinement::default(),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn alert_type(mut self, kind: AlertType) -> Self {
        self.kind = Some(kind);
        self
    }
    pub fn variant(mut self, variant: AlertVariant) -> Self {
        self.variant = Some(variant);
        self
    }
    pub fn banner(mut self, banner: bool) -> Self {
        self.banner = banner;
        self
    }
    pub fn show_icon(mut self, show: bool) -> Self {
        self.show_icon = Some(show);
        self
    }
    pub fn title(mut self, text: impl Into<SharedString>) -> Self {
        self.title = Content::text(text);
        self
    }
    pub fn title_content(mut self, content: impl IntoElement) -> Self {
        self.title = Some(Content::Element(content.into_any_element()));
        self
    }
    pub fn description(mut self, text: impl Into<SharedString>) -> Self {
        self.description = Content::text(text);
        self
    }
    pub fn description_content(mut self, content: impl IntoElement) -> Self {
        self.description = Some(Content::Element(content.into_any_element()));
        self
    }
    /// A custom status icon is displayed only when `show_icon` is enabled.
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self.icon_content = None;
        self
    }
    pub fn icon_content(mut self, content: impl IntoElement) -> Self {
        self.icon_content = Some(content.into_any_element());
        self.icon = None;
        self
    }
    pub fn action(mut self, action: impl IntoElement) -> Self {
        self.action = Some(action.into_any_element());
        self
    }
    /// Explicit `false` overrides custom close content and scoped closability.
    pub fn closable(mut self, closable: bool) -> Self {
        self.closable = Some(closable);
        self
    }
    pub fn close_disabled(mut self, disabled: bool) -> Self {
        self.close_disabled = disabled;
        self
    }
    /// Custom close content enables closing unless explicitly disabled by `closable(false)`.
    pub fn close_icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.close_icon = Some(icon.into());
        self.close_content = None;
        self
    }
    pub fn close_content(mut self, content: impl IntoElement) -> Self {
        self.close_content = Some(content.into_any_element());
        self.close_icon = None;
        self
    }
    pub fn close_focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.close_focus = Some(focus.clone());
        self
    }
    pub fn accessibility_role(mut self, role: gpui::Role) -> Self {
        self.accessibility.role = Some(role);
        self
    }
    /// Override the default assertive Alert / polite Status announcement policy.
    pub fn accessibility_live(mut self, live: accesskit::Live) -> Self {
        self.live = Some(live);
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
    pub fn close_accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.close_label = Some(label.into());
        self
    }
    pub fn close_automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.close_automation_id = Some(id.into());
        self
    }
    pub fn on_close(
        mut self,
        callback: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_close = Some(Rc::new(callback));
        self
    }
    pub fn after_close(mut self, callback: impl Fn(&mut Window, &mut App) + 'static) -> Self {
        self.after_close = Some(Rc::new(callback));
        self
    }
    /// Native click observation does not turn the static Alert into a keyboard button.
    pub fn on_click(
        mut self,
        callback: impl Fn(&gpui::ClickEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Box::new(callback));
        self
    }
    pub fn on_hover(mut self, callback: impl Fn(bool, &mut Window, &mut App) + 'static) -> Self {
        self.on_hover = Some(Box::new(callback));
        self
    }
    pub fn styles(mut self, styles: AlertStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&AlertStyleContext, &mut Window, &mut App) -> AlertStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Box::new(resolver));
        self
    }

    fn effective_live(&self, closing: bool) -> accesskit::Live {
        if closing {
            accesskit::Live::Off
        } else {
            self.live.unwrap_or(match self.accessibility.role {
                Some(gpui::Role::Alert) => accesskit::Live::Assertive,
                Some(gpui::Role::Status) => accesskit::Live::Polite,
                _ => accesskit::Live::Off,
            })
        }
    }
}

impl Styled for Alert {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod tests;
