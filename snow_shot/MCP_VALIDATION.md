# MCP acceptance evidence

This iteration targets Windows. The user deferred macOS validation. This document
separates protocol/dispatch coverage from actual feature behavior; a registered tool
or a rejected unauthorized request is not evidence that its underlying feature ran.

## Completed checks

- The Rust bridge has 23 focused unit tests passing, including wire limits,
  saturation/control admission, persistent request correlation, cancellation,
  schema contracts, subscription coalescing, bounded stdio and raw artifact chunks.
  `cargo clippy -p snow-shot-mcp --all-targets -- -D warnings` passes.
- Stdio checks pass for protocol dates 2024-11-05, 2025-03-26, 2025-06-18,
  2025-11-25 and 2026-07-28. These cover discovery, schemas, resources, prompts,
  completion, errors, secret-safe diagnostics, oversized input, blocked output
  and clean shutdown. They use an absent descriptor and do not capture the desktop.
- The checked matrix agrees across 101 Rust tools and Qt registrations in four
  domains. All 28 original tool input fixtures still validate. Eight reviewed UI
  catalogs and recording option adapter keys have change-detection checks. This
  detects missing coverage updates, not behavioral correctness of every field.
- The real-IPC execution gate passes for all 101 tools and all nine resource
  entries/templates across the document, production application and legacy drivers.
  All 28 legacy tools have successful golden outcomes on both original and current
  session/transport code with compatible response fields and content types. Provider,
  scrolling and copy/pin ports in this golden fixture are deterministic substitutes.
- Real stdio → private IPC → Qt synthetic document fixture passes document editing,
  revision conflicts, idempotent open, cached render/save, deterministic recognition
  and job cancellation, legacy/modern resource notifications, Tasks polling and
  four concurrent owner-isolated clients. Large recognition JSON creates an owned
  artifact: resource read, bounded binary IPC chunks, full digest, empty EOF,
  cross-client privacy and release are checked.
- The expanded document IPC fixture passes every document tool, including cloning,
  erasing with undo, template roundtrip, color sampling, recapture, original content,
  auto-filter jobs, clipboard copy and deterministic pin/presentation ports. Its
  report records 34 distinct tools and six resource URI families.
- The isolated production application fixture passes 51 distinct tools through
  Rust and the real application router, including settings/model credentials,
  archive roundtrip, history, pinned content and groups, permission requests,
  translation/retry/Tasks against a local HTTP provider, updater cancellation and
  storage actions. Native region recording finalizes successfully and publishes
  an owned artifact whose digest remains unchanged after the original file changes.
- Fresh native Windows smoke checks passed WGC window capture (544×407) and DXGI
  monitor capture (3840×2160), including expected fixture colors and display IDs.
  The focused clipboard roundtrip test passed using eager MIME/image/URL retention
  and `OleFlushClipboard`.
- Native Windows system-audio loopback passed 100 packets / 48000 frames over
  approximately 1.03 seconds, including pause/resume/stop. The microphone gate was
  skipped because no active default capture endpoint was available; this does not
  establish microphone support on the current hardware.
- The isolated native 28-route probe exited successfully in 6.53 seconds, including
  clipboard restoration and application cleanup: 20 native capture/edit/export
  successes and eight explicit ownership guards. The latter are boundary checks,
  not provider or scrolling successes.
- The focused native scrolling auto-scroll test passed (1.06 seconds), exercising
  Win32 child-process wheel/horizontal messages at signed physical selection
  coordinates and controller cadence.
- Native resident OCR passed English and Chinese fixtures over two warm cycles on
  CPU (7.99 seconds) and DirectML (7.57 seconds). The native QR fixture passed in
  1.42 seconds. These are focused local provider checks, not external account tests.
- The isolated adjacent-launch regression passed with a privately compiled sentinel:
  default mode never starts the sibling application, opt-in launches exactly once
  across repeated requests, and a missing descriptor remains unavailable. The test
  does not start or stop the user's installed Snow Shot process.

The Qt fixtures use deterministic ports and are offscreen-capable. This Windows
static Qt kit uses the repository's Windows QPA fallback; this report does not
claim that the offscreen QPA plugin was loaded. Synthetic recognition/scrolling,
clipboard acknowledgments and pin acknowledgments do not establish native provider,
input-injection or window-manager behavior.

## Measurements and remaining acceptance gates

Release Qt fixtures were built with `windows-msvc-performance`. Original HEAD and
current Rust bridges use identical `release-size` profiles, target, static CRT and
linker flags in separate target directories. Final repeated measurements are pending
an idle build host. Reports record binary digests, output bytes, latency percentiles,
process CPU/private/working-set counters, GUI thread CPU and timer lateness separately.
Cleanup cycles distinguish resource release from allocator memory retention.
Cache checks assert the reported hit and identical bytes/digests; legacy warm
responses also omit encoding timings. No encoder-invocation counter is exposed,
so this evidence must not be described as a measured zero-encoding count.

The original/current legacy fixture compares real session/transport code with
identical synthetic capture ports and the common renderer. Its fake copy/pin
acknowledgments must not be presented as native clipboard/pinned-window benchmarks.

- Final repeated baseline/current performance and expanded recognition capability
  workflows are pending. Performance must be measured while compilers are idle.
- Physical mixed-DPI capture was skipped because this host lacks a suitable monitor
  pair. Multi-monitor/mixed-scale hardware behavior remains unverified here.
- Configured external translation accounts and additional hardware recording,
  microphone and encoder combinations remain outside the verified gates above.
- Packaged installer/adjacent bridge launch validation is pending. A development
  build does not establish packaging correctness.
- macOS permission prompts, native capture/recording and packaged app startup were
  deferred by the user and are not cleared by Windows results.
