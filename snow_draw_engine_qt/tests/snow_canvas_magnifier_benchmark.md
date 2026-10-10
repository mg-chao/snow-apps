# Magnifier rendering benchmark

Build `snow-canvas-magnifier-benchmark` with `windows-msvc-performance` (Release).
Run only this benchmark; packaging and Debug presets omit benchmark targets.

The executable measures one lens, 24 mixed-shape lenses, independent destination
movement, source movement, and 24 lenses above 1,000 annotations. All lenses use
10× enlargement. It prints mean, median, and p95 milliseconds as CSV, verifies
that original images remain shared, and rejects any filter-path annotation replay.

`creation` includes the dashed source contour on one lens, and `selected` includes
source contours and selectable dashed frames on 24 mixed-shape lenses. Compare
these with `single` and `many` to measure the guide painting cost.

Use `--scenario single|many|lens_drag|source_drag|annotations|creation|selected` to select one case
and `--iterations N` to set measured frames (100 by default, after five warmups).
Only immutable original images and the output image are retained; magnifier
painting allocates no sampled crop or enlarged image surface.

Local validation on 2026-10-10 with `windows-msvc-performance`, 1920×1080,
300 measured frames: 24 lenses averaged 5.154 ms without guides and 5.146 ms
with selection guides (p95: 5.701 ms and 5.637 ms). The painting cost remained
within measurement variation; original image storage and source draw counts were
unchanged. These cases measure painting with retained guides, excluding overlay
composition and selection handles.
