#[cfg(test)]
#[path = "document_tests.rs"]
mod tests;
use std::{
    fmt,
    io::Cursor,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use image::{ImageReader, Pixel, RgbaImage};
use snow_draw_engine::{
    ActiveTool, Camera, Engine, ErrorCode, InputEvent, InputUpdate, MutationResult, Point,
    RuntimeConfig, ViewportConfig, ViewportId,
};

use crate::RetainedScene;

const MAX_FILE_BYTES: usize = 512 * 1024 * 1024;
const MAX_SOURCE_HISTORY_BYTES: usize = 1024 * 1024 * 1024;
const MAX_SOURCE_HISTORY_IMAGES: usize = 128;
pub const MAX_SURFACE_PIXELS: u64 = 16 * 1024 * 1024;
pub const MAX_RASTER_BYTES: u64 = 8 * 1024 * 1024 * 1024;
static NEXT_DOCUMENT: AtomicU64 = AtomicU64::new(1);
static NEXT_RESOURCE_REVISION: AtomicU64 = AtomicU64::new(1);
static NEXT_HOST_STATE: AtomicU64 = AtomicU64::new(1);
static NEXT_PIXELS: AtomicU64 = AtomicU64::new(1);

/// Errors at the host boundary retain the engine error without requiring Qt.
#[derive(Debug)]
pub enum CanvasError {
    Engine(ErrorCode),
    InvalidFile(String),
    Gpu(String),
    ResourceLimit,
    Unsupported(&'static str),
    Cancelled,
}

impl fmt::Display for CanvasError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Engine(error) => write!(f, "Drawing command failed: {error:?}"),
            Self::InvalidFile(error) => write!(f, "Could not read drawing: {error}"),
            Self::Gpu(error) => write!(f, "GPU rendering failed: {error}"),
            Self::ResourceLimit => {
                f.write_str("The image or drawing exceeds the canvas size limit")
            }
            Self::Unsupported(capability) => write!(f, "This canvas does not support {capability}"),
            Self::Cancelled => f.write_str("Canvas rendering was cancelled"),
        }
    }
}

impl std::error::Error for CanvasError {}
impl From<ErrorCode> for CanvasError {
    fn from(value: ErrorCode) -> Self {
        Self::Engine(value)
    }
}

/// Immutable pristine image. Placement is in canvas units, independent of display DPI.
#[derive(Clone)]
pub struct SourceImage {
    pub(crate) pixels: Arc<RgbaImage>,
    gpu_texture: Option<Arc<dyn gpui::GpuTexture>>,
    #[cfg(target_os = "windows")]
    pub(crate) gpu_straight: Option<Arc<crate::gpu_smart_erase::Buffer>>,
    origin: Point<f64>,
    units_per_pixel: f64,
    pub(crate) smart_erase_pixels: Option<Arc<RgbaImage>>,
    pixels_identity: u64,
    layers: Option<Arc<[crate::ImageSourceLayer]>>,
    dimensions: [u32; 2],
}

impl SourceImage {
    pub(crate) fn eligible_for_rendering(&self) -> Self {
        let mut source = self.clone();
        source.pixels_identity = NEXT_PIXELS.fetch_add(1, Ordering::Relaxed);
        if let Some(layers) = &self.layers {
            source.layers = Some(
                layers
                    .iter()
                    .filter(|layer| layer.smart_erase_source)
                    .map(|layer| crate::ImageSourceLayer {
                        image: layer.image.eligible_for_rendering(),
                        ..layer.clone()
                    })
                    .collect::<Vec<_>>()
                    .into(),
            );
        } else if let Some(pixels) = &self.smart_erase_pixels {
            source.pixels = pixels.clone();
        }
        source.smart_erase_pixels = None;
        source
    }
    pub fn new(pixels: RgbaImage) -> Result<Self, CanvasError> {
        validate_raster(pixels.width(), pixels.height())?;
        let dimensions = [pixels.width(), pixels.height()];
        Ok(Self {
            pixels: Arc::new(pixels),
            gpu_texture: None,
            #[cfg(target_os = "windows")]
            gpu_straight: None,
            origin: Point::new(0.0, 0.0),
            units_per_pixel: 1.0,
            smart_erase_pixels: None,
            pixels_identity: NEXT_PIXELS.fetch_add(1, Ordering::Relaxed),
            layers: None,
            dimensions,
        })
    }

    /// Retain an immutable premultiplied GPUI texture without reading its pixels
    /// back. It can be drawn/exported on the same device and epoch. Callers must
    /// retire GPU use before mutating the underlying texture. CPU codecs/donor
    /// sampling require explicit asynchronous materialization first.
    pub fn from_gpu_texture(texture: Arc<dyn gpui::GpuTexture>) -> Result<Self, CanvasError> {
        let dimensions = texture.size();
        validate_raster(dimensions[0], dimensions[1])?;
        Ok(Self {
            pixels: Arc::new(RgbaImage::new(0, 0)),
            gpu_texture: Some(texture),
            #[cfg(target_os = "windows")]
            gpu_straight: None,
            origin: Point::new(0.0, 0.0),
            units_per_pixel: 1.0,
            smart_erase_pixels: None,
            pixels_identity: NEXT_PIXELS.fetch_add(1, Ordering::Relaxed),
            layers: None,
            dimensions,
        })
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, CanvasError> {
        Self::decode_with_limit(bytes, 512 * 1024 * 1024)
    }
    /// Decode budgets are distinct from the compositor's bounded working tiles.
    /// Hosts owning a larger capture may pass it to `new` without a decode copy.
    pub fn decode_with_limit(bytes: &[u8], maximum_rgba_bytes: u64) -> Result<Self, CanvasError> {
        if maximum_rgba_bytes == 0 || maximum_rgba_bytes > MAX_RASTER_BYTES {
            return Err(CanvasError::ResourceLimit);
        }
        if bytes.len() > MAX_FILE_BYTES {
            return Err(CanvasError::ResourceLimit);
        }
        let mut reader = ImageReader::new(Cursor::new(bytes))
            .with_guessed_format()
            .map_err(|error| CanvasError::InvalidFile(error.to_string()))?;
        let (width, height) = ImageReader::new(Cursor::new(bytes))
            .with_guessed_format()
            .map_err(|error| CanvasError::InvalidFile(error.to_string()))?
            .into_dimensions()
            .map_err(|error| CanvasError::InvalidFile(error.to_string()))?;
        validate_raster(width, height)?;
        if u64::from(width) * u64::from(height) * 4 > maximum_rgba_bytes {
            return Err(CanvasError::ResourceLimit);
        }
        let mut limits = image::Limits::default();
        limits.max_alloc = Some(maximum_rgba_bytes);
        limits.max_image_width = Some(i32::MAX as u32 / 4);
        limits.max_image_height = Some(i32::MAX as u32);
        reader.limits(limits);
        let pixels = reader
            .decode()
            .map_err(|error| CanvasError::InvalidFile(error.to_string()))?
            .into_rgba8();
        if u64::from(pixels.width()) * u64::from(pixels.height()) * 4 > maximum_rgba_bytes {
            return Err(CanvasError::ResourceLimit);
        }
        Self::new(pixels)
    }

    pub fn with_placement(
        mut self,
        origin: Point<f64>,
        units_per_pixel: f64,
    ) -> Result<Self, CanvasError> {
        if !origin.x.is_finite()
            || !origin.y.is_finite()
            || !units_per_pixel.is_finite()
            || units_per_pixel <= 0.0
        {
            return Err(ErrorCode::InvalidArgument.into());
        }
        let right = origin.x + f64::from(self.width()) * units_per_pixel;
        let bottom = origin.y + f64::from(self.height()) * units_per_pixel;
        if [origin.x, origin.y, right, bottom]
            .into_iter()
            .any(|value| !value.is_finite() || value.abs() > 1.0e9)
        {
            return Err(ErrorCode::InvalidArgument.into());
        }
        if let Some(layers) = &self.layers {
            let scale = units_per_pixel / self.units_per_pixel;
            let map = |rect: crate::CanvasRect| crate::CanvasRect {
                x: origin.x + (rect.x - self.origin.x) * scale,
                y: origin.y + (rect.y - self.origin.y) * scale,
                width: rect.width * scale,
                height: rect.height * scale,
            };
            self.layers = Some(
                layers
                    .iter()
                    .map(|layer| crate::ImageSourceLayer {
                        image: layer.image.clone(),
                        canvas_rect: Some(map(layer.placement())),
                        coverage: layer.coverage.map(map),
                        smart_erase_source: layer.smart_erase_source,
                    })
                    .collect::<Vec<_>>()
                    .into(),
            );
        }
        self.origin = origin;
        self.units_per_pixel = units_per_pixel;
        Ok(self)
    }

    pub fn layers(&self) -> Option<&[crate::ImageSourceLayer]> {
        self.layers.as_deref()
    }
    pub fn dimensions(&self) -> [u32; 2] {
        self.dimensions
    }
    pub fn width(&self) -> u32 {
        self.dimensions[0]
    }
    pub fn height(&self) -> u32 {
        self.dimensions[1]
    }
    /// Returns immutable CPU pixels; layered and GPU-backed images have no CPU raster.
    pub fn raster_pixels(&self) -> Option<&Arc<RgbaImage>> {
        (self.layers.is_none() && self.gpu_texture.is_none()).then_some(&self.pixels)
    }
    pub fn gpu_texture(&self) -> Option<&Arc<dyn gpui::GpuTexture>> {
        self.gpu_texture.as_ref()
    }
    pub fn origin(&self) -> Point<f64> {
        self.origin
    }
    pub fn units_per_pixel(&self) -> f64 {
        self.units_per_pixel
    }
    pub fn pixels_identity(&self) -> u64 {
        self.pixels_identity
    }
    pub(crate) fn smart_erase_pixels(&self) -> &Arc<RgbaImage> {
        debug_assert!(
            self.layers.is_none(),
            "CPU donor access requires a leaf source"
        );
        self.smart_erase_pixels.as_ref().unwrap_or(&self.pixels)
    }
    fn retained_bytes(&self) -> usize {
        if self.gpu_texture.is_some() {
            let bytes = self.width() as usize * self.height() as usize * 4;
            #[cfg(target_os = "windows")]
            let bytes = bytes * (1 + usize::from(self.gpu_straight.is_some()));
            return bytes;
        }
        if let Some(layers) = &self.layers {
            let mut ids = std::collections::HashSet::new();
            return layers
                .iter()
                .filter(|layer| ids.insert(layer.image.pixels_identity()))
                .map(|layer| layer.image.retained_bytes())
                .sum();
        }
        self.pixels.as_raw().len()
            + self
                .smart_erase_pixels
                .as_ref()
                .map_or(0, |pixels| pixels.as_raw().len())
    }

    /// Retains layered pristine inputs without allocating a composite image.
    /// Coverage, anisotropic placement and donor eligibility remain explicit.
    pub fn from_layers(layers: &[crate::ImageSourceLayer]) -> Result<Option<Self>, CanvasError> {
        Self::from_layers_cancellable(layers, &Default::default())
    }
    pub fn from_layers_cancellable(
        layers: &[crate::ImageSourceLayer],
        cancel: &crate::CanvasCancellation,
    ) -> Result<Option<Self>, CanvasError> {
        cancel.check()?;
        if layers.is_empty() {
            return Ok(None);
        }
        let layers = normalize_source_layers(layers)?;
        if layers.len() > 128
            || layers
                .iter()
                .map(|layer| layer.image.retained_bytes())
                .sum::<usize>()
                > 1024 * 1024 * 1024
        {
            return Err(CanvasError::ResourceLimit);
        }
        let mut left = f64::INFINITY;
        let mut top = f64::INFINITY;
        let mut right = f64::NEG_INFINITY;
        let mut bottom = f64::NEG_INFINITY;
        let mut units = f64::INFINITY;
        for layer in &layers {
            if let Some(coverage) = layer.coverage {
                coverage.validate()?;
            }
            let image = &layer.image;
            let placement = layer.placement();
            placement.validate()?;
            let mut bounds = (
                placement.x,
                placement.y,
                placement.x + placement.width,
                placement.y + placement.height,
            );
            if let Some(area) = layer.coverage {
                bounds.0 = bounds.0.max(area.x);
                bounds.1 = bounds.1.max(area.y);
                bounds.2 = bounds.2.min(area.x + area.width);
                bounds.3 = bounds.3.min(area.y + area.height);
            }
            if bounds.2 <= bounds.0 || bounds.3 <= bounds.1 {
                continue;
            }
            left = left.min(bounds.0);
            top = top.min(bounds.1);
            right = right.max(bounds.2);
            bottom = bottom.max(bounds.3);
            units = units
                .min(placement.width / f64::from(image.width()))
                .min(placement.height / f64::from(image.height()));
        }
        if !left.is_finite() {
            return Ok(None);
        }
        let width = ((right - left) / units).ceil() as u32;
        let height = ((bottom - top) / units).ceil() as u32;
        validate_raster(width, height)?;
        if u64::from(width) * u64::from(height) * 4 > 512 * 1024 * 1024 {
            return Err(CanvasError::ResourceLimit);
        }
        let samples: f64 = layers
            .iter()
            .map(|layer| {
                let bounds = effective_layer_bounds(layer);
                ((bounds.width / units).ceil() + 1.0) * ((bounds.height / units).ceil() + 1.0)
            })
            .sum();
        if samples > 128.0 * 1024.0 * 1024.0 {
            return Err(CanvasError::ResourceLimit);
        }
        cancel.check()?;
        Ok(Some(Self {
            pixels: Arc::new(RgbaImage::new(0, 0)),
            gpu_texture: None,
            #[cfg(target_os = "windows")]
            gpu_straight: None,
            origin: Point::new(left, top),
            units_per_pixel: units,
            smart_erase_pixels: None,
            pixels_identity: NEXT_PIXELS.fetch_add(1, Ordering::Relaxed),
            layers: Some(layers.into()),
            dimensions: [width, height],
        }))
    }

    /// CPU work is limited to the Smart Erase donor neighborhood, in the common
    /// source grid. Visual-only layers never enter the reconstruction input.
    pub(crate) fn eligible_region(
        &self,
        region: [u32; 4],
        cancel: &crate::CanvasCancellation,
    ) -> Result<RgbaImage, CanvasError> {
        let [x, y, width, height] = region;
        if width == 0
            || height == 0
            || x.checked_add(width).is_none_or(|v| v > self.width())
            || y.checked_add(height).is_none_or(|v| v > self.height())
        {
            return Err(ErrorCode::InvalidArgument.into());
        }
        cancel.check()?;
        if self.gpu_texture.is_some() {
            return Err(CanvasError::Unsupported(
                "CPU donor sampling before GPU source materialization",
            ));
        }
        if let Some(layers) = &self.layers {
            sample_donor_layers(
                layers,
                Point::new(
                    self.origin.x + f64::from(x) * self.units_per_pixel,
                    self.origin.y + f64::from(y) * self.units_per_pixel,
                ),
                self.units_per_pixel,
                width,
                height,
                cancel,
            )
        } else {
            let pixels = self.smart_erase_pixels();
            Ok(image::imageops::crop_imm(pixels.as_ref(), x, y, width, height).to_image())
        }
    }
}

fn intersect_rect(a: crate::CanvasRect, b: crate::CanvasRect) -> crate::CanvasRect {
    let x = a.x.max(b.x);
    let y = a.y.max(b.y);
    crate::CanvasRect {
        x,
        y,
        width: ((a.x + a.width).min(b.x + b.width) - x).max(0.0),
        height: ((a.y + a.height).min(b.y + b.height) - y).max(0.0),
    }
}

fn effective_layer_bounds(layer: &crate::ImageSourceLayer) -> crate::CanvasRect {
    let rect = layer.placement();
    layer
        .coverage
        .map_or(rect, |coverage| intersect_rect(rect, coverage))
}

fn normalize_source_layers(
    layers: &[crate::ImageSourceLayer],
) -> Result<Vec<crate::ImageSourceLayer>, CanvasError> {
    let mut result = Vec::new();
    for layer in layers {
        let placement = layer.placement();
        placement.validate()?;
        if let Some(coverage) = layer.coverage {
            coverage.validate()?;
        }
        if placement.width <= 0.0 || placement.height <= 0.0 {
            continue;
        }
        if let Some(children) = layer.image.layers() {
            let origin = layer.image.origin();
            let x_scale =
                placement.width / (f64::from(layer.image.width()) * layer.image.units_per_pixel());
            let y_scale = placement.height
                / (f64::from(layer.image.height()) * layer.image.units_per_pixel());
            let map = |rect: crate::CanvasRect| crate::CanvasRect {
                x: placement.x + (rect.x - origin.x) * x_scale,
                y: placement.y + (rect.y - origin.y) * y_scale,
                width: rect.width * x_scale,
                height: rect.height * y_scale,
            };
            for child in children {
                let rect = map(child.placement());
                let mut coverage = intersect_rect(map(effective_layer_bounds(child)), placement);
                if let Some(parent_coverage) = layer.coverage {
                    coverage = intersect_rect(coverage, parent_coverage);
                }
                rect.validate()?;
                coverage.validate()?;
                result.push(crate::ImageSourceLayer {
                    image: child.image.clone(),
                    canvas_rect: Some(rect),
                    coverage: Some(coverage),
                    smart_erase_source: layer.smart_erase_source && child.smart_erase_source,
                });
                if result.len() > 128 {
                    return Err(CanvasError::ResourceLimit);
                }
            }
        } else {
            result.push(layer.clone());
            if result.len() > 128 {
                return Err(CanvasError::ResourceLimit);
            }
        }
    }
    Ok(result)
}

fn sample_donor_layers(
    layers: &[crate::ImageSourceLayer],
    origin: Point<f64>,
    units: f64,
    width: u32,
    height: u32,
    cancel: &crate::CanvasCancellation,
) -> Result<RgbaImage, CanvasError> {
    let mut output = RgbaImage::new(width, height);
    let mut samples = 0_u64;
    for layer in layers {
        if !layer.smart_erase_source {
            continue;
        }
        if layer.image.gpu_texture.is_some() {
            return Err(CanvasError::Unsupported(
                "CPU donor sampling before GPU source materialization",
            ));
        }
        let mut source = layer.image.clone();
        source.pixels = source.smart_erase_pixels().clone();
        let placement = layer.placement();
        if placement.width <= 0.0 || placement.height <= 0.0 {
            continue;
        }
        let mut left = placement.x;
        let mut top = placement.y;
        let mut right = left + placement.width;
        let mut bottom = top + placement.height;
        if let Some(area) = layer.coverage {
            left = left.max(area.x);
            top = top.max(area.y);
            right = right.min(area.x + area.width);
            bottom = bottom.min(area.y + area.height);
        }
        let x0 = ((left - origin.x) / units)
            .floor()
            .clamp(0.0, f64::from(width)) as u32;
        let y0 = ((top - origin.y) / units)
            .floor()
            .clamp(0.0, f64::from(height)) as u32;
        let x1 = ((right - origin.x) / units)
            .ceil()
            .clamp(0.0, f64::from(width)) as u32;
        let y1 = ((bottom - origin.y) / units)
            .ceil()
            .clamp(0.0, f64::from(height)) as u32;
        samples += u64::from(x1.saturating_sub(x0)) * u64::from(y1.saturating_sub(y0));
        if samples > 128 * 1024 * 1024 {
            return Err(CanvasError::ResourceLimit);
        }
        for y in y0..y1 {
            cancel.check()?;
            for x in x0..x1 {
                let world = Point::new(
                    origin.x + (f64::from(x) + 0.5) * units,
                    origin.y + (f64::from(y) + 0.5) * units,
                );
                let mapped = Point::new(
                    source.origin.x
                        + (world.x - placement.x)
                            * f64::from(source.pixels.width())
                            * source.units_per_pixel
                            / placement.width,
                    source.origin.y
                        + (world.y - placement.y)
                            * f64::from(source.pixels.height())
                            * source.units_per_pixel
                            / placement.height,
                );
                if layer.coverage.is_none_or(|area| area.contains(world))
                    && let Some(pixel) = crate::source_geometry::sample_source(&source, mapped)
                {
                    output.get_pixel_mut(x, y).blend(&pixel);
                }
            }
        }
    }
    Ok(output)
}

/// Owns the engine and external resources. Views keep their viewport state separately.
pub struct CanvasDocument {
    identity: u64,
    engine: Engine,
    background: Option<SourceImage>,
    resource_revision: u64,
    interaction_owner: Option<ViewportId>,
    host_sources: std::collections::HashMap<u64, Option<SourceImage>>,
    host_state: u64,
    host_history_generation: u64,
}

fn validate_source_history_budget<'a>(
    sources: impl IntoIterator<Item = &'a SourceImage>,
    maximum_bytes: usize,
    maximum_images: usize,
) -> Result<(), CanvasError> {
    let mut identities = std::collections::HashSet::new();
    let mut bytes = 0usize;
    for source in sources {
        if !identities.insert(source.pixels_identity()) {
            continue;
        }
        bytes = bytes
            .checked_add(source.retained_bytes())
            .ok_or(CanvasError::ResourceLimit)?;
        if bytes > maximum_bytes || identities.len() > maximum_images {
            return Err(CanvasError::ResourceLimit);
        }
    }
    Ok(())
}

impl Default for CanvasDocument {
    fn default() -> Self {
        Self::new(RuntimeConfig::default()).expect("valid default engine configuration")
    }
}

impl CanvasDocument {
    pub fn new(config: RuntimeConfig) -> Result<Self, CanvasError> {
        Ok(Self {
            identity: NEXT_DOCUMENT.fetch_add(1, Ordering::Relaxed),
            engine: Engine::try_new(config)?,
            background: None,
            resource_revision: NEXT_RESOURCE_REVISION.fetch_add(1, Ordering::Relaxed),
            interaction_owner: None,
            host_sources: [(0, None)].into_iter().collect(),
            host_state: 0,
            host_history_generation: 0,
        })
    }

    pub fn engine(&self) -> &Engine {
        &self.engine
    }
    pub(crate) fn release_source_resources(&mut self) -> Vec<SourceImage> {
        let mut resources: Vec<_> = self
            .host_sources
            .drain()
            .filter_map(|(_, image)| image)
            .collect();
        resources.extend(self.background.take());
        resources
    }

    /// Runs an engine command. Callers must refresh their sessions after the mutation.
    /// The returned engine changed-viewport list lets a future multi-view host notify selectively.
    pub fn command<T>(
        &mut self,
        command: impl FnOnce(&mut Engine) -> Result<T, ErrorCode>,
    ) -> Result<T, CanvasError> {
        let result = command(&mut self.engine).map_err(CanvasError::from);
        self.synchronize_source_history();
        result
    }

    fn synchronize_source_history(&mut self) {
        let state = self.engine.host_history_state(0);
        let generation = self.engine.host_history_generation();
        if state == self.host_state && generation == self.host_history_generation {
            return;
        }
        if state != self.host_state {
            self.host_sources
                .entry(state)
                .or_insert_with(|| self.background.clone());
            if let Some(source) = self.host_sources.get(&state) {
                self.background = source.clone();
                self.resource_revision = NEXT_RESOURCE_REVISION.fetch_add(1, Ordering::Relaxed);
            }
            self.host_state = state;
        }
        if self.host_sources.len() > 1 {
            let states = self.engine.host_history_states(true);
            self.host_sources
                .retain(|state, _| states.binary_search(state).is_ok());
        }
        self.host_history_generation = generation;
    }

    pub fn background(&self) -> Option<&SourceImage> {
        self.background.as_ref()
    }
    pub fn resource_revision(&self) -> u64 {
        self.resource_revision
    }

    /// Background replacement is a host document operation; the engine cannot own image bytes.
    pub fn set_background(&mut self, image: Option<SourceImage>) {
        self.host_sources.insert(self.host_state, image.clone());
        self.background = image;
        self.resource_revision = NEXT_RESOURCE_REVISION.fetch_add(1, Ordering::Relaxed);
    }

    /// Source replacement shares the engine's undo timeline. Resource undo is
    /// runtime-only, as are the engine host-state tokens. Reject new snapshots
    /// beyond 1 GiB/128 distinct assets rather than evicting required undo data.
    /// Admission excludes the redo branch discarded by a successful replacement;
    /// rejected replacements preserve that branch and all of its source assets.
    /// Metadata token references follow the engine history's runtime lifetime.
    pub fn replace_background(&mut self, image: Option<SourceImage>) -> Result<(), CanvasError> {
        if self.engine.has_pointer_capture() {
            return Err(ErrorCode::InvalidState.into());
        }
        let surviving_states = self.engine.host_history_states(false);
        validate_source_history_budget(
            surviving_states
                .iter()
                .filter_map(|state| self.host_sources.get(state))
                .flatten()
                .chain(image.as_ref()),
            MAX_SOURCE_HISTORY_BYTES,
            MAX_SOURCE_HISTORY_IMAGES,
        )?;
        let mut state = NEXT_HOST_STATE.fetch_add(1, Ordering::Relaxed);
        while self.host_sources.contains_key(&state) {
            state = NEXT_HOST_STATE.fetch_add(1, Ordering::Relaxed);
        }
        self.engine.commit_host_history_state(state)?;
        self.host_state = state;
        self.set_background(image);
        self.synchronize_source_history();
        Ok(())
    }

    pub fn persistence_snapshot(&self) -> Result<crate::DrawingSnapshot, CanvasError> {
        crate::DrawingSnapshot::capture(&self.engine, self.background.clone())
    }
    pub fn serialize(&self) -> Result<Vec<u8>, CanvasError> {
        self.persistence_snapshot()?.encode()
    }
    pub fn deserialize(bytes: &[u8]) -> Result<Self, CanvasError> {
        crate::DrawingSnapshot::decode(bytes)?.into_document()
    }
    pub(crate) fn from_parts(engine: Engine, background: Option<SourceImage>) -> Self {
        let host_sources = [(0, background.clone())].into_iter().collect();
        let host_history_generation = engine.host_history_generation();
        Self {
            identity: NEXT_DOCUMENT.fetch_add(1, Ordering::Relaxed),
            engine,
            background,
            resource_revision: NEXT_RESOURCE_REVISION.fetch_add(1, Ordering::Relaxed),
            interaction_owner: None,
            host_sources,
            host_state: 0,
            host_history_generation,
        }
    }
}

pub(crate) fn validate_surface(width: u32, height: u32) -> Result<(), CanvasError> {
    if width == 0
        || height == 0
        || width > 16_384
        || height > 16_384
        || u64::from(width) * u64::from(height) > MAX_SURFACE_PIXELS
    {
        return Err(CanvasError::ResourceLimit);
    }
    Ok(())
}

pub(crate) fn validate_raster(width: u32, height: u32) -> Result<(), CanvasError> {
    if width == 0
        || height == 0
        || width > i32::MAX as u32 / 4
        || height > i32::MAX as u32
        || u64::from(width) * u64::from(height) > MAX_RASTER_BYTES / 4
    {
        return Err(CanvasError::ResourceLimit);
    }
    Ok(())
}

/// A viewport and its retained patch cursor. Call `close` when removing a shared-document view.
pub struct CanvasSession {
    document_identity: u64,
    viewport: ViewportId,
    scene: RetainedScene,
    deferred_composition: bool,
}

impl CanvasSession {
    pub fn new(document: &mut CanvasDocument) -> Result<Self, CanvasError> {
        let mut config = ViewportConfig::default();
        config.engine.min_zoom = 1.0e-6;
        let viewport = document.engine.create_retained_viewport(config)?;
        let mut session = Self {
            document_identity: document.identity,
            viewport,
            scene: RetainedScene::default(),
            deferred_composition: false,
        };
        session.refresh(document)?;
        Ok(session)
    }
    pub fn viewport(&self) -> ViewportId {
        self.viewport
    }
    pub fn scene(&self) -> &RetainedScene {
        &self.scene
    }
    /// Coalesce presentation work until `flush`, while interaction transforms
    /// and engine queries continue to reflect every input event immediately.
    pub fn set_deferred_composition(&mut self, enabled: bool) {
        self.deferred_composition = enabled;
    }
    pub fn snapshot_scene(&self, document: &CanvasDocument) -> Result<RetainedScene, CanvasError> {
        self.check_document(document)?;
        let mut scene = self.scene.clone();
        let delta = document
            .engine
            .acquire_retained_scene_delta(self.viewport, scene.retained_revision())?;
        scene.apply_delta(&delta)?;
        Ok(scene)
    }
    pub fn close(self, document: &mut CanvasDocument) -> Result<Option<InputUpdate>, CanvasError> {
        self.check_document(document)?;
        let cancellation = if document.interaction_owner == Some(self.viewport) {
            document.interaction_owner = None;
            Some(
                document
                    .engine
                    .process_input_with_viewport_changes(self.viewport, InputEvent::FocusLost)?,
            )
        } else {
            None
        };
        document.engine.destroy_viewport(self.viewport)?;
        Ok(cancellation)
    }
    pub fn refresh(&mut self, document: &CanvasDocument) -> Result<bool, CanvasError> {
        self.check_document(document)?;
        if self.deferred_composition {
            let frame = document.engine.viewport_frame(self.viewport)?;
            self.scene = self.scene.with_frame(frame);
            return Ok(true);
        }
        self.flush(document)
    }
    /// Publish all input updates accumulated since the previous presentation.
    pub fn flush(&mut self, document: &CanvasDocument) -> Result<bool, CanvasError> {
        self.check_document(document)?;
        let patch = document
            .engine
            .acquire_retained_scene_delta(self.viewport, self.scene.retained_revision())?;
        match self.scene.apply_delta(&patch) {
            Ok(changed) => Ok(changed),
            Err(_) => {
                let reset = document
                    .engine
                    .acquire_retained_scene_delta(self.viewport, None)?;
                self.scene.apply_delta(&reset)
            }
        }
    }
    pub fn set_surface(
        &mut self,
        document: &mut CanvasDocument,
        width: u32,
        height: u32,
    ) -> Result<bool, CanvasError> {
        self.check_document(document)?;
        validate_surface(width, height)?;
        document
            .engine
            .set_viewport_surface_size(self.viewport, width, height)?;
        self.refresh(document)
    }
    pub fn set_camera(
        &mut self,
        document: &mut CanvasDocument,
        camera: Camera,
    ) -> Result<bool, CanvasError> {
        self.check_document(document)?;
        document.engine.set_viewport_camera(self.viewport, camera)?;
        self.refresh(document)
    }
    pub fn set_tool(
        &mut self,
        document: &mut CanvasDocument,
        tool: ActiveTool,
    ) -> Result<bool, CanvasError> {
        // A tool switch must end the previous gesture before changing creation defaults.
        self.input(document, InputEvent::FocusLost)?;
        document
            .engine
            .set_viewport_active_tool(self.viewport, tool)?;
        self.refresh(document)
    }
    pub fn input(
        &mut self,
        document: &mut CanvasDocument,
        event: InputEvent,
    ) -> Result<InputUpdate, CanvasError> {
        self.check_document(document)?;
        // The engine editor is shared by all viewports. An idle view losing focus
        // must not cancel a gesture owned by another view.
        if document
            .interaction_owner
            .is_some_and(|owner| owner != self.viewport)
        {
            if matches!(event, InputEvent::FocusLost) {
                return Ok(InputUpdate {
                    interaction: Default::default(),
                    changed_viewports: Vec::new(),
                });
            }
            if document.engine.has_pointer_capture() {
                return Err(ErrorCode::InvalidState.into());
            }
        }
        let update = document
            .command(|engine| engine.process_input_with_viewport_changes(self.viewport, event))?;
        if matches!(
            update.interaction.capture,
            snow_draw_engine::PointerCaptureCommand::Capture(_)
        ) || matches!(event, InputEvent::Pointer(pointer) if pointer.event_type == snow_draw_engine::PointerEventType::Down && update.interaction.consumed)
        {
            document.interaction_owner = Some(self.viewport);
        }
        if matches!(event, InputEvent::FocusLost) {
            document.interaction_owner = None;
        }
        self.refresh(document)?;
        Ok(update)
    }
    pub fn mutate(
        &mut self,
        document: &mut CanvasDocument,
        command: impl FnOnce(&mut Engine, ViewportId) -> Result<MutationResult, ErrorCode>,
    ) -> Result<bool, CanvasError> {
        self.check_document(document)?;
        document.command(|engine| command(engine, self.viewport))?;
        self.refresh(document)
    }

    fn check_document(&self, document: &CanvasDocument) -> Result<(), CanvasError> {
        if self.document_identity != document.identity {
            return Err(ErrorCode::InvalidState.into());
        }
        Ok(())
    }
}
