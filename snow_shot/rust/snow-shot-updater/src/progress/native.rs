use super::{
    Bounds, Progress, ProgressAppearance, ProgressPhase, ProgressTexts, Visibility, scaled,
    window_bounds,
};
use std::cell::RefCell;
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::Duration;
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::*;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, GetDpiForMonitor, MDT_EFFECTIVE_DPI,
    SetThreadDpiAwarenessContext,
};
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::{PCWSTR, w};

pub(super) enum Command {
    Update(Progress),
    Visible(bool),
    Texts(Box<ProgressTexts>),
    Appearance(Box<ProgressAppearance>),
    Finish,
}

#[derive(Default)]
struct Inbox {
    progress: Option<Progress>,
    texts: Option<ProgressTexts>,
    appearance: Option<ProgressAppearance>,
    visible: Option<bool>,
    finished: bool,
}

#[derive(Default)]
struct Shared {
    inbox: Mutex<Inbox>,
    changed: Condvar,
}

impl Shared {
    fn receive(&self) -> Option<Inbox> {
        let inbox = self.inbox.lock().ok()?;
        let (mut inbox, _) = self
            .changed
            .wait_timeout_while(inbox, Duration::from_millis(50), |value| {
                !value.finished
                    && value.progress.is_none()
                    && value.texts.is_none()
                    && value.appearance.is_none()
                    && value.visible.is_none()
            })
            .ok()?;
        Some(Inbox {
            progress: inbox.progress.take(),
            texts: inbox.texts.take(),
            appearance: inbox.appearance.take(),
            visible: inbox.visible.take(),
            finished: inbox.finished,
        })
    }
}

pub(super) struct Sender {
    shared: Arc<Shared>,
}

impl Sender {
    pub(super) fn send(&self, command: Command) {
        let Ok(mut inbox) = self.shared.inbox.lock() else {
            return;
        };
        if inbox.finished {
            return;
        }
        match command {
            Command::Update(progress) => inbox.progress = Some(progress),
            Command::Visible(visible) => inbox.visible = Some(visible),
            Command::Texts(texts) => inbox.texts = Some(*texts),
            Command::Appearance(appearance) => inbox.appearance = Some(appearance.normalized()),
            Command::Finish => inbox.finished = true,
        }
        self.shared.changed.notify_one();
    }
}

pub(super) fn spawn(texts: ProgressTexts, appearance: ProgressAppearance) -> Option<Sender> {
    let shared = Arc::new(Shared::default());
    let receiver = shared.clone();
    std::thread::Builder::new()
        .name("updater-progress".to_owned())
        .spawn(move || run(receiver, texts, appearance))
        .ok()
        .map(|_| Sender { shared })
}

struct NativeState {
    visibility: Visibility,
    progress: Progress,
    texts: ProgressTexts,
    appearance: ProgressAppearance,
    close_hover: bool,
    region: Option<(i32, i32, i32)>,
    monitor: HMONITOR,
    dpi: u32,
    tick: u32,
}

impl NativeState {
    fn new(texts: ProgressTexts, appearance: ProgressAppearance) -> Self {
        Self {
            visibility: Visibility::default(),
            progress: Progress::new(ProgressPhase::Preparing, 0, 0),
            texts,
            appearance: appearance.normalized(),
            close_hover: false,
            region: None,
            monitor: HMONITOR::default(),
            dpi: 96,
            tick: 0,
        }
    }

    fn status_text(&self) -> String {
        let phase = self.texts.phase(self.progress.phase);
        self.progress.permille().map_or_else(
            || phase.to_owned(),
            |value| format!("{phase}  {}%", value / 10),
        )
    }

    fn close_bounds(&self, width: i32) -> RECT {
        let side = scaled(22, self.dpi);
        let right = width - scaled(16, self.dpi);
        let top = scaled(16, self.dpi);
        RECT {
            left: right - side,
            top,
            right,
            bottom: top + side,
        }
    }

    fn over_close(&self, width: i32, x: i32, y: i32) -> bool {
        let close = self.close_bounds(width);
        x >= close.left && x < close.right && y >= close.top && y < close.bottom
    }

    fn title_height(&self, dc: HDC, width: i32) -> i32 {
        measured_height(
            dc,
            &self.texts.title,
            width - scaled(70, self.dpi),
            self.dpi,
            &self.appearance,
            self.appearance.font_size + 2,
            1.5,
        )
    }

    fn status_height(&self, dc: HDC, width: i32) -> i32 {
        let reserved = self.percent_width(dc);
        measured_height(
            dc,
            self.texts.phase(self.progress.phase),
            width - scaled(48, self.dpi) - reserved,
            self.dpi,
            &self.appearance,
            self.appearance.font_size,
            1.5715,
        )
    }

    fn percent_width(&self, dc: HDC) -> i32 {
        if self.progress.permille().is_none() {
            return 0;
        }
        unsafe {
            let font = font(self.dpi, &self.appearance, self.appearance.font_size);
            let old = SelectObject(dc, HGDIOBJ(font.0));
            let mut rect = RECT::default();
            let mut text: Vec<u16> = "100%".encode_utf16().collect();
            let _ = DrawTextW(
                dc,
                &mut text,
                &mut rect,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX,
            );
            let _ = SelectObject(dc, old);
            let _ = DeleteObject(HGDIOBJ(font.0));
            (rect.right - rect.left).max(0) + scaled(8, self.dpi)
        }
    }

    fn content_height(&self, dc: HDC, width: i32) -> i32 {
        let counts = self.texts.counts(self.progress).map_or(0, |text| {
            scaled(8, self.dpi)
                + measured_height(
                    dc,
                    &text,
                    width - scaled(48, self.dpi),
                    self.dpi,
                    &self.appearance,
                    self.appearance.small_font_size,
                    1.6667,
                )
        });
        scaled(58, self.dpi) + self.title_height(dc, width) + self.status_height(dc, width) + counts
    }

    fn position(&mut self, hwnd: HWND) {
        unsafe {
            if self.monitor.is_invalid() {
                let mut cursor = POINT::default();
                let _ = GetCursorPos(&mut cursor);
                self.monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            }
            let mut monitor = MONITORINFO {
                cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            if !GetMonitorInfoW(self.monitor, &mut monitor).as_bool() {
                // Preserve the original display while it exists. After removal,
                // use the display nearest the previous window location.
                self.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                if !GetMonitorInfoW(self.monitor, &mut monitor).as_bool() {
                    return;
                }
            }
            let mut dpi_x = self.dpi;
            let mut dpi_y = self.dpi;
            if GetDpiForMonitor(self.monitor, MDT_EFFECTIVE_DPI, &mut dpi_x, &mut dpi_y).is_ok() {
                self.dpi = dpi_x.max(96);
            }
            let work = Bounds {
                left: monitor.rcWork.left,
                top: monitor.rcWork.top,
                right: monitor.rcWork.right,
                bottom: monitor.rcWork.bottom,
            };
            let width =
                window_bounds(work, self.dpi, 1).right - window_bounds(work, self.dpi, 1).left;
            let dc = GetDC(Some(hwnd));
            let height = self.content_height(dc, width);
            let _ = ReleaseDC(Some(hwnd), dc);
            let bounds = window_bounds(work, self.dpi, height);
            let _ = SetWindowPos(
                hwnd,
                Some(HWND_TOPMOST),
                bounds.left,
                bounds.top,
                bounds.right - bounds.left,
                bounds.bottom - bounds.top,
                SWP_NOACTIVATE,
            );
            let width = bounds.right - bounds.left;
            let height = bounds.bottom - bounds.top;
            let radius = scaled(self.appearance.border_radius as i32, self.dpi);
            if self.region != Some((width, height, radius)) {
                let region =
                    CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
                if !region.is_invalid() {
                    if SetWindowRgn(hwnd, Some(region), true) == 0 {
                        let _ = DeleteObject(HGDIOBJ(region.0));
                    } else {
                        self.region = Some((width, height, radius));
                    }
                }
            }
            let counts = self
                .texts
                .counts(self.progress)
                .map_or_else(String::new, |text| format!(" — {text}"));
            let caption = wide(&format!(
                "{} — {}{}",
                self.texts.title,
                self.status_text(),
                counts
            ));
            let _ = SetWindowTextW(hwnd, PCWSTR(caption.as_ptr()));
            let _ = InvalidateRect(Some(hwnd), None, false);
        }
    }

    fn paint(&self, hwnd: HWND) {
        unsafe {
            let mut paint = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut paint);
            let mut client = RECT::default();
            let _ = GetClientRect(hwnd, &mut client);
            let memory = CreateCompatibleDC(Some(dc));
            let bitmap = CreateCompatibleBitmap(dc, client.right.max(1), client.bottom.max(1));
            if memory.is_invalid() || bitmap.is_invalid() {
                self.draw(hwnd, dc);
            } else {
                let previous = SelectObject(memory, HGDIOBJ(bitmap.0));
                self.draw(hwnd, memory);
                let _ = BitBlt(
                    dc,
                    0,
                    0,
                    client.right,
                    client.bottom,
                    Some(memory),
                    0,
                    0,
                    SRCCOPY,
                );
                let _ = SelectObject(memory, previous);
            }
            let _ = DeleteObject(HGDIOBJ(bitmap.0));
            let _ = DeleteDC(memory);
            let _ = EndPaint(hwnd, &paint);
        }
    }

    fn draw(&self, hwnd: HWND, dc: HDC) {
        unsafe {
            let mut client = RECT::default();
            let _ = GetClientRect(hwnd, &mut client);
            let theme = &self.appearance;
            fill(dc, client, theme.background);
            rounded(
                dc,
                RECT {
                    right: client.right - 1,
                    bottom: client.bottom - 1,
                    ..client
                },
                scaled(theme.border_radius as i32, self.dpi),
                theme.background,
                Some((theme.border, scaled(1, self.dpi))),
            );
            let _ = SetBkMode(dc, TRANSPARENT);
            let margin = scaled(24, self.dpi);
            let top = scaled(16, self.dpi);
            let title_height = self.title_height(dc, client.right);
            let title = RECT {
                left: margin,
                top,
                right: client.right - scaled(46, self.dpi),
                bottom: top + title_height,
            };
            draw_text(
                dc,
                &self.texts.title,
                title,
                self.dpi,
                theme,
                TextRole::Title,
                DT_WORDBREAK,
            );
            let phase_top = title.bottom + scaled(8, self.dpi);
            let phase_height = self.status_height(dc, client.right);
            let reserved = self.percent_width(dc);
            let phase = RECT {
                left: margin,
                top: phase_top,
                right: client.right - margin - reserved,
                bottom: phase_top + phase_height,
            };
            draw_text(
                dc,
                self.texts.phase(self.progress.phase),
                phase,
                self.dpi,
                theme,
                TextRole::Body,
                DT_WORDBREAK,
            );
            if let Some(value) = self.progress.permille() {
                let percent = RECT {
                    left: phase.right + scaled(8, self.dpi),
                    right: client.right - margin,
                    ..phase
                };
                draw_text(
                    dc,
                    &format!("{}%", value / 10),
                    percent,
                    self.dpi,
                    theme,
                    TextRole::Percent,
                    DT_RIGHT | DT_SINGLELINE,
                );
            }
            let bar_top = phase.bottom + scaled(12, self.dpi);
            let bar = RECT {
                left: margin,
                top: bar_top,
                right: client.right - margin,
                bottom: bar_top + scaled(6, self.dpi),
            };
            let radius = scaled(3, self.dpi);
            rounded(dc, bar, radius, theme.fill_secondary, None);
            let width = (bar.right - bar.left).max(0);
            let active = if let Some(value) = self.progress.permille() {
                RECT {
                    right: bar.left + ((i64::from(width) * i64::from(value)) / 1000) as i32,
                    ..bar
                }
            } else {
                let segment = (width / 4).max(1);
                let tick = if theme.motion { self.tick % 60 } else { 24 };
                let offset = (i64::from(tick) * i64::from(width + segment) / 60) as i32 - segment;
                RECT {
                    left: (bar.left + offset).max(bar.left),
                    right: (bar.left + offset + segment).min(bar.right),
                    ..bar
                }
            };
            if active.right > active.left {
                let color = match self.progress.phase {
                    ProgressPhase::Failed | ProgressPhase::Restoring => theme.error,
                    ProgressPhase::Complete | ProgressPhase::Ready => theme.success,
                    _ => theme.primary,
                };
                // Clip the moving segment to the same capsule as the rail.
                let clip = CreateRoundRectRgn(
                    bar.left,
                    bar.top,
                    bar.right + 1,
                    bar.bottom + 1,
                    radius * 2,
                    radius * 2,
                );
                let saved = SaveDC(dc);
                let _ = SelectClipRgn(dc, Some(clip));
                rounded(dc, active, radius, color, None);
                let _ = RestoreDC(dc, saved);
                let _ = DeleteObject(HGDIOBJ(clip.0));
            }
            if let Some(counts) = self.texts.counts(self.progress) {
                let count = RECT {
                    left: margin,
                    top: bar.bottom + scaled(8, self.dpi),
                    right: client.right - margin,
                    bottom: client.bottom - scaled(16, self.dpi),
                };
                draw_text(
                    dc,
                    &counts,
                    count,
                    self.dpi,
                    theme,
                    TextRole::Detail,
                    DT_WORDBREAK,
                );
            }
            let close = self.close_bounds(client.right);
            if self.close_hover {
                rounded(dc, close, scaled(4, self.dpi), theme.fill_tertiary, None);
            }
            let icon = scaled(10, self.dpi);
            let cx = (close.left + close.right) / 2;
            let cy = (close.top + close.bottom) / 2;
            let pen = CreatePen(
                PS_SOLID,
                scaled(1, self.dpi),
                rgb(if self.close_hover {
                    theme.text
                } else {
                    theme.text_tertiary
                }),
            );
            let old = SelectObject(dc, HGDIOBJ(pen.0));
            let _ = MoveToEx(dc, cx - icon / 2, cy - icon / 2, None);
            let _ = LineTo(dc, cx + icon / 2, cy + icon / 2);
            let _ = MoveToEx(dc, cx + icon / 2, cy - icon / 2, None);
            let _ = LineTo(dc, cx - icon / 2, cy + icon / 2);
            let _ = SelectObject(dc, old);
            let _ = DeleteObject(HGDIOBJ(pen.0));
        }
    }
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn font(dpi: u32, appearance: &ProgressAppearance, size: u32) -> HFONT {
    let face = wide(&appearance.font_family);
    unsafe {
        CreateFontW(
            -scaled(size as i32, dpi),
            0,
            0,
            0,
            400,
            0,
            0,
            0,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            0,
            PCWSTR(face.as_ptr()),
        )
    }
}

fn measured_height(
    dc: HDC,
    text: &str,
    width: i32,
    dpi: u32,
    appearance: &ProgressAppearance,
    size: u32,
    line_height: f64,
) -> i32 {
    unsafe {
        let font = font(dpi, appearance, size);
        let previous = SelectObject(dc, HGDIOBJ(font.0));
        let mut bounds = RECT {
            right: width.max(1),
            ..Default::default()
        };
        let mut text: Vec<u16> = text.encode_utf16().collect();
        let _ = DrawTextW(
            dc,
            &mut text,
            &mut bounds,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX,
        );
        let _ = SelectObject(dc, previous);
        let _ = DeleteObject(HGDIOBJ(font.0));
        (bounds.bottom - bounds.top)
            .max(scaled((f64::from(size) * line_height).round() as i32, dpi))
    }
}

enum TextRole {
    Title,
    Body,
    Percent,
    Detail,
}

fn draw_text(
    dc: HDC,
    text: &str,
    mut rect: RECT,
    dpi: u32,
    appearance: &ProgressAppearance,
    role: TextRole,
    flags: DRAW_TEXT_FORMAT,
) {
    let (size, color) = match role {
        TextRole::Title => (appearance.font_size + 2, appearance.text),
        TextRole::Body => (appearance.font_size, appearance.text),
        TextRole::Percent => (appearance.font_size, appearance.text_secondary),
        TextRole::Detail => (appearance.small_font_size, appearance.text_secondary),
    };
    unsafe {
        let font = font(dpi, appearance, size);
        let previous = SelectObject(dc, HGDIOBJ(font.0));
        let _ = SetTextColor(dc, rgb(color));
        let mut text: Vec<u16> = text.encode_utf16().collect();
        let _ = DrawTextW(dc, &mut text, &mut rect, flags | DT_NOPREFIX);
        let _ = SelectObject(dc, previous);
        let _ = DeleteObject(HGDIOBJ(font.0));
    }
}

fn rgb(value: u32) -> COLORREF {
    COLORREF(((value >> 16) & 0xff) | (value & 0xff00) | ((value & 0xff) << 16))
}

fn fill(dc: HDC, rect: RECT, color: u32) {
    unsafe {
        let brush = CreateSolidBrush(rgb(color));
        let _ = FillRect(dc, &rect, brush);
        let _ = DeleteObject(HGDIOBJ(brush.0));
    }
}

fn rounded(dc: HDC, rect: RECT, radius: i32, color: u32, border: Option<(u32, i32)>) {
    unsafe {
        let brush = CreateSolidBrush(rgb(color));
        let pen = border
            .map(|(color, width)| CreatePen(PS_SOLID, width, rgb(color)))
            .unwrap_or_else(|| HPEN(GetStockObject(NULL_PEN).0));
        let old_brush = SelectObject(dc, HGDIOBJ(brush.0));
        let old_pen = SelectObject(dc, HGDIOBJ(pen.0));
        let _ = RoundRect(
            dc,
            rect.left,
            rect.top,
            rect.right,
            rect.bottom,
            radius * 2,
            radius * 2,
        );
        let _ = SelectObject(dc, old_pen);
        let _ = SelectObject(dc, old_brush);
        let _ = DeleteObject(HGDIOBJ(brush.0));
        if border.is_some() {
            let _ = DeleteObject(HGDIOBJ(pen.0));
        }
    }
}

fn window_class() -> Option<HINSTANCE> {
    static REGISTERED: OnceLock<bool> = OnceLock::new();
    let instance = HINSTANCE(unsafe { GetModuleHandleW(None).ok()? }.0);
    let registered = REGISTERED.get_or_init(|| unsafe {
        RegisterClassW(&WNDCLASSW {
            style: CS_DROPSHADOW,
            hInstance: instance,
            lpszClassName: w!("SnowShotUpdaterProgress"),
            lpfnWndProc: Some(window_proc),
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            ..Default::default()
        }) != 0
    });
    registered.then_some(instance)
}

fn create(state: &RefCell<NativeState>) -> Option<HWND> {
    let instance = window_class()?;
    unsafe {
        CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            w!("SnowShotUpdaterProgress"),
            w!("Snow Shot update"),
            WS_POPUP,
            0,
            0,
            360,
            124,
            None,
            None,
            Some(instance),
            Some((state as *const RefCell<NativeState>).cast()),
        )
        .ok()
    }
}

unsafe extern "system" fn window_proc(hwnd: HWND, message: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        if message == WM_NCCREATE {
            let create = &*(lp.0 as *const CREATESTRUCTW);
            let _ = SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize);
        }
        let pointer = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const RefCell<NativeState>;
        if let Some(cell) = pointer.as_ref()
            && let Ok(mut state) = cell.try_borrow_mut()
        {
            match message {
                WM_PAINT => {
                    state.paint(hwnd);
                    return LRESULT(0);
                }
                WM_PRINTCLIENT => {
                    state.draw(hwnd, HDC(wp.0 as *mut _));
                    return LRESULT(0);
                }
                WM_ERASEBKGND => return LRESULT(1),
                WM_MOUSEACTIVATE => return LRESULT(MA_NOACTIVATE as isize),
                WM_CLOSE => {
                    state.visibility.dismiss();
                    let _ = ShowWindow(hwnd, SW_HIDE);
                    return LRESULT(0);
                }
                WM_LBUTTONUP => {
                    let x = (lp.0 as u16) as i16 as i32;
                    let y = ((lp.0 >> 16) as u16) as i16 as i32;
                    let mut rect = RECT::default();
                    let _ = GetClientRect(hwnd, &mut rect);
                    if state.over_close(rect.right, x, y) {
                        state.visibility.dismiss();
                        let _ = ShowWindow(hwnd, SW_HIDE);
                    }
                    return LRESULT(0);
                }
                WM_DPICHANGED => {
                    state.dpi = (wp.0 & 0xffff) as u32;
                    state.position(hwnd);
                    return LRESULT(0);
                }
                WM_DISPLAYCHANGE | WM_SETTINGCHANGE => {
                    state.position(hwnd);
                    return LRESULT(0);
                }
                _ => {}
            }
        }
        DefWindowProcW(hwnd, message, wp, lp)
    }
}

fn run(receiver: Arc<Shared>, texts: ProgressTexts, appearance: ProgressAppearance) {
    unsafe {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    let state = Box::new(RefCell::new(NativeState::new(texts, appearance)));
    let mut hwnd = None;
    let mut finished = false;
    while !finished {
        let Some(inbox) = receiver.receive() else {
            break;
        };
        let changed = inbox.progress.is_some()
            || inbox.texts.is_some()
            || inbox.appearance.is_some()
            || inbox.visible.is_some();
        {
            let mut value = state.borrow_mut();
            if let Some(progress) = inbox.progress {
                value.progress = progress;
            }
            if let Some(texts) = inbox.texts {
                value.texts = texts;
            }
            if let Some(appearance) = inbox.appearance {
                value.appearance = appearance;
            }
            if let Some(visible) = inbox.visible {
                value.visibility.request(visible);
            }
            if inbox.finished {
                value.visibility.finish();
                finished = true;
            }
            if changed && !finished {
                if value.visibility.visible() && hwnd.is_none() {
                    hwnd = create(&state);
                }
                if let Some(hwnd) = hwnd {
                    value.position(hwnd);
                    unsafe {
                        let _ = ShowWindow(
                            hwnd,
                            if value.visibility.visible() {
                                SW_SHOWNOACTIVATE
                            } else {
                                SW_HIDE
                            },
                        );
                    }
                }
            }
        }
        unsafe {
            let mut message = MSG::default();
            for _ in 0..64 {
                if !PeekMessageW(&mut message, None, 0, 0, PM_REMOVE).as_bool() {
                    break;
                }
                let _ = TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if let Some(hwnd) = hwnd {
                let mut value = state.borrow_mut();
                value.tick = value.tick.wrapping_add(1);
                if !finished && value.visibility.visible() {
                    let mut cursor = POINT::default();
                    let mut client = RECT::default();
                    let _ = GetCursorPos(&mut cursor);
                    let _ = ScreenToClient(hwnd, &mut cursor);
                    let _ = GetClientRect(hwnd, &mut client);
                    let hover = value.over_close(client.right, cursor.x, cursor.y);
                    let changed = value.close_hover != hover;
                    value.close_hover = hover;
                    if changed || (value.progress.total == 0 && value.appearance.motion) {
                        let _ = InvalidateRect(Some(hwnd), None, false);
                    }
                }
            }
        }
    }
    if let Some(hwnd) = hwnd {
        unsafe {
            let _ = DestroyWindow(hwnd);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cosmetic_inbox_coalesces_floods_and_never_loses_completion() {
        let shared = Arc::new(Shared::default());
        let sender = Sender {
            shared: shared.clone(),
        };
        for completed in 0..50_000 {
            sender.send(Command::Update(Progress::new(
                ProgressPhase::Downloading,
                completed,
                50_000,
            )));
        }
        sender.send(Command::Visible(true));
        let inbox = shared.receive().unwrap();
        assert_eq!(inbox.progress.unwrap().completed, 49_999);
        assert_eq!(inbox.visible, Some(true));
        sender.send(Command::Finish);
        sender.send(Command::Visible(true));
        sender.send(Command::Update(Progress::new(
            ProgressPhase::Downloading,
            0,
            0,
        )));
        let inbox = shared.receive().unwrap();
        assert!(inbox.finished);
        assert!(inbox.progress.is_none() && inbox.visible.is_none());
    }

    #[test]
    fn ant_notification_layout_fits_dpi_theme_and_custom_typography() {
        assert_eq!(rgb(0x1677ff), COLORREF(0xff7716));
        for dpi in [96, 144, 192] {
            let state = RefCell::new(NativeState::new(
                ProgressTexts::default(),
                ProgressAppearance::default(),
            ));
            let hwnd = create(&state).unwrap();
            let dc = unsafe { GetDC(Some(hwnd)) };
            let mut value = state.borrow_mut();
            value.dpi = dpi;
            value.progress = Progress::new(ProgressPhase::BackingUp, 3, 8);
            assert!(value.status_text().ends_with("37%"));
            let width = scaled(360, dpi);
            assert!(value.content_height(dc, width) >= scaled(132, dpi));
            let close = value.close_bounds(width);
            assert_eq!(close.right - close.left, scaled(22, dpi));
            assert!(value.over_close(width, close.left, close.top));
            assert!(!value.over_close(width, close.right, close.top));
            assert!(!value.over_close(width, close.left, close.bottom));
            assert!(!value.over_close(width, 0, 0));
            let percent = value.percent_width(dc);
            value.appearance.font_size = 24;
            value.appearance.small_font_size = 20;
            value.appearance.font_family = "Courier New".to_owned();
            assert!(value.percent_width(dc) > percent);
            assert!(value.content_height(dc, width) > scaled(132, dpi));
            value.progress = Progress::new(ProgressPhase::Preparing, 0, 0);
            assert_eq!(value.status_text(), value.texts.preparing);
            assert_eq!(value.percent_width(dc), 0);
            drop(value);
            unsafe {
                let _ = ReleaseDC(Some(hwnd), dc);
                let _ = DestroyWindow(hwnd);
            }
        }
    }

    #[test]
    fn hidden_native_window_is_topmost_nonactivating_and_close_only_dismisses() {
        let state = RefCell::new(NativeState::new(
            ProgressTexts::default(),
            ProgressAppearance::default(),
        ));
        let hwnd = create(&state).expect("hidden progress fixture window");
        unsafe {
            let styles = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
            assert_eq!(
                styles & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE).0,
                (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE).0
            );
            assert!(!IsWindowVisible(hwnd).as_bool());
            SendMessageW(hwnd, WM_CLOSE, Some(WPARAM(0)), Some(LPARAM(0)));
            assert!(IsWindow(Some(hwnd)).as_bool());
            assert!(state.borrow().visibility.dismissed);
            assert_eq!(
                SendMessageW(hwnd, WM_MOUSEACTIVATE, None, None).0,
                MA_NOACTIVATE as isize
            );
            let _ = DestroyWindow(hwnd);
        }
    }

    // Opt-in image capture renders hidden fixture HWNDs into memory, without
    // displaying a window or capturing the user's desktop.
    #[test]
    fn hidden_native_preview() {
        let Some(directory) = std::env::var_os("SNOW_UPDATE_PROGRESS_PREVIEW") else {
            return;
        };
        let directory = std::path::PathBuf::from(directory);
        std::fs::create_dir_all(&directory).unwrap();
        for (locale, dpi, title, status) in [
            (
                "en_US",
                96,
                "Updating Snow Shot",
                "Backing up application files",
            ),
            ("zh_CN", 144, "正在更新 Snow Shot", "正在备份应用程序文件"),
            (
                "en_US-dark",
                144,
                "Updating Snow Shot",
                "Backing up application files",
            ),
            (
                "en_US-custom",
                96,
                "Updating Snow Shot",
                "Downloading update",
            ),
            (
                "zh_TW",
                192,
                "正在更新 Snow Shot",
                "正在檢查更新後的應用程式",
            ),
        ] {
            let texts = ProgressTexts {
                title: title.to_owned(),
                backing_up: status.to_owned(),
                files: match locale {
                    "zh_CN" => "%1 / %2 个文件",
                    "zh_TW" => "%1 / %2 個檔案",
                    _ => "%1 / %2 files",
                }
                .to_owned(),
                ..Default::default()
            };
            let appearance = if locale.ends_with("-dark") {
                ProgressAppearance {
                    background: 0x1f1f1f,
                    border: 0x303030,
                    text: 0xdddddd,
                    text_secondary: 0xb1b1b1,
                    text_tertiary: 0x848484,
                    fill_secondary: 0x3a3a3a,
                    fill_tertiary: 0x313131,
                    primary: 0x1668dc,
                    ..Default::default()
                }
            } else if locale.ends_with("-custom") {
                ProgressAppearance {
                    primary: 0x722ed1,
                    font_family: "Microsoft YaHei UI".to_owned(),
                    ..Default::default()
                }
            } else {
                ProgressAppearance::default()
            };
            let state = RefCell::new(NativeState::new(texts, appearance));
            let hwnd = create(&state).unwrap();
            let mut value = state.borrow_mut();
            value.dpi = dpi;
            value.progress = Progress::new(ProgressPhase::BackingUp, 3, 8);
            unsafe {
                let dc = GetDC(Some(hwnd));
                let width = scaled(360, dpi);
                let height = value.content_height(dc, width);
                let _ = SetWindowPos(
                    hwnd,
                    None,
                    0,
                    0,
                    width,
                    height,
                    SWP_NOACTIVATE | SWP_NOZORDER,
                );
                drop(value);
                let mut rect = RECT::default();
                let _ = GetClientRect(hwnd, &mut rect);
                let width = rect.right;
                let height = rect.bottom;
                let memory = CreateCompatibleDC(Some(dc));
                let bitmap = CreateCompatibleBitmap(dc, width, height);
                let previous = SelectObject(memory, HGDIOBJ(bitmap.0));
                SendMessageW(hwnd, WM_PRINTCLIENT, Some(WPARAM(memory.0 as usize)), None);
                let _ = SelectObject(memory, previous);
                let mut info = BITMAPINFO::default();
                info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB.0;
                let mut pixels = vec![0_u8; (width * height * 4) as usize];
                assert_eq!(
                    GetDIBits(
                        memory,
                        bitmap,
                        0,
                        height as u32,
                        Some(pixels.as_mut_ptr().cast()),
                        &mut info,
                        DIB_RGB_COLORS
                    ),
                    height
                );
                let mut bytes = b"BM".to_vec();
                bytes.extend_from_slice(&(54_u32 + pixels.len() as u32).to_le_bytes());
                bytes.extend_from_slice(&0_u32.to_le_bytes());
                bytes.extend_from_slice(&54_u32.to_le_bytes());
                bytes.extend_from_slice(&40_u32.to_le_bytes());
                bytes.extend_from_slice(&width.to_le_bytes());
                bytes.extend_from_slice(&(-height).to_le_bytes());
                bytes.extend_from_slice(&1_u16.to_le_bytes());
                bytes.extend_from_slice(&32_u16.to_le_bytes());
                bytes.extend_from_slice(&[0_u8; 24]);
                bytes.extend_from_slice(&pixels);
                std::fs::write(
                    directory.join(format!("update-progress-{locale}-{dpi}.bmp")),
                    bytes,
                )
                .unwrap();
                let _ = DeleteObject(HGDIOBJ(bitmap.0));
                let _ = DeleteDC(memory);
                let _ = ReleaseDC(Some(hwnd), dc);
                assert!(!IsWindowVisible(hwnd).as_bool());
                let _ = DestroyWindow(hwnd);
            }
        }
    }
}
