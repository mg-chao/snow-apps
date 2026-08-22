# ant_design_icons_qt 2.0

`ant_design_icons_qt` provides immutable, typed Qt icon references and generated icon packs. The
built-in `antd` pack contains only SVGs from the revision pinned in
`resources/upstream.lock.json`. Project assets, application icons, branding, and cursors belong to
external project-owned packs and cannot enter the `antd` namespace.

Normal builds use checked-in generated C++ and do not require Python, resource initialization, or
runtime file access. Applications never download icons.

## Public API

An icon is identified by `IconKey { pack, variant, name }`. `IconRef` is an immutable value created
from an immutable generated pack. It contains a descriptor pointer and compact inline color data;
it never owns a generated key, SVG, or entry-table string. Inspect metadata with
`describeIcon(ref)` (owning compatibility values) or `describeIconView(ref)` (non-owning hot-path
view), and derive a differently colored reference with `ref.withColors(colors)`.

Use the rendering facade for every static icon:

- `makeIcon(ref, statePalette)` for Qt controls and window/application icons.
- `renderIconPixmap(ref, request, statePalette)` for labels and cached custom rendering.
- `paintIcon(painter, ref, rect, request, statePalette)` for direct painting.
- `makeCursor(ref, logicalSize, hotSpot, devicePixelRatio)` for full-color cursor assets.

`IconRenderRequest` carries logical size, DPR, `QIcon::Mode`, `QIcon::State`, optional fit override,
and alignment. `IconFit::Contain` is the default and preserves the SVG view box aspect ratio;
`IconFit::Stretch` must be declared or requested explicitly.

Generated packs embed a `constexpr IconDescriptor[]` table in read-only program storage. A generated
factory such as `antd::outlined::Search()` performs no per-entry registration, `QList`, `QString`, or
`QByteArray` entry-table construction, and no runtime SVG parsing until that reference is rendered.
The first call to a pack's `pack()` function records one pack pointer in the process-wide metadata
catalog; `registerWith(renderer)` is an optional per-renderer registration operation for isolated
tests and tooling.

```cpp
#include "antd_icons.h"
#include "icon_registry.h"

button->setIcon(adqt::icons::makeIcon(
    adqt::icons::antd::outlined::Search()));

adqt::icons::IconRenderRequest request;
request.logicalSize = QSize(24, 24);
request.devicePixelRatio = widget->devicePixelRatioF();
label->setPixmap(adqt::icons::renderIconPixmap(
    adqt::icons::antd::outlined::Warning(
        adqt::icons::IconColors::primary(QColor("#D4380D"))),
    request));
```

## Color Models

Pack entries declare one color model:

- `monochrome`: primary slot; the application text color is the default. Fixed accent colors may
  remain in a hybrid asset.
- `twoTone`: primary and secondary slots.
- `threeTone`: primary, secondary, and tertiary slots.
- `fullColor`: source colors are preserved; slot colors and state palettes are rejected.

In source SVG, `currentColor` is the primary slot. Mark additional colored elements with
`data-adqt-slot="secondary"` or `data-adqt-slot="tertiary"`. Generation replaces these markers with
internal placeholders after validating that the declared model and actual slots match.

```cpp
const auto colors = adqt::icons::IconColors::twoTone(
    QColor("#1677FF"), QColor("#E6F4FF"));
button->setIcon(adqt::icons::makeIcon(
    adqt::icons::antd::twotone::Alert(colors)));
```

A hybrid logo may declare `monochrome`, mark only its theme-controlled text with `currentColor`,
and retain a fixed brand color on its mark. A full-color application icon declares `fullColor` and
uses no theme slots.

## State Palettes

`IconStatePalette` can define colors for all eight combinations of `Normal`, `Active`, `Selected`,
and `Disabled` with `Off` and `On`. Resolution order is:

1. Exact mode and state.
2. Same mode with `Off`.
3. `Normal` with the requested state.
4. `Normal` with `Off`.
5. Pack defaults and the application `IconPalette` resolver.

```cpp
adqt::icons::IconStatePalette palette;
palette
    .set(QIcon::Normal, QIcon::Off,
         adqt::icons::IconColors::primary(normalColor))
    .set(QIcon::Active, QIcon::Off,
         adqt::icons::IconColors::primary(hoverColor))
    .set(QIcon::Selected, QIcon::On,
         adqt::icons::IconColors::primary(pressedCheckedColor))
    .set(QIcon::Disabled, QIcon::Off,
         adqt::icons::IconColors::primary(disabledColor));
button->setIcon(adqt::icons::makeIcon(ref, palette));
```

Install an application palette resolver once and increment `revision` whenever its resolved colors
change. The renderer resolves colors before lookup, so the cache key separates descriptor identity,
physical dimensions, mode/state, fit, alignment, and the resolved primary/secondary/tertiary RGBA
values. Palette changes also advance a render generation and reclaim old entries, which prevents an
in-flight render from repopulating a cleared cache.

```cpp
adqt::icons::setPaletteResolver([] {
  adqt::icons::IconPalette palette;
  palette.text = QColor("#1F1F1F");
  palette.textDisabled = QColor("#BFBFBF");
  palette.primary = QColor("#1677FF");
  palette.twoToneSecondary = QColor("#E6F4FF");
  palette.tertiary = QColor("#BAE0FF");
  palette.revision = currentThemeRevision();
  return palette;
});
```

## Cache and Reclamation

The renderer owns a byte-bounded LRU of premultiplied `QImage` rasters. Its production defaults are:

- 2 MiB total cache bytes;
- at most 512 entries; and
- at most 256 KiB for one raster.

An entry's cost is `QImage::sizeInBytes()`, including row stride, rather than a pixel-area estimate.
Rasters larger than either limit are returned to the caller but are never inserted. Caller-held
`QImage`, `QPixmap`, and `QIcon` data is deliberately outside cache-owned statistics, so
`cacheStatistics().costBytes` reaches zero immediately after eviction even when a caller still holds
the returned image.

Use `cacheStatistics()` for exact entry/byte/hit/miss/eviction counters. `trimCacheToBytes(target)`
(`trimToBytes`/`reclaimCache` are equivalent aliases) and `trimIconCache(target)` reclaim whole
least-recently-used entries and return an `IconCacheReclaimReport` with before/after bytes, entry
counts, reclaimed bytes, and the new generation. `clearCache()` is the hard reset. Snow Shot trims
the shared cache to 512 KiB when the application is hidden or capture resources become idle.

The renderer coalesces concurrent requests for the same cache key. `clearCache()`, trim operations,
cache-limit changes, and palette changes advance the generation; a render that began in an older
generation can still complete for its caller, but it cannot repopulate the new cache.

## External Pack Manifest

Each owning project keeps its source SVGs and a JSON manifest. Schema version 1 is:

```json
{
  "schemaVersion": 1,
  "pack": "my-project",
  "cppNamespace": "my_project::icons",
  "headerInclude": "icons/my_icons.h",
  "exportMacro": "MY_PROJECT_EXPORT",
  "source": "My Project static assets",
  "entries": [
    {
      "variant": "outlined",
      "symbol": "OpenPanel",
      "name": "open-panel",
      "source": "icons/open-panel.svg",
      "colorModel": "monochrome",
      "fit": "contain",
      "defaultColors": { "primary": "#1F1F1F" }
    }
  ]
}
```

`headerInclude`, `exportMacro`, `source`, `fit`, and `defaultColors` are optional. Variants and names
are canonical lower-case identifiers; exported symbols and C++ namespace components must be valid
C++ identifiers. Embedded `data:` images are permitted only for a `fullColor` entry that explicitly
sets `allowEmbeddedDataImages` to `true`. Network and non-data image references are forbidden.

Generate and verify checked-in output:

```bash
python tools/generate_icon_pack.py path/to/icons.manifest.json \
  --header path/to/generated_icons.h \
  --source path/to/generated_icons.cpp

python tools/generate_icon_pack.py path/to/icons.manifest.json \
  --header path/to/generated_icons.h \
  --source path/to/generated_icons.cpp \
  --check
```

`ExternalIconPackDefinition`, `ExternalIconPack(ExternalIconPackDefinition)`, and
`IconRenderer::registerPack()` remain available only for explicitly dynamic project/test packs.
They normalize and validate the full pack before committing. Identical repeated registration is
idempotent; conflicting keys, SVG content, or hashes return structured diagnostics without partial
registration. Production generated packs use the static descriptor constructor and do not pay this
runtime table-copy cost.

## Asset Ownership

- `ant_design_icons_qt` owns only the downloaded, pinned upstream Ant set.
- A widget package owns assets used only by that widget package.
- Each application owns its application icon, brand assets, and application-specific glyphs.
- A reusable engine owns its cursors and engine-specific toolbar glyphs.
- Static SVGs are generator inputs, never runtime qrc/file paths.
- Painter-based rendering remains appropriate for live data previews, canvas content, swatches,
  control chrome, and fill/stroke demonstrations that are not static icons.

Syncing upstream and generating an external pack are separate operations. Upstream synchronization
uses the pinned commit by default and rejects local overlays:

```bash
python tools/sync_ant_design_icons.py
python tools/build_antd_manifest.py --check
python tools/check_repository_icon_usage.py
```

The repository check scans tracked and untracked source files while honoring Git ignores, so stale
build output is excluded. It rejects removed 1.x APIs, runtime SVG resource/file loading, SVG qrc
entries, widget-local `QSvgRenderer` use, and the retired procedural static-glyph paths.

## Migrating from 1.x

Version 2.0 is intentionally source breaking. There are no compatibility adapters.

- Replace `IconTheme` identity with the generated variant namespace or `IconKey::variant`.
- Replace `IconRenderModel` with `IconColorModel`.
- Replace mutable `IconRef::id` inspection with `describeIcon(ref).key`.
- Replace mutable `IconRef::colors` with `ref.withColors(IconColors::...)`.
- Replace `IconColorOverrides` and `hasPrimary` flags with optional-slot `IconColors` factories.
- Replace size/DPR render overloads with `IconRenderRequest`.
- Replace manual `QIcon::addPixmap` state assembly with `IconStatePalette`.
- Replace `registerIcon`, `makeIconRef`, qrc callbacks, and handwritten entry tables with a generated
  static `ExternalIconPack`/`IconPack` factory. Keep `registerPack()` only for explicitly dynamic
  test or plugin definitions.
- Move local SVG overlays out of the `antd` pack and into the project that owns them.
- Replace direct `QIcon(":/...svg")`, `QFile` SVG loading, and widget-local `QSvgRenderer` paths with
  the rendering facade.
