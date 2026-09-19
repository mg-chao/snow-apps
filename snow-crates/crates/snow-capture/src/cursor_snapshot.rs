//! Owned cursor state for screenshots whose capture runs after UI preparation.

use snow_cursor::{
    AttachedCursorSample, CursorCaptureError, CursorSampler, CursorShapeCapture, CursorShapeState,
    CursorSnapshot,
};

use crate::frame::Frame;

/// A single immutable cursor observation, independent of subsequent OS/backend updates.
#[derive(Clone)]
pub struct ScreenshotCursorSnapshot {
    snapshot: CursorSnapshot,
}

impl ScreenshotCursorSnapshot {
    pub fn capture() -> Result<Self, CursorCaptureError> {
        Self::from_snapshot(CursorSampler::new()?.sample()?)
    }

    pub fn from_snapshot(snapshot: CursorSnapshot) -> Result<Self, CursorCaptureError> {
        if snapshot.visible && snapshot.shape.shape().is_none() {
            return Err(CursorCaptureError::platform(
                "visible cursor shape is unavailable",
            ));
        }
        Ok(Self { snapshot })
    }

    /// Composites into a cursor-free frame at its physical desktop origin.
    /// The shape is clipped to each frame, including when it crosses monitor edges.
    pub fn composite(&self, frame: &mut Frame, origin_x: i32, origin_y: i32) -> bool {
        let shape = match &self.snapshot.shape {
            CursorShapeCapture::Captured(shape) => CursorShapeState::Embedded(shape.clone()),
            CursorShapeCapture::Unavailable => CursorShapeState::Unavailable,
        };
        let cursor = AttachedCursorSample {
            x: self.snapshot.absolute_x.saturating_sub(origin_x),
            y: self.snapshot.absolute_y.saturating_sub(origin_y),
            visible: self.snapshot.visible,
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
