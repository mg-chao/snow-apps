use crate::{ScreenRecorderError, error::Result};
use snow_core::recording_clock::RecordingClock;
use snow_macos::input::{InputEvent, InputObserver, InputStatus};
use snow_media::{
    geometry::{DesktopTransform, PixelRect, PixelSize},
    time::{ClockDomain, MediaTime},
};
use snow_recording_effects::{
    keyboard_overlay::{KeyboardOverlay, KeyboardOverlayConfig},
    laser_trail::LaserTrail,
    mouse_effects::{CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks_to},
    mouse_hook::ObservedMouseButton,
    surface::{Surface, Tile, TileSurface},
};
use std::collections::VecDeque;

#[derive(Clone)]
pub struct NativeEffectsConfig {
    pub click_rgba: [u8; 4],
    pub trail_rgba: [u8; 4],
    pub trail_duration_ms: u64,
    pub clicks: bool,
    pub trail: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub show_keyboard: bool,
    pub record_mouse_clicks: bool,
    pub highlight_rgba: [u8; 4],
}
impl Default for NativeEffectsConfig {
    fn default() -> Self {
        Self {
            click_rgba: [64, 160, 255, 220],
            trail_rgba: [255, 64, 80, 230],
            trail_duration_ms: 500,
            clicks: false,
            trail: false,
            keyboard: None,
            show_keyboard: false,
            record_mouse_clicks: false,
            highlight_rgba: [0; 4],
        }
    }
}
pub(crate) struct Effects {
    input: Option<InputObserver>,
    cursor: Option<snow_macos::cursor::CursorSampler>,
    config: NativeEffectsConfig,
    output: PixelSize,
    surface: TileSurface,
    highlight: TileSurface,
    held_buttons: [bool; 5],
    pending_input: VecDeque<InputEvent>,
    show_cursor: bool,
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
        let observing =
            config.clicks || config.trail || config.show_keyboard || config.record_mouse_clicks;
        if !observing && !separate && config.highlight_rgba[3] == 0 {
            return Ok(None);
        }
        let input = observing
            .then(|| {
                InputObserver::start(
                    config.show_keyboard,
                    config.clicks || config.trail || config.record_mouse_clicks,
                )
                .map_err(super::macos::native_error)
            })
            .transpose()?;
        let cursor = if separate
            || (cursor_mode != snow_media::CursorMode::Hidden && config.highlight_rgba[3] != 0)
        {
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
            trail: LaserTrail::new(config.trail_duration_ms),
            config,
            output,
            surface: TileSurface::new((output.width, output.height)),
            highlight: TileSurface::new((output.width, output.height)),
            held_buttons: [false; 5],
            pending_input: VecDeque::new(),
            show_cursor: separate,
            clicks: VecDeque::new(),

            keyboard,
            generation: 0,
            status: InputStatus::Active,
        }))
    }
    pub fn reset(&mut self, now: u64) {
        self.held_buttons = [false; 5];
        self.pending_input.clear();
        self.highlight.clear();
        self.clicks.clear();
        self.trail.clear();
        if let Some(keyboard) = &mut self.keyboard {
            keyboard.model.reset(now);
        }
        if let Some(input) = &self.input {
            while input.events.try_recv().is_ok() {}
        }
    }
    pub fn highlight_tiles(&self) -> Vec<Tile> {
        self.highlight.snapshot()
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
        if let Some(input) = &self.input {
            for event in input.events.try_iter() {
                if self.pending_input.len() == 256 {
                    self.pending_input.clear();
                    self.held_buttons = [false; 5];
                    if let Some(keyboard) = &mut self.keyboard {
                        keyboard.model.reset(now);
                    }
                }
                self.pending_input.push_back(event);
            }
        }
        while let Some(event) = self.pending_input.pop_front() {
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
            if at_ms > now {
                self.pending_input.push_front(event);
                break;
            }
            let point = project(event.x, event.y, transform, destination);
            if self.config.clicks
                && let Some((x, y)) = point
            {
                let button = match event.kind {
                    1 => Some(ObservedMouseButton::Left),
                    3 => Some(ObservedMouseButton::Right),
                    25 if event.mouse_button == 2 => Some(ObservedMouseButton::Middle),
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
            if self.config.record_mouse_clicks {
                let button = match event.kind {
                    1 | 2 => Some(ObservedMouseButton::Left),
                    3 | 4 => Some(ObservedMouseButton::Right),
                    25 | 26 => match event.mouse_button {
                        2 => Some(ObservedMouseButton::Middle),
                        3 => Some(ObservedMouseButton::Button4),
                        4 => Some(ObservedMouseButton::Button5),
                        _ => None,
                    },
                    _ => None,
                };
                if let Some(button) = button {
                    let down = matches!(event.kind, 1 | 3 | 25);
                    if (down && point.is_some()) || (!down && self.held_buttons[button as usize]) {
                        self.held_buttons[button as usize] = down;
                        if let (Some(style), Some(keyboard)) =
                            (&self.config.keyboard, &mut self.keyboard)
                        {
                            let observation =
                                snow_recording_effects::mouse_hook::MouseClickObservation {
                                    at,
                                    x: 0,
                                    y: 0,
                                    button,
                                    down,
                                    modifiers: [18, 19, 17, 20]
                                        .map(|bit| event.modifiers & (1 << bit) != 0),
                                };
                            keyboard.model.event(observation.event(
                                at_ms,
                                style,
                                self.config.show_keyboard,
                            ));
                        }
                    }
                }
            }
            if self.config.trail && matches!(event.kind, 5..=7 | 27) {
                let size = (self.output.width, self.output.height);
                self.trail.observe(point, size, size, at_ms);
            }
            if self.config.show_keyboard
                && let Some(keyboard) = &mut self.keyboard
                && let Some(style) = &self.config.keyboard
                && let Some(event) =
                    snow_recording_effects::keyboard_hook::KeyObservation::from_macos(
                        &event,
                        at,
                        self.generation,
                    )
            {
                keyboard.model.event(event.event(at_ms, style));
            }
        }
        self.surface.clear();
        self.highlight.clear();
        let mut cursor_shape = None;
        if let Some(cursor) = &mut self.cursor {
            let sample = cursor.sample().map_err(super::macos::native_error)?;
            let shape = sample.shape.ok_or_else(|| {
                ScreenRecorderError::UnsupportedFeature(
                    "public system cursor shape became unavailable".into(),
                )
            })?;
            if let Some((x, y)) = project(sample.x, sample.y, transform, destination) {
                snow_recording_effects::mouse_effects::draw_highlight_to(
                    &mut self.highlight,
                    (x, y),
                    self.config.highlight_rgba,
                    false,
                );
                if self.show_cursor {
                    cursor_shape = Some((x, y, shape));
                }
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
                self.config.click_rgba,
                (self.output.width, self.output.height),
            );
        }
        if self.config.trail {
            self.trail
                .draw_to(&mut self.surface, now, self.config.trail_rgba);
        }
        if let Some((x, y, shape)) = cursor_shape {
            draw_cursor(
                &mut self.surface,
                &shape,
                x,
                y,
                f64::from(destination.width) / transform.source.width,
                f64::from(destination.height) / transform.source.height,
            );
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
