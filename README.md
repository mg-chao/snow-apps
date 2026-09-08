# Snow Apps

Snow Apps repository, providing source code for Snow Shot and Snow Image Viewer.

<div style="font-size: 128px">🏗️🚧🦺</div>

## Image conversion in Snow Shot

The screenshot and pinned-image toolbars offer **Convert to Markdown** and **Convert to HTML**
in the same popover group as barcode/table recognition. Activation sends the image to a vision model and streams a
selectable document preview. HTML conversion preserves document structure, not pixel-perfect layout.

- Conversion loading uses the same message component as OCR, with format-specific prompts. The
  message remains steady while streaming and closes on completion, failure, cancellation, or a tool
  switch. Failures keep an inline error and **Retry**. The supplied Markdown and HTML SVGs are
  bundled in the theme-aware vector icon pack for the toolbar, popover, and layout editor.
- Older toolbar layouts gain conversions in the existing barcode/table group. The previous default
  with standalone conversion buttons is folded into that group; explicitly hidden tools and custom
  placements are preserved.
- The conversion sub-toolbar's **Settings** popup saves one **Vision Model** for both formats and
  both surfaces. Only models advertising `supports_vision` are offered. The first available vision
  model is used if the saved model is unavailable. Changing a model applies to the active conversion;
  other windows do not silently rerun paid requests.
- Toolbar **Copy** copies the Markdown or HTML source. Selecting text and copying within the preview
  copies the selected readable text and keeps the preview open.
- Completed results are cached separately by format, model, image fingerprint, and prompt version.
  Pins save these results and restore the last visible matching preview without calling the model.
  Partial, cancelled, and failed results are not saved as completed conversions. Retry is explicit.
- Previews use Qt's native rich-text renderer. Scripts are not executed, resources are not loaded,
  and only an explicit HTTP(S) link click can open a browser. This is a document preview, not a web
  browser or source editor; advanced HTML/CSS is intentionally outside its scope.

The client reuses `GET /api/v2/chat/models` and streaming `POST /api/v1/chat/completions`;
no new API endpoint, dependency, or database migration is required. Images are prepared as WebP
off the UI thread (maximum side 2880 pixels, quality 75). The complete request is limited to 2 MiB,
the response to 4 MiB, and conversion to 120 seconds. Empty, interrupted, or token-truncated output
is reported as an error; any partial preview remains available with Retry. Request diagnostics record
model, format, duration, and outcome, never image bytes or generated document content.

Conversion caches are optional versioned extensions of pinned recognition data. This version reads
existing pins; rolling back to an older client can discard recognition caches containing the new
extension, but does not change the saved image. Review AI output before relying on its accuracy.

Focused verification after building the corresponding test targets:

```powershell
ctest --preset test-windows-msvc-debug -R '^snow-shot-(api-client|image-conversion(-toolbar|-pinned|-storage)?)-tests$' --output-on-failure
```

## Open Source Licenses

This is a multi-license repository. See [LICENSE.md](LICENSE.md) for the
repository-level scope rules and third-party material policy.

| Project | License |
| --- | --- |
| `ant_design_qt/` | [Apache License 2.0](ant_design_qt/LICENSE) |
| `snow-crates/` | [Apache License 2.0](snow-crates/LICENSE) |
| `snow_draw_engine_qt/` | [Apache License 2.0](snow_draw_engine_qt/LICENSE) |
| `snow_rust_ffi/` | [Apache License 2.0](snow_rust_ffi/LICENSE) |
| `snow_image/` | [GNU GPL v3.0 or later](snow_image/COPYRIGHT) |
| `snow_image_viewer/` | [GNU GPL v3.0 or later](snow_image_viewer/COPYRIGHT) |
| `snow_shot/` | [GNU GPL v3.0 or later](snow_shot/COPYRIGHT) |

Synchronized and bundled third-party materials retain their upstream licenses.
See [Ant Design Qt third-party notices](ant_design_qt/THIRD_PARTY_NOTICES.md)
and [Snow Shot third-party notices](snow_shot/THIRD_PARTY_NOTICES.md).
