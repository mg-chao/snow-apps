# Notification verification inputs

`visual-cases.json` is shared input data, not expected geometry. The independent
browser harness executes the original Ant Design Notification component, hooks,
styles, and @rc-component/notification 2.1.1. Native code is not used to generate
browser values. `reference-packages.json` and `licenses/` retain the actual bundled
package versions and original license texts.

Build the original component with Node and an existing locked developer cache:

```powershell
node scripts/prepare_notification_reference.mjs --upstream E:/ant-design --dependencies ../build/ant-design-gpui/modal-reference/browser
node scripts/capture_notification_reference.mjs --output ../build/ant-design-gpui/notification-browser
cargo run --locked --offline --example notification_visual -- --output ../build/ant-design-gpui/notification-native
python scripts/compare_notification_visual.py --browser ../build/ant-design-gpui/notification-browser --native ../build/ant-design-gpui/notification-native --output ../build/ant-design-gpui/notification-comparison.json --summary fixtures/notification/visual-validation-windows.json --native-executable ../build/ant-design-gpui/debug/examples/notification_visual.exe
```

The preparer validates upstream revision and notification package version. It
uses cached esbuild/React/Ant Design developer dependencies and never installs
packages. See the existing Modal reference setup for cache provisioning; native
builds and deterministic tests do not need Node, a browser, or the reference tree.

Capture filters `--scene`, `--theme`, `--direction`, and `--scale` select subsets.
Native captures use a hidden HWND and actual 100%, 150%, and 200% scale factors.
No global input or desktop settings are changed. Comparison asserts identical
physical dimensions, notice identities, and visibility; it retains full unaligned,
unmasked image differences and logical geometry errors. Raw PNGs, metrics,
bundles, browser profiles, and diagnostics stay under the ignored build tree.

The verification report documents the scope and status of recorded evidence.

When other work is editing shared sources, the comparison accepts
`--native-source-sha256 <file>` with a JSON path-to-SHA256 mapping captured and
verified immediately around the native build. This preserves the tested source
identity even if the workspace changes while the fixed binary captures images.
Without this option, source hashes describe the workspace at comparison time.
