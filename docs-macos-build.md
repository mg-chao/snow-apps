# Building Snow Shot on macOS

The app presets target macOS 15 or newer, with separate Apple Silicon (`arm64`)
and Intel (`x64`) builds. Each build uses one architecture throughout CMake,
vcpkg and Cargo; universal builds are not supported by these presets.

## Prerequisites

- Xcode command-line tools (`xcode-select --install`) or Xcode, with a macOS 15+ SDK.
- Rust installed through rustup. The checked-in toolchain pins Rust 1.97.1.
- CMake 4.2+, Ninja, pkg-config, Git and Python 3. Intel codec builds also need NASM.
- The official **Qt 6.11.1 macOS** kit, including Qt SVG and Linguist tools.
  The macOS kit must contain the target architecture. Qt is shared; Windows
  static Qt and MSVC settings do not apply.

For example, install host tools with `brew install cmake ninja pkgconf nasm`.
The scripts also recognize tools already installed under `.tools/macos-dev/bin`
and `.tools/macos-media/host/bin`. They do not modify global tool installations.

Set `Qt6_DIR` if Qt is not at `$HOME/Qt/6.11.1/macos/lib/cmake/Qt6`:

```sh
export Qt6_DIR=/path/to/Qt/6.11.1/macos/lib/cmake/Qt6
scripts/bootstrap-macos.sh
scripts/build.sh
scripts/run-snow-shot.sh
```

Bootstrap checks the host tools, installs the matching Rust target, bootstraps
repository-local vcpkg at the registry baseline, and configures the project.
The run script rebuilds the `snow_shot` target before launching by default; pass
`--no-build` to launch an existing build or `--clean` to request a clean rebuild.
CMake installs native dependencies from the manifest. The first run builds
FFmpeg, image codecs, OpenCV, and CPU ONNX Runtime and can take considerable time.
Dependencies live in `.tools/macos/installed`, isolated from Windows and the
standalone macOS media harness. `LIBCLANG_PATH` can override Xcode's libclang.

## Presets and targeted checks

Names follow `snow-shot-macos-{arm64|x64}-{debug|performance|release|fast}`.
The default architecture follows the current shell's `uname -m` (including
Rosetta); pass a preset explicitly to select another architecture.

| Mode | CMake configuration | Tests | Benchmarks | Packaging |
| --- | --- | --- | --- | --- |
| debug | Debug | Enabled | Disabled | Disabled |
| performance | Release | Enabled | Enabled | Disabled |
| release | Release with optimization | Disabled | Disabled | DMG |
| fast | Release without C++ LTO, Cargo release-fast | Disabled | Disabled | Disabled |

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target snow-shot-ocr-layout-tests
ctest --preset test-snow-shot-macos-arm64-debug -R '^snow-shot-ocr-layout-tests$'
scripts/build.sh snow-shot-macos-arm64-fast --clean
scripts/build.sh snow-shot-macos-x64-release
scripts/run-snow-shot.sh snow-shot-macos-arm64-debug -- --help
python3 scripts/test-macos-build-support.py
# Optional native bundle/DMG fixture after provisioning Qt and FFmpeg:
SNOW_TEST_MACOS_BUNDLE=1 python3 scripts/test-macos-build-support.py MacOSBundle
```

The native deployment fixture tests the packaging rules with a small Qt executable
and a simulated versioned OCR library; it does not validate the full app.
It requires CMake, Ninja, pkg-config and CPack on PATH.

Only run tests covering your change. Test presets exclude Windows-only,
interactive, end-to-end and benchmark labels. Build a benchmark explicitly with
`--target` using the performance preset. Running an Intel executable on Apple
Silicon requires Rosetta. `build.sh` defaults to the application target; use
`--target snow-all` only when all enabled targets are needed.

For direct CMake use, put the host tools on PATH and supply `-DQt6_DIR=...`:
`cmake --preset snow-shot-macos-arm64-debug`, then
`cmake --build --preset build-snow-shot-macos-arm64-debug`.
The shell wrapper also sets Xcode libclang discovery for Rust bindgen.

Formatting and lint entry points are `scripts/check-cpp-format.sh [--fix]` and
`scripts/check-rust.sh PACKAGE [-- CARGO_OPTIONS...]`. The `snow-format` and
`snow-lint` CMake targets use native shell scripts on macOS; `snow-lint` checks
the CPU OCR helper. Other changed Rust crates should be linted individually.

## App bundles and packaging

```sh
scripts/package-snow-shot.sh snow-shot-macos-arm64-release
```

The release script builds and produces a DMG plus SHA-256 checksum under the
preset's build directory. CPack installs only the Snow Shot component, runs the
matching Qt kit's `macdeployqt`, and verifies the ad-hoc bundle signature. The
bundle includes Qt plugins/frameworks (including offscreen startup-probe support), native dylibs, the OCR helper and updater,
QR models, shutter audio, and project license notices. Executable-relative
asset lookup paths remain under `Contents/MacOS`; deployment moves data into
`Contents/Resources` and creates relative `assets`/`audios` directory links so
code signing seals them as resources. Libraries are deployed in `Contents/Frameworks`. A plain `cmake --install build/<preset> --component
SnowShot --prefix /path/to/staging` performs the same deployment without a DMG.

Ad-hoc signing supports local testing. Public distribution still requires
Developer ID signing and Apple notarization as a separate release operation.
The run script deploys a signed development copy under `build/<preset>/run`
before launching; this also resolves the OCR helper's dynamically loaded libraries.
Launch through the run script/Finder and grant Screen Recording, Accessibility,
and microphone permissions when using the corresponding capabilities.

Apple Silicon OCR uses native CPU inference with all seven existing V4/V5/V6
models. The ARM64 app bundles its worker, ONNX Runtime, and Small V6 model;
other models download on demand into application storage. No OCR runtime code
is downloaded on macOS. Intel OCR qualification is outside this delivery.
The Windows updater/installer, DirectML, and Crashpad packaging remain Windows-only.
The standalone `bootstrap-macos-media.sh` and `build-macos-media.sh` workflows
remain available for media harness development.

## Apple Silicon OCR validation

The runtime uses protocol 3. Its generated schema-3 manifest records
`macos-arm64`, `delivery: bundled`, the executable name, and the size/SHA-256 of
the worker and ONNX library. The existing Windows schema-2 manifest remains the
source of the seven pinned model contracts; its Windows runtime is never staged
on macOS. Runtime binaries live in `Contents/MacOS`; models and the manifest are
accessed through `Contents/MacOS/assets/ocr` (a resource link in signed bundles). The worker loads its adjacent ONNX library by absolute
path, independent of the shell environment. A damaged runtime requires reinstalling
the ARM64 app; verified downloaded models remain reusable.

Build staging checks content on every relevant target build, including a worker-only
change followed by a host build. Model downloads are verified before promotion and
cached under `artifacts/`. Packaging requires a complete Small V6 payload. Deployment
resolves the pinned native dependency closure, rewrites relocatable Mach-O loads,
and signs nested code, generates hashes of the finalized
runtime, then seals the enclosing app. CPack signs and verifies the DMG before
calculating its SHA-256 checksum. No runtime binary may be modified afterward.
The installer writes `macos-ocr-verification.json` in the build directory, listing
verified Mach-O binaries and runtime hashes. This is local integrity validation;
ad-hoc signing is not Developer ID signing or notarization.

Focused deterministic checks (the asset executable needs no worker or real models):

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-ocr-assets-tests
ctest --preset test-snow-shot-macos-arm64-performance -R '^snow-shot-ocr-assets-tests$'
python3 scripts/test-snow-shot-macos-ocr.py
scripts/check-rust.sh snow-ocr-process -- --no-default-features --features dynamic-onnx-runtime
scripts/check-rust.sh rapid-ocr-rs -- --no-default-features --features dynamic-onnx-runtime
```

Build the recognition test target and fetch all verified models for the opt-in
native checks. Use only the performance preset when passing `--measure`:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-ocr-recognition-service-tests
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-ocr-(cpu-recognition-service|process-lifecycle|managed-runtime)-tests$'
python3 scripts/snow-shot-macos-ocr.py fetch-models --model-root build/ocr-versioned-models
export SNOW_TEST_OCR_TEXT_FIXTURE="$PWD/snow_shot/tests/baselines/ocr-model-versions.png"
# Use the recognition test executable emitted by this build:
OCR_TEST="$PWD/build/snow-shot-macos-arm64-performance/snow_shot/test-bin/snow-shot-ocr-recognition-service-tests"
"$OCR_TEST" --model-root="$PWD/build/ocr-versioned-models" --measure
"$OCR_TEST" --resident-text-fixture
"$OCR_TEST" --model-root="$PWD/build/ocr-versioned-models" --resident-models
```

The seven-model run checks bilingual text, geometry, FIFO callbacks and repeated
inference. Measurements report the first cold request, the following request using
the same session, and the worker's resident bytes sampled after each result (not
peak RSS). Cold means a new worker/model session, without flushing the operating
system's file cache. They are hardware-dependent observations, not a latency guarantee.

After packaging, relocate the app to a path containing spaces or Unicode and test
the exact bundle without modifying its signature:

```sh
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --offline
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --model=extra_small --cache="/tmp/snow-ocr-cache"
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --model=extra_small --cache="/tmp/snow-ocr-cache" --offline
python3 scripts/snow-shot-macos-ocr.py verify \
  --runtime-dir="/path/to/Relocated Snow Shot.app/Contents/MacOS" \
  --app="/path/to/Relocated Snow Shot.app"
```

The first run uses a fresh temporary cache and an unreachable download proxy. The
second acquires another model; the third proves cache reuse without network access.
Run with `DYLD_LIBRARY_PATH`, `DYLD_FALLBACK_LIBRARY_PATH`, and `ORT_DYLIB_PATH` unset.
Also launch the packaged app through Finder and check its screenshot-to-OCR flow.

## Pin to Screen

See [macOS pinned-window validation](snow_shot/tests/macos_pinned_windows.md) for
placement semantics, targeted tests, and the hardware qualification checklist.
Pinned images use all Spaces and preserve their backing-pixel size across display
changes; toolbars retain their logical size. Pinned persistence now uses format 2
in `pinned_windows_v2`, leaving previous-version data untouched.
