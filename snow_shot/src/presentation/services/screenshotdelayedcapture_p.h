// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

// Owns one delayed request and its activity reservation, not a capture session. Expiry
// samples current admission and drops busy requests; suspension does not queue a retry.
// Hooks run on the controller's thread and must not throw. Notifications may synchronously
// replace the request or destroy its owner. A scheduler must retain only the supplied task.
class ScreenshotDelayedCapture final {
  public:
    using Action = std::function<void()>;
    struct Hooks {
        std::function<bool()> allowed;
        Action start;
        std::function<void(bool)> activityChanged;
        std::function<void(int, Action)> schedule;
    };

    explicit ScreenshotDelayedCapture(Hooks hooks,
                                      snow_shot::runtime::RuntimeActivityTracker& tracker =
                                          snow_shot::runtime::RuntimeActivityTracker::shared())
        : m_state(std::make_shared<State>(std::move(hooks), tracker)) {}
    ScreenshotDelayedCapture(const ScreenshotDelayedCapture&) = delete;
    ScreenshotDelayedCapture& operator=(const ScreenshotDelayedCapture&) = delete;
    ~ScreenshotDelayedCapture() {
        stop();
    }

    bool request(int delaySeconds) {
        const auto state = m_state;
        if (state->stopped)
            return false;
        const auto allowed = state->hooks.allowed;
        if (!allowed() || state->stopped)
            return false;
        const auto generation = ++state->generation;
        state->activity = state->tracker.acquire();
        const auto notify = state->hooks.activityChanged;
        notify(true);
        if (state->stopped || state->generation != generation)
            return true;
        const auto schedule = state->hooks.schedule;
        schedule(std::clamp(delaySeconds, 1, 10) * 1000,
                 [weak = std::weak_ptr<State>(state), generation] {
                     const auto current = weak.lock();
                     if (!current || current->stopped || current->generation != generation)
                         return;
                     // Retire before notifying. A new request from an idle notification must
                     // supersede this expiry instead of being consumed by its trailing start.
                     const auto retired = ++current->generation;
                     current->activity = {};
                     const auto activityChanged = current->hooks.activityChanged;
                     activityChanged(false);
                     if (current->stopped || current->generation != retired)
                         return;
                     const auto allowed = current->hooks.allowed;
                     if (!allowed() || current->stopped || current->generation != retired)
                         return;
                     const auto start = current->hooks.start;
                     start();
                 });
        return true;
    }

    void invalidate() {
        const auto state = m_state;
        if (state->stopped)
            return;
        ++state->generation;
        state->activity = {};
        const auto notify = state->hooks.activityChanged;
        notify(false);
    }

    void stop() {
        const auto state = m_state;
        if (std::exchange(state->stopped, true))
            return;
        ++state->generation;
        state->activity = {};
        const auto notify = state->hooks.activityChanged;
        state->hooks = {};
        notify(false);
    }

  private:
    struct State {
        State(Hooks value, snow_shot::runtime::RuntimeActivityTracker& activityTracker)
            : hooks(std::move(value)), tracker(activityTracker) {}
        Hooks hooks;
        snow_shot::runtime::RuntimeActivityTracker& tracker;
        snow_shot::runtime::RuntimeActivityLease activity;
        std::uint64_t generation = 0;
        bool stopped = false;
    };
    std::shared_ptr<State> m_state;
};
