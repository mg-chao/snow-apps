#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include <QDebug>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#endif

namespace snow_shot::runtime {
#ifdef Q_OS_WIN
namespace {
// PROCESS_MEMORY_COUNTERS_EX2's documented layout is supported by current Windows,
// but its declaration is missing from some Windows SDKs.
struct ExtendedProcessMemoryCounters {
    PROCESS_MEMORY_COUNTERS_EX base{};
    SIZE_T privateWorkingSetSize = 0;
    ULONG64 sharedCommitUsage = 0;
};

class WindowsMemoryBackend final {
  public:
    WindowsMemoryBackend() {
        m_pressure = CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (!m_pressure) {
            const auto error = GetLastError();
            qWarning() << "Memory pressure notification unavailable:" << error;
        }
    }
    ~WindowsMemoryBackend() {
        if (m_pressure)
            CloseHandle(m_pressure);
    }
    std::optional<bool> lowMemory() {
        if (!m_pressure)
            return std::nullopt;
        BOOL low = FALSE;
        if (!QueryMemoryResourceNotification(m_pressure, &low)) {
            const auto error = GetLastError();
            CloseHandle(m_pressure);
            m_pressure = nullptr;
            qWarning() << "Memory pressure query unavailable:" << error;
            return std::nullopt;
        }
        return low != FALSE;
    }
    std::optional<ProcessMemorySample> sample() {
        ExtendedProcessMemoryCounters counters{};
        counters.base.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters.base),
                                  sizeof(counters))) {
            const auto error = GetLastError();
            const auto now = std::chrono::steady_clock::now();
            if (!m_lastError || now - *m_lastError >= std::chrono::minutes(5)) {
                m_lastError = now;
                qWarning() << "Memory sample unavailable:" << error;
            }
            return std::nullopt;
        }
        return ProcessMemorySample{counters.privateWorkingSetSize, counters.base.PageFaultCount};
    }

  private:
    HANDLE m_pressure = nullptr;
    std::optional<std::chrono::steady_clock::time_point> m_lastError;
};
} // namespace
#endif

MemoryOptimizationController::Options nativeMemoryOptimizationOptions() {
    MemoryOptimizationController::Options options;
#ifdef Q_OS_WIN
    auto backend = std::make_shared<WindowsMemoryBackend>();
    options.lowMemory = [backend] { return backend->lowMemory(); };
    options.sample = [backend] { return backend->sample(); };
    options.trim = [] {
        const bool succeeded = EmptyWorkingSet(GetCurrentProcess()) != FALSE;
        return MemoryTrimResult{succeeded, succeeded ? 0U : GetLastError()};
    };
    options.report = [](const MemoryTrimReport& report) {
        const auto elapsed = std::chrono::duration<double, std::milli>(report.duration).count();
        if (!report.result.succeeded) {
            qWarning() << "Memory trim failed:" << report.result.error << "duration_ms=" << elapsed;
            return;
        }
        qInfo() << "Memory trim: pressure=" << report.lowMemory
                << "private_working_set_before=" << report.before.memoryBytes
                << "private_working_set_after="
                << (report.after ? QString::number(report.after->memoryBytes)
                                 : QStringLiteral("unavailable"))
                << "duration_ms=" << elapsed;
    };
#endif
    return options;
}

bool currentProcessOwnsForegroundWindow() {
#ifdef Q_OS_WIN
    const auto foreground = GetForegroundWindow();
    if (!foreground)
        return true; // Unknown desktop state is not a safe trim opportunity.
    DWORD process = 0;
    if (!GetWindowThreadProcessId(foreground, &process))
        return true;
    return process == GetCurrentProcessId();
#else
    return false;
#endif
}
} // namespace snow_shot::runtime
