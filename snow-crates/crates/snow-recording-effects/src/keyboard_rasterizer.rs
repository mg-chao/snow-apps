//! Native, cached grayscale text rendering. No font files or GUI thread dependency.
use crate::keyboard_overlay::{KeyboardOverlayConfig, KeycapRasterizer};

pub fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
    platform::create(config)
}

#[cfg(windows)]
mod platform {
    use super::*;
    use crate::keyboard_overlay::{KEYCAP_SIZE, Keycap};
    use windows::Win32::Graphics::Direct2D::Common::*;
    use windows::Win32::Graphics::Direct2D::*;
    use windows::Win32::Graphics::DirectWrite::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
    use windows::Win32::Graphics::Imaging::*;
    use windows::Win32::System::Com::*;
    use windows::Win32::UI::WindowsAndMessaging::{
        NONCLIENTMETRICSW, SPI_GETNONCLIENTMETRICS, SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS,
        SystemParametersInfoW,
    };
    use windows::core::{Interface, PCWSTR, w};

    struct ComApartment;
    impl Drop for ComApartment {
        fn drop(&mut self) {
            unsafe {
                CoUninitialize();
            }
        }
    }
    struct Rasterizer {
        write: IDWriteFactory,
        collection: Option<IDWriteFontCollection>,
        fallback: IDWriteFontFallback,
        family: Vec<u16>,
        weight: DWRITE_FONT_WEIGHT,
        draw: ID2D1Factory,
        imaging: IWICImagingFactory,
        config: KeyboardOverlayConfig,
        // COM objects must be released before uninitializing their apartment.
        _apartment: ComApartment,
    }

    pub fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
        create_native(config, None)
            .map(|value| Box::new(value) as Box<dyn KeycapRasterizer>)
            .map_err(|e| e.to_string())
    }

    fn create_native(
        config: &KeyboardOverlayConfig,
        collection: Option<IDWriteFontCollection>,
    ) -> windows::core::Result<Rasterizer> {
        let native = || -> windows::core::Result<Rasterizer> {
            unsafe {
                CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
                let apartment = ComApartment;
                let write: IDWriteFactory2 = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)?;
                let fallback = write.CreateFontFallbackBuilder()?;
                let (family, weight) = if let Some(font) = &config.font {
                    // Qt resolves the CJK fallback from the same application font
                    // as the rest of Snow Shot. Apply it before measuring labels.
                    let cjk: Vec<u16> = font.cjk_family.encode_utf16().chain(Some(0)).collect();
                    fallback.AddMapping(
                        &[
                            DWRITE_UNICODE_RANGE {
                                first: 0x3000,
                                last: 0x303f,
                            },
                            DWRITE_UNICODE_RANGE {
                                first: 0x3400,
                                last: 0x9fff,
                            },
                            DWRITE_UNICODE_RANGE {
                                first: 0xf900,
                                last: 0xfaff,
                            },
                            DWRITE_UNICODE_RANGE {
                                first: 0x20000,
                                last: 0x323af,
                            },
                        ],
                        &[cjk.as_ptr()],
                        collection.as_ref(),
                        None,
                        None,
                        1.0,
                    )?;
                    (
                        font.family.encode_utf16().chain(Some(0)).collect(),
                        font.weight as i32,
                    )
                } else {
                    // Standalone/older callers inherit the current Windows UI font.
                    let mut metrics = NONCLIENTMETRICSW {
                        cbSize: std::mem::size_of::<NONCLIENTMETRICSW>() as u32,
                        ..Default::default()
                    };
                    SystemParametersInfoW(
                        SPI_GETNONCLIENTMETRICS,
                        metrics.cbSize,
                        Some((&raw mut metrics).cast()),
                        SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS(0),
                    )?;
                    (
                        metrics.lfMessageFont.lfFaceName.to_vec(),
                        metrics.lfMessageFont.lfWeight,
                    )
                };
                fallback.AddMappings(&write.GetSystemFontFallback()?)?;
                Ok(Rasterizer {
                    fallback: fallback.CreateFontFallback()?,
                    family,
                    weight: DWRITE_FONT_WEIGHT(weight),
                    collection,
                    write: write.cast()?,
                    draw: D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, None)?,
                    imaging: CoCreateInstance(
                        &CLSID_WICImagingFactory,
                        None,
                        CLSCTX_INPROC_SERVER,
                    )?,
                    config: config.clone(),
                    _apartment: apartment,
                })
            }
        };
        native().and_then(|value| {
            // Validate the complete rendering path before recording startup reports success.
            value.render("M", config.keycap_size as f32 / KEYCAP_SIZE as f32)?;
            Ok(value)
        })
    }

    fn color(rgba: [u8; 4]) -> D2D1_COLOR_F {
        D2D1_COLOR_F {
            r: f32::from(rgba[0]) / 255.0,
            g: f32::from(rgba[1]) / 255.0,
            b: f32::from(rgba[2]) / 255.0,
            a: f32::from(rgba[3]) / 255.0,
        }
    }

    impl Rasterizer {
        fn text_layout(&self, label: &str) -> windows::core::Result<(IDWriteTextLayout, u32)> {
            unsafe {
                let format = self.write.CreateTextFormat(
                    PCWSTR(self.family.as_ptr()),
                    self.collection.as_ref(),
                    self.weight,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    snow_core::keycap_layout::FONT_SIZE,
                    w!(""),
                )?;
                format.SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)?;
                let text: Vec<_> = label.encode_utf16().collect();
                let layout = self.write.CreateTextLayout(&text, &format, 4096.0, 256.0)?;
                layout
                    .cast::<IDWriteTextLayout2>()?
                    .SetFontFallback(&self.fallback)?;
                let mut metrics = DWRITE_TEXT_METRICS::default();
                layout.GetMetrics(&mut metrics)?;
                let (width, font_size) =
                    snow_core::keycap_layout::fit(metrics.widthIncludingTrailingWhitespace);
                layout.SetFontSize(
                    font_size,
                    DWRITE_TEXT_RANGE {
                        startPosition: 0,
                        length: text.len() as u32,
                    },
                )?;
                layout.SetMaxWidth(width as f32)?;
                layout.SetMaxHeight(KEYCAP_SIZE as f32)?;
                layout.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)?;
                layout.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)?;
                Ok((layout, width))
            }
        }

        fn render(&self, label: &str, scale: f32) -> windows::core::Result<Keycap> {
            let (layout, width) = self.text_layout(label)?;
            self.render_layout(&layout, width, scale)
        }

        fn render_layout(
            &self,
            layout: &IDWriteTextLayout,
            width: u32,
            scale: f32,
        ) -> windows::core::Result<Keycap> {
            unsafe {
                let logical_width = width;
                let width = (width as f32 * scale).ceil() as u32;
                let height = (KEYCAP_SIZE as f32 * scale).round() as u32;
                let bitmap = self.imaging.CreateBitmap(
                    width,
                    height,
                    &GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapCacheOnLoad,
                )?;
                let target = self.draw.CreateWicBitmapRenderTarget(
                    &bitmap,
                    &D2D1_RENDER_TARGET_PROPERTIES {
                        r#type: D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                        pixelFormat: D2D1_PIXEL_FORMAT {
                            format: DXGI_FORMAT_B8G8R8A8_UNORM,
                            alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
                        },
                        dpiX: 96.0 * scale,
                        dpiY: 96.0 * scale,
                        ..Default::default()
                    },
                )?;
                let background =
                    target.CreateSolidColorBrush(&color(self.config.background_rgba), None)?;
                let border = target.CreateSolidColorBrush(&color(self.config.border_rgba), None)?;
                let foreground =
                    target.CreateSolidColorBrush(&color(self.config.text_rgba), None)?;
                target.BeginDraw();
                target.Clear(Some(&D2D1_COLOR_F::default()));
                target.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                let rect = D2D1_ROUNDED_RECT {
                    rect: D2D_RECT_F {
                        left: 1.0,
                        top: 1.0,
                        right: logical_width as f32 - 1.0,
                        bottom: KEYCAP_SIZE as f32 - 1.0,
                    },
                    radiusX: 12.0,
                    radiusY: 12.0,
                };
                target.FillRoundedRectangle(&rect, &background);
                target.DrawRoundedRectangle(&rect, &border, 2.0, None);
                target.DrawTextLayout(
                    Default::default(),
                    layout,
                    &foreground,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                );
                target.EndDraw(None, None)?;
                let mut pixels = vec![0; width as usize * height as usize * 4];
                bitmap.CopyPixels(std::ptr::null(), width * 4, &mut pixels)?;
                for pixel in pixels.chunks_exact_mut(4) {
                    pixel.swap(0, 2);
                }
                Ok(Keycap {
                    width,
                    height,
                    pixels,
                })
            }
        }
    }
    impl KeycapRasterizer for Rasterizer {
        fn rasterize(&mut self, label: &str, scale: f32) -> Result<Keycap, String> {
            self.render(label, scale.clamp(0.5, 2.0))
                .map_err(|e| e.to_string())
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use std::os::windows::ffi::OsStrExt;

        fn fixture_rasterizer(config: &KeyboardOverlayConfig) -> Rasterizer {
            // An isolated font collection exercises the real DirectWrite path,
            // including missing-glyph fallback, without installed-language dependencies.
            unsafe {
                let write: IDWriteFactory5 =
                    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED).unwrap();
                let builder = write.CreateFontSetBuilder().unwrap();
                for family in ["Sans", "Mono", "Han"] {
                    for style in ["Regular", "Bold"] {
                        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                            .join("../../../test-support/fonts")
                            .join(format!("SnowRecordingTest{family}-{style}.ttf"));
                        let path: Vec<u16> =
                            path.as_os_str().encode_wide().chain(Some(0)).collect();
                        builder
                            .AddFontFile(
                                &write
                                    .CreateFontFileReference(PCWSTR(path.as_ptr()), None)
                                    .unwrap(),
                            )
                            .unwrap();
                    }
                }
                let collection = write
                    .CreateFontCollectionFromFontSet(&builder.CreateFontSet().unwrap())
                    .unwrap()
                    .cast()
                    .unwrap();
                create_native(config, Some(collection)).unwrap()
            }
        }

        #[test]
        fn chinese_labels_render_with_one_ui_font() {
            let config = KeyboardOverlayConfig {
                font: Some(
                    crate::keyboard_overlay::KeyboardOverlayFont::new(
                        "Snow Recording Test Sans",
                        "Snow Recording Test Han",
                        400,
                    )
                    .unwrap(),
                ),
                keycap_size: 64,
                background_rgba: [0; 4],
                border_rgba: [0; 4],
                text_rgba: [255; 4],
                labels: Default::default(),
            };
            let rasterizer = fixture_rasterizer(&config);
            // Compare fallback pixels against an explicitly selected fixture
            // family, without a golden image tied to a DirectWrite version.
            for (label, family, start) in [
                ("鼠标左键", w!("Snow Recording Test Han"), 0),
                ("鼠標右鍵", w!("Snow Recording Test Han"), 0),
                ("空格退格鍵", w!("Snow Recording Test Han"), 0),
                ("Ctrl鼠标左键", w!("Snow Recording Test Han"), 4),
                ("\u{20000}", w!("Snow Recording Test Han"), 0),
                ("Ctrl", w!("Snow Recording Test Sans"), 0),
            ] {
                for scale in [0.5, 1.0, 1.5, 2.0] {
                    let actual = rasterizer.render(label, scale).unwrap();
                    let (layout, width) = rasterizer.text_layout(label).unwrap();
                    unsafe {
                        layout
                            .cast::<IDWriteTextLayout2>()
                            .unwrap()
                            .SetFontFallback(None)
                            .unwrap();
                        layout
                            .SetFontFamilyName(
                                family,
                                DWRITE_TEXT_RANGE {
                                    startPosition: start,
                                    length: label.encode_utf16().count() as u32 - start,
                                },
                            )
                            .unwrap();
                    }
                    let expected = rasterizer.render_layout(&layout, width, scale).unwrap();
                    assert!(actual.pixels.iter().any(|value| *value != 0));
                    assert_eq!(
                        (actual.width, actual.height),
                        (expected.width, expected.height)
                    );
                    assert!(
                        actual.pixels == expected.pixels,
                        "{label} at {scale} must use a consistent UI font"
                    );
                }
            }
        }

        #[test]
        fn application_font_family_and_weight_control_rendered_glyphs() {
            for (family, cjk_family, weight) in [
                ("Snow Recording Test Sans", "Snow Recording Test Han", 400),
                ("Snow Recording Test Mono", "Snow Recording Test Han", 700),
            ] {
                let config = KeyboardOverlayConfig {
                    font: Some(
                        crate::keyboard_overlay::KeyboardOverlayFont::new(
                            family, cjk_family, weight,
                        )
                        .unwrap(),
                    ),
                    keycap_size: 64,
                    background_rgba: [0; 4],
                    border_rgba: [0; 4],
                    text_rgba: [255; 4],
                    labels: Default::default(),
                };
                let rasterizer = fixture_rasterizer(&config);
                for (label, expected_family) in [("Ctrl", family), ("鼠标左键", cjk_family)] {
                    let actual = rasterizer.render(label, 1.0).unwrap();
                    let (layout, width) = rasterizer.text_layout(label).unwrap();
                    let name: Vec<_> = expected_family.encode_utf16().chain(Some(0)).collect();
                    let range = DWRITE_TEXT_RANGE {
                        startPosition: 0,
                        length: label.encode_utf16().count() as u32,
                    };
                    unsafe {
                        layout
                            .cast::<IDWriteTextLayout2>()
                            .unwrap()
                            .SetFontFallback(None)
                            .unwrap();
                        layout
                            .SetFontFamilyName(PCWSTR(name.as_ptr()), range)
                            .unwrap();
                        layout
                            .SetFontWeight(DWRITE_FONT_WEIGHT(weight as i32), range)
                            .unwrap();
                    }
                    let expected = rasterizer.render_layout(&layout, width, 1.0).unwrap();
                    assert!(
                        actual.pixels == expected.pixels,
                        "{label}: {expected_family}, {weight}"
                    );
                }
            }
        }

        #[test]
        fn changing_font_family_or_weight_changes_pixels() {
            let render = |family, weight, label| {
                let config = KeyboardOverlayConfig {
                    font: Some(
                        crate::keyboard_overlay::KeyboardOverlayFont::new(
                            family,
                            "Snow Recording Test Han",
                            weight,
                        )
                        .unwrap(),
                    ),
                    keycap_size: 64,
                    background_rgba: [0; 4],
                    border_rgba: [0; 4],
                    text_rgba: [255; 4],
                    labels: Default::default(),
                };
                fixture_rasterizer(&config)
                    .render(label, 1.0)
                    .unwrap()
                    .pixels
            };
            assert_ne!(
                render("Snow Recording Test Sans", 400, "WWW"),
                render("Snow Recording Test Mono", 400, "WWW")
            );
            for label in ["Ctrl", "鼠标左键", "Ctrl\u{20000}"] {
                assert_ne!(
                    render("Snow Recording Test Sans", 400, label),
                    render("Snow Recording Test Sans", 700, label)
                );
            }
        }

        #[test]
        fn native_keycaps_fit_label_width_and_keep_fixed_height() {
            let config = KeyboardOverlayConfig {
                font: Some(
                    crate::keyboard_overlay::KeyboardOverlayFont::new(
                        "Snow Recording Test Sans",
                        "Snow Recording Test Han",
                        400,
                    )
                    .unwrap(),
                ),
                keycap_size: 64,
                background_rgba: [0, 0, 0, 255],
                border_rgba: [20, 20, 20, 255],
                text_rgba: [255, 255, 255, 255],
                labels: Default::default(),
            };
            let mut rasterizer = fixture_rasterizer(&config);
            for size in [32, 64, 96, 128] {
                let cap = rasterizer.rasterize("Ctrl", size as f32 / 64.0).unwrap();
                assert_eq!(cap.height, size);
                assert_eq!(cap.pixels.len(), (cap.width * cap.height * 4) as usize);
                assert!(cap.pixels.chunks_exact(4).any(|p| p[0] > 100));
            }
            let widths: Vec<_> = ["WW", "WWWW", "WWWWWW"]
                .iter()
                .map(|label| rasterizer.rasterize(label, 1.0).unwrap().width)
                .collect();
            assert!(widths[0] < widths[1] && widths[1] < widths[2]);
            assert!(
                widths[2] - widths[1] < widths[1] - widths[0],
                "equal text increments must produce diminishing width growth"
            );
            for label in [
                "A",
                "Ctrl",
                "AltGr",
                "Backspace",
                "Page Down",
                "Previous Track",
                "Num Separator",
                "空格",
                "退格鍵",
                "←",
            ] {
                let reference = rasterizer.rasterize(label, 1.0).unwrap();
                assert_eq!(reference.height, 64, "{label}");
                assert!(reference.width > 40, "{label}");
                if matches!(label, "A" | "←") {
                    assert!(reference.width < 72, "short labels have no square minimum");
                } else if matches!(label, "Backspace" | "Page Down" | "Previous Track") {
                    assert!(reference.width > 64, "long labels must expand: {label}");
                }
                let width = reference.width as usize;
                assert_eq!(reference.pixels.len(), width * 64 * 4);
                let mut glyph_pixels = 0;
                for (index, pixel) in reference.pixels.chunks_exact(4).enumerate() {
                    assert!(pixel[..3].iter().all(|channel| *channel <= pixel[3]));
                    if pixel[0] > 100 {
                        glyph_pixels += 1;
                        let (x, y) = (index % width, index / width);
                        assert!(
                            (18..width - 18).contains(&x) && (6..58).contains(&y),
                            "{label}: {x}, {y}"
                        );
                    }
                }
                assert!(glyph_pixels > 0, "the label must remain visible: {label}");
                if label == "Page Down" {
                    let glyph_rows: Vec<_> = reference
                        .pixels
                        .chunks_exact(width * 4)
                        .map(|row| row.chunks_exact(4).any(|p| p[0] > 100))
                        .collect();
                    let lines = glyph_rows
                        .windows(2)
                        .filter(|pair| !pair[0] && pair[1])
                        .count();
                    assert_eq!(lines, 1, "multiword labels should remain on one line");
                }
                for scale in [0.5, 2.0, 4.0, 8.0] {
                    let cap = rasterizer.rasterize(label, scale).unwrap();
                    assert_eq!(
                        (cap.width, cap.height),
                        (
                            (reference.width as f32 * scale.clamp(0.5, 2.0)).ceil() as u32,
                            (64.0 * scale.clamp(0.5, 2.0)) as u32
                        ),
                        "{label} at {scale}"
                    );
                    assert_eq!(cap.pixels.len(), (cap.width * cap.height * 4) as usize);
                }
            }
        }
    }
}

#[cfg(not(any(windows, target_os = "macos")))]
mod platform {
    use super::*;
    pub fn create(_: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
        Err("keyboard rendering is supported only on Windows".into())
    }
}

#[cfg(target_os = "macos")]
mod platform {
    use super::*;
    struct Rasterizer(KeyboardOverlayConfig);
    impl KeycapRasterizer for Rasterizer {
        fn rasterize(
            &mut self,
            label: &str,
            scale: f32,
        ) -> Result<crate::keyboard_overlay::Keycap, String> {
            let image = snow_macos::text::keycap_with_font(
                label,
                scale,
                self.0.background_rgba,
                self.0.text_rgba,
                self.0.border_rgba,
                self.0
                    .font
                    .as_ref()
                    .map(|font| snow_macos::text::KeycapFont {
                        family: &font.family,
                        cjk_family: &font.cjk_family,
                        weight: font.weight,
                    }),
            )
            .map_err(|e| e.to_string())?;
            Ok(crate::keyboard_overlay::Keycap {
                width: image.width,
                height: image.height,
                pixels: image.rgba,
            })
        }
    }
    pub fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
        let mut rasterizer = Rasterizer(config.clone());
        rasterizer.rasterize("M", config.keycap_size as f32 / 64.0)?;
        Ok(Box::new(rasterizer))
    }
}
