use std::ffi::{CStr, c_char, c_void};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::JoinHandle;
use std::time::Instant;

use crossbeam_channel::Sender;

use super::KeyObservation;

#[repr(C)]
struct NativeKeyEvent {
    keycode: u16,
    down: u8,
    repeat: u8,
    modifiers: u8,
    reset: u8,
    label: [c_char; 64],
}

unsafe extern "C" {
    fn snow_recording_keyboard_start(
        callback: unsafe extern "C" fn(*mut c_void, *const NativeKeyEvent),
        context: *mut c_void,
        error: *mut c_char,
        error_size: usize,
    ) -> *mut c_void;
    fn snow_recording_keyboard_pump(handle: *mut c_void);
    fn snow_recording_keyboard_stop(handle: *mut c_void);
}

// Keep the existing key identity/label protocol shared with the recording container. Printable
// legends come from the active native layout, rather than assuming these physical keys are US.
fn virtual_key(keycode: u16) -> Option<u16> {
    let key = match keycode {
        0 => b'A' as u16,
        1 => b'S' as u16,
        2 => b'D' as u16,
        3 => b'F' as u16,
        4 => b'H' as u16,
        5 => b'G' as u16,
        6 => b'Z' as u16,
        7 => b'X' as u16,
        8 => b'C' as u16,
        9 => b'V' as u16,
        10 => 0xe2,
        11 => b'B' as u16,
        12 => b'Q' as u16,
        13 => b'W' as u16,
        14 => b'E' as u16,
        15 => b'R' as u16,
        16 => b'Y' as u16,
        17 => b'T' as u16,
        18 => b'1' as u16,
        19 => b'2' as u16,
        20 => b'3' as u16,
        21 => b'4' as u16,
        22 => b'6' as u16,
        23 => b'5' as u16,
        24 => 0xbb,
        25 => b'9' as u16,
        26 => b'7' as u16,
        27 => 0xbd,
        28 => b'8' as u16,
        29 => b'0' as u16,
        30 => 0xdd,
        31 => b'O' as u16,
        32 => b'U' as u16,
        33 => 0xdb,
        34 => b'I' as u16,
        35 => b'P' as u16,
        36 | 76 => 0x0d,
        37 => b'L' as u16,
        38 => b'J' as u16,
        39 => 0xde,
        40 => b'K' as u16,
        41 => 0xba,
        42 => 0xdc,
        43 => 0xbc,
        44 => 0xbf,
        45 => b'N' as u16,
        46 => b'M' as u16,
        47 => 0xbe,
        48 => 0x09,
        49 => 0x20,
        50 => 0xc0,
        51 => 0x08,
        53 => 0x1b,
        54 => 0x5c,
        55 => 0x5b,
        56 => 0xa0,
        57 => 0x14,
        58 => 0xa4,
        59 => 0xa2,
        60 => 0xa1,
        61 => 0xa5,
        62 => 0xa3,
        63 => 0xff,
        64 => 0x80,
        65 => 0x6e,
        67 => 0x6a,
        69 => 0x6b,
        71 => 0x0c,
        72 => 0xaf,
        73 => 0xae,
        74 => 0xad,
        75 => 0x6f,
        78 => 0x6d,
        79 => 0x81,
        80 => 0x82,
        81 => 0x92,
        82 => 0x60,
        83 => 0x61,
        84 => 0x62,
        85 => 0x63,
        86 => 0x64,
        87 => 0x65,
        88 => 0x66,
        89 => 0x67,
        90 => 0x83,
        91 => 0x68,
        92 => 0x69,
        93 => 0xe0,
        94 => 0xe1,
        95 => 0x6c,
        96 => 0x74,
        97 => 0x75,
        98 => 0x76,
        99 => 0x72,
        100 => 0x77,
        101 => 0x78,
        102 => 0xf0,
        103 => 0x7a,
        104 => 0xf1,
        105 => 0x7c,
        106 => 0x7f,
        107 => 0x7d,
        109 => 0x79,
        110 => 0x5d,
        111 => 0x7b,
        113 => 0x7e,
        114 => 0x2d,
        115 => 0x24,
        116 => 0x21,
        117 => 0x2e,
        118 => 0x73,
        119 => 0x23,
        120 => 0x71,
        121 => 0x22,
        122 => 0x70,
        123 => 0x25,
        124 => 0x27,
        125 => 0x28,
        126 => 0x26,
        _ => return None,
    };
    Some(key)
}

struct Context {
    sender: Sender<KeyObservation>,
    generation: Arc<AtomicU64>,
    seen_generation: u64,
    pressed: [u8; 256],
}

impl Context {
    fn observe(&mut self, raw: &NativeKeyEvent) {
        if raw.reset != 0 {
            self.pressed.fill(0);
            let generation = self
                .generation
                .fetch_add(1, Ordering::AcqRel)
                .wrapping_add(1);
            // Wake the consumer even if the disabled tap produces no further keys. This
            // unused key-up only resets display state; it is never posted as system input.
            let _ = self.sender.try_send(KeyObservation {
                at: Instant::now(),
                key: 0,
                scan: 0,
                down: false,
                layout: 0,
                pressed: self.pressed,
                alt_gr: false,
                generation,
                printable: None,
                #[cfg(test)]
                extra_info: 0,
            });
            return;
        }
        let Some(key) = virtual_key(raw.keycode) else {
            return;
        };
        let generation = self.generation.load(Ordering::Acquire);
        if generation != self.seen_generation {
            self.pressed.fill(0);
            self.seen_generation = generation;
        }
        for (bit, modifier) in [0xa2, 0xa3, 0xa4, 0xa5, 0xa0, 0xa1, 0x5b, 0x5c]
            .into_iter()
            .enumerate()
        {
            self.pressed[modifier] = if raw.modifiers & (1 << bit) != 0 {
                128
            } else {
                0
            };
        }
        let down = raw.down != 0;
        let repeat = down && raw.repeat != 0;
        self.pressed[usize::from(key)] = if down { 128 } else { 0 };
        if repeat {
            return;
        }
        let label: Vec<u8> = raw
            .label
            .iter()
            .take_while(|&&byte| byte != 0)
            .map(|&byte| byte as u8)
            .collect();
        let printable = String::from_utf8(label)
            .ok()
            .filter(|label| !label.is_empty());
        let event = KeyObservation {
            at: Instant::now(),
            key,
            scan: u32::from(raw.keycode),
            down,
            // The printable label was copied at event time, so no layout object escapes the callback.
            layout: 0,
            pressed: self.pressed,
            alt_gr: false,
            generation,
            printable,
            #[cfg(test)]
            extra_info: 0,
        };
        if self.sender.try_send(event).is_err() {
            self.generation.fetch_add(1, Ordering::AcqRel);
        }
    }
}

unsafe extern "C" fn receive_key(context: *mut c_void, event: *const NativeKeyEvent) {
    // Native creation and destruction run on this same worker's run loop. The Box remains
    // alive until the source has been removed and invalidated, and native never retains event.
    let context = unsafe { &mut *context.cast::<Context>() };
    context.observe(unsafe { &*event });
}

struct NativeObserver(*mut c_void);

impl Drop for NativeObserver {
    fn drop(&mut self) {
        unsafe { snow_recording_keyboard_stop(self.0) };
    }
}

pub struct Observer {
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl Observer {
    pub fn start(
        sender: Sender<KeyObservation>,
        generation: Arc<AtomicU64>,
    ) -> Result<Self, String> {
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let (ready_sender, ready_receiver) = crossbeam_channel::bounded(1);
        let join = std::thread::Builder::new()
            .name("snow-recording-keyboard-observer".into())
            .spawn(move || {
                let mut context = Box::new(Context {
                    sender,
                    generation,
                    seen_generation: 0,
                    pressed: [0; 256],
                });
                let mut error = [0 as c_char; 1024];
                let handle = unsafe {
                    snow_recording_keyboard_start(
                        receive_key,
                        (&raw mut *context).cast(),
                        error.as_mut_ptr(),
                        error.len(),
                    )
                };
                if handle.is_null() {
                    let message = unsafe { CStr::from_ptr(error.as_ptr()) }
                        .to_string_lossy()
                        .into_owned();
                    let _ = ready_sender.send(Err(message));
                    return;
                }
                // Declared after context, so the source is always destroyed before its callback owner.
                let native = NativeObserver(handle);
                if ready_sender.send(Ok(())).is_ok() {
                    while !worker_stop.load(Ordering::Acquire) {
                        unsafe { snow_recording_keyboard_pump(native.0) };
                    }
                }
            })
            .map_err(|error| format!("failed to start keyboard observer: {error}"))?;
        match ready_receiver.recv() {
            Ok(Ok(())) => Ok(Self {
                stop,
                join: Some(join),
            }),
            result => {
                let _ = join.join();
                Err(match result {
                    Ok(Err(error)) => error,
                    _ => "keyboard observer stopped during initialization".into(),
                })
            }
        }
    }
}

impl Drop for Observer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

pub fn printable_label(key: u16, event: &KeyObservation) -> Option<String> {
    let name = match key {
        0x10 | 0xa0 | 0xa1 => "Shift",
        0x11 | 0xa2 | 0xa3 => "Ctrl",
        0x12 | 0xa4 | 0xa5 => "Option",
        0x5b | 0x5c => "Cmd",
        0x92 => "Num =",
        0xf0 => "Eisu",
        0xf1 => "Kana",
        0xff => "Fn",
        _ => {
            return if key == event.key {
                event.printable.clone()
            } else {
                None
            };
        }
    };
    Some(name.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::KeyboardOverlayConfig;

    fn event(keycode: u16, down: bool, modifiers: u8, label: &str) -> NativeKeyEvent {
        let mut result = NativeKeyEvent {
            keycode,
            down: u8::from(down),
            repeat: 0,
            modifiers,
            reset: 0,
            label: [0; 64],
        };
        for (target, source) in result.label.iter_mut().zip(label.bytes()) {
            *target = source as c_char;
        }
        result
    }

    fn context(capacity: usize) -> (Context, crossbeam_channel::Receiver<KeyObservation>) {
        let (sender, receiver) = crossbeam_channel::bounded(capacity);
        (
            Context {
                sender,
                generation: Arc::new(AtomicU64::new(0)),
                seen_generation: 0,
                pressed: [0; 256],
            },
            receiver,
        )
    }

    #[test]
    fn macos_keyboard_maps_modifier_sides_navigation_and_function_keys() {
        for (native, expected) in [
            (55, 0x5b),
            (54, 0x5c),
            (58, 0xa4),
            (61, 0xa5),
            (59, 0xa2),
            (62, 0xa3),
            (56, 0xa0),
            (60, 0xa1),
            (51, 0x08),
            (117, 0x2e),
            (123, 0x25),
            (124, 0x27),
            (125, 0x28),
            (126, 0x26),
            (122, 0x70),
            (90, 0x83),
        ] {
            assert_eq!(virtual_key(native), Some(expected));
        }
        assert_eq!(virtual_key(128), None);
        assert_eq!(virtual_key(u16::MAX), None);
    }

    #[test]
    fn macos_keyboard_uses_the_event_layout_and_native_modifier_names() {
        let (mut context, receiver) = context(4);
        context.observe(&event(0, true, 64 | 8, "Й"));
        let observation = receiver.recv().unwrap();
        let config = KeyboardOverlayConfig {
            keycap_size: 64,
            background_rgba: [0; 4],
            text_rgba: [0; 4],
            border_rgba: [0; 4],
            labels: Default::default(),
        };
        let display = observation.event(0, &config);
        assert_eq!(display.label, "Й");
        assert_eq!(
            display.modifiers,
            [(0xa5, "Option".into()), (0x5b, "Cmd".into())]
        );
        assert!(!observation.alt_gr);
    }

    #[test]
    fn macos_keyboard_suppresses_repeat_and_resets_after_queue_overflow() {
        let (mut context, receiver) = context(1);
        context.observe(&event(0, true, 0, "A"));
        let mut repeated = event(0, true, 0, "A");
        repeated.repeat = 1;
        context.observe(&repeated);
        assert_eq!(context.generation.load(Ordering::Acquire), 0);
        context.observe(&event(0, false, 0, ""));
        assert_eq!(context.generation.load(Ordering::Acquire), 1);
        receiver.try_recv().unwrap();
        context.observe(&event(1, true, 128, "S"));
        let recovered = receiver.try_recv().unwrap();
        assert_eq!(recovered.generation, 1);
        assert_eq!(recovered.pressed[b'A' as usize], 0);
        assert_eq!(recovered.pressed[0x5c], 128);
        assert_eq!(recovered.pressed[b'S' as usize], 128);
    }

    #[test]
    fn macos_keyboard_native_reset_invalidates_held_keys_without_synthesizing_input() {
        let (mut context, receiver) = context(2);
        context.observe(&event(0, true, 64, "A"));
        let mut reset = event(0, false, 0, "");
        reset.reset = 1;
        context.observe(&reset);
        assert_eq!(context.generation.load(Ordering::Acquire), 1);
        assert!(context.pressed.iter().all(|&state| state == 0));
        assert_eq!(receiver.len(), 2);
        receiver.try_recv().unwrap();
        let reset = receiver.try_recv().unwrap();
        assert_eq!(reset.generation, 1);
        assert_eq!(reset.key, 0);
        assert!(!reset.down);
    }

    #[test]
    #[ignore = "requires Input Monitoring permission and observes the live keyboard"]
    fn macos_keyboard_observer_has_a_joined_native_lifecycle() {
        for _ in 0..3 {
            let (sender, receiver) = crossbeam_channel::bounded(4);
            let observer = Observer::start(sender, Arc::new(AtomicU64::new(0))).unwrap();
            drop(observer);
            while receiver.try_recv().is_ok() {}
            assert!(matches!(
                receiver.try_recv(),
                Err(crossbeam_channel::TryRecvError::Disconnected)
            ));
        }
    }
}
