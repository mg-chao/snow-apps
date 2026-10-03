//! Typed raster scratch with guaranteed element alignment and valid zero bytes.
use std::{
    fmt, io,
    ops::{Deref, DerefMut},
};

#[cfg(any(windows, target_os = "macos", target_os = "linux", test))]
use crate::MIN_PAGE_BUFFER_BYTES;
#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
use crate::RasterBuffer;
use crate::allocation_failed;

mod sealed {
    pub trait Element {}
}

/// Numeric raster elements whose zero representation is valid. This trait is
/// sealed so arbitrary types with invalid bit patterns cannot enter OS storage.
pub trait RasterElement: sealed::Element + Copy + Default {}
macro_rules! elements {
    ($($element:ty),*) => { $(
        impl sealed::Element for $element {}
        impl RasterElement for $element {}
    )* };
}
elements!(
    u8, u16, u32, u64, u128, usize, i8, i16, i32, i64, i128, isize, f32, f64
);

enum Storage<T> {
    Heap(Vec<T>),
    #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
    Pages(RasterBuffer),
}

/// Fixed-size numeric raster workspace. Large arrays share the byte buffer's
/// OS ownership policy; small arrays retain Vec's element alignment.
pub struct RasterArray<T: RasterElement> {
    storage: Storage<T>,
    len: usize,
}

impl<T: RasterElement> RasterArray<T> {
    pub fn try_zeroed(len: usize) -> io::Result<Self> {
        let bytes = len
            .checked_mul(std::mem::size_of::<T>())
            .filter(|&size| size <= isize::MAX as usize)
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "raster array size overflow")
            })?;
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        if bytes >= MIN_PAGE_BUFFER_BYTES {
            let buffer = RasterBuffer::try_zeroed(bytes)?;
            debug_assert!(buffer.is_page_backed());
            debug_assert!(buffer.as_ptr().cast::<T>().is_aligned());
            return Ok(Self {
                storage: Storage::Pages(buffer),
                len,
            });
        }
        let _ = bytes;
        let mut values = Vec::new();
        values.try_reserve_exact(len).map_err(io::Error::other)?;
        values.resize(len, T::default());
        Ok(Self {
            storage: Storage::Heap(values),
            len,
        })
    }

    pub fn zeroed(len: usize) -> Self {
        Self::try_zeroed(len).unwrap_or_else(|_| {
            allocation_failed(
                len.checked_mul(std::mem::size_of::<T>())
                    .expect("raster array size overflow"),
            )
        })
    }

    pub fn is_page_backed(&self) -> bool {
        match &self.storage {
            Storage::Heap(_) => false,
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(_) => true,
        }
    }
}

impl<T: RasterElement> Deref for RasterArray<T> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        match &self.storage {
            Storage::Heap(values) => values,
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(bytes) => {
                // Native pages are aligned for all sealed numeric elements.
                // Every bit pattern of these types is valid; len was checked
                // against the byte allocation at construction.
                unsafe { std::slice::from_raw_parts(bytes.as_ptr().cast::<T>(), self.len) }
            }
        }
    }
}
impl<T: RasterElement> DerefMut for RasterArray<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        match &mut self.storage {
            Storage::Heap(values) => values,
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(bytes) => unsafe {
                std::slice::from_raw_parts_mut(bytes.as_mut_ptr().cast::<T>(), self.len)
            },
        }
    }
}
impl<T: RasterElement> Clone for RasterArray<T> {
    fn clone(&self) -> Self {
        let mut copy = Self::zeroed(self.len);
        copy.copy_from_slice(self);
        copy
    }
}
impl<T: RasterElement> fmt::Debug for RasterArray<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RasterArray")
            .field("len", &self.len)
            .field("page_backed", &self.is_page_backed())
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn typed_scratch_is_aligned_zeroed_and_cloned_with_its_allocation_policy() {
        for len in [7, MIN_PAGE_BUFFER_BYTES / 4] {
            let mut values = RasterArray::<f32>::zeroed(len);
            assert!(values.as_ptr().is_aligned());
            assert!(values.iter().all(|&value| value == 0.0));
            values[0] = 0.5;
            let copy = values.clone();
            values[0] = 1.0;
            assert_eq!(copy[0], 0.5);
            assert_eq!(copy.is_page_backed(), values.is_page_backed());
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            assert_eq!(values.is_page_backed(), len * 4 >= MIN_PAGE_BUFFER_BYTES);
        }
        assert!(RasterArray::<u64>::try_zeroed(usize::MAX).is_err());
    }
}
