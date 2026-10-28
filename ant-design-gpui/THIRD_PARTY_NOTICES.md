# Third-party notices

Original Ant Design GPUI code is Apache-2.0; see [LICENSE](LICENSE).
Adapted algorithms, translations and artwork retain the upstream MIT notices.

## Ant Design

Theme derivation, token defaults, Button, Checkbox, Radio, Divider, Tag, Input, Tooltip,
Popover, Popconfirm, Menu, Pagination, Select and Slider visual rules, Select's simple Empty illustration,
rounded popup arrows, gallery examples and descriptions,
and common translations are adapted from
Ant Design 6.6.7, commit `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`:
https://github.com/ant-design/ant-design

MIT LICENSE

Copyright (c) 2015-present Ant UED, https://xtech.antfin.com/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The original license is also preserved in `fixtures/theme/licenses/ant-design.txt`.
Independent Button browser style captures and their input provenance are under
`fixtures/button/`; their generator executes the original MIT-licensed sources.
Divider's independent browser measurements and provenance are under
`fixtures/divider/`; its generator likewise executes the original styles.
Checkbox's independent browser states and tick geometry measurements are under
`fixtures/checkbox/`; its generator executes the original Checkbox styles.
Tag's independent browser measurements and provenance are under
`fixtures/tag/`; its generator executes the original styles and color resolution.

Radio's independent browser measurements and input provenance are under
`fixtures/radio/`; its generator executes the original token defaults and styles.

Tooltip, Popover and Popconfirm browser measurements and input provenance are
under `fixtures/popup/`; their generator executes the original styles and rounded
arrow geometry. Popconfirm translations retain the same upstream MIT notice.
The native collision solver also follows the observable alignment behavior of
`@rc-component/trigger` 3.10.1 (MIT); its notice is preserved at
`fixtures/popup/placement-tools/licenses/rc-component-trigger--LICENSE`.

Live placement fixtures execute that package with React using an isolated,
locked npm toolchain. These packages are development-only and are not Rust runtime
dependencies. Exact versions and integrity hashes are in
`fixtures/popup/placement-tools/package-lock.json`; complete upstream notices,
including React, rc-component packages, esbuild and TypeScript, are preserved in
`fixtures/popup/placement-tools/licenses/` with a hashed index.

## Select and Slider behavior references

Native Select search behavior follows `@rc-component/select` 1.10.1 (MIT).
Its complete upstream notice is preserved in
`fixtures/select/licenses/rc-component-select.txt`.

Native Slider snapping, keyboard steps, disabled anchors and range interaction
follow `@rc-component/slider` 1.1.1 (MIT). Its complete upstream notice is in
`fixtures/slider/licenses/rc-component-slider.txt`. The independent numeric
fixture in `fixtures/slider/model.json` executes the original numeric hook;
its source and generator hashes are recorded in the fixture.

These JavaScript packages are development references, not Rust runtime dependencies.

Native Menu navigation and keyed selection/open behavior follows
`@rc-component/menu` 1.5.0 (MIT). Its complete upstream notice is preserved in
`fixtures/menu/licenses/rc-component-menu.txt`. Independent style measurements
and executable browser/native comparison scenarios are under `fixtures/menu/`.

Pagination's native page-window and editing behavior follows
`@rc-component/pagination` 1.4.0 (MIT). Its complete upstream notice is preserved
in `fixtures/pagination/licenses/rc-component-pagination.txt`. Independent behavior
traces and rendered style measurements execute the original pinned sources;
their source hashes and package integrity are recorded in `fixtures/pagination/`.

## Ant Design colors and fast-color

Palette generation and color conversion behavior are adapted from
`@ant-design/colors` 8.0.1 and `@ant-design/fast-color` 3.0.1, both MIT.
Independent token fixtures execute their original source. Source hashes are
recorded in `fixtures/theme/reference.json`.

- colors: Copyright (c) 2018-present Ant UED, https://xtech.antfin.com/
- fast-color: Copyright (c) 2015-present Alipay.com, https://www.alipay.com/

Their complete MIT notices are preserved in
`fixtures/theme/licenses/ant-design-colors.txt` and
`fixtures/theme/licenses/ant-design-fast-color.txt`.

## Ant Design Icons

The 829 embedded SVGs (445 outlined, 234 filled, 150 two-tone) originate from
https://github.com/ant-design/ant-design-icons at commit
`05fa2ecef7d14382878b8639a4a260513f466c19`. The vendored inputs and lock metadata
are under `resources/icons/`; generated native descriptors do not require Qt.

MIT License

Copyright (c) 2018-present Ant UED, https://xtech.antfin.com/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The original notice is also preserved in `licenses/ant-design-icons-MIT.txt`.

## GPUI and resolved Rust dependencies

The native UI runtime is `gpui` 0.2.2 (Apache-2.0) from `mg-chao/gpui`, revision
`2c0bf70b54c018f99419f386d1cf1105dca57661`. GPUI and its Windows backend resolve
directly to that Git revision without local patches. Their Apache-2.0 notices
are included by the dependency collector in the distributable license bundle.
Native platform support, test support,
Serde and JSON are development-only dependencies. Resolved versions and checksums
are recorded in `Cargo.lock`; transitive dependencies retain their own licenses.

Regenerate distributable notices using the repository's
`scripts/collect-third-party-licenses.ps1` against this locked manifest and the
target platform. The local bundle is written beneath
`../build/ant-design-gpui/licenses/third-party/`.

Input editing also directly uses `unicode-segmentation` 1.13.3 and
`unicode-bidi` 0.3.18; exact numeric support uses `num-bigint` 0.4.8 and
`num-traits` 0.2.19. Each is licensed under MIT OR Apache-2.0. Their original
license files are included in the generated dependency bundle.

The input implementation collector passed on 2026-10-26 with 486 notices covering 256 external runtime
Rust packages and 38 repository vcpkg packages. The input implementation audit is under
`../build/ant-design-gpui/input-unpatched-license-audit/third-party/` and uses the documented
fallback license directory below. GPUI Kit was consulted as an architectural
reference; no source was copied and no GPUI Kit dependency was added.

The popup implementation collector was run on 2026-10-26 against the locked Windows x64 runtime
dependency graph: 465 notices covering 254 Rust packages and 21 vcpkg packages,
using the standalone-media selection. The validation bundle is under
`../build/ant-design-gpui/popup-license-audit/third-party/`.

The shared collector also includes vcpkg and repository attribution records;
this independent library does not link those Qt/vcpkg packages. Canonical
fallback texts for dependency archives that omit licenses are available in
`../snow-draw-engine-gpui/licenses/`.

The Select/Slider audit on 2026-10-27 collected 486 notices for 256 external Rust
packages and 38 installed vcpkg packages using the locked Windows x64 manifest
and standalone-media selection. Its bundle is under
`../build/ant-design-gpui/select-slider-license-audit/third-party/`. These components
add no runtime dependency. The separately executed browser reference packages
retain their notices under `fixtures/select/licenses/` and `fixtures/slider/licenses/`.
