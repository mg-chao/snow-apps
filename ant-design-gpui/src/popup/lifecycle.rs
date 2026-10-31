// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use super::{PopupOpenChange, PopupOpenReason};

/// A cancelable intent, separate from the owner's authoritative controlled value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct PendingChange {
    pub generation: u64,
    pub change: PopupOpenChange,
    pub delay: Duration,
}

/// Pure interaction state. The executor owns timers, this value owns their validity.
/// Dismissal suppresses already-held transient inputs until they are released.
#[derive(Debug)]
pub(super) struct Lifecycle {
    controlled: Option<bool>,
    local: bool,
    disabled: bool,
    desired: bool,
    trigger_hover: bool,
    popup_hover: bool,
    focused: bool,
    latched: bool,
    suppress_hover: bool,
    suppress_focus: bool,
    generation: u64,
    pending: Option<PendingChange>,
    last_proposal: Option<bool>,
}

impl Lifecycle {
    pub fn new(controlled: Option<bool>, default_open: bool) -> Self {
        let open = controlled.unwrap_or(default_open);
        Self {
            controlled,
            local: open,
            disabled: false,
            desired: open,
            trigger_hover: false,
            popup_hover: false,
            focused: false,
            latched: false,
            suppress_hover: false,
            suppress_focus: false,
            generation: 0,
            pending: None,
            last_proposal: None,
        }
    }

    pub fn open(&self) -> bool {
        !self.disabled && self.controlled.unwrap_or(self.local)
    }

    pub fn pending(&self) -> Option<PendingChange> {
        self.pending
    }

    pub fn sync(&mut self, controlled: Option<bool>, disabled: bool) {
        if controlled != self.controlled {
            // Leaving controlled mode retains the last visible owner value.
            self.local = controlled.or(self.controlled).unwrap_or(self.local);
            self.controlled = controlled;
            self.desired = self.open();
            self.last_proposal = None;
            self.invalidate();
        }
        if self.disabled != disabled {
            self.disabled = disabled;
            self.trigger_hover = false;
            self.popup_hover = false;
            self.focused = false;
            self.latched = false;
            self.suppress_hover = false;
            self.suppress_focus = false;
            self.last_proposal = None;
            if disabled {
                self.local = false;
            }
            self.desired = self.open();
            self.invalidate();
        }
    }

    pub fn hover(&mut self, popup: bool, hovered: bool, enter: Duration, leave: Duration) {
        let field = if popup {
            &mut self.popup_hover
        } else {
            &mut self.trigger_hover
        };
        if *field == hovered {
            return;
        }
        *field = hovered;
        if !self.trigger_hover && !self.popup_hover {
            self.suppress_hover = false;
        }
        let desired = self.interested();
        self.schedule(
            desired,
            PopupOpenReason::Hover,
            if desired { enter } else { leave },
        );
    }

    pub fn focus(&mut self, focused: bool) {
        if self.focused == focused {
            return;
        }
        self.focused = focused;
        if !focused {
            self.suppress_focus = false;
        }
        self.schedule(self.interested(), PopupOpenReason::Focus, Duration::ZERO);
    }

    pub fn toggle(&mut self) {
        // Each activation toggles what is actually presented. A controlled
        // owner can decline a previous proposal, and a delayed hover intent
        // has not opened anything yet.
        let next = !self.open();
        self.begin_gesture();
        if next {
            self.latched = true;
            self.schedule(true, PopupOpenReason::Click, Duration::ZERO);
        } else {
            self.dismiss(PopupOpenReason::Click);
        }
    }

    pub fn request(&mut self, open: bool, reason: PopupOpenReason) {
        if open {
            if matches!(
                reason,
                PopupOpenReason::ContextMenu | PopupOpenReason::Accessibility
            ) {
                self.begin_gesture();
            }
            self.latched = true;
            self.schedule(true, reason, Duration::ZERO);
        } else {
            self.dismiss(reason);
        }
    }

    pub fn dismiss(&mut self, reason: PopupOpenReason) {
        if matches!(
            reason,
            PopupOpenReason::Click
                | PopupOpenReason::Accessibility
                | PopupOpenReason::Escape
                | PopupOpenReason::OutsidePress
                | PopupOpenReason::Confirm
                | PopupOpenReason::Cancel
        ) {
            self.begin_gesture();
        }
        self.latched = false;
        self.suppress_hover = self.trigger_hover || self.popup_hover;
        self.suppress_focus = self.focused;
        self.schedule(false, reason, Duration::ZERO);
    }

    fn begin_gesture(&mut self) {
        // Continuous hover/focus signals and repeated programmatic requests
        // still coalesce. Separate user gestures must remain observable even
        // when the owner declined the preceding request.
        self.desired = self.open();
        self.last_proposal = None;
        self.invalidate();
    }

    /// Deliver a still-current timer. Controlled owners receive proposals only.
    pub fn commit(&mut self, generation: u64) -> Option<PopupOpenChange> {
        let pending = self
            .pending
            .filter(|pending| pending.generation == generation)?;
        self.pending = None;
        if self.disabled || self.open() == pending.change.open {
            return None;
        }
        if self.last_proposal == Some(pending.change.open) {
            return None;
        }
        self.last_proposal = Some(pending.change.open);
        if self.controlled.is_none() {
            self.local = pending.change.open;
            self.last_proposal = None;
        }
        Some(pending.change)
    }

    fn interested(&self) -> bool {
        self.latched
            || ((self.trigger_hover || self.popup_hover) && !self.suppress_hover)
            || (self.focused && !self.suppress_focus)
    }

    fn schedule(&mut self, open: bool, reason: PopupOpenReason, delay: Duration) {
        if self.disabled {
            return;
        }
        // Transferring between trigger and surface must not restart the same timer.
        // Immediate focus activation may, however, supersede a delayed hover open.
        if self.desired == open && self.pending.is_some_and(|p| p.delay <= delay) {
            return;
        }
        if self.desired == open && self.pending.is_none() {
            return;
        }
        self.desired = open;
        self.invalidate();
        if self.open() == open {
            self.last_proposal = None;
            return;
        }
        self.pending = Some(PendingChange {
            generation: self.generation,
            change: PopupOpenChange { open, reason },
            delay,
        });
    }

    fn invalidate(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.pending = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DELAY: Duration = Duration::from_millis(100);

    fn commit(state: &mut Lifecycle) -> Option<PopupOpenChange> {
        state.pending().and_then(|p| state.commit(p.generation))
    }

    #[test]
    fn rapid_hover_cancels_the_old_timer_and_never_opens() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        let old = state.pending().unwrap();
        state.hover(false, false, DELAY, DELAY);
        assert_eq!(state.commit(old.generation), None);
        assert!(!state.open());
    }

    #[test]
    fn crossing_the_gap_cancels_close_and_surface_leave_closes() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        assert!(commit(&mut state).unwrap().open);
        state.hover(false, false, DELAY, DELAY);
        let close = state.pending().unwrap();
        state.hover(true, true, DELAY, DELAY);
        assert_eq!(state.commit(close.generation), None);
        assert!(state.open());
        state.hover(true, false, DELAY, DELAY);
        assert!(!commit(&mut state).unwrap().open);
    }

    #[test]
    fn focused_popup_survives_pointer_leave_until_focus_leaves() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        state.focus(true);
        assert_eq!(state.pending().unwrap().delay, Duration::ZERO);
        commit(&mut state);
        state.hover(false, false, DELAY, DELAY);
        assert_eq!(state.pending(), None);
        assert!(state.open());
        state.focus(false);
        assert!(!commit(&mut state).unwrap().open);
    }

    #[test]
    fn escape_suppresses_held_inputs_without_preventing_future_entry() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        state.focus(true);
        commit(&mut state);
        state.dismiss(PopupOpenReason::Escape);
        assert_eq!(commit(&mut state).unwrap().reason, PopupOpenReason::Escape);
        state.focus(true);
        state.hover(false, true, DELAY, DELAY);
        assert_eq!(state.pending(), None);
        state.hover(false, false, DELAY, DELAY);
        state.hover(false, true, DELAY, DELAY);
        assert!(commit(&mut state).unwrap().open);
    }

    #[test]
    fn controlled_proposals_never_change_authoritative_visibility() {
        let mut state = Lifecycle::new(Some(false), true);
        state.toggle();
        assert!(commit(&mut state).unwrap().open);
        assert!(!state.open());
        state.sync(Some(false), false);
        state.request(true, PopupOpenReason::Programmatic);
        assert_eq!(commit(&mut state), None);
        state.sync(Some(true), false);
        assert!(state.open());
        state.dismiss(PopupOpenReason::OutsidePress);
        assert!(!commit(&mut state).unwrap().open);
        assert!(state.open());
        state.sync(Some(false), false);
        assert!(!state.open());
    }

    #[test]
    fn disabling_and_reenabling_invalidates_old_requests() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        let pending = state.pending().unwrap();
        state.sync(None, true);
        state.sync(None, false);
        assert_eq!(state.commit(pending.generation), None);
        assert!(!state.open());
        state.hover(false, true, DELAY, DELAY);
        assert!(commit(&mut state).unwrap().open);
    }

    #[test]
    fn click_latches_open_until_explicit_dismissal() {
        let mut state = Lifecycle::new(None, false);
        state.toggle();
        commit(&mut state);
        state.hover(false, true, DELAY, DELAY);
        state.hover(false, false, DELAY, DELAY);
        assert!(state.open());
        assert_eq!(state.pending(), None);
        state.toggle();
        assert!(!commit(&mut state).unwrap().open);
    }

    #[test]
    fn rejected_controlled_requests_do_not_consume_the_next_gesture() {
        let mut state = Lifecycle::new(Some(false), false);
        for _ in 0..3 {
            state.toggle();
            assert_eq!(
                commit(&mut state),
                Some(PopupOpenChange {
                    open: true,
                    reason: PopupOpenReason::Click
                })
            );
            state.sync(Some(false), false);
            assert!(!state.open());
        }
        state.sync(Some(true), false);
        for reason in [
            PopupOpenReason::Escape,
            PopupOpenReason::Escape,
            PopupOpenReason::OutsidePress,
        ] {
            state.dismiss(reason);
            assert_eq!(
                commit(&mut state),
                Some(PopupOpenChange {
                    open: false,
                    reason
                })
            );
            assert!(state.open());
        }
        state.toggle();
        assert!(!commit(&mut state).unwrap().open);
    }

    #[test]
    fn activation_supersedes_pending_hover_without_toggling_an_unopened_popup_closed() {
        let mut state = Lifecycle::new(None, false);
        state.hover(false, true, DELAY, DELAY);
        let pending_hover = state.pending().unwrap();
        state.toggle();
        assert!(commit(&mut state).unwrap().open);
        assert_eq!(state.commit(pending_hover.generation), None);
        assert!(state.open());
    }
}
