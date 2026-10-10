#include "snow_shot/presentation/screenshotpresentationframeclock.h"
#include "snow_shot/presentation/screenshotpresentationframescheduler.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <vector>

namespace {
class WakeupObserver final : public QObject {
  public:
    WakeupObserver(QObject* fixedBackend, ScreenshotPresentationFrameScheduler* scheduler)
        : m_fixedBackend(fixedBackend), m_scheduler(scheduler) {
        trackBackend();
        QCoreApplication::instance()->installEventFilter(this);
    }

    ~WakeupObserver() override {
        QCoreApplication::instance()->removeEventFilter(this);
    }

    void trackBackend() {
        QObject* backend = m_scheduler ? m_scheduler->wakeupObject() : m_fixedBackend;
        if (m_backends.empty() || m_backends.back() != backend) {
            m_backends.push_back(backend);
            backendHistory.append(QString::fromLatin1(backend->metaObject()->className()));
        }
    }

    int wakeups = 0;
    int timerWakeups = 0;
    int nativeWakeups = 0;
    QJsonArray backendHistory;

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Timer || event->type() == QEvent::WinEventAct) {
            // A native arm failure can switch the helper to QTimer during a callback.
            // Observe its current backend as well as already queued events from the old one.
            trackBackend();
            if (std::find(m_backends.begin(), m_backends.end(), object) != m_backends.end()) {
                ++wakeups;
                if (event->type() == QEvent::Timer)
                    ++timerWakeups;
                else
                    ++nativeWakeups;
            }
        }
        return false;
    }

  private:
    QObject* m_fixedBackend;
    ScreenshotPresentationFrameScheduler* m_scheduler;
    std::vector<QObject*> m_backends;
};

std::optional<double> processCpuMilliseconds() {
#ifdef Q_OS_WIN
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return std::nullopt;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
    const std::clock_t value = std::clock();
    if (value == std::clock_t(-1)) {
        return std::nullopt;
    }
    return static_cast<double>(value) * 1000.0 / CLOCKS_PER_SEC;
#endif
}

QJsonObject measureScheduler(qreal refreshRate, int durationMilliseconds, bool absoluteDeadlines) {
    QElapsedTimer elapsed;
    QEventLoop loop;
    QTimer durationTimer;
    durationTimer.setTimerType(Qt::PreciseTimer);
    durationTimer.setSingleShot(true);
    QObject::connect(&durationTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    std::vector<qint64> frameTimes;
    frameTimes.reserve(static_cast<std::size_t>(refreshRate * durationMilliseconds / 1000.0 + 8));
    qint64 callbackNanoseconds = 0;
    ScreenshotPresentationFrameClock frameClock;
    frameClock.setRefreshRate(refreshRate, 0);

    QTimer integerTimer;
    integerTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&integerTimer, &QTimer::timeout, &integerTimer, [&] {
        const qint64 start = elapsed.nsecsElapsed();
        frameTimes.push_back(start);
        callbackNanoseconds += elapsed.nsecsElapsed() - start;
    });

    ScreenshotPresentationFrameScheduler* activeScheduler = nullptr;
    ScreenshotPresentationFrameScheduler scheduler(
        [&] { return elapsed.nsecsElapsed(); },
        [&] {
            const qint64 start = elapsed.nsecsElapsed();
            frameTimes.push_back(start);
            frameClock.advancePast(elapsed.nsecsElapsed());
            activeScheduler->setDeadline(frameClock.nextFrameNanoseconds());
            callbackNanoseconds += elapsed.nsecsElapsed() - start;
        });
    activeScheduler = &scheduler;

    const auto cpuStart = processCpuMilliseconds();
    elapsed.start();
    if (absoluteDeadlines) {
        scheduler.setDeadline(frameClock.nextFrameNanoseconds());
    } else {
        integerTimer.start(static_cast<int>(std::ceil(1000.0 / refreshRate)));
    }
    WakeupObserver observer(&integerTimer, absoluteDeadlines ? &scheduler : nullptr);
    durationTimer.start(durationMilliseconds);
    loop.exec();
    const qint64 elapsedNanoseconds = elapsed.nsecsElapsed();
    observer.trackBackend();
    scheduler.stop();
    integerTimer.stop();
    const auto cpuEnd = processCpuMilliseconds();

    std::vector<double> intervalsMilliseconds;
    intervalsMilliseconds.reserve(frameTimes.size());
    qint64 previousFrame = 0;
    for (const qint64 frameTime : frameTimes) {
        intervalsMilliseconds.push_back(static_cast<double>(frameTime - previousFrame) / 1000000.0);
        previousFrame = frameTime;
    }
    double intervalMean = 0;
    for (const double interval : intervalsMilliseconds) {
        intervalMean += interval;
    }
    if (!intervalsMilliseconds.empty()) {
        intervalMean /= static_cast<double>(intervalsMilliseconds.size());
    }
    std::sort(intervalsMilliseconds.begin(), intervalsMilliseconds.end());
    const auto percentile = [&intervalsMilliseconds](double fraction) {
        if (intervalsMilliseconds.empty()) {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(intervalsMilliseconds.size())) - 1.0);
        return intervalsMilliseconds[index];
    };
    const double frames = static_cast<double>(frameTimes.size());
    QJsonObject result;
    result.insert(QStringLiteral("implementation"),
                  absoluteDeadlines ? QStringLiteral("absolute_deadline_scheduler")
                                    : QStringLiteral("retained_integer_qtimer"));
    result.insert(QStringLiteral("requested_hz"), refreshRate);
    result.insert(QStringLiteral("duration_ms"),
                  static_cast<double>(elapsedNanoseconds) / 1000000.0);
    result.insert(QStringLiteral("delivered_frames"), frames);
    result.insert(QStringLiteral("delivered_hz"),
                  frames * 1000000000.0 / static_cast<double>(elapsedNanoseconds));
    result.insert(QStringLiteral("interval_mean_ms"), intervalMean);
    result.insert(QStringLiteral("interval_p95_ms"), percentile(0.95));
    result.insert(QStringLiteral("interval_min_ms"),
                  intervalsMilliseconds.empty() ? 0.0 : intervalsMilliseconds.front());
    result.insert(QStringLiteral("interval_max_ms"),
                  intervalsMilliseconds.empty() ? 0.0 : intervalsMilliseconds.back());
    result.insert(QStringLiteral("backend"), observer.backendHistory.last());
    result.insert(QStringLiteral("backend_initial"), observer.backendHistory.first());
    result.insert(QStringLiteral("backend_final"), observer.backendHistory.last());
    result.insert(QStringLiteral("backend_changed"), observer.backendHistory.size() > 1);
    result.insert(QStringLiteral("backend_history"), observer.backendHistory);
    result.insert(QStringLiteral("backend_wake_events"), observer.wakeups);
    result.insert(QStringLiteral("timer_wake_events"), observer.timerWakeups);
    result.insert(QStringLiteral("native_wake_events"), observer.nativeWakeups);
    result.insert(QStringLiteral("callback_total_us"),
                  static_cast<double>(callbackNanoseconds) / 1000.0);
    result.insert(QStringLiteral("callback_mean_us"),
                  frames > 0 ? static_cast<double>(callbackNanoseconds) / frames / 1000.0 : 0.0);
    result.insert(QStringLiteral("process_cpu_ms"),
                  cpuStart && cpuEnd ? QJsonValue(*cpuEnd - *cpuStart) : QJsonValue());
    return result;
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("duration-ms"), QStringLiteral("Duration of each timer run"),
                      QStringLiteral("milliseconds"), QStringLiteral("1000")});
    parser.addOption({QStringLiteral("rounds"), QStringLiteral("Measured runs per implementation"),
                      QStringLiteral("count"), QStringLiteral("2")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("Write JSON measurements to a file"),
                      QStringLiteral("path")});
    parser.process(application);
    bool durationValid = false;
    bool roundsValid = false;
    const int durationMilliseconds =
        parser.value(QStringLiteral("duration-ms")).toInt(&durationValid);
    const int rounds = parser.value(QStringLiteral("rounds")).toInt(&roundsValid);
    if (!durationValid || durationMilliseconds < 100 || !roundsValid || rounds < 1) {
        std::cerr << "duration-ms must be at least 100 and rounds at least 1\n";
        return EXIT_FAILURE;
    }

    QJsonArray runs;
    int orderIndex = 0;
    for (const qreal rate : {60.0, 120.0, 144.0, 240.0}) {
        int runIndex = 0;
        for (int round = 1; round <= rounds; ++round) {
            // Alternating the order of each pair gives ABBA for the default two rounds.
            for (const bool absoluteDeadlines : {round % 2 == 0, round % 2 != 0}) {
                QJsonObject measurement =
                    measureScheduler(rate, durationMilliseconds, absoluteDeadlines);
                measurement.insert(QStringLiteral("round_index"), round);
                measurement.insert(QStringLiteral("run_index"), ++runIndex);
                measurement.insert(QStringLiteral("order_index"), ++orderIndex);
                runs.append(measurement);
            }
        }
    }
    QJsonObject result;
    result.insert(QStringLiteral("schema_version"), 1);
    result.insert(QStringLiteral("requested_run_duration_ms"), durationMilliseconds);
    result.insert(QStringLiteral("rounds_per_implementation"), rounds);
    result.insert(
        QStringLiteral("run_order"),
        QStringLiteral("ABBA for each two rounds: baseline, candidate, candidate, baseline"));
    result.insert(
        QStringLiteral("notes"),
        QStringLiteral("Intervals include the first deadline from run start. Backend wake "
                       "events count Qt timer or WinEventAct events; callback timings "
                       "exclude Qt event dispatch. CPU includes the complete process. "
                       "The observer follows fallback backend changes and counts queued events "
                       "from previous backends. No widgets or rendering are involved."));
    result.insert(QStringLiteral("runs"), runs);
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (parser.isSet(QStringLiteral("output"))) {
        QFile file(parser.value(QStringLiteral("output")));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) {
            std::cerr << "could not write scheduler benchmark results\n";
            return EXIT_FAILURE;
        }
    }
    std::cout << json.constData();
    return EXIT_SUCCESS;
}
