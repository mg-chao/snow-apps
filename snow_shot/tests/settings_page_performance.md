# Settings page performance

The settings page benchmark uses fresh page instances in a warm process. It does not cache
pages between samples. Each page gets one discarded warm-up followed by the requested number
of measured samples. Storage is isolated in a temporary directory. On Windows, the benchmark
loads Segoe UI and Microsoft YaHei explicitly so Qt's offscreen platform measures real text
instead of missing-font glyphs.

Metrics at an 880 × 760 viewport:

- `construction_ms`: constructing the page and its initial controls.
- `first_display_ms`: construction, showing the page, and draining four event-loop passes,
  including initial layout and painting. This is not application startup or GPU presentation
  latency.
- `all_sections_ms`: cumulative time to visit every section and explicitly reveal every custom
  editor, including editors below the fold.
- `slowest_section_ms`: the slowest section visit within each sample.
- `max_popup_open_ms`: the slowest first opening of a color popup in each sample, after visiting
  every section. Zero for pages without color pickers.
- `all_interactions_ms`: cumulative time including section visits and opening/closing every color
  popup. Final widget counts are taken here, so popup deferral cannot hide missing work.
- `retranslation_ms`: reapplying the current language after all controls have been created.
- Widget counts at first display and after complete traversal.
- The time and number of complete session refreshes for 25 backend notifications in one burst.

Median and p95 summaries are reported. Do not run the benchmark alongside builds or other
performance tests. Compare runs on the same machine, platform plugin, Qt version, viewport,
fonts, and sample count, with the same benchmark source on both revisions.

## Run

Use the Release performance preset, never the Debug or packaging presets:

```powershell
. ./scripts/snow-build-environment.ps1
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$buildJobs = (Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum
cmake --build --preset build-windows-msvc-performance `
    --target snow-shot-settings-page-performance-benchmark --parallel $buildJobs
$env:QT_QPA_PLATFORM = 'offscreen'
./build/windows-msvc-performance/snow_shot/Release/snow-shot-settings-page-performance-benchmark.exe `
    --samples 21 --output build/settings-perf-after.json
python scripts/compare-settings-performance.py build/settings-perf-before.json build/settings-perf-after.json
```

The comparison checks at least 25% faster median first display on pages whose baseline exceeds
40 ms, at least 25% improvement in aggregate first display, at least 20% improvement in aggregate
retranslation, and at least 80% improvement in notification-burst time. A burst must cause exactly
one refresh. Complete traversal with popups must still instantiate at least the original number of
widgets; aggregate traversal, both with and without popup opening, must not regress by more than
25%. The median slowest first color-popup opening must stay below 50 ms. Small pages are excluded from the
individual timing threshold because a few milliseconds of scheduling noise dominate them.

## Design and regression coverage

Settings sections retain lightweight, focusable shells. Their controls are created on viewport
entry, keyboard entry, or explicit navigation. Newly created rows are initialized inside a hidden
body and attached together. Toolbar editors are deferred independently, since an editor can be
below the fold within an otherwise visible section. Estimated heights are replaced with actual
layout heights while preserving the visible section's scroll offset or remaining at the bottom
when the user scrolled to the end. Created controls stay alive
until the page is destroyed; cross-page caching is unchanged.

The page performs one value initialization pass, updates style properties only when they change,
and avoids repolishing controls before their first normal polish. Unchanged select options retain
their models. Color pickers disable automatic popup prewarming; unopened popup editors are built
only when requested. Settings pages own their theme and language updates instead of receiving duplicate
updates from the content card.

Backend notifications are coalesced into one deferred full refresh. Keeping a full refresh after
each burst preserves cross-field enablement, dynamic options, conflict detection, and asynchronous
write completion semantics without maintaining a second dependency graph.

`snow-shot-settings-page-loading-tests` covers deferred navigation, scrolling, resize, keyboard
entry, current values and translations on materialization, control reuse, unchanged styling, and
selector-model reuse. `snow-shot-settings-runtime-session-tests` covers notification coalescing,
including a notification during a refresh, alongside existing write/reset/conflict checks. Existing
color, configuration-transfer, shortcut, theme, translation, custom-model, and export-dialog tests
cover the surrounding behaviors. Tests that inspect offscreen controls explicitly navigate to the
relevant section before inspecting them.

## Initial-load changes (2026-10-04)

This follow-up removes work from fresh page construction. It introduces no page cache or data
cache for later visits, and does not change the existing section-level deferral policy.

Form items keep their existing grid while refreshing feedback, labels, and styling. One theme
resolution supplies the layout, label, message, and feedback steps of each refresh; the resolved
style is local to that call. Settings pages avoid repeating the initialization already performed
by their field and custom-widget constructors. Model and translation configuration rows receive
theme updates in place instead of being destroyed and rebuilt.

History and pinned management take one filter snapshot per filtering pass and convert record
timestamps to local dates only when a date boundary is active. They also skip building a set of
every record ID when there is no selection to reconcile.

The added `snow-shot-main-page-performance-benchmark` covers About, Translation, and empty and
populated history/pinned pages. It uses the same fresh-instance, four-event-pass measurement as
the settings benchmark. Synthetic sources with 100 and 10,000 records exclude repository reads
and thumbnail completion; Translation uses a pending local catalog request. These timings measure
initial UI work, not complete image loading, network response time, or application startup.

```powershell
cmake --build --preset build-windows-msvc-performance `
    --target snow-shot-settings-page-performance-benchmark snow-shot-main-page-performance-benchmark `
    --parallel 12
$env:QT_QPA_PLATFORM = 'offscreen'
./build/windows-msvc-performance/snow_shot/Release/snow-shot-settings-page-performance-benchmark.exe `
    --samples 11 --output build/settings-initial-load.json
./build/windows-msvc-performance/snow_shot/Release/snow-shot-main-page-performance-benchmark.exe `
    --samples 11 --output build/main-pages-initial-load.json
```

The comparison script's notification-coalescing thresholds above belong to the September change;
that optimization is already present in this follow-up's baseline.

Validation passed 12 focused CTest cases in the Release performance build: shared form fields
and layout, settings loading/navigation/selectors, color and slider commits, skin settings,
custom models, translation configurations, pinned management, and history page loading. The
color test now follows each field's registered page and covers all nine current pickers.
Changed C++ formatting and `git diff --check` passed. The full suite was not run.

`snow-shot-screenshot-history-page-loading-tests` exercises the existing history rendering,
filtering, empty-state, preview, and cross-page selection checks together with local-date
boundary coverage. The original full history target remains intact. Its offscreen run failed
at the clipboard check: Windows publishes to the native clipboard, while that check reads
Qt's offscreen clipboard. A Windows-platform attempt stopped at the synthetic More-menu hover
check. These full-history checks are not claimed as passing.

Both benchmark executables built and ran successfully. Final wall-clock comparisons are
unverified: unrelated compiler and linker processes were continuously active during the final
runs and materially increased timings, including on unchanged pages. Those contended results
are not reported as optimization gains. Rerun the commands above on an idle machine before
using this revision's timings for a performance threshold.

## Measured results (2026-09-28)

Windows 11 (10.0.26200), AMD Ryzen 9 5950X with 12 physical cores reported by Windows,
Qt 6.11.1, MSVC Release `windows-msvc-performance`, offscreen platform, 21 measured samples
per page plus one warm-up. Builds used all 12 detected physical cores. The original production
sources came from commit `87e2f887d4af0506f1fd7ef5d870750231a9578f`; both versions used the same
final benchmark harness. No build or other test ran concurrently with either measurement.

All numbers below are medians in milliseconds.

| Page | First display before | After | Improvement | All sections before | After |
| --- | ---: | ---: | ---: | ---: | ---: |
| global-hotkeys | 25.86 | 20.82 | 19.5% | 30.51 | 33.95 |
| global-mouse | 11.00 | 12.42 | -12.9% | 11.03 | 12.49 |
| function-settings | 57.93 | 34.22 | 40.9% | 75.19 | 105.20 |
| interface-settings | 203.66 | 33.60 | 83.5% | 221.90 | 117.87 |
| storage-and-privacy | 159.22 | 34.69 | 78.2% | 177.31 | 188.92 |
| api-configuration | 6.05 | 3.64 | 39.8% | 7.80 | 4.07 |
| extended-features | 6.76 | 6.37 | 5.8% | 6.93 | 6.44 |
| system-settings | 48.29 | 22.63 | 53.1% | 60.45 | 65.25 |
| application-shortcuts | 84.24 | 44.29 | 47.4% | 103.42 | 84.06 |

- Sum of page medians: first display **603.02 → 212.68 ms (64.7% faster)**;
  all-section traversal **694.52 → 618.25 ms (11.0% faster)**;
  retranslation **56.14 → 28.90 ms (48.5% faster)**.
- A 25-notification burst: **25 → 1 refresh**, **15.59 → 0.70 ms (95.5% faster)**.
- Interface settings initially creates **148 widgets instead of 724**, an approximately 80%
  reduction. Complete traversal with all popups creates 735 widgets, including the new section
  bodies and editor shells; controls are deferred, not removed.
- Tradeoff: the median slowest first color-popup opening per sample rises from **18.13 to
  45.79 ms** (optimized p95: **64.61 ms**). Exhaustively visiting all sections and opening every
  color popup increases the sum of page medians from **854.56 to 962.41 ms (12.6%)**.
  Function settings also spends more time on complete section traversal (75.19 → 105.20 ms).
  Initial display improves on every page above the 40 ms baseline threshold; small pages have
  minor differences and Global Mouse is 1.42 ms slower.

The comparison script passes all stated acceptance checks. These are same-machine offscreen
measurements, not promises about native compositor latency or other hardware.

Final validation passed all 10 targeted regression tests in the Release performance build.
Changed C++ formatting checks and `git diff --check` also passed. The full test suite was not run.

Raw results: [before](baselines/settings_page_performance_before.json) and
[after](baselines/settings_page_performance_after.json).
