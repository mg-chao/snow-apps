use crate::{ScreenRecorderError, error::Result};
use snow_core::recording_clock::RecordingClock;
use snow_macos::input::{InputEvent, InputObserver, InputStatus};
use snow_media::{
    geometry::{DesktopTransform, PixelRect, PixelSize},
    time::{ClockDomain, MediaTime},
};
use snow_recording_effects::{
    InputEffectsState,
    keyboard_filter::{KeyboardFilter, KeyboardFilterConfig},
    keyboard_overlay::{KeyboardOverlay, KeyboardOverlayConfig},
    mouse_effects::RenderClick,
    mouse_hook::ObservedMouseButton,
    surface::{Surface, Tile, TileSurface},
};
use std::collections::VecDeque;

mod trail;
use trail::FrameTrail;

#[derive(Clone)]
pub struct NativeEffectsConfig {
    pub click_rgba: [u8; 4],
    pub trail_rgba: [u8; 4],
    pub trail_duration_ms: u64,
    pub clicks: bool,
    pub trail: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub keyboard_filter: KeyboardFilterConfig,
    pub show_keyboard: bool,
    pub record_mouse_clicks: bool,
    pub record_mouse_scrolling: bool,
    pub highlight_radius: u32,
    pub highlight_rgba: [u8; 4],
}
impl Default for NativeEffectsConfig {
    fn default() -> Self {
        Self {
            keyboard_filter: Default::default(),
            click_rgba: [64, 160, 255, 220],
            trail_rgba: [255, 64, 80, 230],
            trail_duration_ms: 500,
            clicks: false,
            trail: false,
            keyboard: None,
            show_keyboard: false,
            record_mouse_clicks: false,
            record_mouse_scrolling: false,
            highlight_radius: snow_recording_model::render::DEFAULT_MOUSE_HIGHLIGHT_RADIUS,
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
    input_effects: InputEffectsState,
    keyboard_filter: KeyboardFilter,
    trail: FrameTrail,
    generation: u64,
    status: InputStatus,
    render_policy: snow_recording_model::EffectsConfig,
    last_geometry: Option<(DesktopTransform, PixelRect)>,
    last_cursor: Option<(i32, i32, snow_macos::cursor::CursorShape)>,
    dirty: bool,
    was_animating: bool,
    rendered: RenderedEffects,
}

/// Compare only sparse effect pixels; never scan the captured desktop to prove
/// a held output. Revisions remain pending until the recorder submits them.
#[derive(Default)]
struct RenderedEffects {
    tiles: Vec<Tile>,
    highlight: Vec<Tile>,
    revision: u64,
}

impl RenderedEffects {
    fn update(&mut self, tiles: Vec<Tile>, highlight: Vec<Tile>) {
        let same = |left: &[Tile], right: &[Tile]| {
            left.len() == right.len()
                && left.iter().zip(right).all(|(left, right)| {
                    left.x == right.x && left.y == right.y && left.pixels == right.pixels
                })
        };
        if !same(&self.tiles, &tiles) || !same(&self.highlight, &highlight) {
            self.revision = self
                .revision
                .checked_add(1)
                .expect("effect revision overflow");
            self.tiles = tiles;
            self.highlight = highlight;
        }
    }
}

impl Effects {
    pub fn new(
        config: NativeEffectsConfig,
        output: PixelSize,
        cursor_mode: snow_media::CursorMode,
    ) -> Result<Option<Self>> {
        if !snow_recording_model::render::MOUSE_HIGHLIGHT_RADIUS_RANGE
            .contains(&config.highlight_radius)
        {
            return Err(ScreenRecorderError::InvalidConfig(
                "mouse highlight radius must be between 8 and 128 pixels".into(),
            ));
        }
        let separate = cursor_mode == snow_media::CursorMode::Separate;
        let observing = config.clicks
            || config.trail
            || config.show_keyboard
            || config.record_mouse_clicks
            || config.record_mouse_scrolling;
        if !observing && !separate && config.highlight_rgba[3] == 0 {
            return Ok(None);
        }
        let input = observing
            .then(|| {
                InputObserver::start(
                    config.show_keyboard,
                    config.clicks
                        || config.trail
                        || config.record_mouse_clicks
                        || config.record_mouse_scrolling,
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
                        .with_keycap_size(config.keycap_size)
                        .with_icon_color(config.text_rgba),
                )
            })
            .transpose()?;
        Ok(Some(Self::with_sources(
            config, output, separate, input, cursor, keyboard,
        )))
    }

    fn with_sources(
        config: NativeEffectsConfig,
        output: PixelSize,
        separate: bool,
        input: Option<InputObserver>,
        cursor: Option<snow_macos::cursor::CursorSampler>,
        keyboard: Option<KeyboardOverlay>,
    ) -> Self {
        Self {
            input,
            cursor,
            keyboard_filter: KeyboardFilter::default(),
            trail: FrameTrail::new(),
            input_effects: InputEffectsState::new(config.trail_duration_ms).with_keyboard(keyboard),
            render_policy: snow_recording_model::EffectsConfig {
                mouse_trail_rgba: if config.trail {
                    config.trail_rgba
                } else {
                    [0; 4]
                },
                mouse_click_rgba: if config.clicks {
                    config.click_rgba
                } else {
                    [0; 4]
                },
                ..Default::default()
            },
            config,
            output,
            surface: TileSurface::new((output.width, output.height)),
            highlight: TileSurface::new((output.width, output.height)),
            held_buttons: [false; 5],
            pending_input: VecDeque::new(),
            show_cursor: separate,
            generation: 0,
            status: InputStatus::Active,
            last_geometry: None,
            last_cursor: None,
            dirty: true,
            was_animating: false,
            rendered: RenderedEffects::default(),
        }
    }
    fn key_event(&mut self, event: snow_recording_effects::keyboard_overlay::KeyEvent) {
        if self.input_effects.pending_keys.len() == snow_recording_effects::state::MAX_PENDING_KEYS
        {
            self.keyboard_filter.reset();
            self.input_effects.reset_keyboard(event.at_ms);
        }
        if let Some(event) = self
            .keyboard_filter
            .event(self.config.keyboard_filter, event)
        {
            self.input_effects.queue_key(event);
            self.dirty = true;
        }
    }

    pub fn reset(&mut self, now: u64) {
        self.held_buttons = [false; 5];
        self.pending_input.clear();
        self.highlight.clear();
        self.trail.clear();
        self.keyboard_filter.reset();
        self.input_effects.reset_inputs(now);
        self.dirty = true;
        if let Some(input) = &self.input {
            while input.events.try_recv().is_ok() {}
        }
    }
    pub fn highlight_tiles(&self) -> Vec<Tile> {
        self.rendered.highlight.clone()
    }
    pub fn revision(&self) -> u64 {
        self.rendered.revision
    }
    pub fn draw(
        &mut self,
        clock: &RecordingClock,
        transform: DesktopTransform,
        destination: PixelRect,
        now: u64,
    ) -> Result<(Vec<Tile>, Option<String>)> {
        let geometry = (transform, destination);
        if self.last_geometry != Some(geometry) {
            if let Some((previous, previous_destination)) = self.last_geometry {
                let reproject = snow_recording_model::PointerTransform::between(
                    previous.source,
                    previous_destination,
                    transform.source,
                    destination,
                )
                .map_err(|error| ScreenRecorderError::InvalidConfig(error.to_string()))?;
                self.input_effects.reproject_pointer(reproject, reproject);
                self.trail.reproject(reproject);
            }
            self.dirty = true;
            self.last_geometry = Some(geometry);
        }
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
                    self.keyboard_filter.reset();
                    self.input_effects.reset_keyboard(now);
                    self.dirty = true;
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
                    self.dirty = true;
                    self.input_effects.click(RenderClick {
                        timestamp_ms: at_ms,
                        x,
                        y,
                        button,
                    });
                }
            }
            if self.config.record_mouse_scrolling
                && event.kind == 22
                && point.is_some()
                && let Some(button) = ObservedMouseButton::from_scroll_delta(event.scroll_delta_y)
                && let Some(style) = &self.config.keyboard
            {
                let observation = snow_recording_effects::mouse_hook::MouseClickObservation {
                    at,
                    x: 0,
                    y: 0,
                    button,
                    down: true,
                    modifiers: [false; 8],
                };
                let key = observation.event(at_ms, style, false);
                self.key_event(key);
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
                        if let Some(style) = &self.config.keyboard {
                            let observation =
                                snow_recording_effects::mouse_hook::MouseClickObservation {
                                    at,
                                    x: 0,
                                    y: 0,
                                    button,
                                    down,
                                    modifiers:
                                        snow_recording_effects::keyboard_hook::macos_modifier_sides(
                                            event.modifiers,
                                        ),
                                };
                            self.key_event(
                                observation.event(
                                    at_ms,
                                    style,
                                    self.config
                                        .keyboard_filter
                                        .include_mouse_modifiers(self.config.show_keyboard),
                                ),
                            );
                            self.dirty = true;
                        }
                    }
                }
            }
            if self.config.trail && matches!(event.kind, 5..=7 | 27) {
                self.dirty |= self.trail.observe(point, at_ms);
            }
            if self.config.show_keyboard
                && let Some(style) = &self.config.keyboard
                && let Some(event) =
                    snow_recording_effects::keyboard_hook::KeyObservation::from_macos(
                        &event,
                        at,
                        self.generation,
                    )
            {
                self.key_event(event.event(at_ms, style));
            }
        }
        let cursor_shape = if let Some(cursor) = &mut self.cursor {
            let sample = cursor.sample().map_err(super::macos::native_error)?;
            let shape = sample.shape.ok_or_else(|| {
                ScreenRecorderError::UnsupportedFeature(
                    "public system cursor shape became unavailable".into(),
                )
            })?;
            project(sample.x, sample.y, transform, destination).map(|(x, y)| (x, y, shape))
        } else {
            None
        };
        if self.last_cursor != cursor_shape {
            self.dirty = true;
            self.last_cursor = cursor_shape;
        }
        if self.config.trail {
            self.trail.advance(
                &mut self.input_effects,
                (self.output.width, self.output.height),
                (self.output.width, self.output.height),
                now,
            );
        }
        let animating = self
            .input_effects
            .has_active_animation(&self.render_policy, now);
        if !self.dirty && !self.was_animating && !animating {
            return Ok((self.rendered.tiles.clone(), interruption));
        }
        self.surface.clear();
        self.highlight.clear();
        if let Some((x, y, _)) = &self.last_cursor {
            snow_recording_effects::mouse_effects::draw_highlight_to(
                &mut self.highlight,
                (*x, *y),
                self.config.highlight_rgba,
                false,
                self.config.highlight_radius,
            );
        }
        self.input_effects.draw_mouse_layers_to(
            &mut self.surface,
            now,
            (self.output.width, self.output.height),
            if self.config.trail {
                self.config.trail_rgba
            } else {
                [0; 4]
            },
            self.config.trail_duration_ms,
            if self.config.clicks {
                self.config.click_rgba
            } else {
                [0; 4]
            },
        );
        if self.show_cursor
            && let Some((x, y, shape)) = &self.last_cursor
        {
            draw_cursor(
                &mut self.surface,
                shape,
                *x,
                *y,
                f64::from(destination.width) / transform.source.width,
                f64::from(destination.height) / transform.source.height,
            );
        }
        self.input_effects
            .draw_keyboard_to(&mut self.surface, now)
            .map_err(ScreenRecorderError::Encode)?;
        self.rendered
            .update(self.surface.snapshot(), self.highlight.snapshot());
        self.was_animating = animating;
        self.dirty = false;
        Ok((self.rendered.tiles.clone(), interruption))
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
    let hotspot_x = (shape.hotspot_x * sx)
        .round()
        .clamp(0.0, f64::from(width - 1)) as i32;
    let hotspot_y = (shape.hotspot_y * sy)
        .round()
        .clamp(0.0, f64::from(height - 1)) as i32;
    let x = x - hotspot_x;
    let y = y - hotspot_y;
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

    fn effects_fixture() -> (Effects, RecordingClock, DesktopTransform, PixelRect) {
        let size = PixelSize::new(256, 128).unwrap();
        let config = NativeEffectsConfig {
            clicks: true,
            trail: true,
            ..Default::default()
        };
        let effects = Effects::with_sources(config, size, false, None, None, None);
        let transform = DesktopTransform::new(
            snow_media::geometry::DesktopRect {
                space: snow_media::geometry::DesktopSpace::Points,
                x: 0.0,
                y: 0.0,
                width: 256.0,
                height: 128.0,
            },
            size,
        )
        .unwrap();
        let destination = PixelRect {
            x: 0,
            y: 0,
            width: 256,
            height: 128,
        };
        (
            effects,
            RecordingClock::new(std::time::Instant::now()),
            transform,
            destination,
        )
    }

    #[test]
    fn moving_viewport_reprojects_clicks_without_releasing_input_or_renewing_ages() {
        let (mut effects, clock, transform, destination) = effects_fixture();
        effects.held_buttons[0] = true;
        effects.input_effects.click(RenderClick {
            timestamp_ms: 600,
            x: 100,
            y: 64,
            button: ObservedMouseButton::Left,
        });
        effects.draw(&clock, transform, destination, 617).unwrap();
        let mut moved = transform;
        moved.source.x += 50.0;
        let (_, interruption) = effects.draw(&clock, moved, destination, 634).unwrap();
        assert!(interruption.is_none());
        assert!(effects.held_buttons[0]);
        let click = effects.input_effects.clicks.front().unwrap();
        assert_eq!((click.timestamp_ms, click.x, click.y), (600, 50, 64));
        effects.draw(&clock, moved, destination, 651).unwrap();
        assert_eq!(effects.input_effects.clicks.front().unwrap().x, 50);
    }

    #[test]
    fn inactive_configured_effects_reuse_pixels_and_expired_effects_clear_once() {
        let (mut effects, clock, transform, destination) = effects_fixture();
        for now in [0, 17, 34, 100, 500] {
            assert!(
                effects
                    .draw(&clock, transform, destination, now)
                    .unwrap()
                    .0
                    .is_empty()
            );
            assert_eq!(effects.revision(), 0);
        }
        effects.input_effects.click(RenderClick {
            timestamp_ms: 600,
            x: 100,
            y: 64,
            button: ObservedMouseButton::Left,
        });
        assert!(
            !effects
                .draw(&clock, transform, destination, 617)
                .unwrap()
                .0
                .is_empty()
        );
        let visible_revision = effects.revision();
        assert!(visible_revision > 0);
        assert!(
            effects
                .draw(&clock, transform, destination, 2000)
                .unwrap()
                .0
                .is_empty()
        );
        let cleared_revision = effects.revision();
        assert_eq!(cleared_revision, visible_revision + 1);
        for now in [2017, 2034, 3000] {
            assert!(
                effects
                    .draw(&clock, transform, destination, now)
                    .unwrap()
                    .0
                    .is_empty()
            );
            assert_eq!(effects.revision(), cleared_revision);
        }
    }

    #[test]
    fn cached_effects_match_unconditional_rendering_through_motion_expiry_and_reset() {
        let (mut cached, clock, transform, destination) = effects_fixture();
        let (mut reference, _, _, _) = effects_fixture();
        for (now, position, reset) in [
            (0, Some((10, 64)), false),
            (17, Some((60, 64)), false),
            (34, Some((90, 64)), false),
            (51, Some((90, 64)), false),
            (120, None, false),
            (137, Some((190, 64)), false),
            (154, Some((230, 64)), false),
            (170, Some((230, 64)), false),
            (800, Some((230, 64)), false),
            (817, Some((230, 64)), false),
            (834, Some((230, 64)), true),
            (851, Some((10, 64)), false),
            (868, Some((80, 64)), false),
        ] {
            for effects in [&mut cached, &mut reference] {
                if reset {
                    effects.reset(now);
                }
                effects.dirty |= effects.trail.observe(position, now);
            }
            reference.dirty = true;
            let actual = cached.draw(&clock, transform, destination, now).unwrap().0;
            let expected = reference
                .draw(&clock, transform, destination, now)
                .unwrap()
                .0;
            assert_eq!(actual.len(), expected.len(), "{now}");
            for (actual, expected) in actual.iter().zip(&expected) {
                assert_eq!((actual.x, actual.y), (expected.x, expected.y));
                assert_eq!(actual.pixels, expected.pixels, "{now}");
            }
        }
    }

    #[test]
    fn sparse_revision_tracks_pixel_placement_highlight_and_removal() {
        let tile = |x, value| Tile {
            x,
            y: 0,
            pixels: std::sync::Arc::new(vec![value; 16]),
        };
        let mut rendered = RenderedEffects::default();
        rendered.update(vec![tile(0, 3)], Vec::new());
        let retained = rendered.tiles[0].pixels.clone();
        assert_eq!(rendered.revision, 1);
        rendered.update(vec![tile(0, 3)], Vec::new());
        assert_eq!(rendered.revision, 1);
        assert!(std::sync::Arc::ptr_eq(&retained, &rendered.tiles[0].pixels));
        rendered.update(vec![tile(128, 3)], Vec::new());
        assert_eq!(rendered.revision, 2);
        rendered.update(vec![tile(128, 4)], Vec::new());
        assert_eq!(rendered.revision, 3);
        rendered.update(vec![tile(128, 4)], vec![tile(0, 8)]);
        assert_eq!(rendered.revision, 4);
        rendered.update(Vec::new(), Vec::new());
        assert_eq!(rendered.revision, 5);
        rendered.update(Vec::new(), Vec::new());
        assert_eq!(rendered.revision, 5);
    }

    #[test]
    fn scaled_cursor_endpoint_hotspot_matches_recorded_shape_placement() {
        let shape = snow_macos::cursor::CursorShape {
            width: 2,
            height: 2,
            point_width: 1.0,
            point_height: 1.0,
            hotspot_x: 0.5,
            hotspot_y: 0.5,
            rgba: [128, 0, 0, 128].repeat(4).into(),
        };
        let mut surface = TileSurface::new((128, 128));
        draw_cursor(&mut surface, &shape, 8, 8, 1.0, 1.0);
        let tiles = surface.snapshot();
        assert_eq!(tiles.len(), 1);
        let at = (8 * 128 + 8) * 4;
        assert_eq!(&tiles[0].pixels[at..at + 4], &[128, 0, 0, 128]);
        let previous = (7 * 128 + 7) * 4;
        assert_eq!(&tiles[0].pixels[previous..previous + 4], &[0; 4]);
    }

    #[test]
    fn cursor_hotspot_tracks_highlight_across_retina_scaling_and_tile_edges() {
        // An asymmetric bitmap with a red hotspot and blue pixels below it.
        let mut rgba = vec![0; 4 * 6 * 4];
        rgba[(2 * 4 + 2) * 4..(2 * 4 + 2) * 4 + 4].copy_from_slice(&[255, 0, 0, 255]);
        rgba[(4 * 4 + 2) * 4..(4 * 4 + 2) * 4 + 4].copy_from_slice(&[0, 0, 255, 255]);
        let shape = snow_macos::cursor::CursorShape {
            width: 4,
            height: 6,
            point_width: 2.0,
            point_height: 3.0,
            hotspot_x: 1.0,
            hotspot_y: 1.0,
            rgba: rgba.into(),
        };
        for scale in [1.0, 1.5, 2.0] {
            let transform = DesktopTransform::new(
                snow_media::geometry::DesktopRect {
                    space: snow_media::geometry::DesktopSpace::Points,
                    x: -300.0,
                    y: -200.0,
                    width: 100.0,
                    height: 100.0,
                },
                PixelSize::new(200, 200).unwrap(),
            )
            .unwrap();
            let destination = PixelRect {
                x: 28,
                y: 28,
                width: (100.0 * scale) as u32,
                height: (100.0 * scale) as u32,
            };
            let (x, y) = project(-250.0, -150.0, transform, destination).unwrap();
            let mut surface = TileSurface::new((256, 256));
            draw_cursor(&mut surface, &shape, x, y, scale, scale);
            let tiles = surface.snapshot();
            let pixel = |px: i32, py: i32| {
                let tile = tiles
                    .iter()
                    .find(|tile| {
                        px as u32 >= tile.x
                            && py as u32 >= tile.y
                            && (px as u32) < tile.x + 128
                            && (py as u32) < tile.y + 128
                    })
                    .unwrap();
                let offset = ((py as u32 - tile.y) * 128 + px as u32 - tile.x) as usize * 4;
                &tile.pixels[offset..offset + 4]
            };
            assert_eq!(pixel(x, y), &[255, 0, 0, 255], "hotspot at scale {scale}");
            assert_eq!(
                pixel(x, y + scale.ceil() as i32),
                &[0, 0, 255, 255],
                "cursor must extend below the hotspot at scale {scale}"
            );
        }
    }

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
