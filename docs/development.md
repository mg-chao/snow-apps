# Developing Snow Shot

Snow Shot uses C++ / Qt for its desktop interface and Rust for native services.
Start with the platform instructions below, and follow [AGENTS.md](../AGENTS.md)
and the [contribution guidelines](../CONTRIBUTING.md) when preparing a change.

## Get the source

```sh
git clone --recurse-submodules https://github.com/mg-chao/snow-shot.git
cd snow-shot
```

For an existing checkout, initialize its submodules with:

```sh
git submodule update --init --recursive
```

## Windows

The toolchain requires Visual Studio 2026 developer tools, MSVC 14.51,
CMake 4.2+, Rust 1.97.1, and the repository-managed Qt / vcpkg dependencies.
Run these commands from the repository root in PowerShell:

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1 -Preset windows-msvc-debug -Target snow_shot
./scripts/run-snow-shot.ps1
```

For ARM64 prerequisites, native validation, and packaging, follow the
[Windows ARM64 guide](../docs-windows-arm64-build.md).

## macOS

Follow the [macOS build guide](../docs-macos-build.md) to provision Xcode,
Qt 6.12.0, Rust, and the native dependencies. An Apple Silicon development build:

```sh
scripts/bootstrap-macos.sh snow-shot-macos-arm64-debug
scripts/build.sh snow-shot-macos-arm64-debug --target snow_shot
scripts/run-snow-shot.sh snow-shot-macos-arm64-debug --no-build
```

Intel Macs use the corresponding `snow-shot-macos-x64-debug` preset.
Release packaging has additional toolchain and signing requirements; use the
platform guide rather than copying development build output into a release.

## Focused validation

Run only tests covering your change. The full test suite is not part of the
default development workflow. Build a related test target before running it:

```sh
ctest --preset test-windows-msvc-debug -R '<related-test-regex>' --output-on-failure
```

For Rust changes, run `cargo test -p <crate>` inside `snow-crates/`.
Use the platform's **performance** preset for related benchmarks.
See [AGENTS.md](../AGENTS.md) for formatting, linting, translations, and test naming.

## Maintain the OCR submodule

[`snow-crates/crates/simd-paddle-ocr-rs/`](../snow-crates/crates/simd-paddle-ocr-rs/)
is a Git submodule with its own
[repository](https://github.com/mg-chao/simd-paddle-ocr-rs), history, branches,
and releases. The parent repository records the exact OCR commit to check out.
The submodule is excluded from the Snow Crates Cargo workspace so it retains
its own pinned nightly toolchain and lockfile. Run its Cargo commands from
the submodule directory.

Submodule updates normally check out a detached HEAD. Before editing OCR code,
switch to `main` or a feature branch inside the submodule:

```sh
git -C snow-crates/crates/simd-paddle-ocr-rs switch main
```

Follow the submodule's README and AGENTS.md for build and validation commands.
Commit and push OCR changes to its own repository first, then record the new
revision in the parent repository:

```sh
git add snow-crates/crates/simd-paddle-ocr-rs
git commit -m "build(ocr): update simd paddle ocr submodule"
```

## Packaging and releases

- [Release and package-manager maintenance](snow-shot-releases.md)
- [macOS packaging and local signing](../docs-macos-build.md#app-bundles-and-packaging)
- [Windows ARM64 release validation](../docs-windows-arm64-build.md#installation-updates-and-release-gates)
- [Microsoft Store packaging](snow-shot-msix.md)

## README assets

The [README asset notes](images/readme/README.md) document the original feature
illustrations and the design references used for the project homepage.
