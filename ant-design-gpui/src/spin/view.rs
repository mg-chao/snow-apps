// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    Spin, SpinStyle, SpinStyleContext, SpinTheme,
    element::{Activity, Fullscreen},
    geometry,
    state::{Sample, SpinState},
};
use crate::{
    ConfigScope, Direction,
    behavior::{Accessibility, MotionPolicy},
    icons::{IconPalette, IconRenderOptions, icon},
};
use gpui::{
    App, Bounds, Entity, IntoElement, Pixels, Refineable, RenderOnce, Role, StyleRefinement,
    Styled, Window, canvas, div, point, prelude::*, px, radians, relative, size,
};
use std::{cell::Cell, rc::Rc};

fn builtin(
    metrics: SpinStyle,
    sample: Sample,
    direction: Direction,
    motion: bool,
    style: StyleRefinement,
    state: Entity<SpinState>,
    rem_size: Pixels,
) -> gpui::AnyElement {
    let holder = metrics.dot_size as f32;
    let slot_size = style
        .text
        .font_size
        .map_or(px(holder), |value| value.to_pixels(rem_size));
    let dot_style = style;
    let dot_state = state.clone();
    let dots = canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            let color = window.text_style().color;
            let mix = sample.dots as f32;
            let scale = 0.3 + 0.7 * mix;
            if mix > 0. {
                let actual = holder;
                for dot in geometry::dots(
                    actual,
                    metrics.dot_item_size as f32,
                    sample.elapsed,
                    direction,
                    motion,
                ) {
                    let diameter = px(dot.diameter * scale);
                    let center = bounds.origin
                        + point(px(holder / 2.), px(holder / 2.))
                        + point(
                            px((dot.x - actual / 2.) * scale),
                            px((dot.y - actual / 2.) * scale),
                        );
                    dot_state.update(cx, |state, _| {
                        state.dot_cache.paint(
                            center,
                            diameter,
                            color.opacity(dot.opacity * mix),
                            window,
                        )
                    });
                }
            }
        },
    )
    .size(px(holder))
    .flex_shrink_0();
    let mut dots = div().id("dots").size(slot_size).flex_shrink_0().child(dots);
    dots.style().refine(&dot_style);
    let mut holder_element = div().relative().flex_shrink_0().child(dots);
    if sample.ring > 0. {
        holder_element = holder_element.child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, cx| {
                    let bounds = Bounds::new(
                        point(bounds.center().x - px(holder / 2.), bounds.origin.y),
                        size(px(holder), px(holder)),
                    );
                    let color = window.text_style().color.opacity(sample.ring as f32);
                    state.update(cx, |state, _| {
                        state.ring_cache.paint(
                            bounds,
                            sample.progress,
                            color,
                            metrics.track_color.opacity(sample.ring as f32),
                            window,
                        )
                    });
                },
            )
            .absolute()
            .top(px(0.))
            .left(px(0.))
            .size_full(),
        );
    }
    holder_element.into_any_element()
}

impl RenderOnce for Spin {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            SpinState::new(scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<SpinTheme>();
        let metrics =
            SpinStyle::from_tokens(&tokens, self.size.unwrap_or(config.size), self.fullscreen);
        let motion =
            MotionPolicy::new(config.motion && tokens.globals().motion, cx.reduce_motion());
        let indicator = self
            .indicator
            .clone()
            .or_else(|| config.spin.indicator.clone());
        let has_custom = self.indicator_content.is_some() || indicator.is_some();
        let sample = state.update(cx, |state, cx| {
            state.set_custom(has_custom);
            state.sample(&self, metrics, motion, cx)
        });
        let loading = state.read(cx).loading;
        let custom_animation = self.indicator_spin.unwrap_or_else(|| {
            self.indicator_content.is_none()
                && indicator
                    .as_ref()
                    .is_some_and(|indicator| indicator.animated)
        });
        let context = SpinStyleContext {
            size: self.size.unwrap_or(config.size),
            spinning: loading,
            fullscreen: self.fullscreen,
            has_children: self.has_children,
            direction: config.direction,
            percent: sample.percent,
            motion: motion.allows_motion(),
            rotation: if motion.allows_motion() {
                (sample.elapsed.as_secs_f64().fract() * std::f64::consts::TAU) as f32
            } else {
                0.
            },
        };
        let mut styles = config.spin.styles.clone();
        if let Some(resolver) = &config.spin.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        let nested = self.has_children || self.fullscreen;
        if !nested {
            styles.root.refine(&styles.section);
        }
        styles.root.refine(&self.style);
        let mut section = div()
            .id(if nested {
                "section".into()
            } else {
                self.id.clone()
            })
            .relative()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(metrics.gap as f32))
            .text_color(metrics.color);
        if nested {
            section.style().refine(&styles.section);
        }
        let indicator_visible = Rc::new(Cell::new(false));
        let mut animate = !has_custom && sample.indicator_transitioning;
        if loading {
            animate |= motion.allows_motion()
                && if has_custom {
                    custom_animation
                } else {
                    sample.dots > 0. && metrics.dot_size > 0.
                };
            let indicator_content = if let Some(content) = self.indicator_content.take() {
                content
            } else if let Some(indicator) = indicator {
                if let Some(reference) = indicator.reference() {
                    let icon_size = styles
                        .indicator
                        .text
                        .font_size
                        .map_or(px(metrics.dot_size as f32), |value| {
                            value.to_pixels(window.rem_size())
                        });
                    icon(reference.clone())
                        .inherit_color()
                        .render_options(IconRenderOptions {
                            size: size(icon_size, icon_size),
                            colors: IconPalette::from_theme(tokens.globals()),
                            rotation: radians(if custom_animation {
                                context.rotation
                            } else {
                                0.
                            }),
                            ..Default::default()
                        })
                        .into_any_element()
                } else {
                    indicator
                        .render_content(&context, window, cx)
                        .expect("native indicator factory")
                }
            } else {
                builtin(
                    metrics,
                    sample,
                    config.direction,
                    motion.allows_motion(),
                    styles.indicator.clone(),
                    state.clone(),
                    window.rem_size(),
                )
            };
            let indicator_content = if has_custom {
                let mut holder = div()
                    .id("indicator")
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(indicator_content);
                holder.style().refine(&styles.indicator);
                holder.into_any_element()
            } else {
                indicator_content
            };
            let mut semantics = Accessibility::new(Role::ProgressIndicator);
            semantics.label = self.accessibility.label.clone();
            semantics.description = self.accessibility.description.clone();
            let mut indicator_holder = div().id("progress").child(indicator_content);
            if !matches!(self.percent, Some(super::SpinPercent::Auto))
                && let Some(percent) = sample.percent
            {
                indicator_holder = indicator_holder
                    .aria_numeric_value(percent.clamp(0., 100.))
                    .aria_min_numeric_value(0.)
                    .aria_max_numeric_value(100.);
            }
            section = section.child(Activity {
                element: semantics.apply(indicator_holder).into_element(),
                state: state.clone(),
                animation: false,
                pointer_events: true,
                indicator_visible: indicator_visible.clone(),
                surface_animation: false,
                probe: true,
            });
            if let Some(description) = self.description.take() {
                let mut label = div()
                    .id("description")
                    .text_size(px(metrics.font_size as f32))
                    .line_height(relative(1.))
                    .when(self.fullscreen, |label| {
                        label.text_color(metrics.description_color)
                    })
                    .child(description.into_element(config.direction));
                label.style().refine(&styles.description);
                section = section.child(label);
            }
        }
        let mut section = Some(section);
        let mut root = if nested {
            div().id(self.id.clone()).relative()
        } else {
            section.take().unwrap()
        };
        root = root
            .font(tokens.font().clone())
            .text_size(px(tokens.globals().font_size as f32))
            .line_height(relative(tokens.globals().line_height as f32));
        if nested {
            if self.has_children {
                let mut content = div()
                    .id("container")
                    .relative()
                    .children(std::mem::take(&mut self.children));
                let mut container_style = styles.container.clone();
                let container_opacity =
                    container_style
                        .opacity
                        .take()
                        .map_or(1. - 0.5 * sample.dim, |value| {
                            if value.is_finite() {
                                f64::from(value.clamp(0., 1.))
                            } else {
                                1.
                            }
                        });
                content.style().refine(&container_style);
                // GPUI applies ancestor alpha per primitive. A single wash is
                // equivalent to CSS's isolated opacity group on the standard
                // uniform container backdrop, without rendering child snapshots.
                let opacity = 1. - container_opacity * (1. - 0.4 * sample.dim);
                if opacity > 0. {
                    content = content.child(
                        div()
                            .absolute()
                            .inset_0()
                            .bg(metrics.container_color.opacity(opacity as f32))
                            .when(loading, |mask| mask.occlude()),
                    );
                }
                root = root.child(content);
            }
            if loading {
                root = root.child(
                    div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .when(self.has_children, |overlay| overlay.occlude())
                        .child(section.take().unwrap()),
                );
            }
        }
        if self.fullscreen {
            root = root
                .w(window.viewport_size().width)
                .h(window.viewport_size().height)
                .bg(metrics.mask_color)
                .opacity(sample.mask as f32)
                .when(loading, |mask| mask.occlude());
        }
        root.style().refine(&styles.root);
        let mut accessibility = self.accessibility;
        accessibility.role = Some(Role::Group);
        accessibility.busy = loading;
        let element = Activity {
            element: accessibility.apply(root).into_element(),
            state,
            animation: animate,
            pointer_events: !self.fullscreen || loading,
            indicator_visible,
            surface_animation: sample.surface_transitioning,
            probe: false,
        };
        if self.fullscreen {
            Fullscreen {
                id: self.id,
                child: element.into_any_element(),
                z_index: metrics.z_index,
            }
            .into_any_element()
        } else {
            element.into_any_element()
        }
    }
}
