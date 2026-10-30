# Third-party notices

Original Ant Design GPUI code is Apache-2.0; see [LICENSE](LICENSE).
Adapted algorithms, translations and artwork retain the upstream MIT notices.

## Ant Design

Theme derivation, token defaults, Alert, Button, Checkbox, ColorPicker, Radio, Descriptions, Divider, Tag, Input, Tooltip,
Popover, Popconfirm, Menu, Modal, Pagination, Select, Slider, Tabs, Carousel, Result, Message and Notification visual rules,
Select's simple Empty illustration and Result's 403/404/500 illustrations,
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
Tabs measurements and hashed source provenance are under `fixtures/tabs/`;
its generator executes the original Tabs style functions and cssinjs serializer.
Divider's independent browser measurements and provenance are under
`fixtures/divider/`; its generator likewise executes the original styles.
Checkbox's independent browser states and tick geometry measurements are under
`fixtures/checkbox/`; its generator executes the original Checkbox styles.
Tag's independent browser measurements and provenance are under
`fixtures/tag/`; its generator executes the original styles and color resolution.

Alert's native visual rules, component tokens, gallery examples and descriptions
follow the same pinned Ant Design sources. Its independent browser measurements
and hashed provenance are under `fixtures/alert/`. The developer generator executes
the original `components/alert/Alert.tsx` through ReactDOMServer with the original
Alert style generators, cssinjs serializer, CSSMotion and Ant Design icon components.
Configuration and theme-registration adapters supply the pinned theme fixtures;
the component DOM is rendered by the upstream component. The separate browser/native
capture tools use that output for image and geometry comparisons. These packages
are development references and add no JavaScript runtime dependency to the crate.
Complete original MIT notices for the executed React, ReactDOM, rc-component
motion/util, Ant Design icons and cssinjs packages are retained in
`fixtures/alert/licenses/`; the fixture records loaded package/source hashes.

Radio's independent browser measurements and input provenance are under
`fixtures/radio/`; its generator executes the original token defaults and styles.

Carousel's visual rules, token defaults, translations and examples follow the same
pinned Ant Design source. Its independent developer fixture executes the original
Carousel and `@ant-design/react-slick` 2.0.0 (MIT, copyright 2014 Kiran Abburi).
Complete loaded package licenses and source provenance are preserved under
`fixtures/carousel/`. These are developer references, with no JavaScript runtime
dependency in the native component.

ColorPicker's appearance, translations, examples and observable editing behavior
follow the same pinned Ant Design source and `@rc-component/color-picker` 3.1.1.
Color conversion and named-color references use `@ant-design/fast-color` 3.0.1.
Complete original loaded-package licenses and independently captured source
provenance are preserved under `fixtures/color_picker/`. These JavaScript packages
are developer references; the native component has no JavaScript runtime dependency.

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

## Spin reference fixtures

Spin's developer-only reference generator executes the original Ant Design
6.6.7 component, styles, React/ReactDOM, cssinjs, rc util, throttle-debounce,
TypeScript and their helper packages. These packages are not native runtime
dependencies. Original notices are retained under `fixtures/spin/licenses/`;
Ant Design's MIT notice is retained in `fixtures/theme/licenses/ant-design.txt`.
See `fixtures/spin/README.md` for versions, provenance and regeneration.

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

Result's CSS-compatible text wrapping directly uses `unicode-linebreak` 0.1.5
(Apache-2.0), an existing locked transitive dependency of GPUI, for Unicode
Standard Annex #14 line-break opportunities. Native shaping and bidi placement
remain GPUI-owned. Its original license is retained in
`licenses/unicode-linebreak-Apache-2.0.txt` and the generated dependency bundle.

The Result audit on 2026-10-28 collected 470 notices for 257 external Rust
packages and 21 installed vcpkg packages, using the locked Windows x64 manifest
and standalone-media selection. Its bundle is under
`../build/ant-design-gpui/result-licenses/third-party/`.

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

## Message reference fixtures

Message's developer-only reference generator executes the original Ant Design
6.6.7 Message hook, panel and shared Notification styles, with the published
`@rc-component/notification` 2.1.1, React/ReactDOM, cssinjs, rc util, TypeScript
and helper packages. They are not native runtime dependencies. Original package
notices are retained under `fixtures/message/licenses/`; Ant Design's MIT notice
is retained in `fixtures/theme/licenses/ant-design.txt`. See
`fixtures/message/README.md` for pinned versions, provenance and regeneration.

## Progress reference fixtures

Progress geometry, token defaults and examples are adapted from Ant Design 6.6.7
(MIT) and `@rc-component/progress` 1.0.3 (MIT). The developer-only browser harness
executes the original source using the existing locked reference toolchain.
Neither React nor rc-progress is a native runtime dependency. Original notices
are retained in `fixtures/progress/licenses/rc-component-progress.txt` and
`fixtures/theme/licenses/ant-design.txt`. See `fixtures/progress/README.md` for
source revision, provenance and regeneration. No native dependency was added.

Notification's visual defaults, gallery copy, and original browser reference follow
Ant Design 6.6.7 and @rc-component/notification 2.1.1. Original licenses for every
package executed by the fixture bundle are preserved in `fixtures/notification/licenses/`;
`fixtures/notification/reference-packages.json` records their names, versions, and hashes.
These developer fixtures add no JavaScript runtime dependency to the native library.
## Image and PreviewGroup

Image visual token formulas, interaction behavior, and gallery examples follow
Ant Design 6.6.7 (MIT) and `@rc-component/image` 1.10.0 (MIT). The independent
developer fixture executes the original sources; complete package notices and
source hashes are retained under `fixtures/image/licenses/` and its locked
toolchain manifest. The Rust runtime uses native GPUI elements and rendering.
The original deterministic fixture artwork under `resources/image/` is
Apache-2.0, with generator and content provenance in its manifest.

Direct `image` 0.25.10 (MIT OR Apache-2.0) and `data-url` 0.3.2
(MIT OR Apache-2.0) dependencies support native pixel preparation and data URLs.
