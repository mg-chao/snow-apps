use crate::frame::{CapturePixelFormat, Frame};
use snow_cursor::{AttachedCursorSample, CursorCompositionMode, CursorShapeState};

pub(crate) fn composite(frame: &mut Frame, cursor: &AttachedCursorSample) -> bool {
    if !cursor.visible
        || cursor.x < 0
        || cursor.y < 0
        || cursor.x as u32 >= frame.width()
        || cursor.y as u32 >= frame.height()
    {
        return false;
    }

    composite_clipped(frame, cursor)
}

pub(crate) fn composite_clipped(frame: &mut Frame, cursor: &AttachedCursorSample) -> bool {
    if !cursor.visible {
        return false;
    }
    let CursorShapeState::Embedded(shape) = &cursor.shape else {
        return false;
    };
    let Some(shape_len) = shape
        .width
        .checked_mul(shape.height)
        .and_then(|pixels| pixels.checked_mul(4))
        .map(|bytes| bytes as usize)
    else {
        return false;
    };
    if shape.width == 0 || shape.height == 0 || shape.shape_rgba.len() != shape_len {
        return false;
    }

    let frame_width = i64::from(frame.width());
    let frame_height = i64::from(frame.height());
    let origin_x = i64::from(cursor.x) - i64::from(shape.hotspot_x);
    let origin_y = i64::from(cursor.y) - i64::from(shape.hotspot_y);
    let left = origin_x.clamp(0, frame_width);
    let top = origin_y.clamp(0, frame_height);
    let right = origin_x
        .saturating_add(i64::from(shape.width))
        .clamp(0, frame_width);
    let bottom = origin_y
        .saturating_add(i64::from(shape.height))
        .clamp(0, frame_height);
    if left >= right || top >= bottom {
        return false;
    }

    let frame_width = frame.width() as usize;
    let format = frame.pixel_format();
    let shape_width = shape.width as usize;
    let mut composited = false;
    let pixels = frame.as_mut_bytes();
    for frame_y in top..bottom {
        let shape_y = (frame_y - origin_y) as usize;
        for frame_x in left..right {
            let shape_x = (frame_x - origin_x) as usize;
            let source_offset = (shape_y * shape_width + shape_x) * 4;
            let source = &shape.shape_rgba[source_offset..source_offset + 4];
            let destination_offset = (frame_y as usize * frame_width + frame_x as usize) * 4;
            let destination = &mut pixels[destination_offset..destination_offset + 4];
            composited |= composite_pixel(destination, source, format, shape.composition_mode);
        }
    }
    composited
}

fn composite_pixel(
    destination: &mut [u8],
    source_rgba: &[u8],
    format: CapturePixelFormat,
    mode: CursorCompositionMode,
) -> bool {
    let source_alpha = source_rgba[3];
    match mode {
        CursorCompositionMode::AlphaBlend if source_alpha == 0 => false,
        CursorCompositionMode::AlphaBlend if source_alpha == 255 => {
            write_rgb(destination, source_rgba, format);
            destination[3] = 255;
            true
        }
        CursorCompositionMode::AlphaBlend => {
            blend_rgb(destination, source_rgba, format, source_alpha);
            destination[3] = 255;
            true
        }
        CursorCompositionMode::MaskedColor if source_alpha == 0 => {
            write_rgb(destination, source_rgba, format);
            destination[3] = 255;
            true
        }
        CursorCompositionMode::MaskedColor
            if source_alpha == 255
                && source_rgba[0] == 0
                && source_rgba[1] == 0
                && source_rgba[2] == 0 =>
        {
            false
        }
        CursorCompositionMode::MaskedColor if source_alpha == 255 => {
            xor_rgb(destination, source_rgba, format);
            destination[3] = 255;
            true
        }
        CursorCompositionMode::MaskedColor => {
            blend_rgb(destination, source_rgba, format, source_alpha);
            destination[3] = 255;
            true
        }
    }
}

fn channel_indices(format: CapturePixelFormat) -> [usize; 3] {
    match format {
        CapturePixelFormat::Rgba8 => [0, 1, 2],
        CapturePixelFormat::Bgra8 => [2, 1, 0],
    }
}

fn write_rgb(destination: &mut [u8], source_rgba: &[u8], format: CapturePixelFormat) {
    for (source_index, destination_index) in channel_indices(format).into_iter().enumerate() {
        destination[destination_index] = source_rgba[source_index];
    }
}

fn xor_rgb(destination: &mut [u8], source_rgba: &[u8], format: CapturePixelFormat) {
    for (source_index, destination_index) in channel_indices(format).into_iter().enumerate() {
        destination[destination_index] ^= source_rgba[source_index];
    }
}

fn blend_rgb(destination: &mut [u8], source_rgba: &[u8], format: CapturePixelFormat, alpha: u8) {
    let alpha = u16::from(alpha);
    let inverse_alpha = 255 - alpha;
    for (source_index, destination_index) in channel_indices(format).into_iter().enumerate() {
        destination[destination_index] = ((u16::from(source_rgba[source_index]) * alpha
            + u16::from(destination[destination_index]) * inverse_alpha)
            / 255) as u8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_cursor::{CursorShape, CursorShapeId, CursorShapeState};

    fn frame_with_cursor(
        width: u32,
        height: u32,
        pixels: Vec<u8>,
        format: CapturePixelFormat,
        cursor: AttachedCursorSample,
    ) -> Frame {
        let mut frame = match format {
            CapturePixelFormat::Rgba8 => Frame::from_rgba8(width, height, pixels).unwrap(),
            CapturePixelFormat::Bgra8 => Frame::from_bgra8(width, height, pixels).unwrap(),
        };
        frame.metadata.cursor = Some(cursor);
        frame
    }

    fn cursor(
        x: i32,
        y: i32,
        visible: bool,
        hotspot: (u32, u32),
        dimensions: (u32, u32),
        mode: CursorCompositionMode,
        rgba: Vec<u8>,
    ) -> AttachedCursorSample {
        AttachedCursorSample {
            x,
            y,
            visible,
            shape: CursorShapeState::Embedded(CursorShape::from_rgba(
                hotspot.0,
                hotspot.1,
                dimensions.0,
                dimensions.1,
                mode,
                rgba,
            )),
        }
    }

    #[test]
    fn composites_alpha_cursor_into_rgba_frame() {
        let cursor = cursor(
            0,
            0,
            true,
            (0, 0),
            (2, 1),
            CursorCompositionMode::AlphaBlend,
            vec![200, 100, 50, 255, 100, 50, 0, 128],
        );
        let mut frame = frame_with_cursor(
            3,
            1,
            vec![20, 40, 60, 255, 20, 40, 60, 255, 7, 8, 9, 255],
            CapturePixelFormat::Rgba8,
            cursor,
        );

        assert!(frame.composite_attached_cursor());
        assert_eq!(
            frame.as_bytes(),
            &[200, 100, 50, 255, 60, 45, 29, 255, 7, 8, 9, 255]
        );
    }

    #[test]
    fn alpha_blends_rgba_cursor_channels_into_bgra_frame() {
        let cursor = cursor(
            0,
            0,
            true,
            (0, 0),
            (1, 1),
            CursorCompositionMode::AlphaBlend,
            vec![100, 50, 0, 128],
        );
        let mut frame = frame_with_cursor(
            1,
            1,
            vec![60, 40, 20, 255],
            CapturePixelFormat::Bgra8,
            cursor,
        );

        assert!(frame.composite_attached_cursor());
        assert_eq!(frame.as_bytes(), &[29, 45, 60, 255]);
    }

    #[test]
    fn applies_masked_copy_noop_xor_and_blend() {
        let cursor = cursor(
            0,
            0,
            true,
            (0, 0),
            (4, 1),
            CursorCompositionMode::MaskedColor,
            vec![
                1, 2, 3, 0, 0, 0, 0, 255, 0x0f, 0xf0, 0x55, 255, 100, 50, 0, 128,
            ],
        );
        let mut frame = frame_with_cursor(
            4,
            1,
            vec![
                10, 20, 30, 7, 40, 50, 60, 8, 0xf0, 0x0f, 0xaa, 9, 20, 40, 60, 10,
            ],
            CapturePixelFormat::Rgba8,
            cursor,
        );

        assert!(frame.composite_attached_cursor());
        assert_eq!(
            frame.as_bytes(),
            &[
                1, 2, 3, 255, 40, 50, 60, 8, 0xff, 0xff, 0xff, 255, 60, 45, 29, 255,
            ]
        );
    }

    #[test]
    fn applies_hotspot_offset_and_clips_shape_at_frame_edge() {
        let cursor = cursor(
            0,
            0,
            true,
            (1, 1),
            (2, 2),
            CursorCompositionMode::AlphaBlend,
            vec![1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255],
        );
        let original = vec![20; 3 * 2 * 4];
        let mut frame =
            frame_with_cursor(3, 2, original.clone(), CapturePixelFormat::Rgba8, cursor);

        assert!(frame.composite_attached_cursor());
        assert_eq!(&frame.as_bytes()[0..4], &[10, 11, 12, 255]);
        assert_eq!(&frame.as_bytes()[4..], &original[4..]);
    }

    #[test]
    fn ignores_hidden_outside_missing_cached_and_invalid_cursor_shapes() {
        let valid = cursor(
            0,
            0,
            true,
            (0, 0),
            (1, 1),
            CursorCompositionMode::AlphaBlend,
            vec![255, 255, 255, 255],
        );
        let mut samples = vec![
            AttachedCursorSample {
                visible: false,
                ..valid.clone()
            },
            AttachedCursorSample {
                x: 2,
                ..valid.clone()
            },
            AttachedCursorSample {
                y: -1,
                ..valid.clone()
            },
            AttachedCursorSample {
                shape: CursorShapeState::Unavailable,
                ..valid.clone()
            },
            AttachedCursorSample {
                shape: CursorShapeState::Cached(CursorShapeId::from_raw(7)),
                ..valid.clone()
            },
        ];
        let mut invalid = valid;
        invalid.shape = CursorShapeState::Embedded(CursorShape {
            shape_id: CursorShapeId::from_raw(8),
            hotspot_x: 0,
            hotspot_y: 0,
            width: 1,
            height: 1,
            composition_mode: CursorCompositionMode::AlphaBlend,
            shape_rgba: vec![1, 2, 3].into(),
        });
        samples.push(invalid);

        for sample in samples {
            let original = vec![10, 20, 30, 40];
            let mut frame =
                frame_with_cursor(1, 1, original.clone(), CapturePixelFormat::Rgba8, sample);
            assert!(!frame.composite_attached_cursor());
            assert_eq!(frame.as_bytes(), original);
        }

        let mut frame = Frame::from_rgba8(1, 1, vec![10, 20, 30, 40]).unwrap();
        assert!(!frame.composite_attached_cursor());
        assert_eq!(frame.as_bytes(), &[10, 20, 30, 40]);
    }

    #[test]
    fn transparent_alpha_cursor_is_a_noop() {
        let cursor = cursor(
            0,
            0,
            true,
            (0, 0),
            (1, 1),
            CursorCompositionMode::AlphaBlend,
            vec![255, 255, 255, 0],
        );
        let original = vec![10, 20, 30, 40];
        let mut frame =
            frame_with_cursor(1, 1, original.clone(), CapturePixelFormat::Rgba8, cursor);

        assert!(!frame.composite_attached_cursor());
        assert_eq!(frame.as_bytes(), original);
    }
}
