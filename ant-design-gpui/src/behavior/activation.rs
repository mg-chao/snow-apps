// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    rc::{Rc, Weak},
};

use gpui::{
    AccessibleAction, App, ClickEvent, FocusHandle, KeyDownEvent, KeyboardButton, MouseButton,
    StatefulInteractiveElement, Subscription, Window,
};

/// The availability of an actionable control.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Availability {
    pub disabled: bool,
    pub busy: bool,
}

impl Availability {
    pub fn can_activate(self) -> bool {
        !self.disabled && !self.busy
    }

    /// Busy controls retain their place in keyboard navigation.
    pub fn can_focus(self) -> bool {
        !self.disabled
    }
}

/// The origin of an activation delivered through the shared availability guard.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Activation {
    Pointer,
    Keyboard(KeyboardButton),
    /// Selection changed by a navigation key, such as an arrow in a radio group.
    KeyboardNavigation,
    Accessibility,
}

#[derive(Default)]
struct PressState {
    availability: Availability,
    epoch: u64,
    keyboard: Option<(KeyboardButton, u64)>,
    pointer: Option<u64>,
}

impl PressState {
    fn cancel(&mut self) {
        self.epoch = self.epoch.wrapping_add(1);
        self.keyboard = None;
        self.pointer = None;
    }

    fn key_down(&mut self, event: &KeyDownEvent) {
        let key = activation_key(&event.keystroke.key);
        if !self.availability.can_activate()
            || event.keystroke.modifiers.modified()
            || key.is_none()
        {
            self.keyboard = None;
        } else if !event.is_held {
            self.keyboard = key.map(|key| (key, self.epoch));
        }
    }

    fn key_up(&mut self, key: &str, modified: bool) {
        if modified || self.keyboard != activation_key(key).map(|key| (key, self.epoch)) {
            self.keyboard = None;
        }
    }

    fn take_click(&mut self, event: &ClickEvent) -> Option<Activation> {
        if !self.availability.can_activate() {
            self.cancel();
            return None;
        }
        match event {
            ClickEvent::Keyboard(event) => (self.keyboard.take()
                == Some((event.button, self.epoch)))
            .then_some(Activation::Keyboard(event.button)),
            ClickEvent::Mouse(_) | ClickEvent::Touch(_) => {
                (self.pointer.take() == Some(self.epoch)).then_some(Activation::Pointer)
            }
        }
    }
}

fn activation_key(key: &str) -> Option<KeyboardButton> {
    match key {
        "enter" => Some(KeyboardButton::Enter),
        "space" => Some(KeyboardButton::Space),
        _ => None,
    }
}

/// Retained cancellation state layered on GPUI's native pointer/keyboard click synthesis.
///
/// Keep one guard per control, update availability as soon as application state changes,
/// and retain the subscription returned by [`Self::observe_blur`]. A guard never schedules
/// timers, synthesizes clicks, or changes the element's stable ID.
#[derive(Clone, Default)]
pub struct ActivationGuard(Rc<RefCell<PressState>>);

#[derive(Clone)]
pub(crate) struct WeakActivationGuard(Weak<RefCell<PressState>>);

impl WeakActivationGuard {
    pub fn availability(&self) -> Option<Availability> {
        self.0.upgrade().map(|state| state.borrow().availability)
    }
}

impl ActivationGuard {
    pub(crate) fn downgrade(&self) -> WeakActivationGuard {
        WeakActivationGuard(Rc::downgrade(&self.0))
    }
    pub fn new(availability: Availability) -> Self {
        Self(Rc::new(RefCell::new(PressState {
            availability,
            ..PressState::default()
        })))
    }

    pub fn availability(&self) -> Availability {
        self.0.borrow().availability
    }

    /// Cancel pending input on either edge, including disabled→enabled and busy→ready.
    pub fn set_availability(&self, availability: Availability) {
        let mut state = self.0.borrow_mut();
        if state.availability != availability {
            state.cancel();
            state.availability = availability;
        }
    }

    pub fn cancel(&self) {
        self.0.borrow_mut().cancel();
    }

    /// Update a control's availability and relinquish focus when it becomes disabled.
    /// Busy controls keep keyboard focus and can still be tabbed past normally.
    pub fn update(
        &self,
        availability: Availability,
        focus: &FocusHandle,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.set_availability(availability);
        if !availability.can_focus() && focus.is_focused(window) {
            window.blur(cx);
        }
    }

    pub fn observe_blur(
        &self,
        focus: &FocusHandle,
        window: &mut Window,
        cx: &mut App,
    ) -> Subscription {
        let guard = self.clone();
        window.on_focus_out(focus, cx, move |_, _, _| guard.cancel())
    }

    /// Install native click handling and an explicit accessible action on an identified element.
    ///
    /// The caller applies focus tracking only when `availability().can_focus()` and applies
    /// matching accessibility metadata. Callbacks always check current retained state, even
    /// if a state change has not rendered a replacement element tree yet.
    pub fn install<E: StatefulInteractiveElement>(
        &self,
        element: E,
        on_activate: impl Fn(Activation, &mut Window, &mut App) + 'static,
    ) -> E {
        let callback = Rc::new(on_activate);
        let down = self.clone();
        let up = self.clone();
        let key_cleanup = self.clone();
        let pointer_down = self.clone();
        let pointer_out = self.clone();
        let pointer_cleanup = self.clone();
        let click = self.clone();
        let accessible = self.clone();
        let click_callback = callback.clone();
        element
            .capture_key_down(move |event, _, _| down.0.borrow_mut().key_down(event))
            .capture_key_up(move |event, _, _| {
                up.0.borrow_mut()
                    .key_up(&event.keystroke.key, event.keystroke.modifiers.modified());
            })
            .on_key_up(move |_, _, _| key_cleanup.0.borrow_mut().keyboard = None)
            .capture_any_mouse_down(move |event, _, _| {
                let mut state = pointer_down.0.borrow_mut();
                state.pointer = (event.button == MouseButton::Left
                    && state.availability.can_activate())
                .then_some(state.epoch);
            })
            .on_mouse_up_out(MouseButton::Left, move |_, _, _| {
                pointer_out.0.borrow_mut().pointer = None;
            })
            .on_mouse_up(MouseButton::Left, move |_, _, _| {
                pointer_cleanup.0.borrow_mut().pointer = None;
            })
            .on_click(move |event, window, cx| {
                let activation = click.0.borrow_mut().take_click(event);
                if let Some(activation) = activation {
                    click_callback(activation, window, cx);
                }
            })
            .on_a11y_action(AccessibleAction::Click, move |_, window, cx| {
                let enabled = {
                    let mut state = accessible.0.borrow_mut();
                    state.cancel();
                    state.availability.can_activate()
                };
                if enabled {
                    callback(Activation::Accessibility, window, cx);
                }
            })
    }
}
