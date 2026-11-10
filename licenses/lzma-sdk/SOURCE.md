# LZMA SDK installer decoder

- Upstream: <https://7-zip.org/sdk.html>
- Version: 26.05, released 2026-10-21
- Source archive: <https://github.com/ip7z/7zip/releases/download/26.05/lzma2605.7z>
- SHA-256: `668fda469b7f584f246008cf28359c66ef98d65fafb3b22a52672eebe6970c28`
- License: public domain, as stated in the original `DOC/lzma-sdk.txt` preserved
  in `LICENSE.txt`.

Snow Shot compiles the SDK's reduced C 7z/LZMA/LZMA2/BCJ2 decoder into a temporary,
native Windows installer helper. It does not distribute the general 7-Zip program,
load a generic 7z DLL, or install this helper with the application. The SDK's
prebuilt `7zr.exe` is used only by the build host to create payload archives.

The SDK is fetched and integrity pinned by `cmake/SnowShotInstallerPayload.cmake`.
The application-owned extraction/path/integrity policy is separate from the
unchanged upstream decoder source.
