//! Public cursor sampling. A missing global shape is explicit; the process-local
//! NSCursor and the standard arrow are never substituted for another app's cursor.
use crate::{MacError, MacResult};
use objc2::{rc::Retained, runtime::AnyObject};
use objc2_app_kit::NSCursor;
use objc2_core_foundation::{CGPoint, CGRect, CGSize};
use objc2_core_graphics::*;
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
#[derive(Clone)]
pub struct CursorShape {
    pub width: u32,
    pub height: u32,
    pub point_width: f64,
    pub point_height: f64,
    pub hotspot_x: f64,
    pub hotspot_y: f64,
    /// Top-left rows, premultiplied sRGB RGBA.
    pub rgba: Arc<[u8]>,
}
pub struct CursorSample {
    pub x: f64,
    pub y: f64,
    pub shape: Option<CursorShape>,
}
#[derive(Default)]
pub struct CursorSampler {
    cached: Option<(Retained<NSCursor>, CursorShape, Instant)>,
}
impl CursorSampler {
    pub fn sample(&mut self) -> MacResult<CursorSample> {
        objc2::rc::autoreleasepool(|_| {
            let event = CGEvent::new(None).ok_or(MacError::Inactive)?;
            let location = CGEvent::location(Some(&event));
            // Deprecated but still public. On systems where it returns nil we
            // report shape unavailability; ScreenCaptureKit remains the default.
            #[allow(deprecated)]
            let cursor = NSCursor::currentSystemCursor();
            let Some(cursor) = cursor else {
                self.cached = None;
                return Ok(CursorSample {
                    x: location.x,
                    y: location.y,
                    shape: None,
                });
            };
            if let Some((old, shape, at)) = &self.cached
                && std::ptr::eq::<AnyObject>(old.as_ref(), cursor.as_ref())
                && at.elapsed() < Duration::from_millis(100)
            {
                return Ok(CursorSample {
                    x: location.x,
                    y: location.y,
                    shape: Some(shape.clone()),
                });
            }
            let image = cursor.image();
            let size = image.size();
            let hotspot = cursor.hotSpot();
            let native = unsafe {
                image.CGImageForProposedRect_context_hints(std::ptr::null_mut(), None, None)
            };
            let Some(native) = native else {
                return Ok(CursorSample {
                    x: location.x,
                    y: location.y,
                    shape: None,
                });
            };
            let width = CGImage::width(Some(&native));
            let height = CGImage::height(Some(&native));
            if !(1..=512).contains(&width)
                || !(1..=512).contains(&height)
                || !size.width.is_finite()
                || !size.height.is_finite()
                || size.width <= 0.0
                || size.height <= 0.0
            {
                return Err(MacError::Unsupported(
                    "invalid public cursor image geometry".into(),
                ));
            }
            let mut rgba = vec![0; width * height * 4];
            unsafe {
                let space =
                    CGColorSpace::with_name(Some(kCGColorSpaceSRGB)).ok_or(MacError::Inactive)?;
                let context = CGBitmapContextCreate(
                    rgba.as_mut_ptr().cast(),
                    width,
                    height,
                    8,
                    width * 4,
                    Some(&space),
                    CGImageAlphaInfo::PremultipliedLast.0 | CGImageByteOrderInfo::Order32Big.0,
                )
                .ok_or(MacError::Inactive)?;
                CGContext::translate_ctm(Some(&context), 0.0, height as f64);
                CGContext::scale_ctm(Some(&context), 1.0, -1.0);
                CGContext::draw_image(
                    Some(&context),
                    CGRect {
                        origin: CGPoint::ZERO,
                        size: CGSize {
                            width: width as f64,
                            height: height as f64,
                        },
                    },
                    Some(&native),
                );
            }
            let shape = CursorShape {
                width: width as u32,
                height: height as u32,
                point_width: size.width,
                point_height: size.height,
                hotspot_x: hotspot.x,
                hotspot_y: hotspot.y,
                rgba: rgba.into(),
            };
            self.cached = Some((cursor, shape.clone(), Instant::now()));
            Ok(CursorSample {
                x: location.x,
                y: location.y,
                shape: Some(shape),
            })
        })
    }
}
