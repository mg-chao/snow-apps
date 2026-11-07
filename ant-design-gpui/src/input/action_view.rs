// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    AnyElement, App, Context, ElementId, Entity, FocusHandle, IntoElement, MouseButton,
    Subscription, Window, div, prelude::*, px, size,
};

use super::InputState;
use crate::{
    ResolvedConfig,
    behavior::{Accessibility, ActivationGuard, Availability, FocusRing, MotionPolicy},
    icons::{IconName, IconRenderOptions, icon},
};

use super::motion::{CSS_EASE, ColorMotion};

pub(super) struct ActionTheme<'a> {
    pub config: &'a ResolvedConfig,
    pub tokens: &'a crate::theme::AliasTokens,
    pub font_size: f64,
}

#[derive(Clone, Copy)]
pub(super) enum InputAction {
    Clear,
    Visibility(bool),
}
impl InputAction {
    fn clear(self) -> bool {
        matches!(self, Self::Clear)
    }
    fn activate(self, state: &Entity<InputState>, window: &mut Window, cx: &mut App) {
        state.update(cx, |state, cx| match self {
            Self::Clear => {
                state.focus(window, cx);
                state.clear(window, cx);
            }
            Self::Visibility(_) => state.toggle_password_visibility(window, cx),
        });
    }
}

struct ActionState {
    editor: Entity<InputState>,
    clear: bool,
    focus: FocusHandle,
    guard: ActivationGuard,
    hovered: bool,
    pressed: bool,
    availability_revision: u64,
    epoch: std::time::Instant,
    color: ColorMotion,
    frame_pending: bool,
    _editor: Subscription,
    _focus: Vec<Subscription>,
}
impl ActionState {
    fn new(
        editor: Entity<InputState>,
        clear: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let focus = crate::behavior::focus_handle(cx).tab_stop(!clear);
        let guard = ActivationGuard::default();
        let blur = cx.on_blur(&focus, window, |this, _, cx| {
            this.guard.cancel();
            this.pressed = false;
            cx.notify();
        });
        let focused = cx.on_focus(&focus, window, |_, _, cx| cx.notify());
        let active = cx.observe_window_activation(window, |this, _, cx| {
            this.guard.cancel();
            this.pressed = false;
            this.hovered = false;
            cx.notify();
        });
        let visible = cx.observe_window_visibility(window, |this, _, _, cx| {
            this.guard.cancel();
            this.pressed = false;
            this.hovered = false;
            cx.notify();
        });
        Self {
            _editor: Self::observe(&editor, window, cx),
            availability_revision: editor.read(cx).availability_revision,
            editor,
            clear,
            focus,
            guard,
            hovered: false,
            pressed: false,
            epoch: cx.background_executor().now(),
            color: ColorMotion::default(),
            frame_pending: false,
            _focus: vec![blur, focused, active, visible],
        }
    }
    fn observe(
        editor: &Entity<InputState>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        cx.observe_in(editor, window, |this, _, window, cx| {
            this.sync(window, cx);
            cx.notify();
        })
    }
    fn sync(&mut self, window: &mut Window, cx: &mut App) {
        let editor = self.editor.read(cx);
        // Observe transitions even when an owner disables and reenables before
        // the queued entity observer runs or a replacement tree is rendered.
        if self.availability_revision != editor.availability_revision {
            self.availability_revision = editor.availability_revision;
            self.guard.cancel();
            self.pressed = false;
        }
        let disabled = editor.is_disabled()
            || (self.clear && (editor.is_read_only() || editor.value().is_empty()));
        self.guard.update(
            Availability {
                disabled,
                busy: false,
            },
            &self.focus,
            window,
            cx,
        );
        if disabled {
            self.pressed = false;
            self.hovered = false;
        }
    }
    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if !self.frame_pending {
            self.frame_pending = true;
            let state = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = state.update(cx, |state, cx| {
                    state.frame_pending = false;
                    cx.notify();
                });
            });
        }
    }
}

pub(super) fn input_action(
    id: ElementId,
    action: InputAction,
    editor: &Entity<InputState>,
    theme: ActionTheme<'_>,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let tokens = theme.tokens;
    let config = theme.config;
    let (label, name) = match action {
        InputAction::Clear => (
            config.locale.common.clear_input.clone(),
            IconName::CloseCircleFilled,
        ),
        InputAction::Visibility(true) => (
            config.locale.common.hide_password.clone(),
            IconName::EyeOutlined,
        ),
        InputAction::Visibility(false) => (
            config.locale.common.show_password.clone(),
            IconName::EyeInvisibleOutlined,
        ),
    };
    let key = if action.clear() {
        "clear"
    } else {
        "visibility"
    };
    let state = window.use_keyed_state((id, key), cx, |window, cx| {
        ActionState::new(editor.clone(), action.clear(), window, cx)
    });
    let (color, focus, guard) = state.update(cx, |state, cx| {
        if state.editor != *editor {
            state.guard.cancel();
            state.pressed = false;
            state.hovered = false;
            state.editor = editor.clone();
            state._editor = ActionState::observe(editor, window, cx);
        }
        state.sync(window, cx);
        let focused = state.focus.is_focused(window) && window.last_input_was_keyboard();
        let target = if !state.guard.availability().can_activate() {
            if action.clear() {
                tokens.color_text_disabled
            } else {
                tokens.color_icon
            }
        } else if action.clear() {
            if state.pressed && state.hovered {
                tokens.color_text
            } else if state.hovered || focused {
                tokens.color_icon
            } else {
                tokens.color_text_quaternary
            }
        } else if state.hovered {
            tokens.color_icon_hover
        } else {
            tokens.color_icon
        };
        let mut motion = MotionPolicy::new(config.motion && tokens.motion, cx.reduce_motion());
        motion.enabled &= window.is_visible() && window.is_window_active();
        let now = cx
            .background_executor()
            .now()
            .saturating_duration_since(state.epoch);
        let (color, animating) = state.color.resolve(
            target.to_hsla(),
            now,
            if action.clear() && focused {
                // Ant's focus-outline rule replaces the transition property
                // list, so the focused clear color changes immediately.
                std::time::Duration::ZERO
            } else {
                tokens.motion_duration_slow
            },
            CSS_EASE,
            motion,
        );
        if animating {
            state.request_frame(window, cx);
        }
        (color, state.focus.clone(), state.guard.clone())
    });
    let disabled = !guard.availability().can_activate();
    let icon_size = if action.clear() {
        tokens.font_size_icon
    } else {
        theme.font_size
    } as f32;
    let hovered = state.clone();
    let pressed = state.clone();
    let released = state.clone();
    let outside = state.clone();
    let mut element = div()
        .id(key)
        .key_context("AntDesignInputAction")
        .flex()
        .items_center()
        .justify_center()
        .flex_shrink_0()
        .when(!disabled, |element| {
            element.cursor_pointer().track_focus(&focus)
        })
        .on_hover(move |hover, _, cx| {
            hovered.update(cx, |state, cx| {
                state.hovered = *hover;
                cx.notify();
            })
        })
        .on_mouse_down(MouseButton::Left, move |_, window, cx| {
            pressed.update(cx, |state, cx| {
                state.pressed = state.guard.availability().can_activate();
                cx.notify();
            });
            window.prevent_default();
            cx.stop_propagation();
        })
        .on_mouse_up(MouseButton::Left, move |_, window, cx| {
            released.update(cx, |state, cx| {
                state.pressed = false;
                cx.notify();
            });
            window.prevent_default();
            cx.stop_propagation();
        })
        .on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
            outside.update(cx, |state, cx| {
                state.pressed = false;
                cx.notify();
            })
        })
        .child(icon(name).render_options(IconRenderOptions {
            size: size(px(icon_size), px(icon_size)),
            scale_factor: window.scale_factor(),
            colors: crate::icons::IconPalette {
                text: color.into(),
                ..config.icon_palette
            },
            ..Default::default()
        }));
    if let InputAction::Visibility(visible) = action {
        element = element.aria_toggled(if visible {
            gpui::accesskit::Toggled::True
        } else {
            gpui::accesskit::Toggled::False
        });
    }
    let target = editor.clone();
    let element = guard.install(element, move |_, window, cx| {
        action.activate(&target, window, cx);
        cx.stop_propagation();
    });
    let mut semantics = Accessibility::new(gpui::Role::Button).label(label);
    semantics.disabled = disabled;
    let element = semantics.apply(element);
    if action.clear() {
        FocusRing::new(
            tokens.color_primary_border.to_hsla(),
            px(tokens.line_width_focus as f32),
        )
        .radius(px(tokens.border_radius_sm as f32))
        .apply(element, &focus)
        .into_any_element()
    } else {
        element.into_any_element()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::Hsla;
    use std::time::Duration;
    #[test]
    fn action_color_motion_preserves_premultiplied_hue_reverses_and_cancels() {
        let enabled = MotionPolicy::new(true, false);
        let disabled = MotionPolicy::new(false, false);
        let red: Hsla = gpui::Rgba {
            r: 1.,
            g: 0.,
            b: 0.,
            a: 1.,
        }
        .into();
        let clear = Hsla::transparent_black();
        let mut motion = ColorMotion::default();
        assert!(
            !motion
                .resolve(
                    red,
                    Duration::ZERO,
                    Duration::from_millis(300),
                    CSS_EASE,
                    enabled
                )
                .1
        );
        motion.resolve(
            clear,
            Duration::ZERO,
            Duration::from_millis(300),
            CSS_EASE,
            enabled,
        );
        let now = Duration::from_millis(100);
        let before = motion.sample(now);
        let rgba = gpui::Rgba::from(before);
        assert!((rgba.r - 1.).abs() < 0.001 && rgba.g < 0.001 && rgba.a > 0. && rgba.a < 1.);
        let (reversed, animating) =
            motion.resolve(red, now, Duration::from_millis(300), CSS_EASE, enabled);
        assert!(animating);
        assert!((gpui::Rgba::from(reversed).a - rgba.a).abs() < 0.001);
        let (settled, animating) =
            motion.resolve(red, now, Duration::from_millis(300), CSS_EASE, disabled);
        assert!(!animating);
        assert_eq!(settled, red);
        assert!(
            !motion
                .resolve(
                    red,
                    Duration::from_secs(1),
                    Duration::from_millis(300),
                    CSS_EASE,
                    enabled
                )
                .1
        );
    }
}
