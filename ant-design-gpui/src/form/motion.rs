// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::Cell,
    rc::Rc,
    time::{Duration, Instant},
};

use gpui::{App, Context, Pixels, Window, px};

use crate::{
    behavior::{MotionPolicy, ScalarTransition},
    theme::CubicBezier,
};

/// Retains repeatable help content only for its measured leave transition.
/// Settled states own no timer or frame request.
pub(super) struct HelpMotion<T: Clone + PartialEq + 'static> {
    content: Option<T>,
    visible: bool,
    transition: ScalarTransition,
    height_transition: ScalarTransition,
    resizing: bool,
    measurement_pending: bool,
    policy: MotionPolicy,
    duration: Duration,
    epoch: Instant,
    frame_pending: bool,
    pub natural_height: Rc<Cell<Option<Pixels>>>,
}

pub(super) struct HelpSample<T> {
    pub content: Option<T>,
    pub opacity: f32,
    pub height: Option<Pixels>,
    pub leaving: bool,
}

impl<T: Clone + PartialEq + 'static> HelpMotion<T> {
    pub fn new(content: Option<T>, curve: CubicBezier, cx: &App) -> Self {
        let visible = content.is_some();
        Self {
            content,
            visible,
            transition: ScalarTransition::new(if visible { 1. } else { 0. }, curve),
            height_transition: ScalarTransition::new(0., curve),
            resizing: false,
            measurement_pending: false,
            policy: MotionPolicy::new(false, false),
            duration: Duration::ZERO,
            epoch: cx.background_executor().now(),
            frame_pending: false,
            natural_height: Rc::default(),
        }
    }
    pub fn sample(
        &mut self,
        content: Option<T>,
        policy: MotionPolicy,
        duration: Duration,
        window: &Window,
        cx: &mut Context<Self>,
    ) -> HelpSample<T> {
        let now = cx.background_executor().now().duration_since(self.epoch);
        let visible = content.is_some();
        self.measurement_pending |= visible && content != self.content;
        self.policy = policy;
        self.duration = duration;
        self.transition
            .retarget(if visible { 1. } else { 0. }, now, duration, policy);
        self.visible = visible;
        if content.is_some() {
            self.content = content;
        }
        let sample = self.transition.sample(now);
        if sample.finished && !self.visible {
            self.content = None;
            self.resizing = false;
            self.measurement_pending = false;
        }
        if !policy.allows_motion() {
            self.resizing = false;
            self.measurement_pending = false;
        }
        let resized = self.height_transition.sample(now);
        self.resizing &= !resized.finished;
        if !sample.finished || self.resizing {
            self.request_frame(window, cx);
        }
        let natural = if self.resizing {
            px(resized.value.max(0.) as f32)
        } else {
            self.natural_height.get().unwrap_or(px(0.))
        };
        let height = if self.content.is_none() {
            None
        } else if !sample.finished {
            Some(natural * sample.value.clamp(0., 1.) as f32)
        } else if self.resizing || self.measurement_pending {
            Some(natural)
        } else {
            None
        };
        HelpSample {
            content: self.content.clone(),
            opacity: sample.value.clamp(0., 1.) as f32,
            height,
            leaving: !self.visible && self.content.is_some(),
        }
    }
    pub fn measured(&mut self, height: Pixels, window: &Window, cx: &mut Context<Self>) {
        let previous = self.natural_height.replace(Some(height));
        if previous == Some(height) {
            self.measurement_pending = false;
            return;
        }
        let now = cx.background_executor().now().duration_since(self.epoch);
        if let Some(previous) = previous.filter(|_| {
            self.measurement_pending
                && self.visible
                && self.transition.sample(now).finished
                && self.policy.allows_motion()
        }) {
            if !self.resizing {
                self.height_transition.retarget(
                    f64::from(f32::from(previous)),
                    now,
                    Duration::ZERO,
                    MotionPolicy::new(false, false),
                );
            }
            self.height_transition.retarget(
                f64::from(f32::from(height)),
                now,
                self.duration,
                self.policy,
            );
            self.resizing = !self.height_transition.sample(now).finished;
            if self.resizing {
                self.request_frame(window, cx);
            }
        } else {
            self.height_transition.retarget(
                f64::from(f32::from(height)),
                now,
                Duration::ZERO,
                MotionPolicy::new(false, false),
            );
            self.resizing = false;
        }
        self.measurement_pending = false;
    }
    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if !self.frame_pending {
            self.frame_pending = true;
            let weak = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = weak.update(cx, |state, cx| {
                    state.frame_pending = false;
                    cx.notify();
                });
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{AppContext, EmptyView, Entity, SharedString, TestAppContext, WindowHandle};

    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<EmptyView>,
        state: Entity<HelpMotion<SharedString>>,
    }
    impl Fixture {
        fn new(initial: Option<&str>) -> Self {
            let mut cx = TestAppContext::single();
            let mut state = None;
            let window = cx.add_window(|_, cx| {
                state = Some(cx.new(|cx| {
                    HelpMotion::new(
                        initial.map(Into::into),
                        CubicBezier {
                            x1: 0.,
                            y1: 0.,
                            x2: 1.,
                            y2: 1.,
                        },
                        cx,
                    )
                }));
                EmptyView
            });
            let state = state.unwrap();
            cx.update(|cx| state.read(cx).natural_height.set(Some(px(44.))));
            Self { cx, window, state }
        }
        fn sample(
            &mut self,
            value: Option<&str>,
            policy: MotionPolicy,
        ) -> HelpSample<SharedString> {
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    self.state.update(cx, |state, cx| {
                        state.sample(
                            value.map(Into::into),
                            policy,
                            Duration::from_millis(100),
                            window,
                            cx,
                        )
                    })
                })
                .unwrap()
        }
    }
    #[test]
    fn form_help_motion_uses_measured_height_and_preserves_interruption_continuity() {
        let mut f = Fixture::new(None);
        let policy = MotionPolicy::new(true, false);
        let start = f.sample(Some("Check this value"), policy);
        assert_eq!(start.height, Some(px(0.)));
        assert_eq!(start.opacity, 0.);
        f.cx.executor().advance_clock(Duration::from_millis(50));
        let middle = f.sample(Some("Check this value"), policy);
        assert!((middle.opacity - 0.5).abs() < 0.001);
        assert_eq!(middle.height, Some(px(22.)));
        let leave = f.sample(None, policy);
        assert!(leave.leaving && leave.content.is_some());
        assert_eq!(leave.opacity, middle.opacity);
        f.cx.executor().advance_clock(Duration::from_millis(100));
        let end = f.sample(None, policy);
        assert!(end.content.is_none());
        assert_eq!(end.height, None);
        assert_eq!(end.opacity, 0.);
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx);
        })
        .unwrap();
        f.sample(None, policy);
        assert_eq!(
            f.cx.update_window(f.window.into(), |_, window, cx| window
                .simulate_next_frame(cx))
                .unwrap(),
            0
        );
    }
    #[test]
    fn form_help_motion_initial_and_reduced_states_are_static_without_idle_frames() {
        let mut initial = Fixture::new(Some("Initial help"));
        let sample = initial.sample(Some("Initial help"), MotionPolicy::new(true, false));
        assert_eq!(sample.opacity, 1.);
        assert_eq!(sample.height, None);
        let mut f = Fixture::new(None);
        let shown = f.sample(Some("Help"), MotionPolicy::new(true, true));
        assert_eq!(shown.opacity, 1.);
        assert_eq!(shown.height, None);
        assert!(
            f.sample(None, MotionPolicy::new(true, true))
                .content
                .is_none()
        );
        assert_eq!(
            f.cx.update_window(f.window.into(), |_, window, cx| window
                .simulate_next_frame(cx))
                .unwrap(),
            0
        );
    }
    #[test]
    fn form_help_message_replacement_resizes_from_measured_heights_without_jumps() {
        let mut f = Fixture::new(Some("One line"));
        let policy = MotionPolicy::new(true, false);
        f.sample(Some("One line"), policy);
        f.cx.update_window(f.window.into(), |_, window, cx| {
            f.state
                .update(cx, |state, cx| state.measured(px(22.), window, cx))
        })
        .unwrap();
        assert_eq!(f.sample(Some("Two lines"), policy).height, Some(px(22.)));
        f.cx.update_window(f.window.into(), |_, window, cx| {
            f.state
                .update(cx, |state, cx| state.measured(px(44.), window, cx))
        })
        .unwrap();
        f.cx.executor().advance_clock(Duration::from_millis(50));
        assert_eq!(f.sample(Some("Two lines"), policy).height, Some(px(33.)));
        assert_eq!(f.sample(Some("Three lines"), policy).height, Some(px(33.)));
        f.cx.update_window(f.window.into(), |_, window, cx| {
            f.state
                .update(cx, |state, cx| state.measured(px(66.), window, cx))
        })
        .unwrap();
        f.cx.executor().advance_clock(Duration::from_millis(50));
        assert_eq!(f.sample(Some("Three lines"), policy).height, Some(px(49.5)));
        f.cx.executor().advance_clock(Duration::from_millis(100));
        assert_eq!(f.sample(Some("Three lines"), policy).height, None);
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx);
        })
        .unwrap();
        f.sample(Some("Three lines"), policy);
        assert_eq!(
            f.cx.update_window(f.window.into(), |_, window, cx| window
                .simulate_next_frame(cx))
                .unwrap(),
            0
        );
    }
}
