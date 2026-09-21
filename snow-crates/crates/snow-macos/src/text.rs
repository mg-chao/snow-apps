//! Offscreen CoreText/CoreGraphics keycap rendering with system font fallback.
use crate::{MacError, MacResult};
use objc2_core_foundation::{
    CFArray, CFAttributedString, CFDictionary, CFNumber, CFRetained, CFString, CFType, CGPoint,
    CGRect, CGSize,
};
use objc2_core_graphics::*;
use objc2_core_text::*;

pub struct KeycapImage {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

/// Application-resolved families and an OpenType weight, copied into CoreText descriptors.
#[derive(Clone, Copy)]
pub struct KeycapFont<'a> {
    pub family: &'a str,
    pub cjk_family: &'a str,
    pub weight: u32,
}

fn coretext_weight(weight: u32) -> f64 {
    use objc2_app_kit::{
        NSFontWeightBlack, NSFontWeightBold, NSFontWeightHeavy, NSFontWeightLight,
        NSFontWeightMedium, NSFontWeightRegular, NSFontWeightSemibold, NSFontWeightThin,
        NSFontWeightUltraLight,
    };
    // OpenType and CoreText weights use different, non-linear scales. Interpolate
    // between Apple's named weights so arbitrary Qt weights retain their meaning.
    let anchors = unsafe {
        [
            (1, -1.0),
            (100, NSFontWeightUltraLight),
            (200, NSFontWeightThin),
            (300, NSFontWeightLight),
            (400, NSFontWeightRegular),
            (500, NSFontWeightMedium),
            (600, NSFontWeightSemibold),
            (700, NSFontWeightBold),
            (800, NSFontWeightHeavy),
            (900, NSFontWeightBlack),
            (999, 1.0),
        ]
    };
    let weight = weight.clamp(1, 999);
    let pair = anchors.windows(2).find(|pair| weight <= pair[1].0).unwrap();
    pair[0].1
        + (pair[1].1 - pair[0].1) * f64::from(weight - pair[0].0) / f64::from(pair[1].0 - pair[0].0)
}

fn keycap_font(style: Option<KeycapFont<'_>>, size: f64) -> MacResult<CFRetained<CTFont>> {
    unsafe {
        let Some(style) = style else {
            return CTFont::new_ui_font_for_language(CTFontUIFontType::System, size, None)
                .ok_or_else(|| MacError::Unsupported("system text font unavailable".into()));
        };
        if !(1..=999).contains(&style.weight)
            || [style.family, style.cjk_family].iter().any(|name| {
                name.trim().is_empty() || name.len() > 256 || name.chars().any(char::is_control)
            })
        {
            return Err(MacError::InvalidConfig(
                "invalid keyboard font family or weight".into(),
            ));
        }
        let traits = CFDictionary::<CFType, CFType>::from_slices(
            &[kCTFontWeightTrait.as_ref()],
            &[CFNumber::new_f64(coretext_weight(style.weight)).as_ref()],
        );
        let descriptor = |family: &str| {
            let attributes = CFDictionary::<CFType, CFType>::from_slices(
                &[
                    kCTFontFamilyNameAttribute.as_ref(),
                    kCTFontTraitsAttribute.as_ref(),
                ],
                &[CFString::from_str(family).as_ref(), traits.as_ref()],
            );
            CTFontDescriptor::with_attributes(attributes.as_opaque())
        };
        // CoreText consults this cascade when the primary face lacks a glyph,
        // then retains its native fallback for other scripts, like DirectWrite.
        let fallback = descriptor(style.cjk_family);
        let cascade = CFArray::<CTFontDescriptor>::from_objects(&[&fallback]);
        let attributes = CFDictionary::<CFType, CFType>::from_slices(
            &[kCTFontCascadeListAttribute.as_ref()],
            &[cascade.as_ref()],
        );
        let descriptor = descriptor(style.family).copy_with_attributes(attributes.as_opaque());
        Ok(CTFont::with_font_descriptor(
            &descriptor,
            size,
            std::ptr::null(),
        ))
    }
}

pub fn keycap(
    label: &str,
    scale: f32,
    background: [u8; 4],
    text: [u8; 4],
    border: [u8; 4],
) -> MacResult<KeycapImage> {
    keycap_with_font(label, scale, background, text, border, None)
}

pub fn keycap_with_font(
    label: &str,
    scale: f32,
    background: [u8; 4],
    text: [u8; 4],
    border: [u8; 4],
    font: Option<KeycapFont<'_>>,
) -> MacResult<KeycapImage> {
    if !scale.is_finite() || !(0.25..=4.0).contains(&scale) || label.len() > 1024 {
        return Err(MacError::InvalidConfig(
            "invalid keycap size or label".into(),
        ));
    }
    let scale = f64::from(scale);
    unsafe {
        let base_font = keycap_font(font, f64::from(snow_core::keycap_layout::FONT_SIZE))?;
        let color = CGColor::new_generic_rgb(
            f64::from(text[0]) / 255.0,
            f64::from(text[1]) / 255.0,
            f64::from(text[2]) / 255.0,
            f64::from(text[3]) / 255.0,
        );
        let make_line = |font: &CTFont| -> MacResult<CFRetained<CTLine>> {
            let attributes = CFDictionary::<CFType, CFType>::from_slices(
                &[
                    kCTFontAttributeName.as_ref(),
                    kCTForegroundColorAttributeName.as_ref(),
                ],
                &[font.as_ref(), color.as_ref()],
            );
            let value = CFAttributedString::new(
                None,
                Some(&CFString::from_str(label)),
                Some(attributes.as_opaque()),
            )
            .ok_or_else(|| MacError::Unsupported("text allocation failed".into()))?;
            Ok(CTLine::with_attributed_string(&value))
        };
        let base_line = make_line(&base_font)?;
        let measured = base_line.typographic_bounds(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        );
        let (logical_width, font_size) = snow_core::keycap_layout::fit(measured as f32);
        let fitted_font = keycap_font(font, f64::from(font_size) * scale)?;
        let line = make_line(&fitted_font)?;
        let mut ascent = 0.0;
        let mut descent = 0.0;
        let measured =
            line.typographic_bounds(&raw mut ascent, &raw mut descent, std::ptr::null_mut());
        let height = (f64::from(snow_core::keycap_layout::HEIGHT) * scale).round() as u32;
        let width = (f64::from(logical_width) * scale).ceil().min(4096.0) as u32;
        let mut rgba = vec![0; width as usize * height as usize * 4];
        let space = CGColorSpace::with_name(Some(kCGColorSpaceSRGB)).ok_or(MacError::Inactive)?;
        let context = CGBitmapContextCreate(
            rgba.as_mut_ptr().cast(),
            width as usize,
            height as usize,
            8,
            width as usize * 4,
            Some(&space),
            CGImageAlphaInfo::PremultipliedLast.0 | CGImageByteOrderInfo::Order32Big.0,
        )
        .ok_or_else(|| MacError::Unsupported("RGBA bitmap context unavailable".into()))?;
        let fill = |color: [u8; 4], inset: f64| {
            CGContext::set_rgb_fill_color(
                Some(&context),
                f64::from(color[0]) / 255.0,
                f64::from(color[1]) / 255.0,
                f64::from(color[2]) / 255.0,
                f64::from(color[3]) / 255.0,
            );
            let path = CGPath::with_rounded_rect(
                CGRect {
                    origin: CGPoint { x: inset, y: inset },
                    size: CGSize {
                        width: f64::from(width) - 2.0 * inset,
                        height: f64::from(height) - 2.0 * inset,
                    },
                },
                10.0 * scale,
                10.0 * scale,
                std::ptr::null(),
            );
            CGContext::add_path(Some(&context), Some(&path));
            CGContext::fill_path(Some(&context));
        };
        fill(border, 0.0);
        fill(background, scale);
        // Quartz draws in y-up coordinates, but bitmap memory already exposes the
        // visual top row first. Return those rows unchanged to the RGBA compositor.
        CGContext::set_text_position(
            Some(&context),
            (f64::from(width) - measured) / 2.0,
            (f64::from(height) - ascent - descent) / 2.0 + descent,
        );
        line.draw(&context);
        drop(context);
        Ok(KeycapImage {
            width,
            height,
            rgba,
        })
    }
}
/// Prepare the layout snapshot before starting keyboard consumers. On workers,
/// the host main run loop must be running during this initialization.
pub fn prepare_keyboard_layout() {
    crate::keyboard_layout::prepare();
}

/// Resolve a key legend from the prepared layout snapshot, without consuming
/// dead keys, accessing TIS on a worker, or waiting for the host main thread.
pub fn keyboard_label(key: u16, keyboard_type: u32, modifiers: u64) -> Option<String> {
    use std::ffi::c_void;
    #[link(name = "Carbon", kind = "framework")]
    unsafe extern "C" {
        fn UCKeyTranslate(
            layout: *const c_void,
            key: u16,
            action: u16,
            modifiers: u32,
            keyboard_type: u32,
            options: u32,
            dead: *mut u32,
            capacity: u64,
            length: *mut u64,
            output: *mut u16,
        ) -> i32;
    }
    let layout = crate::keyboard_layout::snapshot()?;
    unsafe {
        let mut dead = 0;
        let mut length = 0;
        let mut text = [0; 16];
        // UCKeyTranslate takes Carbon modifier bits shifted down eight places.
        // Command/Control describe a shortcut and do not change its key legend.
        let flags = (u32::from(modifiers & (1 << 17) != 0) << 1)
            | (u32::from(modifiers & (1 << 16) != 0) << 2)
            | (u32::from(modifiers & (1 << 19) != 0) << 3);
        if UCKeyTranslate(
            layout.as_ptr().cast(),
            key,
            3,
            flags,
            keyboard_type,
            1,
            &raw mut dead,
            text.len() as u64,
            &raw mut length,
            text.as_mut_ptr(),
        ) != 0
        {
            return None;
        }
        let label: String =
            String::from_utf16_lossy(&text[..length.min(text.len() as u64) as usize])
                .chars()
                .filter(|c| !c.is_control())
                .collect();
        (!label.is_empty()).then_some(label)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn register_fixture_fonts() {
        static REGISTER: std::sync::Once = std::sync::Once::new();
        REGISTER.call_once(|| {
            use objc2_core_foundation::{CFURL, CFURLPathStyle};
            for family in ["Sans", "Mono", "Han"] {
                for style in ["Regular", "Bold"] {
                    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                        .join("../../../test-support/fonts")
                        .join(format!("SnowRecordingTest{family}-{style}.ttf"));
                    let url = CFURL::with_file_system_path(
                        None,
                        Some(&CFString::from_str(path.to_str().unwrap())),
                        CFURLPathStyle::CFURLPOSIXPathStyle,
                        false,
                    )
                    .unwrap();
                    assert!(unsafe {
                        CTFontManagerRegisterFontsForURL(
                            &url,
                            CTFontManagerScope::Process,
                            std::ptr::null_mut(),
                        )
                    });
                }
            }
        });
    }

    #[test]
    fn coretext_keycaps_use_compact_pixel_geometry_at_every_scale() {
        register_fixture_fonts();
        // Fixture advances are 600 (Latin) and 900 (W) units per 1000 em.
        // These are the Windows layout widths at a 32-pixel font size.
        for (label, width) in [("F", 60), ("Ctrl", 108), ("WWWWWWWW", 212)] {
            for scale in [0.5, 1.0, 1.25, 2.0] {
                let image = keycap_with_font(
                    label,
                    scale,
                    [0; 4],
                    [255; 4],
                    [0; 4],
                    Some(KeycapFont {
                        family: "Snow Recording Test Sans",
                        cjk_family: "Snow Recording Test Han",
                        weight: 400,
                    }),
                )
                .unwrap();
                assert_eq!(image.width, (width as f32 * scale).ceil() as u32, "{label}");
                assert_eq!(image.height, (64.0 * scale).round() as u32);
            }
        }
    }

    #[test]
    fn coretext_keycap_rows_are_top_down() {
        register_fixture_fonts();
        for scale in [0.5, 1.0, 2.0] {
            let image = keycap_with_font(
                "F",
                scale,
                [0; 4],
                [255; 4],
                [0; 4],
                Some(KeycapFont {
                    family: "Snow Recording Test Sans",
                    cjk_family: "Snow Recording Test Han",
                    weight: 400,
                }),
            )
            .unwrap();
            // The fixture glyph has a full-width TOP bar and a narrow vertical stem.
            let rows: Vec<u32> = image
                .rgba
                .chunks_exact(image.width as usize * 4)
                .map(|row| row.chunks_exact(4).map(|p| u32::from(p[3])).sum())
                .filter(|sum| *sum > 0)
                .collect();
            let middle = rows.len() / 2;
            assert!(
                rows[..middle].iter().sum::<u32>() > rows[middle..].iter().sum::<u32>(),
                "glyph top bar must precede its stem in top-down RGBA rows at scale {scale}"
            );
        }
    }

    #[test]
    fn coretext_application_family_weight_and_han_fallback_control_pixels() {
        register_fixture_fonts();
        let render = |family, weight, label, scale| {
            keycap_with_font(
                label,
                scale,
                [0; 4],
                [255; 4],
                [0; 4],
                Some(KeycapFont {
                    family,
                    cjk_family: "Snow Recording Test Han",
                    weight,
                }),
            )
            .unwrap()
        };
        for family in ["Snow Recording Test Sans", "Snow Recording Test Mono"] {
            for weight in [400, 700] {
                let font = keycap_font(
                    Some(KeycapFont {
                        family,
                        cjk_family: "Snow Recording Test Han",
                        weight,
                    }),
                    30.0,
                )
                .unwrap();
                assert_eq!(unsafe { font.family_name() }.to_string(), family);
                for label in ["鼠标左键", "鼠標右鍵", "\u{20000}"] {
                    for scale in [0.5, 1.0, 2.0] {
                        let actual = render(family, weight, label, scale);
                        let expected = render("Snow Recording Test Han", weight, label, scale);
                        assert!(actual.rgba.iter().any(|value| *value != 0));
                        assert_eq!(
                            (actual.width, actual.height),
                            (expected.width, expected.height)
                        );
                        assert_eq!(
                            actual.rgba, expected.rgba,
                            "{label} {family} {weight} {scale}"
                        );
                    }
                }
            }
        }
        assert_ne!(
            render("Snow Recording Test Sans", 400, "WWW", 1.0).rgba,
            render("Snow Recording Test Mono", 400, "WWW", 1.0).rgba
        );
        for label in ["Ctrl", "鼠标左键", "Ctrl\u{20000}"] {
            assert_ne!(
                render("Snow Recording Test Sans", 400, label, 1.0).rgba,
                render("Snow Recording Test Sans", 700, label, 1.0).rgba
            );
        }
    }

    #[test]
    fn coretext_weight_mapping_and_font_validation() {
        assert_eq!(coretext_weight(400), unsafe {
            objc2_app_kit::NSFontWeightRegular
        });
        assert_eq!(coretext_weight(700), unsafe {
            objc2_app_kit::NSFontWeightBold
        });
        for weight in 1..999 {
            assert!(coretext_weight(weight) < coretext_weight(weight + 1));
        }
        for (family, weight) in [("", 400), ("bad\0family", 400), ("UI", 0), ("UI", 1000)] {
            assert!(
                keycap_font(
                    Some(KeycapFont {
                        family,
                        cjk_family: "UI",
                        weight
                    }),
                    30.0
                )
                .is_err()
            );
        }
        assert!(keycap_font(None, 30.0).is_ok());
    }

    #[test]
    fn coretext_keycap_is_premultiplied_and_supports_unicode() {
        let image = keycap(
            "⌘ 中文",
            1.0,
            [20, 30, 40, 200],
            [255, 255, 255, 255],
            [100, 100, 100, 255],
        )
        .unwrap();
        assert!(image.width >= 64);
        assert_eq!(
            image.rgba.len(),
            image.width as usize * image.height as usize * 4
        );
        assert!(
            image
                .rgba
                .chunks_exact(4)
                .all(|p| p[..3].iter().all(|c| *c <= p[3]))
        );
        assert!(image.rgba.chunks_exact(4).any(|p| p[0] > 200));
        assert!(keycap("x", f32::NAN, [0; 4], [0; 4], [0; 4]).is_err());
    }
}
