//! Owned cursor state for screenshots whose capture runs after UI preparation.

#[cfg(not(target_os = "macos"))]
use snow_cursor::CursorSampler;
use snow_cursor::{
    AttachedCursorSample, CursorCaptureError, CursorShapeCapture, CursorShapeState, CursorSnapshot,
};

use crate::frame::Frame;

/// A single immutable cursor observation, independent of subsequent OS/backend updates.
#[derive(Clone)]
pub struct ScreenshotCursorSnapshot {
    snapshot: CursorSnapshot,
    #[cfg(any(target_os = "macos", test))]
    desktop_points: Option<PointSnapshot>,
}

#[cfg(any(target_os = "macos", test))]
#[derive(Clone)]
struct PointSnapshot {
    x: f64,
    y: f64,
    point_width: f64,
    point_height: f64,
    hotspot_x: f64,
    hotspot_y: f64,
    width: u32,
    height: u32,
    premultiplied_rgba: std::sync::Arc<[u8]>,
}

/// An opaque replacement for only the pixels touched by a cursor.
pub struct ScreenshotCursorPatch {
    pub x: u32,
    pub y: u32,
    pub frame: Frame,
}

impl ScreenshotCursorSnapshot {
    pub fn capture() -> Result<Self, CursorCaptureError> {
        #[cfg(not(target_os = "macos"))]
        {
            Self::from_snapshot(CursorSampler::new()?.sample()?)
        }
        #[cfg(target_os = "macos")]
        {
            let visible = snow_macos::cursor::cursor_visible();
            let mut result = Self::from_snapshot(CursorSnapshot {
                absolute_x: 0,
                absolute_y: 0,
                visible: false,
                shape: CursorShapeCapture::Unavailable,
            })?;
            if visible {
                let sample = snow_macos::cursor::CursorSampler::default()
                    .sample()
                    .map_err(|e| CursorCaptureError::platform(e.to_string()))?;
                let shape = sample.shape.ok_or_else(|| {
                    CursorCaptureError::platform("global cursor shape is unavailable")
                })?;
                result.desktop_points = Some(PointSnapshot {
                    x: sample.x,
                    y: sample.y,
                    point_width: shape.point_width,
                    point_height: shape.point_height,
                    hotspot_x: shape.hotspot_x,
                    hotspot_y: shape.hotspot_y,
                    width: shape.width,
                    height: shape.height,
                    premultiplied_rgba: shape.rgba,
                });
            }
            Ok(result)
        }
    }

    pub fn from_snapshot(snapshot: CursorSnapshot) -> Result<Self, CursorCaptureError> {
        if snapshot.visible && snapshot.shape.shape().is_none() {
            return Err(CursorCaptureError::platform(
                "visible cursor shape is unavailable",
            ));
        }
        Ok(Self {
            snapshot,
            #[cfg(any(target_os = "macos", test))]
            desktop_points: None,
        })
    }

    fn observation(&self, frame: &Frame, origin_x: i32, origin_y: i32) -> CursorSnapshot {
        #[cfg(any(target_os = "macos", test))]
        if let Some(sample) = &self.desktop_points {
            use snow_cursor::{CursorCompositionMode, CursorShape};
            let Some(transform) = frame.metadata().capture_transform() else {
                return self.snapshot.clone();
            };
            let sx = f64::from(frame.width()) / transform.source.width;
            let sy = f64::from(frame.height()) / transform.source.height;
            let pixel_width = (sample.point_width * sx).round();
            let pixel_height = (sample.point_height * sy).round();
            if !pixel_width.is_finite()
                || !pixel_height.is_finite()
                || !(1.0..=2048.0).contains(&pixel_width)
                || !(1.0..=2048.0).contains(&pixel_height)
                || sample.width == 0
                || sample.height == 0
                || sample.premultiplied_rgba.len()
                    != sample.width as usize * sample.height as usize * 4
            {
                return self.snapshot.clone();
            }
            let width = pixel_width as u32;
            let height = pixel_height as u32;
            let mut rgba = Vec::with_capacity(width as usize * height as usize * 4);
            for y in 0..height {
                for x in 0..width {
                    let px = u64::from(x) * u64::from(sample.width) / u64::from(width);
                    let py = u64::from(y) * u64::from(sample.height) / u64::from(height);
                    let offset = (py * u64::from(sample.width) + px) as usize * 4;
                    let pixel = &sample.premultiplied_rgba[offset..offset + 4];
                    let alpha = u32::from(pixel[3]);
                    for value in &pixel[..3] {
                        rgba.push(
                            (u32::from(*value) * 255 + alpha / 2)
                                .checked_div(alpha)
                                .unwrap_or(0)
                                .min(255) as u8,
                        );
                    }
                    rgba.push(pixel[3]);
                }
            }
            return CursorSnapshot {
                absolute_x: ((sample.x - transform.source.x) * sx).round() as i32,
                absolute_y: ((sample.y - transform.source.y) * sy).round() as i32,
                visible: true,
                shape: CursorShapeCapture::Captured(CursorShape::from_rgba(
                    (sample.hotspot_x * sx).round().max(0.) as u32,
                    (sample.hotspot_y * sy).round().max(0.) as u32,
                    width,
                    height,
                    CursorCompositionMode::AlphaBlend,
                    rgba,
                )),
            };
        }
        let _ = frame;
        let mut snapshot = self.snapshot.clone();
        snapshot.absolute_x = snapshot.absolute_x.saturating_sub(origin_x);
        snapshot.absolute_y = snapshot.absolute_y.saturating_sub(origin_y);
        snapshot
    }

    /// Copies and composites only the clipped cursor rectangle. The source stays immutable.
    pub fn patch(
        &self,
        frame: &Frame,
        origin_x: i32,
        origin_y: i32,
    ) -> Option<ScreenshotCursorPatch> {
        let mut observed = self.observation(frame, origin_x, origin_y);
        if !observed.visible {
            return None;
        }
        let CursorShapeCapture::Captured(shape) = &observed.shape else {
            return None;
        };
        let x = i64::from(observed.absolute_x) - i64::from(shape.hotspot_x);
        let y = i64::from(observed.absolute_y) - i64::from(shape.hotspot_y);
        let left = x.clamp(0, i64::from(frame.width()));
        let top = y.clamp(0, i64::from(frame.height()));
        let right = (x + i64::from(shape.width)).clamp(0, i64::from(frame.width()));
        let bottom = (y + i64::from(shape.height)).clamp(0, i64::from(frame.height()));
        if right <= left || bottom <= top {
            return None;
        }
        let width = (right - left) as u32;
        let height = (bottom - top) as u32;
        let mut bytes = Vec::with_capacity(width as usize * height as usize * 4);
        for row in top..bottom {
            let offset = (row as usize * frame.width() as usize + left as usize) * 4;
            bytes.extend_from_slice(&frame.as_bytes()[offset..offset + width as usize * 4]);
        }
        let mut patch = match frame.pixel_format() {
            crate::frame::CapturePixelFormat::Rgba8 => Frame::from_rgba8(width, height, bytes),
            crate::frame::CapturePixelFormat::Bgra8 => Frame::from_bgra8(width, height, bytes),
        }
        .ok()?;
        observed.absolute_x -= left as i32;
        observed.absolute_y -= top as i32;
        Self::from_snapshot(observed)
            .ok()?
            .composite(&mut patch, 0, 0);
        // The patch owns final pixels; it need not retain the full sampled shape.
        patch.metadata.cursor = None;
        Some(ScreenshotCursorPatch {
            x: left as u32,
            y: top as u32,
            frame: patch,
        })
    }

    /// Composites into a cursor-free frame at its physical desktop origin.
    /// The shape is clipped to each frame, including when it crosses monitor edges.
    pub fn composite(&self, frame: &mut Frame, origin_x: i32, origin_y: i32) -> bool {
        let observed = self.observation(frame, origin_x, origin_y);
        let shape = match &observed.shape {
            CursorShapeCapture::Captured(shape) => CursorShapeState::Embedded(shape.clone()),
            CursorShapeCapture::Unavailable => CursorShapeState::Unavailable,
        };
        let cursor = AttachedCursorSample {
            x: observed.absolute_x,
            y: observed.absolute_y,
            visible: observed.visible,
            shape,
        };
        // Explicit snapshots replace any stale backend metadata without compositing it.
        frame.metadata.cursor = Some(cursor.clone());
        crate::cursor_compositor::composite_clipped(frame, &cursor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_cursor::{CursorCompositionMode, CursorShape};

    fn observation(visible: bool, mode: CursorCompositionMode) -> CursorSnapshot {
        CursorSnapshot {
            absolute_x: -1,
            absolute_y: 0,
            visible,
            shape: CursorShapeCapture::Captured(CursorShape::from_rgba(
                0,
                0,
                2,
                1,
                mode,
                vec![200, 100, 50, 255, 20, 40, 60, 255],
            )),
        }
    }

    #[test]
    fn desktop_points_preserve_hotspot_scale_premultiplied_alpha_and_edge_clipping() {
        use snow_media::geometry::{DesktopRect, DesktopSpace, DesktopTransform, PixelSize};
        for scale in [1, 2, 3] {
            let mut saved = ScreenshotCursorSnapshot::from_snapshot(CursorSnapshot {
                absolute_x: 0,
                absolute_y: 0,
                visible: false,
                shape: CursorShapeCapture::Unavailable,
            })
            .unwrap();
            saved.desktop_points = Some(PointSnapshot {
                x: -9.0,
                y: 21.0,
                point_width: 2.0,
                point_height: 1.0,
                hotspot_x: 1.0,
                hotspot_y: 0.0,
                width: 4,
                height: 2,
                premultiplied_rgba: [100, 50, 25, 128].repeat(8).into(),
            });
            let mut frame = Frame::from_rgba8(
                4 * scale,
                3 * scale,
                [20, 40, 60, 255].repeat(12 * scale as usize * scale as usize),
            )
            .unwrap();
            frame.metadata.capture_transform = Some(
                DesktopTransform::new(
                    DesktopRect {
                        space: DesktopSpace::Points,
                        x: -10.0,
                        y: 20.0,
                        width: 4.0,
                        height: 3.0,
                    },
                    PixelSize::new(4 * scale, 3 * scale).unwrap(),
                )
                .unwrap(),
            );
            let patch = saved.patch(&frame, 300, 900).unwrap();
            assert_eq!(
                (patch.x, patch.y, patch.frame.width(), patch.frame.height()),
                (0, scale, 2 * scale, scale)
            );
            let pixel = &patch.frame.as_bytes()[..4];
            assert!((109..=111).contains(&pixel[0]));
            assert!((69..=71).contains(&pixel[1]));
            assert!((54..=56).contains(&pixel[2]));
            assert_eq!(pixel[3], 255);
            saved.desktop_points.as_mut().unwrap().x = -10.0;
            let clipped = saved.patch(&frame, 0, 0).unwrap();
            assert_eq!((clipped.x, clipped.frame.width()), (0, scale));
            saved.desktop_points.as_mut().unwrap().x = 1000.0;
            assert!(saved.patch(&frame, 0, 0).is_none());
        }
    }

    #[test]
    fn patches_match_compositor_without_changing_or_copying_the_desktop() {
        for mode in [
            CursorCompositionMode::AlphaBlend,
            CursorCompositionMode::MaskedColor,
        ] {
            for bgra in [false, true] {
                let observed = observation(true, mode);
                let saved = ScreenshotCursorSnapshot::from_snapshot(observed).unwrap();
                let frame = if bgra {
                    Frame::from_bgra8(8, 4, vec![90; 128])
                } else {
                    Frame::from_rgba8(8, 4, vec![90; 128])
                }
                .unwrap();
                let pointer = frame.as_bytes().as_ptr();
                let patch = saved.patch(&frame, -2, 0).unwrap();
                assert_eq!(
                    (patch.x, patch.y, patch.frame.width(), patch.frame.height()),
                    (1, 0, 2, 1)
                );
                let mut expected = frame.clone();
                saved.composite(&mut expected, -2, 0);
                assert_eq!(patch.frame.as_bytes(), &expected.as_bytes()[4..12]);
                assert_eq!(frame.as_bytes(), &[90; 128]);
                assert_eq!(frame.as_bytes().as_ptr(), pointer);
                assert_eq!(patch.frame.as_bytes().len(), 8);
            }
        }
        let hidden = ScreenshotCursorSnapshot::from_snapshot(observation(
            false,
            CursorCompositionMode::AlphaBlend,
        ))
        .unwrap();
        let frame = Frame::from_rgba8(1, 1, vec![0; 4]).unwrap();
        assert!(hidden.patch(&frame, 0, 0).is_none());
    }

    #[test]
    fn owned_snapshot_ignores_later_cursor_and_backend_changes_across_monitors() {
        let mut live = observation(true, CursorCompositionMode::AlphaBlend);
        let saved = ScreenshotCursorSnapshot::from_snapshot(live.clone()).unwrap();
        live.absolute_x = 100;
        live.visible = false;
        live.shape = CursorShapeCapture::Unavailable;
        let later = ScreenshotCursorSnapshot::from_snapshot(live).unwrap();
        let mut left = Frame::from_rgba8(2, 1, vec![0; 8]).unwrap();
        let mut right = Frame::from_bgra8(2, 1, vec![0; 8]).unwrap();
        later.composite(&mut left, -2, 0);
        later.composite(&mut right, 0, 0);
        assert!(saved.composite(&mut left, -2, 0));
        assert!(saved.composite(&mut right, 0, 0));
        assert_eq!(left.as_bytes(), &[0, 0, 0, 0, 200, 100, 50, 255]);
        assert_eq!(right.as_bytes(), &[60, 40, 20, 255, 0, 0, 0, 0]);
        assert!(left.metadata().cursor().unwrap().visible);
    }

    #[test]
    fn snapshot_retains_hotspot_and_xor_composition() {
        let mut observed = observation(true, CursorCompositionMode::MaskedColor);
        if let CursorShapeCapture::Captured(ref mut shape) = observed.shape {
            shape.hotspot_x = 1;
        }
        let saved = ScreenshotCursorSnapshot::from_snapshot(observed).unwrap();
        let mut frame = Frame::from_rgba8(2, 1, vec![255; 8]).unwrap();
        assert!(saved.composite(&mut frame, -2, 0));
        assert_eq!(frame.as_bytes(), &[55, 155, 205, 255, 235, 215, 195, 255]);
    }

    #[test]
    fn hidden_snapshot_never_resurrects_a_cursor_and_missing_visible_shape_fails() {
        let mut observed = observation(false, CursorCompositionMode::AlphaBlend);
        observed.shape = CursorShapeCapture::Unavailable;
        let saved = ScreenshotCursorSnapshot::from_snapshot(observed.clone()).unwrap();
        let mut frame = Frame::from_rgba8(2, 1, vec![42; 8]).unwrap();
        assert!(!saved.composite(&mut frame, -2, 0));
        assert_eq!(frame.as_bytes(), &[42; 8]);
        observed.visible = true;
        assert!(ScreenshotCursorSnapshot::from_snapshot(observed).is_err());
    }
}
