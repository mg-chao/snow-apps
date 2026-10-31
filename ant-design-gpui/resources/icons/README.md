# Embedded Ant Design icons

The 829 source templates were copied without modification from
`ant_design_qt/packages/ant_design_icons_qt/resources/templates/`. That package synchronized the
upstream Ant Design Icons revision recorded in `upstream.lock.json`. Artwork retains its upstream
MIT license; see `../../licenses/ant-design-icons-MIT.txt`.

`templates/` is the vendored source. `generated/` preserves SVG element order while converting
secondary color slots into the renderer's template marker. `catalog.json` records the generated
identifiers and SHA-256 hashes. The generated Rust table embeds these bytes directly.

From the crate directory, regenerate or verify with:

```powershell
python scripts/generate_icons.py
python scripts/generate_icons.py --check
```

Generation requires Python and the crate's pinned `rustfmt`. Ordinary Cargo builds do not run the
generator, access the network, or read icon files at runtime. Updates must vendor a complete pinned
upstream set, update its lock and attribution, regenerate, and run the icon tests.

Custom project SVGs belong to a separate namespace and remain project owned. A static
`IconDescriptor` or typed `IconRef::asset` uses the same renderer. Monochrome assets use
`currentColor`; two-tone assets additionally use `__ADGPUI_SECONDARY__`; full-color assets preserve
their original colors and reject tint overrides. The host owns validation and updates of its custom
asset source. Clear the icon cache after replacing an asset at the same path.

Monochrome painting uses GPUI's alpha-mask atlas with borrowed SVG bytes. Loading and rotation of
`LoadingOutlined` therefore reuse the same geometry each frame. Color-preserving rendering caches
parsed SVGs and exact display-size rasters. Static rotation and explicit horizontal mirroring are
supported; continuous animation should use monochrome icons to avoid color-raster churn.

CPU caches default to 256 parsed documents and 16 MiB/512 rasters, with a 1 MiB per-raster insertion
limit. Parsed entries are count bounded because GPUI does not expose their internal allocation
size. Raster bytes count actual image storage.

Painted images additionally have scene leases stored in GPUI element state. Cached view replay
retains those leases; LRU eviction therefore cannot invalidate an image still present in a cached
scene. A weak resident index reuses view-owned images when the visible working set exceeds the LRU
budget, without repeated rasterization or forced redraws. Scene-owned images are outside the LRU
budget; `resident_rasters` and `resident_bytes` report all unique live raster resources. Releasing
the last scene/cache owner disposes its GPUI atlas entry. Expired weak entries are reclaimed on
release, misses, and trimming.

App trimming reclaims idle cache ownership while preserving live scenes. Explicit cache clearing
also refreshes windows once, allowing cached scenes to replace changed host assets before their
old image leases are released. Caller-held export images and GPUI's independent monochrome atlas
remain outside these statistics.
