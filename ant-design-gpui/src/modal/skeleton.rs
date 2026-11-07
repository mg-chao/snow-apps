// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::ModalState;
use crate::{
    behavior::evaluate_cubic_bezier,
    theme::{Color, CubicBezier},
};
use gpui::{
    AnyElement, App, Bounds, Corners, Entity, IntoElement, Window, canvas, div, fill,
    linear_color_stop, linear_gradient, point, prelude::*, px, relative, size,
};

pub(super) fn render(state: &Entity<ModalState>, window: &Window, cx: &App) -> AnyElement {
    let state = state.read(cx);
    let config = state.scope.snapshot(cx);
    let g = &config.theme.tokens;
    let phase = if window.is_visible() && config.motion_policy(cx).allows_motion() {
        evaluate_cubic_bezier(
            CubicBezier::new(0.25, 0.1, 0.25, 1.),
            (state.elapsed(cx).as_secs_f64() / 1.4).fract(),
        ) as f32
    } else {
        0.
    };
    let from = g.color_fill_content;
    let to = g.color_fill;
    let radius = px(g.border_radius_sm as f32);
    let header_margin =
        if state.options.title.is_some() && state.options.kind == super::ModalKind::Standard {
            config
                .theme
                .component::<super::ModalTheme>()
                .header_margin_bottom
        } else {
            0.
        };
    div()
        .id("loading")
        .flex()
        .flex_col()
        .gap(px(g.margin as f32))
        .mt(px((g.margin.max(header_margin) - header_margin) as f32))
        .mb(px((g.margin + g.font_size) as f32))
        .children((0..4).map(|i| {
            canvas(
                |_, _, _| (),
                move |bounds, (), w, _| {
                    // CSS background-size:400%; background-position travels 100% -> 0%.
                    // Split only at visible stops, so the native two-stop gradients reproduce
                    // the three-stop reference without a bitmap, texture upload, or per-row timer.
                    let origin = -3. * (1. - phase);
                    let mut stops = [
                        0.,
                        (origin + 1.).clamp(0., 1.),
                        (origin + 1.48).clamp(0., 1.),
                        (origin + 2.52).clamp(0., 1.),
                        1.,
                    ];
                    stops.sort_by(f32::total_cmp);
                    for pair in stops.windows(2) {
                        let [left, right] = [pair[0], pair[1]];
                        if right <= left {
                            continue;
                        }
                        let color = |x: f32| {
                            let x = (x - origin) / 4.;
                            let t = if x < 0.37 {
                                ((x - 0.25) / 0.12).clamp(0., 1.)
                            } else {
                                ((0.63 - x) / 0.26).clamp(0., 1.)
                            } as f64;
                            let mix = |a: u8, b: u8| {
                                (f64::from(a) + (f64::from(b) - f64::from(a)) * t).round() as u8
                            };
                            Color {
                                r: mix(from.r, to.r),
                                g: mix(from.g, to.g),
                                b: mix(from.b, to.b),
                                a: from.a + (to.a - from.a) * t,
                            }
                        };
                        let rect = Bounds::new(
                            point(bounds.left() + bounds.size.width * left, bounds.top()),
                            size(bounds.size.width * (right - left), bounds.size.height),
                        );
                        let l = if left == 0. { radius } else { px(0.) };
                        let r = if right == 1. { radius } else { px(0.) };
                        let left_color = color(left);
                        let right_color = color(right);
                        let background = if left_color == right_color {
                            gpui::solid_background(left_color)
                        } else {
                            linear_gradient(
                                90.,
                                linear_color_stop(left_color, 0.),
                                linear_color_stop(right_color, 1.),
                            )
                        };
                        w.paint_quad(fill(rect, background).corner_radii(Corners {
                            top_left: l,
                            bottom_left: l,
                            top_right: r,
                            bottom_right: r,
                        }));
                    }
                },
            )
            .h(px((g.control_height / 2.) as f32))
            .w(relative(if i == 3 { 0.61 } else { 1. }))
        }))
        .into_any_element()
}
