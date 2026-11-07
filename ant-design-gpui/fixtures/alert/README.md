# Alert upstream and native verification

`reference.json` contains **2,304 independent browser measurements** of Ant Design
6.6.7 at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`: 96 examples in both
directions, four themes (default, dark, compact, dark compact), and rest, close
hover, and close keyboard focus styles. Examples cover all four statuses, both
variants, title/description/description-only content, wrapping and unbroken
words, default/custom/hidden icons, banners, actions, disabled and text closes,
rich elements, component tokens, and semantic first-line alignment.

The generator executes the **actual `Alert.tsx` component** through ReactDOMServer,
the original CSSMotion and Ant Design icon components, original style generators,
and the original cssinjs serializer. Adapters supply ConfigProvider values and
capture style registration; React hooks and Alert markup are unchanged. Global
tokens come from the independent [theme fixtures](../theme/README.md). No Rust
implementation values or manually reconstructed Alert DOM enter the fixtures.
The JSON records source/package hashes, package versions, upstream revision,
generator hash, theme fixture hash, and browser version. Ordinary Cargo tests
consume only checked-in JSON and need no browser, Node, npm cache, or reference
checkout.

`lifecycle.json`, `lifecycle-title.json`, and `lifecycle-styled.json` trace the
**mounted upstream React component** and CSSMotion in a fresh hidden browser,
using real elapsed time. Each includes the open bounds and computed typography,
CSSOM motion/description rules, inline styles, intermediate frames, and callback
order. The browser plugin was unavailable on the validation machine; the scripts
use an isolated local Chrome debugging connection, without existing profiles.

The traces preserve two source details that are easy to miss from CSS intent:
leave starts from integer `offsetHeight`; description padding stays fixed because
its later equal-specificity selector wins over the leave-active padding rule.
Explicit inline padding also stays fixed. Title-only padding collapses. Opacity,
max-height, and bottom margin follow the same 300ms circular easing, while the
border/padding minimum can keep layout height above zero until unmount.

`visual-cases.json` is shared by the genuine browser and native GPU harnesses.
The basic scene has 12 rows and the advanced scene has 13. Four themes, both
directions, and **actual native 100%, 150%, and 200% DPI** produce 48 rest images;
eight additional first-close hover/focus images produce **56 image pairs and
696 compared Alert rows**. The hidden Windows harness sends DPI changes only to
its own uniquely titled window, verifies the resulting platform scale, and asserts
exact physical image dimensions. Screenshots are never resized for comparison.
The 740×1000 logical viewport fits within the validation display at 200%.

Regenerate with the existing pnpm cache and a pre-extracted official
`@rc-component/util` **1.14.0** package (the upstream version, newer than the
validation machine's cached 1.9.0). The generator itself downloads and installs
nothing:

```powershell
node scripts/generate_alert_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_alert_lifecycle.mjs
node scripts/capture_alert_lifecycle.mjs --mode title --output fixtures/alert/lifecycle-title.json
node scripts/capture_alert_lifecycle.mjs --mode styled --output fixtures/alert/lifecycle-styled.json
pwsh -NoProfile -File scripts/capture_alert_reference.ps1
cargo run --example alert_visual -- --output ../build/ant-design-gpui/alert-native
python scripts/compare_alert_visual.py --browser ../build/ant-design-gpui/alert-browser --native ../build/ant-design-gpui/alert-native --output ../build/ant-design-gpui/alert-comparison.json --summary fixtures/alert/visual-validation-windows.json
cargo test --lib alert::reference_tests
```

Pillow is used only by the developer comparison script. Generated pages, client
bundle, screenshots, raw DOM, isolated profiles, and diagnostics remain under the
ignored `build/ant-design-gpui/` directory. The report records dimension mismatches,
signed geometry deltas, mean pixel error, and changed-pixel fractions. GPU and
browser text antialiasing may differ; evaluate pixel differences alongside the
geometry and direct visual inspection.

The committed `visual-validation-windows.json` records actual platform scales,
physical dimensions, native/reference PNG hashes, source hashes, full unaligned
and unmasked canvas error, and bounds differences. Its pixel measurements use
no image resizing, font masking, or origin alignment. An additional diagnostic
in the ignored full report aligns individual measured control crops; that
diagnostic is excluded from the committed pixel measurements.

Native text retains logical accessible content while composing bidi glyph runs
after native line wrapping. The paired RTL scenes verify punctuation order as
well as icon/close placement. Native text uses the same device-pixel line-height
policy in both directions. Explicit clamp/ellipsis truncates logical display
content before shaping, avoiding the framework wrapped-line cache's omission of
the line limit from its key. Four bounded width variants per mounted text element
retain repaired glyph layouts across warm draws; content, style, direction, font,
scale, and width changes invalidate the appropriate cached layout.

Original MIT notices for the fixture-only React, ReactDOM, CSSMotion, util, icons,
and cssinjs inputs are retained in [licenses/](licenses/). The original Ant Design
notice is retained in [the theme licenses](../theme/licenses/ant-design.txt).
