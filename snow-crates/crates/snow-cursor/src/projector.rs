use crate::model::{
    AttachedCursorSample, CursorShapeCapture, CursorShapeState, CursorSnapshot, CursorTargetInfo,
};

/// Converts absolute cursor snapshots into target-relative samples. Each shape
/// transition includes its pixels; repeated observations reference the current
/// shape so consumers only need to retain one bitmap.
#[derive(Default)]
pub struct CursorProjector {
    last_shape_id: Option<crate::CursorShapeId>,
}

impl CursorProjector {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn project(
        &mut self,
        target: &CursorTargetInfo,
        snapshot: CursorSnapshot,
    ) -> AttachedCursorSample {
        let x = snapshot.absolute_x - target.origin_x;
        let y = snapshot.absolute_y - target.origin_y;
        let visible = snapshot.visible && target.contains_relative(x, y);
        let shape = match snapshot.shape {
            CursorShapeCapture::Captured(shape) => {
                let changed = self.last_shape_id != Some(shape.shape_id);
                self.last_shape_id = Some(shape.shape_id);
                if changed {
                    CursorShapeState::Embedded(shape)
                } else {
                    CursorShapeState::Cached(shape.shape_id)
                }
            }
            // Sampling a cursor shape is not synchronized with application
            // cursor changes. If Win32 temporarily cannot expose the current
            // shape, keep using the last authentic one just as the Desktop
            // Duplication API requires when it reports no new shape.
            CursorShapeCapture::Unavailable => self
                .last_shape_id
                .map(CursorShapeState::Cached)
                .unwrap_or(CursorShapeState::Unavailable),
        };

        AttachedCursorSample {
            x,
            y,
            visible,
            shape,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CursorCompositionMode, CursorShape};

    fn sample_shape(seed: u8) -> CursorShape {
        CursorShape::from_rgba(
            0,
            0,
            2,
            2,
            CursorCompositionMode::AlphaBlend,
            vec![seed; 16],
        )
    }

    #[test]
    fn first_shape_is_embedded_then_cached() {
        let mut projector = CursorProjector::new();
        let target = CursorTargetInfo {
            origin_x: 10,
            origin_y: 20,
            width: 100,
            height: 100,
        };
        let shape = sample_shape(7);

        let first = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 11,
                absolute_y: 21,
                visible: true,
                shape: CursorShapeCapture::Captured(shape.clone()),
            },
        );
        let second = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 12,
                absolute_y: 22,
                visible: true,
                shape: CursorShapeCapture::Captured(shape.clone()),
            },
        );

        assert!(matches!(first.shape, CursorShapeState::Embedded(_)));
        assert!(matches!(
            second.shape,
            CursorShapeState::Cached(id) if id == shape.shape_id
        ));
    }

    #[test]
    fn every_shape_transition_reembeds_pixels_including_revisited_shapes() {
        let mut projector = CursorProjector::new();
        let target = CursorTargetInfo {
            origin_x: 0,
            origin_y: 0,
            width: 100,
            height: 100,
        };
        let a = sample_shape(1);
        let b = sample_shape(2);
        for (shape, embedded) in [
            (&a, true),
            (&a, false),
            (&b, true),
            (&b, false),
            (&a, true),
            (&a, false),
        ] {
            let sample = projector.project(
                &target,
                CursorSnapshot {
                    absolute_x: 10,
                    absolute_y: 20,
                    visible: true,
                    shape: CursorShapeCapture::Captured(shape.clone()),
                },
            );
            assert_eq!(sample.shape_id(), Some(shape.shape_id));
            assert_eq!(sample.shape.embedded_shape().is_some(), embedded);
        }
        let sample = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 11,
                absolute_y: 21,
                visible: true,
                shape: CursorShapeCapture::Unavailable,
            },
        );
        assert!(matches!(sample.shape, CursorShapeState::Cached(id) if id == a.shape_id));
    }

    #[test]
    fn unavailable_shape_stays_unavailable() {
        let mut projector = CursorProjector::new();
        let target = CursorTargetInfo {
            origin_x: 0,
            origin_y: 0,
            width: 100,
            height: 100,
        };

        let sample = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 30,
                absolute_y: 40,
                visible: true,
                shape: CursorShapeCapture::Unavailable,
            },
        );

        assert!(matches!(sample.shape, CursorShapeState::Unavailable));
        assert_eq!(sample.shape_id(), None);
    }

    #[test]
    fn unavailable_shape_reuses_last_captured_shape() {
        let mut projector = CursorProjector::new();
        let target = CursorTargetInfo {
            origin_x: 0,
            origin_y: 0,
            width: 100,
            height: 100,
        };
        let shape = sample_shape(9);

        projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 10,
                absolute_y: 20,
                visible: true,
                shape: CursorShapeCapture::Captured(shape.clone()),
            },
        );
        let sample = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 11,
                absolute_y: 21,
                visible: true,
                shape: CursorShapeCapture::Unavailable,
            },
        );

        assert!(matches!(
            sample.shape,
            CursorShapeState::Cached(id) if id == shape.shape_id
        ));
    }

    #[test]
    fn projection_marks_out_of_bounds_cursor_invisible() {
        let mut projector = CursorProjector::new();
        let target = CursorTargetInfo {
            origin_x: 100,
            origin_y: 100,
            width: 50,
            height: 50,
        };
        let shape = sample_shape(1);

        let sample = projector.project(
            &target,
            CursorSnapshot {
                absolute_x: 20,
                absolute_y: 30,
                visible: true,
                shape: CursorShapeCapture::Captured(shape.clone()),
            },
        );

        assert!(!sample.visible);
        assert_eq!(sample.x, -80);
        assert_eq!(sample.y, -70);
        assert_eq!(sample.shape_id(), Some(shape.shape_id));
    }
}
