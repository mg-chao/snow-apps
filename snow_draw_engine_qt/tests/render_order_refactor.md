# Render order: architecture and verification

The logical scene establishes filter source boundaries before spatial culling. Ordinary drawables, including transparent shapes and Smart Erase, terminate a source pass. Hidden/non-emitted elements do not participate; spotlight and watermark are separate decorations. Within a pass, consecutive compatible filters form an ordered effect run. All runs read the same immutable entering backdrop, then write in declared order. Thus invert -> grayscale -> invert has three runs, even if the middle effect is culled.

## Ownership and transport

- `DocumentSceneCache` retains lightweight ordering nodes and generation-aware pass/run identities. Membership, visibility, relationships, order and effect compatibility rebuild topology; geometry-only changes reuse it.
- Arrow labels expand after their owners; serial connectors expand after presented text. Relationship indices replace repeated document scans. Duplicate previews retain their compact model, indices and topology while drag updates change geometry.
- `snow_patch_get_scene_render_plan` is additive and preserves existing public struct layouts. Descriptors address the resulting scene array. Replacement 0 retains metadata; replacement 1 replaces it, including an empty clear. Full resets include metadata. Plan-only changes advance scene revision; overlays do not resend it.
- Qt validates ordered ranges, complete filter coverage, homogeneous runs and non-reappearing identities. Invalid metadata resets the display cache through its existing error path.

## Execution and invalidation

Qt retains an execution plan by scene revision and DPR. Filter paths, bounds, physical parameters and spatial groups are resolved once. Filter-only storage is compact; ordinary items use integer lookups. Tiles and exposed regions select operations without regrouping their order. Direct fixtures construct metadata from complete uncropped arrays; their helper can recognize adjacent effects that resolve to equal physical parameters because no intervening item is missing.

Source-region expansion, replay, snapshot lookup and Rust dirty propagation use explicit membership. Each source pass captures one immutable backdrop per working region. Spatial subdivision remains inside an effect run. Source-cache keys use stable pass identities. Dependency fingerprints for all pass boundaries in one region use a single prefix traversal, reused for lookup and store. Regional invalidation and the existing 64 MiB cache bound remain intact.

Topology invalidation compares old/new ordered pass membership rather than item offsets, dirties changed filter outputs, and propagates through downstream passes. Geometry-only damage remains spatial. Widget and export share spotlight-then-watermark decoration rendering; editor overlays and custom after-canvas hooks retain their positions.

## Verification

Only affected crate tests and filtered Qt tests were run. Coverage includes pre-culling boundaries, offscreen effect changes, generation-aware identities, plan-only/empty/reset/no-op patches, malformed descriptors, ABI storage/lifetimes/null output checks, label ownership, shared serial text/rebinding, reorder/history, duplicate caching, geometry-only reuse, Smart Erase and automatic filters. Pixel regressions cover mixed effects, mixed strengths, partial opacity and pen masks; full/tiled and cold/retained comparisons at DPR 1, 1.25 and 2; partial repaint and viewport crop invariance; and actual runtime export cropping, including fractional selections. Retained-plan tests cover unchanged/decoration/overlay reuse and DPR changes.

## Release measurements

All timings use `windows-msvc-performance` (Release), three warmups and 20 measured frames per trial. Values below are medians of three per-trial medians. Composition changes the camera without changing topology. The baseline and after runs use the same fixture sizes/settings.

| Composition | Elements | Before (ms) | After (ms) | Change |
|---|---:|---:|---:|---:|
| ordinary | 100 | 0.0487 | 0.0446 | -8.4% |
| ordinary | 1,000 | 1.8457 | 0.3135 | -83.0% |
| ordinary | 10,000 | 193.2225 | 4.0396 | -97.9% |
| labels | 100 | 0.1362 | 0.0440 | -67.7% |
| labels | 1,000 | 7.3399 | 0.4220 | -94.3% |
| labels | 10,000 | 749.4910 | 6.4917 | -99.1% |

All composition trials retain one ordering-plan build. The relationship-index count stays at two (initialization and fixture insertion); camera frames do not rebuild it.

| Renderer scenario | Before (ms) | After (ms) | Change | Planning (us) |
|---|---:|---:|---:|---:|
| 10k_mostly_offscreen_gaussian_1080p | 0.8017 | 0.6359 | -20.7% | 31.3 |
| alternating_8_1080p | 5.3607 | 6.3771 | +19.0% | 15.9 |
| grouped_gaussian_8_1080p | 4.7327 | 4.3300 | -8.5% | 12.3 |
| mixed_content_filter_layers_1080p | 7.0606 | 7.1333 | +1.0% | 21.7 |
| retained_plan_source_1080p | New scenario | 5.4769 | - | 0.0 |
| sparse_distant_gaussian_1080p | 0.0430 | 0.0432 | +0.5% | 1.3 |

The alternating-effect baseline incorrectly merged eight effects into two dispatches. Correct ordering executes eight, accounting for its additional work. It is a correctness change, not an unaffected workload. A first implementation regressed the 10,000 mostly-offscreen case by allocating filter structures for ordinary items; compact filter storage removed that regression. No unaffected measured scenario shows a repeatable regression above 5%.

The new warm-source scenario has two source passes and eight Gaussian filters. Each measured frame reports zero execution-plan builds, two cache hits, zero misses and five dependency-item visits (one prefix traversal through the lower pass and separator). Planning time can round to zero at the timer resolution. All measured renderer cases report zero new kernel-workspace allocated bytes after warmup. These allocation diagnostics do not count all C++/Qt heap allocations; composition heap allocations are not instrumented.

Raw CSVs and build/test logs are under ignored `build/render-order-audit/`. The existing bundled-FFI Release benchmark target omits FFmpeg link libraries in this workspace; baseline and after measurements use the same generated-project-only workaround. No unrelated source build configuration was changed.

Related commands:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-scene-order-benchmark_build snow-canvas-filter-benchmark
# Run snow-scene-order-benchmark.exe, or snow-canvas-filter-benchmark.exe with:
# --scenario <exact-scenario> --warmup 3 --iterations 20 --csv <path>
cargo test --manifest-path snow_draw_engine_qt/Cargo.toml -p snow-draw-engine-scene -p snow-draw-engine-model -p snow-draw-engine-document -p snow-draw-engine-c
cargo test --manifest-path snow_draw_engine_qt/Cargo.toml -p snow-draw-engine auto_filter
ctest --preset test-windows-msvc-debug -R "^snow-canvas-(filter-render|retained-filter-paint|spotlight-render|arrow-text|serial-number-drag|duplicate-drag|smart-erase|custom-renderer|text-draft|document-reset)-tests$" --output-on-failure
```
