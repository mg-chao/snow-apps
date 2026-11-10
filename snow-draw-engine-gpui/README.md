# Snow Draw Engine GPUI

An Apache-2.0 GPU canvas backend and standalone GPUI demo. Shared Snow Draw Engine
Rust crates own pure document/editing logic. Meshes, image pages, effects, retained
tiles and presentation use GPUI's own device. Qt is not linked.

The manifest pins `gpui`, `gpui_platform` and `gpui_windows` to revision
`2c0bf70b54c018f99419f386d1cf1105dca57661` of the
[Snow Shot GPUI fork](https://github.com/mg-chao/gpui), with the dedicated
`gpu-extensions` feature. Generic device/job/texture/completion/readback interfaces
stay outside application logic. See the
[fork contract](https://github.com/mg-chao/gpui/blob/2c0bf70b54c018f99419f386d1cf1105dca57661/crates/gpui/GPU_EXTENSIONS.md).
Cargo fetches the fork on the first build; subsequent cached builds can use `--offline`.

The native implementation is Windows D3D11. Display, raster export and visual
regression tests use the retained GPU renderer.

## Run

Use the package's pinned Rust 1.98.1 toolchain and the repository's VS 2026 developer environment:

```powershell
cd D:\snow-apps-workspace-2\snow-apps\snow-draw-engine-gpui
.\scripts\run-demo.ps1
.\scripts\run-demo.ps1 -Optimized
# Same GPU renderer through a GPUI-owned headless device:
cargo run --locked --offline -p snow-draw-engine-gpui --bin snow-draw-demo -- --export-sample ..\artifacts\snow-draw-gpui-sample.png
```

Cargo output is isolated in `../build/snow-draw-gpui`. New starts a drawing; Image
replaces its pristine source; Open/Save use `.snowdraw` schema 1; PNG/SVG export
the viewport. Decode, encode and Smart Erase input preparation run on CPU workers.
Textured Smart Erase reconstruction uses GPU compute; submission and completion
polling stay on the service owner thread.
Native fence events wake completion tasks where supported; other devices retain
timer polling. Callbacks do not submit GPU work.

Shortcuts: V select, R shape, A arrow, L line, P pen, T text, E object eraser;
Space+drag or middle drag pans; wheel zooms/adjusts the tool. Shift constrains or
adds selection. Double click text to edit; Ctrl+Enter commits, Escape cancels,
Enter inserts a line break. Ctrl+Z/Y undo/redo, Ctrl+D duplicates, Delete removes,
Ctrl+C/V transfers templates, Ctrl+Shift+C copies an overlay-free PNG. Ctrl+0 fits
the source; Ctrl+plus/minus changes zoom. Text editing supports graphemes, bidi
selection, local undo and UTF-16 IME composition.

## Architecture

- `CanvasDocument` owns the pure engine and immutable original assets. Layer
  placement, coverage and donor eligibility stay as metadata; `from_layers` does
  not flatten editing inputs.
- `CanvasSession` owns a retained viewport. Stable ID deltas separate geometry,
  material and pose revisions. Enable deferred composition and call `flush` before
  frame preparation; camera/capture/interaction state remains synchronous.
- `RetainedScene` shares 256 copy-on-write ID buckets and painter order. Local
  updates copy touched buckets; ordered iteration reads retained nodes directly.
- `CanvasView` adapts GPUI input, focus and text editing; prepaint coalesces updates.
  Controls, caret and transient text use GPU overlay geometry. Color fonts use
  DirectWrite glyph IDs and Direct2D pages on the same GPUI D3D11 device.
- `GpuCanvas` obtains `cx.gpu_service()` / `window.gpu_service()`. Cached geometry
  and original-image pages feed retained 256-pixel tiles; per-view visibility and
  ordered effect support determine damage. Idle content reuses textures.
- `SmartEraseService` prepares bounded source-grid masks and retains cheap CPU
  affine/periodic fills. Automatic execution also keeps qualified tiny, dense x64
  raster fills on CPU; hosts can select CPU or GPU preference through
  `CanvasViewOptions::smart_erase_execution`. Layered texture inputs use GPU donor assembly and domain/model/
  pyramid preparation; `GpuSmartErase` keeps reconstruction and completed patches
  on the GPU, independently of camera/DPI, with CPU fallback.
  See [migration evidence and remaining work](docs/SMART_ERASE_GPU_MIGRATION.md).
- `GpuExportJob` uses that same renderer/service, with at most two readbacks and
  two queued CPU encoder bands. Normal display requires no readback.

The shared engine serves Qt/C ABI patches through its ordinary viewports. GPU
viewports expose stable-ID deltas and have no flat-scene patch projection.

## Export and persistence

Noninteractive hosts call `prepare_text_metrics(&mut document, &mut session)` then
`CanvasSnapshot::capture(&document, &session)`. Views expose `snapshot()` and
`smart_erase_snapshot()`. `with_region` selects an export camera without creating
an output-sized viewport; `with_presentation` captures a host backdrop or clip.

Construct `GpuExportJob::bytes`, `to_path`, `to_writer`, `packed_png` or
`packed_to_writer` with the current `Arc<dyn gpui::GpuService>`, snapshot, scale,
completed Smart Erase results and cancellation token. Poll on the owner thread,
yielding to the event loop while Pending. The worker encodes pixels and never calls
the GPU service. Path exports publish through atomic replacement.

Plain SVG keeps original image layers and portable outlined text/paths. Visible arrow outlines,
effects, color-font rendering or custom presentation embed a GPU-composited PNG. Missing Smart Erase results
fail strict export. Selection controls are excluded.

`GpuSavePreparation` asynchronously freezes layered visual and donor composites
on the current device. Its completed `DrawingSnapshot` is encoded on a worker;
schema 1 stays unchanged. `encode()` rejects unprepared layered sources. Raster-only
saves need no GPU materialization. Custom hosts can provide already prepared GPU
results through `DrawingSnapshot::materialize_for_save`.

Use `OffscreenGpuHost` only for headless hosts/tests and keep it alive until jobs
and textures are released. Windowed exports use the existing app/window service.

## Focused checks and measurements

```powershell
cargo check --locked --offline -p snow-draw-engine-gpui --lib --bin snow-draw-demo
cargo test --locked --offline -p snow-draw-engine-gpui --lib gpu_export::tests
cargo test --locked --offline -p snow-draw-engine-gpui --lib document::tests
cargo test --locked --offline -p snow-draw-engine-gpui --lib retained_scene::gpu_store
cargo test --locked --offline -p snow-draw-engine-gpui --lib smart_erase_service::tests
cargo bench --locked --offline -p snow-draw-engine-gpui --bench gpu_profile --profile canvas-perf -- ..\artifacts\snow-draw-gpui-gpu-profile.json
cargo bench --locked --offline -p snow-draw-engine-gpui --bench gpu_workloads --profile canvas-perf -- ..\artifacts\snow-draw-gpui-gpu-workloads.json
# Native posted-Windows-input to Present1 submission, measured separately:
cargo bench --locked --offline --bench native_input --profile canvas-perf -- ..\artifacts\snow-draw-gpu-native-input.json
# GPU host/source/export integration checks:
cargo test --locked --offline -p snow-draw-engine-gpui --test canvas_engine
```

Run only checks related to a change. GPU timing needs the optimized `canvas-perf`
profile plus adapter/workload details. An idle cache hit is not cold rendering;
offscreen GPU completion is not native input-to-present latency.

## Resource and platform contracts

Default aggregate GPU budget: 512 MiB. Display/working surfaces keep the 16 Mi-pixel
/ 16,384-dimension contract. Large exports use bounded segments and effect halos,
not a full output texture: 8 MiB / 128-row bands, an 8 MiB scanline limit, up to
8 GiB decoded output, and at most 8192 physical pixels of effect support within
the GPU budget. Encoded buffer exports cap at 96 MiB; writer/path APIs stream.

Decode defaults to 512 MiB and accepts explicit budgets up to 8 GiB. Original
buffers are contiguous, not an out-of-core provider. Layer preflight retains
128 layers / 1 GiB input / 512 MiB virtual composite. Schema-1 save materialization
intentionally returns full source-grid CPU images; editing does not. Files and
default decoded save assets cap at 512 MiB.

Smart Erase retains a 2 Mi-pixel solve ROI, conservative 512 MiB working estimate
and 128 MiB result cache. Text editing is limited to 64 KiB UTF-8. Engine history
destruction stays on its owner thread and has no hard deadline. Device/allocation
errors never trigger a CPU display fallback.

Physical pen/IME/accessibility and other GPU platforms require separate
qualification. No QPainter/OpenCV pixel identity or universal 120 Hz result is
implied. See the current [capability matrix](docs/CAPABILITY_MATRIX.md) for coverage
and [GPU validation record](docs/GPU_BACKEND_VALIDATION.md) for native checks,
measured timings and their limits. See [third-party notices](THIRD_PARTY_NOTICES.md)
for dependency provenance.
