# Image verification inputs

The independent renderer executes the actual Ant Design **6.6.7** Image and
PreviewGroup source at revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`,
with **@rc-component/image 1.10.0** and original styles. `browser-package-lock.json`
locks a developer-only toolchain installed inside the ignored build directory.
No Rust implementation values generate browser expectations.

`visual-cases.json` contains shared scene inputs: thumbnail aspect ratios, fallback,
landscape/portrait/transparent previews, groups, individual flips, rotations,
combined transforms, zoom/pan, disabled masks, custom covers, component tokens,
and indeterminate/determinate/custom progress. The matrix uses four themes,
LTR/RTL, and actual 100/150/200% device scale. The separately recorded backdrop
blur case is excluded from parity because the pinned GPUI supports dimming only.

Both renderers use the same original Apache-2.0 PNG fixtures in `resources/image`.
Their generator and manifest retain source and content hashes. Network loading
checks use a process-owned loopback server with held and failing endpoints.

```powershell
node scripts/generate_image_assets.mjs
node scripts/prepare_image_reference.mjs --upstream E:/ant-design --install true --offline true
node scripts/capture_image_reference.mjs --output ../build/ant-design-gpui/image-browser --fixtures true --behavior true
./scripts/capture-image-native.ps1 -Output ../build/ant-design-gpui/image-native
python scripts/compare_image_visual.py --browser ../build/ant-design-gpui/image-browser --native ../build/ant-design-gpui/image-native --glyph-raster-policy shape --output ../build/ant-design-gpui/image-comparison.json --summary fixtures/image/visual-validation-windows.json
```

Omit `--offline true` only when provisioning the locked developer packages from
the network. Subsequent preparation needs no install flag. Capture filters
`--scene`, `--theme`, `--direction`, and `--scale` help diagnose individual scenes.
Only a complete browser matrix may replace checked-in reference measurements.
The native capture runner records binary and source hashes and rejects a run if
its implementation sources change while building or capturing. `-SkipBuild`
requires a verified build stamp whose executable and source hashes still match.
Publishing a comparison summary requires verified capture provenance and rejects
changed metrics, executable bytes, or implementation sources.

The command explicitly selects the native glyph `shape` policy. Every control
must pass physical-pixel shape, centroid, signed-color, contrast, and integrated
ink checks; layout, solid colors, and image crops retain their original gates.
Raw unaligned pixel errors and all glyph crops above 12/255 remain visible in
the report. Omitting the flag runs the stricter raw-pixel baseline, which retains
fractional-placement and SVG rasterization differences. The policy, limits,
mutation tests, and current results are detailed in
[the verification guide](../../docs/image-verification.md).

The capture tools own fresh browser profiles and hidden native windows. They do
not attach to an existing browser, read user sessions, change desktop DPI, or
send global input. Generated bundles, profiles, screenshots, and raw reports stay
under the ignored build directory. Fixture JSON retains source/package hashes,
upstream revision, renderer version, and actual geometry/style measurements.
Native scaling uses `WM_DPICHANGED` at 96/144/192 DPI on the fixture's own hidden
window, then checks the actual GPUI scale and physical screenshot dimensions.
Desktop and monitor DPI remain unchanged; separate physical monitor layouts
were not exercised.

The comparison uses full unaligned, unmasked images without resizing or color
correction. It verifies exact physical dimensions, signed layout differences,
image-presence/color probes, and raw pixel errors. Platform text and edge
rasterization differences remain in the report. A pass requires measured native
geometry and does not accept browser coordinates substituted into native data.

`python scripts/test-image-comparator.py --browser ../build/ant-design-gpui/image-browser`
fault-injects missing/wrong/faded/inverted glyphs, visible extra ink, altered
geometry, missing cases, and wrong scales or physical dimensions to verify the
comparison cannot silently accept those errors. Each complete image crop has an
8/255 mean channel error ceiling in addition to the one-logical-pixel geometry
and one-channel-level solid-color gates. Text and antialiased edges remain in
the crops.

`motion-cases.json` supplies animation inputs, and `motion-reference.json` records
276 genuine browser samples across opening, closing, rotation, zoom, flips,
hover, and flowing ink. `node scripts/capture_image_motion.mjs --fixtures true`
reproduces them with a fresh browser. WAAPI pauses the original CSS animations
at their declared phases; the fixture preserves computed transforms/opacity,
original keyframes, easing, timing, source identities, and screenshot hashes.
These are browser motion measurements; native virtual-clock tests check motion
separately, and this matrix is not a claim of paired native motion screenshots.

The two Release runners are `scripts/run-image-perf.ps1` for focused CPU/layout
measurements and `scripts/run-image-gpu-perf.ps1` for a real native GPU window.
The GPU runner warms the immutable image texture, then checks 120 pan, zoom,
rotation, and flip samples without screenshot exports. It asserts a native affine
GPU primitive is present, no extra portable texture payload uploads or readbacks,
and zero idle presentations/frame callbacks after GPUI's deliberate one-second
high-rate-input tail. Timing measures CPU submission through Present1 return;
it does not measure GPU retirement or physical display scanout.
