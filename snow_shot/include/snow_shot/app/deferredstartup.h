// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace snow_shot::app {

// Own the finite work deferred until the first explicit file launch has presented. Each step
// observes current admission, including a pause acquired reentrantly by the preceding step.
// The composition root owns services and decides availability; this owner only retains work.
// All calls are on the context's thread. Step and admission callbacks must not throw.
class DeferredStartup final {
  public:
    using Action = std::function<void()>;
    struct Steps {
        Action startUpdates;
        Action maintainRegistrations;
        Action prewarmCapture;
        Action restorePins;
        Action configureOcr;
        Action offerRecordingRecovery;
    };

    DeferredStartup(QObject& context, std::function<bool()> available)
        : m_state(std::make_shared<State>()) {
        m_state->context = &context;
        m_state->available = std::move(available);
    }
    DeferredStartup(const DeferredStartup&) = delete;
    DeferredStartup& operator=(const DeferredStartup&) = delete;
    ~DeferredStartup() {
        stop();
    }

    // The first-file continuation may complete more than once; work is still admitted once.
    void start(Steps steps) {
        const auto state = m_state;
        if (state->started || state->stopped)
            return;
        state->started = true;
        state->steps = {std::move(steps.startUpdates),   std::move(steps.maintainRegistrations),
                        std::move(steps.prewarmCapture), std::move(steps.restorePins),
                        std::move(steps.configureOcr),   std::move(steps.offerRecordingRecovery)};
        schedule(state);
    }

    // Restoration retries only the next unconsumed step, never a completed native operation.
    void resume() {
        schedule(m_state);
    }
    void stop() {
        m_state->stopped = true;
        m_state->steps = {};
        m_state->available = {};
    }

  private:
    struct State {
        QPointer<QObject> context;
        std::function<bool()> available;
        std::array<Action, 6> steps;
        std::size_t next = 0;
        bool started = false;
        bool stopped = false;
        bool scheduled = false;
        bool executing = false;
    };

    static void schedule(const std::shared_ptr<State>& state) {
        if (!state->started || state->stopped || state->scheduled || state->executing ||
            !state->context || state->next == state->steps.size())
            return;
        state->scheduled = true;
        QTimer::singleShot(0, state->context, [weak = std::weak_ptr<State>(state)] {
            const auto pending = weak.lock();
            if (!pending)
                return;
            pending->scheduled = false;
            if (pending->stopped || !pending->context)
                return;
            pending->executing = true;
            const auto available = pending->available;
            if (!available() || pending->stopped || !pending->context) {
                pending->executing = false;
                return;
            }
            auto step = std::move(pending->steps[pending->next++]);
            if (step)
                step();
            // The step may destroy its controller or this owner. Shared state carries only
            // cancellation and remaining work; no member access follows that callback.
            pending->executing = false;
            schedule(pending);
        });
    }

    std::shared_ptr<State> m_state;
};

} // namespace snow_shot::app
