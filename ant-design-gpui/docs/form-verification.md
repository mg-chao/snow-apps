# Form verification

The native Form implementation passes the focused Windows checks below. The
[coverage map](form-coverage.md) connects reference features to native APIs and
tests; [form.md](form.md) describes integration and ownership.

Reference: Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, `@rc-component/form` 1.8.6.
GPUI: `2c0bf70b54c018f99419f386d1cf1105dca57661`. Rust 1.98.1; Windows x64,
AMD Radeon RX 6750 XT / Direct3D 11; original browser Chrome 156.0.8078.74.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| Focused library tests | **747 passed**, including **85 Form tests**, across 15 affected module filters | [test validation](../fixtures/form/test-validation-windows.json) |
| Gallery | **8 focused tests passed**: 4 Form, formatter, sidebar navigation and 2 Windows capture tests; 37 public demos in reference order | [gallery validation](../fixtures/form/gallery-validation-windows.json) |
| Primary visual matrix | **456/456 geometry and paint comparisons passed**, zero failed cases | [visual validation](../fixtures/form/visual-validation-windows.json) |
| Passive layout inspection | **19/19 default scenes have identical RGBA pixels** with inspection compiled in or out | [instrumentation proof](../fixtures/form/instrumentation-validation-windows.json) |
| Native accessibility | **6/6 locale/theme/direction profiles passed**; four distinct settled native trees per profile | [accessibility validation](../fixtures/form/accessibility-validation-windows.json) |
| Release performance | Sharing, stable list keys, zero unrelated watcher/control rebuilds, zero owner notifications and zero settled idle callbacks passed | [performance validation](../fixtures/form/performance-validation-windows.json) |
| Native stack | All 456 captures completed on the ordinary **1,048,576-byte** Windows main stack | PE and executable hashes in visual validation |
| Format and lint | `cargo fmt -- --check`, selected-target Clippy with `-D warnings`, and `git diff --check` passed | Commands below |
| Dependency notices | **479 notices**, 263 external Rust packages and 21 repository vcpkg packages collected | [license validation](../fixtures/form/license-validation.json) |

## Visual comparison

Nineteen compositions run under default, dark, compact and dark-compact themes,
LTR/RTL and 100/150/200% DPI. The browser executes the original unmodified Form
and styles. Native captures use an owned hidden window, verified executable and
source hashes, GPU identity and foreground checks. The source audit verifies
477 input hashes and all 456 original screenshots.

Root/item/label/control/help/extra/feedback geometry, compound children and logical
inner-label boxes pass the one-logical-pixel gate (0.001 float tolerance). Solid
paint passes the one-byte RGB gate. Six negative controls reject wrong geometry,
missing parts and colors; two additional controls verify consistent handling at
the coverage/color tolerance boundary. Screenshots are never aligned or resized
for comparison. Reference glyph/SVG regions and their antialiasing neighborhoods
are excluded from solid-interior gates; raw raster differences remain in the
report for visual review. Shadows have no separate exclusion mask and remain
coverage-gated. This is not a pixel-identical claim between Chromium and GPUI.

The browser DPI harness starts a fresh process at each actual device scale.
`fixtures/form/dpi-reference.json` records an independent reproduction showing
that CDP device-scale emulation alone leaves CSS borders snapped at the launch
DPI. All 456 source captures were regenerated with genuine launch DPI; native
Input border sizing agrees with that corrected baseline.

The actual gallery was also captured in default and dark-compact-RTL modes at
1280-by-900 logical pixels (1920-by-1350 physical pixels at this monitor's 144 DPI).
Both processes stayed hidden and exited successfully. A continuous foreground
event hook and 25 ms PID samples observed no activation. Windows capture sizing
bypasses the pinned GPUI resize call, which omits `SWP_NOACTIVATE`; the scoped
helper changes only the owned hidden gallery window.

## Performance

`scripts/run-form-perf.ps1` verifies the benchmark-enabled
`windows-msvc-performance` preset, optimized Release artifact, source/resource
hashes and executable hash. Measurements below used an AMD Ryzen 9 5950X;
system CPU load was sampled at 9% before and 2% after the run.

| Mounted native controls | Update + automatic GPUI draw p50 | p95 | p99 | Allocations / operation |
| --- | ---: | ---: | ---: | ---: |
| 10 | 0.245 ms | 0.273 ms | 0.329 ms | 551 |
| 100 | 0.988 ms | 1.070 ms | 1.193 ms | 2,180 |
| 1,000 | 15.315 ms | 17.863 ms | 19.124 ms | 19,363 |

All three sizes preserve unrelated control factories, issue zero owner
notifications and settle with zero idle callbacks. GPUI renders ancestors when a
descendant changes; owner render calls are reported rather than claimed absent.
Its test-mode update flush already draws dirty windows, so the timed path does
not force an additional frame.

Persistent snapshot updates measured 1.1 microseconds p50 at 1,000 siblings and
1.2 microseconds at 10,000, with approximately 10 and 11 allocations respectively;
old snapshots retain shared unaffected native leaves. With 1,000 registered
fields and 12 mounted editors, silent updates measured 0.294 ms p50, dependent
user changes 0.303 ms, 64-row list model operations 0.852 ms, and validation plus
reset 2.251 ms. Allocation bytes in the full report describe allocation traffic,
not retained memory. These are offscreen CPU/allocation distributions with
background load sampled, not isolated; GPU presentation, physical input latency
and an absolute frame-rate guarantee are unverified.

## Accessibility and platform limits

The six native profiles combine English/Simplified Chinese/Traditional Chinese
with default and dark-compact-RTL configuration. Each checks a fresh child PID,
four actual native tree frames, stable field identities, full help/extra,
required/busy metadata, password privacy, hidden omission, disabled inheritance,
explicit enablement, a 900-by-850 logical client viewport and no activation.
The pinned Windows AccessKit adapter does not expose UIA `IsDataValidForForm`;
invalid state is verified in the actual native AccessKit node instead of treating
an unsupported UIA property's default value as evidence.

This GPUI revision does not replay accessibility subtrees from scene caches.
Form bypasses named-item scene caching while assistive technology is active.
Entities and editing state remain retained; active accessibility frames do more
rendering work. Those frame costs were not benchmarked. Inactive caching and
zero settled idle callbacks are tested separately.

The auxiliary Switch matrix passes **12/12 geometry** and **7/12 strict paint**
cases. Five failures remain in
[the auxiliary report](../fixtures/form/switch-opacity-validation-windows.json).
Independent plain-CSS probes match documented Skia software byte-alpha
interpolation in all 24 sampled colors; native GPU floating-point blending differs
by up to two bytes in some solid interiors, with additional antialiasing coverage
residuals. No alpha-specific corrections or scene masks hide these failures.
Switch group opacity requires a known opaque uniform backdrop; nonuniform
backgrounds, ancestor opacity and unbounded custom paint remain limitations.

Six reference demos depend on unavailable controls. Their native gallery versions
demonstrate Form behavior using supported controls and explicitly identify those
substitutions. No visual-equivalence claim applies to a substituted control.
Native execution evidence is Windows-only; macOS/Linux validation remains
unverified.

## Reproduction

Run only the relevant filters; the full repository suite was not run and remains
prohibited for this task.

First generate the ignored browser baseline and its audit using
[the reference instructions](../fixtures/form/README.md). For performance, configure
`cmake --preset windows-msvc-performance` from the parent repository root before
running the benchmark script. The script requires that preset's benchmark-enabled
Release cache. Run the commands below from `ant-design-gpui/` with its pinned
toolchain and dependencies available.

```powershell
cargo test --locked --offline --lib form::
cargo test --locked --offline --example gallery tests::form::
cargo fmt -- --check
cargo clippy --locked --offline --lib --bench form --example form_visual --example gallery --example form_accessibility --features test-support -- -D warnings
scripts/test-form-accessibility.ps1
scripts/capture-form-native.ps1 -WithoutLayoutInspection -Theme default -Direction ltr -Scale 1 -Output ../build/ant-design-gpui/form-native-default
scripts/capture-form-native.ps1
python scripts/compare_form_instrumentation.py --plain ../build/ant-design-gpui/form-native-default --instrumented ../build/ant-design-gpui/form-native --output fixtures/form/instrumentation-validation-windows.json
node scripts/pack_form_visual_validation.mjs
scripts/run-form-perf.ps1
cargo run --locked --offline --example gallery -- --component form
```

The other focused library filters are `input::`, `input_number::`, `slider::`,
`select::`, `date_picker::`, `color_picker::`, `checkbox::`, `radio::`, `switch::`,
`segmented::`, `scroll_area::`, `behavior::`, `config::` and `locale::`.
Large screenshots, raw logs and machine-specific executables remain under the
ignored build tree. Compact checked evidence files retain their SHA-256 hashes.
