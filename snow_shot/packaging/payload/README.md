# Windows installer payload

`snow-shot-payload-extractor` is temporary installer machinery. It uses the
integrity-pinned public-domain LZMA SDK C decoder and the Windows SHA-256 provider.
It does not depend on Qt, Rust, a general 7z DLL, or an installed 7-Zip program.
The executable is embedded in the NSIS plugin directory and is not an application
runtime file.

The native GUI-subsystem entry point creates no window or console:

```text
snow-shot-payload-extractor.exe archive.7z destination expected-sha256 [entry]
```

The three-argument form extracts every payload entry. The optional fourth
argument selects exactly one regular file by its archive-relative path, used to
bootstrap the upgrade helper without writing the entire application before the
old uninstaller runs. It still verifies the complete archive SHA-256 and entry
inventory before writing, and checks the selected solid block/file CRC while
decoding. A selected path is preserved below `destination`.

Exit codes are stable installer protocol values:

| Code | Meaning |
| --- | --- |
| 0 | Extraction completed |
| 1 | Invalid argument count |
| 2 | Archive open/read or expected SHA-256 verification failed |
| 3 | Archive header/format could not be read |
| 4 | Unsafe entry inventory or invalid/missing file selection |
| 5 | Destination creation, replacement, or output write failed |
| 6 | Compressed data or CRC verification failed |
| 7 | Memory allocation failed |

The helper validates all names before creating the destination, rejects traversal,
device/stream/ambiguous Windows paths and archive reparse/special-file metadata,
and refuses reparse-point descendants of the selected installation root. The
root itself is resolved to the user's chosen physical destination. Files are
written beside their target and replaced after successful decoding and writes;
failed replacements preserve the old file and remove the temporary file.

The stage is limited to 100,000 entries and 1 GiB of unpacked files. The creator
limits solid blocks and individual files to 256 MiB; individual decoder
allocations are limited to 512 MiB. Current online and offline payloads
fit within those limits. Integrity and write failures produce a failed NSIS
installation before completion metadata and shortcuts are created.

`scripts/snow-shot-installer-payload.ps1` creates archives using the pinned SDK's
build-host encoder. It checks the actual BCJ2 filter for x64 or ARM64 filter for
ARM64, the exact stage inventory, and archive integrity. Source timestamps are
omitted for reproducibility. The source/notice pin is in
`licenses/lzma-sdk/SOURCE.md`.

Standalone build and focused validation:

```powershell
cmake -S snow_shot/packaging/payload -B build/installer-payload-tests -A x64
cmake --build build/installer-payload-tests --config Release
scripts/test-snow-shot-installer-payload.ps1 `
  -Extractor build/installer-payload-tests/installer-payload/Release/snow-shot-payload-extractor.exe `
  -Encoder build/installer-payload-tests/_deps/snow_lzma_sdk-src/bin/x64/7zr.exe
```

For cross-platform archive testing, `-ExecutableFixture` can select a target PE
while running a native host decoder. That verifies the archive filter/decoder
path; it does not replace native ARM64 deployment validation.
