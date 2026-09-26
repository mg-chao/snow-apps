# Snow Shot Third-Party Notices

Snow Shot incorporates third-party software and assets under their respective
licenses. The release build generates a complete, versioned notice bundle from
the resolved Rust dependency graph, installed vcpkg packages, the audited
static Qt build, and repository-owned attribution files.

Installed releases place that bundle under:

```text
share/snow-shot/licenses/third-party/
```

`INDEX.md` in that directory records every collected package and source
license file. The bundle includes the Ant Design Icons MIT notice from
`ant_design_qt/THIRD_PARTY_NOTICES.md`.

Screen color restoration uses nalgebra (Apache-2.0) for fixed-size matrix
inversion and validation. Its license and resolved dependencies are included
in the generated Rust dependency notice bundle.

The OCR worker uses `ort` and `ort-sys` 2.0.0-rc.13 (MIT OR Apache-2.0), pinned to
revision `90018ee581fb1bb1baf8e82eba8e679728ff27dc` of
https://github.com/mg-chao/ort with a native diagnostic-decoding and logger-category
fix. Cargo retrieves their source and original license files from that fork.
The release collector includes the selected Rust FFI and static OCR-worker
dependency graphs, including these Git dependencies.

The GPL-3.0-only `snow-shot-updater` sidecar is implemented in Rust and is
distributed as part of Snow Shot. Its resolved normal and build dependency
graph—including Tokio, Reqwest with native platform TLS, Serde, RSA/SHA-256,
SemVer, ZIP/Deflate, and Microsoft windows-rs—is collected into the same
versioned notice bundle from its independently locked Cargo package.

The optional PP-OCRv4 and PP-OCRv5 detector/recognizer models and dictionaries
are redistributed from RapidAI/RapidOCR's ModelScope release `v3.9.2`, whose
model card declares Apache License 2.0:
https://www.modelscope.cn/models/RapidAI/RapidOCR/files?Revision=v3.9.2.
Snow Shot downloads the matching mobile/server bundles from
https://www.modelscope.cn/models/mgchao/SnowShotOCR and pins their sizes and
SHA-256 hashes in `packaging/snow-shot-ocr-asset-manifest.json`.

Local crash diagnostics on Windows and macOS use Crashpad (Apache-2.0), pinned by the vcpkg baseline
and the repository's Crashpad overlay. The client, helper, Chromium base code,
and their bundled notices are included in the vcpkg license collection. The
Qt-independent OCR bridge and shared Rust panic hook are Apache-2.0 code under
`snow-crates/`.

In-process barcode recognition and Smart Erase reconstruction use OpenCV
(Apache-2.0), pinned by the vcpkg
baseline and built as static libraries with only the `wechat_qrcode` (contrib)
and `barcode` (objdetect) module closure; the dnn module embeds OpenCV's
bundled protobuf (BSD-3-Clause). Both licenses are included in the vcpkg
license collection. Smart Erase uses the existing `core` and `imgproc` modules
for deterministic multiscale patch reconstruction; it adds no model downloads or
additional OpenCV modules. The WeChat QR detection and super-resolution models are
redistributed from https://github.com/WeChatCV/opencv_3rdparty at revision
`a8b69ccc738421293254aec5ddb38bd523503252`, pinned by SHA-512 in
`cmake/FetchSnowShotQrModels.cmake`.

Selected-text acquisition and its C bridge (`snow-selected-text` and
`snow-selected-text-c`) are Apache-2.0 code under `snow-crates/`. They use the
existing Microsoft windows-rs dependency (MIT OR Apache-2.0) for Windows UI
Automation, native edit controls, and clipboard interoperability. On macOS they
use accessibility-sys (MIT OR Apache-2.0), core-foundation (MIT OR Apache-2.0),
and the existing objc2 framework bindings (Zlib OR Apache-2.0 OR MIT) for
Accessibility and pasteboard interoperability. Their resolved notices are
included in the generated Rust dependency bundle. Apple system frameworks are
provided by macOS and are not redistributed.

Smart selection (`snow-ui-selector` and `snow-ui-selector-c`) uses the same
accessibility-sys, core-foundation, and core-foundation-sys dependencies
(MIT OR Apache-2.0) for macOS Accessibility and Quartz window snapshots.
ApplicationServices, CoreFoundation, and CoreGraphics are system frameworks and
are not redistributed. Windows selection continues to use Microsoft windows-rs.

Mouse and keyboard effect rendering and its C bridge (`snow-recording-effects`
and `snow-recording-effects-c`) are Apache-2.0 code under `snow-crates/`. They
reuse crossbeam-channel (MIT OR Apache-2.0) and Microsoft windows-rs (MIT OR
Apache-2.0) for bounded input observation and native font rendering. Their
resolved licenses are included in the generated Rust dependency bundle.

GPU screen recording uses the repository's Apache-2.0 `snow-d3d11` crate and
Microsoft windows-rs (MIT OR Apache-2.0). The restricted FFmpeg 9.0 build enables
native H.264 surfaces using AMD AMF headers 1.5.2 (MIT), NVIDIA nv-codec-headers
13.0.19.0 (MIT), and Intel oneVPL dispatcher 2.17.0 (MIT). The headers and
dispatcher notices are collected from the resolved vcpkg packages, including
the full copyright notices. Vendor display drivers supply the hardware codec
implementations; those drivers are not distributed with Snow Shot.

The standalone macOS media libraries use objc2 framework bindings, block2,
and dispatch2 (MIT), and CoreText/CoreGraphics system font rendering. Apple's
ScreenCaptureKit, CoreVideo, Metal, CoreAudio, and VideoToolbox frameworks are
provided by macOS and are not redistributed. The macOS FFmpeg profile includes
x264 (GPL-2.0-or-later), x265 (GPL-2.0-or-later), WebP (BSD-3-Clause), and zlib-ng
(Zlib). The resulting FFmpeg binaries are GPL builds. Capture-only C libraries
do not link FFmpeg. Each architecture's resolved license bundle is generated
with `scripts/collect-third-party-licenses.ps1 -StandaloneMedia`.

The generated bundle is authoritative for a particular binary because its
contents are produced from that build environment. Dependency licenses and
copyright notices remain the property of their respective owners.

The optional `snow-shot-mcp` stdio bridge uses the `rmcp` Rust SDK (Apache-2.0)
and its macro/schema dependencies, plus `interprocess` (0BSD OR Apache-2.0)
for same-user local sockets. The complete resolved dependency and license
closure is collected from `snow_shot/rust/snow-shot-mcp/Cargo.lock` by the
standard license collection script.

## STranslate OCR Layout Analysis

The Smart Merge implementation in `src/presentation/ocr/screenshotocrlayout.cpp`
and its layout test fixtures are adapted from STranslate's `OcrLayoutAnalyzer.cs`
and `OcrLayoutAnalyzerTests.cs` (https://github.com/STranslate/STranslate).
Snow Shot adapts the algorithm to Qt and retains unsupported OCR geometry as
original translation units. The following license applies to the adapted code:

The MIT License (MIT)

Copyright (c) 2022 zggsong zggsong@foxmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Excalidraw arrow icons

Arrow type and arrowhead SVG assets are adapted from
https://github.com/excalidraw/excalidraw, packages/excalidraw/components/icons.tsx.
Start arrowheads are horizontally mirrored for their endpoint.

MIT License

Copyright (c) 2020 Excalidraw

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## PixiJS Filters emboss effect

The Snow Draw Engine emboss filter is a CPU adaptation of the diagonal sampling formula in the
PixiJS Filters emboss shader:
https://github.com/pixijs/filters/tree/main/src/emboss.

The MIT License

Copyright (c) 2013-2025 Mathew Groves, Chad Engler

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Auto Filter uses the repository's Apache-2.0 `visual-region-detector` and
`snow-visual-region-detector-c` crates with the pure Rust backend. Its image
boundary uses the existing `image` dependency (MIT OR Apache-2.0); JPEG support
adds `zune-core` and `zune-jpeg` (MIT OR Apache-2.0 OR Zlib). Their resolved
licenses are included in the Rust dependency notice bundle. OpenCV is not
required by this feature.


## Snow Shot MCP executable

`snow-shot-mcp` is GPL-3.0-only application code. It uses the official Rust MCP SDK
(`rmcp`, Apache-2.0) and same-user local IPC (`interprocess`, 0BSD OR Apache-2.0).
The following resolved normal/build dependencies are included in the Windows bridge;
platform-specific dependencies for macOS are selected by Cargo for the target architecture.
The packaging license collector includes each package's original license and notice files.

| Package and version | Declared license |
| --- | --- |
| anyhow-1.0.104 | MIT OR Apache-2.0 |
| autocfg-1.5.1 | Apache-2.0 OR MIT |
| base64-0.22.1 | MIT OR Apache-2.0 |
| bytes-1.12.1 | MIT |
| cfg-if-1.0.5 | MIT OR Apache-2.0 |
| chrono-0.4.45 | MIT OR Apache-2.0 |
| darling-0.24.1 | MIT |
| darling_core-0.24.1 | MIT |
| darling_macro-0.24.1 | MIT |
| doctest-file-1.1.1 | 0BSD |
| dyn-clone-1.0.20 | MIT OR Apache-2.0 |
| equivalent-1.0.2 | Apache-2.0 OR MIT |
| futures-0.3.34 | MIT OR Apache-2.0 |
| futures-channel-0.3.34 | MIT OR Apache-2.0 |
| futures-core-0.3.34 | MIT OR Apache-2.0 |
| futures-executor-0.3.34 | MIT OR Apache-2.0 |
| futures-io-0.3.34 | MIT OR Apache-2.0 |
| futures-macro-0.3.34 | MIT OR Apache-2.0 |
| futures-sink-0.3.34 | MIT OR Apache-2.0 |
| futures-task-0.3.34 | MIT OR Apache-2.0 |
| futures-util-0.3.34 | MIT OR Apache-2.0 |
| getrandom-0.3.4 | MIT OR Apache-2.0 |
| getrandom-0.4.3 | MIT OR Apache-2.0 |
| hashbrown-0.17.1 | MIT OR Apache-2.0 |
| ident_case-1.0.1 | MIT/Apache-2.0 |
| indexmap-2.14.2 | Apache-2.0 OR MIT |
| interprocess-2.4.4 | 0BSD OR Apache-2.0 |
| itoa-1.0.18 | MIT OR Apache-2.0 |
| lazy_static-1.5.0 | MIT OR Apache-2.0 |
| log-0.4.34 | MIT OR Apache-2.0 |
| matchers-0.2.0 | MIT |
| memchr-2.8.3 | Unlicense OR MIT |
| mio-1.2.3 | MIT |
| nu-ansi-term-0.50.3 | MIT |
| num-traits-0.2.19 | MIT OR Apache-2.0 |
| once_cell-1.21.4 | MIT OR Apache-2.0 |
| pastey-0.2.3 | MIT OR Apache-2.0 |
| pin-project-lite-0.2.17 | Apache-2.0 OR MIT |
| proc-macro2-1.0.107 | MIT OR Apache-2.0 |
| quote-1.0.47 | MIT OR Apache-2.0 |
| recvmsg-1.0.0 | 0BSD |
| ref-cast-1.0.27 | MIT OR Apache-2.0 |
| ref-cast-impl-1.0.27 | MIT OR Apache-2.0 |
| regex-automata-0.4.18 | MIT OR Apache-2.0 |
| regex-syntax-0.8.11 | MIT OR Apache-2.0 |
| rmcp-3.4.1 | Apache-2.0 (canonical fallback: Apache-2.0.txt) |
| rmcp-macros-3.4.1 | Apache-2.0 (canonical fallback: Apache-2.0.txt) |
| schemars-1.2.2 | MIT |
| schemars_derive-1.2.2 | MIT |
| serde-1.0.229 | MIT OR Apache-2.0 |
| serde_core-1.0.229 | MIT OR Apache-2.0 |
| serde_derive-1.0.229 | MIT OR Apache-2.0 |
| serde_derive_internals-0.30.0 | MIT OR Apache-2.0 |
| serde_json-1.0.151 | MIT OR Apache-2.0 |
| sharded-slab-0.1.7 | MIT |
| slab-0.4.12 | MIT |
| smallvec-1.16.2 | MIT OR Apache-2.0 |
| socket2-0.6.5 | MIT OR Apache-2.0 |
| strsim-0.11.1 | MIT |
| syn-2.0.119 | MIT OR Apache-2.0 |
| syn-3.0.6 | MIT OR Apache-2.0 |
| thiserror-2.0.21 | MIT OR Apache-2.0 |
| thiserror-impl-2.0.21 | MIT OR Apache-2.0 |
| thread_local-1.1.10 | MIT OR Apache-2.0 |
| tokio-1.53.1 | MIT |
| tokio-macros-2.7.2 | MIT |
| tokio-util-0.7.19 | MIT |
| tracing-0.1.44 | MIT |
| tracing-attributes-0.1.31 | MIT |
| tracing-core-0.1.36 | MIT |
| tracing-log-0.2.0 | MIT |
| tracing-subscriber-0.3.23 | MIT |
| unicode-ident-1.0.26 | (MIT OR Apache-2.0) AND Unicode-3.0 |
| uuid-1.26.1 | Apache-2.0 OR MIT |
| widestring-1.2.1 | MIT OR Apache-2.0 |
| windows-link-0.2.1 | MIT OR Apache-2.0 |
| windows-sys-0.61.2 | MIT OR Apache-2.0 |
| zmij-1.0.23 | MIT |
