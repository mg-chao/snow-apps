// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc};

use gpui::prelude::*;
use gpui::{
    AppContext, Background, Bounds, Context, Entity, IntoElement, Pixels, Render, TestAppContext,
    Window, WindowHandle, div, point, px, rgb, size,
};

use super::*;
use crate::{ConfigPatch, UiRoot};

const ROOT: u32 = 0x123456;
const LABELS: [u32; 4] = [0x123401, 0x123402, 0x123403, 0x123404];
const VALUES: [u32; 4] = [0x234501, 0x234502, 0x234503, 0x234504];

struct Probe {
    scope: ConfigScope,
    bordered: bool,
    vertical: bool,
    width: f32,
    context: Rc<RefCell<Option<DescriptionsStyleContext>>>,
    renders: usize,
    title: &'static str,
    extra: &'static str,
    spanning_text: bool,
    responsive: bool,
}

impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        let context = self.context.clone();
        let mut descriptions = Descriptions::new("descriptions")
            .title(self.title)
            .extra(self.extra)
            .scope(self.scope.clone())
            .columns(if self.responsive {
                DescriptionsColumns::default()
            } else {
                2usize.into()
            })
            .bordered(self.bordered)
            .layout(if self.vertical {
                DescriptionsLayout::Vertical
            } else {
                DescriptionsLayout::Horizontal
            })
            .styles(DescriptionsStyles {
                root: gpui::StyleRefinement::default().bg(rgb(ROOT)),
                ..Default::default()
            })
            .styles_fn(move |value, _, _| {
                *context.borrow_mut() = Some(*value);
                DescriptionsStyles::default()
            });
        for index in 0..4 {
            if self.spanning_text {
                let labels = ["Product", "Billing Mode", "Negotiated Amount", "Usage Time"];
                let values = [
                    "Cloud Database",
                    "2018-04-24 18:00:00",
                    "$80.00",
                    "2019-04-24 18:00:00",
                ];
                let mut item = DescriptionItem::new(index)
                    .label(labels[index])
                    .content(values[index])
                    .styles(DescriptionItemStyles {
                        label: StyleRefinement::default().bg(rgb(LABELS[index])),
                        content: StyleRefinement::default().bg(rgb(VALUES[index])),
                    });
                if index >= 2 {
                    item = item.span(DescriptionsSpan::Filled);
                }
                descriptions = descriptions.item(item);
                continue;
            }
            descriptions = descriptions.item(
                DescriptionItem::new(index)
                    .label_content(
                        div()
                            .w(px(40.))
                            .h(px(if index == 0 { 44. } else { 22. }))
                            .bg(rgb(LABELS[index])),
                    )
                    .content_element(div().w(px(120.)).h(px(22.)).bg(rgb(VALUES[index]))),
            );
        }
        div()
            .w(px(self.width + 62.))
            .p(px(31.))
            .child(div().w(px(self.width)).child(descriptions))
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    scope: ConfigScope,
}

impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root = ConfigScope::new_window(cx);
            let child = root.child(ConfigPatch::default(), cx).unwrap();
            let entity = cx.new(|_| Probe {
                scope: child.clone(),
                bordered: false,
                vertical: false,
                width: 600.,
                context: Rc::default(),
                renders: 0,
                title: "",
                extra: "",
                spanning_text: false,
                responsive: false,
            });
            probe = Some(entity.clone());
            scope = Some(child);
            UiRoot::new(root, entity, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            probe: probe.unwrap(),
            scope: scope.unwrap(),
        };
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn edit(&mut self, change: impl FnOnce(&mut Probe)) {
        self.probe.update(&mut self.cx, |probe, cx| {
            change(probe);
            cx.notify();
        });
        self.draw();
    }
    fn bounds(&mut self, color: u32) -> Bounds<Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let background: Background = rgb(color).into();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background == background)
                    .map(|quad| {
                        Bounds::new(
                            point(
                                px(quad.bounds.origin.x.0 / scale),
                                px(quad.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(quad.bounds.size.width.0 / scale),
                                px(quad.bounds.size.height.0 / scale),
                            ),
                        )
                    })
                    .unwrap_or_else(|| panic!("missing color {color:06x}"))
            })
            .unwrap()
    }
}

fn near(actual: Pixels, expected: f32) {
    assert!(
        (f32::from(actual) - expected).abs() <= 1.,
        "{actual:?} != {expected}"
    );
}

#[test]
fn rich_cells_reflow_in_one_frame_and_keep_parent_coordinates() {
    let mut fixture = Fixture::new();
    near(fixture.bounds(ROOT).left(), 31.);
    near(fixture.bounds(ROOT).top(), 31.);
    near(fixture.bounds(ROOT).size.width, 600.);
    // The upstream borderless table retains a one-pixel native cell inset.
    near(fixture.bounds(ROOT).size.height, 84.);
    near(fixture.bounds(LABELS[0]).left(), 32.);
    near(fixture.bounds(VALUES[0]).top(), 32.);
    near(fixture.bounds(LABELS[2]).top(), 93.);
    fixture.edit(|probe| probe.width = 500.);
    near(fixture.bounds(ROOT).size.width, 500.);
    near(fixture.bounds(LABELS[2]).top(), 93.);
}

#[test]
fn vertical_values_align_below_tallest_label() {
    let mut fixture = Fixture::new();
    fixture.edit(|probe| probe.vertical = true);
    near(fixture.bounds(VALUES[0]).top(), 93.);
    assert_eq!(
        fixture.bounds(VALUES[0]).top(),
        fixture.bounds(VALUES[1]).top()
    );
    near(fixture.bounds(ROOT).size.height, 162.);
}

#[test]
fn bordered_cells_stretch_and_inherit_configured_size() {
    let mut fixture = Fixture::new();
    fixture.edit(|probe| probe.bordered = true);
    let default_height = fixture.bounds(ROOT).size.height;
    fixture.cx.update(|cx| {
        fixture
            .scope
            .update_patch(cx, |patch| patch.size = Some(Size::Small))
            .unwrap()
    });
    fixture.draw();
    assert!(fixture.bounds(ROOT).size.height < default_height);
    assert_eq!(
        fixture
            .probe
            .read_with(&fixture.cx, |probe, _| probe.context.borrow().unwrap().size),
        Size::Small
    );
    assert!(fixture.bounds(VALUES[0]).left() > fixture.bounds(LABELS[0]).right());
}

#[test]
fn static_descriptions_schedule_no_frames_when_idle() {
    let fixture = Fixture::new();
    let count = fixture
        .probe
        .read_with(&fixture.cx, |probe, _| probe.renders);
    fixture.cx.run_until_parked();
    assert_eq!(
        fixture
            .probe
            .read_with(&fixture.cx, |probe, _| probe.renders),
        count
    );
}

#[test]
fn fractional_dpi_cell_spacing_does_not_accumulate_over_vertical_rows() {
    let mut fixture = Fixture::new();
    fixture.edit(|probe| probe.vertical = true);
    for scale in [1., 1.5, 2.] {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                window.set_scale_factor(scale)
            })
            .unwrap();
        fixture.draw();
        near(fixture.bounds(ROOT).size.height, 162.);
        assert_eq!(
            fixture.bounds(VALUES[0]).top(),
            fixture.bounds(VALUES[1]).top()
        );
    }
}

#[test]
fn whitespace_headers_keep_semantic_presence_without_inventing_line_height() {
    let mut fixture = Fixture::new();
    for (title, extra, height) in [
        (" ", " \t ", 104.),
        (" ", "Edit", 126.),
        ("Title", " ", 128.),
    ] {
        fixture.edit(|probe| {
            probe.title = title;
            probe.extra = extra;
        });
        let context = fixture
            .probe
            .read_with(&fixture.cx, |probe, _| probe.context.borrow().unwrap());
        assert!(context.has_title && context.has_extra);
        near(fixture.bounds(ROOT).size.height, height);
    }
    fixture.edit(|probe| {
        probe.title = "";
        probe.extra = "";
    });
    near(fixture.bounds(ROOT).size.height, 84.);
}

#[test]
fn spanning_rows_share_track_boundaries_when_available_width_requires_shrinking() {
    let mut fixture = Fixture::new();
    fixture.edit(|probe| {
        probe.bordered = true;
        probe.width = 536.;
        probe.spanning_text = true;
    });
    let first = fixture.bounds(LABELS[0]);
    for index in [2, 3] {
        near(
            fixture.bounds(LABELS[index]).size.width,
            f32::from(first.size.width),
        );
        assert!(fixture.bounds(VALUES[index]).right() <= fixture.bounds(ROOT).right() + px(1.));
    }
}

#[test]
fn fractional_native_viewport_quantization_preserves_css_breakpoint_boundaries() {
    let mut fixture = Fixture::new();
    fixture.edit(|probe| probe.responsive = true);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            window.set_scale_factor(1.5)
        })
        .unwrap();
    // 575 CSS pixels become 863 physical pixels on a 150% display. Resolving
    // the resulting native DIP width directly would fall between xs and sm.
    for (width, columns) in [(863. / 1.5, 1), (864. / 1.5, 2), (1152. / 1.5, 3)] {
        fixture
            .cx
            .simulate_window_resize(fixture.window.into(), size(px(width), px(720.)));
        fixture.draw();
        assert_eq!(
            fixture.probe.read_with(&fixture.cx, |probe, _| probe
                .context
                .borrow()
                .unwrap()
                .columns),
            columns,
            "viewport {width}"
        );
    }
}

#[test]
fn enclosing_borders_paint_before_opaque_cell_backgrounds() {
    for line_type in [
        crate::theme::LineType::Solid,
        crate::theme::LineType::Dashed,
        crate::theme::LineType::Dotted,
    ] {
        let mut fixture = Fixture::new();
        fixture.edit(|probe| {
            probe.bordered = true;
            probe.vertical = true;
            probe.spanning_text = true;
        });
        fixture.cx.update(|cx| {
            fixture
                .scope
                .update_patch(cx, |patch| {
                    let theme = patch.theme.get_or_insert_default();
                    theme.seed.line_type = Some(line_type);
                    theme.seed.line_width = Some(3.);
                })
                .unwrap();
        });
        fixture.draw();
        let bounds = fixture.bounds(ROOT);
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                let scene = window.rendered_scene();
                let border_order = if line_type == crate::theme::LineType::Solid {
                    scene
                        .quads
                        .iter()
                        .find(|quad| {
                            [
                                quad.border_widths.top,
                                quad.border_widths.right,
                                quad.border_widths.bottom,
                                quad.border_widths.left,
                            ]
                            .iter()
                            .all(|width| width.0 > 0.)
                        })
                        .expect("enclosing solid border")
                        .order
                } else {
                    scene
                        .paths
                        .iter()
                        .find(|path| {
                            path.bounds.size.width.0 >= f32::from(bounds.size.width) - 4.
                                && path.bounds.size.height.0 >= f32::from(bounds.size.height) - 4.
                        })
                        .expect("enclosing patterned border")
                        .order
                };
                for color in LABELS {
                    let background: Background = rgb(color).into();
                    let cell = scene
                        .quads
                        .iter()
                        .find(|quad| quad.background == background)
                        .expect("opaque label background");
                    assert!(
                        border_order < cell.order,
                        "{line_type:?} enclosing border must paint below opaque cells"
                    );
                }
            })
            .unwrap();
    }
}
