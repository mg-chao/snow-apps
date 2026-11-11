use snow_media::geometry::{DesktopRect, DesktopTransform};
use std::sync::{Arc, Mutex};

/// Desktop rectangle of the latest frame accepted by the recording output.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CaptureViewport {
    pub revision: u64,
    pub source: DesktopRect,
}

#[derive(Clone, Default)]
pub(crate) struct CaptureViewportState(Arc<Mutex<Option<CaptureViewport>>>);

impl CaptureViewportState {
    /// Readers never wait for capture or encoder work, including during teardown.
    pub fn snapshot(&self) -> Option<CaptureViewport> {
        self.0.try_lock().ok().and_then(|value| *value)
    }

    /// Call only after successful output submission, never on source preparation.
    pub fn publish(&self, transform: Option<DesktopTransform>) {
        let Some(source) = transform.and_then(|value| value.source.validate().ok()) else {
            return;
        };
        let mut latest = self.0.lock().unwrap_or_else(|error| error.into_inner());
        if latest.is_some_and(|value| value.source == source) {
            return;
        }
        let revision = latest.map_or(1, |value| value.revision.saturating_add(1));
        *latest = Some(CaptureViewport { revision, source });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_media::geometry::{DesktopSpace, PixelSize};

    fn transform(x: f64) -> DesktopTransform {
        DesktopTransform::new(
            DesktopRect {
                space: DesktopSpace::Points,
                x,
                y: -20.25,
                width: 120.5,
                height: 80.75,
            },
            PixelSize::new(241, 162).unwrap(),
        )
        .unwrap()
    }

    #[test]
    fn capture_viewport_retains_only_valid_accepted_geometry() {
        let state = CaptureViewportState::default();
        assert_eq!(state.snapshot(), None);
        state.publish(None);
        assert_eq!(state.snapshot(), None);
        state.publish(Some(transform(-100.5)));
        let first = state.snapshot().unwrap();
        assert_eq!(first.revision, 1);
        assert_eq!(first.source, transform(-100.5).source);
        state.publish(Some(transform(-100.5)));
        state.publish(None);
        let mut invalid = transform(0.0);
        invalid.source.width = f64::NAN;
        state.publish(Some(invalid));
        assert_eq!(state.snapshot(), Some(first));
        state.clone().publish(Some(transform(500.25)));
        let second = state.snapshot().unwrap();
        assert_eq!(second.revision, 2);
        assert_eq!(second.source, transform(500.25).source);
    }

    #[test]
    fn capture_viewport_poll_does_not_wait_for_publisher() {
        let state = CaptureViewportState::default();
        state.publish(Some(transform(0.0)));
        let locked = state.0.lock().unwrap();
        assert_eq!(state.snapshot(), None);
        drop(locked);
        assert_eq!(state.snapshot().unwrap().revision, 1);
    }
}
