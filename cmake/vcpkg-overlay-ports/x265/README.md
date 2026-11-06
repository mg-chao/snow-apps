This overlay retains vcpkg x265 4.1 and its existing patches.

Windows Snow Shot requests `x265[main8]`. It records and exports SDR with the
8-bit software HEVC encoder; hardware HEVC and HEVC decoding remain in FFmpeg.
The application profile builds no Main10/Main12 encoder archives and sets
`ENABLE_DYNAMIC_DEPTH=OFF`, so capability queries cannot load another encoder
from a DLL search path. The dynamic-depth patch affects both `x265_api_get` and
`x265_api_query`; unavailable depths return null and the query reports
`X265_API_QUERY_ERR_LIB_NOT_FOUND`.

`main10` remains an independent feature. The Windows
`ffmpeg[snow-recording-compat]` profile requests it for standalone HDR callers,
and the macOS media/application profiles retain their existing Main10 settings.
`main8` and `main10` can be selected together to build the combined API; this
preserves the compatibility profile's inherited application codecs. The Windows
application packaging audit rejects a prefix containing that combined encoder.

The installed `share/x265/snow-encoder-capabilities.json` uses schema version 2.
Its `bitDepth8`, `bitDepth10`, `bitDepth12`, `singlePublicApi`, and
`dynamicDepthLoading` booleans come from the actual configured CMake caches,
including triplet overrides. Release and Debug must agree. The `main8` feature
without `main10` requires exactly the public 8-bit API with dynamic depth loading
disabled. This replaces the old hardcoded `snow-main10-capability.json`; an old
prefix cannot satisfy the new Windows application contract.

For a native API/encode check, compile `encoder_api_check.cpp` against the
selected prefix's x265 headers/library and run `--8bit-only` (the default) or
`--combined`. Both modes encode three 64x64 frames at each expected depth and
check API struct sizes. The 8-bit-only mode also checks that both get/query APIs
reject 10/12-bit encoding. This is an encoding correctness check, not a benchmark.

`short-propagation-row.patch` skips the SIMD loop when a CU-tree row
contains fewer elements than a vector. The original do/while-style loop
processed a full vector and then processed the remainder again, writing
past `Lookahead::m_scratch` for very narrow recordings. Both SSE2 and AVX
variants now enter their existing scalar remainder paths directly.
Normal-sized rows keep the same instructions and quality settings.
The recording-export HDR trim fixtures exercise 32-pixel-wide clips.

`arm64-intrinsics-without-assembly.patch` separates baseline AArch64 NEON C++
primitives from the GAS assembly build. Windows ARM64 keeps assembly disabled,
but enables the independent pixel, interpolation, transform, loop-filter,
intra-prediction, and SAO kernels in both 8-bit and Main10 builds. Four
registrations inside those C++ setup functions refer to GAS implementations:
16x16 DCT/IDCT and the 8-bit 8x8/16x16 planar predictors. They retain C fallbacks,
as do the other primitives available only in the separate assembly tables. This
route advertises only baseline NEON, uses MSVC's `arm64_neon.h` and portable
alignment, and does not enable FP16, dot-product, I8MM, or SVE instructions.

The focused fixture in `test-support/x265-arm-intrinsics` takes a patched x265
4.1 source directory as `SNOW_X265_SOURCE` and builds with assembly disabled.
Configure it separately with `SNOW_X265_HIGH_BIT_DEPTH=OFF` and `ON` to check
8-bit and Main10 primitive parity and scalar/NEON bitstream equality.
The dispatch test requires actual NEON function replacement and execution, and
checks that assembly-only registrations preserve their C pointers. Upstream's
four primitive test groups run with `--cpuid NEON --nobench`; the bitstream tests
run scalar and NEON encoders in separate processes to avoid x265's global
primitive-table cache.

To validate the standalone/macOS combined API locally, first build the private Main10
archive with `EXPORT_C_API=OFF`, then pass it as `SNOW_X265_MAIN10_ARCHIVE` to the
8-bit fixture. This sets the same `EXTRA_LIB` and `LINKED_10BIT=ON` options as the
port. The combined API test compares `x265_api_get(10)` directly with the private
`x265_10bit` API pointer, so dynamic fallback cannot satisfy the test. Both API
depths then encode scalar and NEON clips in separate processes; Main10 uses
16-bit sample storage and checks its internal and picture depth explicitly.

For native macOS ARM64 correctness checks, with the complete patched x265 4.1
tree at `build/patched-x265-4.1`, run:

```sh
x265Source="$PWD/build/patched-x265-4.1"
cmake -S "$x265Source/source" -B build/x265-neon-private-main10 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DVERSION=4.1 \
  -DENABLE_ASSEMBLY=OFF -DENABLE_ARM_INTRINSICS=ON \
  -DENABLE_NEON_DOTPROD=OFF -DENABLE_NEON_I8MM=OFF -DENABLE_SVE=OFF -DENABLE_SVE2=OFF \
  -DENABLE_CLI=OFF -DENABLE_SHARED=OFF -DENABLE_PIC=ON -DENABLE_LIBNUMA=OFF \
  -DHIGH_BIT_DEPTH=ON -DMAIN12=OFF -DEXPORT_C_API=OFF
cmake --build build/x265-neon-private-main10 --target x265-static
cmake -S test-support/x265-arm-intrinsics -B build/x265-neon-combined -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DVERSION=4.1 \
  -DSNOW_X265_SOURCE="$x265Source" -DSNOW_X265_HIGH_BIT_DEPTH=OFF \
  -DSNOW_X265_MAIN10_ARCHIVE="$PWD/build/x265-neon-private-main10/libx265.a"
cmake --build build/x265-neon-combined --target TestBench x265-arm-intrinsics-dispatch-tests
ctest --test-dir build/x265-neon-combined -R '^x265-arm-' --output-on-failure
```

These are codec correctness tests with upstream timing disabled. They do not
replace native Windows MSVC/ARMASM64, ABI, or recording integration validation.

`windows-arm64-gas-assembly.patch` provides an experimental Windows ARM64
assembly route using the same pinned Perl/GAS-preprocessor tools already used
by the x264 and FFmpeg ports, followed by the existing MSVC ARMASM64 tool. It
normalizes ELF comments and local labels, adds quoted custom build commands,
and rejects optional dot-product, I8MM, SVE, and SVE2 configurations. Both the
normal 8-bit and isolated Main10 builds receive their own namespace and depth
definitions. Other platforms retain their existing assembly build commands.

Production assembly remains **off**. To opt in for validation, put
`set(SNOW_X265_ENABLE_WINDOWS_ARM64_ASSEMBLY ON)` in a dedicated custom vcpkg
triplet which includes the repository's `arm64-windows-static.cmake` (or shared
equivalent). Use that custom triplet name for the dependency build. vcpkg reads
this variable from the triplet, whose contents are included in the package ABI;
it is not an environment-variable toggle. The port acquires the existing host
tools only for this opt-in route. Keep the repository's `/GL-` and stack-cookie
workarounds in the included triplet.

The focused configuration regression runs the real patched x265 CMake project
for 8-bit and private Main10 builds. On a POSIX host it checks simulated
Windows/MSVC configuration with empty NEON source flags and native assembly
configuration with the existing `-march=armv8-a` flags. It needs CMake and native
C/C++ compilers, but does not compile the encoder or run an assembler:

```sh
python3 test-support/x265-arm-intrinsics/test_configuration.py \
  --source build/patched-x265-4.1
```

The translation fixture also runs this configuration regression, then runs the
actual new CMake commands for all 14 baseline sources at both bit depths,
including paths with spaces, and checks namespaces, assembler arguments, and
rejection of all optional ISA configurations:

```sh
python3 test-support/x265-arm-intrinsics/test_gas_translation.py \
  --source build/patched-x265-4.1 \
  --gas-preprocessor .tools/vcpkg/downloads/tools/gas-preprocessor/9309c67a/gas-preprocessor.pl
```

Here `--source` must point to x265 4.1 with this port's complete patch stack.
The translation fixture uses an existing Clang preprocessor on a POSIX host and
a recording assembler; it does **not** produce or execute machine code.

On a native Windows ARM64 VS 2026 developer PowerShell, validate the real
assembler and encoder separately for both bit depths. Set `$source` to the
patched source directory and `$perl`/`$gaspp` to the host tools acquired by
vcpkg, then run:

```powershell
$armasm = (Get-Command armasm64.exe -ErrorAction Stop).Source
foreach ($depth in @(8, 10)) {
    $build = "build/x265-armasm64-main$depth"
    $high = if ($depth -eq 10) { "ON" } else { "OFF" }
    cmake -S test-support/x265-arm-intrinsics -B $build -G Ninja `
        -DCMAKE_BUILD_TYPE=Release -DSNOW_X265_SOURCE="$source" `
        -DSNOW_X265_HIGH_BIT_DEPTH=$high -DSNOW_X265_MSVC_ASSEMBLY=ON `
        -DPERL_EXECUTABLE="$perl" -DX265_GASPREPROCESSOR="$gaspp" `
        -DX265_ARMASM64_EXECUTABLE="$armasm"
    if ($LASTEXITCODE) { throw "x265 Main$depth configure failed" }
    cmake --build $build --target TestBench x265-arm-intrinsics-dispatch-tests
    if ($LASTEXITCODE) { throw "x265 Main$depth build failed" }
    ctest --test-dir $build -R '^x265-arm-' --output-on-failure
    if ($LASTEXITCODE) { throw "x265 Main$depth validation failed" }
}
```

The native fixture asserts replacement of assembly DCT/IDCT entries and 8-bit
planar predictors, executes pixel operations, runs the four upstream primitive
test groups, and compares scalar/NEON HEVC output from separate processes.
These Windows assembly, linkage, ABI, and execution checks have not been run in
the current macOS environment. Translation and native macOS intrinsics parity
alone are insufficient to enable production Windows assembly.
