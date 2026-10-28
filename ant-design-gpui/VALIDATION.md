# Foundation validation

## Input gallery reference examples (2026-10-26)

The gallery now has separate Input and InputNumber pages matching the public demo
order and titles in Ant Design 6.6.7, reference revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Input contains 18 cards: 11 complete,
six partial and one planned. InputNumber contains 14: 11 complete, two partial
and one planned. Partial cards show supported rows and explain missing portions.
The numeric-tooltip and spinner cards contain explanations without substitute
controls. Addon/Select rows, custom password visibility icons, manual TextArea
resizing and independent internal-part styles/callbacks remain pending.

All changes use existing component APIs. Named retained entities keep preview
values independent across navigation and display changes. Standard counts use
UTF-16; custom emoji counts use Unicode scalars. The exact-decimal example steps
by `0.00000000000001` and displays its initial value as `1`. Focus examples use
public focus/selection APIs, preserve page scroll for prevent-scroll activation,
and switch Input/TextArea without replacing the retained state. Library sources,
dependencies and the standalone input verification app are unchanged.

- Nine focused input gallery checks and seven related catalog/shell checks passed:
  `cargo test --locked --offline --example gallery -- input_gallery input_number_gallery catalogue toolbar scrolling columns_follow demo_jumps presentation_changes gallery_options --test-threads=1`.
- The nine input checks were rerun after native visual refinements. Coverage
  includes exact reference order/statuses, navigation and jump anchors, isolated
  values, search/action/clear events, loading suppression, password visibility,
  compact submission, OTP formatting/disabled/masked/length/separator options,
  counting and limits, autosizing, cursor placement, disabled/keyboard toggles,
  bounds, exact decimal steps, formatter/parser round trips, focused wheel
  stepping and silent external out-of-range Reset.
- Both pages were checked at logical widths 1280, 1100 and 640 in light and
  dark/compact/RTL configurations.
- Strict Clippy passed for the gallery and tests. Changed Rust files passed
  scoped rustfmt checks; whitespace checks and the native gallery build passed.
- Native captures were visually inspected for both pages in light, narrow and
  dark/compact/RTL configurations. Decorative icons explicitly receive the
  active theme palette. Captures are under
  `build/ant-design-gpui/input-examples/` in the repository root.

The native executable was built with
`cargo rustc --locked --offline --example gallery -- -C extra-filename=-input-examples`.
Capture helpers stopped only the processes they launched. Only related tests
were run; no complete test suite or benchmarks were executed.

## Input validation (2026-10-26)

The Input-specific GPUI patches have been removed. Current results are in
[Public GPUI layout follow-up](#public-gpui-layout-follow-up). The original
implementation results below are historical unless repeated in that follow-up;
old native paragraph measurements do not apply to the public GPUI backend.

Implemented Input, TextArea, PasswordInput, SearchInput, OtpInput, InputNumber and
InputGroup using retained GPUI entities, native input handling and shared layout
geometry. Windows implementation and the finite reference matrices below are
verified. Platform and measurement limits are explicitly listed below.

- `cargo test --locked --offline --lib -- input:: input_number:: button:: locale::tests`:
  **186 passed**, 245 unrelated tests filtered out. This covers 123 input-family
  cases, 60 Button checks for shared group/search integration and three locale checks.
- Covered: grapheme navigation/deletion, UTF-16 range conversion, directional
  selection, bounded edit-delta history, typing coalescence, composition commit
  and cancellation, limits at IME completion, external replacement semantics,
  clipboard guards, multiline Enter, auto-height row limits, disabled focus,
  and refreshing stale geometry after text changes. A deterministic 250-operation
  Unicode edit/undo/redo test and a paste above 64 KB also pass.
- Additional coverage: password reveal with preserved selection/history and
  protected clipboard export, Tab/Enter activation of the visibility button,
  clear-click focus and undo, exact-once search events with source identification,
  suppression during loading/composition, affix/count layout, persistent external
  disabled state, application rebinding of scoped editor actions, and clear hit
  areas/viewport separation under compact RTL configuration.
- Action colors, frame colors/shadows and numeric handle width/opacity/heights
  now transition using the reference ease/linear curves. Tests cover interruption,
  premultiplied transparency, reduced motion, hidden windows, zero settled frames,
  and retained geometry during foreground-color changes. Numeric controls restored
  after disabling are immediately clickable; cancelled presses do not reappear.
  Keyboard actions activate on key release and cancel across availability changes.
  Independent browser measurements now check 1,440 timed samples across 96 cases
  (four themes), covering normal transitions and partial reversals. CSS reversal
  shortening is preserved for colors, shadows and numeric dimensions. The clear
  action's keyboard-focus color is immediate, matching the upstream focus rule;
  leaving keyboard focus restores the slow color transition.
- OTP coverage includes full/partial paste, formatter/filter-before-limit,
  Unicode masks, aggregate history, IME commit/cancellation, equal external
  echoes, stale queued callbacks, holes and backspace, roving Tab focus,
  RTL arrows, read-only/disabled protection and masked text export.
- Group coverage includes horizontal/vertical and nested borders/corners,
  inherited and explicit configuration, flexible composite widths, mixed numeric
  editing, clipped/opacity-correct raised decoration and unchanged Tab order.
  At 150% scaling, automatic-height inputs stretch to meet fixed-height buttons;
  explicit small-child typography and radius remain intact.
  Fitted dashed Button strokes now join the same deferred decoration queue;
  a focused stroke remains above its neighbors with group opacity applied once.
- Soft-wrap geometry retains upstream/downstream caret positions. Tests cover
  pointer selection, Home/End, vertical movement and native IME range bounds.
  Unchanged redraws reuse geometry; edits and viewport changes invalidate it.
  RTL layout now aligns each wrapped row independently. Painted text, pointer
  selection, range bounds and native IME anchors use the same offsets; empty
  placeholders keep their caret at the right edge with its stroke inside the clip.
  Direction changes invalidate geometry while preserving the value and selection.
  Typing after replacing a selection now remains one undo transaction, verified
  in both the editing model and native Windows keyboard input.
- Commands issued before the next draw now refresh a shared paragraph cache.
  Native `input --verify-navigation` passes batched Hebrew/mixed-direction arrows,
  wrapped End/Down and immediate candidate bounds after a 200-character CJK edit.
  Offscreen tests verify current revisions, scroll-to-caret and reuse of that exact
  geometry by the next frame. A weak cache reference avoids retaining an unmounted
  view through its editor state.
- Caret coverage includes 500 ms blink timing, reset after editing, steady IME
  preedit, selection/blur suppression and zero runnable blink tasks after reduced
  motion or hidden-window cancellation. Drag tests cover pointer capture outside
  the field, single-line horizontal and multiline vertical autoscroll, preserved
  anchors, reverse direction, release and provider-disable cancellation.
- Numeric coverage includes exact large integers and decimal steps, exponent
  parsing, resource bounds, intermediate drafts, range/precision interaction,
  finite standard values, representable range boundaries, parser/formatter
  context, formatter cursor/undo/redo, native IME finalization, empty values,
  read-only protection, stale queued edits after external replacement, Enter/blur
  commits, keyboard/Shift stepping, focused opt-in wheel behavior, 600/200 ms
  button repetition, and release/blur/disable/unmount cancellation. Provider
  disabling cancels held work before the next rendered frame. Cancelled holds
  clear their pressed appearance on blur or reenable.
- AccessKit text runs preserve grapheme lengths, source selection direction,
  multiline endpoints and scaled/scrolled character bounds; tests cover masked
  export, stale/foreign selection rejection and AccessKit's u8 length limits.
  SetValue, ReplaceSelectedText and SetTextSelection are wired to the editor.
  Native Windows UIA now verifies Edit/Spinner roles, Unicode Value/Text patterns,
  selections, keyboard editing and undo/redo, disabled/read-only/password flags,
  protected text export, exact-decimal text, RangeValue updates/stepping, OTP
  focus traversal and normal teardown. Run `scripts/test-input-accessibility.ps1`.
  The native test also verifies password Tab/Space/Enter visibility activation,
  protected Value/Text export while revealed, and undoable accessible search clearing.
  Audible screen-reader announcements remain unverified.
- `scripts/test-input-accessibility.ps1 -ChineseIme` passed with the installed
  Windows Chinese IME: native preedit and candidate confirmation, one-step
  undo/redo, Escape cancellation, numeric accepted-value isolation and restoration,
  and OTP's deferred focus advance and aggregate undo. The test changes only the
  fixture thread's IME mode and restores it. It exposed a pinned GPUI bridge
  behavior: empty cancellation preedit lacks an end-composition callback.
  The input adapter now handles that Windows sentinel without changing the
  general cross-platform composition model. Candidate-popup visual placement
  and other installed IMEs still need native checks.
- `cargo test --locked --offline --lib locale::tests`: 3 passed. The typed
  catalogs include all input action messages in en_US, zh_CN and zh_TW.
- Strict Clippy for the library, tests, input/gallery examples and input benchmark passed.
  The native `input` verification example builds.
- The main gallery now includes fifteen Input examples covering text, password,
  search, multiline/counts, availability, OTP, numeric formatting/exact values,
  groups, external updates and component tokens. The final focused gallery run
  passes five input/catalogue/toolbar tests; thirty unrelated tests were filtered out.
  Divider navigation now scrolls its jump link into view after the new input entry
  extended the sidebar; it restores the sidebar before switching back to Button. Native
  light, dark/compact/RTL and 640-pixel-wide captures were inspected in
  `build/ant-design-gpui/input-gallery*.png`. Input values survive navigation and
  display changes; keyboard search, exact stepping, grouped Apply, external value
  changes and programmatic focus were exercised.

Implemented common APIs now include Input, TextArea, PasswordInput, SearchInput,
InputNumber, OtpInput and InputGroup,
prefix/suffix slots, clear actions, counts with advisory maxima/custom formatters,
localized action labels, and namespaced editor key bindings. Search uses the
existing Button and its loading lifecycle. Unchanged layouts reuse their shaped
paragraphs and aggregate layout; painting shares geometry instead of copying it.

Independent original-source browser measurements cover **52,416 settled states**:
8,064 default-token Input/TextArea/InputNumber/unmasked OTP states; 24,192 states
with typography, explicit geometry and color token patches; and 20,160 composite
states with default/custom typography. The base/custom tests compare height,
padding, font/line metrics, radius, border width and colors across four themes,
three sizes, four variants, validation, availability and direction states.
The composite matrix adds affixed Input, Password, counted TextArea, both Search
actions, joined Input/Button groups and masked OTP. It checks frame geometry and
colors, search-button dimensions/colors, affix/clear gaps, password-icon color and
OTP mask typography/color. It does not assert every absolute child position or
pixel-identical icon/glyph rasterization. See `fixtures/input/README.md` for exact
coverage, provenance, stylesheet-order assumptions and regeneration.

This comparison corrected inherited small-font defaults, font-dependent padding,
underlined disabled-hover backgrounds, borderless affix padding, Search button
variants/heights, hidden-clear spacing and masked OTP typography. Additional
offscreen tests cover custom small Search stretching, clear-slot stability in
both directions, mask centering and constrained/block OTP cell widths.
Final native captures were inspected at 150% scaling:
`build/ant-design-gpui/input-composite-final-light.png` (1620×1260),
`input-composite-final-filled-typography.png` and
`input-composite-final-dark-rtl.png` (both 1620×1560). The latter uses dark/compact,
RTL, underlined composites and custom typography. The installed Chinese IME and
native UIA suite passes on this final build; its output is saved in
`build/ant-design-gpui/input-uia-final.txt`.

The reference exposed and corrected intrinsic OTP sizing, underlined backgrounds
and padding, fractional-scale borders, large TextArea line-height and the small
underlined numeric radius rule. Native light and dark/compact/RTL captures were
inspected in `build/ant-design-gpui/input-native-reference-adjusted*.png`.
The subsequent `input-native-motion*.png` light and dark/compact/RTL captures
are pixel-identical (zero changed pixels in both 1620×1260 images), verifying that
the uniform-color glyph paint path preserves settled native rendering. Offscreen
tests use mock fonts that do not rasterize glyphs; they verify retained geometry,
not pixel output.

Release diagnostics run through `scripts/run-input-perf.ps1`, which checks the
repository's `windows-msvc-performance` preset and rejects non-Release builds.
The 100,000-byte Unicode note workload records latency percentiles and process-wide
allocation counts (including reallocation sizes). Representative measured CPU p50/p95:

| Workload | p50 | p95 |
| --- | ---: | ---: |
| 100 KB middle edit + undo, editing model | 2.75 ms | 2.85 ms |
| Large paste + undo + redo | 3.73 ms | 4.05 ms |
| 52-control mixed form redraw | 1.99 ms | 2.24 ms |
| 100 KB note warm redraw | 0.076 ms | 0.143 ms |
| 100 KB note color change + redraw | 0.173 ms | 0.266 ms |
| 100 KB edit/draw/undo/draw | 5.98 ms | 7.13 ms |
| One 100 KB paragraph, warm redraw | 0.449 ms | 0.551 ms |
| One 100 KB paragraph, edit/draw/undo/draw | 27.10 ms | 31.16 ms |

Shared immutable value snapshots and exact geometry capacities reduced allocated
bytes per warm note redraw from 423,180 to 23,796, and per edit/draw/undo/draw cycle
from 27,876,013 to 8,553,705. Editing-model navigation/UTF-16 queries allocate
nothing; a stale visual-navigation query can reshape the changed paragraph. The note
retains 3,720,016 bytes of caret/cluster payload, plus text, indexes, paragraph
layout and GPUI storage. Tests assert no scheduled frames for settled controls.
These are diagnostic offscreen CPU measurements using GPUI's mock text system;
they include allocator instrumentation and do not measure native font shaping,
GPU presentation, physical input latency or total process memory. No hard latency
threshold was agreed. Raw results are in the ignored performance build directory
(`input-perf-composites-and-navigation.txt` for the final run). The edit workloads assert that insertion
actually changed the value before drawing, including after disabled-color workloads.
The single-paragraph case allocates about 50.2 MB per edit/draw/undo/draw pair
because the entire paragraph is reshaped twice; this remains a performance
limitation for very long paragraphs, distinct from the cached multi-paragraph case.

`build/ant-design-gpui/input-native-numeric.png` and
`input-native-numeric-dark-compact.png` were inspected at 150% display scaling
(1620 by 1170 pixels). They add small standard-number, medium exact-decimal and
large currency controls to the native surface. These are rendering smoke checks,
not independent Ant Design browser comparisons. Numeric styles use the upstream
InputNumber tokens, including filled-handle compositing, focused handle backgrounds,
borderless separators, and RTL handle placement. Native RangeValue/Value-pattern
operation is now tested; audible screen-reader text announcements remain unverified.

The initial paragraph renderer used GPUI wrapped-line positioning. A hidden native
Windows capture at 150% display scaling now renders 1620-by-1020 pixels to
`build/ant-design-gpui/input-native-initial.png`. It was visually inspected for
ordinary text, CJK, combining marks, emoji, multiline wrapping, sizes and variants.
This is an implementation smoke check, not an Ant Design reference comparison.
The initial 1-by-1 capture was resolved by explicitly resizing the hidden window
before drawing; Win32 defers its initial placement while hidden.

`build/ant-design-gpui/input-native-controls.png` adds password, joined search,
clear, prefixes/suffixes and counts at 1620-by-1170 pixels. Comparing the initial
control capture with the upstream style source identified the multiline clear
icon's top alignment and count text color; those styles were corrected. These
captures are still smoke checks, not independent visual comparison fixtures.
The dark/compact/RTL capture also identified insufficient clear padding with
compact tokens. The clear size now uses `fontSizeIcon`, and reserved padding is
at least the icon's full inline extent plus a one-pixel gap. The corresponding
offscreen test verifies both viewport separation and pointer activation.

### Public GPUI layout follow-up

The renderer now uses the existing `shape_text` / `WrappedLine` path on all
platforms. Horizontal navigation follows logical grapheme and word boundaries,
as in GPUI Kit (reference commit `611b55e331170ca375214d580b6c5409db858f83`).
The Windows DirectWrite paragraph extension, both vendored crates, the vendoring
script, native-paragraph benchmark and browser-bidi acceptance fixture were removed.
Cargo resolves the same shared GPUI Git revision without workspace patches.

The retained geometry cache provides wrapping, hit testing, selection, accessibility
and immediate IME range queries. Wrapped-row affinity remains necessary. Full
Arabic/Hebrew shaping, mixed-direction selection fragments and ligature caret
precision are limited by the existing GPUI backend. RTL control alignment remains.
GPUI Kit uses a different pinned GPUI package; this change follows its public-API
architecture and logical navigation, not a claim of identical backend rendering.

Current Windows x64 verification on 2026-10-26:

- `cargo test --locked --offline --lib -- input:: input_number::`: **121 passed**,
  308 unrelated tests filtered out. This includes new logical navigation checks
  for Hebrew, mixed text, Arabic, CJK, combining marks and emoji in both layout
  directions, plus the retained IME, wrapping, masking, history and geometry tests.
- `cargo build --locked --offline --example input` and native
  `input.exe --verify-navigation`: passed against the unmodified Git dependency.
- `cargo test --locked --offline --example gallery -- input_gallery
  catalogue_keeps toolbar_controls`: **5 passed**, 30 unrelated tests filtered out.
- `scripts/test-input-accessibility.ps1 -SkipBuild -ChineseIme`: passed, including
  Unicode Text/Value patterns, logical mixed-text navigation/selection, protected
  passwords, native undo/redo, Chinese preedit/candidate confirmation/cancellation,
  numeric draft isolation and OTP focus/aggregate history.
- `cargo clippy --locked --offline --lib --tests --example input --example gallery
  --bench input -- -D warnings` and `cargo fmt --all --check`: passed.
- Light and dark/compact/RTL captures at 150% scaling were visually inspected:
  `build/ant-design-gpui/input-unpatched-{light,rtl}.png`. Ordinary/CJK text,
  multiline wrapping, passwords and composites render coherently. Mixed
  Arabic/Hebrew rendering retains the known GPUI limitation; no browser-bidi
  correctness claim is made.
- Windows-target Cargo metadata confirms both `gpui` and `gpui_windows` resolve
  to Git revision `2c0bf70b54c018f99419f386d1cf1105dca57661`, with no path patches.
  The license collector passed: 486 notices, 256 external runtime Rust packages
  and 38 repository vcpkg packages. The new bundle is
  `build/ant-design-gpui/input-unpatched-license-audit/third-party/`.

The existing gallery process holds `debug/examples/gallery.exe` open, preventing
an in-place relink (LNK1104). It was left running. A native gallery build passed
with `cargo rustc --locked --offline --example gallery -- -C extra-filename=-unpatched`,
producing `debug/examples/gallery-unpatched.exe`; the focused integration tests
also passed. No full test suite or new performance benchmark was run.
macOS/Linux native behavior remains unverified.

### Requirement coverage and remaining limits

| Requirement | Implementation and verification |
| --- | --- |
| Basic editing and Unicode | Grapheme/UTF-16 model, native input handler, selection, clipboard, undo/redo, word/line navigation; deterministic tests and native UIA/Chinese IME |
| Text variants | Single/multiline, password reveal, Search, auto-height and segmented OTP; retained-state tests, native interactions and reference matrices |
| Common configuration | Sizes, variants, validation, placeholder, clear, affixes, counts/limits, disabled/read-only; token matrices and rendered layout tests |
| Numeric values | Finite standard values, exact decimal, empty/intermediate drafts, ranges, precision, parser/formatter and all step sources; model, offscreen and native Spinner tests |
| Groups | Joined borders/corners, inherited sizing, flex layout and raised focus; offscreen geometry/clipping/Tab tests and reference styles |
| Application integration | Silent external setters, readable retained values, change/commit events, programmatic focus; deterministic and gallery tests |
| Accessibility/localization | AccessKit roles, protected values, text selection/actions and numeric ranges; Windows UIA and complete en_US/zh_CN/zh_TW typed catalogs |

Known discrepancies and unverified areas:

- Windows native rendering and accessibility are verified. macOS/Linux retain
  the public GPUI backend and need native testing, especially mixed bidi text.
- Left/Right and word movement use logical source order in both layout directions.
  Browser-equivalent visual navigation and bidi geometry are outside this scope.
- Search's filled hover treatment follows the Search-specialized CSS order in
  the fixture; an Ant application registering equal-specificity Button styles
  later can paint a different hover background.
- Native UIA verifies accessible semantics, but audible Narrator announcements
  were not manually verified. Chinese composition/candidate confirmation and
  native candidate geometry pass; the visible candidate-popup position and other
  installed IMEs were not visually verified. UIA's managed client reports empty
  degenerate caret rectangles, so native geometry tests cover caret bounds.
- Browser measurements and native captures establish style/geometry fidelity for
  the listed matrices, not pixel-identical glyph rasterization or an exhaustive
  raster comparison of every state and arbitrary custom token combination.
- Very long single-paragraph edits still reshape that paragraph. Cached
  redraws avoid reshaping. GPU presentation and end-to-end physical input latency
  were not measured; no hard latency threshold was agreed.

The original formatting, Clippy, focused tests and native-example results are
historical; current verification is recorded in the follow-up above. Only related tests were run for this input implementation;
the repository-wide and unfiltered crate suites were not run.

## Historical foundation checks

- `cargo fmt --check`: passed.
- `cargo check --locked --offline --lib --examples --benches`: passed.
- Strict Clippy for the library, tests, example and benchmark: passed.
- `cargo test --locked --offline --lib`: **57 passed**.
- `cargo test --locked --offline --doc`: **2 passed**.
- Icon generator freshness: all **829** icons and generated descriptors matched.
- Theme fixture regeneration: all **20** independently executed upstream cases
  regenerated byte-identically, SHA-256
  `4feece94a7f8409e6e476ecf61cd28fed965b8bd0d3915700c821f93ff7a4935`.
- Third-party collector: **482 notices**, covering 254 runtime Rust packages and
  38 vcpkg entries included by the shared collector. This library does not link
  those vcpkg/Qt packages.

Only the foundation crate's related tests were run. The repository-wide suite
was not run.

## Native Windows verification

The example was built and exercised through real Windows UI Automation. Checks
passed for names, button roles, keyboard focus, exactly-once accessible invocation,
disabled focusability, busy activation suppression, live availability changes,
all three locale updates, and normal native-window teardown.

Visual inspection covered the light/default configuration and dark/compact
Simplified Chinese with RTL layout and reduced scoped motion. Local screenshots
are in the ignored `build/ant-design-gpui/` directory.

Offscreen regression tests additionally verify actual `UiRoot` focus recovery
after disabled/removed controls, subscribed cached consumers, unchanged-target
zero-duration transitions, and cached-view icon leases across CPU-LRU eviction.
The latter checks that cached replay does not redraw or reference freed atlas
entries, working-set overflow reuses resident images, and unmounting releases
the final scene's image.

macOS/Linux native accessibility and rendering remain to be verified on those
platforms. Windows UIA checks do not constitute manual screen-reader validation.

## Release measurements

Run using `scripts/run-foundation-perf.ps1`, which verifies the configured
`windows-msvc-performance` preset and uses Cargo's optimized Release profile.
These are diagnostic CPU/cache measurements on this host, not input-to-display
latency or hardware-independent performance thresholds.

| Workload | Iterations | Mean time |
| --- | ---: | ---: |
| Complete cold light-theme resolution | 2,000 | 10.06 µs |
| Snapshot read at scope depth 32, including test-context update | 200,000 | 55.3 ns |
| No-op configuration update | 50,000 | 281.7 ns |
| Configuration propagation through 32 scopes | 2,000 | 9.99 µs |
| Warm two-tone raster lookup | 100,000 | 99.2 ns |
| Catalog rasterization with cache churn | 2,000 | 35.49 µs |

The warm icon workload asserted no new parse or raster misses. After churn the
CPU cache remained at 256 parsed entries, 512 rasters and 524,288 raster bytes.
Active/cached scene leases are measured separately from CPU-LRU ownership.

Identity fast paths reduced the measured 32-scope update from approximately
42.92 µs in the initial implementation to 9.99 µs. No-op identity, obsolete-theme
release and local-catalog reuse are also covered by deterministic tests.

## Button component validation (2026-10-26)

Validated on the same Windows x64 host and pinned Rust/GPUI versions above.

- `cargo test --locked --offline --lib button::`: **43 passed**, 58 filtered out.
  These include 28 offscreen interaction/layout tests, nine token tests, five
  stroke geometry/cache tests and one independent browser comparison test.
- The browser comparison checks **780 snapshots** from the original Ant Design
  source: four themes, three interaction states and 65 controls per case. It
  checks colors, borders, dimensions, padding, radii, font metrics, loading opacity
  and shadows. See `fixtures/button/README.md` for provenance and regeneration.
- Two focused font bridge tests and the existing focus-outline regression passed.
  Windows skips Apple-only font aliases so Ant's Segoe UI family takes precedence.
  Pill focus outlines clamp their radius to the actual painted dimensions.
- The README examples compiled and passed. Normal library and native example
  builds, `cargo fmt --check`, strict Clippy for the library/tests/Button example/
  Button benchmark, and `git diff --check` passed.
- Repeating the independent browser capture produced byte-identical fixtures,
  SHA-256 `0bc7761289a8b195bf41af81b29b7a5884d00f2a07a12c3df3112329e13a6827`.

The offscreen tests cover pointer/keyboard activation, held-key suppression,
pointer exit and reentry, release outside, source-order traversal, disabled and
busy semantics, retained/internal focus, subscribed cached consumers, delayed
loading cancellation and unmount, loader entry/exit, interrupted transitions,
RTL icon placement, Chinese label spacing, and reduced/disabled motion.
Idle controls and static custom loaders schedule no continuous animation frames.

`scripts/test-button-accessibility.ps1 -SkipBuild` passed twice on the final
native gallery. It checks names/roles, icon-only labels, native focus and Tab,
exactly-once Invoke/Enter/Space, disabled focus removal, loading suppression and
recovery, configuration changes and normal shutdown. The interactive fixture
explicitly activates and verifies its own foreground HWND before child focus;
the top-level AccessKit provider does not expose a focus action itself.

Native light and dark/compact/RTL screenshots were inspected against the original
browser-generated reference. Primary's width matches at **117 physical pixels**
at 150% scaling. Thin dashed borders match the measured **3-pixel dash / 2-pixel
gap** pattern. Additional source measurements at 100%/150% and border widths
1/2/3 verify device-pixel snapping and the thicker-stroke pattern. Rounded stroke
geometry is cached independently of color and rebuilt when geometry or display
scale changes. Screenshots and measurement JSON are under the ignored
`build/ant-design-gpui/button-reference/` directory.

macOS/Linux native rendering and accessibility remain to be validated on their
respective platforms. No repository-wide or complete library test suite was run.

### Button Release measurements

Run with `scripts/run-button-perf.ps1` under the verified
`windows-msvc-performance` preset and Cargo's optimized Release profile. The grid
measurements redraw 100 retained controls through the offscreen layout/paint
pipeline; they are CPU diagnostics, not GPU presentation latency. Every grid
iteration also asserts that idle controls schedule zero animation frames.

| Workload | Iterations | Mean time |
| --- | ---: | ---: |
| Style from cached component tokens | 200,000 | 21.0 ns |
| Style including cached theme lookup | 200,000 | 43.5 ns |
| Builder with label, primary type and icon | 200,000 | 70.0 ns |
| Warm redraw of 100 buttons | 2,000 | 2.00 ms |
| Warm redraw of 100 buttons with icons | 2,000 | 2.69 ms |
| Warm redraw of 100 dashed buttons | 2,000 | 2.94 ms |

These measurements vary with host load. The final raw output is in
`build/ant-design-gpui/button-performance.txt`; no hardware-independent timing
threshold is imposed by tests.

## Button refinement validation (2026-10-26)

The refined control uses one native GPUI root for parent layout, input, semantics
and painting. Percentage widths, flex growth, margins, styled borders and custom
corners therefore share the same focus and hit bounds. Icon/content refinements
retain independent styling; inherited line heights scale with their font sizes.
Chinese two-character tracking uses a single native text shape without modifying
the text system's cached layout. Loader entry/exit scales its atlas artwork and
keeps the default spinner rotating until it leaves.
Subtle hover backgrounds interpolate continuous alpha; hyperlink controls do not
paint a pressed state for Space.

- `cargo test --locked --offline --lib button::`: **59 passed**, 58 filtered out.
  This includes **2,208 independently captured browser style comparisons** across
  four themes and three interaction states. Icon font sizes and artwork bounds
  are now checked as well as the control's complete visual tokens.
- `cargo test --locked --offline --example gallery`: **7 passed**. The gallery
  now contains nine complete previews, including interactive Loading and semantic
  styling. Its loading regression verifies keyboard/pointer activation, retained
  busy focus, repeated activation suppression and the exact three-second reset.
- The focused shared focus-outline, traversal/input-modality and monochrome icon
  atlas-reuse regressions each passed.
- `cargo check --locked --offline --lib --examples --benches`, strict Clippy for
  the library/tests/Button and gallery examples/Button benchmark, `cargo fmt
  --check`, and `git diff --check` passed.
- `scripts/test-button-accessibility.ps1 -SkipBuild` passed against the rebuilt
  native example: roles/names, focus, exactly-once Invoke/Enter/Space, availability,
  delayed loading, theme/density/RTL changes and normal teardown.

Native light and dark/compact/RTL captures were visually inspected. A separate
24-control native/browser probe at **150%** display scaling compared English and
Chinese labels in all three sizes, including disabled, loading and automatic
spacing opt-outs. Every height matched exactly; all widths differed by at most
one physical pixel. Automatically spaced resting/disabled Chinese labels differed
by less than **0.063 logical pixels**. Text antialiasing remains renderer-specific.
Captures, source and measurements are under the ignored
`build/ant-design-gpui/cjk-parity/` and `button-reference/` directories.

A second native probe verified independently colored 20px icons and 18px English
and Chinese labels, proportional 24px root typography, explicit icon-only size
precedence, individual styled corners and exact full-width parent layout. Its
capture, measurements and source are under `build/ant-design-gpui/semantic-parity/`.

The probe exposed a reference-adapter error: the upstream style hook supplies a
root class when registering common styles. The adapter now supplies that class,
so font resets affect the root and large icons correctly inherit 16px (14px in
compact themes). All browser fixtures and source provenance were regenerated.

The Button-only Release benchmark passed under the verified
`windows-msvc-performance` preset. A final sample after unrelated compiler
processes exited gave:

| Workload | Final mean time |
| --- | ---: |
| Style from cached component tokens | 16.6 ns |
| Style including cached theme lookup | 40.3 ns |
| Builder with label, primary type and icon | 73.6 ns |
| Warm redraw of 100 buttons | 1.61 ms |
| Warm redraw of 100 buttons with icons | 2.28 ms |
| Warm redraw of 100 dashed buttons | 2.18 ms |

Earlier diagnostic samples overlapped unrelated C++ compilation on the shared
host: plain grids ranged from 1.56–1.73 ms, icon grids 2.31–3.30 ms and dashed grids
2.33–3.58 ms. These samples do not establish a precise speedup over the earlier
run. Every redraw iteration still asserted zero scheduled idle frames. Raw
results are in
`build/ant-design-gpui/button-refinement-performance.txt` and
`button-refinement-performance-second-sample.txt`; the final sample is
`button-refinement-performance-final.txt`. Default and monochrome custom loaders
reuse GPU atlas transforms. Opt-in colored-icon rotation uses CPU raster variants
on this pinned GPUI backend; static colored loaders have no frame loop.

No complete library or repository test suite was run. macOS/Linux native rendering
and accessibility still require validation on their respective platforms.

## Gallery refinement validation (2026-10-26)

The gallery now has persistent theme/density/direction/motion controls, anchored
example navigation, a reachable Back to top action, a responsive icon rail and
title-first preview cards. Planned previews state their missing capabilities
without rendering substitute controls. The reference demo order and all 90 real
Button controls are retained. Its Color & Variant matrix uses Small below the
theme's `screen_xxl` breakpoint and Medium at or above it, as upstream does.

- `cargo test --locked --offline --example gallery`: **11 passed**.
- Strict Clippy for the gallery and `cargo fmt --check`: passed.
- Native build passed with `cargo rustc --locked --offline --example gallery --
  -C extra-filename=-refined`. The alternate filename preserved an existing
  running gallery whose executable was locked by Windows.
- Native light and dark/compact/RTL captures at 1280×900 and a narrow capture at
  640×720 were inspected at 150% display scaling. The icon rail, persistent
  toolbar, controls, descriptions and single-column cards fit the narrow window.

The focused tests cover padded-width breakpoints, sidebar/toolbar independence
from page scrolling, planned-card status, keyboard toolbar activation/traversal,
live theme composition, nested anchor jumps after scrolling/resizing, top reset,
scroll clamping after density changes, retained loading/callback behavior and CLI
flags including valid/invalid explicit window sizes.

Native screenshots are in `build/ant-design-gpui/gallery-refinement/`:
`light-final.png`, `dark-compact-rtl-final.png` and `narrow-640-cli.png`. A background
UI Automation probe could not inspect the hidden window's toolbar; native toolbar
UI Automation has not been confirmed. Keyboard and activation behavior passed
through the actual GPUI element path in the offscreen tests.

Only the gallery's focused tests were run for this refinement; no complete
library or repository test suite was run.

## Divider component validation (2026-10-26)

Divider uses native GPUI flex layout, theme scopes and separator semantics.
Rails paint directly, without a layout child per dash or dot. Their bounded
geometry caches survive scrolling and color changes. Unset provider size is
retained separately from its medium fallback, preserving Ant's default 24px
untitled spacing and its explicitly configured 16px medium spacing.

- `cargo test --locked --offline --lib divider::`: **21 passed**. This includes
  13 offscreen layout/semantic tests, four rail geometry/cache tests, three token
  tests and the independent browser comparison test.
- The browser comparison checks **592 measurements** from the original Divider
  style generator across four themes, both directions, sizes, orientations,
  titles, variants and component-token overrides. Repeated capture was identical:
  SHA-256 `ea2a5a47d0d6afa8a3c74eea2fa978f0c36ab5995f74ff63274bb5cd4902096e`.
  See `fixtures/divider/README.md` for provenance and regeneration.
- `cargo test --locked --offline --lib config::tests`: **9 passed**, covering
  inherited size presence/removal, explicit-medium invalidation and no-op identity.
- `cargo test --locked --offline --example gallery divider`: **5 passed**,
  covering nine previews, responsive layout, scoped tokens, navigation and capture.
- The README examples compiled and passed. Strict Clippy passed for the library,
  tests, gallery and Divider benchmark. Formatting, `git diff --check` and the
  native gallery build passed.

Native light, dark/compact/RTL and narrow captures were inspected at 150% display
scaling. A Windows UI Automation probe inspected the hidden preview through its
HWND and found **27 native Separator controls**, with the expected title names,
stable automation IDs and no keyboard focus stops. Offscreen tests also verify
orientation and absence of focus/click actions. Captures remain under ignored
`build/ant-design-gpui/divider-reference/`.

Independent browser pixel probes at 100%/150%/200% verified snapped stroke widths,
thin/thick dash proportions, short-rail solid/proportional patterns and square
versus round dots. Text antialiasing and minor dotted endpoint rasterization
remain renderer-specific. Native macOS/Linux rendering is not validated here.

Release measurements use `scripts/run-divider-perf.ps1` beneath the verified
`windows-msvc-performance` preset. Final 100-control warm draw means were
0.40ms for solid untitled, 1.31ms for solid titled, 1.45ms for dashed titled and
1.44ms for dotted titled. Every draw asserted **zero idle animation frames**.
These are CPU diagnostics rather than GPU presentation latency or CI thresholds;
the final raw sample is `build/ant-design-gpui/divider-performance.txt`.

Only the related component/configuration/gallery tests were run; no complete
library or repository test suite was run.

## Checkbox validation (2026-10-26)

Checkbox and CheckboxGroup use retained GPUI state, native focus and accessibility,
typed group values and stable option IDs. The tick is cached vector geometry;
scrolling, color changes and idle redraws reuse its mesh. Colors interpolate in
premultiplied sRGB. Tick scale and opacity follow the source's separate transitions,
and the checked-only wave uses Ant Design's short Checkbox timing. Scoped and
application reduced motion stop animation frames.

- `cargo test --locked --offline --lib checkbox::`: **31 passed**.
- `cargo test --locked --offline --example gallery -- checkbox catalogue toolbar
  demo_jumps`: **6 passed**, covering Checkbox's examples and shared navigation.
- The two Checkbox README examples compile; the shared README's merged doctest
  runner executed **6 passing examples**.
- Strict Clippy for the library, tests, gallery and Checkbox benchmark: passed.
- `cargo fmt --check`, native gallery build and whitespace checks: passed.
- `scripts/test-checkbox-accessibility.ps1 -SkipBuild`: passed against the native
  Windows gallery, including names/roles, TogglePattern, checked/mixed state,
  disabled focus and invocation guards, Tab/Space/Enter, controlled updates,
  check-all group semantics, theme/density/RTL changes and normal window teardown.

The independent browser fixture contains **576 states** executed from the pinned
Ant Design 6.6.7 styles using Edge 156.0.4314.42. Comparisons cover four themes,
LTR/RTL, all checked/mixed/disabled combinations, hover/focus, empty and wrapped
labels, colors, spacing, outlines and the exact transformed tick polygon. It also
preserves the source's combined checked/mixed hover cascade. Regeneration and
source/package hashes are documented in `fixtures/checkbox/README.md`.

Native light, dark/compact/RTL and 640x720 captures were visually inspected.
Screenshots are in `build/ant-design-gpui/checkbox-light.png`,
`checkbox-dark-compact-rtl.png` and `checkbox-narrow.png`.

Release CPU measurements ran through `scripts/run-checkbox-perf.ps1` under the
verified `windows-msvc-performance` preset. Cached style resolution measured
49.2ns; builders measured 46.0ns. Warm draws of 100 controls measured 1.78ms for
checked controls, 1.83ms for mixed controls and 1.88ms for a group. All **12,000**
draw iterations asserted zero scheduled idle frames. These are diagnostic CPU
means, not GPU presentation latency or hardware-independent thresholds. The raw
sample is `build/ant-design-gpui/checkbox-performance-final.txt`.

No complete library or repository test suite was run. Native macOS/Linux rendering
and accessibility still require validation on their respective platforms.

## Segmented component validation (2026-10-26)

Validated on Windows x64 with the pinned Rust and GPUI versions. The implementation
uses retained typed selection, stable option focus handles, native radio semantics,
and a single GPU-painted selection surface measured from actual GPUI item bounds.
Option reconciliation is linear; fonts, palettes, shadows and styled shadow variants
are cached. Unstyled options avoid carrying an inline native style refinement.

- `cargo test --locked --offline --lib segmented::`: **34 passed**, including
  interaction, retained state, focus, layout, motion, style tokens and the independent
  browser comparison.
- `cargo test --locked --offline --example gallery segmented_`: **3 passed**, covering
  the 13 public reference examples, responsive layout, sidebar navigation, anchors,
  controlled selection, dynamic options and retained state across display changes.
- Strict Clippy for the library, tests, gallery and Segmented benchmark: passed.
- Native gallery build, focused Rust formatting checks and `git diff --check`: passed.
- `scripts/test-segmented-accessibility.ps1 -SkipBuild`: passed. Native Windows UIA
  checks Group/RadioButton roles, names, both group and item selection patterns,
  exactly one selected option, accessible selection, disabled guards, Space/Enter,
  arrows/Home/End, one-stop Tab traversal, controlled updates, icon-only labels,
  live theme/density/RTL changes and normal window teardown.

The independent browser fixture compares **576 states / 1,728 items** executed
from Ant Design 6.6.7's original styles: four themes, both directions, three sizes,
both shapes and orientations, disabled states, icons, block layout, custom tokens,
hover, active and focus. Metrics include radii, padding, line heights, icon gaps,
all shadow layers and focus outlines. Chromium alpha comparisons account for its
8-bit serialization. See `fixtures/segmented/README.md` for provenance and regeneration.
Fixture SHA-256: `c6f245d9395672a902942c303f20508558364f405c7b5832425cea557ff732f4`.

Native light/default and narrow dark/compact LTR/RTL captures were visually inspected.
They are stored under ignored `build/ant-design-gpui/segmented-{light,dark-compact-ltr,
dark-compact-rtl}.png`. Offscreen checks additionally verify interrupted thumb/color
motion, resize snapping, pointer drag exit/reentry, reduced motion, custom surface
corners/opacity, and translucent background/shadow overrides without duplicate layers.

Release CPU measurements used `scripts/run-segmented-perf.ps1` with the verified
`windows-msvc-performance` preset. All **16,000** draw/rerender iterations asserted
zero scheduled idle frames. Diagnostic means on this host:

| Workload | Mean time |
| --- | ---: |
| Style from cached tokens | 9.1 ns |
| Style with cached theme lookup | 31.3 ns |
| Three-option builder | 241 ns |
| 100-option builder | 3.78 µs |
| Warm draw of 100 three-option groups | 11.00 ms |
| Warm draw of 100 round groups | 8.16 ms |
| Warm draw of 100 groups with icons | 11.52 ms |
| Warm draw of 100 vertical groups | 6.85 ms |

Making option style refinements optional reduced the same 100-option builder from
45.93 µs to 3.78 µs. Draw measurements vary with host load and concurrent work;
they measure CPU layout/paint, not GPU presentation latency. Raw output is in
`build/ant-design-gpui/segmented-performance.txt`.

Only Segmented's related tests were executed. Native macOS/Linux rendering and
accessibility still require validation on those platforms.

## Switch component validation (2026-10-26)

Switch uses retained GPUI state, the shared activation guard, native Switch
semantics and cached component tokens. Affine layout offsets resolve handle and
content positions in one layout pass, including RTL, intrinsic content sizing
and negative token padding. An empty resting switch uses just its track and
handle; text and custom content measure both states to avoid width changes.
Interrupted handle/color transitions remain continuous, and reduced motion
settles immediately. Loading reuses the monochrome icon atlas with GPU rotation.

- `cargo test --locked --offline --lib switch::`: **19 passed**, including
  interaction, lifecycle, styling, motion, token, geometry and browser tests.
- Seven related gallery checks passed: four Switch checks and the shared catalog,
  toolbar and anchor-jump regressions. The expanded catalog requires scrolling
  the sidebar to reach later jump links; the test uses real wheel input and
  verifies that navigation scrolling does not move the example page.
- Strict Clippy for the library, tests, gallery and Switch benchmark passed.
  Formatting, native builds and whitespace/UTF-8 LF checks passed.
- The Switch README example compiled and ran through rustdoc's merged runner
  with ten passing README examples.
- `scripts/test-switch-accessibility.ps1 -SkipBuild` passed on both native
  builds, including the final optimized executable. Checks cover native names,
  TogglePattern, checked state, exactly-once accessible activation, Space/Enter,
  physical arrow behavior in RTL, Tab, disabled/loading focus removal and
  recovery, theme/density/direction updates and normal teardown. AccessKit maps
  Switch to a Windows Button exposing TogglePattern; localized control-type text
  follows the operating system language.

The independent fixture compares **1,664 browser states** executed from the
original Ant Design 6.6.7 Switch style source across four themes, both directions,
both sizes, rest/hover/focus/press, checked/disabled/loading states, asymmetric
content and all fourteen component-token overrides. It preserves the source's
disabled handle pseudo-element shadow and empty-content intrinsic width.
Final recapture was byte-identical, SHA-256
`03d8ea9e0a3cf436122f3c100631657f4bb8e0619e2db801ec033a5c9b43d7cf`.
See `fixtures/switch/README.md` for provenance and regeneration.

Native light, dark/compact/RTL and narrow captures were visually inspected at
150% display scaling. They remain under `build/ant-design-gpui/` as
`switch-light.png`, `switch-dark-compact-rtl.png` and `switch-narrow.png`.

Release measurements ran through `scripts/run-switch-perf.ps1` under the verified
`windows-msvc-performance` preset. Cached style resolution measured **47.2 ns**;
builders measured **95.4 ns**. Warm draws of 100 switches measured **1.15 ms**
for plain controls, **4.25 ms** with text and **4.67 ms** with icons. All **12,000**
draw/rerender iterations asserted zero scheduled idle frames with motion enabled.
These are CPU diagnostics collected on a shared host, not GPU presentation
latency or fixed performance thresholds. Raw output is in
`build/ant-design-gpui/switch-performance-final.txt`.

No complete library unit-test or repository suite was run. Native macOS/Linux
rendering and accessibility still require validation on those platforms.

## Radio implementation

Validated on Windows x64 on 2026-10-26 against the pinned Ant Design 6.6.7 source.

- `cargo test --locked --offline --lib radio::`: **47 passed**.
- `cargo test --locked --offline --example gallery radio_gallery`: **3 passed**.
- Strict Clippy for the library, tests, gallery and Radio benchmark passed.
- All Radio source, tests, gallery and benchmark files passed rustfmt checks.
- Independent original-source browser fixtures compare **3,200 controls** across
  four themes and rest/hover/active/focus states, including RTL, wireframe, custom
  tokens, all button sizes and connected group geometry.
- Windows native UI Automation passed on the gallery: RadioButton roles/names,
  SelectionItemPattern, exclusive selection, disabled guards/focus release,
  Space/Enter, roving Tab/Shift+Tab, arrow wrapping, live theme/density/RTL changes
  and normal window teardown.
- Earlier light, dark/compact/RTL and narrow native gallery captures are under
  ignored `build/ant-design-gpui/radio-qa/`; the dot-path change is verified by
  offscreen scene geometry and timing checks.

Offscreen regression checks cover controlled proposals, retained defaults, rapid
inputs before redraw, scope updates inside cached views, stable focus across
reorder, disabled choices, native circle/dot geometry at 150% display scaling
with default and customized borders,
dot size/opacity/reversal, live dot-size tokens and color transition timing,
immediate label/button-border changes, connected borders/corners,
full-width and vertical layouts, continuous motion, idle frame scheduling,
transparent hover-border waves, hidden controls, invalid custom spacing and
clipped decoration opacity.

Release CPU measurements ran through `scripts/run-radio-perf.ps1` under the
verified `windows-msvc-performance` build. Lazy option style storage reduced the
100-option builder from **43.41 us to about 3 us** on this host. In two focused
runs, cached circular and button style resolution measured **56-70 ns** and
**93-98 ns**; warm draws of 100 standard radios measured **2.27-2.92 ms**. Every
warm draw/rerender iteration asserts zero idle animation frames, and group runs
assert retained focus identity. These CPU measurements vary with host load and
are diagnostic; they do not measure GPU presentation latency.

Only Radio's related tests were run. Native macOS/Linux rendering and accessibility
remain unverified on those platforms.

## Component motion audit (2026-10-26)

Compared the seven implemented components with `E:/ant-design` at revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, including each component's style
generators, `_util/wave/{style.ts,WaveEffect.tsx,useWave.ts,util.ts}` and the shared
icon spin rule in `components/style/index.tsx`.

| Component | Animation rules checked |
| --- | --- |
| Button | Mid-duration colors with `motionEaseInOut`; slow loader insertion/removal; one-second linear spinner; regular wave. |
| Checkbox | Slow box colors with CSS `ease`; delayed mid-duration tick entrance with `motionEaseOutBack`; fast tick exit with separate opacity easing; immediate wrapper label; quick wave when checking. |
| Radio | Mid-duration ring colors with CSS `ease`; slow dot size/opacity/color with `motionEaseInOutCirc`; immediate button border; quick circle wave and regular button wave. |
| Switch | Mid-duration track colors with CSS `ease`; handle and content movement with CSS `ease-in-out`; one-second spinner; regular wave. |
| Segmented | Mid-duration colors with CSS `ease`; slow thumb movement with `motionEaseInOut`; continuous interrupted selection. |
| Tag / CheckableTag | Mid-duration colors with CSS `ease`; regular wave for clickable ordinary tags; immediate component removal. |
| Divider | Static styling; no animation or frame loop. |

Fixed Radio's incorrect short button wave and checked-only wave gate. All five
wave consumers now share timing, color selection and retained wave lifetimes:
quick waves use `motionDurationSlow` / `motionEaseInOut`; regular waves expand to
6 px over 400 ms and fade from 0.2 opacity over two seconds with `motionEaseOutCirc`.
Opaque white and transparent wave colors are skipped. Color is captured per click,
multiple clicks retain independent fading waves, and unchecking Checkbox suppresses
only the new wave. Disabling motion cancels all waves permanently.

Also corrected Button's interpolation to premultiplied sRGB so transparent fades
preserve hue, and removed Checkbox's unintended wrapper-label color animation.

Validation used deterministic clocks and offscreen native scenes, including
painted wave spread/opacity, overlapping clicks, controlled-owner refusal,
custom colors/timing, cancellation, interruption, loaders and idle scheduling.
The focused animation test selection passed **57 tests**; strict library/test
Clippy, rustfmt on changed files and `git diff --check` passed. No complete crate
or repository suite was run. This audit covers component-owned motion; custom
animations supplied by application content or third-party demo wrappers are
outside this comparison. Native side-by-side video and macOS/Linux verification
were not performed.

```powershell
cargo test --locked --offline --lib -- wave transition loading loader radio_dot checkbox_tick thumb motion animat color_interpolation checkbox_disabled_label radio_disabled_label radio_button_border button_transparent_color
cargo clippy --locked --offline --lib --tests -- -D warnings
```

## Context Menu and Scroll Area — 2026-10-27

Both controls use retained GPUI controllers, native input dispatch and scoped
component themes. The Qt siblings supplied behavioral references. Context menus
share the popup host, use native variable-height virtualization for long menus,
and retain measured text widths. Scroll areas use one native ScrollHandle for
content, overlay thumbs, keyboard input, anchors and application updates.
See [the integration guide](docs/context-menu-scroll-area.md) for APIs and limits.

Only affected tests were executed:

- **120 library tests passed**: 21 context-menu, 22 scroll-area, 72 shared-popup
  and five locale tests.
- **Four gallery tests passed**: both pages, responsive cards, navigation anchors,
  appearance changes, nested scrolling, menu activation and keyboard dismissal.
- Strict Clippy passed for the library/tests, gallery, native capture example and
  focused benchmark. Targeted rustfmt and `git diff --check` passed.

Regression coverage includes actual painted highlights and virtualized Home/End
jumps, lazy branch freshness/cancellation, stale painted item callbacks, same-cycle
imperative commands, callback-selected focus, Tab dismissal, controller replacement,
offscreen submenu anchor retirement, viewport containment, native control identity,
generic context-region semantics, automation IDs and localized names. Scroll tests
exercise real line/pixel wheel dispatch, Shift and nested boundary routing, repeated
input before redraw, width fitting, thumb capture, paging, RTL, tiny tracks,
programmatic position changes and animation/unmount lifetime guards.

The native GPU capture tool generated **32 Windows frames at 150% DPI**, covering
light/dark/compact/dark-compact, both directions, root/submenu/edge placements and
1,000-item End navigation. Representative frames were visually inspected,
including the fully visible final row after viewport containment. Images are in
`../build/ant-design-gpui/context-scroll-native/`; the reproducible tool is
`examples/context_scroll_visual.rs`.

Final Release CPU/layout measurements used the configured
`windows-msvc-performance` tree with benchmarks enabled:

| Operation | Mean | p95 |
| --- | ---: | ---: |
| 1,000-item menu, retained scene | 0.076 ms | 0.085 ms |
| 1,000-item menu, redraw | 0.433 ms | 0.462 ms |
| 1,000-item menu, navigation | 0.531 ms | 0.602 ms |
| 1,000-item menu, close/reopen | 0.598 ms | 0.760 ms |
| 1,000-row native scroll baseline | 5.976 ms | 8.409 ms |
| 1,000-row themed scroll area | 6.359 ms | 9.468 ms |

Menu results use warmed caches; first layout still shapes label widths. Tall-content
results deliberately measure unvirtualized content. Native and themed scroll runs
each rerendered their translated content 205 times, including warmup. Retained
parent-scene assertions passed, and settled scenes scheduled **zero frames**.
These are host-specific CPU measurements, not GPU presentation or input latency.
The complete report is under
`../build/windows-msvc-performance/ant-design-gpui/context-scroll-results.txt`.

No full crate or repository suite was run. No dependencies were added. The test
platform cannot initialize a live accessibility tree; semantic nodes are probed
directly, and painted behavior is tested separately. Live Windows UI Automation,
screen-reader sessions, macOS and Linux still require native verification.

## Menu — 2026-10-28

Menu uses an immutable indexed model, a retained GPUI controller and renderer,
native focus and accessibility actions, shared popup ownership, and native lazy
rows. All 11 public Ant Design Menu examples are included in the gallery. See
the [integration guide](docs/menu.md) and
[verification ledger](fixtures/menu/verification.json) for the feature-to-test
map, exact run counts, source hashes and reproducible commands.

The visual comparison executes Ant Design 6.6.7 at revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e` independently of Rust. It covers 55
modern component tokens and 640 computed menus, then compares actual browser and
native Windows captures at 100%, 150% and 200% DPI. Surface, foreground-color,
root-layout and chevron checks have explicit budgets. Unmasked pixel metrics and
paired crops retain text and shadow differences for visual review.

Focused library checks cover Menu, shared popups, Context Menu and locale
regressions. Gallery tests, strict Clippy, scoped rustfmt and native Windows UI
Automation complement them. No full crate or repository test suite was run.

The Release benchmark uses the configured `windows-msvc-performance` tree with
benchmarks enabled. It checks bounded construction for 1,000-item roots and
popups, cached sibling work and zero settled animation frames. Results measure
CPU/layout work; they do not establish GPU presentation time or input latency.
The guide explains the pinned GPUI backend's ancestor/focus redraw behavior and
the bounded custom-content width sampling used by virtualized popups.

Popup motion uses native fade/reveal instead of the reference's subtree scale;
the pinned GPUI revision is preserved. Text antialiasing and shadow rasterization
can differ between backends. Interactive screen-reader sessions and native
macOS/Linux rendering and accessibility were not verified.
