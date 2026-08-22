# Snow Apps Licensing

Copyright (C) 2025-2026 mg-chao

Snow Apps is a multi-license repository. Each component is licensed according
to the following table. A component license applies to the original files in
that directory tree unless a file or nested directory states otherwise.

| Scope | License |
| --- | --- |
| `snow_shot/` | GPL-3.0-or-later |
| `snow_image/` | GPL-3.0-or-later |
| `snow_image_viewer/` | GPL-3.0-or-later |
| `ant_design_qt/` | Apache-2.0 |
| `snow-crates/` | Apache-2.0 |
| `snow_draw_engine_qt/` | Apache-2.0 |
| `snow_rust_ffi/` | Apache-2.0 |

Original repository-level orchestration files, including the root build files,
`.github/`, `docs/`, `scripts/`, and shared `cmake/` modules, are licensed under
GPL-3.0-or-later unless otherwise stated.

The complete GPLv3 terms are available in `snow_shot/LICENSE`. The complete
Apache-2.0 terms are available in `snow-crates/LICENSE` and the other
Apache-licensed component directories. Operative license and copyright
notices are kept in each component's `COPYRIGHT` file so the standard license
texts remain unchanged.

Vendored, synchronized, generated, or patched third-party material retains its
upstream copyright and license. In particular, files under
`cmake/vcpkg-overlay-ports/` follow their upstream projects and vcpkg metadata,
canonical texts under `licenses/` retain the terms they contain, and the
synchronized Ant Design icons are covered by the notice in
`ant_design_qt/THIRD_PARTY_NOTICES.md`. Nothing in this file overrides a
third-party notice or license.

Build outputs, downloaded dependencies, and generated artifacts are not
relicensed by this repository-level declaration.

SPDX-License-Identifier: GPL-3.0-or-later
