// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Assert final native scene geometry, independently of the style resolver.

use gpui::{
    AppContext, Context, Entity, FocusHandle, Hsla, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, PlatformInput, Render, StyleRefinement, TestAppContext, Window, WindowHandle, div,
    px,
};

use super::*;
use crate::{ConfigPatch, UiRoot};

const WIDTH: f32 = 336.;

fn surface_color(index: usize) -> Hsla {
    gpui::rgb([0xffdddd, 0xddffdd, 0xddddff][index]).into()
}

struct LayoutView {
    scope: ConfigScope,
    focus: [FocusHandle; 3],
    size: Size,
    vertical: bool,
    block: bool,
    button: bool,
    opacity: bool,
    clipped: bool,
    hidden_selected: bool,
    standalone: bool,
}

impl Render for LayoutView {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .w(px(WIDTH + 24.))
            .p(px(12.))
            .flex()
            .flex_col()
            .items_start()
            .when(self.clipped, |root| {
                root.w(px(120.)).h(px(100.)).overflow_hidden()
            })
            .when(!self.standalone, |root| {
                root.child(
                    RadioGroup::new("layout-group")
                        .scope(self.scope.clone())
                        .options(
                            ["A", "Much longer choice", "C"]
                                .into_iter()
                                .enumerate()
                                .map(|(index, label)| {
                                    let mut style = StyleRefinement::default()
                                        .bg(surface_color(index))
                                        .opacity(if self.opacity { 0.5 } else { 1. });
                                    if self.hidden_selected && index == 1 {
                                        style = style.invisible();
                                    }
                                    RadioOption::new(("option", index), index, label)
                                        .focus_handle(&self.focus[index])
                                        .style(style)
                                }),
                        )
                        .value(1)
                        .size(self.size)
                        .vertical(self.vertical)
                        .block(self.block)
                        .opacity(if self.opacity { 0.5 } else { 1. })
                        .option_type(if self.button {
                            RadioOptionType::Button
                        } else {
                            RadioOptionType::Default
                        }),
                )
            })
            .when(self.standalone, |root| {
                root.child(
                    Radio::new("standalone-layout-radio")
                        .scope(self.scope.clone())
                        .label("Selected")
                        .checked(true)
                        .focus_handle(&self.focus[1])
                        .option_type(RadioOptionType::Button)
                        .bg(surface_color(1))
                        .when(self.hidden_selected, |radio| radio.invisible()),
                )
            })
    }
}

#[derive(Clone, Copy, Debug)]
struct Quad {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    corners: [f32; 4],
    border_width: f32,
    border_color: Hsla,
    background: Option<Hsla>,
    mask_right: f32,
    mask_bottom: f32,
}

impl Quad {
    fn right(self) -> f32 {
        self.x + self.width
    }

    fn bottom(self) -> f32 {
        self.y + self.height
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    view: Entity<LayoutView>,
}

impl Fixture {
    fn new(size: Size, rtl: bool, vertical: bool, block: bool, button: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut view = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .set_patch(
                    ConfigPatch {
                        motion: Some(false),
                        direction: Some(if rtl { Direction::Rtl } else { Direction::Ltr }),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| LayoutView {
                scope: scope.clone(),
                focus: std::array::from_fn(|_| cx.focus_handle().tab_index(0)),
                size,
                vertical,
                block,
                button,
                opacity: false,
                clipped: false,
                hidden_selected: false,
                standalone: false,
            });
            view = Some(content.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            view: view.unwrap(),
        };
        fixture.draw();
        fixture
    }

    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn scene(&mut self) -> Vec<Quad> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .map(|quad| Quad {
                        x: quad.bounds.origin.x.0 / scale,
                        y: quad.bounds.origin.y.0 / scale,
                        width: quad.bounds.size.width.0 / scale,
                        height: quad.bounds.size.height.0 / scale,
                        corners: [
                            quad.corner_radii.top_left.0 / scale,
                            quad.corner_radii.top_right.0 / scale,
                            quad.corner_radii.bottom_right.0 / scale,
                            quad.corner_radii.bottom_left.0 / scale,
                        ],
                        border_width: quad.border_widths.top.0 / scale,
                        border_color: quad.border_color,
                        background: quad.background.as_solid(),
                        mask_right: quad.content_mask.bounds.right().0 / scale,
                        mask_bottom: quad.content_mask.bounds.bottom().0 / scale,
                    })
                    .collect()
            })
            .unwrap()
    }

    fn surfaces(&mut self) -> [Quad; 3] {
        let scene = self.scene();
        std::array::from_fn(|index| {
            *scene
                .iter()
                .find(|quad| quad.background == Some(surface_color(index)))
                .expect("each option paints one identifiable root surface")
        })
    }

    fn tab(&mut self) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                let keystroke = Keystroke::parse("tab").unwrap();
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: keystroke.clone(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
            })
            .unwrap();
        self.draw();
    }
}

fn near(actual: f32, expected: f32) {
    assert!((actual - expected).abs() < 0.05, "{actual} != {expected}");
}

#[test]
fn radio_layout_button_sizes_and_physical_outer_corners_follow_direction() {
    for (size, height, radius) in [
        (Size::Small, 24., 4.),
        (Size::Medium, 32., 6.),
        (Size::Large, 40., 8.),
    ] {
        for rtl in [false, true] {
            let mut fixture = Fixture::new(size, rtl, false, false, true);
            let surfaces = fixture.surfaces();
            for surface in surfaces {
                near(surface.height, height);
                near(surface.y, surfaces[0].y);
            }
            assert_eq!(surfaces[1].corners, [0.; 4]);
            assert_eq!(
                surfaces[0].corners,
                if rtl {
                    [0., radius, radius, 0.]
                } else {
                    [radius, 0., 0., radius]
                },
            );
            assert_eq!(
                surfaces[2].corners,
                if rtl {
                    [radius, 0., 0., radius]
                } else {
                    [0., radius, radius, 0.]
                },
            );
            for pair in surfaces.windows(2) {
                let distance = if rtl {
                    pair[0].x - pair[1].right()
                } else {
                    pair[1].x - pair[0].right()
                };
                assert!(
                    (-1.05..0.05).contains(&distance),
                    "connected surfaces: {pair:?}"
                );
            }
        }
    }
}

#[test]
fn radio_layout_block_options_share_width_and_fill_the_parent() {
    for button in [false, true] {
        for rtl in [false, true] {
            let mut fixture = Fixture::new(Size::Medium, rtl, false, true, button);
            let surfaces = fixture.surfaces();
            let left = surfaces
                .iter()
                .map(|surface| surface.x)
                .fold(f32::INFINITY, f32::min);
            let right = surfaces
                .iter()
                .map(|surface| surface.right())
                .fold(0., f32::max);
            near(left, 12.);
            near(right - left, WIDTH);
            for surface in surfaces {
                assert!(
                    (surface.width - surfaces[0].width).abs() <= 1.,
                    "equal-width block options: {surfaces:?}"
                );
            }
        }
    }
}

#[test]
fn radio_layout_vertical_buttons_stretch_and_overlap_one_shared_border() {
    for rtl in [false, true] {
        for block in [false, true] {
            let mut fixture = Fixture::new(Size::Medium, rtl, true, block, true);
            let surfaces = fixture.surfaces();
            for surface in surfaces {
                near(surface.x, surfaces[0].x);
                near(surface.width, surfaces[0].width);
                near(surface.height, 32.);
            }
            for pair in surfaces.windows(2) {
                near(pair[1].y, pair[0].bottom() - 1.);
            }
            near(surfaces[2].bottom() - surfaces[0].y, 94.);
            assert_eq!(surfaces[0].corners, [6., 6., 0., 0.]);
            assert_eq!(surfaces[1].corners, [0.; 4]);
            assert_eq!(surfaces[2].corners, [0., 0., 6., 6.]);
            if block {
                near(surfaces[0].width, WIDTH);
            }
        }
    }
}

#[test]
fn radio_layout_vertical_circles_keep_reference_row_gap() {
    for rtl in [false, true] {
        let mut fixture = Fixture::new(Size::Medium, rtl, true, false, false);
        let surfaces = fixture.surfaces();
        for pair in surfaces.windows(2) {
            near(pair[1].y - pair[0].bottom(), 8.);
        }
    }
}

#[test]
fn radio_layout_keyboard_focus_encloses_the_complete_selected_button_border() {
    for rtl in [false, true] {
        let mut fixture = Fixture::new(Size::Medium, rtl, false, false, true);
        fixture.tab();
        let selected = fixture.surfaces()[1];
        let scene = fixture.scene();
        let ring = scene
            .iter()
            .find(|quad| quad.border_width == 3.)
            .expect("keyboard focus outline");
        let mut left = selected.x;
        let mut right = selected.right();
        // Implementations may paint a selected shared edge as a separate quad.
        // Include that visible border when checking the uniform focus-ring gap.
        for seam in &scene {
            if seam.background == Some(selected.border_color)
                && seam.width <= 1.05
                && (seam.y - selected.y).abs() < 0.05
                && (seam.height - selected.height).abs() < 0.05
            {
                left = left.min(seam.x);
                right = right.max(seam.right());
            }
        }
        near(ring.x, left - 4.);
        near(ring.right(), right + 4.);
        near(ring.y, selected.y - 4.);
        near(ring.bottom(), selected.bottom() + 4.);
        assert_eq!(ring.corners, [4.; 4]);
    }
}

#[test]
fn radio_layout_group_decoration_retains_ancestor_clipping_and_each_opacity_once() {
    let mut fixture = Fixture::new(Size::Medium, false, false, false, true);
    fixture.tab();
    fixture.view.update(&mut fixture.cx, |view, cx| {
        view.opacity = true;
        view.clipped = true;
        cx.notify();
    });
    fixture.draw();
    let scene = fixture.scene();
    let ring = scene
        .iter()
        .find(|quad| quad.border_width == 3.)
        .expect("focused group option");
    near(ring.border_color.a, 0.25);
    assert!(ring.mask_right <= 120.05);
    assert!(ring.mask_bottom <= 100.05);
}

#[test]
fn radio_layout_invalid_component_gaps_never_reach_native_layout() {
    let mut fixture = Fixture::new(Size::Medium, false, false, false, false);
    let scope = fixture
        .view
        .read_with(&fixture.cx, |view, _| view.scope.clone());
    for gap in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -2.] {
        fixture.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch
                        .theme
                        .get_or_insert_default()
                        .components
                        .set::<RadioTheme>(RadioTokenPatch {
                            wrapper_margin_inline_end: Some(gap),
                            ..Default::default()
                        });
                })
                .unwrap();
        });
        fixture.draw();
        let surfaces = fixture.surfaces();
        for surface in surfaces {
            assert!(surface.x.is_finite() && surface.width.is_finite());
        }
        for pair in surfaces.windows(2) {
            near(pair[1].x, pair[0].right());
        }
    }
}

#[test]
fn radio_layout_hidden_selected_button_paints_neither_shared_border_nor_focus_outline() {
    for vertical in [false, true] {
        for rtl in [false, true] {
            let mut fixture = Fixture::new(Size::Medium, rtl, vertical, false, true);
            fixture.tab();
            let primary = crate::theme::Color::rgb(0x1677ff).to_hsla();
            let visible = fixture.scene();
            assert!(visible.iter().any(|quad| quad.border_color == primary));
            assert!(visible.iter().any(|quad| quad.border_width == 3.));
            fixture.view.update(&mut fixture.cx, |view, cx| {
                view.hidden_selected = true;
                cx.notify();
            });
            fixture.draw();
            let hidden = fixture.scene();
            assert!(
                hidden
                    .iter()
                    .all(|quad| quad.background != Some(surface_color(1)))
            );
            assert!(
                hidden.iter().all(|quad| quad.border_color != primary),
                "a hidden selected option must not leave its queued border visible"
            );
            assert!(
                hidden.iter().all(|quad| quad.border_width != 3.),
                "a hidden focused option must not leave a queued keyboard outline"
            );
            for index in [0, 2] {
                assert!(
                    hidden
                        .iter()
                        .any(|quad| quad.background == Some(surface_color(index))),
                    "hiding one option keeps its visible neighbors painted"
                );
            }
        }
    }
}

#[test]
fn radio_layout_hidden_standalone_button_paints_no_keyboard_outline() {
    let mut fixture = Fixture::new(Size::Medium, false, false, false, true);
    fixture.view.update(&mut fixture.cx, |view, cx| {
        view.standalone = true;
        cx.notify();
    });
    fixture.draw();
    fixture.tab();
    let visible = fixture.scene();
    assert!(
        visible
            .iter()
            .any(|quad| quad.background == Some(surface_color(1)))
    );
    assert!(visible.iter().any(|quad| quad.border_width == 3.));
    fixture.view.update(&mut fixture.cx, |view, cx| {
        view.hidden_selected = true;
        cx.notify();
    });
    fixture.draw();
    let hidden = fixture.scene();
    assert!(
        hidden
            .iter()
            .all(|quad| quad.background != Some(surface_color(1)))
    );
    assert!(
        hidden.iter().all(|quad| quad.border_width != 3.),
        "the external FocusRing adapter must honor a hidden standalone root"
    );
}
