#ifndef SNOW_SHOT_PLATFORM_MACOS_RECAPTUREFOCUS_H
#define SNOW_SHOT_PLATFORM_MACOS_RECAPTUREFOCUS_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <functional>
#include <memory>
#include <utility>

class QWidget;

namespace snow_shot::platform::macos {
// The transaction owns input transparency and focus until capture completes, not
// merely until the foreign application has activated. Destruction also cancels
// pending preparation, so an old callback cannot dispatch another capture.
class RecaptureFocus final : public QObject {
  public:
    struct Backend {
        std::function<bool()> begin;
        std::function<bool()> ready;
        std::function<bool()> refreshCursor;
        std::function<void()> restore;
        std::function<bool()> cursorReady = {};
    };

    explicit RecaptureFocus(Backend backend) : m_backend(std::move(backend)) {
        m_poll.setInterval(10);
        connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
    }
    ~RecaptureFocus() override {
        m_poll.stop();
        m_backend.restore();
    }
    void prepare(std::function<void(bool)> completed) {
        m_completed = std::move(completed);
        m_deadline.start();
        if (!m_backend.begin()) {
            complete(false);
            return;
        }
        m_poll.start();
        poll();
    }

  private:
    void complete(bool ready) {
        m_poll.stop();
        auto completed = std::exchange(m_completed, {});
        // Completion may destroy this transaction.
        if (completed)
            completed(ready);
    }
    void poll() {
        if (m_cursorPending) {
            if (!m_backend.cursorReady || m_backend.cursorReady()) {
                complete(true);
                return;
            }
        } else if (m_backend.ready()) {
            if (!m_backend.refreshCursor()) {
                complete(false);
                return;
            }
            m_cursorPending = true;
        }
        if (m_deadline.hasExpired(1000))
            complete(false);
    }

    bool m_cursorPending = false;
    Backend m_backend;
    QTimer m_poll;
    QElapsedTimer m_deadline;
    std::function<void(bool)> m_completed;
};

std::unique_ptr<RecaptureFocus> createRecaptureFocus(const QVector<QWidget*>& windows);
} // namespace snow_shot::platform::macos
#endif
