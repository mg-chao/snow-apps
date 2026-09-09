# Shared toolbar controls

Scope: the screenshot drawing toolbar, pinned-image editing toolbar, and recording toolbar.
All three consume `ScreenshotToolPalette`, `ScreenshotToolbarMainPanel`, and the palette's
style editor components. Window placement, recording state, and canvas command ownership
remain with their existing consumers.

## Behavior contract

- Preserve preset colors, including transparent and translucent values, and exactly one
  edit command per click. Rebinding a retained editor must route commands to its new consumer.
- Preserve signal-blocked inbound state updates, preview versus commit behavior, mixed
  selection indicators, tooltip translation, and accessible names.
- Preserve hover-open color pickers, popup placement, canvas sampling availability, and
  the independent DPI scaling of toolbar controls and popup contents.
- Preserve each preview's content: color swatch, stroke pattern, fill pattern, or stroke width.
- Consolidate picker-trigger chrome, preset construction/state/metrics, and select setup.
  Content and semantic options are configuration of shared controls.
- Standardize secondary-row shadows on the main row's fractional DPI calculation. Previously
  secondary rows rounded the blur radius and offset to integers. The shared surface retains
  the same base colors, radius, blur, and offset and tests equality at 50%, 100%, 125%, 150%,
  and 200% scales.

## Implementation review

| Control | Shared implementation |
| --- | --- |
| Swatch, stroke, fill, and width picker triggers | `ColorPickerTrigger`, selected by `Preview` |
| Color preset rows in drawing editors and recording settings | `ScreenshotToolPaletteColorPresets` |
| Picker setup and optional canvas sampling | `createScreenshotToolPaletteColorPicker` |
| Font, filter, recording format, formatting, and punctuation selects | `createScreenshotToolPaletteSelectEditor` |
| Button metrics and active-state styling | `configureToolbarButton` and `setScreenshotToolPaletteButtonActive` |
| Main and secondary panel backgrounds, shadows, and separators | `ScreenshotToolbarPanel` |
| Sliders and radio groups | Existing shared slider and radio editor factories |

Editor components retain their own canvas commands and preview/commit semantics. The preset
group owns the color-to-button mapping and presentation updates; Qt parents own widget lifetimes.
The common picker trigger delegates content painting to the existing swatch, stroke, fill, and
width renderers. Separate stroke and fill trigger classes and factories were removed, with no
temporary adapters. The pinned image's circular edit/close overlay already shares
`PinnedControlButton`; it is distinct from the drawing toolbar's rounded controls.

## Focused verification

`snow-shot-screenshot-tool-palette-tests` supports these related cases (set
`QT_QPA_PLATFORM=offscreen` for direct execution):

- `--color-control-styles-only`: rendering, alpha, editor contracts, preset rebinding,
  mixed state, and toolbar/popup DPI separation.
- `--stroke-editor-only`: stroke color dragging, width previews, and shared catalogs.
- `--recording-controls-only`: export settings, color presets, and recording state changes.
- `--style-reconcile-only`: retained editor lifetimes, rebinding, and mixed-state updates.
- `--ocr-translation-only`: formatting and punctuation selects and recognition actions.
- `--pinned-actions-only`: pinned editing action availability.
- `--dynamic-i18n-only`: translation of existing toolbar controls.
- `--canvas-color-sampling-only`: drawing picker sampling requests and commits.

The registered `snow-shot-toolbar-shared-controls-tests` runs the color-control case.
`snow-shot-toolbar-main-panel-tests` additionally checks common panel rendering, button and
select sizing, themes, shadows, late materialization, and DPI behavior.

The preset rebinding characterization is established against the original implementation
before the shared controls replace it. The six baseline groups (color controls, stroke editors,
recording controls, reconciliation, OCR, and pinned actions) passed. This refactor does not change
persisted settings, protocols, dependencies, or translation sources. Native hover and multi-monitor interaction
require separate interactive verification; the deterministic tests cover their control state
and sizing contracts.

Final validation on `windows-msvc-debug`:

- Built `snow_shot`, `snow-shot-screenshot-tool-palette-tests`, and
  `snow-shot-toolbar-main-panel-tests` successfully.
- All eight direct focused cases listed above passed after the component consolidation.
- After panel and active-state consolidation, the following filtered CTest run passed 4/4:

```powershell
ctest --preset test-windows-msvc-debug -R '^snow-shot-(toolbar-(shared-controls|main-panel)|screen-recording-toolbar|screenshot-toolbar-i18n)-tests$' --output-on-failure
```

- Changed C++ files pass `clang-format --dry-run --Werror --style=file`; `git diff --check`
  passes. The configured Debug preset has clang-tidy disabled.
- No full test suite or performance suite was run.
