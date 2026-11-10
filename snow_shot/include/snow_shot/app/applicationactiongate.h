#pragma once

#include <functional>
#include <utility>

namespace snow_shot::app {

// Application admission is a live snapshot, not a reservation or feature entitlement.
// Authorization may notify observers or enter a native event loop. Recheck availability
// after it returns; feature owners still authorize final options and own completion.
class ApplicationActionGate final {
  public:
    using Action = std::function<void()>;
    using Check = std::function<bool()>;

    explicit ApplicationActionGate(Check available) : m_available(std::move(available)) {}

    [[nodiscard]] bool dispatch(Action action, Check authorize = {}) const {
        // Callbacks can destroy the composition root. The copied predicate must include
        // its owner's lifetime guard, and no member may be accessed after a callback.
        const auto available = m_available;
        if (!available() || (authorize && !authorize()) || !available())
            return false;
        if (action)
            action();
        return true;
    }

    [[nodiscard]] bool beginGesture(Action cancel, Action action, Check authorize = {}) const {
        if (dispatch(std::move(action), std::move(authorize)))
            return true;
        if (cancel)
            cancel();
        return false;
    }

  private:
    Check m_available;
};
} // namespace snow_shot::app
