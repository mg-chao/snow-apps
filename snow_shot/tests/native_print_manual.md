# Native printing validation

Build `snow-shot-native-print-smoke` and `snow_shot` in the matching debug preset.
The smoke fixture opens no print UI until its Print button is clicked. Its image
contains transparency, a border, text, and an orientation marker. Dialog submission
can create a real printer job; use a PDF printer for these checks.

## Windows

Snow Shot uses the modern print UI on Windows 11 and falls back to the Windows
Photo Printing Wizard (the native Print Pictures dialog) on older Windows versions
or when the modern interface is unavailable. The legacy backend directly activates
Microsoft's `CLSID_PrintPhotosDropTarget` and passes a lossless temporary PNG as a
Shell data object. It does not depend on the user's default image application. The temporary
PNG stays alive until Windows releases its data object, including any references
retained beyond the dialog's closure. Application shutdown also removes retained
snapshots. Its native host remains above a topmost
capture without changing the capture's flags. The wizard exposes no HWND owner
interface; closing the originating window requests cancellation through WM_CLOSE.

The Photo Printing Wizard does not expose a print submission/cancellation callback.
When the native dialog closes, the service reports `HandedOff` and restores the
capture or pin. It never closes a capture based only on a successful Shell handoff.
Activation and handoff failures still report an error, and closing the owner closes
the wizard without deleting its snapshot prematurely.
The smoke fixture defaults to the same automatic backend selection as the
application; use `--legacy` to exercise the fallback dialog directly.

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
- Run `ctest --preset test-windows-msvc-debug -R
  '^snow-shot-windows-print-dialog-tests$' --output-on-failure` for the offscreen
  wizard regression. It verifies the documented wizard CLSID and copy-only handoff,
  immutable full-size PNG pixels and sRGB metadata, activation and handoff failures,
  cancellation, deferred/worker-thread release, native window hide events with
  retained Shell references, owner destruction, prevention of duplicate dialogs,
  exactly-once completion and temporary-file cleanup.
- On a Windows desktop with an installed printer, run
  `snow-shot-windows-print-dialog-tests.exe --native-cancel-only` to check that
  the real Print Pictures dialog opens above its topmost originating window.
  The probe closes the wizard without pressing Print, then repeats by destroying
  the owner. Neither request creates a printer job.
- Run with `--legacy` to exercise Print Pictures. Confirm the image preview,
  printer, paper size, quality, photo layouts, copies and Fit picture to frame
  controls match Windows' native dialog. Repeat paper, layout, PDF, cancellation,
  and reopen checks. Both printing and cancellation must report Photo dialog closed
  in the fixture. Uncheck Fit picture to frame to preserve the full border; selecting
  photo layouts may crop the image according to the user's choice. On an older
  compatible Windows installation, confirm the default backend selects this path.
  An initialization failure must report an error without reopening another interface.
- `--classic` remains an alias of `--legacy` in the Windows fixture and uses the
  same Photo Printing Wizard fallback interface as the application.
- Run with `--modern-unavailable`. Confirm exactly one legacy dialog opens and
  closing it reports Photo dialog closed. Confirm initialization
  failures in modern printing also take this path, while failures after a task
  starts show an error without reopening another dialog.
- With a real screenshot, draw annotations, apply result styling, and print.
  The modern backend closes the capture only after confirmed submission. The Photo
  Printing Wizard restores focus and editing after it closes, including after a
  print, because it does not report submission. Print a trimmed scrolling capture
  and verify the trimmed result.
- Pin an image. Keep the drawing toolbar hidden and press Ctrl+P. Confirm it
  stays hidden and the pin stays open. Repeat with rotation, zoom, opacity, and
  thumbnail mode; no controls or resize handles may appear in the output.
- Show the pinned toolbar, move it manually, and invoke Print or Ctrl+P. Confirm
  it stays visible in the same position during preparation, while the native
  dialog is open, and after closing it. Input must be blocked until completion.
  Repeat with OCR: the comparison preview must hide during printing and return afterward.
- Print scrolled OCR, translation, table, QR, and formatted text results. Confirm
  only the visible content viewport prints, without selections or caret. Rebind
  Print and confirm live updates. Editing text and other modal interactions must
  suppress the shortcut. Closing/replacing the owner during preparation/native UI
  must release the job and restore interaction without stale callbacks.

## macOS

Run the deterministic print-service and native-panel ownership checks with
`ctest --preset test-snow-shot-macos-arm64-debug -R
'^snow-shot-(print-service|macos-native-print-stacking|macos-file-panel-stacking)-tests$'
--output-on-failure`. Use the matching x64 preset on Intel.

Build `snow-shot-macos-native-print-stacking-tests` and run
`ctest --test-dir build/snow-shot-macos-arm64-debug -R
'^snow-shot-macos-native-print-stacking-native-tests$' --output-on-failure`
for an automated AppKit regression. It opens and cancels the real print panel
twice from both a screenshot and a topmost pin, with floating tools visible.
It checks that the sheet remains above the toolbar and popups after a toolbar
raise, preserves capture levels and native handles, and releases its override
after cancellation. It never submits a printer job.

Run the smoke fixture on macOS 15+ and repeat paper/orientation, PDF, cancellation,
reopen, stacking, screenshot, pinned viewport, and recognition checks above. Confirm
the native AppKit print panel belongs to the originating window, fits one page,
and submission/cancellation preserve the corresponding session behavior.

These interactive checks are separate from the deterministic offscreen tests.
