use std::collections::HashMap;
use std::sync::Arc;

use snow_capture::gpu::GpuCapturedFrame;
use snow_d3d11::{Rect, SharedDevice, Texture, VideoLayer, VideoProcessor};
use snow_recording_effects::surface::{TILE_SIZE, Tile, TileSurface};
use snow_recording_export::gpu::GpuEncoderFrame;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::*;

use super::*;

#[path = "direct_gpu_cursor.rs"]
mod cursor;
use cursor::CursorRenderer;

#[path = "direct_gpu_resize.rs"]
pub(crate) mod resize;
use resize::{AreaScaler, CropCopy};

pub(super) fn eligible(config: &DirectRecordingConfig) -> bool {
    config.format == ExportFormat::Mp4
        && config.prefer_hardware_encoder
        && !crate::capture_policy::recording_gpu_backends(config.capture_backend).is_empty()
}

pub(crate) use crate::gpu_startup::Negotiation;
use crate::gpu_startup::{ProbePurpose, prepare_capture, probe_encoder};

pub(super) fn prepare(
    config: &DirectRecordingConfig,
    include_cursor: bool,
    state: &mut VisualCompositor,
    diagnostics: &mut Negotiation,
) -> Result<
    Option<(
        snow_capture::gpu::GpuCaptureStream,
        GpuVisualCompositor,
        SharedDevice,
    )>,
> {
    let Some((stream, observation)) = prepare_capture(config, include_cursor, diagnostics)? else {
        return Ok(None);
    };
    let device = stream.device().clone();
    diagnostics.adapter = Some(device.identity().description.clone());
    diagnostics.stage = Some("compositor_startup".into());
    let mut compositor = GpuVisualCompositor::new(
        device.clone(),
        (config.region.width, config.region.height),
        config.output_dimensions(),
        config.output_fps,
    )?;
    let mut probe_config = config.clone();
    probe_config.show_cursor = true;
    probe_encoder(
        config.streaming_config(),
        ProbePurpose::LiveOutput,
        device.clone(),
        diagnostics,
        |pts, surface| {
            // Exercise the cursor shader even if the physical cursor is hidden or
            // outside the recording region when startup is negotiated.
            let cursor = AttachedCursorSample {
                x: 0,
                y: 0,
                visible: true,
                shape: CursorShapeState::Embedded(CursorShape::from_rgba(
                    0,
                    0,
                    2,
                    2,
                    if pts % 2 == 0 {
                        CursorCompositionMode::AlphaBlend
                    } else {
                        CursorCompositionMode::MaskedColor
                    },
                    vec![
                        255, 0, 127, 128, 255, 255, 255, 255, 0, 0, 0, 0, 30, 60, 90, 255,
                    ],
                )),
            };
            compositor.compose(
                state,
                &probe_config,
                &observation,
                0,
                Some(&cursor),
                surface,
            )
        },
    )?;
    state.input_effects.trail.clear();
    compositor.overlay_upload_bytes = 0;
    Ok(Some((stream, compositor, device)))
}

fn gpu_error(error: impl std::fmt::Display) -> ScreenRecorderError {
    ScreenRecorderError::Encode(format!("GPU composition: {error:#}"))
}

#[derive(Default)]
pub(super) struct Metrics {
    pub memory_bytes: u64,
    pub overlay_bytes: u64,
    last_sample: Option<Instant>,
}

impl Metrics {
    pub fn sample(&mut self, compositor: Option<&GpuVisualCompositor>) {
        let Some(compositor) = compositor else { return };
        self.overlay_bytes = compositor.overlay_upload_bytes;
        if self
            .last_sample
            .is_none_or(|last| last.elapsed() >= Duration::from_secs(1))
        {
            self.memory_bytes = self
                .memory_bytes
                .max(compositor.memory_usage().unwrap_or(0));
            self.last_sample = Some(Instant::now());
        }
    }
}

struct OverlayTexture {
    texture: Texture,
    previous: HashMap<(u32, u32), Arc<Vec<u8>>>,
    upload: Vec<u8>,
}

/// Camera uploads are bounded by the overlay size, independent of desktop resolution.
/// Keep them separate from effects so an unchanged camera image is never uploaded twice.
struct WebcamTexture {
    texture: Texture,
    generation: u64,
}

impl OverlayTexture {
    fn new(device: &SharedDevice, size: (u32, u32)) -> Result<Self> {
        let texture = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
            )
            .map_err(gpu_error)?;
        let mut view = None;
        unsafe {
            device
                .device()
                .CreateRenderTargetView(texture.raw(), None, Some(&mut view))
                .map_err(gpu_error)?;
            device.context().ClearRenderTargetView(
                view.as_ref()
                    .ok_or_else(|| gpu_error("missing overlay render target"))?,
                &[0.0; 4],
            );
        }
        Ok(Self {
            texture,
            previous: HashMap::new(),
            upload: vec![0; (TILE_SIZE * TILE_SIZE * 4) as usize],
        })
    }

    fn upload(&mut self, tiles: Vec<Tile>) -> Result<u64> {
        let next: HashMap<_, _> = tiles
            .into_iter()
            .map(|tile| ((tile.x, tile.y), tile.pixels))
            .collect();
        let mut bytes = 0;
        let mut changed: Vec<_> = self
            .previous
            .keys()
            .filter(|key| !next.contains_key(key))
            .copied()
            .collect();
        changed.extend(
            next.iter()
                .filter(|(key, pixels)| {
                    self.previous
                        .get(key)
                        .is_none_or(|old| old.as_slice() != pixels.as_slice())
                })
                .map(|(key, _)| *key),
        );
        let size = self.texture.dimensions();
        for (x, y) in changed {
            self.upload.fill(0);
            if let Some(pixels) = next.get(&(x, y)) {
                for (source, destination) in
                    pixels.chunks_exact(4).zip(self.upload.chunks_exact_mut(4))
                {
                    // TileSurface is premultiplied; VideoProcessor consumes straight alpha.
                    let alpha = u32::from(source[3]);
                    for (out, input) in [2, 1, 0].into_iter().enumerate() {
                        destination[out] = (u32::from(source[input]) * 255 + alpha / 2)
                            .checked_div(alpha)
                            .unwrap_or(0)
                            .min(255) as u8;
                    }
                    destination[3] = source[3];
                }
            }
            let rect = D3D11_BOX {
                left: x,
                top: y,
                front: 0,
                right: (x + TILE_SIZE).min(size.0),
                bottom: (y + TILE_SIZE).min(size.1),
                back: 1,
            };
            unsafe {
                self.texture.device().context().UpdateSubresource(
                    self.texture.raw(),
                    0,
                    Some(&rect),
                    self.upload.as_ptr().cast(),
                    TILE_SIZE * 4,
                    0,
                )
            };
            bytes += u64::from((rect.right - x) * (rect.bottom - y) * 4);
        }
        self.previous = next;
        Ok(bytes)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
struct DesktopRevision {
    content: u64,
    configuration: u64,
    size: (u32, u32),
}

pub(super) struct GpuVisualCompositor {
    #[cfg(test)]
    final_rgb: Option<Texture>,
    #[cfg(test)]
    desktop_render_count: u64,
    #[cfg(test)]
    force_empty_overlays: bool,
    #[cfg(any(test, feature = "bench-synthetic-input"))]
    pub inject_failure: bool,
    device: SharedDevice,
    processor: VideoProcessor,
    scratch: [Texture; 2],
    desktop_output: Option<Texture>,
    desktop_revision: Option<DesktopRevision>,
    overlay: Option<OverlayTexture>,
    webcam: Option<WebcamTexture>,
    webcam_upload: Vec<u8>,
    tiles: TileSurface,
    cursor_renderer: CursorRenderer,
    scaler: Option<AreaScaler>,
    crop_copy: Option<CropCopy>,
    native_desktop: Option<NativeDesktop>,
    size: (u32, u32),
    pub overlay_upload_bytes: u64,
}

impl GpuVisualCompositor {
    pub fn memory_usage(&self) -> Result<u64> {
        self.device.video_memory_usage().map_err(gpu_error)
    }
    pub fn new(
        device: SharedDevice,
        source_size: (u32, u32),
        size: (u32, u32),
        fps: u32,
    ) -> Result<Self> {
        let _lock = device.lock();
        let processor =
            VideoProcessor::new(device.clone(), source_size, size, fps).map_err(gpu_error)?;
        let make = || {
            device
                .texture(
                    size.0,
                    size.1,
                    DXGI_FORMAT_B8G8R8X8_UNORM,
                    (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
                )
                .map_err(gpu_error)
        };
        let scratch = [make()?, make()?];
        processor
            .check_conversion(DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_NV12)
            .map_err(gpu_error)?;
        let cursor_renderer = CursorRenderer::new(device.clone())?;
        drop(_lock);
        Ok(Self {
            device,
            #[cfg(test)]
            final_rgb: None,
            #[cfg(test)]
            desktop_render_count: 0,
            #[cfg(test)]
            force_empty_overlays: false,
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            inject_failure: false,
            processor,
            scratch,
            desktop_output: None,
            desktop_revision: None,
            overlay: None,
            webcam: None,
            webcam_upload: Vec::new(),
            tiles: TileSurface::new(size),
            cursor_renderer,
            scaler: None,
            crop_copy: None,
            native_desktop: None,
            size,
            overlay_upload_bytes: 0,
        })
    }

    pub fn compose(
        &mut self,
        state: &mut VisualCompositor,
        config: &DirectRecordingConfig,
        frame: &GpuCapturedFrame,
        timestamp: u64,
        cursor: Option<&AttachedCursorSample>,
        output: &GpuEncoderFrame,
    ) -> Result<()> {
        #[cfg(any(test, feature = "bench-synthetic-input"))]
        if std::mem::take(&mut self.inject_failure) {
            return Err(gpu_error("injected VideoProcessor submission failure"));
        }
        let device = self.device.clone();
        if !device.same_device(output.device()) {
            return Err(gpu_error("encoder output belongs to another GPU device"));
        }
        // CPU rasterization and camera conversion never hold the shared D3D
        // immediate-context lock needed by capture and hardware encoding.
        let source_size = frame.dimensions();
        self.tiles.clear();
        state
            .input_effects
            .trail
            .set_lifetime_ms(config.mouse_trail_duration_ms);
        if config.mouse_trail_rgba[3] != 0 {
            state.input_effects.trail.observe(
                cursor
                    .filter(|cursor| cursor.visible)
                    .map(|cursor| (cursor.x, cursor.y)),
                source_size,
                self.size,
                timestamp,
            );
            state
                .input_effects
                .trail
                .draw_to(&mut self.tiles, timestamp, config.mouse_trail_rgba);
        } else {
            state.input_effects.trail.clear();
        }
        while state
            .input_effects
            .clicks
            .front()
            .is_some_and(|click| timestamp.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS)
        {
            state.input_effects.clicks.pop_front();
        }
        snow_recording_effects::mouse_effects::draw_clicks_to(
            &mut self.tiles,
            &state.input_effects.clicks,
            timestamp,
            config.mouse_click_rgba,
            source_size,
        );
        let mouse_tiles = self.tiles.snapshot();
        self.tiles.clear();
        state.input_effects.advance_keys(timestamp);
        if let Some(keyboard) = state.input_effects.keyboard.as_mut() {
            keyboard
                .draw_to(&mut self.tiles, timestamp)
                .map_err(gpu_error)?;
        }
        let keyboard_tiles = self.tiles.snapshot();
        if let Some(image) = state.webcam.image()
            && self.webcam.as_ref().is_none_or(|webcam| {
                webcam.generation != image.generation
                    || webcam.texture.dimensions() != (image.rect.2, image.rect.2)
            })
        {
            let pixels = image.pixels;
            self.webcam_upload.resize(pixels.len(), 0);
            for (from, to) in pixels
                .chunks_exact(4)
                .zip(self.webcam_upload.chunks_exact_mut(4))
            {
                let alpha = u32::from(from[3]);
                for (out, input) in [2, 1, 0].into_iter().enumerate() {
                    to[out] = ((u32::from(from[input]) * 255 + alpha / 2)
                        .checked_div(alpha)
                        .unwrap_or(0)
                        .min(255)) as u8;
                }
                to[3] = from[3];
            }
        }
        let _lock = device.lock();
        let revision = frame
            .metadata()
            .content_generation()
            .map(|content| DesktopRevision {
                content,
                configuration: frame.metadata().configuration_generation(),
                size: source_size,
            });
        let mut background = self.desktop(&frame.layers, source_size, revision)?;
        let mut index = 0;
        if let Some(image) = state.webcam.image() {
            let (x, y, side) = image.rect;
            let (pixels, generation) = (image.pixels, image.generation);
            if self
                .webcam
                .as_ref()
                .is_none_or(|v| v.texture.dimensions() != (side, side))
            {
                self.webcam = Some(WebcamTexture {
                    texture: device
                        .texture(
                            side,
                            side,
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
                        )
                        .map_err(gpu_error)?,
                    generation: generation.wrapping_sub(1),
                });
            }
            let webcam = self.webcam.as_mut().unwrap();
            if webcam.generation != generation {
                unsafe {
                    device.context().UpdateSubresource(
                        webcam.texture.raw(),
                        0,
                        None,
                        self.webcam_upload.as_ptr().cast(),
                        side * 4,
                        0,
                    );
                }
                self.overlay_upload_bytes += pixels.len() as u64;
                webcam.generation = generation;
            }
            let layer = VideoLayer {
                source: Rect::full((side, side)),
                destination: Rect {
                    x: x as i32,
                    y: y as i32,
                    width: side,
                    height: side,
                },
                texture: webcam.texture.clone(),
                rotation: 0,
                alpha: true,
            };
            self.processor
                .blit(
                    &[self.layer(background.clone(), false), layer],
                    self.scratch[index].raw(),
                    0,
                )
                .map_err(gpu_error)?;
            background = self.scratch[index].clone();
            index = 1 - index;
        }
        if config.show_cursor
            && config.mouse_highlight_rgba[3] != 0
            && let Some(sample) = cursor.filter(|c| {
                c.visible
                    && c.x >= 0
                    && c.y >= 0
                    && c.x < source_size.0 as i32
                    && c.y < source_size.1 as i32
            })
        {
            self.cursor_renderer.highlight(
                &background,
                &self.scratch[index],
                scale_point(sample.x, sample.y, source_size, self.size),
                config.mouse_highlight_rgba,
                config.mouse_highlight_radius,
            )?;
            background = self.scratch[index].clone();
            index = 1 - index;
        }
        if let Some(overlay) = self.upload_overlay(mouse_tiles)? {
            self.processor
                .blit(
                    &[
                        self.layer(background.clone(), false),
                        self.layer(overlay, true),
                    ],
                    self.scratch[index].raw(),
                    0,
                )
                .map_err(|error| gpu_error(format!("effects pass: {error:#}")))?;
            background = self.scratch[index].clone();
            index = 1 - index;
        }
        if config.show_cursor
            && let Some(cursor) = cursor
        {
            let shape = resolve_cursor_shape(&mut state.cursor_shape, cursor).cloned();
            if cursor.visible
                && let Some(shape) = shape
            {
                self.overlay_upload_bytes += self.cursor_renderer.cursor(
                    &background,
                    &self.scratch[index],
                    cursor,
                    &shape,
                    source_size,
                )?;
                background = self.scratch[index].clone();
                index = 1 - index;
            }
        }
        if let Some(overlay) = self.upload_overlay(keyboard_tiles)? {
            self.processor
                .blit(
                    &[self.layer(background, false), self.layer(overlay, true)],
                    self.scratch[index].raw(),
                    0,
                )
                .map_err(|error| gpu_error(format!("keyboard pass: {error:#}")))?;
            background = self.scratch[index].clone();
        }
        #[cfg(test)]
        {
            self.final_rgb = Some(background.clone());
        }
        self.processor
            .blit(
                &[self.layer(background, false)],
                &output.texture()?,
                output.slice(),
            )
            .map_err(|error| gpu_error(format!("NV12 pass: {error:#}")))
    }

    fn upload_overlay(&mut self, tiles: Vec<Tile>) -> Result<Option<Texture>> {
        let empty = tiles.is_empty();
        #[cfg(test)]
        let empty = empty && !self.force_empty_overlays;
        if empty {
            // The texture is not sampled by this pass. Keep its last contents
            // and diff against them when effects reappear; no clear or full-frame
            // blend is needed while empty. Allocate only after a visible effect.
            return Ok(None);
        }
        if self.overlay.is_none() {
            self.overlay = Some(OverlayTexture::new(&self.device, self.size)?);
        }
        let overlay = self.overlay.as_mut().unwrap();
        self.overlay_upload_bytes += overlay.upload(tiles)?;
        Ok(Some(overlay.texture.clone()))
    }

    fn layer(&self, texture: Texture, alpha: bool) -> VideoLayer {
        VideoLayer {
            source: Rect::full(texture.dimensions()),
            destination: Rect::full(self.size),
            texture,
            rotation: 0,
            alpha,
        }
    }

    /// The clean desktop has its own output, so overlay passes cannot dirty it.
    /// Swap it into the existing render targets while updating: changed frames
    /// incur no extra full-frame copy, and overlay-only frames reuse the result.
    fn desktop(
        &mut self,
        layers: &[VideoLayer],
        source_size: (u32, u32),
        revision: Option<DesktopRevision>,
    ) -> Result<Texture> {
        if revision.is_some() && self.desktop_revision == revision {
            return Ok(self
                .desktop_output
                .as_ref()
                .expect("cached desktop")
                .clone());
        }
        if self.desktop_output.is_none() {
            self.desktop_output = Some(
                self.device
                    .texture(
                        self.size.0,
                        self.size.1,
                        DXGI_FORMAT_B8G8R8X8_UNORM,
                        (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
                    )
                    .map_err(gpu_error)?,
            );
        }
        self.desktop_revision = None;
        std::mem::swap(&mut self.scratch[0], self.desktop_output.as_mut().unwrap());
        let rendered = self.render_desktop(layers, source_size);
        if matches!(rendered, Ok(1)) {
            self.scratch.swap(0, 1);
        }
        std::mem::swap(&mut self.scratch[0], self.desktop_output.as_mut().unwrap());
        rendered?;
        self.desktop_revision = revision;
        Ok(self.desktop_output.as_ref().unwrap().clone())
    }

    /// Resize the complete desktop once in RGB. Full native frames need no
    /// filtering intermediate; explicit crops lazily allocate exact-copy storage.
    fn render_desktop(&mut self, layers: &[VideoLayer], source_size: (u32, u32)) -> Result<usize> {
        #[cfg(test)]
        {
            self.desktop_render_count += 1;
        }
        if source_size.0 <= self.size.0 && source_size.1 <= self.size.1 {
            self.scaler = None;
            if source_size == self.size {
                if layers.len() == 1
                    && layers[0].source == Rect::full(layers[0].texture.dimensions())
                    && layers[0].destination == Rect::full(self.size)
                    && layers[0].rotation == 0
                {
                    if let Some(copy) = self.crop_copy.as_mut() {
                        copy.clear_views();
                    }
                    self.native_desktop = None;
                    return composite_desktop(&self.processor, &self.scratch, layers, self.size);
                }
                if self.crop_copy.is_none() {
                    self.crop_copy = Some(CropCopy::new(self.device.clone())?);
                }
                if self
                    .native_desktop
                    .as_ref()
                    .is_none_or(|desktop| desktop.size != source_size)
                {
                    self.native_desktop = Some(NativeDesktop::new(&self.device, source_size)?);
                }
                let native = self.native_desktop.as_ref().unwrap().texture.clone();
                self.crop_copy
                    .as_mut()
                    .unwrap()
                    .copy_into(layers, &native)?;
                self.processor
                    .blit(&[self.layer(native, false)], self.scratch[0].raw(), 0)
                    .map_err(gpu_error)?;
                return Ok(0);
            }
            if let Some(copy) = self.crop_copy.as_mut() {
                copy.clear_views();
            }
            self.native_desktop = None;
            let mut enlarged = layers.to_vec();
            for layer in &mut enlarged {
                let left = scale_coordinate(layer.destination.x, source_size.0, self.size.0);
                let top = scale_coordinate(layer.destination.y, source_size.1, self.size.1);
                let right = scale_coordinate(
                    layer.destination.x + layer.destination.width as i32,
                    source_size.0,
                    self.size.0,
                );
                let bottom = scale_coordinate(
                    layer.destination.y + layer.destination.height as i32,
                    source_size.1,
                    self.size.1,
                );
                layer.destination = Rect {
                    x: left,
                    y: top,
                    width: (right - left).max(0) as u32,
                    height: (bottom - top).max(0) as u32,
                };
            }
            enlarged.retain(|layer| layer.destination.width != 0 && layer.destination.height != 0);
            return composite_desktop(&self.processor, &self.scratch, &enlarged, self.size);
        }
        let direct = layers.first().filter(|layer| {
            let oriented = if layer.rotation.is_multiple_of(2) {
                (layer.source.width, layer.source.height)
            } else {
                (layer.source.height, layer.source.width)
            };
            layers.len() == 1
                && !layer.alpha
                && layer.destination == Rect::full(source_size)
                && oriented == source_size
        });
        let source = if let Some(layer) = direct {
            if self.native_desktop.take().is_some()
                && let Some(scaler) = self.scaler.as_mut()
            {
                scaler.clear_source_views();
            }
            if let Some(copy) = self.crop_copy.as_mut() {
                copy.clear_views();
            }
            // The compute shader clamps every tap to the explicit crop; driver
            // filtering can otherwise leak neighboring monitor pixels into it.
            layer.clone()
        } else {
            if self
                .native_desktop
                .as_ref()
                .is_none_or(|desktop| desktop.size != source_size)
            {
                self.native_desktop = Some(NativeDesktop::new(&self.device, source_size)?);
            }
            if self.crop_copy.is_none() {
                self.crop_copy = Some(CropCopy::new(self.device.clone())?);
            }
            let desktop = self.native_desktop.as_ref().unwrap();
            self.crop_copy
                .as_mut()
                .unwrap()
                .copy_into(layers, &desktop.texture)?;
            VideoLayer {
                texture: desktop.texture.clone(),
                source: Rect::full(source_size),
                destination: Rect::full(self.size),
                rotation: 0,
                alpha: false,
            }
        };
        if self.scaler.is_none() {
            self.scaler = Some(AreaScaler::new(self.device.clone())?);
        }
        let filtered = self.scaler.as_mut().unwrap().resize(&source, self.size)?;
        self.processor
            .blit(&[self.layer(filtered, false)], self.scratch[0].raw(), 0)
            .map_err(|error| gpu_error(format!("filtered desktop pass: {error:#}")))?;
        Ok(0)
    }
}

struct NativeDesktop {
    size: (u32, u32),
    texture: Texture,
}

impl NativeDesktop {
    fn new(device: &SharedDevice, size: (u32, u32)) -> Result<Self> {
        Ok(Self {
            size,
            texture: device
                .texture(
                    size.0,
                    size.1,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    (D3D11_BIND_SHADER_RESOURCE
                        | D3D11_BIND_UNORDERED_ACCESS
                        | D3D11_BIND_RENDER_TARGET)
                        .0 as u32,
                )
                .map_err(gpu_error)?,
        })
    }
}

fn composite_desktop(
    processor: &VideoProcessor,
    scratch: &[Texture; 2],
    layers: &[VideoLayer],
    size: (u32, u32),
) -> Result<usize> {
    let mut index = 0;
    let mut consumed = 0;
    while consumed < layers.len() {
        let mut batch = Vec::with_capacity(processor.max_streams().min(layers.len() + 1));
        if consumed != 0 {
            batch.push(VideoLayer {
                texture: scratch[1 - index].clone(),
                source: Rect::full(size),
                destination: Rect::full(size),
                rotation: 0,
                alpha: false,
            });
        }
        let count = (processor.max_streams() - batch.len()).min(layers.len() - consumed);
        batch.extend_from_slice(&layers[consumed..consumed + count]);
        processor
            .blit(&batch, scratch[index].raw(), 0)
            .map_err(|error| gpu_error(format!("desktop pass: {error:#}")))?;
        consumed += count;
        if consumed < layers.len() {
            index = 1 - index;
        }
    }
    if layers.is_empty() {
        processor
            .blit(&[], scratch[index].raw(), 0)
            .map_err(|error| gpu_error(format!("desktop pass: {error:#}")))?;
    }
    Ok(index)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mouse_hook::ObservedMouseButton;

    fn hardware_vendor(vendor: u32) -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let identity = snow_d3d11::AdapterIdentity::inspect(&adapter).map_err(gpu_error)?;
        if identity.vendor != vendor {
            eprintln!(
                "CAPABILITY SKIP: {} requires vendor {vendor:#06x}; primary adapter is {} ({:#06x})",
                snow_d3d11::h264_encoder(vendor).unwrap(),
                identity.description,
                identity.vendor
            );
            return Ok(());
        }
        hardware_dxgi_and_wgc_record_native_surfaces()
    }

    #[test]
    #[ignore = "requires NVIDIA hardware and an interactive desktop; prints capability skip otherwise"]
    fn hardware_nvenc_native_recording() -> Result<()> {
        hardware_vendor(0x10de)
    }

    #[test]
    #[ignore = "requires Intel hardware and an interactive desktop; prints capability skip otherwise"]
    fn hardware_qsv_native_recording() -> Result<()> {
        hardware_vendor(0x8086)
    }

    fn config(path: PathBuf, backend: CaptureBackendKind) -> DirectRecordingConfig {
        DirectRecordingConfig {
            keyboard_filter: Default::default(),
            webcam: None,
            audio_mode: Default::default(),
            system_audio_gain_db: 0,
            microphone_gain_db: 0,
            excluded_windows: Default::default(),
            excluded_processes: Default::default(),
            loop_animated_images: true,
            region: RecordingRegion {
                x: 0,
                y: 0,
                width: 640,
                height: 480,
            },
            capture_backend: backend,
            output_path: path,
            format: ExportFormat::Mp4,
            capture_fps: 30,
            output_fps: 30,
            maximum_width: None,
            maximum_height: None,
            codec: VideoCodec::H264,
            preset: VideoEncodingSpeed::VeryFast,
            quality: 80,
            prefer_hardware_encoder: true,
            enable_microphone: false,
            enable_system_audio: false,
            show_cursor: true,
            keyboard: None,
            mouse_trail_rgba: [255, 0, 0, 180],
            mouse_trail_duration_ms: 400,
            mouse_click_rgba: [255, 0, 0, 180],
            mouse_highlight_radius: snow_recording_model::render::DEFAULT_MOUSE_HIGHLIGHT_RADIUS,
            mouse_highlight_rgba: [0; 4],
            record_mouse_clicks: false,
            record_mouse_scrolling: false,
            show_keyboard: true,
        }
    }

    #[test]
    #[ignore = "requires an offscreen D3D11 VideoProcessor"]
    fn hardware_desktop_cache_survives_overlay_targets_and_invalidates_source_changes() -> Result<()>
    {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let output = (64, 48);
        for source in [output, (128, 96)] {
            // Capture layers use source coordinates; the compositor owns the resize.
            let layer = downscale_pattern_layer(
                &device,
                source,
                source,
                DownscalePattern::Checkerboard,
                0,
                0,
                false,
            )?;
            let mut compositor = GpuVisualCompositor::new(device.clone(), source, output, 60)?;
            let revision = DesktopRevision {
                content: 1,
                configuration: 1,
                size: source,
            };
            let first = compositor.desktop(std::slice::from_ref(&layer), source, Some(revision))?;
            let expected = read_downscale_rgb(&device, &first)?;
            assert_eq!(compositor.desktop_render_count, 1);
            let poison = [17u8; 64 * 48 * 4];
            for texture in &compositor.scratch {
                unsafe {
                    device.context().UpdateSubresource(
                        texture.raw(),
                        0,
                        None,
                        poison.as_ptr().cast(),
                        output.0 * 4,
                        0,
                    );
                }
            }
            let held = compositor.desktop(std::slice::from_ref(&layer), source, Some(revision))?;
            assert_eq!(compositor.desktop_render_count, 1);
            assert_eq!(read_downscale_rgb(&device, &held)?, expected);
            for revision in [
                Some(DesktopRevision {
                    content: 2,
                    ..revision
                }),
                Some(DesktopRevision {
                    content: 2,
                    configuration: 2,
                    ..revision
                }),
                None,
                None,
            ] {
                let before = compositor.desktop_render_count;
                let frame = compositor.desktop(std::slice::from_ref(&layer), source, revision)?;
                assert_eq!(compositor.desktop_render_count, before + 1);
                assert_eq!(read_downscale_rgb(&device, &frame)?, expected);
            }
        }
        Ok(())
    }

    #[test]
    #[ignore = "requires offscreen D3D11 VideoProcessor and a native H.264 encoder"]
    fn hardware_sparse_effects_and_webcam_match_cpu_draw_order_and_expiration() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        struct Rasterizer;
        impl crate::keyboard_overlay::KeycapRasterizer for Rasterizer {
            fn rasterize(
                &mut self,
                _: &str,
                _: f32,
            ) -> std::result::Result<crate::keyboard_overlay::Keycap, String> {
                Ok(crate::keyboard_overlay::Keycap {
                    width: 40,
                    height: 20,
                    pixels: [30, 140, 60, 180].repeat(800),
                })
            }
        }
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let size = (320, 180);
        let directory = tempfile::tempdir()?;
        let mut config = config(
            directory.path().join("surface.mp4"),
            CaptureBackendKind::Auto,
        );
        config.region.width = size.0;
        config.region.height = size.1;
        config.mouse_click_rgba = [20, 220, 80, 180];
        let encoder = StreamingEncoder::builder(config.streaming_config())
            .gpu_input(snow_recording_export::gpu::GpuInputConfig {
                device: device.clone(),
            })
            .create()?;
        let surface = encoder.allocate_gpu_frame()?.unwrap();
        let texture = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8X8_UNORM,
                (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE).0 as u32,
            )
            .map_err(gpu_error)?;
        let original = [20, 40, 60, 255].repeat((size.0 * size.1) as usize);
        let bgra = [60u8, 40, 20, 255].repeat((size.0 * size.1) as usize);
        unsafe {
            device.context().UpdateSubresource(
                texture.raw(),
                0,
                None,
                bgra.as_ptr().cast(),
                size.0 * 4,
                0,
            );
        }
        let observation = GpuCapturedFrame::from_layers(
            vec![VideoLayer {
                texture,
                source: Rect::full(size),
                destination: Rect::full(size),
                rotation: 0,
                alpha: false,
            }],
            size,
            snow_capture::FrameMetadata::default(),
        )?;
        let cpu_frame: CapturedFrame =
            snow_capture::frame::Frame::from_rgba8(size.0, size.1, original)?.into();
        let mut gpu = GpuVisualCompositor::new(device.clone(), size, size, 30)?;
        let mut cpu_state = VisualCompositor::new(size);
        let mut gpu_state = VisualCompositor::new(size);
        gpu.compose(&mut gpu_state, &config, &observation, 0, None, &surface)?;
        assert!(
            gpu.overlay.is_none(),
            "empty effects must not allocate a texture"
        );
        assert_eq!(gpu.overlay_upload_bytes, 0);
        for pixel in read_downscale_rgb(&device, gpu.final_rgb.as_ref().unwrap())?.chunks_exact(4) {
            assert_eq!(&pixel[..3], &bgra[..3]);
        }
        for state in [&mut cpu_state, &mut gpu_state] {
            state.input_effects.keyboard = Some(KeyboardOverlay::new(size, Box::new(Rasterizer)));
            for (at_ms, down) in [(0, true), (150, false)] {
                state.input_effects.pending_keys.push_back(KeyEvent {
                    at_ms,
                    key: 65,
                    down,
                    label: "A".into(),
                    modifiers: vec![],
                });
            }
            state.input_effects.clicks.push_back(RenderClick {
                timestamp_ms: 50,
                x: 160,
                y: 150,
                button: ObservedMouseButton::Left,
            });
        }
        let shape = CursorShape::from_rgba(
            0,
            0,
            8,
            8,
            CursorCompositionMode::AlphaBlend,
            [230, 80, 10, 160].repeat(64),
        );
        for (index, timestamp) in [0, 33, 66, 100, 150, 250, 400, 600, 1800, 2200, 2400]
            .into_iter()
            .enumerate()
        {
            if timestamp >= 2200 {
                config.mouse_trail_rgba = [0; 4];
            }
            if timestamp == 2400 {
                for state in [&mut cpu_state, &mut gpu_state] {
                    state.input_effects.clicks.push_back(RenderClick {
                        timestamp_ms: timestamp,
                        x: 30,
                        y: 30,
                        button: ObservedMouseButton::Left,
                    });
                }
            }
            let webcam = snow_webcam::Frame {
                revision: index as u64 + 1,
                at: Instant::now(),
                size: 100,
                rgba: Arc::new(
                    (0..100 * 100)
                        .flat_map(|pixel| {
                            [
                                ((pixel % 100) * 2) as u8,
                                ((pixel / 100) * 2) as u8,
                                index as u8 * 20,
                                255,
                            ]
                        })
                        .collect(),
                ),
            };
            for state in [&mut cpu_state, &mut gpu_state] {
                state.webcam.prepare(
                    (timestamp < 1800).then_some(&webcam),
                    snow_webcam::WebcamStyle {
                        size: 150,
                        shape: if index < 4 {
                            snow_webcam::WebcamShape::Square
                        } else {
                            snow_webcam::WebcamShape::Circle
                        },
                        offset_left: 32 + index as u32 * 12,
                        offset_bottom: 32 + index as u32 * 6,
                        orientation: snow_webcam::WebcamOrientation::from_raw(index as u32 % 8)
                            .unwrap(),
                    },
                    size,
                );
            }
            let cursor = AttachedCursorSample {
                x: 145 + index as i32 * 2,
                y: 150,
                visible: !(400..2200).contains(&timestamp),
                shape: CursorShapeState::Embedded(shape.clone()),
            };
            let expected =
                cpu_state.compose_with_cursor(&config, &cpu_frame, timestamp, Some(&cursor))?;
            let uploaded_before = gpu.overlay_upload_bytes;
            gpu.compose(
                &mut gpu_state,
                &config,
                &observation,
                timestamp,
                Some(&cursor),
                &surface,
            )?;
            if timestamp == 2200 {
                assert_eq!(gpu.overlay_upload_bytes, uploaded_before);
                assert_eq!(
                    gpu.final_rgb.as_ref().unwrap().desc().Format,
                    DXGI_FORMAT_B8G8R8X8_UNORM
                );
            }
            let final_rgb = gpu.final_rgb.as_ref().unwrap();
            let pixels = read_downscale_rgb(&device, final_rgb)?;
            let channels = if final_rgb.desc().Format == DXGI_FORMAT_R8G8B8A8_UNORM {
                [0, 1, 2]
            } else {
                [2, 1, 0]
            };
            let mut max_error = 0;
            for (pixel, reference) in pixels.chunks_exact(4).zip(expected.chunks_exact(4)) {
                for (channel, component) in channels.into_iter().enumerate() {
                    max_error = max_error.max(pixel[component].abs_diff(reference[channel]));
                }
            }
            assert!(
                max_error <= 4,
                "overlay RGB error {max_error} at {timestamp} ms"
            );
        }
        assert!(gpu.overlay_upload_bytes > 0);
        assert!(
            gpu_state.background.is_empty() && gpu_state.rgba.is_empty(),
            "GPU composition must not materialize captured pixels"
        );
        Ok(())
    }

    #[test]
    fn only_hardware_video_direct_mp4_uses_native_surfaces() {
        let mut config = config(PathBuf::from("test.mp4"), CaptureBackendKind::Auto);
        assert!(eligible(&config));
        config.codec = VideoCodec::H265;
        assert!(eligible(&config));
        config.prefer_hardware_encoder = false;
        assert!(!eligible(&config));
        config.prefer_hardware_encoder = true;
        config.format = ExportFormat::Gif;
        assert!(!eligible(&config));
    }

    #[test]
    #[ignore = "requires offscreen D3D11 video processing and native H.264/HEVC encoders"]
    fn hardware_encoded_cursor_transitions_preserve_red_and_blue() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let size = (320, 240);
        let texture = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8X8_UNORM,
                (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE).0 as u32,
            )
            .map_err(gpu_error)?;
        // Saturated asymmetric colors expose a frame-wide swap that grayscale
        // cursor fixtures and successful codec submission cannot detect.
        let colors = [
            [0u8, 0, 255, 255],
            [255, 0, 0, 255],
            [0, 255, 0, 255],
            [255, 255, 255, 255],
        ];
        let pixels: Vec<_> = (0..size.1)
            .flat_map(|y| (0..size.0).flat_map(move |x| colors[((y / 120) * 2 + x / 160) as usize]))
            .collect();
        {
            let _lock = device.lock();
            unsafe {
                device.context().UpdateSubresource(
                    texture.raw(),
                    0,
                    None,
                    pixels.as_ptr().cast(),
                    size.0 * 4,
                    0,
                );
            }
        }
        let observation = GpuCapturedFrame::from_layers(
            vec![VideoLayer {
                texture,
                source: Rect::full(size),
                destination: Rect::full(size),
                rotation: 0,
                alpha: false,
            }],
            size,
            snow_capture::FrameMetadata::default(),
        )?;
        let directory = tempfile::tempdir()?;
        for codec in [VideoCodec::H264, VideoCodec::H265] {
            let path = directory
                .path()
                .join(format!("cursor-colors-{codec:?}.mp4"));
            let mut config = config(path.clone(), CaptureBackendKind::Auto);
            config.region.width = size.0;
            config.region.height = size.1;
            config.codec = codec;
            config.quality = 90;
            config.show_keyboard = false;
            config.mouse_trail_rgba = [0; 4];
            config.mouse_click_rgba = [0; 4];
            config.mouse_highlight_radius = 20;
            let mut encoder = StreamingEncoder::builder(config.streaming_config())
                .gpu_input(snow_recording_export::gpu::GpuInputConfig {
                    device: device.clone(),
                })
                .create()?;
            let mut gpu = GpuVisualCompositor::new(device.clone(), size, size, 30)?;
            let mut state = VisualCompositor::new(size);
            // Hidden, monochrome, blue alpha, blue masked, highlight, hidden,
            // blue again: exercise both channel layouts and cached resources.
            let stages = [0, 1, 2, 3, 4, 0, 2, 0];
            for (pts, stage) in stages.into_iter().enumerate() {
                let mode = if stage == 3 {
                    CursorCompositionMode::MaskedColor
                } else {
                    CursorCompositionMode::AlphaBlend
                };
                let color = if stage == 1 {
                    [255, 255, 255, 255]
                } else {
                    [0, 0, 255, if stage == 3 { 0 } else { 255 }]
                };
                let cursor = AttachedCursorSample {
                    x: 64,
                    y: 64,
                    visible: stage != 0,
                    shape: CursorShapeState::Embedded(CursorShape::from_rgba(
                        0,
                        0,
                        32,
                        32,
                        mode,
                        color.repeat(32 * 32),
                    )),
                };
                config.mouse_highlight_rgba = if stage == 4 {
                    [30, 255, 80, 150]
                } else {
                    [0; 4]
                };
                let surface = encoder
                    .allocate_gpu_frame()?
                    .ok_or_else(|| gpu_error("color fixture exhausted encoder surfaces"))?;
                gpu.compose(
                    &mut state,
                    &config,
                    &observation,
                    pts as u64 * 1000 / 30,
                    Some(&cursor),
                    &surface,
                )?;
                assert_eq!(
                    gpu.final_rgb.as_ref().unwrap().desc().Format,
                    DXGI_FORMAT_B8G8R8X8_UNORM
                );
                encoder.push_gpu_frame_at_pts(pts as u64, surface)?;
            }
            let report = encoder.finish_at_pts(stages.len() as u64)?;
            assert!(
                report.used_hardware_video_encoder && !report.hardware_fallback,
                "{report:?}"
            );
            let mut media = ffmpeg_next::format::input(&path).map_err(gpu_error)?;
            let stream = media
                .streams()
                .best(ffmpeg_next::media::Type::Video)
                .ok_or_else(|| gpu_error("missing cursor color video"))?;
            let video_index = stream.index();
            let mut decoder =
                ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                    .map_err(gpu_error)?
                    .decoder()
                    .video()
                    .map_err(gpu_error)?;
            let mut frame = ffmpeg_next::frame::Video::empty();
            let mut decoded = 0;
            let mut observe = |frame: &ffmpeg_next::frame::Video| {
                assert_eq!(frame.format(), ffmpeg_next::format::Pixel::YUV420P);
                let sample = |x: usize, y: usize, expected: [u8; 3]| {
                    let actual = [
                        frame.data(0)[y * frame.stride(0) + x],
                        frame.data(1)[(y / 2) * frame.stride(1) + x / 2],
                        frame.data(2)[(y / 2) * frame.stride(2) + x / 2],
                    ];
                    assert!(
                        actual
                            .into_iter()
                            .zip(expected)
                            .all(|(a, e)| a.abs_diff(e) <= 8),
                        "{codec:?} frame {decoded} at {x},{y}: {actual:?}, expected {expected:?}"
                    );
                };
                // BT.709 limited-range primary colors, sampled away from edges
                // and overlays; these must remain unchanged on every frame.
                sample(24, 24, [63, 102, 240]);
                sample(280, 24, [32, 240, 118]);
                sample(24, 200, [173, 42, 26]);
                sample(280, 200, [235, 128, 128]);
                sample(
                    80,
                    80,
                    match stages[decoded] {
                        0 => [63, 102, 240],
                        1 => [235, 128, 128],
                        _ => [32, 240, 118],
                    },
                );
                decoded += 1;
            };
            for (stream, packet) in media.packets() {
                if stream.index() == video_index {
                    decoder.send_packet(&packet).map_err(gpu_error)?;
                    while decoder.receive_frame(&mut frame).is_ok() {
                        observe(&frame);
                    }
                }
            }
            decoder.send_eof().map_err(gpu_error)?;
            while decoder.receive_frame(&mut frame).is_ok() {
                observe(&frame);
            }
            assert_eq!(decoded, stages.len());
        }
        Ok(())
    }

    #[derive(Clone, Copy, Debug)]
    enum DownscalePattern {
        VerticalStripes,
        HorizontalStripes,
        Checkerboard,
        VerticalLine,
        HorizontalLine,
        VerticalStems,
    }

    impl DownscalePattern {
        fn value(self, x: u32, y: u32, size: (u32, u32), phase: u32) -> u8 {
            let white = match self {
                Self::VerticalStripes => (x + phase).is_multiple_of(2),
                Self::HorizontalStripes => (y + phase).is_multiple_of(2),
                Self::Checkerboard => (x + y + phase).is_multiple_of(2),
                Self::VerticalLine => x == size.0 / 2 + phase,
                Self::HorizontalLine => y == size.1 / 2 + phase,
                Self::VerticalStems => x % 8 == phase,
            };
            if white { 255 } else { 0 }
        }
    }

    fn downscale_area_reference(
        pattern: DownscalePattern,
        source: (u32, u32),
        output: (u32, u32),
        phase: u32,
    ) -> Vec<f64> {
        let scale = (
            f64::from(source.0) / f64::from(output.0),
            f64::from(source.1) / f64::from(output.1),
        );
        let mut reference = Vec::with_capacity((output.0 * output.1) as usize);
        for y in 0..output.1 {
            let (top, bottom) = if source.1 >= output.1 {
                (f64::from(y) * scale.1, f64::from(y + 1) * scale.1)
            } else {
                let point = (f64::from(y) * scale.1).floor();
                (point, point + 1.0)
            };
            for x in 0..output.0 {
                let (left, right) = if source.0 >= output.0 {
                    (f64::from(x) * scale.0, f64::from(x + 1) * scale.0)
                } else {
                    let point = (f64::from(x) * scale.0).floor();
                    (point, point + 1.0)
                };
                let mut total = 0.0;
                for sy in top.floor() as u32..bottom.ceil() as u32 {
                    let height = bottom.min(f64::from(sy + 1)) - top.max(f64::from(sy));
                    for sx in left.floor() as u32..right.ceil() as u32 {
                        let width = right.min(f64::from(sx + 1)) - left.max(f64::from(sx));
                        total += f64::from(pattern.value(sx, sy, source, phase)) * width * height;
                    }
                }
                reference.push(total / ((right - left) * (bottom - top)));
            }
        }
        reference
    }

    fn read_downscale_rgb(device: &SharedDevice, texture: &Texture) -> Result<Vec<u8>> {
        let mut desc = texture.desc();
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
        let mut staging = None;
        unsafe {
            device
                .device()
                .CreateTexture2D(&desc, None, Some(&mut staging))
        }
        .map_err(gpu_error)?;
        let staging = staging.ok_or_else(|| gpu_error("downscale staging texture"))?;
        unsafe { device.context().CopyResource(&staging, texture.raw()) };
        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        unsafe {
            device
                .context()
                .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
        }
        .map_err(gpu_error)?;
        let mut pixels = Vec::with_capacity((desc.Width * desc.Height * 4) as usize);
        for row in 0..desc.Height as usize {
            pixels.extend_from_slice(unsafe {
                std::slice::from_raw_parts(
                    mapped
                        .pData
                        .cast::<u8>()
                        .add(row * mapped.RowPitch as usize),
                    desc.Width as usize * 4,
                )
            });
        }
        unsafe { device.context().Unmap(&staging, 0) };
        Ok(pixels)
    }

    fn downscale_pattern_layer(
        device: &SharedDevice,
        source: (u32, u32),
        output: (u32, u32),
        pattern: DownscalePattern,
        phase: u32,
        rotation: u32,
        cropped: bool,
    ) -> Result<VideoLayer> {
        let native = if rotation.is_multiple_of(2) {
            source
        } else {
            (source.1, source.0)
        };
        let origin = if cropped { (7u32, 5u32) } else { (0, 0) };
        let padding = if cropped { (19, 13) } else { (0, 0) };
        let size = (native.0 + padding.0, native.1 + padding.1);
        let texture = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8X8_UNORM,
                (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE).0 as u32,
            )
            .map_err(gpu_error)?;
        // Magenta pixels outside the crop reveal source-rectangle leakage.
        let mut pixels = [255u8, 0, 255, 255].repeat((size.0 * size.1) as usize);
        for y in 0..native.1 {
            for x in 0..native.0 {
                let (dx, dy) = match rotation {
                    0 => (x, y),
                    1 => (native.1 - 1 - y, x),
                    2 => (native.0 - 1 - x, native.1 - 1 - y),
                    _ => (y, native.0 - 1 - x),
                };
                let value = pattern.value(dx, dy, source, phase);
                let offset = (((y + origin.1) * size.0 + x + origin.0) * 4) as usize;
                pixels[offset..offset + 4].copy_from_slice(&[value, value, value, 255]);
            }
        }
        unsafe {
            device.context().UpdateSubresource(
                texture.raw(),
                0,
                None,
                pixels.as_ptr().cast(),
                size.0 * 4,
                0,
            );
        }
        Ok(VideoLayer {
            texture,
            source: Rect {
                x: origin.0 as i32,
                y: origin.1 as i32,
                width: native.0,
                height: native.1,
            },
            destination: Rect::full(output),
            rotation,
            alpha: false,
        })
    }

    #[test]
    fn downscale_quality_reference_integrates_both_line_phases() {
        let output = (128, 96);
        for source in [(256, 192), (192, 144)] {
            for pattern in [
                DownscalePattern::VerticalStripes,
                DownscalePattern::HorizontalStripes,
                DownscalePattern::Checkerboard,
                DownscalePattern::VerticalLine,
                DownscalePattern::HorizontalLine,
            ] {
                let expected_energy = match pattern {
                    DownscalePattern::VerticalLine => {
                        255.0 * f64::from(output.0 * output.1) / f64::from(source.0)
                    }
                    DownscalePattern::HorizontalLine => {
                        255.0 * f64::from(output.0 * output.1) / f64::from(source.1)
                    }
                    _ => 127.5 * f64::from(output.0 * output.1),
                };
                for phase in 0..2 {
                    let reference = downscale_area_reference(pattern, source, output, phase);
                    assert!(
                        (reference.iter().sum::<f64>() - expected_energy).abs() < 1e-6,
                        "area reference {pattern:?}, {source:?}, phase {phase}"
                    );
                }
            }
        }
    }

    #[test]
    #[ignore = "requires an offscreen D3D11 hardware video processor"]
    fn hardware_desktop_downscale_preserves_phase_energy_crop_and_rotation() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let output = (128, 96);
        let mut failures = Vec::new();
        let mut validator = AreaScaler::new(device.clone())?;
        let valid = downscale_pattern_layer(
            &device,
            (256, 192),
            output,
            DownscalePattern::Checkerboard,
            0,
            0,
            true,
        )?;
        for (x, y, width, height, rotation) in [
            (-1, 5, 256, 192, 0),
            (7, -1, 256, 192, 0),
            (7, 5, 0, 192, 0),
            (7, 5, 256, 0, 0),
            (7, 5, 300, 192, 0),
            (7, 5, 256, 300, 0),
            (7, 5, 256, 192, 4),
        ] {
            let mut invalid = valid.clone();
            invalid.source = Rect {
                x,
                y,
                width,
                height,
            };
            invalid.rotation = rotation;
            assert!(validator.resize(&invalid, output).is_err());
        }
        assert!(validator.resize(&valid, (0, output.1)).is_err());
        assert!(validator.resize(&valid, (output.0, 0)).is_err());
        let foreign_device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let foreign = downscale_pattern_layer(
            &foreign_device,
            (256, 192),
            output,
            DownscalePattern::Checkerboard,
            0,
            0,
            true,
        )?;
        assert!(validator.resize(&foreign, output).is_err());
        drop(validator);
        for source in [(256, 192), (192, 144)] {
            let mut compositor = GpuVisualCompositor::new(device.clone(), source, output, 30)?;
            for pattern in [
                DownscalePattern::VerticalStripes,
                DownscalePattern::HorizontalStripes,
                DownscalePattern::Checkerboard,
                DownscalePattern::VerticalLine,
                DownscalePattern::HorizontalLine,
            ] {
                let mut phases = [0.0; 2];
                for phase in 0..2 {
                    let reference = downscale_area_reference(pattern, source, output, phase);
                    let reference_energy = reference.iter().sum::<f64>();
                    let mut baseline = None::<Vec<u8>>;
                    for cropped in [false, true] {
                        for rotation in 0..4 {
                            let mut layer = downscale_pattern_layer(
                                &device, source, output, pattern, phase, rotation, cropped,
                            )?;
                            layer.destination = Rect::full(source);
                            let index = compositor.render_desktop(&[layer], source)?;
                            assert_eq!(compositor.scaler.as_ref().unwrap().cached_view_count(), 1);
                            let pixels = read_downscale_rgb(&device, &compositor.scratch[index])?;
                            let energy = pixels
                                .chunks_exact(4)
                                .map(|pixel| f64::from(pixel[1]))
                                .sum::<f64>();
                            let ratio = energy / reference_energy;
                            let area_mae = pixels
                                .chunks_exact(4)
                                .zip(&reference)
                                .map(|(pixel, expected)| (f64::from(pixel[1]) - expected).abs())
                                .sum::<f64>()
                                / f64::from(output.0 * output.1);
                            let area_max = pixels
                                .chunks_exact(4)
                                .zip(&reference)
                                .map(|(pixel, expected)| (f64::from(pixel[1]) - expected).abs())
                                .fold(0.0f64, f64::max);
                            let color_leak = pixels
                                .chunks_exact(4)
                                .map(|pixel| {
                                    pixel[0].abs_diff(pixel[1]).max(pixel[2].abs_diff(pixel[1]))
                                })
                                .max()
                                .unwrap();
                            let geometry_error = baseline.as_ref().map_or(0, |baseline| {
                                pixels
                                    .chunks_exact(4)
                                    .zip(baseline.chunks_exact(4))
                                    .flat_map(|(actual, expected)| {
                                        (0..3).map(move |channel| {
                                            actual[channel].abs_diff(expected[channel])
                                        })
                                    })
                                    .max()
                                    .unwrap()
                            });
                            eprintln!(
                                "GPU downscale {source:?}->{output:?} {pattern:?} phase={phase} \
                                 cropped={cropped} rotation={rotation}: energy_ratio={ratio:.4} \
                                 area_mae={area_mae:.3} area_max={area_max:.3} color_leak={color_leak} \
                                 geometry_error={geometry_error}"
                            );
                            // Integrated brightness must remain stable when a one-pixel
                            // detail crosses the sampling lattice. Nearest selection
                            // loses one 2:1 phase completely and doubles the other.
                            if !(0.98..=1.02).contains(&ratio)
                                || area_mae > 2.0
                                || area_max > 2.0
                                || color_leak > 2
                                || geometry_error > 4
                            {
                                failures.push(format!(
                                    "{source:?} {pattern:?} phase {phase}, crop {cropped}, \
                                     rotation {rotation}: ratio {ratio:.4}, color leakage \
                                     {color_leak}, geometry error {geometry_error}, area error {area_mae:.3}, \
                                     maximum {area_max:.3}"
                                ));
                            }
                            if baseline.is_none() {
                                phases[phase as usize] = ratio;
                                baseline = Some(pixels);
                            }
                        }
                    }
                }
                if (phases[0] - phases[1]).abs() > 0.02 {
                    failures.push(format!(
                        "{source:?} {pattern:?}: phase energy changed from {:.4} to {:.4}",
                        phases[0], phases[1]
                    ));
                }
            }
        }
        assert!(
            failures.is_empty(),
            "GPU downscale quality:\n{}",
            failures.join("\n")
        );
        Ok(())
    }

    #[test]
    #[ignore = "requires an offscreen D3D11 hardware video processor"]
    fn hardware_desktop_downscale_filters_monitor_seams_and_releases_unused_scratch() -> Result<()>
    {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let output = (128, 96);
        let mut compositor = GpuVisualCompositor::new(device.clone(), (256, 192), output, 30)?;
        for source in [(256, 192), (192, 144)] {
            for phase in 0..2 {
                let pattern = DownscalePattern::VerticalLine;
                let original =
                    downscale_pattern_layer(&device, source, source, pattern, phase, 0, true)?;
                // The odd seam bisects a destination pixel's source footprint.
                // Resizing displays independently cannot preserve this coverage.
                let seam = source.0 / 2 + 1;
                let two_displays = [(0, seam), (seam, source.0 - seam)].map(|(x, width)| {
                    let mut layer = original.clone();
                    layer.source.x += x as i32;
                    layer.source.width = width;
                    layer.destination = Rect {
                        x: x as i32,
                        y: 0,
                        width,
                        height: source.1,
                    };
                    layer
                });
                let batch_count = (compositor.processor.max_streams() + 1).min(source.0 as usize);
                let many_displays = (0..batch_count)
                    .map(|index| {
                        let left = index as u32 * source.0 / batch_count as u32;
                        let right = (index + 1) as u32 * source.0 / batch_count as u32;
                        let mut layer = original.clone();
                        layer.source.x += left as i32;
                        layer.source.width = right - left;
                        layer.destination = Rect {
                            x: left as i32,
                            y: 0,
                            width: right - left,
                            height: source.1,
                        };
                        layer
                    })
                    .collect::<Vec<_>>();
                let reference = downscale_area_reference(pattern, source, output, phase);
                for layers in [two_displays.as_slice(), many_displays.as_slice()] {
                    let index = compositor.render_desktop(layers, source)?;
                    let actual = read_downscale_rgb(&device, &compositor.scratch[index])?;
                    for (position, (pixel, expected)) in
                        actual.chunks_exact(4).zip(&reference).enumerate()
                    {
                        for component in &pixel[..3] {
                            assert!(
                                (f64::from(*component) - expected).abs() <= 2.0,
                                "monitor seam {source:?}, phase {phase}, {} layers, pixel {position}: \
                                 {pixel:?}, expected {expected}",
                                layers.len()
                            );
                        }
                    }
                }
                assert!(compositor.native_desktop.is_some());
                assert_eq!(
                    compositor.crop_copy.as_ref().unwrap().cached_views(),
                    (1, true)
                );
                compositor.render_desktop(&[original], source)?;
                assert!(compositor.native_desktop.is_none());
                assert_eq!(
                    compositor.crop_copy.as_ref().unwrap().cached_views(),
                    (0, false)
                );
            }
        }
        for rotation in 0..4 {
            let native = downscale_pattern_layer(
                &device,
                output,
                output,
                DownscalePattern::Checkerboard,
                0,
                rotation,
                true,
            )?;
            let index = compositor.render_desktop(&[native], output)?;
            assert!(compositor.scaler.is_none());
            // Native crops now own a lazy RGBA buffer instead of borrowing a
            // permanently allocated cursor output. Full frames release it below.
            assert_eq!(compositor.native_desktop.as_ref().unwrap().size, output);
            assert_eq!(
                compositor.crop_copy.as_ref().unwrap().cached_views(),
                (1, true)
            );
            let actual = read_downscale_rgb(&device, &compositor.scratch[index])?;
            for (index, pixel) in actual.chunks_exact(4).enumerate() {
                let expected = DownscalePattern::Checkerboard.value(
                    index as u32 % output.0,
                    index as u32 / output.0,
                    output,
                    0,
                );
                assert_eq!(
                    &pixel[..3],
                    &[expected; 3],
                    "native crop rotation {rotation}"
                );
            }
        }
        let full_native = downscale_pattern_layer(
            &device,
            output,
            output,
            DownscalePattern::Checkerboard,
            0,
            0,
            false,
        )?;
        compositor.render_desktop(&[full_native], output)?;
        assert!(compositor.native_desktop.is_none());
        assert_eq!(
            compositor.crop_copy.as_ref().unwrap().cached_views(),
            (0, false)
        );
        let enlarged = downscale_pattern_layer(
            &device,
            (64, 48),
            (64, 48),
            DownscalePattern::Checkerboard,
            0,
            0,
            true,
        )?;
        compositor.render_desktop(&[enlarged], (64, 48))?;
        assert!(compositor.native_desktop.is_none());
        assert_eq!(
            compositor.crop_copy.as_ref().unwrap().cached_views(),
            (0, false)
        );
        for source in [(96, 192), (256, 72)] {
            for pattern in [
                DownscalePattern::VerticalStripes,
                DownscalePattern::HorizontalStripes,
            ] {
                let layer = downscale_pattern_layer(&device, source, source, pattern, 0, 0, true)?;
                let index = compositor.render_desktop(&[layer], source)?;
                let actual = read_downscale_rgb(&device, &compositor.scratch[index])?;
                let reference = downscale_area_reference(pattern, source, output, 0);
                for (pixel, expected) in actual.chunks_exact(4).zip(&reference) {
                    assert!(
                        (f64::from(pixel[1]) - expected).abs() <= 2.0,
                        "mixed-axis {source:?}: {pixel:?} != {expected}"
                    );
                }
            }
        }
        Ok(())
    }

    fn decoded_stem_luma(frame: &ffmpeg_next::frame::Video, size: (u32, u32)) -> (f64, f64) {
        assert_eq!((frame.width(), frame.height()), size);
        assert!(matches!(
            frame.format(),
            ffmpeg_next::format::Pixel::YUV420P | ffmpeg_next::format::Pixel::NV12
        ));
        let mut stems = 0u64;
        let mut stem_count = 0u64;
        let mut gaps = 0u64;
        let mut gap_count = 0u64;
        for y in 8..size.1 - 8 {
            for x in 4..size.0 - 4 {
                let value = u64::from(frame.data(0)[y as usize * frame.stride(0) + x as usize]);
                if x.is_multiple_of(4) {
                    stems += value;
                    stem_count += 1;
                } else if x % 4 == 2 {
                    gaps += value;
                    gap_count += 1;
                }
            }
        }
        (
            stems as f64 / stem_count as f64,
            gaps as f64 / gap_count as f64,
        )
    }

    #[test]
    #[ignore = "requires offscreen D3D11 video processing and a native hardware H.264 encoder"]
    fn hardware_downscale_encoded_h264_preserves_both_thin_stem_phases() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        // AMF rejects very small encoder surfaces (128x96 on the test adapter).
        // Keep the same 2:1 pixel coverage in a portable native-encoder fixture.
        let source = (512, 384);
        let output = (256, 192);
        let directory = tempfile::tempdir()?;
        let mut phase_contrast = [0.0; 2];
        for phase in 0..2 {
            let path = directory.path().join(format!("gpu-stems-{phase}.mp4"));
            let mut config = config(path.clone(), CaptureBackendKind::Auto);
            config.region.width = source.0;
            config.region.height = source.1;
            config.maximum_width = Some(output.0);
            config.maximum_height = Some(output.1);
            config.quality = 90;
            config.show_cursor = false;
            config.show_keyboard = false;
            config.mouse_trail_rgba = [0; 4];
            config.mouse_click_rgba = [0; 4];
            assert_eq!(config.output_dimensions(), output);
            let mut encoder = StreamingEncoder::builder(config.streaming_config())
                .gpu_input(snow_recording_export::gpu::GpuInputConfig {
                    device: device.clone(),
                })
                .create()?;
            let layer = downscale_pattern_layer(
                &device,
                source,
                source,
                DownscalePattern::VerticalStems,
                phase,
                0,
                true,
            )?;
            let observation = GpuCapturedFrame::from_layers(
                vec![layer],
                source,
                snow_capture::FrameMetadata::default(),
            )?;
            let mut compositor = GpuVisualCompositor::new(device.clone(), source, output, 30)?;
            let mut state = VisualCompositor::new(output);
            for pts in 0..4 {
                let surface = encoder.allocate_gpu_frame()?.ok_or_else(|| {
                    gpu_error("encoded clarity fixture exhausted its surface pool")
                })?;
                compositor.compose(
                    &mut state,
                    &config,
                    &observation,
                    pts * 1000 / 30,
                    None,
                    &surface,
                )?;
                encoder.push_gpu_frame_at_pts(pts, surface)?;
            }
            let report = encoder.finish_at_pts(4)?;
            assert!(
                report.used_hardware_video_encoder && !report.hardware_fallback,
                "{report:?}"
            );
            assert_eq!(report.encoded_frames, 4);
            let mut media = ffmpeg_next::format::input(&path).map_err(gpu_error)?;
            let stream = media
                .streams()
                .best(ffmpeg_next::media::Type::Video)
                .ok_or_else(|| gpu_error("missing hardware clarity video"))?;
            let video_index = stream.index();
            let mut decoder =
                ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                    .map_err(gpu_error)?
                    .decoder()
                    .video()
                    .map_err(gpu_error)?;
            let mut frame = ffmpeg_next::frame::Video::empty();
            let mut decoded = 0;
            let mut contrast = 0.0;
            let mut observe = |frame: &ffmpeg_next::frame::Video| {
                let (stem, gap) = decoded_stem_luma(frame, output);
                eprintln!(
                    "encoded GPU stem phase={phase} frame={decoded}: stem_luma={stem:.3} gap_luma={gap:.3}"
                );
                // A one-pixel stem occupies exactly half a 2:1 output pixel:
                // BT.709 limited-range luma is 16 + 219 / 2 = 125.5. Allow
                // H.264 quantization while rejecting missing/doubled stems.
                assert!(
                    (stem - 125.5).abs() <= 12.0,
                    "phase {phase} lost stem coverage: {stem}"
                );
                assert!(
                    (gap - 16.0).abs() <= 8.0,
                    "phase {phase} blurred black gaps: {gap}"
                );
                contrast += stem - gap;
                decoded += 1;
            };
            for (stream, packet) in media.packets() {
                if stream.index() != video_index {
                    continue;
                }
                decoder.send_packet(&packet).map_err(gpu_error)?;
                while decoder.receive_frame(&mut frame).is_ok() {
                    observe(&frame);
                }
            }
            decoder.send_eof().map_err(gpu_error)?;
            while decoder.receive_frame(&mut frame).is_ok() {
                observe(&frame);
            }
            assert_eq!(decoded, 4);
            phase_contrast[phase as usize] = contrast / f64::from(decoded);
        }
        assert!(
            (phase_contrast[0] - phase_contrast[1]).abs() <= 12.0,
            "hardware encoding changed stem contrast between phases: {phase_contrast:?}"
        );
        Ok(())
    }

    fn time_completed_gpu_batch(
        device: &SharedDevice,
        iterations: u32,
        mut operation: impl FnMut() -> Result<()>,
    ) -> Result<(f64, f64)> {
        let query = |kind| -> Result<ID3D11Query> {
            let mut result = None;
            unsafe {
                device.device().CreateQuery(
                    &D3D11_QUERY_DESC {
                        Query: kind,
                        MiscFlags: 0,
                    },
                    Some(&mut result),
                )
            }
            .map_err(gpu_error)?;
            result.ok_or_else(|| gpu_error("missing GPU timing query"))
        };
        let disjoint = query(D3D11_QUERY_TIMESTAMP_DISJOINT)?;
        let first = query(D3D11_QUERY_TIMESTAMP)?;
        let last = query(D3D11_QUERY_TIMESTAMP)?;
        let completed = query(D3D11_QUERY_EVENT)?;
        let context = device.context();
        let started = Instant::now();
        unsafe {
            context.Begin(&disjoint);
            context.End(&first);
        }
        for _ in 0..iterations {
            operation()?;
        }
        unsafe {
            context.End(&last);
            context.End(&disjoint);
            context.End(&completed);
            context.Flush();
        }
        let mut done = 0u32;
        let deadline = Instant::now() + Duration::from_secs(10);
        while done == 0 {
            unsafe {
                context.GetData(
                    &completed,
                    Some((&mut done as *mut u32).cast()),
                    std::mem::size_of_val(&done) as u32,
                    0,
                )
            }
            .map_err(gpu_error)?;
            if Instant::now() >= deadline {
                return Err(gpu_error("GPU resize timing completion timeout"));
            }
            if done == 0 {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        let cpu_ms = started.elapsed().as_secs_f64() * 1000.0 / f64::from(iterations);
        let mut frequency = D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::default();
        let mut start = 0u64;
        let mut end = 0u64;
        unsafe {
            context
                .GetData(
                    &disjoint,
                    Some((&mut frequency as *mut D3D11_QUERY_DATA_TIMESTAMP_DISJOINT).cast()),
                    std::mem::size_of_val(&frequency) as u32,
                    0,
                )
                .map_err(gpu_error)?;
            context
                .GetData(&first, Some((&mut start as *mut u64).cast()), 8, 0)
                .map_err(gpu_error)?;
            context
                .GetData(&last, Some((&mut end as *mut u64).cast()), 8, 0)
                .map_err(gpu_error)?;
        }
        if frequency.Disjoint.as_bool() || frequency.Frequency == 0 || end <= start {
            return Err(gpu_error(
                "GPU resize timestamps are disjoint or unavailable",
            ));
        }
        Ok((
            (end - start) as f64 * 1000.0 / frequency.Frequency as f64 / f64::from(iterations),
            cpu_ms,
        ))
    }

    #[test]
    #[ignore = "requires offscreen D3D11, a native encoder, and the Release performance preset"]
    fn hardware_cursor_and_highlight_stage_benchmark() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        if cfg!(debug_assertions) {
            panic!("use windows-msvc-performance Release");
        }
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let directory = tempfile::tempdir()?;
        for size in [(1920, 1080), (3840, 2160)] {
            let mut config = config(
                directory.path().join("cursor.mp4"),
                CaptureBackendKind::Auto,
            );
            config.region.width = size.0;
            config.region.height = size.1;
            config.show_cursor = true;
            config.show_keyboard = false;
            config.mouse_trail_rgba = [0; 4];
            config.mouse_click_rgba = [0; 4];
            let encoder = StreamingEncoder::builder(config.streaming_config())
                .gpu_input(snow_recording_export::gpu::GpuInputConfig {
                    device: device.clone(),
                })
                .create()?;
            let surface = encoder.allocate_gpu_frame()?.unwrap();
            let observation = GpuCapturedFrame::from_layers(
                vec![downscale_pattern_layer(
                    &device,
                    size,
                    size,
                    DownscalePattern::Checkerboard,
                    0,
                    0,
                    false,
                )?],
                size,
                snow_capture::FrameMetadata::default(),
            )?;
            let cursor = AttachedCursorSample {
                x: size.0 as i32 / 2,
                y: size.1 as i32 / 2,
                visible: true,
                shape: CursorShapeState::Embedded(CursorShape::from_rgba(
                    0,
                    0,
                    32,
                    32,
                    CursorCompositionMode::AlphaBlend,
                    [20, 80, 240, 160].repeat(32 * 32),
                )),
            };
            let mut compositor = GpuVisualCompositor::new(device.clone(), size, size, 60)?;
            let mut state = VisualCompositor::new(size);
            for highlighted in [false, true] {
                config.mouse_highlight_rgba = if highlighted {
                    [40, 100, 240, 128]
                } else {
                    [0; 4]
                };
                time_completed_gpu_batch(&device, 120, || {
                    compositor.compose(
                        &mut state,
                        &config,
                        &observation,
                        0,
                        Some(&cursor),
                        &surface,
                    )
                })?;
                for sample in 0..10 {
                    let (gpu_ms, completed_ms) = time_completed_gpu_batch(&device, 240, || {
                        compositor.compose(
                            &mut state,
                            &config,
                            &observation,
                            0,
                            Some(&cursor),
                            &surface,
                        )
                    })?;
                    eprintln!(
                        "GPU cursor {size:?} highlight={highlighted} sample={sample} gpu_ms={gpu_ms:.6} completed_ms={completed_ms:.6}"
                    );
                }
            }
        }
        Ok(())
    }

    #[test]
    #[ignore = "requires offscreen D3D11, a native encoder, and the Release performance preset"]
    fn hardware_empty_overlay_stage_benchmark() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        if cfg!(debug_assertions) {
            panic!("use windows-msvc-performance Release");
        }
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let directory = tempfile::tempdir()?;
        for size in [(1920, 1080), (3840, 2160)] {
            let mut config = config(directory.path().join("empty.mp4"), CaptureBackendKind::Auto);
            config.region.width = size.0;
            config.region.height = size.1;
            config.show_cursor = false;
            config.show_keyboard = false;
            config.mouse_trail_rgba = [0; 4];
            config.mouse_click_rgba = [0; 4];
            let encoder = StreamingEncoder::builder(config.streaming_config())
                .gpu_input(snow_recording_export::gpu::GpuInputConfig {
                    device: device.clone(),
                })
                .create()?;
            let surface = encoder.allocate_gpu_frame()?.unwrap();
            let observation = GpuCapturedFrame::from_layers(
                vec![downscale_pattern_layer(
                    &device,
                    size,
                    size,
                    DownscalePattern::Checkerboard,
                    0,
                    0,
                    false,
                )?],
                size,
                snow_capture::FrameMetadata::default(),
            )?;
            let mut compositors = [
                GpuVisualCompositor::new(device.clone(), size, size, 60)?,
                GpuVisualCompositor::new(device.clone(), size, size, 60)?,
            ];
            // Test-only control reproduces the previous two transparent passes,
            // including their full-size overlay allocation and upload path.
            compositors[0].force_empty_overlays = true;
            let mut state = VisualCompositor::new(size);
            for compositor in &mut compositors {
                time_completed_gpu_batch(&device, 20, || {
                    compositor.compose(&mut state, &config, &observation, 0, None, &surface)
                })?;
            }
            assert!(compositors[0].overlay.is_some());
            assert!(compositors[1].overlay.is_none());
            assert_eq!(
                read_downscale_rgb(&device, compositors[0].final_rgb.as_ref().unwrap())?,
                read_downscale_rgb(&device, compositors[1].final_rgb.as_ref().unwrap())?
            );
            for pair in 0u32..10 {
                for index in if pair.is_multiple_of(2) {
                    [0, 1]
                } else {
                    [1, 0]
                } {
                    let (gpu_ms, completed_ms) = time_completed_gpu_batch(&device, 120, || {
                        compositors[index].compose(
                            &mut state,
                            &config,
                            &observation,
                            0,
                            None,
                            &surface,
                        )
                    })?;
                    eprintln!(
                        "GPU empty overlays {size:?} pair={pair} bypass={}: gpu_ms/frame={gpu_ms:.4} completed_wall_ms/frame={completed_ms:.4}",
                        index == 1
                    );
                }
            }
        }
        Ok(())
    }

    #[test]
    #[ignore = "requires an offscreen D3D11 hardware device and the Release performance preset"]
    fn hardware_area_resize_stage_benchmark() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        if cfg!(debug_assertions) {
            panic!("run with windows-msvc-performance Release");
        }
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        for (source, output) in [
            ((2560, 1440), (1920, 1080)),
            ((3840, 2160), (1920, 1080)),
            ((3840, 2160), (1280, 720)),
        ] {
            let mut compositor = GpuVisualCompositor::new(device.clone(), source, output, 60)?;
            let legacy = downscale_pattern_layer(
                &device,
                source,
                output,
                DownscalePattern::Checkerboard,
                0,
                0,
                true,
            )?;
            let mut filtered = legacy.clone();
            filtered.destination = Rect::full(source);
            for _ in 0..10 {
                compositor
                    .processor
                    .blit(
                        std::slice::from_ref(&legacy),
                        compositor.scratch[0].raw(),
                        0,
                    )
                    .map_err(gpu_error)?;
                compositor.render_desktop(std::slice::from_ref(&filtered), source)?;
            }
            for pair in 0u32..5 {
                for candidate in if pair.is_multiple_of(2) {
                    [false, true]
                } else {
                    [true, false]
                } {
                    let (gpu_ms, completed_ms) = time_completed_gpu_batch(&device, 120, || {
                        if candidate {
                            compositor.render_desktop(std::slice::from_ref(&filtered), source)?;
                            Ok(())
                        } else {
                            compositor
                                .processor
                                .blit(
                                    std::slice::from_ref(&legacy),
                                    compositor.scratch[0].raw(),
                                    0,
                                )
                                .map_err(gpu_error)
                        }
                    })?;
                    eprintln!(
                        "GPU resize stage {source:?}->{output:?} pair={pair} area={candidate}: \
                         gpu_ms/frame={gpu_ms:.4} completed_wall_ms/frame={completed_ms:.4}"
                    );
                }
            }
        }
        Ok(())
    }

    #[test]
    fn warp_highlight_shader_matches_cpu_multiply_and_clipping() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIAdapter, IDXGIFactory4};
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter: IDXGIAdapter = unsafe { factory.EnumWarpAdapter() }.map_err(gpu_error)?;
        let device = SharedDevice::create_compute(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        // Include the complete disk and untouched surrounding pixels, plus
        // partially/fully clipped disks. A small frame can miss these invariants.
        let size = (129, 113);
        let stride = size.0 as usize * 4;
        let background = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8X8_UNORM,
                (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
            )
            .map_err(gpu_error)?;
        let mut reference = vec![0; stride * size.1 as usize];
        for (index, pixel) in reference.chunks_exact_mut(4).enumerate() {
            pixel.copy_from_slice(&[index as u8, (index / 7) as u8, (index / 11) as u8, 255]);
        }
        let mut bgra = reference.clone();
        for pixel in bgra.chunks_exact_mut(4) {
            pixel.swap(0, 2);
        }
        unsafe {
            device.context().UpdateSubresource(
                background.raw(),
                0,
                None,
                bgra.as_ptr().cast(),
                stride as u32,
                0,
            );
        }
        for format in [DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM] {
            let mut renderer = CursorRenderer::new(device.clone())?;
            let output = device
                .texture(
                    size.0,
                    size.1,
                    format,
                    (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
                )
                .map_err(gpu_error)?;
            let mut desc = output.desc();
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
            let mut staging = None;
            unsafe {
                device
                    .device()
                    .CreateTexture2D(&desc, None, Some(&mut staging))
            }
            .map_err(gpu_error)?;
            let staging = staging.ok_or_else(|| gpu_error("staging texture"))?;
            for radius in [8, 41, 96, 128] {
                for center in [
                    (0, 0),
                    (64, 56),
                    (127, 111),
                    (-radius, 56),
                    (64, -radius - 1),
                ] {
                    for color in [
                        [255, 255, 0, 128],
                        [80, 140, 220, 255],
                        [255, 255, 255, 255],
                        [0, 0, 0, 0],
                    ] {
                        let mut expected = reference.clone();
                        snow_recording_effects::mouse_effects::draw_highlight_to(
                            &mut snow_recording_effects::surface::RgbaSurface {
                                pixels: &mut expected,
                                dimensions: size,
                            },
                            center,
                            color,
                            true,
                            radius as u32,
                        );
                        renderer.highlight(&background, &output, center, color, radius as u32)?;
                        unsafe {
                            device.context().CopyResource(&staging, output.raw());
                        }
                        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
                        unsafe {
                            device
                                .context()
                                .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                        }
                        .map_err(gpu_error)?;
                        let mut actual = Vec::new();
                        for row in 0..size.1 as usize {
                            actual.extend_from_slice(unsafe {
                                std::slice::from_raw_parts(
                                    mapped
                                        .pData
                                        .cast::<u8>()
                                        .add(row * mapped.RowPitch as usize),
                                    stride,
                                )
                            });
                        }
                        unsafe {
                            device.context().Unmap(&staging, 0);
                        }
                        for pixel in actual.chunks_exact_mut(4) {
                            pixel.swap(0, 2);
                            // BGRX has no alpha storage; X readback is unspecified.
                            // Typed sampling treats it as opaque. BGRA above must
                            // store the shader's alpha exactly, without adjustment.
                            if format == DXGI_FORMAT_B8G8R8X8_UNORM {
                                pixel[3] = 255;
                            }
                        }
                        assert_eq!(actual.len(), expected.len());
                        for (index, (actual, expected)) in actual.iter().zip(&expected).enumerate()
                        {
                            let pixel = index / 4;
                            let channel = index % 4;
                            let x = (pixel % size.0 as usize) as i32 - center.0;
                            let y = (pixel / size.0 as usize) as i32 - center.1;
                            let distance = (f64::from(x) + 0.5).hypot(f64::from(y) + 0.5);
                            let feather = (-radius..radius).contains(&x)
                                && (-radius..radius).contains(&y)
                                && distance > f64::from(radius) - 0.5
                                && distance < f64::from(radius) + 0.5;
                            // Only feathered RGB values depend on sqrt precision. Alpha,
                            // the interior, clipped pixels, and identity colors remain exact.
                            let tolerance = u8::from(
                                feather && color[3] != 0 && channel < 3 && color[channel] != 255,
                            );
                            assert!(
                                actual.abs_diff(*expected) <= tolerance,
                                "highlight {center:?} {color:?} pixel ({x}, {y}) channel {channel}: \
                         {actual} != {expected} (tolerance {tolerance})"
                            );
                        }
                    }
                }
            }
        }
        Ok(())
    }

    #[test]
    fn warp_cursor_shader_matches_cpu_for_alpha_mask_xor_scaling_and_clipping() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIAdapter, IDXGIFactory4};
        let factory: IDXGIFactory4 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter: IDXGIAdapter = unsafe { factory.EnumWarpAdapter() }.map_err(gpu_error)?;
        let device = SharedDevice::create_compute(&adapter).map_err(gpu_error)?;
        let _lock = device.lock();
        let size = (64, 48);
        let background = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_B8G8R8X8_UNORM,
                (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
            )
            .map_err(gpu_error)?;
        let mut reference = vec![0; 64 * 48 * 4];
        for (index, pixel) in reference.chunks_exact_mut(4).enumerate() {
            // Include the mid-gray inversion boundary as well as colored pixels.
            let color = if index / 64 < 24 {
                [0x7f, 0x7f, 0x7f, 255]
            } else {
                [index as u8, (index / 7) as u8, (index / 11) as u8, 255]
            };
            pixel.copy_from_slice(&color);
        }
        let mut bgra = reference.clone();
        for pixel in bgra.chunks_exact_mut(4) {
            pixel.swap(0, 2);
        }
        unsafe {
            device.context().UpdateSubresource(
                background.raw(),
                0,
                None,
                bgra.as_ptr().cast(),
                256,
                0,
            );
        }
        for format in [DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM] {
            let mut renderer = CursorRenderer::new(device.clone())?;
            let output = device
                .texture(
                    size.0,
                    size.1,
                    format,
                    (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET).0 as u32,
                )
                .map_err(gpu_error)?;
            let mut desc = output.desc();
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
            let mut staging = None;
            unsafe {
                device
                    .device()
                    .CreateTexture2D(&desc, None, Some(&mut staging))
            }
            .map_err(gpu_error)?;
            let staging = staging.ok_or_else(|| gpu_error("staging texture"))?;
            let mut shapes = [
                CursorCompositionMode::AlphaBlend,
                CursorCompositionMode::MaskedColor,
            ]
            .map(|mode| {
                CursorShape::from_rgba(
                    2,
                    1,
                    4,
                    3,
                    mode,
                    [
                        255, 0, 0, 0, 0, 255, 0, 255, 19, 81, 243, 128, 255, 255, 255, 255,
                    ]
                    .repeat(3),
                )
            })
            .to_vec();
            shapes.push(snow_cursor::crosshair_cursor_shape(32, 32).unwrap());
            for shape in shapes {
                for source in [(64, 48), (128, 96), (45, 37)] {
                    for (x, y) in [(-1, -1), (23, 17), (63, 47)] {
                        let sample = AttachedCursorSample {
                            x,
                            y,
                            visible: true,
                            shape: CursorShapeState::Embedded(shape.clone()),
                        };
                        let mut expected = reference.clone();
                        draw_cursor(&mut expected, size, source, &sample, &mut None);
                        renderer.cursor(&background, &output, &sample, &shape, source)?;
                        unsafe {
                            device.context().CopyResource(&staging, output.raw());
                        }
                        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
                        unsafe {
                            device
                                .context()
                                .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                        }
                        .map_err(gpu_error)?;
                        let mut actual = Vec::new();
                        for row in 0..48 {
                            actual.extend_from_slice(unsafe {
                                std::slice::from_raw_parts(
                                    mapped
                                        .pData
                                        .cast::<u8>()
                                        .add(row * mapped.RowPitch as usize),
                                    256,
                                )
                            });
                        }
                        unsafe {
                            device.context().Unmap(&staging, 0);
                        }
                        for pixel in actual.chunks_exact_mut(4) {
                            pixel.swap(0, 2);
                            // BGRX has no alpha storage; X readback is unspecified.
                            // Typed sampling treats it as opaque. BGRA above must
                            // store the shader's alpha exactly, without adjustment.
                            if format == DXGI_FORMAT_B8G8R8X8_UNORM {
                                pixel[3] = 255;
                            }
                        }
                        assert_eq!(
                            actual, expected,
                            "cursor {:?} at {x},{y}, source {source:?}",
                            shape.composition_mode
                        );
                    }
                }
            }
        }
        Ok(())
    }

    #[cfg(feature = "bench-synthetic-input")]
    #[test]
    #[ignore = "requires desktop capture; records five synthetic buttons without injecting OS input"]
    fn mouse_recording_native_smoke() -> Result<()> {
        for hardware in [false, true] {
            let directory = tempfile::tempdir()?;
            let path = directory.path().join("mouse.mp4");
            let mut config = config(path.clone(), CaptureBackendKind::Auto);
            config.prefer_hardware_encoder = hardware;
            config.mouse_highlight_rgba = [255, 255, 0, 128];
            config.record_mouse_clicks = true;
            config.show_keyboard = false;
            config.mouse_click_rgba = [0; 4];
            config.mouse_trail_rgba = [0; 4];
            config.keyboard = Some(KeyboardOverlayConfig {
                font: None,
                keycap_size: 64,
                background_rgba: [0, 0, 0, 204],
                text_rgba: [255; 4],
                border_rgba: [80, 80, 80, 204],
                labels: Default::default(),
            });
            let mut session = DirectRecordingSession::create(config)?;
            session.set_bench_synthetic_input(true)?;
            session.start()?;
            session.bench_observe_cursor(320, 240)?;
            for button in [
                ObservedMouseButton::Left,
                ObservedMouseButton::Right,
                ObservedMouseButton::Middle,
                ObservedMouseButton::Button4,
                ObservedMouseButton::Button5,
            ] {
                for down in [true, false] {
                    session.with_bench_synthetic(|input| {
                        input
                            .clicks
                            .try_send(MouseClickObservation {
                                at: Instant::now(),
                                x: 320,
                                y: 240,
                                button,
                                down,
                                modifiers: [false; 8],
                            })
                            .map_err(gpu_error)
                    })?;
                    std::thread::sleep(Duration::from_millis(70));
                }
            }
            session.pause()?;
            std::thread::sleep(Duration::from_millis(50));
            session.resume()?;
            session.bench_observe_cursor(100, 100)?;
            std::thread::sleep(Duration::from_millis(200));
            let report = session.stop()?;
            assert!(report.encoded_frames > 0, "{report:?}");
            if hardware {
                assert_eq!(report.selected_pipeline, "d3d11", "{report:?}");
            }
            let mut media = ffmpeg_next::format::input(&path).map_err(gpu_error)?;
            let stream = media
                .streams()
                .best(ffmpeg_next::media::Type::Video)
                .ok_or_else(|| gpu_error("missing video"))?;
            let mut decoder =
                ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                    .map_err(gpu_error)?
                    .decoder()
                    .video()
                    .map_err(gpu_error)?;
            let mut decoded = 0;
            for (_, packet) in media.packets() {
                decoder.send_packet(&packet).map_err(gpu_error)?;
                while decoder
                    .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                    .is_ok()
                {
                    decoded += 1;
                }
            }
            decoder.send_eof().map_err(gpu_error)?;
            while decoder
                .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                .is_ok()
            {
                decoded += 1;
            }
            assert!(decoded > 0);
        }
        Ok(())
    }

    #[test]
    #[ignore = "requires an interactive desktop and hardware H.264 encoder"]
    fn hardware_dxgi_and_wgc_record_native_surfaces() -> Result<()> {
        for backend in [
            CaptureBackendKind::DxgiDuplication,
            CaptureBackendKind::WindowsGraphicsCapture,
        ] {
            let directory = tempfile::tempdir()?;
            let path = directory.path().join("hardware.mp4");
            let config = config(path.clone(), backend);
            // Probe GPU startup before accepting a CPU fallback. The report
            // below must still prove that both native backends were exercised.
            let mut state = VisualCompositor::new(config.output_dimensions());
            drop(prepare(
                &config,
                true,
                &mut state,
                &mut Negotiation::default(),
            )?);
            let mut session = DirectRecordingSession::create(config)?;
            session.start()?;
            std::thread::sleep(Duration::from_millis(600));
            session.pause()?;
            std::thread::sleep(Duration::from_millis(100));
            session.resume()?;
            std::thread::sleep(Duration::from_millis(600));
            let report = session.stop()?;
            assert_eq!(report.selected_pipeline, "d3d11", "{report:?}");
            assert_eq!(report.capture_backend, backend.as_str(), "{report:?}");
            assert!(report.used_hardware_video_encoder, "{report:?}");
            let mut media = ffmpeg_next::format::input(&path).map_err(gpu_error)?;
            let stream = media
                .streams()
                .best(ffmpeg_next::media::Type::Video)
                .ok_or_else(|| gpu_error("missing video"))?;
            let mut decoder =
                ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                    .map_err(gpu_error)?
                    .decoder()
                    .video()
                    .map_err(gpu_error)?;
            let mut decoded = 0;
            for (_, packet) in media.packets() {
                decoder.send_packet(&packet).map_err(gpu_error)?;
                while decoder
                    .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                    .is_ok()
                {
                    decoded += 1;
                }
            }
            decoder.send_eof().map_err(gpu_error)?;
            while decoder
                .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                .is_ok()
            {
                decoded += 1;
            }
            assert!(decoded > 0);
        }
        Ok(())
    }

    #[test]
    #[ignore = "requires an interactive desktop, audio endpoint and hardware H.264 encoder"]
    fn hardware_failure_finishes_one_playable_mp4_with_audio() -> Result<()> {
        use ffmpeg_next::Rescale;
        use snow_recording_export::streaming::GpuFailureStage;
        for stage in [
            GpuFailureStage::Capture,
            GpuFailureStage::CaptureDisconnected,
            GpuFailureStage::Composition,
            GpuFailureStage::Submission,
            GpuFailureStage::Drain,
            GpuFailureStage::Stop,
        ] {
            for backend in [
                CaptureBackendKind::DxgiDuplication,
                CaptureBackendKind::WindowsGraphicsCapture,
            ] {
                eprintln!("RECOVERY CASE: {stage:?}, {backend:?}");
                let directory = tempfile::tempdir()?;
                let path = directory.path().join("recovered.mp4");
                let mut config = config(path.clone(), backend);
                config.enable_system_audio = true;
                let mut session = DirectRecordingSession::create(config)?;
                session.start()?;
                std::thread::sleep(Duration::from_millis(500));
                if matches!(stage, GpuFailureStage::CaptureDisconnected) {
                    session.disconnect_gpu_capture_for_diagnostics()?;
                } else {
                    session.inject_gpu_failure_at(stage)?;
                }
                std::thread::sleep(Duration::from_millis(700));
                let report = session.stop()?;
                assert_eq!(report.recovery_count, 1, "{report:?}");
                assert_eq!(report.video_encoder, "libx264");
                assert_eq!(report.selected_pipeline, "d3d11_to_software");
                let mut media = ffmpeg_next::format::input(&path).map_err(gpu_error)?;
                assert_eq!(media.nb_streams(), 2);
                let stream = media
                    .streams()
                    .best(ffmpeg_next::media::Type::Video)
                    .ok_or_else(|| gpu_error("missing video"))?;
                let index = stream.index();
                let base = stream.time_base();
                let mut decoder =
                    ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                        .map_err(gpu_error)?
                        .decoder()
                        .video()
                        .map_err(gpu_error)?;
                let mut pts = Vec::new();
                let mut audio_pts = Vec::new();
                let mut endpoint = 0;
                let mut audio_endpoint = 0;
                let mut decoded = 0;
                for (stream, packet) in media.packets() {
                    if stream.index() == index {
                        pts.push(packet.pts().unwrap());
                        endpoint =
                            (packet.pts().unwrap() + packet.duration()).rescale(base, (1, 1000));
                        decoder.send_packet(&packet).map_err(gpu_error)?;
                        while decoder
                            .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                            .is_ok()
                        {
                            decoded += 1;
                        }
                    } else {
                        audio_pts.push(packet.pts().unwrap());
                        audio_endpoint = (packet.pts().unwrap() + packet.duration())
                            .rescale(stream.time_base(), (1, 1000));
                    }
                }
                decoder.send_eof().map_err(gpu_error)?;
                while decoder
                    .receive_frame(&mut ffmpeg_next::frame::Video::empty())
                    .is_ok()
                {
                    decoded += 1;
                }
                assert!(decoded > 0);
                assert!(pts.windows(2).all(|pair| pair[1] > pair[0]));
                assert!(audio_pts.len() > 40);
                assert!(audio_pts.windows(2).all(|pair| pair[1] - pair[0] == 1024));
                assert!((1100..1600).contains(&endpoint), "endpoint {endpoint}");
                assert!(
                    (audio_endpoint - endpoint).abs() <= 22,
                    "video endpoint {endpoint}, audio endpoint {audio_endpoint}"
                );
            }
        }
        Ok(())
    }
}
