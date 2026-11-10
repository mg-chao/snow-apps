use std::{
    collections::{BTreeSet, HashMap, HashSet},
    sync::Arc,
};

use anyhow::{Context, Result, ensure};
use gpui::{GpuCompletion, GpuRect, GpuService, GpuTexture};
use gpui_windows::gpu_extensions::DirectXGpuService;
use snow_draw_engine::*;
use windows::Win32::Graphics::Direct3D11::*;

use crate::{
    CanvasCancellation, CanvasPresentation, RetainedScene, SmartEraseSnapshot, SourceImage,
    gpu_geometry::{GpuGeometryCache, ItemGeometry, Paint},
    gpu_memory::{Allocation, GpuMemory},
    gpu_pipeline::{Parameters, Pipeline, Texture},
};

#[path = "gpu_source.rs"]
mod gpu_source;

pub const GPU_TILE_SIZE: u32 = 256;
pub const DEFAULT_GPU_MEMORY_BUDGET: usize = 512 * 1024 * 1024;

#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct GpuRenderProfile {
    pub cpu_prepare_ms: f64,
    pub uploaded_bytes: u64,
    pub mesh_upload_bytes: u64,
    pub source_upload_bytes: u64,
    pub uniform_upload_bytes: u64,
    pub damaged_pixels: u64,
    pub rendered_tiles: usize,
    pub retained_bytes: usize,
    pub peak_bytes: usize,
    pub allocations: usize,
    pub color_glyph_raster_pages: usize,
    pub submitted_draws: usize,
    pub rebuilt_geometry: usize,
    pub geometry_bytes: usize,
    pub gpu_ms: Option<f64>,
    pub revision: u64,
}

#[derive(Clone)]
pub struct GpuCanvasTile {
    pub region: GpuRect,
    pub texture: Arc<dyn GpuTexture>,
    pub revision: u64,
}

#[derive(Clone)]
pub struct GpuCanvasFrame {
    pub size: [u32; 2],
    pub scale: f32,
    pub tiles: Arc<[GpuCanvasTile]>,
    pub revision: u64,
    pub profile: GpuRenderProfile,
}

struct ManagedTexture {
    texture: Arc<dyn GpuTexture>,
    _allocation: Arc<Allocation>,
}
impl std::fmt::Debug for ManagedTexture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.texture.fmt(f)
    }
}
impl GpuTexture for ManagedTexture {
    fn service_id(&self) -> u64 {
        self.texture.service_id()
    }
    fn epoch(&self) -> u64 {
        self.texture.epoch()
    }
    fn resource_id(&self) -> u64 {
        self.texture.resource_id()
    }
    fn size(&self) -> [u32; 2] {
        self.texture.size()
    }
    fn as_any(&self) -> &dyn std::any::Any {
        self.texture.as_any()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Region {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}
impl Region {
    fn size(self) -> [u32; 2] {
        [self.width, self.height]
    }
    fn bounds(self) -> [f64; 4] {
        [
            self.x as f64,
            self.y as f64,
            self.x as f64 + self.width as f64,
            self.y as f64 + self.height as f64,
        ]
    }
    fn expanded(self, halo: i32) -> Self {
        Self {
            x: self.x - halo,
            y: self.y - halo,
            width: self.width + halo as u32 * 2,
            height: self.height + halo as u32 * 2,
        }
    }
}

struct SourcePage {
    texture: Texture,
    rect: [u32; 4],
    last_use: u64,
}

struct PendingSubmission {
    completion: Box<dyn GpuCompletion>,
    _textures: Vec<Texture>,
    _sources: Vec<Arc<dyn GpuTexture>>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct ColorGlyphKey {
    font: usvg::fontdb::ID,
    glyph: u16,
    scale: u64,
    foreground: [u8; 4],
    page: [u32; 2],
}
struct ColorGlyphPage {
    texture: Texture,
    core: [f64; 4],
    bounds: [f64; 4],
    last_use: u64,
}
struct ColorGlyphState {
    renderer: crate::gpu_color_glyph::ColorGlyphRenderer,
    allocation: Arc<Allocation>,
}
struct ScratchPool {
    free: Vec<Texture>,
    bytes: usize,
}
impl ScratchPool {
    fn take(
        &mut self,
        device: &ID3D11Device,
        size: [u32; 2],
        samples: u32,
        memory: &Arc<GpuMemory>,
    ) -> Result<Texture> {
        if let Some(index) = self
            .free
            .iter()
            .position(|texture| texture.size == size && texture.samples == samples)
        {
            let texture = self.free.remove(index);
            self.bytes -= texture.bytes();
            return Ok(texture);
        }
        if memory
            .used()
            .saturating_add(size[0] as usize * size[1] as usize * 4 * samples as usize)
            > memory.limit()
        {
            self.free.clear();
            self.bytes = 0;
        }
        Texture::with_samples(device, size, samples, memory)
    }
    fn put(&mut self, texture: Texture) {
        const BUDGET: usize = 64 * 1024 * 1024;
        if texture.bytes() > BUDGET {
            return;
        }
        // A moving/clipped text item changes surface sizes. Keep recent sizes
        // reusable instead of filling the pool permanently with obsolete ones.
        while self.bytes + texture.bytes() > BUDGET {
            let oldest = self.free.remove(0);
            self.bytes -= oldest.bytes();
        }
        self.bytes += texture.bytes();
        self.free.push(texture);
    }
}

/// All native work executes through the service belonging to the GPUI window.
pub struct GpuCanvas {
    service: Arc<dyn GpuService>,
    wake: Option<crate::gpu_wake::Notifier>,
    epoch: u64,
    memory: Arc<GpuMemory>,
    pipeline: Option<Pipeline>,
    geometry: GpuGeometryCache,
    sources: HashMap<(u64, u32, u32, u8, u8), SourcePage>,
    color_glyphs: HashMap<ColorGlyphKey, ColorGlyphPage>,
    color_glyph_bytes: usize,
    color_glyph_renderer: Option<ColorGlyphState>,
    scratch: ScratchPool,
    frame: Option<GpuCanvasFrame>,
    scene_revision: Option<u64>,
    view: Option<(FrameView, f32, u64, u64)>,
    smart_signature: u64,
    scene_identity: Option<u64>,
    decoration_revision: Option<DecorationRevision>,
    large_nodes: HashSet<RetainedNodeId>,
    filters: BTreeSet<(usize, usize)>,
    filter_ids: Vec<RetainedNodeId>,
    nodes: HashMap<RetainedNodeId, Arc<RetainedSceneNode>>,
    positions: HashMap<RetainedNodeId, usize>,
    bins: HashMap<(i32, i32), HashSet<RetainedNodeId>>,
    bounds: HashMap<RetainedNodeId, [f64; 4]>,
    prepared: HashMap<RetainedNodeId, Arc<ItemGeometry>>,
    sequence: u64,
    in_flight: Vec<PendingSubmission>,
    current_resources: Vec<Texture>,
    current_sources: Vec<Arc<dyn GpuTexture>>,
    profile: GpuRenderProfile,
    profiling: bool,
    timings: Vec<(GpuTiming, GpuRenderProfile)>,
    completed_profiles: Vec<GpuRenderProfile>,
}

pub(crate) struct GpuTiming {
    disjoint: ID3D11Query,
    start: ID3D11Query,
    end: ID3D11Query,
    context: ID3D11DeviceContext,
    ended: std::cell::Cell<bool>,
}
impl Drop for GpuTiming {
    fn drop(&mut self) {
        self.end(&self.context);
    }
}
impl GpuTiming {
    pub(crate) fn start(device: &ID3D11Device, context: &ID3D11DeviceContext) -> Result<Self> {
        let query = |kind| -> Result<ID3D11Query> {
            let mut output = None;
            unsafe {
                device.CreateQuery(
                    &D3D11_QUERY_DESC {
                        Query: kind,
                        MiscFlags: 0,
                    },
                    Some(&mut output),
                )?;
            }
            output.context("GPU timing query unavailable")
        };
        let result = Self {
            disjoint: query(D3D11_QUERY_TIMESTAMP_DISJOINT)?,
            start: query(D3D11_QUERY_TIMESTAMP)?,
            end: query(D3D11_QUERY_TIMESTAMP)?,
            context: context.clone(),
            ended: std::cell::Cell::new(false),
        };
        unsafe {
            context.Begin(&result.disjoint);
            context.End(&result.start);
        }
        Ok(result)
    }
    pub(crate) fn end(&self, context: &ID3D11DeviceContext) {
        if !self.ended.replace(true) {
            unsafe {
                context.End(&self.end);
                context.End(&self.disjoint);
            }
        }
    }
    pub(crate) fn poll(&self, context: &ID3D11DeviceContext) -> Result<Option<Option<f64>>> {
        let mut status = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::default();
        let mut start = 0_u64;
        let mut end = 0_u64;
        unsafe {
            use windows::core::Interface;
            let get = |query: &ID3D11Query, data: *mut core::ffi::c_void, size: u32| {
                (context.vtable().GetData)(
                    context.as_raw(),
                    query.as_raw(),
                    data,
                    size,
                    D3D11_ASYNC_GETDATA_DONOTFLUSH.0 as u32,
                )
            };
            let result = get(
                &self.disjoint,
                (&mut status as *mut D3D11_QUERY_DATA_TIMESTAMP_DISJOINT).cast(),
                std::mem::size_of_val(&status) as u32,
            );
            if result == windows::core::HRESULT(1) {
                return Ok(None);
            }
            result.ok()?;
            let result = get(&self.start, (&mut start as *mut u64).cast(), 8);
            if result == windows::core::HRESULT(1) {
                return Ok(None);
            }
            result.ok()?;
            let result = get(&self.end, (&mut end as *mut u64).cast(), 8);
            if result == windows::core::HRESULT(1) {
                return Ok(None);
            }
            result.ok()?;
        }
        Ok(Some(
            (!status.Disjoint.as_bool() && status.Frequency > 0)
                .then(|| end.saturating_sub(start) as f64 / status.Frequency as f64 * 1000.0),
        ))
    }
}

impl Drop for GpuCanvas {
    fn drop(&mut self) {
        self.clear();
    }
}

impl GpuCanvas {
    pub fn new(service: Arc<dyn GpuService>) -> Result<Self> {
        Self::create(service, None)
    }
    pub fn with_memory_budget(service: Arc<dyn GpuService>, budget: usize) -> Result<Self> {
        Self::create(service, Some(budget))
    }
    fn create(service: Arc<dyn GpuService>, requested_budget: Option<usize>) -> Result<Self> {
        ensure!(
            service.as_any().is::<DirectXGpuService>(),
            "This canvas requires the GPUI D3D11 extension"
        );
        ensure!(
            service.capabilities().rgba8_sample_counts.contains(&4),
            "The GPUI GPU does not support four-sample RGBA coverage surfaces"
        );
        ensure!(
            requested_budget.is_none_or(|budget| budget >= 16 * 1024 * 1024),
            "Canvas GPU budget must be at least 16 MiB"
        );
        let memory = GpuMemory::shared(&service, requested_budget, DEFAULT_GPU_MEMORY_BUDGET)?;
        Ok(Self {
            epoch: service.epoch(),
            service,
            wake: None,
            memory,
            pipeline: None,
            geometry: GpuGeometryCache::default(),
            sources: HashMap::new(),
            color_glyphs: HashMap::new(),
            color_glyph_bytes: 0,
            color_glyph_renderer: None,
            scratch: ScratchPool {
                free: Vec::new(),
                bytes: 0,
            },
            frame: None,
            scene_revision: None,
            view: None,
            smart_signature: 0,
            scene_identity: None,
            decoration_revision: None,
            large_nodes: HashSet::new(),
            filters: BTreeSet::new(),
            filter_ids: Vec::new(),
            nodes: HashMap::new(),
            positions: HashMap::new(),
            bins: HashMap::new(),
            bounds: HashMap::new(),
            prepared: HashMap::new(),
            sequence: 0,
            in_flight: Vec::new(),
            current_resources: Vec::new(),
            current_sources: Vec::new(),
            profile: GpuRenderProfile::default(),
            profiling: false,
            timings: Vec::new(),
            completed_profiles: Vec::new(),
        })
    }
    pub fn is_ready(&mut self) -> Result<bool> {
        if self.epoch != self.service.epoch() {
            self.clear();
            self.memory = GpuMemory::shared(
                &self.service,
                Some(self.memory.limit()),
                DEFAULT_GPU_MEMORY_BUDGET,
            )?;
            self.epoch = self.service.epoch();
            self.wake = None;
        }
        self.service.poll_deferred()?;
        let mut index = 0;
        while index < self.in_flight.len() {
            // Preserve every pending completion/resource on a poll error. A
            // draining iterator would destroy the unvisited leases on `?`.
            if self.in_flight[index].completion.poll()? {
                drop(self.in_flight.swap_remove(index));
            } else {
                index += 1;
            }
        }
        let deferred_pending = self.service.poll_deferred()?;
        if self.memory.limit().saturating_sub(self.memory.used()) < 16 * 1024 * 1024
            && (deferred_pending || self.memory.has_retirement() || !self.in_flight.is_empty())
        {
            return Ok(false);
        }
        Ok(self.in_flight.len() < 2)
    }
    /// Scheduling hint for submitted frame work. Poll `is_ready` again after
    /// waking; retirement, device errors and publication remain owner operations.
    pub fn completion_wake(&mut self) -> Result<Option<crate::GpuCompletionWake>> {
        ensure!(
            self.epoch == self.service.epoch(),
            "Canvas completion device epoch changed"
        );
        if self.wake.is_none() {
            self.wake = Some(crate::gpu_wake::Notifier::new(self.service.clone())?);
        }
        self.wake.as_ref().unwrap().wake()
    }
    pub(crate) fn smart_filters(&self) -> Vec<FilterDisplayItem> {
        self.filters
            .iter()
            .filter_map(|(_, slot)| self.nodes.get(&self.filter_ids[*slot]))
            .filter_map(|node| match node.item.as_ref() {
                SceneDisplayItem::Filter(filter)
                    if filter.filter.filter_type == DisplayFilterType::SmartErase
                        && filter.filter.render_phase == 0 =>
                {
                    Some(filter.clone())
                }
                _ => None,
            })
            .collect()
    }
    pub fn service(&self) -> Arc<dyn GpuService> {
        self.service.clone()
    }
    pub(crate) fn reserve_readback(&self, bytes: usize) -> Result<Arc<Allocation>> {
        self.memory.reserve(bytes)
    }
    pub fn set_profiling(&mut self, enabled: bool) {
        self.profiling = enabled;
    }
    pub fn take_completed_profiles(&mut self) -> Result<Vec<GpuRenderProfile>> {
        let service = self.service.clone();
        let native = service
            .as_any()
            .downcast_ref::<DirectXGpuService>()
            .context("GPUI D3D11 service unavailable")?;
        native.with_context(|context| {
            let mut pending = Vec::new();
            for (timing, mut profile) in self.timings.drain(..) {
                if let Some(duration) = timing.poll(context.immediate_context)? {
                    profile.gpu_ms = duration;
                    self.completed_profiles.push(profile);
                } else {
                    pending.push((timing, profile));
                }
            }
            self.timings = pending;
            Ok(())
        })?;
        Ok(std::mem::take(&mut self.completed_profiles))
    }
    pub fn clear(&mut self) {
        // Keep native resources on the service owner thread until the queued
        // GPU uses finish. Final Allocation drops then retire their accounting
        // behind a fresh marker, including frames outliving this canvas.
        let resources = (
            self.frame.take(),
            self.pipeline.take(),
            std::mem::take(&mut self.sources),
            std::mem::take(&mut self.scratch.free),
            std::mem::take(&mut self.current_resources),
            std::mem::take(&mut self.in_flight),
            std::mem::take(&mut self.timings),
            self.color_glyph_renderer.take(),
            std::mem::take(&mut self.color_glyphs),
            std::mem::take(&mut self.current_sources),
        );
        if resources.0.is_some()
            || resources.1.is_some()
            || !resources.2.is_empty()
            || !resources.3.is_empty()
            || !resources.4.is_empty()
            || !resources.5.is_empty()
            || !resources.6.is_empty()
            || resources.7.is_some()
            || !resources.8.is_empty()
            || !resources.9.is_empty()
        {
            self.service.defer_release(Box::new(resources));
        }
        self.view = None;
        self.scene_revision = None;
        self.scratch.bytes = 0;
        self.color_glyph_bytes = 0;
        self.nodes.clear();
        self.positions.clear();
        self.bins.clear();
        self.bounds.clear();
        self.prepared.clear();
        self.large_nodes.clear();
        self.filters.clear();
        self.filter_ids.clear();
        self.scene_identity = None;
        self.decoration_revision = None;
        self.geometry.clear();
        self.completed_profiles.clear();
    }

    pub(crate) fn prepare_overlay(
        &mut self,
        geometry: &ItemGeometry,
        frame: FrameView,
        scale: f32,
    ) -> Result<GpuCanvasFrame> {
        let start = std::time::Instant::now();
        ensure!(self.is_ready()?, "GPU overlay queue is busy");
        let allocations_before = self.memory.allocations();
        let mesh_before = self
            .pipeline
            .as_ref()
            .map_or(0, |pipeline| pipeline.uploaded_bytes);
        let uniform_before = self.pipeline.as_ref().map_or(0, |pipeline| {
            pipeline
                .uniform_bytes
                .load(std::sync::atomic::Ordering::Relaxed)
        });
        let size = crate::export::scaled_dimensions(frame.surface, scale)?;
        crate::document::validate_surface(size[0], size[1])?;
        let mut bounds = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for part in &geometry.parts {
            let [left, top, right, bottom] = part.mesh.bounds.map(f64::from);
            let [xx, yx, xy, yy, tx, ty] = part.transform;
            for [x, y] in [[left, top], [right, top], [right, bottom], [left, bottom]] {
                let point = canvas_to_view(
                    Point::new(xx * x + xy * y + tx, yx * x + yy * y + ty),
                    &frame.camera,
                    frame.surface,
                );
                bounds = union(
                    bounds,
                    [
                        point.x * f64::from(scale),
                        point.y * f64::from(scale),
                        point.x * f64::from(scale),
                        point.y * f64::from(scale),
                    ],
                );
            }
        }
        let bounds = intersection(
            expand(bounds, 2.0),
            [0.0, 0.0, size[0] as f64, size[1] as f64],
        );
        self.sequence += 1;
        self.profile = GpuRenderProfile::default();
        let service = self.service.clone();
        let native = service
            .as_any()
            .downcast_ref::<DirectXGpuService>()
            .context("GPUI D3D11 service unavailable")?;
        let textures = native.with_context(|context| {
            if self.pipeline.is_none() {
                self.pipeline = Some(Pipeline::new(context.device, self.memory.clone())?);
            }
            let mut output = Vec::new();
            if bounds[2] <= bounds[0] || bounds[3] <= bounds[1] {
                return Ok(output);
            }
            for (x, y) in tile_coordinates(bounds) {
                if x < 0
                    || y < 0
                    || x as u32 * GPU_TILE_SIZE >= size[0]
                    || y as u32 * GPU_TILE_SIZE >= size[1]
                {
                    continue;
                }
                let region = Region {
                    x: x * GPU_TILE_SIZE as i32,
                    y: y * GPU_TILE_SIZE as i32,
                    width: GPU_TILE_SIZE.min(size[0] - x as u32 * GPU_TILE_SIZE),
                    height: GPU_TILE_SIZE.min(size[1] - y as u32 * GPU_TILE_SIZE),
                };
                let mut texture = Texture::new(context.device, region.size(), &self.memory)?;
                Pipeline::clear(context.immediate_context, &texture, [0.0; 4]);
                self.paint_item(
                    context.device,
                    context.immediate_context,
                    &mut texture,
                    geometry,
                    frame,
                    scale,
                    region,
                )?;
                output.push((region, texture));
            }
            Ok(output)
        })?;
        let mut tiles = Vec::new();
        for (region, texture) in textures {
            let wrapped = native.wrap_texture(texture.image.clone())?;
            tiles.push(GpuCanvasTile {
                region: GpuRect {
                    x: region.x as u32,
                    y: region.y as u32,
                    width: region.width,
                    height: region.height,
                },
                texture: Arc::new(ManagedTexture {
                    texture: wrapped,
                    _allocation: texture.allocation,
                }),
                revision: self.sequence,
            });
        }
        self.retain_submission(native.completion()?);
        self.profile.allocations = self.memory.allocations() - allocations_before;
        if let Some(pipeline) = &self.pipeline {
            self.profile.mesh_upload_bytes = pipeline.uploaded_bytes - mesh_before;
            self.profile.uniform_upload_bytes = pipeline
                .uniform_bytes
                .load(std::sync::atomic::Ordering::Relaxed)
                - uniform_before;
            self.profile.uploaded_bytes =
                self.profile.mesh_upload_bytes + self.profile.uniform_upload_bytes;
        }
        self.profile.rendered_tiles = tiles.len();
        self.profile.retained_bytes = self.memory.used();
        self.profile.peak_bytes = self.memory.peak();
        self.profile.cpu_prepare_ms = start.elapsed().as_secs_f64() * 1000.0;
        Ok(GpuCanvasFrame {
            size,
            scale,
            tiles: tiles.into(),
            revision: self.sequence,
            profile: self.profile.clone(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn prepare(
        &mut self,
        scene: &RetainedScene,
        background: Option<&SourceImage>,
        resource_revision: u64,
        scale: f32,
        presentation: &CanvasPresentation,
        smart: &SmartEraseSnapshot,
        cancel: &CanvasCancellation,
    ) -> Result<GpuCanvasFrame> {
        let start = std::time::Instant::now();
        cancel.check()?;
        presentation.validate()?;
        ensure!(
            scale.is_finite() && (0.1..=8.0).contains(&scale),
            "Invalid canvas rendering scale"
        );
        ensure!(self.is_ready()?, "GPU frame queue is busy");
        let allocations_before = self.memory.allocations();
        let frame = scene.frame();
        let size = crate::export::scaled_dimensions(frame.surface, scale)?;
        crate::document::validate_surface(size[0], size[1])?;
        let key = (frame, scale, resource_revision, presentation.cache_key());
        let smart_signature = smart.entries.iter().fold(0_u64, |key, patch| {
            key.wrapping_mul(37).wrapping_add(Arc::as_ptr(patch) as u64)
        });
        let identity = scene.retained_identity();
        let reset = self.view != Some(key)
            || self.frame.is_none()
            || self.scene_identity != Some(identity)
            || scene
                .changes_since(self.scene_revision.unwrap_or(0))
                .is_none();
        let decoration_revision = scene.cursor().unwrap_or_default().decoration_revision;
        let decorations_changed = self.decoration_revision != Some(decoration_revision);
        let keep_prepared = self.scene_identity == Some(identity)
            && self.view.is_some_and(|(previous, previous_scale, _, _)| {
                previous.camera.zoom * f64::from(previous_scale)
                    == frame.camera.zoom * f64::from(scale)
                    && previous.clear_color == frame.clear_color
            });
        let mut changed = self.sync_scene(scene, frame, scale, reset, keep_prepared);
        if !reset && self.smart_signature != smart_signature {
            for (_, slot) in &self.filters {
                let id = self.filter_ids[*slot];
                if self.nodes.get(&id).is_some_and(|node| matches!(node.item.as_ref(),SceneDisplayItem::Filter(filter) if filter.filter.filter_type==DisplayFilterType::SmartErase))
                    && let Some(bounds)=self.bounds.get(&id) {changed.push(*bounds);}
            }
        }
        let columns = size[0].div_ceil(GPU_TILE_SIZE);
        let rows = size[1].div_ceil(GPU_TILE_SIZE);
        let mut dirty = BTreeSet::new();
        if reset || decorations_changed {
            dirty.extend(0..columns * rows);
        } else {
            for bounds in changed {
                let expanded = self.propagate_damage(bounds, frame, scale);
                for (x, y) in tile_coordinates(intersection(
                    expanded,
                    [0.0, 0.0, size[0] as f64, size[1] as f64],
                )) {
                    if x >= 0 && y >= 0 && x < columns as i32 && y < rows as i32 {
                        dirty.insert(y as u32 * columns + x as u32);
                    }
                }
            }
        }
        if dirty.is_empty() {
            self.view = Some(key);
            self.smart_signature = smart_signature;
            let mut frame = self
                .frame
                .clone()
                .context("Canvas has no completed frame")?;
            frame.profile = GpuRenderProfile {
                retained_bytes: self.memory.used(),
                peak_bytes: self.memory.peak(),
                revision: frame.revision,
                ..Default::default()
            };
            return Ok(frame);
        }
        self.sequence += 1;
        self.profile = GpuRenderProfile::default();
        // A failed candidate must never advance the consumer's successful damage cursor.
        // Keep the previous frame published and force a full rebuild on the next request.
        self.view = None;
        let service = self.service.clone();
        let native = service
            .as_any()
            .downcast_ref::<DirectXGpuService>()
            .context("GPUI GPU backend changed")?;
        let candidate = native.with_context(|context| {
            if self.pipeline.is_none() {
                self.pipeline = Some(Pipeline::new(context.device, self.memory.clone())?);
            }
            let timing = if self.profiling {
                Some(GpuTiming::start(context.device, context.immediate_context)?)
            } else {
                None
            };
            let before = self
                .pipeline
                .as_ref()
                .context("Canvas pipeline missing")?
                .uploaded_bytes;
            let uniforms_before = self
                .pipeline
                .as_ref()
                .unwrap()
                .uniform_bytes
                .load(std::sync::atomic::Ordering::Relaxed);
            let mut outputs = Vec::with_capacity(dirty.len());
            for &index in &dirty {
                cancel.check()?;
                let x = index % columns * GPU_TILE_SIZE;
                let y = index / columns * GPU_TILE_SIZE;
                let region = Region {
                    x: x as i32,
                    y: y as i32,
                    width: GPU_TILE_SIZE.min(size[0] - x),
                    height: GPU_TILE_SIZE.min(size[1] - y),
                };
                let texture = self.render_tile(
                    context.device,
                    context.immediate_context,
                    scene,
                    background,
                    frame,
                    scale,
                    presentation,
                    smart,
                    region,
                    cancel,
                )?;
                self.profile.damaged_pixels += u64::from(region.width) * u64::from(region.height);
                outputs.push((
                    index as usize,
                    GpuRect {
                        x,
                        y,
                        width: region.width,
                        height: region.height,
                    },
                    texture,
                ));
            }
            self.profile.mesh_upload_bytes = self
                .pipeline
                .as_ref()
                .context("Canvas pipeline missing")?
                .uploaded_bytes
                - before;
            self.profile.uniform_upload_bytes = self
                .pipeline
                .as_ref()
                .unwrap()
                .uniform_bytes
                .load(std::sync::atomic::Ordering::Relaxed)
                - uniforms_before;
            self.profile.uploaded_bytes +=
                self.profile.mesh_upload_bytes + self.profile.uniform_upload_bytes;
            if let Some(timing) = &timing {
                timing.end(context.immediate_context);
            }
            Ok((outputs, timing))
        });
        let (outputs, timing) = match candidate {
            Ok(candidate) => candidate,
            Err(error) => {
                if let Ok(completion) = native.completion() {
                    self.retain_submission(completion);
                }
                return Err(error);
            }
        };
        let mut tiles: Vec<Option<GpuCanvasTile>> = if reset {
            vec![None; (columns * rows) as usize]
        } else {
            self.frame
                .as_ref()
                .context("Previous GPU frame missing")?
                .tiles
                .iter()
                .cloned()
                .map(Some)
                .collect()
        };
        for (index, region, texture) in outputs {
            let wrapped = native.wrap_texture(texture.image.clone())?;
            let texture: Arc<dyn GpuTexture> = Arc::new(ManagedTexture {
                texture: wrapped,
                _allocation: texture.allocation.clone(),
            });
            tiles[index] = Some(GpuCanvasTile {
                region,
                texture,
                revision: self.sequence,
            });
        }
        self.profile.rendered_tiles = dirty.len();
        self.profile.geometry_bytes = self.geometry.retained_bytes();
        if let Some(pipeline) = self.pipeline.as_mut()
            && self.profile.mesh_upload_bytes > 0
            && pipeline.mesh_bytes > 128 * 1024 * 1024
        {
            pipeline.collect_meshes();
        }
        self.profile.retained_bytes = self.memory.used();
        self.profile.peak_bytes = self.memory.peak();
        self.profile.allocations = self.memory.allocations() - allocations_before;
        self.profile.cpu_prepare_ms = start.elapsed().as_secs_f64() * 1000.0;
        self.profile.revision = self.sequence;
        if let Some(timing) = timing {
            if self.timings.len() == 64 {
                self.timings.remove(0);
            }
            self.timings.push((timing, self.profile.clone()));
        }
        let result = GpuCanvasFrame {
            size,
            scale,
            tiles: tiles
                .into_iter()
                .map(|tile| tile.context("Incomplete canvas tile set"))
                .collect::<Result<Vec<_>>>()?
                .into(),
            revision: self.sequence,
            profile: self.profile.clone(),
        };
        self.retain_submission(native.completion()?);
        self.view = Some(key);
        self.frame = Some(result.clone());
        self.scene_identity = Some(identity);
        self.decoration_revision = Some(decoration_revision);
        self.smart_signature = smart_signature;
        Ok(result)
    }

    fn retain_submission(&mut self, completion: Box<dyn GpuCompletion>) {
        self.in_flight.push(PendingSubmission {
            completion,
            _textures: std::mem::take(&mut self.current_resources),
            _sources: std::mem::take(&mut self.current_sources),
        });
    }

    fn sync_scene(
        &mut self,
        scene: &RetainedScene,
        frame: FrameView,
        scale: f32,
        reset: bool,
        keep_prepared: bool,
    ) -> Vec<[f64; 4]> {
        // The transport retains changes between publications; a stale cursor forces a reset.
        let changed_ids = if reset {
            None
        } else {
            scene.changes_since(self.scene_revision.unwrap_or(0))
        };
        let ids: Vec<_> = match changed_ids {
            Some(changes) => changes,
            None => {
                if keep_prepared {
                    // Camera translation changes visibility, but local geometry,
                    // material and world pose remain reusable at the same density.
                    self.prepared.retain(|id, _| {
                        self.nodes
                            .get(id)
                            .zip(scene.node(*id))
                            .is_some_and(|(old, new)| {
                                old.geometry_revision == new.geometry_revision
                                    && old.material_revision == new.material_revision
                                    && old.pose_revision == new.pose_revision
                            })
                    });
                } else {
                    self.prepared.clear();
                }
                self.nodes.clear();
                self.bins.clear();
                self.bounds.clear();
                self.positions.clear();
                self.large_nodes.clear();
                self.filters.clear();
                self.filter_ids.clear();
                for (index, id) in scene.order_ids().iter().enumerate() {
                    self.positions.insert(*id, index);
                    if scene.node(*id).is_some_and(|node| {
                        matches!(node.item.as_ref(), SceneDisplayItem::Filter(_))
                    }) {
                        self.filters.insert((index, self.filter_ids.len()));
                        self.filter_ids.push(*id);
                    }
                }
                scene.order_ids().to_vec()
            }
        };
        let mut damage = Vec::new();
        for id in ids {
            let chunk_damage = self
                .nodes
                .get(&id)
                .zip(scene.node(id))
                .and_then(|(old, new)| changed_stroke_bounds(old, new))
                .map(|bounds| pixel_bounds(bounds, frame, scale));
            if let Some(bounds) = chunk_damage {
                damage.push(bounds);
            }
            if let Some(old) = self.bounds.remove(&id) {
                if chunk_damage.is_none() {
                    damage.push(old);
                }
                if !self.large_nodes.remove(&id) {
                    for cell in spatial_cells(old) {
                        if let Some(items) = self.bins.get_mut(&cell) {
                            items.remove(&id);
                        }
                    }
                }
            }
            if let Some(node) = scene.node(id) {
                let bounds = pixel_bounds(node.bounds, frame, scale);
                if !reset
                    && self.nodes.get(&id).is_none_or(|old| {
                        old.geometry_revision != node.geometry_revision
                            || old.material_revision != node.material_revision
                            || old.pose_revision != node.pose_revision
                    })
                {
                    self.prepared.remove(&id);
                }
                self.nodes.insert(id, node.clone());
                self.bounds.insert(id, bounds);
                if chunk_damage.is_none() {
                    damage.push(bounds);
                }
                let cells =
                    ((bounds[2] - bounds[0]) / 64.0 + 2.0) * ((bounds[3] - bounds[1]) / 64.0 + 2.0);
                if cells > 256.0 {
                    self.large_nodes.insert(id);
                } else {
                    for cell in spatial_cells(bounds) {
                        self.bins.entry(cell).or_default().insert(id);
                    }
                }
            } else {
                self.nodes.remove(&id);
                self.prepared.remove(&id);
            }
        }
        self.scene_revision = scene.retained_revision();
        damage
    }

    fn propagate_damage(&self, bounds: [f64; 4], frame: FrameView, scale: f32) -> [f64; 4] {
        let mut result = bounds;
        for (_, slot) in &self.filters {
            let Some(node) = self.nodes.get(&self.filter_ids[*slot]) else {
                continue;
            };
            let SceneDisplayItem::Filter(filter) = node.item.as_ref() else {
                continue;
            };
            let filter_bounds = pixel_bounds(
                crate::source_geometry::filter_canvas_bounds(filter).into_array(),
                frame,
                scale,
            );
            let support =
                filter.filter.sampling_radius * frame.camera.zoom * f64::from(scale) + 2.0;
            let affected = expand(result, support);
            if intersects(affected, filter_bounds) {
                result = union(result, intersection(affected, filter_bounds));
            }
        }
        result
    }

    fn candidates(&self, region: Region) -> Vec<RetainedNodeId> {
        let mut ids: HashSet<_> = self
            .large_nodes
            .iter()
            .filter(|id| {
                self.bounds
                    .get(id)
                    .is_some_and(|bounds| intersects(*bounds, region.bounds()))
            })
            .copied()
            .collect();
        for cell in spatial_cells(region.bounds()) {
            if let Some(items) = self.bins.get(&cell) {
                ids.extend(items.iter().copied());
            }
        }
        let mut ids: Vec<_> = ids
            .into_iter()
            .filter(|id| {
                self.bounds
                    .get(id)
                    .is_some_and(|bounds| intersects(*bounds, region.bounds()))
            })
            .collect();
        ids.sort_by_key(|id| self.positions.get(id).copied().unwrap_or(usize::MAX));
        ids
    }

    #[allow(clippy::too_many_arguments)]
    fn render_tile(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        scene: &RetainedScene,
        background: Option<&SourceImage>,
        frame: FrameView,
        scale: f32,
        presentation: &CanvasPresentation,
        smart: &SmartEraseSnapshot,
        core: Region,
        cancel: &CanvasCancellation,
    ) -> Result<Texture> {
        let zoom = frame.camera.zoom * f64::from(scale);
        let mut halo = 2.0_f64;
        let mut source_pass = None;
        let mut pass_support = 0.0_f64;
        for run in scene.runs().iter().rev() {
            if source_pass != Some(run.source_pass) {
                halo += pass_support;
                pass_support = 0.0;
                source_pass = Some(run.source_pass);
            }
            let region = core.expanded(halo.ceil() as i32);
            let support = scene.order_ids()[run.start as usize..(run.start + run.count) as usize]
                .iter()
                .filter_map(|id| self.nodes.get(id))
                .filter_map(|node| match node.item.as_ref() {
                    SceneDisplayItem::Filter(filter)
                        if intersects(pixel_bounds(node.bounds, frame, scale), region.bounds()) =>
                    {
                        Some(filter.filter.sampling_radius * zoom + 2.0)
                    }
                    _ => None,
                })
                .fold(0.0_f64, f64::max);
            pass_support = pass_support.max(support);
        }
        halo += pass_support;
        ensure!(
            halo <= 8192.0,
            "Effect support exceeds the working GPU region limit"
        );
        let region = core.expanded(halo.ceil() as i32);
        let mut target = self.scratch.take(device, region.size(), 1, &self.memory)?;
        let clear = if presentation.clear_background {
            rgba(frame.clear_color)
        } else {
            [0.0; 4]
        };
        let pristine_frame = FrameView {
            clear_color: if presentation.clear_background {
                frame.clear_color
            } else {
                ColorRgba8::default()
            },
            ..frame
        };
        Pipeline::clear(context, &target, premultiply(clear));
        self.paint_source(
            device,
            context,
            &target,
            presentation.background_override.as_ref().or(background),
            frame,
            scale,
            region,
            1.0,
            None,
        )?;
        if let Some(color) = presentation.background_tint {
            let mut parameters = Parameters::for_target(region.size());
            parameters.color = rgba(color);
            parameters.effect[0] = 11.;
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                &target,
                [None, None, None],
                &parameters,
                true,
                None,
            );
        }
        self.paint_source(
            device,
            context,
            &target,
            presentation.before.as_ref(),
            frame,
            scale,
            region,
            1.0,
            None,
        )?;
        let before_content =
            if presentation.content_clip.is_some() || presentation.content_mask.is_some() {
                let copy = self.scratch.take(device, region.size(), 1, &self.memory)?;
                Pipeline::copy(context, &target, &copy);
                Some(copy)
            } else {
                None
            };
        if presentation.content_visible {
            self.paint_spotlight(
                device,
                context,
                &target,
                scene,
                frame,
                scale,
                region,
                presentation,
            )?;
            let candidates = self.candidates(region);
            let mut source_pass = None;
            let mut entering: Option<Texture> = None;
            let mut visited_runs = HashSet::new();
            let mut vectors: Option<Texture> = None;
            for id in candidates {
                cancel.check()?;
                let node = self
                    .nodes
                    .get(&id)
                    .cloned()
                    .context("Retained item disappeared")?;
                let index = self
                    .positions
                    .get(&id)
                    .copied()
                    .context("Retained item order missing")?;
                if let Some(run) = scene.runs().iter().find(|run| {
                    index >= run.start as usize && index < (run.start + run.count) as usize
                }) {
                    self.resolve_vectors(context, &target, &mut vectors);
                    if !visited_runs.insert(run.effect_run) {
                        continue;
                    }
                    if source_pass != Some(run.source_pass) {
                        if let Some(old) = entering.take() {
                            self.scratch.put(old);
                        }
                        let copy = self.scratch.take(device, region.size(), 1, &self.memory)?;
                        Pipeline::copy(context, &target, &copy);
                        entering = Some(copy);
                        source_pass = Some(run.source_pass);
                    }
                    let filters: Vec<_> = scene.order_ids()
                        [run.start as usize..(run.start + run.count) as usize]
                        .iter()
                        .filter_map(|id| self.nodes.get(id))
                        .filter_map(|node| {
                            if let SceneDisplayItem::Filter(filter) = node.item.as_ref() {
                                Some(filter.clone())
                            } else {
                                None
                            }
                        })
                        .collect();
                    self.paint_filter_run(
                        device,
                        context,
                        &mut target,
                        entering.as_ref().context("Effect input missing")?,
                        &filters,
                        background,
                        pristine_frame,
                        scale,
                        region,
                        smart,
                    )?;
                } else {
                    match node.item.as_ref() {
                        SceneDisplayItem::Filter(filter) => {
                            self.resolve_vectors(context, &target, &mut vectors);
                            let source = target.clone();
                            self.paint_filter_run(
                                device,
                                context,
                                &mut target,
                                &source,
                                std::slice::from_ref(filter),
                                background,
                                pristine_frame,
                                scale,
                                region,
                                smart,
                            )?;
                        }
                        SceneDisplayItem::Magnifier(lens) => {
                            self.resolve_vectors(context, &target, &mut vectors);
                            self.paint_magnifier(
                                device,
                                context,
                                &mut target,
                                lens,
                                background,
                                frame,
                                scale,
                                region,
                            )?;
                        }
                        _ => {
                            let geometry = if let Some(geometry) = self.prepared.get(&id) {
                                geometry.clone()
                            } else {
                                let local = self.geometry.item(
                                    node.local_item.as_ref(),
                                    zoom,
                                    frame.clear_color,
                                )?;
                                let p = node.pose;
                                let transform = [p.xx, p.yx, p.xy, p.yy, p.tx, p.ty];
                                let mut parts = local.parts.clone();
                                for part in &mut parts {
                                    part.transform = compose_affine(transform, part.transform);
                                }
                                let label_gap = local.label_gap.map(|gap| {
                                    let mut bounds = [
                                        f64::INFINITY,
                                        f64::INFINITY,
                                        f64::NEG_INFINITY,
                                        f64::NEG_INFINITY,
                                    ];
                                    for [x, y] in [
                                        [gap.min_x, gap.min_y],
                                        [gap.max_x, gap.min_y],
                                        [gap.max_x, gap.max_y],
                                        [gap.min_x, gap.max_y],
                                    ] {
                                        let px = p.xx * x + p.xy * y + p.tx;
                                        let py = p.yx * x + p.yy * y + p.ty;
                                        bounds = union(bounds, [px, py, px, py]);
                                    }
                                    DrawRect {
                                        min_x: bounds[0],
                                        min_y: bounds[1],
                                        max_x: bounds[2],
                                        max_y: bounds[3],
                                    }
                                });
                                let geometry = Arc::new(ItemGeometry {
                                    parts,
                                    opacity: local.opacity,
                                    blend: local.blend,
                                    label_gap,
                                });
                                self.prepared.insert(id, geometry.clone());
                                self.profile.rebuilt_geometry += 1;
                                geometry
                            };
                            if geometry.blend == DisplayBlendMode::Multiply {
                                self.resolve_vectors(context, &target, &mut vectors);
                                self.paint_item(
                                    device,
                                    context,
                                    &mut target,
                                    &geometry,
                                    frame,
                                    scale,
                                    region,
                                )?;
                            } else {
                                if vectors.is_none() {
                                    let msaa = self.scratch.take(
                                        device,
                                        region.size(),
                                        4,
                                        &self.memory,
                                    )?;
                                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                                        context,
                                        &msaa,
                                        [Some(&target), None, None],
                                        &Parameters::for_target(region.size()),
                                        false,
                                        None,
                                    );
                                    vectors = Some(msaa);
                                }
                                self.paint_item(
                                    device,
                                    context,
                                    vectors.as_mut().unwrap(),
                                    &geometry,
                                    frame,
                                    scale,
                                    region,
                                )?;
                            }
                        }
                    }
                }
            }
            self.resolve_vectors(context, &target, &mut vectors);
            if let Some(entering) = entering {
                self.scratch.put(entering);
            }
            self.paint_watermark(
                device,
                context,
                &mut target,
                scene,
                frame,
                scale,
                region,
                presentation,
            )?;
        }
        if let (Some(before), Some(mask)) =
            (before_content.as_ref(), presentation.content_mask.as_ref())
        {
            let coverage = self.scratch.take(device, region.size(), 1, &self.memory)?;
            Pipeline::clear(context, &coverage, [0.; 4]);
            self.paint_source(
                device,
                context,
                &coverage,
                Some(mask),
                frame,
                scale,
                region,
                1.,
                None,
            )?;
            let composed = self.scratch.take(device, region.size(), 1, &self.memory)?;
            let mut parameters = Parameters::for_target(region.size());
            parameters.effect[0] = 18.;
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                &composed,
                [Some(&target), Some(&coverage), Some(before)],
                &parameters,
                false,
                None,
            );
            self.scratch.put(coverage);
            self.scratch.put(target);
            target = composed;
        }
        if let (Some(before), Some(clip)) = (before_content.as_ref(), presentation.content_clip) {
            let inside = pixel_rect(
                [clip.x, clip.y, clip.x + clip.width, clip.y + clip.height],
                frame,
                scale,
            );
            let clipped = clip_scissor(inside, region);
            let composed = self.scratch.take(device, region.size(), 1, &self.memory)?;
            Pipeline::copy(context, before, &composed);
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                &composed,
                [Some(&target), None, None],
                &Parameters::for_target(region.size()),
                false,
                Some(clipped),
            );
            self.scratch.put(target);
            target = composed;
        }
        if let Some(before) = before_content {
            self.scratch.put(before);
        }
        self.paint_source(
            device,
            context,
            &target,
            presentation.after.as_ref(),
            frame,
            scale,
            region,
            1.0,
            None,
        )?;
        let output = Texture::new(device, core.size(), &self.memory)?;
        Pipeline::unbind(context);
        let left = (core.x - region.x) as u32;
        let top = (core.y - region.y) as u32;
        output.allocation.mark_gpu_use();
        target.allocation.mark_gpu_use();
        unsafe {
            context.CopySubresourceRegion(
                &output.image,
                0,
                0,
                0,
                0,
                &target.image,
                0,
                Some(&D3D11_BOX {
                    left,
                    top,
                    front: 0,
                    right: left + core.width,
                    bottom: top + core.height,
                    back: 1,
                }),
            );
        }
        self.scratch.put(target);
        Ok(output)
    }

    fn resolve_vectors(
        &mut self,
        context: &ID3D11DeviceContext,
        target: &Texture,
        vectors: &mut Option<Texture>,
    ) {
        if let Some(vector) = vectors.take() {
            Pipeline::resolve(context, &vector, target);
            self.scratch.put(vector);
        }
    }

    fn paint_parameters(
        &self,
        paint: &Paint,
        frame: FrameView,
        scale: f32,
        region: Region,
        opacity: f64,
        gap: Option<DrawRect>,
    ) -> Parameters {
        let zoom = frame.camera.zoom * f64::from(scale);
        let origin = canvas_to_view(
            Point::new(paint.transform[4], paint.transform[5]),
            &frame.camera,
            frame.surface,
        );
        let mut parameters = Parameters::for_target(region.size());
        parameters.matrix = [
            paint.transform[0],
            paint.transform[1],
            paint.transform[2],
            paint.transform[3],
        ]
        .map(|value| (value * zoom) as f32);
        parameters.translation = [
            (origin.x * f64::from(scale) - f64::from(region.x)) as f32,
            (origin.y * f64::from(scale) - f64::from(region.y)) as f32,
            opacity as f32,
            match paint.fill_style {
                DisplayFillStyle::Line => 1.0,
                DisplayFillStyle::CrossLine => 2.0,
                _ => 0.0,
            },
        ];
        parameters.color = rgba(paint.color);
        if let Some(gap) = gap {
            let top = canvas_to_view(
                Point::new(gap.min_x, gap.min_y),
                &frame.camera,
                frame.surface,
            );
            let bottom = canvas_to_view(
                Point::new(gap.max_x, gap.max_y),
                &frame.camera,
                frame.surface,
            );
            parameters.gap = [
                (top.x * f64::from(scale) - region.x as f64) as f32,
                (top.y * f64::from(scale) - region.y as f64) as f32,
                (bottom.x * f64::from(scale) - region.x as f64) as f32,
                (bottom.y * f64::from(scale) - region.y as f64) as f32,
            ];
        }
        parameters
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_mesh(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &Texture,
        paint: &Paint,
        frame: FrameView,
        scale: f32,
        region: Region,
        opacity: f64,
        gap: Option<DrawRect>,
    ) -> Result<()> {
        if !intersects(paint_bounds(paint, frame, scale), region.bounds()) {
            return Ok(());
        }
        if paint.color_glyph.is_some() {
            return self.paint_color_glyph(
                device, context, target, paint, frame, scale, region, opacity, gap,
            );
        }
        let parameters = self.paint_parameters(paint, frame, scale, region, opacity, gap);
        let pipeline = self.pipeline.as_mut().context("Canvas pipeline missing")?;
        if let Some(mesh) = pipeline.upload_mesh(device, &paint.mesh)? {
            pipeline.mesh(context, target, &mesh, &parameters, None);
            self.profile.submitted_draws += 1;
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_color_glyph(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &Texture,
        paint: &Paint,
        frame: FrameView,
        scale: f32,
        region: Region,
        opacity: f64,
        gap: Option<DrawRect>,
    ) -> Result<()> {
        const PAGE: u32 = 1024;
        const CACHE_BYTES: usize = 32 * 1024 * 1024;
        let glyph = paint.color_glyph.as_ref().context("Missing color glyph")?;
        let [a, b, c, d, tx, ty] = paint.transform;
        let determinant = a * d - b * c;
        if determinant.abs() < f64::MIN_POSITIVE {
            return Ok(());
        }
        let raster_scale = a.hypot(b).max(c.hypot(d)) * frame.camera.zoom * f64::from(scale);
        // Quantize the em size, so floating-point rotation noise does not rerasterize glyphs.
        let raster_scale =
            (raster_scale * glyph.units_per_em * 64.0).ceil() / 64.0 / glyph.units_per_em;
        let full = crate::gpu_color_glyph::ColorGlyphRenderer::raster_metrics(glyph, raster_scale)?;
        let mut visible = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for [x, y] in [
            [region.x as f64, region.y as f64],
            [region.x as f64 + region.width as f64, region.y as f64],
            [region.x as f64, region.y as f64 + region.height as f64],
            [
                region.x as f64 + region.width as f64,
                region.y as f64 + region.height as f64,
            ],
        ] {
            let world = view_to_canvas(
                Point::new(x / f64::from(scale), y / f64::from(scale)),
                &frame.camera,
                frame.surface,
            );
            let x = world.x - tx;
            let y = world.y - ty;
            let x_local = (d * x - c * y) / determinant;
            let y_local = (a * y - b * x) / determinant;
            visible = union(visible, [x_local, y_local, x_local, y_local]);
        }
        let visible = intersection(expand(visible, 2.0 / raster_scale), full.bounds);
        if visible[2] <= visible[0] || visible[3] <= visible[1] {
            return Ok(());
        }
        let first = [
            ((visible[0] - full.bounds[0]) * raster_scale)
                .floor()
                .max(0.0) as u32
                / PAGE,
            ((full.bounds[3] - visible[3]) * raster_scale)
                .floor()
                .max(0.0) as u32
                / PAGE,
        ];
        let last = [
            (((visible[2] - full.bounds[0]) * raster_scale).ceil() as u32)
                .saturating_sub(1)
                .min(full.size[0] - 1)
                / PAGE,
            (((full.bounds[3] - visible[1]) * raster_scale).ceil() as u32)
                .saturating_sub(1)
                .min(full.size[1] - 1)
                / PAGE,
        ];
        if self.color_glyph_renderer.is_none() {
            let allocation = self.memory.reserve(8 * 1024 * 1024)?;
            self.color_glyph_renderer = Some(ColorGlyphState {
                renderer: crate::gpu_color_glyph::ColorGlyphRenderer::new(device)?,
                allocation,
            });
        }
        for page_y in first[1]..=last[1] {
            for page_x in first[0]..=last[0] {
                let key = ColorGlyphKey {
                    font: glyph.font_id,
                    glyph: glyph.glyph_id,
                    scale: raster_scale.to_bits(),
                    foreground: [paint.color.r, paint.color.g, paint.color.b, 255],
                    page: [page_x, page_y],
                };
                if !self.color_glyphs.contains_key(&key) {
                    let core = [
                        page_x * PAGE,
                        page_y * PAGE,
                        (page_x * PAGE + PAGE).min(full.size[0]),
                        (page_y * PAGE + PAGE).min(full.size[1]),
                    ];
                    let padded = [
                        core[0].saturating_sub(2),
                        core[1].saturating_sub(2),
                        (core[2] + 2).min(full.size[0]),
                        (core[3] + 2).min(full.size[1]),
                    ];
                    let size = [padded[2] - padded[0], padded[3] - padded[1]];
                    let required = size[0] as usize * size[1] as usize * 4;
                    while self.color_glyph_bytes + required > CACHE_BYTES
                        || self.memory.used().saturating_add(required) > self.memory.limit()
                    {
                        let Some(old) = self
                            .color_glyphs
                            .iter()
                            .min_by_key(|(_, page)| page.last_use)
                            .map(|(key, _)| *key)
                        else {
                            break;
                        };
                        self.color_glyph_bytes -=
                            self.color_glyphs.remove(&old).unwrap().texture.bytes();
                    }
                    let bounds = [
                        full.bounds[0] + f64::from(padded[0]) / raster_scale,
                        full.bounds[3] - f64::from(padded[3]) / raster_scale,
                        full.bounds[0] + f64::from(padded[2]) / raster_scale,
                        full.bounds[3] - f64::from(padded[1]) / raster_scale,
                    ];
                    let metrics = crate::gpu_color_glyph::GlyphRasterMetrics {
                        size,
                        bounds,
                        baseline: [
                            full.baseline[0] - padded[0] as f32,
                            full.baseline[1] - padded[1] as f32,
                        ],
                        em_size: full.em_size,
                    };
                    let texture = Texture::bgra(device, size, &self.memory)?;
                    Pipeline::unbind(context);
                    texture.allocation.mark_gpu_use();
                    let rasterizer = self.color_glyph_renderer.as_mut().unwrap();
                    rasterizer.allocation.mark_gpu_use();
                    rasterizer
                        .renderer
                        .paint(&texture.image, glyph, &metrics, paint.color)?;
                    self.profile.color_glyph_raster_pages += 1;
                    let left = full.bounds[0] + f64::from(core[0]) / raster_scale;
                    let right = full.bounds[0] + f64::from(core[2]) / raster_scale;
                    let bottom = full.bounds[3] - f64::from(core[3]) / raster_scale;
                    let top = full.bounds[3] - f64::from(core[1]) / raster_scale;
                    self.color_glyph_bytes += texture.bytes();
                    self.color_glyphs.insert(
                        key,
                        ColorGlyphPage {
                            texture,
                            core: [left, bottom, right, top],
                            bounds,
                            last_use: self.sequence,
                        },
                    );
                }
                let page = self.color_glyphs.get_mut(&key).unwrap();
                page.last_use = self.sequence;
                let texture = page.texture.clone();
                let core = page.core;
                let bounds = page.bounds;
                let mut parameters =
                    self.paint_parameters(paint, frame, scale, region, opacity, gap);
                parameters.translation[3] = if paint.glyph_tint { -2.0 } else { -1.0 };
                if !paint.glyph_tint {
                    parameters.translation[2] *= f32::from(paint.color.a) / 255.0;
                }
                parameters.world = core.map(|value| value as f32);
                parameters.sample_mapping = [
                    (1.0 / (bounds[2] - bounds[0])) as f32,
                    (-1.0 / (bounds[3] - bounds[1])) as f32,
                    (-bounds[0] / (bounds[2] - bounds[0])) as f32,
                    (bounds[3] / (bounds[3] - bounds[1])) as f32,
                ];
                let pipeline = self.pipeline.as_mut().context("Canvas pipeline missing")?;
                if let Some(mesh) = pipeline.upload_mesh(device, &paint.mesh)? {
                    pipeline.textured_mesh(context, target, &mesh, &texture, &parameters);
                    self.profile.submitted_draws += 1;
                }
            }
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_item(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &mut Texture,
        item: &ItemGeometry,
        frame: FrameView,
        scale: f32,
        region: Region,
    ) -> Result<()> {
        if item.parts.is_empty() || item.opacity <= 0.0 {
            return Ok(());
        }
        if target.samples > 1
            && item.opacity == 1.0
            && item.blend == DisplayBlendMode::Normal
            && item.parts.iter().all(|part| {
                part.coverage_group == 0
                    && !part.is_text
                    && part.rough_style == StrokeRenderingStyle::Architect
            })
        {
            for part in &item.parts {
                self.paint_mesh(
                    device,
                    context,
                    target,
                    part,
                    frame,
                    scale,
                    region,
                    1.0,
                    item.label_gap,
                )?;
            }
            return Ok(());
        }
        let zoom = frame.camera.zoom * f64::from(scale);
        let mut bounds = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for part in &item.parts {
            bounds = union(bounds, paint_bounds(part, frame, scale));
        }
        let bounds = intersection(bounds, region.bounds());
        if bounds[2] <= bounds[0] || bounds[3] <= bounds[1] {
            return Ok(());
        }
        let is_text = item.parts.iter().any(|part| part.is_text);
        // Preserve the device-pixel origin and scale, adding transparent
        // right/bottom padding only. Exact clipped glyph sizes otherwise churn
        // scratch allocations that cannot retire until this frame completes.
        let quantum = if is_text { 16 } else { 1 };
        let group = Region {
            x: bounds[0].floor() as i32,
            y: bounds[1].floor() as i32,
            width: ((bounds[2].ceil() - bounds[0].floor()) as u32).div_ceil(quantum) * quantum,
            height: ((bounds[3].ceil() - bounds[1].floor()) as u32).div_ceil(quantum) * quantum,
        };
        // Small glyphs receive sixteen spatial coverage samples (2x2 supersampling
        // with four hardware samples); the shared shaping positions stay unchanged.
        let raster_factor = if is_text { 2 } else { 1 };
        let raster_region = Region {
            x: group.x * raster_factor,
            y: group.y * raster_factor,
            width: group.width * raster_factor as u32,
            height: group.height * raster_factor as u32,
        };
        let raster_scale = scale * raster_factor as f32;
        let multisample = self
            .scratch
            .take(device, raster_region.size(), 4, &self.memory)?;
        let resolved = self
            .scratch
            .take(device, raster_region.size(), 1, &self.memory)?;
        Pipeline::clear(context, &multisample, [0.0; 4]);
        let mut index = 0;
        while index < item.parts.len() {
            let part = &item.parts[index];
            let end = if part.coverage_group == 0 {
                index + 1
            } else {
                (index + 1..item.parts.len())
                    .find(|&next| item.parts[next].coverage_group != part.coverage_group)
                    .unwrap_or(item.parts.len())
            };
            if part.coverage_group == 0 && part.rough_style == StrokeRenderingStyle::Architect {
                self.paint_mesh(
                    device,
                    context,
                    &multisample,
                    part,
                    frame,
                    raster_scale,
                    raster_region,
                    1.0,
                    None,
                )?;
            } else {
                // A text line can be outside this tile even when another line
                // makes the item visible. Match the per-member culling below
                // before allocating and compositing an empty coverage surface.
                if !item.parts[index..end].iter().any(|member| {
                    intersects(
                        paint_bounds(member, frame, raster_scale),
                        raster_region.bounds(),
                    )
                }) {
                    index = end;
                    continue;
                }
                let coverage = self
                    .scratch
                    .take(device, raster_region.size(), 4, &self.memory)?;
                let coverage_resolved =
                    self.scratch
                        .take(device, raster_region.size(), 1, &self.memory)?;
                Pipeline::clear(context, &coverage, [0.0; 4]);
                for member in &item.parts[index..end] {
                    if !intersects(
                        paint_bounds(member, frame, raster_scale),
                        raster_region.bounds(),
                    ) {
                        continue;
                    }
                    let mut parameters = self.paint_parameters(
                        member,
                        frame,
                        raster_scale,
                        raster_region,
                        1.0,
                        None,
                    );
                    if part.coverage_group != 0 {
                        parameters.color = [1.0; 4];
                        parameters.target[3] = 1.0;
                    }
                    let pipeline = self.pipeline.as_mut().context("Pipeline missing")?;
                    if let Some(mesh) = pipeline.upload_mesh(device, &member.mesh)? {
                        pipeline.mesh(context, &coverage, &mesh, &parameters, None);
                        self.profile.submitted_draws += 1;
                    }
                }
                Pipeline::resolve(context, &coverage, &coverage_resolved);
                let mut parameters = self.effect_parameters(frame, raster_scale, raster_region);
                parameters.color = rgba(part.color);
                parameters.effect = [
                    if part.coverage_group != 0 { 14.0 } else { 0.0 },
                    0.0,
                    0.0,
                    1.0,
                ];
                if part.rough_style != StrokeRenderingStyle::Architect {
                    let displaced =
                        self.scratch
                            .take(device, raster_region.size(), 1, &self.memory)?;
                    let mut rough_parameters = parameters;
                    rough_parameters.effect = [
                        12.0,
                        part.rough_seed as f32,
                        (snow_draw_engine_core::arrow::stroke_rendering_outset(
                            part.rough_style,
                            part.rough_scale,
                        ) * 2.0
                            * zoom
                            * raster_factor as f64) as f32,
                        1.0,
                    ];
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        &displaced,
                        [Some(&coverage_resolved), None, None],
                        &rough_parameters,
                        false,
                        None,
                    );
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        &multisample,
                        [Some(&displaced), None, None],
                        &parameters,
                        true,
                        None,
                    );
                    self.scratch.put(displaced);
                } else {
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        &multisample,
                        [Some(&coverage_resolved), None, None],
                        &parameters,
                        true,
                        None,
                    );
                }
                self.scratch.put(coverage);
                self.scratch.put(coverage_resolved);
            }
            index = end;
        }
        Pipeline::resolve(context, &multisample, &resolved);
        let mut parameters = Parameters::for_target(region.size());
        parameters.sample_mapping = [
            1.0 / group.width as f32,
            1.0 / group.height as f32,
            (region.x - group.x) as f32 / group.width as f32,
            (region.y - group.y) as f32 / group.height as f32,
        ];
        parameters.effect[3] = item.opacity as f32;
        // Clip the completed item after rough displacement. Clipping the source
        // coverage first lets displaced ink and outlines leak back into labels.
        parameters.gap = self
            .paint_parameters(&item.parts[0], frame, scale, region, 1.0, item.label_gap)
            .gap;
        let scissor = Some(local_scissor(group.bounds(), region));
        if item.blend == DisplayBlendMode::Multiply {
            let output = self.scratch.take(device, region.size(), 1, &self.memory)?;
            Pipeline::copy(context, target, &output);
            parameters.effect[0] = 1.0;
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                &output,
                [Some(&resolved), None, Some(target)],
                &parameters,
                false,
                scissor,
            );
            self.scratch.put(std::mem::replace(target, output));
        } else {
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                target,
                [Some(&resolved), None, None],
                &parameters,
                true,
                scissor,
            );
        }
        self.scratch.put(multisample);
        self.scratch.put(resolved);
        Ok(())
    }

    fn effect_parameters(&self, frame: FrameView, scale: f32, region: Region) -> Parameters {
        let world = view_to_canvas(
            Point::new(
                region.x as f64 / f64::from(scale),
                region.y as f64 / f64::from(scale),
            ),
            &frame.camera,
            frame.surface,
        );
        let mut parameters = Parameters::for_target(region.size());
        parameters.world = [
            (1.0 / (frame.camera.zoom * f64::from(scale))) as f32,
            world.x as f32,
            world.y as f32,
            0.0,
        ];
        parameters
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_source(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &Texture,
        source: Option<&SourceImage>,
        frame: FrameView,
        scale: f32,
        region: Region,
        opacity: f64,
        mapping: Option<([f64; 2], [f64; 2], [f64; 2])>,
    ) -> Result<()> {
        let Some(source) = source else {
            return Ok(());
        };
        let zoom = frame.camera.zoom * f64::from(scale);
        let [width, height] = source.dimensions();
        let (destination_center, source_center, factor) =
            mapping.unwrap_or(([0.0, 0.0], [0.0, 0.0], [1.0, 1.0]));
        if factor
            .iter()
            .any(|factor| !factor.is_finite() || *factor <= 0.0)
        {
            return Ok(());
        }
        if let Some(layers) = source.layers() {
            for layer in layers {
                let placement = layer.placement();
                let coverage = layer.coverage.unwrap_or(placement);
                let destination = [
                    destination_center[0] + (placement.x - source_center[0]) * factor[0],
                    destination_center[1] + (placement.y - source_center[1]) * factor[1],
                ];
                let source_origin = layer.image.origin();
                let factors = [
                    factor[0] * placement.width
                        / (layer.image.width() as f64 * layer.image.units_per_pixel()),
                    factor[1] * placement.height
                        / (layer.image.height() as f64 * layer.image.units_per_pixel()),
                ];
                let a = canvas_to_view(
                    Point::new(
                        destination_center[0] + (coverage.x - source_center[0]) * factor[0],
                        destination_center[1] + (coverage.y - source_center[1]) * factor[1],
                    ),
                    &frame.camera,
                    frame.surface,
                );
                let b = canvas_to_view(
                    Point::new(
                        destination_center[0]
                            + (coverage.x + coverage.width - source_center[0]) * factor[0],
                        destination_center[1]
                            + (coverage.y + coverage.height - source_center[1]) * factor[1],
                    ),
                    &frame.camera,
                    frame.surface,
                );
                let bounds = [
                    a.x * f64::from(scale),
                    a.y * f64::from(scale),
                    b.x * f64::from(scale),
                    b.y * f64::from(scale),
                ];
                if !intersects(bounds, region.bounds()) {
                    continue;
                }
                let image = self.scratch.take(device, region.size(), 1, &self.memory)?;
                Pipeline::clear(context, &image, [0.0; 4]);
                self.paint_source(
                    device,
                    context,
                    &image,
                    Some(&layer.image),
                    frame,
                    scale,
                    region,
                    opacity,
                    Some((destination, [source_origin.x, source_origin.y], factors)),
                )?;
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    target,
                    [Some(&image), None, None],
                    &Parameters::for_target(region.size()),
                    true,
                    Some(clip_scissor(bounds, region)),
                );
                self.scratch.put(image);
            }
            return Ok(());
        }
        if source.gpu_texture().is_some() {
            return self.paint_gpu_source(
                device,
                context,
                target,
                source,
                frame,
                scale,
                region,
                opacity,
                (destination_center, source_center, factor),
            );
        }
        let pixels = source
            .raster_pixels()
            .context("Expected immutable raster source")?;
        let world_origin = view_to_canvas(
            Point::new(
                region.x as f64 / f64::from(scale),
                region.y as f64 / f64::from(scale),
            ),
            &frame.camera,
            frame.surface,
        );
        let mapped_origin = [
            (world_origin.x - destination_center[0]) / factor[0] + source_center[0],
            (world_origin.y - destination_center[1]) / factor[1] + source_center[1],
        ];
        let source_origin = source.origin();
        let units = source.units_per_pixel();
        let first = [
            ((mapped_origin[0] - source_origin.x) / units)
                .floor()
                .max(0.0) as u32,
            ((mapped_origin[1] - source_origin.y) / units)
                .floor()
                .max(0.0) as u32,
        ];
        let last = [
            ((mapped_origin[0] + region.width as f64 / zoom / factor[0] - source_origin.x) / units)
                .ceil()
                .clamp(0.0, width as f64) as u32,
            ((mapped_origin[1] + region.height as f64 / zoom / factor[1] - source_origin.y) / units)
                .ceil()
                .clamp(0.0, height as f64) as u32,
        ];
        if first[0] >= last[0] || first[1] >= last[1] {
            return Ok(());
        }
        // Cache tiles in the selected mip grid, not in the full-resolution
        // image. A tiny view of an 8 GiB source therefore retains only the
        // pixels needed for that view. Reduction is entirely premultiplied GPU
        // box filtering; raw uploads never require a CPU crop or downsample.
        const PAGE: u32 = 512;
        let lod = factor.map(|factor| source_lod(zoom * units * factor));
        let step = lod.map(|lod| 1_u32 << lod);
        let span = step.map(|step| u64::from(PAGE) * u64::from(step));
        let pixel_origin = [
            (mapped_origin[0] - source_origin.x) / units,
            (mapped_origin[1] - source_origin.y) / units,
        ];
        let image_start = canvas_to_view(
            Point::new(
                destination_center[0] + (source_origin.x - source_center[0]) * factor[0],
                destination_center[1] + (source_origin.y - source_center[1]) * factor[1],
            ),
            &frame.camera,
            frame.surface,
        );
        let image_end = canvas_to_view(
            Point::new(
                destination_center[0]
                    + (source_origin.x + width as f64 * units - source_center[0]) * factor[0],
                destination_center[1]
                    + (source_origin.y + height as f64 * units - source_center[1]) * factor[1],
            ),
            &frame.camera,
            frame.surface,
        );
        for page_y in u64::from(first[1]) / span[1]..=(u64::from(last[1]) - 1) / span[1] {
            for page_x in u64::from(first[0]) / span[0]..=(u64::from(last[0]) - 1) / span[0] {
                let key = (
                    source.pixels_identity(),
                    page_x as u32,
                    page_y as u32,
                    lod[0],
                    lod[1],
                );
                if !self.sources.contains_key(&key) {
                    let origin = [
                        (page_x * span[0]).saturating_sub(u64::from(step[0])) as u32,
                        (page_y * span[1]).saturating_sub(u64::from(step[1])) as u32,
                    ];
                    let end = [
                        ((page_x + 1) * span[0] + u64::from(step[0])).min(u64::from(width)) as u32,
                        ((page_y + 1) * span[1] + u64::from(step[1])).min(u64::from(height)) as u32,
                    ];
                    let rect = [origin[0], origin[1], end[0] - origin[0], end[1] - origin[1]];
                    self.evict_source_pages();
                    let texture = self.build_source_page(
                        device,
                        context,
                        pixels,
                        source.pixels_identity(),
                        rect,
                        step,
                    )?;
                    self.sources.insert(
                        key,
                        SourcePage {
                            texture,
                            rect,
                            last_use: self.sequence,
                        },
                    );
                }
                let page = self.sources.get_mut(&key).context("Source page missing")?;
                page.last_use = self.sequence;
                let texture = page.texture.clone();
                let rect = page.rect;
                let span_pixels = [
                    f64::from(texture.size[0]) * f64::from(step[0]),
                    f64::from(texture.size[1]) * f64::from(step[1]),
                ];
                let mut parameters = Parameters::for_target(region.size());
                parameters.sample_mapping = [
                    (1.0 / zoom / factor[0] / units / span_pixels[0]) as f32,
                    (1.0 / zoom / factor[1] / units / span_pixels[1]) as f32,
                    ((pixel_origin[0] - f64::from(rect[0])) / span_pixels[0]) as f32,
                    ((pixel_origin[1] - f64::from(rect[1])) / span_pixels[1]) as f32,
                ];
                parameters.effect = [17.0, 0.0, 0.0, opacity as f32];
                parameters.gap = [
                    (image_start.x * scale as f64 - region.x as f64) as f32,
                    (image_start.y * scale as f64 - region.y as f64) as f32,
                    (image_end.x * scale as f64 - region.x as f64) as f32,
                    (image_end.y * scale as f64 - region.y as f64) as f32,
                ];
                let first_world = [
                    destination_center[0]
                        + (source_origin.x + (page_x * span[0]) as f64 * units - source_center[0])
                            * factor[0],
                    destination_center[1]
                        + (source_origin.y + (page_y * span[1]) as f64 * units - source_center[1])
                            * factor[1],
                ];
                let last_world = [
                    destination_center[0]
                        + (source_origin.x
                            + ((page_x + 1) * span[0]).min(u64::from(width)) as f64 * units
                            - source_center[0])
                            * factor[0],
                    destination_center[1]
                        + (source_origin.y
                            + ((page_y + 1) * span[1]).min(u64::from(height)) as f64 * units
                            - source_center[1])
                            * factor[1],
                ];
                let start = canvas_to_view(
                    Point::new(first_world[0], first_world[1]),
                    &frame.camera,
                    frame.surface,
                );
                let end = canvas_to_view(
                    Point::new(last_world[0], last_world[1]),
                    &frame.camera,
                    frame.surface,
                );
                let mut scissor = local_scissor(
                    [
                        start.x * f64::from(scale),
                        start.y * f64::from(scale),
                        end.x * f64::from(scale),
                        end.y * f64::from(scale),
                    ],
                    region,
                );
                if page_x > 0 {
                    scissor[0] = ((start.x * scale as f64 - 0.5).ceil() - region.x as f64)
                        .clamp(0.0, region.width as f64) as i32;
                }
                if page_y > 0 {
                    scissor[1] = ((start.y * scale as f64 - 0.5).ceil() - region.y as f64)
                        .clamp(0.0, region.height as f64) as i32;
                }
                if (page_x + 1) * span[0] < u64::from(width) {
                    scissor[2] = ((end.x * scale as f64 - 0.5).ceil() - region.x as f64)
                        .clamp(0.0, region.width as f64) as i32;
                }
                if (page_y + 1) * span[1] < u64::from(height) {
                    scissor[3] = ((end.y * scale as f64 - 0.5).ceil() - region.y as f64)
                        .clamp(0.0, region.height as f64) as i32;
                }
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    target,
                    [Some(&texture), None, None],
                    &parameters,
                    true,
                    Some(scissor),
                );
            }
        }
        Ok(())
    }

    /// Stream a source region through bounded upload textures. Each block starts
    /// on the selected box-filter grid, so block and page boundaries are exact.
    fn build_source_page(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        pixels: &image::RgbaImage,
        source_id: u64,
        rect: [u32; 4],
        step: [u32; 2],
    ) -> Result<Texture> {
        let base_step = step.map(|step| step.min(512));
        let base_size = [
            rect[2].div_ceil(base_step[0]),
            rect[3].div_ceil(base_step[1]),
        ];
        if base_size.into_iter().all(|size| size <= 16384) {
            return self.build_source_region(device, context, pixels, source_id, rect, step);
        }
        // Extremely wide/short images can exceed the native texture axis limit
        // even after nine reductions. Build aligned final-mip stripes instead.
        let size = [rect[2].div_ceil(step[0]), rect[3].div_ceil(step[1])];
        let stripe = [
            16384_u64 * u64::from(base_step[0]) / u64::from(step[0]),
            16384_u64 * u64::from(base_step[1]) / u64::from(step[1]),
        ];
        ensure!(
            stripe.into_iter().all(|size| size > 0),
            "Source minification exceeds the native texture support"
        );
        let stripe = stripe.map(|size| size as u32);
        let target = Texture::new(device, size, &self.memory)?;
        for y in (0..size[1]).step_by(stripe[1] as usize) {
            for x in (0..size[0]).step_by(stripe[0] as usize) {
                let offset = [x * step[0], y * step[1]];
                let extent = [
                    (u64::from(stripe[0]) * u64::from(step[0])).min(u64::from(rect[2] - offset[0]))
                        as u32,
                    (u64::from(stripe[1]) * u64::from(step[1])).min(u64::from(rect[3] - offset[1]))
                        as u32,
                ];
                let part = self.build_source_region(
                    device,
                    context,
                    pixels,
                    source_id,
                    [
                        rect[0] + offset[0],
                        rect[1] + offset[1],
                        extent[0],
                        extent[1],
                    ],
                    step,
                )?;
                Pipeline::unbind(context);
                target.allocation.mark_gpu_use();
                part.allocation.mark_gpu_use();
                unsafe {
                    context.CopySubresourceRegion(&target.image, 0, x, y, 0, &part.image, 0, None);
                }
                self.scratch.put(part);
            }
        }
        Ok(target)
    }

    fn build_source_region(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        pixels: &image::RgbaImage,
        source_id: u64,
        rect: [u32; 4],
        step: [u32; 2],
    ) -> Result<Texture> {
        const BLOCK: u32 = 512;
        let base_step = step.map(|step| step.min(BLOCK));
        let base_size = [
            rect[2].div_ceil(base_step[0]),
            rect[3].div_ceil(base_step[1]),
        ];
        let assembled = Texture::new(device, base_size, &self.memory)?;
        for y in (0..rect[3]).step_by(BLOCK as usize) {
            for x in (0..rect[2]).step_by(BLOCK as usize) {
                let size = [BLOCK.min(rect[2] - x), BLOCK.min(rect[3] - y)];
                let converted = self.source_block(
                    device,
                    context,
                    pixels,
                    source_id,
                    [rect[0] + x, rect[1] + y, size[0], size[1]],
                )?;
                let reduced = self.reduce_source(device, context, converted, base_step)?;
                Pipeline::unbind(context);
                assembled.allocation.mark_gpu_use();
                reduced.allocation.mark_gpu_use();
                unsafe {
                    context.CopySubresourceRegion(
                        &assembled.image,
                        0,
                        x / base_step[0],
                        y / base_step[1],
                        0,
                        &reduced.image,
                        0,
                        None,
                    );
                }
                self.scratch.put(reduced);
            }
        }
        self.reduce_source(
            device,
            context,
            assembled,
            [step[0] / base_step[0], step[1] / base_step[1]],
        )
    }

    /// Resident full-resolution pages are already premultiplied. Assemble each
    /// bounded reduction block from them, uploading only missing subregions.
    fn source_block(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        pixels: &image::RgbaImage,
        source_id: u64,
        rect: [u32; 4],
    ) -> Result<Texture> {
        const PAGE: u32 = 512;
        let first = [rect[0] / PAGE, rect[1] / PAGE];
        let last = [
            (rect[0] + rect[2] - 1) / PAGE,
            (rect[1] + rect[3] - 1) / PAGE,
        ];
        let any_resident = (first[1]..=last[1]).any(|y| {
            (first[0]..=last[0]).any(|x| self.sources.contains_key(&(source_id, x, y, 0, 0)))
        });
        if !any_resident {
            return self.upload_source_block(device, context, pixels, rect);
        }
        let target = self
            .scratch
            .take(device, [rect[2], rect[3]], 1, &self.memory)?;
        for y in first[1]..=last[1] {
            for x in first[0]..=last[0] {
                let origin = [rect[0].max(x * PAGE), rect[1].max(y * PAGE)];
                let end = [
                    (rect[0] + rect[2]).min((x + 1) * PAGE),
                    (rect[1] + rect[3]).min((y + 1) * PAGE),
                ];
                let extent = [end[0] - origin[0], end[1] - origin[1]];
                let (texture, offset, transient) =
                    if let Some(page) = self.sources.get_mut(&(source_id, x, y, 0, 0)) {
                        page.last_use = self.sequence;
                        (
                            page.texture.clone(),
                            [origin[0] - page.rect[0], origin[1] - page.rect[1]],
                            false,
                        )
                    } else {
                        (
                            self.upload_source_block(
                                device,
                                context,
                                pixels,
                                [origin[0], origin[1], extent[0], extent[1]],
                            )?,
                            [0, 0],
                            true,
                        )
                    };
                let source_box = D3D11_BOX {
                    left: offset[0],
                    top: offset[1],
                    front: 0,
                    right: offset[0] + extent[0],
                    bottom: offset[1] + extent[1],
                    back: 1,
                };
                Pipeline::unbind(context);
                target.allocation.mark_gpu_use();
                texture.allocation.mark_gpu_use();
                unsafe {
                    context.CopySubresourceRegion(
                        &target.image,
                        0,
                        origin[0] - rect[0],
                        origin[1] - rect[1],
                        0,
                        &texture.image,
                        0,
                        Some(&source_box),
                    );
                }
                if transient {
                    self.scratch.put(texture);
                }
            }
        }
        Ok(target)
    }

    fn upload_source_block(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        pixels: &image::RgbaImage,
        rect: [u32; 4],
    ) -> Result<Texture> {
        let size = [rect[2], rect[3]];
        let upload = self.scratch.take(device, size, 1, &self.memory)?;
        let converted = self.scratch.take(device, size, 1, &self.memory)?;
        let offset = (rect[1] as usize * pixels.width() as usize + rect[0] as usize) * 4;
        upload.allocation.mark_gpu_use();
        unsafe {
            context.UpdateSubresource(
                &upload.image,
                0,
                None,
                pixels.as_raw().as_ptr().add(offset).cast(),
                pixels.width() * 4,
                0,
            );
        }
        let uploaded = u64::from(size[0]) * u64::from(size[1]) * 4;
        self.profile.uploaded_bytes += uploaded;
        self.profile.source_upload_bytes += uploaded;
        let mut parameters = Parameters::for_target(size);
        parameters.effect[0] = 10.0;
        self.pipeline.as_ref().context("Pipeline missing")?.effect(
            context,
            &converted,
            [Some(&upload), None, None],
            &parameters,
            false,
            None,
        );
        self.scratch.put(upload);
        Ok(converted)
    }

    fn reduce_source(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        mut source: Texture,
        mut step: [u32; 2],
    ) -> Result<Texture> {
        while step != [1, 1] {
            let factor = step.map(|step| if step > 1 { 2 } else { 1 });
            let size = [
                source.size[0].div_ceil(factor[0]),
                source.size[1].div_ceil(factor[1]),
            ];
            let target = self.scratch.take(device, size, 1, &self.memory)?;
            let mut parameters = Parameters::for_target(size);
            // Explicit 2:1 mapping avoids stretching odd final blocks. Clamp
            // supplies the image-edge texel where a box extends past the source.
            parameters.sample_mapping = [
                factor[0] as f32 / source.size[0] as f32,
                factor[1] as f32 / source.size[1] as f32,
                0.0,
                0.0,
            ];
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                &target,
                [Some(&source), None, None],
                &parameters,
                false,
                None,
            );
            self.scratch.put(source);
            source = target;
            step = [step[0] / factor[0], step[1] / factor[1]];
        }
        Ok(source)
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_filter_run(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &mut Texture,
        entering: &Texture,
        filters: &[FilterDisplayItem],
        background: Option<&SourceImage>,
        frame: FrameView,
        scale: f32,
        region: Region,
        smart: &SmartEraseSnapshot,
    ) -> Result<()> {
        let Some(first) = filters.first() else {
            return Ok(());
        };
        if first.filter.filter_type == DisplayFilterType::SmartErase {
            for filter in filters {
                let mask_msaa = self.scratch.take(device, region.size(), 4, &self.memory)?;
                let mask = self.scratch.take(device, region.size(), 1, &self.memory)?;
                let patch_texture = self.scratch.take(device, region.size(), 1, &self.memory)?;
                Pipeline::clear(context, &mask_msaa, [0.0; 4]);
                let mut normalized = filter.clone();
                normalized.opacity = 1.0;
                let paint = self
                    .geometry
                    .filter_mask(&normalized, frame.camera.zoom * f64::from(scale))?;
                self.paint_mesh(
                    device, context, &mask_msaa, &paint, frame, scale, region, 1.0, None,
                )?;
                Pipeline::resolve(context, &mask_msaa, &mask);
                let mut parameters = Parameters::for_target(region.size());
                parameters.effect = [13.0, 0.0, 0.0, 1.0];
                if let Some(patch) = background
                    .and_then(|source| smart.patch(filter, source))
                    .filter(|patch| patch.supports_device(&self.service))
                {
                    Pipeline::clear(context, &patch_texture, [0.0; 4]);
                    self.paint_source(
                        device,
                        context,
                        &patch_texture,
                        Some(patch.original()),
                        frame,
                        scale,
                        region,
                        1.0,
                        None,
                    )?;
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        target,
                        [Some(&patch_texture), Some(&mask), None],
                        &parameters,
                        true,
                        None,
                    );
                    Pipeline::clear(context, &patch_texture, [0.0; 4]);
                    self.paint_source(
                        device,
                        context,
                        &patch_texture,
                        Some(patch.reconstructed()),
                        frame,
                        scale,
                        region,
                        1.0,
                        None,
                    )?;
                    parameters.effect[3] = filter.opacity as f32;
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        target,
                        [Some(&patch_texture), Some(&mask), None],
                        &parameters,
                        true,
                        None,
                    );
                } else if background.is_some() || filter.filter.render_phase != 0 {
                    Pipeline::clear(
                        context,
                        &patch_texture,
                        premultiply([220.0 / 255.0, 66.0 / 255.0, 66.0 / 255.0, 54.0 / 255.0]),
                    );
                    self.pipeline.as_ref().context("Pipeline missing")?.effect(
                        context,
                        target,
                        [Some(&patch_texture), Some(&mask), None],
                        &parameters,
                        true,
                        None,
                    );
                }
                self.scratch.put(mask_msaa);
                self.scratch.put(mask);
                self.scratch.put(patch_texture);
            }
            return Ok(());
        }
        let mask_multisample = self.scratch.take(device, region.size(), 4, &self.memory)?;
        let mask = self.scratch.take(device, region.size(), 1, &self.memory)?;
        Pipeline::clear(context, &mask_multisample, [0.0; 4]);
        for filter in filters {
            let paint = self
                .geometry
                .filter_mask(filter, frame.camera.zoom * f64::from(scale))?;
            self.paint_mesh(
                device,
                context,
                &mask_multisample,
                &paint,
                frame,
                scale,
                region,
                filter.opacity,
                None,
            )?;
        }
        Pipeline::resolve(context, &mask_multisample, &mask);
        self.scratch.put(mask_multisample);
        let filtered = self.scratch.take(device, region.size(), 1, &self.memory)?;
        let mut parameters = self.effect_parameters(frame, scale, region);
        let zoom = frame.camera.zoom * f64::from(scale);
        match first.filter.filter_type {
            DisplayFilterType::RestoreBackground => {
                Pipeline::clear(context, &filtered, premultiply(rgba(frame.clear_color)));
                self.paint_source(
                    device, context, &filtered, background, frame, scale, region, 1.0, None,
                )?;
            }
            DisplayFilterType::GaussianBlur => {
                let intermediate = self.scratch.take(device, region.size(), 1, &self.memory)?;
                parameters.effect = [
                    3.0,
                    (first.filter.blur_sigma * zoom) as f32,
                    (3.0 * first.filter.blur_sigma * zoom).ceil() as f32,
                    1.0,
                ];
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    &intermediate,
                    [Some(entering), None, None],
                    &parameters,
                    false,
                    None,
                );
                parameters.effect[0] = 4.0;
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    &filtered,
                    [Some(&intermediate), None, None],
                    &parameters,
                    false,
                    None,
                );
                self.scratch.put(intermediate);
            }
            kind => {
                let operation = match kind {
                    DisplayFilterType::Mosaic => 5.0,
                    DisplayFilterType::Grayscale => 6.0,
                    DisplayFilterType::Inversion => 7.0,
                    DisplayFilterType::Brightness => 8.0,
                    DisplayFilterType::Emboss => 9.0,
                    _ => unreachable!(),
                };
                parameters.effect = [
                    operation,
                    first.filter.strength as f32,
                    if kind == DisplayFilterType::Mosaic {
                        first.filter.mosaic_block_size as f32
                    } else {
                        zoom.round().max(1.0) as f32
                    },
                    1.0,
                ];
                if kind == DisplayFilterType::Mosaic {
                    let origin = view_to_canvas(
                        Point::new(
                            region.x as f64 / scale as f64,
                            region.y as f64 / scale as f64,
                        ),
                        &frame.camera,
                        frame.surface,
                    );
                    parameters.world[1] =
                        origin.x.rem_euclid(first.filter.mosaic_block_size) as f32;
                    parameters.world[2] =
                        origin.y.rem_euclid(first.filter.mosaic_block_size) as f32;
                }
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    &filtered,
                    [Some(entering), None, None],
                    &parameters,
                    false,
                    None,
                );
            }
        }
        let output = self.scratch.take(device, region.size(), 1, &self.memory)?;
        parameters.effect = [2.0, 0.0, 0.0, 1.0];
        self.pipeline.as_ref().context("Pipeline missing")?.effect(
            context,
            &output,
            [Some(&filtered), Some(&mask), Some(target)],
            &parameters,
            false,
            None,
        );
        self.scratch.put(std::mem::replace(target, output));
        self.scratch.put(mask);
        self.scratch.put(filtered);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_spotlight(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &Texture,
        scene: &RetainedScene,
        frame: FrameView,
        scale: f32,
        region: Region,
        presentation: &CanvasPresentation,
    ) -> Result<()> {
        let spotlight = scene.decoration().spotlight;
        if !spotlight.active {
            return Ok(());
        }
        let mask_msaa = self.scratch.take(device, region.size(), 4, &self.memory)?;
        let mask = self.scratch.take(device, region.size(), 1, &self.memory)?;
        Pipeline::clear(context, &mask_msaa, [0.0; 4]);
        for cutout in scene.spotlights() {
            let shape = RectangleDisplayItem {
                center_x: cutout.center_x,
                center_y: cutout.center_y,
                width: cutout.width,
                height: cutout.height,
                rotation: cutout.rotation,
                corner_radii: cutout.corner_radii,
                shape: cutout.shape,
                ..Default::default()
            };
            let paint = self
                .geometry
                .rectangle_mask(&shape, frame.camera.zoom * f64::from(scale))?;
            self.paint_mesh(
                device, context, &mask_msaa, &paint, frame, scale, region, 1.0, None,
            )?;
        }
        Pipeline::resolve(context, &mask_msaa, &mask);
        let mut parameters = Parameters::for_target(region.size());
        parameters.color = rgba(spotlight.color);
        parameters.effect = [11.0, 0.0, 0.0, spotlight.opacity as f32];
        let scissor = presentation.spotlight_area.map(|area| {
            clip_scissor(
                pixel_rect(
                    [area.x, area.y, area.x + area.width, area.y + area.height],
                    frame,
                    scale,
                ),
                region,
            )
        });
        self.pipeline.as_ref().context("Pipeline missing")?.effect(
            context,
            target,
            [None, Some(&mask), None],
            &parameters,
            true,
            scissor,
        );
        self.scratch.put(mask_msaa);
        self.scratch.put(mask);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_watermark(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &mut Texture,
        scene: &RetainedScene,
        frame: FrameView,
        scale: f32,
        region: Region,
        presentation: &CanvasPresentation,
    ) -> Result<()> {
        let watermark = scene.decoration().watermark;
        let text = String::from_utf8_lossy(&watermark.text[..watermark.text_len as usize]);
        if text.is_empty() || watermark.opacity <= 0.0 {
            return Ok(());
        }
        let mut first = view_to_canvas(
            Point::new(
                region.x as f64 / f64::from(scale),
                region.y as f64 / f64::from(scale),
            ),
            &frame.camera,
            frame.surface,
        );
        let mut last = view_to_canvas(
            Point::new(
                (region.x as f64 + region.width as f64) / f64::from(scale),
                (region.y as f64 + region.height as f64) / f64::from(scale),
            ),
            &frame.camera,
            frame.surface,
        );
        // Generate only stamps that can touch the requested watermark area.
        // The lattice below stays anchored in canvas coordinates and retains
        // the text-width margin for rotated stamps crossing a clipped edge.
        // Painting the entire desktop then clipping wastes text geometry and
        // scratch allocations, which can exhaust retirement budget in one frame.
        if let Some(area) = presentation.watermark_area {
            first.x = first.x.max(area.x);
            first.y = first.y.max(area.y);
            last.x = last.x.min(area.x + area.width);
            last.y = last.y.min(area.y + area.height);
            if last.x <= first.x || last.y <= first.y {
                return Ok(());
            }
        }
        let gap = (watermark.gap + watermark.font_size * 4.0).max(32.0);
        let family =
            String::from_utf8_lossy(&watermark.font_family[..watermark.font_family_len as usize])
                .into_owned();
        let style = TextStyle {
            font_size: watermark.font_size,
            font_family: Some(family.clone()),
            color: watermark.color,
            bold: false,
            italic: false,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Border,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: Default::default(),
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: watermark.opacity,
        };
        let width = self.geometry.text.text_width(&text, &style);
        let mut clipped_layer = if presentation.watermark_area.is_some() {
            let texture = self.scratch.take(device, region.size(), 1, &self.memory)?;
            Pipeline::clear(context, &texture, [0.0; 4]);
            Some(texture)
        } else {
            None
        };
        let destination = clipped_layer.as_mut().unwrap_or(target);
        let angle = watermark.angle.to_radians();
        let (sin, cos) = angle.sin_cos();
        let height = watermark.font_size * 1.2;
        let local_center = [width / 2.0, height / 2.0 - watermark.font_size];
        let mut y = ((first.y - width) / gap).floor() * gap;
        let mut count = 0;
        while y < last.y + width + gap {
            let mut x = ((first.x - width) / gap).floor() * gap;
            while x < last.x + width + gap {
                let item = SceneDisplayItem::Text(TextDisplayItem {
                    center_x: x + cos * local_center[0] - sin * local_center[1],
                    center_y: y + sin * local_center[0] + cos * local_center[1],
                    width: width.max(1.0),
                    height,
                    rotation: angle,
                    vertical_align: DisplayTextVerticalAlign::Top,
                    text: text.to_string(),
                    font_family: Some(family.clone()),
                    font_size: watermark.font_size,
                    color: watermark.color,
                    opacity: watermark.opacity,
                    horizontal_align: DisplayTextHorizontalAlign::Left,
                    ..Default::default()
                });
                let geometry = self.geometry.item(
                    &item,
                    frame.camera.zoom * f64::from(scale),
                    frame.clear_color,
                )?;
                self.paint_item(
                    device,
                    context,
                    destination,
                    &geometry,
                    frame,
                    scale,
                    region,
                )?;
                count += 1;
                ensure!(count <= 4096, "Watermark density exceeds canvas limit");
                x += gap;
            }
            y += gap;
        }
        if let (Some(layer), Some(area)) = (clipped_layer, presentation.watermark_area) {
            let scissor = clip_scissor(
                pixel_rect(
                    [area.x, area.y, area.x + area.width, area.y + area.height],
                    frame,
                    scale,
                ),
                region,
            );
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                target,
                [Some(&layer), None, None],
                &Parameters::for_target(region.size()),
                true,
                Some(scissor),
            );
            self.scratch.put(layer);
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn paint_magnifier(
        &mut self,
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        target: &mut Texture,
        lens: &MagnifierDisplayItem,
        background: Option<&SourceImage>,
        frame: FrameView,
        scale: f32,
        region: Region,
    ) -> Result<()> {
        let mask_msaa = self.scratch.take(device, region.size(), 4, &self.memory)?;
        let mask = self.scratch.take(device, region.size(), 1, &self.memory)?;
        let mut sampled = self.scratch.take(device, region.size(), 1, &self.memory)?;
        Pipeline::clear(context, &mask_msaa, [0.0; 4]);
        Pipeline::clear(context, &sampled, [0.0; 4]);
        let body = &lens.lens;
        let paint = self
            .geometry
            .rectangle_mask(body, frame.camera.zoom * f64::from(scale))?;
        self.paint_mesh(
            device, context, &mask_msaa, &paint, frame, scale, region, 1.0, None,
        )?;
        Pipeline::resolve(context, &mask_msaa, &mask);
        let source_body = RectangleDisplayItem {
            center_x: lens.source_center.x,
            center_y: lens.source_center.y,
            width: lens.source_width,
            height: lens.source_height,
            corner_radii: body.corner_radii.scaled(1.0 / lens.factor),
            fill: ColorRgba8::default(),
            ..*body
        };
        if lens.connection_type == MagnifierConnectionType::Gradient && body.stroke.a > 0 {
            let source_paint = self
                .geometry
                .rectangle_mask(&source_body, frame.camera.zoom * f64::from(scale))?;
            let source_mask = self.scratch.take(device, region.size(), 1, &self.memory)?;
            Pipeline::clear(context, &mask_msaa, [0.0; 4]);
            self.paint_mesh(
                device,
                context,
                &mask_msaa,
                &source_paint,
                frame,
                scale,
                region,
                1.0,
                None,
            )?;
            Pipeline::resolve(context, &mask_msaa, &source_mask);
            let mut points = Vec::new();
            for shape in [&paint, &source_paint] {
                let [xx, yx, xy, yy, tx, ty] = shape.transform;
                points.extend(shape.mesh.vertices.iter().map(|[x, y]| {
                    [
                        xx * (*x as f64) + xy * (*y as f64) + tx - body.center_x,
                        yx * (*x as f64) + yy * (*y as f64) + ty - body.center_y,
                    ]
                }));
            }
            let hull = convex_hull(points);
            if hull.len() >= 3 {
                let indices = (1..hull.len() - 1)
                    .flat_map(|i| [0, i as u32, i as u32 + 1])
                    .collect();
                let mesh = crate::gpu_geometry::Mesh::from_vertices(
                    hull.into_iter().map(|p| p.map(|v| v as f32)).collect(),
                    indices,
                )?;
                let hull_paint = Paint {
                    mesh: Arc::new(mesh),
                    transform: [1.0, 0.0, 0.0, 1.0, body.center_x, body.center_y],
                    ..paint.clone()
                };
                Pipeline::clear(context, &mask_msaa, [0.0; 4]);
                self.paint_mesh(
                    device,
                    context,
                    &mask_msaa,
                    &hull_paint,
                    frame,
                    scale,
                    region,
                    1.0,
                    None,
                )?;
                Pipeline::resolve(context, &mask_msaa, &sampled);
                let mut parameters = Parameters::for_target(region.size());
                parameters.effect = [15.0, 0.0, 0.0, body.opacity as f32];
                parameters.color = rgba(body.stroke);
                let start = canvas_to_view(lens.source_center, &frame.camera, frame.surface);
                let end = canvas_to_view(
                    Point::new(body.center_x, body.center_y),
                    &frame.camera,
                    frame.surface,
                );
                parameters.gap = [
                    (start.x * scale as f64 - region.x as f64) as f32,
                    (start.y * scale as f64 - region.y as f64) as f32,
                    (end.x * scale as f64 - region.x as f64) as f32,
                    (end.y * scale as f64 - region.y as f64) as f32,
                ];
                self.pipeline.as_ref().context("Pipeline missing")?.effect(
                    context,
                    target,
                    [Some(&sampled), Some(&mask), Some(&source_mask)],
                    &parameters,
                    true,
                    None,
                );
            }
            self.scratch.put(source_mask);
        }
        if lens.source_outline {
            Pipeline::clear(context, &sampled, [0.0; 4]);
            let geometry = self.geometry.item(
                &SceneDisplayItem::Rectangle(source_body),
                frame.camera.zoom * f64::from(scale),
                frame.clear_color,
            )?;
            self.paint_item(
                device,
                context,
                &mut sampled,
                &geometry,
                frame,
                scale,
                region,
            )?;
            let mut parameters = Parameters::for_target(region.size());
            parameters.effect[0] = 16.0;
            self.pipeline.as_ref().context("Pipeline missing")?.effect(
                context,
                target,
                [Some(&sampled), Some(&mask), None],
                &parameters,
                true,
                None,
            );
        }
        Pipeline::clear(context, &sampled, [0.0; 4]);
        self.paint_source(
            device,
            context,
            &sampled,
            background,
            frame,
            scale,
            region,
            1.0,
            Some((
                [body.center_x, body.center_y],
                [lens.source_center.x, lens.source_center.y],
                [lens.factor, lens.factor],
            )),
        )?;
        let mut parameters = Parameters::for_target(region.size());
        parameters.effect = [13.0, 0.0, 0.0, body.opacity as f32];
        self.pipeline.as_ref().context("Pipeline missing")?.effect(
            context,
            target,
            [Some(&sampled), Some(&mask), None],
            &parameters,
            true,
            None,
        );
        if let Some(leader) = &lens.leader {
            let geometry = self.geometry.item(
                &SceneDisplayItem::Arrow(leader.clone()),
                frame.camera.zoom * f64::from(scale),
                frame.clear_color,
            )?;
            self.paint_item(device, context, target, &geometry, frame, scale, region)?;
        }
        let outline = RectangleDisplayItem {
            fill: ColorRgba8::default(),
            ..*body
        };
        let geometry = self.geometry.item(
            &SceneDisplayItem::Rectangle(outline),
            frame.camera.zoom * f64::from(scale),
            frame.clear_color,
        )?;
        self.paint_item(device, context, target, &geometry, frame, scale, region)?;
        self.scratch.put(mask_msaa);
        self.scratch.put(mask);
        self.scratch.put(sampled);
        Ok(())
    }
}

fn rgba(color: ColorRgba8) -> [f32; 4] {
    [color.r, color.g, color.b, color.a].map(|v| v as f32 / 255.0)
}
fn premultiply(mut color: [f32; 4]) -> [f32; 4] {
    for index in 0..3 {
        color[index] *= color[3];
    }
    color
}
fn pixel_bounds(bounds: [f64; 4], frame: FrameView, scale: f32) -> [f64; 4] {
    expand(pixel_rect(bounds, frame, scale), 2.0)
}
fn pixel_rect(bounds: [f64; 4], frame: FrameView, scale: f32) -> [f64; 4] {
    let first = canvas_to_view(
        Point::new(bounds[0], bounds[1]),
        &frame.camera,
        frame.surface,
    );
    let last = canvas_to_view(
        Point::new(bounds[2], bounds[3]),
        &frame.camera,
        frame.surface,
    );
    [
        first.x * f64::from(scale),
        first.y * f64::from(scale),
        last.x * f64::from(scale),
        last.y * f64::from(scale),
    ]
}
fn tile_coordinates(bounds: [f64; 4]) -> impl Iterator<Item = (i32, i32)> {
    let first = [
        (bounds[0] / 256.0).floor().max(-65536.0) as i32,
        (bounds[1] / 256.0).floor().max(-65536.0) as i32,
    ];
    let last = [
        (bounds[2] / 256.0).floor().min(65536.0) as i32,
        (bounds[3] / 256.0).floor().min(65536.0) as i32,
    ];
    (first[1]..=last[1]).flat_map(move |y| (first[0]..=last[0]).map(move |x| (x, y)))
}
fn intersects(a: [f64; 4], b: [f64; 4]) -> bool {
    a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]
}
fn intersection(a: [f64; 4], b: [f64; 4]) -> [f64; 4] {
    [
        a[0].max(b[0]),
        a[1].max(b[1]),
        a[2].min(b[2]),
        a[3].min(b[3]),
    ]
}
fn union(a: [f64; 4], b: [f64; 4]) -> [f64; 4] {
    [
        a[0].min(b[0]),
        a[1].min(b[1]),
        a[2].max(b[2]),
        a[3].max(b[3]),
    ]
}
fn expand(a: [f64; 4], margin: f64) -> [f64; 4] {
    [a[0] - margin, a[1] - margin, a[2] + margin, a[3] + margin]
}
fn local_scissor(bounds: [f64; 4], region: Region) -> [i32; 4] {
    [
        (bounds[0] - region.x as f64)
            .floor()
            .clamp(0.0, region.width as f64) as i32,
        (bounds[1] - region.y as f64)
            .floor()
            .clamp(0.0, region.height as f64) as i32,
        (bounds[2] - region.x as f64)
            .ceil()
            .clamp(0.0, region.width as f64) as i32,
        (bounds[3] - region.y as f64)
            .ceil()
            .clamp(0.0, region.height as f64) as i32,
    ]
}

// Semantic host/source coverage uses the same half-open pixel-center rule as
// CanvasRect::contains. Conservative geometric scissors must not expand it.
fn clip_scissor(bounds: [f64; 4], region: Region) -> [i32; 4] {
    [
        ((bounds[0] - 0.5).ceil() - f64::from(region.x)).clamp(0.0, f64::from(region.width)) as i32,
        ((bounds[1] - 0.5).ceil() - f64::from(region.y)).clamp(0.0, f64::from(region.height))
            as i32,
        ((bounds[2] - 0.5).ceil() - f64::from(region.x)).clamp(0.0, f64::from(region.width)) as i32,
        ((bounds[3] - 0.5).ceil() - f64::from(region.y)).clamp(0.0, f64::from(region.height))
            as i32,
    ]
}
trait BoundsTuple {
    fn into_array(self) -> [f64; 4];
}
impl BoundsTuple for (f64, f64, f64, f64) {
    fn into_array(self) -> [f64; 4] {
        [self.0, self.1, self.2, self.3]
    }
}

fn convex_hull(mut points: Vec<[f64; 2]>) -> Vec<[f64; 2]> {
    points.sort_by(|a, b| a[0].total_cmp(&b[0]).then(a[1].total_cmp(&b[1])));
    points.dedup();
    if points.len() < 3 {
        return points;
    }
    let cross = |a: [f64; 2], b: [f64; 2], c: [f64; 2]| {
        (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    };
    // Build each monotone chain separately so the upper pass cannot remove lower vertices.
    let mut lower = Vec::new();
    let mut upper = Vec::new();
    for p in &points {
        while lower.len() >= 2 && cross(lower[lower.len() - 2], lower[lower.len() - 1], *p) <= 0.0 {
            lower.pop();
        }
        lower.push(*p);
    }
    for p in points.iter().rev() {
        while upper.len() >= 2 && cross(upper[upper.len() - 2], upper[upper.len() - 1], *p) <= 0.0 {
            upper.pop();
        }
        upper.push(*p);
    }
    lower.pop();
    upper.pop();
    lower.extend(upper);
    lower
}

fn compose_affine(a: [f64; 6], b: [f64; 6]) -> [f64; 6] {
    [
        a[0] * b[0] + a[2] * b[1],
        a[1] * b[0] + a[3] * b[1],
        a[0] * b[2] + a[2] * b[3],
        a[1] * b[2] + a[3] * b[3],
        a[0] * b[4] + a[2] * b[5] + a[4],
        a[1] * b[4] + a[3] * b[5] + a[5],
    ]
}

// Each retained source texel covers between half and one physical pixel along
// each axis. The upper bound also keeps shifts safe for extreme camera zooms.
fn source_lod(pixels_per_source_pixel: f64) -> u8 {
    if pixels_per_source_pixel >= 1.0 {
        0
    } else {
        (-pixels_per_source_pixel.max(f64::MIN_POSITIVE).log2())
            .floor()
            .clamp(0.0, 30.0) as u8
    }
}

fn spatial_cells(bounds: [f64; 4]) -> impl Iterator<Item = (i32, i32)> {
    let first = [
        (bounds[0] / 64.0)
            .floor()
            .clamp(i32::MIN as f64, i32::MAX as f64 - 1.0) as i32,
        (bounds[1] / 64.0)
            .floor()
            .clamp(i32::MIN as f64, i32::MAX as f64 - 1.0) as i32,
    ];
    let last = [
        (bounds[2] / 64.0)
            .floor()
            .clamp(i32::MIN as f64, i32::MAX as f64 - 1.0) as i32,
        (bounds[3] / 64.0)
            .floor()
            .clamp(i32::MIN as f64, i32::MAX as f64 - 1.0) as i32,
    ];
    (first[1]..=last[1]).flat_map(move |y| (first[0]..=last[0]).map(move |x| (x, y)))
}

fn paint_bounds(paint: &Paint, frame: FrameView, scale: f32) -> [f64; 4] {
    let [left, top, right, bottom] = paint.mesh.bounds.map(f64::from);
    let [xx, yx, xy, yy, tx, ty] = paint.transform;
    let mut bounds = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    for [x, y] in [[left, top], [right, top], [right, bottom], [left, bottom]] {
        let point = canvas_to_view(
            Point::new(xx * x + xy * y + tx, yx * x + yy * y + ty),
            &frame.camera,
            frame.surface,
        );
        bounds = union(
            bounds,
            [
                point.x * scale as f64,
                point.y * scale as f64,
                point.x * scale as f64,
                point.y * scale as f64,
            ],
        );
    }
    let rough = if paint.rough_style == StrokeRenderingStyle::Architect {
        0.0
    } else {
        snow_draw_engine_core::arrow::stroke_rendering_outset(paint.rough_style, paint.rough_scale)
            * frame.camera.zoom
            * scale as f64
    };
    expand(bounds, rough + 2.0)
}

// Appending a retained stroke changes its tail, not the coverage of every stable
// prefix chunk. Material, pose, fill and arrowhead edits keep conservative owner damage.
fn changed_stroke_bounds(old: &RetainedSceneNode, new: &RetainedSceneNode) -> Option<[f64; 4]> {
    if old.pose != new.pose || old.material_revision != new.material_revision {
        return None;
    }
    let (SceneDisplayItem::Arrow(a), SceneDisplayItem::Arrow(b)) =
        (old.local_item.as_ref(), new.local_item.as_ref())
    else {
        return None;
    };
    if !a.is_free_draw
        || !b.is_free_draw
        || a.fill_style.is_visible(a.fill)
        || b.fill_style.is_visible(b.fill)
        || !a.arrowhead_primitives.is_empty()
        || !b.arrowhead_primitives.is_empty()
        || a.label_bounds.is_some()
        || b.label_bounds.is_some()
        || a.stroke_width != b.stroke_width
        || a.stroke_style != b.stroke_style
        || a.brush_mode != b.brush_mode
        || a.arrow_shaft_type != b.arrow_shaft_type
        || a.stroke_rendering_style != b.stroke_rendering_style
        || a.stroke_rendering_seed != b.stroke_rendering_seed
    {
        return None;
    }
    let (left, right) = match (&a.pressure_geometry, &b.pressure_geometry) {
        (Some(a), Some(b)) => (a, b),
        (None, None) => (&a.geometry, &b.geometry),
        _ => return None,
    };
    if left.closed || right.closed || left.chunks.is_empty() || right.chunks.is_empty() {
        return None;
    }
    let mut bounds = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    for index in 0..left.chunks.len().max(right.chunks.len()) {
        let a = left.chunks.get(index);
        let b = right.chunks.get(index);
        if a.zip(b).is_some_and(|(a, b)| {
            Arc::ptr_eq(&a.commands, &b.commands)
                && a.start_point == b.start_point
                && a.cumulative_start_length == b.cumulative_start_length
        }) {
            continue;
        }
        for chunk in [a, b].into_iter().flatten() {
            bounds = union(bounds, chunk.canvas_bounds);
        }
    }
    if !bounds.iter().all(|value| value.is_finite()) {
        return None;
    }
    let p = new.pose;
    let mut world = [
        f64::INFINITY,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NEG_INFINITY,
    ];
    for [x, y] in [
        [bounds[0], bounds[1]],
        [bounds[2], bounds[1]],
        [bounds[2], bounds[3]],
        [bounds[0], bounds[3]],
    ] {
        let px = p.xx * x + p.xy * y + p.tx;
        let py = p.yx * x + p.yy * y + p.ty;
        world = union(world, [px, py, px, py]);
    }
    Some(expand(
        world,
        b.stroke_width * 2.0
            + snow_draw_engine_core::arrow::stroke_rendering_outset(
                b.stroke_rendering_style,
                b.stroke_width * 10.0,
            )
            + 1.0,
    ))
}

#[cfg(test)]
mod scratch_pool_tests {
    use super::*;
    use gpui_windows::gpu_extensions::OffscreenGpuHost;

    #[test]
    fn full_pool_reuses_recent_surface_sizes() -> Result<()> {
        let host = OffscreenGpuHost::new()?;
        let service = host.service();
        let memory = GpuMemory::shared(&service, None, DEFAULT_GPU_MEMORY_BUDGET)?;
        let native = service
            .as_any()
            .downcast_ref::<DirectXGpuService>()
            .context("D3D11 service")?;
        native.with_context(|gpu| {
            let mut pool = ScratchPool {
                free: Vec::new(),
                bytes: 0,
            };
            // Fill the cache with an obsolete size, then return a surface used
            // by the new clipping bounds. Its next use must reuse that lease.
            pool.put(Texture::with_samples(gpu.device, [2048, 2048], 4, &memory)?);
            pool.put(Texture::with_samples(gpu.device, [256, 256], 1, &memory)?);
            assert!(pool.bytes <= 64 * 1024 * 1024);
            let allocations = memory.allocations();
            let _reused = pool.take(gpu.device, [256, 256], 1, &memory)?;
            assert_eq!(
                memory.allocations(),
                allocations,
                "full cache discarded the recently used surface"
            );
            Ok(())
        })
    }
}
