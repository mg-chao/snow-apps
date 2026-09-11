# Snow Shot on macOS

The Apple Silicon port targets the Qt/Rust application. It adds native display
and window capture through ScreenCaptureKit, window snapping, and global
keyboard shortcuts. Annotation, export, and history use the shared Qt code.

This is an initial screenshot port. Windows UIA/MSAA element selection,
global mouse gestures, recording, managed OCR asset installation, automatic
updates, and login-item registration are separate platform work. Building the
OCR helper does not make the Windows-only managed OCR distribution usable on
macOS. The image viewer is not included in this preset.

## Build

Use an Apple Silicon Mac with Xcode command-line tools, Homebrew, and rustup.
The source requires macOS 14 or later for `SCScreenshotManager`. Homebrew bottles
can require a newer macOS release; the packager computes the minimum supported
version from the actual bundled Mach-O libraries instead of promising that a
binary built with newer bottles will run on macOS 14.

```sh
scripts/bootstrap-macos.sh
scripts/build-macos.sh
```

The bootstrap keeps Qt 6.11.1, the pinned zlib-ng 2.3.3 build, and the license
collector runtime in `.tools/`. Other native dependencies use Homebrew, with
FFmpeg pinned to the `ffmpeg@8` formula to match the Rust bindings. Rust
uses the repository's pinned toolchain. The preset uses two build jobs and
disables unity builds so independent translation units also compile correctly.
Override concurrency with `SNOW_BUILD_JOBS` and `CARGO_BUILD_JOBS` when needed.

Run the development app with:

```sh
build/macos-arm64/snow_shot/snow_shot.app/Contents/MacOS/snow_shot --show-main-window
```

The app normally lives in the menu bar. The default screenshot shortcut is F1;
depending on the keyboard's function-key setting, this may require Fn+F1.
Allow screen recording in System Settings when macOS requests it. Keyboard
shortcuts use Carbon hot-key registration and do not require an input-monitoring
event tap. Qt's portable `Ctrl` modifier corresponds to Command by default.

## Focused validation

Keep the repository's full test suite disabled for port validation:

```sh
scripts/build-macos.sh -DSNOW_SHOT_BUILD_MACOS_TESTS=ON
cmake --build build/macos-arm64 --parallel 2 --target \
    snow-shot-macos-shortcut-tests snow-shot-macos-capture-smoke-test
ctest --test-dir build/macos-arm64 -R '^snow-shot-macos-shortcut-tests$' --output-on-failure
cmake -P cmake/tests/rust-target-tests.cmake

cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos -p snow-ui-selector-c --lib
cargo test --manifest-path snow-crates/Cargo.toml -p snow-capture --lib macos_layout
cargo test --manifest-path snow-crates/Cargo.toml -p snow-capture --lib convert::
cargo clippy --manifest-path snow-crates/Cargo.toml \
    -p snow-macos -p snow-capture -p snow-ui-selector-c --lib -- -D warnings
```

The shortcut test covers mapping, native registration, application event
dispatch, and releasing registrations. It does not synthesize physical keystrokes.

The capture smoke test is interactive and requires an active desktop session
and screen-recording permission. It displays a temporary red/blue fixture and
checks pixel orientation, RGBA/BGRA channel order, and alpha:

```sh
ctest --test-dir build/macos-arm64 -R '^snow-shot-macos-capture-smoke-test$' --output-on-failure
```

## Package

```sh
python3 scripts/package-macos.py
```

The script copies the app to temporary staging, generates a self-contained Qt
bundle, includes the exact native dependencies and license notices, verifies
every Mach-O load command, runs the built-in startup probe with isolated Qt and
dyld search paths, and creates an ARM64 DMG plus a SHA-256 file under
`build/macos-arm64/package/`. Existing packages are not overwritten; use
`--output-dir` for another build. `--pwsh` selects an existing PowerShell runtime.

The default signature is ad hoc and is not Apple notarization. Distribution
with a Developer ID and notarization requires a separate signing setup.

Before sharing a package, mount the DMG and launch a copy outside the build
tree. Verify selecting a region, drawing an annotation, saving the result,
repeating a capture with the global shortcut, and capture on each connected
display. A successful link alone is not an end-to-end screenshot test.

## Review boundaries

Generic Clang fixes make integer conversions and default member initialization
explicit, remove unused internal helpers, and place Qt metatype declarations
at global scope. Strict warnings remain errors. On macOS, the app uses the ordinary
`-Wswitch` coverage from `-Wall`: it permits intentional catch-all Qt event
filters and exhaustive enum switches without requiring both styles at once.
Exact floating-point equality remains intentional for cache identity checks.

macOS platform code is conditionally selected. Windows presets, native capture
implementations, and release packaging remain the Windows path; a macOS build
cannot substitute for running the Windows validation before upstream merge.
