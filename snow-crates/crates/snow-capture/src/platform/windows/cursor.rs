use windows::Win32::Graphics::Dxgi::{
    DXGI_OUTDUPL_FRAME_INFO, DXGI_OUTDUPL_POINTER_POSITION, DXGI_OUTDUPL_POINTER_SHAPE_INFO,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME,
};

use snow_cursor::{CursorCompositionMode, CursorShape};

pub(crate) fn update_dxgi_pointer_position(
    position: &mut Option<DXGI_OUTDUPL_POINTER_POSITION>,
    frame_info: &DXGI_OUTDUPL_FRAME_INFO,
) {
    // Desktop-only updates (for example, typing) contain no valid pointer
    // position or visibility. Preserve the last mouse update in those frames.
    // https://learn.microsoft.com/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info
    if frame_info.LastMouseUpdateTime != 0 {
        *position = Some(frame_info.PointerPosition);
    }
}

pub(crate) fn decode_dxgi_pointer_shape(
    shape_info: &DXGI_OUTDUPL_POINTER_SHAPE_INFO,
    buffer: &[u8],
) -> Option<CursorShape> {
    let hotspot_x = shape_info.HotSpot.x.max(0) as u32;
    let hotspot_y = shape_info.HotSpot.y.max(0) as u32;
    let width = shape_info.Width;
    let height = shape_info.Height;
    let pitch = shape_info.Pitch as usize;

    match DXGI_OUTDUPL_POINTER_SHAPE_TYPE(shape_info.Type as i32) {
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR => Some(CursorShape::from_rgba(
            hotspot_x,
            hotspot_y,
            width,
            height,
            CursorCompositionMode::AlphaBlend,
            decode_color_pointer_rgba(width, height, pitch, buffer)?,
        )),
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR => Some(CursorShape::from_rgba(
            hotspot_x,
            hotspot_y,
            width,
            height,
            CursorCompositionMode::MaskedColor,
            decode_color_pointer_rgba(width, height, pitch, buffer)?,
        )),
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME => Some(decode_monochrome_dxgi_shape(
            hotspot_x, hotspot_y, width, height, pitch, buffer,
        )?),
        _ => None,
    }
}

fn decode_color_pointer_rgba(
    width: u32,
    height: u32,
    pitch: usize,
    buffer: &[u8],
) -> Option<Vec<u8>> {
    let row_bytes = (width as usize).checked_mul(4)?;
    let total_bytes = pitch.checked_mul(height as usize)?;
    if pitch < row_bytes || buffer.len() < total_bytes {
        return None;
    }

    let mut rgba = vec![0u8; row_bytes.checked_mul(height as usize)?];
    for y in 0..height as usize {
        let src_row = y.checked_mul(pitch)?;
        let dst_row = y.checked_mul(row_bytes)?;
        for x in 0..width as usize {
            let src = src_row.checked_add(x.checked_mul(4)?)?;
            let dst = dst_row.checked_add(x.checked_mul(4)?)?;
            rgba[dst] = buffer[src + 2];
            rgba[dst + 1] = buffer[src + 1];
            rgba[dst + 2] = buffer[src];
            rgba[dst + 3] = buffer[src + 3];
        }
    }
    Some(rgba)
}

fn decode_monochrome_dxgi_shape(
    hotspot_x: u32,
    hotspot_y: u32,
    width: u32,
    height: u32,
    pitch: usize,
    buffer: &[u8],
) -> Option<CursorShape> {
    let half_len = pitch.checked_mul(height as usize)?;
    let total_len = half_len.checked_mul(2)?;
    if buffer.len() < total_len {
        return None;
    }

    let mut rgba = vec![0u8; checked_rgba_len(width, height)?];
    for y in 0..height as usize {
        for x in 0..width as usize {
            let and_set = monochrome_mask_bit(buffer, pitch, y, x);
            let xor_set = monochrome_mask_bit(&buffer[half_len..], pitch, y, x);
            let (r, g, b, a) = match (and_set, xor_set) {
                (false, false) => (0, 0, 0, 0),
                (false, true) => (255, 255, 255, 0),
                (true, false) => (0, 0, 0, 255),
                (true, true) => (255, 255, 255, 255),
            };
            let dst = (y * width as usize + x).checked_mul(4)?;
            rgba[dst] = r;
            rgba[dst + 1] = g;
            rgba[dst + 2] = b;
            rgba[dst + 3] = a;
        }
    }

    Some(CursorShape::from_rgba(
        hotspot_x,
        hotspot_y,
        width,
        height,
        CursorCompositionMode::MaskedColor,
        rgba,
    ))
}

fn monochrome_mask_bit(buffer: &[u8], pitch: usize, y: usize, x: usize) -> bool {
    let byte_index = y * pitch + x / 8;
    let mask = 0x80u8 >> (x & 7);
    buffer
        .get(byte_index)
        .is_some_and(|byte| (byte & mask) != 0)
}

fn checked_rgba_len(width: u32, height: u32) -> Option<usize> {
    (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(4)
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Foundation::POINT;

    fn pointer_update(timestamp: i64, x: i32, y: i32, visible: bool) -> DXGI_OUTDUPL_FRAME_INFO {
        DXGI_OUTDUPL_FRAME_INFO {
            LastMouseUpdateTime: timestamp,
            PointerPosition: DXGI_OUTDUPL_POINTER_POSITION {
                Position: POINT { x, y },
                Visible: visible.into(),
            },
            ..Default::default()
        }
    }

    #[test]
    fn dxgi_pointer_preserves_stationary_cursor_during_desktop_updates() {
        let mut position = None;
        update_dxgi_pointer_position(&mut position, &pointer_update(1, 120, 80, true));

        // Typing updates the desktop without updating the mouse. DXGI's
        // pointer fields are invalid in those frames, even if zero-filled.
        for timestamp in 2..32 {
            update_dxgi_pointer_position(
                &mut position,
                &DXGI_OUTDUPL_FRAME_INFO {
                    LastPresentTime: timestamp,
                    AccumulatedFrames: 1,
                    ..Default::default()
                },
            );
            let pointer = position.expect("the last mouse update must remain available");
            assert!(
                pointer.Visible.as_bool(),
                "desktop updates must not hide the cursor"
            );
            assert_eq!((pointer.Position.x, pointer.Position.y), (120, 80));
        }
    }

    #[test]
    fn dxgi_pointer_waits_for_first_valid_mouse_update() {
        let mut position = None;
        for visible in [false, true] {
            update_dxgi_pointer_position(&mut position, &pointer_update(0, 120, 80, visible));
            assert!(
                position.is_none(),
                "invalid pointer fields must not initialize state"
            );
        }
        update_dxgi_pointer_position(&mut position, &pointer_update(1, 120, 80, true));
        assert!(position.unwrap().Visible.as_bool());
    }

    #[test]
    fn dxgi_pointer_applies_real_movement_and_visibility_changes() {
        let mut position = None;
        for (timestamp, x, y, visible) in
            [(1, 120, 80, true), (2, 120, 80, false), (3, 240, 90, true)]
        {
            // Pointer-only updates are valid even without a desktop present.
            update_dxgi_pointer_position(&mut position, &pointer_update(timestamp, x, y, visible));
            // Invalid fields must neither hide nor resurrect the cursor.
            update_dxgi_pointer_position(&mut position, &pointer_update(0, -1, -1, !visible));
            let pointer = position.unwrap();
            assert_eq!(pointer.Visible.as_bool(), visible);
            assert_eq!((pointer.Position.x, pointer.Position.y), (x, y));
        }
    }

    #[test]
    fn dxgi_monochrome_decode_produces_expected_pixels() {
        let shape = decode_monochrome_dxgi_shape(
            0,
            0,
            2,
            2,
            1,
            &[0b1000_0000, 0b0100_0000, 0b0100_0000, 0b1000_0000],
        )
        .expect("shape should decode");

        assert_eq!(shape.width, 2);
        assert_eq!(shape.height, 2);
        assert_eq!(shape.composition_mode, CursorCompositionMode::MaskedColor);
    }
}
