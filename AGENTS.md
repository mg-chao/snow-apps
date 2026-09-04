# Repository Guidelines

## Project Structure & Module Organization
- `snow_shot/` – Snow Shot app (GPL-3.0). Code lives in `src/{app,image,network,platform,presentation,storage}`, tests in `tests/`, assets in `resources/` and `i18n/`.
- `snow_image_viewer/`, `snow_image/`, `ant_design_qt/`, `snow_draw_engine_qt/` – Qt/C++ libraries and the viewer app; each has its own `CMakeLists.txt`, `tests/`, and license file.
- `snow-crates/crates/*` – Rust workspace (`snow-capture`, `snow-ocr-process`, …); `snow_rust_ffi/` bundles the C FFI static library consumed by CMake.
- `cmake/` – shared modules, `vcpkg-overlay-ports/`, `vcpkg-overlay-triplets/`, `test-support/`. `scripts/` – PowerShell entry points. `build/<preset>/` and `.tools/vcpkg/` are generated; never commit them.

## Build, Test, and Development Commands
- Toolchain: VS 2026 x64 developer shell, CMake 4.2+, MSVC 14.51, Rust 1.97.1, repository-managed vcpkg/Qt.
- `scripts/bootstrap.ps1` – installs vcpkg deps and validates Qt/MSVC once per machine.
- `scripts/build.ps1 -Preset windows-msvc-debug [-Target snow_shot] [-Clean]` – configure + build (`snow-all` by default). Presets: `windows-msvc-debug`, `windows-msvc-performance`, `snow-shot-msvc-release`, `snow-shot-msvc-fast`.
- `ctest --preset test-windows-msvc-debug -R <regex>` – run only matching tests after a change. Do not run unfiltered `ctest --preset …` (full suite) unless the user explicitly asks.
- Performance benchmarks: build **Release** with `windows-msvc-performance` (e.g. `cmake --build --preset build-windows-msvc-performance --target <benchmark>`, or an existing `**/scripts/run-*-perf.ps1`). Do not use Debug, `snow-shot-msvc-release`, or `snow-shot-msvc-fast` (`SNOW_APPS_BUILD_BENCHMARKS=OFF`).
- `scripts/run-snow-shot.ps1` / `scripts/run-snow-image-viewer.ps1` – launch from the build tree.
- `scripts/check-cpp-format.ps1 [-Fix]`, `scripts/check-rust.ps1 [-Fix]` – format and lint (also CMake targets `snow-format`, `snow-lint`).
- `scripts/package-snow-shot.ps1` – NSIS installer (`snow-shot-msvc-release`).

## Coding Style & Naming Conventions
- C++: `.clang-format` (LLVM base, 4 spaces, 100 columns, attached braces, `T* ptr`, includes unsorted). Strict warnings are on; `.clang-tidy` is enforced when `SNOW_APPS_ENABLE_CLANG_TIDY=ON`.
- Headers are lowercase without separators (`screenshothistoryservice.h`), namespaces `snow_shot::presentation`, classes `PascalCase`, functions `camelCase`. (snow_image_viewer and ant_design_qt use snake_case filenames; match the file you edit.)
- Rust: edition 2024, `rustfmt.toml` (100 columns), `cargo clippy -D warnings` must pass. Crates are kebab-case `snow-*`.
- `.editorconfig`: UTF-8, LF, final newline; JSON/TOML/YAML use 2 spaces.

## Testing Guidelines
- After any code change, run **only** the tests that cover those changes. Running the full suite is **strictly prohibited** unless the user explicitly asks.
- Tests are plain executables using a `require(condition, message)` helper (no external framework). snow_shot registers them as `add_test(NAME snow-shot-<feature>-tests …)` in `snow_shot/CMakeLists.txt`; other projects use their own prefixes (`adqt-*`, `snow-canvas-*`, `snow_image_viewer_*`).
- Name files `<feature>_tests.cpp`; benchmarks `<feature>_performance_benchmark.cpp` or `<feature>_benchmark.cpp`; UI Automation runs `<feature>_uia_e2e_test.cpp`.
- CTest labels: `unit` (default), `interactive`/`e2e`, `benchmark`, `windows`. Default presets exclude interactive, e2e, and benchmark labels.
- When running performance benchmarks, compile with **Release** via `windows-msvc-performance`. Debug and packaging presets do not build benchmark targets. Still run only the related benchmark, not the full performance suite.
- Rust tests are mostly inline `#[cfg(test)]` modules (a few crates also have `tests/` integration tests). After a crate change, run `cargo test -p <crate>` (optionally `-- <test_name>`) inside `snow-crates/`. Do not run `cargo test --workspace` unless the user explicitly asks.
- Cover every behavioral change with a deterministic, offscreen-capable test.

## Commit & Pull Request Guidelines
- Conventional Commits, lowercase imperative: `fix(screenshot): stop cursor flicker during smart frame selection`. Types in use: `feat`, `fix`, `refactor`, `perf`, `test`, `style`, `build`. Scopes mirror features (`recording`, `pinned`, `dpi`, `ocr`, `toolbar`).
- Work on `feat/YYYYMMDD_main_NN` branches; release tags `v*_snow-shot` trigger `.github/workflows/snow-shot-release.yml`.
- PRs: state motivation, list affected presets/targets, link issues, and confirm format, lint, and related tests pass. Do not run the full suite for a PR unless the user explicitly asks.

## Licensing & Configuration
- Multi-license repo: keep Apache-2.0 code in library directories and GPL-3.0 code in `snow_shot/`, `snow_image/`, `snow_image_viewer/`. After adding a dependency, run `scripts/collect-third-party-licenses.ps1` and update the project's `THIRD_PARTY_NOTICES.md` (`snow_shot/`, `ant_design_qt/`).
- `SNOW_APPS_API_BASE_URL` is set per preset; never hard-code endpoints or secrets.
