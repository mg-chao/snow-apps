#![allow(clippy::missing_safety_doc)]
use std::panic::{AssertUnwindSafe, catch_unwind};
use visual_region_detector::{BgrImage, detect_regions};

#[repr(C)]
pub struct SnowDetectedRegion {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub category: u32,
}
pub struct SnowDetectedRegions {
    regions: Vec<SnowDetectedRegion>,
}
const CATEGORIES: [&str; 7] = [
    "text",
    "text_in_box",
    "image",
    "avatar",
    "icon",
    "message_box",
    "text_block",
];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_detect_visual_regions(
    pixels: *const u8,
    length: usize,
    width: u32,
    height: u32,
    stride: usize,
    result: *mut *mut SnowDetectedRegions,
) -> i32 {
    if result.is_null() {
        return 1;
    }
    unsafe {
        result.write(std::ptr::null_mut());
    }
    catch_unwind(AssertUnwindSafe(|| {
        let row = width as usize * 3;
        let Some(required) = stride
            .checked_mul(height.saturating_sub(1) as usize)
            .and_then(|n| n.checked_add(row))
        else {
            return 1;
        };
        if pixels.is_null()
            || width == 0
            || height == 0
            || u64::from(width) * u64::from(height) > 100_000_000
            || stride < row
            || length < required
        {
            return 1;
        }
        let input = unsafe { std::slice::from_raw_parts(pixels, required) };
        let mut bytes = Vec::with_capacity(row * height as usize);
        for y in 0..height as usize {
            bytes.extend_from_slice(&input[y * stride..y * stride + row]);
        }
        let Ok(image) = BgrImage::from_bgr_bytes(width, height, bytes) else {
            return 1;
        };
        let Ok(regions) = detect_regions(&image) else {
            return 2;
        };
        let regions = regions
            .into_iter()
            .filter_map(|r| {
                let category = CATEGORIES.iter().position(|kind| *kind == r.kind)? as u32;
                Some(SnowDetectedRegion {
                    x: r.rect.x,
                    y: r.rect.y,
                    width: r.rect.w,
                    height: r.rect.h,
                    category,
                })
            })
            .collect();
        unsafe {
            result.write(Box::into_raw(Box::new(SnowDetectedRegions { regions })));
        }
        0
    }))
    .unwrap_or(2)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_detected_regions_data(
    result: *const SnowDetectedRegions,
    count: *mut usize,
) -> *const SnowDetectedRegion {
    if count.is_null() {
        return std::ptr::null();
    }
    let Some(result) = (unsafe { result.as_ref() }) else {
        unsafe {
            count.write(0);
        }
        return std::ptr::null();
    };
    unsafe {
        count.write(result.regions.len());
    }
    result.regions.as_ptr()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_detected_regions_release(result: *mut SnowDetectedRegions) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_invalid_input_and_clears_result() {
        let mut result = std::ptr::dangling_mut();
        assert_eq!(
            unsafe { snow_detect_visual_regions(std::ptr::null(), 0, 1, 1, 3, &mut result) },
            1
        );
        assert!(result.is_null());
        let pixels = [255; 12];
        assert_eq!(
            unsafe {
                snow_detect_visual_regions(pixels.as_ptr(), pixels.len(), 2, 2, 5, &mut result)
            },
            1
        );
    }
}
