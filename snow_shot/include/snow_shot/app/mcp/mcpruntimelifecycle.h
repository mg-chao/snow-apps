#ifndef SNOW_SHOT_APP_MCP_MCPRUNTIMELIFECYCLE_H
#define SNOW_SHOT_APP_MCP_MCPRUNTIMELIFECYCLE_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <exception>
#include <functional>
#include <utility>

namespace snow_shot::app::mcp {
// Owner-thread reconciliation for one listener/service graph. Configuration is desired state;
// a draining graph must retire before a new graph starts. Suspension is synchronous quiescence.
class McpRuntimeLifecycle final : public QObject {
  public:
    enum class State { Stopped, Starting, Running, Draining, Stopping, Failed };
    struct StartOutcome {
        bool available = false;
        QString error;
    };
    struct Hooks {
        // Synchronous on the owner thread. Failed or throwing construction may leave a partial
        // graph; stop must retire it. Drain and stop must not throw, and stop is a worker barrier.
        std::function<StartOutcome()> start;
        std::function<void(std::function<void()>)> drain;
        std::function<void()> stop;
    };
    explicit McpRuntimeLifecycle(Hooks hooks) : m_hooks(std::move(hooks)) {}
    ~McpRuntimeLifecycle() override {
        // Destruction cannot defer partial-graph cleanup to a later reconciliation. A start
        // callback that destroys its lifecycle must relinquish graph use at that point.
        if (m_state == State::Starting)
            m_state = State::Running;
        shutdown();
    }
    void setEnabled(bool enabled) {
        m_enabled = enabled;
        reconcile();
    }
    [[nodiscard]] State state() const {
        return m_state;
    }
    [[nodiscard]] const QString& error() const {
        return m_error;
    }
    // Reapplying configuration does not spin on an unavailable endpoint. A caller can retry
    // explicitly, or disable/re-enable after resolving the reported failure.
    void retry() {
        if (m_terminal || m_suspended || !m_enabled || m_state != State::Failed)
            return;
        m_state = State::Stopped;
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
    void stopNow(State next = State::Stopped) {
        ++m_generation;
        // A synchronous start can notify observers. Let its stack finish constructing before
        // retiring the graph, even if an observer suspends or shuts down this lifecycle.
        if (m_state == State::Starting || m_state == State::Stopping)
            return;
        if (m_state == State::Stopped || m_state == State::Failed) {
            m_state = next;
            if (next == State::Stopped)
                m_error.clear();
            return;
        }
        m_state = State::Stopping;
        const QPointer<McpRuntimeLifecycle> guard(this);
        const auto stop = m_hooks.stop;
        stop();
        if (!guard)
            return;
        m_state = next;
        if (next == State::Stopped)
            m_error.clear();
        reconcile();
    }
    void reconcile() {
        if (m_terminal || m_suspended)
            return;
        if (m_state == State::Failed && !m_enabled) {
            m_state = State::Stopped;
            m_error.clear();
        }
        if (m_state == State::Stopped && m_enabled) {
            m_state = State::Starting;
            m_error.clear();
            const QPointer<McpRuntimeLifecycle> guard(this);
            const auto start = m_hooks.start;
            StartOutcome outcome;
            try {
                outcome = start();
            } catch (const std::exception& error) {
                outcome.error = QString::fromUtf8(error.what());
            } catch (...) {
                // Even an unknown construction failure must retire its partial graph.
            }
            if (!guard)
                return;
            m_state = State::Running;
            if (!outcome.available) {
                m_error = std::move(outcome.error);
                stopNow(m_terminal || m_suspended ? State::Stopped : State::Failed);
            } else if (m_terminal || m_suspended) {
                stopNow();
            } else {
                reconcile();
            }
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
    QString m_error;
    quint64 m_generation = 0;
    bool m_enabled = false;
    bool m_suspended = false;
    bool m_terminal = false;
};
} // namespace snow_shot::app::mcp
#endif
