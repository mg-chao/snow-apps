//! Session-scoped observation only: the hook never consumes or synthesizes input.
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use crate::keyboard_overlay::{KeyEvent, KeyboardOverlayConfig, modifier};
use crossbeam_channel::{Receiver, Sender};

#[derive(Clone)]
pub struct KeyObservation {
    pub at: Instant,
    pub key: u16,
    pub scan: u32,
    pub down: bool,
    pub layout: usize,
    pub pressed: [u8; 256],
    pub alt_gr: bool,
    pub generation: u64,
    #[cfg(test)]
    pub extra_info: usize,
}

pub struct KeyboardInput {
    pub receiver: Receiver<KeyObservation>,
    pub generation: Arc<AtomicU64>,
    _observer: platform::Observer,
}

impl KeyboardInput {
    pub fn start() -> Result<Self, String> {
        let (sender, receiver) = crossbeam_channel::bounded(256);
        let generation = Arc::new(AtomicU64::new(0));
        let observer = platform::Observer::start(sender, Arc::clone(&generation))?;
        Ok(Self {
            receiver,
            generation,
            _observer: observer,
        })
    }
    pub fn reset(&self) {
        self.generation.fetch_add(1, Ordering::AcqRel);
        while self.receiver.try_recv().is_ok() {}
    }
}

impl KeyObservation {
    pub fn event(&self, at_ms: u64, config: &KeyboardOverlayConfig) -> KeyEvent {
        let mut modifiers = Vec::with_capacity(4);
        for (left, right, label_key) in [
            (0xa2, 0xa3, 0x11),
            (0xa4, 0xa5, 0x12),
            (0xa0, 0xa1, 0x10),
            (0x5b, 0x5c, 0x5b),
        ] {
            let left_down = self.pressed[left] != 0;
            let right_down = self.pressed[right] != 0;
            if self.alt_gr && left == 0xa2 && !right_down {
                continue;
            }
            if left_down || right_down {
                let label_key = if self.alt_gr && left == 0xa4 {
                    0xa5
                } else {
                    label_key
                };
                modifiers.push((
                    if left_down { left as u16 } else { right as u16 },
                    key_label(label_key, self, config),
                ));
            }
        }
        KeyEvent {
            at_ms,
            key: self.key,
            down: self.down,
            label: key_label(self.key, self, config),
            modifiers,
        }
    }
}

fn key_label(key: u16, observation: &KeyObservation, config: &KeyboardOverlayConfig) -> String {
    if let Some(label) = config.labels.get(&key) {
        return label.clone();
    }
    if (0x70..=0x87).contains(&key) {
        return format!("F{}", key - 0x6f);
    }
    if (0x60..=0x69).contains(&key) {
        return format!("Num {}", key - 0x60);
    }
    platform::printable_label(key, observation).unwrap_or_else(|| format!("VK {key:02X}"))
}

#[cfg(windows)]
mod platform {
    use super::*;
    use std::cell::RefCell;
    use std::sync::mpsc;
    use std::thread::JoinHandle;
    use windows::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::Threading::GetCurrentThreadId;
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        GetAsyncKeyState, GetKeyboardLayout, HKL, MAPVK_VK_TO_CHAR, MapVirtualKeyExW, ToUnicodeEx,
    };
    use windows::Win32::UI::WindowsAndMessaging::*;

    struct Context {
        sender: Sender<KeyObservation>,
        generation: Arc<AtomicU64>,
        seen_generation: u64,
        pressed: [u8; 256],
        left_ctrl_time: Option<u32>,
        alt_gr: bool,
    }
    thread_local! { static CONTEXT: RefCell<Option<Context>> = const { RefCell::new(None) }; }

    fn pressed_keys() -> [u8; 256] {
        let mut keys = [0; 256];
        for (key, state) in keys.iter_mut().enumerate() {
            *state = u8::from(unsafe { GetAsyncKeyState(key as i32) } < 0) * 0x80;
        }
        keys
    }

    unsafe extern "system" fn hook_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
        if code == HC_ACTION as i32 {
            let down = matches!(wparam.0 as u32, WM_KEYDOWN | WM_SYSKEYDOWN);
            let up = matches!(wparam.0 as u32, WM_KEYUP | WM_SYSKEYUP);
            if down || up {
                let raw = unsafe { &*(lparam.0 as *const KBDLLHOOKSTRUCT) };
                CONTEXT.with(|slot| {
                    let mut slot = slot.borrow_mut();
                    let Some(context) = slot.as_mut() else {
                        return;
                    };
                    let key = raw.vkCode as usize;
                    if key >= 256 {
                        return;
                    }
                    let generation = context.generation.load(Ordering::Acquire);
                    if generation != context.seen_generation {
                        context.pressed = pressed_keys();
                        context.alt_gr = false;
                        context.left_ctrl_time = None;
                        context.seen_generation = generation;
                    }
                    let repeat = down && context.pressed[key] != 0;
                    context.pressed[key] = if down { 0x80 } else { 0 };
                    if key == 0xa2 && down {
                        context.left_ctrl_time = Some(raw.time);
                    }
                    if key == 0xa5 {
                        context.alt_gr = down && context.left_ctrl_time == Some(raw.time);
                    }
                    if repeat {
                        return;
                    }
                    let foreground = unsafe { GetForegroundWindow() };
                    let thread_id = unsafe { GetWindowThreadProcessId(foreground, None) };
                    let layout = unsafe { GetKeyboardLayout(thread_id) };
                    let event = KeyObservation {
                        at: Instant::now(),
                        key: key as u16,
                        scan: raw.scanCode,
                        down,
                        layout: layout.0 as usize,
                        pressed: context.pressed,
                        alt_gr: context.alt_gr,
                        generation,
                        #[cfg(test)]
                        extra_info: raw.dwExtraInfo,
                    };
                    if context.sender.try_send(event).is_err() {
                        // The next event carries a fresh generation and physical modifier snapshot.
                        context.generation.fetch_add(1, Ordering::AcqRel);
                    }
                });
            }
        }
        unsafe { CallNextHookEx(None, code, wparam, lparam) }
    }

    pub struct Observer {
        thread_id: u32,
        join: Option<JoinHandle<()>>,
    }

    impl Observer {
        pub fn start(
            sender: Sender<KeyObservation>,
            generation: Arc<AtomicU64>,
        ) -> Result<Self, String> {
            let (ready_tx, ready_rx) = mpsc::sync_channel(1);
            let join = std::thread::Builder::new()
                .name("snow-recording-keyboard-hook".into())
                .spawn(move || {
                    let thread_id = unsafe { GetCurrentThreadId() };
                    let mut message = MSG::default();
                    unsafe {
                        let _ = PeekMessageW(&mut message, None, 0, 0, PM_NOREMOVE);
                    }
                    CONTEXT.with(|slot| {
                        *slot.borrow_mut() = Some(Context {
                            sender,
                            generation,
                            seen_generation: 0,
                            pressed: pressed_keys(),
                            left_ctrl_time: None,
                            alt_gr: false,
                        })
                    });
                    let hook =
                        unsafe { SetWindowsHookExW(WH_KEYBOARD_LL, Some(hook_proc), None, 0) };
                    match hook {
                        Ok(hook) => {
                            let _ = ready_tx.send(Ok(thread_id));
                            while unsafe { GetMessageW(&mut message, None, 0, 0) }.0 > 0 {}
                            let _ = unsafe { UnhookWindowsHookEx(hook) };
                        }
                        Err(error) => {
                            let _ = ready_tx.send(Err(error.to_string()));
                        }
                    }
                    CONTEXT.with(|slot| *slot.borrow_mut() = None);
                })
                .map_err(|error| error.to_string())?;
            match ready_rx.recv() {
                Ok(Ok(thread_id)) => Ok(Self {
                    thread_id,
                    join: Some(join),
                }),
                result => {
                    let _ = join.join();
                    Err(match result {
                        Ok(Err(error)) => error,
                        _ => "keyboard hook initialization failed".into(),
                    })
                }
            }
        }
    }
    impl Drop for Observer {
        fn drop(&mut self) {
            let _ = unsafe { PostThreadMessageW(self.thread_id, WM_QUIT, WPARAM(0), LPARAM(0)) };
            if let Some(join) = self.join.take() {
                let _ = join.join();
            }
        }
    }

    pub fn printable_label(key: u16, event: &KeyObservation) -> Option<String> {
        if modifier(key) {
            return None;
        }
        let layout = HKL(event.layout as *mut _);
        let mapped = unsafe { MapVirtualKeyExW(u32::from(key), MAPVK_VK_TO_CHAR, Some(layout)) }
            & 0x7fff_ffff;
        // Key names stay uppercase. Shortcuts display their base key, not control characters.
        if let Some(ch) = char::from_u32(mapped).filter(|ch| ch.is_alphanumeric()) {
            return Some(ch.to_uppercase().collect());
        }
        let state = [0; 256];
        let mut text = [0u16; 8];
        // Flag 4 prevents changing the application's dead-key composition state (Windows 10+).
        let count = unsafe {
            ToUnicodeEx(
                u32::from(key),
                event.scan,
                &state,
                &mut text,
                4,
                Some(layout),
            )
        };
        let count = count.unsigned_abs() as usize;
        if count > 0 && count <= text.len() {
            let label = String::from_utf16_lossy(&text[..count]);
            if !label.chars().any(char::is_control) && !label.trim().is_empty() {
                return Some(label);
            }
        }
        None
    }

    #[cfg(test)]
    mod native_tests {
        use super::*;

        #[test]
        #[ignore = "briefly focuses an isolated native test window; run explicitly"]
        fn keyboard_observer_receives_shortcut_from_isolated_native_window() {
            use windows::Win32::Foundation::HWND;
            use windows::Win32::System::Threading::AttachThreadInput;
            use windows::Win32::UI::Input::KeyboardAndMouse::*;
            use windows::core::w;
            const MARKER: usize = 0x534e4f57;
            struct Window {
                hwnd: HWND,
                previous: HWND,
            }
            impl Drop for Window {
                fn drop(&mut self) {
                    unsafe {
                        if GetForegroundWindow() == self.hwnd {
                            let _ = SetForegroundWindow(self.previous);
                        }
                        let _ = DestroyWindow(self.hwnd);
                    }
                }
            }
            // Never release a modifier physically held by the person using the machine.
            for key in [0xa2, 0xa0, 83] {
                assert!(
                    unsafe { GetAsyncKeyState(key) } >= 0,
                    "test keys must initially be up"
                );
            }
            let window = unsafe {
                let previous = GetForegroundWindow();
                let hwnd = CreateWindowExW(
                    WINDOW_EX_STYLE::default(),
                    w!("STATIC"),
                    w!("Snow Shot keyboard recording verification"),
                    WS_OVERLAPPEDWINDOW,
                    30,
                    30,
                    380,
                    120,
                    None,
                    None,
                    None,
                    None,
                )
                .unwrap();
                let window = Window { hwnd, previous };
                let _ = ShowWindow(hwnd, SW_SHOW);
                let foreground_thread = GetWindowThreadProcessId(previous, None);
                let current_thread = GetCurrentThreadId();
                // Use the same foreground activation approach as the repository's native Qt tests.
                let attached = foreground_thread != 0
                    && foreground_thread != current_thread
                    && AttachThreadInput(current_thread, foreground_thread, true).as_bool();
                let _ = BringWindowToTop(hwnd);
                let _ = SetForegroundWindow(hwnd);
                if attached {
                    let _ = AttachThreadInput(current_thread, foreground_thread, false);
                }
                window
            };
            let input = KeyboardInput::start().unwrap();
            let sequence = [
                (0xa2, false),
                (0xa0, false),
                (83, false),
                (83, true),
                (0xa0, true),
                (0xa2, true),
            ];
            let events: Vec<_> = sequence
                .iter()
                .map(|&(key, up)| INPUT {
                    r#type: INPUT_KEYBOARD,
                    Anonymous: INPUT_0 {
                        ki: KEYBDINPUT {
                            wVk: VIRTUAL_KEY(key),
                            dwFlags: if up {
                                KEYEVENTF_KEYUP
                            } else {
                                KEYBD_EVENT_FLAGS(0)
                            },
                            dwExtraInfo: MARKER,
                            ..Default::default()
                        },
                    },
                })
                .collect();
            unsafe {
                assert_eq!(
                    GetForegroundWindow(),
                    window.hwnd,
                    "do not send input to another application"
                );
                assert_eq!(
                    SendInput(&events, std::mem::size_of::<INPUT>() as i32),
                    events.len() as u32
                );
            }
            let deadline = Instant::now() + std::time::Duration::from_secs(2);
            let mut received = Vec::new();
            while received.len() < 6 && Instant::now() < deadline {
                let mut message = MSG::default();
                unsafe {
                    while PeekMessageW(&mut message, Some(window.hwnd), 0, 0, PM_REMOVE).as_bool() {
                        let _ = TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                while let Ok(event) = input.receiver.try_recv() {
                    if event.extra_info == MARKER {
                        received.push(event);
                    }
                }
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            assert_eq!(
                received
                    .iter()
                    .map(|e| (e.key, !e.down))
                    .collect::<Vec<_>>(),
                sequence
            );
            let config = KeyboardOverlayConfig {
                keycap_size: 64,
                background_rgba: [0; 4],
                text_rgba: [0; 4],
                border_rgba: [0; 4],
                labels: [(0x11, "Ctrl".into()), (0x10, "Shift".into())].into(),
            };
            let chord = received[2].event(0, &config);
            assert_eq!(chord.label, "S");
            assert_eq!(
                chord
                    .modifiers
                    .iter()
                    .map(|(_, label)| label.as_str())
                    .collect::<Vec<_>>(),
                ["Ctrl", "Shift"]
            );
            drop(input);
        }

        #[test]
        fn native_keyboard_observer_has_a_repeatable_joined_lifecycle() {
            for _ in 0..3 {
                let (sender, receiver) = crossbeam_channel::bounded(16);
                let observer = Observer::start(sender, Arc::new(AtomicU64::new(0))).unwrap();
                drop(observer);
                while receiver.try_recv().is_ok() {}
                assert!(matches!(
                    receiver.try_recv(),
                    Err(crossbeam_channel::TryRecvError::Disconnected)
                ));
            }
        }

        #[test]
        fn native_hook_copies_key_edges_suppresses_repeat_and_signals_overflow() {
            let (sender, receiver) = crossbeam_channel::bounded(2);
            let generation = Arc::new(AtomicU64::new(0));
            CONTEXT.with(|slot| {
                *slot.borrow_mut() = Some(Context {
                    sender,
                    generation: Arc::clone(&generation),
                    seen_generation: 0,
                    pressed: [0; 256],
                    left_ctrl_time: None,
                    alt_gr: false,
                })
            });
            let send = |key, message, time| {
                let raw = KBDLLHOOKSTRUCT {
                    vkCode: key,
                    scanCode: 30,
                    time,
                    ..Default::default()
                };
                unsafe {
                    hook_proc(
                        HC_ACTION as i32,
                        WPARAM(message as usize),
                        LPARAM((&raw const raw) as isize),
                    );
                }
            };
            send(65, WM_KEYDOWN, 100);
            send(65, WM_KEYDOWN, 110);
            send(65, WM_KEYUP, 120);
            assert_eq!(receiver.len(), 2);
            send(66, WM_KEYDOWN, 130);
            assert_eq!(generation.load(Ordering::Acquire), 1);
            let down = receiver.try_recv().unwrap();
            let up = receiver.try_recv().unwrap();
            assert!(down.down && !up.down);
            assert_eq!(down.key, 65);
            assert_eq!(down.pressed[65], 128);
            assert_eq!(up.pressed[65], 0);
            CONTEXT.with(|slot| *slot.borrow_mut() = None);
        }
    }
}

#[cfg(not(windows))]
mod platform {
    use super::*;
    pub struct Observer;
    impl Observer {
        pub fn start(_: Sender<KeyObservation>, _: Arc<AtomicU64>) -> Result<Self, String> {
            Err("keyboard recording is supported only on Windows".into())
        }
    }
    pub fn printable_label(_: u16, _: &KeyObservation) -> Option<String> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn modifier_sides_collapse_and_altgr_hides_only_synthetic_control() {
        let config = KeyboardOverlayConfig {
            keycap_size: 64,
            background_rgba: [0; 4],
            text_rgba: [0; 4],
            border_rgba: [0; 4],
            labels: [
                (0x11, "Ctrl".into()),
                (0x12, "Alt".into()),
                (0xa5, "AltGr".into()),
            ]
            .into(),
        };
        let mut event = KeyObservation {
            at: Instant::now(),
            key: 65,
            scan: 0,
            down: true,
            layout: 0,
            pressed: [0; 256],
            alt_gr: true,
            generation: 0,
            extra_info: 0,
        };
        event.pressed[0xa2] = 128;
        event.pressed[0xa5] = 128;
        assert_eq!(event.event(0, &config).modifiers, [(0xa5, "AltGr".into())]);
        event.pressed[0xa3] = 128;
        assert_eq!(event.event(0, &config).modifiers.len(), 2);
    }
}
