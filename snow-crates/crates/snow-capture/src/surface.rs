//! GPU surface delivery types.
//!
//! The default capture contract reads pixels back into CPU memory. The
//! `SurfaceDelivery::GpuTexture` mode instead keeps frames as D3D11
//! textures on the capture device so downstream consumers (the GPU
//! recording encoder) can process them without any CPU pixel traffic.

use std::sync::Arc;
use std::time::Instant;

use crate::frame::FrameMetadata;

/// Selects how captured frames leave the capture pipeline.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SurfaceDelivery {
    /// Frames are read back into CPU pixel buffers (default).
    #[default]
    CpuPixels,
    /// Frames stay as D3D11 textures on the GPU; no CPU readback happens.
    ///
    /// Windows Graphics Capture only: asking any other backend fails with a
    /// fallback-eligible error when the session prepares its capturer.
    /// `native_cursor` bakes the cursor into the delivered texture using
    /// WGC's own cursor capture instead of the CPU cursor compositor.
    GpuTexture { native_cursor: bool },
}

/// Sub-rectangle of a delivered surface texture that contains the
/// requested capture target. `None` on [`GpuSurfaceFrame`] means the whole
/// texture is the target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceCropRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

/// GPU payload shared by every handle of one delivered surface frame.
///
/// Dropping the last handle runs `release`, which returns the delivery
/// ring slot to the capture worker.
pub(crate) struct GpuSurfacePayload {
    pub(crate) release: Option<Box<dyn FnOnce() + Send + Sync>>,
    #[cfg(target_os = "windows")]
    pub(crate) device: Option<windows::Win32::Graphics::Direct3D11::ID3D11Device>,
    #[cfg(target_os = "windows")]
    pub(crate) texture: Option<windows::Win32::Graphics::Direct3D11::ID3D11Texture2D>,
}

// SAFETY: D3D11 device and texture interfaces are free-threaded. The
// capture device is created without `D3D11_CREATE_DEVICE_SINGLETHREADED`
// (required by WGC's free-threaded frame pool), so the runtime serializes
// cross-thread access internally. The release closure only sends into a
// crossbeam channel.
unsafe impl Send for GpuSurfacePayload {}
unsafe impl Sync for GpuSurfacePayload {}

impl std::fmt::Debug for GpuSurfacePayload {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("GpuSurfacePayload").finish()
    }
}

impl Drop for GpuSurfacePayload {
    fn drop(&mut self) {
        if let Some(release) = self.release.take() {
            release();
        }
    }
}

/// One captured frame delivered as a GPU texture, without CPU readback.
///
/// Cloning shares the underlying payload; the delivery ring slot returns
/// to the capture worker when the last clone drops. Consumers must finish
/// issuing GPU commands that read the texture before dropping the last
/// handle.
#[derive(Clone, Debug)]
pub struct GpuSurfaceFrame {
    width: u32,
    height: u32,
    texture_width: u32,
    texture_height: u32,
    crop: Option<SurfaceCropRect>,
    metadata: FrameMetadata,
    payload: Option<Arc<GpuSurfacePayload>>,
}

impl GpuSurfaceFrame {
    pub(crate) fn new(
        width: u32,
        height: u32,
        texture_width: u32,
        texture_height: u32,
        crop: Option<SurfaceCropRect>,
        metadata: FrameMetadata,
        payload: Option<Arc<GpuSurfacePayload>>,
    ) -> Self {
        Self {
            width,
            height,
            texture_width,
            texture_height,
            crop,
            metadata,
            payload,
        }
    }

    /// Width of the captured target (after applying the crop rect).
    pub fn width(&self) -> u32 {
        self.width
    }

    /// Height of the captured target (after applying the crop rect).
    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn dimensions(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// Dimensions of the full delivered texture (before cropping).
    pub fn texture_dimensions(&self) -> (u32, u32) {
        (self.texture_width, self.texture_height)
    }

    /// Sub-rectangle of the texture that holds the capture target.
    pub fn crop(&self) -> Option<SurfaceCropRect> {
        self.crop
    }

    pub fn metadata(&self) -> &FrameMetadata {
        &self.metadata
    }

    pub fn is_duplicate(&self) -> bool {
        self.metadata.is_duplicate()
    }

    /// The D3D11 texture holding the frame pixels. The texture lives on
    /// the device returned by [`Self::d3d11_device`].
    #[cfg(target_os = "windows")]
    pub fn d3d11_texture(&self) -> Option<windows::Win32::Graphics::Direct3D11::ID3D11Texture2D> {
        self.payload
            .as_ref()
            .and_then(|payload| payload.texture.clone())
    }

    /// The D3D11 device that owns the delivered texture. Consumers that
    /// issue GPU work against the texture must use this device.
    #[cfg(target_os = "windows")]
    pub fn d3d11_device(&self) -> Option<windows::Win32::Graphics::Direct3D11::ID3D11Device> {
        self.payload
            .as_ref()
            .and_then(|payload| payload.device.clone())
    }
}

/// Capture-time description of one surface frame produced by a backend.
///
/// The session layers target-level information (crop rect, sequence
/// number) on top of this before handing a [`GpuSurfaceFrame`] to the
/// consumer.
#[derive(Debug)]
pub(crate) struct BackendSurfaceFrame {
    pub texture_width: u32,
    pub texture_height: u32,
    pub is_duplicate: bool,
    pub capture_time: Instant,
    pub system_relative_time_hns: i64,
    pub backend_kind: crate::backend::CaptureBackendKind,
    pub payload: Option<Arc<GpuSurfacePayload>>,
}

/// Pure bookkeeping for a fixed-size surface delivery ring: which slots
/// are free, which are leased by in-flight frames, and which generation
/// was published most recently. The Windows pipeline owns the actual
/// textures and drives this state machine, which keeps the policy
/// deterministic and unit-testable without a GPU.
#[derive(Clone, Debug)]
pub(crate) struct SurfaceRingState {
    capacity: usize,
    ring_epoch: u64,
    free: Vec<u32>,
    leased: Vec<bool>,
    published: Option<(u64, u32)>,
}

impl SurfaceRingState {
    pub(crate) fn new(capacity: usize) -> Self {
        Self {
            capacity,
            ring_epoch: 1,
            free: (0..capacity as u32).collect(),
            leased: vec![false; capacity],
            published: None,
        }
    }

    pub(crate) fn epoch(&self) -> u64 {
        self.ring_epoch
    }

    /// Lease a slot for a newly published frame. Returns `None` when every
    /// slot is still held by consumers; the publish is then skipped and
    /// consumers observe a duplicate instead.
    pub(crate) fn lease(&mut self) -> Option<u32> {
        let slot = self.free.pop()?;
        debug_assert!(!self.leased[slot as usize]);
        self.leased[slot as usize] = true;
        Some(slot)
    }

    /// Record that `slot` now holds `generation`.
    pub(crate) fn publish(&mut self, slot: u32, generation: u64) {
        debug_assert!(self.leased[slot as usize]);
        self.published = Some((generation, slot));
    }

    /// The most recently published generation and its slot.
    pub(crate) fn published(&self) -> Option<(u64, u32)> {
        self.published
    }

    /// Apply a slot release. Stale ring epochs, unknown slots, and double
    /// releases (the slot is already free) are ignored.
    pub(crate) fn release(&mut self, epoch: u64, slot: u32) -> bool {
        if epoch != self.ring_epoch || slot as usize >= self.capacity {
            return false;
        }
        if !self.leased[slot as usize] {
            return false;
        }
        self.leased[slot as usize] = false;
        self.free.push(slot);
        true
    }

    /// Invalidate the ring after a texture rebuild. In-flight payloads
    /// keep their (now stale) release tokens, which later releases ignore.
    /// Returns the new ring epoch.
    pub(crate) fn rebuild(&mut self) -> u64 {
        self.ring_epoch = self.ring_epoch.wrapping_add(1).max(1);
        self.free = (0..self.capacity as u32).collect();
        self.leased = vec![false; self.capacity];
        self.published = None;
        self.ring_epoch
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn leases_exhaust_then_reuse_after_release() {
        let mut ring = SurfaceRingState::new(3);
        let first = ring.lease().expect("fresh ring must lease");
        ring.publish(first, 1);

        let slots: Vec<u32> = (0..2).map(|_| ring.lease().expect("slot")).collect();
        assert_eq!(ring.lease(), None, "exhausted ring must refuse to lease");

        assert!(ring.release(ring.epoch(), slots[0]));
        assert!(ring.lease().is_some(), "released slot must be reusable");
    }

    #[test]
    fn release_frees_each_slot_at_most_once() {
        let mut ring = SurfaceRingState::new(2);
        let slot = ring.lease().expect("slot");
        assert!(ring.release(ring.epoch(), slot));
        assert!(!ring.release(ring.epoch(), slot), "double release");
        // Lease both slots back; a duplicated free would allow a third.
        assert!(ring.lease().is_some());
        assert!(ring.lease().is_some());
        assert_eq!(ring.lease(), None);
    }

    #[test]
    fn stale_epoch_releases_are_ignored() {
        let mut ring = SurfaceRingState::new(2);
        let slot = ring.lease().expect("slot");
        let stale_epoch = ring.epoch();
        let new_epoch = ring.rebuild();
        assert_ne!(stale_epoch, new_epoch);
        assert!(
            !ring.release(stale_epoch, slot),
            "release from a torn-down ring must be ignored"
        );
        // The rebuilt ring still offers its full capacity.
        assert!(ring.lease().is_some());
        assert!(ring.lease().is_some());
    }

    #[test]
    fn rebuild_clears_published_generation() {
        let mut ring = SurfaceRingState::new(2);
        let slot = ring.lease().expect("slot");
        ring.publish(slot, 7);
        assert_eq!(ring.published(), Some((7, slot)));
        ring.rebuild();
        assert_eq!(ring.published(), None);
    }

    #[test]
    fn out_of_range_slot_release_is_ignored() {
        let mut ring = SurfaceRingState::new(2);
        assert!(!ring.release(ring.epoch(), 99));
    }

    #[test]
    fn cpu_pixel_delivery_is_the_default() {
        assert_eq!(SurfaceDelivery::default(), SurfaceDelivery::CpuPixels);
        assert_ne!(
            SurfaceDelivery::CpuPixels,
            SurfaceDelivery::GpuTexture {
                native_cursor: true
            }
        );
    }
}
