//! CPU raster storage with explicit whole-region release. Small buffers use the
//! heap; large buffers own OS pages. Cloning preserves that allocation policy,
//! including when used with `Arc::make_mut` for copy-on-write.
use std::{
    fmt, io,
    ops::{Deref, DerefMut},
};

mod array;
mod pages;

pub use array::{RasterArray, RasterElement};

pub const MIN_PAGE_BUFFER_BYTES: usize = 1024 * 1024;

enum Storage {
    Heap(Vec<u8>),
    #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
    Pages(pages::PageAllocation),
}

/// Initialized, exclusively mutable bytes. Capacity remains alive until this
/// owner is dropped; `clear` and `truncate` deliberately support active reuse.
pub struct RasterBuffer {
    storage: Storage,
    len: usize,
}

impl RasterBuffer {
    pub fn new() -> Self {
        Self {
            storage: Storage::Heap(Vec::new()),
            len: 0,
        }
    }

    pub fn try_with_capacity(capacity: usize) -> io::Result<Self> {
        if capacity > isize::MAX as usize {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "raster capacity overflow",
            ));
        }
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        if capacity >= MIN_PAGE_BUFFER_BYTES {
            // Leave a small initialized SIMD tail so consumers such as swscale
            // can append padding without reallocating an entire large raster.
            let capacity = capacity
                .checked_add(64)
                .filter(|&size| size <= isize::MAX as usize)
                .unwrap_or(capacity);
            return pages::PageAllocation::new(capacity).map(|pages| Self {
                storage: Storage::Pages(pages),
                len: 0,
            });
        }
        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(capacity)
            .map_err(io::Error::other)?;
        // Keep all capacity initialized so growing a slice never exposes
        // uninitialized bytes, even after truncation and recycling.
        bytes.resize(capacity, 0);
        Ok(Self {
            storage: Storage::Heap(bytes),
            len: 0,
        })
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self::try_with_capacity(capacity).unwrap_or_else(|_| allocation_failed(capacity))
    }

    pub fn try_zeroed(len: usize) -> io::Result<Self> {
        let mut buffer = Self::try_with_capacity(len)?;
        buffer.len = len;
        Ok(buffer)
    }

    pub fn zeroed(len: usize) -> Self {
        Self::try_zeroed(len).unwrap_or_else(|_| allocation_failed(len))
    }

    pub fn len(&self) -> usize {
        self.len
    }
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    pub fn capacity(&self) -> usize {
        match &self.storage {
            Storage::Heap(bytes) => bytes.len(),
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(pages) => pages.capacity,
        }
    }

    pub fn is_page_backed(&self) -> bool {
        match &self.storage {
            Storage::Heap(_) => false,
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(_) => true,
        }
    }

    pub fn try_reserve(&mut self, additional: usize) -> io::Result<()> {
        let capacity = self
            .len
            .checked_add(additional)
            .filter(|&size| size <= isize::MAX as usize)
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "raster capacity overflow")
            })?;
        if capacity > self.capacity() {
            // Unknown-length producers grow with bounded headroom; planned raster
            // producers use with_capacity and allocate once.
            let capacity =
                capacity.max(self.capacity().saturating_add(self.capacity() / 8).max(64));
            let mut replacement = Self::try_with_capacity(capacity)?;
            replacement.len = self.len;
            replacement.copy_from_slice(self);
            *self = replacement;
        }
        Ok(())
    }

    pub fn resize(&mut self, len: usize, value: u8) {
        if len > self.len {
            let old_len = self.len;
            let fresh_capacity = len > self.capacity();
            self.try_reserve(len - old_len)
                .unwrap_or_else(|_| allocation_failed(len));
            self.len = len;
            if !fresh_capacity || value != 0 {
                self[old_len..].fill(value);
            }
        } else {
            self.len = len;
        }
    }

    /// Prepare initialized bytes for a producer that overwrites the complete
    /// output. Retained contents are unspecified, avoiding a redundant clear
    /// during capture recycling. This never exposes uninitialized memory.
    pub fn resize_for_overwrite(&mut self, len: usize) {
        if len > self.len {
            self.try_reserve(len - self.len)
                .unwrap_or_else(|_| allocation_failed(len));
        }
        self.len = len;
    }

    pub fn truncate(&mut self, len: usize) {
        self.len = self.len.min(len);
    }
    pub fn clear(&mut self) {
        self.len = 0;
    }

    pub fn extend_from_slice(&mut self, bytes: &[u8]) {
        let old_len = self.len;
        let len = old_len
            .checked_add(bytes.len())
            .expect("raster length overflow");
        self.try_reserve(bytes.len())
            .unwrap_or_else(|_| allocation_failed(len));
        self.len = len;
        self[old_len..].copy_from_slice(bytes);
    }

    /// Legacy ownership boundary for consumers that require a `Vec`. Raster
    /// pipelines should retain this buffer to avoid copying mapped pixels.
    pub fn into_vec(self) -> Vec<u8> {
        match self.storage {
            Storage::Heap(mut bytes) => {
                bytes.truncate(self.len);
                bytes
            }
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(pages) => pages.as_slice(self.len).to_vec(),
        }
    }

    pub fn as_slice(&self) -> &[u8] {
        self
    }
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        self
    }
}

fn allocation_failed(size: usize) -> ! {
    std::alloc::handle_alloc_error(
        std::alloc::Layout::array::<u8>(size).expect("raster allocation exceeds addressable size"),
    )
}

impl Default for RasterBuffer {
    fn default() -> Self {
        Self::new()
    }
}
impl Clone for RasterBuffer {
    fn clone(&self) -> Self {
        Self::from(self.as_slice())
    }
}
impl Deref for RasterBuffer {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match &self.storage {
            Storage::Heap(bytes) => &bytes[..self.len],
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(pages) => pages.as_slice(self.len),
        }
    }
}
impl DerefMut for RasterBuffer {
    fn deref_mut(&mut self) -> &mut [u8] {
        match &mut self.storage {
            Storage::Heap(bytes) => &mut bytes[..self.len],
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            Storage::Pages(pages) => pages.as_mut_slice(self.len),
        }
    }
}
impl AsRef<[u8]> for RasterBuffer {
    fn as_ref(&self) -> &[u8] {
        self
    }
}
impl From<Vec<u8>> for RasterBuffer {
    fn from(mut bytes: Vec<u8>) -> Self {
        // Imported vectors can have been truncated while retaining a large
        // heap allocation. Rebuild from live bytes so those allocations are
        // released instead of being initialized and retained as small rasters.
        if bytes.capacity() >= MIN_PAGE_BUFFER_BYTES
            && (cfg!(any(windows, target_os = "macos", target_os = "linux"))
                || bytes.len() < MIN_PAGE_BUFFER_BYTES)
        {
            return Self::from(bytes.as_slice());
        }
        let len = bytes.len();
        bytes.resize(bytes.capacity(), 0);
        Self {
            storage: Storage::Heap(bytes),
            len,
        }
    }
}
impl From<&[u8]> for RasterBuffer {
    fn from(bytes: &[u8]) -> Self {
        if bytes.len() < MIN_PAGE_BUFFER_BYTES {
            // The copy initializes every exposed capacity byte. Avoid clearing
            // the heap destination before replacing its complete contents.
            return Self {
                storage: Storage::Heap(bytes.to_vec()),
                len: bytes.len(),
            };
        }
        let mut buffer = Self::zeroed(bytes.len());
        buffer.copy_from_slice(bytes);
        buffer
    }
}
impl<const N: usize> From<[u8; N]> for RasterBuffer {
    fn from(bytes: [u8; N]) -> Self {
        Self::from(bytes.as_slice())
    }
}
impl fmt::Debug for RasterBuffer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("RasterBuffer")
            .field("len", &self.len)
            .field("capacity", &self.capacity())
            .field("page_backed", &self.is_page_backed())
            .finish()
    }
}
impl<T: AsRef<[u8]> + ?Sized> PartialEq<T> for RasterBuffer {
    fn eq(&self, other: &T) -> bool {
        self.as_slice() == other.as_ref()
    }
}
impl Eq for RasterBuffer {}

#[cfg(feature = "serde")]
impl serde::Serialize for RasterBuffer {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serde::Serialize::serialize(self.as_slice(), serializer)
    }
}
#[cfg(feature = "serde")]
fn sequence_growth_capacity(required: usize, hint: Option<usize>) -> usize {
    // Grow only from verified bytes, allowing logarithmic copying without
    // trusting a claimed sequence length. An underestimated hint must not
    // reduce growth to one allocation per decoded chunk.
    let doubled = required.saturating_mul(2).min(isize::MAX as usize);
    hint.filter(|&hint| hint > required)
        .map_or(doubled, |hint| doubled.min(hint))
}

#[cfg(feature = "serde")]
fn finish_deserialized_sequence(buffer: RasterBuffer) -> io::Result<RasterBuffer> {
    // Decoder growth is temporary. Keep no more headroom than ordinary raster
    // growth, and move small final payloads back below the page threshold.
    let allowed = buffer.len.saturating_add(buffer.len / 8).saturating_add(64);
    if buffer.capacity() > allowed
        || (buffer.len < MIN_PAGE_BUFFER_BYTES && buffer.capacity() >= MIN_PAGE_BUFFER_BYTES)
    {
        let mut compact = RasterBuffer::try_zeroed(buffer.len)?;
        compact.copy_from_slice(&buffer);
        Ok(compact)
    } else {
        Ok(buffer)
    }
}

#[cfg(feature = "serde")]
impl<'de> serde::Deserialize<'de> for RasterBuffer {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Visitor;
        impl<'de> serde::de::Visitor<'de> for Visitor {
            type Value = RasterBuffer;
            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a raster byte sequence")
            }

            fn visit_bytes<E: serde::de::Error>(self, bytes: &[u8]) -> Result<Self::Value, E> {
                let mut buffer = RasterBuffer::try_zeroed(bytes.len()).map_err(E::custom)?;
                buffer.copy_from_slice(bytes);
                Ok(buffer)
            }

            fn visit_seq<A: serde::de::SeqAccess<'de>>(
                self,
                mut sequence: A,
            ) -> Result<Self::Value, A::Error> {
                // A sequence hint need not be trustworthy or exact. Allocate
                // only after actual bytes have been successfully decoded.
                let hint = sequence.size_hint();
                let mut buffer = RasterBuffer::new();
                let mut chunk = [0_u8; 8192];
                loop {
                    let mut count = 0;
                    while count < chunk.len() {
                        let Some(byte) = sequence.next_element::<u8>()? else {
                            break;
                        };
                        chunk[count] = byte;
                        count += 1;
                    }
                    if count == 0 {
                        return finish_deserialized_sequence(buffer)
                            .map_err(serde::de::Error::custom);
                    }
                    let start = buffer.len;
                    let required = start
                        .checked_add(count)
                        .filter(|&size| size <= isize::MAX as usize)
                        .ok_or_else(|| {
                            <A::Error as serde::de::Error>::custom("raster capacity overflow")
                        })?;
                    if required > buffer.capacity() {
                        let capacity = if count < chunk.len() {
                            required
                        } else {
                            sequence_growth_capacity(required, hint)
                        };
                        let mut replacement = RasterBuffer::try_with_capacity(capacity)
                            .map_err(serde::de::Error::custom)?;
                        replacement.len = start;
                        replacement.copy_from_slice(&buffer);
                        buffer = replacement;
                    }
                    // Every reserved byte is initialized, including SIMD tail.
                    buffer.len = required;
                    buffer[start..].copy_from_slice(&chunk[..count]);
                    if count < chunk.len() {
                        return finish_deserialized_sequence(buffer)
                            .map_err(serde::de::Error::custom);
                    }
                }
            }
        }
        // Request the original sequence representation for every format. In
        // particular, bincode readers must not allocate a temporary heap raster
        // from an unvalidated length before this fallible visitor runs. A format
        // that supplies bytes for this request can still use visit_bytes above.
        deserializer.deserialize_seq(Visitor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    #[test]
    fn threshold_growth_shrink_and_copy_on_write_keep_storage_policy() {
        let mut small = RasterBuffer::from([1, 2, 3]);
        assert!(!small.is_page_backed());
        small.resize(MIN_PAGE_BUFFER_BYTES + 7, 0x5a);
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        assert!(small.is_page_backed());
        assert_eq!(&small[..3], &[1, 2, 3]);
        let pointer = small.as_ptr();
        small.truncate(3);
        small.resize(10, 0x7f);
        assert_eq!(pointer, small.as_ptr());
        assert_eq!(&small[3..], &[0x7f; 7]);
        small.resize(MIN_PAGE_BUFFER_BYTES, 0);
        let original = Arc::new(small);
        let mut copy = original.clone();
        Arc::make_mut(&mut copy)[0] = 99;
        assert_eq!(original[0], 1);
        assert_eq!(copy[0], 99);
        assert_ne!(original.as_ptr(), copy.as_ptr());
        assert_eq!(original.is_page_backed(), copy.is_page_backed());
    }
    #[test]
    fn reserve_failure_preserves_pixels_and_capacity() {
        let mut buffer = RasterBuffer::from([1, 2, 3]);
        assert!(buffer.try_reserve(usize::MAX).is_err());
        assert_eq!(buffer, [1, 2, 3]);
        assert_eq!(buffer.capacity(), 3);
    }

    #[test]
    fn truncated_vector_import_discards_oversized_heap_capacity() {
        let mut bytes = Vec::with_capacity(MIN_PAGE_BUFFER_BYTES * 2);
        bytes.extend_from_slice(&[17, 43, 91]);
        let buffer = RasterBuffer::from(bytes);
        assert_eq!(buffer, [17, 43, 91]);
        assert_eq!(buffer.len(), 3);
        assert!(buffer.capacity() < MIN_PAGE_BUFFER_BYTES);
        assert!(!buffer.is_page_backed());

        let empty = RasterBuffer::from(Vec::with_capacity(MIN_PAGE_BUFFER_BYTES));
        assert!(empty.is_empty());
        assert_eq!(empty.capacity(), 0);
    }

    #[test]
    fn copied_rasters_preserve_pixels_storage_threshold_and_initialized_capacity() {
        for len in [
            0,
            3,
            MIN_PAGE_BUFFER_BYTES - 1,
            MIN_PAGE_BUFFER_BYTES,
            MIN_PAGE_BUFFER_BYTES + 7,
        ] {
            let pixels: Vec<u8> = (0..len).map(|index| (index % 251) as u8).collect();
            let imported = RasterBuffer::from(pixels.as_slice());
            let cloned = imported.clone();
            for mut buffer in [imported, cloned] {
                assert_eq!(buffer, pixels);
                #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
                assert_eq!(buffer.is_page_backed(), len >= MIN_PAGE_BUFFER_BYTES);
                let capacity = buffer.capacity();
                buffer.resize_for_overwrite(capacity);
                assert_eq!(&buffer[..len], pixels.as_slice());
                assert!(buffer[len..].iter().all(|&byte| byte == 0));
                buffer.truncate(len / 2);
                buffer.resize(len, 0x5a);
                assert_eq!(&buffer[..len / 2], &pixels[..len / 2]);
                assert!(buffer[len / 2..].iter().all(|&byte| byte == 0x5a));
            }
        }

        let mut large = RasterBuffer::zeroed(MIN_PAGE_BUFFER_BYTES);
        large[..3].copy_from_slice(&[17, 43, 91]);
        large.truncate(3);
        let compact = large.clone();
        assert_eq!(compact, [17, 43, 91]);
        assert_eq!(compact.capacity(), 3);
        assert!(!compact.is_page_backed());
    }

    #[cfg(feature = "serde")]
    #[test]
    fn binary_rasters_interoperate_with_legacy_vectors_and_reject_truncated_data() {
        for len in [0, 3, MIN_PAGE_BUFFER_BYTES - 1, MIN_PAGE_BUFFER_BYTES + 7] {
            let legacy: Vec<u8> = (0..len).map(|index| (index % 251) as u8).collect();
            let encoded = bincode::serialize(&legacy).unwrap();
            let raster: RasterBuffer = bincode::deserialize(&encoded).unwrap();
            assert_eq!(raster, legacy);
            let from_reader: RasterBuffer =
                bincode::deserialize_from(std::io::Cursor::new(&encoded)).unwrap();
            assert_eq!(from_reader, legacy);
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            assert_eq!(raster.is_page_backed(), len >= MIN_PAGE_BUFFER_BYTES);
            let raster_encoded = bincode::serialize(&raster).unwrap();
            assert_eq!(raster_encoded, encoded);
            let decoded: Vec<u8> = bincode::deserialize(&raster_encoded).unwrap();
            assert_eq!(decoded, legacy);
            if len != 0 {
                for end in [0, 7, 8, encoded.len() - 1] {
                    assert!(bincode::deserialize::<RasterBuffer>(&encoded[..end]).is_err());
                    assert!(
                        bincode::deserialize_from::<_, RasterBuffer>(std::io::Cursor::new(
                            &encoded[..end]
                        ))
                        .is_err()
                    );
                }
            }
        }
        // A malformed claimed length must fail before allocating its payload.
        assert!(bincode::deserialize::<RasterBuffer>(&u64::MAX.to_le_bytes()).is_err());
        assert!(
            bincode::deserialize_from::<_, RasterBuffer>(std::io::Cursor::new(
                u64::MAX.to_le_bytes()
            ))
            .is_err()
        );
    }

    #[cfg(feature = "serde")]
    #[test]
    fn text_and_unknown_length_sequences_preserve_pixels_and_storage_policy() {
        use serde::Deserialize;
        use serde::de::value::{self, Error, SeqDeserializer};

        let bytes: Vec<u8> = (0..MIN_PAGE_BUFFER_BYTES + 7)
            .map(|index| (index % 251) as u8)
            .collect();
        let encoded = serde_json::to_string(&bytes).unwrap();
        let raster: RasterBuffer = serde_json::from_str(&encoded).unwrap();
        assert_eq!(raster, bytes);
        assert_eq!(serde_json::to_string(&raster).unwrap(), encoded);

        let byte_deserializer = value::BorrowedBytesDeserializer::<Error>::new(&bytes);
        let borrowed = RasterBuffer::deserialize(byte_deserializer).unwrap();
        assert_eq!(borrowed, bytes);

        let mut values = bytes.iter().copied();
        let unknown_length = std::iter::from_fn(|| values.next());
        let sequence = SeqDeserializer::<_, Error>::new(unknown_length);
        let decoded = RasterBuffer::deserialize(sequence).unwrap();
        assert_eq!(decoded, bytes);
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        assert!(raster.is_page_backed() && decoded.is_page_backed() && borrowed.is_page_backed());

        let empty: RasterBuffer = serde_json::from_str("[]").unwrap();
        assert!(empty.is_empty());
        for malformed in ["[1,", "[256]", "[-1]", "[null]", "\"pixels\"", "{}"] {
            assert!(serde_json::from_str::<RasterBuffer>(malformed).is_err());
        }
        let invalid_byte = SeqDeserializer::<_, Error>::new([0_u16, 256].into_iter());
        assert!(RasterBuffer::deserialize(invalid_byte).is_err());
    }

    #[cfg(feature = "serde")]
    #[test]
    fn binary_sequence_fallback_bounds_misleading_hints_and_propagates_errors() {
        use serde::Deserialize;
        use serde::de::{DeserializeSeed, Deserializer, SeqAccess, Visitor, value};

        // A binary format is allowed to deliver a sequence rather than bytes,
        // and its size hint is not an allocation or length guarantee.
        struct BinarySequence {
            values: std::vec::IntoIter<u16>,
            hint: usize,
        }
        impl<'de> SeqAccess<'de> for BinarySequence {
            type Error = value::Error;
            fn next_element_seed<T: DeserializeSeed<'de>>(
                &mut self,
                seed: T,
            ) -> Result<Option<T::Value>, Self::Error> {
                self.values
                    .next()
                    .map(|value| seed.deserialize(value::U16Deserializer::new(value)))
                    .transpose()
            }
            fn size_hint(&self) -> Option<usize> {
                Some(self.hint)
            }
        }
        impl<'de> Deserializer<'de> for BinarySequence {
            type Error = value::Error;
            fn deserialize_any<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
                visitor.visit_seq(self)
            }
            fn is_human_readable(&self) -> bool {
                false
            }
            serde::forward_to_deserialize_any! {
                bool i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 char str string
                bytes byte_buf option unit unit_struct newtype_struct seq tuple tuple_struct
                map struct enum identifier ignored_any
            }
        }

        for hint in [0, 3, usize::MAX] {
            let raster = RasterBuffer::deserialize(BinarySequence {
                values: vec![1, 42, 255].into_iter(),
                hint,
            })
            .unwrap();
            assert_eq!(raster, [1, 42, 255]);
            assert!(raster.capacity() <= MIN_PAGE_BUFFER_BYTES + 64);

            assert!(
                RasterBuffer::deserialize(BinarySequence {
                    values: vec![1, 256].into_iter(),
                    hint,
                })
                .is_err()
            );
        }
        let empty = RasterBuffer::deserialize(BinarySequence {
            values: Vec::new().into_iter(),
            hint: usize::MAX,
        })
        .unwrap();
        assert!(empty.is_empty());
        assert_eq!(empty.capacity(), 0);

        for len in [
            8192,
            8193,
            MIN_PAGE_BUFFER_BYTES - 1,
            MIN_PAGE_BUFFER_BYTES + 8192,
        ] {
            for hint in [0, 1, len / 2, len, len + 1, usize::MAX] {
                let values: Vec<u16> = (0..len).map(|index| (index % 251) as u16).collect();
                let raster = RasterBuffer::deserialize(BinarySequence {
                    values: values.into_iter(),
                    hint,
                })
                .unwrap();
                assert_eq!(raster.len(), len);
                assert!(
                    raster
                        .iter()
                        .enumerate()
                        .all(|(i, &byte)| byte == (i % 251) as u8)
                );
                assert!(
                    raster.capacity() <= len + len / 8 + 64,
                    "len={len}, hint={hint}"
                );
                #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
                assert_eq!(raster.is_page_backed(), len >= MIN_PAGE_BUFFER_BYTES);
            }
        }
    }

    #[cfg(feature = "serde")]
    #[test]
    fn sequence_growth_bounds_verified_headroom_and_prefix_copies() {
        for required in [1, 8192, MIN_PAGE_BUFFER_BYTES, isize::MAX as usize / 2 + 1] {
            for hint in [None, Some(0), Some(1), Some(required), Some(usize::MAX)] {
                let capacity = sequence_growth_capacity(required, hint);
                assert!(capacity >= required);
                assert!(capacity <= required.saturating_mul(2));
                assert!(capacity <= isize::MAX as usize);
            }
            assert_eq!(
                sequence_growth_capacity(required, Some(required + 1)),
                required + 1
            );
        }

        // A 4K RGBA stream needs logarithmic growth even with no hint or a
        // dishonest undershoot/overshoot. SIMD tail is initialized separately.
        let len = 3840 * 2160 * 4;
        for hint in [None, Some(1), Some(len), Some(usize::MAX)] {
            let mut capacity = 0;
            let mut allocations = 0;
            let mut copied = 0;
            for required in (8192..=len).step_by(8192) {
                if required > capacity {
                    capacity = sequence_growth_capacity(required, hint) + 64;
                    allocations += 1;
                    copied += required - 8192;
                    assert!(capacity <= required * 2 + 64);
                }
            }
            // Includes room for one final exact compaction when necessary.
            assert!(
                allocations <= 13,
                "hint={hint:?}, allocations={allocations}"
            );
            assert!(copied < len * 2, "hint={hint:?}, copied={copied}");
        }
    }
}
