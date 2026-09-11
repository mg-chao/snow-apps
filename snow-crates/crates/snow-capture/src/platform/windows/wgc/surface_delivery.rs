//! GPU surface delivery ring for the WGC worker.
//!
//! Each updated canonical frame is copied into one of a fixed number of
//! ring textures. Consumers hold clones of the published payload; when the
//! last handle drops, a release message returns the slot to the worker.
//! Publishing with an exhausted ring skips the frame, which surfaces as a
//! duplicate on the next pull, mirroring the bounded-queue backpressure of
//! the CPU readback lane.

use std::sync::Arc;
use std::time::Instant;

use anyhow::Context;
use crossbeam_channel::{Receiver, Sender};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_SHADER_RESOURCE, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, ID3D11Device,
    ID3D11DeviceContext, ID3D11Resource, ID3D11Texture2D,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
use windows::core::Interface;

use crate::error::{CaptureError, CaptureResult};
use crate::surface::{GpuSurfacePayload, SurfaceRingState};

use super::update::CanonicalFrameMetadata;

const SURFACE_RING_CAPACITY: usize = 4;
const SURFACE_RELEASE_CAPACITY: usize = SURFACE_RING_CAPACITY * 2;

#[derive(Clone)]
struct SurfaceSlot {
    texture: ID3D11Texture2D,
    resource: ID3D11Resource,
}

/// One published frame: the shared payload plus the timing metadata the
/// worker needs to stamp duplicates and consumer frames.
pub(super) struct PublishedSurface {
    pub(super) payload: Arc<GpuSurfacePayload>,
    pub(super) generation: u64,
    pub(super) capture_time: Instant,
    pub(super) system_relative_time_hns: i64,
    pub(super) width: u32,
    pub(super) height: u32,
}

impl Clone for PublishedSurface {
    fn clone(&self) -> Self {
        Self {
            payload: Arc::clone(&self.payload),
            generation: self.generation,
            capture_time: self.capture_time,
            system_relative_time_hns: self.system_relative_time_hns,
            width: self.width,
            height: self.height,
        }
    }
}

pub(super) struct SurfaceDeliveryPipeline {
    device: ID3D11Device,
    slots: Vec<Option<SurfaceSlot>>,
    state: SurfaceRingState,
    slot_desc: Option<(u32, u32, DXGI_FORMAT)>,
    release_rx: Receiver<(u64, u32)>,
    release_tx: Sender<(u64, u32)>,
    published: Option<PublishedSurface>,
}

impl SurfaceDeliveryPipeline {
    pub(super) fn new(device: &ID3D11Device) -> Self {
        let (release_tx, release_rx) = crossbeam_channel::bounded(SURFACE_RELEASE_CAPACITY);
        Self {
            device: device.clone(),
            slots: (0..SURFACE_RING_CAPACITY).map(|_| None).collect(),
            state: SurfaceRingState::new(SURFACE_RING_CAPACITY),
            slot_desc: None,
            release_rx,
            release_tx,
            published: None,
        }
    }

    fn drain_releases(&mut self) {
        while let Ok((epoch, slot)) = self.release_rx.try_recv() {
            self.state.release(epoch, slot);
        }
    }

    fn ensure_slots(&mut self, width: u32, height: u32, format: DXGI_FORMAT) -> CaptureResult<()> {
        if self
            .slot_desc
            .is_some_and(|(w, h, f)| w == width && h == height && f == format)
        {
            return Ok(());
        }
        // In-flight payloads keep their own COM references; only the
        // worker-side slots are rebuilt, and stale releases are ignored by
        // the ring-state epoch check.
        self.slots.iter_mut().for_each(|slot| *slot = None);
        self.state.rebuild();
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };
        for slot in &mut self.slots {
            let mut texture = None;
            unsafe { self.device.CreateTexture2D(&desc, None, Some(&mut texture)) }
                .context("CreateTexture2D for WGC surface delivery ring failed")
                .map_err(CaptureError::platform)?;
            let texture = texture.ok_or_else(|| {
                CaptureError::platform(anyhow::anyhow!(
                    "CreateTexture2D for WGC surface delivery ring returned no texture"
                ))
            })?;
            let resource: ID3D11Resource = texture
                .cast()
                .context("failed to cast surface delivery texture to ID3D11Resource")
                .map_err(CaptureError::platform)?;
            *slot = Some(SurfaceSlot { texture, resource });
        }
        self.slot_desc = Some((width, height, format));
        Ok(())
    }

    /// Copy the canonical surface into the next free ring slot. Returns
    /// `Ok(false)` when the ring is exhausted (consumers hold every slot);
    /// the previous publication stays current and pulls then report
    /// duplicates.
    pub(super) fn publish(
        &mut self,
        context: &ID3D11DeviceContext,
        canonical: &ID3D11Resource,
        desc: &D3D11_TEXTURE2D_DESC,
        metadata: &CanonicalFrameMetadata,
    ) -> CaptureResult<bool> {
        self.drain_releases();
        self.ensure_slots(desc.Width, desc.Height, desc.Format)?;
        let Some(slot_index) = self.state.lease() else {
            return Ok(false);
        };
        let slot = self.slots[slot_index as usize]
            .as_ref()
            .expect("leased slot owns a texture");
        unsafe { context.CopyResource(&slot.resource, canonical) };
        self.state.publish(slot_index, metadata.generation);
        let ring_epoch = self.state.epoch();
        let release_tx = self.release_tx.clone();
        let payload = Arc::new(GpuSurfacePayload {
            release: Some(Box::new(move || {
                let _ = release_tx.try_send((ring_epoch, slot_index));
            })),
            device: Some(self.device.clone()),
            texture: Some(slot.texture.clone()),
        });
        self.published = Some(PublishedSurface {
            payload,
            generation: metadata.generation,
            capture_time: metadata.capture_time,
            system_relative_time_hns: metadata.system_relative_time_hns,
            width: desc.Width,
            height: desc.Height,
        });
        Ok(true)
    }

    /// The most recently published frame, if any.
    pub(super) fn take(&mut self) -> Option<PublishedSurface> {
        self.drain_releases();
        self.published.clone()
    }

    /// Generation of the most recent publication.
    pub(super) fn published_generation(&self) -> Option<u64> {
        self.state.published().map(|(generation, _)| generation)
    }
}
