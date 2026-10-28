// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    paint::{Brush, ShapeCache},
    state::{ProgressState, Sample},
    *,
};
use crate::{
    behavior::{MotionPolicy, evaluate_cubic_bezier},
    icons::{IconName, icon},
};
use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, Entity, GlobalElementId, InspectorElementId,
    LayoutId, Pixels, RenderOnce, Role, Window, accesskit, canvas, div, point, prelude::*, px,
    relative,
};
use std::f32::consts::TAU;

fn brush(
    stroke: Option<&ProgressStroke>,
    fallback: Hsla,
    kind: ProgressKind,
    direction: Direction,
    gap: f32,
) -> Brush {
    match stroke {
        Some(ProgressStroke::Solid(color)) => Brush::solid(*color),
        Some(ProgressStroke::Steps(colors)) => {
            Brush::solid(colors.first().copied().unwrap_or(fallback))
        }
        Some(ProgressStroke::Gradient(gradient)) if !gradient.stops().is_empty() => {
            if kind == ProgressKind::Line {
                Brush::Linear(
                    gradient.clone(),
                    gradient.angle.unwrap_or(if direction == Direction::Ltr {
                        90.
                    } else {
                        270.
                    }),
                )
            } else {
                Brush::Conic(
                    gradient.clone(),
                    if gap > 0. {
                        (90. + gap / 2.).to_radians()
                    } else {
                        -std::f32::consts::FRAC_PI_2
                    },
                    (360. - gap).to_radians(),
                )
            }
        }
        _ => Brush::solid(fallback),
    }
}
fn styled_brush(style: &StyleRefinement, fallback: Brush) -> Brush {
    style
        .background
        .as_ref()
        .and_then(gpui::Fill::color)
        .map_or(fallback, Brush::Native)
}
fn opacity(style: &StyleRefinement) -> f32 {
    style.opacity.unwrap_or(1.).clamp(0., 1.)
}
fn radius(style: &StyleRefinement, fallback: f32, window: &Window) -> f32 {
    style
        .corner_radii
        .top_left
        .map_or(fallback, |v| f32::from(v.to_pixels(window.rem_size())))
}
fn cap_value(cap: ProgressLinecap) -> f32 {
    match cap {
        ProgressLinecap::Round => 0.,
        ProgressLinecap::Butt => 1.,
        ProgressLinecap::Square => 2.,
    }
}

#[derive(Clone)]
struct CirclePaint {
    diameter: f32,
    stroke: f32,
    gap: f32,
    start: f32,
    cap: ProgressLinecap,
    track: Brush,
    rail: Brush,
    success: Brush,
    success_present: bool,
    steps: Option<ProgressSteps>,
    active: usize,
    track_opacity: f32,
    explicit_track_opacity: bool,
    rail_opacity: f32,
}
impl CirclePaint {
    fn paint(
        &self,
        bounds: Bounds<Pixels>,
        sample: Sample,
        state: &mut ProgressState,
        window: &mut Window,
    ) {
        let diameter = self
            .diameter
            .min(f32::from(bounds.size.width))
            .min(f32::from(bounds.size.height));
        let width = diameter * sample.stroke / 100.;
        // SVG radius changes immediately; stroke thickness has its own delayed transition.
        let radius = diameter * (100. - self.stroke) / 200.;
        let span = (360. - self.gap).to_radians();
        let center = bounds.center();
        if let Some(steps) = self.steps {
            if radius <= 0. || steps.count == 0 {
                return;
            }
            let sweep =
                (span / steps.count as f32 - steps.gap.max(0.) / 100. * diameter / radius).max(0.);
            // No positive path remains when gaps consume the available segment.
            if sweep <= 0. {
                return;
            }
            let resolution = (TAU * radius * window.scale_factor()).ceil().max(1.) as usize;
            let count = steps.count.min(resolution);
            let active =
                ((self.active as f64 / steps.count as f64) * count as f64).round() as usize;
            let effective_sweep = if steps.count > resolution {
                // Aggregate subpixel cells across the entire arc, never a prefix.
                span / count as f32
            } else {
                sweep
            };
            let density = if steps.count > resolution {
                sweep / (span / steps.count as f32)
            } else {
                1.
            };
            state.steps.resize_with(count, ShapeCache::default);
            for (index, cache) in state.steps.iter_mut().enumerate() {
                let start = self.start + span * index as f32 / count as f32;
                state.paint_animation |= cache.prepare_motion(
                    &[diameter, width, radius, start, effective_sweep],
                    || paint::arc(radius, width, start, effective_sweep, ProgressLinecap::Butt),
                    if index < active {
                        self.track.clone()
                    } else {
                        self.rail.clone()
                    },
                    sample.paint_motion,
                );
            }
            // Consecutive cells with the same sampled color share one mask.
            // Membership changes concatenate cached vertices, preserving per-cell
            // color transitions without a GPU draw for every settled step.
            let mut groups = Vec::new();
            let mut begin = 0;
            for end in 1..=count {
                if end == count || state.steps[end].brush() != state.steps[begin].brush() {
                    groups.push((begin, end));
                    begin = end;
                }
            }
            state
                .step_groups
                .resize_with(groups.len(), ShapeCache::default);
            for (cache, (begin, end)) in state.step_groups.iter_mut().zip(groups) {
                cache.prepare(
                    &[
                        diameter,
                        width,
                        radius,
                        self.start,
                        span,
                        steps.gap,
                        count as f32,
                        begin as f32,
                        end as f32,
                    ],
                    || paint::compound(state.steps[begin..end].iter().filter_map(ShapeCache::path)),
                    state.steps[begin].brush().unwrap().clone(),
                );
                cache.paint(center, self.track_opacity * density, window);
            }
            return;
        }
        state.steps.clear();
        state.step_groups.clear();
        state.paint_animation |= state.rail.prepare_motion(
            &[
                diameter,
                width,
                radius,
                self.start,
                span,
                cap_value(self.cap),
            ],
            || paint::arc(radius, width, self.start, span, self.cap),
            self.rail.clone(),
            sample.paint_motion,
        );
        state.rail.paint(center, self.rail_opacity, window);
        let success = if self.success_present {
            sample.success
        } else {
            0.
        };
        let remaining = (sample.percent - success).clamp(0., 100.);
        let draw = |cache: &mut ShapeCache,
                    offset: f64,
                    percent: f64,
                    fill: Brush,
                    alpha: f32,
                    window: &mut Window| {
            if (percent <= 0. && !self.explicit_track_opacity) || radius <= 0. {
                return false;
            }
            let start = self.start + span * (offset / 100.) as f32;
            let mut sweep = span * (percent / 100.) as f32;
            if self.cap == ProgressLinecap::Round && percent != 100. {
                sweep = (sweep - width / 2. / radius).max(0.01 * diameter / 100. / radius);
            }
            let animated = cache.prepare_motion(
                &[diameter, width, radius, start, sweep, cap_value(self.cap)],
                || paint::arc(radius, width, start, sweep, self.cap),
                fill,
                sample.paint_motion,
            );
            // The reference's semantic opacity overrides the SVG zero-percent
            // opacity attribute, including its minimum rounded-cap mark.
            cache.paint(
                center,
                self.track_opacity
                    * if self.explicit_track_opacity {
                        1.
                    } else {
                        alpha
                    },
                window,
            );
            animated
        };
        // rc-progress reverses the stack, so success caps cover the remaining segment.
        state.paint_animation |= draw(
            &mut state.track,
            success,
            remaining,
            self.track.clone(),
            sample.track_opacity,
            window,
        );
        state.paint_animation |= draw(
            &mut state.success_track,
            0.,
            success,
            self.success.clone(),
            sample.success_opacity,
            window,
        );
    }
}

fn line_surface(
    state: Entity<ProgressState>,
    fill: Brush,
    style: &StyleRefinement,
    cap: ProgressLinecap,
    fallback_radius: f32,
    success: bool,
    motion: paint::PaintMotion,
) -> gpui::AnyElement {
    // The native track container applies opacity to fill, label and active sweep.
    let alpha = 1.;
    let corner = style.corner_radii.top_left;
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            let w = f32::from(bounds.size.width);
            let h = f32::from(bounds.size.height);
            let r = if cap == ProgressLinecap::Round {
                corner.map_or(fallback_radius, |v| {
                    f32::from(v.to_pixels(window.rem_size()))
                })
            } else {
                0.
            };
            state.update(cx, |state, _| {
                let cache = if success {
                    &mut state.success_track
                } else {
                    &mut state.track
                };
                state.paint_animation |= cache.prepare_motion(
                    &[w, h, r],
                    || paint::rounded_rect(w, h, r),
                    fill.clone(),
                    motion,
                );
                cache.paint(bounds.origin, alpha, window);
            });
        },
    )
    .absolute()
    .inset_0()
    .into_any_element()
}

pub(super) fn active_values(
    elapsed: std::time::Duration,
    curve: crate::theme::CubicBezier,
) -> (f64, f64, f64) {
    let phase = (elapsed.as_secs_f64() / 2.4).fract();
    let (scale, alpha) = if phase <= 0.2 {
        (0., 0.1 + 0.4 * evaluate_cubic_bezier(curve, phase / 0.2))
    } else {
        let eased = evaluate_cubic_bezier(curve, (phase - 0.2) / 0.8);
        (eased, 0.5 * (1. - eased))
    };
    ((scale - 1.) / 2., scale, alpha)
}
fn active_overlay(
    sample: Sample,
    radius: f32,
    color: Hsla,
    curve: crate::theme::CubicBezier,
    state: Entity<ProgressState>,
    success: bool,
) -> gpui::AnyElement {
    let (left, scale, alpha) = active_values(sample.elapsed, curve);
    div()
        .absolute()
        .inset_0()
        .overflow_hidden()
        .rounded(px(radius))
        .child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, cx| {
                    if scale <= 0. || alpha <= 0. {
                        return;
                    }
                    let state = state.read(cx);
                    let cache = if success {
                        &state.success_track
                    } else {
                        &state.track
                    };
                    if let Some(path) = cache.path() {
                        // CSS transforms scale the cap horizontally as well as the fill.
                        // Reuse the track mesh rather than tessellating on each frame.
                        let mut path = path.clone();
                        path.bounds.origin.x *= scale as f32;
                        path.bounds.size.width *= scale as f32;
                        for vertex in &mut path.vertices {
                            vertex.xy_position.x *= scale as f32;
                        }
                        let origin = bounds.origin + point(bounds.size.width * left as f32, px(0.));
                        window.paint_path(
                            paint::translate(path, origin),
                            color.opacity(alpha as f32),
                        );
                    }
                },
            )
            .absolute()
            .inset_0(),
        )
        .into_any_element()
}

impl RenderOnce for Progress {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<ProgressTheme>();
        let g = tokens.globals();
        let direction = config.direction;
        let policy = MotionPolicy::new(config.motion && g.motion, cx.reduce_motion());
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            ProgressState::new(scope.clone(), window, cx)
        });
        let sample = state.update(cx, |state, cx| {
            state.sample(&self, scope.clone(), &tokens, policy, window, cx)
        });
        let status = geometry::status(&self);
        let percent = geometry::percent(self.percent);
        let success_percent = self.success.map(|s| geometry::percent(s.percent));
        let context = ProgressStyleContext {
            kind: self.kind,
            status,
            percent,
            success_percent,
            size: self.size.clone(),
            steps: self.steps,
            show_info: self.show_info,
            percent_position: self.percent_position,
            direction,
            motion: policy.allows_motion(),
        };
        let mut styles = config.progress.styles.clone();
        if let Some(resolver) = &config.progress.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        if let Some(local) = &self.styles {
            styles.refine(local);
        }
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        if let Some(root) = &self.style {
            styles.root.refine(root);
        }
        let circular = self.kind != ProgressKind::Line;
        let (body_width, height) = if !circular && self.steps.is_some() {
            geometry::step_size(&self.size)
        } else {
            geometry::dimensions(&self.size, self.kind)
        };
        let small = self.size == ProgressSize::Small;
        let font_size = if circular {
            (height * 0.15 + 6.) * tokens.circle_text_font_size as f32
        } else if small && self.steps.is_none() {
            g.font_size_sm as f32
        } else {
            g.font_size as f32
        };
        let base_color = match if !circular && self.steps.is_some() {
            ProgressStatus::Normal
        } else {
            status
        } {
            ProgressStatus::Success => g.color_success,
            ProgressStatus::Exception => g.color_error,
            _ => tokens.default_color,
        }
        .to_hsla();
        let text_color = match status {
            ProgressStatus::Success => g.color_success,
            ProgressStatus::Exception => g.color_error,
            _ if circular => tokens.circle_text_color,
            _ => g.color_text,
        }
        .to_hsla();
        let gap = geometry::gap(&self);
        let track = styled_brush(
            &styles.track,
            brush(self.stroke.as_ref(), base_color, self.kind, direction, gap),
        );
        let rail = styled_brush(
            &styles.rail,
            Brush::solid(self.rail_color.unwrap_or(tokens.remaining_color.into())),
        );
        let success_color = self.success.and_then(|s| s.color).unwrap_or_else(|| {
            if circular {
                crate::theme::Color::rgb(0x52c41a).into()
            } else {
                g.color_success.into()
            }
        });
        let success_brush = styled_brush(&styles.track, Brush::solid(success_color));
        let pure_inner = !circular
            && self.steps.is_none()
            && self.percent_position.position == ProgressInfoPosition::Inner;
        let formatted = self
            .format
            .as_ref()
            .map(|f| f(percent, success_percent.unwrap_or(0.)));
        let text: SharedString = formatted
            .clone()
            .unwrap_or_else(|| format!("{percent}%").into());
        let use_icon = !pure_inner
            && self.format.is_none()
            && self.format_content.is_none()
            && matches!(status, ProgressStatus::Success | ProgressStatus::Exception);
        let icon_name = match (circular, status) {
            (false, ProgressStatus::Success) => IconName::CheckCircleFilled,
            (false, _) => IconName::CloseCircleFilled,
            (true, ProgressStatus::Success) => IconName::CheckOutlined,
            (true, _) => IconName::CloseOutlined,
        };
        let indicator_color = if pure_inner {
            let color = match self.stroke.as_ref() {
                Some(ProgressStroke::Solid(c)) => Some(*c),
                Some(ProgressStroke::Gradient(g)) => g.stops().first().map(|s| s.1),
                Some(ProgressStroke::Steps(c)) => c.first().copied(),
                None => None,
            };
            let bright = color.is_some_and(|color| {
                let c = gpui::Rgba::from(color);
                (c.r * 299. + c.g * 587. + c.b * 114.) / 1000. > 128. / 255.
            });
            if bright {
                crate::theme::Color::BLACK.with_alpha(0.45).into()
            } else {
                g.color_white.into()
            }
        } else {
            text_color
        };
        let format_content = self.format_content.clone();
        let label_style = styles.indicator.clone();
        let icon_size = if circular {
            font_size * tokens.circle_icon_font_size as f32
        } else {
            font_size
        };
        let make_label = move |window: &mut Window, cx: &mut App| {
            let mut label = div()
                .id("indicator")
                .flex()
                .items_center()
                .justify_center()
                .flex_shrink_0()
                .text_size(px(font_size))
                .line_height(relative(1.))
                .whitespace_nowrap()
                .text_color(indicator_color);
            let content = if let Some(format) = &format_content {
                format(percent, success_percent.unwrap_or(0.), window, cx)
            } else if use_icon {
                icon(icon_name)
                    .inherit_color()
                    .size(px(icon_size))
                    .into_any_element()
            } else {
                crate::behavior::text::normal_paragraph_text(text.clone(), direction)
            };
            label = label.child(content);
            label.style().refine(&label_style);
            label
        };
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .items_center()
            .min_w(px(0.))
            .when(direction == Direction::Rtl, |d| d.flex_row_reverse())
            .font(config.font.clone())
            .text_size(px(font_size))
            .line_height(relative(1.))
            .text_color(text_color)
            .aria_numeric_value(percent)
            .aria_min_numeric_value(0.)
            .aria_max_numeric_value(100.);
        let mut body;
        if circular {
            let stroke = geometry::length(
                self.stroke_width
                    .unwrap_or((300. / height.max(0.001)).max(6.)),
                6.,
            )
            .min(100.);
            let has_gradient = matches!(self.stroke, Some(ProgressStroke::Gradient(_)));
            let cap = if has_gradient {
                ProgressLinecap::Butt
            } else {
                self.linecap
            };
            let active = self.steps.map_or(0, |steps| {
                geometry::active_steps(
                    steps.count,
                    (percent - success_percent.unwrap_or(0.)).max(0.),
                    None,
                )
            });
            let painter = CirclePaint {
                diameter: height,
                stroke,
                gap,
                start: geometry::start_angle(gap, self.gap_placement, direction),
                cap,
                track: if self.steps.is_some() && has_gradient {
                    styled_brush(
                        &styles.track,
                        Brush::solid(crate::theme::Color::TRANSPARENT.into()),
                    )
                } else {
                    track.clone()
                },
                rail: if self.steps.is_some() {
                    styled_brush(
                        &styles.track,
                        self.rail_color.map_or_else(
                            || {
                                if has_gradient {
                                    Brush::solid(crate::theme::Color::TRANSPARENT.into())
                                } else {
                                    track
                                }
                            },
                            Brush::solid,
                        ),
                    )
                } else {
                    rail
                },
                success: success_brush,
                success_present: self.success.is_some(),
                steps: self.steps,
                active,
                track_opacity: opacity(&styles.track),
                explicit_track_opacity: styles.track.opacity.is_some(),
                rail_opacity: opacity(&styles.rail),
            };
            let paint_state = state.clone();
            body = div()
                .id("body")
                .relative()
                .size(px(height))
                .flex_shrink_0()
                .child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, cx| {
                            paint_state
                                .update(cx, |state, _| painter.paint(bounds, sample, state, window))
                        },
                    )
                    .absolute()
                    .inset_0(),
                );
            if self.show_info && height > 20. {
                body = body.child(
                    div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .child(make_label(window, cx)),
                );
            }
        } else if let Some(steps) = self.steps {
            let step_width = body_width
                .and_then(|w| match w {
                    gpui::Length::Definite(v) => {
                        Some(f32::from(v.to_pixels(px(14.).into(), window.rem_size())))
                    }
                    _ => None,
                })
                .unwrap_or(14.)
                .max(g.margin_xxs as f32 / 2.);
            let step_gap = g.margin_xxs as f32 / 2.;
            let total = ((step_width + step_gap) * steps.count as f32 - step_gap).max(0.);
            let active = geometry::active_steps(steps.count, percent, self.rounding.as_deref());
            let colors = match &self.stroke {
                Some(ProgressStroke::Steps(c)) => Some(c.clone()),
                _ => None,
            };
            let default_track = styled_brush(
                &styles.track,
                match &self.stroke {
                    Some(ProgressStroke::Gradient(_)) => Brush::solid(tokens.default_color.into()),
                    _ => track,
                },
            );
            let paint_state = state.clone();
            let track_alpha = opacity(&styles.track);
            let step_style = styles.track.clone();
            let bars = canvas(
                |_, _, _| (),
                move |bounds, (), window, cx| {
                    let viewport = bounds.intersect(&window.content_mask().bounds);
                    if viewport.is_empty() {
                        return;
                    }
                    let first = ((f32::from(viewport.origin.x - bounds.origin.x)
                        / (step_width + step_gap))
                        .floor()
                        .max(0.) as usize)
                        .min(steps.count);
                    let last = ((f32::from(viewport.right() - bounds.origin.x)
                        / (step_width + step_gap))
                        .ceil()
                        .max(0.) as usize)
                        .min(steps.count);
                    paint_state.update(cx, |state, _| {
                        state.steps.resize_with(last - first, ShapeCache::default);
                        for (offset, cache) in state.steps.iter_mut().enumerate() {
                            let physical = first + offset;
                            let index = if direction == Direction::Rtl {
                                steps.count - 1 - physical
                            } else {
                                physical
                            };
                            let fill = if index < active {
                                colors
                                    .as_ref()
                                    .and_then(|c| c.get(index))
                                    .copied()
                                    .map_or_else(|| default_track.clone(), Brush::solid)
                            } else {
                                styled_brush(&step_style, rail.clone())
                            };
                            state.paint_animation |= cache.prepare_motion(
                                &[step_width, height],
                                || paint::rounded_rect(step_width, height, 0.),
                                fill,
                                sample.paint_motion,
                            );
                            cache.paint(
                                bounds.origin
                                    + point(px(physical as f32 * (step_width + step_gap)), px(0.)),
                                track_alpha,
                                window,
                            );
                        }
                    });
                },
            )
            .w(px(total))
            .h(px(height))
            .flex_shrink_0();
            body = div()
                .id("body")
                .flex()
                .items_center()
                .gap(px(step_gap))
                .when(direction == Direction::Rtl, |d| d.flex_row_reverse())
                .child(bars);
            if self.show_info {
                body = body.child(
                    div()
                        .when(direction == Direction::Ltr, |d| {
                            d.ml(px(g.margin_xs as f32))
                        })
                        .when(direction == Direction::Rtl, |d| {
                            d.mr(px(g.margin_xs as f32))
                        })
                        .child(make_label(window, cx)),
                );
            }
        } else {
            let fallback_radius = tokens.line_border_radius as f32;
            let r = if self.linecap == ProgressLinecap::Round {
                radius(&styles.rail, fallback_radius, window)
            } else {
                0.
            };
            let mut rail_element = div()
                .id("rail")
                .relative()
                .flex_1()
                .min_w(px(0.))
                .h(px(height))
                .overflow_hidden()
                .rounded(px(r));
            if let Brush::Native(fill) = rail {
                rail_element = rail_element.bg(fill);
            }
            rail_element.style().refine(&styles.rail);
            let mut track_element = div()
                .id("track")
                .absolute()
                .top_0()
                .bottom_0()
                .w(relative((sample.percent / 100.) as f32))
                .when(direction == Direction::Ltr, |d| d.left_0())
                .when(direction == Direction::Rtl, |d| d.right_0())
                .child(line_surface(
                    state.clone(),
                    track,
                    &styles.track,
                    self.linecap,
                    fallback_radius,
                    false,
                    sample.paint_motion,
                ));
            let active = status == ProgressStatus::Active && policy.allows_motion();
            if active {
                track_element = track_element.child(active_overlay(
                    sample,
                    r,
                    g.color_bg_container.into(),
                    g.motion_ease_out_quint,
                    state.clone(),
                    false,
                ));
            }
            // Apply native geometry styles to the track container. Paint-specific
            // background/opacity are handled by the cached surface, exactly once.
            let mut track_layout = styles.track.clone();
            track_layout.background = None;
            track_element.style().refine(&track_layout);
            if pure_inner && self.show_info {
                let layout = track_element.style().clone();
                let mut surface_layout = StyleRefinement::default().relative().size_full();
                surface_layout.opacity = styles.track.opacity;
                // The surface's absolute children fill the measured track.
                *track_element.style() = std::mem::take(&mut surface_layout);
                rail_element = rail_element.child(super::element::InnerTrack {
                    label: make_label(window, cx)
                        .px(px(g.padding_xxs as f32))
                        .opacity(opacity(&styles.track))
                        .into_any_element(),
                    surface: track_element.into_any_element(),
                    style: layout,
                    align: self.percent_position.align,
                    direction,
                });
            } else {
                rail_element = rail_element.child(track_element);
            }
            if self.success.is_some() {
                let mut success = div()
                    .id("success")
                    .absolute()
                    .top_0()
                    .bottom_0()
                    .w(relative((sample.success / 100.) as f32))
                    .when(direction == Direction::Ltr, |d| d.left_0())
                    .when(direction == Direction::Rtl, |d| d.right_0())
                    .child(line_surface(
                        state.clone(),
                        success_brush,
                        &styles.track,
                        self.linecap,
                        fallback_radius,
                        true,
                        sample.paint_motion,
                    ));
                if active {
                    success = success.child(active_overlay(
                        sample,
                        r,
                        g.color_bg_container.into(),
                        g.motion_ease_out_quint,
                        state.clone(),
                        true,
                    ));
                }
                success.style().refine(&track_layout);
                rail_element = rail_element.child(success);
            }
            body = div()
                .id("body")
                .flex()
                .items_center()
                .min_w(px(0.))
                .w_full()
                .gap(px(g.margin_xs as f32));
            if let Some(width) = body_width {
                body = body.w(width);
            }
            let bottom = self.show_info
                && !pure_inner
                && self.percent_position.align == ProgressAlign::Center;
            if bottom {
                body = body.flex_col().gap(px(g.margin_xxs as f32));
                rail_element = rail_element.flex_none().w_full();
            }
            if self.show_info && !pure_inner && self.percent_position.align == ProgressAlign::Start
            {
                body = body.child(make_label(window, cx)).child(rail_element);
            } else {
                body = body.child(rail_element);
                if self.show_info && !pure_inner {
                    body = body.child(make_label(window, cx));
                }
            }
            if !bottom && direction == Direction::Rtl {
                body = body.flex_row_reverse();
            }
            root = root.w_full();
        }
        body.style().refine(&styles.body);
        let body = if circular && height <= 20. && self.show_info {
            crate::Tooltip::new("progress-tooltip")
                .scope(scope)
                .title_content(move |_, window, cx| make_label(window, cx))
                .child(body)
                .into_any_element()
        } else {
            body.into_any_element()
        };
        root = root.child(body);
        root.style().refine(&styles.root);
        self.accessibility.role = Some(Role::ProgressIndicator);
        self.accessibility.read_only = true;
        self.accessibility.apply(Activity {
            element: root.into_element(),
            state,
            animate: sample.animate,
        })
    }
}

/// Scheduling follows the final clip, after native layout and painting.
struct Activity<E> {
    element: E,
    state: Entity<ProgressState>,
    animate: bool,
}
impl<E: Element> IntoElement for Activity<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl<E: Element> Element for Activity<E> {
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
        let visible =
            window.is_visible() && !bounds.intersect(&window.content_mask().bounds).is_empty();
        self.state.update(cx, |state, cx| {
            state.schedule(visible, self.animate || state.paint_animation, window, cx)
        });
    }
}
