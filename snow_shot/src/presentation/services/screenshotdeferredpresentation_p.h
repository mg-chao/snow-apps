// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>
#include <memory>
#include <utility>

// Retain only idle presentation intent, never captured pixels or worker reservations.
// The controller closes this admission together with capture admission, before notifying
// observers. Each step rechecks suspension at dispatch; resume retries only pending work.
// All calls and non-throwing actions execute on the context's thread.
class ScreenshotDeferredPresentation final {
  public:
    using Action = std::function<void()>;
    struct Actions {
        Action prewarmCapture;
        Action prewarmPin;
        Action restorePins;
    };
    ScreenshotDeferredPresentation(QObject& context, Actions actions)
        : m_state(std::make_shared<State>()) {
        m_state->context = &context;
        m_state->actions = {std::move(actions.prewarmCapture), std::move(actions.prewarmPin),
                            std::move(actions.restorePins)};
    }
    ScreenshotDeferredPresentation(const ScreenshotDeferredPresentation&) = delete;
    ScreenshotDeferredPresentation& operator=(const ScreenshotDeferredPresentation&) = delete;
    ~ScreenshotDeferredPresentation() {
        stop();
    }
    void requestPrewarm() {
        if (m_state->stopped)
            return;
        m_state->pending[0] = true;
        m_state->pending[1] = true;
        schedule(m_state);
    }
    // Startup and active-group restoration have the same operation: read current repository
    // and group state at execution. Equivalent pending requests can therefore coalesce.
    void requestRestore() {
        if (m_state->stopped)
            return;
        m_state->pending[2] = true;
        schedule(m_state);
    }
    void setSuspended(bool suspended) {
        m_state->suspended = suspended;
        if (!suspended)
            schedule(m_state);
    }
    void stop() {
        m_state->stopped = true;
        m_state->pending = {};
        m_state->actions = {};
    }

  private:
    struct State {
        QPointer<QObject> context;
        std::array<Action, 3> actions;
        std::array<bool, 3> pending{};
        bool suspended = false;
        bool stopped = false;
        bool scheduled = false;
        bool executing = false;
    };
    static void schedule(const std::shared_ptr<State>& state) {
        if (state->stopped || state->suspended || state->scheduled || state->executing ||
            !state->context || state->pending == std::array<bool, 3>{})
            return;
        state->scheduled = true;
        QTimer::singleShot(0, state->context, [weak = std::weak_ptr<State>(state)] {
            const auto current = weak.lock();
            if (!current)
                return;
            current->scheduled = false;
            if (current->stopped || current->suspended || !current->context)
                return;
            for (std::size_t i = 0; i != current->pending.size(); ++i) {
                if (!std::exchange(current->pending[i], false))
                    continue;
                current->executing = true;
                const auto action = current->actions[i];
                if (action)
                    action();
                // The callback can destroy the controller or this owner, suspend it, or
                // process nested events. No owner access follows external code.
                current->executing = false;
                schedule(current);
                return;
            }
        });
    }
    std::shared_ptr<State> m_state;
};
