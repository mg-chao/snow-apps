#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QTimer>
#include <QPointer>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#else
#include <QChronoTimer>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

struct ScreenshotPresentationFrameScheduler::Impl {
    Impl(std::function<qint64()> monotonicClock, std::function<void()> onWakeup)
        : monotonicNanoseconds(std::move(monotonicClock)), wakeup(std::move(onWakeup)) {
        Q_ASSERT(monotonicNanoseconds);
#ifdef Q_OS_WIN
        fallbackTimer.setObjectName(QStringLiteral("screenshotPresentationFrameTimer"));
        fallbackTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&fallbackTimer, &QTimer::timeout, &fallbackTimer,
                         [this] { handleWakeup(); });
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
        timer.setObjectName(QStringLiteral("screenshotPresentationFrameTimer"));
        timer.setTimerType(Qt::PreciseTimer);
        timer.setSingleShot(true);
        QObject::connect(&timer, &QChronoTimer::timeout, &timer, [this] { handleWakeup(); });
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
#ifdef Q_OS_WIN
        fallbackTimer.stop();
        if (timerHandle) {
            CancelWaitableTimer(timerHandle);
        }
        if (notifier) {
            notifier->setEnabled(false);
        }
#else
        timer.stop();
#endif
    }

    void cancelDeadline() {
        if (!armed) {
            return;
        }
        armed = false;
#ifdef Q_OS_WIN
        if (nativeAvailable) {
            // Keep the notifier registered for the next capture frame deadline.
            CancelWaitableTimer(timerHandle);
        } else {
            fallbackTimer.stop();
        }
#else
        timer.stop();
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
        if (!fallbackTimer.isActive()) {
            fallbackTimer.start(1);
        }
#else
        timer.setInterval(std::chrono::nanoseconds(remaining));
        if (!timer.isActive()) {
            timer.start();
        }
#endif
    }

    void handleWakeup() {
        if (!armed) {
            return;
        }
        if (monotonicNanoseconds() < deadlineNs) {
#ifdef Q_OS_WIN
            if (nativeAvailable) {
                armWakeup();
            }
#else
            armWakeup();
#endif
            return;
        }
        armed = false;
#ifdef Q_OS_WIN
        const QPointer<QTimer> fallbackGuard(nativeAvailable ? nullptr : &fallbackTimer);
#endif
        // The callback may destroy its owner. Hold the callable locally and leave
        // the backend retained for a callback that arms the next frame deadline.
        const auto callback = wakeup;
        if (callback) {
            callback();
        }
#ifdef Q_OS_WIN
        // A callback that re-arms retains the precision timer. If the callback
        // destroyed its owner, the guard prevents accessing the retired Impl.
        if (fallbackGuard && !armed) {
            fallbackGuard->stop();
        }
#endif
    }

    std::function<qint64()> monotonicNanoseconds;
    std::function<void()> wakeup;
    qint64 deadlineNs = 0;
    bool armed = false;
#ifdef Q_OS_WIN
    QTimer fallbackTimer;
    HANDLE timerHandle = nullptr;
    std::unique_ptr<QWinEventNotifier> notifier;
    bool nativeAvailable = false;
#else
    QChronoTimer timer;
#endif
};

ScreenshotPresentationFrameScheduler::ScreenshotPresentationFrameScheduler(
    std::function<qint64()> monotonicNanoseconds, std::function<void()> wakeup, QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(std::move(monotonicNanoseconds), std::move(wakeup))) {}

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
    return m_impl->nativeAvailable ? static_cast<QObject*>(m_impl->notifier.get())
                                   : &m_impl->fallbackTimer;
#else
    return &m_impl->timer;
#endif
}
