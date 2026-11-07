# Image verification

Image is compared with the genuine pinned Ant Design 6.6.7 Image and PreviewGroup
component, including @rc-component/image 1.10.0. The developer reference imports
the original checked-out source and styles using an isolated locked toolchain.
Native GPUI rendering remains independent of React and the browser.

The verification records source and asset SHA-256 hashes, package versions and
license notices, browser version, native render scale, physical screenshot sizes,
native layout measurements, and full unmasked pixel differences. Reproduction
commands and fixture inputs are in [the fixture guide](../fixtures/image/README.md).

The independent static browser matrix contains 432 captures: 18 scenes, four
themes, both directions, and 100/150/200% device scale. It covers thumbnail and
fallback rendering, preview sizing and controls, groups, transform order,
zoom/pan, custom covers/tokens, and progress placeholders.
Async-load checks exercise loading, stale completions, and fallback errors using
only local fixture endpoints. The accepted blur fallback preserves mask dimming.

The separate motion reference contains 276 genuine browser samples across
opening, closing, rotation, zoom, flips, hover, and flowing ink. WAAPI freezes
the original CSS animations and preserves computed transforms, opacity,
keyframes, and timing. Native virtual-clock tests compare interpolation with
these independent values. This is separate from the paired static screenshot
matrix; paired native animation screenshots are not claimed.

The pixel comparator preserves complete unaligned and unmasked frames. It gates
actual native layout to one logical pixel, solid colors to one channel level,
and complete image crops to 8/255 mean channel error. Its default `strict` policy
also gates glyph crops at 12/255. The published native comparison explicitly uses
`--glyph-raster-policy shape`: every glyph must satisfy signed-color, contrast,
integrated-ink, centroid, and bidirectional physical-pixel shape checks. There is
no raw-error shortcut for faint icons. Counts must contain actual painted ink.

The shape policy allows at most one physical pixel of centroid displacement,
0.5 pixel of weighted mean support distance, and 1.5 pixels at the weighted 95th
percentile. Pixels with at least half the reference peak contrast also have a
hard maximum distance of 1.5 pixels, so a bright extra dot cannot hide in a weak
background tail. Integrated ink must remain within 0.75–1.33 of the reference;
peak contrast within 0.75–1.25, and normalized signed RGB contrast within 0.12.
Coverage integrates every 8-bit contrast value: a one-level quantization change
across the support cutoff must not invent ink. Binary support counts, 99th
percentiles, and weak-pixel maximum distances remain diagnostic.

Thirty-six comparator tests cover missing/wrong/faded/inverted glyphs, visible
extra ink, weak background outliers, one-level fringe quantization, layout/DPI,
canonical matrix completeness, duplicates, and missing/stale build provenance.
They include 180 wrong substitutions of genuine browser toolbar glyphs and 36
correct identities. Independent review additionally checked 216 wrong full-group
control substitutions and 27 identities, including close/navigation controls.
A published summary requires the complete canonical 432-case product and current
source, executable, linked library, compiler dep-info, and screenshot identities.

Only related Image, touched foundation, and gallery tests should run. Performance
measurements require Release through the benchmark-enabled
`windows-msvc-performance` configuration and record binary/source identities.
The CPU benchmark uses `TestAppContext` to measure offscreen GPUI layout and scene
construction. The GPU runner separately uses a real visible Windows window,
requests no activation or focus, observes activation per sample, warms a texture, then submits
120 pan/zoom/rotation/flip samples. It checks native affine GPU primitives,
portable payload upload/readback counters, and idle presentations/frame callbacks.
Timing ends at Present1 submission return and excludes GPU retirement and
physical display scanout. The transform benchmark disables motion interpolation
to isolate transform submission; motion is checked separately.

The focused validation entry point builds only library/gallery test artifacts,
verifies the compiler's source manifest, and runs the Image and affected
foundation filters. It includes the gallery's nested preview regression on a
default 1 MiB Windows thread stack. The corresponding checks are:

```powershell
./scripts/test-image.ps1
python scripts/test-image-comparator.py --browser ../build/ant-design-gpui/image-browser-current --raster-report ../build/ant-design-gpui/image-glyph-candidate-faults.json
cargo clippy --locked --offline --lib --tests --example gallery --example image_visual --example image_gpu_perf --example image_accessibility --bench image -- -D warnings
./scripts/test-image-accessibility.ps1
./scripts/run-image-perf.ps1
./scripts/run-image-gpu-perf.ps1
```

Run UIA exclusively, with capture and GPU windows closed: its process-owned
window is activated for focus checks and receives UIA operations and targeted
window messages. It does not send global input. Run the GPU measurements after
all capture/UIA fixtures and heavy comparison work have finished.

The fixture guide explicitly selects the native-AA policy. Omitting
`--glyph-raster-policy shape` reproduces the stricter raw-pixel baseline. After
the guide's toolchain preparation, these commands regenerate the named native
comparison artifacts:

```powershell
node scripts/capture_image_reference.mjs --output ../build/ant-design-gpui/image-browser-current --fixtures true --behavior true
./scripts/capture-image-native.ps1 -Output ../build/ant-design-gpui/image-native-current
python scripts/compare_image_visual.py --browser ../build/ant-design-gpui/image-browser-current --native ../build/ant-design-gpui/image-native-current --glyph-raster-policy shape --output ../build/ant-design-gpui/image-comparison-current.json --summary fixtures/image/visual-validation-windows.json
```

The native screenshot, CPU, and GPU runners check the linked library's actual
Rust dep-info for `CARGO_MANIFEST_DIR`, record its hash, and bind the executable
to its implementation hashes. This prevents a shared Cargo target directory
from accidentally validating an artifact built from an older copied checkout.
Performance runs and screenshot exports run separately so capture readbacks
and other fixture windows do not affect transform submission measurements.
The DPI fixture sends `WM_DPICHANGED` at 96/144/192 DPI only to its own hidden
window and asserts GPUI's reported scale and the exported physical pixel size.
It does not change desktop or monitor DPI, and it does not claim validation on
three physical monitor configurations.

The final Windows x64 run passed all 432 cases under the explicit shape policy:
624 image crops, 936 geometry rectangles, 2,280 glyphs, and 48 count labels.
Maximum layout error was 0.5 logical pixels; maximum complete-image mean channel
error 3.13175/255; average full-frame mean channel error 0.159036/255.
The strict raw-glyph baseline remains **failed**: 86 crops exceed 12/255, with
maximum 22.80435/255. These exceedances and complete pixel errors remain in the
published JSON. The residual includes SVG atlas/frame scaling, different edge
antialiasing, and fractional device-pixel placement. Matching paths and requested
artwork dimensions do not imply pixel-identical native/browser rasterization.

The focused source-bound run passed 208 tests: 77 Image, 29 Modal, 20 Config,
8 locale, 5 root, 42 behavior, 20 icons, and 7 gallery tests. Three exclusive UIA
runs each passed all eight checks. Scoped Clippy with `-D warnings`, rustfmt,
and `git diff --check` passed; the GPU observability follow-up also passed its
example-specific Clippy check.

Release CPU measurements on an AMD Ryzen 9 5950X passed 18 cases and the zero-idle
callback assertions. One-image warm scene construction measured median 96.5 µs,
p95 115.5 µs; 100 images measured median 3.6939 ms, p95 4.4288 ms. On an AMD Radeon
RX 6750 XT / Direct3D11, all 120 transform samples changed the retained transform
and used the native GPU texture primitive. After the 256,000-byte warm upload,
there were no additional image payload uploads/readbacks and no idle
presentations/callbacks. The final run observed the window visible and inactive
in every sample. P95 dispatch-to-Present1-return was 7.3091 ms; compositor CPU
51.9 µs; Present1 CPU 414 µs. These are CPU submission observations, not display
latency or GPU execution timings.

The 2026-10-29 UTC receipts below contain the source and artifact identities used for
these results. Paths under `build/` are ignored generated evidence. The full
comparison retains every frame/crop metric and both screenshot hashes; the
focused test receipt retains individual logs and all 2,276 input hashes.

| Receipt | SHA-256 |
| --- | --- |
| [Visual summary](../fixtures/image/visual-validation-windows.json) | `8631da5e25898f42b8d2060dd50e606ae682a51f959a4a5be3e969b5d2e4480b` |
| [Focused tests](../fixtures/image/core-validation-windows.json) | `c821ec033cc08ab043f9be2ba8e7ba4161e843cbe0b9b0f307fb8fb440b5b394` |
| [Exclusive UIA](../fixtures/image/uia-validation-windows.json) | `a466a1d8a0f6606e80d845186a515f31e1f4e46dd10316214b400357ef56fafd` |
| [Release CPU](../fixtures/image/performance-windows.json) | `17e41e4389bf9645a0f7fc13d5d31dfc0b33199e01753d53cd00e8bf23df3c53` |
| [Release GPU](../fixtures/image/gpu-performance-windows.json) | `fcad2defd78be9f116bbf10c7a459a57908263c1ba2d5e79e55154747702d4c1` |
| [Full native comparison](../../build/ant-design-gpui/image-comparison-current.json) | `0f75c4dd6af14f5622ce74c47497830ab3b8551974a963af2e0b12c6ae445395` |
| [Strict raw baseline](../../build/ant-design-gpui/image-comparison-current-strict.json) | `9f73affa9de1c171daf1a428d8e1324ce52da1763ff77b6748dc939d1b118e50` |
| [Native capture provenance](../../build/ant-design-gpui/image-native-current/native-provenance.json) | `29ca81cb59fae58806e64758f69274af3d5fa857ac6c195b65d2ca875c98903f` |
| [Full focused-test receipt](../../build/ant-design-gpui/image-current-core-verification/focused-tests.json) | `c6fa112c7b56d00b206fa52c49a04e493759a775eb0c15eda52a9328e81848d1` |

The captured native executable SHA-256 is
`7136f657adb527e3f22162c81ed6b66a92d599643010cdf0b1b7d24518b7a0b5`;
the actually linked library is
`7bec9a24374ed83e70c555c5f888b073f638adbe03c831443140f65130e8c4c5`;
its source-bound compiler dep-info is
`241fb7ac420dbd378a5c069d06a441103a33094d348bd8fc5975cb5eac12e237`.
The approved comparator SHA-256 is
`005f02277d1ef81a01d336d5cd76b707887914b5b58a48b1e8b547e5ec64cc77`.

Representative native exports are [thumbnails](../../build/ant-design-gpui/image-native-current/thumbnails-default-ltr-100.png),
[progress](../../build/ant-design-gpui/image-native-current/progress-public-default-ltr-100.png),
[group preview](../../build/ant-design-gpui/image-native-current/preview-group-default-ltr-100.png),
and [compact single-item group at 150%](../../build/ant-design-gpui/image-native-current/preview-group-single-compact-ltr-150.png).

Validation here targets native Windows x64. Windows ARM64, macOS/Linux rendering,
and physical touch hardware were not exercised; pinch/touch behavior uses deterministic GPUI
events. Native animation interpolation is checked against the browser fixture,
but paired native motion-phase screenshots are not claimed. Platform font
shaping, antialiasing, and fractional device-pixel rounding remain visible in
the unmasked comparisons. Optional backdrop blur uses the accepted dim-mask
fallback. Custom preview hooks can host application-owned native media views;
the component does not bundle a video decoder/player. Portable fallback rendering
quantizes intermediate rotation angles to cached quarter-turn orientations.
Extremely large images may be downsampled to adapter/resource limits, which can
limit sharpness at high zoom; normal reference fixtures retain their full pixels.
When both the original image and its fallback fail, native thumbnails use the
picture icon plus alt text and previews use the localized loading-error message.
Upstream plain HTML images use browser broken-image/alt presentation. The static
matrix covers a failed original with a valid fallback PNG; the no-valid-fallback
error artwork was behavior-tested but was not pixel-compared with browser error
artwork. UIA does not validate screen-reader speech or physical OS keyboard routing.
