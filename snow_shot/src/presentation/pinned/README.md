# Pinned image presentation

`ScreenshotPinnedWindow` coordinates pin state and editing. `PinnedWindowHost` owns its event
surface and auxiliary widgets. On Windows desktop the image surface is a `QWindow`, without a
`QBackingStore`; macOS and offscreen fixtures use a QWidget adapter. Native presentation errors
do not select a different renderer.

## Coordinate ownership

The HWND client rectangle is authoritative on Windows. Selection, placement, resize limits,
animation, and saved geometry use physical pixels. QWindow geometry is an observation used at Qt
UI boundaries; it must not be rounded and written back as the image rectangle during painting.

`SnowCanvasView` receives the exact physical size and DPR. Its fractional logical extent is
`physicalSize / DPR`. The engine viewport remains ceiling-sized because the Rust viewport ABI
uses integer dimensions. `updateCanvasViewport()` offsets the camera by the surplus logical
extent divided by twice the zoom. At 100% this cancels viewport rounding: the final
canvas-to-device transform has unit scale and an integer origin. Rendering, hit testing, OCR,
and color sampling use that transform or its inverse.

An established pin accepts Windows' suggested DPI-transition rectangle. Once it settles, the
physical image size determines the displayed scale. Initial placement remains at the requested
selection rectangle. Programmatic zoom keeps its requested percentage when the exact requested
physical size settles; it does not use a native rounding tolerance.

## Frame publication

`PinnedImagePresenter` owns a reusable surface through an injectable backend. The Windows
backend creates a top-down premultiplied BGRA DIB and exposes its memory as a QImage. QPainter
writes directly into that memory and `UpdateLayeredWindowIndirect` publishes it.

Dirty invalidations are coalesced on the GUI thread. Position-only moves use `SetWindowPos`
without rendering; successful system moves also update the presenter's committed position.
The first complete frame is published before the native pin becomes visible, including when
image materialization is asynchronous.

Allocation and publication failures do not advance committed presenter state. USER32 retains
the previous bitmap; the host restores geometry, surface metrics, and opacity as needed. A
failed paint invalidates the working buffer so the next publication repaints it completely.
Queued invalidations have the host as their QObject context and cannot outlive it.

## Input and auxiliary windows

Both hosts forward input to `SnowCanvasView`, which owns canvas state, rendering, text sessions,
cursor decisions, and invalidation. `SnowCanvasWidget` retains its public API and forwards its
signals. The native window exposes the shared view as its focus object and routes input-method
events and queries to it.

Image-aligned content, including OCR selection and formatted text, is painted into the native
frame. Formatted-text keyboard selection is delivered to its existing graphics text item;
its hidden QWidget does not need activation. Buttons, toolbars, menus, and rich editors remain
Qt widgets with explicit transient ownership. Independent toolbars retain their own opacity.

During USER32's modal move loop, keyboard translation is intercepted at the application event
boundary, before shortcut scopes. Matching QWindow events continue through normal dispatch
once; QWidget hosts receive the translated event through their adapter. Unmatched keys remain
owned by the native move loop.

## Focused validation

- `snow-shot-windows-pinned-pixels-*`: real Windows hosts at five scaling factors, odd/even
  dimensions, first-show geometry, repeated painting, and drawing-mode transitions.
- `snow-shot-pinned-image-presenter-tests` and its `-native-tests` variant: source-pixel and
  alpha patterns, injected failures, rollback, position-only moves, native resources,
  queued-update disposal, and QWindow text-input/query routing.
- `snow-shot-pinned-native-drag-dpi-tests`: real monitor transitions; returns CTest's skip code
  when the required monitor arrangement is unavailable.
- Canvas input/rendering, recognition, color-sampling, shortcut, restoration, and pinned-mode
  tests cover the shared view and QWidget adapter. `--native-frame-only` on the recognition
  test executable isolates formatted scene rendering and keyboard selection.

Synthetic composition events verify the Qt bridge. Interactive Windows IME candidate placement
and popup focus restoration still need verification with an installed IME; synthetic events
alone do not establish those OS-level behaviors.
