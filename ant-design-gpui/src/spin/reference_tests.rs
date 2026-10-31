// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent values from actual upstream markup, CSS, animations, and hooks.

use std::{collections::BTreeMap, sync::OnceLock, time::Duration};

use gpui::{
    AppContext, Context, Entity, Hsla, IntoElement, Render, TestAppContext, Window, WindowHandle,
    div, px, size,
};
use serde::Deserialize;
use serde_json::Value;

use super::{
    Spin, SpinPercent, SpinStyle, SpinTheme, SpinTokenPatch, geometry,
    state::{Sample, SpinState},
};
use crate::{
    ConfigScope, Direction, Size,
    behavior::MotionPolicy,
    theme::{AlgorithmChain, ResolvedTheme, ThemeConfig},
};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Reference {
    component_defaults: BTreeMap<String, ThemeDefaults>,
    cases: Vec<ReferenceCase>,
    animation_cases: Vec<AnimationCase>,
    behavior: Behavior,
    transitions: Vec<BrowserTransition>,
}

#[derive(Deserialize)]
struct ThemeDefaults {
    global: BTreeMap<String, Value>,
    defaults: DefaultTokens,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct DefaultTokens {
    content_height: f64,
    dot_size: f64,
    #[serde(rename = "dotSizeSM")]
    dot_size_sm: f64,
    #[serde(rename = "dotSizeLG")]
    dot_size_lg: f64,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    results: Vec<BrowserSpin>,
}

#[derive(Deserialize)]
struct AnimationCase {
    theme: String,
    direction: String,
    time: u64,
    results: Vec<BrowserSpin>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Options {
    size: Option<String>,
    percent: Option<f64>,
    indicator: Option<String>,
    spinning: Option<bool>,
    direction: Option<String>,
    #[serde(default)]
    nested: bool,
    #[serde(default)]
    token_case: bool,
    #[serde(default)]
    semantic: bool,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserPart {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
    color: String,
    background: String,
    opacity: f64,
    font_size: f64,
    line_height: String,
    gap: String,
    transform: String,
    layout_width: Option<f64>,
    layout_height: Option<f64>,
    pointer_events: String,
}

#[derive(Deserialize)]
struct BrowserMask {
    background: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserProgress {
    radius: String,
    stroke_dasharray: String,
    value: f64,
    track: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserSpin {
    index: usize,
    options: Options,
    spin: BrowserPart,
    section: Option<BrowserPart>,
    holder: Option<BrowserPart>,
    progress_holder: Option<BrowserPart>,
    description: Option<BrowserPart>,
    content: Option<BrowserPart>,
    mask: Option<BrowserMask>,
    dots: Vec<BrowserPart>,
    progress: Option<BrowserProgress>,
    holder_size: f64,
    busy: String,
    live: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct TraceStep {
    label: String,
    time: u64,
    busy: Option<String>,
    progress: Option<f64>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Behavior {
    delay: Vec<TraceStep>,
    #[serde(rename = "auto")]
    automatic: Vec<TraceStep>,
    stopped: TraceStep,
    restarted: TraceStep,
    restarted_first_tick: TraceStep,
}

#[derive(Deserialize)]
struct TransitionProperty {
    duration: f64,
    easing: String,
}

#[derive(Deserialize)]
struct BrowserTransition {
    from: f64,
    to: f64,
    properties: Vec<TransitionProperty>,
    samples: Vec<TransitionSample>,
}

#[derive(Deserialize)]
struct TransitionSample {
    time: u64,
    #[serde(flatten)]
    spin: BrowserSpin,
}

fn reference() -> &'static Reference {
    static REFERENCE: OnceLock<Reference> = OnceLock::new();
    REFERENCE.get_or_init(|| {
        serde_json::from_str(include_str!("../../fixtures/spin/reference.json"))
            .expect("actual upstream Spin browser measurements")
    })
}

fn config(name: &str) -> ThemeConfig {
    ThemeConfig {
        algorithms: Some(match name {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            unknown => panic!("unknown reference theme {unknown}"),
        }),
        ..Default::default()
    }
}

fn tokens_theme(config: &ThemeConfig) -> ResolvedTheme {
    let mut config = config.clone();
    // These are the developer inputs in generate_spin_reference.mjs, not native defaults.
    config.components.set::<SpinTheme>(SpinTokenPatch {
        content_height: Some(120.),
        dot_size: Some(28.),
        dot_size_sm: Some(18.),
        dot_size_lg: Some(44.),
    });
    config.resolve()
}

fn size_option(options: &Options) -> Size {
    match options.size.as_deref() {
        Some("small") => Size::Small,
        None | Some("medium") => Size::Medium,
        Some("large") => Size::Large,
        unknown => panic!("unknown reference size {unknown:?}"),
    }
}

fn direction(value: Option<&str>) -> Direction {
    match value {
        None | Some("ltr") => Direction::Ltr,
        Some("rtl") => Direction::Rtl,
        unknown => panic!("unknown reference direction {unknown:?}"),
    }
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() <= tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn pixels(value: &str) -> f64 {
    value
        .trim()
        .trim_end_matches("px")
        .parse()
        .expect("CSS length")
}

fn seconds(value: &str) -> f64 {
    value.trim_end_matches('s').parse().expect("CSS duration")
}

fn color(native: Hsla, browser: &str, context: &str) {
    let channels: Vec<f64> = browser
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().expect("CSS color channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4), "{context}: {browser}");
    let native: gpui::Rgba = native.into();
    for (native, browser) in [native.r, native.g, native.b].into_iter().zip(&channels) {
        near(f64::from(native), browser / 255., 0.0001, context);
    }
    near(
        f64::from(native.a),
        channels.get(3).copied().unwrap_or(1.),
        0.0021,
        context,
    );
}

fn transform_scale(value: &str) -> f64 {
    if value == "none" {
        return 1.;
    }
    let components: Vec<f64> = value
        .trim_start_matches("matrix(")
        .trim_end_matches(')')
        .split(',')
        .map(|value| value.trim().parse().expect("CSS matrix component"))
        .collect();
    assert_eq!(components.len(), 6);
    components[0].hypot(components[1])
}

fn compare_dots(browser: &BrowserSpin, metrics: SpinStyle, elapsed: Duration, context: &str) {
    let Some(holder) = &browser.holder else {
        assert!(browser.dots.is_empty());
        return;
    };
    let dots = geometry::dots(
        metrics.dot_size as f32,
        metrics.dot_item_size as f32,
        elapsed,
        direction(browser.options.direction.as_deref()),
        true,
    );
    assert_eq!(browser.dots.len(), dots.len(), "{context}");
    let holder_scale = transform_scale(&holder.transform);
    for (index, (native, browser)) in dots.into_iter().zip(&browser.dots).enumerate() {
        let context = format!("{context} dot {index}");
        near(
            f64::from(native.opacity),
            browser.opacity,
            0.000001,
            &context,
        );
        near(
            metrics.dot_item_size,
            browser.layout_width.unwrap(),
            0.0001,
            &context,
        );
        near(
            f64::from(native.diameter),
            browser.layout_width.unwrap() * transform_scale(&browser.transform),
            0.0001,
            &context,
        );
        // DOM bounds enclose the rotated square; its center is also the painted circle center.
        let expected_x = holder.x
            + holder.width / 2.
            + (f64::from(native.x) - metrics.dot_size / 2.) * holder_scale;
        let expected_y = holder.y
            + holder.height / 2.
            + (f64::from(native.y) - metrics.dot_size / 2.) * holder_scale;
        near(expected_x, browser.x + browser.width / 2., 0.003, &context);
        near(expected_y, browser.y + browser.height / 2., 0.003, &context);
    }
}

#[test]
fn spin_reference_component_tokens_and_global_aliases_match_actual_generators() {
    assert_eq!(reference().component_defaults.len(), 4);
    for (name, browser) in &reference().component_defaults {
        let theme = config(name).resolve();
        let tokens = theme.component::<SpinTheme>();
        for (native, browser) in [
            (tokens.content_height, browser.defaults.content_height),
            (tokens.dot_size, browser.defaults.dot_size),
            (tokens.dot_size_sm, browser.defaults.dot_size_sm),
            (tokens.dot_size_lg, browser.defaults.dot_size_lg),
        ] {
            near(native, browser, 0.000001, name);
        }
        let normal = SpinStyle::from_tokens(&tokens, Size::Medium, false);
        let fullscreen = SpinStyle::from_tokens(&tokens, Size::Medium, true);
        for (native, field) in [
            (normal.color, "colorPrimary"),
            (fullscreen.color, "colorWhite"),
            (fullscreen.description_color, "colorTextLightSolid"),
            (normal.track_color, "colorFillSecondary"),
            (normal.container_color, "colorBgContainer"),
            (normal.mask_color, "colorBgMask"),
        ] {
            color(native, browser.global[field].as_str().unwrap(), name);
        }
        near(
            normal.motion_duration.as_secs_f64(),
            seconds(browser.global["motionDurationSlow"].as_str().unwrap()),
            0.000001,
            name,
        );
        near(
            normal.fullscreen_duration.as_secs_f64(),
            seconds(browser.global["motionDurationMid"].as_str().unwrap()),
            0.000001,
            name,
        );
        near(
            normal.z_index as f64,
            browser.global["zIndexPopupBase"].as_f64().unwrap(),
            0.,
            name,
        );
    }
}

#[test]
fn spin_reference_styles_and_geometry_match_all_728_actual_browser_rows() {
    assert_eq!(reference().cases.len(), 4);
    let (mut rows, mut custom_tokens, mut custom_indicators, mut progress) = (0, 0, 0, 0);
    for case in &reference().cases {
        assert_eq!(case.results.len(), 182);
        let config = config(&case.theme);
        let theme = config.resolve();
        let custom_theme = tokens_theme(&config);
        for browser in &case.results {
            let options = &browser.options;
            let context = format!("{} #{} {:?}", case.theme, browser.index, options.direction);
            let metrics = SpinStyle::resolve(
                if options.token_case {
                    &custom_theme
                } else {
                    &theme
                },
                size_option(options),
                false,
            );
            rows += 1;
            custom_tokens += usize::from(options.token_case);
            near(metrics.dot_size, browser.holder_size, 0.0001, &context);
            near(metrics.font_size, browser.spin.font_size, 0.0001, &context);
            assert_eq!(
                browser.busy,
                if options.spinning == Some(false) {
                    "false"
                } else {
                    "true"
                },
                "{context}"
            );
            assert_eq!(browser.live, "polite", "{context}");
            if options.spinning == Some(false) {
                assert!(
                    browser.holder.is_none()
                        && browser.progress.is_none()
                        && browser.dots.is_empty(),
                    "{context}"
                );
            }
            if let Some(section) = &browser.section {
                color(metrics.color, &section.color, &context);
                if !options.semantic {
                    near(metrics.gap, pixels(&section.gap), 0.0001, &context);
                }
                if options.spinning != Some(false) {
                    if let Some(description) = &browser.description {
                        if !options.semantic {
                            color(metrics.description_color, &description.color, &context);
                            near(metrics.font_size, description.font_size, 0.0001, &context);
                            near(
                                metrics.font_size,
                                pixels(&description.line_height),
                                0.0001,
                                &context,
                            );
                            near(
                                metrics.dot_size + metrics.gap + metrics.font_size,
                                section.layout_height.unwrap(),
                                0.001,
                                &context,
                            );
                        }
                    } else {
                        near(
                            metrics.dot_size,
                            section.layout_height.unwrap(),
                            0.001,
                            &context,
                        );
                    }
                    if options.nested {
                        near(
                            section.x + section.width / 2.,
                            browser.spin.x + browser.spin.width / 2.,
                            0.001,
                            &context,
                        );
                        near(
                            section.y + section.height / 2.,
                            browser.spin.y + browser.spin.height / 2.,
                            0.001,
                            &context,
                        );
                    }
                }
            }
            if let Some(content) = &browser.content {
                near(content.width, browser.spin.width, 0.001, &context);
                near(content.height, browser.spin.height, 0.001, &context);
                assert_eq!(
                    content.pointer_events,
                    if options.spinning == Some(false) {
                        "auto"
                    } else {
                        "none"
                    },
                    "{context}"
                );
            }
            if let Some(mask) = &browser.mask {
                color(metrics.container_color, &mask.background, &context);
            }
            if options.indicator.is_some() {
                custom_indicators += 1;
                assert!(
                    browser.holder.is_none()
                        && browser.progress.is_none()
                        && browser.dots.is_empty(),
                    "{context}"
                );
            } else {
                compare_dots(browser, metrics, Duration::ZERO, &context);
                if let Some(holder) = &browser.holder {
                    if !options.semantic {
                        color(metrics.color, &holder.color, &context);
                    }
                    for dot in &browser.dots {
                        if !options.semantic {
                            color(metrics.color, &dot.background, &context);
                        }
                    }
                }
            }
            if let Some(ring) = &browser.progress {
                progress += 1;
                color(metrics.track_color, &ring.track, &context);
                near(
                    geometry::finite_percent(options.percent.unwrap()).clamp(0., 100.),
                    ring.value,
                    0.000001,
                    &context,
                );
            }
        }
    }
    assert_eq!((rows, custom_tokens, custom_indicators), (728, 48, 48));
    assert!(progress > 300, "positive and clipped percentage coverage");
}

#[test]
fn spin_reference_dot_motion_matches_all_56_browser_animation_cases() {
    assert_eq!(reference().animation_cases.len(), 56);
    let mut compared = 0;
    for case in &reference().animation_cases {
        let theme = config(&case.theme).resolve();
        assert_eq!(case.results.len(), 5);
        for browser in &case.results {
            assert_eq!(
                browser.options.direction.as_deref(),
                Some(case.direction.as_str())
            );
            let metrics = SpinStyle::resolve(&theme, size_option(&browser.options), false);
            compare_dots(
                browser,
                metrics,
                Duration::from_millis(case.time),
                &format!(
                    "{} {} {}ms #{}",
                    case.theme, case.direction, case.time, browser.index
                ),
            );
            compared += browser.dots.len();
        }
    }
    assert_eq!(compared, 896);
}

struct Owner;
impl Render for Owner {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
    }
}

struct StateHarness {
    cx: TestAppContext,
    window: WindowHandle<Owner>,
    scope: ConfigScope,
    state: Option<Entity<SpinState>>,
    time: u64,
}

impl StateHarness {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let (mut scope, mut state) = (None, None);
        let window = cx.open_window(size(px(360.), px(112.)), |window, cx| {
            let config = ConfigScope::new_window(cx);
            state = Some(cx.new(|cx| SpinState::new(config.clone(), window, cx)));
            scope = Some(config);
            Owner
        });
        Self {
            cx,
            window,
            scope: scope.unwrap(),
            state,
            time: 0,
        }
    }

    fn advance_to(&mut self, time: u64) {
        assert!(time >= self.time);
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(time - self.time));
        self.cx.run_until_parked();
        self.time = time;
    }

    fn sample(&mut self, input: &Spin) -> Sample {
        let state = self.state.as_ref().unwrap().clone();
        let scope = self.scope.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| {
                    state.update(input, scope.clone(), window, cx);
                    let config = scope.snapshot(cx);
                    let metrics = SpinStyle::resolve(&config.theme, Size::Medium, input.fullscreen);
                    state.sample(input, metrics, MotionPolicy::new(true, false), cx)
                })
            })
            .unwrap()
    }

    fn compare_step(&mut self, input: &Spin, browser: &TraceStep, offset: u64) {
        self.advance_to(browser.time - offset);
        let sample = self.sample(input);
        if let Some(busy) = &browser.busy {
            let busy_native = self
                .state
                .as_ref()
                .unwrap()
                .read_with(&self.cx, |state, _| state.loading);
            assert_eq!(busy_native, busy == "true", "{}", browser.label);
        }
        near(
            sample.percent.unwrap_or(0.).clamp(0., 100.),
            browser.progress.unwrap_or(0.),
            0.000000001,
            &browser.label,
        );
    }
}

#[test]
fn spin_reference_delayed_visibility_matches_original_hook_trace() {
    let trace = &reference().behavior.delay;
    assert_eq!(trace.len(), 9);
    let mut harness = StateHarness::new();
    let input = |spinning, delay| {
        Spin::new("reference-delay")
            .spinning(spinning)
            .delay(Duration::from_millis(delay))
    };
    harness.compare_step(&input(true, 500), &trace[0], 0);
    harness.compare_step(&input(true, 500), &trace[1], 0);
    harness.compare_step(&input(false, 500), &trace[2], 0);
    harness.compare_step(&input(false, 500), &trace[3], 0);
    harness.sample(&input(true, 200));
    harness.compare_step(&input(true, 200), &trace[4], 0);
    harness.compare_step(&input(false, 200), &trace[5], 0);
    harness.sample(&input(true, 500));
    harness.advance_to(1300);
    harness.sample(&input(true, 200));
    harness.compare_step(&input(true, 200), &trace[6], 0);
    harness.compare_step(&input(true, 200), &trace[7], 0);
    harness.sample(&input(false, 0));
    harness.sample(&input(true, 500));
    let weak = harness.state.take().unwrap().downgrade();
    harness.advance_to(trace[8].time + 1000);
    assert!(
        weak.upgrade().is_none(),
        "unmount releases pending delay ownership"
    );
}

#[test]
fn spin_reference_auto_progress_matches_all_original_hook_ticks_and_restart() {
    let browser = &reference().behavior;
    assert_eq!(browser.automatic.len(), 241);
    let offset = browser.automatic[0].time;
    let input = |spinning| {
        Spin::new("reference-auto")
            .spinning(spinning)
            .percent(SpinPercent::Auto)
    };
    let mut harness = StateHarness::new();
    for step in &browser.automatic {
        harness.compare_step(&input(true), step, offset);
    }
    harness.compare_step(&input(false), &browser.stopped, offset);
    harness.compare_step(&input(true), &browser.restarted, offset);
    harness.compare_step(&input(true), &browser.restarted_first_tick, offset);
}

#[test]
fn spin_reference_progress_transitions_match_actual_browser_waapi_samples() {
    assert_eq!(reference().transitions.len(), 3);
    let mut harness = StateHarness::new();
    let metrics = SpinStyle::resolve(&config("default").resolve(), Size::Medium, false);
    harness.sample(&Spin::new("reference-transition").percent(0.));
    for transition in &reference().transitions {
        let start = harness.time;
        let initial = harness.sample(&Spin::new("reference-transition").percent(transition.from));
        near(
            initial.progress,
            transition.from,
            0.000001,
            "settled transition source",
        );
        let input = Spin::new("reference-transition").percent(transition.to);
        harness.sample(&input);
        for property in &transition.properties {
            near(
                metrics.motion_duration.as_secs_f64() * 1000.,
                property.duration,
                0.,
                "CSS transition duration",
            );
            assert_eq!(property.easing, "ease");
        }
        assert_eq!(transition.samples.len(), 5);
        for browser in &transition.samples {
            harness.advance_to(start + browser.time);
            let native = harness.sample(&input);
            let context = format!(
                "{} -> {} at {}ms",
                transition.from, transition.to, browser.time
            );
            let holder = browser.spin.holder.as_ref().unwrap();
            let progress_holder = browser.spin.progress_holder.as_ref().unwrap();
            near(native.dots, holder.opacity, 0.000002, &context);
            near(
                0.3 + 0.7 * native.dots,
                holder.width / holder.layout_width.unwrap(),
                0.000004,
                &context,
            );
            near(native.ring, progress_holder.opacity, 0.000002, &context);
            near(
                progress_holder.width,
                progress_holder.layout_width.unwrap(),
                0.000001,
                &context,
            );
            let ring = browser.spin.progress.as_ref().unwrap();
            let painted_percent = pixels(ring.stroke_dasharray.split(',').next().unwrap())
                / (std::f64::consts::TAU * pixels(&ring.radius))
                * 100.;
            near(native.progress, painted_percent, 0.0003, &context);
        }
    }
}
