use std::time::Instant;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObservedMouseButton {
    Left,
    Right,
    Middle,
    Button4,
    Button5,
}

#[derive(Clone, Copy, Debug)]
pub struct MouseClickObservation {
    pub at: Instant,
    pub x: i32,
    pub y: i32,
    pub button: ObservedMouseButton,
    pub down: bool,
    pub modifiers: [bool; 4],
}

impl ObservedMouseButton {
    pub fn key(self) -> u16 {
        0x200 + self as u16
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::Left => "Left click",
            Self::Right => "Right click",
            Self::Middle => "Middle click",
            Self::Button4 => "Mouse button 4",
            Self::Button5 => "Mouse button 5",
        }
    }
    pub fn has_ring(self) -> bool {
        matches!(self, Self::Left | Self::Right | Self::Middle)
    }
}
impl MouseClickObservation {
    pub fn event(
        &self,
        at_ms: u64,
        style: &crate::keyboard_overlay::KeyboardOverlayConfig,
        include_modifiers: bool,
    ) -> crate::keyboard_overlay::KeyEvent {
        let key = self.button.key();
        let names = if cfg!(target_os = "macos") {
            ["Control", "Option", "Shift", "Command"]
        } else {
            ["Ctrl", "Alt", "Shift", "Win"]
        };
        let modifiers = [0x11u16, 0x12, 0x10, 0x5b]
            .into_iter()
            .zip(self.modifiers)
            .zip(names)
            .filter(|((_, held), _)| include_modifiers && *held)
            .map(|((key, _), name)| {
                (
                    key,
                    style
                        .labels
                        .get(&key)
                        .cloned()
                        .unwrap_or_else(|| name.into()),
                )
            })
            .collect();
        crate::keyboard_overlay::KeyEvent {
            at_ms,
            key,
            down: self.down,
            label: style
                .labels
                .get(&key)
                .cloned()
                .unwrap_or_else(|| self.button.label().into()),
            modifiers,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct MouseMovement {
    pub at: Instant,
    pub position: Option<(i32, i32)>,
    /// Changes on region exit, even if that movement is coalesced before presentation.
    pub continuity: u64,
}

#[cfg(windows)]
mod platform {
    use std::cell::RefCell;
    use std::sync::mpsc;
    use std::sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    };
    use std::thread::JoinHandle;

    use crossbeam_channel::Sender;
    use windows::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::Threading::GetCurrentThreadId;
    use windows::Win32::UI::WindowsAndMessaging::{
        CallNextHookEx, GetMessageW, HHOOK, MSG, MSLLHOOKSTRUCT, PM_NOREMOVE, PeekMessageW,
        PostThreadMessageW, SetWindowsHookExW, UnhookWindowsHookEx, WH_MOUSE_LL, WM_LBUTTONDOWN,
        WM_LBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEMOVE, WM_QUIT, WM_RBUTTONDOWN,
        WM_RBUTTONUP, WM_XBUTTONDOWN, WM_XBUTTONUP,
    };

    use super::{MouseClickObservation, MouseMovement, ObservedMouseButton};

    #[derive(Clone)]
    struct HookContext {
        sender: Sender<MouseClickObservation>,
        region: (i32, i32, u32, u32),
        movement: Option<(
            Sender<MouseMovement>,
            crossbeam_channel::Receiver<MouseMovement>,
        )>,
        continuity: u64,
        was_inside: bool,
        held: [bool; 5],
        generation: Arc<AtomicU64>,
    }

    thread_local! { static HOOK_CONTEXT: RefCell<Option<HookContext>> = const { RefCell::new(None) }; }

    unsafe extern "system" fn mouse_hook_proc(
        code: i32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        if code >= 0 {
            let hook = unsafe { &*(lparam.0 as *const MSLLHOOKSTRUCT) };
            let button = match wparam.0 as u32 {
                WM_LBUTTONDOWN | WM_LBUTTONUP => Some(ObservedMouseButton::Left),
                WM_RBUTTONDOWN | WM_RBUTTONUP => Some(ObservedMouseButton::Right),
                WM_MBUTTONDOWN | WM_MBUTTONUP => Some(ObservedMouseButton::Middle),
                WM_XBUTTONDOWN | WM_XBUTTONUP => Some(if hook.mouseData >> 16 == 1 {
                    ObservedMouseButton::Button4
                } else {
                    ObservedMouseButton::Button5
                }),
                _ => None,
            };
            let down = matches!(
                wparam.0 as u32,
                WM_LBUTTONDOWN | WM_RBUTTONDOWN | WM_MBUTTONDOWN | WM_XBUTTONDOWN
            );
            if button.is_some() || wparam.0 as u32 == WM_MOUSEMOVE {
                let hook = unsafe { &*(lparam.0 as *const MSLLHOOKSTRUCT) };
                HOOK_CONTEXT.with(|slot| {
                    let mut context = slot.borrow_mut();
                    let Some(context) = context.as_mut() else {
                        return;
                    };
                    let (x, y, width, height) = context.region;
                    let inside = i64::from(hook.pt.x) >= i64::from(x)
                        && i64::from(hook.pt.x) < i64::from(x) + i64::from(width)
                        && i64::from(hook.pt.y) >= i64::from(y)
                        && i64::from(hook.pt.y) < i64::from(y) + i64::from(height);
                    if let Some(button) = button.filter(|button| {
                        if down {
                            inside
                        } else {
                            context.held[*button as usize]
                        }
                    }) {
                        context.held[button as usize] = down;
                        if context
                            .sender
                            .try_send(MouseClickObservation {
                                at: std::time::Instant::now(),
                                x: hook.pt.x.saturating_sub(x),
                                y: hook.pt.y.saturating_sub(y),
                                button,
                                down,
                                modifiers: [0x11, 0x12, 0x10, 0x5b].map(|key| unsafe {
                                    windows::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState(
                                        key,
                                    ) < 0
                                }),
                            })
                            .is_err()
                        {
                            context.generation.fetch_add(1, Ordering::AcqRel);
                            context.held = [false; 5];
                        }
                    }
                    if wparam.0 as u32 == WM_MOUSEMOVE {
                        if context.was_inside && !inside {
                            context.continuity = context.continuity.wrapping_add(1);
                        }
                        context.was_inside = inside;
                        let Some((sender, drain)) = context.movement.as_ref() else {
                            return;
                        };
                        let event = MouseMovement {
                            at: std::time::Instant::now(),
                            continuity: context.continuity,
                            position: inside.then_some((
                                hook.pt.x.saturating_sub(x),
                                hook.pt.y.saturating_sub(y),
                            )),
                        };
                        if let Err(crossbeam_channel::TrySendError::Full(event)) =
                            sender.try_send(event)
                        {
                            let _ = drain.try_recv();
                            let _ = sender.try_send(event);
                        }
                    }
                });
            }
        }
        unsafe { CallNextHookEx(None, code, wparam, lparam) }
    }

    pub struct MouseHookObserver {
        thread_id: u32,
        generation: Arc<AtomicU64>,
        join: Option<JoinHandle<()>>,
    }

    impl MouseHookObserver {
        pub fn generation(&self) -> u64 {
            self.generation.load(Ordering::Acquire)
        }
        pub fn start(
            region: (i32, i32, u32, u32),
            sender: Sender<MouseClickObservation>,
        ) -> Result<Self, String> {
            Self::start_with_movement(region, sender, None)
        }

        pub fn start_with_movement(
            region: (i32, i32, u32, u32),
            sender: Sender<MouseClickObservation>,
            movement: Option<(
                Sender<MouseMovement>,
                crossbeam_channel::Receiver<MouseMovement>,
            )>,
        ) -> Result<Self, String> {
            let generation = Arc::new(AtomicU64::new(0));
            let hook_generation = generation.clone();
            let (ready_tx, ready_rx) = mpsc::sync_channel(1);
            let join = std::thread::Builder::new()
                .name("snow-recording-mouse-hook".to_string())
                .spawn(move || {
                    let thread_id = unsafe { GetCurrentThreadId() };
                    let mut message = MSG::default();
                    unsafe {
                        let _ = PeekMessageW(&mut message, None, 0, 0, PM_NOREMOVE);
                    }
                    HOOK_CONTEXT.with(|slot| {
                        *slot.borrow_mut() = Some(HookContext {
                            sender,
                            region,
                            movement,
                            continuity: 0,
                            was_inside: false,
                            held: [false; 5],
                            generation: hook_generation,
                        })
                    });
                    let hook = match unsafe {
                        SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_hook_proc), None, 0)
                    } {
                        Ok(hook) => hook,
                        Err(error) => {
                            clear_hook_context();
                            let _ = ready_tx.send(Err(format!(
                                "failed to install recording mouse observer: {error}"
                            )));
                            return;
                        }
                    };
                    let mut point = windows::Win32::Foundation::POINT::default();
                    if unsafe { windows::Win32::UI::WindowsAndMessaging::GetCursorPos(&mut point) }
                        .is_ok()
                    {
                        let initial = MSLLHOOKSTRUCT {
                            pt: point,
                            ..Default::default()
                        };
                        unsafe {
                            mouse_hook_proc(
                                0,
                                WPARAM(WM_MOUSEMOVE as usize),
                                LPARAM((&raw const initial) as isize),
                            );
                        }
                    }
                    let _ = ready_tx.send(Ok(thread_id));
                    while unsafe { GetMessageW(&mut message, None, 0, 0) }.as_bool() {}
                    clear_hook_context();
                    let _ = unsafe { UnhookWindowsHookEx(hook) };
                })
                .map_err(|error| format!("failed to start recording mouse observer: {error}"))?;
            match ready_rx.recv() {
                Ok(Ok(thread_id)) => Ok(Self {
                    thread_id,
                    generation,
                    join: Some(join),
                }),
                Ok(Err(error)) => {
                    let _ = join.join();
                    Err(error)
                }
                Err(_) => {
                    let _ = join.join();
                    Err("recording mouse observer stopped during initialization".to_string())
                }
            }
        }

        fn stop_and_join(&mut self) {
            if self.thread_id != 0 {
                let _ =
                    unsafe { PostThreadMessageW(self.thread_id, WM_QUIT, WPARAM(0), LPARAM(0)) };
                self.thread_id = 0;
            }
            if let Some(join) = self.join.take() {
                let _ = join.join();
            }
        }
    }

    fn clear_hook_context() {
        HOOK_CONTEXT.with(|slot| *slot.borrow_mut() = None);
    }

    impl Drop for MouseHookObserver {
        fn drop(&mut self) {
            self.stop_and_join();
        }
    }

    #[allow(dead_code)]
    fn _assert_hook_is_handle(_: HHOOK) {}
    #[cfg(test)]
    mod tests {
        use super::*;
        #[test]
        fn five_buttons_emit_edges_release_outside_and_never_accept_outside_presses() {
            let (sender, receiver) = crossbeam_channel::bounded(32);
            HOOK_CONTEXT.with(|slot| {
                *slot.borrow_mut() = Some(HookContext {
                    sender,
                    region: (10, 10, 100, 100),
                    movement: None,
                    continuity: 0,
                    was_inside: false,
                    held: [false; 5],
                    generation: Arc::new(AtomicU64::new(0)),
                })
            });
            for (button, down, up, data) in [
                (ObservedMouseButton::Left, WM_LBUTTONDOWN, WM_LBUTTONUP, 0),
                (ObservedMouseButton::Right, WM_RBUTTONDOWN, WM_RBUTTONUP, 0),
                (ObservedMouseButton::Middle, WM_MBUTTONDOWN, WM_MBUTTONUP, 0),
                (
                    ObservedMouseButton::Button4,
                    WM_XBUTTONDOWN,
                    WM_XBUTTONUP,
                    1 << 16,
                ),
                (
                    ObservedMouseButton::Button5,
                    WM_XBUTTONDOWN,
                    WM_XBUTTONUP,
                    2 << 16,
                ),
            ] {
                for (message, point) in [(down, 0), (up, 0), (down, 20), (up, 200)] {
                    let raw = MSLLHOOKSTRUCT {
                        pt: windows::Win32::Foundation::POINT { x: point, y: point },
                        mouseData: data,
                        ..Default::default()
                    };
                    unsafe {
                        mouse_hook_proc(
                            0,
                            WPARAM(message as usize),
                            LPARAM((&raw const raw) as isize),
                        );
                    }
                }
                let events: Vec<_> = receiver.try_iter().collect();
                assert_eq!(events.len(), 2);
                assert!(events[0].down && !events[1].down);
                assert_eq!(events[0].button, button);
                assert_eq!((events[0].x, events[0].y), (10, 10));
            }
            clear_hook_context();
        }
    }
}

#[cfg(target_os = "macos")]
mod platform {
    use super::*;
    use crossbeam_channel::{Receiver, Sender};
    use std::sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    };
    use std::time::Duration;
    pub struct MouseHookObserver {
        stop: Sender<()>,
        worker: Option<std::thread::JoinHandle<()>>,
        generation: Arc<AtomicU64>,
    }
    impl MouseHookObserver {
        pub fn generation(&self) -> u64 {
            self.generation.load(Ordering::Acquire)
        }
        pub fn start(
            region: (i32, i32, u32, u32),
            sender: Sender<MouseClickObservation>,
        ) -> Result<Self, String> {
            Self::start_with_movement(region, sender, None)
        }
        pub fn start_with_movement(
            region: (i32, i32, u32, u32),
            sender: Sender<MouseClickObservation>,
            movement: Option<(Sender<MouseMovement>, Receiver<MouseMovement>)>,
        ) -> Result<Self, String> {
            let input =
                snow_macos::input::InputObserver::start(false, true).map_err(|e| e.to_string())?;
            let (stop, stopped) = crossbeam_channel::bounded(1);
            let generation = Arc::new(AtomicU64::new(0));
            let shared = generation.clone();
            let worker = std::thread::Builder::new().name("snow-mouse-adapter".into()).spawn(move || {
                let mut held = [false;5];
                let mut input_generation = input.generation();
                let mut continuity = 0;
                let mut was_inside = false;
                loop {
                    let current = input.generation();
                    if current != input_generation {
                        input_generation = current; held = [false;5];
                        shared.fetch_add(1, Ordering::AcqRel);
                    }
                    crossbeam_channel::select_biased! {
                        recv(stopped) -> _ => break,
                        recv(input.events) -> event => {
                            let Ok(event) = event else { break; };
                            if event.generation != input_generation { continue; }
                            let Some(at) = event.instant() else { continue; };
                            // The compatibility recording region is in desktop points on macOS.
                            let x = event.x.round() as i32 - region.0;
                            let y = event.y.round() as i32 - region.1;
                            let inside = x >= 0 && y >= 0 && x < region.2 as i32 && y < region.3 as i32;
                            let button = match event.kind { 1|2 => Some(ObservedMouseButton::Left), 3|4 => Some(ObservedMouseButton::Right), 25|26 => match event.mouse_button { 2 => Some(ObservedMouseButton::Middle), 3 => Some(ObservedMouseButton::Button4), 4 => Some(ObservedMouseButton::Button5), _ => None }, _ => None };
                            let down = matches!(event.kind,1|3|25);
                            if let Some(button) = button.filter(|button| if down { inside } else { held[*button as usize] }) {
                                held[button as usize] = down;
                                if sender.try_send(MouseClickObservation { at,x,y,button,down,modifiers:[18,19,17,20].map(|bit| event.modifiers & (1 << bit) != 0) }).is_err() {
                                    shared.fetch_add(1,Ordering::AcqRel); held = [false;5];
                                }
                            }
                            if matches!(event.kind,5..=7|27) && let Some((sender,drain)) = &movement {
                                if was_inside && !inside { continuity += 1; }
                                was_inside = inside;
                                let event = MouseMovement { at, position: inside.then_some((x,y)), continuity };
                                if let Err(crossbeam_channel::TrySendError::Full(event)) = sender.try_send(event) { let _ = drain.try_recv(); let _ = sender.try_send(event); }
                            }
                        },
                        default(Duration::from_millis(50)) => {},
                    }
                }
            }).map_err(|e| e.to_string())?;
            Ok(Self {
                stop,
                worker: Some(worker),
                generation,
            })
        }
    }
    impl Drop for MouseHookObserver {
        fn drop(&mut self) {
            let _ = self.stop.try_send(());
            if let Some(worker) = self.worker.take() {
                let _ = worker.join();
            }
        }
    }
}

#[cfg(not(any(windows, target_os = "macos")))]
mod platform {
    use crossbeam_channel::Sender;

    use super::{MouseClickObservation, MouseMovement};

    pub struct MouseHookObserver;

    impl MouseHookObserver {
        pub fn generation(&self) -> u64 {
            0
        }
        pub fn start(
            _region: (i32, i32, u32, u32),
            _sender: Sender<MouseClickObservation>,
        ) -> Result<Self, String> {
            Ok(Self)
        }
        pub fn start_with_movement(
            _region: (i32, i32, u32, u32),
            _sender: Sender<MouseClickObservation>,
            _movement: Option<(
                Sender<MouseMovement>,
                crossbeam_channel::Receiver<MouseMovement>,
            )>,
        ) -> Result<Self, String> {
            Err("native mouse observation requires Windows".into())
        }
    }
}

pub use platform::MouseHookObserver;
