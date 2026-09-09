use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::clipboard::*;
use crate::model::*;
use crate::policy::*;
use crate::runtime::RequestState;

fn source() -> SourceWindow {
    SourceWindow {
        window: 1,
        focused_control: 2,
        process_id: 3,
        executable: "editor.exe".into(),
    }
}

fn context() -> Context {
    Context {
        source: source(),
        options: CaptureOptions::default(),
        state: Arc::new(RequestState::new(Instant::now() + Duration::from_secs(10))),
    }
}

fn range(text: &str) -> SelectedRange {
    SelectedRange {
        text: text.into(),
        bounds: Vec::new(),
    }
}

struct FakeBackend {
    probes: std::collections::VecDeque<Result<Probe, SelectionError>>,
    calls: Vec<&'static str>,
    changed_at: Option<usize>,
}

impl Backend for FakeBackend {
    fn validate_target(&mut self, _: &Context) -> Result<(), SelectionError> {
        self.calls.push("validate");
        if self.changed_at == Some(self.calls.len()) {
            Err(SelectionError::new(ErrorKind::TargetChanged, "fake focus"))
        } else {
            Ok(())
        }
    }
    fn uia(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
        self.calls.push("uia");
        self.probes.pop_front().unwrap()
    }
    fn native(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
        self.calls.push("native");
        self.probes.pop_front().unwrap()
    }
    fn copy(&mut self, context: &Context) -> CaptureResult {
        self.calls.push("copy");
        assemble(
            vec![range("copied")],
            context.source.clone(),
            RetrievalMethod::Clipboard,
            ClipboardStatus::Restored,
            context.options.max_text_bytes,
        )
    }
}

fn backend(probes: Vec<Result<Probe, SelectionError>>) -> FakeBackend {
    FakeBackend {
        probes: probes.into(),
        calls: Vec::new(),
        changed_at: None,
    }
}

#[test]
fn explicit_provider_strategies_never_attempt_other_methods() {
    for (strategy, method, call) in [
        (CaptureStrategy::Uia, RetrievalMethod::Uia, "uia"),
        (
            CaptureStrategy::NativeEdit,
            RetrievalMethod::NativeEdit,
            "native",
        ),
    ] {
        let mut context = context();
        context.options.strategy = strategy;
        for probe in [
            Probe::Selected(vec![range("selected")]),
            Probe::Empty,
            Probe::Unsupported,
        ] {
            let expected = match &probe {
                Probe::Selected(ranges) => assemble(
                    ranges.clone(),
                    source(),
                    method,
                    ClipboardStatus::Unchanged,
                    context.options.max_text_bytes,
                ),
                Probe::Empty => Ok(SelectionOutcome::NoSelection),
                Probe::Unsupported => Ok(SelectionOutcome::Unsupported),
            };
            let mut backend = backend(vec![Ok(probe)]);
            assert_eq!(acquire(&mut backend, &context), expected);
            assert_eq!(backend.calls, ["validate", call, "validate"]);
        }
        let error = SelectionError::new(ErrorKind::NativeApi, "fake provider");
        let mut backend = backend(vec![Err(error.clone())]);
        assert_eq!(acquire(&mut backend, &context), Err(error));
        assert_eq!(backend.calls, ["validate", call]);
    }
}

#[test]
fn clipboard_strategy_skips_providers_regardless_of_auto_fallback_flag() {
    for copy_fallback in [false, true] {
        let mut context = context();
        context.options.strategy = CaptureStrategy::Clipboard;
        context.options.copy_fallback = copy_fallback;
        // No probes are available: invoking UIA or native extraction would panic.
        let mut backend = backend(Vec::new());
        let SelectionOutcome::Selected(text) = acquire(&mut backend, &context).unwrap() else {
            panic!()
        };
        assert_eq!(text.text, "copied");
        assert_eq!(text.method, RetrievalMethod::Clipboard);
        assert_eq!(text.clipboard_status, ClipboardStatus::Restored);
        assert_eq!(backend.calls, ["validate", "validate", "copy"]);
    }
}

#[test]
fn clipboard_strategy_preserves_target_deadline_and_error_handling() {
    let mut context = context();
    context.options.strategy = CaptureStrategy::Clipboard;
    for changed_at in [1, 2] {
        let mut backend = backend(Vec::new());
        backend.changed_at = Some(changed_at);
        assert_eq!(
            acquire(&mut backend, &context).unwrap_err().kind,
            ErrorKind::TargetChanged
        );
        assert!(!backend.calls.contains(&"copy"));
    }
    context.options.max_text_bytes = 1;
    let mut failed = backend(Vec::new());
    let error = acquire(&mut failed, &context).unwrap_err();
    assert_eq!(error.kind, ErrorKind::LimitExceeded);
    assert_eq!(error.clipboard_status, ClipboardStatus::Restored);
    assert_eq!(failed.calls, ["validate", "validate", "copy"]);
    context.state = Arc::new(RequestState::new(Instant::now()));
    let mut expired = backend(Vec::new());
    assert_eq!(
        acquire(&mut expired, &context).unwrap_err().kind,
        ErrorKind::TimedOut
    );
    assert!(expired.calls.is_empty());
}

#[test]
fn uia_success_preserves_unicode_whitespace_and_range_order_without_copy() {
    let mut backend = backend(vec![Ok(Probe::Selected(vec![
        range("你好😀\r\n"),
        range(""),
        range("  "),
    ]))]);
    let result = acquire(&mut backend, &context()).unwrap();
    let SelectionOutcome::Selected(text) = result else {
        panic!()
    };
    assert_eq!(text.text, "你好😀\r\n\n  ");
    assert_eq!(text.ranges.len(), 2);
    assert_eq!(text.method, RetrievalMethod::Uia);
    assert_eq!(text.clipboard_status, ClipboardStatus::Unchanged);
    assert_eq!(backend.calls, ["validate", "uia", "validate"]);
}

#[test]
fn confirmed_empty_selection_stops_every_fallback() {
    for probes in [
        vec![Ok(Probe::Empty)],
        vec![Ok(Probe::Unsupported), Ok(Probe::Empty)],
    ] {
        let mut backend = backend(probes);
        assert_eq!(
            acquire(&mut backend, &context()),
            Ok(SelectionOutcome::NoSelection)
        );
        assert!(!backend.calls.contains(&"copy"));
    }
}

#[test]
fn native_selection_precedes_default_copy_and_disabled_copy_returns_unsupported() {
    let mut native = backend(vec![
        Ok(Probe::Unsupported),
        Ok(Probe::Selected(vec![range("native")])),
    ]);
    let SelectionOutcome::Selected(text) = acquire(&mut native, &context()).unwrap() else {
        panic!()
    };
    assert_eq!(text.method, RetrievalMethod::NativeEdit);
    let mut copy = backend(vec![Ok(Probe::Unsupported), Ok(Probe::Unsupported)]);
    let SelectionOutcome::Selected(text) = acquire(&mut copy, &context()).unwrap() else {
        panic!()
    };
    assert_eq!(text.method, RetrievalMethod::Clipboard);
    let mut context = context();
    context.options.copy_fallback = false;
    let mut disabled = backend(vec![Ok(Probe::Unsupported), Ok(Probe::Unsupported)]);
    assert_eq!(
        acquire(&mut disabled, &context),
        Ok(SelectionOutcome::Unsupported)
    );
    assert!(!disabled.calls.contains(&"copy"));
}

#[test]
fn protected_denied_stale_failed_and_timed_out_providers_never_inject_copy() {
    for kind in [
        ErrorKind::ProtectedContent,
        ErrorKind::AccessDenied,
        ErrorKind::TargetChanged,
        ErrorKind::TimedOut,
        ErrorKind::NativeApi,
        ErrorKind::TargetUnavailable,
        ErrorKind::LimitExceeded,
    ] {
        let expected = SelectionError {
            kind,
            operation: "provider operation",
            native_code: Some(-42),
            clipboard_status: ClipboardStatus::Unchanged,
        };
        let mut backend = backend(vec![Err(expected.clone())]);
        assert_eq!(acquire(&mut backend, &context()), Err(expected));
        assert_eq!(backend.calls, ["validate", "uia"]);
    }
}

#[test]
fn target_changes_discard_results_and_stop_later_stages() {
    for changed_at in [1, 3, 5, 6] {
        let mut backend = backend(vec![Ok(Probe::Unsupported), Ok(Probe::Unsupported)]);
        backend.changed_at = Some(changed_at);
        assert_eq!(
            acquire(&mut backend, &context()).unwrap_err().kind,
            ErrorKind::TargetChanged
        );
        assert!(!backend.calls.contains(&"copy"));
    }
}

#[test]
fn expired_request_does_not_touch_any_backend() {
    let mut context = context();
    context.state = Arc::new(RequestState::new(Instant::now()));
    let mut backend = backend(Vec::new());
    assert_eq!(
        acquire(&mut backend, &context).unwrap_err().kind,
        ErrorKind::TimedOut
    );
    assert!(backend.calls.is_empty());
}

#[test]
fn limits_include_range_separators_and_do_not_truncate() {
    assert!(
        assemble(
            vec![range("ab"), range("cd")],
            source(),
            RetrievalMethod::Uia,
            ClipboardStatus::Unchanged,
            5
        )
        .is_ok()
    );
    assert_eq!(
        assemble(
            vec![range("ab"), range("cd")],
            source(),
            RetrievalMethod::Uia,
            ClipboardStatus::Unchanged,
            4
        )
        .unwrap_err()
        .kind,
        ErrorKind::LimitExceeded
    );
    assert_eq!(
        assemble(
            vec![range("x"); 129],
            source(),
            RetrievalMethod::Uia,
            ClipboardStatus::Unchanged,
            1000
        )
        .unwrap_err()
        .kind,
        ErrorKind::LimitExceeded
    );
}

#[test]
fn clipboard_parser_requires_valid_terminated_utf16_with_bounded_utf8() {
    let bytes: Vec<_> = "中文😀\0"
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect();
    assert_eq!(clipboard_utf16(&bytes, 10).unwrap(), "中文😀");
    assert_eq!(
        clipboard_utf16(&bytes, 9).unwrap_err().kind,
        ErrorKind::LimitExceeded
    );
    for malformed in [vec![0], vec![65, 0], vec![0, 0xD8, 0, 0]] {
        assert_eq!(
            clipboard_utf16(&malformed, 100).unwrap_err().kind,
            ErrorKind::MalformedData
        );
    }
    assert_eq!(clipboard_utf16(&[65, 0, 0, 0, 42, 42], 1).unwrap(), "A");
    assert_eq!(clipboard_utf16(&[0, 0], 1).unwrap(), "");
}

struct FakeClipboard {
    calls: Vec<&'static str>,
    complete: bool,
    current_sequence: u32,
    text: Result<String, SelectionError>,
    prepare_error: Option<ErrorKind>,
    inject_error: bool,
    read_error: Option<ErrorKind>,
    restore_fails: bool,
}

impl Default for FakeClipboard {
    fn default() -> Self {
        Self {
            calls: Vec::new(),
            complete: true,
            current_sequence: 11,
            text: Ok("new text".into()),
            prepare_error: None,
            inject_error: false,
            read_error: None,
            restore_fails: false,
        }
    }
}

impl ClipboardIo for FakeClipboard {
    type Saved = String;
    fn snapshot(&mut self, _: &Context) -> Result<Snapshot<String>, SelectionError> {
        self.calls.push("snapshot");
        Ok(Snapshot {
            data: "old clipboard".into(),
            sequence: 10,
            complete: self.complete,
        })
    }
    fn prepare_copy(&mut self, _: &Context, sequence: u32) -> Result<(), SelectionError> {
        assert_eq!(sequence, 10);
        self.calls.push("prepare");
        self.prepare_error
            .map_or(Ok(()), |kind| Err(SelectionError::new(kind, "prepare")))
    }
    fn inject(&mut self) -> Result<(), SelectionError> {
        self.calls.push("inject");
        if self.inject_error {
            Err(SelectionError::new(
                ErrorKind::InputInjectionFailed,
                "injection",
            ))
        } else {
            Ok(())
        }
    }
    fn read_copy(&mut self, _: &Context, old: u32) -> Result<CopiedText, SelectionError> {
        assert_eq!(old, 10);
        self.calls.push("read");
        if let Some(kind) = self.read_error {
            return Err(SelectionError::new(kind, "read"));
        }
        Ok(CopiedText {
            sequence: 11,
            text: self.text.clone(),
        })
    }
    fn restore(
        &mut self,
        _: &Context,
        snapshot: Snapshot<String>,
        sequence: u32,
    ) -> ClipboardStatus {
        self.calls.push("restore");
        assert_eq!(snapshot.data, "old clipboard");
        restoration_decision(snapshot.complete, sequence, self.current_sequence).unwrap_or(
            if self.restore_fails {
                ClipboardStatus::RestorationFailed
            } else {
                ClipboardStatus::Restored
            },
        )
    }
}

#[test]
fn clipboard_transaction_reports_restoration_without_losing_selected_text() {
    for (complete, current, failed, status) in [
        (true, 11, false, ClipboardStatus::Restored),
        (false, 11, false, ClipboardStatus::PreservationIncomplete),
        (true, 12, false, ClipboardStatus::Superseded),
        (false, 12, false, ClipboardStatus::Superseded),
        (true, 11, true, ClipboardStatus::RestorationFailed),
    ] {
        let mut io = FakeClipboard {
            complete,
            current_sequence: current,
            restore_fails: failed,
            ..Default::default()
        };
        let SelectionOutcome::Selected(text) = transaction(&mut io, &context()).unwrap() else {
            panic!()
        };
        assert_eq!(text.text, "new text");
        assert_eq!(text.clipboard_status, status);
        assert_eq!(
            io.calls,
            ["snapshot", "prepare", "inject", "read", "restore"]
        );
    }
}

#[test]
fn restoration_still_runs_for_attributable_malformed_or_oversized_copy() {
    for kind in [ErrorKind::MalformedData, ErrorKind::LimitExceeded] {
        let mut io = FakeClipboard {
            text: Err(SelectionError::new(kind, "parse")),
            ..Default::default()
        };
        let error = transaction(&mut io, &context()).unwrap_err();
        assert_eq!(error.kind, kind);
        assert_eq!(error.clipboard_status, ClipboardStatus::Restored);
        assert_eq!(io.calls.last(), Some(&"restore"));
    }
}

#[test]
fn empty_copied_text_is_ambiguous_and_retains_cleanup_status() {
    let mut io = FakeClipboard {
        text: Ok(String::new()),
        ..Default::default()
    };
    let error = transaction(&mut io, &context()).unwrap_err();
    assert_eq!(error.kind, ErrorKind::ClipboardAmbiguous);
    assert_eq!(error.clipboard_status, ClipboardStatus::Restored);
}

#[test]
fn no_input_after_failed_safeguard_or_changed_clipboard() {
    for kind in [
        ErrorKind::CopyBlocked,
        ErrorKind::TargetChanged,
        ErrorKind::TimedOut,
        ErrorKind::ClipboardAmbiguous,
        ErrorKind::ProtectedContent,
    ] {
        let mut io = FakeClipboard {
            prepare_error: Some(kind),
            ..Default::default()
        };
        let error = transaction(&mut io, &context()).unwrap_err();
        assert_eq!(error.kind, kind);
        assert_eq!(error.clipboard_status, ClipboardStatus::Unchanged);
        assert_eq!(io.calls, ["snapshot", "prepare"]);
    }
}

#[test]
fn partial_injection_never_retries_and_unattributable_updates_never_restore() {
    let mut io = FakeClipboard {
        inject_error: true,
        ..Default::default()
    };
    let error = transaction(&mut io, &context()).unwrap_err();
    assert_eq!(error.kind, ErrorKind::InputInjectionFailed);
    assert_eq!(io.calls, ["snapshot", "prepare", "inject"]);
    for kind in [
        ErrorKind::TimedOut,
        ErrorKind::ClipboardAmbiguous,
        ErrorKind::ClipboardBusy,
    ] {
        let mut io = FakeClipboard {
            read_error: Some(kind),
            ..Default::default()
        };
        let error = transaction(&mut io, &context()).unwrap_err();
        assert_eq!(error.kind, kind);
        assert_eq!(error.clipboard_status, ClipboardStatus::Unknown);
        assert!(!io.calls.contains(&"restore"));
    }
}

#[test]
fn default_terminal_exclusions_are_case_insensitive() {
    for name in [
        "cmd.exe",
        "PWSH.EXE",
        "WindowsTerminal.exe",
        "mintty.exe",
        "wsl.exe",
        "conhost.exe",
    ] {
        assert!(terminal_executable(name));
    }
    assert!(!terminal_executable("notepad.exe"));
    assert!(!terminal_executable("my-cmd.exe"));
}

#[test]
fn options_reject_invalid_limits_and_paths_but_default_to_copy() {
    assert_eq!(CaptureOptions::default().strategy, CaptureStrategy::Auto);
    assert!(CaptureOptions::default().copy_fallback);
    for value in [0, 64 * 1024 * 1024 + 1] {
        assert!(
            CaptureOptions {
                max_text_bytes: value,
                ..Default::default()
            }
            .validate()
            .is_err()
        );
    }
    for value in [Duration::ZERO, Duration::from_secs(61)] {
        assert!(
            CaptureOptions {
                timeout: value,
                ..Default::default()
            }
            .validate()
            .is_err()
        );
    }
    for name in ["", "a/b.exe", "a\\b.exe", "a\0.exe"] {
        assert!(
            CaptureOptions {
                excluded_executables: vec![name.into()],
                ..Default::default()
            }
            .validate()
            .is_err()
        );
    }
}
