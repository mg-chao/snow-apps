# Capture exclusion

Snow Shot keeps capture controls visible while omitting them from captured display
or region pixels. The scrolling overlay and toolbar follow the scrolling UI
capture setting; the recording toolbar follows the recording toolbar setting.
Exclusion failures are best effort and never hide a window.

Windows uses `WDA_EXCLUDEFROMCAPTURE`. macOS 15+ saves the actual NSWindow sharing
policy and temporarily sets it to `NSWindowSharingNone`; **ScreenCaptureKit content
filters enforce omission in Snow Shot's captures**. Changing an NSWindow sharing
policy alone does not guarantee omission from third-party ScreenCaptureKit apps.
Restoration preserves the original policy, including a pre-existing `None` policy.
The native state retains the original NSWindow until restoration and is also
cleaned up when the owning QWidget is destroyed. Qt window access is GUI-thread
only; offscreen Qt platforms return unavailable without interpreting fake handles.

The shared `SnowCaptureExclusions` C structure is accepted by desktop, monitor,
region and continuous stream creation, and direct recording. Arrays are copied
before creation returns, sorted and deduplicated, with a 4096-entry limit per
input array. Empty lists may have null pointers. These IDs are macOS WindowServer
IDs and process IDs, never NSView pointers. A session owns its fixed exclusion
snapshot through pauses and native stream recreation. New native window IDs
require a new session. Independent-window capture rejects nonempty filters.
Windows continues to use display affinity; the macOS lists do not implement
filtering for Windows backends.

Stream configuration version is 2; direct recording configuration version is 6.
Recompile users of the expanded unversioned desktop/monitor/region configurations.
Current direct recording callers use version 6; previous version decoding remains
bounded by the supplied version and struct size.

Build and run the relevant checks:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-window-capture-exclusion-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-macos-window-capture-exclusion-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-screen-recording-controller-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-scrolling-capture-exclusion-tests
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-(window-capture-exclusion|screen-recording-controller|recording-capture-exclusion|scrolling-capture-exclusion)-tests$' --output-on-failure
# Native policy tests need a Cocoa desktop. Pixel tests additionally need existing
# Screen Recording permission; they return 77 when permission is unavailable.
ctest --test-dir build/snow-shot-macos-arm64-performance \
  -R '^snow-shot-macos-(window-capture-exclusion(-pixels)?|recording-capture-exclusion)-tests$' --output-on-failure
```

The offscreen test verifies independent failures, visibility, successful-ID
selection, duplicate exclusion, reverse-order restoration and destructor cleanup.
The Cocoa test verifies native identity, repeated exclusion, policy restoration
and QWidget destruction. Its pixel mode checks a colored overlay above a known
background through generic region snapshot and continuous stream C APIs, and
verifies capture visibility returns after restoration. The recording controller
test verifies the toolbar preference reaches the C configuration and native
sharing is restored after failure, stop and controller destruction. The scrolling
pipeline test exercises actual asynchronous source creation, mode replacement,
export pause/resume, failed replacement and destruction with every combination
of successful and failed window exclusions. It checks each C stream configuration
and verifies streams are joined before restoration. Run the native tests on
both Apple Silicon and Intel and on macOS 15 before platform-wide qualification;
one machine does not qualify other OS versions or mixed-display configurations.
