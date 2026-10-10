#include "snow_shot/presentation/screenshotpresentationframeclock.h"
#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#endif

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

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

void deadlinesNeverRunBeforeTheInjectedClock() {
    qint64 now = 0;
    int callbacks = 0;
    QEventLoop loop;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; },
                                                   [&] {
                                                       ++callbacks;
                                                       loop.quit();
                                                   });
    scheduler.setDeadline(5000000);
    require(scheduler.active(), "arming a deadline must mark the scheduler active");
    pumpEvents(20);
    require(callbacks == 0,
            "native timer wakeups must not invoke a callback before the monotonic deadline");
    now = 5000000;
    waitForCallback(loop, callbacks, 1);
    require(!scheduler.active(), "a delivered deadline must become inactive before rearming");
    pumpEvents(20);
    require(callbacks == 1, "an unrearmed deadline must not repeat its callback");
}

void cancellationDiscardsThePendingDeadline() {
    qint64 now = 0;
    int callbacks = 0;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; }, [&] { ++callbacks; });
    scheduler.setDeadline(5000000);
    scheduler.stop();
    now = 10000000;
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 0,
            "a stopped deadline must not invoke callbacks after its scheduled time");
}

void cancelDeadlineRetainsTheBackendWithoutDispatchingCanceledFrames() {
    qint64 now = 0;
    int callbacks = 0;
    QEventLoop loop;
    ScreenshotPresentationFrameScheduler scheduler([&] { return now; },
                                                   [&] {
                                                       ++callbacks;
                                                       loop.quit();
                                                   });
    scheduler.setDeadline(5000000);
    QObject* backend = scheduler.wakeupObject();
    scheduler.cancelDeadline();
    scheduler.cancelDeadline();
    now = 10000000;
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 0 && scheduler.wakeupObject() == backend,
            "canceling a deadline must discard callbacks and retain its backend");
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
    pumpEvents(20);
    require(!scheduler.active() && callbacks == 1,
            "canceling an already delivered deadline must not schedule further wakeups");
    scheduler.stop();
#ifdef Q_OS_WIN
    require(!notifier || !notifier->isEnabled(),
            "stopping the scheduler must disable its retained native wait");
#endif
}

void rearmingRetainsTheBackendAndUsesTheLatestDeadline() {
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
                                                   });
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

void callbacksMayDestroyTheScheduler() {
    int callbacks = 0;
    QEventLoop loop;
    std::unique_ptr<ScreenshotPresentationFrameScheduler> scheduler;
    scheduler = std::make_unique<ScreenshotPresentationFrameScheduler>([] { return qint64(0); },
                                                                       [&] {
                                                                           ++callbacks;
                                                                           scheduler.reset();
                                                                           loop.quit();
                                                                       });
    scheduler->setDeadline(0);
    waitForCallback(loop, callbacks, 1);
    require(!scheduler, "a wakeup callback must be able to retire its scheduler safely");
    pumpEvents(10);
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
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    deadlinesNeverRunBeforeTheInjectedClock();
    cancellationDiscardsThePendingDeadline();
    cancelDeadlineRetainsTheBackendWithoutDispatchingCanceledFrames();
    rearmingRetainsTheBackendAndUsesTheLatestDeadline();
    callbacksMayDestroyTheScheduler();
    fractionalFrameDeadlinesRetainTheirPhaseAfterLateWork();
    return 0;
}
