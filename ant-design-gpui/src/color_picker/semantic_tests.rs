// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::PickerColor;
use crate::UiRoot;
use gpui::{AppContext, Context, Render, TestAppContext, WindowHandle};

#[test]
fn preset_boundaries_validate_and_canonicalize_public_gradient_values() {
    let blue = PickerColor::parse("#1677ff").unwrap();
    let red = PickerColor::parse("#f5222d").unwrap();
    let raw = ColorPickerValue::Gradient(
        [
            super::super::ColorStop {
                color: red,
                percent: 100.,
            },
            super::super::ColorStop {
                color: blue,
                percent: 0.,
            },
        ]
        .into(),
    );
    let preset = ColorPickerPreset::try_new(
        "valid",
        "Colors",
        [raw.clone(), ColorPickerValue::Gradient(Arc::from([]))],
    )
    .unwrap();
    let ColorPickerValue::Gradient(stops) = &preset.colors[0] else {
        panic!("gradient")
    };
    assert_eq!((stops[0].percent, stops[1].percent), (0., 100.));
    assert_eq!(preset.colors[1], ColorPickerValue::Cleared);
    assert!(Arc::ptr_eq(
        &preset.colors,
        &preset.canonicalized().unwrap().colors
    ));
    let mut modified = preset.clone();
    modified.colors = [raw].into();
    assert_eq!(
        modified.canonicalized().unwrap().colors[0],
        preset.colors[0]
    );
    modified.colors = [ColorPickerValue::Gradient(
        [super::super::ColorStop {
            color: blue,
            percent: f64::NAN,
        }]
        .into(),
    )]
    .into();
    assert!(modified.canonicalized().is_err());
    assert!(
        ColorPickerPreset::try_new("invalid", "Colors", modified.colors.iter().cloned()).is_err()
    );
}

#[test]
fn format_labels_fit_their_unclipped_viewport_in_both_directions() {
    // The ordinary offscreen text system measures correctly but emits empty
    // rasters. Give glyphs deterministic ink so this test observes paint masks.
    struct FormatTextSystem;
    impl gpui::PlatformTextSystem for FormatTextSystem {
        fn add_fonts(&self, fonts: Vec<std::borrow::Cow<'static, [u8]>>) -> gpui::Result<()> {
            gpui::NoopTextSystem.add_fonts(fonts)
        }
        fn all_font_names(&self) -> Vec<String> {
            gpui::NoopTextSystem.all_font_names()
        }
        fn font_id(&self, font: &gpui::Font) -> gpui::Result<gpui::FontId> {
            gpui::NoopTextSystem.font_id(font)
        }
        fn font_metrics(&self, font: gpui::FontId) -> gpui::FontMetrics {
            gpui::NoopTextSystem.font_metrics(font)
        }
        fn typographic_bounds(
            &self,
            font: gpui::FontId,
            glyph: gpui::GlyphId,
        ) -> gpui::Result<gpui::Bounds<f32>> {
            gpui::NoopTextSystem.typographic_bounds(font, glyph)
        }
        fn advance(
            &self,
            font: gpui::FontId,
            glyph: gpui::GlyphId,
        ) -> gpui::Result<gpui::Size<f32>> {
            gpui::NoopTextSystem.advance(font, glyph)
        }
        fn glyph_for_char(&self, font: gpui::FontId, ch: char) -> Option<gpui::GlyphId> {
            gpui::NoopTextSystem.glyph_for_char(font, ch)
        }
        fn glyph_raster_bounds(
            &self,
            params: &gpui::RenderGlyphParams,
        ) -> gpui::Result<gpui::Bounds<gpui::DevicePixels>> {
            let size = f32::from(params.font_size) * params.scale_factor;
            Ok(gpui::Bounds::new(
                gpui::point(
                    gpui::DevicePixels(0),
                    gpui::DevicePixels(-(size * 0.8).round() as i32),
                ),
                gpui::size(
                    gpui::DevicePixels((size * 0.6).ceil() as i32),
                    gpui::DevicePixels((size * 0.8).ceil() as i32),
                ),
            ))
        }
        fn rasterize_glyph(
            &self,
            _: &gpui::RenderGlyphParams,
            bounds: gpui::Bounds<gpui::DevicePixels>,
        ) -> gpui::Result<(gpui::Size<gpui::DevicePixels>, Vec<u8>)> {
            Ok((
                bounds.size,
                vec![255; (bounds.size.width.0 * bounds.size.height.0) as usize],
            ))
        }
        fn layout_line(
            &self,
            text: &str,
            font_size: gpui::Pixels,
            runs: &[gpui::FontRun],
        ) -> gpui::LineLayout {
            gpui::NoopTextSystem.layout_line(text, font_size, runs)
        }
        fn recommended_rendering_mode(
            &self,
            font: gpui::FontId,
            size: gpui::Pixels,
        ) -> gpui::TextRenderingMode {
            gpui::NoopTextSystem.recommended_rendering_mode(font, size)
        }
    }
    struct FormatProbe {
        scope: ConfigScope,
        state: Entity<ColorPickerState>,
        format: ColorPickerFormat,
    }
    impl Render for FormatProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().p(px(40.)).child(
                ColorPickerPanel::new("format-probe", &self.state)
                    .scope(self.scope.clone())
                    .w(px(258.))
                    .format(self.format)
                    .value(PickerColor::parse("#1677ff").unwrap()),
            )
        }
    }
    for direction in [Direction::Ltr, Direction::Rtl] {
        for compact in [false, true] {
            for scale in [1., 1.5, 2.] {
                for format in [
                    ColorPickerFormat::Hex,
                    ColorPickerFormat::Rgb,
                    ColorPickerFormat::Hsb,
                ] {
                    let mut cx = TestAppContext::build_with_text_system(
                        gpui::TestDispatcher::new(0),
                        None,
                        Arc::new(FormatTextSystem),
                    );
                    let mut retained = None;
                    let window = cx.add_window(|window, cx| {
                        let scope = ConfigScope::new_window(cx);
                        scope
                            .update_patch(cx, |patch| {
                                patch.direction = Some(direction);
                                patch.motion = Some(false);
                                patch.theme = Some(crate::theme::ThemeConfig {
                                    algorithms: Some(if compact {
                                        crate::theme::AlgorithmChain::dark_compact()
                                    } else {
                                        crate::theme::AlgorithmChain::dark()
                                    }),
                                    ..Default::default()
                                });
                            })
                            .unwrap();
                        let state = cx.new(|cx| ColorPickerState::new(window, cx));
                        retained = Some((scope.clone(), state.clone()));
                        let content = cx.new(|_| FormatProbe {
                            scope: scope.clone(),
                            state,
                            format,
                        });
                        UiRoot::new(scope, content, window, cx)
                    });
                    cx.simulate_window_scale_factor_change(window.into(), scale);
                    for _ in 0..2 {
                        cx.run_until_parked();
                        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                            .unwrap();
                    }
                    let (scope, state) = retained.unwrap();
                    let geometry = state.read_with(&cx, |state, _| state.geometry());
                    cx.update_window(window.into(), |_, window, cx| {
                        let config = scope.snapshot(cx);
                        let g = config.theme.component::<ColorPickerTheme>();
                        let text_color = g.globals().color_text.to_hsla();
                        let row = geometry.editors[0];
                        let panel = geometry.panel.unwrap();
                        let edge = if direction == Direction::Rtl {
                            panel.right() - px(g.globals().padding_sm as f32 + 38.)
                        } else {
                            panel.left() + px(g.globals().padding_sm as f32 + 38.)
                        };
                        let scene = window.rendered_scene();
                        let sprites: Vec<_> = scene.monochrome_sprites.iter()
                            .map(|sprite| (sprite.bounds, sprite.content_mask.bounds, sprite.color))
                            .chain(scene.subpixel_sprites.iter().map(|sprite| (sprite.bounds, sprite.content_mask.bounds, sprite.color)))
                            .filter(|(bounds, _, color)| {
                            let x = bounds.origin.x.0 / scale;
                            let y = bounds.origin.y.0 / scale;
                            *color == text_color && y >= f32::from(row.top()) && y < f32::from(row.bottom())
                                && if direction == Direction::Rtl {x >= f32::from(edge)} else {x < f32::from(edge)}
                        }).collect();
                        assert_eq!(sprites.len(), 3, "all format glyphs exist: {direction:?},compact={compact},scale={scale},format={format:?}");
                        for (bounds, mask, _) in sprites {
                            assert!(bounds.left().0 >= mask.left().0 - 1.
                                && bounds.right().0 <= mask.right().0 + 1.,
                                "format glyph clipped: {direction:?},compact={compact},scale={scale},format={format:?},glyph={bounds:?},mask={mask:?}");
                        }
                    }).unwrap();
                }
            }
        }
    }
}

struct Probe {
    scope: ConfigScope,
    state: Entity<ColorPickerState>,
    value: ColorPickerValue,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().p(px(40.)).child(
            ColorPicker::new("semantic-picker", &self.state)
                .scope(self.scope.clone())
                .value(self.value.clone())
                .open(true)
                .styles(ColorPickerStyles {
                    body: div()
                        .size(px(40.))
                        .rounded(px(12.))
                        .bg(gpui::rgb(0x00ff00))
                        .border(px(3.))
                        .border_color(gpui::rgb(0x722ed1))
                        .style()
                        .clone(),
                    content: div()
                        .size(px(20.))
                        .rounded(px(5.))
                        .bg(gpui::rgb(0xff0000))
                        .border(px(2.))
                        .border_color(gpui::rgb(0x0000ff))
                        .style()
                        .clone(),
                    ..Default::default()
                }),
        )
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    state: Entity<ColorPickerState>,
}
impl Fixture {
    fn new(value: ColorPickerValue) -> Self {
        Self::with_styles(value, ColorPickerStyles::default())
    }
    fn with_styles(value: ColorPickerValue, styles: ColorPickerStyles) -> Self {
        Self::with_options(value, styles, Direction::Ltr)
    }
    fn with_options(
        value: ColorPickerValue,
        styles: ColorPickerStyles,
        direction: Direction,
    ) -> Self {
        let mut cx = TestAppContext::single();
        let mut state = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| {
                    patch.motion = Some(false);
                    patch.direction = Some(direction);
                    patch.color_picker.styles = styles;
                })
                .unwrap();
            let picker = cx.new(|cx| ColorPickerState::new(window, cx));
            state = Some(picker.clone());
            let probe = cx.new(|_| Probe {
                scope: scope.clone(),
                state: picker,
                value,
            });
            UiRoot::new(scope, probe, window, cx)
        });
        for _ in 0..2 {
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
        }
        Self {
            cx,
            window,
            state: state.unwrap(),
        }
    }
}

#[test]
fn color_picker_semantic_body_and_content_style_the_trigger_swatch_only() {
    let mut f = Fixture::new(PickerColor::parse("#1677ff80").unwrap().into());
    let geometry = f.state.read_with(&f.cx, |state, _| state.geometry());
    assert_eq!(
        geometry.trigger_swatch.unwrap().size,
        gpui::size(px(40.), px(40.))
    );
    assert_eq!(
        geometry.palette.unwrap().size,
        gpui::size(px(234.), px(160.))
    );
    assert_eq!(geometry.panel.unwrap().size.width, px(258.));
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let quads = &window.rendered_scene().quads;
        assert!(
            quads.iter().any(
                |q| q.background == gpui::Background::from(gpui::rgb(0x00ff00))
                    && (q.bounds.size.width.0 / scale - 40.).abs() < 0.01
                    && (q.corner_radii.top_left.0 / scale - 12.).abs() < 0.01
            ),
            "body background, size and radius"
        );
        assert!(
            quads.iter().any(
                |q| q.background == gpui::Background::from(gpui::rgb(0xff0000))
                    && (q.bounds.size.width.0 / scale - 20.).abs() < 0.01
                    && (q.corner_radii.top_left.0 / scale - 5.).abs() < 0.01
            ),
            "content overrides selected color with its own size and radius"
        );
        assert!(
            quads
                .iter()
                .any(|q| q.border_color == gpui::Hsla::from(gpui::rgb(0x722ed1))
                    && (q.border_widths.left.0 / scale - 3.).abs() < 0.01),
            "body border"
        );
        assert!(
            quads
                .iter()
                .any(|q| q.border_color == gpui::Hsla::from(gpui::rgb(0x0000ff))
                    && (q.border_widths.left.0 / scale - 2.).abs() < 0.01),
            "content border"
        );
    })
    .unwrap();
}

#[test]
fn cleared_semantic_body_has_no_synthetic_inner_color_content() {
    let mut f = Fixture::new(ColorPickerValue::Cleared);
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(
            !window.rendered_scene().quads.iter().any(|q| q.background
                == gpui::Background::from(gpui::rgb(0xff0000))
                && (q.bounds.size.width.0 / window.scale_factor() - 20.).abs() < 0.01
                && (q.bounds.size.height.0 / window.scale_factor() - 20.).abs() < 0.01),
            "clear has a body and slash but no content fill"
        );
    })
    .unwrap();
}

#[test]
fn custom_swatch_content_follows_the_logical_inline_edge() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        let mut f = Fixture::with_options(
            PickerColor::parse("#1677ff").unwrap().into(),
            ColorPickerStyles::default(),
            direction,
        );
        let body = f
            .state
            .read_with(&f.cx, |state, _| state.geometry().trigger_swatch.unwrap());
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let content = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.background == gpui::Background::from(gpui::rgb(0xff0000))
                        && (quad.bounds.size.width.0 / scale - 20.).abs() < 0.01
                })
                .expect("custom content fill");
            let offset = if direction == Direction::Rtl { 17. } else { 3. };
            assert!(
                (content.bounds.origin.x.0 / scale - f32::from(body.left()) - offset).abs() < 0.01
            );
            assert!((content.bounds.origin.y.0 / scale - f32::from(body.top()) - 3.).abs() < 0.01);
        })
        .unwrap();
    }
}

#[test]
fn styled_trigger_nominal_minimum_survives_fractional_dpi_border_rounding() {
    for scale in [1.5, 2.] {
        let mut f = Fixture::new(PickerColor::parse("#1677ff").unwrap().into());
        f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
        for _ in 0..2 {
            f.cx.run_until_parked();
            f.cx.update_window(f.window.into(), |_, window, cx| {
                window.refresh();
                window.draw(cx).clear(cx);
            })
            .unwrap();
        }
        let geometry = f.state.read_with(&f.cx, |state, _| state.geometry());
        assert_eq!(geometry.trigger.unwrap().size, gpui::size(px(48.), px(48.)));
    }
}

#[test]
fn native_color_surface_styles_replace_the_painted_background() {
    let mut f = Fixture::with_styles(
        PickerColor::parse("#1677ff").unwrap().into(),
        ColorPickerStyles {
            palette: div()
                .rounded(px(0.))
                .bg(gpui::rgb(0xff00ff))
                .style()
                .clone(),
            slider: div()
                .rounded(px(0.))
                .bg(gpui::rgb(0x00ffff))
                .style()
                .clone(),
            ..Default::default()
        },
    );
    f.cx.update_window(f.window.into(), |_, window, _| {
        let quads = &window.rendered_scene().quads;
        assert!(
            quads.iter().any(
                |quad| quad.background == gpui::Background::from(gpui::rgb(0xff00ff))
                    && (quad.bounds.size.width.0 / window.scale_factor() - 234.).abs() < 0.01
                    && quad.corner_radii.top_left.0 == 0.
            ),
            "custom palette is painted as a square native fill"
        );
        assert!(
            quads
                .iter()
                .filter(
                    |quad| quad.background == gpui::Background::from(gpui::rgb(0x00ffff))
                        && (quad.bounds.size.height.0 / window.scale_factor() - 8.).abs() < 0.01
                )
                .count()
                >= 2,
            "both compact rails use the requested native fill"
        );
    })
    .unwrap();
}

#[test]
fn standalone_styled_methods_refine_the_outer_panel() {
    struct Standalone {
        scope: ConfigScope,
        state: Entity<ColorPickerState>,
    }
    impl Render for Standalone {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            ColorPickerPanel::new("inline", &self.state)
                .scope(self.scope.clone())
                .w(px(400.))
                .bg(gpui::rgb(0x123456))
        }
    }
    let mut cx = TestAppContext::single();
    let mut state = None;
    let window = cx.add_window(|window, cx| {
        let scope = ConfigScope::new_window(cx);
        let controller = cx.new(|cx| ColorPickerState::new(window, cx));
        state = Some(controller.clone());
        let content = cx.new(|_| Standalone {
            scope: scope.clone(),
            state: controller,
        });
        UiRoot::new(scope, content, window, cx)
    });
    for _ in 0..2 {
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    }
    let geometry = state.unwrap().read_with(&cx, |state, _| state.geometry());
    assert_eq!(geometry.panel.unwrap().size.width, px(400.));
    cx.update_window(window.into(), |_, window, _| {
        assert!(
            window
                .rendered_scene()
                .quads
                .iter()
                .any(|quad| quad.background == gpui::Background::from(gpui::rgb(0x123456)))
        );
    })
    .unwrap();
}

#[test]
fn authored_trigger_spacing_and_relative_width_reach_the_actual_anchor() {
    struct StyledProbe {
        scope: ConfigScope,
        states: [Entity<ColorPickerState>; 2],
        styles: ColorPickerStyles,
        full: bool,
    }
    impl Render for StyledProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let root = div().w(px(400.)).p(px(40.));
            if self.full {
                root.child(
                    ColorPicker::new("styled", &self.states[1])
                        .scope(self.scope.clone())
                        .show_text(true)
                        .w_full(),
                )
                .into_any_element()
            } else {
                root.flex()
                    .gap(px(16.))
                    .child(
                        ColorPicker::new("baseline", &self.states[0])
                            .scope(self.scope.clone())
                            .show_text(true),
                    )
                    .child(
                        ColorPicker::new("styled", &self.states[1])
                            .scope(self.scope.clone())
                            .show_text(true)
                            .styles(self.styles.clone()),
                    )
                    .into_any_element()
            }
        }
    }
    for direction in [Direction::Ltr, Direction::Rtl] {
        for (styles, reduction, full) in [
            (
                ColorPickerStyles {
                    root: div().p_0().style().clone(),
                    ..Default::default()
                },
                6.,
                false,
            ),
            (
                ColorPickerStyles {
                    root: div().border_0().style().clone(),
                    ..Default::default()
                },
                2.,
                false,
            ),
            (
                ColorPickerStyles {
                    description: div().mx_0().style().clone(),
                    ..Default::default()
                },
                13.,
                false,
            ),
            (ColorPickerStyles::default(), 0., true),
        ] {
            let mut cx = TestAppContext::single();
            let mut retained = None;
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                scope
                    .update_patch(cx, |patch| {
                        patch.direction = Some(direction);
                        patch.motion = Some(false);
                    })
                    .unwrap();
                let states = std::array::from_fn(|_| {
                    cx.new(|cx| {
                        let mut state = ColorPickerState::new(window, cx);
                        state
                            .set_value("#1677ff".parse().unwrap(), window, cx)
                            .unwrap();
                        state
                    })
                });
                retained = Some(states.clone());
                let probe = cx.new(|_| StyledProbe {
                    scope: scope.clone(),
                    states,
                    styles,
                    full,
                });
                UiRoot::new(scope, probe, window, cx)
            });
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            let states = retained.unwrap();
            let styled = states[1].read_with(&cx, |state, _| state.geometry().trigger.unwrap());
            if full {
                assert_eq!(styled.size.width, px(320.), "{direction:?}");
            } else {
                let baseline =
                    states[0].read_with(&cx, |state, _| state.geometry().trigger.unwrap());
                assert!(
                    (f32::from(baseline.size.width - styled.size.width) - reduction).abs() <= 0.51,
                    "{direction:?}: baseline={baseline:?}, styled={styled:?}, reduction={reduction}"
                );
            }
        }
    }
}
