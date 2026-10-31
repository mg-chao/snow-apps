// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Role, SharedString, Window, accesskit,
};

/// A context region is a named group, independent of any ordinary activation its
/// child control supplies. Delegate directly to preserve the native focus ID.
pub(super) struct MenuTrigger<E> {
    element: E,
    label: SharedString,
    disabled: bool,
    expanded: bool,
}

pub(super) fn semantics<E: IntoElement>(
    element: E,
    label: SharedString,
    disabled: bool,
    expanded: bool,
) -> MenuTrigger<E::Element> {
    MenuTrigger {
        element: element.into_element(),
        label,
        disabled,
        expanded,
    }
}

impl<E: Element> IntoElement for MenuTrigger<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for MenuTrigger<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;
    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }
    fn a11y_role(&self) -> Option<Role> {
        Some(Role::Group)
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
        node.set_label(self.label.to_string());
        node.set_has_popup(accesskit::HasPopup::Menu);
        node.set_expanded(self.expanded);
        if self.disabled {
            node.set_disabled();
            node.remove_action(accesskit::Action::Focus);
            node.remove_action(accesskit::Action::ShowContextMenu);
        }
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
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector, window, cx)
    }
    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        layout: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.element
            .prepaint(id, inspector, bounds, layout, window, cx)
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        layout: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector, bounds, layout, prepaint, window, cx);
    }
}
