#ifndef SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H
#define SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H

#include <QtTypes>
#include <QDebug>
#include <QObject>
#include <QPoint>
#include <QTimer>
#include <functional>
#include <utility>

namespace snow_shot::platform::windows::detail {
// WinEvent's source thread is the producer, which may be DWM rather than the target app.
[[nodiscard]] bool isUnderlyingCursorUpdate(quint32 event, qint32 object, quint32 sourceThread,
                                            quint32 callerThread, quint32 targetThread);

struct CursorRefreshTarget {
    quintptr window = 0;
    quint32 thread = 0;
    QPoint position;
    bool operator==(const CursorRefreshTarget&) const = default;
};

// Native input dispatch acknowledges a local target even when its cursor is unchanged.
// Desktop WinEvents can acknowledge only foreign targets; our own cursor changes are
// not evidence that the underlying window has handled the requested mouse update.
class CursorRefreshOperation final : public QObject {
  public:
    struct Backend {
        quint32 callerThread;
        std::function<CursorRefreshTarget()> target;
        std::function<bool(bool localTarget)> refresh;
        std::function<bool()> flush;
        std::function<void()> disarm;
    };

    CursorRefreshOperation(QObject* context, Backend backend) : backend_(std::move(backend)) {
        connect(context, &QObject::destroyed, this, [this] {
            cancelled_ = true;
            completed_ = {};
            backend_.disarm();
        });
    }
    void start(std::function<void(bool)> completed) {
        if (cancelled_ || started_)
            return;
        started_ = true;
        completed_ = std::move(completed);
        const auto target = backend_.target();
        if (!target.window || !target.thread ||
            !backend_.refresh(target.thread == backend_.callerThread)) {
            finish(false);
            return;
        }
        QTimer::singleShot(1000, this, [this] {
            if (completed_) {
                qWarning("Timed out waiting for a desktop cursor update before recapture");
                finish(false);
            }
        });
        // Hiding the editor can route input to a foreign window before start().
        // Its already observed desktop update is valid only at that same target.
        if (desktopNotification_ && acknowledgedTarget_ == backend_.target())
            acknowledge(acknowledgedTarget_);
    }
    void cursorChanged(quint32 event, qint32 object, quint32 sourceThread) {
        if (cancelled_ || (started_ && !completed_))
            return;
        const auto target = backend_.target();
        if (!isUnderlyingCursorUpdate(event, object, sourceThread, backend_.callerThread,
                                      target.thread))
            return;
        desktopNotification_ = true;
        acknowledgedTarget_ = target;
        if (completed_)
            acknowledge(target);
    }
    void mouseDispatched(quintptr receiver, const QPoint& position) {
        if (!completed_)
            return;
        const auto target = backend_.target();
        if (receiver && receiver == target.window && position == target.position &&
            target.thread == backend_.callerThread)
            acknowledge(target);
    }

  private:
    void acknowledge(const CursorRefreshTarget& target) {
        acknowledgedTarget_ = target;
        if (completionQueued_)
            return;
        completionQueued_ = true;
        // Qt's Windows dispatcher delivers posted calls before window-system input.
        // Cross that batch boundary first, then commit in the next posted batch so
        // queued enter/leave events have selected the child widget's cursor.
        QTimer::singleShot(0, this, [this] {
            QTimer::singleShot(0, this, [this] {
                completionQueued_ = false;
                if (!completed_ || acknowledgedTarget_ != backend_.target())
                    return;
                if (!backend_.flush()) {
                    finish(false);
                    return;
                }
                if (acknowledgedTarget_ == backend_.target())
                    finish(true);
            });
        });
    }
    void finish(bool ready) {
        backend_.disarm();
        auto completed = std::exchange(completed_, {});
        // The callback may destroy this operation.
        if (completed)
            completed(ready);
    }
    Backend backend_;
    CursorRefreshTarget acknowledgedTarget_;
    std::function<void(bool)> completed_;
    bool cancelled_ = false;
    bool started_ = false;
    bool completionQueued_ = false;
    bool desktopNotification_ = false;
};
} // namespace snow_shot::platform::windows::detail

#endif // SNOW_SHOT_PLATFORM_WINDOWS_WINDOWCURSORREFRESH_H
