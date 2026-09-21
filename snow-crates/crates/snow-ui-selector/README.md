# Native element selection

## Windows behavior and limits

UIA selection lazily caches one level at a time in Control View. Sibling order is
used as the default precedence, but is not assumed to describe visual stacking:
when a hit branch ends without a matching child, selection can search earlier
overlapping siblings through `Pane` and `Group` ancestors whose clipped bounds
equal their parent's bounds. This allows content behind redundant structural
branches (including Chromium containers) to remain reachable. Concrete controls
and containers with distinct bounds retain their precedence. Structural containers
are traversed, not removed by a control-type blacklist.

Successful native calls returning a null child collection represent a leaf.
Actual provider failures are preserved. Alternative searches share the existing
168 ms foreground budget, 1,500 ms refinement budget, 500 ms refinement call limit,
80-step traversal limit, cancellation, and lazy cache. These limits are not extended
to accommodate a particular application.

## macOS selection

The macOS backend selects the native element under the pointer and searches its exposed
children for finer frames. When hit testing is unsupported, it searches the AX window
matching the Quartz snapshot instead. Results run from the deepest eligible element to
its enclosing window. Native AX objects never cross worker threads.

## macOS behavior and limits

- Queries follow the hit element’s parent chain to its first AX window, then validate
  its PID and bounds against the Quartz snapshot before publishing child frames.
  They do not depend on `AXWindow`, which can be absent or return a stale WebKit proxy
  despite a valid parent chain. Frames are clipped to the visible display; duplicate
  rectangles and hidden, unrelated, or broken parent branches are rejected.
- Child enumeration prefers `AXVisibleChildren`, falling back to `AXChildren` when unsupported.
  Pages contain at most 32 references; traversal is limited to 512 visited nodes, 64 levels,
  and 48 distinct frames. Missing bounds do not hide a container's descendants.
- The native hit branch takes priority. Within that branch, deeper candidates win, then
  smaller visible area, then provider order. Provider order is not a guarantee of stacking.
- Ordinary foreground queries have a 168 ms budget. Refinement has a 1,500 ms budget and
  500 ms per-call limit. Cancellation and stale-result checks remain active throughout.
- Smart selection temporarily enables `AXManualAccessibility`, falling back to
  `AXEnhancedUserInterface` for providers such as Firefox. Firefox can apply an enhanced
  accessibility write while replying `NotImplemented`; the new value must be verified
  before that reply is accepted. Failed activation writes receive bounded cleanup.
  Applications that are already enabled are left enabled; unsupported applications still
  use ordinary AX.
  No hover path prompts for system permission or changes persistent application settings.
- A newly activated Electron tree can initialize asynchronously. `AccessibilityPending`
  preserves the initial window/element fallback and admits refinement. Probes back off at
  100, 200, 400, then 500 ms, with a five-second deadline from activation. Each probe is bounded
  to 168 ms; an initialized tree then receives the normal refinement budget. Web content
  appearing in the validated hierarchy signals application readiness. A complete query
  with a concrete control (such as a text field, button, or image) and a child frame
  is immediately ready for that branch. This does not mark other branches ready:
  selecting a toolbar must not suppress later page initialization. Queries with only
  structural containers get a final bounded traversal at the deadline. This is not a longer per-call timeout.
- Activation is retained across snapshot refreshes and pointer movement in the capture.
  Closing the session cancels queries and releases its activation leases once those queries
  finish. Overlapping captures share per-process ownership, identified by PID and start time.
  Only the last owner restores the specific flag Snow Shot enabled. Restoration is
  best-effort: macOS has no exclusive ownership token for changes made by other
  accessibility clients.

No complete window tree is cached. Apps with inaccessible or unexposed content retain a
window fallback. Native calls depend on the target honoring macOS IPC timeouts; the UI
thread never performs those calls or waits for worker shutdown.

## Focused diagnostics

Set `SNOW_SELECTOR_DIAGNOSTICS=1` to log activation outcome, initialization time, visited
nodes, maximum depth, frame count, and traversal stop reason. Element text is never logged.
Restoration failures are reported even without that switch.

From `snow-crates/`, run the live diagnostic:

```sh
SNOW_SELECTOR_DIAGNOSTICS=1 cargo run -p snow-ui-selector --example macos_probe -- X Y DISPLAY_ID [EXCLUDED_WINDOW_ID ...]
```

Coordinates use Snow Shot's physical display coordinates. The optional exclusions are
Quartz window IDs. The diagnostic performs foreground and refinement queries and releases
the session before exiting. It is a functional probe, not a performance benchmark.

Related deterministic checks:

```sh
cargo test -p snow-ui-selector --lib
cargo test -p snow-ui-selector-c --lib
cargo clippy -p snow-ui-selector -p snow-ui-selector-c --all-targets -- -D warnings
```

Qt coverage is in the `snow-shot-selector-phased-tests` and
`snow-shot-capture-workflow-tests` targets. The desktop fixture is
`snow-shot-macos-smart-selection-smoke`. Run only these related tests when changing the
selector. Use the macOS **performance** preset for benchmarks.
