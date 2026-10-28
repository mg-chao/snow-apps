// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    App, IntoElement, Refineable, RenderOnce, Role, Style, Styled, Window, canvas, div, prelude::*,
    px, relative,
};

use super::{
    Alert, AlertStyle, AlertStyleContext, AlertTheme, AlertType,
    element::surface,
    state::{AlertState, Phase},
};
use crate::{
    ConfigScope, Direction,
    behavior::{Accessibility, FocusRing, MotionPolicy},
    icons::{IconName, IconPalette, IconRef, IconRenderOptions, IconRenderer},
    theme::LineType,
};

impl RenderOnce for Alert {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            AlertState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |s, cx| s.update(&self, scope.clone(), window, cx));
        if state.read(cx).phase == Phase::Closed {
            return div().id(self.id).hidden().into_any_element();
        }
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<AlertTheme>();
        let g = tokens.globals();
        let kind = self.kind.unwrap_or(if self.banner {
            AlertType::Warning
        } else {
            AlertType::Info
        });
        let variant = self.variant.unwrap_or(config.alert.variant);
        let show_icon = self.show_icon.unwrap_or(self.banner);
        let closable = state.read(cx).closable(cx);
        let has_description = self.description.is_some();
        let context = AlertStyleContext {
            kind,
            variant,
            banner: self.banner,
            show_icon,
            closable,
            has_description,
            direction: config.direction,
        };
        let metrics = AlertStyle::from_tokens(&tokens, kind, variant, has_description, self.banner);
        let policy = MotionPolicy::new(config.motion && g.motion, cx.reduce_motion());
        let mut styles = config.alert.styles.clone();
        if let Some(resolver) = &config.alert.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        styles.root.refine(&self.style);

        let (ratio, hover) = state.update(cx, |s, cx| {
            s.metrics = Some(metrics);
            let now = s.now(cx);
            if s.phase == Phase::Open {
                s.hover.retarget(
                    if s.close_hovered && !self.close_disabled {
                        1.
                    } else {
                        0.
                    },
                    now,
                    metrics.hover_duration,
                    policy,
                );
            }
            let hover = s.hover.sample(now);
            if !hover.finished && s.phase == Phase::Open {
                s.request_frame(window, cx);
            }
            (s.sample_close(policy, window, cx), hover.value as f32)
        });
        if state.read(cx).phase == Phase::Closing && ratio <= 0. {
            let (callback, focus) = state.update(cx, |s, _| s.complete());
            if let Some(focus) = focus
                && focus.is_focused(window)
            {
                window.focus_next(cx);
                if focus.is_focused(window) {
                    window.blur(cx);
                }
            }
            if let Some(callback) = callback {
                let weak = state.downgrade();
                window.defer(cx, move |window, cx| {
                    if weak.upgrade().is_some() {
                        callback(window, cx);
                    }
                });
            }
            return div().id(self.id).hidden().into_any_element();
        }
        let phase = state.read(cx).phase;
        let live = self.effective_live(phase == Phase::Closing);
        let (close_focus, close_guard) = state
            .read(cx)
            .input
            .as_ref()
            .map(|input| (Some(input.focus.clone()), Some(input.guard.clone())))
            .unwrap_or_default();
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .w_full()
            .min_w(px(0.))
            .flex_shrink_0()
            .items_center()
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .text_color(metrics.description_color)
            .bg(metrics.background)
            .border_color(metrics.border)
            .border(px(metrics.line_width as f32))
            .rounded(px(metrics.radius as f32))
            .pt(px(metrics.padding.top as f32))
            .pr(px(metrics.padding.right as f32))
            .pb(px(metrics.padding.bottom as f32))
            .pl(px(metrics.padding.left as f32))
            .when(has_description, |d| d.items_start())
            .when(config.direction == Direction::Rtl, |d| {
                d.flex_row_reverse().text_right()
            })
            .when(g.line_type == LineType::Dashed, |d| d.border_dashed());
        if self.banner {
            root = root.mb(px(0.));
        }
        root.style().refine(&styles.root);
        // Banner structural rules win over semantic root refinements, as upstream !important does.
        if self.banner {
            root = root.border_0();
        }
        let root_font_size = root
            .style()
            .text
            .font_size
            .map_or(px(metrics.font_size as f32), |v| {
                v.to_pixels(window.rem_size())
            });
        if let Some(callback) = self.on_click {
            root = root.on_click(move |event, window, cx| callback(event, window, cx));
        }
        if let Some(callback) = self.on_hover {
            root = root.on_hover(move |hovered, window, cx| callback(*hovered, window, cx));
        }

        if show_icon {
            let mut slot = div()
                .id("icon")
                .flex()
                .flex_none()
                .line_height(px(0.))
                .text_color(metrics.icon_color)
                .when(config.direction == Direction::Ltr, |d| {
                    d.mr(px(metrics.icon_gap as f32))
                })
                .when(config.direction == Direction::Rtl, |d| {
                    d.ml(px(metrics.icon_gap as f32))
                });
            if has_description {
                slot = slot.text_size(px(metrics.icon_size as f32));
            }
            slot.style().refine(&styles.icon);
            let icon_size = slot.style().text.font_size.map_or(
                if has_description {
                    px(metrics.icon_size as f32)
                } else {
                    root_font_size
                },
                |v| v.to_pixels(window.rem_size()),
            );
            let content = if let Some(content) = self.icon_content {
                content
            } else {
                let scoped = match kind {
                    AlertType::Success => &config.alert.success_icon,
                    AlertType::Info => &config.alert.info_icon,
                    AlertType::Warning => &config.alert.warning_icon,
                    AlertType::Error => &config.alert.error_icon,
                };
                let builtin = match kind {
                    AlertType::Success => IconName::CheckCircleFilled,
                    AlertType::Info => IconName::InfoCircleFilled,
                    AlertType::Warning => IconName::ExclamationCircleFilled,
                    AlertType::Error => IconName::CloseCircleFilled,
                };
                let scoped_content = if self.icon.is_none() {
                    scoped
                        .as_ref()
                        .and_then(|icon| icon.render_content(&context, window, cx))
                } else {
                    None
                };
                scoped_content.unwrap_or_else(|| {
                    foreground_icon(
                        self.icon
                            .or_else(|| scoped.as_ref().and_then(|i| i.reference().cloned()))
                            .unwrap_or_else(|| builtin.into()),
                        tokens.icon_palette(),
                    )
                    .size(icon_size)
                    .into_any_element()
                })
            };
            root = root.child(slot.child(content));
        }

        let mut section = div().id("section").flex().flex_col().flex_1().min_w(px(0.));
        section.style().refine(&styles.section);
        let title_line_height = section
            .style()
            .text
            .line_height
            .or(root.style().text.line_height);
        if let Some(title) = self.title.take() {
            let mut slot = div().id("title").text_color(metrics.title_color);
            if has_description {
                slot = slot
                    .text_size(px(metrics.title_font_size as f32))
                    .mb(px(metrics.title_gap as f32));
            }
            // Preserve inherited unitless leading when applying the larger title font.
            if let Some(line_height) = title_line_height {
                slot = slot.line_height(line_height);
            }
            slot.style().refine(&styles.title);
            section = section.child(slot.child(title.into_element(config.direction)));
        }
        if let Some(description) = self.description.take() {
            let mut slot = div()
                .id("description")
                .text_size(px(metrics.font_size as f32))
                .line_height(relative(g.line_height as f32))
                .text_color(metrics.description_color);
            slot.style().refine(&styles.description);
            section = section.child(slot.child(description.into_element(config.direction)));
        }
        root = root.child(section);
        if let Some(action) = self.action {
            let mut slot = div()
                .id("actions")
                .when(config.direction == Direction::Ltr, |d| {
                    d.ml(px(metrics.action_gap as f32))
                })
                .when(config.direction == Direction::Rtl, |d| {
                    d.mr(px(metrics.action_gap as f32))
                });
            slot.style().refine(&styles.actions);
            root = root.child(slot.child(action));
        }

        if closable {
            let focus = close_focus.expect("closable Alert retains close input");
            let guard = close_guard.expect("closable Alert retains close input");
            let scoped_content = if self.close_icon.is_none() && self.close_content.is_none() {
                config
                    .alert
                    .close_icon
                    .as_ref()
                    .and_then(|icon| icon.render_content(&context, window, cx))
            } else {
                None
            };
            let custom_content = self.close_content.or(scoped_content);
            let reference = self
                .close_icon
                .or_else(|| {
                    config
                        .alert
                        .close_icon
                        .as_ref()
                        .and_then(|i| i.reference().cloned())
                })
                .unwrap_or_else(|| IconName::CloseOutlined.into());
            let default_icon = custom_content.is_none()
                && reference.key() == IconName::CloseOutlined.descriptor().key;
            let close_color = if self.close_disabled {
                metrics.close_disabled_color
            } else if default_icon {
                mix(metrics.close_color, metrics.close_hover_color, hover)
            } else {
                crate::theme::Color::BLACK.to_hsla()
            };
            let mut close = div()
                .id("close")
                .role(Role::Button)
                .relative()
                .flex()
                .flex_none()
                .items_center()
                .justify_center()
                .text_size(px(metrics.close_size as f32))
                .line_height(px(metrics.close_size as f32))
                .text_color(close_color)
                .when(config.direction == Direction::Ltr, |d| {
                    d.ml(px(metrics.close_gap as f32))
                })
                .when(config.direction == Direction::Rtl, |d| {
                    d.mr(px(metrics.close_gap as f32))
                })
                .when(
                    !self.close_disabled && (phase == Phase::Open || focus.is_focused(window)),
                    |d| d.track_focus(&focus),
                );
            close.style().refine(&styles.close);
            let mut close_style = Style::default();
            close_style.refine(close.style());
            let close_corners = close_style.corner_radii.to_pixels(window.rem_size());
            let size = close
                .style()
                .text
                .font_size
                .map_or(px(metrics.close_size as f32), |v| {
                    v.to_pixels(window.rem_size())
                });
            let content = custom_content.unwrap_or_else(|| {
                foreground_icon(reference, tokens.icon_palette())
                    .size(size)
                    .into_any_element()
            });
            close = close
                .child(content)
                .when(self.close_disabled, |d| d.cursor_not_allowed())
                .when(!self.close_disabled, |d| d.cursor_pointer());
            let weak = state.downgrade();
            close = close.on_hover(move |hovered, _, cx| {
                let _ = weak.update(cx, |s, cx| {
                    if s.close_hovered != *hovered {
                        s.close_hovered = *hovered;
                        cx.notify();
                    }
                });
            });
            let weak = state.downgrade();
            let on_close = self.on_close;
            let close = guard.install(close, move |activation, window, cx| {
                let accepted = weak
                    .update(cx, |s, cx| s.begin_close(window, cx))
                    .unwrap_or(false);
                if accepted && let Some(callback) = &on_close {
                    callback(&activation, window, cx);
                }
            });
            let close = Accessibility {
                label: Some(
                    self.close_label
                        .or_else(|| config.alert.close_label.clone())
                        .unwrap_or_else(|| config.locale.common.close.clone()),
                ),
                disabled: self.close_disabled || phase != Phase::Open,
                automation_id: self.close_automation_id,
                ..Default::default()
            }
            .apply(close);
            root = root.child(
                FocusRing::new(metrics.focus_color, px(metrics.focus_width as f32))
                    .radius(px(0.))
                    .apply(close, &focus)
                    .corners(close_corners),
            );
        }
        let open_style = root.style().clone();
        if phase == Phase::Closing
            && let Some(geometry) = state.read(cx).collapse
        {
            root = root
                // Upstream measures offsetHeight, which rounds the open border box.
                .max_h(geometry.bounds.size.height.round() * ratio)
                // Description and inline padding win the upstream leave-active cascade.
                .pt(geometry.padding_top
                    * if has_description || styles.root.padding.top.is_some() {
                        1.
                    } else {
                        ratio
                    })
                .pb(geometry.padding_bottom
                    * if has_description || styles.root.padding.bottom.is_some() {
                        1.
                    } else {
                        ratio
                    })
                .mb(geometry.margin_bottom * ratio)
                .opacity(geometry.opacity * ratio)
                .overflow_hidden();
        }
        surface(
            self.accessibility.apply(root),
            (phase == Phase::Open).then(|| state.read(cx).open_geometry.clone()),
            open_style,
            live,
        )
        .into_any_element()
    }
}

fn mix(from: gpui::Hsla, to: gpui::Hsla, progress: f32) -> gpui::Hsla {
    let a = from.to_rgb();
    let b = to.to_rgb();
    gpui::Rgba {
        r: a.r + (b.r - a.r) * progress,
        g: a.g + (b.g - a.g) * progress,
        b: a.b + (b.b - a.b) * progress,
        a: a.a + (b.a - a.a) * progress,
    }
    .into()
}

fn foreground_icon(reference: IconRef, palette: IconPalette) -> gpui::Canvas<()> {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            if cx.try_global::<IconRenderer>().is_none() {
                cx.set_global(IconRenderer::default());
            }
            let options = IconRenderOptions {
                size: bounds.size,
                scale_factor: window.scale_factor(),
                colors: IconPalette {
                    text: window.text_style().color.into(),
                    ..palette
                },
                ..Default::default()
            };
            cx.update_global::<IconRenderer, _>(|renderer, cx| {
                let _ = renderer.paint_with_scale(
                    bounds,
                    &reference,
                    &options,
                    reference.ant_design_viewbox_scale(),
                    window,
                    cx,
                );
            });
        },
    )
    .flex_shrink_0()
}
