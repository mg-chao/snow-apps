//! Hidden native control fixtures. These never activate a window or access the clipboard.
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{LPARAM, WPARAM};
use windows::Win32::UI::Controls::{EM_LIMITTEXT, EM_SETSEL};
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::{PCWSTR, w};

use crate::model::*;
use crate::policy::Context;
use crate::runtime::RequestState;

pub(super) struct EditFixture {
    pub window: usize,
    stop: mpsc::Sender<()>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl EditFixture {
    pub fn new(text: &str, start: u32, end: u32, password: bool) -> Self {
        let (ready, window) = mpsc::channel();
        let (stop, stopped) = mpsc::channel();
        let text: Vec<u16> = text.encode_utf16().chain(Some(0)).collect();
        let worker = std::thread::spawn(move || unsafe {
            let style = WS_POPUP
                | WINDOW_STYLE(ES_MULTILINE as u32 | if password { ES_PASSWORD as u32 } else { 0 });
            let window = CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                w!("EDIT"),
                w!(""),
                style,
                0,
                0,
                300,
                100,
                None,
                None,
                None,
                None,
            )
            .unwrap();
            SendMessageW(
                window,
                EM_LIMITTEXT,
                Some(WPARAM(32 * 1024 * 1024)),
                Some(LPARAM(0)),
            );
            SetWindowTextW(window, PCWSTR(text.as_ptr())).unwrap();
            SendMessageW(
                window,
                EM_SETSEL,
                Some(WPARAM(start as usize)),
                Some(LPARAM(end as isize)),
            );
            ready.send(window.0 as usize).unwrap();
            while stopped.try_recv().is_err() {
                let mut message = MSG::default();
                while PeekMessageW(&mut message, None, 0, 0, PM_REMOVE).as_bool() {
                    let _ = TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            DestroyWindow(window).unwrap();
        });
        Self {
            window: window.recv_timeout(Duration::from_secs(5)).unwrap(),
            stop,
            worker: Some(worker),
        }
    }

    pub fn context(&self) -> Context {
        Context {
            source: SourceWindow {
                window: self.window,
                focused_control: self.window,
                process_id: std::process::id(),
                executable: "test.exe".into(),
            },
            options: CaptureOptions::default(),
            state: Arc::new(RequestState::new(Instant::now() + Duration::from_secs(5))),
        }
    }
}

impl Drop for EditFixture {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(worker) = self.worker.take() {
            worker.join().unwrap();
        }
    }
}
