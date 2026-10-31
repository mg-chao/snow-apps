// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context as ViewContext, IntoElement, Render, TestAppContext, WindowHandle, div,
    prelude::*, px, size,
};
use std::{cell::Cell, task::Context as FutureContext};

struct Page;
impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut ViewContext<Self>) -> impl IntoElement {
        div().size_full()
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    api: Notification,
    scope: ConfigScope,
}
impl Fixture {
    fn new(motion: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut created = None;
        let window = cx.open_window(size(px(800.), px(600.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(motion))
                .unwrap();
            let page = cx.new(|_| Page);
            let root = UiRoot::new(scope.clone(), page, window, cx);
            created = Some((root.notifications(), scope));
            root
        });
        let (api, scope) = created.unwrap();
        let mut f = Self {
            cx,
            window,
            api,
            scope,
        };
        f.draw();
        f
    }
    fn run<R>(&mut self, f: impl FnOnce(&Notification, &mut Window, &mut App) -> R) -> R {
        let api = self.api.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| f(&api, window, cx))
            .unwrap()
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
    }
    fn open(&mut self, options: NotificationOptions) -> NotificationHandle {
        let result = self.run(|api, w, cx| api.open(options, w, cx));
        self.draw();
        result
    }
    fn count(&mut self) -> usize {
        self.run(|api, _, cx| api.len(cx))
    }
    fn layouts(&mut self) -> Vec<NotificationLayout> {
        self.run(|api, _, cx| api.layouts(cx))
    }
    fn configure(&mut self, patch: NotificationConfigPatch) {
        self.run(|api, w, cx| api.set_config(patch, w, cx));
        self.draw();
    }
    fn hover(&mut self, handle: &NotificationHandle, hovered: bool) {
        self.run(|api, w, cx| {
            api.state
                .upgrade()
                .unwrap()
                .update(cx, |s, cx| s.hover(handle.id, hovered, w, cx))
        });
    }
}
fn persistent(title: &str) -> NotificationOptions {
    NotificationOptions::new(title.to_owned()).duration(Duration::ZERO)
}
fn completed(handle: &NotificationHandle) -> Option<NotificationCloseEvent> {
    let mut future = handle.completion();
    let mut context = FutureContext::from_waker(Waker::noop());
    match Pin::new(&mut future).poll(&mut context) {
        Poll::Ready(event) => Some(event),
        Poll::Pending => None,
    }
}

#[test]
fn defaults_match_reference() {
    let c = NotificationConfig::default();
    assert_eq!(c.duration, Duration::from_millis(4500));
    assert_eq!(c.placement, NotificationPlacement::TopRight);
    assert_eq!(c.stack, NotificationStack::Enabled { threshold: 3 });
    assert!(c.pause_on_hover && c.closable);
    assert!(!c.show_progress);
    assert_eq!(c.max_count, None);
}
#[test]
fn window_service_uses_the_ui_root_controller() {
    let mut f = Fixture::new(false);
    f.run(|api, w, cx| {
        let window_api = Notification::for_window(w, cx).unwrap();
        assert_eq!(
            api.state.upgrade().unwrap().entity_id(),
            window_api.state.upgrade().unwrap().entity_id()
        );
    });
}
#[test]
fn default_expiry_and_persistent_lifetimes() {
    let mut f = Fixture::new(false);
    let timed = f.open(NotificationOptions::new("Expires"));
    let kept = f.open(persistent("Persistent"));
    f.advance(4499);
    assert_eq!(f.count(), 2);
    f.advance(1);
    assert_eq!(f.count(), 1);
    assert_eq!(
        completed(&timed).unwrap().reason,
        NotificationCloseReason::Timeout
    );
    assert!(!kept.is_dismissed());
    f.run(|_, w, cx| kept.close(w, cx));
    assert_eq!(
        completed(&kept).unwrap().reason,
        NotificationCloseReason::Programmatic
    );
}
#[test]
fn same_key_preserves_lifetime_and_deadline_but_changed_duration_resets() {
    let mut f = Fixture::new(false);
    let first = f.open(
        NotificationOptions::new("First")
            .key("key")
            .duration(Duration::from_secs(3)),
    );
    f.advance(1000);
    let updated = f.open(
        NotificationOptions::new("Updated")
            .key("key")
            .duration(Duration::from_secs(3)),
    );
    assert_eq!(first.id, updated.id);
    assert_eq!(f.count(), 1);
    f.advance(1999);
    assert!(!first.is_dismissed());
    f.advance(1);
    assert!(updated.is_dismissed());
    let fresh = f.open(
        NotificationOptions::new("Fresh")
            .key("key")
            .duration(Duration::from_secs(3)),
    );
    assert_ne!(first.id, fresh.id);
    f.advance(1000);
    f.open(
        NotificationOptions::new("Changed duration")
            .key("key")
            .duration(Duration::from_secs(5)),
    );
    f.advance(4999);
    assert!(!fresh.is_dismissed());
    f.advance(1);
    assert!(fresh.is_dismissed());
}
#[test]
fn hover_pauses_remaining_time_and_opt_out_keeps_running() {
    let mut f = Fixture::new(false);
    f.configure(NotificationConfigPatch {
        stack: Some(NotificationStack::Disabled),
        ..Default::default()
    });
    let handle = f.open(NotificationOptions::new("Paused").duration(Duration::from_secs(2)));
    f.advance(500);
    f.hover(&handle, true);
    f.advance(3000);
    assert!(!handle.is_dismissed());
    f.hover(&handle, false);
    f.advance(1499);
    assert!(!handle.is_dismissed());
    f.advance(1);
    assert!(handle.is_dismissed());
    let running = f.open(
        NotificationOptions::new("Running")
            .pause_on_hover(false)
            .duration(Duration::from_secs(1)),
    );
    f.hover(&running, true);
    f.advance(1000);
    assert!(running.is_dismissed());
}
#[test]
fn count_limit_is_global_and_updates_do_not_reorder() {
    let mut f = Fixture::new(false);
    f.configure(NotificationConfigPatch {
        max_count: Some(Some(2)),
        ..Default::default()
    });
    let old = f.open(
        persistent("Old")
            .key("old")
            .placement(NotificationPlacement::TopLeft),
    );
    let middle = f.open(persistent("Middle").placement(NotificationPlacement::BottomRight));
    f.open(persistent("Old updated").key("old"));
    let latest = f.open(persistent("Latest"));
    assert_eq!(f.count(), 2);
    assert_eq!(
        completed(&old).unwrap().reason,
        NotificationCloseReason::Evicted
    );
    assert!(!middle.is_dismissed() && !latest.is_dismissed());
    f.run(|api, w, cx| api.destroy_all(w, cx));
    assert_eq!(f.count(), 0);
}
#[test]
fn placements_anchor_and_migration_preserves_handle() {
    let mut f = Fixture::new(false);
    for placement in NotificationPlacement::ALL {
        f.open(persistent("Title").placement(placement));
    }
    let layouts = f.layouts();
    assert_eq!(layouts.len(), 6);
    for layout in layouts {
        assert!((f32::from(layout.bounds.size.width) - 384.).abs() < 0.01);
        assert!(
            (f32::from(if layout.placement.bottom() {
                px(600.) - layout.bounds.bottom()
            } else {
                layout.bounds.top()
            }) - 24.)
                .abs()
                < 0.01
        );
    }
    f.run(|api, w, cx| api.destroy_all(w, cx));
    let a = f.open(persistent("Moves").key("move"));
    let b = f.open(
        persistent("Moved")
            .key("move")
            .placement(NotificationPlacement::BottomLeft),
    );
    assert_eq!(a.id, b.id);
    assert_eq!(f.layouts()[0].placement, NotificationPlacement::BottomLeft);
}
#[test]
fn default_stack_collapses_four_and_expands_on_group_hover() {
    let mut f = Fixture::new(false);
    for i in 0..4 {
        f.open(persistent(&format!("Notice {i}")));
    }
    assert_eq!(f.layouts().iter().filter(|l| l.visible).count(), 3);
    let center = f
        .layouts()
        .into_iter()
        .find(|l| l.visible)
        .unwrap()
        .bounds
        .center();
    f.run(|_, w, cx| w.simulate_mouse_move(center, cx));
    f.draw();
    assert_eq!(f.layouts().iter().filter(|l| l.visible).count(), 4);
}
#[test]
fn callbacks_settle_once_and_may_open_another_notice() {
    let mut f = Fixture::new(false);
    let calls = Rc::new(Cell::new(0));
    let called = calls.clone();
    let api = f.api.clone();
    let handle = f.open(persistent("First").on_close(move |_, w, cx| {
        called.set(called.get() + 1);
        api.open(persistent("From callback"), w, cx);
    }));
    f.run(|_, w, cx| {
        handle.close(w, cx);
        handle.close(w, cx);
    });
    f.draw();
    assert_eq!(calls.get(), 1);
    assert_eq!(f.count(), 1);
}
#[test]
fn immediate_close_and_reopen_do_not_keep_an_unpresented_exit() {
    let mut f = Fixture::new(true);
    let old = f.run(|api, w, cx| {
        let h = api.open(persistent("Invisible").key("same"), w, cx);
        h.close(w, cx);
        h
    });
    let fresh = f.open(persistent("Visible").key("same"));
    assert_ne!(old.id, fresh.id);
    assert_eq!(f.count(), 1);
    assert_eq!(f.layouts().len(), 1);
}
#[test]
fn native_content_can_retain_actions_and_does_not_rebuild_each_progress_frame() {
    let mut f = Fixture::new(false);
    let builds = Rc::new(Cell::new(0));
    let observed = builds.clone();
    f.open(
        NotificationOptions::new("Countdown")
            .show_progress(true)
            .description_content(move |_, _, _| {
                observed.set(observed.get() + 1);
                div().child("Native description")
            }),
    );
    f.advance(16);
    f.advance(16);
    let warm = builds.get();
    for _ in 0..5 {
        f.advance(16);
    }
    assert_eq!(builds.get(), warm);
}
#[test]
fn scoped_defaults_update_without_resetting_active_lifetimes() {
    let mut f = Fixture::new(false);
    let scope = f.scope.clone();
    f.run(|_, _, cx| {
        scope
            .update_patch(cx, |patch| {
                patch.notification.duration = Some(Duration::from_secs(1));
                patch.notification.placement = Some(NotificationPlacement::BottomLeft);
            })
            .unwrap();
    });
    let handle = f.open(NotificationOptions::new("Scoped"));
    f.advance(500);
    let scope = f.scope.clone();
    f.run(|_, _, cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
            .unwrap();
    });
    f.advance(500);
    assert!(handle.is_dismissed());
}

fn key(f: &mut Fixture, value: &'static str) {
    f.run(|_, window, cx| {
        let keystroke = gpui::Keystroke::parse(value).unwrap();
        window.dispatch_event(
            gpui::PlatformInput::KeyDown(gpui::KeyDownEvent {
                keystroke: keystroke.clone(),
                is_held: false,
                prefer_character_input: false,
            }),
            cx,
        );
        window.dispatch_event(
            gpui::PlatformInput::KeyUp(gpui::KeyUpEvent { keystroke }),
            cx,
        );
    });
    f.cx.run_until_parked();
}
fn click(f: &mut Fixture, position: gpui::Point<Pixels>) {
    f.run(|_, window, cx| {
        window.simulate_mouse_move(position, cx);
        window.dispatch_event(
            gpui::PlatformInput::MouseDown(gpui::MouseDownEvent {
                button: gpui::MouseButton::Left,
                position,
                modifiers: Default::default(),
                click_count: 1,
                first_mouse: false,
            }),
            cx,
        );
        window.dispatch_event(
            gpui::PlatformInput::MouseUp(gpui::MouseUpEvent {
                button: gpui::MouseButton::Left,
                position,
                modifiers: Default::default(),
                click_count: 1,
            }),
            cx,
        );
    });
    f.draw();
}
#[test]
fn close_button_pointer_does_not_bubble_to_the_card() {
    let mut f = Fixture::new(false);
    let clicks = Rc::new(Cell::new(0));
    let seen = clicks.clone();
    let h = f.open(persistent("Closable").on_click(move |_, _, _| seen.set(seen.get() + 1)));
    let bounds = f.layouts()[0].bounds;
    click(
        &mut f,
        gpui::point(bounds.left() + px(30.), bounds.top() + px(40.)),
    );
    assert_eq!(clicks.get(), 1);
    click(
        &mut f,
        gpui::point(bounds.right() - px(35.), bounds.top() + px(31.)),
    );
    assert_eq!(
        completed(&h).unwrap().reason,
        NotificationCloseReason::CloseButton
    );
    assert_eq!(clicks.get(), 1);
}
#[test]
fn close_supports_enter_and_space_and_disabled_controls_do_not_activate() {
    for stroke in ["enter", "space"] {
        let mut f = Fixture::new(false);
        let h = f.open(persistent("Keyboard"));
        f.run(|_, w, cx| w.focus_next(cx));
        f.draw();
        key(&mut f, stroke);
        assert_eq!(
            completed(&h).unwrap().reason,
            NotificationCloseReason::CloseButton
        );
    }
    let mut f = Fixture::new(false);
    let h = f.open(persistent("Disabled").close_disabled(true));
    let bounds = f.layouts()[0].bounds;
    click(
        &mut f,
        gpui::point(bounds.right() - px(35.), bounds.top() + px(31.)),
    );
    f.run(|_, w, cx| w.focus_next(cx));
    key(&mut f, "enter");
    assert!(!h.is_dismissed());
}
#[test]
fn keyboard_focus_pauses_the_group_before_another_render() {
    let mut f = Fixture::new(false);
    let h = f.open(
        NotificationOptions::new("Focused")
            .duration(Duration::from_secs(2))
            .pause_on_hover(false),
    );
    f.advance(1500);
    f.run(|_, w, cx| w.focus_next(cx));
    f.cx.run_until_parked();
    // The focus subscription must freeze the timer without relying on prepaint.
    f.cx.executor().advance_clock(Duration::from_secs(10));
    f.cx.run_until_parked();
    assert!(!h.is_dismissed());
    f.run(|_, w, cx| w.blur(cx));
    f.cx.run_until_parked();
    f.advance(499);
    assert!(!h.is_dismissed());
    f.advance(1);
    assert_eq!(
        completed(&h).unwrap().reason,
        NotificationCloseReason::Timeout
    );
}
#[test]
fn rich_actions_preserve_focus_on_open_and_restore_it_on_close() {
    let mut f = Fixture::new(true);
    let outside = f.run(|_, w, cx| w.focused(cx).unwrap());
    let action = f.run(|_, _, cx| crate::behavior::focus_handle(cx));
    let button_focus = action.clone();
    let scope = f.scope.clone();
    let h = f.open(persistent("Action").actions(move |handle, _, _| {
        let handle = handle.clone();
        crate::Button::new("finish")
            .scope(scope.clone())
            .label("Finish")
            .focus_handle(&button_focus)
            .on_click(move |_, w, cx| handle.close(w, cx))
    }));
    f.advance(250);
    f.run(|_, w, cx| {
        assert!(outside.is_focused(w));
        w.focus(&action, cx);
    });
    f.draw();
    key(&mut f, "enter");
    assert_eq!(
        completed(&h).unwrap().reason,
        NotificationCloseReason::Programmatic
    );
    f.run(|_, w, _| assert!(outside.is_focused(w)));
    f.draw();
    assert!(f.layouts()[0].closing);
    f.advance(250);
    assert!(f.layouts().is_empty());
}
#[test]
fn window_identity_rejects_cross_window_mutations() {
    let mut f = Fixture::new(false);
    let original = f.open(persistent("Local"));
    let other = f.cx.open_window(size(px(500.), px(400.)), |_, _| Page);
    let api = f.api.clone();
    let rejected =
        f.cx.update_window(other.into(), |_, w, cx| {
            api.destroy_all(w, cx);
            api.open(persistent("Wrong window"), w, cx)
        })
        .unwrap();
    assert_eq!(
        completed(&rejected).unwrap().reason,
        NotificationCloseReason::Unavailable
    );
    assert!(!original.is_dismissed());
    assert_eq!(f.count(), 1);
}
#[test]
fn stale_handle_cannot_close_a_reused_key_and_dropped_waiters_are_removed() {
    let mut f = Fixture::new(false);
    let old = f.open(persistent("Old").key("reused"));
    {
        let mut pending = old.completion();
        assert!(
            Pin::new(&mut pending)
                .poll(&mut FutureContext::from_waker(Waker::noop()))
                .is_pending()
        );
    }
    assert!(old.completion.borrow().waiters.is_empty());
    f.run(|_, w, cx| old.close(w, cx));
    let fresh = f.open(persistent("New").key("reused"));
    f.run(|_, w, cx| old.close(w, cx));
    assert!(!fresh.is_dismissed());
    assert_ne!(old.id, fresh.id);
}
#[test]
fn idle_and_persistent_progress_hosts_schedule_no_continuous_frames() {
    let mut f = Fixture::new(false);
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
    f.open(persistent("Static").show_progress(true));
    for _ in 0..5 {
        f.draw();
        assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
    }
    f.open(
        NotificationOptions::new("Progress")
            .duration(Duration::from_secs(30))
            .show_progress(true),
    );
    for _ in 0..8 {
        f.draw();
    }
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 1);
    f.run(|api, w, cx| api.destroy_all(w, cx));
    f.advance(500);
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
}
#[test]
fn unmount_settles_lifetimes_and_releases_factories_without_ui_callbacks() {
    struct Owner {
        controller: Entity<NotificationState>,
        mounted: bool,
    }
    impl Render for Owner {
        fn render(&mut self, _: &mut Window, _: &mut ViewContext<Self>) -> impl IntoElement {
            if self.mounted {
                NotificationHost::new("scoped", &self.controller, div()).into_any_element()
            } else {
                div().into_any_element()
            }
        }
    }
    let mut cx = TestAppContext::single();
    let mut controller = None;
    let window = cx.open_window(size(px(800.), px(600.)), |w, cx| {
        let scope = ConfigScope::new_window(cx);
        let state = cx.new(|cx| NotificationState::new(scope, w, cx));
        controller = Some(state.clone());
        Owner {
            controller: state,
            mounted: true,
        }
    });
    let state = controller.unwrap();
    let api = Notification::new(&state);
    let calls = Rc::new(Cell::new(0));
    let observed = calls.clone();
    let retained = Rc::new(());
    let weak = Rc::downgrade(&retained);
    let handle = cx
        .update_window(window.into(), |_, w, cx| {
            let handle = api.open(
                persistent("Owned")
                    .description_content(move |_, _, _| {
                        let _ = &retained;
                        div()
                    })
                    .on_close(move |_, _, _| observed.set(observed.get() + 1)),
                w,
                cx,
            );
            w.draw(cx).clear(cx);
            handle
        })
        .unwrap();
    window
        .update(&mut cx, |owner, _, cx| {
            owner.mounted = false;
            cx.notify();
        })
        .unwrap();
    for _ in 0..3 {
        cx.run_until_parked();
        cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
    }
    assert_eq!(
        completed(&handle).unwrap().reason,
        NotificationCloseReason::OwnerDropped
    );
    assert_eq!(calls.get(), 0);
    assert!(weak.upgrade().is_none());
    assert_eq!(state.read_with(&cx, |state, _| state.active_count()), 0);
}

#[test]
fn holder_and_content_semantics_participate_in_native_layout() {
    let mut f = Fixture::new(false);
    let styles = NotificationStyles {
        list: gpui::StyleRefinement::default().p(px(31.)),
        list_content: gpui::StyleRefinement::default().gap(px(37.)),
        ..Default::default()
    };
    f.configure(NotificationConfigPatch {
        styles,
        stack: Some(NotificationStack::Disabled),
        ..Default::default()
    });
    f.open(persistent("Older"));
    f.open(persistent("Newest"));
    let layouts = f.layouts();
    assert_eq!(layouts[0].bounds.top(), px(31.));
    assert_eq!(layouts[0].bounds.right(), px(769.));
    assert_eq!(
        layouts[1].bounds.top() - layouts[0].bounds.bottom(),
        px(37.)
    );
}
#[test]
fn hidden_roots_and_offscreen_progress_do_not_request_frames() {
    let mut f = Fixture::new(false);
    let builds = Rc::new(Cell::new(0));
    let observed = builds.clone();
    f.open(
        NotificationOptions::new("Hidden")
            .show_progress(true)
            .description_content(move |_, _, _| {
                observed.set(observed.get() + 1);
                div()
            })
            .styles(NotificationStyles {
                root: gpui::StyleRefinement::default().hidden(),
                ..Default::default()
            }),
    );
    assert_eq!(builds.get(), 0);
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
    f.run(|api, w, cx| api.destroy_all(w, cx));
    f.configure(NotificationConfigPatch {
        stack: Some(NotificationStack::Disabled),
        ..Default::default()
    });
    f.open(
        NotificationOptions::new("Offscreen progress")
            .show_progress(true)
            .duration(Duration::from_secs(600)),
    );
    for _ in 0..10 {
        f.open(persistent("Static foreground").description("Short content"));
    }
    f.advance(100);
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
}
#[test]
fn collapsed_stack_respects_threshold_and_keeps_physical_bottom_anchor() {
    for (threshold, visible) in [(1, 1), (3, 3), (5, 3)] {
        let mut f = Fixture::new(false);
        f.configure(NotificationConfigPatch {
            stack: Some(NotificationStack::Enabled { threshold }),
            placement: Some(NotificationPlacement::BottomLeft),
            ..Default::default()
        });
        for _ in 0..6 {
            f.open(persistent("Stacked").description("Description"));
        }
        let layouts = f.layouts();
        assert_eq!(layouts.iter().filter(|l| l.visible).count(), visible);
        assert_eq!(layouts[0].bounds.bottom(), px(576.));
        if visible > 1 {
            assert_eq!(layouts[1].bounds.top(), layouts[0].bounds.top() - px(8.));
            assert!((f32::from(layouts[1].bounds.size.width) - 384. * 0.94).abs() < 0.001);
        }
    }
}

#[test]
fn content_factory_may_close_itself_without_painting_a_stale_snapshot() {
    let mut f = Fixture::new(false);
    let handle = f.open(
        persistent("Reentrant").description_content(|handle, w, cx| {
            handle.close(w, cx);
            div().child("Already closed")
        }),
    );
    assert!(handle.is_dismissed());
    assert_eq!(f.count(), 0);
    assert!(f.layouts().is_empty());
}

#[test]
fn interrupted_entry_closes_from_current_geometry_and_then_becomes_idle() {
    let mut f = Fixture::new(true);
    let h = f.open(persistent("Animated"));
    let start = f.layouts()[0].bounds.left();
    f.advance(80);
    let current = f.layouts()[0].bounds.left();
    assert!(current < start);
    f.run(|_, w, cx| h.close(w, cx));
    f.draw();
    assert_eq!(f.layouts()[0].bounds.left(), current);
    assert!(f.layouts()[0].closing);
    f.advance(1000);
    assert!(f.layouts().is_empty());
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
}

#[test]
fn overflowing_top_and_bottom_groups_scroll_in_their_physical_direction() {
    for placement in [
        NotificationPlacement::TopRight,
        NotificationPlacement::BottomRight,
    ] {
        let mut f = Fixture::new(false);
        f.configure(NotificationConfigPatch {
            placement: Some(placement),
            stack: Some(NotificationStack::Disabled),
            ..Default::default()
        });
        for _ in 0..10 {
            f.open(persistent("Scroll").description("Content"));
        }
        let before = f.layouts()[0].bounds;
        f.run(|_, w, cx| {
            let position = before.center();
            w.simulate_mouse_move(position, cx);
            w.dispatch_event(
                gpui::PlatformInput::Scroll(gpui::ScrollEvent {
                    position,
                    delta: gpui::ScrollDelta::Pixels(gpui::point(
                        px(0.),
                        px(if placement.bottom() { 100. } else { -100. }),
                    )),
                    modifiers: Default::default(),
                    touch_phase: gpui::TouchPhase::Moved,
                    ..Default::default()
                }),
                cx,
            );
        });
        f.draw();
        let after = f.layouts()[0].bounds;
        assert_eq!(
            after.top() - before.top(),
            px(if placement.bottom() { 100. } else { -100. })
        );
    }
}
#[test]
fn transparent_holder_does_not_sustain_progress_frames() {
    let mut f = Fixture::new(false);
    f.configure(NotificationConfigPatch {
        styles: NotificationStyles {
            list: gpui::StyleRefinement::default().opacity(0.),
            ..Default::default()
        },
        ..Default::default()
    });
    f.open(NotificationOptions::new("Invisible progress").show_progress(true));
    assert_eq!(f.run(|_, w, cx| w.simulate_next_frame(cx)), 0);
}

#[test]
fn placement_migration_discards_old_hover_but_preserves_descendant_focus_pause() {
    for focused in [false, true] {
        let mut f = Fixture::new(false);
        let options = NotificationOptions::new("Moves")
            .key("moves")
            .duration(Duration::from_secs(2));
        let h = f.open(options.clone());
        f.advance(500);
        let center = f.layouts()[0].bounds.center();
        f.run(|_, w, cx| {
            if focused {
                w.focus_next(cx);
            } else {
                w.simulate_mouse_move(center, cx);
            }
        });
        f.draw();
        f.advance(3000);
        assert!(!h.is_dismissed());
        f.open(options.placement(NotificationPlacement::BottomLeft));
        f.advance(1500);
        assert_eq!(h.is_dismissed(), !focused);
    }
}

#[test]
fn action_footer_stays_on_the_physical_right_in_both_directions() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(false);
        let scope = f.scope.clone();
        f.run(|_, _, cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap()
        });
        let bounds = Rc::new(Cell::new(None));
        let observed = bounds.clone();
        f.open(persistent("Actions").actions(move |_, _, _| {
            let observed = observed.clone();
            gpui::canvas(
                move |bounds, _, _| observed.set(Some(bounds)),
                |_, _, _, _| {},
            )
            .w(px(80.))
            .h(px(20.))
        }));
        let card = f.layouts()[0].bounds;
        let action = bounds.get().expect("native action was laid out");
        assert_eq!(action.right(), card.right() - px(24.));
        assert_eq!(action.size.width, px(80.));
    }
}
