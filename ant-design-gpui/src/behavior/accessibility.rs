// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Role, SharedString, Window, accesskit,
};

/// Passive inspection of the exact native node, without another semantic node.
#[cfg(any(test, feature = "test-support"))]
#[derive(Clone)]
pub struct AccessibilityInspection(std::rc::Rc<dyn Fn(&accesskit::Node)>);
#[cfg(any(test, feature = "test-support"))]
impl AccessibilityInspection {
    pub fn new(callback: impl Fn(&accesskit::Node) + 'static) -> Self {
        Self(std::rc::Rc::new(callback))
    }
}
#[cfg(any(test, feature = "test-support"))]
impl std::fmt::Debug for AccessibilityInspection {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("AccessibilityInspection")
    }
}

/// Accessibility metadata applied to the same element that owns focus and activation.
///
/// Labels and descriptions should be resolved through the caller's localization catalog.
/// Stable `automation_id` values are identifiers and must never be translated.
#[derive(Clone, Debug, Default)]
pub struct Accessibility {
    #[cfg(any(test, feature = "test-support"))]
    pub inspection: Option<AccessibilityInspection>,
    pub role: Option<Role>,
    pub label: Option<SharedString>,
    pub description: Option<SharedString>,
    pub automation_id: Option<SharedString>,
    pub disabled: bool,
    pub busy: bool,
    /// Background work reports busy without preventing the user from editing.
    /// Loading controls leave this false while their activation is unavailable.
    pub busy_allows_activation: bool,
    /// Marks a dialog that confines interaction to its active subtree.
    pub modal: bool,
    pub invalid: bool,
    /// The field requires a value independently of its current validity.
    pub required: bool,
    pub hidden: bool,
    pub read_only: bool,
    /// Identifies the current destination without implying a selectable widget.
    pub current: Option<accesskit::AriaCurrent>,
}

impl Accessibility {
    pub(crate) fn in_form(&mut self, context: &crate::form::FormItemContext) {
        self.label = self.label.clone().or_else(|| context.label.clone());
        self.description = self
            .description
            .clone()
            .or_else(|| context.description.clone());
        self.required |= context.required;
        self.invalid |= context.invalid;
        self.busy |= context.busy;
        self.busy_allows_activation |= context.busy;
    }

    /// Carry metadata onto an embedded editor, which owns the actual native
    /// focus node. Availability is resolved by that editor's control builder.
    pub(crate) fn inherit_control(&mut self, source: &Self) {
        self.label = self.label.clone().or_else(|| source.label.clone());
        self.description = self
            .description
            .clone()
            .or_else(|| source.description.clone());
        self.automation_id = self
            .automation_id
            .clone()
            .or_else(|| source.automation_id.clone());
        self.required |= source.required;
        self.invalid |= source.invalid;
        self.busy |= source.busy;
        self.busy_allows_activation |= source.busy_allows_activation;
        #[cfg(any(test, feature = "test-support"))]
        {
            self.inspection = self
                .inspection
                .clone()
                .or_else(|| source.inspection.clone());
        }
    }

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
            // AccessKit static text derives its platform name from value.
            // Control labels (buttons, dialogs, images) use the label property.
            if node.role() == Role::Label {
                node.set_value(label.to_string());
            } else {
                node.set_label(label.to_string());
            }
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
        if self.modal {
            node.set_modal();
        }
        if self.invalid {
            node.set_invalid(accesskit::Invalid::True);
        }
        if self.required {
            node.set_required();
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
        if self.disabled
            || (self.busy && !self.busy_allows_activation)
            || self.hidden
            || self.read_only
        {
            node.remove_action(accesskit::Action::Click);
        }
        #[cfg(any(test, feature = "test-support"))]
        if let Some(inspection) = &self.inspection {
            (inspection.0)(node);
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
