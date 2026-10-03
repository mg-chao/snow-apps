#pragma once

#include <functional>

namespace snow_canvas_runtime {

// Cleanup jobs run serially on a process-owned worker. They must own only detached,
// thread-independent resources and must not throw. QWidget and runtime-client detachment stays
// on the owner thread.
void enqueueRuntimeCleanup(std::function<void()> cleanup);

// Drains queued cleanup at explicit synchronization boundaries, never from runtime destruction.
void waitForRuntimeCleanup();

} // namespace snow_canvas_runtime
