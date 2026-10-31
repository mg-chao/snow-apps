// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A metrics-free native action for calendar headers and popup footer controls.

use crate::{
    ConfigScope,
    behavior::{Accessibility, ActivationGuard, Availability, FocusRing, focus_handle},
};
use gpui::{
    AnyElement, App, Context, ElementId, FocusHandle, IntoElement, Refineable, RenderOnce, Role,
    SharedString, StyleRefinement, Styled, Subscription, Window, div, prelude::*, px,
};

type Callback = Box<dyn Fn(&mut Window, &mut App)>;

// The offscreen GPUI platform has no active accessibility adapter. This optional
// test probe reads the exact native metadata before the element is type-erased.
#[cfg(test)]
#[derive(Default)]
pub(super) struct PickerActionSemanticsProbe {
    pub nodes: Vec<gpui::accesskit::Node>,
}
#[cfg(test)]
impl gpui::Global for PickerActionSemanticsProbe {}

/// One identified root owns the caller's styles, native semantics and retained focus.
/// No button padding, typography, border or wrapper dimensions are introduced.
#[derive(IntoElement)]
pub(super) struct PickerAction {
    id: ElementId,
    label: SharedString,
    content: AnyElement,
    callback: Callback,
    scope: Option<ConfigScope>,
    disabled: Option<bool>,
    style: StyleRefinement,
    hover_style: Option<StyleRefinement>,
}

impl PickerAction {
    pub fn new(
        id: impl Into<ElementId>,
        label: impl Into<SharedString>,
        content: AnyElement,
        callback: impl Fn(&mut Window, &mut App) + 'static,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            content,
            callback: Box::new(callback),
            scope: None,
            disabled: None,
            style: StyleRefinement::default(),
            hover_style: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn hover_style(mut self, style: StyleRefinement) -> Self {
        self.hover_style = Some(style);
        self
    }
}

impl Styled for PickerAction {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct ActionState {
    scope: ConfigScope,
    disabled: Option<bool>,
    focus: FocusHandle,
    guard: ActivationGuard,
    _scope: Subscription,
    _focus: Vec<Subscription>,
}

impl ActionState {
    fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let focus = focus_handle(cx);
        let guard = ActivationGuard::default();
        let blur_guard = guard.clone();
        let blur = cx.on_blur(&focus, window, move |_, _, cx| {
            blur_guard.cancel();
            cx.notify();
        });
        let focused = cx.on_focus(&focus, window, |_, _, cx| cx.notify());
        let inactive_guard = guard.clone();
        let activation = cx.observe_window_activation(window, move |_, _, cx| {
            inactive_guard.cancel();
            cx.notify();
        });
        let hidden_guard = guard.clone();
        let visibility = cx.observe_window_visibility(window, move |_, _, _, cx| {
            hidden_guard.cancel();
            cx.notify();
        });
        Self {
            _scope: Self::observe_scope(&scope, window, cx),
            scope,
            disabled: None,
            focus,
            guard,
            _focus: vec![blur, focused, activation, visibility],
        }
    }

    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |state, window, cx| {
            state.sync(window, cx);
            cx.notify();
        })
    }

    fn sync(&self, window: &mut Window, cx: &mut App) {
        let availability = self.scope.snapshot(cx).availability(self.disabled, false);
        self.guard.update(availability, &self.focus, window, cx);
    }
}

impl Drop for ActionState {
    fn drop(&mut self) {
        // Old painted trees may still own a guard while their keyed state retires.
        // Retiring the native control invalidates pointer, keyboard and UIA actions.
        self.guard.set_availability(Availability {
            disabled: true,
            busy: false,
        });
    }
}

impl RenderOnce for PickerAction {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state((self.id.clone(), "picker-action"), cx, |window, cx| {
            ActionState::new(scope.clone(), window, cx)
        });
        let (focus, guard, availability) = state.update(cx, |state, cx| {
            if state.scope != scope {
                state.guard.cancel();
                state._scope = ActionState::observe_scope(&scope, window, cx);
                state.scope = scope.clone();
            }
            state.disabled = self.disabled;
            state.sync(window, cx);
            (
                state.focus.clone(),
                state.guard.clone(),
                state.guard.availability(),
            )
        });
        let config = scope.snapshot(cx);
        let g = &config.theme.tokens;
        let mut element = div()
            .id(self.id)
            .child(self.content)
            .when(availability.can_focus(), |element| {
                element.track_focus(&focus)
            })
            .when(availability.can_activate(), |element| {
                element.cursor_pointer()
            });
        element.style().refine(&self.style);
        let corners = gpui::Corners {
            top_left: self.style.corner_radii.top_left.unwrap_or_default(),
            top_right: self.style.corner_radii.top_right.unwrap_or_default(),
            bottom_left: self.style.corner_radii.bottom_left.unwrap_or_default(),
            bottom_right: self.style.corner_radii.bottom_right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        if let Some(hover) = self.hover_style.filter(|_| availability.can_activate()) {
            element = element.hover(move |mut style| {
                style.refine(&hover);
                style
            });
        }
        let weak = state.downgrade();
        let callback = self.callback;
        let element = guard.install(element, move |_, window, cx| {
            // Leave the retained-state borrow before invoking consumer code.
            let enabled = weak
                .update(cx, |state, cx| {
                    state.sync(window, cx);
                    state.guard.availability().can_activate()
                })
                .unwrap_or(false);
            if enabled {
                callback(window, cx);
            }
            cx.stop_propagation();
        });
        let semantics = Accessibility {
            label: Some(self.label),
            disabled: availability.disabled,
            ..Accessibility::new(Role::Button)
        };
        let element = FocusRing::new(g.color_primary_border, px(g.line_width_focus as f32))
            .apply(semantics.apply(element), &focus)
            .corners(corners);
        #[cfg(test)]
        if cx.has_global::<PickerActionSemanticsProbe>() {
            let mut node = gpui::accesskit::Node::new(gpui::Element::a11y_role(&element).unwrap());
            gpui::Element::write_a11y_info(&element, &mut node);
            cx.global_mut::<PickerActionSemanticsProbe>()
                .nodes
                .push(node);
        }
        element
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{
        Element, Entity, KeyDownEvent, KeyUpEvent, Keystroke, Modifiers, MouseButton,
        MouseDownEvent, MouseUpEvent, PlatformInput, Render, TestAppContext, VisualTestContext,
        WindowHandle, accesskit, point, size,
    };

    struct Consumer {
        scope: ConfigScope,
        root_focus: FocusHandle,
        disabled: Option<bool>,
        visible: bool,
        count: usize,
        semantics: accesskit::Node,
    }

    impl Consumer {
        fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
            let root_focus = cx.focus_handle().tab_stop(false);
            window.focus(&root_focus, cx);
            Self {
                scope: ConfigScope::new_window(cx),
                root_focus,
                disabled: None,
                visible: true,
                count: 0,
                semantics: accesskit::Node::new(Role::Button),
            }
        }
    }

    impl Render for Consumer {
        fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
            let first = self.visible.then(|| {
                let target = cx.entity();
                let mut first = PickerAction::new(
                    "first",
                    "Previous month",
                    div()
                        .size_full()
                        .debug_selector(|| "picker-action.first".into())
                        .into_any_element(),
                    move |_, cx| {
                        target.update(cx, |state, cx| {
                            state.count += 1;
                            cx.notify();
                        })
                    },
                )
                .scope(self.scope.clone())
                .w(px(22.))
                .h(px(40.))
                .hover_style(StyleRefinement::default().text_color(gpui::rgb(0x1677ff)));
                if let Some(disabled) = self.disabled {
                    first = first.disabled(disabled);
                }
                let first = first.render(window, cx).into_element();
                assert_eq!(first.id(), Some("first".into()));
                let mut node = accesskit::Node::new(first.a11y_role().unwrap());
                first.write_a11y_info(&mut node);
                self.semantics = node;
                first.into_any_element()
            });
            let target = cx.entity();
            crate::behavior::traversal(
                div()
                    .id("actions")
                    .track_focus(&self.root_focus)
                    .flex()
                    .size_full(),
            )
            .when_some(first, |row, first| row.child(first))
            .child(
                PickerAction::new(
                    "second",
                    "Next month",
                    div()
                        .size_full()
                        .debug_selector(|| "picker-action.second".into())
                        .into_any_element(),
                    move |_, cx| {
                        target.update(cx, |state, cx| {
                            state.count += 10;
                            cx.notify();
                        })
                    },
                )
                .scope(self.scope.clone())
                .w(px(22.))
                .h(px(40.)),
            )
        }
    }

    struct Fixture {
        cx: VisualTestContext,
        consumer: Entity<Consumer>,
        _window: WindowHandle<Consumer>,
    }

    impl Fixture {
        fn new() -> Self {
            let mut cx = TestAppContext::single();
            let window = cx.add_window(Consumer::new);
            let consumer = window.root(&mut cx).unwrap();
            let mut fixture = Self {
                cx: VisualTestContext::from_window(window.into(), &cx),
                consumer,
                _window: window,
            };
            fixture.cx.simulate_resize(size(px(320.), px(180.)));
            fixture.draw();
            fixture
        }

        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx.update(|window, cx| window.draw(cx).clear(cx));
            self.cx.run_until_parked();
        }

        fn key(&mut self, stroke: &'static str, down: bool, held: bool) {
            let keystroke = Keystroke::parse(stroke).unwrap();
            self.cx.update(|window, cx| {
                window.dispatch_event(
                    if down {
                        PlatformInput::KeyDown(KeyDownEvent {
                            keystroke,
                            is_held: held,
                            prefer_character_input: false,
                        })
                    } else {
                        PlatformInput::KeyUp(KeyUpEvent { keystroke })
                    },
                    cx,
                )
            });
            self.draw();
        }

        fn focus_first(&mut self) -> FocusHandle {
            let root = self
                .consumer
                .read_with(&self.cx, |state, _| state.root_focus.clone());
            self.cx.update(|window, cx| window.focus(&root, cx));
            self.key("tab", true, false);
            self.cx
                .update(|window, cx| window.focused(cx).expect("first action focused"))
        }

        fn count(&self) -> usize {
            self.consumer.read_with(&self.cx, |state, _| state.count)
        }
    }

    #[test]
    fn picker_action_has_native_activation_unique_focus_and_exact_caller_metrics() {
        let mut fixture = Fixture::new();
        let first = fixture.cx.debug_bounds("picker-action.first").unwrap();
        let second = fixture.cx.debug_bounds("picker-action.second").unwrap();
        assert_eq!(first.size, size(px(22.), px(40.)));
        assert_eq!(second.left(), first.right());
        fixture.consumer.read_with(&fixture.cx, |state, _| {
            assert_eq!(state.semantics.role(), Role::Button);
            assert_eq!(state.semantics.label(), Some("Previous month"));
            assert!(state.semantics.supports_action(accesskit::Action::Click));
            assert!(state.semantics.supports_action(accesskit::Action::Focus));
        });
        let first_focus = fixture.focus_first();
        fixture
            .cx
            .update(|window, _| assert!(!window.rendered_scene().quads.is_empty()));
        fixture.key("enter", true, false);
        fixture.key("enter", false, false);
        assert_eq!(fixture.count(), 1);
        fixture.key("space", true, false);
        fixture.key("space", true, true);
        fixture.key("space", false, false);
        assert_eq!(fixture.count(), 2);
        fixture
            .cx
            .update(|window, cx| assert_eq!(window.focused(cx), Some(first_focus.clone())));
        fixture.key("tab", true, false);
        let second_focus = fixture.cx.update(|window, cx| window.focused(cx).unwrap());
        assert_ne!(first_focus, second_focus);
        fixture.key("shift-tab", true, false);
        fixture
            .cx
            .update(|window, cx| assert_eq!(window.focused(cx), Some(first_focus)));
        fixture.key("space", true, false);
        fixture.key("tab", true, false);
        fixture.key("shift-tab", true, false);
        fixture.key("space", false, false);
        assert_eq!(fixture.count(), 2, "a blurred press is cancelled");
        assert_eq!(
            fixture.cx.debug_bounds("picker-action.first").unwrap(),
            first
        );
    }

    #[test]
    fn picker_action_disabled_and_unmounted_presses_never_activate_after_reenable() {
        let mut fixture = Fixture::new();
        fixture.focus_first();
        fixture.key("space", true, false);
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.disabled = Some(true);
            cx.notify();
        });
        fixture.draw();
        fixture.consumer.read_with(&fixture.cx, |state, _| {
            assert!(state.semantics.is_disabled());
            assert!(!state.semantics.supports_action(accesskit::Action::Click));
            assert!(!state.semantics.supports_action(accesskit::Action::Focus));
        });
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.disabled = Some(false);
            cx.notify();
        });
        fixture.draw();
        fixture.focus_first();
        fixture.key("space", false, false);
        assert_eq!(fixture.count(), 0);
        let retired_focus = fixture.focus_first();
        fixture.key("space", true, false);
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.visible = false;
            cx.notify();
        });
        fixture.draw();
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.visible = true;
            cx.notify();
        });
        fixture.draw();
        let replacement_focus = fixture.focus_first();
        assert_ne!(retired_focus, replacement_focus);
        fixture.key("space", false, false);
        assert_eq!(fixture.count(), 0);
        fixture.key("enter", true, false);
        fixture.key("enter", false, false);
        assert_eq!(fixture.count(), 1);
    }

    #[test]
    fn picker_action_scope_availability_and_pointer_release_share_the_guard() {
        let mut fixture = Fixture::new();
        let inside = fixture
            .cx
            .debug_bounds("picker-action.first")
            .unwrap()
            .center();
        let outside = point(px(200.), px(100.));
        fixture.cx.simulate_event(MouseDownEvent {
            button: MouseButton::Left,
            position: inside,
            ..Default::default()
        });
        fixture.cx.simulate_event(MouseUpEvent {
            button: MouseButton::Left,
            position: outside,
            ..Default::default()
        });
        fixture.draw();
        assert_eq!(fixture.count(), 0);
        fixture.focus_first();
        fixture.key("space", true, false);
        let scope = fixture
            .consumer
            .read_with(&fixture.cx, |state, _| state.scope.clone());
        fixture.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(true))
                .unwrap()
        });
        fixture.cx.run_until_parked();
        fixture.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(false))
                .unwrap()
        });
        fixture.draw();
        fixture.focus_first();
        fixture.key("space", false, false);
        assert_eq!(fixture.count(), 0);
        fixture.cx.simulate_click(inside, Modifiers::none());
        fixture.draw();
        assert_eq!(fixture.count(), 1);
    }
}
