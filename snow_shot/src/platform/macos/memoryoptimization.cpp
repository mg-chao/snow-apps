#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include <QDebug>
#include <QGuiApplication>
#include <atomic>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <malloc/malloc.h>

namespace snow_shot::runtime {
namespace {
// The dispatch source owns this context until cancellation finishes. Handlers never
// reference the backend, which may already have been destroyed on another thread.
struct MemoryPressureState {
    dispatch_source_t source = nullptr;
    std::atomic<bool> elevated = false;
};

class MacOSMemoryBackend final {
  public:
    MacOSMemoryBackend() {
        m_pressure =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                   DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                                       DISPATCH_MEMORYPRESSURE_CRITICAL,
                                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        if (!m_pressure) {
            qWarning() << "Memory pressure notification unavailable";
            return;
        }
        m_pressureState = new MemoryPressureState;
        m_pressureState->source = m_pressure;
        dispatch_set_context(m_pressure, m_pressureState);
        dispatch_source_set_event_handler_f(m_pressure, [](void* context) {
            auto* state = static_cast<MemoryPressureState*>(context);
            const auto flags = dispatch_source_get_data(state->source);
            // Only record pressure here. Reclamation waits for the controller's idle
            // admission instead of touching allocations on the notification queue.
            state->elevated.store(
                (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL)) != 0,
                std::memory_order_relaxed);
        });
        dispatch_source_set_cancel_handler_f(
            m_pressure, [](void* context) { delete static_cast<MemoryPressureState*>(context); });
        dispatch_resume(m_pressure);
    }

    ~MacOSMemoryBackend() {
        if (m_pressure) {
            dispatch_source_cancel(m_pressure);
            dispatch_release(m_pressure);
        }
    }

    std::optional<bool> lowMemory() const {
        if (!m_pressureState)
            return std::nullopt;
        return m_pressureState->elevated.load(std::memory_order_relaxed);
    }

    std::optional<ProcessMemorySample> sample() {
        task_vm_info_data_t memory{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        auto error = task_info(mach_task_self(), TASK_VM_INFO,
                               reinterpret_cast<task_info_t>(&memory), &count);
        if (error != KERN_SUCCESS || count < TASK_VM_INFO_REV1_COUNT) {
            reportSampleError(error);
            return std::nullopt;
        }
        task_events_info_data_t events{};
        count = TASK_EVENTS_INFO_COUNT;
        error = task_info(mach_task_self(), TASK_EVENTS_INFO,
                          reinterpret_cast<task_info_t>(&events), &count);
        if (error != KERN_SUCCESS || count < TASK_EVENTS_INFO_COUNT) {
            reportSampleError(error);
            return std::nullopt;
        }
        return ProcessMemorySample{memory.phys_footprint,
                                   static_cast<std::uint32_t>(events.faults)};
    }

  private:
    void reportSampleError(kern_return_t error) {
        const auto now = std::chrono::steady_clock::now();
        if (!m_lastError || now - *m_lastError >= std::chrono::minutes(5)) {
            m_lastError = now;
            qWarning() << "Memory sample unavailable:" << error;
        }
    }

    dispatch_source_t m_pressure = nullptr;
    MemoryPressureState* m_pressureState = nullptr;
    std::optional<std::chrono::steady_clock::time_point> m_lastError;
};
} // namespace

MemoryOptimizationController::Options nativeMemoryOptimizationOptions() {
    MemoryOptimizationController::Options options;
    auto backend = std::make_shared<MacOSMemoryBackend>();
    options.lowMemory = [backend] { return backend->lowMemory(); };
    options.sample = [backend] { return backend->sample(); };
    options.trim = [] {
        // This releases unused allocator pages, preserving all live allocations.
        // Zero bytes is a successful best-effort no-op, not an API failure.
        return MemoryTrimResult{true, 0, malloc_zone_pressure_relief(nullptr, 0)};
    };
    options.report = [](const MemoryTrimReport& report) {
        qInfo() << "Memory trim: pressure=" << report.lowMemory
                << "physical_footprint_before=" << report.before.memoryBytes
                << "physical_footprint_after="
                << (report.after ? QString::number(report.after->memoryBytes)
                                 : QStringLiteral("unavailable"))
                << "allocator_released_bytes=" << report.result.releasedBytes << "duration_ms="
                << std::chrono::duration<double, std::milli>(report.duration).count();
    };
    return options;
}

bool currentProcessOwnsForegroundWindow() {
    const auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    // Without GUI state, conservatively block automatic reclamation.
    return !application || QGuiApplication::applicationState() == Qt::ApplicationActive;
}
} // namespace snow_shot::runtime
