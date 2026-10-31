// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Real native GPU upload/readback and Present1 submission evidence, without exports.
//! Run the guarded Release script, scripts/run-image-gpu-perf.ps1.
use ant_design_gpui::{
    ConfigScope, PreviewGroup, UiRoot,
    image::{PreviewAction, PreviewItem, PreviewState},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, PlatformInput, Render, Window, WindowBounds, WindowOptions, div,
    point, prelude::*, px, size,
};
use serde_json::json;
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

struct Surface {
    state: Entity<PreviewState>,
}
impl Render for Surface {
    fn render(&mut self, window: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PreviewGroup::new("gpu-perf-preview", &self.state).child(
            div()
                .w(window.viewport_size().width)
                .h(window.viewport_size().height)
                .bg(gpui::rgb(0xffffff)),
        )
    }
}
fn p95(values: &mut [f64]) -> f64 {
    values.sort_by(f64::total_cmp);
    values[(values.len() * 95).div_ceil(100).saturating_sub(1)]
}
fn pan(window: &mut Window, cx: &mut App, sample: usize) {
    let origin = point(
        window.viewport_size().width / 2.,
        window.viewport_size().height / 2.,
    );
    let target = origin
        + point(
            px(if (sample / 6).is_multiple_of(2) {
                35.
            } else {
                -35.
            }),
            px(if (sample / 6).is_multiple_of(2) {
                20.
            } else {
                -20.
            }),
        );
    window.dispatch_event(
        PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position: origin,
            click_count: 1,
            ..Default::default()
        }),
        cx,
    );
    window.dispatch_event(
        PlatformInput::MouseMove(MouseMoveEvent {
            position: target,
            pressed_button: Some(MouseButton::Left),
            ..Default::default()
        }),
        cx,
    );
    window.dispatch_event(
        PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position: target,
            click_count: 1,
            ..Default::default()
        }),
        cx,
    );
}

fn require_release() {
    #[cfg(debug_assertions)]
    panic!("GPU performance evidence requires Release");
}
fn main() {
    require_release();
    let output = PathBuf::from(std::env::args_os().nth(1).expect("output JSON path"));
    let source = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("resources/image/landscape.png");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(800.), px(600.)), cx))),
            show: true, focus: false, ..Default::default()
        }, |window, cx| {
            window.set_window_title(&format!("Ant Design Image GPU validation {}", std::process::id()));
            let state = cx.new(|cx| PreviewState::new(vec![PreviewItem::new("gpu-perf", source).alt("GPU performance fixture")], window, cx));
            state.update(cx, |state, cx| {
                state.set_scope(scope.clone(), cx);
                state.set_open(true, window, cx);
                for _ in 0..4 { state.apply_action(PreviewAction::ZoomIn, window, cx); }
            });
            let service = window.gpu_service().expect("native GPU service");
            let baseline = service.diagnostics();
            let view = cx.new(|_| Surface { state: state.clone() });
            let root = cx.new(|cx| UiRoot::new(scope, view, window, cx));
            window.spawn(cx, async move |cx| {
                let started = Instant::now();
                loop {
                    let diagnostics = service.diagnostics();
                    if diagnostics.uploaded_bytes > baseline.uploaded_bytes && diagnostics.presentations > baseline.presentations { break; }
                    assert!(started.elapsed() < Duration::from_secs(30), "GPU texture did not warm and present: {diagnostics:?}");
                    cx.background_executor().timer(Duration::from_millis(5)).await;
                }
                // Warm every orientation and the action controls before sampling.
                for action in [PreviewAction::RotateRight, PreviewAction::FlipX, PreviewAction::FlipY, PreviewAction::ZoomIn, PreviewAction::ZoomOut] {
                    let before = service.diagnostics().presentations;
                    cx.update(|window, cx| { state.update(cx, |state, cx| state.apply_action(action, window, cx)); window.refresh(); }).unwrap();
                    let deadline = Instant::now() + Duration::from_secs(10);
                    while service.diagnostics().presentations <= before {
                        assert!(Instant::now() < deadline, "warm native presentation timed out");
                        cx.background_executor().timer(Duration::from_millis(1)).await;
                    }
                }
                cx.background_executor().timer(Duration::from_millis(100)).await;
                let warm = service.diagnostics();
                let mut samples = Vec::new();
                for index in 0..120 {
                    let before = service.diagnostics();
                    let transform_before = cx.update(|_, cx| state.read(cx).transform()).unwrap();
                    let dispatched = Instant::now();
                    let transform_after = cx.update(|window, cx| {
                        match index % 6 {
                            0 => state.update(cx, |s, cx| s.apply_action(PreviewAction::RotateRight, window, cx)),
                            1 => state.update(cx, |s, cx| s.apply_action(PreviewAction::FlipX, window, cx)),
                            2 => state.update(cx, |s, cx| s.apply_action(PreviewAction::FlipY, window, cx)),
                            3 => state.update(cx, |s, cx| s.apply_action(PreviewAction::ZoomIn, window, cx)),
                            4 => state.update(cx, |s, cx| s.apply_action(PreviewAction::ZoomOut, window, cx)),
                            _ => pan(window, cx, index),
                        }
                        window.refresh();
                        state.read(cx).transform()
                    }).unwrap();
                    let dispatch_cpu_us = dispatched.elapsed().as_secs_f64() * 1e6;
                    assert_ne!(transform_before, transform_after, "sample did not change the native image transform");
                    let deadline = Instant::now() + Duration::from_secs(10);
                    let after = loop {
                        let after = service.diagnostics();
                        if after.presentations > before.presentations && after.last_presentation_end.is_some_and(|end| end >= dispatched) { break after; }
                        assert!(Instant::now() < deadline, "transform native presentation timed out");
                        cx.background_executor().timer(Duration::from_millis(1)).await;
                    };
                    assert_eq!(after.uploaded_bytes, warm.uploaded_bytes, "transform-only frames uploaded new image payload");
                    assert_eq!(after.readback_bytes, warm.readback_bytes, "transform-only frames read GPU image data back");
                    let (textures, window_active, window_visible) = cx.update(|window, _| (
                        window.rendered_scene().gpu_textures.len(), window.is_window_active(), window.is_visible(),
                    )).unwrap();
                    assert!(textures > 0, "transform frame fell back from the native affine GPU compositor");
                    let action_name = ["rotateRight","flipX","flipY","zoomIn","zoomOut","pan"][index%6];
                    samples.push(json!({"action":action_name,"dispatch_cpu_us":dispatch_cpu_us,"native_gpu_textures":textures,"transform_changed":true,
                        "window_active":window_active,"window_visible":window_visible,
                        "dispatch_to_present1_return_ms":after.last_presentation_end.unwrap().duration_since(dispatched).as_secs_f64()*1000.,
                        "compositor_cpu_us":(after.compositor_cpu_ns-before.compositor_cpu_ns) as f64/1000.,
                        "present1_cpu_us":(after.present_cpu_ns-before.present_cpu_ns) as f64/1000.,"presentations":after.presentations-before.presentations}));
                }
                // GPUI deliberately sustains presentation for one second after
                // high-rate pointer input. Measure idle after that framework tail.
                cx.background_executor().timer(Duration::from_millis(1250)).await;
                let settled = service.diagnostics();
                cx.background_executor().timer(Duration::from_millis(300)).await;
                let idle = service.diagnostics();
                assert_eq!(idle.presentations, settled.presentations, "settled preview kept presenting idle frames");
                let callbacks = cx.update(|window, cx| window.simulate_next_frame(cx)).unwrap();
                assert_eq!(callbacks, 0, "settled preview retained frame callbacks");
                let mut input_latency: Vec<_> = samples.iter().map(|sample| sample["dispatch_to_present1_return_ms"].as_f64().unwrap()).collect();
                let mut compositor: Vec<_> = samples.iter().map(|sample| sample["compositor_cpu_us"].as_f64().unwrap()).collect();
                let mut presentation: Vec<_> = samples.iter().map(|sample| sample["present1_cpu_us"].as_f64().unwrap()).collect();
                let caps = service.capabilities();
                let (scale, viewport) = cx.update(|window, _| (window.scale_factor(), window.viewport_size())).unwrap();
                let any_window_active = samples.iter().any(|sample| sample["window_active"].as_bool().unwrap());
                let all_windows_visible = samples.iter().all(|sample| sample["window_visible"].as_bool().unwrap());
                let report = json!({"status":"passed","profile":"Release","operating_system":std::env::consts::OS,"architecture":std::env::consts::ARCH,
                    "adapter":caps.adapter_name.to_string(),"backend":format!("{:?}",caps.backend),"vendor_id":caps.vendor_id,"device_id":caps.device_id,
                    "window_visible":all_windows_visible,"window_focused_by_harness":false,"any_window_active":any_window_active,"reduced_motion":true,"scale":scale,"viewport":[f32::from(viewport.width),f32::from(viewport.height)],
                    "samples":samples,"sample_count":samples.len(),"warm_texture_upload_bytes":warm.uploaded_bytes-baseline.uploaded_bytes,
                    "transform_upload_bytes":idle.uploaded_bytes-warm.uploaded_bytes,"transform_readback_bytes":idle.readback_bytes-warm.readback_bytes,
                    "idle_presentations":idle.presentations-settled.presentations,"idle_callbacks":callbacks,
                    "dispatch_to_present1_return_p95_ms":p95(&mut input_latency),"compositor_cpu_p95_us":p95(&mut compositor),"present1_cpu_p95_us":p95(&mut presentation),
                    "measurement":"Real visible native window; harness does not request activation or focus. Per-sample activation is observed. Typed actions and native GPUI mouse dispatch to Present1 submission return. No screenshot exports in measured frames. GPU-service portable upload/readback counters exclude native atlas uploads. CPU submission time excludes GPU execution and physical scanout latency."});
                std::fs::create_dir_all(output.parent().unwrap()).unwrap();
                std::fs::write(&output, serde_json::to_vec_pretty(&report).unwrap()).unwrap();
                println!("GPU Image transforms: {} samples; uploads={} readbacks={} idle_frames={}", samples.len(), idle.uploaded_bytes-warm.uploaded_bytes, idle.readback_bytes-warm.readback_bytes, callbacks);
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("native GPU validation window");
    });
}
