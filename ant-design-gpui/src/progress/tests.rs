// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigPatch, UiRoot, theme::Color};
use gpui::{
    AppContext, Context, Entity, Render, TestAppContext, WindowHandle, div, prelude::*, px, size,
};
use std::time::Duration;

struct Consumer {
    scope: ConfigScope,
    percent: f64,
    kind: ProgressKind,
    status: ProgressStatus,
    mounted: bool,
    offset: f32,
    steps: Option<usize>,
    color: Color,
    custom_content: Option<std::rc::Rc<std::cell::Cell<usize>>>,
    renders: usize,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        let mut container = div().size_full().overflow_hidden().flex().flex_col();
        container = container.child(div().h(px(self.offset)).flex_shrink_0());
        if self.mounted {
            let mut p = Progress::new("progress")
                .scope(self.scope.clone())
                .percent(self.percent)
                .kind(self.kind)
                .status(self.status)
                .stroke_color(self.color)
                .show_info(false);
            if let Some(count) = self.steps {
                p = p.steps(count);
            }
            if let Some(count) = &self.custom_content {
                let count = count.clone();
                p = p
                    .show_info(true)
                    .size([300., 20.])
                    .percent_position(ProgressPercentPosition {
                        align: ProgressAlign::Center,
                        position: ProgressInfoPosition::Inner,
                    })
                    .format_content(move |_, _, _, _| {
                        count.set(count.get() + 1);
                        div().w(px(80.)).h(px(10.)).bg(Color::WHITE)
                    });
            }
            container = container.child(div().w(px(300.)).child(p));
        }
        container
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    view: Entity<Consumer>,
    scope: ConfigScope,
}
impl Fixture {
    fn new(motion: bool, edit: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut view = None;
        let mut retained_scope = None;
        let window = cx.open_window(size(px(400.), px(300.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(motion)).unwrap();
            let content = cx.new(|_| {
                let mut c = Consumer {
                    scope: scope.clone(),
                    percent: 20.,
                    kind: ProgressKind::Line,
                    status: ProgressStatus::Normal,
                    mounted: true,
                    offset: 0.,
                    steps: None,
                    color: Color::rgb(0x8b19d3),
                    custom_content: None,
                    renders: 0,
                };
                edit(&mut c);
                c
            });
            view = Some(content.clone());
            retained_scope = Some(scope.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let mut f = Self {
            cx,
            window,
            view: view.unwrap(),
            scope: retained_scope.unwrap(),
        };
        f.draw();
        f
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn edit(&mut self, f: impl FnOnce(&mut Consumer)) {
        self.view.update(&mut self.cx, |v, cx| {
            f(v);
            cx.notify();
        });
        self.draw();
    }
    fn patch(&mut self, f: impl FnOnce(&mut ConfigPatch)) {
        self.cx.update(|cx| self.scope.update_patch(cx, f).unwrap());
        self.draw();
    }
    fn frames(&mut self) -> usize {
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap()
    }
    fn advance(&mut self, ms: u64) -> usize {
        self.cx.executor().advance_clock(Duration::from_millis(ms));
        self.cx.run_until_parked();
        let count = self.frames();
        self.draw();
        count
    }
    fn track_width(&mut self) -> f32 {
        self.cx
            .update_window(self.window.into(), |_, w, _| {
                w.rendered_scene()
                    .paths
                    .iter()
                    .find(|p| {
                        p.color.as_solid().is_some_and(|c| {
                            let rgb = gpui::Rgba::from(c);
                            (rgb.r - 139. / 255.).abs() < 0.001
                                && (rgb.g - 25. / 255.).abs() < 0.001
                        })
                    })
                    .map_or(0., |p| p.bounds.size.width.0 / w.scale_factor())
            })
            .unwrap()
    }
}

#[test]
fn settled_controls_have_no_frame_loop() {
    for kind in [
        ProgressKind::Line,
        ProgressKind::Circle,
        ProgressKind::Dashboard,
    ] {
        let mut f = Fixture::new(true, |v| v.kind = kind);
        assert_eq!(f.frames(), 0);
        f.draw();
        assert_eq!(f.frames(), 0);
    }
}
#[test]
fn line_transitions_retarget_from_painted_value_without_owner_timers() {
    let mut f = Fixture::new(true, |_| {});
    assert!((f.track_width() - 60.).abs() < 0.1);
    f.edit(|v| v.percent = 80.);
    assert!((f.track_width() - 60.).abs() < 0.1);
    assert_eq!(f.advance(150), 1);
    let mid = f.track_width();
    assert!(mid > 60. && mid < 240.);
    f.edit(|v| v.percent = 40.);
    assert!((f.track_width() - mid).abs() < 0.1);
    f.advance(300);
    assert!((f.track_width() - 120.).abs() < 0.1);
    assert_eq!(f.frames(), 0);
}
#[test]
fn active_motion_stops_when_clipped_hidden_reduced_or_unmounted() {
    let mut f = Fixture::new(true, |v| v.status = ProgressStatus::Active);
    assert_eq!(f.frames(), 1);
    f.draw();
    f.edit(|v| v.offset = 1000.);
    f.advance(10);
    assert_eq!(f.frames(), 0);
    f.edit(|v| v.offset = 0.);
    assert_eq!(f.frames(), 1);
    f.draw();
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.draw();
    f.advance(10);
    assert_eq!(f.frames(), 0);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert_eq!(f.frames(), 1);
    f.patch(|p| p.motion = Some(false));
    f.advance(10);
    assert_eq!(f.frames(), 0);
    f.patch(|p| p.motion = Some(true));
    assert_eq!(f.frames(), 1);
    f.draw();
    f.edit(|v| v.mounted = false);
    f.advance(10);
    assert_eq!(f.frames(), 0);
}
#[test]
fn changing_motion_policy_settles_progress_immediately() {
    let mut f = Fixture::new(true, |_| {});
    f.edit(|v| v.percent = 80.);
    f.advance(75);
    f.patch(|p| p.motion = Some(false));
    assert!((f.track_width() - 240.).abs() < 0.1);
    f.advance(1);
    assert_eq!(f.frames(), 0);
}
#[test]
fn enormous_step_counts_only_realize_visible_geometry() {
    let mut f = Fixture::new(false, |v| v.steps = Some(10_000_000));
    let count =
        f.cx.update_window(f.window.into(), |_, w, _| w.rendered_scene().paths.len())
            .unwrap();
    assert!(count <= 30, "only visible steps should be painted: {count}");
    assert_eq!(f.frames(), 0);
}
#[test]
fn geometry_cache_reuses_empty_shapes_and_color_only_changes() {
    let mut cache = paint::ShapeCache::default();
    cache.prepare(&[0.], || None, paint::Brush::solid(gpui::black()));
    cache.prepare(
        &[0.],
        || panic!("empty geometry is cached"),
        paint::Brush::solid(gpui::black()),
    );
    assert_eq!(cache.builds, 1);
    cache.prepare(
        &[1.],
        || paint::rounded_rect(100., 8., 4.),
        paint::Brush::solid(gpui::black()),
    );
    cache.prepare(
        &[1.],
        || panic!("color changes keep tessellation"),
        paint::Brush::solid(gpui::white()),
    );
    assert_eq!(cache.builds, 2);
}
#[test]
fn immutable_gradients_sort_stops_and_interpolate_premultiplied_srgb() {
    let g = ProgressGradient::new([
        (1., Color::rgb(0x00ff00)),
        (f32::NAN, Color::BLACK),
        (0., Color::rgb(0xff0000).with_alpha(0.)),
    ]);
    assert_eq!(g.stops().len(), 2);
    assert_eq!(g.stops()[0].0, 0.);
    let c = gpui::Rgba::from(paint::color_at(&g, 0.5, gpui::black()));
    assert!(c.r < 0.001 && (c.g - 1.).abs() < 0.001 && (c.a - 0.5).abs() < 0.001);
}

#[test]
fn reference_status_precedence_and_percent_bounds() {
    assert_eq!(
        geometry::status(&Progress::new("a").percent(100.)),
        ProgressStatus::Success
    );
    assert_eq!(
        geometry::status(&Progress::new("a").percent(100.).success(50.)),
        ProgressStatus::Normal
    );
    assert_eq!(
        geometry::status(&Progress::new("a").percent(50.).success(100.)),
        ProgressStatus::Success
    );
    assert_eq!(
        geometry::status(
            &Progress::new("a")
                .percent(100.)
                .status(ProgressStatus::Normal)
        ),
        ProgressStatus::Normal
    );
    assert_eq!(geometry::percent(f64::NAN), 0.);
    assert_eq!(geometry::percent(-20.), 0.);
    assert_eq!(geometry::percent(120.), 100.);
    assert_eq!(geometry::percent(99.9), 99.9);
}
#[test]
fn reference_step_rounding_and_logical_gaps() {
    assert_eq!(geometry::active_steps(3, 50., None), 2);
    assert_eq!(geometry::active_steps(3, 50., Some(&f64::floor)), 1);
    assert_eq!(geometry::active_steps(10, 100., Some(&|_| f64::NAN)), 0);
    assert_eq!(
        geometry::start_angle(75., ProgressGapPlacement::Start, Direction::Rtl),
        geometry::start_angle(75., ProgressGapPlacement::End, Direction::Ltr)
    );
    assert_eq!(
        geometry::gap(&Progress::new("a").kind(ProgressKind::Dashboard)),
        75.
    );
    assert_eq!(
        geometry::gap(
            &Progress::new("a")
                .kind(ProgressKind::Dashboard)
                .gap_degree(0.)
        ),
        0.
    );
}
#[test]
fn native_geometry_is_finite_at_endpoints() {
    for cap in [
        ProgressLinecap::Round,
        ProgressLinecap::Butt,
        ProgressLinecap::Square,
    ] {
        for sweep in [0., 0.001, 1., 3.5, std::f32::consts::TAU] {
            if let Some(path) = paint::arc(47., 6., -std::f32::consts::FRAC_PI_2, sweep, cap) {
                assert!(
                    path.vertices
                        .iter()
                        .all(|v| f32::from(v.xy_position.x).is_finite()
                            && f32::from(v.xy_position.y).is_finite())
                );
            }
        }
    }
}

#[test]
fn custom_inner_content_is_measured_natively_once_per_render() {
    let calls = std::rc::Rc::new(std::cell::Cell::new(0));
    let mut f = Fixture::new(false, |v| {
        v.percent = 0.;
        v.custom_content = Some(calls.clone());
    });
    assert!((f.track_width() - 88.).abs() < 0.1);
    assert_eq!(calls.get(), f.view.read_with(&f.cx, |v, _| v.renders));
    f.edit(|v| v.percent = 50.);
    assert!((f.track_width() - 150.).abs() < 0.1);
    assert_eq!(calls.get(), f.view.read_with(&f.cx, |v, _| v.renders));
}

#[test]
fn color_only_changes_animate_and_settle_without_geometry_changes() {
    for steps in [None, Some(5)] {
        let mut f = Fixture::new(true, |v| v.steps = steps);
        f.edit(|v| v.color = Color::rgb(0xff0000));
        assert_eq!(f.advance(150), 1);
        assert_eq!(f.advance(151), 1);
        assert_eq!(f.frames(), 0);
        f.edit(|v| v.color = Color::rgb(0x00ff00));
        f.patch(|p| p.motion = Some(false));
        f.advance(1);
        assert_eq!(f.frames(), 0);
    }
}

#[test]
fn scoped_progress_styles_and_tokens_merge_and_invalidate() {
    let mut f = Fixture::new(false, |_| {});
    f.patch(|p| {
        p.progress.styles.track = gpui::StyleRefinement::default().bg(Color::rgb(0x722ed1));
        p.theme
            .get_or_insert_default()
            .components
            .set::<ProgressTheme>(ProgressTokenPatch {
                line_border_radius: Some(3.),
                ..Default::default()
            });
    });
    let child = f.cx.update(|cx| {
        let child = f
            .scope
            .child(
                ConfigPatch {
                    progress: ProgressConfigPatch {
                        styles: ProgressStyles {
                            indicator: gpui::StyleRefinement::default().text_color(Color::BLACK),
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        let snapshot = child.snapshot(cx);
        assert_eq!(
            snapshot.progress.styles.track.background,
            f.scope.snapshot(cx).progress.styles.track.background
        );
        assert!(snapshot.progress.styles.indicator.text.is_some());
        assert_eq!(
            snapshot
                .theme
                .component::<ProgressTheme>()
                .line_border_radius,
            3.
        );
        f.scope
            .update_patch(cx, |p| {
                p.theme
                    .get_or_insert_default()
                    .components
                    .set::<ProgressTheme>(ProgressTokenPatch {
                        line_border_radius: Some(7.),
                        ..Default::default()
                    })
            })
            .unwrap();
        child
    });
    f.cx.run_until_parked();
    f.cx.update(|cx| {
        assert_eq!(
            child
                .snapshot(cx)
                .theme
                .component::<ProgressTheme>()
                .line_border_radius,
            7.
        )
    });
}

#[test]
fn native_motion_matches_sampled_reference_css() {
    let fixture: serde_json::Value =
        serde_json::from_str(include_str!("../../fixtures/progress/motion-samples.json")).unwrap();
    let theme = crate::theme::ThemeConfig::default().resolve();
    let mut line = Fixture::new(true, |_| {});
    line.advance(130);
    line.edit(|v| v.percent = 80.);
    let samples = fixture["probes"][0]["samples"].as_array().unwrap();
    let mut previous = 0;
    for sample in samples {
        let ms = sample["ms"].as_u64().unwrap();
        line.advance(ms - previous);
        previous = ms;
        assert!(
            (line.track_width() as f64 - sample["track"]["width"].as_f64().unwrap()).abs() < 0.51,
            "native pixel rounding at {ms} ms"
        );
    }
    for sample in fixture["probes"][1]["samples"].as_array().unwrap() {
        let ms = sample["ms"].as_f64().unwrap();
        let value = 20.
            + 60.
                * crate::behavior::evaluate_cubic_bezier(
                    crate::theme::CubicBezier::new(0.25, 0.1, 0.25, 1.),
                    ms / 300.,
                );
        let dash = (100. - value) / 100. * std::f64::consts::TAU * 47. + 3.;
        let expected = sample["dash"]
            .as_str()
            .unwrap()
            .trim_end_matches("px")
            .parse::<f64>()
            .unwrap();
        assert!((dash - expected).abs() < 0.002);
    }
    for sample in fixture["probes"][3]["samples"].as_array().unwrap() {
        let ms = sample["ms"].as_u64().unwrap();
        let (left, scale, alpha) = super::view::active_values(
            Duration::from_millis(ms),
            theme.tokens.motion_ease_out_quint,
        );
        let matrix: Vec<f64> = sample["overlay"]["transform"]
            .as_str()
            .unwrap()
            .trim_start_matches("matrix(")
            .trim_end_matches(')')
            .split(',')
            .map(|s| s.trim().parse().unwrap())
            .collect();
        let expected_alpha = sample["overlay"]["opacity"]
            .as_str()
            .unwrap()
            .parse::<f64>()
            .unwrap();
        assert!((scale - matrix[0]).abs() < 0.00001);
        assert!((left - (matrix[4] / 150. + (1. - matrix[0]) / 2.)).abs() < 0.00001);
        assert!((alpha - expected_alpha).abs() < 0.00001);
    }
}

#[test]
fn circular_semantic_opacity_overrides_zero_segment_visibility() {
    let mut f = Fixture::new(false, |v| {
        v.kind = ProgressKind::Circle;
        v.percent = 40.;
    });
    let path_count = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, w, _| w.rendered_scene().paths.len())
            .unwrap()
    };
    assert_eq!(path_count(&mut f), 2);
    f.patch(|p| p.progress.styles.track.opacity = Some(0.4));
    // The source's semantic opacity overrides the SVG zero-percent attribute;
    // the zero success segment contributes its minimum rounded mark.
    assert_eq!(path_count(&mut f), 3);
    f.patch(|p| p.progress.styles.track.opacity = None);
    assert_eq!(path_count(&mut f), 2);
}
