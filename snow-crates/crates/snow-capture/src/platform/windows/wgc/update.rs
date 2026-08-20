use std::time::Instant;

use anyhow::Context;
use windows::Graphics::Capture::{Direct3D11CaptureFrame, GraphicsCaptureDirtyRegionMode};
use windows::Graphics::RectInt32;
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_SHADER_RESOURCE, D3D11_BOX, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, ID3D11Device,
    ID3D11DeviceContext, ID3D11Resource, ID3D11Texture2D,
};
use windows::core::Interface;

use crate::error::{CaptureError, CaptureResult};
use crate::frame::DirtyRect;

use super::super::d3d11;

const DIRTY_REGION_BATCH: usize = 64;
const MAX_ORDERED_DIRTY_REGIONS: u32 = 65_536;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SurfaceIdentity {
    width: u32,
    height: u32,
    format: i32,
    sample_count: u32,
    sample_quality: u32,
}

impl SurfaceIdentity {
    fn from_desc(desc: &D3D11_TEXTURE2D_DESC) -> Self {
        Self {
            width: desc.Width,
            height: desc.Height,
            format: desc.Format.0,
            sample_count: desc.SampleDesc.Count,
            sample_quality: desc.SampleDesc.Quality,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FrameContract {
    Complete,
    OrderedDelta,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Transition {
    Duplicate,
    ApplyComplete { new_epoch: bool },
    ApplyDelta,
    Resynchronize,
}

enum DirtyRegionFailure {
    Invalid,
    DeviceLost,
}

#[derive(Clone, Copy, Debug, Default)]
struct ContinuityState {
    identity: Option<SurfaceIdentity>,
    baseline_valid: bool,
    contract: Option<FrameContract>,
    last_timestamp_hns: i64,
}

impl ContinuityState {
    fn classify(
        &self,
        contract: FrameContract,
        identity: SurfaceIdentity,
        timestamp_hns: i64,
        dirty_region_count: usize,
    ) -> Transition {
        let identity_changed = self.identity.is_some_and(|current| current != identity);
        match contract {
            FrameContract::Complete => Transition::ApplyComplete {
                new_epoch: identity_changed
                    || !self.baseline_valid
                    || self.contract == Some(FrameContract::OrderedDelta),
            },
            FrameContract::OrderedDelta => {
                let timestamp_regressed = timestamp_hns != 0
                    && self.last_timestamp_hns != 0
                    && timestamp_hns < self.last_timestamp_hns;
                if timestamp_regressed {
                    return Transition::Resynchronize;
                }
                if !self.baseline_valid
                    || identity_changed
                    || self.contract != Some(FrameContract::OrderedDelta)
                {
                    return Transition::Resynchronize;
                }
                if dirty_region_count == 0 {
                    if timestamp_hns != 0 && timestamp_hns == self.last_timestamp_hns {
                        Transition::Duplicate
                    } else {
                        Transition::Resynchronize
                    }
                } else {
                    Transition::ApplyDelta
                }
            }
        }
    }

    fn establish_complete(
        &mut self,
        identity: SurfaceIdentity,
        timestamp_hns: i64,
        ordered_next: bool,
    ) {
        self.identity = Some(identity);
        self.baseline_valid = true;
        self.contract = Some(if ordered_next {
            FrameContract::OrderedDelta
        } else {
            FrameContract::Complete
        });
        self.last_timestamp_hns = timestamp_hns;
    }

    fn advance_delta(&mut self, timestamp_hns: i64) {
        self.last_timestamp_hns = timestamp_hns;
    }

    fn invalidate(&mut self) {
        self.baseline_valid = false;
        self.contract = None;
        self.last_timestamp_hns = 0;
    }
}

#[derive(Clone, Debug)]
pub(super) struct CanonicalFrameMetadata {
    pub epoch: u64,
    pub generation: u64,
    pub parent_generation: u64,
    pub capture_time: Instant,
    pub system_relative_time_hns: i64,
    pub dirty_rects: Vec<DirtyRect>,
    pub ordered_delta: bool,
}

pub(super) enum ApplyOutcome {
    Updated(CanonicalFrameMetadata),
    Duplicate,
    Resynchronize,
}

pub(super) struct CanonicalSurface {
    texture: Option<ID3D11Texture2D>,
    resource: Option<ID3D11Resource>,
    desc: Option<D3D11_TEXTURE2D_DESC>,
    continuity: ContinuityState,
    epoch: u64,
    generation: u64,
    latest: Option<CanonicalFrameMetadata>,
}

impl CanonicalSurface {
    pub fn new() -> Self {
        Self {
            texture: None,
            resource: None,
            desc: None,
            continuity: ContinuityState::default(),
            epoch: 1,
            generation: 0,
            latest: None,
        }
    }

    pub fn texture(&self) -> Option<&ID3D11Texture2D> {
        self.texture
            .as_ref()
            .filter(|_| self.continuity.baseline_valid)
    }

    pub fn desc(&self) -> Option<D3D11_TEXTURE2D_DESC> {
        self.desc.filter(|_| self.continuity.baseline_valid)
    }

    pub fn latest(&self) -> Option<&CanonicalFrameMetadata> {
        self.latest
            .as_ref()
            .filter(|_| self.continuity.baseline_valid)
    }

    pub fn has_baseline(&self) -> bool {
        self.continuity.baseline_valid
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn invalidate(&mut self) {
        self.epoch = self.epoch.wrapping_add(1).max(1);
        self.continuity.invalidate();
        self.latest = None;
    }

    pub fn apply(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        frame: &Direct3D11CaptureFrame,
        source: &ID3D11Texture2D,
        source_desc: D3D11_TEXTURE2D_DESC,
        reported_mode: GraphicsCaptureDirtyRegionMode,
        system_relative_time_hns: i64,
        capture_time: Instant,
        ordered_next: bool,
        retain_complete_texture: bool,
    ) -> CaptureResult<ApplyOutcome> {
        if source_desc.SampleDesc.Count != 1 {
            return Err(CaptureError::UnsupportedFormat(format!(
                "WGC multisampled surface with {} samples",
                source_desc.SampleDesc.Count
            )));
        }

        let identity = SurfaceIdentity::from_desc(&source_desc);
        let contract = if reported_mode == GraphicsCaptureDirtyRegionMode::ReportOnly {
            FrameContract::Complete
        } else if reported_mode == GraphicsCaptureDirtyRegionMode::ReportAndRender {
            FrameContract::OrderedDelta
        } else {
            return Ok(ApplyOutcome::Resynchronize);
        };

        let dirty_rects = if contract == FrameContract::OrderedDelta {
            match extract_exact_dirty_rects(frame, source_desc.Width, source_desc.Height) {
                Ok(rects) => rects,
                Err(DirtyRegionFailure::Invalid) => return Ok(ApplyOutcome::Resynchronize),
                Err(DirtyRegionFailure::DeviceLost) => return Err(CaptureError::AccessLost),
            }
        } else {
            Vec::new()
        };

        match self.continuity.classify(
            contract,
            identity,
            system_relative_time_hns,
            dirty_rects.len(),
        ) {
            Transition::Duplicate => Ok(ApplyOutcome::Duplicate),
            Transition::Resynchronize => Ok(ApplyOutcome::Resynchronize),
            Transition::ApplyComplete { new_epoch } => {
                if new_epoch {
                    self.epoch = self.epoch.wrapping_add(1).max(1);
                }
                if retain_complete_texture {
                    self.ensure_texture(device, source_desc)?;
                    let destination = self.resource.as_ref().ok_or_else(|| {
                        CaptureError::platform(anyhow::anyhow!(
                            "WGC canonical texture is missing its resource interface"
                        ))
                    })?;
                    d3d11::with_texture_resource(
                        source,
                        "failed to cast WGC complete surface to ID3D11Resource",
                        |source_resource| {
                            unsafe { context.CopyResource(destination, source_resource) };
                            Ok(())
                        },
                    )?;
                } else {
                    self.texture = None;
                    self.resource = None;
                    self.desc = None;
                }
                self.continuity.establish_complete(
                    identity,
                    system_relative_time_hns,
                    ordered_next,
                );
                self.publish(capture_time, system_relative_time_hns, Vec::new(), false)
            }
            Transition::ApplyDelta => {
                let destination = self.resource.as_ref().ok_or_else(|| {
                    CaptureError::platform(anyhow::anyhow!(
                        "WGC ordered delta arrived without a canonical resource"
                    ))
                })?;
                d3d11::with_texture_resource(
                    source,
                    "failed to cast WGC delta surface to ID3D11Resource",
                    |source_resource| {
                        for rect in &dirty_rects {
                            let right = rect
                                .x
                                .checked_add(rect.width)
                                .ok_or(CaptureError::BufferOverflow)?;
                            let bottom = rect
                                .y
                                .checked_add(rect.height)
                                .ok_or(CaptureError::BufferOverflow)?;
                            let source_box = D3D11_BOX {
                                left: rect.x,
                                top: rect.y,
                                front: 0,
                                right,
                                bottom,
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
                )?;
                self.continuity.advance_delta(system_relative_time_hns);
                self.publish(capture_time, system_relative_time_hns, dirty_rects, true)
            }
        }
    }

    fn ensure_texture(
        &mut self,
        device: &ID3D11Device,
        source_desc: D3D11_TEXTURE2D_DESC,
    ) -> CaptureResult<()> {
        let needs_recreate = self.desc.is_none_or(|current| {
            SurfaceIdentity::from_desc(&current) != SurfaceIdentity::from_desc(&source_desc)
        });
        if !needs_recreate {
            return Ok(());
        }

        let desc = D3D11_TEXTURE2D_DESC {
            Width: source_desc.Width,
            Height: source_desc.Height,
            MipLevels: 1,
            ArraySize: 1,
            Format: source_desc.Format,
            SampleDesc: source_desc.SampleDesc,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            CPUAccessFlags: 0,
            MiscFlags: 0,
        };
        let mut texture = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture)) }
            .context("CreateTexture2D for WGC canonical surface failed")
            .map_err(CaptureError::platform)?;
        let texture = texture.ok_or_else(|| {
            CaptureError::platform(anyhow::anyhow!(
                "CreateTexture2D for WGC canonical surface returned no texture"
            ))
        })?;
        let resource = texture
            .cast::<ID3D11Resource>()
            .context("failed to cast WGC canonical texture to ID3D11Resource")
            .map_err(CaptureError::platform)?;
        self.texture = Some(texture);
        self.resource = Some(resource);
        self.desc = Some(desc);
        Ok(())
    }

    fn publish(
        &mut self,
        capture_time: Instant,
        system_relative_time_hns: i64,
        dirty_rects: Vec<DirtyRect>,
        ordered_delta: bool,
    ) -> CaptureResult<ApplyOutcome> {
        let parent_generation = self.generation;
        self.generation = self.generation.wrapping_add(1).max(1);
        let metadata = CanonicalFrameMetadata {
            epoch: self.epoch,
            generation: self.generation,
            parent_generation,
            capture_time,
            system_relative_time_hns,
            dirty_rects,
            ordered_delta,
        };
        self.latest = Some(metadata.clone());
        Ok(ApplyOutcome::Updated(metadata))
    }
}

fn exact_rect(raw: RectInt32, width: u32, height: u32) -> Result<DirtyRect, ()> {
    if raw.X < 0 || raw.Y < 0 || raw.Width <= 0 || raw.Height <= 0 {
        return Err(());
    }
    let x = u32::try_from(raw.X).map_err(|_| ())?;
    let y = u32::try_from(raw.Y).map_err(|_| ())?;
    let rect_width = u32::try_from(raw.Width).map_err(|_| ())?;
    let rect_height = u32::try_from(raw.Height).map_err(|_| ())?;
    let right = x.checked_add(rect_width).ok_or(())?;
    let bottom = y.checked_add(rect_height).ok_or(())?;
    if right > width || bottom > height {
        return Err(());
    }
    Ok(DirtyRect {
        x,
        y,
        width: rect_width,
        height: rect_height,
    })
}

fn extract_exact_dirty_rects(
    frame: &Direct3D11CaptureFrame,
    width: u32,
    height: u32,
) -> Result<Vec<DirtyRect>, DirtyRegionFailure> {
    let regions = frame.DirtyRegions().map_err(classify_dirty_region_error)?;
    let count = regions.Size().map_err(classify_dirty_region_error)?;
    if count > MAX_ORDERED_DIRTY_REGIONS {
        return Err(DirtyRegionFailure::Invalid);
    }

    let mut out =
        Vec::with_capacity(usize::try_from(count).map_err(|_| DirtyRegionFailure::Invalid)?);
    let mut start = 0u32;
    let mut batch = [RectInt32::default(); DIRTY_REGION_BATCH];
    while start < count {
        let remaining = usize::try_from(count - start).map_err(|_| DirtyRegionFailure::Invalid)?;
        let requested = remaining.min(batch.len());
        let fetched = regions
            .GetMany(start, &mut batch[..requested])
            .map_err(classify_dirty_region_error)?;
        let fetched = usize::try_from(fetched)
            .map_err(|_| DirtyRegionFailure::Invalid)?
            .min(requested);
        if fetched == 0 {
            return Err(DirtyRegionFailure::Invalid);
        }
        for raw in &batch[..fetched] {
            out.push(exact_rect(*raw, width, height).map_err(|_| DirtyRegionFailure::Invalid)?);
        }
        start = start
            .checked_add(u32::try_from(fetched).map_err(|_| DirtyRegionFailure::Invalid)?)
            .ok_or(DirtyRegionFailure::Invalid)?;
    }
    Ok(out)
}

fn classify_dirty_region_error(error: windows::core::Error) -> DirtyRegionFailure {
    if super::is_device_lost_hresult(error.code()) {
        DirtyRegionFailure::DeviceLost
    } else {
        DirtyRegionFailure::Invalid
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn patterned_frame(width: usize, height: usize) -> Vec<u32> {
        (0..width * height)
            .map(|index| {
                let x = index % width;
                let y = index / width;
                ((y as u32) << 16) | x as u32
            })
            .collect()
    }

    fn apply_delta(
        accumulator: &mut [u32],
        delta_surface: &[u32],
        width: usize,
        rects: &[DirtyRect],
    ) {
        for rect in rects {
            for y in rect.y as usize..(rect.y + rect.height) as usize {
                let start = y * width + rect.x as usize;
                let end = start + rect.width as usize;
                accumulator[start..end].copy_from_slice(&delta_surface[start..end]);
            }
        }
    }

    fn vertical_scroll(source: &[u32], width: usize, height: usize, rows: usize) -> Vec<u32> {
        let mut output = vec![0; source.len()];
        for y in 0..height - rows {
            let source_start = (y + rows) * width;
            let destination_start = y * width;
            output[destination_start..destination_start + width]
                .copy_from_slice(&source[source_start..source_start + width]);
        }
        for y in height - rows..height {
            for x in 0..width {
                output[y * width + x] = 0xff00_0000 | ((y as u32) << 8) | x as u32;
            }
        }
        output
    }

    fn horizontal_scroll(source: &[u32], width: usize, height: usize, columns: usize) -> Vec<u32> {
        let mut output = vec![0; source.len()];
        for y in 0..height {
            let source_start = y * width + columns;
            let destination_start = y * width;
            output[destination_start..destination_start + width - columns]
                .copy_from_slice(&source[source_start..source_start + width - columns]);
            for x in width - columns..width {
                output[y * width + x] = 0xee00_0000 | ((y as u32) << 8) | x as u32;
            }
        }
        output
    }

    fn identity(width: u32, height: u32) -> SurfaceIdentity {
        SurfaceIdentity {
            width,
            height,
            format: 87,
            sample_count: 1,
            sample_quality: 0,
        }
    }

    #[test]
    fn ordered_delta_requires_complete_baseline() {
        let state = ContinuityState::default();
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(1920, 1080), 100, 1),
            Transition::Resynchronize
        );
    }

    #[test]
    fn complete_baseline_can_transition_to_ordered_deltas() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 100, true);
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(1920, 1080), 110, 2),
            Transition::ApplyDelta
        );
    }

    #[test]
    fn timestamp_regression_forces_resynchronization() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 200, true);
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(1920, 1080), 199, 1),
            Transition::Resynchronize
        );
    }

    #[test]
    fn complete_surface_is_not_skipped_when_timestamp_is_reused() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 200, false);
        assert_eq!(
            state.classify(FrameContract::Complete, identity(1920, 1080), 200, 0),
            Transition::ApplyComplete { new_epoch: false }
        );
    }

    #[test]
    fn complete_surface_does_not_depend_on_timestamp_monotonicity() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 200, false);
        assert_eq!(
            state.classify(FrameContract::Complete, identity(1920, 1080), 199, 0),
            Transition::ApplyComplete { new_epoch: false }
        );
    }

    #[test]
    fn resize_forces_ordered_resynchronization() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 100, true);
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(2560, 1440), 110, 1),
            Transition::Resynchronize
        );
    }

    #[test]
    fn empty_new_delta_is_not_assumed_unchanged() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 100, true);
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(1920, 1080), 110, 0),
            Transition::Resynchronize
        );
    }

    #[test]
    fn exact_timestamp_empty_delta_is_duplicate() {
        let mut state = ContinuityState::default();
        state.establish_complete(identity(1920, 1080), 100, true);
        assert_eq!(
            state.classify(FrameContract::OrderedDelta, identity(1920, 1080), 100, 0),
            Transition::Duplicate
        );
    }

    #[test]
    fn exact_rect_rejects_out_of_bounds_metadata() {
        assert!(
            exact_rect(
                RectInt32 {
                    X: 0,
                    Y: 0,
                    Width: 10,
                    Height: 10
                },
                10,
                10
            )
            .is_ok()
        );
        assert!(
            exact_rect(
                RectInt32 {
                    X: -1,
                    Y: 0,
                    Width: 10,
                    Height: 10
                },
                10,
                10
            )
            .is_err()
        );
        assert!(
            exact_rect(
                RectInt32 {
                    X: 1,
                    Y: 0,
                    Width: 10,
                    Height: 10
                },
                10,
                10
            )
            .is_err()
        );
    }

    #[test]
    fn full_destination_damage_reconstructs_vertical_scroll() {
        let width = 23;
        let height = 17;
        let baseline = patterned_frame(width, height);
        let expected = vertical_scroll(&baseline, width, height, 6);
        let mut accumulator = baseline;
        apply_delta(
            &mut accumulator,
            &expected,
            width,
            &[DirtyRect {
                x: 0,
                y: 0,
                width: width as u32,
                height: height as u32,
            }],
        );
        assert_eq!(accumulator, expected);
    }

    #[test]
    fn full_destination_damage_reconstructs_horizontal_scroll() {
        let width = 29;
        let height = 13;
        let baseline = patterned_frame(width, height);
        let expected = horizontal_scroll(&baseline, width, height, 7);
        let mut accumulator = baseline;
        apply_delta(
            &mut accumulator,
            &expected,
            width,
            &[DirtyRect {
                x: 0,
                y: 0,
                width: width as u32,
                height: height as u32,
            }],
        );
        assert_eq!(accumulator, expected);
    }

    #[test]
    fn bottom_strip_alone_cannot_reconstruct_a_scroll() {
        let width = 23;
        let height = 17;
        let rows = 6;
        let baseline = patterned_frame(width, height);
        let expected = vertical_scroll(&baseline, width, height, rows);
        let mut malformed = baseline;
        apply_delta(
            &mut malformed,
            &expected,
            width,
            &[DirtyRect {
                x: 0,
                y: (height - rows) as u32,
                width: width as u32,
                height: rows as u32,
            }],
        );
        assert_ne!(malformed, expected);
        assert_eq!(
            &malformed[..(height - rows) * width],
            &patterned_frame(width, height)[..(height - rows) * width]
        );
    }
}
