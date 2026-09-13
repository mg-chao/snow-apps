use std::collections::HashMap;
use std::sync::Arc;

use snow_capture::gpu::GpuCapturedFrame;
use snow_d3d11::{Rect, SharedDevice, Texture, VideoLayer, VideoProcessor};
use snow_recording_effects::surface::{TILE_SIZE, TileSurface};
use snow_recording_export::gpu::GpuEncoderFrame;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::core::PCSTR;

use super::*;

pub(super) fn eligible(config: &DirectRecordingConfig) -> bool {
    config.format == ExportFormat::Mp4
        && config.codec == VideoCodec::H264
        && config.prefer_hardware_encoder
}

/// Probe the actual capture adapter, compositor and vendor encoder together.
/// No CPU capture stream is allocated while this path is being negotiated.
#[derive(Default)]
pub(super) struct Negotiation {
    pub adapter: Option<String>,
    pub encoder_attempts: Vec<String>,
    pub stage: Option<String>,
}

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
    use snow_capture::gpu::{GpuCaptureEvent, GpuCaptureStream};
    diagnostics.stage = Some("capture_startup".into());
    let stream = GpuCaptureStream::spawn(
        snow_capture::CaptureRegion {
            x: config.region.x,
            y: config.region.y,
            width: config.region.width,
            height: config.region.height,
        },
        config.capture_backend,
        capture::stream_config(config, include_cursor),
    )?;
    let device = stream.device().clone();
    diagnostics.adapter = Some(device.identity().description.clone());
    diagnostics.stage = Some("compositor_startup".into());
    let mut compositor = GpuVisualCompositor::new(
        device.clone(),
        (config.region.width, config.region.height),
        config.output_dimensions(),
        config.output_fps,
    )?;
    diagnostics.stage = Some("capture_probe".into());
    let deadline = Instant::now() + Duration::from_secs(5);
    let observation = loop {
        match stream.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
            Ok(GpuCaptureEvent::Frame(frame)) => break frame,
            Ok(GpuCaptureEvent::Error(error)) => return Err(error.into()),
            Ok(GpuCaptureEvent::StreamEnded) | Err(_) => {
                return Err(gpu_error("capture startup probe produced no image"));
            }
            _ => {}
        }
    };
    struct ProbeFile(PathBuf);
    impl Drop for ProbeFile {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let path = ProbeFile(
        std::env::temp_dir().join(format!("snow-gpu-probe-{}.mp4", uuid::Uuid::new_v4())),
    );
    let mut settings = config.streaming_config();
    settings.output_path = path.0.clone();
    settings.audio = None;
    diagnostics.stage = Some("encoder_startup".into());
    if let Some(name) = snow_d3d11::h264_encoder(device.identity().vendor) {
        diagnostics.encoder_attempts.push(name.into());
    }
    let mut encoder = StreamingEncoder::builder(settings)
        .gpu_input(snow_recording_export::gpu::GpuInputConfig {
            device: device.clone(),
        })
        .create()?;
    let mut probe_config = config.clone();
    probe_config.show_cursor = true;
    for pts in 0..4 {
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
        let surface = encoder
            .allocate_gpu_frame()?
            .ok_or_else(|| gpu_error("startup encoder exhausted its surface pool"))?;
        diagnostics.stage = Some("composition_probe".into());
        compositor.compose(
            state,
            &probe_config,
            &observation,
            0,
            Some(&cursor),
            &surface,
        )?;
        diagnostics.stage = Some("submission_probe".into());
        encoder.push_gpu_frame_at_pts(pts, surface)?;
    }
    diagnostics.stage = Some("packet_probe".into());
    encoder.finish_at_pts(4)?;
    let mut media = ffmpeg_next::format::input(&path.0).map_err(gpu_error)?;
    if !media.packets().any(|(stream, packet)| {
        stream.parameters().medium() == ffmpeg_next::media::Type::Video && packet.size() != 0
    }) {
        return Err(gpu_error(
            "hardware encoder startup probe produced no video packet",
        ));
    }
    drop(media);
    diagnostics.stage = None;
    state.trail.clear();
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

    fn upload(&mut self, surface: &TileSurface) -> Result<u64> {
        let next: HashMap<_, _> = surface
            .snapshot()
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

pub(super) struct GpuVisualCompositor {
    #[cfg(test)]
    final_rgb: Option<Texture>,
    #[cfg(any(test, feature = "bench-synthetic-input"))]
    pub inject_failure: bool,
    device: SharedDevice,
    processor: VideoProcessor,
    scratch: [Texture; 2],
    cursor_output: Texture,
    overlay: OverlayTexture,
    tiles: TileSurface,
    cursor_shader: ID3D11ComputeShader,
    cursor_constants: ID3D11Buffer,
    cursor_texture: Option<(u64, Texture)>,
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
        let cursor_output = device
            .texture(
                size.0,
                size.1,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                (D3D11_BIND_SHADER_RESOURCE
                    | D3D11_BIND_UNORDERED_ACCESS
                    | D3D11_BIND_RENDER_TARGET)
                    .0 as u32,
            )
            .map_err(gpu_error)?;
        processor
            .check_conversion(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_NV12)
            .map_err(gpu_error)?;
        let source = include_str!("recording_cursor.hlsl");
        let mut bytecode = None;
        let mut errors = None;
        unsafe {
            windows::Win32::Graphics::Direct3D::Fxc::D3DCompile(
                source.as_ptr().cast(),
                source.len(),
                None,
                None,
                None,
                PCSTR(c"main".as_ptr().cast()),
                PCSTR(c"cs_5_0".as_ptr().cast()),
                0,
                0,
                &mut bytecode,
                Some(&mut errors),
            )
            .map_err(gpu_error)?;
        }
        let bytecode =
            bytecode.ok_or_else(|| gpu_error("cursor shader compiler returned no bytecode"))?;
        let mut cursor_shader = None;
        let mut cursor_constants = None;
        unsafe {
            let bytes = std::slice::from_raw_parts(
                bytecode.GetBufferPointer().cast::<u8>(),
                bytecode.GetBufferSize(),
            );
            device
                .device()
                .CreateComputeShader(bytes, None, Some(&mut cursor_shader))
                .map_err(gpu_error)?;
            device
                .device()
                .CreateBuffer(
                    &D3D11_BUFFER_DESC {
                        ByteWidth: 48,
                        Usage: D3D11_USAGE_DEFAULT,
                        BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
                        ..Default::default()
                    },
                    None,
                    Some(&mut cursor_constants),
                )
                .map_err(gpu_error)?;
        }
        let overlay = OverlayTexture::new(&device, size)?;
        drop(_lock);
        Ok(Self {
            device,
            #[cfg(test)]
            final_rgb: None,
            #[cfg(any(test, feature = "bench-synthetic-input"))]
            inject_failure: false,
            processor,
            scratch,
            cursor_output,
            overlay,
            tiles: TileSurface::new(size),
            cursor_shader: cursor_shader.ok_or_else(|| gpu_error("missing cursor shader"))?,
            cursor_constants: cursor_constants
                .ok_or_else(|| gpu_error("missing cursor constants"))?,
            cursor_texture: None,
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
        let _lock = device.lock();
        if !device.same_device(output.device()) {
            return Err(gpu_error("encoder output belongs to another GPU device"));
        }
        let source_size = frame.dimensions();
        let mut layers = frame.layers.clone();
        for layer in &mut layers {
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
        layers.retain(|layer| layer.destination.width != 0 && layer.destination.height != 0);
        let mut index = 0;
        let mut consumed = 0;
        while consumed < layers.len() {
            let mut batch = Vec::new();
            if consumed != 0 {
                batch.push(self.layer(self.scratch[1 - index].clone(), false));
            }
            let count = (self.processor.max_streams() - batch.len()).min(layers.len() - consumed);
            batch.extend_from_slice(&layers[consumed..consumed + count]);
            self.processor
                .blit(&batch, self.scratch[index].raw(), 0)
                .map_err(|error| gpu_error(format!("desktop pass: {error:#}")))?;
            consumed += count;
            if consumed < layers.len() {
                index = 1 - index;
            }
        }
        if layers.is_empty() {
            self.processor
                .blit(&[], self.scratch[index].raw(), 0)
                .map_err(|error| gpu_error(format!("desktop pass: {error:#}")))?;
        }
        self.tiles.clear();
        state.trail.set_lifetime_ms(config.mouse_trail_duration_ms);
        if config.mouse_trail_rgba[3] != 0 {
            state.trail.observe(
                cursor
                    .filter(|cursor| cursor.visible)
                    .map(|cursor| (cursor.x, cursor.y)),
                source_size,
                self.size,
                timestamp,
            );
            state
                .trail
                .draw_to(&mut self.tiles, timestamp, config.mouse_trail_rgba);
        } else {
            state.trail.clear();
        }
        while state
            .clicks
            .front()
            .is_some_and(|click| timestamp.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS)
        {
            state.clicks.pop_front();
        }
        snow_recording_effects::mouse_effects::draw_clicks_to(
            &mut self.tiles,
            &state.clicks,
            timestamp,
            config.mouse_click_rgba,
            source_size,
        );
        self.overlay_upload_bytes += self.overlay.upload(&self.tiles)?;
        self.processor
            .blit(
                &[
                    self.layer(self.scratch[index].clone(), false),
                    self.layer(self.overlay.texture.clone(), true),
                ],
                self.scratch[1 - index].raw(),
                0,
            )
            .map_err(|error| gpu_error(format!("effects pass: {error:#}")))?;
        index = 1 - index;
        let mut background = self.scratch[index].clone();
        if config.show_cursor
            && let Some(cursor) = cursor
        {
            let shape = match &cursor.shape {
                CursorShapeState::Embedded(shape) => {
                    state
                        .cursor_shapes
                        .insert(shape.shape_id.get(), shape.clone());
                    Some(shape.clone())
                }
                CursorShapeState::Cached(id) => state.cursor_shapes.get(&id.get()).cloned(),
                CursorShapeState::Unavailable => None,
            };
            if cursor.visible
                && let Some(shape) = shape
            {
                self.cursor(&background, cursor, &shape, source_size)?;
                background = self.cursor_output.clone();
            }
        }
        self.tiles.clear();
        if let Some(keyboard) = state.keyboard.as_mut() {
            while state
                .pending_keys
                .front()
                .is_some_and(|event| event.at_ms <= timestamp)
            {
                keyboard
                    .model
                    .event(state.pending_keys.pop_front().expect("pending key"));
            }
            keyboard
                .draw_to(&mut self.tiles, timestamp)
                .map_err(gpu_error)?;
        }
        self.overlay_upload_bytes += self.overlay.upload(&self.tiles)?;
        self.processor
            .blit(
                &[
                    self.layer(background, false),
                    self.layer(self.overlay.texture.clone(), true),
                ],
                self.scratch[1 - index].raw(),
                0,
            )
            .map_err(|error| gpu_error(format!("keyboard pass: {error:#}")))?;
        #[cfg(test)]
        {
            self.final_rgb = Some(self.scratch[1 - index].clone());
        }
        self.processor
            .blit(
                &[self.layer(self.scratch[1 - index].clone(), false)],
                &output.texture()?,
                output.slice(),
            )
            .map_err(|error| gpu_error(format!("NV12 pass: {error:#}")))
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

    fn cursor(
        &mut self,
        background: &Texture,
        cursor: &AttachedCursorSample,
        shape: &CursorShape,
        source_size: (u32, u32),
    ) -> Result<()> {
        if self
            .cursor_texture
            .as_ref()
            .is_none_or(|(id, _)| *id != shape.shape_id.get())
        {
            if shape.shape_rgba.len() != shape.width as usize * shape.height as usize * 4 {
                return Err(gpu_error("invalid cursor shape"));
            }
            let texture = self
                .device
                .texture(
                    shape.width,
                    shape.height,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    D3D11_BIND_SHADER_RESOURCE.0 as u32,
                )
                .map_err(gpu_error)?;
            unsafe {
                self.device.context().UpdateSubresource(
                    texture.raw(),
                    0,
                    None,
                    shape.shape_rgba.as_ptr().cast(),
                    shape.width * 4,
                    0,
                )
            };
            self.overlay_upload_bytes += shape.shape_rgba.len() as u64;
            self.cursor_texture = Some((shape.shape_id.get(), texture));
        }
        let origin = scale_point(cursor.x, cursor.y, source_size, self.size);
        let constants: [i32; 12] = [
            self.size.0 as i32,
            self.size.1 as i32,
            origin.0 - scale_coordinate(shape.hotspot_x as i32, source_size.0, self.size.0),
            origin.1 - scale_coordinate(shape.hotspot_y as i32, source_size.1, self.size.1),
            ((u64::from(shape.width) * u64::from(self.size.0) / u64::from(source_size.0)).max(1))
                as i32,
            ((u64::from(shape.height) * u64::from(self.size.1) / u64::from(source_size.1)).max(1))
                as i32,
            shape.width as i32,
            shape.height as i32,
            i32::from(shape.composition_mode == CursorCompositionMode::MaskedColor),
            0,
            0,
            0,
        ];
        let mut background_view = None;
        let mut cursor_view = None;
        let mut output_view = None;
        unsafe {
            self.device
                .device()
                .CreateShaderResourceView(background.raw(), None, Some(&mut background_view))
                .map_err(gpu_error)?;
            self.device
                .device()
                .CreateShaderResourceView(
                    self.cursor_texture.as_ref().unwrap().1.raw(),
                    None,
                    Some(&mut cursor_view),
                )
                .map_err(gpu_error)?;
            self.device
                .device()
                .CreateUnorderedAccessView(self.cursor_output.raw(), None, Some(&mut output_view))
                .map_err(gpu_error)?;
            let context = self.device.context();
            context.UpdateSubresource(
                &self.cursor_constants,
                0,
                None,
                constants.as_ptr().cast(),
                0,
                0,
            );
            context.CSSetShader(&self.cursor_shader, None);
            context.CSSetConstantBuffers(0, Some(&[Some(self.cursor_constants.clone())]));
            context.CSSetShaderResources(0, Some(&[background_view, cursor_view]));
            context.CSSetUnorderedAccessViews(0, 1, Some(&output_view), None);
            context.Dispatch(self.size.0.div_ceil(16), self.size.1.div_ceil(16), 1);
            context.CSSetShaderResources(0, Some(&[None, None]));
            context.CSSetUnorderedAccessViews(0, 1, Some(&None), None);
            context.CSSetShader(None, None);
        }
        self.device.check().map_err(gpu_error)
    }
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
            prefer_hardware_encoder: true,
            enable_microphone: false,
            enable_system_audio: false,
            show_cursor: true,
            keyboard: None,
            mouse_trail_rgba: [255, 0, 0, 180],
            mouse_trail_duration_ms: 400,
            mouse_click_rgba: [255, 0, 0, 180],
        }
    }

    #[test]
    #[ignore = "requires offscreen D3D11 VideoProcessor and a native H.264 encoder"]
    fn hardware_sparse_effects_match_cpu_draw_order_and_expiration() -> Result<()> {
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
        for state in [&mut cpu_state, &mut gpu_state] {
            state.keyboard = Some(KeyboardOverlay::new(size, Box::new(Rasterizer)));
            for (at_ms, down) in [(0, true), (150, false)] {
                state.pending_keys.push_back(KeyEvent {
                    at_ms,
                    key: 65,
                    down,
                    label: "A".into(),
                    modifiers: vec![],
                });
            }
            state.clicks.push_back(RenderClick {
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
        let mut desc = observation.layers[0].texture.desc();
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
        let staging = staging.unwrap();
        for (index, timestamp) in [0, 33, 66, 100, 150, 250, 400, 600, 1800, 2200]
            .into_iter()
            .enumerate()
        {
            let cursor = AttachedCursorSample {
                x: 145 + index as i32 * 2,
                y: 150,
                visible: timestamp < 400,
                shape: CursorShapeState::Embedded(shape.clone()),
            };
            let expected =
                cpu_state.compose_with_cursor(&config, &cpu_frame, timestamp, Some(&cursor))?;
            gpu.compose(
                &mut gpu_state,
                &config,
                &observation,
                timestamp,
                Some(&cursor),
                &surface,
            )?;
            unsafe {
                device
                    .context()
                    .CopyResource(&staging, gpu.final_rgb.as_ref().unwrap().raw());
            }
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                device
                    .context()
                    .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
            }
            .map_err(gpu_error)?;
            let mut max_error = 0;
            for y in 0..size.1 as usize {
                let row = unsafe {
                    std::slice::from_raw_parts(
                        mapped.pData.cast::<u8>().add(y * mapped.RowPitch as usize),
                        size.0 as usize * 4,
                    )
                };
                for (x, pixel) in row.chunks_exact(4).enumerate() {
                    let reference = &expected[(y * size.0 as usize + x) * 4..][..4];
                    for (channel, component) in [2, 1, 0].into_iter().enumerate() {
                        max_error = max_error.max(pixel[component].abs_diff(reference[channel]));
                    }
                }
            }
            unsafe {
                device.context().Unmap(&staging, 0);
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
    fn only_hardware_h264_direct_mp4_uses_native_surfaces() {
        let mut config = config(PathBuf::from("test.mp4"), CaptureBackendKind::Auto);
        assert!(eligible(&config));
        config.prefer_hardware_encoder = false;
        assert!(!eligible(&config));
        config.prefer_hardware_encoder = true;
        config.format = ExportFormat::Gif;
        assert!(!eligible(&config));
    }

    #[test]
    #[ignore = "requires an offscreen D3D11 hardware device"]
    fn hardware_cursor_shader_matches_cpu_for_alpha_mask_xor_scaling_and_clipping() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        use windows::core::Interface;
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }.map_err(gpu_error)?;
        let adapter = unsafe { factory.EnumAdapters1(0) }
            .map_err(gpu_error)?
            .cast()
            .map_err(gpu_error)?;
        let device = SharedDevice::create(&adapter).map_err(gpu_error)?;
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
                256,
                0,
            );
        }
        let mut compositor = GpuVisualCompositor::new(device.clone(), size, size, 30)?;
        let mut desc = compositor.cursor_output.desc();
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
        for mode in [
            CursorCompositionMode::AlphaBlend,
            CursorCompositionMode::MaskedColor,
        ] {
            let shape = CursorShape::from_rgba(
                2,
                1,
                4,
                3,
                mode,
                [
                    255, 0, 0, 0, 0, 255, 0, 255, 19, 81, 243, 128, 255, 255, 255, 255,
                ]
                .repeat(3),
            );
            for source in [(64, 48), (128, 96), (45, 37)] {
                for (x, y) in [(-1, -1), (23, 17), (63, 47)] {
                    let sample = AttachedCursorSample {
                        x,
                        y,
                        visible: true,
                        shape: CursorShapeState::Embedded(shape.clone()),
                    };
                    let mut expected = reference.clone();
                    draw_cursor(&mut expected, size, source, &sample, &mut HashMap::new());
                    compositor.cursor(&background, &sample, &shape, source)?;
                    unsafe {
                        device
                            .context()
                            .CopyResource(&staging, compositor.cursor_output.raw());
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
                    assert_eq!(
                        actual, expected,
                        "cursor {mode:?} at {x},{y}, source {source:?}"
                    );
                }
            }
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
            // Fail with the precise stage instead of accepting startup fallback.
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
