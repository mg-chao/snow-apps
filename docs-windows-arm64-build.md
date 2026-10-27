# Snow Shot on Windows ARM64

Snow Shot and Mini target native ARM64 on Windows 11. Builds are supported from
x64 Windows hosts and ARM64 Windows hosts. ARM32 and ARM64EC are not supported.
Existing x64 presets, artifact names, and update feeds retain their identities.

## Prerequisites

Use PowerShell 7.2+, CMake 4.2+, Visual Studio 2026/MSVC 14.51 or newer, the matching
Windows SDK ARM64 libraries, and Rust 1.97.1 or newer. Install the Visual Studio C++ ARM64
build-tools component, including `armasm64.exe`. A native ARM64 host uses
`HostARM64/arm64`; an x64 host uses `Hostx64/arm64`.

The OCR worker enables the `gpu` Cargo feature on both Windows architectures.
Its host-side shaderc build requires Python, CMake, and the host C++ compiler;
the resulting SPIR-V shaders are embedded. Deployment needs no Vulkan SDK or
shader compiler. Hardware acceleration defaults off and requires a physical GPU
with the Vulkan 1.3 features accepted by `simd-paddle-ocr-rs`. Missing loaders or
unsupported devices fall back to CPU without blocking recognition on discovery.

Keep the checkout and build directories short: shaderc's bundled SPIRV-Tools has
long object filenames that can exceed MSVC's 260-character path limit even when
Windows long paths are enabled. For runtime-only builds in a deep checkout, use
`scripts/prepare-snow-shot-ocr-runtime.ps1 -Architecture arm64 -BuildDirectory build/ocr-arm64`.
For application builds, use a short checkout root or configure CMake with a shorter `-B` path.

Rust bindgen loads libclang into the Rust host process. Native ARM64 Rust requires
an ARM64 `libclang.dll`; place it under `.tools/llvm-arm64/bin`, select a native
Visual Studio LLVM installation, or set `LIBCLANG_PATH` to its directory. An x64
Rust host uses x64 libclang even when building ARM64 application binaries.

Toolchain versions in `scripts/build-toolchain.json` are minimums. Build scripts
select the newest supported installed MSVC toolset and use the active Rust
toolchain. Bootstrap adds Rust components and targets to that toolchain. vcpkg
accepts installed host tools meeting its own minimums and downloads them when needed.

ARM64 dependencies are isolated under `.tools/vcpkg/installed/arm64/{static,dynamic}`.
Host tools use the physical host triplet independently of the application target.

ARM64 MSVC builds use optimized native C/C++ objects without MSVC LTCG. The
19.51.36252 backend generated stack-cookie return sequences that lost the return
address, causing a CPU spin during sorting and preventing the running application
from reopening. The application, static Qt, and static vcpkg triplet share this
policy; `/GS`, `/sdl`, ordinary optimization, and Rust LLVM LTO remain enabled.
Rebuild dependencies after the triplet change. Old ARM64 Qt kits stamped with
`Ltcg: true` are rejected; use a distinct installation/build directory or `-Force`
to rebuild the kit. Full and Mini packaging also audit every staged executable,
including the OCR worker and crash handler, for the demonstrated malformed return
sequence. Native regression coverage includes singleton activation-key sorting,
INI settings, and time-zone enumeration.
Previously published OCR runtime bytes are immutable. A production OCR update
must use a new runtime version and trusted architecture descriptor before upload;
local verification assets do not replace the public runtime's pins.

## ARM64 acceleration and focused validation

First-party pixel pipelines select baseline AArch64 NEON for capture channel
conversion, half-float/HDR conversion and screen-color restoration, GDI dirty-row
scans, recording area resize, BGRA extraction and keyboard compositing,
scrolling-image ORB and region analysis, OCR detection preprocessing/postprocessing,
canvas color effects, Gaussian reduction/reconstruction, pen masks, clipboard DIB
decoding, and integer alpha classification. Raster-store CRC32C selects hardware
instructions only after an OS capability check. Optional FP16, dot-product, SVE,
and SME instructions are not required by these first-party kernels. Existing scalar tails and unsupported
architecture fallbacks retain the same bounds and rounding contracts.

The x265 overlay enables baseline C++ NEON primitives independently of GAS in
both 8-bit and Main10 builds. Windows ARM64 assembly remains disabled by default,
and assembly-only primitives retain their C implementations. An experimental
GAS-preprocessor/ARMASM64 route is available for native validation; see the
[x265 overlay instructions](cmake/vcpkg-overlay-ports/x265/README.md). The focused
`test-support/x265-arm-intrinsics` fixture checks primitive execution, the combined
8-bit/Main10 API, and complete scalar/NEON bitstream equality for each depth.

OpenCV's MSVC FP16/dot-product variants retain upstream's compiler restrictions.
FFmpeg's unstable AArch64 scaler backend remains disabled because its host helper
does not work with the current cross-toolchain setup; current conversions use
the legacy API, which already has ARM assembly. Baseline OpenCV/FFmpeg NEON and
The simd-paddle-ocr-rs CPU kernels remain available. These restrictions are separate from
the first-party backends and the compiler correctness workarounds above.

The rotation, laser, real-time/GPU recording, paired recording comparison, and
scrolling-image performance runners accept `-Architecture arm64`. They select
`snow-shot-msvc-arm64-performance`, matching Rust targets and dependency triplets,
and require a native matching host when executing measurements. The real-time
runner's `-BuildOnly` mode also supports cross compilation. Runner fixtures can be
checked without running a benchmark or requiring Windows build tools:

```powershell
./scripts/test-arm64-performance-runners.ps1
./scripts/run-capture-rotation-perf.ps1 -Architecture arm64
./scripts/run-snow-shot-scrolling-image-performance.ps1 -Architecture arm64
```

SIMD tests assert backend execution as well as output parity; running portable
reference tests alone does not prove that an ARM kernel was selected. Use only
the affected tests, such as the canvas filter SIMD selector, the Snow Image alpha
and CRC selectors, clipboard DIB tests, or the corresponding Rust module filters.

## Bootstrap Qt and dependencies

From the repository root, install the static dependency graph first:

```powershell
./scripts/bootstrap.ps1 -Architecture arm64 -VcpkgVariants Static -SkipQtValidation
```

On an x64 host, prepare a same-version x64 Qt kit containing `moc`, `rcc`, `uic`,
`lrelease`, and `lupdate`, then cross-build the ARM64 target kit:

```powershell
./scripts/build-static-qt.ps1 -Architecture arm64 `
  -InstallPrefix .tools/qt/6.12.0/msvc2026_arm64-static-system-codecs `
  -HostQtPrefix .tools/qt/6.12.0/msvc2026_64-static-system-codecs
$env:SNOW_QT_HOST_PREFIX = "$PWD/.tools/qt/6.12.0/msvc2026_64-static-system-codecs"
$env:SNOW_QT_STATIC_DIR = "$PWD/.tools/qt/6.12.0/msvc2026_arm64-static-system-codecs/lib/cmake/Qt6"
```

On an ARM64 host, use the same target-kit command without `-HostQtPrefix`.
The scripts verify target library architecture and host-tool architecture rather
than trusting kit directory names. New static Qt stamps include target/host
architecture and host-tool hashes; audited legacy x64 stamps remain accepted
after binary architecture checks.

For development, install `Dynamic` dependencies and select a matching Debug Qt
kit. `build-static-qt.ps1 -Architecture arm64 -Configuration Debug` builds a
development kit with Qt Test and Concurrent. Performance builds also require
those modules; pass `-DevelopmentModules` when preparing a Release kit for them.

The Windows Qt source policy includes `qtbase-graphicsview-exposed-rect.patch`.
MSVC 19.51 ARM64 can miscompile Qt 6.12's integer rectangle adjustment followed
by conversion to `QRectF`: a negative paint boundary such as `-1` becomes
`4294967295`. The scene index then culls visible OCR text on full viewport
repaints; interior repaints after interaction can appear to repair it. The patch
converts the exposed rectangle to floating-point coordinates before expanding
it, retaining the scene index and OCR glyph caches. Rebuild Qt and relink Snow
Shot; changing application code alone does not replace a statically linked Qt.
The source-patch fingerprint rejects kits built without this correction.

The focused `snow-shot-qt-graphicsview-rendering-tests` target checks full and
partial damage, first-frame text, negative scene coordinates, transformed views,
and fractional device pixel ratios. It is also available without building the
app through `test-support/qt-graphicsview`. Validate an optimized ARM64 build on
Windows ARM64, since an x64 or Debug pass does not cover this compiler failure.

## Build and prepare OCR

The configure/build presets are `snow-shot-msvc-arm64-debug`, `performance`,
`release`, and `fast`, with the common `snow-shot-msvc-arm64-` prefix. Debug and
performance have matching `test-` presets. The NSIS package preset is
`package-snow-shot-msvc-arm64-release`.

The ARM64 OCR runtime must be built before its real hashes can be pinned:

```powershell
./scripts/prepare-snow-shot-ocr-runtime.ps1 -Architecture arm64
```

This builds the worker and stages a versioned ARM64 runtime archive, checksum,
runtime report, and trusted asset manifest under `build/snow-shot-msvc-arm64-release`.
It does not upload anything. The manifest preserves the existing model hashes and
protocol 6, and records the actual ARM64 worker bytes. A cross build
is marked as requiring native validation.

Use the prepared archive for local packaging:

```powershell
./scripts/package-snow-shot.ps1 -Architecture arm64 `
  -OcrRuntimeArchive build/snow-shot-msvc-arm64-release/snow-ocr-runtime-1.1.0-windows-arm64.zip
```

Use the runtime version reported by the preparation command if it differs from
the example. This stages both editions, their installers, portable archives,
update archives, manifests, checksums, and symbols. All shipped application,
helper, and DLL payloads must be ARM64; the standard NSIS bootstrapper is retained.
The local archive must match the descriptor generated by preparation, or the
checked-in ARM64 descriptor once available. Packaging never creates new trust
pins from an otherwise unapproved archive.

For ordinary development builds, configure `SNOW_SHOT_OCR_ASSET_MANIFEST` to the
generated trusted manifest, or commit the verified ARM64 descriptor as
`snow_shot/packaging/snow-shot-ocr-asset-manifest-arm64.json` after verifying it
against the real prepared archive. The descriptor may be committed before the
separately authorized runtime upload. Until that descriptor exists, building the
runtime is supported and managed application OCR staging reports the missing
prerequisite.

```powershell
./scripts/build.ps1 -Preset snow-shot-msvc-arm64-fast
./scripts/run-snow-shot.ps1 -Preset snow-shot-msvc-arm64-fast -Edition Mini
ctest --preset test-snow-shot-msvc-arm64-debug -R '^snow-shot-ocr-assets-tests$'
```

Run ARM64 executables on an ARM64 Windows machine. Cross packaging performs
static integrity checks and does not execute target binaries on an x64 host.
Native validation must cover the exact final artifact hashes before production
publication. Use only the performance preset for ARM64 benchmarks and run only
the affected CTest tests or individual Rust crates.

On the matching native host, validate the final archived payloads and write the
proof files used by publication:

```powershell
./scripts/test-snow-shot-native-package.ps1 -Architecture arm64 `
  -BuildDirectory build/snow-shot-msvc-arm64-release `
  -OutputDirectory build/native-validation
```

This verifies archive inventories and executes startup, updater, and worker
identity probes. Native release acceptance also requires OCR with CPU and
CPU inference, offline OCR without network access, clipboard fallback, capture,
H.264/H.265 recording, crash handling, update recovery, and explicit installer
replacement with settings/history retained. Check both editions on Windows 11
ARM64 hardware; a successful static audit cannot establish these results.
Carry these proof files back with the unchanged package artifacts when the
release is signed on an x64 host. Supply `-NativeValidationDirectory` to
`publish-snow-shot-release.ps1` and `-Arm64OcrRuntimeArchive` for the prepared
runtime until the checked-in ARM64 descriptor is available. The publisher still
downloads and verifies the public OCR archive before it permits production
publication.

## Installation, updates, and release gates

An explicitly run ARM64 installer replaces the existing edition, including an
x64 installation running under emulation, while preserving settings/history.
Full and Mini retain separate installation identities. The ARM64 installer checks
the native Windows architecture before stopping applications or uninstalling.

An installed updater follows its compiled architecture. Existing x64 copies on
ARM64 PCs continue using `latest-version.json` or `latest-version-mini.json`.
Native copies use `latest-version-windows-arm64.json` or
`latest-version-mini-windows-arm64.json`. Signed schema 1 remains unchanged;
opposite-architecture metadata and packages are rejected. New installation
records include platform identity; missing identity in a legacy record means x64.

New complete Windows releases include both architectures and both editions.
WinGet and Scoop metadata select the matching architecture; historical x64-only
releases remain readable. Build and package locally, then supply the final x64 and
ARM64 artifacts and matching native validation evidence to
`scripts/publish-snow-shot-release.ps1`. Cross-build reports cannot substitute for
native execution validation.

The ARM64 OCR archive must be published separately to the existing ModelScope
runtime directory before production publication. Production preflight must download
the canonical public runtime archive and verify its pinned size and SHA-256; it
also requires native validation evidence for the exact final package bytes.
Committing the prepared descriptor does not satisfy these gates. Preparing local
artifacts neither publishes the application nor changes the public update feeds.

The OCR worker uses the submodule-pinned `nightly-2026-09-08` compiler and
`aarch64-pc-windows-msvc` standard library. Bootstrap installs them separately
from the Rust toolchain used by other packages. OCR 1.1.0 ships no inference DLLs.
