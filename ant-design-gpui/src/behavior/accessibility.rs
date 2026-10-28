// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Role, SharedString, Window, accesskit,
};

/// Accessibility metadata applied to the same element that owns focus and activation.
///
/// Labels and descriptions should be resolved through the caller's localization catalog.
/// Stable `automation_id` values are identifiers and must never be translated.
#[derive(Clone, Debug, Default)]
pub struct Accessibility {
    pub role: Option<Role>,
    pub label: Option<SharedString>,
    pub description: Option<SharedString>,
    pub automation_id: Option<SharedString>,
    pub disabled: bool,
    pub busy: bool,
    pub invalid: bool,
    pub hidden: bool,
    pub read_only: bool,
    /// Identifies the current destination without implying a selectable widget.
    pub current: Option<accesskit::AriaCurrent>,
}

impl Accessibility {
    pub fn new(role: Role) -> Self {
        Self {
            role: Some(role),
            ..Self::default()
        }
    }

    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = Some(label.into());
        self
    }

    pub fn description(mut self, description: impl Into<SharedString>) -> Self {
        self.description = Some(description.into());
        self
    }

    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }

    pub fn apply<E: IntoElement>(self, element: E) -> SemanticElement<E::Element> {
        SemanticElement {
            element: element.into_element(),
            accessibility: self,
        }
    }

    fn write(&self, node: &mut accesskit::Node) {
        if let Some(label) = &self.label {
            node.set_label(label.to_string());
        }
        if let Some(description) = &self.description {
            node.set_description(description.to_string());
        }
        if let Some(id) = &self.automation_id {
            node.set_author_id(id.to_string());
        }
        if self.disabled {
            node.set_disabled();
            node.remove_action(accesskit::Action::Focus);
        }
        if self.busy {
            node.set_busy();
        }
        if self.invalid {
            node.set_invalid(accesskit::Invalid::True);
        }
        if self.hidden {
            node.set_hidden();
        }
        if self.read_only {
            node.set_read_only();
        }
        if let Some(current) = self.current {
            node.set_aria_current(current);
        }
        if self.disabled || self.busy || self.hidden || self.read_only {
            node.remove_action(accesskit::Action::Click);
        }
    }
}

/// A transparent semantic adapter: one layout node, hitbox, focus owner and accessible ID.
///
/// Direct delegation is deliberate. Drawing the wrapped element through `AnyElement`
/// would create a second identity and could detach its focus from the semantic node.
pub struct SemanticElement<E> {
    element: E,
    accessibility: Accessibility,
}

impl<E: Element> IntoElement for SemanticElement<E> {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for SemanticElement<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;

    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }

    fn a11y_role(&self) -> Option<Role> {
        self.accessibility
            .role
            .or_else(|| self.element.a11y_role())
            .filter(|role| *role != Role::GenericContainer)
    }

    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
        self.accessibility.write(node);
    }

    fn a11y_synthetic_children(
        &mut self,
        prepaint: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(prepaint, builder);
    }

    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector_id, window, cx)
    }

    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.element
            .prepaint(id, inspector_id, bounds, state, window, cx)
    }

    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector_id, bounds, state, prepaint, window, cx);
    }
}
