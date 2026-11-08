#ifndef SNOW_SHOT_APP_MCP_MCPRUNTIMELIFECYCLE_H
#define SNOW_SHOT_APP_MCP_MCPRUNTIMELIFECYCLE_H

#include <QObject>
#include <QPointer>
#include <functional>
#include <utility>

namespace snow_shot::app::mcp {
// Owner-thread reconciliation for one listener/service graph. Configuration is desired state;
// a draining graph must retire before a new graph starts. Suspension is synchronous quiescence.
class McpRuntimeLifecycle final : public QObject {
  public:
    struct Hooks {
        std::function<void()> start;
        std::function<void(std::function<void()>)> drain;
        std::function<void()> stop;
    };
    explicit McpRuntimeLifecycle(Hooks hooks) : m_hooks(std::move(hooks)) {}
    ~McpRuntimeLifecycle() override {
        shutdown();
    }
    void setEnabled(bool enabled) {
        m_enabled = enabled;
        reconcile();
    }
    void setSuspended(bool suspended) {
        if (m_suspended == suspended)
            return;
        m_suspended = suspended;
        if (suspended)
            stopNow();
        else
            reconcile();
    }
    void shutdown() {
        m_terminal = true;
        stopNow();
    }

  private:
    enum class State { Stopped, Running, Draining, Stopping };
    void stopNow() {
        ++m_generation;
        if (m_state == State::Stopped || m_state == State::Stopping)
            return;
        m_state = State::Stopping;
        const QPointer<McpRuntimeLifecycle> guard(this);
        const auto stop = m_hooks.stop;
        stop();
        if (!guard)
            return;
        m_state = State::Stopped;
        reconcile();
    }
    void reconcile() {
        if (m_terminal || m_suspended)
            return;
        if (m_state == State::Stopped && m_enabled) {
            m_state = State::Running;
            const auto start = m_hooks.start;
            start();
        } else if (m_state == State::Running && !m_enabled) {
            m_state = State::Draining;
            const auto generation = ++m_generation;
            const auto drain = m_hooks.drain;
            drain([guard = QPointer<McpRuntimeLifecycle>(this), generation] {
                if (!guard || guard->m_generation != generation)
                    return;
                guard->stopNow();
            });
        }
    }
    Hooks m_hooks;
    State m_state = State::Stopped;
    quint64 m_generation = 0;
    bool m_enabled = false;
    bool m_suspended = false;
    bool m_terminal = false;
};
} // namespace snow_shot::app::mcp
#endif
