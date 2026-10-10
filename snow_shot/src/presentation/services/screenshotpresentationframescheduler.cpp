#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QChronoTimer>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

struct ScreenshotPresentationFrameScheduler::Impl {
    Impl(std::function<qint64()> monotonicClock, std::function<void()> onWakeup, Backend backend)
        : monotonicNanoseconds(std::move(monotonicClock)), wakeup(std::move(onWakeup)) {
        Q_ASSERT(monotonicNanoseconds);
        timer.setObjectName(QStringLiteral("screenshotPresentationFrameTimer"));
        timer.setTimerType(Qt::PreciseTimer);
        timer.setSingleShot(true);
        QObject::connect(&timer, &QChronoTimer::timeout, &timer, [this] { handleWakeup(); });
#ifdef Q_OS_WIN
        if (backend == Backend::QtTimer) {
            return;
        }
        timerHandle =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timerHandle) {
            timerHandle =
                CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        if (timerHandle) {
            notifier = std::make_unique<QWinEventNotifier>();
            notifier->setHandle(timerHandle);
            notifier->setObjectName(QStringLiteral("screenshotPresentationFrameNotifier"));
            notifier->setEnabled(false);
            QObject::connect(notifier.get(), &QWinEventNotifier::activated, notifier.get(),
                             [this] { handleWakeup(); });
            nativeAvailable = true;
        }
#else
        Q_UNUSED(backend);
#endif
    }

    ~Impl() {
        stop();
#ifdef Q_OS_WIN
        // The notifier's callback and posted events must retire before its HANDLE.
        notifier.reset();
        if (timerHandle) {
            CloseHandle(timerHandle);
        }
#endif
    }

    void setDeadline(qint64 deadline) {
        if (armed && deadlineNs == deadline) {
            return;
        }
        deadlineNs = deadline;
        armed = true;
        armWakeup();
    }

    void stop() {
        armed = false;
        timer.stop();
#ifdef Q_OS_WIN
        if (timerHandle) {
            CancelWaitableTimer(timerHandle);
        }
        if (notifier) {
            notifier->setEnabled(false);
        }
#endif
    }

    void cancelDeadline() {
        if (!armed) {
            return;
        }
        armed = false;
        timer.stop();
#ifdef Q_OS_WIN
        if (nativeAvailable) {
            // Keep the notifier registered for the next capture frame deadline.
            CancelWaitableTimer(timerHandle);
        }
#endif
    }

    void armWakeup() {
        const qint64 now = monotonicNanoseconds();
        const qint64 remaining = deadlineNs > now ? deadlineNs - now : 0;
#ifdef Q_OS_WIN
        if (nativeAvailable) {
            // Relative Windows deadlines are negative counts of 100 ns. Round up so
            // the native callback cannot deliberately precede the display deadline.
            const qint64 ticks = std::max<qint64>(1, remaining / 100 + (remaining % 100 != 0));
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -ticks;
            if (SetWaitableTimerEx(timerHandle, &dueTime, 0, nullptr, nullptr, nullptr, 0)) {
                notifier->setEnabled(true);
                return;
            }
            // Keep a deadline-gated fallback if the platform cannot arm the native timer.
            nativeAvailable = false;
            CancelWaitableTimer(timerHandle);
            notifier->setEnabled(false);
        }
#endif
        // Replacing a deadline must also restart an equal-length interval. Keep the
        // timer object, but wake only for the remaining deadline instead of polling.
        timer.stop();
        timer.setInterval(std::chrono::nanoseconds(remaining));
        timer.start();
    }

    void handleWakeup() {
        if (!armed) {
            return;
        }
        if (monotonicNanoseconds() < deadlineNs) {
            armWakeup();
            return;
        }
        armed = false;
        // Retire any other pending backend wake before the callback can rearm or destroy us.
        timer.stop();
        // The callback may destroy its owner. Hold the callable locally and leave
        // all member access before it.
        const auto callback = wakeup;
        if (callback) {
            callback();
        }
    }

    std::function<qint64()> monotonicNanoseconds;
    std::function<void()> wakeup;
    qint64 deadlineNs = 0;
    bool armed = false;
    QChronoTimer timer;
#ifdef Q_OS_WIN
    HANDLE timerHandle = nullptr;
    std::unique_ptr<QWinEventNotifier> notifier;
    bool nativeAvailable = false;
#endif
};

ScreenshotPresentationFrameScheduler::ScreenshotPresentationFrameScheduler(
    std::function<qint64()> monotonicNanoseconds, std::function<void()> wakeup, QObject* parent)
    : ScreenshotPresentationFrameScheduler(std::move(monotonicNanoseconds), std::move(wakeup),
                                           Backend::Automatic, parent) {}

ScreenshotPresentationFrameScheduler::ScreenshotPresentationFrameScheduler(
    std::function<qint64()> monotonicNanoseconds, std::function<void()> wakeup, Backend backend,
    QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(std::move(monotonicNanoseconds), std::move(wakeup), backend)) {}

ScreenshotPresentationFrameScheduler::~ScreenshotPresentationFrameScheduler() = default;

void ScreenshotPresentationFrameScheduler::setDeadline(qint64 deadlineNs) {
    m_impl->setDeadline(deadlineNs);
}

void ScreenshotPresentationFrameScheduler::cancelDeadline() {
    m_impl->cancelDeadline();
}

void ScreenshotPresentationFrameScheduler::stop() {
    m_impl->stop();
}

bool ScreenshotPresentationFrameScheduler::active() const {
    return m_impl->armed;
}

QObject* ScreenshotPresentationFrameScheduler::wakeupObject() const {
#ifdef Q_OS_WIN
    return m_impl->nativeAvailable ? static_cast<QObject*>(m_impl->notifier.get()) : &m_impl->timer;
#else
    return &m_impl->timer;
#endif
}
