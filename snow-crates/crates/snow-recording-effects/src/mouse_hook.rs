use std::time::Instant;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObservedMouseButton {
    Left,
    Right,
    Middle,
}

#[derive(Clone, Copy, Debug)]
pub struct MouseClickObservation {
    pub at: Instant,
    pub x: i32,
    pub y: i32,
    pub button: ObservedMouseButton,
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
    use std::thread::JoinHandle;

    use crossbeam_channel::Sender;
    use windows::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::Threading::GetCurrentThreadId;
    use windows::Win32::UI::WindowsAndMessaging::{
        CallNextHookEx, GetMessageW, HHOOK, MSG, MSLLHOOKSTRUCT, PM_NOREMOVE, PeekMessageW,
        PostThreadMessageW, SetWindowsHookExW, UnhookWindowsHookEx, WH_MOUSE_LL, WM_LBUTTONDOWN,
        WM_MBUTTONDOWN, WM_MOUSEMOVE, WM_QUIT, WM_RBUTTONDOWN,
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
    }

    thread_local! { static HOOK_CONTEXT: RefCell<Option<HookContext>> = const { RefCell::new(None) }; }

    unsafe extern "system" fn mouse_hook_proc(
        code: i32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        if code >= 0 {
            let button = match wparam.0 as u32 {
                WM_LBUTTONDOWN => Some(ObservedMouseButton::Left),
                WM_RBUTTONDOWN => Some(ObservedMouseButton::Right),
                WM_MBUTTONDOWN => Some(ObservedMouseButton::Middle),
                _ => None,
            };
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
                    if let Some(button) = button.filter(|_| inside) {
                        let _ = context.sender.try_send(MouseClickObservation {
                            at: std::time::Instant::now(),
                            x: hook.pt.x.saturating_sub(x),
                            y: hook.pt.y.saturating_sub(y),
                            button,
                        });
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
            movement: Option<(
                Sender<MouseMovement>,
                crossbeam_channel::Receiver<MouseMovement>,
            )>,
        ) -> Result<Self, String> {
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
                    let _ = ready_tx.send(Ok(thread_id));
                    while unsafe { GetMessageW(&mut message, None, 0, 0) }.as_bool() {}
                    clear_hook_context();
                    let _ = unsafe { UnhookWindowsHookEx(hook) };
                })
                .map_err(|error| format!("failed to start recording mouse observer: {error}"))?;
            match ready_rx.recv() {
                Ok(Ok(thread_id)) => Ok(Self {
                    thread_id,
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
}

#[cfg(not(windows))]
mod platform {
    use crossbeam_channel::Sender;

    use super::{MouseClickObservation, MouseMovement};

    pub struct MouseHookObserver;

    impl MouseHookObserver {
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
