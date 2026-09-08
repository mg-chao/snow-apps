# Direct recording laser trail

Snow Shot's direct recording compositor uses `src/laser_trail.rs`. The buffered
recording/export mouse-effect API is independent and retains its existing behavior.
No settings, FFI, dependency, or recording-format migration is required.

The reference is Excalidraw's `packages/excalidraw/laser-trails.ts`,
`animated-trail.ts`, and `@excalidraw/laser-pointer` 1.3.1. Quality priorities are
visual fidelity, bounded rendering cost, then maintainability.

| Rule | Observable contract | Deterministic verification |
| --- | --- | --- |
| R1 | A first stationary observation produces no dot or animation requests. | `initial_and_repeated_stationary_observations_do_not_animate` |
| R2 | Movement uses streamline 0.4 and radius 2 output pixels. Width follows the minimum of quadratic time decay and quartic 50-sample taper. Geometry expires after 500 ms without movement. | `movement_history_keeps_quartic_taper_with_faster_half_second_decay` |
| R3 | Raw duplicate observations do not refresh the trail, including cached frames after expiry. The last visible overlay receives a clean successor frame before scheduling stops. | `stationary_tail_shrinks_expires_and_emits_a_final_clean_frame`, `overlay_scheduler_stays_active_for_trail_and_click_decay` |
| R4 | Downscaling preserves distinct raw movements and fractional geometry. Missing/invisible cursor samples start a separate path upon return while the old path fades. | `downscaling_keeps_distinct_raw_movements_and_subpixel_streamlining`, `invisible_cursor_breaks_path_without_erasing_the_fading_tail` |
| R5 | Antialiased joins and crossings blend the chosen RGBA color once per pixel. Disabled color is a no-op. | `antialiased_crossings_blend_user_opacity_once_and_clear_scratch`, `coverage_union_is_invariant_under_repeated_spans`, `disabled_color_is_noop_and_clear_resets_observations` |
| R6 | History holds at most 50 points. Raster work clips to the output, curve subdivision has a fixed depth limit, and reused scratch has no stale coverage. | `clipped_extreme_coordinates_and_resize_remain_bounded`, `scanline_clipping_matches_exhaustive_pixel_coverage` |

The renderer uses midpoint quadratic centerlines with rounded variable-radius spans,
instead of porting the reference's SVG outline construction. It keeps actual sample
timestamps instead of smoothing timestamp pressure. These deliberate approximations
were independently reviewed and visually compared with the installed reference on
2026-09-08: straight motion, figure-eight loops, zigzags, U-turns, and their decay.
The subsequent requested faster fade halves the lifetime to 500 ms and changes
temporal width to `1 - (age / 500)^2`: 75% width remains at 250 ms, 36% at
400 ms, and zero at 500 ms. Spatial taper and smoothing retain their reference settings.
Keep these choices local to the private renderer; revisit them if tight-turn shape
or timing differences become visible in real recordings. No temporary dependency or
deferred implementation is introduced.

The reusable coverage buffer uses one byte per output pixel (about 7.9 MiB at 4K),
plus an index per touched pixel. It is allocated lazily. Only touched coverage is
cleared, and fully covered or uncovered pixels avoid a square root. At a stationary
cursor, animation scheduling ends after the final cleanup frame.

Run focused tests with `cargo test -p snow-recording-runtime --lib` inside
`snow-crates`, using the repository MSVC/FFmpeg environment. Run the reproducible
Release comparison from the repository root:

```powershell
./scripts/run-recording-laser-perf.ps1 -PreviewDirectory build/windows-msvc-performance/perf/recording-laser
```

The benchmark reports p50/p95 overlay-only time for 1080p and 4K trajectories. It
compares the actual old 350 ms stamped-circle effect with the new 500 ms effect;
the new effect can cost more per frame because it draws a longer, smoother trail.
It excludes capture, frame resizing/copying, and encoding. PPM outputs cover fresh,
decaying, and expired geometry. Runtime acceptance beyond these synthetic checks
is inspection of a recorded clip on the target display/capture backend.

Rollback consists of replacing the private renderer and its direct-compositor
integration; no persisted data changes or point of no return are involved.
