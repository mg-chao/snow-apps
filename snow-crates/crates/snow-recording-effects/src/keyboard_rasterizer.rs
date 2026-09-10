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
    use windows::core::w;

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
        draw: ID2D1Factory,
        imaging: IWICImagingFactory,
        config: KeyboardOverlayConfig,
        // COM objects must be released before uninitializing their apartment.
        _apartment: ComApartment,
    }

    pub fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
        let native = || -> windows::core::Result<Rasterizer> {
            unsafe {
                CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
                let apartment = ComApartment;
                Ok(Rasterizer {
                    write: DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)?,
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
        native()
            .and_then(|value| {
                // Validate the complete rendering path before recording startup reports success.
                value.render("M")?;
                Ok(value)
            })
            .map(|value| Box::new(value) as Box<dyn KeycapRasterizer>)
            .map_err(|e| e.to_string())
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
                    w!("Segoe UI"),
                    None,
                    DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    32.0,
                    w!(""),
                )?;
                format.SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)?;
                let text: Vec<_> = label.encode_utf16().collect();
                let layout = self.write.CreateTextLayout(&text, &format, 4096.0, 256.0)?;
                let mut metrics = DWRITE_TEXT_METRICS::default();
                layout.GetMetrics(&mut metrics)?;
                // Sublinear growth keeps long legends compact, with equal padding for
                // single-character and multi-character keys. Fit glyphs into that width.
                let measured = metrics.widthIncludingTrailingWhitespace.max(1.0);
                let content_width = if measured <= 32.0 {
                    measured
                } else {
                    32.0 * (measured / 32.0).powf(0.85)
                };
                layout.SetFontSize(
                    32.0 * content_width / measured,
                    DWRITE_TEXT_RANGE {
                        startPosition: 0,
                        length: text.len() as u32,
                    },
                )?;
                let width = (content_width + 40.0).ceil() as u32;
                layout.SetMaxWidth(width as f32)?;
                layout.SetMaxHeight(KEYCAP_SIZE as f32)?;
                layout.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)?;
                layout.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)?;
                Ok((layout, width))
            }
        }

        fn render(&self, label: &str) -> windows::core::Result<Keycap> {
            unsafe {
                let (layout, width) = self.text_layout(label)?;
                let height = KEYCAP_SIZE;
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
                        dpiX: 96.0,
                        dpiY: 96.0,
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
                        right: width as f32 - 1.0,
                        bottom: height as f32 - 1.0,
                    },
                    radiusX: 12.0,
                    radiusY: 12.0,
                };
                target.FillRoundedRectangle(&rect, &background);
                target.DrawRoundedRectangle(&rect, &border, 2.0, None);
                target.DrawTextLayout(
                    Default::default(),
                    &layout,
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
        fn rasterize(&mut self, label: &str, _: f32) -> Result<Keycap, String> {
            self.render(label).map_err(|e| e.to_string())
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn native_keycaps_fit_label_width_and_keep_fixed_height() {
            let config = KeyboardOverlayConfig {
                background_rgba: [0, 0, 0, 255],
                border_rgba: [20, 20, 20, 255],
                text_rgba: [255, 255, 255, 255],
                labels: Default::default(),
            };
            let mut rasterizer = create(&config).unwrap();
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
                        (reference.width, 64),
                        "{label} at {scale}"
                    );
                    assert_eq!(cap.pixels, reference.pixels, "{label} at {scale}");
                }
            }
        }
    }
}

#[cfg(not(windows))]
mod platform {
    use super::*;
    pub fn create(_: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
        Err("keyboard rendering is supported only on Windows".into())
    }
}
