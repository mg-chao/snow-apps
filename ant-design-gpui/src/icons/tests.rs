// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    borrow::Cow,
    sync::atomic::{AtomicUsize, Ordering},
};

use super::*;
use gpui::{AppContext, ParentElement};

static TWO_TONE: IconDescriptor = IconDescriptor {
    key: "fixture/ordered-two-tone",
    color_model: IconColorModel::TwoTone,
    svg: br#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><path fill="currentColor" d="M0 0h10v10H0z"/><path fill="__ADGPUI_SECONDARY__" d="M2 0h6v10H2z"/><path fill="currentColor" d="M4 0h2v10H4z"/></svg>"#,
};

static FULL_COLOR: IconDescriptor = IconDescriptor {
    key: "fixture/full-color",
    color_model: IconColorModel::FullColor,
    svg: br##"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><path fill="#ff0000" d="M0 0h5v10H0z"/><path fill="#0000ff" d="M5 0h5v10H5z"/></svg>"##,
};

fn renderer() -> SvgRenderer {
    SvgRenderer::new(Arc::new(()))
}

fn pixel(image: &RenderImage, x: usize, y: usize) -> [u8; 4] {
    let width = image.size(0).width.0 as usize;
    image.as_bytes(0).unwrap()[(y * width + x) * 4..][..4]
        .try_into()
        .unwrap()
}

#[test]
fn every_builtin_icon_parses_and_rasterizes() {
    let renderer = renderer();
    let mut cache = IconRenderer::default();
    for name in IconName::ALL {
        let image = cache
            .rasterize(&renderer, &(), &name.icon(), &IconRenderOptions::default())
            .unwrap_or_else(|error| panic!("{}: {error}", name.descriptor().key));
        assert_eq!(image.size(0), size(DevicePixels(16), DevicePixels(16)));
        assert_eq!(image.as_bytes(0).unwrap().len(), 16 * 16 * 4);
        assert!(
            image
                .as_bytes(0)
                .unwrap()
                .as_chunks::<4>()
                .0
                .iter()
                .any(|pixel| pixel[3] > 0),
            "{} produced no visible pixels",
            name.descriptor().key,
        );
    }
    assert_eq!(cache.statistics().parse_misses, 829);
    assert!(cache.statistics().parsed_entries <= 256);
    assert!(cache.statistics().raster_entries <= 512);
}

#[test]
fn two_tone_preserves_interleaved_source_order_and_exact_colors() {
    let mut cache = IconRenderer::default();
    let icon = IconRef::custom(&TWO_TONE)
        .unwrap()
        .with_colors(IconColors::two_tone(rgba(0xff0000ff), rgba(0x00ff00ff)));
    let options = IconRenderOptions {
        size: size(px(10.), px(10.)),
        ..IconRenderOptions::default()
    };
    let image = cache.rasterize(&renderer(), &(), &icon, &options).unwrap();
    assert_eq!(pixel(&image, 0, 5), [0, 0, 255, 255]);
    assert_eq!(pixel(&image, 2, 5), [0, 255, 0, 255]);
    assert_eq!(pixel(&image, 4, 5), [0, 0, 255, 255]);
}

#[test]
fn same_colors_reuse_raster_and_dpr_changes_reuse_parsed_svg() {
    let renderer = renderer();
    let mut cache = IconRenderer::default();
    let icon = IconName::AlertTwoTone.icon();
    let options = IconRenderOptions::default();
    let first = cache.rasterize(&renderer, &(), &icon, &options).unwrap();
    let again = cache.rasterize(&renderer, &(), &icon, &options).unwrap();
    assert!(Arc::ptr_eq(&first, &again));
    let high_dpi = cache
        .rasterize(
            &renderer,
            &(),
            &icon,
            &IconRenderOptions {
                scale_factor: 2.,
                ..options
            },
        )
        .unwrap();
    assert_eq!(high_dpi.size(0), size(DevicePixels(32), DevicePixels(32)));
    assert!(!Arc::ptr_eq(&first, &high_dpi));
    let stats = cache.statistics();
    assert_eq!(stats.parse_misses, 1);
    assert_eq!(stats.parse_hits, 1);
    assert_eq!(stats.raster_hits, 1);
    assert_eq!(stats.raster_misses, 2);
}

#[test]
fn color_changes_have_distinct_rasters_and_references_remain_immutable() {
    let renderer = renderer();
    let mut cache = IconRenderer::default();
    let original = IconName::AlertTwoTone.icon();
    let changed = original
        .clone()
        .with_colors(IconColors::primary(rgba(0xff0000ff)));
    let options = IconRenderOptions::default();
    let first = cache
        .rasterize(&renderer, &(), &original, &options)
        .unwrap();
    let second = cache.rasterize(&renderer, &(), &changed, &options).unwrap();
    assert!(!Arc::ptr_eq(&first, &second));
    assert_ne!(first.as_bytes(0), second.as_bytes(0));
    assert_eq!(original.colors, IconColors::default());
    assert_eq!(cache.statistics().parse_misses, 2);
}

#[test]
fn theme_palette_uses_resolved_text_primary_and_generated_secondary() {
    let mut tokens = crate::theme::ThemeConfig::default()
        .resolve()
        .tokens
        .as_ref()
        .clone();
    tokens.color_text = crate::theme::Color::rgba(0x123456ab);
    tokens.color_primary = crate::theme::Color::rgb(0xff0000);
    let palette = IconPalette::from_theme(&tokens);
    assert_eq!(palette.text, tokens.color_text.to_rgba());
    assert_eq!(palette.primary, rgba(0xff0000ff));
    assert_eq!(
        palette.secondary,
        crate::theme::generate_palette(tokens.color_primary, false)[0].to_rgba()
    );
    assert_ne!(palette.secondary, tokens.color_primary_bg.to_rgba());
}

#[test]
fn primary_only_two_tone_overrides_derive_the_matching_tint_once() {
    let red = rgba(0xff0000ff);
    let icon = IconName::AlertTwoTone
        .icon()
        .with_colors(IconColors::primary(red));
    let expected =
        crate::theme::generate_palette(crate::theme::Color::rgb(0xff0000), false)[0].to_rgba();
    assert_eq!(icon.colors.primary, Some(red));
    assert_eq!(icon.colors.secondary, Some(expected));
    assert_eq!(
        resolve_colors(&icon, &IconRenderOptions::default())
            .unwrap()
            .1,
        pack_color(expected).unwrap()
    );
    let explicit = IconName::AlertTwoTone
        .icon()
        .with_colors(IconColors::two_tone(red, rgba(0x00ff00ff)));
    assert_eq!(explicit.colors.secondary, Some(rgba(0x00ff00ff)));
    let invalid = IconName::AlertTwoTone
        .icon()
        .with_colors(IconColors::primary(Rgba { r: f32::NAN, ..red }));
    assert!(matches!(
        resolve_colors(&invalid, &IconRenderOptions::default()),
        Err(IconError::InvalidColor)
    ));
}

#[test]
fn full_color_source_is_preserved_and_mirroring_is_explicit() {
    let renderer = renderer();
    let mut cache = IconRenderer::default();
    let icon = IconRef::custom(&FULL_COLOR).unwrap();
    let options = IconRenderOptions {
        size: size(px(10.), px(10.)),
        ..IconRenderOptions::default()
    };
    let image = cache.rasterize(&renderer, &(), &icon, &options).unwrap();
    assert_eq!(pixel(&image, 0, 5), [0, 0, 255, 255]);
    assert_eq!(pixel(&image, 9, 5), [255, 0, 0, 255]);
    let mirrored = cache
        .rasterize(
            &renderer,
            &(),
            &icon,
            &IconRenderOptions {
                mirror: true,
                ..options
            },
        )
        .unwrap();
    assert_eq!(pixel(&mirrored, 0, 5), [255, 0, 0, 255]);
    let invalid = icon.with_colors(IconColors::primary(rgba(0xff0000ff)));
    assert!(matches!(
        cache.rasterize(&renderer, &(), &invalid, &options),
        Err(IconError::FullColorOverrides)
    ));
}

#[test]
fn byte_entry_and_parsed_limits_are_enforced_with_caller_images_retained() {
    let renderer = renderer();
    let mut cache = IconRenderer::new(IconCacheLimits {
        parsed_entries: 1,
        raster_entries: 1,
        raster_bytes: 1024,
        max_raster_bytes: 1024,
    });
    let options = IconRenderOptions::default();
    let first = cache
        .rasterize(&renderer, &(), &IconName::AlertTwoTone.icon(), &options)
        .unwrap();
    cache
        .rasterize(
            &renderer,
            &(),
            &IconName::AccountBookTwoTone.icon(),
            &options,
        )
        .unwrap();
    let stats = cache.statistics();
    assert_eq!(stats.parsed_entries, 1);
    assert_eq!(stats.raster_entries, 1);
    assert_eq!(stats.raster_bytes, 1024);
    assert_eq!(stats.evictions, 1);
    assert_eq!(first.as_bytes(0).unwrap().len(), 1024);
    let retired = cache.clear();
    assert_eq!(retired.len(), 1);
    assert_eq!(cache.statistics().raster_bytes, 0);
    assert_eq!(cache.statistics().parsed_entries, 0);
    assert_eq!(first.as_bytes(0).unwrap().len(), 1024);
}

#[test]
fn oversize_and_disabled_cache_entries_are_not_retained() {
    let renderer = renderer();
    let mut cache = IconRenderer::new(IconCacheLimits {
        max_raster_bytes: 1023,
        parsed_entries: 0,
        ..IconCacheLimits::default()
    });
    cache
        .rasterize(
            &renderer,
            &(),
            &IconName::AlertTwoTone.icon(),
            &IconRenderOptions::default(),
        )
        .unwrap();
    assert_eq!(cache.statistics().raster_bytes, 0);
    assert_eq!(cache.statistics().raster_entries, 0);
    assert_eq!(cache.statistics().parsed_entries, 0);
}

struct HostAssets {
    loads: AtomicUsize,
}

impl AssetSource for HostAssets {
    fn load(&self, path: &str) -> gpui::Result<Option<Cow<'static, [u8]>>> {
        self.loads.fetch_add(1, Ordering::Relaxed);
        Ok((path == "project/logo.svg").then_some(Cow::Borrowed(FULL_COLOR.svg)))
    }

    fn list(&self, _path: &str) -> gpui::Result<Vec<SharedString>> {
        Ok(vec![SharedString::from("project/logo.svg")])
    }
}

#[test]
fn custom_assets_use_the_host_source_and_cache_loads() {
    let host = Arc::new(HostAssets {
        loads: AtomicUsize::new(0),
    });
    let renderer = SvgRenderer::new(host.clone());
    let mut cache = IconRenderer::default();
    let icon = IconRef::asset("project/logo.svg", IconColorModel::FullColor);
    let options = IconRenderOptions::default();
    cache.rasterize(&renderer, &*host, &icon, &options).unwrap();
    cache.rasterize(&renderer, &*host, &icon, &options).unwrap();
    assert_eq!(host.loads.load(Ordering::Relaxed), 1);
    assert_eq!(host.list("").unwrap().len(), 1);
    assert!(matches!(
        cache.rasterize(
            &renderer,
            &*host,
            &IconRef::asset("project/missing.svg", IconColorModel::FullColor),
            &options,
        ),
        Err(IconError::AssetMissing(_))
    ));
}

#[test]
fn invalid_dimensions_colors_and_reserved_custom_keys_are_rejected() {
    let mut cache = IconRenderer::default();
    for width in [0., -1., f32::NAN, f32::INFINITY, 4097.] {
        assert!(matches!(
            cache.rasterize(
                &renderer(),
                &(),
                &IconName::AlertTwoTone.icon(),
                &IconRenderOptions {
                    size: size(px(width), px(16.)),
                    ..IconRenderOptions::default()
                },
            ),
            Err(IconError::InvalidSize)
        ));
    }
    static RESERVED: IconDescriptor = IconDescriptor {
        key: "antd/custom-logo",
        color_model: IconColorModel::Monochrome,
        svg: FULL_COLOR.svg,
    };
    assert!(matches!(
        IconRef::custom(&RESERVED),
        Err(IconError::InvalidCustomKey)
    ));
    let invalid = IconName::AlertTwoTone
        .icon()
        .with_colors(IconColors::primary(Rgba {
            r: f32::NAN,
            ..rgba(0xffffffff)
        }));
    assert!(matches!(
        cache.rasterize(&renderer(), &(), &invalid, &IconRenderOptions::default()),
        Err(IconError::InvalidColor)
    ));
    assert_eq!(cache.statistics().parse_misses, 0);
}

#[test]
fn malformed_custom_svg_is_reported_without_corrupting_cached_icons() {
    static INVALID: IconDescriptor = IconDescriptor {
        key: "fixture/invalid-svg",
        color_model: IconColorModel::FullColor,
        svg: b"<svg><path",
    };
    let renderer = renderer();
    let mut cache = IconRenderer::default();
    let valid = IconName::AlertTwoTone.icon();
    let options = IconRenderOptions::default();
    let original = cache.rasterize(&renderer, &(), &valid, &options).unwrap();
    assert!(matches!(
        cache.rasterize(
            &renderer,
            &(),
            &IconRef::custom(&INVALID).unwrap(),
            &options
        ),
        Err(IconError::InvalidSvg(_))
    ));
    let healthy = cache.rasterize(&renderer, &(), &valid, &options).unwrap();
    assert!(Arc::ptr_eq(&original, &healthy));
    assert_eq!(cache.statistics().raster_entries, 1);
}

#[test]
fn tightening_single_raster_limit_reclaims_existing_oversize_entries() {
    let mut cache = IconRenderer::default();
    cache
        .rasterize(
            &renderer(),
            &(),
            &IconName::AlertTwoTone.icon(),
            &IconRenderOptions::default(),
        )
        .unwrap();
    let retired = cache.set_limits(IconCacheLimits {
        max_raster_bytes: 1023,
        ..cache.limits()
    });
    assert_eq!(retired.len(), 1);
    assert_eq!(cache.statistics().raster_entries, 0);
    assert_eq!(cache.statistics().raster_bytes, 0);
}

struct PaintFixture {
    rotation: Radians,
    colorful: bool,
}

struct InheritedColorFixture(gpui::Rgba);
impl gpui::Render for InheritedColorFixture {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        gpui::div()
            .text_color(self.0)
            .child(icon(IconName::DownOutlined).inherit_color())
            .child(icon(IconName::CloseOutlined).palette(IconPalette {
                text: gpui::rgb(0x00ff00),
                ..Default::default()
            }))
    }
}

#[test]
fn inherited_icon_color_follows_native_text_style_without_changing_explicit_palettes() {
    let mut cx = gpui::TestAppContext::single();
    let window = cx.add_window(|_, _| InheritedColorFixture(gpui::rgb(0xff0000)));
    for color in [gpui::rgb(0xff0000), gpui::rgb(0x0000ff)] {
        window
            .update(&mut cx, |view, window, cx| {
                view.0 = color;
                cx.notify();
                window.refresh();
            })
            .unwrap();
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            let colors: Vec<_> = window
                .rendered_scene()
                .monochrome_sprites
                .iter()
                .map(|sprite| sprite.color.to_rgb())
                .collect();
            assert_eq!(colors, [color, gpui::rgb(0x00ff00)]);
        })
        .unwrap();
    }
}

impl gpui::Render for PaintFixture {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        icon(if self.colorful {
            IconName::AlertTwoTone
        } else {
            IconName::LoadingOutlined
        })
        .rotation(self.rotation)
    }
}

#[test]
fn rotating_monochrome_uses_the_same_atlas_tile_without_color_raster_cache_work() {
    let mut cx = gpui::TestAppContext::single();
    let window = cx.add_window(|_, _| PaintFixture {
        rotation: radians(0.),
        colorful: false,
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    let (tile, transform) = cx
        .update_window(window.into(), |_, window, cx| {
            let primitive = &window.rendered_scene().monochrome_sprites[0];
            assert_eq!(cx.global::<IconRenderer>().statistics().parse_misses, 0);
            (primitive.tile.tile_id, primitive.transformation)
        })
        .unwrap();
    window.root(&mut cx).unwrap().update(&mut cx, |view, cx| {
        view.rotation = radians(std::f32::consts::FRAC_PI_2);
        cx.notify();
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.update_window(window.into(), |_, window, cx| {
        let primitive = &window.rendered_scene().monochrome_sprites[0];
        assert_eq!(primitive.tile.tile_id, tile);
        assert_ne!(primitive.transformation, transform);
        assert_eq!(cx.global::<IconRenderer>().statistics().raster_entries, 0);
    })
    .unwrap();
}

#[test]
fn fractional_artwork_size_preserves_declared_extent_without_changing_default_slots() {
    struct FractionalIcons;
    impl gpui::Render for FractionalIcons {
        fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
            gpui::div()
                .flex()
                .gap(px(4.))
                .child(icon(IconName::ZoomInOutlined).size(px(15.)))
                .child(
                    icon(IconName::ZoomInOutlined)
                        .size(px(15.))
                        .paint_size(size(px(15.), px(15.))),
                )
                .child(
                    icon(IconName::ZoomInOutlined)
                        .size(px(31.))
                        .paint_size(size(px(15.), px(15.))),
                )
        }
    }
    let mut cx = gpui::TestAppContext::single();
    let window = cx.add_window(|_, _| FractionalIcons);
    cx.simulate_window_scale_factor_change(window.into(), 1.5);
    cx.update_window(window.into(), |_, window, cx| {
        window.draw(cx).clear(cx);
        let sprites = &window.rendered_scene().monochrome_sprites;
        assert_eq!(sprites.len(), 3);
        assert_eq!(sprites[0].tile.tile_id, sprites[1].tile.tile_id);
        assert_eq!(sprites[0].tile.tile_id, sprites[2].tile.tile_id);
        let extent = |index: usize| {
            sprites[index].bounds.size.width.0 * sprites[index].transformation.rotation_scale[0][0]
                / window.scale_factor()
        };
        assert!((extent(0) - 14.666_667).abs() < 1e-5);
        assert!((extent(1) - 15.).abs() < 1e-5);
        assert!((extent(2) - 15.).abs() < 1e-5);
        let centered = &sprites[2];
        let transform = centered.transformation;
        let center = centered.bounds.center();
        assert!(
            (center.x.0 * transform.rotation_scale[0][0] + transform.translation[0] - 79.).abs()
                < 1e-4
        );
        assert!(
            (center.y.0 * transform.rotation_scale[1][1] + transform.translation[1] - 23.).abs()
                < 1e-4
        );
        assert_eq!(cx.global::<IconRenderer>().statistics().raster_entries, 0);
    })
    .unwrap();
}

#[test]
fn app_cache_clear_preserves_live_scene_until_the_replacement_frame() {
    let mut cx = gpui::TestAppContext::single();
    let window = cx.add_window(|_, _| PaintFixture {
        rotation: radians(0.),
        colorful: true,
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    let image = cx
        .update_window(window.into(), |_, window, cx| {
            let image = cx
                .global::<IconRenderer>()
                .resident
                .values()
                .find_map(std::sync::Weak::upgrade)
                .expect("scene leases two-tone raster")
                .image
                .clone();
            assert!(window.has_image_atlas_entry(&image));
            clear_icon_cache(window, cx);
            assert!(window.has_image_atlas_entry(&image));
            assert_eq!(cx.global::<IconRenderer>().statistics().raster_bytes, 0);
            image
        })
        .unwrap();
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.update_window(window.into(), |_, window, _| {
        assert!(!window.has_image_atlas_entry(&image));
    })
    .unwrap();
}

struct EvictionFixture {
    first_image: std::rc::Rc<std::cell::RefCell<Option<Arc<RenderImage>>>>,
}

impl gpui::Render for EvictionFixture {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        let first_image = self.first_image.clone();
        canvas(
            |_, _, _| (),
            move |bounds, (), window, cx| {
                cx.update_global::<IconRenderer, _>(|renderer, cx| {
                    renderer
                        .paint(
                            bounds,
                            &IconName::AlertTwoTone.icon(),
                            &IconRenderOptions::default(),
                            window,
                            cx,
                        )
                        .unwrap();
                    let first = renderer
                        .resident
                        .iter()
                        .find(|(key, _)| {
                            key.parsed.source
                                == SourceKey::Embedded(IconName::AlertTwoTone.descriptor()
                                    as *const IconDescriptor
                                    as usize)
                        })
                        .and_then(|(_, resource)| resource.upgrade())
                        .unwrap()
                        .image
                        .clone();
                    renderer
                        .paint(
                            bounds,
                            &IconName::AccountBookTwoTone.icon(),
                            &IconRenderOptions::default(),
                            window,
                            cx,
                        )
                        .unwrap();
                    assert_eq!(renderer.statistics().raster_entries, 0);
                    assert!(
                        window.has_image_atlas_entry(&first),
                        "earlier elements still need the evicted atlas tile during this paint"
                    );
                    *first_image.borrow_mut() = Some(first);
                });
            },
        )
        .w(px(16.))
        .h(px(16.))
    }
}

#[test]
fn paint_eviction_keeps_current_frame_tiles_while_scene_leases_exist() {
    let mut cx = gpui::TestAppContext::single();
    cx.update(|cx| {
        cx.set_global(IconRenderer::new(IconCacheLimits {
            raster_entries: 1,
            ..IconCacheLimits::default()
        }));
    });
    let first_image = std::rc::Rc::new(std::cell::RefCell::new(None));
    let holder = first_image.clone();
    let window = cx.add_window(move |_, _| EvictionFixture {
        first_image: holder,
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.update_window(window.into(), |_, window, _| {
        assert!(window.has_image_atlas_entry(first_image.borrow().as_ref().unwrap()));
    })
    .unwrap();
}

struct CachedIconView {
    render_count: std::rc::Rc<std::cell::Cell<usize>>,
}

impl gpui::Render for CachedIconView {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        self.render_count.set(self.render_count.get() + 1);
        icon(IconName::AlertTwoTone)
    }
}

struct CachedIconHost {
    child: gpui::Entity<CachedIconView>,
    show_child: bool,
}

impl gpui::Render for CachedIconHost {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        let mut root = gpui::div().flex();
        if self.show_child {
            let mut sizing = gpui::div().w(px(16.)).h(px(16.));
            root = root.child(self.child.clone().cached(sizing.style().clone()));
        }
        root.child(icon(IconName::AccountBookTwoTone))
    }
}

#[test]
fn cached_view_leases_survive_lru_eviction_reuse_without_rasterization_and_release_on_unmount() {
    let mut cx = gpui::TestAppContext::single();
    cx.update(|cx| {
        cx.set_global(IconRenderer::new(IconCacheLimits {
            raster_entries: 1,
            raster_bytes: 64 * 1024,
            ..IconCacheLimits::default()
        }));
    });
    let count = std::rc::Rc::new(std::cell::Cell::new(0));
    let child_count = count.clone();
    let window = cx.add_window(move |_, cx| CachedIconHost {
        child: cx.new(|_| CachedIconView {
            render_count: child_count,
        }),
        show_child: true,
    });
    let draw = |cx: &mut gpui::TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    };
    draw(&mut cx);
    assert_eq!(count.get(), 1);
    let first = cx
        .update_window(window.into(), |_, window, cx| {
            let cache = cx.global::<IconRenderer>();
            assert_eq!(
                cache.statistics().raster_bytes,
                0,
                "paint retains no idle rasters"
            );
            assert!(cache.statistics().resident_bytes > 0);
            assert_eq!(
                cache.statistics().parsed_entries,
                0,
                "paint retains no idle parsed SVG"
            );
            let first = cache
                .resident
                .iter()
                .find(|(key, _)| {
                    key.parsed.source
                        == SourceKey::Embedded(IconName::AlertTwoTone.descriptor()
                            as *const IconDescriptor
                            as usize)
                })
                .and_then(|(_, resource)| resource.upgrade())
                .unwrap()
                .image
                .clone();
            assert!(window.has_image_atlas_entry(&first));
            first
        })
        .unwrap();
    window
        .root(&mut cx)
        .unwrap()
        .update(&mut cx, |_, cx| cx.notify());
    draw(&mut cx);
    assert_eq!(
        count.get(),
        1,
        "cached scene should replay without calling its view renderer"
    );
    cx.update_window(window.into(), |_, window, cx| {
        assert!(window.has_image_atlas_entry(&first));
        assert_eq!(cx.global::<IconRenderer>().statistics().raster_misses, 2);
    })
    .unwrap();
    let child = window
        .root(&mut cx)
        .unwrap()
        .read_with(&cx, |view, _| view.child.clone());
    child.update(&mut cx, |_, cx| cx.notify());
    draw(&mut cx);
    assert_eq!(count.get(), 2);
    cx.update_window(window.into(), |_, window, cx| {
        assert!(window.has_image_atlas_entry(&first));
        assert_eq!(cx.global::<IconRenderer>().statistics().raster_misses, 2,
            "active view-owned images must be reused when the CPU budget is smaller than the visible working set");
    }).unwrap();
    window.root(&mut cx).unwrap().update(&mut cx, |view, cx| {
        view.show_child = false;
        cx.notify();
    });
    draw(&mut cx);
    cx.update_window(window.into(), |_, window, cx| {
        assert!(
            !window.has_image_atlas_entry(&first),
            "unmounted cached scene must release its atlas resource"
        );
        let stats = cx.global::<IconRenderer>().statistics();
        assert_eq!(stats.raster_bytes, 0);
        assert_eq!(
            stats.resident_rasters, 1,
            "the visible sibling keeps its lease"
        );
    })
    .unwrap();
}

#[test]
fn exported_icons_shared_with_cached_images_keep_native_assets_until_the_last_scene() {
    use std::{cell::Cell, rc::Rc};
    struct IconView {
        shown: bool,
    }
    impl gpui::Render for IconView {
        fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
            if self.shown {
                icon(IconRef::custom(&FULL_COLOR).unwrap()).into_any_element()
            } else {
                gpui::Empty.into_any_element()
            }
        }
    }
    struct ImageView {
        image: gpui::Entity<crate::ImageState>,
        shown: bool,
        renders: Rc<Cell<usize>>,
    }
    impl gpui::Render for ImageView {
        fn render(&mut self, _: &mut Window, cx: &mut gpui::Context<Self>) -> impl IntoElement {
            self.renders.set(self.renders.get() + 1);
            if self.shown {
                crate::Image::new("exported-icon-image", &self.image, cx)
                    .preview(false)
                    .w(px(16.))
                    .h(px(16.))
                    .into_any_element()
            } else {
                gpui::Empty.into_any_element()
            }
        }
    }
    struct ImageHost {
        child: gpui::Entity<ImageView>,
    }
    impl gpui::Render for ImageHost {
        fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
            let mut sizing = gpui::div().w(px(16.)).h(px(16.));
            self.child.clone().cached(sizing.style().clone())
        }
    }
    for native_icon in [false, true] {
        let mut cx = gpui::TestAppContext::single();
        let image = cx.update(|cx| {
            crate::init(cx);
            cx.set_global(IconRenderer::default());
            let svg = cx.svg_renderer();
            let assets = cx.asset_source().clone();
            cx.update_global::<IconRenderer, _>(|cache, _| {
                cache
                    .rasterize(
                        &svg,
                        &*assets,
                        &IconRef::custom(&FULL_COLOR).unwrap(),
                        &IconRenderOptions::default(),
                    )
                    .unwrap()
            })
        });
        let icon_window = cx.add_window(|_, _| IconView { shown: native_icon });
        let renders = Rc::new(Cell::new(0));
        let image_window = cx.add_window(|window, cx| {
            let picture = cx.new(|cx| crate::ImageState::new(image.clone(), window, cx));
            ImageHost {
                child: cx.new(|_| ImageView {
                    image: picture,
                    shown: true,
                    renders: renders.clone(),
                }),
            }
        });
        let draw = |cx: &mut gpui::TestAppContext, window: gpui::AnyWindowHandle| {
            cx.update_window(window, |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx, image_window.into());
        draw(&mut cx, image_window.into());
        draw(&mut cx, icon_window.into());
        cx.update_window(image_window.into(), |_, window, _| {
            assert!(window.has_image_atlas_entry(&image));
        })
        .unwrap();
        let count = renders.get();
        if native_icon {
            // Verify the fixture actually shares one image across the two APIs.
            cx.update(|cx| {
                assert!(
                    cx.global::<IconRenderer>()
                        .resident
                        .values()
                        .filter_map(std::sync::Weak::upgrade)
                        .any(|record| record.image.id == image.id)
                )
            });
            icon_window
                .update(&mut cx, |view, _, cx| {
                    view.shown = false;
                    cx.notify();
                })
                .unwrap();
            draw(&mut cx, icon_window.into());
            draw(&mut cx, image_window.into());
            assert_eq!(
                renders.get(),
                count,
                "the Image user really replays a cached scene"
            );
        } else {
            // The export cache does not own the independent Image scene's lease.
            cx.update_window(icon_window.into(), |_, window, cx| {
                clear_icon_cache(window, cx)
            })
            .unwrap();
            cx.run_until_parked();
        }
        cx.update_window(image_window.into(), |_, window, _| {
            assert!(
                window.has_image_atlas_entry(&image),
                "icon retirement/cache clear must preserve the cached Image user ({native_icon})"
            );
        })
        .unwrap();
        let child = image_window
            .root(&mut cx)
            .unwrap()
            .read_with(&cx, |host, _| host.child.clone());
        child.update(&mut cx, |view, cx| {
            view.shown = false;
            cx.notify();
        });
        draw(&mut cx, image_window.into());
        cx.update_window(image_window.into(), |_, window, _| {
            assert!(
                !window.has_image_atlas_entry(&image),
                "the final scene releases its atlas asset"
            );
        })
        .unwrap();
    }
}

#[test]
fn painted_icons_survive_weak_lookup_pressure_and_retire_after_clear_and_unmount() {
    use std::{cell::RefCell, rc::Rc};

    struct PressureIcons {
        shown: bool,
        records: Rc<RefCell<Vec<Weak<IconImageResource>>>>,
    }
    impl gpui::Render for PressureIcons {
        fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
            let shown = self.shown;
            let records = self.records.clone();
            canvas(
                |_, _, _| (),
                move |bounds, (), window, cx| {
                    if !shown {
                        return;
                    }
                    let assets = cx.asset_source().clone();
                    cx.update_global::<IconRenderer, _>(|renderer, cx| {
                        for index in 0..513 {
                            let icon = IconRef::custom(&TWO_TONE).unwrap();
                            let options = IconRenderOptions {
                                size: bounds.size,
                                scale_factor: window.scale_factor(),
                                colors: IconPalette {
                                    primary: rgba((index as u32) << 8 | 0xff),
                                    ..IconPalette::default()
                                },
                                ..IconRenderOptions::default()
                            };
                            renderer.paint(bounds, &icon, &options, window, cx).unwrap();
                            if records.borrow().len() == index {
                                let key = renderer.raster_key(&*assets, &icon, &options).unwrap();
                                records.borrow_mut().push(renderer.resident[&key].clone());
                            }
                        }
                    });
                },
            )
            .w(px(8.))
            .h(px(8.))
        }
    }

    let mut cx = gpui::TestAppContext::single();
    cx.update(|cx| cx.set_global(IconRenderer::default()));
    let records = Rc::new(RefCell::new(Vec::new()));
    let window = cx.add_window(|_, _| PressureIcons {
        shown: true,
        records: records.clone(),
    });
    let draw = |cx: &mut gpui::TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
    };
    draw(&mut cx);
    let original_records = records.borrow().clone();
    let original_images: Vec<_> = original_records
        .iter()
        .map(|record| record.upgrade().unwrap().image.clone())
        .collect();
    assert_eq!(original_records.len(), 513);
    for _ in 0..3 {
        window.update(&mut cx, |_, _, cx| cx.notify()).unwrap();
        draw(&mut cx);
    }
    cx.update_window(window.into(), |_, window, cx| {
        let renderer = cx.global::<IconRenderer>();
        let stats = renderer.statistics();
        assert_eq!(stats.raster_misses, 513);
        assert_eq!(stats.parse_misses, 513);
        assert!(stats.raster_hits >= 3 * 513);
        assert_eq!(renderer.resident.len(), 512);
        assert_eq!(stats.raster_entries, 0);
        assert_eq!(stats.parsed_entries, 0);
        for (record, image) in original_records.iter().zip(&original_images) {
            assert_eq!(record.upgrade().unwrap().image.id, image.id);
            assert!(window.has_image_atlas_entry(image));
        }
        records.borrow_mut().clear();
        clear_icon_cache(window, cx);
    })
    .unwrap();
    draw(&mut cx);
    assert!(
        original_records
            .iter()
            .all(|record| record.upgrade().is_none())
    );
    cx.update_window(window.into(), |_, window, cx| {
        let stats = cx.global::<IconRenderer>().statistics();
        assert_eq!(stats.raster_misses, 2 * 513);
        assert_eq!(stats.parse_misses, 2 * 513);
        assert!(
            original_images
                .iter()
                .all(|image| !window.has_image_atlas_entry(image))
        );
    })
    .unwrap();
    let replacement_images: Vec<_> = records
        .borrow()
        .iter()
        .map(|record| record.upgrade().unwrap().image.clone())
        .collect();
    window
        .update(&mut cx, |view, _, cx| {
            view.shown = false;
            cx.notify();
        })
        .unwrap();
    draw(&mut cx);
    assert!(
        records
            .borrow()
            .iter()
            .all(|record| record.upgrade().is_none())
    );
    cx.update_window(window.into(), |_, window, cx| {
        assert!(
            replacement_images
                .iter()
                .all(|image| !window.has_image_atlas_entry(image))
        );
        assert_eq!(cx.global::<IconRenderer>().statistics().resident_rasters, 0);
    })
    .unwrap();
}
