# Date/time picker reference fixtures

The reference executes Ant Design **6.6.7**, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with
`@rc-component/picker` **1.12.2**. It bundles the actual source in
`E:/ant-design/components`; it does not reproduce the native component.
The loaded original MIT licenses and hashed package inventory are preserved in
`licenses/` and `reference-packages.json`. JavaScript is developer-only.

`visual-cases.json` is shared by browser and native tools. Its 56 scenes span
four themes, two directions and three scales, for **1,344 expected pairs**.
Every scene has one trigger on a 1000×760 logical canvas with 32px padding.
The fixed clock is **2024-02-15 13:24:35.120 Asia/Shanghai**; neither browser
capture timing nor the desktop clock may influence today or time values.

## Scene and native metric contract

The manifest has `schema_version`, `clock` (ISO-8601), `timezone`, `canvas`,
`themes`, `directions`, `scales`, and a `scenes` object keyed by stable ID.
Scene values specify `component: date|time`, `picker` (date/week/month/quarter/year),
`range`, `multiple`, `value` (ISO string, array or null), `open` and explicit
reference props. Omitted props use Ant Design defaults. Range values may contain
a null end. `hoverDate` supplies an exact `YYYY-MM-DD` cell title; `focusInput`
is a zero-based input index; `hoverTrigger` requests the clear affordance.

Both capture files have `{schema_version:1,cases:[...]}`. Cases use the exact
name `${scene}-${theme}-${direction}-${scale*100}` and include `scene`, `theme`,
`direction`, numeric `scale`, `canvas` and `geometry`. Native output is named
`native-metrics.json`; the browser emits `browser-metrics.json`. PNG filenames
are `${name}.png` at actual device scale. A native scene that cannot render must
emit `unsupported` with a reason, which fails comparison and remains pending
in the coverage matrix.

Geometry uses logical coordinates. Rectangles have `x,y,width,height`:

- `trigger`, `popup` (panel container), `footer`, `presets`, `activeBar`: rectangle or null.
- `rangeArrow`: source padded arrow-element rectangle; interior paint above the
  popup is checked independently of the popup's own surface.
- `inputs`: rectangles with current `value`, in logical input order.
- `prefix`, `prefixText`: independently measured prefix slot/text-range records
  when present; native records expose the actual prefix slot.
- `panels`, `headers`, `tables`, `timeColumns`: rectangles in source panel order.
- `timeViewports`: actual scroll client rectangles, excluding separators/rails.
- `scrollbars`: `{column,track,side,scrollTop,scrollHeight,clientHeight}`; native
  records may additionally retain actual thumb/arrow bounds. `track` here means
  the complete occupied rail, including its end buttons.
- `footerActions`: actual OK button `{key,label,rect}` records.
- `tags`: visible `{key,label,rect,content,remove}` records; `remove` is the
  actual SVG silhouette box, not its larger pointer target. Source records
  include computed CSS color; native records add `paintRgbaCandidates` from
  transformed monochrome sprites centered inside that slot. Exactly one tint
  must match the source within one RGBA byte.
- `cells`: all visible calendar cells in chronological table order, including adjacent-month cells.
- `timeCells`: cells intersecting a time-column viewport, in column then value order.

Each calendar cell includes `key`, `title`, `text`, booleans `selected,today,
disabled,inView,inRange,rangeStart,rangeEnd,rangeHover`, plus `rect` for its table
cell and `inner` for its visible mark/text square. Time cells include `key`
(`${column}:${value}`), `column`, `value`, `text`, `selected`, `disabled`, `rect`
and `inner`. Browser cells additionally retain actual `::before` styles.
Week cells additionally expose `weekSelected`, `weekRangeStart`, `weekRangeEnd`,
`weekRangeHover` and `weekHover` from the actual selected/hovered table row.
Week-row primary/active fills are sampled independently of ordinary date-cell
selection; upstream week cells leave their ordinary `selected` flag false. A
selected week can also carry rc-picker's internal `hoverValue` alias with a
neutral pointer. The comparator checks its selected/range state and actual
continuous primary band; it does not require GPUI to duplicate that redundant
hover alias. An unselected hover remains checked independently. A
range hover can use normal range-start/in-range/range-end classes in upstream;
the comparator checks the actual resulting flags and pixels.

## Reproduction

```powershell
node scripts/prepare_date_time_picker_reference.mjs
node scripts/capture_date_time_picker_reference.mjs
node scripts/pack_date_time_picker_reference.mjs
python scripts/test_date_time_picker_visual_comparator.py
python scripts/compare_date_time_picker_visual.py --reference ../build/ant-design-gpui/date-time-picker-reference/browser-capture --native ../build/ant-design-gpui/date-time-picker-reference/native-capture --output ../build/ant-design-gpui/date-time-picker-reference/comparison.json
python scripts/summarize_date_time_picker_validation.py --reference ../build/ant-design-gpui/date-time-picker-reference/browser-capture/browser-metrics.json --native ../build/ant-design-gpui/date-time-picker-reference/native-capture/native-metrics.json --comparison ../build/ant-design-gpui/date-time-picker-reference/comparison.json
python scripts/pack_date_time_picker_validation.py --visual ../build/ant-design-gpui/date-time-picker-reference/comparison.json --native ../build/ant-design-gpui/date-time-picker-reference/native-capture --uia ../build/ant-design-gpui/date-time-picker-uia/result.json --performance ../build/windows-msvc-performance/ant-design-gpui
```

Capture accepts `--scene` (comma-separated IDs), `--theme`, `--direction` and
`--scale`. The comparison tool accepts the same scene/theme/direction/scale filters. Browser capture
uses a dedicated hidden Chrome process/profile and original component DOM,
with CSS motion disabled and time-column alignment settled before readback.
The fixture keeps actual default placement. In RTL the source dropdown may
stretch to the trigger's minimum width while its inner panel aligns at the
right; `dropdown` records its class/style separately from `geometry.popup`.
Source, dependency-lock, scene and image SHA-256 hashes are recorded. Focused
supplemental recaptures retain complete baseline and supplemental provenance
through `merge_date_time_picker_reference.mjs`; replaced metrics/PNGs remain
under the build capture's `replaced-<capture-id>` directory.

The comparator requires matching case presence, physical PNG dimensions,
logical geometry (≤1px), input values and every calendar/time-cell state.
The hidden HTML focus-helper input in multiple mode has no separate native editor
requirement: GPUI's retained chip group owns focus. Its raw DOM geometry and all
unmasked pixels remain in the evidence; visible nonempty input values remain
strict. The native multiple ComboBox exposes the joined value read-only in UIA.
Chromium scrollbars are never suppressed. Its Windows `scrollbar-width: thin`
rail occupies 10 logical pixels in the pinned browser; the older 8px WebKit
declaration is overridden by this modern rule. Browser client extents and
independently observed arrow/thumb ink are compared, including complete painted
silhouettes (1 logical pixel) and dominant ink colors (1 RGB byte). Extraction
uses the rail interior, excluding its outer 1 logical pixel where rounded
captures can include a neighboring separator/selected fill. Those boundary
pixels remain in every unmasked capture. At least 50%-covered source stroke
pixels must have paint within 1 logical pixel in the other renderer, and vice
versa. Distance is from a strong pixel center to the other image's occupied
pixel square, allowing half a physical pixel of sampling uncertainty per axis.
Both images must contain a strong core. Authored slot/rail bounds stay strict;
raw AA extent/width differences and complete antialias-fringe residuals are
reported as diagnostics.
Actual selected fills, range bands and today outlines are independently sampled
with a 1 RGB byte limit. Unmasked full-image text/shadow raster error is reported
separately; it cannot hide missing marks. The 69 deterministic comparator controls
cover geometry, wrong/missing selection, range bands/endpoints, today outlines,
missing cells, wrong scale, explicit unsupported scenes, missing week rows and
incorrectly separated selected squares instead of a continuous selected week,
missing/moved/wrong-color or hollow scrollbar thumbs, missing arrows, wrong
scroll extents/client clipping, wrong primary footer fills, visible tag geometry,
fill contrast and remove marks. Tag fill contrast uses the adjacent gap at the
same scanline to isolate the fill from overlapping popup shadows; absolute RGB
values and full shadow-region residuals remain reported. The independent arrow
interior samples detect a range arrow clipped above its popup; antialiased seams
are kept in the full unmasked raster diagnostics.
Remove glyphs also require the reference achromatic hue and actual native scene
RGBA tint within 1 byte. Their gray raster coverage brightness is reported
independently because tiny SVG antialiasing differs across renderers; correctly
shaped red or faded gray replacements fail. Fractional-phase positives and
paint-only displacement negatives verify the pixel-square distance method.
Custom borders use source computed CSS boxes/colors against the last matching
native painted Quad at each of twelve side points, including side widths and
the clip union of rounded-border fragments with independent draw orders.
Today outlines search
independently for the expected full-color stroke within one physical pixel of
the top-center probe, preserving the 1 byte color and 1 logical pixel box limits.
Popup flat-fill probes use the clear top band rather than a scrollbar edge.

Native capture can run one attested shard per theme via
`capture-date-time-picker-native.ps1 -Theme <name> -Output <directory>`.
`merge_date_time_picker_native.py --shard <directory> ... --output <directory>`
requires disjoint cases, identical source/binary/toolchain hashes, valid metrics
and image hashes, and zero foreground violations before preserving all shard
attestations in the merged output. Performance runs remain separate and quiet.
Each native case also publishes an immutable `<name>.metrics.json` only after
its complete PNG exists. These progress records support early diagnostics;
interrupted captures lack final provenance and cannot establish a passing run.

The final Windows evidence is checked in as `visual-validation-windows.json`,
`accessibility-windows.json` and `performance-windows.json`. All 1,344 visual
configurations pass, with zero failed checks; all 126 read-only UIA checks and
all 17 Release CPU/allocation paths pass. These files share one source-map
digest and preserve executable/report hashes, unmasked text/shadow statistics,
AA residuals and limitations. The performance harness uses offscreen
TestAppContext and a deterministic test text system; it does not measure
production font shaping, GPU presentation or physical input latency.

Supplemental source probes accept `--millisecond-step 1`, `--empty-multiple true`
and `--trigger-width 240`. They are explicitly marked with `probe_overrides` and
cannot be merged into the official baseline. `scrollbar-step1-reference.json`
preserves all 24 actual browser configurations for the 1,000-row case.

`coverage-matrix.json` records every scene separately. Reference capture,
native capture and strict visual passing are independent statuses. A complete
reference capture does not establish native parity.
