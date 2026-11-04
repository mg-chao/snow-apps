//! Live observation is an adapter around the same explicitly clocked effects used by video.
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::keyboard_filter::{KeyboardFilter, KeyboardFilterConfig};
use crate::keyboard_hook::KeyboardInput;
use crate::keyboard_overlay::{KeyboardOverlay, KeyboardOverlayConfig, KeycapRasterizer};
use crate::mouse_effects::{CLICK_QUEUE_DEPTH, RenderClick};
use crate::mouse_hook::{MouseClickObservation, MouseHookObserver, MouseMovement};
use crate::state::InputEffectsState;
use crate::surface::{Tile, TileSurface};
use crossbeam_channel::{Receiver, Sender, bounded, select_biased};

#[derive(Clone, PartialEq, Eq)]
pub struct PreviewConfig {
    pub webcam: Option<snow_webcam::Config>,
    /// Native desktop units: points on macOS, physical pixels on Windows.
    pub region: (i32, i32, u32, u32),
    /// Physical display pixels used to rasterize fixed-size preview styles.
    pub canvas: (u32, u32),
    pub output: (u32, u32),
    pub trail: [u8; 4],
    pub trail_duration_ms: u64,
    pub click: [u8; 4],
    pub highlight_radius: u32,
    pub highlight: [u8; 4],
    pub record_mouse_clicks: bool,
    pub record_mouse_scrolling: bool,
    pub show_keyboard: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub keyboard_filter: KeyboardFilterConfig,
    pub generation: u64,
}

impl PreviewConfig {
    fn same_scene(&self, next: &Self) -> bool {
        self.region == next.region
            && self.canvas == next.canvas
            && self.output == next.output
            && self.trail == next.trail
            && self.trail_duration_ms == next.trail_duration_ms
            && self.click == next.click
            && self.highlight_radius == next.highlight_radius
            && self.highlight == next.highlight
            && self.record_mouse_clicks == next.record_mouse_clicks
            && self.record_mouse_scrolling == next.record_mouse_scrolling
            && self.show_keyboard == next.show_keyboard
            && self.keyboard_filter == next.keyboard_filter
            && self.keyboard == next.keyboard
            && self.generation == next.generation
            && self.webcam.as_ref().map(|w| &w.source) == next.webcam.as_ref().map(|w| &w.source)
    }

    fn effects_output(&self) -> (u32, u32) {
        // Preview styles belong to the desktop capture canvas. Export dimensions
        // only affect saved video and must not rescale live effects.
        self.canvas
    }

    pub fn validate(&self) -> Result<(), String> {
        if let Some(webcam) = &self.webcam {
            webcam.style.validate()?;
        }
        if !snow_recording_model::render::MOUSE_HIGHLIGHT_RADIUS_RANGE
            .contains(&self.highlight_radius)
        {
            return Err("mouse highlight radius must be between 8 and 128 pixels".into());
        }
        if !(100..=2000).contains(&self.trail_duration_ms) {
            return Err("trail duration must be between 100 and 2000 ms".into());
        }
        if [
            self.region.2,
            self.region.3,
            self.canvas.0,
            self.canvas.1,
            self.output.0,
            self.output.1,
        ]
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

/// Both layers use physical display pixels, independent of export dimensions.
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
    pub input_effects: InputEffectsState,
    surface: TileSurface,
    keyboard_surface: TileSurface,
    position: Option<(i32, i32)>,
    continuity: u64,
    webcam: crate::webcam::WebcamOverlay,
    keyboard_filter: KeyboardFilter,
}

impl std::ops::Deref for EffectsPreview {
    type Target = InputEffectsState;
    fn deref(&self) -> &Self::Target {
        &self.input_effects
    }
}
impl std::ops::DerefMut for EffectsPreview {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.input_effects
    }
}

impl EffectsPreview {
    fn update_webcam_layout(&mut self, next: &PreviewConfig) -> bool {
        if !self.config.same_scene(next) {
            return false;
        }
        // Retain observers, input animations, surfaces and camera pixels during a gesture.
        self.config.webcam = next.webcam.clone();
        true
    }

    pub fn new(config: PreviewConfig, rasterizer: Option<Box<dyn KeycapRasterizer>>) -> Self {
        let output = config.effects_output();
        let keycap_size = config
            .keyboard
            .as_ref()
            .map_or(64, |style| style.keycap_size);
        let input_effects = InputEffectsState {
            keyboard: rasterizer.map(|r| {
                KeyboardOverlay::new(output, r)
                    .with_keycap_size(keycap_size)
                    .with_icon_color(config.keyboard.as_ref().map_or([255; 4], |s| s.text_rgba))
            }),
            ..InputEffectsState::new(config.trail_duration_ms)
        };
        Self {
            config,
            input_effects,
            surface: TileSurface::new(output),
            keyboard_surface: TileSurface::new(output),
            position: None,
            continuity: 0,
            webcam: Default::default(),
            keyboard_filter: KeyboardFilter::default(),
        }
    }
    pub fn observe(&mut self, position: Option<(i32, i32)>, at: u64) {
        self.position = position;
        if self.config.trail[3] != 0 {
            self.input_effects.observe_pointer(
                position,
                (self.config.region.2, self.config.region.3),
                self.config.effects_output(),
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
        self.input_effects.click(click);
    }
    pub fn key_event(&mut self, event: crate::keyboard_overlay::KeyEvent) {
        if self.input_effects.pending_keys.len() == crate::state::MAX_PENDING_KEYS {
            self.reset_inputs(event.at_ms);
        }
        if let Some(event) = self
            .keyboard_filter
            .event(self.config.keyboard_filter, event)
        {
            self.input_effects.queue_key(event);
        }
    }
    pub fn reset_inputs(&mut self, now: u64) {
        self.keyboard_filter.reset();
        self.input_effects.reset_keyboard(now);
    }
    pub fn mouse_event(&mut self, event: &MouseClickObservation, at_ms: u64) {
        if event.button.scroll_direction().is_some() {
            if self.config.record_mouse_scrolling
                && let Some(style) = &self.config.keyboard
            {
                let key = event.event(at_ms, style, false);
                self.key_event(key);
            }
            return;
        }
        if event.down && event.button.has_ring() && self.config.click[3] != 0 {
            self.click(RenderClick {
                timestamp_ms: at_ms,
                x: event.x,
                y: event.y,
                button: event.button,
            });
        }
        if self.config.record_mouse_clicks
            && let Some(style) = &self.config.keyboard
        {
            self.key_event(event.event(at_ms, style, self.config.show_keyboard));
        }
    }
    pub fn render(&mut self, now: u64) -> Result<PreviewLayers, String> {
        self.surface.clear();
        self.keyboard_surface.clear();
        if let Some(webcam) = &self.config.webcam {
            let frame = webcam.source.snapshot(Instant::now());
            self.webcam.prepare(
                frame.frame.as_deref(),
                webcam.style,
                self.config.effects_output(),
            );
            self.webcam.draw_to(&mut self.surface);
        }
        self.observe(self.position, now);
        if let Some(position) = self.position {
            let center = crate::mouse_effects::scale_point(
                position.0,
                position.1,
                (self.config.region.2, self.config.region.3),
                self.config.effects_output(),
            );
            crate::mouse_effects::draw_highlight_to(
                &mut self.surface,
                center,
                self.config.highlight,
                false,
                self.config.highlight_radius,
            );
        }
        self.input_effects.draw_mouse_layers_to(
            &mut self.surface,
            now,
            (self.config.region.2, self.config.region.3),
            self.config.trail,
            self.config.trail_duration_ms,
            self.config.click,
        );
        self.input_effects
            .draw_keyboard_to(&mut self.keyboard_surface, now)?;
        Ok(PreviewLayers {
            mouse: self.surface.snapshot(),
            keyboard: self.keyboard_surface.snapshot(),
        })
    }
    pub fn next_frame_at(&mut self, now: u64) -> Option<u64> {
        let keyboard = self.input_effects.next_key_frame_at(now);
        let motion = (self.trail.has_active_animation(now) || !self.clicks.is_empty())
            .then(|| now.saturating_add(16));
        let camera = self
            .config
            .webcam
            .as_ref()
            .is_some_and(|webcam| webcam.source.snapshot(Instant::now()).error.is_none())
            .then(|| now.saturating_add(33));
        [keyboard, motion, camera].into_iter().flatten().min()
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
        // Initialize on the caller before spawning the preview worker: stop()
        // may join that worker from the main thread, so key translation cannot
        // synchronously dispatch back to the host.
        #[cfg(target_os = "macos")]
        if config.show_keyboard {
            snow_macos::text::prepare_keyboard_layout();
        }
        let (sender, receiver) = bounded(1);
        let pending = receiver.clone();
        let latest = Arc::new(Mutex::new(None));
        let shared = Arc::clone(&latest);
        let worker = std::thread::Builder::new()
            .name("snow-effects-preview".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                run(config, receiver, shared, notify)
            })
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
        // Enabling keyboard display on an existing preview has the same
        // initialization contract as starting one with keyboard display enabled.
        #[cfg(target_os = "macos")]
        if config.show_keyboard {
            snow_macos::text::prepare_keyboard_layout();
        }
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
        .filter(|_| config.show_keyboard)
        .map(|_| KeyboardInput::start())
        .transpose()?;
    let (click_tx, clicks) = bounded(CLICK_QUEUE_DEPTH);
    let (move_tx, mouse) = bounded(1);
    let observer = if config.trail[3] != 0
        || config.highlight[3] != 0
        || config.click[3] != 0
        || config.record_mouse_clicks
        || config.record_mouse_scrolling
    {
        Some(MouseHookObserver::start_with_movement(
            config.region,
            click_tx,
            (config.trail[3] != 0 || config.highlight[3] != 0).then(|| (move_tx, mouse.clone())),
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
                        output: config.effects_output(),
                        tiles: vec![],
                        keyboard_output: config.effects_output(),
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
        let mouse = if config.trail[3] != 0 || config.highlight[3] != 0 {
            &input.mouse
        } else {
            &no_mouse
        };
        // An active mouse observer also reports disabled button/wheel categories.
        // Drain them without invalidating pixels so they cannot overflow its queue.
        let clicks = if input._mouse.is_some() {
            &input.clicks
        } else {
            &no_clicks
        };
        let mut dirty = true;
        let mut next_frame = elapsed(Instant::now());
        let mut keyboard_generation = 0;
        let mut mouse_generation = 0;
        let mut pending_position: Option<MouseMovement> = None;
        loop {
            let now = elapsed(Instant::now());
            let generation = input
                ._mouse
                .as_ref()
                .map_or(0, MouseHookObserver::generation);
            if generation != mouse_generation {
                mouse_generation = generation;
                while input.clicks.try_recv().is_ok() {}
                effects.reset_inputs(now);
                dirty = true;
            }
            let generation = input
                .keyboard
                .as_ref()
                .map_or(0, |input| input.generation.load(Ordering::Acquire));
            if generation != keyboard_generation {
                effects.reset_inputs(now);
                keyboard_generation = generation;
            }
            // Collect both observers before advancing their shared source clock.
            // In particular, a scroll cannot overtake an earlier keyboard event
            // that is still waiting in the other observer's channel.
            for event in input.clicks.try_iter() {
                effects.mouse_event(&event, elapsed(event.at));
            }
            if let (Some(input), Some(style)) = (&input.keyboard, &config.keyboard) {
                for event in input.receiver.try_iter() {
                    if event.generation == keyboard_generation {
                        effects.key_event(event.event(elapsed(event.at), style));
                    }
                }
            }
            dirty |= effects.input_effects.keyboard_needs_frame(now) || !effects.clicks.is_empty();
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
                        output: config.effects_output(),
                        tiles: tiles.mouse,
                        keyboard_output: config.effects_output(),
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
            let reset_timer = if config.record_mouse_clicks || config.record_mouse_scrolling {
                crossbeam_channel::after(Duration::from_millis(50))
            } else {
                crossbeam_channel::never()
            };
            select_biased! {
                recv(receiver) -> command => match command {
                    Ok(Command::Configure(next)) => {
                        let layout_only = effects.update_webcam_layout(&next);
                        config = next;
                        if layout_only {
                            dirty = true;
                        } else {
                            continue 'configure;
                        }
                    }
                    _ => return,
                },
                recv(mouse) -> event => if let Ok(event) = event {
                    pending_position = Some(event);
                    dirty = true;
                },
                recv(clicks) -> event => if let Ok(event) = event {
                    effects.mouse_event(&event, elapsed(event.at));
                },
                recv(keys) -> event => if let Ok(event) = event
                    && let (Some(input), Some(style)) = (&input.keyboard, &config.keyboard) {
                        let generation = input.generation.load(Ordering::Acquire);
                        if generation != keyboard_generation {
                            effects.reset_inputs(now);
                            keyboard_generation = generation;
                        }
                        if event.generation == generation { effects.key_event(event.event(elapsed(event.at), style)); }
                },
                recv(reset_timer) -> _ => {},
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

    #[test]
    fn mixed_keycap_batches_match_chronological_input_before_and_after_admission() {
        use crate::recorded::RecordedEffects;
        use snow_recording_model::{
            EffectsConfig, InputMouseButton, PlaybackOverlay, RecordedInput, RecordedInputEvent,
            RecordedMouseClick, RenderConfig,
        };

        struct Colored;
        impl KeycapRasterizer for Colored {
            fn rasterize(
                &mut self,
                label: &str,
                _: f32,
            ) -> Result<crate::keyboard_overlay::Keycap, String> {
                let color = match label {
                    "A" => [200, 0, 0, 255],
                    "B" => [0, 200, 0, 255],
                    _ => [0, 0, 200, 255],
                };
                Ok(crate::keyboard_overlay::Keycap {
                    width: 120,
                    height: 64,
                    pixels: color.repeat(120 * 64),
                })
            }
        }
        let style = KeyboardOverlayConfig {
            font: None,
            keycap_size: 64,
            background_rgba: [0; 4],
            text_rgba: [255; 4],
            border_rgba: [0; 4],
            labels: Default::default(),
        };
        let key = |at_ms, key, down, label: &str| crate::keyboard_overlay::KeyEvent {
            at_ms,
            key,
            down,
            label: label.into(),
            modifiers: vec![],
        };
        let events = [
            key(10, 65, true, "A"),
            key(20, 0x200, true, "Left"),
            key(30, 0x205, true, "Scroll up"),
            key(40, 0x200, false, "Left"),
            key(50, 65, false, "A"),
            key(60, 66, true, "B"),
            key(70, 66, false, "B"),
            key(80, 0x206, true, "Scroll down"),
        ];
        let orders = [
            [0, 1, 2, 3, 4, 5, 6, 7],
            [1, 2, 3, 7, 0, 4, 5, 6],
            [0, 4, 5, 6, 1, 2, 3, 7],
            [7, 6, 5, 4, 3, 2, 1, 0],
        ];
        let assert_tiles = |actual: Vec<Tile>, expected: &[Tile], description: &str| {
            assert_eq!(actual.len(), expected.len(), "{description}: tile count");
            for (actual, expected) in actual.iter().zip(expected) {
                assert_eq!(
                    (actual.x, actual.y, &actual.pixels),
                    (expected.x, expected.y, &expected.pixels),
                    "{description}"
                );
            }
        };
        for admission_ms in [0, 100] {
            for order in orders {
                let mut cfg = config();
                cfg.region = (0, 0, 640, 480);
                cfg.canvas = (640, 480);
                cfg.output = cfg.canvas;
                cfg.trail = [0; 4];
                cfg.click = [0; 4];
                cfg.record_mouse_clicks = true;
                cfg.record_mouse_scrolling = true;
                cfg.keyboard = Some(style.clone());
                let mut preview = EffectsPreview::new(cfg, Some(Box::new(Colored)));
                let mut replay = RecordedEffects::new(
                    RenderConfig {
                        output_width: 640,
                        output_height: 480,
                        output_fps: 30,
                        playback_overlay: PlaybackOverlay::None,
                        effects: EffectsConfig {
                            show_cursor: false,
                            show_keyboard: true,
                            record_mouse_clicks: true,
                            record_mouse_scrolling: true,
                            keyboard: Some(style.clone()),
                            ..Default::default()
                        },
                    },
                    (640, 480),
                    Some(Box::new(Colored)),
                )
                .unwrap();
                let mut reference = KeyboardOverlay::new((640, 480), Box::new(Colored));
                for (sequence, index) in order.into_iter().enumerate() {
                    let key = events[index].clone();
                    let button = match key.key {
                        0x200 => Some(ObservedMouseButton::Left),
                        0x205 => Some(ObservedMouseButton::ScrollUp),
                        0x206 => Some(ObservedMouseButton::ScrollDown),
                        _ => None,
                    };
                    let record = if let Some(button) = button {
                        preview.mouse_event(
                            &MouseClickObservation {
                                at: Instant::now(),
                                x: 0,
                                y: 0,
                                button,
                                down: key.down,
                                modifiers: [false; 8],
                            },
                            key.at_ms,
                        );
                        if button.scroll_direction().is_some() {
                            RecordedInput::Scroll(key)
                        } else {
                            RecordedInput::Click(RecordedMouseClick {
                                x: 0,
                                y: 0,
                                button: InputMouseButton::Left,
                                down: key.down,
                                key,
                            })
                        }
                    } else {
                        preview.key_event(key.clone());
                        RecordedInput::Key(key)
                    };
                    replay
                        .observe(&RecordedInputEvent {
                            timestamp_ms: admission_ms,
                            sequence: sequence as u64,
                            event: record,
                        })
                        .unwrap();
                }
                let mut next = 0;
                for now in [0, 9, 10, 20, 30, 50, 60, 80, 100, 280, 1300, 1600, 2000] {
                    if now < admission_ms {
                        continue;
                    }
                    while next < events.len() && events[next].at_ms <= now {
                        reference.model.event(events[next].clone());
                        next += 1;
                    }
                    let mut expected = TileSurface::new((640, 480));
                    reference.draw_to(&mut expected, now).unwrap();
                    let expected = expected.snapshot();
                    let description =
                        format!("admission {admission_ms}, order {order:?}, frame {now}");
                    assert_tiles(
                        preview.render(now).unwrap().keyboard,
                        &expected,
                        &description,
                    );
                    let mut actual = TileSurface::new((640, 480));
                    replay
                        .input_effects
                        .draw_keyboard_to(&mut actual, now)
                        .unwrap();
                    assert_tiles(actual.snapshot(), &expected, &description);
                }
            }
        }
    }
    use crate::keyboard_overlay::{KeyEvent, Keycap};
    use crate::laser_trail::LaserTrail;
    use crate::mouse_effects::draw_clicks_to;
    use crate::mouse_hook::ObservedMouseButton;
    use crate::surface::{RgbaSurface, Surface, TILE_SIZE};
    use std::collections::VecDeque;

    fn config() -> PreviewConfig {
        PreviewConfig {
            keyboard_filter: Default::default(),
            webcam: None,
            region: (-400, -200, 1920, 1080),
            canvas: (1920, 1080),
            output: (1920, 1080),
            trail: [255, 0, 0, 128],
            trail_duration_ms: 500,
            click: [0, 255, 0, 128],
            highlight_radius: snow_recording_model::render::DEFAULT_MOUSE_HIGHLIGHT_RADIUS,
            highlight: [0; 4],
            record_mouse_clicks: false,
            record_mouse_scrolling: false,
            show_keyboard: true,
            keyboard: None,
            generation: 7,
        }
    }

    #[test]
    fn scrolling_preview_matches_replay_and_unchanged_bursts_do_not_publish() {
        use crate::recorded::RecordedEffects;
        use snow_recording_model::{
            EffectsConfig, PlaybackOverlay, RecordedInput, RecordedInputEvent, RenderConfig,
        };
        struct Blank;
        impl KeycapRasterizer for Blank {
            fn rasterize(&mut self, _: &str, _: f32) -> Result<Keycap, String> {
                Ok(Keycap {
                    width: 240,
                    height: 64,
                    pixels: [10, 20, 30, 200].repeat(240 * 64),
                })
            }
        }
        let style = KeyboardOverlayConfig {
            font: None,
            keycap_size: 64,
            background_rgba: [0, 0, 0, 200],
            text_rgba: [255; 4],
            border_rgba: [0; 4],
            labels: [
                (0x205, "Localized up".into()),
                (0x206, "Localized down".into()),
            ]
            .into(),
        };
        let mut cfg = config();
        cfg.trail = [0; 4];
        cfg.click = [0; 4];
        cfg.show_keyboard = false;
        cfg.record_mouse_scrolling = true;
        cfg.keyboard = Some(style.clone());
        let mut preview = EffectsPreview::new(cfg.clone(), Some(Box::new(Blank)));
        let policy = RenderConfig {
            output_width: cfg.canvas.0,
            output_height: cfg.canvas.1,
            output_fps: 30,
            playback_overlay: PlaybackOverlay::None,
            effects: EffectsConfig {
                show_cursor: false,
                record_mouse_scrolling: true,
                keyboard: Some(style.clone()),
                ..EffectsConfig::default()
            },
        };
        let mut replay =
            RecordedEffects::new(policy.clone(), cfg.canvas, Some(Box::new(Blank))).unwrap();
        let mut records = Vec::new();
        let mut revision = 0;
        for (sequence, (at_ms, button, changed)) in [
            (0, ObservedMouseButton::ScrollUp, true),
            (200, ObservedMouseButton::ScrollUp, false),
            (450, ObservedMouseButton::ScrollUp, false),
            (500, ObservedMouseButton::ScrollDown, true),
            (751, ObservedMouseButton::ScrollDown, false),
        ]
        .into_iter()
        .enumerate()
        {
            let observation = MouseClickObservation {
                at: Instant::now(),
                x: 20,
                y: 20,
                button,
                down: true,
                modifiers: [true; 8],
            };
            preview.mouse_event(&observation, at_ms);
            let record = RecordedInputEvent {
                timestamp_ms: at_ms,
                sequence: sequence as u64,
                event: RecordedInput::Scroll(observation.event(at_ms, &style, true)),
            };
            if let RecordedInput::Scroll(key) = &record.event {
                assert!(key.modifiers.is_empty());
            }
            replay.observe(&record).unwrap();
            records.push(record);
            assert_eq!(replay.observation_revision(), revision);
            assert_eq!(preview.input_effects.keyboard_needs_frame(at_ms), changed);
            assert_eq!(replay.needs_frame(at_ms), changed);
            revision = replay.observation_revision();
            let live = preview.render(at_ms + 180).unwrap();
            let mut surface = TileSurface::new(cfg.canvas);
            replay
                .input_effects
                .draw_keyboard_to(&mut surface, at_ms + 180)
                .unwrap();
            let saved = surface.snapshot();
            assert_eq!(live.keyboard.len(), saved.len());
            for (a, b) in live.keyboard.iter().zip(saved.iter()) {
                assert_eq!((a.x, a.y, &a.pixels), (b.x, b.y, &b.pixels));
            }
        }
        // Seeking reconstructs the same burst history from original observation timestamps.
        let mut seek = RecordedEffects::new(policy, cfg.canvas, Some(Box::new(Blank))).unwrap();
        for record in &records {
            seek.observe(record).unwrap();
        }
        let mut a = TileSurface::new(cfg.canvas);
        let mut b = TileSurface::new(cfg.canvas);
        replay.input_effects.draw_keyboard_to(&mut a, 1000).unwrap();
        seek.input_effects.draw_keyboard_to(&mut b, 1000).unwrap();
        for (a, b) in a.snapshot().iter().zip(b.snapshot().iter()) {
            assert_eq!(a.pixels, b.pixels);
        }
        // A reversal admitted on an earlier output slot cannot change current pixels.
        let future = KeyEvent {
            at_ms: 1100,
            key: 0x205,
            down: true,
            label: "Localized up".into(),
            modifiers: vec![],
        };
        replay
            .observe(&RecordedInputEvent {
                timestamp_ms: 1000,
                sequence: 99,
                event: RecordedInput::Scroll(future.clone()),
            })
            .unwrap();
        a.clear();
        b.clear();
        replay.input_effects.draw_keyboard_to(&mut a, 1050).unwrap();
        seek.input_effects.draw_keyboard_to(&mut b, 1050).unwrap();
        assert_eq!(a.snapshot().len(), b.snapshot().len());
        for (a, b) in a.snapshot().iter().zip(b.snapshot().iter()) {
            assert_eq!(a.pixels, b.pixels);
        }
        seek.observe(&RecordedInputEvent {
            timestamp_ms: 1100,
            sequence: 99,
            event: RecordedInput::Scroll(future),
        })
        .unwrap();
        a.clear();
        b.clear();
        replay.input_effects.draw_keyboard_to(&mut a, 1280).unwrap();
        seek.input_effects.draw_keyboard_to(&mut b, 1280).unwrap();
        assert_eq!(a.snapshot().len(), b.snapshot().len());
        for (a, b) in a.snapshot().iter().zip(b.snapshot().iter()) {
            assert_eq!(a.pixels, b.pixels);
        }
        preview.reset_inputs(1000);
        preview.mouse_event(
            &MouseClickObservation {
                at: Instant::now(),
                x: 20,
                y: 20,
                button: ObservedMouseButton::ScrollUp,
                down: true,
                modifiers: [false; 8],
            },
            1010,
        );
        assert!(preview.input_effects.keyboard_needs_frame(1010));
        preview.render(1010).unwrap();
        let pending = preview.pending_keys.len();
        preview.config.record_mouse_scrolling = false;
        preview.mouse_event(
            &MouseClickObservation {
                at: Instant::now(),
                x: 20,
                y: 20,
                button: ObservedMouseButton::ScrollUp,
                down: true,
                modifiers: [false; 8],
            },
            1020,
        );
        assert_eq!(preview.pending_keys.len(), pending);
    }
    #[test]
    fn webcam_session_keeps_presenting_during_live_layout_changes() {
        struct OneFrame(Option<snow_webcam::RawFrame>);
        impl snow_webcam::CameraBackend for OneFrame {
            fn poll(&mut self, timeout: Duration) -> Result<Option<snow_webcam::RawFrame>, String> {
                if self.0.is_none() {
                    std::thread::sleep(timeout);
                }
                Ok(self.0.take())
            }
        }
        let source = snow_webcam::Source::with_backend(
            snow_webcam::CaptureOptions { size: 150, fps: 30 },
            |_| {
                Ok(Box::new(OneFrame(Some(snow_webcam::RawFrame {
                    width: 150,
                    height: 150,
                    stride: 600,
                    rgba: [220, 20, 10, 255].repeat(150 * 150),
                    at: Instant::now(),
                }))))
            },
        )
        .unwrap();
        source.set_active(true);
        let deadline = Instant::now() + Duration::from_secs(2);
        while source.snapshot(Instant::now()).frame.is_none() {
            assert!(Instant::now() < deadline);
            std::thread::yield_now();
        }
        let mut cfg = config();
        cfg.region = (-400, -200, 640, 480);
        cfg.canvas = (640, 480);
        cfg.output = (640, 480);
        cfg.trail = [0; 4];
        cfg.click = [0; 4];
        cfg.show_keyboard = false;
        cfg.webcam = Some(snow_webcam::Config {
            source: source.clone(),
            style: Default::default(),
        });
        let (notifications, received) = bounded(1);
        let mut session = PreviewSession::start(
            cfg.clone(),
            Arc::new(move || {
                let _ = notifications.try_send(());
            }),
        )
        .unwrap();
        for step in 0..8 {
            let style = &mut cfg.webcam.as_mut().unwrap().style;
            style.offset_left = 20 + step * 20;
            style.offset_bottom = 30 + step * 10;
            style.size = 100 + step * 20;
            source
                .configure(snow_webcam::CaptureOptions {
                    size: style.size,
                    fps: 30,
                })
                .unwrap();
            let (x, y, side) = style.placement(cfg.canvas);
            let expected = (x, y, x + side - 1, y + side - 1);
            session.configure(cfg.clone()).unwrap();
            loop {
                received.recv_timeout(Duration::from_secs(1)).unwrap();
                let frame = session.latest().unwrap();
                assert!(frame.error.is_none());
                let mut bounds = (u32::MAX, u32::MAX, 0, 0);
                for tile in &frame.tiles {
                    for (i, pixel) in tile.pixels.chunks_exact(4).enumerate() {
                        if pixel[3] != 0 {
                            let (px, py) =
                                (tile.x + i as u32 % TILE_SIZE, tile.y + i as u32 / TILE_SIZE);
                            bounds = (
                                bounds.0.min(px),
                                bounds.1.min(py),
                                bounds.2.max(px),
                                bounds.3.max(py),
                            );
                        }
                    }
                }
                assert_ne!(
                    bounds.0,
                    u32::MAX,
                    "layout changes never publish a blank camera frame"
                );
                if bounds == expected {
                    break;
                }
                assert!(
                    Instant::now() < deadline,
                    "latest webcam layout must reach the preview"
                );
            }
        }
        session.stop();
        source.set_active(false);
    }

    #[test]
    fn webcam_layout_updates_preserve_input_animation_and_scene_boundaries() {
        let source = snow_webcam::Source::with_backend(
            snow_webcam::CaptureOptions { size: 150, fps: 30 },
            |_| unreachable!(),
        )
        .unwrap();
        let mut cfg = config();
        cfg.webcam = Some(snow_webcam::Config {
            source,
            style: Default::default(),
        });
        let mut preview = EffectsPreview::new(cfg, None);
        preview.observe(Some((40, 50)), 10);
        preview.observe(Some((80, 70)), 20);
        preview.key_event(key(20, true));
        let pending = preview.pending_keys.len();
        let render_trail = |preview: &mut EffectsPreview| {
            let mut surface = TileSurface::new(preview.config.canvas);
            preview.trail.draw_to(&mut surface, 20, [255, 0, 0, 128]);
            surface
                .snapshot()
                .into_iter()
                .map(|tile| (tile.x, tile.y, tile.pixels))
                .collect::<Vec<_>>()
        };
        let trail = render_trail(&mut preview);
        assert!(!trail.is_empty());
        let mut next = preview.config.clone();
        let style = &mut next.webcam.as_mut().unwrap().style;
        style.offset_left = 310;
        style.offset_bottom = 120;
        style.size = 260;
        style.shape = snow_webcam::WebcamShape::Circle;
        style.orientation = snow_webcam::WebcamOrientation::MirroredClockwise90;
        assert!(preview.update_webcam_layout(&next));
        assert!(preview.config == next);
        assert_eq!(preview.pending_keys.len(), pending);
        assert_eq!(render_trail(&mut preview), trail);
        assert_eq!(preview.position, Some((80, 70)));
        // Real scene changes must still reconstruct observers and invalidate old frames.
        for change in 0..5 {
            let mut changed = next.clone();
            match change {
                0 => changed.region.0 += 1,
                1 => changed.canvas.0 += 1,
                2 => changed.show_keyboard = !changed.show_keyboard,
                3 => changed.generation += 1,
                _ => changed.webcam = None,
            }
            assert!(!preview.update_webcam_layout(&changed));
            assert!(preview.config == next);
        }
    }

    #[test]
    fn desktop_points_map_to_display_pixels_without_scaling_effect_styles() {
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
        for scale in [1.0, 1.25, 1.5, 1.75, 2.0] {
            for output in [(320, 240), (1920, 1080)] {
                let mut cfg = config();
                cfg.region = (-400, -200, 640, 480);
                cfg.canvas = ((640.0 * scale) as u32, (480.0 * scale) as u32);
                cfg.output = output;
                cfg.highlight = [255, 255, 0, 128];
                let mut reference = cfg.clone();
                reference.region.2 = cfg.canvas.0;
                reference.region.3 = cfg.canvas.1;
                let render = |cfg: PreviewConfig, input_scale: f64| {
                    let mut preview = EffectsPreview::new(cfg, Some(Box::new(FixedSquare)));
                    preview.key_event(key(0, true));
                    preview.observe(
                        Some(((160.0 * input_scale) as i32, (120.0 * input_scale) as i32)),
                        0,
                    );
                    preview.observe(
                        Some(((200.0 * input_scale) as i32, (160.0 * input_scale) as i32)),
                        17,
                    );
                    preview.click(RenderClick {
                        timestamp_ms: 17,
                        x: (300.0 * input_scale) as i32,
                        y: (200.0 * input_scale) as i32,
                        button: ObservedMouseButton::Left,
                    });
                    preview.render(200).unwrap()
                };
                let frame = render(cfg, 1.0);
                let expected = render(reference, scale);
                let pixels = |tiles: &[Tile]| {
                    tiles
                        .iter()
                        .map(|tile| (tile.x, tile.y, tile.pixels.clone()))
                        .collect::<Vec<_>>()
                };
                assert_eq!(
                    pixels(&frame.mouse),
                    pixels(&expected.mouse),
                    "pointer effects at scale {scale}, export {output:?}"
                );
                assert_eq!(
                    pixels(&frame.keyboard),
                    pixels(&expected.keyboard),
                    "keyboard at scale {scale}, export {output:?}"
                );
                let count = frame
                    .keyboard
                    .iter()
                    .flat_map(|tile| tile.pixels.chunks_exact(4))
                    .filter(|pixel| pixel[3] != 0)
                    .count();
                assert_eq!(count, 64 * 64, "keycaps keep their physical pixel size");
            }
        }
    }

    #[test]
    fn fixed_pixel_preview_styles_do_not_depend_on_export_dimensions() {
        for capture in [(640, 480), (1920, 1080), (3840, 2160), (641, 479)] {
            let mut reference = None;
            for output in [(320, 180), (640, 480), (1920, 1080), (3840, 2160)] {
                let mut cfg = config();
                cfg.region = (-400, -200, capture.0, capture.1);
                cfg.canvas = capture;
                cfg.output = output;
                cfg.highlight = [255, 255, 0, 128];
                let mut preview = EffectsPreview::new(cfg, Some(Box::new(Solid)));
                preview.key_event(key(0, true));
                preview.observe(Some((160, 160)), 0);
                preview.observe(Some((200, 200)), 17);
                preview.click(RenderClick {
                    timestamp_ms: 17,
                    x: 200,
                    y: 200,
                    button: crate::mouse_hook::ObservedMouseButton::Left,
                });
                let frame = preview.render(200).unwrap();
                let pixels: Vec<_> = frame
                    .iter()
                    .map(|tile| (tile.x, tile.y, tile.pixels.clone()))
                    .collect();
                if let Some(reference) = &reference {
                    assert!(
                        &pixels == reference,
                        "capture {capture:?}, export {output:?}"
                    );
                } else {
                    reference = Some(pixels);
                }
            }
        }
    }
    #[test]
    fn highlight_preview_moves_and_clears_without_animating_a_stationary_pointer() {
        let mut cfg = config();
        cfg.trail = [0; 4];
        cfg.click = [0; 4];
        cfg.highlight = [255, 255, 0, 128];
        let mut preview = EffectsPreview::new(cfg, None);
        preview.observe(Some((200, 200)), 0);
        let first = preview.render(0).unwrap();
        assert!(!first.mouse.is_empty());
        assert!(preview.next_frame_at(20).is_none());
        preview.observe(Some((800, 800)), 40);
        let second = preview.render(40).unwrap();
        assert!(
            first
                .mouse
                .iter()
                .all(|a| second.mouse.iter().all(|b| a.x != b.x || a.y != b.y))
        );
        preview.observe(None, 60);
        assert!(preview.render(60).unwrap().is_empty());
        preview.config.highlight = [0; 4];
        preview.observe(Some((200, 200)), 80);
        assert!(preview.render(80).unwrap().is_empty());
    }
    #[test]
    fn preview_uses_custom_radius_without_export_dimension_scaling() {
        for radius in [8, 41, 96, 128] {
            let mut cfg = config();
            cfg.trail = [0; 4];
            cfg.click = [0; 4];
            cfg.highlight = [255, 255, 0, 255];
            cfg.highlight_radius = radius;
            cfg.output = (480, 270);
            let mut preview = EffectsPreview::new(cfg, None);
            preview.observe(Some((400, 200)), 0);
            let frame = preview.render(0).unwrap();
            let alpha = |x: u32, y: u32| {
                frame
                    .mouse
                    .iter()
                    .find(|tile| {
                        tile.x == x / TILE_SIZE * TILE_SIZE && tile.y == y / TILE_SIZE * TILE_SIZE
                    })
                    .map_or(0, |tile| {
                        tile.pixels[(((y - tile.y) * TILE_SIZE + x - tile.x) * 4 + 3) as usize]
                    })
            };
            assert_eq!(alpha(400 + radius - 2, 200), 255);
            assert_eq!(alpha(400 + radius, 200), 0);
        }
    }
    #[test]
    fn webcam_preview_keeps_physical_pixels_at_every_dpi_and_clears_failure_without_input() {
        struct OneFrame(Option<snow_webcam::RawFrame>);
        impl snow_webcam::CameraBackend for OneFrame {
            fn poll(&mut self, timeout: Duration) -> Result<Option<snow_webcam::RawFrame>, String> {
                if self.0.is_none() {
                    std::thread::sleep(timeout);
                }
                Ok(self.0.take())
            }
        }
        let source = snow_webcam::Source::with_backend(
            snow_webcam::CaptureOptions { size: 150, fps: 30 },
            |_| {
                Ok(Box::new(OneFrame(Some(snow_webcam::RawFrame {
                    width: 150,
                    height: 150,
                    stride: 600,
                    rgba: [220, 20, 10, 255].repeat(150 * 150),
                    at: Instant::now(),
                }))))
            },
        )
        .unwrap();
        source.set_active(true);
        let deadline = Instant::now() + Duration::from_secs(3);
        while source.snapshot(Instant::now()).frame.is_none() {
            assert!(Instant::now() < deadline);
            std::thread::yield_now();
        }
        for canvas in [(640, 480), (1280, 960), (1920, 1440)] {
            for output in [(320, 240), (1920, 1080)] {
                let mut cfg = config();
                cfg.region = (0, 0, 640, 480);
                cfg.canvas = canvas;
                cfg.output = output;
                cfg.trail = [0; 4];
                cfg.click = [0; 4];
                cfg.highlight = [0; 4];
                cfg.keyboard = None;
                cfg.show_keyboard = false;
                cfg.webcam = Some(snow_webcam::Config {
                    source: source.clone(),
                    style: snow_webcam::WebcamStyle {
                        offset_left: 123,
                        offset_bottom: 67,
                        ..Default::default()
                    },
                });
                let mut preview = EffectsPreview::new(cfg, None);
                let frame = preview.render(0).unwrap();
                let mut bounds = (u32::MAX, u32::MAX, 0, 0);
                let mut count = 0;
                for tile in frame.mouse {
                    for (i, pixel) in tile.pixels.chunks_exact(4).enumerate() {
                        if pixel[3] == 0 {
                            continue;
                        }
                        let (x, y) = (tile.x + i as u32 % TILE_SIZE, tile.y + i as u32 / TILE_SIZE);
                        bounds.0 = bounds.0.min(x);
                        bounds.1 = bounds.1.min(y);
                        bounds.2 = bounds.2.max(x);
                        bounds.3 = bounds.3.max(y);
                        count += 1;
                    }
                }
                assert_eq!(bounds, (123, canvas.1 - 217, 272, canvas.1 - 68));
                assert_eq!(count, 150 * 150);
                assert_eq!(preview.next_frame_at(0), Some(33));
                source.report_error("disconnected".into());
                assert!(preview.render(33).unwrap().is_empty());
                assert_eq!(
                    preview.next_frame_at(33),
                    None,
                    "a failed camera must not keep notifying the UI"
                );
                source.retry_failed();
                source.set_active(true);
                let deadline = Instant::now() + Duration::from_secs(3);
                while source.snapshot(Instant::now()).frame.is_none() {
                    assert!(Instant::now() < deadline);
                    std::thread::yield_now();
                }
            }
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
    #[test]
    fn preview_queues_future_input_and_orders_mouse_with_keyboard() {
        let mut cfg = config();
        cfg.click = [0; 4];
        cfg.trail = [0; 4];
        let mut preview = EffectsPreview::new(cfg, Some(Box::new(Solid)));
        preview.key_event(key(200, false));
        preview.key_event(key(100, true));
        assert!(preview.render(50).unwrap().is_empty());
        assert!(!preview.render(150).unwrap().is_empty());
        assert!(!preview.render(250).unwrap().is_empty());
        assert!(preview.render(1900).unwrap().is_empty());
        preview.key_event(key(2200, true));
        preview.reset_inputs(2100);
        assert!(preview.render(2300).unwrap().is_empty());
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
    fn preview_filters_plain_keys_but_renders_preserved_shortcuts() {
        let mut cfg = config();
        cfg.trail = [0; 4];
        cfg.click = [0; 4];
        cfg.keyboard_filter.record_letters_and_symbols = false;
        let mut preview = EffectsPreview::new(cfg, Some(Box::new(Solid)));
        preview.key_event(key(0, true));
        preview.key_event(key(10, false));
        assert!(preview.render(10).unwrap().is_empty());
        let mut shortcut = key(20, true);
        shortcut.modifiers = vec![(0xa2, "Left Ctrl".into())];
        preview.key_event(shortcut);
        preview.key_event(key(30, false));
        assert!(!preview.render(40).unwrap().is_empty());
        assert!(preview.render(2000).unwrap().is_empty());
        preview.reset_inputs(2010);
        preview.key_event(key(2020, true));
        assert!(preview.render(2030).unwrap().is_empty());
    }

    #[test]
    fn preview_control_filter_also_removes_mouse_modifier_labels() {
        let mut cfg = config();
        cfg.keyboard_filter.record_control_keys = false;
        let mut preview = EffectsPreview::new(cfg, None);
        let mut event = key(0, true);
        event.modifiers = vec![(0xa2, "Left Ctrl".into())];
        preview.key_event(event.clone());
        event.key = 0x200;
        preview.key_event(event);
        assert_eq!(preview.pending_keys.len(), 2);
        assert!(
            preview
                .pending_keys
                .iter()
                .all(|event| event.modifiers.is_empty())
        );
    }
    #[test]
    fn preview_keycaps_keep_fixed_size_in_capture_coordinates() {
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
                config.canvas = capture;
                config.output = output;
                let mut preview = EffectsPreview::new(config, Some(Box::new(FixedSquare)));
                preview.keyboard.as_mut().unwrap().model.event(key(0, true));
                assert_eq!(preview.config.effects_output(), capture);
                let frame = preview.render(200).unwrap();
                assert!(frame.mouse.is_empty());
                let mut count = 0;
                let (mut left, mut top, mut right, mut bottom) = (u32::MAX, u32::MAX, 0, 0);
                for tile in &frame.keyboard {
                    for (index, pixel) in tile.pixels.chunks_exact(4).enumerate() {
                        if pixel[3] != 0 {
                            let x = tile.x + index as u32 % TILE_SIZE;
                            let y = tile.y + index as u32 / TILE_SIZE;
                            assert!(
                                x < preview.config.effects_output().0
                                    && y < preview.config.effects_output().1
                            );
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
        value.canvas = (131, 129);
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
            value.canvas = (256, 128);
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
