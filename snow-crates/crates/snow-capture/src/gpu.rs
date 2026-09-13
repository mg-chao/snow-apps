//! Opt-in GPU recording capture. CPU `Frame` and screenshot APIs remain byte-backed.
use std::sync::Arc;
use std::time::Instant;

use snow_cursor::{
    CursorProjector, CursorSampler, CursorShape, CursorShapeState, CursorTargetInfo,
};
use snow_d3d11::{AdapterIdentity, Rect, SharedDevice, Texture, VideoLayer};
use windows::core::Interface;

use crate::CaptureRegion;
use crate::backend::CaptureBackendKind;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::FrameMetadata;
use crate::platform::windows::{duplication, monitor, wgc};

mod stream;
pub use stream::{GpuCaptureEvent, GpuCaptureStream};

#[derive(Clone)]
pub struct GpuCapturedFrame {
    pub layers: Vec<VideoLayer>,
    dimensions: (u32, u32),
    metadata: Arc<FrameMetadata>,
}

impl GpuCapturedFrame {
    /// Build an observation from owned immutable layers without a host pixel buffer.
    pub fn from_layers(
        layers: Vec<VideoLayer>,
        dimensions: (u32, u32),
        metadata: FrameMetadata,
    ) -> CaptureResult<Self> {
        if dimensions.0 == 0 || dimensions.1 == 0 {
            return Err(CaptureError::InvalidConfig("empty GPU observation".into()));
        }
        if let Some(first) = layers.first()
            && layers
                .iter()
                .any(|layer| !first.texture.device().same_device(layer.texture.device()))
        {
            return Err(CaptureError::InvalidConfig(
                "GPU observation spans devices".into(),
            ));
        }
        Ok(Self {
            layers,
            dimensions,
            metadata: Arc::new(metadata),
        })
    }
    pub fn dimensions(&self) -> (u32, u32) {
        self.dimensions
    }
    pub fn metadata(&self) -> &FrameMetadata {
        &self.metadata
    }
}

#[derive(Clone)]
pub(crate) struct GpuMonitorFrame {
    pub texture: Texture,
    pub rotation: u32,
    pub metadata: FrameMetadata,
}

enum Source {
    Dxgi(Box<duplication::GpuDxgiCapturer>),
    Wgc(Box<wgc::GpuWgcCapturer>),
}

impl Source {
    fn capture(&mut self) -> CaptureResult<GpuMonitorFrame> {
        match self {
            Self::Dxgi(source) => source.capture(),
            Self::Wgc(source) => source.capture(),
        }
    }
}

struct LayerSource {
    source: Source,
    desktop: Rect,
    intersection: Rect,
    delivered: Option<u64>,
}

/// Construct and use on the capture thread; WGC apartment lifetime stays there.
pub struct GpuCaptureSession {
    device: SharedDevice,
    sources: Vec<LayerSource>,
    region: CaptureRegion,
    sequence: u64,
    generation: u64,
    cursor: CursorSampler,
    projector: CursorProjector,
    cursor_shape: Option<CursorShape>,
}

impl GpuCaptureSession {
    fn pool_pressure(&self) -> u64 {
        self.sources
            .iter()
            .map(|source| match &source.source {
                Source::Dxgi(source) => source.pool_pressure(),
                Source::Wgc(source) => source.pool_pressure(),
            })
            .sum()
    }
    pub fn open(region: CaptureRegion, backend: CaptureBackendKind) -> CaptureResult<Self> {
        let monitors = monitor::enumerate_resolved()?;
        let mut overlapping = Vec::new();
        for monitor in monitors {
            let desc = unsafe { monitor.output.GetDesc() }.map_err(CaptureError::platform)?;
            let rect = desc.DesktopCoordinates;
            let desktop = Rect {
                x: rect.left,
                y: rect.top,
                width: (rect.right - rect.left) as u32,
                height: (rect.bottom - rect.top) as u32,
            };
            if let Some(intersection) = intersect(region, desktop)? {
                overlapping.push((monitor, desktop, intersection));
            }
        }
        let first = overlapping.first().ok_or_else(|| {
            CaptureError::InvalidTarget("GPU recording region does not intersect a display".into())
        })?;
        let identity =
            AdapterIdentity::inspect(&first.0.adapter).map_err(CaptureError::platform)?;
        for (monitor, _, _) in &overlapping {
            let other =
                AdapterIdentity::inspect(&monitor.adapter).map_err(CaptureError::platform)?;
            if other.luid != identity.luid {
                return Err(CaptureError::BackendUnavailable(
                    "GPU recording region spans different adapters".into(),
                ));
            }
        }
        let device = SharedDevice::create(&first.0.adapter).map_err(CaptureError::platform)?;
        let mut sources = Vec::new();
        for (monitor, desktop, intersection) in overlapping {
            let source = match backend {
                CaptureBackendKind::DxgiDuplication => Source::Dxgi(Box::new(
                    duplication::GpuDxgiCapturer::new(&monitor, device.clone())?,
                )),
                CaptureBackendKind::WindowsGraphicsCapture => Source::Wgc(Box::new(
                    wgc::GpuWgcCapturer::new(&monitor, device.clone())?,
                )),
                CaptureBackendKind::Auto => {
                    match duplication::GpuDxgiCapturer::new(&monitor, device.clone()) {
                        Ok(source) => Source::Dxgi(Box::new(source)),
                        Err(_) => Source::Wgc(Box::new(wgc::GpuWgcCapturer::new(
                            &monitor,
                            device.clone(),
                        )?)),
                    }
                }
                _ => {
                    return Err(CaptureError::BackendUnavailable(
                        "selected backend has no GPU frame delivery".into(),
                    ));
                }
            };
            sources.push(LayerSource {
                source,
                desktop,
                intersection,
                delivered: None,
            });
        }
        Ok(Self {
            device,
            sources,
            region,
            sequence: 0,
            generation: 0,
            cursor: CursorSampler::new()
                .map_err(|error| CaptureError::BackendUnavailable(error.to_string()))?,
            projector: CursorProjector::new(),
            cursor_shape: None,
        })
    }

    pub fn device(&self) -> &SharedDevice {
        &self.device
    }

    pub fn capture(&mut self) -> CaptureResult<GpuCapturedFrame> {
        let started = Instant::now();
        let mut layers = Vec::with_capacity(self.sources.len());
        let mut generations = Vec::with_capacity(self.sources.len());
        let mut metadata = FrameMetadata {
            is_duplicate: self.sequence != 0,
            observation_started_at: Some(started),
            ..Default::default()
        };
        for source in &mut self.sources {
            let frame = source.source.capture()?;
            let crop = Rect {
                x: source.intersection.x - source.desktop.x,
                y: source.intersection.y - source.desktop.y,
                width: source.intersection.width,
                height: source.intersection.height,
            };
            let native = native_crop(crop, frame.texture.dimensions(), frame.rotation)?;
            layers.push(VideoLayer {
                texture: frame.texture,
                source: native,
                destination: Rect {
                    x: source.intersection.x - self.region.x,
                    y: source.intersection.y - self.region.y,
                    width: source.intersection.width,
                    height: source.intersection.height,
                },
                rotation: frame.rotation,
                alpha: false,
            });
            let generation = frame.metadata.content_generation.unwrap_or(0);
            metadata.is_duplicate &= source.delivered == Some(generation);
            generations.push(generation);
            metadata.backend_kind = frame.metadata.backend_kind;
            if metadata.stream_timestamp.as_ref().is_none_or(|old| {
                frame
                    .metadata
                    .stream_timestamp
                    .as_ref()
                    .is_some_and(|new| new.instant > old.instant)
            }) {
                metadata.stream_timestamp = frame.metadata.stream_timestamp;
            }
        }
        self.sequence = self.sequence.wrapping_add(1);
        for (source, generation) in self.sources.iter_mut().zip(generations) {
            source.delivered = Some(generation);
        }
        if !metadata.is_duplicate {
            self.generation = self.generation.wrapping_add(1);
        }
        metadata.sequence = self.sequence;
        metadata.content_generation = Some(self.generation);
        if let Ok(snapshot) = self.cursor.sample() {
            metadata.cursor = Some(project_cursor(
                &mut self.projector,
                &mut self.cursor_shape,
                &CursorTargetInfo {
                    origin_x: self.region.x,
                    origin_y: self.region.y,
                    width: self.region.width,
                    height: self.region.height,
                },
                snapshot,
            ));
        }
        metadata.queued_at = Some(Instant::now());
        #[cfg(feature = "stage-timing")]
        {
            metadata.capture_duration = Some(started.elapsed());
        }
        GpuCapturedFrame::from_layers(layers, (self.region.width, self.region.height), metadata)
    }
}

fn project_cursor(
    projector: &mut CursorProjector,
    retained: &mut Option<CursorShape>,
    target: &CursorTargetInfo,
    snapshot: snow_cursor::CursorSnapshot,
) -> snow_cursor::AttachedCursorSample {
    // Retain before deduplication: A -> B -> A returns Cached(A), even if the
    // bounded queue dropped A. Embedded shapes share their immutable Arc bytes.
    if let Some(shape) = snapshot.shape.shape() {
        *retained = Some(shape.clone());
    }
    let mut cursor = projector.project(target, snapshot);
    if let Some(shape) = retained
        .as_ref()
        .filter(|shape| cursor.shape.shape_id() == Some(shape.shape_id))
    {
        cursor.shape = CursorShapeState::Embedded(shape.clone());
    }
    cursor
}

fn intersect(region: CaptureRegion, desktop: Rect) -> CaptureResult<Option<Rect>> {
    let x = i64::from(region.x).max(i64::from(desktop.x));
    let y = i64::from(region.y).max(i64::from(desktop.y));
    let right = (i64::from(region.x) + i64::from(region.width))
        .min(i64::from(desktop.x) + i64::from(desktop.width));
    let bottom = (i64::from(region.y) + i64::from(region.height))
        .min(i64::from(desktop.y) + i64::from(desktop.height));
    if x >= right || y >= bottom {
        return Ok(None);
    }
    let convert = || -> Option<Rect> {
        Some(Rect {
            x: x.try_into().ok()?,
            y: y.try_into().ok()?,
            width: (right - x).try_into().ok()?,
            height: (bottom - y).try_into().ok()?,
        })
    };
    convert().map(Some).ok_or(CaptureError::BufferOverflow)
}

fn native_crop(crop: Rect, size: (u32, u32), rotation: u32) -> CaptureResult<Rect> {
    let x = i64::from(crop.x);
    let y = i64::from(crop.y);
    let w = i64::from(crop.width);
    let h = i64::from(crop.height);
    let (x, y, w, h) = match rotation {
        0 => (x, y, w, h),
        1 => (y, i64::from(size.1) - x - w, h, w),
        2 => (i64::from(size.0) - x - w, i64::from(size.1) - y - h, w, h),
        3 => (i64::from(size.0) - y - h, x, h, w),
        _ => {
            return Err(CaptureError::InvalidConfig(
                "invalid GPU source rotation".into(),
            ));
        }
    };
    if x < 0 || y < 0 || x + w > i64::from(size.0) || y + h > i64::from(size.1) {
        return Err(CaptureError::InvalidTarget(
            "display geometry changed during GPU capture".into(),
        ));
    }
    Ok(Rect {
        x: x as i32,
        y: y as i32,
        width: w as u32,
        height: h as u32,
    })
}

/// Copy while the acquisition/canonical source is still owned. The shared
/// context orders this before later VideoProcessor operations and pool reuse.
pub(crate) fn copy_texture(
    device: &SharedDevice,
    pool: &mut snow_d3d11::TexturePool,
    source: &windows::Win32::Graphics::Direct3D11::ID3D11Texture2D,
) -> CaptureResult<Texture> {
    use windows::Win32::Graphics::Direct3D11::*;
    let mut desc = D3D11_TEXTURE2D_DESC::default();
    unsafe { source.GetDesc(&mut desc) };
    let texture = pool
        .acquire(
            desc.Width,
            desc.Height,
            desc.Format,
            (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
        )
        .map_err(CaptureError::platform)?
        .ok_or(CaptureError::Timeout)?;
    let src: ID3D11Resource = source.cast().map_err(CaptureError::platform)?;
    let dst: ID3D11Resource = texture.raw().cast().map_err(CaptureError::platform)?;
    unsafe { device.context().CopyResource(&dst, &src) };
    Ok(texture)
}

#[cfg(test)]
mod tests {
    #[test]
    fn gpu_cursor_observations_survive_drops_and_shape_switches() {
        use snow_cursor::{CursorCompositionMode, CursorShapeCapture, CursorSnapshot};
        let mut projector = CursorProjector::new();
        let mut retained = None;
        let target = CursorTargetInfo {
            origin_x: -10,
            origin_y: -20,
            width: 100,
            height: 100,
        };
        let a = CursorShape::from_rgba(0, 0, 2, 2, CursorCompositionMode::AlphaBlend, vec![50; 16]);
        let b = CursorShape::from_rgba(
            0,
            0,
            2,
            2,
            CursorCompositionMode::MaskedColor,
            vec![255; 16],
        );
        for shape in [&a, &b, &a, &a] {
            let frame = project_cursor(
                &mut projector,
                &mut retained,
                &target,
                CursorSnapshot {
                    absolute_x: 0,
                    absolute_y: 0,
                    visible: true,
                    shape: CursorShapeCapture::Captured(shape.clone()),
                },
            );
            assert_eq!(frame.shape.embedded_shape(), Some(shape));
            assert_eq!((frame.x, frame.y, frame.visible), (10, 20, true));
        }
        let frame = project_cursor(
            &mut projector,
            &mut retained,
            &target,
            CursorSnapshot {
                absolute_x: 0,
                absolute_y: 0,
                visible: true,
                shape: CursorShapeCapture::Unavailable,
            },
        );
        assert_eq!(frame.shape.embedded_shape(), Some(&a));
    }

    use super::*;
    #[test]
    fn region_intersection_preserves_negative_origins() {
        let region = CaptureRegion::new(-50, -20, 100, 80).unwrap();
        assert_eq!(
            intersect(
                region,
                Rect {
                    x: -1920,
                    y: 0,
                    width: 1920,
                    height: 1080
                }
            )
            .unwrap(),
            Some(Rect {
                x: -50,
                y: 0,
                width: 50,
                height: 60
            })
        );
    }
    #[test]
    fn rotations_map_crop_to_native_surface() {
        let crop = Rect {
            x: 10,
            y: 20,
            width: 30,
            height: 40,
        };
        assert_eq!(
            native_crop(crop, (200, 100), 1).unwrap(),
            Rect {
                x: 20,
                y: 60,
                width: 40,
                height: 30
            }
        );
        assert_eq!(
            native_crop(crop, (200, 100), 3).unwrap(),
            Rect {
                x: 140,
                y: 10,
                width: 40,
                height: 30
            }
        );
        assert!(native_crop(Rect::full((1000, 1000)), (200, 100), 0).is_err());
    }
}
