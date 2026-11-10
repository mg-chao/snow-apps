# GPU backend capability and verification matrix

Updated 2026-10-25. This describes the Windows GPU architecture and its supported contracts.

## Ownership and retained updates

| Responsibility | Implementation | Contract and evidence |
| --- | --- | --- |
| Document/editing logic, history and hit testing | Shared engine/document/editor crates | No GPUI/GPU dependency enters these crates. Qt/C ABI patches remain available; related ABI patch tests cover the boundary. |
| Logical publication | `RetainedSceneComposer` and `acquire_retained_scene_delta` | Stable element generation/role IDs; base/result revisions, sparse upserts/removals, geometry/material/pose revisions, bounded journals and explicit reset after eviction. |
| Local edits | Changed owners plus dependent labels/connectors | 10,000-item operation-count tests cover local and coalesced edits. Topology changes rebuild painter order and are not constant-time. |
| Document mutation validation | Certified ownership/fixed-layer invariants, reverse label owners and derived paint ranks | Ordinary data/meta edits synchronize only touched owners. A 10,000-element test checks no global synchronization or rank-table rebuild; mixed-owner edits match forced full synchronization and undo. Imports, relation changes and structural changes retain full validation. |
| Path poses | Local path/chunk payload retained across rigid translation/rotation | Long-path regression checks the same local `Arc` and geometry revision. Compatibility world descriptors still lower changed path coordinates and verify rigid correspondence; this remaining CPU work is not constant-time. |
| Text measurement | Indexed pending-owner caches and bounded shared shaping cache | 10,000 visible angle-label regression and one-touched-bubble test verify unrelated measurements are not rebuilt. |
| Input/presentation timing | `CanvasSession` deferred composition and `flush`, prepaint publication | Input, capture and current camera update immediately. Presentation coalesces deltas. Snapshot capture freezes the same retained scene. |
| Renderer scene storage | 256 copy-on-write ID buckets, shared painter order | Tests check one-bucket copying, immutable snapshots, stale update rejection and journal eviction. Topology changes reset the GPU order index. |
| Device ownership | [GPUI fork](https://github.com/mg-chao/gpui) pinned in Cargo.toml, default-off `gpu-extensions` | App/window access returns the existing GPUI device. Generic texture/job/completion/readback/damage interfaces contain no Snow application concepts. See the fork's `crates/gpui/GPU_EXTENSIONS.md`. |
| Rasterization and visibility | `GpuCanvas`, cached meshes, per-view spatial bins, retained 256-pixel tiles | CPU prepares reusable geometry; document rasterization, blending, image sampling and effects execute on GPUI's D3D11 device. Old/new bounds and downstream filter support determine damage. |
| Presentation | GPUI texture primitives and damaged-window composition | Published textures remain immutable while referenced by a scene. Completion tracking bounds in-flight work; device generation changes invalidate resources explicitly. |
| GPU retirement and budget | One configured budget per device generation, inherited by display/overlay/export canvases | Live, submitted, staged-readback and retiring allocations remain charged. Final reference drops coalesce into device-owned completion batches; native owner polling continues after canvas destruction. Epoch replacement preserves policy and creates a fresh accounting domain. |
| Completion scheduling | Native D3D11 fence events wake Smart Erase and frame-readiness tasks where supported; timer polling remains available | Windows callbacks only wake tasks; owner readiness checks validate retirement/cancellation/device state before publication. Early-drop event handles remain in deferred retirement. Native timing control and device-backed lifecycle tests are recorded in the Smart Erase migration report. |

## Drawing, effects and interaction

| Capability | Backend path | Verification scope |
| --- | --- | --- |
| Rectangle/ellipse/diamond, independent rounded corners and rotation | Cached meshes from engine geometry | Geometry and device-backed fixtures. |
| Straight/curved/elbow arrows, head/shaft variants and labels | Path/head meshes, polygon union/difference for round ink-boundary outlines and separate pose | All 15 heads across solid/dashed/dotted shafts and three path types; hollow-head, outline-alpha, label-gap, tile-edge and PNG/SVG parity tests. |
| Freehand, pressure outlines, dash/dot and continuation | Shared chunk geometry and GPU mesh cache | Chunk/pressure/rigid-edit regressions. Mouse events do not invent pressure; native hosts can supply typed pen input. |
| Fill/border/hatch/crosshatch, opacity, multiply and rough strokes | GPU materials, coverage groups and seeded roughness | Premultiplied coverage tests; no QPainter-identical rough-pixel claim. |
| Text, fallback fonts, bidi, wrapping, alignment and labels | Shared shaping/line layout, GPU glyph outlines, and same-device Direct2D color-font pages | UTF-16, grapheme, bidi, wrapping, caret and bound-label contracts. Physical IME candidate windows and accessibility remain separate qualification work. |
| Inline editing, preedit, selection and caret | GPUI input handler and GPU overlays | Rotated coordinate mapping and native text protocol. No document readback paints editor overlays. |
| Speech bubbles, serial badges/connectors and distance/angle labels | Engine geometry and pending text requests | Shared command/history behavior and focused geometry/label checks. |
| Selection/marquee, move/resize/rotate/duplicate, constraints, handles and snaps | Shared engine interaction plus GPU overlays | Capture/cancellation/undo behavior remains in the engine; controls are excluded from exports. No new grouping/lasso workflow is implied. |
| Mosaic, blur, grayscale, inversion, emboss and brightness | Ordered GPU input passes, masks and shaders | Device-backed fixtures and tile/export paths; no byte-identical OpenCV claim. |
| Restore Background | GPU pristine-source replacement, including alpha | Separate from object erasing; transparent original pixels can remove annotation coverage. |
| Smart Erase solve | CPU source-grid mask, cheap affine/periodic paths and bounded tiny-raster x64 routing; GPU layered donor assembly, domain/model/pyramid preparation and multiscale compute with CPU fallback | One 352-byte resident preparation readback; no reconstruction image transfer. Exact-geometry native pairs qualify execution policy; hosts can override CPU/GPU preference. Device-backed tests cover exact sampling, adaptive expansion, conservative pyramids, deterministic matching, quality, alpha/unmasked bytes, cancellation, worker/owner fallback and memory pressure. |
| Smart Erase display/export | Completed GPU patches stay resident; CPU patches upload once | Camera/opacity cache reuse; strict frozen exports validate device/epoch. Exact straight buffers support lossless patch persistence. The worker never renders a display frame. |
| Magnifier, spotlight and watermark | GPU pristine-source sampling, masks and glyph geometry | Shared source/interaction data and ordered decoration stages; per-view render areas remain separate from history. |
| Auto Filter | Existing engine region/category logic | Host-supplied regions are supported; demo sample regions are not an OCR service. |

## Sources, persistence and exports

| Capability | Contract | Focused verification |
| --- | --- | --- |
| Original images and layered sources | Original leaf buffers are shared. GPU source pages retain LODs and reuse resident base pixels during downsampling. `from_layers` retains order, anisotropic placement, coverage, donor eligibility and virtual dimensions | No full editing-time visual/donor flattening. Smart Erase assembles bounded eligible GPU regions with a 64 MiB immutable page cache; CPU fast models/fallback remain. Nested placement/coverage and source identity tests. |
| Host presentation | Before/after layers, backdrop override, visibility and clips | Per-view state is separate from engine history/persistence and is consumed by the same GPU renderer during export. |
| Immutable export camera | `prepare_text_metrics`, `CanvasSnapshot::capture`, `with_region`, `with_presentation` | Does not allocate an output-sized engine viewport or GPU texture. |
| PNG, packed PNG and clipboard | `GpuExportJob` on supplied GPUI service; at most two reusable staging handles and two queued CPU bands | Staging leases remain charged while handles are cached. Native checks cover alpha, tile seams, packed gaps, reuse, cancellation and staging bounds. Clipboard uses the current app service. |
| SVG | Original image layers plus portable outlined vectors; visible arrow outlines, effects, rough strokes, color glyphs and custom presentation embed a GPU-composited PNG | Bytes/writer/path entry points share the eligibility rule. Layer coverage/anisotropic placement, outlined glyph and arrow-outline fixtures. Pure vector serializer rejects content requiring GPU compositing. |
| Streaming/cancellation | Nonblocking owner-thread polling; CPU encoding worker; atomic path publication | Packed painter order, incomplete/cancelled bands and file failure preserve explicit outcomes. |
| `.snowdraw` schema 1 | `GpuSavePreparation` asynchronously freezes visual and donor composites before worker encoding | Unprepared layered `encode` fails explicitly. Native save fixture checks both composites and coverage. Raster-only saves skip materialization. |
| Headless | GPUI `OffscreenGpuHost` and the same `GpuCanvas`/export job | `--export-sample` uses this path; windowed exports never create another device. |

## Bounds and qualification

- The implemented native GPU backend is Windows D3D11. Other backends need the
  generic GPUI extension implementation before this renderer can run there.
- Default aggregate canvas GPU budget: 512 MiB. Source pages, meshes, retained
  tiles, scratch, export staging and pending retirements are accounted for. The
  first configured per-device budget is inherited by sibling canvases and exports;
  conflicting explicit limits fail. Device/allocation errors are explicit, with
  no silent CPU renderer fallback.
- Display/working surfaces retain the 16 Mi-pixel / 16,384-dimension contract.
  Streaming output supports 8 GiB decoded RGBA, an 8 MiB scanline, 8 MiB / 128-row
  bands and 1024-pixel render segments. Effect support caps at 8192 physical pixels
  and must also fit the GPU budget. Buffer exports cap encoded bytes at 96 MiB.
- Decode defaults to 512 MiB; original buffers accept explicit budgets up to 8 GiB.
  Layer preflight retains 128 layers / 1 GiB input / 512 MiB virtual composite.
  Sources are contiguous buffers, not an out-of-core provider. Source save/readback
  intentionally returns full CPU images; display does not read back pixels.
- Smart Erase retains its 2 Mi-pixel solve ROI, conservative 512 MiB working
  estimate and 128 MiB result cache. Persistent files/default assets cap at 512 MiB.
- Physical pen/IME/accessibility, ARM64 and other platforms need separate native
  qualification. No universal 120 Hz or input-to-present result follows from
  operation counts, cached timings or offscreen GPU completion measurements.
- History destruction remains on its owner thread and has no hard deadline.

## Reproduce focused checks

Run only filters relevant to a change; these are independent targeted checks.

```powershell
cd D:\snow-apps-workspace-2\snow-apps\snow-draw-engine-gpui
cargo check --locked --offline -p snow-draw-engine-gpui --lib --bin snow-draw-demo
cargo check --locked --offline -p snow-draw-engine-gpui --target aarch64-pc-windows-msvc --all-targets
cargo test --locked --offline -p snow-draw-engine-gpui --lib gpu_export::tests
cargo test --locked --offline -p snow-draw-engine-gpui --lib gpu_memory::tests
cargo test --locked --offline -p snow-draw-engine-gpui --lib document::tests
cargo test --locked --offline -p snow-draw-engine-gpui --lib retained_scene::gpu_store
cargo test --locked --offline -p snow-draw-engine-gpui --lib smart_erase_service::tests
cargo bench --locked --offline -p snow-draw-engine-gpui --bench gpu_profile --profile canvas-perf -- ..\artifacts\snow-draw-gpui-gpu-profile.json
cargo bench --locked --offline -p snow-draw-engine-gpui --bench gpu_workloads --profile canvas-perf -- ..\artifacts\snow-draw-gpui-gpu-workloads.json

cd ..\snow_draw_engine_qt
cargo test --locked --offline -p snow-draw-engine-document annotation_sync_tests
cargo test --locked --offline -p snow-draw-engine-scene retained::tests
cargo test --locked --offline -p snow-draw-engine retained_tests
cargo test --locked --offline -p snow-draw-engine angle_ten_thousand_warm_visible_labels
cargo test --locked --offline -p snow-draw-engine speech_bubble_text_measurement_cache_reuses_warm_frames
cargo test --locked --offline -p snow-draw-engine-c abi::patch
```

Related retained scene/engine, GPU export, layered-source, Smart Erase and
text-cache checks were exercised during this overhaul. The final native export
filter passed all 11 cases, including reusable staging and cancellation; the GPU
memory filter passed all 7 cases. The default-feature ARM64 all-targets check
passed with the required Rust 1.98.1; this is cross-compilation evidence, not native ARM64
runtime qualification. The benchmark command is a measurement instruction;
compilation alone is not a performance result.
