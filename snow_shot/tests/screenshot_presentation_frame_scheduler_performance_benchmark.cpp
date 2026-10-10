#include "snow_shot/presentation/screenshotpresentationframeclock.h"
#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QCommandLineParser>
#include <QChronoTimer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
enum class Phase { Warmup, Active, IdleSettle, Idle };
enum class Implementation { PollingQtTimer, QtDeadline, AutomaticDeadline };
constexpr int kIdleSettleMilliseconds = 10;
constexpr double kQtDeadlineMaximumWakesPerFrame = 2.0;
enum class WakeClassification { Early, Due, StaleTimer, Inactive, RetiredBackend };

QString implementationName(Implementation implementation) {
    switch (implementation) {
    case Implementation::PollingQtTimer:
        return QStringLiteral("polling_qtimer");
    case Implementation::QtDeadline:
        return QStringLiteral("qt_deadline");
    case Implementation::AutomaticDeadline:
        return QStringLiteral("automatic_deadline");
    }
    return {};
}

struct PhaseWork {
    qint64 wakeups = 0;
    qint64 timerWakeups = 0;
    qint64 nativeWakeups = 0;
    qint64 dispatchNanoseconds = 0;
    qint64 callbackNanoseconds = 0;
    qint64 callbacks = 0;
    qint64 entryEarlyWakeups = 0;
    qint64 entryDueWakeups = 0;
    qint64 gatedEarlyWakeups = 0;
    qint64 dueWakeups = 0;
    qint64 deliveringWakeups = 0;
    qint64 staleTimerWakeups = 0;
    qint64 inactiveWakeups = 0;
    qint64 retiredBackendWakeups = 0;
    qint64 unexplainedWakeups = 0;
    qint64 invalidDeliveries = 0;
};

struct WakeSample {
    Phase phase;
    WakeClassification entryClassification;
    qint64 entryNs;
    qint64 exitNs = 0;
    qint64 deadlineNs;
    qint64 dispatchNs = 0;
    qint64 deliveredCallbacks = 0;
    std::optional<int> eventTimerId;
    std::optional<int> liveTimerId;
    bool timerIdValid = true;
    bool timerEvent;
};

// Time the complete QObject event delivery, including deadline gating and rearming,
// for every implementation. Callback timings alone omit that work for polling.
class WakeupObserver final {
  public:
    WakeupObserver(std::function<QObject*()> backend, std::function<qint64()> clock,
                   std::function<qint64()> deadline, std::function<bool()> armed)
        : m_backend(std::move(backend)), m_clock(std::move(clock)), m_deadline(std::move(deadline)),
          m_armed(std::move(armed)) {
        trackBackend();
    }

    void reserveSamples(std::size_t count) {
        samples.reserve(count);
    }

    void trackBackend() {
        QObject* backend = m_backend();
        m_currentBackend = backend;
        if (m_backends.empty() || m_backends.back() != backend) {
            m_backends.push_back(backend);
            backendHistory.append(QString::fromLatin1(backend->metaObject()->className()));
        }
    }

    bool observes(QObject* receiver, QEvent* event) {
        if (event->type() != QEvent::Timer && event->type() != QEvent::WinEventAct)
            return false;
        trackBackend();
        return std::find(m_backends.begin(), m_backends.end(), receiver) != m_backends.end();
    }

    Phase phase = Phase::Warmup;
    std::array<PhaseWork, 4> work;
    QJsonArray backendHistory;

    PhaseWork& currentWork() {
        return work[static_cast<std::size_t>(phase)];
    }

    WakeSample beginEvent(QObject* receiver, QEvent* event) {
        WakeSample sample{};
        sample.phase = phase;
        sample.entryClassification = WakeClassification::Due;
        sample.entryNs = m_clock();
        sample.deadlineNs = m_deadline();
        sample.timerEvent = event->type() == QEvent::Timer;
        if (sample.timerEvent) {
            const auto eventId = static_cast<QTimerEvent*>(event)->id();
            Qt::TimerId liveId = Qt::TimerId::Invalid;
            if (auto* chronoTimer = qobject_cast<QChronoTimer*>(receiver))
                liveId = chronoTimer->id();
            else if (auto* timer = qobject_cast<QTimer*>(receiver))
                liveId = timer->id();
            sample.eventTimerId = static_cast<int>(eventId);
            sample.liveTimerId = static_cast<int>(liveId);
            sample.timerIdValid = liveId != Qt::TimerId::Invalid && liveId == eventId;
        }
        if (receiver != m_currentBackend)
            sample.entryClassification = WakeClassification::RetiredBackend;
        else if (!sample.timerIdValid)
            sample.entryClassification = WakeClassification::StaleTimer;
        else if (!m_armed())
            sample.entryClassification = WakeClassification::Inactive;
        else if (sample.entryNs < sample.deadlineNs)
            sample.entryClassification = WakeClassification::Early;
        auto& phaseWork = currentWork();
        ++phaseWork.wakeups;
        if (sample.timerEvent)
            ++phaseWork.timerWakeups;
        else
            ++phaseWork.nativeWakeups;
        return sample;
    }

    void finishEvent(WakeSample sample, qint64 dispatchNs, qint64 callbacksBefore) {
        sample.exitNs = m_clock();
        sample.dispatchNs = dispatchNs;
        auto& phaseWork = work[static_cast<std::size_t>(sample.phase)];
        phaseWork.dispatchNanoseconds += dispatchNs;
        sample.deliveredCallbacks = phaseWork.callbacks - callbacksBefore;
        if (sample.deliveredCallbacks > 0)
            ++phaseWork.deliveringWakeups;
        switch (sample.entryClassification) {
        case WakeClassification::Early:
            ++phaseWork.entryEarlyWakeups;
            if (sample.deliveredCallbacks == 0)
                ++phaseWork.gatedEarlyWakeups;
            else
                ++phaseWork.dueWakeups;
            break;
        case WakeClassification::Due:
            ++phaseWork.entryDueWakeups;
            ++phaseWork.dueWakeups;
            if (sample.deliveredCallbacks == 0)
                ++phaseWork.unexplainedWakeups;
            break;
        case WakeClassification::StaleTimer:
            ++phaseWork.staleTimerWakeups;
            break;
        case WakeClassification::Inactive:
            ++phaseWork.inactiveWakeups;
            break;
        case WakeClassification::RetiredBackend:
            ++phaseWork.retiredBackendWakeups;
            break;
        }
        if (sample.deliveredCallbacks > 1 ||
            (sample.deliveredCallbacks > 0 &&
             sample.entryClassification != WakeClassification::Early &&
             sample.entryClassification != WakeClassification::Due))
            ++phaseWork.invalidDeliveries;
        samples.push_back(sample);
    }

    std::vector<WakeSample> samples;

  private:
    std::function<QObject*()> m_backend;
    std::function<qint64()> m_clock;
    std::function<qint64()> m_deadline;
    std::function<bool()> m_armed;
    QObject* m_currentBackend = nullptr;
    std::vector<QObject*> m_backends;
};

class BenchmarkApplication final : public QCoreApplication {
  public:
    using QCoreApplication::QCoreApplication;
    WakeupObserver* observer = nullptr;

    bool notify(QObject* receiver, QEvent* event) override {
        if (!observer || !observer->observes(receiver, event))
            return QCoreApplication::notify(receiver, event);
        const auto sample = observer->beginEvent(receiver, event);
        const qint64 callbacksBefore = observer->currentWork().callbacks;
        QElapsedTimer elapsed;
        elapsed.start();
        const bool accepted = QCoreApplication::notify(receiver, event);
        observer->finishEvent(sample, elapsed.nsecsElapsed(), callbacksBefore);
        return accepted;
    }
};

struct CpuSnapshot {
    std::optional<double> processMilliseconds;
    std::optional<double> guiThreadMilliseconds;
};

CpuSnapshot cpuSnapshot() {
#ifdef Q_OS_WIN
    const auto milliseconds = [](const FILETIME& kernel, const FILETIME& user) {
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    };
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    CpuSnapshot result;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        result.processMilliseconds = milliseconds(kernel, user);
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        result.guiThreadMilliseconds = milliseconds(kernel, user);
    return result;
#else
    const std::clock_t value = std::clock();
    return {value == std::clock_t(-1)
                ? std::nullopt
                : std::optional<double>(static_cast<double>(value) * 1000.0 / CLOCKS_PER_SEC),
            std::nullopt};
#endif
}

QJsonValue cpuDifference(const std::optional<double>& begin, const std::optional<double>& end) {
    return begin && end ? QJsonValue(*end - *begin) : QJsonValue(QJsonValue::Null);
}

QJsonObject distribution(std::vector<double> values) {
    if (values.empty())
        return {{QStringLiteral("samples"), 0}};
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const double value : values)
        sum += value;
    const auto percentile = [&values](double fraction) {
        const auto index = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
        return values[index];
    };
    return {{QStringLiteral("samples"), static_cast<qint64>(values.size())},
            {QStringLiteral("mean"), sum / static_cast<double>(values.size())},
            {QStringLiteral("median"), percentile(0.5)},
            {QStringLiteral("p95"), percentile(0.95)},
            {QStringLiteral("p99"), percentile(0.99)},
            {QStringLiteral("min"), values.front()},
            {QStringLiteral("max"), values.back()}};
}

QJsonObject phaseWork(const PhaseWork& work) {
    return {{QStringLiteral("backend_wake_events"), work.wakeups},
            {QStringLiteral("timer_wake_events"), work.timerWakeups},
            {QStringLiteral("native_wake_events"), work.nativeWakeups},
            {QStringLiteral("delivered_frames"), work.callbacks},
            {QStringLiteral("entry_early_backend_wake_events"), work.entryEarlyWakeups},
            {QStringLiteral("entry_due_backend_wake_events"), work.entryDueWakeups},
            {QStringLiteral("gated_early_backend_wake_events"), work.gatedEarlyWakeups},
            {QStringLiteral("due_backend_wake_events"), work.dueWakeups},
            {QStringLiteral("delivering_backend_wake_events"), work.deliveringWakeups},
            {QStringLiteral("stale_timer_id_wake_events"), work.staleTimerWakeups},
            {QStringLiteral("inactive_backend_wake_events"), work.inactiveWakeups},
            {QStringLiteral("retired_backend_wake_events"), work.retiredBackendWakeups},
            {QStringLiteral("unexplained_backend_wake_events"), work.unexplainedWakeups},
            {QStringLiteral("invalid_backend_deliveries"), work.invalidDeliveries},
            {QStringLiteral("dispatch_work_us"),
             static_cast<double>(work.dispatchNanoseconds) / 1000.0},
            {QStringLiteral("callback_work_us"),
             static_cast<double>(work.callbackNanoseconds) / 1000.0}};
}

QString phaseName(Phase phase) {
    switch (phase) {
    case Phase::Warmup:
        return QStringLiteral("warmup");
    case Phase::Active:
        return QStringLiteral("active");
    case Phase::IdleSettle:
        return QStringLiteral("idle_settle");
    case Phase::Idle:
        return QStringLiteral("idle");
    }
    return {};
}

QString classificationName(WakeClassification classification) {
    switch (classification) {
    case WakeClassification::Early:
        return QStringLiteral("early");
    case WakeClassification::Due:
        return QStringLiteral("due");
    case WakeClassification::StaleTimer:
        return QStringLiteral("stale_timer_id");
    case WakeClassification::Inactive:
        return QStringLiteral("inactive");
    case WakeClassification::RetiredBackend:
        return QStringLiteral("retired_backend");
    }
    return {};
}

QJsonArray wakeSamples(const std::vector<WakeSample>& samples) {
    QJsonArray result;
    for (const auto& sample : samples) {
        QString dispatchClassification = classificationName(sample.entryClassification);
        if (sample.deliveredCallbacks > 0) {
            dispatchClassification = sample.entryClassification == WakeClassification::Due
                                         ? QStringLiteral("delivered_due_frame")
                                     : sample.entryClassification == WakeClassification::Early
                                         ? QStringLiteral("deadline_crossed_during_dispatch")
                                         : QStringLiteral("invalid_delivery");
        } else if (sample.entryClassification == WakeClassification::Early) {
            dispatchClassification = QStringLiteral("gated_early");
        }
        result.append(QJsonObject{
            {QStringLiteral("phase"), phaseName(sample.phase)},
            {QStringLiteral("event_kind"),
             sample.timerEvent ? QStringLiteral("timer") : QStringLiteral("native")},
            {QStringLiteral("entry_ns"), sample.entryNs},
            {QStringLiteral("exit_ns"), sample.exitNs},
            {QStringLiteral("deadline_ns"), sample.deadlineNs},
            {QStringLiteral("entry_remaining_ns"), sample.deadlineNs - sample.entryNs},
            {QStringLiteral("entry_classification"),
             classificationName(sample.entryClassification)},
            {QStringLiteral("dispatch_classification"), dispatchClassification},
            {QStringLiteral("delivered_callbacks"), sample.deliveredCallbacks},
            {QStringLiteral("dispatch_work_ns"), sample.dispatchNs},
            {QStringLiteral("event_timer_id"),
             sample.eventTimerId ? QJsonValue(*sample.eventTimerId) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("live_timer_id"),
             sample.liveTimerId ? QJsonValue(*sample.liveTimerId) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("timer_id_valid"),
             sample.timerEvent ? QJsonValue(sample.timerIdValid) : QJsonValue(QJsonValue::Null)}});
    }
    return result;
}

void runEventLoop(int milliseconds) {
    if (milliseconds == 0)
        return;
    QEventLoop loop;
    QTimer duration;
    duration.setTimerType(Qt::PreciseTimer);
    duration.setSingleShot(true);
    QObject::connect(&duration, &QTimer::timeout, &loop, &QEventLoop::quit);
    duration.start(milliseconds);
    loop.exec();
}

QJsonObject measureScheduler(BenchmarkApplication& application, qreal refreshRate,
                             int durationMilliseconds, int warmupMilliseconds, int idleMilliseconds,
                             Implementation implementation) {
    QElapsedTimer elapsed;
    ScreenshotPresentationFrameClock frameClock;
    frameClock.setRefreshRate(refreshRate, 0);
    const qint64 periodNs = frameClock.nextFrameNanoseconds();
    qint64 previousFrameNs = 0;
    qint64 earlyCallbacks = 0;
    qint64 skippedDeadlines = 0;
    QJsonArray samples;
    std::vector<double> latenessMicroseconds;
    std::vector<double> intervalsMilliseconds;
    std::vector<double> callbackWorkMicroseconds;
    const auto reserve = static_cast<std::size_t>(refreshRate * durationMilliseconds / 1000.0 + 8);
    latenessMicroseconds.reserve(reserve);
    intervalsMilliseconds.reserve(reserve);
    callbackWorkMicroseconds.reserve(reserve);

    QTimer pollingTimer;
    pollingTimer.setTimerType(Qt::PreciseTimer);
    std::unique_ptr<ScreenshotPresentationFrameScheduler> scheduler;
    std::unique_ptr<WakeupObserver> observer;
    std::function<void(qint64)> armDeadline;
    qint64 pollingDeadlineNs = 0;
    bool pollingArmed = false;

    const auto presentFrame = [&] {
        const qint64 startNs = elapsed.nsecsElapsed();
        const qint64 deadlineNs = frameClock.nextFrameNanoseconds();
        const qint64 intervalNs = startNs - previousFrameNs;
        previousFrameNs = startNs;
        auto& work = observer->currentWork();
        ++work.callbacks;
        if (startNs < deadlineNs)
            ++earlyCallbacks;
        frameClock.advancePast(elapsed.nsecsElapsed());
        const qint64 skipped =
            std::max<qint64>(0, (frameClock.nextFrameNanoseconds() - deadlineNs) / periodNs - 1);
        armDeadline(frameClock.nextFrameNanoseconds());
        const qint64 callbackNs = elapsed.nsecsElapsed() - startNs;
        work.callbackNanoseconds += callbackNs;
        if (observer->phase == Phase::Active) {
            skippedDeadlines += skipped;
            latenessMicroseconds.push_back(static_cast<double>(startNs - deadlineNs) / 1000.0);
            intervalsMilliseconds.push_back(static_cast<double>(intervalNs) / 1000000.0);
            callbackWorkMicroseconds.push_back(static_cast<double>(callbackNs) / 1000.0);
            samples.append(QJsonObject{{QStringLiteral("deadline_ns"), deadlineNs},
                                       {QStringLiteral("callback_ns"), startNs},
                                       {QStringLiteral("lateness_ns"), startNs - deadlineNs},
                                       {QStringLiteral("interval_ns"), intervalNs},
                                       {QStringLiteral("callback_work_ns"), callbackNs},
                                       {QStringLiteral("skipped_deadlines"), skipped}});
        }
    };

    if (implementation == Implementation::PollingQtTimer) {
        armDeadline = [&](qint64 deadlineNs) {
            pollingDeadlineNs = deadlineNs;
            pollingArmed = true;
            if (!pollingTimer.isActive())
                pollingTimer.start(1);
        };
        QObject::connect(&pollingTimer, &QTimer::timeout, &pollingTimer, [&] {
            if (!pollingArmed || elapsed.nsecsElapsed() < pollingDeadlineNs)
                return;
            pollingArmed = false;
            presentFrame();
            if (!pollingArmed)
                pollingTimer.stop();
        });
        observer = std::make_unique<WakeupObserver>(
            [&] { return &pollingTimer; }, [&] { return elapsed.nsecsElapsed(); },
            [&] { return frameClock.nextFrameNanoseconds(); }, [&] { return pollingArmed; });
    } else {
        scheduler = std::make_unique<ScreenshotPresentationFrameScheduler>(
            [&] { return elapsed.nsecsElapsed(); }, presentFrame,
            implementation == Implementation::QtDeadline
                ? ScreenshotPresentationFrameScheduler::Backend::QtTimer
                : ScreenshotPresentationFrameScheduler::Backend::Automatic);
        armDeadline = [&](qint64 deadlineNs) { scheduler->setDeadline(deadlineNs); };
        observer = std::make_unique<WakeupObserver>(
            [&] { return scheduler->wakeupObject(); }, [&] { return elapsed.nsecsElapsed(); },
            [&] { return frameClock.nextFrameNanoseconds(); }, [&] { return scheduler->active(); });
    }
    observer->reserveSamples(static_cast<std::size_t>(durationMilliseconds + warmupMilliseconds +
                                                      idleMilliseconds + kIdleSettleMilliseconds) *
                             2);
    application.observer = observer.get();
    elapsed.start();
    armDeadline(frameClock.nextFrameNanoseconds());
    runEventLoop(warmupMilliseconds);
    observer->phase = Phase::Active;
    const CpuSnapshot cpuStart = cpuSnapshot();
    const qint64 activeStartNs = elapsed.nsecsElapsed();
    runEventLoop(durationMilliseconds);
    const qint64 activeEndNs = elapsed.nsecsElapsed();
    const CpuSnapshot cpuEnd = cpuSnapshot();
    observer->trackBackend();
    const auto& activeWork = observer->work[static_cast<std::size_t>(Phase::Active)];
    const double durationMs = static_cast<double>(activeEndNs - activeStartNs) / 1000000.0;

    // Cancel the pending frame while retaining the same backend, as presentation does
    // when capture work becomes idle. A native activation may already be posted;
    // retain its immediate drain count separately from sustained idle activity.
    if (scheduler)
        scheduler->cancelDeadline();
    pollingArmed = false;
    pollingTimer.stop();
    observer->phase = Phase::IdleSettle;
    const qint64 idleSettleStartNs = elapsed.nsecsElapsed();
    runEventLoop(kIdleSettleMilliseconds);
    const double idleSettleDurationMs =
        static_cast<double>(elapsed.nsecsElapsed() - idleSettleStartNs) / 1000000.0;
    const auto& idleSettleWork = observer->work[static_cast<std::size_t>(Phase::IdleSettle)];
    observer->phase = Phase::Idle;
    const qint64 idleStartNs = elapsed.nsecsElapsed();
    runEventLoop(idleMilliseconds);
    const double idleDurationMs =
        static_cast<double>(elapsed.nsecsElapsed() - idleStartNs) / 1000000.0;
    const auto& idleWork = observer->work[static_cast<std::size_t>(Phase::Idle)];
    if (scheduler)
        scheduler->stop();
    application.observer = nullptr;

    const bool idleWithoutCallbacks = idleSettleWork.callbacks == 0 && idleWork.callbacks == 0;
    const bool sustainedIdleWithoutWakeups = idleWork.wakeups == 0;
    const bool idleWithoutWakeups =
        idleSettleWork.wakeups == 0 && sustainedIdleWithoutWakeups && idleWithoutCallbacks;
    const bool exactlyOneWakePerFrame = activeWork.wakeups == activeWork.callbacks;
    const bool oneDueWakePerFrame = activeWork.dueWakeups == activeWork.callbacks &&
                                    activeWork.deliveringWakeups == activeWork.callbacks;
    const qint64 explainedExtras = activeWork.gatedEarlyWakeups + activeWork.staleTimerWakeups +
                                   activeWork.inactiveWakeups + activeWork.retiredBackendWakeups;
    const bool allExtraWakeupsExplained =
        activeWork.wakeups == activeWork.callbacks + explainedExtras &&
        activeWork.unexplainedWakeups == 0 && activeWork.invalidDeliveries == 0;
    const double wakesPerFrame =
        activeWork.callbacks > 0
            ? static_cast<double>(activeWork.wakeups) / static_cast<double>(activeWork.callbacks)
            : 0.0;
    const bool qtDeadlineWakeBudgetCheck = implementation != Implementation::QtDeadline ||
                                           wakesPerFrame <= kQtDeadlineMaximumWakesPerFrame;
    const bool passed = earlyCallbacks == 0 && idleWithoutCallbacks &&
                        sustainedIdleWithoutWakeups && activeWork.callbacks > 0 &&
                        oneDueWakePerFrame && allExtraWakeupsExplained && qtDeadlineWakeBudgetCheck;
    return {
        {QStringLiteral("implementation"), implementationName(implementation)},
        {QStringLiteral("requested_hz"), refreshRate},
        {QStringLiteral("period_ns"), periodNs},
        {QStringLiteral("duration_ms"), durationMs},
        {QStringLiteral("delivered_hz"),
         static_cast<double>(activeWork.callbacks) * 1000.0 / durationMs},
        {QStringLiteral("backend_initial"), observer->backendHistory.first()},
        {QStringLiteral("backend_final"), observer->backendHistory.last()},
        {QStringLiteral("backend_changed"), observer->backendHistory.size() > 1},
        {QStringLiteral("backend_history"), observer->backendHistory},
        {QStringLiteral("active"), phaseWork(activeWork)},
        {QStringLiteral("warmup"),
         phaseWork(observer->work[static_cast<std::size_t>(Phase::Warmup)])},
        {QStringLiteral("idle_settle"), phaseWork(idleSettleWork)},
        {QStringLiteral("idle_settle_duration_ms"), idleSettleDurationMs},
        {QStringLiteral("idle"), phaseWork(idleWork)},
        {QStringLiteral("idle_duration_ms"), idleDurationMs},
        {QStringLiteral("idle_total_duration_ms"), idleSettleDurationMs + idleDurationMs},
        {QStringLiteral("idle_total_backend_wake_events"),
         idleSettleWork.wakeups + idleWork.wakeups},
        {QStringLiteral("deadline_lateness_us"), distribution(latenessMicroseconds)},
        {QStringLiteral("interval_ms"), distribution(intervalsMilliseconds)},
        {QStringLiteral("callback_work_us"), distribution(callbackWorkMicroseconds)},
        {QStringLiteral("process_cpu_ms"),
         cpuDifference(cpuStart.processMilliseconds, cpuEnd.processMilliseconds)},
        {QStringLiteral("gui_thread_cpu_ms"),
         cpuDifference(cpuStart.guiThreadMilliseconds, cpuEnd.guiThreadMilliseconds)},
        {QStringLiteral("skipped_deadlines"), skippedDeadlines},
        {QStringLiteral("extra_backend_wake_events"), activeWork.wakeups - activeWork.callbacks},
        {QStringLiteral("backend_wakes_per_delivered_frame"),
         activeWork.callbacks > 0 ? QJsonValue(wakesPerFrame) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("checks"),
         QJsonObject{
             {QStringLiteral("early_frame_callbacks"), earlyCallbacks},
             {QStringLiteral("idle_without_wakeups"), idleWithoutWakeups},
             {QStringLiteral("idle_without_delivered_callbacks"), idleWithoutCallbacks},
             {QStringLiteral("sustained_idle_without_backend_wakeups"),
              sustainedIdleWithoutWakeups},
             {QStringLiteral("one_backend_wake_per_delivered_frame"), exactlyOneWakePerFrame},
             {QStringLiteral("qt_deadline_one_wake_per_frame_passed"),
              implementation != Implementation::QtDeadline || exactlyOneWakePerFrame},
             {QStringLiteral("one_due_backend_wake_per_delivered_frame"), oneDueWakePerFrame},
             {QStringLiteral("all_extra_backend_wakes_explained"), allExtraWakeupsExplained},
             {QStringLiteral("qt_deadline_maximum_wakes_per_frame"),
              kQtDeadlineMaximumWakesPerFrame},
             {QStringLiteral("qt_deadline_wake_budget_passed"), qtDeadlineWakeBudgetCheck},
             {QStringLiteral("passed"), passed}}},
        {QStringLiteral("samples"), samples},
        {QStringLiteral("backend_event_samples"), wakeSamples(observer->samples)}};
}

int integerOption(const QCommandLineParser& parser, const QString& name, int minimum, int maximum) {
    bool valid = false;
    const int value = parser.value(name).toInt(&valid);
    if (!valid || value < minimum || value > maximum)
        throw std::runtime_error(name.toStdString() + " is outside its supported range");
    return value;
}
} // namespace

int main(int argc, char* argv[]) {
    BenchmarkApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("duration-ms"),
                      QStringLiteral("Measured duration per run, 100..60000 ms"),
                      QStringLiteral("milliseconds"), QStringLiteral("1000")});
    parser.addOption({QStringLiteral("warmup-ms"),
                      QStringLiteral("Warmup duration per run, 0..60000 ms"),
                      QStringLiteral("milliseconds"), QStringLiteral("250")});
    parser.addOption({QStringLiteral("idle-ms"),
                      QStringLiteral("Real event-loop idle observation, 25..60000 ms"),
                      QStringLiteral("milliseconds"), QStringLiteral("250")});
    parser.addOption({QStringLiteral("rounds"),
                      QStringLiteral("Measured runs per implementation, 1..100"),
                      QStringLiteral("count"), QStringLiteral("2")});
    parser.addOption({QStringLiteral("implementation"),
                      QStringLiteral("all, polling_qtimer, qt_deadline, or automatic_deadline"),
                      QStringLiteral("name"), QStringLiteral("all")});
    parser.addOption(
        {QStringLiteral("rates"),
         QStringLiteral("Comma-separated refresh rates, greater than 1 and at most 1000 Hz"),
         QStringLiteral("hertz"), QStringLiteral("60,120,144,240")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("Write JSON measurements to a file"),
                      QStringLiteral("path")});
    parser.process(application);

    try {
        const int durationMilliseconds =
            integerOption(parser, QStringLiteral("duration-ms"), 100, 60000);
        const int warmupMilliseconds = integerOption(parser, QStringLiteral("warmup-ms"), 0, 60000);
        const int idleMilliseconds = integerOption(parser, QStringLiteral("idle-ms"), 25, 60000);
        const int rounds = integerOption(parser, QStringLiteral("rounds"), 1, 100);
        std::vector<qreal> rates;
        for (const QString& supplied :
             parser.value(QStringLiteral("rates")).split(QLatin1Char(','))) {
            bool valid = false;
            const qreal rate = supplied.toDouble(&valid);
            if (!valid || !std::isfinite(rate) || rate <= 1.0 || rate > 1000.0)
                throw std::runtime_error(
                    "rates must contain refresh rates greater than 1 and at most 1000 Hz");
            rates.push_back(rate);
        }
        const QString selected = parser.value(QStringLiteral("implementation"));
        std::vector<Implementation> implementations;
        for (const auto implementation :
             {Implementation::PollingQtTimer, Implementation::QtDeadline,
              Implementation::AutomaticDeadline}) {
            if (selected == QStringLiteral("all") || selected == implementationName(implementation))
                implementations.push_back(implementation);
        }
        if (implementations.empty())
            throw std::runtime_error("unknown scheduler implementation");

        QJsonArray runs;
        bool checksPassed = true;
        int orderIndex = 0;
        for (const qreal rate : rates) {
            int runIndex = 0;
            for (int round = 1; round <= rounds; ++round) {
                auto order = implementations;
                if (round % 2 == 0)
                    std::reverse(order.begin(), order.end());
                for (const Implementation implementation : order) {
                    QJsonObject measurement =
                        measureScheduler(application, rate, durationMilliseconds,
                                         warmupMilliseconds, idleMilliseconds, implementation);
                    measurement.insert(QStringLiteral("round_index"), round);
                    measurement.insert(QStringLiteral("run_index"), ++runIndex);
                    measurement.insert(QStringLiteral("order_index"), ++orderIndex);
                    checksPassed = measurement.value(QStringLiteral("checks"))
                                       .toObject()
                                       .value(QStringLiteral("passed"))
                                       .toBool() &&
                                   checksPassed;
                    runs.append(measurement);
                }
            }
        }
        QJsonObject report{
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("benchmark"),
             QStringLiteral("screenshot_presentation_frame_scheduler")},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("operating_system"), QSysInfo::prettyProductName()},
            {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
            {QStringLiteral("executable"), QCoreApplication::applicationFilePath()},
#ifdef NDEBUG
            {QStringLiteral("configuration"), QStringLiteral("Release")},
#else
            {QStringLiteral("configuration"), QStringLiteral("Debug")},
#endif
            {QStringLiteral("requested_run_duration_ms"), durationMilliseconds},
            {QStringLiteral("requested_warmup_ms"), warmupMilliseconds},
            {QStringLiteral("requested_idle_ms"), idleMilliseconds},
            {QStringLiteral("requested_idle_settle_ms"), kIdleSettleMilliseconds},
            {QStringLiteral("rounds_per_implementation"), rounds},
            {QStringLiteral("run_order"),
             QStringLiteral(
                 "Forward then reverse implementation order for each two rounds and rate")},
            {QStringLiteral("deadline_policy"),
             QStringLiteral("Identical absolute nanosecond FrameClock deadlines; advance past "
                            "expired periods before rearming, then collect callback samples")},
            {QStringLiteral("measurement"),
             QStringLiteral("All implementations time the same frame bookkeeping and rearm "
                            "callback. Dispatch work spans complete backend QObject event "
                            "delivery, including early polling wakes and backend arm bookkeeping. "
                            "Observer diagnostics and raw backend sample collection are outside "
                            "dispatch timing but included in process/GUI-thread CPU. Collecting a "
                            "record for every backend event adds instrumentation overhead, "
                            "particularly to polling; these CPU readings are not uninstrumented "
                            "production cost. "
                            "Frame samples and work collection add identical instrumentation to "
                            "delivered callbacks. No widgets or rendering are involved.")},
            {QStringLiteral("backend_event_sampling"),
             QStringLiteral(
                 "Record entry/exit monotonic times, current FrameClock deadline, "
                 "and event/live timer IDs before QObject dispatch. Timer ID validity "
                 "is checked before single-shot timers stop themselves. An early-at-entry "
                 "event with a callback is counted as due during dispatch; a callback-free "
                 "early event is a gated retry. Native events have no timer ID. All "
                 "classifications, including stale/inactive events, are retained.")},
            {QStringLiteral("cpu_precision"),
             QStringLiteral(
                 "Dispatch/callback work is elapsed active-call time, including possible "
                 "preemption, not CPU time. Process and GUI-thread CPU cover the entire measured "
                 "event-loop run. Windows FILETIME is expressed in 100 ns units but accounting can "
                 "advance in scheduler-sized quanta; zero/small differences do not establish zero "
                 "overhead. Non-Windows process clock precision is platform-dependent; GUI-thread "
                 "CPU is unavailable.")},
            {QStringLiteral("checks_policy"),
             QStringLiteral(
                 "Require zero early frame callbacks and at least one delivered frame. After "
                 "cancellation, record a 10 ms event-loop settle/drain separately; require zero "
                 "delivered callbacks during both settle and sustained idle, and zero backend "
                 "events during sustained idle. Retain all settle wake counts. Every "
                 "implementation "
                 "requires exactly one due/delivering backend event per frame and an explanation "
                 "for "
                 "every extra event. Literal one-wake-per-frame checks remain diagnostic and can "
                 "be false when early notifications are gated safely. Forced Qt deadline mode has "
                 "an explicit non-polling budget of at most 2 total backend events per frame; "
                 "gated early retries and stale events remain visible in raw counts/samples.")},
            {QStringLiteral("checks_passed"), checksPassed},
            {QStringLiteral("runs"), runs}};
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (parser.isSet(QStringLiteral("output"))) {
            QFile file(parser.value(QStringLiteral("output")));
            if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size())
                throw std::runtime_error("could not write scheduler benchmark results");
        }
        std::cout << json.constData();
        if (!checksPassed)
            std::cerr << "Scheduler benchmark invariant checks failed; inspect the saved counts "
                         "and samples.\n";
        return checksPassed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
