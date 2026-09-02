#pragma once

#include <chrono>
#include <cstdint>

namespace snow_canvas_render_diagnostics {

// Render diagnostics (stage timers, replay accounting, watermark metrics)
// exist for the module's test and benchmark executables. Collection is
// disabled by default so production paint paths perform no measurement work;
// consumers opt in once at process start. Simple counter increments next to
// real work are not affected by this switch.
void setEnabled(bool enabled);
bool isEnabled();

// Accumulates wall time into `destination` while in scope. Inert unless
// diagnostics were enabled for the process.
class StageTimer {
public:
    explicit StageTimer(std::uint64_t& destination)
        : m_destination(destination), m_enabled(isEnabled()) {
        if (m_enabled) {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ~StageTimer() {
        if (m_enabled) {
            m_destination +=
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - m_start)
                                               .count());
        }
    }

    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;

private:
    std::uint64_t& m_destination;
    bool m_enabled;
    std::chrono::steady_clock::time_point m_start{};
};

} // namespace snow_canvas_render_diagnostics
