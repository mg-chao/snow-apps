#include "screen_recording_window_startup_performance_benchmark.h"

#include "recording_effect_test_source.h"
#include "../src/presentation/recording/screenrecordingperfinstrumentation.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#if !defined(SNOW_SHOT_RECORDING_PERF_INSTRUMENTATION)
#error "The recording window startup benchmark requires SNOW_SHOT_RECORDING_PERF_INSTRUMENTATION."
#endif

namespace {
namespace recording_perf = snow_shot::presentation::recording_perf;

void require(bool success, const char* error) {
    if (!success) {
        throw std::runtime_error(error);
    }
}

void processFor(int milliseconds) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(milliseconds);
    loop.exec();
}

// Pumps until predicate() holds or the deadline expires; returns the predicate.
template <typename Predicate> bool pumpUntil(Predicate predicate, int timeoutMilliseconds) {
    QElapsedTimer deadline;
    deadline.start();
    while (!predicate()) {
        if (deadline.elapsed() >= timeoutMilliseconds) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 20);
    }
    return true;
}

double percentile(std::vector<double> values, double fraction) {
    require(!values.empty(), "benchmark collected no samples");
    std::sort(values.begin(), values.end());
    return values.at(static_cast<size_t>((values.size() - 1) * fraction));
}

struct Statistics {
    double p50 = 0;
    double p95 = 0;
    double max = 0;
};

Statistics summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    Statistics result;
    result.p50 = percentile(values, 0.5);
    result.p95 = percentile(values, 0.95);
    result.max = values.back();
    return result;
}

class SampleSink final : public recording_perf::Sink {
  public:
    void reset() {
        mMilestonesMs.clear();
        mSpansMs.clear();
        mCounters.clear();
        mFinished = false;
    }
    void recordScope(const char* name, qint64 nanoseconds) override {
        mSpansMs[QString::fromLatin1(name)] += nanoseconds / 1000000.0;
    }
    void recordMilestone(const char* name, qint64 nanoseconds) override {
        mMilestonesMs[QString::fromLatin1(name)] = nanoseconds / 1000000.0;
    }
    void recordCounter(const char* name, qint64 value) override {
        mCounters[QString::fromLatin1(name)] += value;
    }
    void finish(bool success) override {
        mFinished = success;
    }

    bool hasMilestone(const QString& name) const {
        return mMilestonesMs.contains(name);
    }
    double milestone(const QString& name) const {
        return mMilestonesMs.value(name);
    }
    const QMap<QString, double>& milestones() const {
        return mMilestonesMs;
    }
    const QMap<QString, double>& spans() const {
        return mSpansMs;
    }
    const QMap<QString, double>& counters() const {
        return mCounters;
    }
    bool finished() const {
        return mFinished;
    }

  private:
    QMap<QString, double> mMilestonesMs;
    QMap<QString, double> mSpansMs;
    QMap<QString, double> mCounters;
    bool mFinished = false;
};

// Application-level event filter: observes Paint delivery to the recording
// windows, which the toolbar covers without a paintEvent override.
class PaintObserver final : public QObject {
  public:
    explicit PaintObserver(QApplication& app) : m_app(app) {
        m_app.installEventFilter(this);
    }
    ~PaintObserver() {
        m_app.removeEventFilter(this);
    }
    PaintObserver(const PaintObserver&) = delete;
    PaintObserver& operator=(const PaintObserver&) = delete;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            if (watched == m_area && m_areaPaintMs < 0) {
                m_areaPaintMs = elapsedMs();
            }
            if (watched == m_toolbar && m_toolbarPaintMs < 0) {
                m_toolbarPaintMs = elapsedMs();
            }
            if (watched == m_canvas) {
                if (m_canvasPaintTimes.size() < kMaxTrackedCanvasPaints) {
                    m_canvasPaintTimes.push_back(elapsedMs());
                }
                ++m_canvasPaints;
            }
            ++m_windowPaints;
        }
        return false;
    }

    void beginSample() {
        m_clock.start();
        m_areaPaintMs = -1;
        m_toolbarPaintMs = -1;
        m_canvasPaintTimes.clear();
        m_canvasPaints = 0;
        m_windowPaints = 0;
        m_idleBase = 0;
    }
    void watch(ScreenRecordingAreaWindow* area, ScreenRecordingToolbarWindow* toolbar) {
        m_area = area;
        m_toolbar = toolbar;
        m_canvas = area != nullptr ? area->canvas() : nullptr;
    }
    void startIdleWindow() {
        m_idleBase = m_windowPaints;
    }

    double areaPaintMs() const {
        return m_areaPaintMs;
    }
    double toolbarPaintMs() const {
        return m_toolbarPaintMs;
    }
    int canvasPaints() const {
        return m_canvasPaints;
    }
    double canvasPaintMs(int zeroBasedIndex) const {
        return zeroBasedIndex >= 0 && zeroBasedIndex < m_canvasPaintTimes.size()
                   ? m_canvasPaintTimes.at(zeroBasedIndex)
                   : -1;
    }
    int idlePaints() const {
        return m_windowPaints - m_idleBase;
    }

  private:
    double elapsedMs() const {
        return m_clock.isValid() ? m_clock.elapsed() : 0;
    }
    static constexpr int kMaxTrackedCanvasPaints = 64;
    QApplication& m_app;
    QElapsedTimer m_clock;
    QPointer<QObject> m_area;
    QPointer<QObject> m_toolbar;
    QPointer<SnowCanvasWidget> m_canvas;
    double m_areaPaintMs = -1;
    double m_toolbarPaintMs = -1;
    std::vector<double> m_canvasPaintTimes;
    int m_canvasPaints = 0;
    int m_windowPaints = 0;
    int m_idleBase = 0;
};

struct SampleResult {
    QMap<QString, double> metrics;
    QMap<QString, double> counters;
};

// Consecutive open() milestones whose deltas form the startup phase metrics.
const std::vector<std::pair<QString, QString>>& phaseBoundaries() {
    static const std::vector<std::pair<QString, QString>> phases{
        {QStringLiteral("phase.construct_ui"), QStringLiteral("open.ui_session_constructed")},
        {QStringLiteral("phase.create_preview"), QStringLiteral("open.preview_created")},
        {QStringLiteral("phase.apply_region"), QStringLiteral("open.region_applied")},
        {QStringLiteral("phase.connect_toolbar"), QStringLiteral("open.toolbar_connected")},
        {QStringLiteral("phase.create_shortcuts"), QStringLiteral("open.shortcuts_created")},
        {QStringLiteral("phase.sync_ui"), QStringLiteral("open.ui_synced")},
        {QStringLiteral("phase.show_windows"), QStringLiteral("open.show_returned")},
    };
    return phases;
}

SampleResult collectSample(PaintObserver& observer, SampleSink& sink, const QString& scenario,
                           const QRect& region, double& endToEndMs) {
    std::vector<std::shared_ptr<RecordingEffectTestState>> effectStates;
    sink.reset();
    observer.beginSample();

    QElapsedTimer wall;
    recording_perf::installSink(&sink);
    int canvasPaintsAtFrame = -1;
    {
        wall.start();
        recording_perf::beginSample(scenario.toLatin1().constData(), region.width(),
                                    region.height());
        ScreenRecordingController controller([&effectStates]() {
            auto state = std::make_shared<RecordingEffectTestState>();
            effectStates.push_back(state);
            return std::make_unique<RecordingEffectTestSource>(state);
        });
        controller.open(region);

        ScreenRecordingAreaWindow* area = nullptr;
        ScreenRecordingToolbarWindow* toolbar = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<ScreenRecordingAreaWindow*>(widget)) {
                area = candidate;
            } else if (auto* candidateToolbar =
                           qobject_cast<ScreenRecordingToolbarWindow*>(widget)) {
                toolbar = candidateToolbar;
            }
        }
        require(area != nullptr && toolbar != nullptr, "recording windows must exist after open");
        observer.watch(area, toolbar);

        require(pumpUntil(
                    [&observer]() {
                        return observer.areaPaintMs() >= 0 && observer.toolbarPaintMs() >= 0;
                    },
                    5000),
                "recording windows must paint within the sample deadline");
        const int canvasPaintsBeforeFrame = observer.canvasPaints();
        canvasPaintsAtFrame = canvasPaintsBeforeFrame;
        require(pumpUntil(
                    [&sink, &observer, canvasPaintsBeforeFrame]() {
                        return sink.hasMilestone(QStringLiteral("preview.first_frame_received")) &&
                               observer.canvasPaints() > canvasPaintsBeforeFrame;
                    },
                    5000),
                "motion preview must deliver its first frame and repaint the canvas");

        observer.startIdleWindow();
        processFor(50);

        recording_perf::finish(true);
        endToEndMs = wall.elapsed();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    static const QStringList requiredMilestones{
        QStringLiteral("open.before_ui_session"), QStringLiteral("open.ui_session_constructed"),
        QStringLiteral("open.preview_created"),   QStringLiteral("open.region_applied"),
        QStringLiteral("open.toolbar_connected"), QStringLiteral("open.shortcuts_created"),
        QStringLiteral("open.ui_synced"),         QStringLiteral("open.show_returned"),
        QStringLiteral("area.first_paint_end"),   QStringLiteral("preview.first_frame_received"),
    };
    for (const QString& name : requiredMilestones) {
        require(sink.hasMilestone(name), "startup milestone missing from the sample");
    }

    SampleResult result;
    for (const QString& name : sink.milestones().keys()) {
        result.metrics.insert(name, sink.milestones().value(name));
    }
    for (const QString& name : sink.spans().keys()) {
        result.metrics.insert(name, sink.spans().value(name));
    }
    double previous = 0;
    for (const auto& [phase, boundary] : phaseBoundaries()) {
        require(sink.hasMilestone(boundary), "startup phase boundary missing from the sample");
        result.metrics.insert(phase, sink.milestone(boundary) - previous);
        previous = sink.milestone(boundary);
    }
    result.metrics.insert(QStringLiteral("paint.area_window"), observer.areaPaintMs());
    result.metrics.insert(QStringLiteral("paint.toolbar_window"), observer.toolbarPaintMs());
    result.metrics.insert(QStringLiteral("paint.canvas_after_preview_frame"),
                          observer.canvasPaintMs(canvasPaintsAtFrame));
    for (const QString& name : sink.counters().keys()) {
        result.counters.insert(name, sink.counters().value(name));
    }
    result.counters.insert(QStringLiteral("count.idle_paints"), observer.idlePaints());
    return result;
}

QJsonObject statisticsJson(const Statistics& statistics) {
    return QJsonObject{{QStringLiteral("p50_ms"), statistics.p50},
                       {QStringLiteral("p95_ms"), statistics.p95},
                       {QStringLiteral("max_ms"), statistics.max}};
}

void printMetricGroup(const char* title, const QMap<QString, std::vector<double>>& values,
                      const std::vector<QString>& names) {
    std::cout << "  " << title << "(p50/p95)";
    for (const QString& name : names) {
        const Statistics statistics = summarize(values.value(name));
        std::cout << ' ' << name.toStdString() << '=' << statistics.p50 << '/' << statistics.p95;
    }
    std::cout << '\n';
}

std::vector<QString> namesWithPrefix(const QMap<QString, std::vector<double>>& values,
                                     const std::vector<QString>& prefixes) {
    std::vector<QString> names;
    for (const QString& name : values.keys()) {
        for (const QString& prefix : prefixes) {
            if (name.startsWith(prefix)) {
                names.push_back(name);
                break;
            }
        }
    }
    return names;
}
} // namespace

int runRecordingWindowStartupPerformanceBenchmark(QApplication& app) {
    try {
        int warmups = 3;
        int samples = 15;
        QString jsonPath;
        const QStringList arguments = app.arguments();
        for (int i = 1; i < arguments.size(); ++i) {
            if (arguments.at(i) == QStringLiteral("--warmups") && i + 1 < arguments.size()) {
                warmups = qMax(0, arguments.at(++i).toInt());
            } else if (arguments.at(i) == QStringLiteral("--samples") && i + 1 < arguments.size()) {
                samples = qMax(1, arguments.at(++i).toInt());
            } else if (arguments.at(i) == QStringLiteral("--json") && i + 1 < arguments.size()) {
                jsonPath = arguments.at(++i);
            }
        }

        // The motion preview stays disabled with the stock fully transparent
        // trail; a visible trail keeps the preview startup phase measurable.
        require(
            snow_shot::storage::RecordingSettings().setMouseTrailColor(QColor(255, 40, 60, 180)),
            "benchmark must enable the mouse trail");

        struct Scenario {
            const char* name;
            QSize size;
        };
        const std::vector<Scenario> scenarios{
            {"1280x720", QSize(1280, 720)},
            {"1920x1080", QSize(1920, 1080)},
            {"3840x2160", QSize(3840, 2160)},
        };

        PaintObserver observer(app);
        SampleSink sink;
        QJsonArray scenarioReports;
        for (const Scenario& scenario : scenarios) {
            const QRect region(16, 16, scenario.size.width(), scenario.size.height());
            double endToEndMs = 0;
            for (int i = 0; i < warmups; ++i) {
                collectSample(observer, sink,
                              QString::fromLatin1(scenario.name) + QStringLiteral(".warmup"),
                              region, endToEndMs);
            }
            std::vector<SampleResult> results;
            std::vector<double> endToEnd;
            for (int i = 0; i < samples; ++i) {
                SampleResult result = collectSample(
                    observer, sink, QString::fromLatin1(scenario.name), region, endToEndMs);
                require(result.counters.value(QStringLiteral("count.idle_paints")) <= 20,
                        "recording windows must not repaint continuously while idle");
                results.push_back(std::move(result));
                endToEnd.push_back(endToEndMs);
            }

            QMap<QString, std::vector<double>> metricValues;
            QMap<QString, std::vector<double>> counterValues;
            QJsonArray sampleReports;
            for (const SampleResult& result : results) {
                for (const QString& name : result.metrics.keys()) {
                    metricValues[name].push_back(result.metrics.value(name));
                }
                for (const QString& name : result.counters.keys()) {
                    counterValues[name].push_back(result.counters.value(name));
                }
                QJsonObject sample;
                for (const QString& name : result.metrics.keys()) {
                    sample.insert(name, result.metrics.value(name));
                }
                sampleReports.append(sample);
            }
            metricValues[QStringLiteral("sample.end_to_end")] = endToEnd;

            std::cout << "scenario=" << scenario.name << " warmups=" << warmups
                      << " samples=" << samples << '\n';
            printMetricGroup("phases_ms", metricValues,
                             namesWithPrefix(metricValues, {QStringLiteral("phase.")}));
            printMetricGroup(
                "milestones_ms", metricValues,
                namesWithPrefix(metricValues,
                                {QStringLiteral("open."), QStringLiteral("area."),
                                 QStringLiteral("preview.first"), QStringLiteral("toolbar.show")}));
            printMetricGroup("paints_ms", metricValues,
                             namesWithPrefix(metricValues, {QStringLiteral("paint."),
                                                            QStringLiteral("sample.")}));
            printMetricGroup(
                "spans_ms", metricValues,
                namesWithPrefix(metricValues,
                                {QStringLiteral("toolbar.place"), QStringLiteral("palette."),
                                 QStringLiteral("preview.synchronize")}));
            std::cout << "  counters(median/max)";
            for (const QString& name : counterValues.keys()) {
                const Statistics statistics = summarize(counterValues.value(name));
                std::cout << ' ' << name.toStdString() << '=' << statistics.p50 << '/'
                          << statistics.max;
            }
            std::cout << '\n';
            std::cout.flush();

            QJsonObject metricsJson;
            for (const QString& name : metricValues.keys()) {
                metricsJson.insert(name, statisticsJson(summarize(metricValues.value(name))));
            }
            QJsonObject countersJson;
            for (const QString& name : counterValues.keys()) {
                countersJson.insert(name, statisticsJson(summarize(counterValues.value(name))));
            }
            scenarioReports.append(QJsonObject{
                {QStringLiteral("scenario"), QString::fromLatin1(scenario.name)},
                {QStringLiteral("warmups"), warmups},
                {QStringLiteral("samples"), samples},
                {QStringLiteral("metrics"), metricsJson},
                {QStringLiteral("counters"), countersJson},
                {QStringLiteral("raw_samples"), sampleReports},
            });
        }

        if (!jsonPath.isEmpty()) {
            const QJsonObject report{
                {QStringLiteral("schema"),
                 QStringLiteral("snow-shot.recording-window-startup-performance.v1")},
                {QStringLiteral("created_utc"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {QStringLiteral("qt_platform"), QGuiApplication::platformName()},
                {QStringLiteral("scenarios"), scenarioReports},
            };
            QFile file(jsonPath);
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                    "benchmark report must be writable");
            file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
            file.close();
            std::cout << "wrote json report to " << jsonPath.toStdString() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
