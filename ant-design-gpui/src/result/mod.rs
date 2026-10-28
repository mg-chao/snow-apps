// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native result pages with application-owned text, actions and body content.
//!
//! Build with a stable ID. Results observe their configuration scope, retain no
//! interaction state and schedule no frames. Actions remain ordinary GPUI children.

mod config;
mod style;
mod view;

pub use config::{ResultConfig, ResultConfigPatch, ResultStyleResolver};
pub use style::{ResultMargin, ResultStyle, ResultTheme, ResultTokenPatch, ResultTokens};

use std::sync::OnceLock;

use gpui::{
    AnyElement, App, ElementId, IntoElement, ParentElement, Refineable, SharedString,
    StyleRefinement, Styled, Window,
};

use crate::{
    ConfigScope, Direction,
    behavior::Accessibility,
    icons::{IconColorModel, IconDescriptor, IconRef},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ResultStatus {
    Success,
    Error,
    #[default]
    Info,
    Warning,
    Forbidden,
    NotFound,
    InternalServerError,
}

impl ResultStatus {
    /// The original full-color exception illustration, also usable independently.
    /// Artwork keeps its source colors and is never mirrored in RTL layouts.
    pub fn illustration(self) -> Option<IconRef> {
        static FORBIDDEN: IconDescriptor = IconDescriptor {
            key: "adgpui/result/403",
            color_model: IconColorModel::FullColor,
            svg: include_bytes!("resources/403.svg"),
        };
        static NOT_FOUND: IconDescriptor = IconDescriptor {
            key: "adgpui/result/404",
            color_model: IconColorModel::FullColor,
            svg: include_bytes!("resources/404.svg"),
        };
        static SERVER_ERROR: IconDescriptor = IconDescriptor {
            key: "adgpui/result/500",
            color_model: IconColorModel::FullColor,
            svg: include_bytes!("resources/500.svg"),
        };
        static REFERENCES: OnceLock<[IconRef; 3]> = OnceLock::new();
        let index = match self {
            Self::Forbidden => 0,
            Self::NotFound => 1,
            Self::InternalServerError => 2,
            _ => return None,
        };
        Some(
            REFERENCES.get_or_init(|| {
                [&FORBIDDEN, &NOT_FOUND, &SERVER_ERROR]
                    .map(|descriptor| IconRef::custom(descriptor).expect("valid Result artwork"))
            })[index]
                .clone(),
        )
    }

    pub(crate) fn illustration_width(self) -> Option<f32> {
        match self {
            Self::Forbidden => Some(251.),
            Self::NotFound => Some(252.),
            Self::InternalServerError => Some(254.),
            _ => None,
        }
    }
}

/// Native semantic refinements; absent properties inherit the previous layer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ResultStyles {
    pub root: StyleRefinement,
    pub icon: StyleRefinement,
    pub title: StyleRefinement,
    pub sub_title: StyleRefinement,
    pub extra: StyleRefinement,
    pub body: StyleRefinement,
}

impl ResultStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.icon.refine(&other.icon);
        self.title.refine(&other.title);
        self.sub_title.refine(&other.sub_title);
        self.extra.refine(&other.extra);
        self.body.refine(&other.body);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResultStyleContext {
    pub status: ResultStatus,
    pub direction: Direction,
    pub has_title: bool,
    pub has_sub_title: bool,
    pub has_extra: bool,
    pub has_body: bool,
    pub show_icon: bool,
}

enum Content {
    Text(SharedString),
    Element(AnyElement),
}

impl Content {
    fn from_element(content: impl IntoElement) -> Option<Self> {
        let mut element = content.into_any_element();
        let text = element.downcast_mut::<SharedString>().cloned().or_else(|| {
            element
                .downcast_mut::<&'static str>()
                .map(|text| SharedString::new_static(text))
        });
        match text {
            Some(text) => (!text.is_empty()).then_some(Self::Text(text)),
            None => Some(Self::Element(element)),
        }
    }

    fn text(text: impl Into<SharedString>) -> Option<Self> {
        let text = text.into();
        (!text.is_empty()).then_some(Self::Text(text))
    }

    fn into_element(self, direction: Direction) -> AnyElement {
        match self {
            Self::Text(text) => crate::behavior::text::normal_paragraph_text(text, direction),
            Self::Element(element) => element,
        }
    }
}

type StylesFn = Box<dyn Fn(&ResultStyleContext, &mut Window, &mut App) -> ResultStyles>;

#[derive(IntoElement)]
pub struct Result {
    id: ElementId,
    scope: Option<ConfigScope>,
    status: ResultStatus,
    title: Option<Content>,
    sub_title: Option<Content>,
    icon: Option<IconRef>,
    icon_content: Option<AnyElement>,
    show_icon: bool,
    extra: Vec<Content>,
    children: Vec<Content>,
    accessibility: Accessibility,
    styles: ResultStyles,
    styles_fn: Option<StylesFn>,
    style: StyleRefinement,
}

impl Result {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            status: ResultStatus::Info,
            title: None,
            sub_title: None,
            icon: None,
            icon_content: None,
            show_icon: true,
            extra: Vec::new(),
            children: Vec::new(),
            accessibility: Accessibility::new(gpui::Role::Group),
            styles: ResultStyles::default(),
            styles_fn: None,
            style: StyleRefinement::default(),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn status(mut self, status: ResultStatus) -> Self {
        self.status = status;
        self
    }

    /// Empty text removes the title, including its margins.
    pub fn title(mut self, text: impl Into<SharedString>) -> Self {
        self.title = Content::text(text);
        self
    }

    pub fn title_content(mut self, content: impl IntoElement) -> Self {
        self.title = Some(Content::Element(content.into_any_element()));
        self
    }

    pub fn sub_title(mut self, text: impl Into<SharedString>) -> Self {
        self.sub_title = Content::text(text);
        self
    }

    pub fn sub_title_content(mut self, content: impl IntoElement) -> Self {
        self.sub_title = Some(Content::Element(content.into_any_element()));
        self
    }

    /// Customize a semantic status icon. Exception statuses always use their illustration.
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

    /// Hide semantic status icons. Exception illustrations remain visible, as upstream.
    pub fn show_icon(mut self, show: bool) -> Self {
        self.show_icon = show;
        self
    }

    /// Append an action. Native action children retain their own focus and handlers.
    pub fn extra(mut self, action: impl IntoElement) -> Self {
        self.extra.extend(Content::from_element(action));
        self
    }

    pub fn accessibility_role(mut self, role: gpui::Role) -> Self {
        self.accessibility.role = Some(role);
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

    pub fn styles(mut self, styles: ResultStyles) -> Self {
        self.styles = styles;
        self
    }

    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&ResultStyleContext, &mut Window, &mut App) -> ResultStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Box::new(resolver));
        self
    }
}

impl ParentElement for Result {
    fn extend(&mut self, elements: impl IntoIterator<Item = AnyElement>) {
        self.children
            .extend(elements.into_iter().filter_map(Content::from_element));
    }
}

impl Styled for Result {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod precedence_tests;
