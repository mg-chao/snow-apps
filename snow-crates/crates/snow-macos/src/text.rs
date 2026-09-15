//! Offscreen CoreText/CoreGraphics keycap rendering with system font fallback.
use crate::{MacError, MacResult};
use objc2_core_foundation::{
    CFAttributedString, CFDictionary, CFString, CFType, CGPoint, CGRect, CGSize,
};
use objc2_core_graphics::*;
use objc2_core_text::*;

pub struct KeycapImage {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}
pub fn keycap(
    label: &str,
    scale: f32,
    background: [u8; 4],
    text: [u8; 4],
    border: [u8; 4],
) -> MacResult<KeycapImage> {
    if !scale.is_finite() || !(0.25..=4.0).contains(&scale) || label.len() > 1024 {
        return Err(MacError::InvalidConfig(
            "invalid keycap size or label".into(),
        ));
    }
    let scale = f64::from(scale);
    unsafe {
        let font = CTFont::new_ui_font_for_language(CTFontUIFontType::System, 30.0 * scale, None)
            .ok_or_else(|| MacError::Unsupported("system text font unavailable".into()))?;
        let color = CGColor::new_generic_rgb(
            f64::from(text[0]) / 255.0,
            f64::from(text[1]) / 255.0,
            f64::from(text[2]) / 255.0,
            f64::from(text[3]) / 255.0,
        );
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
        let line = CTLine::with_attributed_string(&value);
        let mut ascent = 0.0;
        let mut descent = 0.0;
        let measured =
            line.typographic_bounds(&raw mut ascent, &raw mut descent, std::ptr::null_mut());
        let height = (64.0 * scale).ceil() as u32;
        let width = (measured + 28.0 * scale)
            .max(f64::from(height))
            .ceil()
            .min(4096.0) as u32;
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
        // Quartz text uses a bottom-left origin. Flip only the backing rows after drawing.
        CGContext::set_text_position(
            Some(&context),
            (f64::from(width) - measured) / 2.0,
            (f64::from(height) - ascent - descent) / 2.0 + descent,
        );
        line.draw(&context);
        drop(context);
        let stride = width as usize * 4;
        for y in 0..height as usize / 2 {
            let (top, rest) = rgba.split_at_mut((height as usize - 1 - y) * stride);
            top[y * stride..(y + 1) * stride].swap_with_slice(&mut rest[..stride]);
        }
        Ok(KeycapImage {
            width,
            height,
            rgba,
        })
    }
}
/// Resolve a key legend using the active macOS layout, without consuming dead
/// keys or changing the user's input state. Call outside the event tap callback.
pub fn keyboard_label(key: u16, keyboard_type: u32, modifiers: u64) -> Option<String> {
    use objc2_core_foundation::{CFData, CFRetained};
    use std::{ffi::c_void, ptr::NonNull};
    #[link(name = "Carbon", kind = "framework")]
    unsafe extern "C" {
        static kTISPropertyUnicodeKeyLayoutData: *const CFString;
        fn TISCopyCurrentKeyboardLayoutInputSource() -> *mut CFType;
        fn TISGetInputSourceProperty(
            source: *const CFType,
            property: *const CFString,
        ) -> *const c_void;
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
    unsafe {
        let source = CFRetained::from_raw(NonNull::new(TISCopyCurrentKeyboardLayoutInputSource())?);
        let data = TISGetInputSourceProperty(
            CFRetained::as_ptr(&source).as_ptr(),
            kTISPropertyUnicodeKeyLayoutData,
        )
        .cast::<CFData>()
        .as_ref()?;
        if data.length() <= 0 {
            return None;
        }
        let mut dead = 0;
        let mut length = 0;
        let mut text = [0; 16];
        // UCKeyTranslate takes Carbon modifier bits shifted down eight places.
        // Command/Control describe a shortcut and do not change its key legend.
        let flags = (u32::from(modifiers & (1 << 17) != 0) << 1)
            | (u32::from(modifiers & (1 << 16) != 0) << 2)
            | (u32::from(modifiers & (1 << 19) != 0) << 3);
        if UCKeyTranslate(
            data.byte_ptr().cast(),
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
