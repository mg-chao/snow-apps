//! Session-scoped, read-only polling; this does not install an input event tap or require
//! Input Monitoring permission. A press and release entirely between polls can be missed.
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, RecvTimeoutError, Sender, TrySendError};

use super::{MouseClickObservation, MouseMovement, ObservedMouseButton};

const POLL_INTERVAL: Duration = Duration::from_millis(8);
type MovementChannel = (Sender<MouseMovement>, Receiver<MouseMovement>);

#[derive(Clone, Copy, Debug)]
struct PointerSample {
    x: i32,
    y: i32,
    buttons: u8,
}

struct Observation {
    movement: Option<MouseMovement>,
    position: Option<(i32, i32)>,
    pressed: u8,
}

struct PointerTracker {
    region: (i32, i32, u32, u32),
    previous: Option<PointerSample>,
    was_inside: bool,
    continuity: u64,
}

impl PointerTracker {
    fn new(region: (i32, i32, u32, u32)) -> Self {
        Self {
            region,
            previous: None,
            was_inside: false,
            continuity: 0,
        }
    }

    fn observe(&mut self, sample: Option<PointerSample>, at: Instant) -> Observation {
        let position = sample.and_then(|sample| {
            let (x, y, width, height) = self.region;
            (i64::from(sample.x) >= i64::from(x)
                && i64::from(sample.x) < i64::from(x) + i64::from(width)
                && i64::from(sample.y) >= i64::from(y)
                && i64::from(sample.y) < i64::from(y) + i64::from(height))
            .then_some((sample.x.saturating_sub(x), sample.y.saturating_sub(y)))
        });
        let inside = position.is_some();
        if self.was_inside && !inside {
            self.continuity = self.continuity.wrapping_add(1);
        }
        let changed = match (self.previous, sample) {
            (Some(previous), Some(sample)) => previous.x != sample.x || previous.y != sample.y,
            (None, None) => false,
            _ => true,
        };
        // The first reading establishes a baseline: a button already held when recording
        // starts, or after a read failure, is not a newly observed click.
        let pressed = match (self.previous, sample) {
            (Some(previous), Some(sample)) if inside => sample.buttons & !previous.buttons & 7,
            _ => 0,
        };
        self.previous = sample;
        self.was_inside = inside;
        Observation {
            movement: changed.then_some(MouseMovement {
                at,
                position,
                continuity: self.continuity,
            }),
            position,
            pressed,
        }
    }
}

fn publish(
    observation: Observation,
    at: Instant,
    clicks: &Sender<MouseClickObservation>,
    movement: Option<&MovementChannel>,
) {
    if let Some((x, y)) = observation.position {
        for (mask, button) in [
            (1, ObservedMouseButton::Left),
            (2, ObservedMouseButton::Right),
            (4, ObservedMouseButton::Middle),
        ] {
            if observation.pressed & mask != 0 {
                let _ = clicks.try_send(MouseClickObservation { at, x, y, button });
            }
        }
    }
    if let Some(event) = observation.movement
        && let Some((sender, drain)) = movement
        && let Err(TrySendError::Full(event)) = sender.try_send(event)
    {
        // Keep the newest position while retaining the continuity change from any exit.
        let _ = drain.try_recv();
        let _ = sender.try_send(event);
    }
}

fn read_pointer() -> Result<PointerSample, String> {
    snow_macos::pointer().map(|pointer| PointerSample {
        x: pointer.x,
        y: pointer.y,
        buttons: pointer.buttons,
    })
}

pub struct MouseHookObserver {
    stop: Sender<()>,
    join: Option<JoinHandle<()>>,
}

impl MouseHookObserver {
    pub fn start(
        region: (i32, i32, u32, u32),
        sender: Sender<MouseClickObservation>,
    ) -> Result<Self, String> {
        Self::start_with_movement(region, sender, None)
    }

    pub fn start_with_movement(
        region: (i32, i32, u32, u32),
        sender: Sender<MouseClickObservation>,
        movement: Option<MovementChannel>,
    ) -> Result<Self, String> {
        let initial = read_pointer()?;
        Self::spawn(region, sender, movement, initial, read_pointer)
    }

    fn spawn(
        region: (i32, i32, u32, u32),
        sender: Sender<MouseClickObservation>,
        movement: Option<MovementChannel>,
        initial: PointerSample,
        mut read: impl FnMut() -> Result<PointerSample, String> + Send + 'static,
    ) -> Result<Self, String> {
        let (stop, stop_receiver) = crossbeam_channel::bounded(1);
        let join = std::thread::Builder::new()
            .name("snow-recording-mouse-observer".into())
            .spawn(move || {
                let mut tracker = PointerTracker::new(region);
                let at = Instant::now();
                publish(
                    tracker.observe(Some(initial), at),
                    at,
                    &sender,
                    movement.as_ref(),
                );
                while let Err(RecvTimeoutError::Timeout) = stop_receiver.recv_timeout(POLL_INTERVAL)
                {
                    let at = Instant::now();
                    let observation = tracker.observe(read().ok(), at);
                    publish(observation, at, &sender, movement.as_ref());
                }
            })
            .map_err(|error| format!("failed to start recording mouse observer: {error}"))?;
        Ok(Self {
            stop,
            join: Some(join),
        })
    }
}

impl Drop for MouseHookObserver {
    fn drop(&mut self) {
        let _ = self.stop.try_send(());
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(x: i32, y: i32, buttons: u8) -> Option<PointerSample> {
        Some(PointerSample { x, y, buttons })
    }

    #[test]
    fn macos_mouse_clicks_only_report_new_presses_inside_the_region() {
        let mut tracker = PointerTracker::new((-20, 30, 40, 50));
        let at = Instant::now();
        assert_eq!(tracker.observe(sample(0, 40, 1), at).pressed, 0);
        assert_eq!(tracker.observe(sample(0, 40, 0), at).pressed, 0);
        let press = tracker.observe(sample(0, 40, 7), at);
        assert_eq!(press.pressed, 7);
        assert_eq!(press.position, Some((20, 10)));
        assert_eq!(tracker.observe(sample(0, 40, 7), at).pressed, 0);
        assert_eq!(tracker.observe(sample(30, 40, 0), at).pressed, 0);
        assert_eq!(tracker.observe(sample(30, 40, 1), at).pressed, 0);
        assert_eq!(tracker.observe(sample(0, 40, 1), at).pressed, 0);
        assert_eq!(tracker.observe(sample(0, 40, 0), at).pressed, 0);
        assert_eq!(tracker.observe(sample(0, 40, 2), at).pressed, 2);
    }

    #[test]
    fn macos_mouse_exit_continuity_survives_latest_movement_coalescing() {
        let mut tracker = PointerTracker::new((0, 0, 100, 100));
        let at = Instant::now();
        let (clicks, _) = crossbeam_channel::bounded(1);
        let (sender, receiver) = crossbeam_channel::bounded(1);
        let movement = (sender, receiver.clone());
        for point in [(10, 10), (100, 10), (20, 20)] {
            publish(
                tracker.observe(sample(point.0, point.1, 0), at),
                at,
                &clicks,
                Some(&movement),
            );
        }
        let latest = receiver.try_recv().unwrap();
        assert_eq!(latest.position, Some((20, 20)));
        assert_eq!(latest.continuity, 1);
        assert!(receiver.try_recv().is_err());
        assert!(tracker.observe(sample(20, 20, 0), at).movement.is_none());
    }

    #[test]
    fn macos_mouse_read_failure_breaks_trails_and_resynchronizes_buttons() {
        let mut tracker = PointerTracker::new((0, 0, 10, 10));
        let at = Instant::now();
        tracker.observe(sample(1, 1, 0), at);
        let failure = tracker.observe(None, at).movement.unwrap();
        assert_eq!(failure.position, None);
        assert_eq!(failure.continuity, 1);
        assert!(tracker.observe(None, at).movement.is_none());
        let recovered = tracker.observe(sample(2, 2, 1), at);
        assert_eq!(recovered.pressed, 0);
        assert_eq!(recovered.movement.unwrap().continuity, 1);
    }

    #[test]
    fn macos_mouse_region_edges_use_half_open_bounds_without_overflow() {
        let at = Instant::now();
        let mut tracker = PointerTracker::new((i32::MAX - 2, i32::MIN, u32::MAX, 2));
        assert_eq!(
            tracker
                .observe(sample(i32::MAX, i32::MIN + 1, 0), at)
                .position,
            Some((2, 1))
        );
        assert_eq!(
            tracker
                .observe(sample(i32::MAX - 3, i32::MIN, 0), at)
                .position,
            None
        );
        assert_eq!(
            tracker
                .observe(sample(i32::MAX, i32::MIN + 2, 0), at)
                .position,
            None
        );
        assert_eq!(
            PointerTracker::new((0, 0, 0, 10))
                .observe(sample(0, 0, 0), at)
                .position,
            None
        );
    }

    #[test]
    fn macos_mouse_click_delivery_never_blocks_on_a_full_queue() {
        let (sender, receiver) = crossbeam_channel::bounded(1);
        let mut tracker = PointerTracker::new((0, 0, 10, 10));
        let at = Instant::now();
        tracker.observe(sample(1, 2, 0), at);
        publish(tracker.observe(sample(1, 2, 7), at), at, &sender, None);
        let event = receiver.try_recv().unwrap();
        assert_eq!(event.button, ObservedMouseButton::Left);
        assert_eq!((event.x, event.y), (1, 2));
        assert!(receiver.try_recv().is_err());
    }

    #[test]
    fn macos_mouse_drop_joins_the_worker_without_native_input() {
        let (clicks, click_receiver) = crossbeam_channel::bounded(1);
        let (sender, receiver) = crossbeam_channel::bounded(1);
        let observer = MouseHookObserver::spawn(
            (0, 0, 10, 10),
            clicks,
            Some((sender, receiver.clone())),
            PointerSample {
                x: 1,
                y: 2,
                buttons: 0,
            },
            || {
                Ok(PointerSample {
                    x: 2,
                    y: 3,
                    buttons: 0,
                })
            },
        )
        .unwrap();
        let first = receiver.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(first.position.is_some());
        drop(observer);
        // Worker owns the last sender; disconnected after Drop proves shutdown completed.
        assert!(matches!(
            click_receiver.try_recv(),
            Err(crossbeam_channel::TryRecvError::Disconnected)
        ));
    }
}
