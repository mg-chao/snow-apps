use std::time::Instant;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ObservedMouseButton {
    Left,
    Right,
    Middle,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct MouseClickObservation {
    pub(crate) at: Instant,
    pub(crate) x: i32,
    pub(crate) y: i32,
    pub(crate) button: ObservedMouseButton,
}

#[cfg(windows)]
mod platform {
    use std::sync::{Mutex, OnceLock, mpsc};
    use std::thread::JoinHandle;

    use crossbeam_channel::Sender;
    use windows::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows::Win32::System::Threading::GetCurrentThreadId;
    use windows::Win32::UI::WindowsAndMessaging::{
        CallNextHookEx, GetMessageW, HHOOK, MSG, MSLLHOOKSTRUCT, PM_NOREMOVE, PeekMessageW,
        PostThreadMessageW, SetWindowsHookExW, UnhookWindowsHookEx, WH_MOUSE_LL, WM_LBUTTONDOWN,
        WM_MBUTTONDOWN, WM_QUIT, WM_RBUTTONDOWN,
    };

    use super::{MouseClickObservation, ObservedMouseButton};

    #[derive(Clone)]
    struct HookContext {
        sender: Sender<MouseClickObservation>,
        region: (i32, i32, u32, u32),
    }

    static HOOK_CONTEXT: OnceLock<Mutex<Option<HookContext>>> = OnceLock::new();

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
            if let Some(button) = button {
                let hook = unsafe { &*(lparam.0 as *const MSLLHOOKSTRUCT) };
                if let Ok(context) = HOOK_CONTEXT.get_or_init(Default::default).lock()
                    && let Some(context) = context.as_ref()
                {
                    let (x, y, width, height) = context.region;
                    let right = i64::from(x).saturating_add(i64::from(width));
                    let bottom = i64::from(y).saturating_add(i64::from(height));
                    if i64::from(hook.pt.x) >= i64::from(x)
                        && i64::from(hook.pt.x) < right
                        && i64::from(hook.pt.y) >= i64::from(y)
                        && i64::from(hook.pt.y) < bottom
                    {
                        let _ = context.sender.try_send(MouseClickObservation {
                            at: std::time::Instant::now(),
                            x: hook.pt.x.saturating_sub(x),
                            y: hook.pt.y.saturating_sub(y),
                            button,
                        });
                    }
                }
            }
        }
        unsafe { CallNextHookEx(None, code, wparam, lparam) }
    }

    pub(crate) struct MouseHookObserver {
        thread_id: u32,
        join: Option<JoinHandle<()>>,
    }

    impl MouseHookObserver {
        pub(crate) fn start(
            region: (i32, i32, u32, u32),
            sender: Sender<MouseClickObservation>,
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
                    if let Ok(mut slot) = HOOK_CONTEXT.get_or_init(Default::default).lock() {
                        *slot = Some(HookContext { sender, region });
                    }
                    let hook = match unsafe {
                        SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_hook_proc), None, 0)
                    } {
                        Ok(hook) => hook,
                        Err(error) => {
                            if let Ok(mut slot) = HOOK_CONTEXT.get_or_init(Default::default).lock()
                            {
                                *slot = None;
                            }
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
            clear_hook_context();
        }
    }

    fn clear_hook_context() {
        if let Ok(mut slot) = HOOK_CONTEXT.get_or_init(Default::default).lock() {
            *slot = None;
        }
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

    use super::MouseClickObservation;

    pub(crate) struct MouseHookObserver;

    impl MouseHookObserver {
        pub(crate) fn start(
            _region: (i32, i32, u32, u32),
            _sender: Sender<MouseClickObservation>,
        ) -> Result<Self, String> {
            Ok(Self)
        }
    }
}

pub(crate) use platform::MouseHookObserver;
