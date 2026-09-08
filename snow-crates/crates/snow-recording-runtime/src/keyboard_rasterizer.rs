//! Native, cached grayscale text rendering. No font files or GUI thread dependency.
use crate::keyboard_overlay::{KeyboardOverlayConfig, KeycapRasterizer};

pub(crate) fn create(config: &KeyboardOverlayConfig) -> Result<Box<dyn KeycapRasterizer>, String> {
    platform::create(config)
}

#[cfg(windows)]
mod platform {
    use super::*;
    use crate::keyboard_overlay::Keycap;
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
                value.render("M", 1.0)?;
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
        fn render(&self, label: &str, scale: f32) -> windows::core::Result<Keycap> {
            unsafe {
                let format = self.write.CreateTextFormat(
                    w!("Segoe UI"),
                    None,
                    DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    22.0 * scale,
                    w!(""),
                )?;
                format.SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)?;
                let text: Vec<_> = label.encode_utf16().collect();
                let layout = self
                    .write
                    .CreateTextLayout(&text, &format, 4096.0, 256.0 * scale)?;
                let mut metrics = DWRITE_TEXT_METRICS::default();
                layout.GetMetrics(&mut metrics)?;
                let width = (metrics.widthIncludingTrailingWhitespace + 24.0 * scale)
                    .ceil()
                    .max(40.0 * scale) as u32;
                let height = (40.0 * scale).ceil() as u32;
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
                        left: 0.5,
                        top: 0.5,
                        right: width as f32 - 0.5,
                        bottom: height as f32 - 0.5,
                    },
                    radiusX: 8.0 * scale,
                    radiusY: 8.0 * scale,
                };
                target.FillRoundedRectangle(&rect, &background);
                target.DrawRoundedRectangle(&rect, &border, scale.max(1.0), None);
                format.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)?;
                format.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)?;
                target.DrawText(
                    &text,
                    &format,
                    &rect.rect,
                    &foreground,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
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
            self.render(label, scale).map_err(|e| e.to_string())
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
