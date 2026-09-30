# Progress

`adqt::widgets::AdProgress` is a passive Qt Widgets indicator for line, circle, and dashboard
progress. Include `widgets/progress.h` or the `widgets/widgets.h` umbrella and link `adqt::widgets`.
Its rendering is implemented with `QPainter`; no embedded browser or per-segment child widgets are
used. Colors, typography, scoped theme overrides, and motion follow `ThemeManager`.

```cpp
auto* progress = new adqt::widgets::AdProgress(parent);
progress->setPercent(42.5);
progress->setStatus(adqt::widgets::AdProgress::Status::Active);

auto* dashboard = new adqt::widgets::AdProgress(parent);
dashboard->setType(adqt::widgets::AdProgress::Type::Dashboard);
dashboard->setSuccessPercent(25);
dashboard->setPercent(65);
dashboard->setGapPlacement(adqt::widgets::AdProgress::GapPlacement::Bottom);
```

## Values and status

- Percentages are finite values clamped to `[0, 100]`; nonfinite inputs are ignored. A negative
  `successPercent` clears the separate success segment. Success can extend beyond total progress.
- The default `Status::Automatic` becomes success at 100%. When a success segment is configured,
  its percentage determines automatic completion. Explicit Normal, Active, Success, and Exception
  override automatic status. Active adds a shimmer to nonempty, incomplete continuous lines.
- `setFormat()` accepts a callback receiving the target percent and success percent (zero when
  unset). A formatter replaces the default percentage/status icon. It is evaluated when content
  changes, never by painting or size queries. Translate application-provided formatter text.
- `showInfo` controls the visible label/icon. Accessibility continues to expose the target numeric
  value through a read-only `QAccessibleValueInterface`, independently of visible formatting.
- Changing a visible continuous value transitions its geometry for 300 ms. `displayedPercent` is
  the painted value; `percent` is the target. Disable motion with `setAnimationEnabled(false)`, a
  scoped theme override, or the global theme. Hidden indicators snap to the target and unsubscribe
  from the shared frame scheduler.

## Size and layout

All sizes use Qt logical pixels except circular `strokeWidth`, which is a percentage of diameter.
Natural line heights are 8 pixels (Small: 6), and circle diameters are 120 pixels (Small: 60).
Middle and Large share Ant Design's normal metrics. `progressSize` supplies the preferred size,
not a fixed QWidget constraint; use layout alignment or `setFixedSize` when an exact size is needed.

For continuous lines, width includes outer information and height specifies the rail. For stepped
lines, width specifies **one step**, with natural units of 14 × 8 (Small: 2 × 8). Explicit height
outranks `strokeWidth`. Nonpositive dimensions and zero stroke width select natural metrics.
Circles use width as the preferred diameter, default height to the same value, and preserve a square
canvas when given a rectangular widget. Circular strokes have a minimum natural width of 3 pixels.
At an actual diameter of 20 pixels or less, center information becomes a tooltip. An application
tooltip is preserved.

`percentAlignment` and `percentPlacement` support start/end outer labels, a centered label below
the rail, and labels inside the filled track. Inner tracks reserve enough width for their text even
at zero. Stepped lines always use outer end information. Logical start/end, line fill, default
gradient direction, and dashboard gap placement follow Qt's layout direction.

## Colors, gradients, and segments

`strokeColor`, `railColor`, and `successColor` override their theme defaults. Invalid colors reset
these overrides. Explicit stroke colors and gradients take precedence over status fill colors.
Component tokens customize theme defaults; semantic styles customize root, body, rail, track, and
indicator slots. Circle text/icon font tokens use logical pixel sizes.

```cpp
progress->setStrokeGradient({{0.0, QColor("#108ee9")}, {1.0, QColor("#87d068")}});
progress->setGradientReversed(true);
progress->setSteps(10);
progress->setStepColors({QColor("#108ee9"), QColor("#87d068")});
```

Gradient stop positions are fractions in `[0, 1]`. Stops are normalized and sorted, nonfinite or
invalid stops are ignored, and the last duplicate position wins. An empty list clears the gradient.
Line gradients span the filled track; circular gradients span the available sweep and use butt caps.
Line steps use solid/per-step colors. Circle steps use butt caps and quantize to the nearest completed
step. `setStepRounding()` customizes line rounding only. Steps are bounded to 10,000; gap defaults to
2 logical pixels for lines and 2 percent of diameter for circles. Circular steps show total completion
without a separate success overlay. Dashboard gap degrees default to 75, clamp to `[0, 295]`, and
respect explicit zero; a negative value restores automatic defaults.

## Verification and examples

The theme-demo navigation contains 16 interactive Progress sections. Focused checks:

```powershell
cmake --build --preset build-windows-msvc-debug --target adqt-progress-tests adqt-progress-demo-tests theme-demo
ctest --preset test-windows-msvc-debug -R '^adqt-progress(-demo)?-tests$'
build/windows-msvc-debug/ant_design_qt/Debug/adqt-progress-tests.exe -platform offscreen --render-dir build/progress-qa
cmake --build --preset build-windows-msvc-performance --target adqt-progress-benchmark
build/windows-msvc-performance/ant_design_qt/Release/adqt-progress-benchmark.exe -platform offscreen
```

The tests can export light/dark contact sheets at 1× and 2×. The Release-only benchmark measures
construction and steady/update rendering across 128 indicators for each visual form and emits CSV.
Static circular rasters are cached at the effective paint scale, with one entry capped at 4 MiB per
indicator. Theme, content, and geometry changes invalidate the cache; hidden widgets release it.
Transitions and nonuniform or rotated exports paint directly.
