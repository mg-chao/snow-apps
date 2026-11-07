# Message upstream verification

`reference.json` records **272 independent browser measurements** of Ant Design
6.6.7 at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`: ten scenes in both
directions across default, dark, compact, and dark compact themes. The shared
`visual-cases.json` covers all five types, a plain notice, custom/hidden icons,
rich and empty content, collapsed whitespace, long text, mixed Hebrew/Arabic/English,
root and semantic refinements, scoped component tokens, ordinary lists,
custom top offsets, narrow windows, collapsed/hover-expanded stacks, and
maximum-count eviction.

The generator mounts the **actual upstream `message/useMessage.tsx` and
`PurePanel.tsx`**, using the published **`@rc-component/notification` 2.1.1**.
It executes the original Message and shared Notification style generators with
the genuine cssinjs serializer. Small adapters supply scoped ConfigProvider
values and capture style registration. Global tokens come from the independent
[theme fixtures](../theme/README.md). Unitless line-height custom properties are
serialized as strings to preserve normal registration semantics; passing a raw
number to cssinjs would append `px`. No native source or reconstructed Message
markup supplies expected values.

The fixture also contains the following direct observations:

- Original enter/leave CSS transitions sampled at 0/50/100/150/200 ms. Framework
  phase classes are applied to a genuine settled upstream card; WAAPI pauses the
  original transitions and sets exact times. The easing is
  `cubic-bezier(0.645, 0.045, 0.355, 1)` with 64 px upward translation.
- Original runtime actions produce additional WAAPI traces: close reflows cards
  over 200 ms, while list height shrinks over 600 ms after a 200 ms delay;
  stack placeholder opacity/translation take 100 ms, and width takes 300 ms.
  These samples set each created transition's own relative time rather than
  assuming synchronous React frame delivery.
- Interrupted actual entry is sampled with a MutationObserver before short reverse
  transitions can finish. Open-and-close before a visual frame commits no notice.
  Closing a 50 ms entry freezes opacity 0.0872301, then reverses over 17.446027 ms
  (200 ms times eased progress), with opacity decreasing to zero without a flash.
- Original React hooks running with virtual host animation-frame timers and
  `Date.now`: hover preserves remaining duration; same-duration keyed updates
  preserve the original deadline; changed content is rendered in place.
- The upstream manual close filters notice state without calling its close
  callback, leaving its completion promise unresolved. The native component
  deliberately settles completion on all close paths and owner release; it
  retains the reference's hover and same-duration update timing.
- Upstream 2.1.1 measures notice sizes only when attaching refs. A same-key width
  update changes the card but leaves placeholder widths stale. That observation
  and a genuine new-key width transition are both recorded. Native continuous
  measurement deliberately keeps placeholders in sync with keyed content.
- All six semantic slots are measured. Per-open `root`, `wrapper`, `icon`, and
  `title` refinements affect a notice. Per-open `list`/`listContent` are ignored
  upstream; holder-level styles apply them. The runtime trace records both cases.

`visual-cases.json` is shared by independent browser/native renderers. Ten
scenes, four themes, both directions, and **100%, 150%, and 200% physical DPI**
produce **240 paired images and 816 notice rows** (including collapsed hidden
notices). Every viewport is 740x1000 logical pixels except `narrow`, 320x1000.
Body background is `colorBgContainer`, with zero margin/padding. Static captures
settle transitions and pause actual loading-icon rotation at zero.

The comparison retains complete unaligned, unmasked physical images, plus union
crops covering both card positions and their shadows. It never resizes captures,
shifts origins, filters text, or masks icons. Hidden stack rows retain their
identity/visibility in metrics, while pixel comparison includes the full image.
Root geometry is compared by stable notice key. Browser subpart typography,
colors, padding, shadows, gap and radius additionally drive offline native tests.
Stack reveal is a native fade/vertical-motion adaptation: older cards keep their
natural size, while the CSS reference scales them from 94%/88% to 100%. The pinned
GPUI public API cannot affine-transform an arbitrary element subtree. The settled
matrix and timing/opacity tests do not claim intermediate stack-width or animated
pixel equality. Inherited native opacity also blends individual primitives rather
than a CSS isolated group; overlapping graphics can differ during fades.

The final [Windows comparison](visual-validation-windows.json) retains all 240
image pairs and 816 notice rows. Maximum logical-pixel errors are 0.9375 in
width, 0.46875 in horizontal position, 0.33334 in vertical position and 0.25 in
height, with no cumulative row-spacing drift. The height residual is the compact
empty icon's browser inline line box; native font metrics and device rounding
account for the other small bounds differences. Complete-image mean absolute
channel error is at most 2.095575 on a 0-255 scale; text and icons remain unmasked.
The typed hidden-icon case uses the original public `styles.icon.display: none`;
native `show_icon(false)` provides that behavior directly. Empty/null/false icon
values upstream fall back to the type icon and are not used as a fake hide API.
Long unbroken text keeps its min-content width and overflows toward the logical
end, while the card remains constrained by the reference viewport edge margin.
For example, in a default 320 px window the card is 272 px wide and 40 px high;
the 657.53125 px unbroken title extends beyond it. The fixture preserves this
behavior instead of adding emergency character wrapping.

Generated HTML, bundled dependency code, raw DOM, PNGs, isolated browser profiles
and diagnostics stay beneath ignored `../build/ant-design-gpui/`. Package source
and license hashes, pins, upstream revision, theme-fixture hash, generator/capture
hashes and browser version are retained in JSON. Original developer-package
licenses are in [licenses/](licenses/), and Ant Design's original MIT notice is
in [the theme licenses](../theme/licenses/ant-design.txt). These packages add no
native runtime dependencies.
The icons-svg and is-mobile archives omit license files; their already retained
canonical repository notices are copied and hashed as explicit fallbacks.

Regenerate using the existing pinned pnpm store and previously extracted official
util package. The generator downloads and installs nothing; supply the extracted
official `@rc-component/notification` 2.1.1 package separately. For example:

```powershell
$vendor = '../build/ant-design-gpui/message-reference/vendor'
New-Item -ItemType Directory -Force "$vendor/notification" | Out-Null
Invoke-WebRequest 'https://registry.npmjs.org/@rc-component/notification/-/notification-2.1.1.tgz' -OutFile "$vendor/notification-2.1.1.tgz"
tar -xzf "$vendor/notification-2.1.1.tgz" -C "$vendor/notification" --strip-components 1
node scripts/generate_message_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --notification-package "$vendor/notification" --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_message_reference.mjs --output ../build/ant-design-gpui/message-browser
cargo run --locked --offline --example message_visual -- --output ../build/ant-design-gpui/message-native
python scripts/compare_message_visual.py --browser ../build/ant-design-gpui/message-browser --native ../build/ant-design-gpui/message-native --output ../build/ant-design-gpui/message-comparison.json --summary fixtures/message/visual-validation-windows.json
cargo test --locked --offline --lib message::reference_tests
```

The provisioning command retains the official package archive only in the ignored vendor
folder. Ordinary Cargo tests read checked-in JSON and require neither Node nor
Chrome. The visual capture process owns an isolated headless browser and does
not attach to a user's browser or signed-in session.
