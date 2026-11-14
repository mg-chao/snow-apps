# Select reference evidence

`visual-reference.json` preserves 32 baseline captures of the executable Ant
Design 6.6.7 Select, including source hashes and package integrity. The separate
`browser-package-lock.json` pins developer-only reference dependencies. The
renderer imports the original checkout rather than reconstructed styles.

`multiple-token-events.json` and `tags-token-events.json` preserve callback
traces from the original running controls after separator input. Model tests
check event order, all resulting selected values and disabled-option behavior.
The original Select package notice is retained in `licenses/`.

The complete 784-scenario matrix, native accessibility and Release performance
results are summarized in [`../select-slider/verification.json`](../select-slider/verification.json)
and the [verification report](../../docs/select-slider-progress.md). Full images
remain in the ignored build tree. Tests use these checked-in fixtures offline.
