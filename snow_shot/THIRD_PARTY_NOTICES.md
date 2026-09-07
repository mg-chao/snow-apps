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

Local crash diagnostics use Crashpad (Apache-2.0), pinned by the vcpkg baseline
and the repository's Crashpad overlay. The client, helper, Chromium base code,
and their bundled notices are included in the vcpkg license collection. The
Qt-independent OCR bridge and shared Rust panic hook are Apache-2.0 code under
`snow-crates/`.

The generated bundle is authoritative for a particular binary because its
contents are produced from that build environment. Dependency licenses and
copyright notices remain the property of their respective owners.

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
