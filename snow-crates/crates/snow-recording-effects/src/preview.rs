//! Live observation is an adapter around the same explicitly clocked effects used by video.
use std::collections::VecDeque;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::keyboard_hook::KeyboardInput;
use crate::keyboard_overlay::{KeyboardOverlay, KeyboardOverlayConfig, KeycapRasterizer};
use crate::laser_trail::LaserTrail;
use crate::mouse_effects::{CLICK_ANIMATION_MS, CLICK_QUEUE_DEPTH, RenderClick, draw_clicks_to};
use crate::mouse_hook::{MouseClickObservation, MouseHookObserver, MouseMovement};
use crate::surface::{Tile, TileSurface};
use crossbeam_channel::{Receiver, Sender, bounded, select_biased};

#[derive(Clone, PartialEq, Eq)]
pub struct PreviewConfig {
    pub region: (i32, i32, u32, u32),
    pub output: (u32, u32),
    pub trail: [u8; 4],
    pub trail_duration_ms: u64,
    pub click: [u8; 4],
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub generation: u64,
}

impl PreviewConfig {
    pub fn validate(&self) -> Result<(), String> {
        if !(100..=2000).contains(&self.trail_duration_ms) {
            return Err("trail duration must be between 100 and 2000 ms".into());
        }
        if [self.region.2, self.region.3, self.output.0, self.output.1]
            .into_iter()
            .any(|v| v == 0 || v > 32768)
        {
            return Err("effect dimensions must be between 1 and 32768 pixels".into());
        }
        Ok(())
    }
}

pub struct PreviewFrame {
    pub generation: u64,
    pub revision: u64,
    pub output: (u32, u32),
    pub tiles: Vec<Tile>,
    pub keyboard_output: (u32, u32),
    pub keyboard_tiles: Vec<Tile>,
    pub error: Option<String>,
}

/// Mouse tiles use export coordinates; keyboard tiles use physical capture pixels.
/// Keeping these destinations separate prevents export scaling from resizing keycaps.
#[derive(Default)]
pub struct PreviewLayers {
    pub mouse: Vec<Tile>,
    pub keyboard: Vec<Tile>,
}

impl PreviewLayers {
    pub fn is_empty(&self) -> bool {
        self.mouse.is_empty() && self.keyboard.is_empty()
    }
    pub fn len(&self) -> usize {
        self.mouse.len() + self.keyboard.len()
    }
    pub fn iter(&self) -> impl Iterator<Item = &Tile> {
        self.mouse.iter().chain(&self.keyboard)
    }
}

/// Deterministic preview destination. No native APIs, worker, or wall clock are required.
pub struct EffectsPreview {
    pub config: PreviewConfig,
    pub trail: LaserTrail,
    pub clicks: VecDeque<RenderClick>,
    pub keyboard: Option<KeyboardOverlay>,
    surface: TileSurface,
    keyboard_surface: TileSurface,
    position: Option<(i32, i32)>,
    continuity: u64,
}

impl EffectsPreview {
    pub fn new(config: PreviewConfig, rasterizer: Option<Box<dyn KeycapRasterizer>>) -> Self {
        let output = config.output;
        let trail = LaserTrail::new(config.trail_duration_ms);
        let keyboard_output = (config.region.2, config.region.3);
        let keycap_size = config
            .keyboard
            .as_ref()
            .map_or(64, |style| style.keycap_size);
        Self {
            config,
            trail,
            clicks: VecDeque::new(),
            keyboard: rasterizer
                .map(|r| KeyboardOverlay::new(keyboard_output, r).with_keycap_size(keycap_size)),
            surface: TileSurface::new(output),
            keyboard_surface: TileSurface::new(keyboard_output),
            position: None,
            continuity: 0,
        }
    }
    pub fn observe(&mut self, position: Option<(i32, i32)>, at: u64) {
        self.position = position;
        if self.config.trail[3] != 0 {
            self.trail.observe(
                position,
                (self.config.region.2, self.config.region.3),
                self.config.output,
                at,
            );
        }
    }
    /// Preserve a path break when the observer coalesced an exit and re-entry.
    pub fn observe_input(&mut self, position: Option<(i32, i32)>, at: u64, continuity: u64) {
        if self.continuity != continuity {
            self.observe(None, at);
            self.continuity = continuity;
        }
        self.observe(position, at);
    }
    pub fn click(&mut self, click: RenderClick) {
        if self.clicks.len() == CLICK_QUEUE_DEPTH {
            self.clicks.pop_front();
        }
        self.clicks.push_back(click);
    }
    pub fn render(&mut self, now: u64) -> Result<PreviewLayers, String> {
        self.surface.clear();
        self.keyboard_surface.clear();
        self.observe(self.position, now);
        self.clicks
            .retain(|click| now.saturating_sub(click.timestamp_ms) < CLICK_ANIMATION_MS);
        self.trail
            .draw_to(&mut self.surface, now, self.config.trail);
        draw_clicks_to(
            &mut self.surface,
            &self.clicks,
            now,
            self.config.click,
            (self.config.region.2, self.config.region.3),
        );
        if let Some(keyboard) = self.keyboard.as_mut() {
            keyboard.draw_to(&mut self.keyboard_surface, now)?;
        }
        Ok(PreviewLayers {
            mouse: self.surface.snapshot(),
            keyboard: self.keyboard_surface.snapshot(),
        })
    }
    pub fn next_frame_at(&self, now: u64) -> Option<u64> {
        if self.trail.has_active_animation(now) || !self.clicks.is_empty() {
            Some(now.saturating_add(16))
        } else {
            self.keyboard
                .as_ref()
                .and_then(|k| k.model.next_frame_at(now))
        }
    }
    pub fn allocated_bytes(&self) -> usize {
        self.surface.allocated_bytes() + self.keyboard_surface.allocated_bytes()
    }
}

enum Command {
    Configure(PreviewConfig),
    Stop,
}

pub struct PreviewSession {
    sender: Sender<Command>,
    pending: Receiver<Command>,
    latest: Arc<Mutex<Option<Arc<PreviewFrame>>>>,
    worker: Option<JoinHandle<()>>,
}

impl PreviewSession {
    pub fn start(
        config: PreviewConfig,
        notify: Arc<dyn Fn() + Send + Sync>,
    ) -> Result<Self, String> {
        config.validate()?;
        let (sender, receiver) = bounded(1);
        let pending = receiver.clone();
        let latest = Arc::new(Mutex::new(None));
        let shared = Arc::clone(&latest);
        let worker = std::thread::Builder::new()
            .name("snow-effects-preview".into())
            .spawn(move || run(config, receiver, shared, notify))
            .map_err(|e| e.to_string())?;
        Ok(Self {
            sender,
            pending,
            latest,
            worker: Some(worker),
        })
    }
    pub fn configure(&self, config: PreviewConfig) -> Result<(), String> {
        config.validate()?;
        // Keep only the newest configuration; rapid color/geometry updates never block Qt.
        let command = Command::Configure(config);
        match self.sender.try_send(command) {
            Ok(()) => Ok(()),
            Err(crossbeam_channel::TrySendError::Full(command)) => {
                let _ = self.pending.try_recv();
                self.sender.try_send(command).map_err(|e| e.to_string())
            }
            Err(error) => Err(error.to_string()),
        }
    }
    pub fn latest(&self) -> Option<Arc<PreviewFrame>> {
        self.latest.lock().ok().and_then(|frame| frame.clone())
    }
    pub fn stop(&mut self) {
        if let Some(worker) = self.worker.take() {
            let _ = self.pending.try_recv();
            let _ = self.sender.send(Command::Stop);
            let _ = worker.join();
        }
        if let Ok(mut frame) = self.latest.lock() {
            *frame = None;
        }
    }
}

impl Drop for PreviewSession {
    fn drop(&mut self) {
        self.stop();
    }
}

struct Input {
    _mouse: Option<MouseHookObserver>,
    mouse: Receiver<MouseMovement>,
    clicks: Receiver<MouseClickObservation>,
    keyboard: Option<KeyboardInput>,
}

fn initialize(config: &PreviewConfig) -> Result<(Input, EffectsPreview), String> {
    let rasterizer = config
        .keyboard
        .as_ref()
        .map(crate::keyboard_rasterizer::create)
        .transpose()?;
    let keyboard = config
        .keyboard
        .as_ref()
        .map(|_| KeyboardInput::start())
        .transpose()?;
    let (click_tx, clicks) = bounded(CLICK_QUEUE_DEPTH);
    let (move_tx, mouse) = bounded(1);
    let observer = if config.trail[3] != 0 || config.click[3] != 0 {
        Some(MouseHookObserver::start_with_movement(
            config.region,
            click_tx,
            (config.trail[3] != 0).then(|| (move_tx, mouse.clone())),
        )?)
    } else {
        None
    };
    Ok((
        Input {
            _mouse: observer,
            mouse,
            clicks,
            keyboard,
        },
        EffectsPreview::new(config.clone(), rasterizer),
    ))
}

fn run(
    mut config: PreviewConfig,
    receiver: Receiver<Command>,
    latest: Arc<Mutex<Option<Arc<PreviewFrame>>>>,
    notify: Arc<dyn Fn() + Send + Sync>,
) {
    let origin = Instant::now();
    let elapsed = |at: Instant| {
        at.saturating_duration_since(origin)
            .as_millis()
            .min(u128::from(u64::MAX)) as u64
    };
    let mut revision = 0;
    'configure: loop {
        let (input, mut effects) = match initialize(&config) {
            Ok(value) => value,
            Err(error) => {
                revision += 1;
                publish(
                    &latest,
                    notify.as_ref(),
                    PreviewFrame {
                        generation: config.generation,
                        revision,
                        output: config.output,
                        tiles: vec![],
                        keyboard_output: (config.region.2, config.region.3),
                        keyboard_tiles: vec![],
                        error: Some(error),
                    },
                );
                match receiver.recv() {
                    Ok(Command::Configure(next)) => {
                        config = next;
                        continue;
                    }
                    _ => break,
                }
            }
        };
        let never = crossbeam_channel::never();
        let keys = input.keyboard.as_ref().map_or(&never, |k| &k.receiver);
        // Disabled sources must never turn a disconnected receiver into a busy loop.
        let no_mouse = crossbeam_channel::never();
        let no_clicks = crossbeam_channel::never();
        let mouse = if config.trail[3] != 0 {
            &input.mouse
        } else {
            &no_mouse
        };
        let clicks = if config.click[3] != 0 {
            &input.clicks
        } else {
            &no_clicks
        };
        let mut dirty = true;
        let mut next_frame = elapsed(Instant::now());
        let mut keyboard_generation = 0;
        let mut pending_position: Option<MouseMovement> = None;
        loop {
            let now = elapsed(Instant::now());
            if dirty && now >= next_frame {
                if let Some(movement) = pending_position.take() {
                    effects.observe_input(
                        movement.position,
                        elapsed(movement.at),
                        movement.continuity,
                    );
                }
                revision += 1;
                let result = effects.render(now);
                let failed = result.is_err();
                let (tiles, error) = match result {
                    Ok(tiles) => (tiles, None),
                    Err(e) => (PreviewLayers::default(), Some(e)),
                };
                publish(
                    &latest,
                    notify.as_ref(),
                    PreviewFrame {
                        generation: config.generation,
                        revision,
                        output: config.output,
                        tiles: tiles.mouse,
                        keyboard_output: (config.region.2, config.region.3),
                        keyboard_tiles: tiles.keyboard,
                        error,
                    },
                );
                if failed {
                    break;
                }
                next_frame = now.saturating_add(17);
                dirty = false;
            }
            let now = elapsed(Instant::now());
            let deadline = if dirty {
                Some(next_frame)
            } else {
                effects.next_frame_at(now)
            };
            let timer = deadline
                .map(|at| crossbeam_channel::after(Duration::from_millis(at.saturating_sub(now))))
                .unwrap_or_else(crossbeam_channel::never);
            select_biased! {
                recv(receiver) -> command => match command {
                    Ok(Command::Configure(next)) => { config = next; continue 'configure; }
                    _ => return,
                },
                recv(mouse) -> event => if let Ok(event) = event {
                    pending_position = Some(event);
                    dirty = true;
                },
                recv(clicks) -> event => if let Ok(event) = event {
                    effects.click(RenderClick { timestamp_ms: elapsed(event.at), x: event.x, y: event.y, button: event.button });
                    dirty = true;
                },
                recv(keys) -> event => if let Ok(event) = event
                    && let (Some(input), Some(style), Some(overlay)) = (&input.keyboard, &config.keyboard, &mut effects.keyboard) {
                        let generation = input.generation.load(Ordering::Acquire);
                        if generation != keyboard_generation {
                            overlay.model.reset(now);
                            keyboard_generation = generation;
                        }
                        if event.generation == generation { overlay.model.event(event.event(elapsed(event.at), style)); }
                        dirty = true;
                },
                recv(timer) -> _ => { dirty = true; },
            }
        }
        // A rasterization failure is reported once and does not cause automatic retries.
        drop(input);
        match receiver.recv() {
            Ok(Command::Configure(next)) => config = next,
            _ => break,
        }
    }
}

fn publish(latest: &Mutex<Option<Arc<PreviewFrame>>>, notify: &dyn Fn(), frame: PreviewFrame) {
    if let Ok(mut slot) = latest.lock() {
        *slot = Some(Arc::new(frame));
    }
    notify();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::keyboard_overlay::{KeyEvent, Keycap};
    use crate::mouse_hook::ObservedMouseButton;
    use crate::surface::{RgbaSurface, Surface, TILE_SIZE};

    fn config() -> PreviewConfig {
        PreviewConfig {
            region: (-400, -200, 1920, 1080),
            output: (1920, 1080),
            trail: [255, 0, 0, 128],
            trail_duration_ms: 500,
            click: [0, 255, 0, 128],
            keyboard: None,
            generation: 7,
        }
    }
    struct Solid;
    impl KeycapRasterizer for Solid {
        fn rasterize(&mut self, _: &str, _: f32) -> Result<Keycap, String> {
            Ok(Keycap {
                width: 40,
                height: 20,
                pixels: [80, 20, 10, 128].repeat(800),
            })
        }
    }
    fn key(at_ms: u64, down: bool) -> KeyEvent {
        KeyEvent {
            at_ms,
            down,
            key: 65,
            label: "A".into(),
            modifiers: vec![],
        }
    }
    #[test]
    fn preview_keycaps_stay_64_physical_pixels_independent_of_capture_and_export_size() {
        struct FixedSquare;
        impl KeycapRasterizer for FixedSquare {
            fn rasterize(&mut self, _: &str, scale: f32) -> Result<Keycap, String> {
                assert_eq!(scale, 1.0);
                Ok(Keycap {
                    width: 64,
                    height: 64,
                    pixels: [100, 0, 0, 255].repeat(64 * 64),
                })
            }
        }
        for capture in [(640, 480), (1920, 1080), (3840, 2160), (641, 479)] {
            for output in [(320, 180), (640, 480), (1920, 1080)] {
                let mut config = config();
                config.region = (-400, -200, capture.0, capture.1);
                config.output = output;
                let mut preview = EffectsPreview::new(config, Some(Box::new(FixedSquare)));
                preview.keyboard.as_mut().unwrap().model.event(key(0, true));
                let frame = preview.render(200).unwrap();
                assert!(frame.mouse.is_empty());
                let mut count = 0;
                let (mut left, mut top, mut right, mut bottom) = (u32::MAX, u32::MAX, 0, 0);
                for tile in &frame.keyboard {
                    for (index, pixel) in tile.pixels.chunks_exact(4).enumerate() {
                        if pixel[3] != 0 {
                            let x = tile.x + index as u32 % TILE_SIZE;
                            let y = tile.y + index as u32 / TILE_SIZE;
                            assert!(x < capture.0 && y < capture.1);
                            left = left.min(x);
                            top = top.min(y);
                            right = right.max(x);
                            bottom = bottom.max(y);
                            count += 1;
                        }
                    }
                }
                assert_eq!((right - left + 1, bottom - top + 1), (64, 64));
                assert_eq!(count, 64 * 64);
                preview
                    .keyboard
                    .as_mut()
                    .unwrap()
                    .model
                    .event(key(210, false));
                assert!(preview.render(1810).unwrap().is_empty());
                assert_eq!(preview.next_frame_at(1810), None);
                assert!(
                    !frame.keyboard.is_empty(),
                    "displayed lease must survive expiry"
                );
            }
        }
    }
    #[test]
    fn sparse_frames_are_transparent_immutable_and_finish_with_no_deadline() {
        let mut preview = EffectsPreview::new(config(), Some(Box::new(Solid)));
        assert!(preview.render(0).unwrap().is_empty());
        assert_eq!(preview.next_frame_at(0), None);
        preview.observe(Some((100, 100)), 0);
        preview.observe(Some((130, 100)), 10);
        preview.click(RenderClick {
            timestamp_ms: 10,
            x: 180,
            y: 100,
            button: ObservedMouseButton::Left,
        });
        preview
            .keyboard
            .as_mut()
            .unwrap()
            .model
            .event(key(10, true));
        preview
            .keyboard
            .as_mut()
            .unwrap()
            .model
            .event(key(20, false));
        let frame = preview.render(100).unwrap();
        assert!(!frame.is_empty());
        assert!(
            frame.len() < 12,
            "separated small effects must not allocate their bounding rectangle"
        );
        assert!(
            frame
                .iter()
                .flat_map(|t| t.pixels.chunks_exact(4))
                .any(|p| p[3] > 0 && p[3] < 255)
        );
        let retained: Vec<_> = frame.iter().map(|t| t.pixels.to_vec()).collect();
        assert!(preview.render(2000).unwrap().is_empty());
        assert_eq!(preview.next_frame_at(2000), None);
        for (tile, original) in frame.iter().zip(retained) {
            assert_eq!(tile.pixels.as_ref(), &original);
        }
    }
    #[test]
    fn keyboard_hold_sleeps_until_fade_and_geometry_is_clipped_at_tile_edges() {
        let mut value = config();
        value.output = (131, 129);
        value.region = (-400, -200, 131, 129);
        let mut preview = EffectsPreview::new(value, Some(Box::new(Solid)));
        preview.keyboard.as_mut().unwrap().model.event(key(0, true));
        preview.render(200).unwrap();
        assert_eq!(preview.next_frame_at(200), None);
        preview
            .keyboard
            .as_mut()
            .unwrap()
            .model
            .event(key(210, false));
        preview.render(220).unwrap();
        assert_eq!(preview.next_frame_at(220), Some(1410));
        for tile in preview.render(1500).unwrap().keyboard {
            for y in 0..TILE_SIZE {
                for x in 0..TILE_SIZE {
                    if tile.x + x >= 131 || tile.y + y >= 129 {
                        assert_eq!(tile.pixels[((y * TILE_SIZE + x) * 4 + 3) as usize], 0);
                    }
                }
            }
        }
        assert!(preview.render(1810).unwrap().is_empty());
        assert_eq!(preview.next_frame_at(1810), None);
    }
    #[test]
    fn coalesced_region_exit_breaks_the_trail_without_losing_the_old_tail() {
        let mut preview = EffectsPreview::new(config(), None);
        preview.observe_input(Some((20, 20)), 0, 0);
        preview.observe_input(Some((60, 20)), 20, 0);
        // The native observer saw an exit, but the UI cadence receives only the re-entry.
        preview.observe_input(Some((500, 20)), 40, 1);
        preview.observe_input(Some((540, 20)), 60, 1);
        let tiles = preview.render(80).unwrap();
        assert!(tiles.iter().any(|tile| tile.x == 0));
        assert!(tiles.iter().any(|tile| tile.x >= 384));
        assert!(
            !tiles.iter().any(|tile| tile.x == 128 || tile.x == 256),
            "coalescing must not connect positions across a region exit"
        );
        assert!(preview.render(600).unwrap().is_empty());
        assert_eq!(preview.next_frame_at(600), None);
    }
    #[test]
    fn tiled_alpha_matches_video_composition_for_clicks_and_keyboard() {
        for (button, duration) in [
            (ObservedMouseButton::Left, 100),
            (ObservedMouseButton::Right, 500),
            (ObservedMouseButton::Middle, 2000),
        ] {
            let mut value = config();
            value.trail_duration_ms = duration;
            value.output = (256, 128);
            value.region = (-500, 20, 256, 128);
            let mut preview = EffectsPreview::new(value.clone(), Some(Box::new(Solid)));
            let mut video_trail = LaserTrail::new(duration);
            for (index, point) in [(120, 96), (130, 100), (140, 96)].into_iter().enumerate() {
                preview.observe(Some(point), index as u64 * 50);
                video_trail.observe(Some(point), value.output, value.output, index as u64 * 50);
            }
            let click = RenderClick {
                timestamp_ms: 0,
                x: 128,
                y: 100,
                button,
            };
            preview.click(click);
            preview.keyboard.as_mut().unwrap().model.event(key(0, true));
            let frame = preview.render(200).unwrap();
            let mut expected = [20, 40, 60, 255].repeat(256 * 128);
            video_trail.draw(&mut expected, value.output, 200, value.trail);
            draw_clicks_to(
                &mut RgbaSurface {
                    pixels: &mut expected,
                    dimensions: value.output,
                },
                &VecDeque::from([click]),
                200,
                value.click,
                value.output,
            );
            let mut keyboard = KeyboardOverlay::new(value.output, Box::new(Solid));
            keyboard.model.event(key(0, true));
            keyboard.draw(&mut expected, 200).unwrap();
            let mut actual = [20, 40, 60, 255].repeat(256 * 128);
            for tile in frame.mouse.into_iter().chain(frame.keyboard) {
                for y in 0..TILE_SIZE.min(128 - tile.y) {
                    for x in 0..TILE_SIZE.min(256 - tile.x) {
                        let src = &tile.pixels[((y * TILE_SIZE + x) * 4) as usize..][..4];
                        let dst =
                            &mut actual[(((tile.y + y) * 256 + tile.x + x) * 4) as usize..][..4];
                        for channel in 0..3 {
                            dst[channel] = (u32::from(src[channel])
                                + (u32::from(dst[channel]) * (255 - u32::from(src[3])) + 127) / 255)
                                .min(255) as u8;
                        }
                    }
                }
            }
            assert!(actual.iter().zip(expected).all(|(a, b)| a.abs_diff(b) <= 1));
        }
    }
    #[test]
    fn repeated_clicks_and_stationary_input_remain_bounded() {
        let mut preview = EffectsPreview::new(config(), None);
        for i in 0..1000 {
            preview.click(RenderClick {
                timestamp_ms: i,
                x: 20,
                y: 20,
                button: ObservedMouseButton::Left,
            });
        }
        assert_eq!(preview.clicks.len(), CLICK_QUEUE_DEPTH);
        preview.render(1100).unwrap();
        preview.render(1600).unwrap();
        assert_eq!(preview.next_frame_at(1600), None);
        assert!(preview.allocated_bytes() <= 4 * (TILE_SIZE * TILE_SIZE * 4) as usize);
        let mut surface = TileSurface::new((3840, 2160));
        surface.blend_pixel(-1, -1, [255; 4]);
        surface.blend_pixel(3840, 2160, [255; 4]);
        assert_eq!(surface.allocated_bytes(), 0);
    }
}
