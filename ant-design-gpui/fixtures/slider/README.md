# Slider reference evidence

`model.json` contains 302 independently executed numeric cases and 21 disabled
anchor cases from `@rc-component/slider` 1.1.1, the version specified by the
Ant Design 6.6.7 reference checkout. It covers click/value snapping, keyboard
movement, directed dragging, fractional grids, marks-only scales, endpoint ties,
and disabled-handle selection. It does not establish visual or accessibility parity.

The generator evaluates the original `es/hooks/useOffset.js`. Only module syntax
and `React.useCallback` are adapted; the latter returns its original callback.
No Rust-generated values supply expected results. Source and generator SHA-256
hashes accompany the cases. The original MIT license is preserved in `licenses/`.

Regenerate using an extracted, integrity-checked reference package:

```powershell
node scripts/generate_slider_model_reference.mjs --package ../build/ant-design-gpui/select-slider-reference/packages/rc-component+slider-1.1.1
```

The generator rejects other package versions. `--output` selects an alternate
fixture destination. Cargo tests use the checked-in JSON and require no Node.js,
browser, network access, or reference checkout.

`visual-reference.json` contains 40 baseline captures of the executable browser
control, including dependency integrity and source hashes. The larger native/
browser comparison, UIA results and Release measurements are summarized in
[`../select-slider/verification.json`](../select-slider/verification.json) and
the [verification report](../../docs/select-slider-progress.md).
