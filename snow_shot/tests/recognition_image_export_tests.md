# Recognition image export

## Behavior contract

The reviewed feature plan applies to screenshot selection and pinned windows. Priorities are
visual correctness and responsiveness, followed by maintainability and throughput.

- R1: Function settings places Text Recognition immediately after Pin to screen. Its
  `text_recognition/save_recognition_result_as_image` switch defaults to true, applies to open
  windows at the next save, and has a reset independent of OCR appearance/model settings.
- R2: Quick Save and both Save as File dialogs use recognition pixels when the active text
  recognition session displays the original image. Original-image translation is included;
  text editors, text-only translation, inactive recognition, and other recognition modes are excluded.
- R3: Save captures the displayed result before modal event processing or asynchronous work.
  Partial translation is captured immediately. Pending or empty OCR saves the current image.
  Later edits, mode changes, and setting changes do not mutate an already prepared artifact.
- R4: Output uses native image dimensions, transformed text/background geometry, and existing
  result effects. It excludes selection/caret/toolbar chrome and annotations hidden by recognition.
  Clipboard, automatic saves after copying, capture history eligibility, and pin persistence retain
  their existing behavior.
- R5: Rendering and encoding stay on export workers. Snapshots contain Qt value types with
  copy-on-write ownership and no widget/session references. Preview and encoding share the artifact.
  Cancellation discards partial output; ordinary save errors remain visible without substituting
  the original image for a failed recognition render.

## Implementation boundaries

`ScreenshotOcrTextLayout` owns shaping and painting, with explicit font/color inputs. The live
QGraphicsItem and recognition exporter use the same layout and quad fitting. Selection painting
is an explicit input and is empty for export. The snapshot retains the background patch actually
shown, including its canvas coordinates; saving never starts recognition or background filtering.

Merged horizontal paragraphs share source-row fitting between the canvas and export. Valid OCR
rows retain their indentation, gaps, and shorter final row with bounded spacing and uniform glyph
scaling. Overlapping rows or translations that cannot fit readably use ordinary paragraph layout.
The canvas keeps selection and grapheme-safe hit testing; export renders without selection.

Screenshot selection captures through its OCR controller. Pinned saving uses the transformed
recognition window and canvas background. Both use the session's original-image visibility rule.
The configuration schema supplies the default for existing installations without migrating data.
Disabling the setting restores the previous image-save path.

## Focused verification

| Rules | Coverage |
| --- | --- |
| R1 | settings catalog order/default/binding; translation settings backend persistence and isolated reset |
| R2–R3 | recognition session visibility; offscreen recognition window snapshot excludes editor/QR pages and owns copied values |
| R2–R4 | offscreen pinned recognition save integration: enabled/disabled, OCR/translation/editor, Quick Save, both dialogs, dialog-time changes, zoom, rotated paragraph source regions |
| R3–R5 | recognition image tests: native pixels, unchanged source buffers, displayed background crop, effects, mixed Unicode, paragraph/vertical/perspective text, independent worker equality, cancellation and invalid input |
| R4–R5 | `mergedParagraphUsesSourceRows`: row geometry, Unicode hit testing, selection, zoom, cache invalidation and fallback; `sourceRowsSurviveImageExportOnWorkers`: row occupancy, gaps, shorter final row, worker equality and overlap fallback; existing export artifact, Quick Save, and save-dialog regressions |

The standalone renderer tests and the performance target disable unused default Qt plugins and
load Windows test fonts explicitly for deterministic offscreen shaping. Test providers use cached
results; no OCR model or translation network request is needed.

## Performance measurement

Build `snow-shot-recognition-image-performance-benchmark` with `windows-msvc-performance`
(Release), then run it with `-platform offscreen`. It reports 1080p and 4K fixtures with 20 and
500 lines, snapshot median time, render median/p95, and source-image bytes. Each fixture discards
two warmups and measures twenty runs. It asserts that snapshot copies retain the source image and
line-container backing buffers. Rasterization is measured separately from snapshot capture;
encoding and disk timing belong to the existing export pipeline.

Run only the named related tests; do not run the full repository suite. Benchmark measurements
and final verification results belong in the delivery report.
