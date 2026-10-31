# Pagination

`Pagination` uses a retained GPUI controller, native layout, native text editing,
and the library's existing Select popup. Its appearance and behavior follow Ant
Design 6.6.7, reference revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`,
with `@rc-component/pagination` 1.4.0. Browser classes, React component types and
DOM handlers are represented by typed native builders and render callbacks.

Create one controller per mounted pager and reuse it between renders:

```rust
use ant_design_gpui::{Pagination, PaginationState};
use ant_design_gpui::pagination::QuickJumper;

// During view construction:
let pagination = cx.new(|cx| PaginationState::new(window, cx));

// During rendering, with a stable locally unique ID:
Pagination::new("results-pages", &pagination)
    .scope(scope.clone())
    .total(500)
    .default_current(6)
    .quick_jumper(QuickJumper::Blur)
    .on_change(cx.listener(|view, change, window, cx| {
        // Load change.current using change.page_size.
    }))
```

## Values and events

Pages are one-based. Defaults are page 1, page size 10 and total 0. Numeric counts
use `u64`; page arithmetic cannot overflow, and the visible page sequence remains
bounded even for `u64::MAX` records. A zero page size normalizes to one. Empty data
shows disabled previous/next controls and a disabled page 1; simple mode shows
`1 / 0`. Total renderers receive an inclusive range, or `[0, 0]` when empty.

Omit `.current(...)` and `.page_size(...)` for retained internal values. Their
respective `.default_*` builders initialize each value once. Set either property
to control it independently: interactions emit a proposed value, and the accepted
value changes when the owner supplies an update. State setters are silent and
do not emit user events. Owner-provided builder properties take precedence.

Subscribe to `PaginationEvent` on the controller or use `.on_event`, `.on_change`
and `.on_show_size_change`. A page-size action first emits `ShowSizeChange` with
the previous current page and requested size, then `Change` with the resulting
page and size. Changing size preserves the page number when possible and clamps
it otherwise. Event sources identify page, previous/next, ellipsis, jump editor,
simple editor, size selection and accessibility requests.

When simple mode also has a separate quick jumper, its confirmation button
commits the adjacent quick-jumper draft, just like Enter. This corrects the pinned
reference's Go-button wiring, which instead targets the simple editor's draft.

## Presentation and composition

- `.size(Size::{Small, Medium, Large})` follows the provider unless explicit.
  `.align(PaginationAlign::{Start, Center, End})` uses logical RTL alignment.
- `.show_less_items(true)` uses a smaller page window and three-page ellipsis
  jumps; normal ellipsis jumps move five pages. `.show_prev_next_jumpers(false)`
  suppresses jump controls. `.hide_on_single_page(true)` hides the complete pager.
- `.show_size_changer(bool)` overrides the inherited preference. Otherwise the
  changer appears when total exceeds 50, or the configured boundary. The default
  choices are 10, 20, 50 and 100; a missing current size is inserted and sorted.
- `.quick_jumper(QuickJumper::Blur)` commits on Enter or ordinary blur. Blur into
  a page control clears the draft without a competing page change.
  `QuickJumper::Confirm` commits through Enter or its confirmation button.
- `.mode(PaginationMode::Simple)` uses a centered native editor and total page
  count. Up selects the preceding page and Down the following page, matching the
  reference. `SimpleReadOnly` replaces the editor with text. Native IME
  composition never commits a page prematurely.

Custom total content uses `.show_total`; `.show_total_text` measures formatted
text with the inherited native font, applies paragraph direction, and preserves
wrapping in narrow layouts. Application-owned total content controls its own text
layout.
`.item_render` receives the typed item,
its original content and native rendering context; return `None` to suppress it.
The outer native control retains activation, focus and semantics. Icon renderers
replace the previous/next or ellipsis content. `.styles` and `.styles_fn` refine
the reference's root/item slots, and ordinary GPUI `Styled` methods refine the root.

`.size_changer_select` configures the built-in Select. `.size_changer_render`
replaces it with application-owned content. Its `PaginationSizeChangerRender`
argument carries the value, size, availability, scope and a weak `change_size`
request connection. Custom confirmation content uses `.go_button`.

`PaginationTheme`, `PaginationTokens` and `PaginationTokenPatch` expose all
reference component tokens through the existing theme system. English,
Simplified Chinese and Traditional Chinese catalogs and per-field locale patches
include visible labels, titles and accessible names. Theme, direction, locale,
size, availability and motion changes propagate through retained ConfigScopes.

## Reference details

The pinned v1.4 page-window algorithm reduces the middle neighborhood when both
ellipsis controls appear: page 50 of 100 shows `1 … 49 50 51 … 100`.

The current reference emits an obsolete `mini` class for automatic responsive
sizing, while its stylesheet targets `small`. Pagination preserves this measured
appearance. Explicit small size works normally. Independently of `.responsive`,
the reference hides the options at window widths through 576 logical pixels and
hides the marked page neighbors beside ellipses through 992 pixels. Native
layout follows these exact boundaries.

## Verification

The gallery supports `--component pagination` and includes all 13 public reference
examples with retained state. Source-derived behavior and visual fixtures are in
`fixtures/pagination`. Generated captures and measurement reports belong under the
ignored repository build directory. See [verification evidence and limits](pagination-verification.md)
for the state matrix, native accessibility checks, performance measurements and
documented reference details.

Run only the related checks:

```powershell
cargo test --locked --offline --lib -- pagination:: locale::tests config::tests::pagination behavior::tests::semantic_adapter --test-threads=1
cargo test --locked --offline --example gallery pagination_gallery -- --test-threads=1
scripts/test-pagination-accessibility.ps1
scripts/run-pagination-perf.ps1
```

The accessibility script uses a hidden fixture by default and records whether
the Windows backend exposes its native tree. `-Interactive` explicitly enables a
visible fixture and guarded keyboard checks. The benchmark script validates the
Release performance preset; it does not run unrelated benchmarks. CPU timing
and allocations do not measure physical display or input latency.
