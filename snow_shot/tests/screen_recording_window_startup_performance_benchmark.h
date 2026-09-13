#pragma once

class QApplication;

// Measures the screen recording window startup path (controller open → windows
// shown → first paint → first motion-preview frame) through the compile-time
// gated recording_perf instrumentation. Registered in CMake as
// snow-shot-recording-window-startup-performance-benchmark and driven with
// --recording-window-startup-performance.
int runRecordingWindowStartupPerformanceBenchmark(QApplication& app);
