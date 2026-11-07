// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Responsive description lists using native GPUI elements and application-owned content.
//!
//! Rebuild with stable component and item IDs. Text belongs to the application's
//! localization catalog; custom content retains its own focus and interaction.
//!
//! ```
//! use ant_design_gpui::{DescriptionItem, Descriptions, DescriptionsSpan};
//!
//! let _details = Descriptions::new("account-details")
//!     .title("Account")
//!     .columns(3)
//!     .bordered(true)
//!     .item(DescriptionItem::new("name").label("Name").content("Lin"))
//!     .item(DescriptionItem::new("address")
//!         .label("Address")
//!         .content("Hangzhou, Zhejiang")
//!         .span(DescriptionsSpan::Filled));
//! ```

mod borders;
mod config;
mod element;
mod geometry;
mod model;
mod paint;
mod style;
mod view;

pub use config::{DescriptionsConfig, DescriptionsConfigPatch, DescriptionsStyleResolver};
pub use model::{
    DescriptionsBreakpoints, DescriptionsColumns, DescriptionsLayout, DescriptionsSpan,
};
pub use style::{DescriptionsStyle, DescriptionsTheme, DescriptionsTokenPatch, DescriptionsTokens};

use gpui::{
    AnyElement, App, ElementId, IntoElement, Refineable, Role, SharedString, StyleRefinement,
    Styled, Window,
};

use crate::{ConfigScope, Direction, Size, behavior::Accessibility};

/// Native refinements for the component's semantic slots.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DescriptionsStyles {
    pub root: StyleRefinement,
    pub header: StyleRefinement,
    pub title: StyleRefinement,
    pub extra: StyleRefinement,
    pub label: StyleRefinement,
    pub content: StyleRefinement,
}

impl DescriptionsStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.header.refine(&other.header);
        self.title.refine(&other.title);
        self.extra.refine(&other.extra);
        self.label.refine(&other.label);
        self.content.refine(&other.content);
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct DescriptionItemStyles {
    pub label: StyleRefinement,
    pub content: StyleRefinement,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DescriptionsStyleContext {
    pub layout: DescriptionsLayout,
    pub bordered: bool,
    pub colon: bool,
    pub columns: usize,
    pub size: Size,
    pub direction: Direction,
    pub item_count: usize,
    pub has_title: bool,
    pub has_extra: bool,
}

pub(crate) enum Content {
    Text(SharedString),
    Element(AnyElement),
}

impl Content {
    fn text(text: impl Into<SharedString>) -> Option<Self> {
        let text = text.into();
        (!text.is_empty()).then_some(Self::Text(text))
    }

    fn element(content: impl IntoElement) -> Option<Self> {
        let mut element = content.into_any_element();
        let text = element.downcast_mut::<SharedString>().cloned().or_else(|| {
            element
                .downcast_mut::<&'static str>()
                .map(|text| SharedString::new_static(text))
        });
        match text {
            Some(text) => Self::text(text),
            None => Some(Self::Element(element)),
        }
    }

    pub(crate) fn into_element(self, direction: Direction, break_word: bool) -> AnyElement {
        match self {
            Self::Text(text) if break_word => {
                crate::behavior::text::anywhere_paragraph_text(text, direction)
            }
            Self::Text(text) => crate::behavior::text::normal_paragraph_text(text, direction),
            Self::Element(element) => element,
        }
    }
}

/// A logical label/value pair. Its ID remains stable when responsive rows change.
/// Terms and definition groups share the label's accessible name, so their
/// association remains clear when vertical layout groups labels into one row.
/// Rich values retain their own accessible descendants.
pub struct DescriptionItem {
    pub(crate) id: ElementId,
    pub(crate) label: Option<Content>,
    pub(crate) content: Option<Content>,
    pub(crate) accessible_label: Option<SharedString>,
    pub(crate) span: DescriptionsSpan,
    pub(crate) styles: DescriptionItemStyles,
    pub(crate) style: StyleRefinement,
}

impl DescriptionItem {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            label: None,
            content: None,
            accessible_label: None,
            span: DescriptionsSpan::default(),
            styles: DescriptionItemStyles::default(),
            style: StyleRefinement::default(),
        }
    }

    /// Empty text removes the label and its colon, while retaining the item.
    pub fn label(mut self, text: impl Into<SharedString>) -> Self {
        self.label = Content::text(text);
        self
    }

    pub fn label_content(mut self, content: impl IntoElement) -> Self {
        self.label = Content::element(content);
        self
    }

    pub fn content(mut self, text: impl Into<SharedString>) -> Self {
        self.content = Content::text(text);
        self
    }

    pub fn content_element(mut self, content: impl IntoElement) -> Self {
        self.content = Content::element(content);
        self
    }

    /// Name the term and its definition group. Plain text labels supply this
    /// automatically; provide it for custom label content. Values are never
    /// flattened into this name and keep their own native semantics.
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessible_label = Some(label.into());
        self
    }

    pub(crate) fn resolved_accessibility_label(&self) -> Option<SharedString> {
        self.accessible_label.clone().or_else(|| match &self.label {
            Some(Content::Text(text)) => Some(text.clone()),
            _ => None,
        })
    }

    pub fn span(mut self, span: impl Into<DescriptionsSpan>) -> Self {
        self.span = span.into();
        self
    }

    pub fn styles(mut self, styles: DescriptionItemStyles) -> Self {
        self.styles = styles;
        self
    }
}

impl Styled for DescriptionItem {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

type StylesFn = Box<dyn Fn(&DescriptionsStyleContext, &mut Window, &mut App) -> DescriptionsStyles>;

#[derive(IntoElement)]
pub struct Descriptions {
    pub(crate) id: ElementId,
    pub(crate) scope: Option<ConfigScope>,
    pub(crate) items: Vec<DescriptionItem>,
    pub(crate) title: Option<Content>,
    pub(crate) extra: Option<Content>,
    pub(crate) layout: DescriptionsLayout,
    pub(crate) bordered: bool,
    pub(crate) colon: bool,
    pub(crate) columns: DescriptionsColumns,
    pub(crate) size: Option<Size>,
    pub(crate) styles: DescriptionsStyles,
    pub(crate) styles_fn: Option<StylesFn>,
    pub(crate) style: StyleRefinement,
    pub(crate) accessibility: Accessibility,
}

impl Descriptions {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            items: Vec::new(),
            title: None,
            extra: None,
            layout: DescriptionsLayout::Horizontal,
            bordered: false,
            colon: true,
            columns: DescriptionsColumns::default(),
            size: None,
            styles: DescriptionsStyles::default(),
            styles_fn: None,
            style: StyleRefinement::default(),
            accessibility: Accessibility::new(Role::DescriptionList),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn item(mut self, item: DescriptionItem) -> Self {
        self.items.push(item);
        self
    }

    pub fn items(mut self, items: impl IntoIterator<Item = DescriptionItem>) -> Self {
        self.items.extend(items);
        self
    }

    /// Empty text removes the title; whitespace remains meaningful content.
    pub fn title(mut self, text: impl Into<SharedString>) -> Self {
        self.title = Content::text(text);
        self
    }

    pub fn title_content(mut self, content: impl IntoElement) -> Self {
        self.title = Content::element(content);
        self
    }

    pub fn extra(mut self, text: impl Into<SharedString>) -> Self {
        self.extra = Content::text(text);
        self
    }

    pub fn extra_content(mut self, content: impl IntoElement) -> Self {
        self.extra = Content::element(content);
        self
    }

    pub fn layout(mut self, layout: DescriptionsLayout) -> Self {
        self.layout = layout;
        self
    }

    pub fn bordered(mut self, bordered: bool) -> Self {
        self.bordered = bordered;
        self
    }

    pub fn colon(mut self, colon: bool) -> Self {
        self.colon = colon;
        self
    }

    pub fn columns(mut self, columns: impl Into<DescriptionsColumns>) -> Self {
        self.columns = columns.into();
        self
    }

    /// Inherits an explicitly configured size; otherwise uses large spacing.
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }

    pub fn styles(mut self, styles: DescriptionsStyles) -> Self {
        self.styles = styles;
        self
    }

    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&DescriptionsStyleContext, &mut Window, &mut App) -> DescriptionsStyles
        + 'static,
    ) -> Self {
        self.styles_fn = Some(Box::new(resolver));
        self
    }

    pub fn accessibility_role(mut self, role: Role) -> Self {
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
}

impl Styled for Descriptions {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[cfg(test)]
mod api_tests;
#[cfg(test)]
mod inline_tests;
#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod tests;
