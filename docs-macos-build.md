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
bundle includes Qt plugins/frameworks, native dylibs, the OCR helper and updater,
QR models, shutter audio, and project license notices. Executable-relative
assets remain under `Contents/MacOS`; libraries are deployed in
`Contents/Frameworks`. A plain `cmake --install build/<preset> --component
SnowShot --prefix /path/to/staging` performs the same deployment without a DMG.

Ad-hoc signing supports local testing. Public distribution still requires
Developer ID signing and Apple notarization as a separate release operation.
The run script deploys a signed development copy under `build/<preset>/run`
before launching; this also resolves the OCR helper's dynamically loaded libraries.
Launch through the run script/Finder and grant Screen Recording, Accessibility,
and microphone permissions when using the corresponding capabilities.

These build changes do not implement feature parity with Windows. In particular,
the managed OCR download manifest currently describes a Windows x64 payload;
shipping the CPU OCR helper does not port that asset-management contract. The
Windows updater/installer, DirectML, and Crashpad packaging remain Windows-only.
The standalone `bootstrap-macos-media.sh` and `build-macos-media.sh` workflows
remain available for media harness development.
