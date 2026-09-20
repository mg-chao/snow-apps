//! Passive, session-scoped input observation. The listen-only tap cannot consume
//! or synthesize input. Callback work is bounded and allocation-free.
use crate::{MacError, MacResult};
use crossbeam_channel::{Receiver, Sender};
use objc2_core_foundation::{CFMachPort, CFRunLoop, kCFRunLoopDefaultMode};
use objc2_core_graphics::*;
use std::{
    ffi::c_void,
    ptr::NonNull,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU8, AtomicU64, Ordering},
    },
    time::Duration,
};

#[link(name = "Carbon", kind = "framework")]
unsafe extern "C" {
    fn IsSecureEventInputEnabled() -> bool;
}

pub fn authorized() -> bool {
    CGPreflightListenEventAccess()
}
pub fn request_access() -> bool {
    CGRequestListenEventAccess()
}
pub fn secure_input_enabled() -> bool {
    unsafe { IsSecureEventInputEnabled() }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InputStatus {
    Active = 0,
    SecureInput = 1,
    Disabled = 2,
    PermissionRevoked = 3,
    Stopped = 4,
}
#[derive(Clone, Copy, Debug)]
pub struct InputEvent {
    pub kind: u32,
    /// Desktop points, top-left origin; convert through the capture transform.
    pub x: f64,
    pub y: f64,
    pub timestamp_ns: u64,
    pub key_code: u16,
    pub mouse_button: u16,
    pub keyboard_type: u32,
    pub modifiers: u64,
    pub repeat: bool,
    pub text: [u16; 16],
    pub text_len: usize,
    /// Changes after overflow, tap interruption or secure input transitions.
    pub generation: u64,
}
impl InputEvent {
    pub fn instant(&self) -> Option<std::time::Instant> {
        crate::time::host_time_to_instant(snow_media::time::MediaTime {
            value: i64::try_from(self.timestamp_ns).ok()?,
            timescale: 1_000_000_000,
            domain: snow_media::time::ClockDomain::MacHostTime,
            epoch: 0,
        })
    }
}
struct State {
    sender: Sender<InputEvent>,
    stop: AtomicBool,
    generation: AtomicU64,
    status: AtomicU8,
    dropped: AtomicU64,
}
impl State {
    fn transition(&self, status: InputStatus) {
        if self.status.swap(status as u8, Ordering::AcqRel) != status as u8 {
            self.generation.fetch_add(1, Ordering::AcqRel);
        }
    }
    fn publish(&self, mut event: InputEvent) {
        if self.stop.load(Ordering::Acquire) || self.status.load(Ordering::Acquire) != 0 {
            return;
        }
        event.generation = self.generation.load(Ordering::Acquire);
        if self.sender.try_send(event).is_err() {
            self.generation.fetch_add(1, Ordering::AcqRel);
            self.dropped.fetch_add(1, Ordering::Relaxed);
        }
    }
}
unsafe extern "C-unwind" fn receive(
    _: CGEventTapProxy,
    kind: CGEventType,
    event: NonNull<CGEvent>,
    context: *mut c_void,
) -> *mut CGEvent {
    // Context outlives the run-loop source and is freed only after tap invalidation.
    let state = unsafe { &*context.cast::<State>() };
    if kind == CGEventType::TapDisabledByTimeout || kind == CGEventType::TapDisabledByUserInput {
        state.transition(InputStatus::Disabled);
        return event.as_ptr();
    }
    let event_ref = Some(unsafe { event.as_ref() });
    let location = CGEvent::location(event_ref);
    let mut text = [0; 16];
    let mut text_len = 0;
    if kind == CGEventType::KeyDown || kind == CGEventType::KeyUp {
        unsafe {
            CGEvent::keyboard_get_unicode_string(
                event_ref,
                text.len() as u64,
                &raw mut text_len,
                text.as_mut_ptr(),
            );
        }
    }
    state.publish(InputEvent {
        kind: kind.0,
        mouse_button: CGEvent::integer_value_field(event_ref, CGEventField::MouseEventButtonNumber)
            as u16,
        x: location.x,
        y: location.y,
        timestamp_ns: CGEvent::timestamp(event_ref),
        key_code: CGEvent::integer_value_field(event_ref, CGEventField::KeyboardEventKeycode)
            as u16,
        keyboard_type: CGEvent::integer_value_field(
            event_ref,
            CGEventField::KeyboardEventKeyboardType,
        ) as u32,
        modifiers: CGEvent::flags(event_ref).0,
        repeat: CGEvent::integer_value_field(event_ref, CGEventField::KeyboardEventAutorepeat) != 0,
        text,
        text_len: text_len.min(text.len() as u64) as usize,
        generation: 0,
    });
    event.as_ptr()
}

pub struct InputObserver {
    pub events: Receiver<InputEvent>,
    state: Arc<State>,
    done: Receiver<()>,
    worker: Option<std::thread::JoinHandle<()>>,
}
impl InputObserver {
    pub fn start(keyboard: bool, mouse: bool) -> MacResult<Self> {
        if !authorized() {
            return Err(MacError::InputPermissionDenied);
        }
        if !keyboard && !mouse {
            return Err(MacError::InvalidConfig(
                "no input event types selected".into(),
            ));
        }
        let (sender, events) = crossbeam_channel::bounded(256);
        let state = Arc::new(State {
            sender,
            stop: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            status: AtomicU8::new(0),
            dropped: AtomicU64::new(0),
        });
        let thread_state = state.clone();
        let (ready_tx, ready_rx) = crossbeam_channel::bounded(1);
        let (done_tx, done) = crossbeam_channel::bounded(1);
        let worker = std::thread::Builder::new()
            .name("snow-input".into())
            .spawn(move || {
                let result = run(thread_state, keyboard, mouse, &ready_tx);
                if let Err(error) = result {
                    let _ = ready_tx.try_send(Err(error));
                }
                let _ = done_tx.try_send(());
            })
            .map_err(|e| MacError::Unsupported(e.to_string()))?;
        let observer = Self {
            events,
            state,
            done,
            worker: Some(worker),
        };
        ready_rx
            .recv_timeout(Duration::from_secs(3))
            .map_err(|_| MacError::Timeout)??;
        Ok(observer)
    }
    pub fn status(&self) -> InputStatus {
        match self.state.status.load(Ordering::Acquire) {
            0 => InputStatus::Active,
            1 => InputStatus::SecureInput,
            2 => InputStatus::Disabled,
            3 => InputStatus::PermissionRevoked,
            _ => InputStatus::Stopped,
        }
    }
    pub fn generation(&self) -> u64 {
        self.state.generation.load(Ordering::Acquire)
    }
    pub fn dropped(&self) -> u64 {
        self.state.dropped.load(Ordering::Relaxed)
    }
}
fn run(
    state: Arc<State>,
    keyboard: bool,
    mouse: bool,
    ready: &Sender<MacResult<()>>,
) -> MacResult<()> {
    let mut mask = 0;
    if keyboard {
        for kind in [
            CGEventType::KeyDown,
            CGEventType::KeyUp,
            CGEventType::FlagsChanged,
        ] {
            mask |= 1 << kind.0;
        }
    }
    if mouse {
        for kind in [
            CGEventType::MouseMoved,
            CGEventType::LeftMouseDown,
            CGEventType::LeftMouseUp,
            CGEventType::RightMouseDown,
            CGEventType::RightMouseUp,
            CGEventType::OtherMouseDown,
            CGEventType::OtherMouseUp,
            CGEventType::LeftMouseDragged,
            CGEventType::RightMouseDragged,
            CGEventType::OtherMouseDragged,
        ] {
            mask |= 1 << kind.0;
        }
    }
    let tap = unsafe {
        CGEvent::tap_create(
            CGEventTapLocation::SessionEventTap,
            CGEventTapPlacement::TailAppendEventTap,
            CGEventTapOptions::ListenOnly,
            mask,
            Some(receive),
            Arc::as_ptr(&state).cast_mut().cast(),
        )
    }
    .ok_or(MacError::InputPermissionDenied)?;
    let source = CFMachPort::new_run_loop_source(None, Some(&tap), 0).ok_or(MacError::Inactive)?;
    let run_loop = CFRunLoop::current().ok_or(MacError::Inactive)?;
    run_loop.add_source(Some(&source), unsafe { kCFRunLoopDefaultMode });
    if keyboard && secure_input_enabled() {
        state.transition(InputStatus::SecureInput);
    }
    let _ = ready.try_send(Ok(()));
    while !state.stop.load(Ordering::Acquire) {
        if !authorized() {
            state.transition(InputStatus::PermissionRevoked);
            break;
        }
        if keyboard && secure_input_enabled() {
            state.transition(InputStatus::SecureInput);
        } else if state.status.load(Ordering::Acquire) == InputStatus::SecureInput as u8 {
            state.transition(InputStatus::Active);
        }
        // A disabled tap is surfaced to consumers and never silently re-enabled.
        if state.status.load(Ordering::Acquire) == InputStatus::Disabled as u8 {
            break;
        }
        unsafe {
            CFRunLoop::run_in_mode(kCFRunLoopDefaultMode, 0.05, false);
        }
    }
    source.invalidate();
    tap.invalidate();
    if state.stop.load(Ordering::Acquire) {
        state.transition(InputStatus::Stopped);
    }
    Ok(())
}
impl Drop for InputObserver {
    fn drop(&mut self) {
        self.state.stop.store(true, Ordering::Release);
        if self.done.recv_timeout(Duration::from_secs(1)).is_ok()
            && let Some(worker) = self.worker.take()
        {
            let _ = worker.join();
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn overflow_and_security_transitions_invalidate_state_without_blocking() {
        let (sender, receiver) = crossbeam_channel::bounded(1);
        let state = State {
            sender,
            stop: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            status: AtomicU8::new(0),
            dropped: AtomicU64::new(0),
        };
        let event = InputEvent {
            kind: 10,
            x: 0.0,
            y: 0.0,
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
        state.publish(event);
        state.publish(event);
        assert_eq!(state.dropped.load(Ordering::Relaxed), 1);
        assert_eq!(state.generation.load(Ordering::Relaxed), 1);
        receiver.try_recv().unwrap();
        state.transition(InputStatus::SecureInput);
        state.publish(event);
        assert!(receiver.is_empty());
        state.transition(InputStatus::Active);
        state.publish(event);
        assert_eq!(receiver.try_recv().unwrap().generation, 3);
        state.stop.store(true, Ordering::Release);
        state.publish(event);
        assert!(receiver.is_empty());
    }
}
