# OCR runtime and V6 models

Snow Shot uses CPU inference and optional GPU acceleration through the `simd-paddle-ocr-rs` Git submodule.
Runtime **1.2.1** uses protocol **7**. Initialize submodules before building;
the worker alone uses the library's pinned `nightly-2026-09-08` compiler.
Other Rust packages keep their existing toolchain. The runtime takes verified
local model paths, borrows child-owned BGR pixels, and returns owned text,
confidence, and quadrilaterals in original-image coordinates. Orientation
classification is disabled. The CPU context uses the physical-core budget.

| Label | Configuration value | Cache ID | ModelScope directory |
| --- | --- | --- | --- |
| Ultra Small V6 | `extra_small` | `ppocrv6-tiny-cd609a1` | `PP-OCRv6/tiny` |
| Small V6 | `small` | `ppocrv6-small-463ea9f` | `PP-OCRv6/small` |
| Medium V6 | `medium` | `ppocrv6-medium-f5063c6` | `PP-OCRv6/medium` |

Small V6 remains the default. The original V6 model files, dictionaries, hashes,
and cache IDs are unchanged. Saved V4/V5 selections normalize to Small. Obsolete
DirectML settings have no effect, and existing cached files need no deletion.
The selected model reports acquisition failure without switching to another model.
Verified caches are retained and stale activation results are discarded.

Downloads, extracted runtime files, and packaged assets are verified with SHA-256
before publication. Normal startup reuses installed assets after checking the
manifest, file types and sizes, completion markers, and bundled executable
permissions and architecture. It does not hash the full files again, so same-size
content changes after installation are not detected by startup validation.

The default maximum-side detector policy selects limits of 960, 1500, or 2000
according to the screenshot's longest side. Minimum-side mode uses 736. Accuracy
First uses a recognition-width floor of 320; Speed First uses 48. Widths align to
32 pixels. Recognition text and confidence can differ from the previous runtime.
The recognizer uses the model's Softmax probabilities for confidence. Empty
lines and confidence below 0.5 are filtered. Residency defaults on in Full and off
in Mini. Hot start defaults off in both editions and is effective only when
residency is enabled.

Hardware acceleration also defaults off. Windows builds enable Vulkan 1.3
through the GPU driver's loader; Apple Silicon builds enable Metal. Device
initialization runs asynchronously in the worker. Requests use CPU while the
GPU is unavailable or not ready, and reuse the context after discovery. Cold
nonresident requests may finish on CPU. GPU errors fall back to CPU without
changing the preference; enabling it never enables residency or hot start.
Windows builds require Python, CMake, and the C++ compiler for build-time
shaderc compilation; deployed apps need no Vulkan SDK or shader compiler.

Protocol 7 adds an acceleration preference to `PrepareSession`, plus separate
configuration and asynchronous readiness messages. Image transfer and recognition
result layouts are unchanged. Both sides explicitly reject older workers. See
[`PROTOCOL.md`](../../snow-crates/crates/snow-ocr-process/PROTOCOL.md).

Recognition and callback delivery are FIFO across interactive and prefetch work.
The single inference executor can overlap the next image transfer. Model/settings
changes replace sessions in the existing child; waiting images use the newest
configuration when inference is dispatched. Canceled resident inference drains
naturally, and local rendering does not retain OCR resources.

Process startup, mapped-file work, image conversion, and process cleanup belong
to the transport thread. One shared image slot is allocated only for undelivered
images, bounded by the existing 3840 × 2160 total-pixel limit. Larger images wait
for transfer acknowledgement before replacing the mapping; smaller images reuse
capacity. The mapping is released after the last image is copied into the child,
independently of inference and session lifetime.

After three unexpected exits in 60 seconds, background residency is suspended.
The first two retries wait 1 and 2 seconds. The next user recognition re-arms
residency; automatic prefetch runs on demand while suspended. Background resource
acquisition retries after 5, 15, and then 60 seconds without showing settings
errors. Warm-up failures are logged and retried on the next recognition.

## Runtime preparation and publication

Windows x64 and ARM64 ZIPs contain exactly the versioned worker executable and
`runtime-manifest.json`. There are no inference DLLs. The trusted descriptors in
this directory are generated from the actual built bytes, including archive and
entry hashes. Preparation creates local artifacts; publication is a release step.
Keep all earlier published archives unchanged. Publish the exact pinned 1.2.1
archives before distributing this application revision.

```powershell
scripts/prepare-snow-shot-ocr-runtime.ps1 -Architecture x64
scripts/prepare-snow-shot-ocr-runtime.ps1 -Architecture arm64 -BuildDirectory build/ocr-arm64
```

Use `-SkipBuild` only with an already-built Release worker. The helper audits PE
architecture, dependency imports, archive inventory and hashes, then emits the
trusted descriptor and native-validation status. Cross-built ARM64 artifacts
still require native validation before production publication.

Full offline Windows packages bundle Small V6. Online and Mini variants acquire
the trusted runtime and selected model on demand. Offline staging and managed
downloads use the same pinned descriptors. Rollback requires the preceding app
and its matching runtime and manifest.

macOS x64/ARM64 builds bundle the local worker, without ONNX Runtime dylibs, and
use a generated schema-3 manifest. Full bundles Small V6; Mini bundles the worker
and downloads selected models. Runtime code updates only with the app. Models
and manifests are sealed under `Contents/Resources/assets` and exposed through
the relative `Contents/MacOS/assets` resource link. Hashes are finalized after
Mach-O deployment and nested signing, before signing the outer app. Native
macOS and ARM64 checks remain necessary before publication.

## Focused verification

Run only affected worker, library, OCR assets/lifecycle/recognition, settings,
configuration and search tests. The deterministic asset tests use download hooks
and do not require large models. Release dependency/license checks require a
closure free of RapidOCR, ONNX Runtime and DirectML. The library tests cover
scalar shape reshaping, invalid metadata, resize policies, width floors and
reusable-request options.

Supply verified models as `<model-root>/<cache-id>/<filename>` for real inference:

```powershell
$env:SNOW_TEST_OCR_TEXT_FIXTURE = (Resolve-Path snow_shot/tests/baselines/ocr-model-versions.png).Path
$test = 'build/windows-msvc-debug/snow_shot/test-bin/Debug/snow-shot-ocr-recognition-service-tests.exe'
$modelRoot = (Resolve-Path build/ocr-versioned-models).Path
$worker = (Resolve-Path artifacts/snow-ocr-runtime-1.2.1/snow-ocr-process-1.2.1-windows-x64.exe).Path
& $test "--model-root=$modelRoot" "--worker=$worker"
& $test "--model-root=$modelRoot" "--worker=$worker" --resident-models
& $test "--downloaded-runtime=$((Resolve-Path artifacts/snow-ocr-runtime-1.2.1-windows-x64.zip).Path)"
```

The downloaded-runtime check copies the prepared archive and models through the
managed downloader hook, then verifies, extracts and recognizes from a fresh cache
without requiring publication. Each model must recognize the English/Chinese
fixture. Also check blank, narrow, and large images, original-image coordinates,
cancellation, session replacement,
and hot start. `--validate-model-set <detector> <recognizer> <dictionary>` checks
initialization independently. Performance measurements require a performance
preset; do not use Debug or packaging presets for benchmarks.
