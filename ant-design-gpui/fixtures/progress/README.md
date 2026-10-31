# Progress reference fixtures

Reference: Ant Design **6.6.7**, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, `@rc-component/progress` **1.0.3**.
The independent browser harness imports the original source and styles from
`E:/ant-design`. The reference dependency lock is the existing checked-in
`fixtures/select/browser-package-lock.json`; no browser dependency enters the
native library.

`visual-cases.json` describes the shared input matrix. `motion-samples.json`
records actual paused CSS transition measurements. Checked-in Windows evidence:

- `visual-validation-windows.json`: all 408 image pairs and 2,712 row results.
- `test-validation-windows.json`: focused tests, Clippy, formatting, and source hashes.
- `accessibility-validation-windows.json`: twelve native UI Automation states.
- `gallery-validation-windows.json`: six gallery captures using the default stack.
- `performance-windows.json`: 72 Release CPU measurements and allocation counts.

```powershell
node scripts/prepare_progress_reference.mjs
node scripts/capture_progress_reference.mjs --output ../build/ant-design-gpui/progress-browser
cargo build --locked --offline --example progress_visual
./scripts/capture-progress-native.ps1 -Output ../build/ant-design-gpui/progress-native
python scripts/compare_progress_visual.py --browser ../build/ant-design-gpui/progress-browser --native ../build/ant-design-gpui/progress-native --output ../build/ant-design-gpui/progress-comparison.json
node scripts/capture_progress_reference.mjs --output ../build/ant-design-gpui/progress-motion --scene line --theme default --direction ltr --scale 1 --motion yes
./scripts/test-progress-gallery.ps1
```

The generator verifies the source revision and dependency lock and hashes every
bundled input. Raw browser/native PNGs and capture metadata remain in the ignored
build tree; the comparison report retains their hashes. See
[verification](../../docs/progress-verification.md) for gates and platform limits.

Original rc-progress notices are under `licenses/`. Ant Design's MIT notice is
retained under `fixtures/theme/licenses/ant-design.txt`; shared browser package
notices are retained by the existing reference toolchain fixtures.
