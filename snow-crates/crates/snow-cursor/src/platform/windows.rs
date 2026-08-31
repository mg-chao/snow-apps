use std::ffi::c_void;
use std::mem::size_of;
use std::ptr;

use windows::Win32::Graphics::Gdi::{
    BI_RGB, BITMAP, BITMAPINFO, BITMAPINFOHEADER, CreateCompatibleDC, CreateDIBSection,
    DIB_RGB_COLORS, DeleteDC, DeleteObject, GetDIBits, GetObjectW, HBITMAP, HDC, HGDIOBJ,
    SelectObject,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CURSOR_SHOWING, CURSORINFO, DI_NORMAL, DrawIconEx, GetCursorInfo, GetIconInfo, HCURSOR, HICON,
};

use crate::CursorCaptureError;
use crate::CursorShapeCapture;
use crate::model::{CursorCompositionMode, CursorShape};
use crate::sampler::CursorProbe;

/// Samples the global Win32 cursor and retains the last successfully decoded
/// shape. Windows changes the cursor position much more often than its shape;
/// a transient failure to inspect an application-owned cursor must therefore
/// not erase a still-valid shape.
pub(crate) struct WindowsCursorSampler {
    last_shape: Option<CursorShape>,
}

impl WindowsCursorSampler {
    pub(crate) fn new() -> Result<Self, CursorCaptureError> {
        Ok(Self { last_shape: None })
    }

    pub(crate) fn sample_cursor(&mut self) -> Result<CursorProbe, CursorCaptureError> {
        let mut info = CURSORINFO {
            cbSize: size_of::<CURSORINFO>() as u32,
            ..Default::default()
        };

        if unsafe { GetCursorInfo(&mut info) }.is_err() {
            return Err(CursorCaptureError::platform("GetCursorInfo failed"));
        }

        let visible = (info.flags.0 & CURSOR_SHOWING.0) != 0;
        let shape = if info.hCursor.is_invalid() {
            CursorShapeCapture::Unavailable
        } else {
            self.shape_for_cursor(info.hCursor)
        };

        Ok(CursorProbe {
            x: info.ptScreenPos.x,
            y: info.ptScreenPos.y,
            visible,
            shape,
        })
    }

    fn shape_for_cursor(&mut self, cursor: HCURSOR) -> CursorShapeCapture {
        // GetIconInfo accepts an HCURSOR directly. Do not CopyIcon first:
        // application-created cursors need not be copyable, even though the
        // original handle is valid for GetIconInfo and DrawIconEx.
        if let Some(shape) = extract_shape(cursor) {
            self.last_shape = Some(shape.clone());
            return CursorShapeCapture::Captured(shape);
        }

        // Preserve the last authentic image through a transient Win32
        // inspection failure instead of inventing a replacement cursor.
        self.last_shape
            .as_ref()
            .cloned()
            .map(CursorShapeCapture::Captured)
            .unwrap_or(CursorShapeCapture::Unavailable)
    }
}

struct BitmapGuard(HBITMAP);

impl BitmapGuard {
    fn new(handle: HBITMAP) -> Option<Self> {
        (!handle.is_invalid()).then_some(Self(handle))
    }

    fn handle(&self) -> HBITMAP {
        self.0
    }
}

impl Drop for BitmapGuard {
    fn drop(&mut self) {
        let _ = unsafe { DeleteObject(self.0.into()) };
    }
}

struct MemoryDc(HDC);

impl MemoryDc {
    fn new() -> Option<Self> {
        let dc = unsafe { CreateCompatibleDC(None) };
        (!dc.is_invalid()).then_some(Self(dc))
    }
}

impl Drop for MemoryDc {
    fn drop(&mut self) {
        let _ = unsafe { DeleteDC(self.0) };
    }
}

struct SelectedBitmap {
    dc: HDC,
    previous: HGDIOBJ,
}

impl SelectedBitmap {
    fn new(dc: HDC, bitmap: HBITMAP) -> Option<Self> {
        let previous = unsafe { SelectObject(dc, bitmap.into()) };
        let failed = previous.0.is_null() || previous.0 as isize == -1;
        (!failed).then_some(Self { dc, previous })
    }
}

impl Drop for SelectedBitmap {
    fn drop(&mut self) {
        let _ = unsafe { SelectObject(self.dc, self.previous) };
    }
}

fn extract_shape(cursor: HCURSOR) -> Option<CursorShape> {
    let mut icon_info = Default::default();
    if unsafe { GetIconInfo(HICON(cursor.0), &mut icon_info) }.is_err() {
        return None;
    }

    // GetIconInfo allocates both returned bitmaps for the caller. The original
    // application-owned HCURSOR itself remains borrowed and must not be freed.
    let color = BitmapGuard::new(icon_info.hbmColor);
    let mask = BitmapGuard::new(icon_info.hbmMask);
    if let Some(color) = color.as_ref() {
        return extract_color_shape(
            cursor,
            icon_info.xHotspot,
            icon_info.yHotspot,
            color.handle(),
            mask.as_ref().map(BitmapGuard::handle),
        );
    }

    extract_monochrome_shape(icon_info.xHotspot, icon_info.yHotspot, mask?.handle())
}

fn extract_color_shape(
    cursor: HCURSOR,
    hotspot_x: u32,
    hotspot_y: u32,
    color: HBITMAP,
    mask: Option<HBITMAP>,
) -> Option<CursorShape> {
    let (width, height, mut rgba) = read_bitmap_rgba(color)?;

    // DrawIconEx ignores hbmMask for a 32-bit alpha cursor. Render the original
    // HCURSOR so User32 applies the application's actual alpha representation.
    if rgba.chunks_exact(4).any(|pixel| pixel[3] != 0) {
        if let Some(rendered) = render_alpha_cursor_rgba(cursor, width, height) {
            rgba = rendered;
        }
        return Some(CursorShape::from_rgba(
            hotspot_x,
            hotspot_y,
            width,
            height,
            CursorCompositionMode::AlphaBlend,
            rgba,
        ));
    }

    // Non-alpha color cursors use an AND followed by XOR operation. Store the
    // AND bit in alpha; the exporter already implements these four exact ROP
    // combinations for MaskedColor shapes.
    let (mask_width, mask_height, mask_rgba) = read_bitmap_rgba(mask?)?;
    apply_and_mask(
        &mut rgba,
        width,
        height,
        mask_width,
        mask_height,
        &mask_rgba,
    )?;
    Some(CursorShape::from_rgba(
        hotspot_x,
        hotspot_y,
        width,
        height,
        CursorCompositionMode::MaskedColor,
        rgba,
    ))
}

fn extract_monochrome_shape(hotspot_x: u32, hotspot_y: u32, mask: HBITMAP) -> Option<CursorShape> {
    let (width, stacked_height, mask_rgba) = read_bitmap_rgba(mask)?;
    if stacked_height < 2 || stacked_height % 2 != 0 {
        return None;
    }
    let height = stacked_height / 2;
    let mut rgba = vec![0u8; checked_rgba_len(width, height)?];

    // A monochrome hbmMask stacks the AND plane above the XOR plane.
    for y in 0..height {
        for x in 0..width {
            let and_idx = ((y * width + x) * 4) as usize;
            let xor_idx = (((y + height) * width + x) * 4) as usize;
            let and_set = pixel_is_set(&mask_rgba[and_idx..and_idx + 4]);
            let xor_set = pixel_is_set(&mask_rgba[xor_idx..xor_idx + 4]);
            let dst = ((y * width + x) * 4) as usize;
            let value = if xor_set { 255 } else { 0 };
            rgba[dst] = value;
            rgba[dst + 1] = value;
            rgba[dst + 2] = value;
            rgba[dst + 3] = if and_set { 255 } else { 0 };
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

fn render_alpha_cursor_rgba(cursor: HCURSOR, width: u32, height: u32) -> Option<Vec<u8>> {
    let byte_len = checked_rgba_len(width, height)?;
    let width_i32 = i32::try_from(width).ok()?;
    let height_i32 = i32::try_from(height).ok()?;
    let dc = MemoryDc::new()?;
    let mut bits: *mut c_void = ptr::null_mut();
    let bitmap_info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width_i32,
            biHeight: -height_i32,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            ..Default::default()
        },
        ..Default::default()
    };
    let bitmap =
        unsafe { CreateDIBSection(Some(dc.0), &bitmap_info, DIB_RGB_COLORS, &mut bits, None, 0) }
            .ok()?;
    let bitmap = BitmapGuard::new(bitmap)?;
    if bits.is_null() {
        return None;
    }
    let _selection = SelectedBitmap::new(dc.0, bitmap.handle())?;

    unsafe { ptr::write_bytes(bits.cast::<u8>(), 0, byte_len) };
    unsafe {
        DrawIconEx(
            dc.0,
            0,
            0,
            HICON(cursor.0),
            width_i32,
            height_i32,
            0,
            None,
            DI_NORMAL,
        )
    }
    .ok()?;

    let bgra = unsafe { std::slice::from_raw_parts(bits.cast::<u8>(), byte_len) };
    let mut rgba = bgra.to_vec();
    for pixel in rgba.chunks_exact_mut(4) {
        pixel.swap(0, 2);
        // Drawing into transparent black produces premultiplied RGB. The
        // recording model stores straight-alpha RGBA.
        let alpha = u32::from(pixel[3]);
        if alpha > 0 && alpha < 255 {
            for channel in &mut pixel[..3] {
                *channel = ((u32::from(*channel) * 255 + alpha / 2) / alpha).min(255) as u8;
            }
        }
    }

    has_visible_pixel(&rgba).then_some(rgba)
}

fn read_bitmap_rgba(bitmap: HBITMAP) -> Option<(u32, u32, Vec<u8>)> {
    let mut info = BITMAP::default();
    let copied = unsafe {
        GetObjectW(
            bitmap.into(),
            size_of::<BITMAP>() as i32,
            Some((&mut info as *mut BITMAP).cast()),
        )
    };
    if copied != size_of::<BITMAP>() as i32 || info.bmWidth <= 0 || info.bmHeight == 0 {
        return None;
    }

    let width = info.bmWidth as u32;
    let height = info.bmHeight.unsigned_abs();
    let mut bgra = vec![0u8; checked_rgba_len(width, height)?];
    let mut bitmap_info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: i32::try_from(width).ok()?,
            biHeight: -i32::try_from(height).ok()?,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB.0,
            ..Default::default()
        },
        ..Default::default()
    };
    let dc = MemoryDc::new()?;
    let rows = unsafe {
        GetDIBits(
            dc.0,
            bitmap,
            0,
            height,
            Some(bgra.as_mut_ptr().cast()),
            &mut bitmap_info,
            DIB_RGB_COLORS,
        )
    };
    if rows != height as i32 {
        return None;
    }

    for pixel in bgra.chunks_exact_mut(4) {
        pixel.swap(0, 2);
    }
    Some((width, height, bgra))
}

fn apply_and_mask(
    rgba: &mut [u8],
    width: u32,
    height: u32,
    mask_width: u32,
    mask_height: u32,
    mask_rgba: &[u8],
) -> Option<()> {
    if mask_width < width || mask_height < height {
        return None;
    }
    if rgba.len() < checked_rgba_len(width, height)?
        || mask_rgba.len() < checked_rgba_len(mask_width, height)?
    {
        return None;
    }

    for y in 0..height {
        for x in 0..width {
            let mask_idx = ((y * mask_width + x) * 4) as usize;
            let dst_alpha = ((y * width + x) * 4 + 3) as usize;
            rgba[dst_alpha] = if pixel_is_set(&mask_rgba[mask_idx..mask_idx + 4]) {
                255
            } else {
                0
            };
        }
    }
    Some(())
}

fn pixel_is_set(pixel: &[u8]) -> bool {
    pixel[0] > 127 || pixel[1] > 127 || pixel[2] > 127
}

fn checked_rgba_len(width: u32, height: u32) -> Option<usize> {
    (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(4)
}

fn has_visible_pixel(rgba: &[u8]) -> bool {
    rgba.chunks_exact(4).any(|pixel| pixel[3] != 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::UI::WindowsAndMessaging::{CreateCursor, DestroyCursor};

    struct CursorGuard(HCURSOR);

    impl Drop for CursorGuard {
        fn drop(&mut self) {
            let _ = unsafe { DestroyCursor(self.0) };
        }
    }

    #[test]
    fn checked_rgba_len_rejects_overflow() {
        assert_eq!(checked_rgba_len(2, 3), Some(24));
        if usize::BITS == 32 {
            assert_eq!(checked_rgba_len(u32::MAX, u32::MAX), None);
        }
    }

    #[test]
    fn visible_pixel_detection_uses_alpha() {
        assert!(!has_visible_pixel(&[255, 255, 255, 0, 1, 2, 3, 0]));
        assert!(has_visible_pixel(&[0, 0, 0, 0, 1, 2, 3, 1]));
    }

    #[test]
    fn apply_and_mask_preserves_xor_rgb_and_sets_composition_bit() {
        let mut rgba = vec![10, 20, 30, 0, 40, 50, 60, 0];
        let mask = vec![255, 255, 255, 255, 0, 0, 0, 255];

        apply_and_mask(&mut rgba, 2, 1, 2, 1, &mask).unwrap();

        assert_eq!(&rgba[..4], &[10, 20, 30, 255]);
        assert_eq!(&rgba[4..], &[40, 50, 60, 0]);
    }

    #[test]
    fn application_created_monochrome_cursor_is_extracted_from_original_handle() {
        // CreateCursor expects one bit per pixel with scanlines aligned to a
        // 16-bit boundary. These planes describe a 2x2 cursor with all four
        // AND/XOR composition combinations.
        let and_plane = [0b1000_0000, 0, 0b0100_0000, 0];
        let xor_plane = [0b0100_0000, 0, 0b1000_0000, 0];
        let cursor = CursorGuard(
            unsafe {
                CreateCursor(
                    None,
                    1,
                    1,
                    2,
                    2,
                    and_plane.as_ptr().cast(),
                    xor_plane.as_ptr().cast(),
                )
            }
            .expect("CreateCursor should create an application-owned cursor"),
        );

        let shape = extract_shape(cursor.0).expect("original HCURSOR should be extractable");

        assert_eq!((shape.hotspot_x, shape.hotspot_y), (1, 1));
        assert_eq!((shape.width, shape.height), (2, 2));
        assert_eq!(shape.composition_mode, CursorCompositionMode::MaskedColor);
        assert_eq!(shape.shape_rgba.len(), 16);
    }
}
