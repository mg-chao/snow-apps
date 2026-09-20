//! Cursor observations are sampled independently in host time. Assets are
//! spooled to disk; only a bounded shape cache and last observation remain live.
use super::*;
use snow_macos::cursor::{CursorSample, CursorSampler, CursorShape};
use snow_recording_model::{
    CursorFrameRecord, CursorShapeCompositionMode, CursorShapeRecord, MouseStoreWriter,
};
use std::collections::VecDeque;

pub(super) struct EditableCursor {
    sampler: Option<CursorSampler>,
    input: Option<snow_macos::input::InputObserver>,
    clicks: bool,
    generation: u64,
    status: snow_macos::input::InputStatus,
    assets: CursorAssets,
}
impl EditableCursor {
    pub fn new(
        directory: &std::path::Path,
        separate: bool,
        clicks: bool,
        trail: bool,
    ) -> Result<Self> {
        let sampler = if separate {
            let mut sampler = CursorSampler::default();
            if sampler.sample().map_err(native_error)?.shape.is_none() {
                return Err(unavailable());
            }
            Some(sampler)
        } else {
            None
        };
        let input = (clicks || trail)
            .then(|| snow_macos::input::InputObserver::start(false, true).map_err(native_error))
            .transpose()?;
        Ok(Self {
            sampler,
            input,
            clicks,
            generation: 0,
            status: snow_macos::input::InputStatus::Active,
            assets: CursorAssets::new(directory)?,
        })
    }
    pub fn sample(&mut self, video: &NativeRecordingSession) -> Result<Option<String>> {
        if video.config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        let transform = video.capture.transform();
        let destination = aspect_fit(transform.output, video.config.output)
            .map_err(|error| ScreenRecorderError::InvalidConfig(error.to_string()))?;
        let mut interruption = None;
        if let Some(input) = &self.input {
            let generation = input.generation();
            let status = input.status();
            if generation != self.generation || status != self.status {
                interruption = Some(format!(
                    "editable input observation: {status:?}; generation {generation}"
                ));
                self.generation = generation;
                self.status = status;
                self.assets
                    .position(video.clock.active_elapsed_ms(Instant::now()), None, None)?;
                while input.events.try_recv().is_ok() {}
                return Ok(interruption);
            }
            while let Ok(event) = input.events.try_recv() {
                if event.generation != generation || video.paused {
                    continue;
                }
                let Some(at) = i64::try_from(event.timestamp_ns).ok().and_then(|value| {
                    snow_macos::time::host_time_to_instant(snow_media::time::MediaTime {
                        value,
                        timescale: 1_000_000_000,
                        domain: snow_media::time::ClockDomain::MacHostTime,
                        epoch: 0,
                    })
                }) else {
                    continue;
                };
                if !video.clock.is_active_at(at) {
                    continue;
                }
                self.assets.input(
                    &event,
                    video.clock.active_elapsed_ms(at),
                    transform,
                    destination,
                    self.clicks,
                    self.sampler.is_none(),
                )?;
            }
        }
        if !video.paused
            && let Some(sampler) = &mut self.sampler
        {
            self.assets.observe(
                sampler.sample().map_err(native_error)?,
                video.clock.active_elapsed_ms(Instant::now()),
                transform,
                destination,
            )?;
        }
        Ok(interruption)
    }
    pub fn finish(self, path: &std::path::Path) -> Result<()> {
        self.assets.writer.finish(path)?;
        Ok(())
    }
}
fn unavailable() -> ScreenRecorderError {
    ScreenRecorderError::UnsupportedFeature(
        "public system cursor shape is unavailable for editable separate cursor capture".into(),
    )
}
struct CachedShape {
    source: CursorShape,
    scale: (f64, f64),
    id: u64,
}
struct CursorAssets {
    writer: MouseStoreWriter,
    cache: VecDeque<CachedShape>,
    next_id: u64,
    last: Option<(i32, i32, bool, Option<u64>)>,
}
impl CursorAssets {
    fn new(directory: &std::path::Path) -> Result<Self> {
        Ok(Self {
            writer: MouseStoreWriter::new(directory)?,
            cache: VecDeque::new(),
            next_id: 1,
            last: None,
        })
    }
    fn observe(
        &mut self,
        sample: CursorSample,
        timestamp_ms: u64,
        transform: DesktopTransform,
        destination: PixelRect,
    ) -> Result<()> {
        let point = crate::macos_effects::project(sample.x, sample.y, transform, destination);
        let shape_id = if point.is_some() {
            let shape = sample.shape.ok_or_else(unavailable)?;
            let scale = (
                f64::from(destination.width) / transform.source.width,
                f64::from(destination.height) / transform.source.height,
            );
            let cached = self.cache.iter().position(|cached| {
                cached.scale == scale
                    && cached.source.width == shape.width
                    && cached.source.height == shape.height
                    && cached.source.point_width == shape.point_width
                    && cached.source.point_height == shape.point_height
                    && cached.source.hotspot_x == shape.hotspot_x
                    && cached.source.hotspot_y == shape.hotspot_y
                    && cached.source.rgba == shape.rgba
            });
            if let Some(index) = cached {
                let cached = self.cache.remove(index).unwrap();
                let id = cached.id;
                self.cache.push_back(cached);
                Some(id)
            } else {
                let id = self.next_id;
                self.next_id = id.checked_add(1).ok_or_else(|| {
                    ScreenRecorderError::InvalidConfig("cursor shape identifier overflow".into())
                })?;
                self.writer.shape(&rasterize(&shape, scale, id)?)?;
                if self.cache.len() == 8 {
                    self.cache.pop_front();
                }
                self.cache.push_back(CachedShape {
                    source: shape,
                    scale,
                    id,
                });
                Some(id)
            }
        } else {
            None
        };
        self.position(timestamp_ms, point, shape_id)
    }
    fn input(
        &mut self,
        event: &snow_macos::input::InputEvent,
        timestamp_ms: u64,
        transform: DesktopTransform,
        destination: PixelRect,
        clicks: bool,
        positions: bool,
    ) -> Result<()> {
        let point = crate::macos_effects::project(event.x, event.y, transform, destination);
        if positions {
            self.position(timestamp_ms, point, None)?;
        }
        if clicks && let Some((x, y)) = point {
            let button = match event.kind {
                1 | 2 => Some(snow_recording_model::MouseButton::Left),
                3 | 4 => Some(snow_recording_model::MouseButton::Right),
                25 | 26 => Some(snow_recording_model::MouseButton::Middle),
                _ => None,
            };
            if let Some(button) = button {
                self.writer.click(&snow_recording_model::ClickEventRecord {
                    timestamp_ms,
                    x,
                    y,
                    button,
                    down: matches!(event.kind, 1 | 3 | 25),
                })?;
            }
        }
        Ok(())
    }
    fn position(
        &mut self,
        timestamp_ms: u64,
        point: Option<(i32, i32)>,
        shape_id: Option<u64>,
    ) -> Result<()> {
        let (x, y) = point.unwrap_or((0, 0));
        let key = (x, y, point.is_some(), shape_id);
        if self.last != Some(key) {
            self.writer.frame(&CursorFrameRecord {
                timestamp_ms,
                x,
                y,
                visible: point.is_some(),
                shape_id,
            })?;
            self.last = Some(key);
        }
        Ok(())
    }
}
fn rasterize(shape: &CursorShape, scale: (f64, f64), id: u64) -> Result<CursorShapeRecord> {
    let width = (shape.point_width * scale.0).round();
    let height = (shape.point_height * scale.1).round();
    let hotspot_x = (shape.hotspot_x * scale.0).round();
    let hotspot_y = (shape.hotspot_y * scale.1).round();
    if !width.is_finite()
        || !height.is_finite()
        || !(1.0..=2048.0).contains(&width)
        || !(1.0..=2048.0).contains(&height)
        || !hotspot_x.is_finite()
        || !hotspot_y.is_finite()
        || hotspot_x < 0.0
        || hotspot_y < 0.0
        || hotspot_x > width
        || hotspot_y > height
        || shape.width == 0
        || shape.height == 0
        || u64::from(shape.width) * u64::from(shape.height) * 4 != shape.rgba.len() as u64
    {
        return Err(ScreenRecorderError::UnsupportedFeature(
            "editable cursor geometry is unsupported".into(),
        ));
    }
    let (width, height) = (width as u32, height as u32);
    let mut rgba = Vec::with_capacity(width as usize * height as usize * 4);
    for y in 0..height {
        for x in 0..width {
            let sx = u64::from(x) * u64::from(shape.width) / u64::from(width);
            let sy = u64::from(y) * u64::from(shape.height) / u64::from(height);
            let offset = (sy * u64::from(shape.width) + sx) as usize * 4;
            let pixel = &shape.rgba[offset..offset + 4];
            let alpha = u32::from(pixel[3]);
            for value in &pixel[..3] {
                rgba.push(
                    (u32::from(*value) * 255 + alpha / 2)
                        .checked_div(alpha)
                        .unwrap_or(0)
                        .min(255) as u8,
                );
            }
            rgba.push(pixel[3]);
        }
    }
    Ok(CursorShapeRecord {
        shape_id: id,
        width,
        height,
        hotspot_x: hotspot_x as u32,
        hotspot_y: hotspot_y as u32,
        mode: CursorShapeCompositionMode::AlphaBlend,
        shape_rgba: rgba,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn shape(value: u8) -> CursorShape {
        CursorShape {
            width: 2,
            height: 2,
            point_width: 1.0,
            point_height: 1.0,
            hotspot_x: 0.5,
            hotspot_y: 0.5,
            rgba: [value, 0, 0, 128].repeat(4).into(),
        }
    }
    #[test]
    fn input_assets_preserve_buttons_and_transform_without_requiring_cursor_shapes() {
        let directory = tempfile::tempdir().unwrap();
        let mut assets = CursorAssets::new(directory.path()).unwrap();
        let transform = DesktopTransform::new(
            snow_media::geometry::DesktopRect {
                space: snow_media::geometry::DesktopSpace::Points,
                x: -10.0,
                y: 20.0,
                width: 20.0,
                height: 10.0,
            },
            PixelSize::new(40, 20).unwrap(),
        )
        .unwrap();
        let destination = PixelRect {
            x: 0,
            y: 10,
            width: 40,
            height: 20,
        };
        let mut event = snow_macos::input::InputEvent {
            kind: 5,
            x: 0.0,
            y: 25.0,
            timestamp_ns: 0,
            key_code: 0,
            mouse_button: 0,
            keyboard_type: 0,
            modifiers: 0,
            repeat: false,
            text: [0; 16],
            text_len: 0,
            generation: 0,
        };
        for (at, kind) in [5, 1, 2, 3, 4, 25, 26].into_iter().enumerate() {
            event.kind = kind;
            assets
                .input(&event, at as u64, transform, destination, true, true)
                .unwrap();
        }
        event.x = 11.0;
        assets
            .input(&event, 8, transform, destination, true, true)
            .unwrap();
        let path = directory.path().join("mouse.bin");
        assets.writer.finish(&path).unwrap();
        let store = snow_recording_model::read_mouse_records(&path).unwrap();
        assert!(store.cursor_shapes.is_empty());
        assert_eq!(store.cursor_frames.len(), 2);
        assert_eq!(
            (store.cursor_frames[0].x, store.cursor_frames[0].y),
            (20, 20)
        );
        assert!(!store.cursor_frames[1].visible);
        assert_eq!(store.clicks.len(), 6);
        for pair in store.clicks.chunks_exact(2) {
            assert!(pair[0].down);
            assert!(!pair[1].down);
            assert_eq!(pair[0].button, pair[1].button);
        }
        assert_eq!(
            store.clicks[4].button,
            snow_recording_model::MouseButton::Middle
        );
    }
    #[test]
    fn cursor_assets_preserve_retina_hotspots_alpha_visibility_and_bounded_cache() {
        let directory = tempfile::tempdir().unwrap();
        let mut assets = CursorAssets::new(directory.path()).unwrap();
        let transform = DesktopTransform::new(
            snow_media::geometry::DesktopRect {
                space: snow_media::geometry::DesktopSpace::Points,
                x: -10.0,
                y: 20.0,
                width: 20.0,
                height: 10.0,
            },
            PixelSize::new(40, 20).unwrap(),
        )
        .unwrap();
        let destination = PixelRect {
            x: 0,
            y: 10,
            width: 40,
            height: 20,
        };
        for at in [0, 10] {
            assets
                .observe(
                    CursorSample {
                        x: 0.0,
                        y: 25.0,
                        shape: Some(shape(128)),
                    },
                    at,
                    transform,
                    destination,
                )
                .unwrap();
        }
        assets
            .observe(
                CursorSample {
                    x: 30.0,
                    y: 25.0,
                    shape: None,
                },
                20,
                transform,
                destination,
            )
            .unwrap();
        assert!(
            assets
                .observe(
                    CursorSample {
                        x: 0.0,
                        y: 25.0,
                        shape: None
                    },
                    21,
                    transform,
                    destination
                )
                .is_err()
        );
        for value in 0..20 {
            assets
                .observe(
                    CursorSample {
                        x: 0.0,
                        y: 25.0,
                        shape: Some(shape(value)),
                    },
                    30 + u64::from(value),
                    transform,
                    destination,
                )
                .unwrap();
            assert!(assets.cache.len() <= 8);
        }
        let path = directory.path().join("mouse.bin");
        assets.writer.finish(&path).unwrap();
        let store = snow_recording_model::read_mouse_records(&path).unwrap();
        assert_eq!(store.cursor_frames.len(), 22);
        assert_eq!(
            (store.cursor_frames[0].x, store.cursor_frames[0].y),
            (20, 20)
        );
        assert!(!store.cursor_frames[1].visible);
        let first = &store.cursor_shapes[0];
        assert_eq!(
            (first.width, first.height, first.hotspot_x, first.hotspot_y),
            (2, 2, 1, 1)
        );
        assert_eq!(&first.shape_rgba[..4], &[255, 0, 0, 128]);
        assert!(rasterize(&shape(128), (f64::INFINITY, 1.0), 2).is_err());
    }
}
