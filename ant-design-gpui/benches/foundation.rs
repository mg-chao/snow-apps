// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release-only measurements. Run through scripts/run-foundation-perf.ps1.
use ant_design_gpui::{
    ConfigPatch, ConfigScope, Size,
    icons::{IconName, IconRef, IconRenderOptions, IconRenderer},
    theme::ThemeConfig,
};
use gpui::{SvgRenderer, TestAppContext};
use std::{hint::black_box, sync::Arc, time::Instant};

fn measure(name: &str, iterations: u32, mut work: impl FnMut()) {
    let started = Instant::now();
    for _ in 0..iterations {
        work();
    }
    let elapsed = started.elapsed();
    println!(
        "{name}: {iterations} iterations, {:.1} ns/iteration",
        elapsed.as_nanos() as f64 / f64::from(iterations)
    );
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("foundation benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default();
    measure("theme/light/cold", 2_000, || {
        black_box(theme.resolve());
    });
    let cx = TestAppContext::single();
    let root = cx.update(ConfigScope::new_window);
    let mut scope = root.clone();
    for _ in 0..32 {
        scope = cx.update(|cx| scope.child(ConfigPatch::default(), cx).unwrap());
    }
    measure("scope/depth32/read", 200_000, || {
        cx.update(|cx| {
            black_box(scope.snapshot(cx));
        });
    });
    measure("scope/noop", 50_000, || {
        cx.update(|cx| {
            black_box(root.set_patch(ConfigPatch::default(), cx).unwrap());
        });
    });
    let mut small = false;
    measure("scope/depth32/update", 2_000, || {
        small = !small;
        cx.update(|cx| {
            root.update_patch(cx, |patch| {
                patch.size = Some(if small { Size::Small } else { Size::Medium })
            })
            .unwrap();
        });
    });
    let svg = SvgRenderer::new(Arc::new(()));
    let mut renderer = IconRenderer::default();
    let options = IconRenderOptions::default();
    let icon = IconRef::builtin(IconName::CheckCircleTwoTone);
    black_box(renderer.rasterize(&svg, &(), &icon, &options).unwrap());
    let before = renderer.statistics();
    measure("icon/twotone/warm-raster", 100_000, || {
        black_box(renderer.rasterize(&svg, &(), &icon, &options).unwrap());
    });
    let after = renderer.statistics();
    assert_eq!(before.parse_misses, after.parse_misses);
    assert_eq!(before.raster_misses, after.raster_misses);
    let mut index = 0;
    measure("icon/catalog/cache-churn", 2_000, || {
        let icon = IconRef::builtin(IconName::ALL[index % IconName::ALL.len()]);
        index += 1;
        black_box(renderer.rasterize(&svg, &(), &icon, &options).unwrap());
    });
    println!("icon cache after churn: {:?}", renderer.statistics());
}
