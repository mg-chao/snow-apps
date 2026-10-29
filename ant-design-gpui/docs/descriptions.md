# Descriptions

Descriptions presents application-owned label/value pairs using native GPUI
layout, text, accessibility, and controls. The reference is Ant Design 6.6.7,
revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The component has no browser
or JavaScript runtime dependency; browser tooling only generates independent
development fixtures.

## Composition and identity

Build the component during render with stable component and item IDs:

```rust
use ant_design_gpui::{DescriptionItem, Descriptions, DescriptionsSpan};

let details = Descriptions::new("account-details")
    .title("Account")
    .columns(3)
    .bordered(true)
    .item(DescriptionItem::new("name").label("Name").content("Lin"))
    .item(DescriptionItem::new("status").label("Status").content("Active"))
    .item(DescriptionItem::new("balance").label("Balance").content("0"))
    .item(DescriptionItem::new("address")
        .label("Address")
        .content("Hangzhou, Zhejiang")
        .span(DescriptionsSpan::Filled));
```

Repeated `.item` and `.items` calls append items. IDs identify data, rather than
row or column positions: keep them stable when reordering or resizing, and do
not use translated labels as IDs. Items are keyed independently of responsive
row placement. Application entities and keyed state inside rich content retain
their native focus, edit selection, and interaction when an item moves.

The defaults are horizontal layout, no border, visible colons, responsive
columns, and large spacing. `.layout(DescriptionsLayout::Vertical)` places each
row's labels above its values; `.bordered(true)` gives labels separate table
cells. `.colon(false)` hides punctuation in borderless layouts. Colons are
decorative and excluded from the accessible text.

`.title` and `.extra` set header text; `.title_content` and `.extra_content`
accept native elements. Each setter replaces the preceding value for that
slot. The title takes the remaining header space and truncates at its visual
inline end; the extra slot keeps its own width and interactions. Header content
and all other copy belong to the application's localization system.

## Responsive columns and spans

`.columns(3)` fixes the number of logical item columns. A
`DescriptionsBreakpoints<usize>` selects responsive columns:

```rust
use ant_design_gpui::{Descriptions, DescriptionsBreakpoints};

let details = Descriptions::new("responsive-details")
    .columns(DescriptionsBreakpoints {
        xs: Some(1),
        sm: Some(2),
        lg: Some(3),
        ..Default::default()
    });
```

Breakpoints use the theme's window-width aliases, rather than the component's
containing width. Native logical window width is rounded to an integral CSS
pixel before resolution, avoiding fractional-DPI readback gaps around values
such as 575/576. The default column map is:

| Breakpoint | Default window range | Columns |
| --- | --- | --- |
| `xs` | At most 575 | 1 |
| `sm` | At least 576 | 2 |
| `md` | At least 768 | 3 |
| `lg` | At least 992 | 3 |
| `xl` | At least 1200 | 3 |
| `xxl` | At least 1600 | 3 |
| `xxxl` | At least 1920 | 4 |

The largest active supplied value wins. `sm` and larger values continue through
higher breakpoints; `xs` applies only to its maximum-width range. A responsive
column map without an active supplied value falls back to the default map.
Theme overrides to the alias breakpoints are respected.

An item's `.span(2)` consumes two logical columns. `.span(breakpoints)` accepts
the same responsive map, with a fallback of one column.
`DescriptionsSpan::Filled` consumes the rest of its row. Spans that exceed the
remaining space are clipped to that space, and the final item expands to fill
an incomplete final row. Zero columns or spans normalize to one. Bordered
horizontal layout internally has separate label/value tracks; the public
column and span counts always describe complete items.

## Text, rich content, and accessibility

`.label` and `.content` accept text. `.label_content` and `.content_element`
accept arbitrary native elements. The element setters also recognize direct
string elements, so those follow the same text policy. Nested native views and
controls retain their own layout, semantics, and input behavior.

An empty string omits that slot. An absent label also removes its colon in
borderless layout, while retaining the item. Whitespace-only text remains a
present slot; normal CSS whitespace collapse can leave it with no visible
glyphs. The string `"0"` is visible text. Numeric application values should be
formatted explicitly before passing them to text setters. An arbitrary rich
element is considered present even when it later renders nothing.

Primitive text collapses ASCII whitespace, preserves logical source text for
accessibility, and uses Unicode line-breaking and native font shaping. Labels
wrap at ordinary opportunities. Values use the reference's emergency grapheme
wrapping, including its min-content behavior. For deliberate paragraph breaks
or custom text policy, supply a native element.

The root exposes `DescriptionList`; item groups expose `Term` and `Definition`.
Plain labels name both groups, which disambiguates vertical layout's visual
row order. Rich labels should supply `.accessibility_label("Account status")`:

```rust
use ant_design_gpui::DescriptionItem;
use gpui::{div, prelude::*};

let status = DescriptionItem::new("status")
    .label_content(div().child("Status"))
    .accessibility_label("Account status")
    .content("Active");
```

Values are never flattened into group names: native Input, Button, and other
descendants retain their own accessible identities and actions. The list itself
introduces no tab stop. A plain title supplies the default root name; set
`.accessibility_label`, `.accessibility_description`, `.automation_id`, or
`.accessibility_role` explicitly when appropriate. A rich title needs an
explicit list name; rich title contents do not supply the automatic root name.

## Configuration and styling

`.scope(scope)` selects a `ConfigScope`; otherwise the application scope is
used. An explicit instance `.size` wins over an explicitly configured inherited
size. If neither supplies a size, Descriptions uses `Size::Large`; the generic
configuration's default medium value does not override that component default.

`DescriptionsStyles` exposes `root`, `header`, `title`, `extra`, `label`, and
`content`, each a native `StyleRefinement`. Resolution order is scoped styles,
scoped resolver, instance styles, instance resolver, then ordinary `Styled`
refinements on the root. Resolvers receive effective layout, border and colon
flags, columns, size, direction, item count, and header-slot presence.

`DescriptionItemStyles` refines `label` and `content` after the corresponding
component slots. An item's ordinary `Styled` refinements target its enclosing
item cell; bordered layout applies them to both physical label/value cells.
In borderless layouts those enclosing refinements precede the semantic inner
label/content refinements. Explicit width refinements participate in intrinsic
track sizing.

`ConfigPatch.descriptions` supplies inherited styles and an optional
`DescriptionsStyleResolver`. Retain or clone that resolver wrapper across scope
updates: equality uses the retained callback identity, allowing no-op updates
to preserve shared snapshots. Changing scope replaces the component's previous
subscription, and unmounting releases it.

`DescriptionsTheme` uses the existing component-theme registry with
`DescriptionsTokenPatch`. Its tokens are `label_bg`, `label_color`,
`title_color`, `title_margin_bottom`, `item_padding_bottom`, `item_padding_end`,
`colon_margin_right`, `colon_margin_left`, `content_color`, and `extra_color`.
Global tokens supply font, title typography, border, radius, and size-specific
cell padding. Medium and small sizes retain the reference's specific padding
rules; the component's `item_padding_bottom` override controls large borderless
spacing. Theme changes invalidate the relevant native text/style caches.

## Native layout architecture

Descriptions is a render-once builder. Pure functions resolve breakpoints,
plan rows, and solve intrinsic track constraints. Native children are requested
once, measured at minimum/preferred sizes, and reused in the final flex rows.
Spanning cells share those constraints. The native tree therefore determines
row heights, final hitboxes, focus, and rich-child behavior in one frame.

Plain text publishes cached raw intrinsic advances for track sizing. Borderless
horizontal pairs keep the complete native preferred width of their inline
subtree: advertising a smaller raw width while its descendants still consume
rounded device-pixel widths would wrap a value even at max-content. Small
differences from browser fractional glyph boxes remain possible. Final finite
text wrapping uses the original fractional layout constraint; edge snapping
does not add fitting slack or trigger a different prepaint line break.

Collapsed borders share cumulatively rounded half-edges so fractional DPI does
not accumulate row or column errors. Solid borders use native quads. Dashed and
dotted borders use cached native paths, with interior strokes deferred until
cell content has painted and the outer outline painted beneath its children.
RTL keeps the same logical item order while reversing physical placement.

Static lists schedule no animation work. The component retains its scope
observer and bounded text/path caches, including width variants, intrinsic
metrics, and per-cell border geometry. Custom descendants control their own
retained entities and animation lifetime. Focused verification and
platform-specific evidence are recorded separately in the
[Descriptions verification report](descriptions-verification.md) and
`fixtures/descriptions/`.
