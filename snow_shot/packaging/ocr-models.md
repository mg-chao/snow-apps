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

When changing models, update both the trusted asset manifest and the packaging
descriptors, and regenerate/check
`cmake/vcpkg-overlay-ports/onnxruntime/required_operators.config` from original
and fully CPU-optimized ONNX graphs. Include operators introduced by graph
optimization, not just those present in the uploaded graphs.

The release maintainer must publish the exact hash-pinned runtime archive before
shipping the updated app or expecting clean development machines to download it:

```text
https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/runtime/1.0.5/windows-x64/snow-ocr-runtime-1.0.5-windows-x64.zip
```

`scripts/package-snow-shot.ps1 -PrepareOcrRuntimeOnly` prepares the runtime ZIP,
checksum, and descriptor without producing an installer. Keep the manifest's
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
run the built test executable with the packaged 1.0.5 worker:

```powershell
$env:SNOW_TEST_OCR_TEXT_FIXTURE = (Resolve-Path snow_shot/tests/baselines/ocr-model-versions.png).Path
$test = 'build/windows-msvc-debug/snow_shot/test-bin/Debug/snow-shot-ocr-recognition-service-tests.exe'
$modelRoot = (Resolve-Path build/ocr-versioned-models).Path
$worker = (Resolve-Path artifacts/snow-ocr-runtime-1.0.5/snow-ocr-process-1.0.5-windows-x64.exe).Path
& $test "--model-root=$modelRoot" "--worker=$worker"
& $test "--model-root=$modelRoot" "--worker=$worker" --directml
```

Both runs must recognize `Snow Shot 12345` and `文字识别` with all four bundles
(ignoring model-dependent spacing between Latin words).
Initialization-only checks (`--validate-model-set`) are also required but do not
replace recognition tests. DirectML requests retain the existing CPU fallback
when acceleration is unavailable. Model payloads are not committed or downloaded
by deterministic unit tests.
