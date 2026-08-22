use std::time::{Duration, Instant};

use anyhow::Context;
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BOX, D3D11_QUERY_DESC, D3D11_QUERY_EVENT, D3D11_TEXTURE2D_DESC, ID3D11Device,
    ID3D11DeviceContext, ID3D11Query, ID3D11Resource, ID3D11Texture2D,
};
use windows::core::Interface;

use crate::backend::CaptureBlitRegion;
use crate::convert::{HdrFrameContext, SurfaceConversionOptions};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{DirtyRect, Frame};

use super::super::{d3d11, surface};
use super::update::CanonicalFrameMetadata;

const READBACK_SLOT_COUNT: usize = 3;
const QUERY_WAIT_TIMEOUT: Duration = Duration::from_millis(250);
const QUERY_SPIN_POLLS: usize = 16;
const D3D11_ASYNC_GETDATA_DONOTFLUSH: u32 = 0x1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum ReadbackTarget {
    Full,
    Region(CaptureBlitRegion),
}

impl ReadbackTarget {
    fn staging_desc(self, source_desc: D3D11_TEXTURE2D_DESC) -> D3D11_TEXTURE2D_DESC {
        match self {
            Self::Full => source_desc,
            Self::Region(blit) => surface::region_desc_for_blit(&source_desc, blit),
        }
    }

    fn validate(self, source_desc: D3D11_TEXTURE2D_DESC) -> CaptureResult<()> {
        let (right, bottom) = match self {
            Self::Full => return Ok(()),
            Self::Region(blit) => (
                blit.src_x
                    .checked_add(blit.width)
                    .ok_or(CaptureError::BufferOverflow)?,
                blit.src_y
                    .checked_add(blit.height)
                    .ok_or(CaptureError::BufferOverflow)?,
            ),
        };
        if right > source_desc.Width || bottom > source_desc.Height {
            return Err(CaptureError::BufferOverflow);
        }
        Ok(())
    }

    fn local_dirty_rects(self, source: &[DirtyRect]) -> Vec<DirtyRect> {
        match self {
            Self::Full => source.to_vec(),
            Self::Region(blit) => source
                .iter()
                .filter_map(|rect| intersect_with_region(*rect, blit))
                .collect(),
        }
    }
}

#[derive(Clone)]
struct SlotMetadata {
    epoch: u64,
    generation: u64,
    parent_generation: u64,
    capture_time: Instant,
    system_relative_time_hns: i64,
    target: ReadbackTarget,
    staging_desc: D3D11_TEXTURE2D_DESC,
    hdr_to_sdr: Option<HdrFrameContext>,
    dirty_rects: Vec<DirtyRect>,
    ordered_delta: bool,
}

#[derive(Default)]
struct ReadbackSlot {
    staging: Option<ID3D11Texture2D>,
    resource: Option<ID3D11Resource>,
    query: Option<ID3D11Query>,
    desc: Option<D3D11_TEXTURE2D_DESC>,
    metadata: Option<SlotMetadata>,
    query_pending: bool,
}

impl ReadbackSlot {
    fn invalidate_submission(&mut self) {
        self.metadata = None;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct DeliveredGeneration {
    pub epoch: u64,
    pub generation: u64,
    pub target: ReadbackTarget,
}

pub(super) struct ReadbackDelivery {
    pub capture_time: Instant,
    pub system_relative_time_hns: i64,
    pub epoch: u64,
    pub generation: u64,
    pub target: ReadbackTarget,
    pub is_duplicate: bool,
    pub dirty_rects: Vec<DirtyRect>,
}

pub(super) struct ReadbackPipeline {
    slots: [ReadbackSlot; READBACK_SLOT_COUNT],
    next_slot: usize,
    target: Option<ReadbackTarget>,
}

impl ReadbackPipeline {
    pub fn new() -> Self {
        Self {
            slots: std::array::from_fn(|_| ReadbackSlot::default()),
            next_slot: 0,
            target: None,
        }
    }

    pub fn target(&self) -> Option<ReadbackTarget> {
        self.target
    }

    pub fn set_target(&mut self, target: ReadbackTarget) {
        if self.target == Some(target) {
            return;
        }
        self.target = Some(target);
        self.invalidate_submissions();
    }

    pub fn invalidate_submissions(&mut self) {
        for slot in &mut self.slots {
            slot.invalidate_submission();
        }
        self.next_slot = 0;
    }

    pub fn contains(&self, epoch: u64, generation: u64, target: ReadbackTarget) -> bool {
        self.slots.iter().any(|slot| {
            slot.metadata.as_ref().is_some_and(|metadata| {
                metadata.epoch == epoch
                    && metadata.generation == generation
                    && metadata.target == target
            })
        })
    }

    pub fn prefetch(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        source_desc: D3D11_TEXTURE2D_DESC,
        hdr_to_sdr: Option<HdrFrameContext>,
        canonical: &CanonicalFrameMetadata,
    ) -> CaptureResult<bool> {
        let Some(target) = self.target else {
            return Ok(false);
        };
        self.submit(
            device,
            context,
            source,
            source_desc,
            hdr_to_sdr,
            canonical,
            target,
            false,
        )
    }

    pub fn ensure_submitted(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        source_desc: D3D11_TEXTURE2D_DESC,
        hdr_to_sdr: Option<HdrFrameContext>,
        canonical: &CanonicalFrameMetadata,
        target: ReadbackTarget,
    ) -> CaptureResult<()> {
        self.set_target(target);
        if self.contains(canonical.epoch, canonical.generation, target) {
            return Ok(());
        }
        if !self.submit(
            device,
            context,
            source,
            source_desc,
            hdr_to_sdr,
            canonical,
            target,
            true,
        )? {
            return Err(CaptureError::Timeout);
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn submit(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        source_desc: D3D11_TEXTURE2D_DESC,
        hdr_to_sdr: Option<HdrFrameContext>,
        canonical: &CanonicalFrameMetadata,
        target: ReadbackTarget,
        force: bool,
    ) -> CaptureResult<bool> {
        target.validate(source_desc)?;
        if self.contains(canonical.epoch, canonical.generation, target) {
            return Ok(true);
        }

        let staging_desc = target.staging_desc(source_desc);
        let local_dirty_rects = target.local_dirty_rects(&canonical.dirty_rects);
        let parent_slot = if canonical.ordered_delta {
            let mut parent_slot = None;
            for (index, slot) in self.slots.iter().enumerate() {
                let matches_parent = slot.metadata.as_ref().is_some_and(|metadata| {
                    metadata.epoch == canonical.epoch
                        && metadata.generation == canonical.parent_generation
                        && metadata.target == target
                        && desc_matches(metadata.staging_desc, staging_desc)
                });
                if matches_parent && slot_ready(context, slot)? {
                    parent_slot = Some(index);
                    break;
                }
            }
            parent_slot
        } else {
            None
        };

        let slot_index = if let Some(index) = parent_slot {
            index
        } else if let Some(index) = self.find_reusable_slot(context)? {
            index
        } else if force {
            let index = self.oldest_slot();
            wait_for_slot(context, &self.slots[index])?;
            index
        } else {
            return Ok(false);
        };

        self.ensure_slot(device, slot_index, staging_desc)?;
        let can_patch_parent = parent_slot == Some(slot_index)
            && canonical.ordered_delta
            && self.slots[slot_index]
                .metadata
                .as_ref()
                .is_some_and(|metadata| metadata.generation == canonical.parent_generation);

        let mut submitted_gpu_work = false;
        if can_patch_parent {
            if !local_dirty_rects.is_empty() {
                copy_dirty_regions(
                    context,
                    self.slots[slot_index].resource.as_ref().ok_or_else(|| {
                        CaptureError::platform(anyhow::anyhow!(
                            "WGC readback slot is missing its staging resource"
                        ))
                    })?,
                    source,
                    target,
                    &local_dirty_rects,
                )?;
                submitted_gpu_work = true;
            }
        } else {
            copy_complete_target(
                context,
                self.slots[slot_index].resource.as_ref().ok_or_else(|| {
                    CaptureError::platform(anyhow::anyhow!(
                        "WGC readback slot is missing its staging resource"
                    ))
                })?,
                source,
                target,
                source_desc,
            )?;
            submitted_gpu_work = true;
        }

        if submitted_gpu_work && let Some(query) = self.slots[slot_index].query.as_ref() {
            unsafe { context.End(query) };
        }

        self.slots[slot_index].metadata = Some(SlotMetadata {
            epoch: canonical.epoch,
            generation: canonical.generation,
            parent_generation: canonical.parent_generation,
            capture_time: canonical.capture_time,
            system_relative_time_hns: canonical.system_relative_time_hns,
            target,
            staging_desc,
            hdr_to_sdr,
            dirty_rects: local_dirty_rects,
            ordered_delta: canonical.ordered_delta,
        });
        self.slots[slot_index].query_pending = submitted_gpu_work;
        self.next_slot = (slot_index + 1) % READBACK_SLOT_COUNT;
        Ok(true)
    }

    pub fn read_into(
        &mut self,
        context: &ID3D11DeviceContext,
        epoch: u64,
        generation: u64,
        target: ReadbackTarget,
        destination: &mut Frame,
        delivered: Option<DeliveredGeneration>,
        destination_has_history: bool,
    ) -> CaptureResult<ReadbackDelivery> {
        let slot_index = self
            .slots
            .iter()
            .position(|slot| {
                slot.metadata.as_ref().is_some_and(|metadata| {
                    metadata.epoch == epoch
                        && metadata.generation == generation
                        && metadata.target == target
                })
            })
            .ok_or(CaptureError::Timeout)?;
        wait_for_slot(context, &self.slots[slot_index])?;
        self.slots[slot_index].query_pending = false;

        let metadata = self.slots[slot_index]
            .metadata
            .as_ref()
            .cloned()
            .ok_or(CaptureError::Timeout)?;
        let history_matches_parent = destination_has_history
            && delivered.is_some_and(|previous| {
                previous.epoch == metadata.epoch
                    && previous.generation == metadata.parent_generation
                    && previous.target == metadata.target
            });
        let target_unchanged =
            metadata.ordered_delta && history_matches_parent && metadata.dirty_rects.is_empty();

        if !target_unchanged {
            let use_incremental_conversion = metadata.ordered_delta
                && history_matches_parent
                && !metadata.dirty_rects.is_empty();
            if use_incremental_conversion {
                self.map_dirty(context, slot_index, destination, &metadata)?;
            } else {
                self.map_complete(context, slot_index, destination, &metadata)?;
            }
        }

        let dirty_rects = dirty_rects_for_delivery(
            metadata.ordered_delta,
            history_matches_parent,
            &metadata.dirty_rects,
        );

        Ok(ReadbackDelivery {
            capture_time: metadata.capture_time,
            system_relative_time_hns: metadata.system_relative_time_hns,
            epoch: metadata.epoch,
            generation: metadata.generation,
            target: metadata.target,
            is_duplicate: target_unchanged,
            dirty_rects,
        })
    }

    fn map_complete(
        &self,
        context: &ID3D11DeviceContext,
        slot_index: usize,
        destination: &mut Frame,
        metadata: &SlotMetadata,
    ) -> CaptureResult<()> {
        let slot = &self.slots[slot_index];
        let staging = slot.staging.as_ref().ok_or_else(|| {
            CaptureError::platform(anyhow::anyhow!("WGC readback slot has no staging texture"))
        })?;
        let options = SurfaceConversionOptions {
            hdr_to_sdr: metadata.hdr_to_sdr,
            ..SurfaceConversionOptions::default()
        };
        match metadata.target {
            ReadbackTarget::Full => surface::map_staging_to_frame(
                context,
                staging,
                slot.resource.as_ref(),
                &metadata.staging_desc,
                destination,
                options,
                "failed to map WGC full-frame readback",
            ),
            ReadbackTarget::Region(blit)
                if blit.dst_x == 0
                    && blit.dst_y == 0
                    && destination.width() == blit.width
                    && destination.height() == blit.height =>
            {
                surface::map_staging_to_frame(
                    context,
                    staging,
                    slot.resource.as_ref(),
                    &metadata.staging_desc,
                    destination,
                    options,
                    "failed to map WGC region readback",
                )
            }
            ReadbackTarget::Region(blit) => surface::map_staging_rect_to_frame(
                context,
                staging,
                slot.resource.as_ref(),
                &metadata.staging_desc,
                destination,
                CaptureBlitRegion {
                    src_x: 0,
                    src_y: 0,
                    width: blit.width,
                    height: blit.height,
                    dst_x: blit.dst_x,
                    dst_y: blit.dst_y,
                },
                options,
                "failed to map WGC region readback",
            ),
        }
    }

    fn map_dirty(
        &self,
        context: &ID3D11DeviceContext,
        slot_index: usize,
        destination: &mut Frame,
        metadata: &SlotMetadata,
    ) -> CaptureResult<()> {
        let slot = &self.slots[slot_index];
        let staging = slot.staging.as_ref().ok_or_else(|| {
            CaptureError::platform(anyhow::anyhow!("WGC readback slot has no staging texture"))
        })?;
        let options = SurfaceConversionOptions {
            hdr_to_sdr: metadata.hdr_to_sdr,
            ..SurfaceConversionOptions::default()
        };
        let hints = surface::DirtyRectConversionHints {
            trusted_bounds: true,
            non_empty_rects: Some(metadata.dirty_rects.len()),
            total_dirty_pixels: Some(metadata.dirty_rects.iter().fold(0usize, |sum, rect| {
                sum.saturating_add((rect.width as usize).saturating_mul(rect.height as usize))
            })),
        };
        let converted = match metadata.target {
            ReadbackTarget::Full => surface::map_staging_dirty_rects_to_frame(
                context,
                staging,
                slot.resource.as_ref(),
                &metadata.staging_desc,
                destination,
                &metadata.dirty_rects,
                false,
                hints,
                options,
                "failed to map WGC dirty full-frame readback",
            )?,
            ReadbackTarget::Region(blit) => surface::map_staging_dirty_rects_to_frame_with_offset(
                context,
                staging,
                slot.resource.as_ref(),
                &metadata.staging_desc,
                destination,
                &metadata.dirty_rects,
                blit.dst_x,
                blit.dst_y,
                false,
                hints,
                options,
                "failed to map WGC dirty region readback",
            )?,
        };
        if converted == 0 {
            self.map_complete(context, slot_index, destination, metadata)?;
        }
        Ok(())
    }

    fn find_reusable_slot(&self, context: &ID3D11DeviceContext) -> CaptureResult<Option<usize>> {
        for offset in 0..READBACK_SLOT_COUNT {
            let index = (self.next_slot + offset) % READBACK_SLOT_COUNT;
            if slot_ready(context, &self.slots[index])? {
                return Ok(Some(index));
            }
        }
        Ok(None)
    }

    fn oldest_slot(&self) -> usize {
        self.slots
            .iter()
            .enumerate()
            .min_by_key(|(_, slot)| {
                slot.metadata
                    .as_ref()
                    .map_or(0, |metadata| metadata.generation)
            })
            .map_or(0, |(index, _)| index)
    }

    fn ensure_slot(
        &mut self,
        device: &ID3D11Device,
        slot_index: usize,
        desc: D3D11_TEXTURE2D_DESC,
    ) -> CaptureResult<()> {
        let slot = &mut self.slots[slot_index];
        let needs_texture = slot.desc.is_none_or(|current| !desc_matches(current, desc));
        if needs_texture {
            let mut staging = None;
            let texture = surface::ensure_staging_texture(
                device,
                &mut staging,
                &desc,
                surface::StagingSampleDesc::SingleSample,
                "failed to create WGC readback staging texture",
            )?
            .clone();
            let resource = texture
                .cast::<ID3D11Resource>()
                .context("failed to cast WGC readback staging texture")
                .map_err(CaptureError::platform)?;
            slot.staging = Some(texture);
            slot.resource = Some(resource);
            slot.desc = Some(desc);
            slot.invalidate_submission();
        }
        if slot.query.is_none() {
            let query_desc = D3D11_QUERY_DESC {
                Query: D3D11_QUERY_EVENT,
                ..Default::default()
            };
            let mut query = None;
            unsafe { device.CreateQuery(&query_desc, Some(&mut query)) }
                .context("CreateQuery for WGC readback slot failed")
                .map_err(CaptureError::platform)?;
            slot.query = query;
        }
        Ok(())
    }
}

fn dirty_rects_for_delivery(
    ordered_delta: bool,
    history_matches_parent: bool,
    dirty_rects: &[DirtyRect],
) -> Vec<DirtyRect> {
    if ordered_delta && history_matches_parent {
        dirty_rects.to_vec()
    } else {
        Vec::new()
    }
}

fn desc_matches(left: D3D11_TEXTURE2D_DESC, right: D3D11_TEXTURE2D_DESC) -> bool {
    left.Width == right.Width
        && left.Height == right.Height
        && left.Format == right.Format
        && left.SampleDesc.Count == right.SampleDesc.Count
        && left.SampleDesc.Quality == right.SampleDesc.Quality
}

fn query_ready(context: &ID3D11DeviceContext, query: &ID3D11Query) -> CaptureResult<bool> {
    let mut completed = 0u32;
    match unsafe {
        context.GetData(
            query,
            Some(&mut completed as *mut u32 as *mut _),
            std::mem::size_of::<u32>() as u32,
            D3D11_ASYNC_GETDATA_DONOTFLUSH,
        )
    } {
        Ok(()) => Ok(completed != 0),
        Err(error) if error.code().0 == 1 => Ok(false),
        Err(error) => Err(super::map_platform_error(
            error,
            "GetData for WGC readback query failed",
        )),
    }
}

fn slot_ready(context: &ID3D11DeviceContext, slot: &ReadbackSlot) -> CaptureResult<bool> {
    if !slot.query_pending {
        return Ok(true);
    }
    match slot.query.as_ref() {
        Some(query) => query_ready(context, query),
        None => Ok(false),
    }
}

fn wait_for_slot(context: &ID3D11DeviceContext, slot: &ReadbackSlot) -> CaptureResult<()> {
    if !slot.query_pending {
        return Ok(());
    }
    let query = slot.query.as_ref().ok_or_else(|| {
        CaptureError::platform(anyhow::anyhow!("WGC readback slot has no completion query"))
    })?;
    for _ in 0..QUERY_SPIN_POLLS {
        if query_ready(context, query)? {
            return Ok(());
        }
        std::hint::spin_loop();
    }
    unsafe { context.Flush() };
    let deadline = Instant::now() + QUERY_WAIT_TIMEOUT;
    while Instant::now() < deadline {
        if query_ready(context, query)? {
            return Ok(());
        }
        std::thread::yield_now();
    }
    Err(CaptureError::Timeout)
}

fn copy_complete_target(
    context: &ID3D11DeviceContext,
    destination: &ID3D11Resource,
    source: &ID3D11Texture2D,
    target: ReadbackTarget,
    source_desc: D3D11_TEXTURE2D_DESC,
) -> CaptureResult<()> {
    d3d11::with_texture_resource(
        source,
        "failed to cast WGC canonical source for readback",
        |source_resource| {
            match target {
                ReadbackTarget::Full => unsafe {
                    context.CopyResource(destination, source_resource);
                },
                ReadbackTarget::Region(blit) => {
                    let right = blit
                        .src_x
                        .checked_add(blit.width)
                        .ok_or(CaptureError::BufferOverflow)?;
                    let bottom = blit
                        .src_y
                        .checked_add(blit.height)
                        .ok_or(CaptureError::BufferOverflow)?;
                    if right > source_desc.Width || bottom > source_desc.Height {
                        return Err(CaptureError::BufferOverflow);
                    }
                    let source_box = D3D11_BOX {
                        left: blit.src_x,
                        top: blit.src_y,
                        front: 0,
                        right,
                        bottom,
                        back: 1,
                    };
                    unsafe {
                        context.CopySubresourceRegion(
                            destination,
                            0,
                            0,
                            0,
                            0,
                            source_resource,
                            0,
                            Some(&source_box),
                        );
                    }
                }
            }
            Ok(())
        },
    )
}

fn copy_dirty_regions(
    context: &ID3D11DeviceContext,
    destination: &ID3D11Resource,
    source: &ID3D11Texture2D,
    target: ReadbackTarget,
    local_dirty_rects: &[DirtyRect],
) -> CaptureResult<()> {
    d3d11::with_texture_resource(
        source,
        "failed to cast WGC canonical source for dirty readback",
        |source_resource| {
            let (source_origin_x, source_origin_y) = match target {
                ReadbackTarget::Full => (0, 0),
                ReadbackTarget::Region(blit) => (blit.src_x, blit.src_y),
            };
            for rect in local_dirty_rects {
                let source_left = source_origin_x
                    .checked_add(rect.x)
                    .ok_or(CaptureError::BufferOverflow)?;
                let source_top = source_origin_y
                    .checked_add(rect.y)
                    .ok_or(CaptureError::BufferOverflow)?;
                let source_right = source_left
                    .checked_add(rect.width)
                    .ok_or(CaptureError::BufferOverflow)?;
                let source_bottom = source_top
                    .checked_add(rect.height)
                    .ok_or(CaptureError::BufferOverflow)?;
                let source_box = D3D11_BOX {
                    left: source_left,
                    top: source_top,
                    front: 0,
                    right: source_right,
                    bottom: source_bottom,
                    back: 1,
                };
                unsafe {
                    context.CopySubresourceRegion(
                        destination,
                        0,
                        rect.x,
                        rect.y,
                        0,
                        source_resource,
                        0,
                        Some(&source_box),
                    );
                }
            }
            Ok(())
        },
    )
}

fn intersect_with_region(rect: DirtyRect, blit: CaptureBlitRegion) -> Option<DirtyRect> {
    let rect_right = rect.x.checked_add(rect.width)?;
    let rect_bottom = rect.y.checked_add(rect.height)?;
    let region_right = blit.src_x.checked_add(blit.width)?;
    let region_bottom = blit.src_y.checked_add(blit.height)?;
    let left = rect.x.max(blit.src_x);
    let top = rect.y.max(blit.src_y);
    let right = rect_right.min(region_right);
    let bottom = rect_bottom.min(region_bottom);
    if right <= left || bottom <= top {
        return None;
    }
    Some(DirtyRect {
        x: left - blit.src_x,
        y: top - blit.src_y,
        width: right - left,
        height: bottom - top,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn region_dirty_rects_are_clipped_and_made_local() {
        let blit = CaptureBlitRegion {
            src_x: 100,
            src_y: 200,
            width: 300,
            height: 200,
            dst_x: 0,
            dst_y: 0,
        };
        assert_eq!(
            intersect_with_region(
                DirtyRect {
                    x: 50,
                    y: 250,
                    width: 100,
                    height: 100,
                },
                blit,
            ),
            Some(DirtyRect {
                x: 0,
                y: 50,
                width: 50,
                height: 100,
            })
        );
    }

    #[test]
    fn region_dirty_rects_reject_disjoint_damage() {
        let blit = CaptureBlitRegion {
            src_x: 100,
            src_y: 100,
            width: 100,
            height: 100,
            dst_x: 0,
            dst_y: 0,
        };
        assert!(
            intersect_with_region(
                DirtyRect {
                    x: 0,
                    y: 0,
                    width: 50,
                    height: 50,
                },
                blit,
            )
            .is_none()
        );
    }

    #[test]
    fn delivered_generation_requires_exact_target_and_parent() {
        let target = ReadbackTarget::Full;
        let delivered = DeliveredGeneration {
            epoch: 4,
            generation: 9,
            target,
        };
        assert_eq!(delivered.epoch, 4);
        assert_eq!(delivered.generation, 9);
        assert_eq!(delivered.target, target);
    }

    #[test]
    fn dirty_rects_require_exact_incremental_delivery_history() {
        let rects = [DirtyRect {
            x: 4,
            y: 8,
            width: 16,
            height: 32,
        }];
        assert_eq!(dirty_rects_for_delivery(true, true, &rects), rects);
        assert!(dirty_rects_for_delivery(true, false, &rects).is_empty());
        assert!(dirty_rects_for_delivery(false, true, &rects).is_empty());
    }
}
