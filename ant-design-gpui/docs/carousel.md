# Carousel

Native GPUI implementation of the documented Ant Design 6.6.7 Carousel surface,
using reference commit `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
The component has no browser or JavaScript runtime dependency.

## Ownership and integration

Create and retain one controller per mounted Carousel. Keep the immutable slide
snapshot and its stable keys when changing metadata or order:

```rust,ignore
use ant_design_gpui::{Carousel, CarouselItems, CarouselSlide, CarouselState};

let items = CarouselItems::new([
    CarouselSlide::new("overview").text("Overview"),
    CarouselSlide::new("details").view(|window, cx| {
        cx.new(|cx| DetailsView::new(window, cx))
    }),
])?;
let state = cx.new(|cx| CarouselState::new(items, window, cx));

// In the owner's Render implementation:
Carousel::new("project-carousel", &state)
    .scope(scope.clone())
    .arrows(true)
    .w_full()
    .h(px(160.))
```

Duplicate keys return an error. Labels can be duplicated or localized. Clone
existing slides while reordering to preserve their factory and entity identity;
replacing a factory deliberately replaces its retained view. `set_items` preserves
the active key, or clamps its previous position when that key was removed.
`current_index()` and `current_key()` expose the current logical selection.

`view` observes the typed entity's notifications to invalidate its intrinsic size.
`any_view` accepts type-erased content. For changes outside an observed entity,
call `invalidate_slide_measurement(key, cx)` or `invalidate_measurements(cx)`.
`content` creates a retained view around an element-rendering closure; application
state belongs in captured entities.

## Features and behavior

The render builder supports scroll/fade effects, logical top/bottom/start/end dots,
explicit vertical orientation, arrows and custom arrow/dot content, infinite or
finite navigation, initial selection, desktop dragging, adaptive height, transition
duration and cubic-bezier easing, autoplay with dot progress, and semantic styles.
Start/end placement implies vertical movement unless explicitly overridden.

Defaults match the reference: dots on, arrows off, infinite scrolling, initial
slide zero, dragging/autoplay off, 500 ms transitions, and a 3000 ms autoplay
interval. Transition easing is CSS `ease`, which is the reference's actual visual
curve. The reference's separately documented `easing="linear"` does not control
its CSS transition.

`go_to(index, dont_animate, window, cx)`, `next`, and `prev` provide imperative
navigation. Indices always name the original application order, including RTL.
`BeforeChange` and `AfterChange` events expose navigation source, indices, and stable keys;
callbacks run outside mutable state borrows. No-op requests emit no events.
Interrupted transitions start at their current visual position and do not emit
completion for an abandoned target. `wait_for_animate(true)` ignores requests
while moving. Immediate navigation completes without a timer delay.

Arrow keys navigate when a Carousel control owns focus; vertical movement supports
Up/Down. Enter/Space activate focused arrows and dots. Native interactive slide
content retains its own keyboard handling. Hidden slide content cannot receive
input or appear in the accessibility tree. Leaving a focused slide recovers focus
to the Carousel. Labels resolve through the English, Simplified Chinese, and
Traditional Chinese catalogs, including scoped per-field overrides.

Autoplay and dot progress share a monotonic clock. Hover, contained keyboard focus,
explicit pause, invisibility, and inherited disabling pause the remaining interval.
Manual navigation resets it. Reduced motion preserves timed rotation, switches
slides immediately, and suppresses animated dot progress. Finite autoplay stops at
the end. Empty and single-slide groups schedule no rotation or animation work.
`pause` suspends configured autoplay; `play` (or `resume`) resumes it. Enable
`autoplay(true)` on the builder to configure automatic rotation.

Explicit height constraints avoid measuring offscreen slides. Intrinsic horizontal
layout caches the maximum slide height; vertical layout uses the first slide;
adaptive layout uses the active slide. Measurement invalidation is separate from
painting: hidden measurement does not install input or accessibility nodes. Visible
layers use native clipping and offsets, while slide entities remain retained.
When native accessibility is active, visible slide content renders without GPUI's
explicit subtree cache: this pinned GPUI cache does not replay descendant semantic
nodes or action listeners. Ordinary rendering retains its cache.

Advanced inherited Slick modes (multiple visible slides, grouped scrolling, grids,
center/variable-width modes, responsive settings, linked carousels) are outside
this implementation. Native key preservation, consistent RTL indices, synchronized
paused progress, keyboard-focus pausing, and accepted-change event semantics are
deliberate corrections to browser implementation quirks.

## Gallery and verification

Run `cargo run --locked --offline --example gallery -- --component carousel`.
The page includes the six public reference demos and component-token example,
with the gallery's theme, density, direction, motion, and narrow-layout controls.

Focused verification:

```powershell
cargo test --locked --offline --lib carousel:: -- --test-threads=1
cargo test --locked --offline --example gallery carousel_gallery -- --test-threads=1
cargo test --locked --offline --lib locale::tests::carousel
cargo test --locked --offline --lib config::tests::carousel
./scripts/test-carousel-accessibility.ps1
./scripts/run-carousel-perf.ps1
```

The benchmark uses the validated `windows-msvc-performance` Release configuration.
CPU layout/scene timings do not measure GPU presentation latency. Windows native
validation does not establish macOS/Linux behavior. See the
[verification report](carousel-verification.md) and
[fixture provenance](../fixtures/carousel/README.md) for measurements and discrepancies.
