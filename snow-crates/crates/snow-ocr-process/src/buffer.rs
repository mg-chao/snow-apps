use memmap2::Mmap;
use snow_memory::RasterBuffer;
use std::{
    io,
    sync::{
        Arc,
        atomic::{Ordering, fence},
    },
};

pub(super) const SLOT_HEADER: usize = 32;
const SLOT_SEQUENCE: usize = 0;
const SLOT_STATE: usize = 8;
const SLOT_WIDTH: usize = 12;
const SLOT_HEIGHT: usize = 16;
const SLOT_STRIDE: usize = 20;
const SLOT_BYTES: usize = 24;
const SLOT_MAGIC: usize = 28;
const SLOT_READY: u32 = 1;
const SLOT_MAGIC_VALUE: u32 = 0x544f4c53;

pub(super) struct SharedImage {
    pub(super) mmap: Arc<Mmap>,
    pub(super) slot_bytes: usize,
}
impl SharedImage {
    pub(super) fn read_bgr(
        &self,
        slot: usize,
        width: usize,
        height: usize,
        stride: usize,
        sequence: u64,
    ) -> io::Result<RasterBuffer> {
        if width == 0
            || height == 0
            || width
                .checked_mul(height)
                .is_none_or(|pixels| pixels > 3840 * 2160)
            || stride != width.saturating_mul(4)
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid OCR image dimensions",
            ));
        }
        let required = stride
            .checked_mul(height)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "OCR image size overflow"))?;
        if required > self.slot_bytes.saturating_sub(SLOT_HEADER) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR image exceeds shared-memory slot",
            ));
        }
        let start = slot
            .checked_mul(self.slot_bytes)
            .and_then(|v| v.checked_add(SLOT_HEADER))
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "invalid OCR shared-memory slot")
            })?;
        let end = start.checked_add(required).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "OCR image range overflow")
        })?;
        if end > self.mmap.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR shared-memory slot is out of range",
            ));
        }
        let header_start = slot.checked_mul(self.slot_bytes).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "invalid OCR shared-memory slot")
        })?;
        let header = self
            .mmap
            .get(header_start..header_start + SLOT_HEADER)
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "OCR shared-memory slot header is out of range",
                )
            })?;
        fence(Ordering::Acquire);
        let header_sequence =
            u64::from_le_bytes(header[SLOT_SEQUENCE..SLOT_SEQUENCE + 8].try_into().unwrap());
        let state = u32::from_le_bytes(header[SLOT_STATE..SLOT_STATE + 4].try_into().unwrap());
        let header_width =
            u32::from_le_bytes(header[SLOT_WIDTH..SLOT_WIDTH + 4].try_into().unwrap()) as usize;
        let header_height =
            u32::from_le_bytes(header[SLOT_HEIGHT..SLOT_HEIGHT + 4].try_into().unwrap()) as usize;
        let header_stride =
            u32::from_le_bytes(header[SLOT_STRIDE..SLOT_STRIDE + 4].try_into().unwrap()) as usize;
        let header_bytes =
            u32::from_le_bytes(header[SLOT_BYTES..SLOT_BYTES + 4].try_into().unwrap()) as usize;
        let header_magic =
            u32::from_le_bytes(header[SLOT_MAGIC..SLOT_MAGIC + 4].try_into().unwrap());
        if state != SLOT_READY
            || header_sequence != sequence
            || header_width != width
            || header_height != height
            || header_stride != stride
            || header_bytes != required
            || header_magic != SLOT_MAGIC_VALUE
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR shared-memory slot ownership mismatch",
            ));
        }
        let source = &self.mmap[start..end];
        let mut bgr = RasterBuffer::try_zeroed(width * height * 3)?;
        // The packed layout and complete byte range have already been checked.
        // Borrow once so each pixel writes directly into its final allocation.
        for (pixel, output) in source
            .chunks_exact(4)
            .zip(bgr.as_mut_slice().chunks_exact_mut(3))
        {
            output.copy_from_slice(&[pixel[2], pixel[1], pixel[0]]);
        }
        Ok(bgr)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use memmap2::MmapMut;

    #[test]
    fn large_bgr_transfer_keeps_mapped_ownership_through_recognition_input() {
        let (width, height) = (1024, 512);
        let stride = width * 4;
        let size = stride * height;
        let mut map = MmapMut::map_anon(SLOT_HEADER + size).unwrap();
        map[SLOT_SEQUENCE..SLOT_SEQUENCE + 8].copy_from_slice(&7_u64.to_le_bytes());
        for (offset, value) in [
            (SLOT_STATE, SLOT_READY),
            (SLOT_WIDTH, width as u32),
            (SLOT_HEIGHT, height as u32),
            (SLOT_STRIDE, stride as u32),
            (SLOT_BYTES, size as u32),
            (SLOT_MAGIC, SLOT_MAGIC_VALUE),
        ] {
            map[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        for pixel in map[SLOT_HEADER..].chunks_exact_mut(4) {
            pixel.copy_from_slice(&[1, 2, 3, 255]);
        }
        let mapped = SharedImage {
            mmap: Arc::new(map.make_read_only().unwrap()),
            slot_bytes: SLOT_HEADER + size,
        };
        let pixels = mapped.read_bgr(0, width, height, stride, 7).unwrap();
        let pointer = pixels.as_ptr();
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        assert!(pixels.is_page_backed());
        drop(mapped);
        let image = rapid_ocr_rs::RecImage::from_bgr_buffer(width, height, pixels).unwrap();
        assert_eq!(
            image.as_bytes().as_ptr(),
            pointer,
            "staging must not copy the raster again"
        );
        assert!(
            image
                .as_bytes()
                .chunks_exact(3)
                .all(|pixel| pixel == [3, 2, 1])
        );
    }

    fn image(change: impl FnOnce(&mut [u8])) -> SharedImage {
        let mut map = MmapMut::map_anon(SLOT_HEADER + 8).unwrap();
        map[SLOT_SEQUENCE..SLOT_SEQUENCE + 8].copy_from_slice(&7_u64.to_le_bytes());
        for (offset, value) in [
            (SLOT_STATE, SLOT_READY),
            (SLOT_WIDTH, 2),
            (SLOT_HEIGHT, 1),
            (SLOT_STRIDE, 8),
            (SLOT_BYTES, 8),
            (SLOT_MAGIC, SLOT_MAGIC_VALUE),
        ] {
            map[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        map[SLOT_HEADER..].copy_from_slice(&[1, 2, 3, 255, 4, 5, 6, 255]);
        change(&mut map);
        SharedImage {
            mmap: Arc::new(map.make_read_only().unwrap()),
            slot_bytes: SLOT_HEADER + 8,
        }
    }

    #[test]
    fn copied_pixels_survive_mapping_release() {
        let mapped = image(|_| {});
        let pixels = mapped.read_bgr(0, 2, 1, 8, 7).unwrap();
        drop(mapped);
        assert_eq!(pixels, [3, 2, 1, 6, 5, 4]);
    }

    #[test]
    fn packed_odd_rows_and_slot_offsets_preserve_bgr_and_ignore_alpha() {
        for (width, height) in [(1, 1), (3, 2), (7, 3), (1921, 1)] {
            let stride = width * 4;
            let size = stride * height;
            let slot_bytes = SLOT_HEADER + size;
            let mut map = MmapMut::map_anon(slot_bytes * 2).unwrap();
            map[..slot_bytes].fill(0xab);
            let header = &mut map[slot_bytes..slot_bytes + SLOT_HEADER];
            header[SLOT_SEQUENCE..SLOT_SEQUENCE + 8].copy_from_slice(&9_u64.to_le_bytes());
            for (offset, value) in [
                (SLOT_STATE, SLOT_READY),
                (SLOT_WIDTH, width as u32),
                (SLOT_HEIGHT, height as u32),
                (SLOT_STRIDE, stride as u32),
                (SLOT_BYTES, size as u32),
                (SLOT_MAGIC, SLOT_MAGIC_VALUE),
            ] {
                header[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            }
            let mut expected = Vec::with_capacity(width * height * 3);
            for (index, pixel) in map[slot_bytes + SLOT_HEADER..]
                .chunks_exact_mut(4)
                .enumerate()
            {
                let red = (index % 251) as u8;
                let green = ((index * 3 + 5) % 251) as u8;
                let blue = ((index * 7 + 11) % 251) as u8;
                let alpha = ((index * 13) % 256) as u8;
                pixel.copy_from_slice(&[red, green, blue, alpha]);
                expected.extend_from_slice(&[blue, green, red]);
            }
            let image = SharedImage {
                mmap: Arc::new(map.make_read_only().unwrap()),
                slot_bytes,
            };
            let pixels = image.read_bgr(1, width, height, stride, 9).unwrap();
            assert_eq!(pixels.len(), width * height * 3);
            assert_eq!(pixels, expected);
            assert!(image.read_bgr(1, width, height, stride + 1, 9).is_err());
            assert!(image.read_bgr(1, width, height, stride - 1, 9).is_err());
        }
    }

    #[test]
    fn invalid_dimensions_capacity_and_ownership_are_rejected() {
        let mapped = image(|_| {});
        assert!(mapped.read_bgr(0, 2, 1, 8, 6).is_err());
        assert!(mapped.read_bgr(1, 2, 1, 8, 7).is_err());
        assert!(mapped.read_bgr(0, 3, 1, 12, 7).is_err());
        assert!(mapped.read_bgr(0, 2, 1, 9, 7).is_err());
        assert!(mapped.read_bgr(0, 0, 1, 0, 7).is_err());
        assert!(mapped.read_bgr(0, 3840, 2161, 3840 * 4, 7).is_err());
        assert!(mapped.read_bgr(0, usize::MAX, 2, usize::MAX, 7).is_err());
        for offset in [
            SLOT_STATE,
            SLOT_WIDTH,
            SLOT_HEIGHT,
            SLOT_STRIDE,
            SLOT_BYTES,
            SLOT_MAGIC,
        ] {
            let broken = image(|map| map[offset] ^= 1);
            assert!(broken.read_bgr(0, 2, 1, 8, 7).is_err());
        }
    }
}
