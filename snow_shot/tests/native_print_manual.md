# Native printing validation

Build `snow-shot-native-print-smoke` and `snow_shot` in the matching debug preset.
The smoke fixture opens no print UI until its Print button is clicked. Its image
contains transparency, a border, text, and an orientation marker. Dialog submission
can create a real printer job; use a PDF printer for these checks.

## Windows

Snow Shot temporarily defaults to PrintDlgExW/GDI for compatibility testing on all
Windows versions. The smoke fixture still defaults to the automatic backend;
use `--legacy` to match the application's current print dialog.

- Run `ctest --preset test-windows-msvc-debug -R
  '^snow-shot-windows-print-document-tests$' --output-on-failure` for the headless
  native document regression. It verifies pagination does not invalidate itself,
  application-defined and repeated page requests deliver rendered DXGI pixels,
  and orientation changes update the preview layout without opening a dialog.
- Run `snow-shot-native-print-smoke.exe`. Windows 11 must show the modern print UI.
  The print panel must be visible above the topmost fixture without lowering it.
  Verify the preview fits one page. Change paper and orientation and check centering.
  Print to Microsoft Print to PDF; verify a single page with white transparency,
  correct colors, and the full border. The fixture must report Submitted.
- Cancel and reopen. The fixture must report Cancelled and must not open a legacy
  dialog after cancellation. The status must also report Topmost preserved: the
  fixture monitors native events to detect topmost changes throughout printing.
  Repeat with the real capture's floating toolbar visible and another application's
  window behind the capture. After cancellation, bring that application forward;
  both the screenshot overlay and its toolbar must remain above it. Repeat on each
  capture monitor and with a pinned image's drawing toolbar visible.
- Run with `--legacy` to exercise PrintDlgExW/GDI. Repeat paper, orientation, PDF,
  cancellation, and reopen checks. On an older compatible Windows installation,
  confirm the default backend selects this path. A common-dialog initialization
  failure must attempt PrintDlgW; cancellation must not reopen either interface.
- Run with `--classic` to force the PrintDlgW compatibility interface. Repeat PDF,
  orientation, cancellation, and reopen checks. This switch belongs to the fixture;
  the application currently defaults to PrintDlgExW/GDI on Windows.
- Run with `--modern-unavailable`. Confirm exactly one legacy dialog opens and
  submitting/cancelling reports the corresponding result. Confirm initialization
  failures in modern printing also take this path, while failures after a task
  starts show an error without reopening another dialog.
- With a real screenshot, draw annotations, apply result styling, and print.
  Only successful submission should close the capture; cancel/failure must restore
  focus and editing. Print a trimmed scrolling capture and verify the trimmed result.
- Pin an image. Keep the drawing toolbar hidden and press Ctrl+P. Confirm it
  stays hidden and the pin stays open. Repeat with rotation, zoom, opacity, and
  thumbnail mode; no controls or resize handles may appear in the output.
- Print scrolled OCR, translation, table, QR, and formatted text results. Confirm
  only the visible content viewport prints, without selections or caret. Rebind
  Print and confirm live updates. Editing text and other modal interactions must
  suppress the shortcut. Closing/replacing the owner during preparation/native UI
  must release the job and restore interaction without stale callbacks.

## macOS

Run the smoke fixture on macOS 15+ and repeat paper/orientation, PDF, cancellation,
reopen, stacking, screenshot, pinned viewport, and recognition checks above. Confirm
the native AppKit print panel belongs to the originating window, fits one page,
and submission/cancellation preserve the corresponding session behavior.

These interactive checks are separate from the deterministic offscreen tests.
