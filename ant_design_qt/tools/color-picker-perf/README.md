# Color picker performance benchmark

This release-mode benchmark measures the real Windows widget path for `AdColorPicker`.
It does not use Qt's offscreen platform because native `QtTool` creation is part of the
measurement.

The first-show benchmark creates a fresh picker for every sample, lets its idle prewarm
complete, presses the trigger, and measures from trigger release through the first event-loop
paint. Output separates synchronous release handling, event-loop painting, and an untimed
diagnostic render. Both `InWindow` and `QtTool` modes are covered.

The drag benchmark sends real press/move/release events to the saturation panel, hue slider,
and alpha slider. Every measured move flushes pending layouts and paints. It also checks that
each gesture emitted color value changes, preventing missing or stale widgets from producing
false high-FPS results. Each drag starts from the same opaque color so one sweep cannot leave a
later control at an unmeasurable endpoint.

Defaults are 40 first-show samples per popup mode and 240 measured frames per drag surface.
For focused experiments, set `ADQT_PERF_FIRST_SHOW_ITERATIONS` or `ADQT_PERF_DRAG_FRAMES`.
Set `ADQT_PERF_MIN_DRAG_FPS` to a machine-specific positive integer to enable a failing FPS
regression gate; the default `0` records timings without applying a platform-dependent limit.
Set `ADQT_PERF_SCREENSHOT_DIR` to capture the first measured frame of each popup mode.

Build the benchmark through the supported top-level CMake project, then run the resulting
benchmark target from its build directory:

```powershell
cmake --build ..\..\build\windows-msvc-performance --config Release --target adqt-color-picker-perf
```

The benchmark is not available as a standalone project.
