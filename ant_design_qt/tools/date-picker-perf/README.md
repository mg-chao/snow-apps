# Date picker performance benchmark

This release-mode benchmark exercises the real widget implementation for `AdDatePicker`,
`AdDateRangePicker`, and `AdDatePickerPanel`. It covers lifecycle cost, fresh first popup opening,
steady-state opening, hidden value updates, calendar navigation and painting, time-enabled panels,
multiple selection, disabled-date predicates, and both popup layer modes.

Configure and build with CMake:

```powershell
cmake -S . -B .codex-build/date-picker-perf -G "Visual Studio 18 2026" -T "host=x64,version=14.50" -A x64 `
  -DCMAKE_PREFIX_PATH=$env:QTDIR `
  -DADQT_BUILD_BENCHMARKS=ON -DADQT_STRICT_COMPILE=OFF
cmake --build .codex-build/date-picker-perf --config Release `
  --target adqt-date-picker-perf
$env:QT_QPA_PLATFORM='offscreen'
.\.codex-build\date-picker-perf\Release\adqt-date-picker-perf.exe
```

Use a generator and Qt installation built with the same compiler. The paths above match a standard
Qt 6.11 MSVC installation and can be adjusted for the local toolchain.

Run the executable several times when comparing revisions. `process_first_single_popup_in_window`
is deliberately the first component operation after QApplication startup, so it includes one-time
Qt/theme initialization. The `fresh_*` distributions reuse the process but create a new component
for every sample. Reported distribution fields are minimum, median, mean, p95, and maximum
milliseconds. Run the executable in a new process for each revision and machine-level cold-start
comparison.
