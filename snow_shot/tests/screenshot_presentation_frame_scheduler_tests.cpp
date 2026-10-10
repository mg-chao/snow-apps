#include "snow_shot/presentation/screenshotpresentationframeclock.h"
#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QChronoTimer>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#endif

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace {
using Backend = ScreenshotPresentationFrameScheduler::Backend;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class WakeupObserver final : public QObject {
  public:
    explicit WakeupObserver(QObject* backend) : m_backend(backend) {
        backend->installEventFilter(this);
    }

    void waitForNextWakeup() {
        const int initialWakeups = wakeups;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        m_waitingLoop = &loop;
        watchdog.start(1000);
        loop.exec();
        m_waitingLoop = nullptr;
        require(wakeups > initialWakeups, "the backend must deliver the armed timer wakeup");
    }

    int wakeups = 0;

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == m_backend &&
            (event->type() == QEvent::Timer || event->type() == QEvent::WinEventAct)) {
            ++wakeups;
            if (m_waitingLoop) {
                m_waitingLoop->quit();
            }
        }
        return false;
    }

  private:
    QPointer<QObject> m_backend;
    QEventLoop* m_waitingLoop = nullptr;
};

void pumpEvents(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void waitForCallback(QEventLoop& loop, const int& callbacks, int expected) {
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(1000);
    if (callbacks < expected) {
        loop.exec();
    }
    require(callbacks == expected, "the armed presentation deadline must wake exactly once");
}

void deadlinesNeverRunBeforeTheInjectedClock(Backend backend) {
    qint64 now = 0;
    int callbacks = 0;
    QEventLoop loop;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; },
                                                   [&] {
                                                       ++callbacks;
                                                       loop.quit();
                                                   },
                                                   backend);
    WakeupObserver observer(scheduler.wakeupObject());
    scheduler.setDeadline(5000000);
    require(scheduler.active(), "arming a deadline must mark the scheduler active");
    observer.waitForNextWakeup();
    require(callbacks == 0,
            "timer wakeups must not invoke a callback before the monotonic deadline");
    now = 5000000;
    waitForCallback(loop, callbacks, 1);
    require(!scheduler.active(), "a delivered deadline must become inactive before rearming");
    const int deliveredWakeups = observer.wakeups;
    pumpEvents(20);
    require(callbacks == 1 && observer.wakeups == deliveredWakeups,
            "an unrearmed deadline must leave no idle backend wakeups or repeated callbacks");
}

void cancellationDiscardsThePendingDeadline(Backend backend) {
    qint64 now = 0;
    int callbacks = 0;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; }, [&] { ++callbacks; },
                                                   backend);
    WakeupObserver observer(scheduler.wakeupObject());
    scheduler.setDeadline(5000000);
    scheduler.stop();
    now = 10000000;
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 0,
            "a stopped deadline must not invoke callbacks after its scheduled time");
    const int canceledWakeups = observer.wakeups;
    pumpEvents(20);
    require(observer.wakeups == canceledWakeups,
            "a stopped backend must not continue waking during idle presentation");
}

void cancelDeadlineRetainsTheBackendWithoutDispatchingCanceledFrames(Backend selectedBackend) {
    qint64 now = 0;
    int callbacks = 0;
    QEventLoop loop;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; },
                                                   [&] {
                                                       ++callbacks;
                                                       loop.quit();
                                                   },
                                                   selectedBackend);
    WakeupObserver observer(scheduler.wakeupObject());
    scheduler.setDeadline(5000000);
    QObject* backend = scheduler.wakeupObject();
    scheduler.cancelDeadline();
    scheduler.cancelDeadline();
    now = 10000000;
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 0 && scheduler.wakeupObject() == backend,
            "canceling a deadline must discard callbacks and retain its backend");
    const int canceledWakeups = observer.wakeups;
    pumpEvents(20);
    require(observer.wakeups == canceledWakeups,
            "retaining a canceled backend must not poll for idle frames");
#ifdef Q_OS_WIN
    auto* notifier = qobject_cast<QWinEventNotifier*>(backend);
    require(!notifier || notifier->isEnabled(),
            "canceling a deadline must retain the enabled Windows native wait");
#endif
    scheduler.setDeadline(15000000);
    require(scheduler.active() && scheduler.wakeupObject() == backend,
            "arming after cancellation must reuse the active capture backend");
    pumpEvents(20);
    require(callbacks == 0, "a rearmed deadline must still respect its monotonic target");
    now = 15000000;
    waitForCallback(loop, callbacks, 1);
    scheduler.cancelDeadline();
    const int deliveredWakeups = observer.wakeups;
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 1 && observer.wakeups == deliveredWakeups,
            "canceling an already delivered deadline must not schedule further backend wakeups");
    scheduler.stop();
#ifdef Q_OS_WIN
    require(!notifier || !notifier->isEnabled(),
            "stopping the scheduler must disable its retained native wait");
#endif
}

void rearmingRetainsTheBackendAndUsesTheLatestDeadline(Backend selectedBackend) {
    qint64 now = 0;
    int callbacks = 0;
    QEventLoop loop;
    ScreenshotPresentationFrameScheduler* activeScheduler = nullptr;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; },
                                                   [&] {
                                                       ++callbacks;
                                                       if (callbacks == 1) {
                                                           activeScheduler->setDeadline(20000000);
                                                       }
                                                       loop.quit();
                                                   },
                                                   selectedBackend);
    activeScheduler = &scheduler;
    scheduler.setDeadline(5000000);
    QObject* backend = scheduler.wakeupObject();
    for (int request = 0; request < 20; ++request) {
        scheduler.setDeadline(10000000);
        require(scheduler.wakeupObject() == backend,
                "updating a deadline must retain the same backend object");
    }
    now = 5000000;
    pumpEvents(20);
    require(callbacks == 0, "replacing a deadline must discard its earlier scheduled time");
    now = 10000000;
    waitForCallback(loop, callbacks, 1);
    require(scheduler.active() && scheduler.wakeupObject() == backend,
            "rearming from a callback must retain the backend and remain active");
    now = 20000000;
    waitForCallback(loop, callbacks, 2);
    require(!scheduler.active(), "an unrearmed second deadline must become inactive");
    scheduler.stop();
    scheduler.setDeadline(30000000);
    require(scheduler.wakeupObject() == backend,
            "stopping and restarting must preserve the reusable native resource");
    scheduler.stop();
}

void callbacksMayDestroyTheScheduler(Backend backend) {
    int callbacks = 0;
    QEventLoop loop;
    std::unique_ptr<ScreenshotPresentationFrameScheduler> scheduler;
    scheduler = std::make_unique<ScreenshotPresentationFrameScheduler>([] { return qint64(0); },
                                                                       [&] {
                                                                           ++callbacks;
                                                                           scheduler.reset();
                                                                           loop.quit();
                                                                       },
                                                                       backend);
    const QPointer<QObject> wakeupObject = scheduler->wakeupObject();
    scheduler->setDeadline(0);
    waitForCallback(loop, callbacks, 1);
    require(!scheduler && !wakeupObject,
            "a wakeup callback must be able to retire its scheduler and backend safely");
    pumpEvents(10);
}

void qtBackendArmsOnlyTheRemainingDeadline() {
    qint64 now = 10000000;
    int callbacks = 0;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; }, [&] { ++callbacks; },
                                                   Backend::QtTimer);
    auto* timer = qobject_cast<QChronoTimer*>(scheduler.wakeupObject());
    require(timer && timer->isSingleShot() && timer->timerType() == Qt::PreciseTimer,
            "the Qt backend must retain a precise single-shot timer");
    WakeupObserver observer(timer);
    scheduler.setDeadline(35000000);
    require(
        timer->isActive() && timer->interval() == std::chrono::milliseconds(25),
        "the Qt timer must wait for the remaining deadline instead of polling every millisecond");

    QTimerEvent earlyWakeup(timer->id());
    QCoreApplication::sendEvent(timer, &earlyWakeup);
    require(callbacks == 0 && scheduler.active() && timer->isActive() &&
                timer->interval() == std::chrono::milliseconds(25),
            "an early Qt timer event must rearm the remaining absolute deadline");

    now = 15000000;
    scheduler.setDeadline(22000000);
    require(timer->isActive() && timer->interval() == std::chrono::milliseconds(7),
            "replacing a deadline must restart the same timer for the new remaining interval");
    now = 22000000;
    QTimerEvent dueWakeup(timer->id());
    QCoreApplication::sendEvent(timer, &dueWakeup);
    require(callbacks == 1 && !scheduler.active() && !timer->isActive(),
            "the delivered Qt deadline must leave both the scheduler and timer inactive");
    const int deliveredWakeups = observer.wakeups;
    pumpEvents(20);
    require(callbacks == 1 && observer.wakeups == deliveredWakeups,
            "a manually delivered deadline must also retire real queued timer wakeups");
}

void fractionalFrameDeadlinesRetainTheirPhaseAfterLateWork() {
    ScreenshotPresentationFrameClock clock;
    constexpr qint64 origin = 1000000000;
    for (const qreal rate : {60.0, 120.0, 144.0, 240.0}) {
        clock.reset();
        clock.setRefreshRate(rate, origin);
        const qint64 period = qRound64(1000000000.0 / rate);
        for (qint64 frame = 1; frame <= 100; ++frame) {
            const qint64 deadline = origin + frame * period;
            require(clock.nextFrameNanoseconds() == deadline && !clock.due(deadline - 1) &&
                        clock.due(deadline),
                    "fractional periods must retain absolute frame deadlines without ms rounding");
            clock.advancePast(deadline);
        }
        clock.advancePast(origin + 104 * period + 1);
        require(clock.nextFrameNanoseconds() == origin + 105 * period,
                "late work must skip elapsed deadlines without shifting the display phase");
    }
    clock.reset();
    clock.setRefreshRate(std::numeric_limits<qreal>::quiet_NaN(), origin);
    require(clock.nextFrameNanoseconds() == origin + 16666667,
            "invalid refresh rates must use the 60 Hz frame period");
}

void changingRefreshRateReanchorsAtTheLastCommittedFrame() {
    ScreenshotPresentationFrameClock clock;
    constexpr qint64 origin = 1000000000;
    clock.setRefreshRate(60.0, origin);
    const qint64 committedFrame = clock.nextFrameNanoseconds();
    clock.advancePast(committedFrame + 50000000);
    const qint64 nextAt60Hz = clock.nextFrameNanoseconds();
    clock.setRefreshRate(60.0, committedFrame);
    require(clock.nextFrameNanoseconds() == nextAt60Hz,
            "an unchanged refresh rate must preserve its advanced frame phase");
    clock.setRefreshRate(240.0, committedFrame);
    const qint64 period = qRound64(1000000000.0 / 240.0);
    require(clock.nextFrameNanoseconds() == committedFrame + period,
            "a changed refresh rate must start its new phase at the last committed frame");
    clock.advancePast(committedFrame + 3 * period);
    require(clock.nextFrameNanoseconds() == committedFrame + 4 * period,
            "the changed frame rate must still skip expired deadlines without phase drift");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    for (const Backend backend : {Backend::Automatic, Backend::QtTimer}) {
        deadlinesNeverRunBeforeTheInjectedClock(backend);
        cancellationDiscardsThePendingDeadline(backend);
        cancelDeadlineRetainsTheBackendWithoutDispatchingCanceledFrames(backend);
        rearmingRetainsTheBackendAndUsesTheLatestDeadline(backend);
        callbacksMayDestroyTheScheduler(backend);
    }
    qtBackendArmsOnlyTheRemainingDeadline();
    fractionalFrameDeadlinesRetainTheirPhaseAfterLateWork();
    changingRefreshRateReanchorsAtTheLastCommittedFrame();
    return 0;
}
