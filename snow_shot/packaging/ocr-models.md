# OCR model versions

The Model Type setting lists models in this order. Existing V6 configuration
values and cache IDs remain compatible; `small` (Small V6) remains the default.

| Label | Configuration value | ModelScope directory |
| --- | --- | --- |
| Ultra Small V6 | `extra_small` | `PP-OCRv6/tiny` |
| Small V6 | `small` | `PP-OCRv6/small` |
| Medium V6 | `medium` | `PP-OCRv6/medium` |
| Small V5 | `small_v5` | `PP-OCRv5/mobile` |
| Medium V5 | `medium_v5` | `PP-OCRv5/server` |
| Small V4 | `small_v4` | `PP-OCRv4/mobile` |
| Medium V4 | `medium_v4` | `PP-OCRv4/server` |

V4/V5 bundles come from the `mgchao/SnowShotOCR` upload at revision
`7b2a75a2a11c53b03d492b664073d48c2c51d86e`. Each bundle uses its matching
detector, recognizer, and dictionary. V4 uses `ppocr_keys_v1.txt`; V5 uses
`ppocrv5_dict.txt`. Orientation classifiers and the V4 document-enhanced
recognizer are outside this feature. The selected model must report acquisition
failure rather than silently switch to another model. Switching models retains
verified model caches and discards stale activation results.

## Runtime compatibility and release

Runtime 1.0.4 cannot initialize these four bundles: its reduced ONNX Runtime
operator list supports only the V6 models. Observed failures include `Mul(14)`,
`Relu(14)`, `Clip(12)`, and `MaxPool(12)`. Runtime **1.0.5** includes the V4/V5
original and CPU-optimized graph operator requirements while preserving V6's
requirements. The process protocol remains version 2.

Runtime **1.0.7**, protocol **3**, separates command-channel readiness, model
sessions, and image-transfer mappings. Both new settings default off:
`text_recognition/resident_process` keeps the child alive while idle;
`text_recognition/model_hot_start` additionally creates a fresh image-free model
session after each inference cycle. Hot start is effective only with residency
and retains its saved preference when residency is disabled.

Runtime **1.0.8**, protocol **4**, adds the detector resize policy to session
preparation. `text_recognition/detector_resize_policy` defaults to `max`, which
avoids enlarging short screenshots to a 736-pixel minimum side. Selecting `min`
restores that earlier behavior for screenshots with very small text. Changing the
setting replaces the model session in the existing process.

Runtime **1.0.9**, protocol **5**, adds Text Detection Processing to session
preparation. `text_recognition/text_detection_processing` defaults to
`accuracy_first`, preserving the recognizer's 320-pixel input-width floor.
Selecting `speed_first` lowers that floor to 48 before the existing 32-pixel
alignment. This reduces work on short text but can change text and confidence;
it is an explicit accuracy/speed tradeoff. Changing it replaces the session.
Protocol 5 adds this byte after the detector policy and before model paths, so
the app and runtime must be upgraded together.

Runtime **1.0.10** keeps protocol **5** and refreshes both Windows architectures
from the current release toolchain. The ARM64 archive includes the ONNX Runtime
MLAS compiler workaround and passes the packaged stack-cookie return-path guard;
the published 1.0.9 ARM64 archive predates that fix. Keep all earlier published
archives unchanged and update both trusted descriptors to the new exact bytes.

Recognition and callback delivery are FIFO across interactive and prefetch work.
The single inference executor can overlap the next image transfer. Model/backend
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

The locally prepared artifacts are:

| Architecture | Archive | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| x64 | `artifacts/snow-ocr-runtime-1.0.10-windows-x64.zip` | 15,988,806 | `429ab6e8180c2781bc863218c470c931b6016d25cac3fe3b76536a4d3d77d020` |
| ARM64 | `artifacts/windows-arm64/snow-ocr-runtime-1.0.10-windows-arm64.zip` | 15,543,068 | `05dde0d3a72d6877e592cc2dcac0bbb5174fa41591bf8639eade2ca9084e7241` |

Preparation does not upload these archives. Publish the exact pinned bytes before
distributing this app revision; earlier published artifacts must remain unchanged.
Existing verified model files remain reusable; the application and runtime must
use matching protocol versions.

When changing models, update both the trusted asset manifest and the packaging
descriptors, and regenerate/check
`cmake/vcpkg-overlay-ports/onnxruntime/required_operators.config` from original
and fully CPU-optimized ONNX graphs. Include operators introduced by graph
optimization, not just those present in the uploaded graphs.

The release maintainer must publish the exact hash-pinned runtime archive before
shipping the updated app or expecting clean development machines to download it:

```text
https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/runtime/1.0.10/windows-x64/snow-ocr-runtime-1.0.10-windows-x64.zip
https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/runtime/1.0.10/windows-arm64/snow-ocr-runtime-1.0.10-windows-arm64.zip
```

`scripts/package-snow-shot.ps1 -PrepareOcrRuntimeOnly` prepares the runtime ZIP,
checksum, and descriptor without producing an installer. It delegates to
`scripts/prepare-snow-shot-ocr-runtime.ps1`, builds only the Release OCR target,
and verifies the ZIP through the same pinned importer used by application
releases. `-SkipBuild` packages an already-built runtime without requiring an
installed application or updater. Keep the manifest's
runtime file sizes and hashes synchronized with that artifact. Never replace an
already-published runtime version with different bytes. Offline installers still
bundle only Small V6; other selections download on demand.

For rollback, reinstall the preceding app together with its matching trusted
manifest/runtime. Older configuration schemas normalize unsupported V4/V5
selections to their default; no destructive configuration migration is needed.

## Focused verification

Run the settings catalog, application storage, OCR recognition service, and OCR
process lifecycle tests only. The recognition executable's default tests use
temporary model files and download hooks to check all seven asset contracts,
cache reuse, failed acquisition/retry, and model changes during acquisition.

For actual V4/V5 inference, place verified files under
`<model-root>/<model-id>/<filename>`, as described by the trusted manifest. Then
run the built test executable with the packaged 1.0.10 worker:

```powershell
$env:SNOW_TEST_OCR_TEXT_FIXTURE = (Resolve-Path snow_shot/tests/baselines/ocr-model-versions.png).Path
$test = 'build/windows-msvc-debug/snow_shot/test-bin/Debug/snow-shot-ocr-recognition-service-tests.exe'
$modelRoot = (Resolve-Path build/ocr-versioned-models).Path
$worker = (Resolve-Path artifacts/snow-ocr-runtime-1.0.10/snow-ocr-process-1.0.10-windows-x64.exe).Path
& $test "--model-root=$modelRoot" "--worker=$worker"
& $test "--model-root=$modelRoot" "--worker=$worker" --directml
```

Both runs must recognize `Snow Shot 12345` and `文字识别` with all four bundles
(ignoring model-dependent spacing between Latin words).
To verify two resident/hot-start cycles using the staged default model and the
same packaged worker, run:

```powershell
& $test "--worker=$worker" --resident-text-fixture
& $test "--worker=$worker" --resident-text-fixture --directml
```

Initialization-only checks (`--validate-model-set`) are also required but do not
replace recognition tests. DirectML requests retain the existing CPU fallback
when acceleration is unavailable. Model payloads are not committed or downloaded
by deterministic unit tests.

## macOS ARM64 bundled runtime

macOS 15+ Apple Silicon uses the same seven model IDs and protocol 5 with CPU
inference. The app supplies a generated schema-3 `macos-arm64` manifest with
`delivery: bundled`; Windows schema-2 runtime archives and their pinned hashes
remain separate. Full bundles Small V6 for offline recognition on first launch.
Mini bundles only the runtime and trusted model descriptors; it downloads the
selected model on first use. Both editions reuse verified model caches and
never download executable code.
The macOS runtime is updated only with the application.

`scripts/snow-shot-macos-ocr.py` stages pinned models for Full and uses
`--runtime-only` for Mini staging, bundle preparation, finalization and verification.
Mini's model payload must be absent, including stale model directories in reused
staging trees. The script generates/verifies the manifest from finalized native
binaries. Runtime hashes are generated after
Mach-O deployment and nested signing, before signing the outer app bundle.
See `docs-macos-build.md` for the native seven-model, lifecycle, relocated-bundle,
and performance checks required before delivery.

Signed macOS bundles preserve `Contents/MacOS/assets/ocr` through a relative
`assets` link into `Contents/Resources/assets`. This lets code signing seal the
manifest and models as data. Runtime lookup normalizes `../..` before following
that link, so executable code remains in `Contents/MacOS`.
