# snow-selected-text

Selected-text acquisition for translation and other explicitly invoked desktop actions. Rust 2024;
Apache-2.0; Windows 10/11 x64 and macOS 15+ on Apple Silicon or Intel are supported. Other
platforms compile and return `UnsupportedPlatform`. There is no translation service, global hook,
UI, permission prompt, or network access.

```rust,no_run
use snow_selected_text::{CaptureOptions, SelectedTextService, SelectionOutcome};

let service = SelectedTextService::new()?;
// Submit while the source application still owns foreground focus.
let request = service.start_capture(CaptureOptions::default())?;
// A GUI should poll request.try_result() on a timer, rather than wait().
let result = request.wait();
match result.as_ref() {
    Ok(SelectionOutcome::Selected(text)) => {
        // Send text.text to the host's translation service.
        // Inspect text.method and text.clipboard_status before presenting diagnostics.
    }
    Ok(SelectionOutcome::NoSelection) => {}
    Ok(SelectionOutcome::Unsupported) => {}
    Err(error) => { /* Map error.kind to localized host UI; retain its native code. */ }
}
# Ok::<(), snow_selected_text::SelectionError>(())
```

Run `cargo run -p snow-selected-text --example selected_text` from `snow-crates`. The example waits
five seconds for you to switch applications and select text. Add `--no-copy`, `--accessibility`,
`--native-control` (Windows only), or `--clipboard` to exercise a specific public strategy. Only
this explicit example prints captured text; the library has no payload logging.

## Contract and defaults

The default `CaptureStrategy::Auto` prefers platform accessibility, stops on a confirmed empty
selection, and uses guarded clipboard fallback only when the provider is unsupported or the macOS
provider reports a known transient failure. Explicit provider errors remain terminal on Windows.
The platform orders are:

1. Windows: UI Automation `TextPattern`, standard native `EDIT`, then guarded Ctrl+C.
2. macOS: Accessibility `AXSelectedTextRange(s)`/`AXStringForRange`, then guarded Command-C.

The Windows Accessibility provider checks the focused element and relevant ancestors, then performs
a bounded search for one document beneath a focused container. The native-control provider uses
system-marshalled `EM_GETSEL`, bounded `WM_GETTEXT`, and a second selection-offset check. The macOS
provider checks the frontmost process and focused AX element before and after reading, rejects
`AXSecureTextField`, retains noncontiguous range order, and retrieves optional `AXBoundsForRange`.

Set `CaptureOptions::strategy` to `CaptureStrategy::Accessibility`, `NativeControl`, or `Clipboard`
to attempt only that acquisition method. `NativeControl` is Windows-only. Explicit strategies never
fall back, including on unsupported, empty, or failed results. `copy_fallback` applies only to
`Auto`; selecting `Clipboard` explicitly enables Copy even when `copy_fallback` is false. Target
checks, exclusions, deadlines, text limits, permissions, and clipboard safeguards still apply.
Snow Shot uses Auto on macOS and retains clipboard-only capture on Windows.

```rust
use snow_selected_text::{CaptureOptions, CaptureStrategy};

let options = CaptureOptions {
    strategy: CaptureStrategy::Clipboard,
    ..Default::default()
};
```

Combined text joins nonempty ranges with one `\n`. Individual text is not trimmed or normalized;
whitespace, CRLF and embedded NULs from accessibility/native providers are preserved. Clipboard
text uses its first UTF-16 NUL terminator on Windows. Invalid UTF-16 is an error, never replacement
characters. Rectangles are optional screen coordinates: physical pixels on Windows and Accessibility
screen points on macOS. Unavailable geometry is not a text failure.

| Setting | Default / bound |
| --- | --- |
| Overall timeout | 2 seconds; configurable 1 ns–60 seconds in Rust, 1–60000 ms in C |
| Accessibility budget | At most 600 ms of the overall timeout |
| Native control budget | At most 150 ms of the remaining timeout |
| Windows clipboard open for snapshot | At most 200 ms of the remaining timeout |
| Cleanup reserve | Copy response polling stops 100 ms before the overall deadline |
| Text | 1 MiB UTF-8; configurable up to 64 MiB; oversized results fail without truncation |
| Windows Accessibility traversal | 16 ancestors; 128 descendants; eight descendant levels; at most one document |
| Selected ranges | 128; optional geometry at most 1024 rectangles per range |
| Whole native edit text | At most 32 Mi UTF-16 units |
| Clipboard snapshot | 256 representations, 64 MiB of copied data; at most 128 macOS items |
| Exclusions | Up to 1024 platform-native window/control IDs and 1024 executable basenames |

Submissions capture the foreground process identity synchronously; cross-process provider work runs
on a fixed worker. Windows also records HWND/native-focus identity. macOS exposes no stable public
native window/focus identifier here, so those optional source fields are `None` (zero through C).
Submit before opening or activating a translation popup. The library never forces foreground focus,
requests elevation, prompts for permission, or bypasses protected content. Selection capture is a
best-effort snapshot because neither platform provides one atomic focus/selection/clipboard
transaction.

## Windows clipboard behavior

Copy is blocked for recognized console window classes and these executable basenames (case
insensitive): WindowsTerminal, WindowsTerminalPreview, OpenConsole, conhost, powershell, pwsh,
cmd, wsl, bash, and mintty, each with `.exe`. Caller exclusions stop every acquisition method.
Terminals embedded in custom applications cannot always be recognized. The copy shortcut is fixed
to Ctrl+C; applications requiring another shortcut need a future explicit adapter.

The worker waits for Shift/Ctrl/Alt/Windows/C keys to be released, checks the original foreground
context again, and sends Copy once. Partial insertion is an error; only synthetic keys known to
have been pressed are released, and Copy is not retried. Windows UIPI can reject injection into
higher-integrity applications without reporting that UIPI was the reason.

A fresh clipboard sequence and an owner belonging to the captured process/window are required.
Unknown or unrelated owners produce `ClipboardAmbiguous`; the old clipboard is never returned as
selected text. Delayed rendering is handled while the clipboard is open. Clipboard content is
untrusted and parsed using allocation bounds. An empty copied string is an ambiguity error.

Before Copy, supported formats are duplicated in order. Standard HGLOBAL and registered formats
are copied as bounded byte allocations; bitmap, palette, metafile-picture and enhanced-metafile
handles use their corresponding Windows duplication/cleanup rules. Owner-display/private handle
formats, known OLE interface/storage transfer formats, and anything that cannot be duplicated mark
the snapshot incomplete. Delayed providers may
block a read. No partial snapshot is restored. A valid empty original clipboard is restorable.

After an attributable Copy, restoration checks the sequence again **with the clipboard open**.
It runs even when the new text is malformed or too large, and after cancellation if cleanup time
remains. A newer writer always wins. Restoration can fail after partially setting formats; this is
reported rather than hidden. Clipboard status is separate from text acquisition:

| Status | Meaning |
| --- | --- |
| `Unchanged` | Clipboard acquisition was not started |
| `Restored` | The complete original snapshot was restored |
| `PreservationIncomplete` | Copy succeeded but the original snapshot was incomplete; copied content remains |
| `Superseded` | A newer clipboard update was preserved |
| `RestorationFailed` | Cleanup could not complete within its deadline or a Windows restore operation failed |
| `Unknown` | Caller stopped waiting or input/read failed before an attributable result; clipboard effects may remain |

Restoring clipboard contents cannot undo clipboard history, cloud sync, or clipboard-manager effects.
A Copy result is explicitly labelled `RetrievalMethod::Clipboard`: without accessibility support,
the library cannot prove that an application's Copy handler copied a selection rather than a line.

## macOS Accessibility and pasteboard behavior

Capture requires the host to already be trusted for Accessibility. Clipboard fallback additionally
requires `CGPreflightPostEventAccess`. Missing permission returns `AccessDenied` before the general
pasteboard is read or changed; the crate deliberately provides no implicit prompt or prompt helper.

Before Command-C, the worker materializes ordered pasteboard items and types within the documented
limits. A complete snapshot is replaced by a request-unique private marker, allowing a no-op Copy to
be distinguished from stale text. After a targeted `CGEventPostToPid`, a changed pasteboard with the
marker removed is read as plain text. The original snapshot is restored only while that exact change
count remains current. A newer writer produces `Superseded` and is never overwritten.

If some original representation cannot be materialized, the worker does not install a marker or
clear the pasteboard before Copy. A successful result remains on the pasteboard and reports
`PreservationIncomplete`. macOS exposes no public pasteboard-owner identity; targeted input,
frontmost/focused target checks, marker state, and change counts make fallback conservative but
cannot make it atomic. Pasteboard privacy UI and clipboard-manager/history effects are controlled by
macOS and cannot be undone by restoration.

## Runtime and failure handling

All service handles share one process-wide runtime. Windows uses a windowless COM MTA worker and a
lazily created clipboard-window worker. macOS runs AX, CoreGraphics, and AppKit work inside
autorelease pools on the fixed worker. Native interfaces and clipboard objects do not escape to
caller threads; only owned data crosses channels.

One request runs at a time. Another submission returns `Busy`, including after a caller timeout
while a provider or clipboard renderer remains stuck. No replacement workers accumulate. When the
outstanding operation returns, expired results are discarded and the runtime can accept new work.
An unexpectedly terminated worker returns `WorkerUnavailable`; there is no automatic restart.
Dropping a service never joins workers. Dropping a request cancels it without waiting. Results are
immutable and independently reference-counted; completed results survive request cancellation.

`try_result()` returns `None` while pending; `wait()` stops at the request deadline. Cancellation and
deadlines are cooperative stage boundaries. They cannot kill a native call already executing or undo
input already submitted. If a request expires after clipboard work begins, its immutable error
may report `Unknown` even if cleanup subsequently succeeds. A permanently stuck provider requires
restarting the host process. A helper-process backend is the documented future option if automatic
recovery becomes a product requirement.

Errors carry a stable kind, operation name, optional HRESULT/Win32 or macOS AX error code, and
clipboard status.
No error includes selection text, clipboard payloads, or window titles. Host applications should
localize messages based on the kind; native operation names are diagnostics, not UI copy.

## Verification and compatibility record

Default tests use fake backends for fallback/clipboard policy, uniquely named macOS pasteboards, and
hidden native Windows controls. They do not activate another application or change the macOS general
pasteboard. Runtime tests deliberately block a provider, expire/cancel its caller, reject overlapping
work, and then release it to verify recovery without late fallback.

```powershell
cargo test -p snow-selected-text -p snow-selected-text-c
cargo fmt -p snow-selected-text -p snow-selected-text-c -- --check
cargo clippy -p snow-selected-text -p snow-selected-text-c --all-targets -- -D warnings
cargo check -p snow-selected-text -p snow-selected-text-c --target x86_64-pc-windows-msvc --all-targets
cargo check -p snow-selected-text -p snow-selected-text-c --target x86_64-apple-darwin --all-targets
```

The C wrapper has a standalone C/C++ ABI consumer in its `tests` directory. It can be linked to
either the wrapper archive or the `snow_rust_ffi` bundle without invoking text capture.

| Target | Verification / expectation |
| --- | --- |
| Hidden Win32 EDIT, including >65535 selection offsets | Automated native extraction, Unicode, caret, password, limits |
| Windows Accessibility EDIT provider | Automated selected text, Unicode, caret and password handling |
| Notepad, Edge/Chrome, Firefox, Word, PDF viewers, VS Code | Manual compatibility matrix still to be exercised with the example; provider support varies by app/version |
| Windows Terminal / console hosts | Deterministic Copy exclusion tests; accessibility retrieval remains permitted |
| Elevated apps / secure desktops | No bypass; access/injection failures are explicit |
| Native macOS pasteboard | Automated multi-item/type snapshot, marker, restoration, and superseding-writer tests |
| TextEdit, Safari/Chrome/Firefox, Preview, VS Code on macOS | Manual compatibility matrix; AX and Copy behavior varies by app/version |
| macOS secure fields / missing Accessibility permission | No bypass or prompt; capture fails before pasteboard mutation |
| Remote desktops, screenshots, custom canvases | Usually unsupported without Copy; OCR is a separate host feature |

Real foreground/clipboard integration tests are ignored by default, must be explicitly selected,
and must run with one test thread. Do not run a full workspace suite for this crate.

```text
cargo test -p snow-selected-text platform::windows::clipboard::tests::foreground_copy_restores_existing_clipboard -- --ignored --exact --test-threads=1
cargo test -p snow-selected-text platform::macos::clipboard::tests::foreground_copy_uses_the_general_pasteboard -- --ignored --exact --test-threads=1
```

The Windows foreground fixture requires foreground activation. The macOS fixture pauses for five
seconds so a developer can focus an application and select text, and it requires existing AX and
event-posting authorization. Run either explicit test from an interactive developer console.
Clipboard policy and native data duplication are covered by the passing default tests, but those do
not establish compatibility with external Copy handlers.

The pre-1.0 Rust/C source vocabulary changed from Windows-specific `Uia`, `NativeEdit`, `window`,
and `focused_control` names to `Accessibility`, `NativeControl`, `native_window`, and `native_focus`.
Numeric strategy/method values, C structure layout, and ABI version 1 remain unchanged. No persistent
settings, payload telemetry, or deployment behavior are introduced.
