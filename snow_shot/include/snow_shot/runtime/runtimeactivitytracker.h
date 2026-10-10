#ifndef SNOW_SHOT_RUNTIME_RUNTIMEACTIVITYTRACKER_H
#define SNOW_SHOT_RUNTIME_RUNTIMEACTIVITYTRACKER_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace snow_shot::runtime {
class RuntimeActivityTracker;

// Copies retain one reservation, including callbacks that outlive their UI owner.
class RuntimeActivityLease final {
  public:
    RuntimeActivityLease() = default;
    [[nodiscard]] explicit operator bool() const {
        return static_cast<bool>(m_reservation);
    }

  private:
    friend class RuntimeActivityTracker;
    explicit RuntimeActivityLease(std::shared_ptr<void> reservation)
        : m_reservation(std::move(reservation)) {}
    std::shared_ptr<void> m_reservation;
};

class RuntimeActivityTracker final {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;
    struct Snapshot {
        std::uint64_t generation = 0;
        std::uint64_t activeCount = 0;
        std::uint64_t idlePeriod = 0;
        TimePoint lastActivity;
    };

    explicit RuntimeActivityTracker(Clock clock = [] { return std::chrono::steady_clock::now(); })
        : m_state(std::make_shared<State>(std::move(clock))) {}

    [[nodiscard]] static RuntimeActivityTracker& shared() {
        // Detached native cleanup can post its final completion during static
        // teardown. Admission state intentionally lasts until process exit.
        static auto* tracker = new RuntimeActivityTracker;
        return *tracker;
    }

    [[nodiscard]] RuntimeActivityLease acquire(bool restartsQuiet = true) {
        auto reservation = std::make_shared<Reservation>(m_state);
        {
            const std::lock_guard lock(m_state->mutex);
            ++m_state->activeCount;
            ++m_state->generation;
            reservation->restartsQuiet = restartsQuiet;
            if (restartsQuiet) {
                ++m_state->idlePeriod;
                m_state->lastActivity = m_state->clock();
            }
            reservation->accepted = true;
        }
        return RuntimeActivityLease(std::move(reservation));
    }

    void markActivity() {
        const std::lock_guard lock(m_state->mutex);
        ++m_state->generation;
        ++m_state->idlePeriod;
        m_state->lastActivity = m_state->clock();
    }

    [[nodiscard]] Snapshot snapshot() const {
        const std::lock_guard lock(m_state->mutex);
        return {m_state->generation, m_state->activeCount, m_state->idlePeriod,
                m_state->lastActivity};
    }

    // Admission uses the same lock as acquire(). Never log, dispatch events, or
    // acquire another activity lease inside callback.
    template <typename Callback>
    bool tryRunWhenIdle(std::uint64_t expectedGeneration, Callback&& callback) {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->activeCount != 0 || m_state->generation != expectedGeneration)
            return false;
        std::forward<Callback>(callback)();
        return true;
    }

  private:
    struct State {
        explicit State(Clock value) : clock(std::move(value)), lastActivity(clock()) {}
        std::mutex mutex;
        Clock clock;
        TimePoint lastActivity;
        std::uint64_t generation = 0;
        std::uint64_t activeCount = 0;
        std::uint64_t idlePeriod = 0;
    };
    struct Reservation {
        explicit Reservation(std::shared_ptr<State> value) : state(std::move(value)) {}
        ~Reservation() {
            if (!accepted)
                return;
            const std::lock_guard lock(state->mutex);
            --state->activeCount;
            ++state->generation;
            if (restartsQuiet) {
                ++state->idlePeriod;
                state->lastActivity = state->clock();
            }
        }
        std::shared_ptr<State> state;
        bool accepted = false;
        bool restartsQuiet = true;
    };
    std::shared_ptr<State> m_state;
};

namespace detail {
class RuntimeWorkReservation final {
  public:
    explicit RuntimeWorkReservation(bool restartsQuiet)
        : m_restartsQuiet(restartsQuiet),
          m_pending(RuntimeActivityTracker::shared().acquire(restartsQuiet)) {}

    [[nodiscard]] RuntimeActivityLease begin() {
        {
            const std::lock_guard lock(m_mutex);
            if (m_pending)
                return std::exchange(m_pending, {});
        }
        // Repeated or concurrent invocations need their own execution lease.
        return RuntimeActivityTracker::shared().acquire(m_restartsQuiet);
    }

  private:
    std::mutex m_mutex;
    const bool m_restartsQuiet;
    RuntimeActivityLease m_pending;
};
} // namespace detail

// Reserve one submission before queuing. Executor copies alias its pending reservation;
// independently queued operations must each call trackRuntimeWork().
template <typename Work> auto trackRuntimeWork(Work work, bool restartsQuiet = true) {
    return [reservation = std::make_shared<detail::RuntimeWorkReservation>(restartsQuiet),
            work = std::move(work)]<typename... Arguments>(
               Arguments&&... arguments) mutable -> std::invoke_result_t<Work&, Arguments...> {
        // Futures and callback storage may retain every callable copy after execution.
        // Only this invocation owns the active lease once work starts.
        const auto activity = reservation->begin();
        return std::invoke(work, std::forward<Arguments>(arguments)...);
    };
}
} // namespace snow_shot::runtime

#endif // SNOW_SHOT_RUNTIME_RUNTIMEACTIVITYTRACKER_H
