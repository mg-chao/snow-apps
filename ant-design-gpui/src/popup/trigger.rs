// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Attach popup semantics to a trigger's existing native control identity.

use std::{cell::RefCell, rc::Rc};

use gpui::{
    A11ySubtreeBuilder, AccessibleAction, App, Bounds, Element, ElementId, FocusHandle, Global,
    GlobalElementId, InspectorElementId, IntoElement, LayoutId, Pixels, Role, SharedString,
    StatefulInteractiveElement, WeakEntity, Window, accesskit,
};

use super::{
    PopupOpenReason, PopupTriggers,
    runtime::{PopupState, update_interest},
};
use crate::behavior::{ActivationGuard, WeakActivationGuard};

#[derive(Default)]
struct TriggerContext(Rc<RefCell<Vec<(Binding, bool)>>>);
impl Global for TriggerContext {}

#[derive(Clone)]
struct Binding {
    state: WeakEntity<PopupState>,
    metadata: Rc<RefCell<Metadata>>,
}

#[derive(Default)]
pub(super) struct Metadata {
    pub tooltip: bool,
    pub menu: bool,
    pub enabled: bool,
    pub expanded: bool,
    pub clickable: bool,
    pub description: Option<SharedString>,
    pub guard: Option<WeakActivationGuard>,
    pub focus: Option<FocusHandle>,
}

impl Metadata {
    pub fn can_activate(&self) -> bool {
        self.guard
            .as_ref()
            .and_then(WeakActivationGuard::availability)
            .is_none_or(|availability| availability.can_activate())
    }
}

pub(super) fn with_trigger<R>(
    state: &WeakEntity<PopupState>,
    cx: &mut App,
    f: impl FnOnce(&mut App) -> R,
) -> R {
    if !cx.has_global::<TriggerContext>() {
        cx.set_global(TriggerContext::default());
    }
    let stack = cx.global::<TriggerContext>().0.clone();
    let metadata = state
        .upgrade()
        .expect("mounted trigger")
        .read(cx)
        .trigger_metadata
        .clone();
    stack.borrow_mut().push((
        Binding {
            state: state.clone(),
            metadata,
        },
        false,
    ));
    struct Restore(Rc<RefCell<Vec<(Binding, bool)>>>);
    impl Drop for Restore {
        fn drop(&mut self) {
            self.0.borrow_mut().pop();
        }
    }
    let _restore = Restore(stack);
    f(cx)
}

/// The popup binding for a custom native trigger control.
///
/// Standard library controls install this automatically. Custom controls call
/// `current` at the start of `RenderOnce::render`, `install` after their own
/// activation handler, and `apply` around their final semantic element. This
/// preserves its accessible identity, focus, native callback and availability.
/// One primary control owns each trigger; compound triggers can expose their
/// own semantic root. Bindings are weak and never retain an unmounted popup.
#[derive(Clone, Default)]
pub struct PopupTrigger(Vec<Binding>);

impl PopupTrigger {
    pub fn current(cx: &App) -> Self {
        let Some(context) = cx.try_global::<TriggerContext>() else {
            return Self::default();
        };
        let mut stack = context.0.borrow_mut();
        Self(
            stack
                .iter_mut()
                .filter_map(|(binding, claimed)| {
                    if std::mem::replace(claimed, true) {
                        None
                    } else {
                        Some(binding.clone())
                    }
                })
                .collect(),
        )
    }

    pub fn install<E: StatefulInteractiveElement>(
        &self,
        mut element: E,
        guard: &ActivationGuard,
        focus: &FocusHandle,
    ) -> E {
        for binding in &self.0 {
            binding.metadata.borrow_mut().guard = Some(guard.downgrade());
            binding.metadata.borrow_mut().focus = Some(focus.clone());
        }
        // GPUI runs all listeners for an action, in registration order. The
        // control's existing callback runs first; the popup then handles it.
        for (action, open) in [
            (AccessibleAction::Click, None),
            (AccessibleAction::Expand, Some(true)),
            (AccessibleAction::Collapse, Some(false)),
        ] {
            if self.0.is_empty() {
                break;
            }
            let trigger = self.clone();
            element = element.on_a11y_action(action, move |_, window, cx| {
                trigger.activate(open, window, cx)
            });
        }
        element
    }

    fn activate(&self, open: Option<bool>, window: &mut Window, cx: &mut App) {
        for binding in &self.0 {
            let enabled = {
                let metadata = binding.metadata.borrow();
                metadata.enabled
                    && metadata.can_activate()
                    && if open.is_some() {
                        !metadata.tooltip
                    } else {
                        metadata.clickable
                    }
            };
            if !enabled {
                continue;
            }
            update_interest(&binding.state, window, cx, |state| {
                if let Some(open) = open {
                    state
                        .lifecycle
                        .request(open, PopupOpenReason::Accessibility);
                } else if state.options.triggers.contains(PopupTriggers::CLICK) {
                    state
                        .lifecycle
                        .request(!state.lifecycle.open(), PopupOpenReason::Accessibility);
                }
            });
            let _ = binding.state.update(cx, |state, _| {
                if state.lifecycle.open() {
                    state.return_focus = binding.metadata.borrow().focus.clone();
                }
            });
        }
    }

    pub fn apply<E: IntoElement>(self, element: E) -> PopupTriggerElement<E::Element> {
        PopupTriggerElement {
            element: element.into_element(),
            trigger: self,
        }
    }

    fn write(&self, node: &mut accesskit::Node) {
        // install() adds all native handlers; advertise only actions supported
        // by this popup's current state, without affecting the control's Click.
        node.remove_action(AccessibleAction::Expand);
        node.remove_action(AccessibleAction::Collapse);
        for binding in &self.0 {
            let metadata = binding.metadata.borrow();
            if !metadata.enabled {
                continue;
            }
            if metadata.tooltip {
                if let Some(description) = &metadata.description {
                    let description = match node.description() {
                        Some(existing) if !existing.is_empty() => {
                            format!("{existing}\n{description}")
                        }
                        _ => description.to_string(),
                    };
                    node.set_description(description);
                }
            } else {
                node.set_has_popup(if metadata.menu {
                    accesskit::HasPopup::Menu
                } else {
                    accesskit::HasPopup::Dialog
                });
                node.set_expanded(metadata.expanded);
                if metadata.can_activate() {
                    node.add_action(if metadata.expanded {
                        AccessibleAction::Collapse
                    } else {
                        AccessibleAction::Expand
                    });
                }
            }
        }
    }
}

/// Transparent adapter returned by [`PopupTrigger::apply`].
pub struct PopupTriggerElement<E> {
    element: E,
    trigger: PopupTrigger,
}

impl<E: Element> IntoElement for PopupTriggerElement<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for PopupTriggerElement<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;
    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }
    fn a11y_role(&self) -> Option<Role> {
        self.element.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
        if !self.trigger.0.is_empty() {
            self.trigger.write(node);
        }
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(state, builder);
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
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.element
            .prepaint(id, inspector, bounds, state, window, cx)
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector, bounds, state, prepaint, window, cx);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::popup::tests::Fixture;
    use gpui::{AppContext, Context, Render, RenderOnce, TestAppContext, div, prelude::*, px};

    fn binding(f: &mut Fixture) -> PopupTrigger {
        let weak = f.handle().state().unwrap();
        let metadata =
            f.cx.update(|cx| weak.upgrade().unwrap().read(cx).trigger_metadata.clone());
        assert!(
            metadata.borrow().guard.is_some(),
            "native Button must bind automatically"
        );
        PopupTrigger(vec![Binding {
            state: weak,
            metadata,
        }])
    }

    fn activate(f: &mut Fixture, trigger: &PopupTrigger, open: Option<bool>) {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            trigger.activate(open, window, cx)
        })
        .unwrap();
        f.draw();
    }

    #[test]
    fn accessible_popup_actions_follow_authoritative_visibility_and_preserve_identity() {
        let mut f = Fixture::new(|_| {});
        let trigger = binding(&mut f);
        let element = trigger.clone().apply(
            crate::behavior::Accessibility::new(Role::Button)
                .label("Open")
                .apply(div().id("original")),
        );
        assert_eq!(element.id(), Some("original".into()));
        assert_eq!(element.a11y_role(), Some(Role::Button));
        let mut node = accesskit::Node::new(Role::Button);
        element.write_a11y_info(&mut node);
        assert_eq!(node.label(), Some("Open"));
        assert_eq!(node.has_popup(), Some(accesskit::HasPopup::Dialog));
        assert_eq!(node.is_expanded(), Some(false));
        assert!(node.supports_action(AccessibleAction::Expand));
        activate(&mut f, &trigger, Some(true));
        assert!(f.open());
        let mut node = accesskit::Node::new(Role::Button);
        element.write_a11y_info(&mut node);
        assert_eq!(
            node.is_expanded(),
            Some(true),
            "retained trigger reads current metadata"
        );
        assert!(node.supports_action(AccessibleAction::Collapse));
        assert!(!node.supports_action(AccessibleAction::Expand));
        activate(&mut f, &trigger, Some(false));
        assert!(!f.open());
        activate(&mut f, &trigger, None);
        assert!(f.open());
        assert!(
            f.changes()
                .iter()
                .all(|change| change.reason == PopupOpenReason::Accessibility)
        );
    }

    #[test]
    fn accessible_requests_can_repeat_after_controlled_owner_declines() {
        let mut f = Fixture::new(|state| state.controlled = Some(false));
        let trigger = binding(&mut f);
        for _ in 0..3 {
            activate(&mut f, &trigger, Some(true));
        }
        assert!(!f.open());
        assert_eq!(f.changes().len(), 3);
        assert!(f.changes().iter().all(|change| change.open));
    }

    #[test]
    fn tooltip_description_extends_control_help_without_dialog_actions() {
        let mut f = Fixture::new(|_| {});
        let trigger = binding(&mut f);
        {
            let mut metadata = trigger.0[0].metadata.borrow_mut();
            metadata.tooltip = true;
            metadata.clickable = false;
            metadata.description = Some("Helpful hint".into());
        }
        let element = trigger.clone().apply(
            crate::behavior::Accessibility::new(Role::Button)
                .label("Original")
                .description("Original help")
                .apply(div().id("original")),
        );
        let mut node = accesskit::Node::new(Role::Button);
        element.write_a11y_info(&mut node);
        assert_eq!(node.description(), Some("Original help\nHelpful hint"));
        assert_eq!(node.has_popup(), None);
        assert_eq!(node.is_expanded(), None);
        activate(&mut f, &trigger, Some(true));
        assert!(!f.open());
    }

    #[test]
    fn empty_binding_preserves_custom_control_expansion_semantics() {
        let cx = TestAppContext::single();
        let trigger = cx.update(|cx| PopupTrigger::current(cx));
        let element = trigger.apply(div().id("tree-node").aria_expanded(true));
        let mut node = accesskit::Node::new(Role::TreeItem);
        element.write_a11y_info(&mut node);
        assert_eq!(node.is_expanded(), Some(true));
    }

    #[derive(Clone, IntoElement)]
    struct CustomControl {
        focus: FocusHandle,
        guard: ActivationGuard,
        binding: Rc<RefCell<PopupTrigger>>,
    }
    impl RenderOnce for CustomControl {
        fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
            let binding = PopupTrigger::current(cx);
            assert_eq!(binding.0.len(), 1);
            assert!(
                PopupTrigger::current(cx).0.is_empty(),
                "descendants must not steal the semantic root's binding"
            );
            *self.binding.borrow_mut() = binding.clone();
            let element = self.guard.install(
                div()
                    .id("custom-root")
                    .track_focus(&self.focus)
                    .size(px(80.)),
                |_, _, _| {},
            );
            let element = binding.install(element, &self.guard, &self.focus);
            let element = binding.apply(
                crate::behavior::Accessibility::new(Role::Button)
                    .label("Compound control")
                    .description("Existing help")
                    .apply(
                        element
                            .child(div().child("Compound"))
                            .child(div().child("control")),
                    ),
            );
            let mut node = accesskit::Node::new(Role::Button);
            element.write_a11y_info(&mut node);
            assert_eq!(element.id(), Some("custom-root".into()));
            assert_eq!(node.label(), Some("Compound control"));
            assert_eq!(node.description(), Some("Existing help"));
            assert_eq!(node.has_popup(), Some(accesskit::HasPopup::Dialog));
            element
        }
    }
    struct CustomView {
        control: CustomControl,
        handle: super::super::PopupHandle,
    }
    impl Render for CustomView {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().size_full().p(px(100.)).child(
                super::super::Popup::new("custom-popup")
                    .handle(&self.handle)
                    .triggers(PopupTriggers::CLICK)
                    .child(self.control.clone())
                    .content(|_, _, _| div().size(px(80.)).child("Content")),
            )
        }
    }

    #[test]
    fn custom_semantic_root_claims_binding_before_descendants_and_keeps_live_availability() {
        let mut cx = TestAppContext::single();
        let focus = cx.update(crate::behavior::focus_handle);
        let guard = ActivationGuard::default();
        let binding = Rc::new(RefCell::new(PopupTrigger::default()));
        let handle = super::super::PopupHandle::new();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = crate::ConfigScope::new_window(cx);
            let view = cx.new(|_| CustomView {
                control: CustomControl {
                    focus: focus.clone(),
                    guard: guard.clone(),
                    binding: binding.clone(),
                },
                handle: handle.clone(),
            });
            crate::UiRoot::new(scope, view, window, cx)
        });
        let draw = |cx: &mut TestAppContext| {
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
        };
        draw(&mut cx);
        assert!(cx.update(|cx| PopupTrigger::current(cx).0.is_empty()));
        guard.set_availability(crate::behavior::Availability {
            busy: true,
            disabled: false,
        });
        let open = |cx: &mut TestAppContext| {
            cx.update_window(window.into(), |_, window, cx| {
                binding.borrow().activate(Some(true), window, cx)
            })
            .unwrap();
        };
        open(&mut cx);
        assert!(!cx.update(|cx| handle.is_open(cx)), "availability is live");
        guard.set_availability(Default::default());
        open(&mut cx);
        draw(&mut cx);
        assert!(cx.update(|cx| handle.is_open(cx)));
        cx.update(|cx| {
            let state = handle.state().unwrap().upgrade().unwrap();
            assert_eq!(state.read(cx).return_focus.as_ref(), Some(&focus));
        });
    }

    #[test]
    fn trigger_metadata_does_not_keep_a_removed_controls_availability_alive() {
        let guard = ActivationGuard::new(crate::behavior::Availability {
            disabled: true,
            busy: false,
        });
        let metadata = Metadata {
            guard: Some(guard.downgrade()),
            ..Default::default()
        };
        assert!(!metadata.can_activate());
        drop(guard);
        assert!(
            metadata.can_activate(),
            "replacing a disabled control must not disable a later plain trigger"
        );
    }
}
