# Date picker performance benchmark

This release-mode benchmark exercises the real widget implementation for `AdDatePicker`,
`AdDateRangePicker`, and `AdDatePickerPanel`. It covers lifecycle cost, fresh first popup opening,
steady-state opening, hidden value updates, calendar navigation and painting, time-enabled panels,
multiple selection, disabled-date predicates, and both popup layer modes.

From the repository root, configure and build with the Release performance preset:

```powershell
./snow_shot/scripts/configure-msvc-perf.ps1
cmake --build --preset build-windows-msvc-performance --target adqt-date-picker-perf
$env:QT_QPA_PLATFORM='offscreen'
./build/windows-msvc-performance/ant_design_qt/Release/adqt-date-picker-perf.exe
```

The configuration helper selects Qt 6.12.0, MSVC 14.51, and a matching runtime. A source-built
Release Qt kit needs `-DevelopmentModules` in `scripts/build-static-qt.ps1` so the performance
preset has Qt Test and Concurrent available.

Run the executable several times when comparing revisions. `process_first_single_popup_in_window`
is deliberately the first component operation after QApplication startup, so it includes one-time
Qt/theme initialization. The `fresh_*` distributions reuse the process but create a new component
for every sample. Reported distribution fields are minimum, median, mean, p95, and maximum
milliseconds. Run the executable in a new process for each revision and machine-level cold-start
comparison.
