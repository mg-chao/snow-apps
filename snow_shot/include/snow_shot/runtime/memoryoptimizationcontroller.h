#ifndef SNOW_SHOT_RUNTIME_MEMORYOPTIMIZATIONCONTROLLER_H
#define SNOW_SHOT_RUNTIME_MEMORYOPTIMIZATIONCONTROLLER_H

#include "snow_shot/runtime/runtimeactivitytracker.h"
#include <QObject>
#include <QTimer>
#include <optional>

namespace snow_shot::runtime {
struct ProcessMemorySample {
    // Windows: private working set. macOS: physical footprint, including compressed memory.
    std::uint64_t memoryBytes = 0;
    std::uint64_t pageFaultCount = 0;
};
struct MemoryTrimResult {
    bool succeeded = false;
    std::uint32_t error = 0;
    // Allocator-reported reclamation on macOS; Windows does not report this value.
    std::uint64_t releasedBytes = 0;
};
struct MemoryTrimReport {
    bool lowMemory = false;
    ProcessMemorySample before;
    std::optional<ProcessMemorySample> after;
    MemoryTrimResult result;
    std::chrono::steady_clock::duration duration{};
};

class MemoryOptimizationController final : public QObject {
  public:
    struct Options {
        RuntimeActivityTracker::Clock clock = [] { return std::chrono::steady_clock::now(); };
        std::function<bool()> enabled;
        std::function<bool()> safe;
        std::function<std::optional<bool>()> lowMemory;
        std::function<std::optional<ProcessMemorySample>()> sample;
        std::function<MemoryTrimResult()> trim;
        std::function<void(const MemoryTrimReport&)> report;
    };
    MemoryOptimizationController(RuntimeActivityTracker& activity, Options options,
                                 QObject* parent = nullptr);
    void synchronizePolicy();
    void evaluate();
    void stop();
    [[nodiscard]] bool isScheduling() const {
        return m_timer.isActive();
    }

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    RuntimeActivityTracker& m_activity;
    Options m_options;
    QTimer m_timer;
    RuntimeActivityTracker::TimePoint m_started;
    RuntimeActivityTracker::TimePoint m_policyChanged;
    std::optional<RuntimeActivityTracker::TimePoint> m_lastAttempt;
    std::optional<std::uint64_t> m_attemptedIdlePeriod;
    bool m_enabled = false;
    bool m_blocked = true;
    bool m_stopped = false;
};

// Copies of native options retain the pressure-notification backend.
[[nodiscard]] MemoryOptimizationController::Options nativeMemoryOptimizationOptions();
[[nodiscard]] bool currentProcessOwnsForegroundWindow();
} // namespace snow_shot::runtime
#endif
