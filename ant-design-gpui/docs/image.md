# Image

Native GPUI Image and PreviewGroup follow Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The GPUI dependency stays at
`2c0bf70b54c018f99419f386d1cf1105dca57661`.

## Ownership and sources

Create an `Entity<ImageState>` once in the owning view and rebuild `Image` with
a stable ID. Mount the application under `UiRoot`, which installs the existing
window-local Modal host used by previews.

```rust,ignore
let image = cx.new(|cx| {
    let mut state = ImageState::new(source, window, cx);
    state.set_scope(scope.clone(), cx);
    state.set_alt("Mountain landscape", cx);
    state
});

// In render:
Image::new("landscape", &image, cx).w(px(200.))
```

Sources use GPUI's `ImageSource`: URLs, data URLs, filesystem paths (`PathBuf`),
embedded assets, encoded `Arc<gpui::Image>`, decoded `Arc<RenderImage>`, and custom
loaders. String paths identify embedded assets; pass filesystem paths as paths.
HTTP requests use the application's configured GPUI HTTP client. The component
does not replace that client or the application's asset source.

`set_source`, `set_preview_source`, `set_fallback`, and `set_alt` update retained
state. Thumbnail and preview sources are independent. Load errors and completion
emit `ImageEvent`; fallback is attempted once and reports its own error if it
fails. A stale request cannot replace a newer source. GIF and WebP frame timing
is preserved; hidden/inactive windows and reduced motion suspend animation.
When no valid fallback exists, thumbnails use a native picture icon and alt
text, and previews display the localized load error. This differs from the
browser's built-in broken-image presentation; that error artwork is not covered
by the paired screenshot matrix.

## Collections and controlled previews

Create one `PreviewState` for a collection and mount one `PreviewGroup` around
its content. Link each thumbnail with `.preview_group(&preview)`.

```rust,ignore
PreviewGroup::new("photos", &preview)
    .child(Image::new("first", &first, cx).preview_group(&preview))
    .child(Image::new("second", &second, cx).preview_group(&preview))
```

Creating `PreviewState` with an empty item list uses registered thumbnails in
registration order. Calling `set_items`, including with an empty list, switches
to an explicit collection; `use_registered_items` restores automatic registration.
Explicit `PreviewItem` collections can include images without visible thumbnails.
A registered thumbnail selects its matching explicit source; an unmatched trigger
opens the collection's first image. Preview-disabled and unmounted thumbnails
are removed from registration. Empty explicit collections close the preview.

Controllers are window-local. `set_open` and `set_current` are silent owner
updates. User actions emit `PreviewEvent::OpenChange`, `Change`, and `Transform`.
With `controlled_open` or `controlled_current`, the owner accepts a request by
updating the corresponding option. Indices are zero-based; the displayed count
is one-based. Groups display the count even for one item; standalone images hide
it. `show_count` overrides this default. Navigation stops at both endpoints and
resets transforms.

Enter/Space opens a focused thumbnail, Escape closes the top preview, and arrow
keys navigate groups. Focus is trapped by default and restored to the trigger.
Mask closing is configurable. Previews share Modal's nested ownership, scroll
lock, popup ordering, and background accessibility handling.
Wheel zoom preserves the pointer anchor. Native pinch gestures and touch-source
pan events use the same bounded transform controller; physical touch hardware
has not been verified.

## Loading, appearance, and customization

`ImagePlaceholder::Content` displays custom native content while loading;
`Default` displays the neutral loading surface. A source-less image can display
a placeholder by itself. `ImagePlaceholder::Progress` is an explicit standalone
display mode, independent of source download progress. It supports animated ink,
a finite rounded/clamped percentage, and a renderer receiving the default rail
and percentage. The default percentage label is omitted when a custom renderer
is supplied. Percentage updates ease the rail width using the scoped medium
motion duration; interrupted updates continue from the current width, and
reduced motion applies the target immediately.

The default hover/focus cover is empty and translucent, matching the current
reference. `.cover` and `.cover_placement` provide custom content and top, bottom,
or center placement. `.preview(false)` disables thumbnail preview activation.
`ImageStyles` refines native semantic slots; `ImageStyleResolver` supplies dynamic
styles. `ConfigPatch.image` provides inherited defaults and `ImageTheme` exposes
the reference component tokens. Built-in copy uses the typed locale catalog.

`actions_render`, `image_render`, and `count_render` compose native elements or
retained views. Preview rendering receives the current transform, collection
position, image metadata, and controller for typed actions. Retain callbacks
when their identity should remain stable across renders. Custom content can
include application-supplied media views; no video backend is bundled.
Configure standalone preview options on `Image::preview_options`; the image
builder reapplies its configuration on each render. The retained preview
controller handles opening, navigation, and transforms.

## Rendering and limits

Decoded sources share a bounded idle cache (128 entries / 64 MiB). Prepared
textures and portable oriented rasters use bounded caches (32 entries / 64 MiB).
Active controllers and cached scenes retain independent resource leases, so
their working set can exceed the idle budget without invalidating scene replay.

Windows previews use the existing GPU extension to upload each prepared frame
once, then apply pan/zoom/rotation/flips through affine composition. Pixel
conversion and raster preparation run off the UI thread. Device changes discard
incompatible textures. Oversized sources are downsampled within the adapter's
side/upload limits and the preparation budget while retaining their original
natural dimensions for layout; extreme zoom can reveal this reduced raster
resolution. Portable preparation is bounded to 4096 pixels
per side and 64 MiB. Other backends use cached quarter-turn orientations;
intermediate rotation animation angles are quantized there. Native validation
currently targets Windows x64; Windows ARM64, macOS, and Linux have not been
exercised. The pinned layout engine snaps coordinates to device pixels, so SVG
strokes can retain a subpixel phase and antialiasing difference from browser
rendering, especially at 150% scale. The native SVG atlas also applies the
catalog's cropped frame after rasterization, which can change edge coverage.
Verification preserves the raw pixel errors
and checks icon shape, color, position, and presence separately.

The pinned renderer does not support in-window backdrop blur. The accepted
`blur` option uses a dimmed-mask fallback. This applies to optional blur effects;
watercolor placeholder masks are prerasterized and animated natively.

Open the gallery with `cargo run --locked --offline --example gallery -- --component image`.
See [verification evidence and reproduction](image-verification.md) for focused
tests, visual comparisons, performance measurements, and remaining differences.
