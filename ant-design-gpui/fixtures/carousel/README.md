# Carousel verification fixtures

The independent reference executes the original Ant Design 6.6.7 Carousel and
styles at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, together with
`@ant-design/react-slick` 2.0.0. Expected markup, colors, tokens and geometry are
not reconstructed from native output.

`reference.json` records upstream component-token defaults for all four algorithms
and source provenance. `reference-packages.json` identifies the packages actually
bundled for the browser fixture; `licenses/` retains their original notices.
`visual-cases.json` supplies common content and scenario inputs to the original
browser component and native GPU fixture.

## Reproduction

Use the existing ignored developer dependency cache; fixture creation installs no
runtime dependency in the Rust crate:

```powershell
node scripts/prepare_carousel_reference.mjs --upstream E:/ant-design --dependencies ../build/ant-design-gpui/modal-reference/browser
node scripts/capture_carousel_reference.mjs --output ../build/ant-design-gpui/carousel-browser-current
./scripts/capture-carousel-visual.ps1 -Output ../build/ant-design-gpui/carousel-native-current
python scripts/compare_carousel_visual.py --browser ../build/ant-design-gpui/carousel-browser-current --native ../build/ant-design-gpui/carousel-native-current --output ../build/ant-design-gpui/carousel-comparison-current.json --summary fixtures/carousel/visual-validation-windows.json --native-executable ../build/ant-design-gpui/carousel-native-current/carousel_visual.exe --native-source-sha256 ../build/ant-design-gpui/carousel-native-current/build-manifest.json
python scripts/test_carousel_visual_comparator.py
./scripts/test-carousel-accessibility.ps1
./scripts/run-carousel-perf.ps1
```

Capture scripts support scene, theme, direction and scale filters for diagnosis.
Complete validation requires matching case sets, actual physical image dimensions
and display scales. Comparison enforces a one-logical-pixel geometry tolerance,
and measures full images and unaligned component crops without resampling,
registration, or masking. Known upstream or renderer differences remain in the
raw comparisons and are classified explicitly in the verification report.

PNG captures, DOM measurements, built bundles and detailed logs stay under ignored
build directories. Committed reports bind results to their recorded sources and
binaries; rerun them after relevant source changes. Interim isolated snapshots
are diagnostic evidence and do not establish a successful current-worktree build.

See [the component guide](../../docs/carousel.md) and
[verification report](../../docs/carousel-verification.md) for supported native
behavior, measured discrepancies, source snapshots, and platform limits. The final
recorded native captures are in `build/ant-design-gpui/carousel-native-final` under
the parent repository, with their immutable copied executable and build manifest.
