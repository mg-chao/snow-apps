// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-descriptions-perf.ps1.

use std::{cell::Cell, hint::black_box, rc::Rc, time::Instant};

use ant_design_gpui::{
    Button, ConfigScope, DescriptionItem, Descriptions, DescriptionsLayout, DescriptionsSpan,
    Direction, UiRoot,
};
use gpui::{
    AppContext, Context, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};
use serde_json::{Value, json};

fn sample(name: &str, count: usize, mut operation: impl FnMut()) -> Value {
    let mut values = Vec::with_capacity(count);
    for _ in 0..count {
        let start = Instant::now();
        operation();
        values.push(start.elapsed().as_secs_f64() * 1e6);
    }
    values.sort_by(f64::total_cmp);
    let median = values[count / 2];
    let p95 = values[count * 95 / 100];
    println!("{name}: median={median:.2} us p95={p95:.2} us n={count}");
    json!({"name":name,"iterations":count,"median_microseconds":median,"p95_microseconds":p95})
}

struct Bench {
    scope: ConfigScope,
    count: usize,
    bordered: bool,
    vertical: bool,
    rich: bool,
    items: usize,
    renders: Rc<Cell<usize>>,
}

impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        div()
            .w(px(960.))
            .flex()
            .flex_col()
            .children((0..self.count).map(|index| {
                let mut description = Descriptions::new(("description", index))
                    .scope(self.scope.clone())
                    .columns(3)
                    .bordered(self.bordered)
                    .layout(if self.vertical {
                        DescriptionsLayout::Vertical
                    } else {
                        DescriptionsLayout::Horizontal
                    })
                    .title("Database details");
                for item in 0..self.items {
                    let mut value = DescriptionItem::new(("item", item))
                        .label("Product details")
                        .content("Cloud Database");
                    if item + 1 == self.items {
                        value = value.span(DescriptionsSpan::Filled).content_element(
                            div()
                                .flex()
                                .flex_col()
                                .child("Data disk type: MongoDB")
                                .child("Database version: 3.4")
                                .child("Storage space: 10 GB"),
                        );
                    }
                    description = description.item(value);
                }
                if self.rich {
                    description = description.extra_content(
                        Button::new("edit")
                            .scope(self.scope.clone())
                            .label("Edit")
                            .wave(false),
                    );
                }
                description
            }))
    }
}

fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext, refresh: bool) -> usize {
    cx.update_window(window.into(), |_, window, cx| {
        if refresh {
            window.refresh();
        }
        window.draw(cx).clear(cx);
        window.simulate_next_frame(cx)
    })
    .unwrap()
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Descriptions measurements require Release");
        std::process::exit(2);
    }
    let mut measurements =
        vec![sample("descriptions/builder/12-items", 10_000, || {
            black_box(Descriptions::new("bench").title("Database details").items(
                (0usize..12).map(|index| {
                    DescriptionItem::new(("item", index))
                        .label("Product")
                        .content("Cloud Database")
                }),
            ));
        })];
    let mut structure = Vec::new();
    for (count, scale) in [1, 20, 100]
        .into_iter()
        .flat_map(|count| [1., 1.5, 2.].map(move |scale| (count, scale)))
    {
        let mut cx = TestAppContext::single();
        let mut content = None;
        let renders = Rc::new(Cell::new(0));
        let window = cx.open_window(size(px(1000.), px(1600.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            // The rich action can sit under the test pointer in RTL. Exclude
            // its hover animation from static Descriptions layout measurements.
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let view = cx.new(|_| Bench {
                scope: scope.clone(),
                count,
                bordered: false,
                vertical: false,
                rich: false,
                items: 9,
                renders: renders.clone(),
            });
            content = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let content = content.unwrap();
        cx.simulate_window_scale_factor_change(window.into(), scale);
        for direction in [Direction::Ltr, Direction::Rtl] {
            content.update(&mut cx, |view, cx| {
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(direction))
                    .unwrap();
            });
            for (bordered, vertical, rich, name) in [
                (false, false, false, "horizontal"),
                (true, false, false, "bordered"),
                (false, true, false, "vertical"),
                (true, true, true, "vertical-bordered-rich"),
            ] {
                content.update(&mut cx, |view, cx| {
                    view.bordered = bordered;
                    view.vertical = vertical;
                    view.rich = rich;
                    cx.notify();
                });
                let mut transition_callbacks = 0;
                measurements.push(sample(
                    &format!("descriptions/{count}/{scale}x/{direction:?}/{name}/first-draw"),
                    1,
                    || {
                        transition_callbacks += draw(window, &mut cx, true);
                    },
                ));
                let mut settled = false;
                let mut settle_frames = 0;
                // Rich actions can schedule one host cleanup frame when
                // direction or layout changes. Record those separately from
                // the settled static component's callback/render gates.
                for _ in 0..8 {
                    cx.run_until_parked();
                    let callbacks = draw(window, &mut cx, true);
                    transition_callbacks += callbacks;
                    settle_frames += 1;
                    if callbacks == 0 {
                        settled = true;
                        break;
                    }
                }
                assert!(settled, "presentation changes must settle");
                let before_warm = renders.get();
                measurements.push(sample(
                    &format!("descriptions/{count}/{scale}x/{direction:?}/{name}/warm-redraw"),
                    40,
                    || {
                        assert_eq!(
                            draw(window, &mut cx, true),
                            0,
                            "settled Descriptions must schedule no callbacks"
                        );
                    },
                ));
                cx.run_until_parked();
                let after_warm = renders.get();
                measurements.push(sample(
                    &format!("descriptions/{count}/{scale}x/{direction:?}/{name}/idle-event-pump"),
                    40,
                    || cx.run_until_parked(),
                ));
                assert_eq!(
                    renders.get(),
                    after_warm,
                    "idle event processing must not trigger a render"
                );
                let idle_callbacks = cx
                    .update_window(window.into(), |_, window, cx| {
                        window.simulate_next_frame(cx)
                    })
                    .unwrap();
                assert_eq!(
                    idle_callbacks, 0,
                    "idle Descriptions must schedule no callbacks"
                );
                structure.push(json!({"components":count,"items_per_component":9,"scale":scale,"direction":format!("{direction:?}"),"layout":name,"warm_render_calls":after_warm-before_warm,"idle_render_calls":renders.get()-after_warm,"idle_callbacks":idle_callbacks,"transition_callbacks":transition_callbacks,"settle_frames":settle_frames}));
            }
        }
        if count == 1 && scale == 1. {
            for items in [30, 300, 3000] {
                content.update(&mut cx, |view, cx| {
                    view.items = items;
                    view.bordered = true;
                    view.vertical = false;
                    view.rich = false;
                    cx.notify();
                });
                draw(window, &mut cx, true);
                measurements.push(sample(
                    &format!("descriptions/one-table/{items}-items/warm-redraw"),
                    20,
                    || {
                        assert_eq!(
                            draw(window, &mut cx, true),
                            0,
                            "settled long table must schedule no callbacks"
                        );
                    },
                ));
            }
        }
    }
    let report = json!({
        "kind":"focused Release CPU and offscreen GPUI drawing with TestAppContext/NoopTextSystem",
        "idle_animation_callbacks":0,
        "motion_enabled":false,
        "measurements":measurements,
        "structural_cases":structure,
        "limitations":["Offscreen CPU measurements do not measure native GPU frame latency.", "The deterministic GPUI test text system does not measure production operating-system font shaping or glyph rasterization latency.", "First-draw measures one observation after a layout/direction change, not a repeated cold-cache distribution.", "Decorative motion is disabled to exclude rich-action hover animation under the deterministic test pointer. Descriptions has no component animation."]
    });
    if let Some(path) = std::env::var_os("ADGPUI_DESCRIPTIONS_PERF_OUTPUT") {
        let path = std::path::PathBuf::from(path);
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        std::fs::write(path, serde_json::to_string_pretty(&report).unwrap() + "\n").unwrap();
    }
}
