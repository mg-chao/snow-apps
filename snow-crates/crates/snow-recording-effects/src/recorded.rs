//! Shared deterministic composition for live observations and recorded input replay.
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::sync::Arc;

use snow_recording_model::{
    CursorFrameRecord, CursorShapeCompositionMode, CursorShapeRecord, FinalizedTimeline,
    InputMouseButton, RecordedInput, RecordedInputEvent, RenderConfig, RenderFrame,
};

use crate::keyboard_overlay::{KeyboardOverlay, KeyboardOverlayConfig, KeycapRasterizer};
use crate::mouse_effects::{RenderClick, draw_highlight_to, scale_coordinate, scale_point};
use crate::mouse_hook::ObservedMouseButton;
use crate::playback::PlaybackRenderer;
use crate::state::{CoalescedPointer, InputEffectsState};
use crate::surface::{RgbaSurface, Surface, Tile, TileSurface};

const MAX_CURSOR_SHAPES: usize = 128;
const MAX_CURSOR_BYTES: usize = 32 * 1024 * 1024;

pub struct RecordedEffects {
    config: RenderConfig,
    source_size: (u32, u32),
    timeline: Option<FinalizedTimeline>,
    playback_origin_ms: u64,
    cursor: Option<CursorFrameRecord>,
    shapes: HashMap<u64, Arc<CursorShapeRecord>>,
    shape_order: VecDeque<u64>,
    shape_bytes: usize,
    pub input_effects: InputEffectsState,
    continuity: u64,
    pointer: CoalescedPointer,
    last_admission: Option<(u64, u64)>,
    last_frame: Option<u64>,
    playback: PlaybackRenderer,
    tiles: TileSurface,
}

impl RecordedEffects {
    pub fn new(
        config: RenderConfig,
        source_size: (u32, u32),
        rasterizer: Option<Box<dyn KeycapRasterizer>>,
    ) -> Result<Self, String> {
        let playback_rasterizer = if matches!(
            config.playback_overlay,
            snow_recording_model::PlaybackOverlay::PlaybackTime { .. }
        ) {
            let style = KeyboardOverlayConfig {
                font: config
                    .effects
                    .keyboard
                    .as_ref()
                    .and_then(|style| style.font.clone()),
                keycap_size: 64,
                background_rgba: [0; 4],
                text_rgba: [255; 4],
                border_rgba: [0; 4],
                labels: BTreeMap::new(),
            };
            Some(crate::keyboard_rasterizer::create(&style)?)
        } else {
            None
        };
        Self::new_with_rasterizers(config, source_size, rasterizer, playback_rasterizer)
    }

    pub fn new_with_rasterizers(
        config: RenderConfig,
        source_size: (u32, u32),
        rasterizer: Option<Box<dyn KeycapRasterizer>>,
        playback_rasterizer: Option<Box<dyn KeycapRasterizer>>,
    ) -> Result<Self, String> {
        config.validate()?;
        if [source_size.0, source_size.1]
            .into_iter()
            .any(|value| value == 0 || value > 32768)
        {
            return Err("effect source dimensions must be between 1 and 32768 pixels".into());
        }
        let output = (config.output_width, config.output_height);
        let playback = PlaybackRenderer::new(config.playback_overlay, output, playback_rasterizer)?;
        let mut keyboard = if config.effects.show_keyboard || config.effects.record_mouse_clicks {
            let style = config
                .effects
                .keyboard
                .as_ref()
                .ok_or("keyboard effects require a style")?;
            let rasterizer = match rasterizer {
                Some(value) => value,
                None => crate::keyboard_rasterizer::create(style)?,
            };
            Some(KeyboardOverlay::new(output, rasterizer).with_keycap_size(style.keycap_size))
        } else {
            None
        };
        if let Some(keyboard) = &mut keyboard {
            keyboard.set_top_reservation(playback.top_reservation());
        }
        let input_effects = InputEffectsState {
            keyboard,
            ..InputEffectsState::new(config.effects.mouse_trail_duration_ms)
        };
        Ok(Self {
            config,
            source_size,
            timeline: None,
            playback_origin_ms: 0,
            cursor: None,
            shapes: HashMap::new(),
            shape_order: VecDeque::new(),
            shape_bytes: 0,
            input_effects,
            continuity: 0,
            pointer: CoalescedPointer::new(),
            last_admission: None,
            last_frame: None,
            playback,
            tiles: TileSurface::new(output),
        })
    }

    pub fn with_timeline(mut self, timeline: FinalizedTimeline) -> Self {
        self.timeline = Some(timeline);
        self
    }

    /// Input observations stay on the source clock; the playback badge uses clip time.
    pub fn with_playback_origin(mut self, origin_ms: u64) -> Self {
        self.playback_origin_ms = origin_ms;
        self
    }

    pub fn observe(&mut self, event: &RecordedInputEvent) -> Result<(), String> {
        event.validate().map_err(|error| error.to_string())?;
        if self
            .last_admission
            .is_some_and(|(at, sequence)| event.timestamp_ms < at || event.sequence <= sequence)
        {
            return Err("effect input admission order is not monotonic".into());
        }
        self.last_admission = Some((event.timestamp_ms, event.sequence));
        match &event.event {
            RecordedInput::CursorShape(shape) => self.store_shape(shape),
            RecordedInput::Cursor(cursor) => {
                self.cursor = Some(cursor.clone());
            }
            RecordedInput::Pointer {
                position,
                continuity,
                at_ms,
            } => {
                if self.continuity != *continuity {
                    self.pointer.break_path();
                }
                self.continuity = *continuity;
                self.pointer.observe(*position, *at_ms);
            }
            RecordedInput::Click(click) => {
                let button = observed_button(click.button);
                if click.down && button.has_ring() && self.config.effects.mouse_click_rgba[3] != 0 {
                    self.input_effects.click(RenderClick {
                        timestamp_ms: click.key.at_ms,
                        x: click.x,
                        y: click.y,
                        button,
                    });
                }
                if self.config.effects.record_mouse_clicks {
                    self.input_effects.queue_key(click.key.clone());
                }
            }
            RecordedInput::Key(key) => {
                if self.config.effects.show_keyboard {
                    self.input_effects.queue_key(key.clone());
                }
            }
            RecordedInput::Reset => {
                self.input_effects.reset_inputs(event.timestamp_ms);
                self.cursor = None;
                self.pointer.clear();
            }
            RecordedInput::KeyboardReset => self.input_effects.reset_keyboard(event.timestamp_ms),
        }
        Ok(())
    }

    fn store_shape(&mut self, shape: &CursorShapeRecord) {
        if let Some(previous) = self.shapes.remove(&shape.shape_id) {
            self.shape_bytes -= previous.shape_rgba.len();
            self.shape_order.retain(|id| *id != shape.shape_id);
        }
        while self.shapes.len() >= MAX_CURSOR_SHAPES
            || self.shape_bytes + shape.shape_rgba.len() > MAX_CURSOR_BYTES
        {
            let Some(id) = self.shape_order.pop_front() else {
                break;
            };
            if let Some(shape) = self.shapes.remove(&id) {
                self.shape_bytes -= shape.shape_rgba.len();
            }
        }
        self.shape_bytes += shape.shape_rgba.len();
        self.shape_order.push_back(shape.shape_id);
        self.shapes.insert(shape.shape_id, Arc::new(shape.clone()));
    }

    /// The destination is pristine source content; old overlays must never be fed back.
    pub fn apply_rgba(&mut self, pixels: &mut [u8], frame: RenderFrame) -> Result<(), String> {
        let size = (self.config.output_width, self.config.output_height);
        if pixels.len() != size.0 as usize * size.1 as usize * 4 {
            return Err("effect destination size disagrees with rendering policy".into());
        }
        self.draw_to(
            &mut RgbaSurface {
                pixels,
                dimensions: size,
            },
            frame,
        )
    }

    pub fn draw_to<S: Surface>(
        &mut self,
        surface: &mut S,
        frame: RenderFrame,
    ) -> Result<(), String> {
        if surface.size() != (self.config.output_width, self.config.output_height) {
            return Err("effect destination dimensions disagree with rendering policy".into());
        }
        if self
            .last_frame
            .is_some_and(|last| frame.timestamp_ms < last)
        {
            return Err("effect rendering cannot move backwards on the timeline".into());
        }
        self.last_frame = Some(frame.timestamp_ms);
        let now = frame.timestamp_ms;
        if self.config.effects.mouse_trail_rgba[3] != 0 {
            self.pointer.advance(
                &mut self.input_effects,
                self.source_size,
                surface.size(),
                now,
            );
        }
        let effects = &self.config.effects;
        if effects.show_cursor
            && effects.mouse_highlight_rgba[3] != 0
            && let Some(cursor) = self.cursor.as_ref().filter(|cursor| {
                cursor.visible
                    && cursor.x >= 0
                    && cursor.y >= 0
                    && cursor.x < self.source_size.0 as i32
                    && cursor.y < self.source_size.1 as i32
            })
        {
            draw_highlight_to(
                surface,
                scale_point(cursor.x, cursor.y, self.source_size, surface.size()),
                effects.mouse_highlight_rgba,
                cfg!(windows) && S::OPAQUE,
            );
        }
        self.input_effects
            .draw_mouse_to(surface, now, self.source_size, effects);
        if effects.show_cursor
            && let Some(cursor) = self.cursor.as_ref().filter(|cursor| cursor.visible)
            && let Some(id) = cursor.shape_id
        {
            let shape = self
                .shapes
                .get(&id)
                .ok_or("recorded cursor references an unavailable shape")?;
            draw_cursor_to(surface, cursor, shape, self.source_size)?;
        }
        self.input_effects.draw_keyboard_to(surface, now)?;
        let playback_frame = RenderFrame {
            timestamp_ms: frame.timestamp_ms.saturating_sub(self.playback_origin_ms),
            ..frame
        };
        self.playback
            .draw_to(surface, playback_frame, self.timeline)?;
        Ok(())
    }

    /// Premultiplied SDR layers for native HDR composition. Masked cursor pixels need a
    /// background-aware backend and are rejected instead of silently changing their meaning.
    pub fn render_tiles(&mut self, frame: RenderFrame) -> Result<Vec<Tile>, String> {
        let mut tiles = std::mem::replace(
            &mut self.tiles,
            TileSurface::new((self.config.output_width, self.config.output_height)),
        );
        tiles.clear();
        let result = self.draw_to(&mut tiles, frame);
        let snapshot = result.map(|()| tiles.snapshot());
        self.tiles = tiles;
        snapshot
    }

    pub fn retained_shape_bytes(&self) -> usize {
        self.shape_bytes
    }
    pub fn playback_caption(&self) -> &str {
        self.playback.caption()
    }
}

fn observed_button(button: InputMouseButton) -> ObservedMouseButton {
    match button {
        InputMouseButton::Left => ObservedMouseButton::Left,
        InputMouseButton::Right => ObservedMouseButton::Right,
        InputMouseButton::Middle => ObservedMouseButton::Middle,
        InputMouseButton::Button4 => ObservedMouseButton::Button4,
        InputMouseButton::Button5 => ObservedMouseButton::Button5,
    }
}

fn draw_cursor_to<S: Surface>(
    surface: &mut S,
    cursor: &CursorFrameRecord,
    shape: &CursorShapeRecord,
    source: (u32, u32),
) -> Result<(), String> {
    let output = surface.size();
    let (cursor_x, cursor_y) = scale_point(cursor.x, cursor.y, source, output);
    let width = (u64::from(shape.width) * u64::from(output.0) / u64::from(source.0)).max(1) as u32;
    let height =
        (u64::from(shape.height) * u64::from(output.1) / u64::from(source.1)).max(1) as u32;
    let x = cursor_x.saturating_sub(scale_coordinate(shape.hotspot_x as i32, source.0, output.0));
    let y = cursor_y.saturating_sub(scale_coordinate(shape.hotspot_y as i32, source.1, output.1));
    let left = i64::from(x).max(0);
    let top = i64::from(y).max(0);
    let right = (i64::from(x) + i64::from(width)).min(i64::from(output.0));
    let bottom = (i64::from(y) + i64::from(height)).min(i64::from(output.1));
    for target_y in top..bottom {
        let sy = (target_y - i64::from(y)) as u64 * u64::from(shape.height) / u64::from(height);
        for target_x in left..right {
            let sx = (target_x - i64::from(x)) as u64 * u64::from(shape.width) / u64::from(width);
            let index = ((sy * u64::from(shape.width) + sx) * 4) as usize;
            let color: [u8; 4] = shape.shape_rgba[index..index + 4]
                .try_into()
                .expect("validated cursor pixel");
            match (shape.mode, color[3]) {
                (CursorShapeCompositionMode::AlphaBlend, _)
                | (CursorShapeCompositionMode::MaskedColor, 1..=254) => {
                    surface.blend_pixel(target_x as i32, target_y as i32, color)
                }
                (CursorShapeCompositionMode::MaskedColor, 0 | 255) => {
                    if color == [0, 0, 0, 255] {
                        continue;
                    }
                    if !S::OPAQUE && color[3] == 255 {
                        return Err("masked XOR cursor needs a background-aware compositor".into());
                    }
                    surface.span(target_x as u32, target_y as u32, 1, |pixel, _| {
                        for channel in 0..3 {
                            pixel[channel] = (pixel[channel] & color[3]) ^ color[channel];
                        }
                        pixel[3] = 255;
                    });
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::keyboard_overlay::KeyEvent;
    use crate::mouse_effects::draw_clicks_to;
    use snow_recording_model::{EffectsConfig, PlaybackOverlay, RecordedMouseClick};
    fn config(overlay: PlaybackOverlay) -> RenderConfig {
        RenderConfig {
            output_width: 120,
            output_height: 100,
            output_fps: 30,
            effects: EffectsConfig::default(),
            playback_overlay: overlay,
        }
    }
    fn input(sequence: u64, at: u64, event: RecordedInput) -> RecordedInputEvent {
        RecordedInputEvent {
            sequence,
            timestamp_ms: at,
            event,
        }
    }
    fn frame(index: u64, at: u64, progress: f64) -> RenderFrame {
        RenderFrame {
            index,
            pts: index,
            timestamp_ms: at,
            progress,
        }
    }
    fn pixels() -> Vec<u8> {
        [20, 40, 60, 255].repeat(120 * 100)
    }
    struct Glyph;
    impl KeycapRasterizer for Glyph {
        fn rasterize(
            &mut self,
            _: &str,
            _: f32,
        ) -> Result<crate::keyboard_overlay::Keycap, String> {
            Ok(crate::keyboard_overlay::Keycap {
                width: 8,
                height: 12,
                pixels: [255; 4].repeat(8 * 12),
            })
        }
    }
    #[test]
    fn actual_live_preview_and_replay_match_clocked_inputs_resets_and_expiry() {
        use crate::preview::{EffectsPreview, PreviewConfig};
        use crate::surface::TILE_SIZE;

        let size = (512, 256);
        let style = KeyboardOverlayConfig {
            font: None,
            keycap_size: 32,
            background_rgba: [0; 4],
            text_rgba: [255; 4],
            border_rgba: [0; 4],
            labels: BTreeMap::new(),
        };
        let effects = EffectsConfig {
            show_cursor: false,
            keyboard: Some(style.clone()),
            show_keyboard: true,
            mouse_trail_rgba: [255, 40, 60, 180],
            mouse_click_rgba: [40, 180, 255, 160],
            ..EffectsConfig::default()
        };
        let mut live = EffectsPreview::new(
            PreviewConfig {
                region: (0, 0, size.0, size.1),
                canvas: size,
                output: size,
                trail: effects.mouse_trail_rgba,
                trail_duration_ms: effects.mouse_trail_duration_ms,
                click: effects.mouse_click_rgba,
                highlight: [0; 4],
                record_mouse_clicks: false,
                show_keyboard: true,
                keyboard: Some(style),
                generation: 0,
            },
            Some(Box::new(Glyph)),
        );
        let mut replay = RecordedEffects::new_with_rasterizers(
            RenderConfig {
                output_width: size.0,
                output_height: size.1,
                output_fps: 60,
                effects,
                playback_overlay: PlaybackOverlay::None,
            },
            size,
            Some(Box::new(Glyph)),
            None,
        )
        .unwrap();
        let mut sequence = 0;
        for (index, at) in [0, 17, 34, 51, 100, 200, 500, 800, 2000]
            .into_iter()
            .enumerate()
        {
            let position = (30 + index as i32 * 20, 60);
            let continuity = u64::from(at >= 51);
            live.observe_input(Some(position), at, continuity);
            replay
                .observe(&input(
                    sequence,
                    at,
                    RecordedInput::Pointer {
                        position: Some(position),
                        continuity,
                        at_ms: at,
                    },
                ))
                .unwrap();
            sequence += 1;
            if at == 17 {
                live.click(RenderClick {
                    timestamp_ms: at,
                    x: position.0,
                    y: position.1,
                    button: ObservedMouseButton::Left,
                });
                replay
                    .observe(&input(
                        sequence,
                        at,
                        RecordedInput::Click(RecordedMouseClick {
                            x: position.0,
                            y: position.1,
                            button: InputMouseButton::Left,
                            down: true,
                            key: KeyEvent {
                                at_ms: at,
                                key: 0x200,
                                down: true,
                                label: "Left click".into(),
                                modifiers: vec![],
                            },
                        }),
                    ))
                    .unwrap();
                sequence += 1;
                // Both adapters must sort observation times stably, while
                // retaining equal-time press/release admission order.
                for (at_ms, down) in [(100, false), (34, true), (34, false)] {
                    let key = KeyEvent {
                        at_ms,
                        key: 65,
                        down,
                        label: "Captured A".into(),
                        modifiers: vec![(0x11, "Ctrl".into())],
                    };
                    live.key_event(key.clone());
                    replay
                        .observe(&input(sequence, at, RecordedInput::Key(key)))
                        .unwrap();
                    sequence += 1;
                }
            }
            if at == 500 {
                live.input_effects.reset_inputs(at);
                live.observe(None, at);
                replay
                    .observe(&input(sequence, at, RecordedInput::Reset))
                    .unwrap();
                sequence += 1;
            }
            let live = live.render(at).unwrap();
            let replay = replay.render_tiles(frame(index as u64, at, 0.0)).unwrap();
            let mut expected = HashMap::new();
            for tile in live.iter() {
                let pixels = expected
                    .entry((tile.x, tile.y))
                    .or_insert_with(|| vec![0u8; (TILE_SIZE * TILE_SIZE * 4) as usize]);
                for (destination, source) in
                    pixels.chunks_exact_mut(4).zip(tile.pixels.chunks_exact(4))
                {
                    for channel in 0..4 {
                        destination[channel] = (u32::from(source[channel])
                            + (u32::from(destination[channel]) * (255 - u32::from(source[3]))
                                + 127)
                                / 255)
                            .min(255) as u8;
                    }
                }
            }
            assert_eq!(replay.len(), expected.len(), "sparse layer count at {at}");
            for tile in replay {
                assert_eq!(
                    tile.pixels.as_slice(),
                    expected[&(tile.x, tile.y)].as_slice(),
                    "sparse live/replay pixels at {at}"
                );
            }
        }
    }
    #[test]
    fn cursor_samples_do_not_double_count_trail_and_coalesced_points_keep_observation_age() {
        let mut policy = config(PlaybackOverlay::None);
        policy.effects.show_cursor = false;
        policy.effects.mouse_trail_rgba = [255, 40, 60, 180];
        policy.effects.mouse_trail_duration_ms = 100;
        let mut replay = RecordedEffects::new(policy, (120, 100), None).unwrap();
        let mut sequence = 0;
        let mut reference = InputEffectsState::new(100);
        for (at, x, visible) in [
            (0, 10, true),
            (10, 20, true),
            (15, 80, false),
            (20, 90, true),
        ] {
            replay
                .observe(&input(
                    sequence,
                    at,
                    RecordedInput::Cursor(CursorFrameRecord {
                        timestamp_ms: at,
                        x,
                        y: 50,
                        visible,
                        shape_id: None,
                    }),
                ))
                .unwrap();
            sequence += 1;
        }
        assert!(replay.render_tiles(frame(0, 20, 0.0)).unwrap().is_empty());
        for (index, observed) in [20, 40].into_iter().enumerate() {
            for (at_ms, x) in [(observed - 10, 50), (observed, 30 + index as i32 * 50)] {
                replay
                    .observe(&input(
                        sequence,
                        50 + index as u64 * 20,
                        RecordedInput::Pointer {
                            position: Some((x, 50)),
                            continuity: 0,
                            at_ms,
                        },
                    ))
                    .unwrap();
                sequence += 1;
            }
            reference.observe_pointer(
                Some((30 + index as i32 * 50, 50)),
                (120, 100),
                (120, 100),
                observed,
            );
            let rendered = 50 + index as u64 * 20;
            let actual = replay
                .render_tiles(frame(index as u64 + 1, rendered, 0.0))
                .unwrap();
            let mut surface = TileSurface::new((120, 100));
            reference
                .trail
                .draw_to(&mut surface, rendered, [255, 40, 60, 180]);
            let expected = surface.snapshot();
            assert_eq!(actual.len(), expected.len());
            for (actual, expected) in actual.iter().zip(expected) {
                assert_eq!(actual.pixels, expected.pixels);
            }
        }
        assert!(replay.render_tiles(frame(3, 140, 0.0)).unwrap().is_empty());
    }
    #[test]
    fn masked_cursor_preserves_exact_copy_xor_and_clipped_hotspots() {
        let mut effects =
            RecordedEffects::new(config(PlaybackOverlay::None), (120, 100), None).unwrap();
        effects
            .observe(&input(
                0,
                0,
                RecordedInput::CursorShape(CursorShapeRecord {
                    shape_id: 1,
                    hotspot_x: 0,
                    hotspot_y: 0,
                    width: 3,
                    height: 1,
                    mode: CursorShapeCompositionMode::MaskedColor,
                    shape_rgba: vec![200, 100, 50, 0, 255, 255, 255, 255, 0, 0, 0, 255],
                }),
            ))
            .unwrap();
        effects
            .observe(&input(
                1,
                0,
                RecordedInput::Cursor(CursorFrameRecord {
                    timestamp_ms: 0,
                    x: 0,
                    y: 0,
                    visible: true,
                    shape_id: Some(1),
                }),
            ))
            .unwrap();
        let mut output = pixels();
        effects.apply_rgba(&mut output, frame(0, 0, 0.0)).unwrap();
        assert_eq!(
            &output[..12],
            &[200, 100, 50, 255, 235, 215, 195, 255, 20, 40, 60, 255]
        );
        assert!(effects.render_tiles(frame(0, 0, 0.0)).is_err());
    }
    #[test]
    fn replayed_click_matches_shared_live_rasterizer_and_reset_clears_history() {
        let mut policy = config(PlaybackOverlay::None);
        policy.effects.show_cursor = false;
        policy.effects.mouse_click_rgba = [30, 180, 255, 180];
        let mut effects = RecordedEffects::new(policy, (120, 100), None).unwrap();
        effects
            .observe(&input(
                0,
                0,
                RecordedInput::Click(RecordedMouseClick {
                    x: 60,
                    y: 50,
                    button: InputMouseButton::Left,
                    down: true,
                    key: KeyEvent {
                        at_ms: 0,
                        key: 0x200,
                        down: true,
                        label: "Left click".into(),
                        modifiers: vec![],
                    },
                }),
            ))
            .unwrap();
        let mut expected = pixels();
        draw_clicks_to(
            &mut RgbaSurface {
                pixels: &mut expected,
                dimensions: (120, 100),
            },
            &VecDeque::from([RenderClick {
                timestamp_ms: 0,
                x: 60,
                y: 50,
                button: ObservedMouseButton::Left,
            }]),
            200,
            [30, 180, 255, 180],
            (120, 100),
        );
        let mut output = pixels();
        effects.apply_rgba(&mut output, frame(6, 200, 0.0)).unwrap();
        assert_eq!(output, expected);
        effects
            .observe(&input(1, 201, RecordedInput::Reset))
            .unwrap();
        let mut reset = pixels();
        effects.apply_rgba(&mut reset, frame(7, 234, 0.0)).unwrap();
        assert_eq!(reset, pixels());
    }
    #[test]
    fn playback_bar_advances_when_source_is_static_and_replay_is_idempotent() {
        let mut effects = RecordedEffects::new(
            config(PlaybackOverlay::ProgressBar {
                rgba: [22, 119, 255, 255],
            }),
            (120, 100),
            None,
        )
        .unwrap();
        let mut start = pixels();
        effects.apply_rgba(&mut start, frame(0, 0, 0.0)).unwrap();
        let mut end = pixels();
        effects.apply_rgba(&mut end, frame(1, 33, 1.0)).unwrap();
        assert_eq!(
            &end[(99 * 120) * 4..(99 * 120) * 4 + 4],
            &[22, 119, 255, 255]
        );
        assert_ne!(start, end);
        let mut repeated = pixels();
        effects
            .apply_rgba(&mut repeated, frame(1, 33, 1.0))
            .unwrap();
        assert_eq!(end, repeated);
        assert!(effects.apply_rgba(&mut repeated, frame(0, 0, 0.0)).is_err());
    }
    #[test]
    fn captured_mouse_buttons_and_key_labels_survive_admission_and_future_timing() {
        let mut policy = config(PlaybackOverlay::None);
        policy.effects.show_cursor = false;
        policy.effects.record_mouse_clicks = true;
        policy.effects.keyboard = Some(KeyboardOverlayConfig {
            font: None,
            keycap_size: 32,
            background_rgba: [0; 4],
            text_rgba: [255; 4],
            border_rgba: [0; 4],
            labels: BTreeMap::new(),
        });
        let mut effects = RecordedEffects::new(policy, (120, 100), Some(Box::new(Glyph))).unwrap();
        effects
            .observe(&input(
                0,
                0,
                RecordedInput::Click(RecordedMouseClick {
                    x: 60,
                    y: 50,
                    button: InputMouseButton::Button5,
                    down: true,
                    key: KeyEvent {
                        at_ms: 100,
                        key: 0x204,
                        down: true,
                        label: "Captured fifth button".into(),
                        modifiers: vec![],
                    },
                }),
            ))
            .unwrap();
        let mut before = pixels();
        effects.apply_rgba(&mut before, frame(0, 99, 0.0)).unwrap();
        assert_eq!(before, pixels());
        let mut after = pixels();
        effects.apply_rgba(&mut after, frame(1, 100, 0.0)).unwrap();
        assert_ne!(after, pixels());
    }
    #[test]
    fn time_badge_requires_timeline_and_uses_finalized_duration() {
        let policy = config(PlaybackOverlay::PlaybackTime { rgba: [255; 4] });
        let mut effects =
            RecordedEffects::new_with_rasterizers(policy, (120, 100), None, Some(Box::new(Glyph)))
                .unwrap()
                .with_timeline(FinalizedTimeline::new(3_600_000, 30).unwrap());
        let mut output = pixels();
        effects
            .apply_rgba(&mut output, frame(30, 1000, 0.001))
            .unwrap();
        assert_eq!(effects.playback_caption(), "0:00:01 / 1:00:00");
        assert_ne!(output, pixels());
    }
    #[test]
    fn trimmed_time_badge_uses_a_rebased_clock_and_trimmed_total() {
        let policy = config(PlaybackOverlay::PlaybackTime { rgba: [255; 4] });
        let mut effects =
            RecordedEffects::new_with_rasterizers(policy, (120, 100), None, Some(Box::new(Glyph)))
                .unwrap()
                .with_timeline(FinalizedTimeline::new(3000, 30).unwrap())
                .with_playback_origin(5000);
        effects
            .apply_rgba(&mut pixels(), frame(0, 5000, 0.0))
            .unwrap();
        assert_eq!(effects.playback_caption(), "00:00 / 00:03");
        effects
            .apply_rgba(&mut pixels(), frame(30, 6000, 0.5))
            .unwrap();
        assert_eq!(effects.playback_caption(), "00:01 / 00:03");
    }
    #[test]
    fn top_badge_preserves_keycap_layout_and_fixed_insets_clip_tiny_frames() {
        struct RedGlyph;
        impl KeycapRasterizer for RedGlyph {
            fn rasterize(
                &mut self,
                _: &str,
                _: f32,
            ) -> Result<crate::keyboard_overlay::Keycap, String> {
                Ok(crate::keyboard_overlay::Keycap {
                    width: 8,
                    height: 12,
                    pixels: [255, 0, 0, 255].repeat(8 * 12),
                })
            }
        }
        for size in [(120, 180), (640, 480), (8, 24), (16, 22), (1, 1)] {
            let mut policy = config(PlaybackOverlay::PlaybackTime { rgba: [255; 4] });
            policy.output_width = size.0;
            policy.output_height = size.1;
            policy.effects.show_keyboard = true;
            policy.effects.keyboard = Some(KeyboardOverlayConfig {
                font: None,
                keycap_size: 32,
                background_rgba: [0; 4],
                text_rgba: [255; 4],
                border_rgba: [0; 4],
                labels: BTreeMap::new(),
            });
            let mut keyboard_policy = policy.clone();
            keyboard_policy.playback_overlay = PlaybackOverlay::None;
            let mut keyboard_only = RecordedEffects::new_with_rasterizers(
                keyboard_policy,
                size,
                Some(Box::new(RedGlyph)),
                None,
            )
            .unwrap();
            let mut badge_policy = policy.clone();
            badge_policy.effects.show_keyboard = false;
            let mut badge_only = RecordedEffects::new_with_rasterizers(
                badge_policy,
                size,
                None,
                Some(Box::new(Glyph)),
            )
            .unwrap()
            .with_timeline(FinalizedTimeline::new(10_000, 30).unwrap());
            let mut effects = RecordedEffects::new_with_rasterizers(
                policy,
                size,
                Some(Box::new(RedGlyph)),
                Some(Box::new(Glyph)),
            )
            .unwrap()
            .with_timeline(FinalizedTimeline::new(10_000, 30).unwrap());
            let key = input(
                0,
                0,
                RecordedInput::Key(KeyEvent {
                    at_ms: 0,
                    key: 65,
                    down: true,
                    label: "A".into(),
                    modifiers: vec![],
                }),
            );
            effects.observe(&key).unwrap();
            keyboard_only.observe(&key).unwrap();
            for (index, now) in [0, 80, 160, 240].into_iter().enumerate() {
                let background = [20, 40, 60, 255].repeat(size.0 as usize * size.1 as usize);
                let mut output = background.clone();
                let mut keyboard_pixels = background.clone();
                let mut badge_pixels = background.clone();
                let frame = frame(index as u64, now, 0.0);
                effects.apply_rgba(&mut output, frame).unwrap();
                keyboard_only
                    .apply_rgba(&mut keyboard_pixels, frame)
                    .unwrap();
                badge_only.apply_rgba(&mut badge_pixels, frame).unwrap();
                if size.1 < 100 {
                    assert_eq!(badge_pixels, background, "fixed timer inset clips {size:?}");
                    assert_eq!(output, background, "fixed overlay insets clip {size:?}");
                    continue;
                }
                assert_ne!(badge_pixels, background, "visible timer at {size:?}");
                let badge_bottom = badge_pixels
                    .chunks_exact(4)
                    .enumerate()
                    .filter(|(_, pixel)| *pixel != [20, 40, 60, 255])
                    .map(|(index, _)| index / size.0 as usize)
                    .max()
                    .unwrap();
                let keyboard_top = keyboard_pixels
                    .chunks_exact(4)
                    .enumerate()
                    .filter(|(_, pixel)| *pixel != [20, 40, 60, 255])
                    .map(|(index, _)| index / size.0 as usize)
                    .min()
                    .expect("visible keyboard overlay");
                assert!(badge_bottom < keyboard_top, "overlays overlap at {size:?}");
                let keyboard_start = (badge_bottom + 1) * size.0 as usize * 4;
                assert_eq!(output[keyboard_start..], keyboard_pixels[keyboard_start..]);
            }
        }
    }
    #[test]
    fn source_shapes_remain_bounded_and_missing_visible_references_fail() {
        let mut effects =
            RecordedEffects::new(config(PlaybackOverlay::None), (120, 100), None).unwrap();
        for sequence in 0..1000 {
            effects
                .observe(&input(
                    sequence,
                    0,
                    RecordedInput::CursorShape(CursorShapeRecord {
                        shape_id: sequence,
                        hotspot_x: 0,
                        hotspot_y: 0,
                        width: 1,
                        height: 1,
                        mode: CursorShapeCompositionMode::AlphaBlend,
                        shape_rgba: vec![255; 4],
                    }),
                ))
                .unwrap();
        }
        assert_eq!(effects.shapes.len(), MAX_CURSOR_SHAPES);
        assert_eq!(effects.retained_shape_bytes(), MAX_CURSOR_SHAPES * 4);
        effects
            .observe(&input(
                1000,
                0,
                RecordedInput::Cursor(CursorFrameRecord {
                    timestamp_ms: 0,
                    x: 0,
                    y: 0,
                    visible: true,
                    shape_id: Some(0),
                }),
            ))
            .unwrap();
        assert!(effects.apply_rgba(&mut pixels(), frame(0, 0, 0.0)).is_err());
    }
}
