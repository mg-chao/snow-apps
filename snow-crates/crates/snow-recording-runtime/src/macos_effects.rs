use crate::{ScreenRecorderError, error::Result};
use snow_core::recording_clock::RecordingClock;
use snow_macos::input::{InputEvent, InputObserver, InputStatus};
use snow_media::{
    geometry::{DesktopTransform, PixelRect, PixelSize},
    time::{ClockDomain, MediaTime},
};
use snow_recording_effects::{
    keyboard_overlay::{KeyEvent, KeyboardOverlay, KeyboardOverlayConfig},
    laser_trail::LaserTrail,
    mouse_effects::{CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks_to},
    mouse_hook::ObservedMouseButton,
    surface::{Surface, Tile, TileSurface},
};
use std::collections::VecDeque;

#[derive(Clone, Default)]
pub struct NativeEffectsConfig {
    pub clicks: bool,
    pub trail: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
}
pub(crate) struct Effects {
    input: Option<InputObserver>,
    cursor: Option<snow_macos::cursor::CursorSampler>,
    config: NativeEffectsConfig,
    output: PixelSize,
    surface: TileSurface,
    clicks: VecDeque<RenderClick>,
    trail: LaserTrail,
    keyboard: Option<KeyboardOverlay>,
    generation: u64,
    status: InputStatus,
}
impl Effects {
    pub fn new(
        config: NativeEffectsConfig,
        output: PixelSize,
        cursor_mode: snow_media::CursorMode,
    ) -> Result<Option<Self>> {
        let separate = cursor_mode == snow_media::CursorMode::Separate;
        let observing = config.clicks || config.trail || config.keyboard.is_some();
        if !observing && !separate {
            return Ok(None);
        }
        let input = observing
            .then(|| {
                InputObserver::start(config.keyboard.is_some(), config.clicks || config.trail)
                    .map_err(super::macos::native_error)
            })
            .transpose()?;
        let cursor = if separate {
            let mut sampler = snow_macos::cursor::CursorSampler::default();
            if sampler
                .sample()
                .map_err(super::macos::native_error)?
                .shape
                .is_none()
            {
                return Err(ScreenRecorderError::UnsupportedFeature(
                    "public system cursor shape is unavailable; separate cursor mode cannot start"
                        .into(),
                ));
            }
            Some(sampler)
        } else {
            None
        };
        let keyboard = config
            .keyboard
            .as_ref()
            .map(|config| {
                let rasterizer = snow_recording_effects::keyboard_rasterizer::create(config)
                    .map_err(ScreenRecorderError::UnsupportedFeature)?;
                Ok::<_, ScreenRecorderError>(
                    KeyboardOverlay::new((output.width, output.height), rasterizer)
                        .with_keycap_size(config.keycap_size),
                )
            })
            .transpose()?;
        Ok(Some(Self {
            input,
            cursor,
            config,
            output,
            surface: TileSurface::new((output.width, output.height)),
            clicks: VecDeque::new(),
            trail: LaserTrail::default(),
            keyboard,
            generation: 0,
            status: InputStatus::Active,
        }))
    }
    pub fn reset(&mut self, now: u64) {
        self.clicks.clear();
        self.trail.clear();
        if let Some(keyboard) = &mut self.keyboard {
            keyboard.model.reset(now);
        }
        if let Some(input) = &self.input {
            while input.events.try_recv().is_ok() {}
        }
    }
    pub fn draw(
        &mut self,
        clock: &RecordingClock,
        transform: DesktopTransform,
        destination: PixelRect,
        now: u64,
    ) -> Result<(Vec<Tile>, Option<String>)> {
        let generation = self.input.as_ref().map_or(0, InputObserver::generation);
        let status = self
            .input
            .as_ref()
            .map_or(InputStatus::Active, InputObserver::status);
        let interruption = (status != self.status || generation != self.generation)
            .then(|| format!("input observation: {status:?}; generation {generation}"));
        if interruption.is_some() {
            self.reset(now);
        }
        self.generation = generation;
        self.status = status;
        while let Some(event) = self
            .input
            .as_ref()
            .and_then(|input| input.events.try_recv().ok())
        {
            if event.generation != generation {
                continue;
            }
            let Some(at) = i64::try_from(event.timestamp_ns).ok().and_then(|value| {
                snow_macos::time::host_time_to_instant(MediaTime {
                    value,
                    timescale: 1_000_000_000,
                    domain: ClockDomain::MacHostTime,
                    epoch: 0,
                })
            }) else {
                continue;
            };
            if !clock.is_active_at(at) {
                continue;
            }
            let at_ms = clock.active_elapsed_ms(at);
            let point = project(event.x, event.y, transform, destination);
            if self.config.clicks
                && let Some((x, y)) = point
            {
                let button = match event.kind {
                    1 => Some(ObservedMouseButton::Left),
                    3 => Some(ObservedMouseButton::Right),
                    25 => Some(ObservedMouseButton::Middle),
                    _ => None,
                };
                if let Some(button) = button {
                    if self.clicks.len() == CLICK_QUEUE_DEPTH {
                        self.clicks.pop_front();
                    }
                    self.clicks.push_back(RenderClick {
                        timestamp_ms: at_ms,
                        x,
                        y,
                        button,
                    });
                }
            }
            if self.config.trail && matches!(event.kind, 5..=7 | 27) {
                let size = (self.output.width, self.output.height);
                self.trail.observe(point, size, size, at_ms);
            }
            if let Some(keyboard) = &mut self.keyboard
                && let Some(event) = key_event(&event, at_ms)
            {
                keyboard.model.event(event);
            }
        }
        self.surface.clear();
        if let Some(cursor) = &mut self.cursor {
            let sample = cursor.sample().map_err(super::macos::native_error)?;
            let shape = sample.shape.ok_or_else(|| {
                ScreenRecorderError::UnsupportedFeature(
                    "public system cursor shape became unavailable".into(),
                )
            })?;
            if let Some((x, y)) = project(sample.x, sample.y, transform, destination) {
                draw_cursor(
                    &mut self.surface,
                    &shape,
                    x,
                    y,
                    f64::from(destination.width) / transform.source.width,
                    f64::from(destination.height) / transform.source.height,
                );
            }
        }
        while self
            .clicks
            .front()
            .is_some_and(|click| now.saturating_sub(click.timestamp_ms) > CLICK_ANIMATION_MS)
        {
            self.clicks.pop_front();
        }
        if self.config.clicks {
            draw_clicks_to(
                &mut self.surface,
                &self.clicks,
                now,
                [64, 160, 255, 220],
                (self.output.width, self.output.height),
            );
        }
        if self.config.trail {
            self.trail
                .draw_to(&mut self.surface, now, [255, 64, 80, 230]);
        }
        if let Some(keyboard) = &mut self.keyboard {
            keyboard
                .draw_to(&mut self.surface, now)
                .map_err(ScreenRecorderError::Encode)?;
        }
        Ok((self.surface.snapshot(), interruption))
    }
}
fn draw_cursor(
    surface: &mut TileSurface,
    shape: &snow_macos::cursor::CursorShape,
    x: i32,
    y: i32,
    sx: f64,
    sy: f64,
) {
    let width = (shape.point_width * sx).round().clamp(1.0, 2048.0) as u32;
    let height = (shape.point_height * sy).round().clamp(1.0, 2048.0) as u32;
    let x = x - (shape.hotspot_x * sx).round() as i32;
    let y = y - (shape.hotspot_y * sy).round() as i32;
    for dy in 0..height {
        for dx in 0..width {
            let px = x + i32::try_from(dx).unwrap();
            let py = y + i32::try_from(dy).unwrap();
            if px < 0 || py < 0 || px as u32 >= surface.size().0 || py as u32 >= surface.size().1 {
                continue;
            }
            let source_x = u64::from(dx) * u64::from(shape.width) / u64::from(width);
            let source_y = u64::from(dy) * u64::from(shape.height) / u64::from(height);
            let index = (source_y as usize * shape.width as usize + source_x as usize) * 4;
            let source = &shape.rgba[index..index + 4];
            surface.span(px as u32, py as u32, 1, |pixel, _| {
                for channel in 0..4 {
                    pixel[channel] = (u32::from(source[channel])
                        + (u32::from(pixel[channel]) * (255 - u32::from(source[3])) + 127) / 255)
                        .min(255) as u8;
                }
            });
        }
    }
}
pub(crate) fn project(
    x: f64,
    y: f64,
    transform: DesktopTransform,
    destination: PixelRect,
) -> Option<(i32, i32)> {
    let r = transform.source;
    if !x.is_finite() || !y.is_finite() || x < r.x || y < r.y || x >= r.right() || y >= r.bottom() {
        return None;
    }
    Some((
        (f64::from(destination.x) + (x - r.x) / r.width * f64::from(destination.width)).floor()
            as i32,
        (f64::from(destination.y) + (y - r.y) / r.height * f64::from(destination.height)).floor()
            as i32,
    ))
}
fn key_event(event: &InputEvent, at_ms: u64) -> Option<KeyEvent> {
    if !matches!(event.kind, 10 | 11) || event.repeat {
        return None;
    }
    let label = match event.key_code {
        36 => "Return".into(),
        48 => "Tab".into(),
        49 => "Space".into(),
        51 => "Delete".into(),
        53 => "Esc".into(),
        123 => "←".into(),
        124 => "→".into(),
        125 => "↓".into(),
        126 => "↑".into(),
        _ => String::from_utf16_lossy(&event.text[..event.text_len.min(event.text.len())])
            .chars()
            .filter(|c| !c.is_control())
            .collect::<String>(),
    };
    let label =
        snow_macos::text::keyboard_label(event.key_code, event.keyboard_type, event.modifiers)
            .filter(|_| !matches!(event.key_code, 36 | 48 | 49 | 51 | 53 | 123..=126))
            .unwrap_or_else(|| {
                if label.is_empty() {
                    format!("Key {}", event.key_code)
                } else {
                    label
                }
            });
    let modifiers = [
        (18, 0x11, "Control"),
        (19, 0x12, "Option"),
        (17, 0x10, "Shift"),
        (20, 0x5b, "Command"),
    ]
    .into_iter()
    .filter(|(bit, _, _)| event.modifiers & (1 << bit) != 0)
    .map(|(_, key, label)| (key, label.to_owned()))
    .collect();
    Some(KeyEvent {
        at_ms,
        key: 0x100 + event.key_code,
        down: event.kind == 10,
        label,
        modifiers,
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn point_projection_honors_letterbox_and_retina_points() {
        let transform = DesktopTransform::new(
            snow_media::geometry::DesktopRect {
                space: snow_media::geometry::DesktopSpace::Points,
                x: -100.0,
                y: 40.0,
                width: 100.0,
                height: 50.0,
            },
            PixelSize::new(200, 100).unwrap(),
        )
        .unwrap();
        let dest = PixelRect {
            x: 0,
            y: 25,
            width: 200,
            height: 100,
        };
        assert_eq!(project(-50.0, 65.0, transform, dest), Some((100, 75)));
        assert_eq!(project(0.0, 65.0, transform, dest), None);
        assert_eq!(project(f64::NAN, 65.0, transform, dest), None);
    }
}
