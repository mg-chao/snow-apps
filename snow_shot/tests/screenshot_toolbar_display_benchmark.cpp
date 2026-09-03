// Screenshot toolbar display performance benchmark.
//
// Measures two user-visible toolbar display costs with phase-level detail:
//
//   first_frame   Toolbar creation -> first displayed frame. A fresh
//                 ScreenshotToolbarWindow is constructed per sample and shown
//                 through the same prepared-show sequence the overlay host
//                 uses (opacity-concealed show -> synchronous repaint -> sent
//                 UpdateRequest -> RedrawWindow(RDW_INVALIDATE|RDW_UPDATENOW|
//                 RDW_ALLCHILDREN) -> opacity restore -> raise -> DwmFlush).
//
//   tools.cycle   One sweep activates every drawing-related tool in sequence
//                 after a Move reset. Each switch records the sub-toolbar
//                 display cost: the setActiveTool call itself, its internal
//                 phases via the toolbar perf scopes (evict, hydrate, rebind,
//                 layout), the first style-panel paint, the event-loop settle,
//                 and the compositor flush. Consecutive same-family tools
//                 (shape/line/free-draw, rect/pen-highlight, rect/pen-filter)
//                 exercise the warm rebind path; family crossings exercise the
//                 cold evict+hydrate path, exactly like real tool cycling.
//
//   tools.cold    Isolated first activation per tool (Move reset between
//                 samples) to separate family hydration cost from sweep-order
//                 effects.
//
// The benchmark links instrumented copies of the toolbar sources
// (SNOW_SHOT_TOOLBAR_PERF_INSTRUMENTATION=1) and is only built when
// SNOW_SHOT_BUILD_BENCHMARKS=ON; production builds compile every scope below
// to a no-op. It is intentionally not registered with CTest: it creates real
// always-on-top native windows and measures real DWM presentation, so it
// belongs to explicit benchmarking sessions only.
//
// Compositor timings use DwmFlush(), which is quantized to the display
// refresh interval; treat compositor numbers as upper bounds. The report
// includes an "idle_floor" calibration (no-op settle + DwmFlush) so consumers
// can subtract the harness floor from settle/compositor/total_to_display and
// a "screen.refresh_rate_hz" field to interpret the quantization. Per-tool
// JSON entries carry an "id", per-switch "counters" (layout commits, host
// resizes, ...), and "raw_samples"; hydration is attributed two levels deep
// (palette.create_style_family.<family> and style.add_<control>_editor), and
// the first-frame geometry pass is split into
// window.refresh_geometry.<sync_scale|palette_geometry|size_hint|
// apply_geometry|stable_physical_size> sub-scopes.

#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "../src/presentation/tools/screenshottoolbarperfinstrumentation.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QToolTip>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <dwmapi.h>
#include <qt_windows.h>
#endif

namespace toolbar_perf = snow_shot::presentation::toolbar_perf;

namespace {

constexpr int kReportSchemaVersion = 2;
constexpr const char* kOwnerObjectName = "toolbarDisplayOwner";

qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double nsToMs(qint64 nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000000.0;
}

struct Stats {
    int count = 0;
    double p50 = std::numeric_limits<double>::quiet_NaN();
    double p90 = std::numeric_limits<double>::quiet_NaN();
    double p95 = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    double mean = std::numeric_limits<double>::quiet_NaN();
};

Stats computeStats(QVector<double> values) {
    Stats stats;
    stats.count = static_cast<int>(values.size());
    if (values.isEmpty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double p) {
        const double position = p / 100.0 * static_cast<double>(values.size() - 1);
        return values[static_cast<int>(std::min(static_cast<double>(values.size() - 1),
                                                std::ceil(position)))];
    };
    stats.p50 = percentile(50.0);
    stats.p90 = percentile(90.0);
    stats.p95 = percentile(95.0);
    stats.max = values.last();
    stats.mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    return stats;
}

QString msCell(double milliseconds, int width) {
    if (std::isnan(milliseconds)) {
        return QString::fromLatin1("-").rightJustified(width);
    }
    return QString::number(milliseconds, 'f', 2).rightJustified(width);
}

QString intCell(qint64 value, int width) {
    return QString::number(value).rightJustified(width);
}

struct ToolInfo {
    ScreenshotToolPalette::Tool tool;
    const char* id;
    const char* styleObjectName; // nullptr when the tool has no style toolbar.
    bool hasStyleToolbar;
};

// Drawing-related tools in the order a user cycles through them. Consecutive
// same-family entries (shape/line/free-draw, rect/pen-highlight, rect/
// pen-filter) intentionally measure the warm rebind path inside a sweep.
const ToolInfo kDrawingTools[] = {
    {ScreenshotToolPalette::Tool::Shape, "shape", "screenshotRectangleStyleControls", true},
    {ScreenshotToolPalette::Tool::Arrow, "arrow", "screenshotArrowStyleControls", true},
    {ScreenshotToolPalette::Tool::Line, "line", "screenshotLineStyleControls", true},
    {ScreenshotToolPalette::Tool::FreeDraw, "free-draw", "screenshotFreeDrawStyleControls",
     true},
    {ScreenshotToolPalette::Tool::RectangleHighlight, "rect-highlight",
     "screenshotHighlightStyleControls", true},
    {ScreenshotToolPalette::Tool::PenHighlight, "pen-highlight",
     "screenshotPenHighlightStyleControls", true},
    {ScreenshotToolPalette::Tool::Spotlight, "spotlight", "screenshotSpotlightStyleControls",
     true},
    {ScreenshotToolPalette::Tool::Text, "text", "screenshotTextStyleControls", true},
    {ScreenshotToolPalette::Tool::SerialNumber, "serial-number",
     "screenshotSerialNumberStyleControls", true},
    {ScreenshotToolPalette::Tool::RectangleFilter, "rect-filter", "screenshotFilterStyleControls",
     true},
    {ScreenshotToolPalette::Tool::PenFilter, "pen-filter", "screenshotPenFilterStyleControls",
     true},
    {ScreenshotToolPalette::Tool::Watermark, "watermark", "screenshotWatermarkStyleControls",
     true},
    {ScreenshotToolPalette::Tool::Eraser, "eraser", nullptr, false},
};
constexpr int kDrawingToolCount = static_cast<int>(sizeof(kDrawingTools) / sizeof(kDrawingTools[0]));

class NullToolbarCommands final : public ScreenshotToolbarCommandSink {
  public:
    void setMoveTool() override {}
    void setSelectTool() override {}
    void setShapeTool() override {}
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
};

void dwmFlush() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    static_cast<void>(DwmFlush());
#endif
}

double flushCompositorMs() {
    const qint64 started = nowNs();
    dwmFlush();
    return nsToMs(nowNs() - started);
}

#if defined(Q_OS_WIN) || defined(_WIN32)
HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

double redrawAllChildrenNowMs(QWidget* widget) {
    const qint64 started = nowNs();
    const HWND hwnd = widget != nullptr ? toNativeHwnd(widget->winId()) : nullptr;
    if (hwnd != nullptr) {
        static_cast<void>(
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN));
    }
    return nsToMs(nowNs() - started);
}

QRect nativeScreenPhysicalBounds(QScreen* screen) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (screen == nullptr || screen->handle() == nullptr ||
        GetMonitorInfoW(reinterpret_cast<HMONITOR>(screen->handle()), &info) == FALSE) {
        return QRect();
    }
    return QRect(info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top);
}
#else
double redrawAllChildrenNowMs(QWidget*) {
    return 0.0;
}

QRect nativeScreenPhysicalBounds(QScreen* screen) {
    return screen != nullptr ? screen->geometry() : QRect();
}
#endif

// Observes every delivered event so paint timestamps can be attributed to a
// phase without touching the widget code under test. The event revision also
// drives the idle detection used to settle samples.
class BenchmarkApplication final : public QApplication {
  public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override {
        ++m_eventRevision;
        if (event->type() == QEvent::Paint && m_trackedWindow != nullptr) {
            if (QWidget* widget = qobject_cast<QWidget*>(receiver)) {
                if (widget->window() == m_trackedWindow) {
                    recordPaint(widget);
                }
            }
        }
        return QApplication::notify(receiver, event);
    }

    void arm(QWidget* window, bool trackStylePanel) {
        m_trackedWindow = window;
        m_trackStylePanel = trackStylePanel;
        m_stylePanel.clear();
        m_armNs = nowNs();
        m_firstPaintSeen = false;
        m_firstPaintNs = 0;
        m_firstStylePaintSeen = false;
        m_firstStylePaintNs = 0;
        m_paintCount = 0;
        m_stylePaintCount = 0;
    }

    void disarm() {
        m_trackedWindow.clear();
        m_stylePanel.clear();
    }

    // Drains posted events, layout requests, and window-system events until
    // two consecutive turns deliver no events at all.
    void processUntilIdle(int maxTurns = 60) {
        quint64 previousRevision = m_eventRevision;
        int stableTurns = 0;
        for (int turn = 0; turn < maxTurns && stableTurns < 2; ++turn) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const quint64 revision = m_eventRevision;
            if (revision == previousRevision) {
                ++stableTurns;
            } else {
                stableTurns = 0;
            }
            previousRevision = revision;
        }
    }

    double settleMs() {
        const qint64 started = nowNs();
        processUntilIdle();
        return nsToMs(nowNs() - started);
    }

    qint64 armStampNs() const { return m_armNs; }
    bool firstPaintSeen() const { return m_firstPaintSeen; }
    double firstPaintMs() const {
        return m_firstPaintSeen ? nsToMs(m_firstPaintNs - m_armNs)
                                : std::numeric_limits<double>::quiet_NaN();
    }
    bool firstStylePaintSeen() const { return m_firstStylePaintSeen; }
    double firstStylePaintMs() const {
        return m_firstStylePaintSeen ? nsToMs(m_firstStylePaintNs - m_armNs)
                                     : std::numeric_limits<double>::quiet_NaN();
    }
    qint64 paintCount() const { return m_paintCount; }
    qint64 stylePaintCount() const { return m_stylePaintCount; }

  private:
    void recordPaint(QWidget* widget) {
        const qint64 stamp = nowNs();
        ++m_paintCount;
        if (!m_firstPaintSeen) {
            m_firstPaintSeen = true;
            m_firstPaintNs = stamp;
        }
        if (!m_trackStylePanel) {
            return;
        }
        if (m_stylePanel == nullptr && m_trackedWindow != nullptr) {
            // The panel is created lazily with the first hydrated family, so
            // resolve it on demand instead of once at arm time.
            m_stylePanel = m_trackedWindow->findChild<QWidget*>(
                QStringLiteral("screenshotRectangleStylePanel"));
        }
        if (m_stylePanel != nullptr && (widget == m_stylePanel.data() ||
                                        m_stylePanel->isAncestorOf(widget))) {
            ++m_stylePaintCount;
            if (!m_firstStylePaintSeen) {
                m_firstStylePaintSeen = true;
                m_firstStylePaintNs = stamp;
            }
        }
    }

    QPointer<QWidget> m_trackedWindow;
    QPointer<QWidget> m_stylePanel;
    qint64 m_armNs = 0;
    quint64 m_eventRevision = 0;
    bool m_trackStylePanel = false;
    bool m_firstPaintSeen = false;
    qint64 m_firstPaintNs = 0;
    bool m_firstStylePaintSeen = false;
    qint64 m_firstStylePaintNs = 0;
    qint64 m_paintCount = 0;
    qint64 m_stylePaintCount = 0;
};

// Collects the macro-guarded scopes/counters emitted by the toolbar sources
// into the currently active sample bucket.
class SinkCollector final : public toolbar_perf::Sink {
  public:
    void recordScope(const char* name, qint64 elapsedNanoseconds) override {
        ++m_totalScopeCount;
        if (m_currentIndex >= 0) {
            current().scopes[QString::fromLatin1(name)] += elapsedNanoseconds;
        }
    }

    void recordCounter(const char* name, qint64 value) override {
        if (m_currentIndex >= 0) {
            current().counters[QString::fromLatin1(name)] += value;
        }
    }

    struct Bucket {
        QString label;
        QMap<QString, qint64> scopes;
        QMap<QString, qint64> counters;
    };

    void begin(const QString& label) {
        m_buckets.append(Bucket{label, {}, {}});
        m_currentIndex = m_buckets.size() - 1;
    }

    void end() { m_currentIndex = -1; }

    const Bucket& bucketAt(int index) const { return m_buckets.at(index); }
    int bucketCount() const { return m_buckets.size(); }
    qint64 totalScopeCount() const { return m_totalScopeCount; }

  private:
    Bucket& current() { return m_buckets[m_currentIndex]; }

    QVector<Bucket> m_buckets;
    int m_currentIndex = -1;
    qint64 m_totalScopeCount = 0;
};

qint64 scopeSum(const QMap<QString, qint64>& scopes, std::initializer_list<const char*> names) {
    qint64 total = 0;
    for (const char* name : names) {
        total += scopes.value(QString::fromLatin1(name), 0);
    }
    return total;
}

struct FirstFrameSample {
    QMap<QString, double> phases;
    double firstPaintMs = std::numeric_limits<double>::quiet_NaN();
    double totalCreateToPresentMs = 0.0;
    qint64 paintCountAfterShow = 0;
    qint64 paintCountAfterRepaint = 0;
    qint64 paintCountAfterRedraw = 0;
    qint64 paintCountTotal = 0;
    qint64 widgetCount = 0;
    QMap<QString, qint64> scopes;
    QMap<QString, qint64> counters;
};

struct ToolSwitchSample {
    double switchMs = 0.0;
    double settleMs = 0.0;
    double compositorMs = 0.0;
    double firstPaintMs = std::numeric_limits<double>::quiet_NaN();
    double firstStylePaintMs = std::numeric_limits<double>::quiet_NaN();
    double totalToDisplayMs = 0.0;
    double hydrateMs = 0.0;
    double evictMs = 0.0;
    double rebindMs = 0.0;
    double layoutMs = 0.0;
    bool styleControlsVisible = false;
    bool hydrated = false;
    qint64 paintCount = 0;
    qint64 stylePaintCount = 0;
    qint64 windowWidgetCount = 0;
    QMap<QString, qint64> scopes;
    QMap<QString, qint64> counters;
};

struct ToolAggregate {
    QString id;
    QVector<ToolSwitchSample> samples;
    int invalidCount = 0;
    int totalSweeps = 0;
};

// Shows the toolbar through the production prepared-show sequence
// (screenshotoverlayuihost.cpp showPreparedWidget) with per-step timing. The
// concealment branch matches production: fresh window + windows QPA ->
// first paint happens while opacity is 0, the frame becomes visible at the
// opacity restore and is presented by the following DwmFlush.
void showPreparedWithPhases(BenchmarkApplication& app, ScreenshotToolbarWindow& toolbar,
                            FirstFrameSample& sample) {
    const qreal previousOpacity = toolbar.windowOpacity();
    {
        const qint64 started = nowNs();
        toolbar.setWindowOpacity(0.0);
        sample.phases[QStringLiteral("conceal_ms")] = nsToMs(nowNs() - started);
    }
    {
        const qint64 started = nowNs();
        toolbar.show();
        sample.phases[QStringLiteral("show_ms")] = nsToMs(nowNs() - started);
        sample.paintCountAfterShow = app.paintCount();
    }
    {
        const qint64 started = nowNs();
        toolbar.repaint();
        sample.phases[QStringLiteral("repaint_ms")] = nsToMs(nowNs() - started);
        sample.paintCountAfterRepaint = app.paintCount();
    }
    {
        const qint64 started = nowNs();
        QCoreApplication::sendPostedEvents(toolbar.window(), QEvent::UpdateRequest);
        sample.phases[QStringLiteral("posted_events_ms")] = nsToMs(nowNs() - started);
    }
    {
        const double elapsed = redrawAllChildrenNowMs(&toolbar);
        sample.phases[QStringLiteral("redraw_window_ms")] = elapsed;
        sample.paintCountAfterRedraw = app.paintCount();
    }
    {
        const qint64 started = nowNs();
        toolbar.setWindowOpacity(previousOpacity);
        sample.phases[QStringLiteral("reveal_ms")] = nsToMs(nowNs() - started);
    }
    {
        const qint64 started = nowNs();
        toolbar.raise();
        sample.phases[QStringLiteral("raise_ms")] = nsToMs(nowNs() - started);
    }
}

FirstFrameSample runFirstFrameSample(BenchmarkApplication& app, SinkCollector& collector,
                                     NullToolbarCommands& commands, QScreen* screen,
                                     const QRect& logicalBounds, const QRect& physicalBounds) {
    collector.begin(QStringLiteral("first_frame"));
    FirstFrameSample sample;
    const qint64 totalStarted = nowNs();
    {
        std::unique_ptr<ScreenshotToolbarWindow> toolbar;
        {
            const qint64 started = nowNs();
            toolbar = std::make_unique<ScreenshotToolbarWindow>(commands);
            sample.phases[QStringLiteral("ctor_ms")] = nsToMs(nowNs() - started);
        }
        {
            const qint64 started = nowNs();
            toolbar->setPlacementContext(screen, logicalBounds, physicalBounds);
            sample.phases[QStringLiteral("placement_ms")] = nsToMs(nowNs() - started);
        }
        {
            const qint64 started = nowNs();
            toolbar->prepareForDisplay();
            sample.phases[QStringLiteral("prepare_ms")] = nsToMs(nowNs() - started);
        }
        {
            const qint64 started = nowNs();
            toolbar->moveContentTo(logicalBounds.center() -
                                   QPoint(toolbar->contentSizeHint().width() / 2,
                                          toolbar->contentSizeHint().height() / 2));
            sample.phases[QStringLiteral("position_ms")] = nsToMs(nowNs() - started);
        }
        app.arm(toolbar.get(), false);
        showPreparedWithPhases(app, *toolbar, sample);
        sample.phases[QStringLiteral("compositor_flush_ms")] = flushCompositorMs();
        app.disarm();
        sample.totalCreateToPresentMs = nsToMs(nowNs() - totalStarted);
        sample.firstPaintMs = app.firstPaintMs();
        sample.paintCountTotal = app.paintCount();
        sample.widgetCount = toolbar->findChildren<QWidget*>().size();
        sample.phases[QStringLiteral("settle_after_present_ms")] = app.settleMs();
        toolbar->hide();
        toolbar.release()->deleteLater();
    }
    app.processUntilIdle();
    dwmFlush();

    const SinkCollector::Bucket& bucket = collector.bucketAt(collector.bucketCount() - 1);
    sample.scopes = bucket.scopes;
    sample.counters = bucket.counters;
    collector.end();
    return sample;
}

bool styleControlsMatchTool(const QWidget& toolbarRoot, const ToolInfo& info) {
    if (!info.hasStyleToolbar) {
        QWidget* panel = toolbarRoot.findChild<QWidget*>(
            QStringLiteral("screenshotRectangleStylePanel"));
        return panel == nullptr || !panel->isVisible();
    }
    QWidget* controls = toolbarRoot.findChild<QWidget*>(QLatin1String(info.styleObjectName));
    return controls != nullptr && controls->isVisible();
}

// Reset to Move exactly like the start of a new capture: resetForNewCapture
// parks the palette on Move, and the resulting family crossing evicts any
// materialized secondary toolbar, so the next style-tool activation is cold.
// The reset runs inside its own collector bucket so its evict/layout costs are
// attributed instead of being dropped between samples.
double resetToolbarToMove(BenchmarkApplication& app, SinkCollector& collector,
                          ScreenshotToolbarWindow& toolbar, const QWidget& owner) {
    collector.begin(QStringLiteral("tools.reset"));
    const qint64 started = nowNs();
    QToolTip::hideText();
    for (QWidget* topLevel : QApplication::topLevelWidgets()) {
        if (topLevel != &toolbar && topLevel != &owner && topLevel->isVisible()) {
            topLevel->hide();
        }
    }
    toolbar.resetForNewCapture();
    toolbar.setScrollingScreenshotMode(false);
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Move);
    toolbar.show();
    toolbar.raise();
    app.processUntilIdle();
    dwmFlush();
    const double elapsedMs = nsToMs(nowNs() - started);
    collector.end();
    return elapsedMs;
}

// Defined below the scenario runners; forward-declared for the helpers that
// format shared stats output.
QJsonValue statsJsonValue(const Stats& stats);
void printStatsHeader();
void printStatsRow(const QString& label, const Stats& stats);

QMap<QString, QVector<double>> bucketScopeValues(const SinkCollector& collector,
                                                  const QVector<int>& bucketIndices) {
    QMap<QString, QVector<double>> scopeValues;
    for (int index : bucketIndices) {
        const SinkCollector::Bucket& bucket = collector.bucketAt(index);
        for (auto iterator = bucket.scopes.constBegin(); iterator != bucket.scopes.constEnd();
             ++iterator) {
            scopeValues[iterator.key()].append(nsToMs(iterator.value()));
        }
    }
    return scopeValues;
}

QMap<QString, QVector<double>> bucketCounterValues(const SinkCollector& collector,
                                                   const QVector<int>& bucketIndices) {
    QMap<QString, QVector<double>> counterValues;
    for (int index : bucketIndices) {
        const SinkCollector::Bucket& bucket = collector.bucketAt(index);
        for (auto iterator = bucket.counters.constBegin(); iterator != bucket.counters.constEnd();
             ++iterator) {
            counterValues[iterator.key()].append(static_cast<double>(iterator.value()));
        }
    }
    return counterValues;
}

// Aggregates the per-reset buckets (scopes in ms, counters in raw counts) so
// the report shows what the Move reset itself costs beyond its wall time.
QJsonObject resetBucketsJson(const SinkCollector& collector, const QVector<int>& bucketIndices) {
    const QMap<QString, QVector<double>> scopeValues =
        bucketScopeValues(collector, bucketIndices);
    const QMap<QString, QVector<double>> counterValues =
        bucketCounterValues(collector, bucketIndices);
    QJsonObject scopesJson;
    for (auto iterator = scopeValues.constBegin(); iterator != scopeValues.constEnd(); ++iterator) {
        scopesJson.insert(iterator.key(), statsJsonValue(computeStats(iterator.value())).toObject());
    }
    QJsonObject countersJson;
    for (auto iterator = counterValues.constBegin(); iterator != counterValues.constEnd();
         ++iterator) {
        countersJson.insert(iterator.key(),
                            statsJsonValue(computeStats(iterator.value())).toObject());
    }
    QJsonObject out;
    out.insert(QStringLiteral("buckets"), bucketIndices.size());
    out.insert(QStringLiteral("scopes"), scopesJson);
    out.insert(QStringLiteral("counters"), countersJson);
    return out;
}

void printScopeStats(const QString& title, const QMap<QString, QVector<double>>& scopeValues) {
    std::cout << "\n" << title.toStdString() << "\n";
    printStatsHeader();
    QVector<QPair<double, QString>> sorted;
    for (auto iterator = scopeValues.constBegin(); iterator != scopeValues.constEnd();
         ++iterator) {
        const Stats stats = computeStats(iterator.value());
        sorted.append(qMakePair(std::isnan(stats.p50) ? 0.0 : stats.p50, iterator.key()));
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<double, QString>& left, const QPair<double, QString>& right) {
                  return left.first > right.first;
              });
    for (const QPair<double, QString>& entry : sorted) {
        printStatsRow(entry.second, computeStats(scopeValues.value(entry.second)));
    }
}

ToolSwitchSample runToolSwitchSample(BenchmarkApplication& app, SinkCollector& collector,
                                     ScreenshotToolbarWindow& toolbar, const ToolInfo& info,
                                     const QString& bucketLabel) {
    collector.begin(bucketLabel);
    ToolSwitchSample sample;
    app.arm(&toolbar, true);
    const qint64 started = nowNs();
    toolbar.setActiveTool(info.tool);
    const qint64 afterSwitch = nowNs();
    sample.settleMs = app.settleMs();
    sample.compositorMs = flushCompositorMs();
    const qint64 afterFlush = nowNs();
    app.disarm();

    sample.switchMs = nsToMs(afterSwitch - started);
    sample.firstPaintMs = app.firstPaintMs();
    sample.firstStylePaintMs = app.firstStylePaintMs();
    sample.paintCount = app.paintCount();
    sample.stylePaintCount = app.stylePaintCount();
    sample.windowWidgetCount = toolbar.findChildren<QWidget*>().size();
    sample.totalToDisplayMs = nsToMs(afterFlush - started);
    sample.styleControlsVisible = styleControlsMatchTool(toolbar, info);

    const SinkCollector::Bucket& bucket = collector.bucketAt(collector.bucketCount() - 1);
    sample.hydrated = bucket.counters.contains(QStringLiteral("hydrate.style_family"));
    sample.scopes = bucket.scopes;
    sample.counters = bucket.counters;
    sample.hydrateMs = nsToMs(scopeSum(bucket.scopes,
                                       {"palette.create_style_family",
                                        "palette.replay_materialized_state",
                                        "palette.initialize_style_layout_profiles",
                                        "palette.apply_scaled_toolbar_metrics"}));
    sample.evictMs =
        nsToMs(scopeSum(bucket.scopes, {"palette.evict_secondary_contents"}));
    sample.rebindMs = nsToMs(scopeSum(bucket.scopes, {"palette.set_style_controls_active",
                                                      "palette.set_secondary_visibility"}));
    sample.layoutMs = nsToMs(scopeSum(bucket.scopes,
                                      {"palette.update_row_geometry", "palette.ensure_layout_applied",
                                       "palette.layout_commit", "host.apply_size", "host.sync_size"}));
    collector.end();
    return sample;
}

std::unique_ptr<ScreenshotToolbarWindow> createShownToolbar(BenchmarkApplication& app,
                                                            NullToolbarCommands& commands,
                                                            QScreen* screen,
                                                            const QRect& logicalBounds,
                                                            const QRect& physicalBounds) {
    auto toolbar = std::make_unique<ScreenshotToolbarWindow>(commands);
    toolbar->setPlacementContext(screen, logicalBounds, physicalBounds);
    toolbar->prepareForDisplay();
    toolbar->moveContentTo(logicalBounds.center() -
                           QPoint(toolbar->contentSizeHint().width() / 2,
                                  toolbar->contentSizeHint().height() / 2));
    toolbar->show();
    toolbar->raise();
    app.processUntilIdle();
    dwmFlush();
    return toolbar;
}

QMap<QString, ToolAggregate> runToolCycle(BenchmarkApplication& app, SinkCollector& collector,
                                          NullToolbarCommands& commands, QWidget& owner,
                                          QScreen* screen, const QRect& logicalBounds,
                                          const QRect& physicalBounds, int warmupSweeps,
                                          int sweeps, double& resetMsP50Out,
                                          QVector<int>& resetBucketsOut) {
    QMap<QString, ToolAggregate> aggregates;
    for (int index = 0; index < kDrawingToolCount; ++index) {
        ToolAggregate aggregate;
        aggregate.id = QString::fromLatin1(kDrawingTools[index].id);
        aggregate.totalSweeps = sweeps;
        aggregates.insert(aggregate.id, aggregate);
    }

    auto toolbar = createShownToolbar(app, commands, screen, logicalBounds, physicalBounds);
    QVector<double> resetMs;
    for (int sweep = 0; sweep < warmupSweeps + sweeps; ++sweep) {
        QCursor::setPos(logicalBounds.topLeft() + QPoint(4, 4));
        resetBucketsOut.append(collector.bucketCount());
        resetMs.append(resetToolbarToMove(app, collector, *toolbar, owner));
        for (int index = 0; index < kDrawingToolCount; ++index) {
            const ToolInfo& info = kDrawingTools[index];
            const ToolSwitchSample sample = runToolSwitchSample(
                app, collector, *toolbar, info,
                QStringLiteral("tools.cycle.%1").arg(QLatin1String(info.id)));
            if (sweep < warmupSweeps) {
                continue;
            }
            ToolAggregate& aggregate = aggregates[QString::fromLatin1(info.id)];
            if (sample.styleControlsVisible) {
                aggregate.samples.append(sample);
            } else {
                ++aggregate.invalidCount;
            }
        }
    }
    QToolTip::hideText();
    toolbar->hide();
    toolbar.release()->deleteLater();
    app.processUntilIdle();
    dwmFlush();

    std::sort(resetMs.begin(), resetMs.end());
    resetMsP50Out = resetMs.isEmpty()
                        ? std::numeric_limits<double>::quiet_NaN()
                        : resetMs[resetMs.size() / 2];
    return aggregates;
}

QMap<QString, ToolAggregate> runToolCold(BenchmarkApplication& app, SinkCollector& collector,
                                         NullToolbarCommands& commands, QWidget& owner,
                                         QScreen* screen, const QRect& logicalBounds,
                                         const QRect& physicalBounds, int warmups, int samples,
                                         double& resetMsP50Out, QVector<int>& resetBucketsOut) {
    QMap<QString, ToolAggregate> aggregates;
    for (int index = 0; index < kDrawingToolCount; ++index) {
        ToolAggregate aggregate;
        aggregate.id = QString::fromLatin1(kDrawingTools[index].id);
        aggregate.totalSweeps = samples;
        aggregates.insert(aggregate.id, aggregate);
    }

    auto toolbar = createShownToolbar(app, commands, screen, logicalBounds, physicalBounds);
    QVector<double> resetMs;
    for (int index = 0; index < kDrawingToolCount; ++index) {
        const ToolInfo& info = kDrawingTools[index];
        for (int sampleIndex = 0; sampleIndex < warmups + samples; ++sampleIndex) {
            QCursor::setPos(logicalBounds.topLeft() + QPoint(4, 4));
            resetBucketsOut.append(collector.bucketCount());
            resetMs.append(resetToolbarToMove(app, collector, *toolbar, owner));
            const ToolSwitchSample sample = runToolSwitchSample(
                app, collector, *toolbar, info,
                QStringLiteral("tools.cold.%1").arg(QLatin1String(info.id)));
            if (sampleIndex < warmups) {
                continue;
            }
            ToolAggregate& aggregate = aggregates[QString::fromLatin1(info.id)];
            if (sample.styleControlsVisible) {
                aggregate.samples.append(sample);
            } else {
                ++aggregate.invalidCount;
            }
        }
    }
    QToolTip::hideText();
    toolbar->hide();
    toolbar.release()->deleteLater();
    app.processUntilIdle();
    dwmFlush();

    std::sort(resetMs.begin(), resetMs.end());
    resetMsP50Out = resetMs.isEmpty()
                        ? std::numeric_limits<double>::quiet_NaN()
                        : resetMs[resetMs.size() / 2];
    return aggregates;
}

QVector<double> extract(const QVector<ToolSwitchSample>& samples,
                        double ToolSwitchSample::*field) {
    QVector<double> values;
    values.reserve(samples.size());
    for (const ToolSwitchSample& sample : samples) {
        values.append(sample.*field);
    }
    return values;
}

QJsonValue statsJsonValue(const Stats& stats) {
    QJsonObject object;
    object.insert(QStringLiteral("count"), stats.count);
    const auto insertOrNaN = [&object](const char* key, double value) {
        object.insert(QString::fromLatin1(key),
                      std::isnan(value) ? QJsonValue() : QJsonValue(value));
    };
    insertOrNaN("p50", stats.p50);
    insertOrNaN("p90", stats.p90);
    insertOrNaN("p95", stats.p95);
    insertOrNaN("max", stats.max);
    insertOrNaN("mean", stats.mean);
    return object;
}

QJsonObject statsFieldJson(const QVector<ToolSwitchSample>& samples,
                           double ToolSwitchSample::*field) {
    return statsJsonValue(computeStats(extract(samples, field))).toObject();
}

void printStatsHeader() {
    std::cout << "phase                                    count      p50      p90      p95      "
                 "max     mean\n";
}

void printStatsRow(const QString& label, const Stats& stats) {
    std::cout << label.leftJustified(38).toStdString() << intCell(stats.count, 6).toStdString()
              << msCell(stats.p50, 9).toStdString() << msCell(stats.p90, 9).toStdString()
              << msCell(stats.p95, 9).toStdString() << msCell(stats.max, 9).toStdString()
              << msCell(stats.mean, 9).toStdString() << '\n';
}

void reportFirstFrame(const QVector<FirstFrameSample>& samples, QJsonObject& reportOut) {
    std::cout << "\n=== first_frame: toolbar creation -> first displayed frame ("
              << samples.size() << " samples) ===\n";
    printStatsHeader();
    QVector<QString> phaseOrder;
    if (!samples.isEmpty()) {
        // Chronological creation/show order; QMap key order would sort alphabetically.
        phaseOrder = QVector<QString>{
            QStringLiteral("ctor_ms"),           QStringLiteral("placement_ms"),
            QStringLiteral("prepare_ms"),        QStringLiteral("position_ms"),
            QStringLiteral("conceal_ms"),        QStringLiteral("show_ms"),
            QStringLiteral("repaint_ms"),        QStringLiteral("posted_events_ms"),
            QStringLiteral("redraw_window_ms"),  QStringLiteral("reveal_ms"),
            QStringLiteral("raise_ms"),          QStringLiteral("compositor_flush_ms"),
            QStringLiteral("settle_after_present_ms")};
    }
    QJsonObject phasesJson;
    for (const QString& phase : phaseOrder) {
        QVector<double> values;
        for (const FirstFrameSample& sample : samples) {
            values.append(sample.phases.value(phase));
        }
        const Stats stats = computeStats(values);
        printStatsRow(phase, stats);
        phasesJson.insert(phase, statsJsonValue(stats).toObject());
    }
    {
        QVector<double> values;
        for (const FirstFrameSample& sample : samples) {
            values.append(sample.firstPaintMs);
        }
        const Stats stats = computeStats(values);
        printStatsRow(QStringLiteral("first_paint_ms (from arm start)"), stats);
        phasesJson.insert(QStringLiteral("first_paint_ms"), statsJsonValue(stats).toObject());
    }
    {
        QVector<double> values;
        for (const FirstFrameSample& sample : samples) {
            values.append(sample.totalCreateToPresentMs);
        }
        const Stats stats = computeStats(values);
        printStatsRow(QStringLiteral("TOTAL create->present_ms"), stats);
        phasesJson.insert(QStringLiteral("total_create_to_present_ms"),
                          statsJsonValue(stats).toObject());
    }

    {
        QVector<double> afterShow;
        QVector<double> afterRepaint;
        QVector<double> afterRedraw;
        QVector<double> total;
        for (const FirstFrameSample& sample : samples) {
            afterShow.append(static_cast<double>(sample.paintCountAfterShow));
            afterRepaint.append(static_cast<double>(sample.paintCountAfterRepaint));
            afterRedraw.append(static_cast<double>(sample.paintCountAfterRedraw));
            total.append(static_cast<double>(sample.paintCountTotal));
        }
        std::cout << "\npaint counts (p50): after_show="
                  << computeStats(afterShow).p50 << " after_repaint="
                  << computeStats(afterRepaint).p50 << " after_redraw="
                  << computeStats(afterRedraw).p50 << " total=" << computeStats(total).p50
                  << '\n';
        QJsonObject paintsJson;
        paintsJson.insert(QStringLiteral("after_show"), computeStats(afterShow).p50);
        paintsJson.insert(QStringLiteral("after_repaint"), computeStats(afterRepaint).p50);
        paintsJson.insert(QStringLiteral("after_redraw"), computeStats(afterRedraw).p50);
        paintsJson.insert(QStringLiteral("total"), computeStats(total).p50);
        reportOut.insert(QStringLiteral("paints"), paintsJson);
    }

    std::cout << "\ninstrumented scopes (per creation):\n";
    printStatsHeader();
    QJsonObject scopesJson;
    QMap<QString, QVector<double>> scopeValues;
    for (const FirstFrameSample& sample : samples) {
        for (auto iterator = sample.scopes.constBegin(); iterator != sample.scopes.constEnd();
             ++iterator) {
            scopeValues[iterator.key()].append(nsToMs(iterator.value()));
        }
    }
    QVector<QPair<double, QString>> sortedScopes;
    for (auto iterator = scopeValues.constBegin(); iterator != scopeValues.constEnd();
         ++iterator) {
        const Stats stats = computeStats(iterator.value());
        sortedScopes.append(qMakePair(std::isnan(stats.p50) ? 0.0 : stats.p50, iterator.key()));
        scopesJson.insert(iterator.key(), statsJsonValue(stats).toObject());
    }
    std::sort(sortedScopes.begin(), sortedScopes.end(),
              [](const QPair<double, QString>& left, const QPair<double, QString>& right) {
                  return left.first > right.first;
              });
    for (const QPair<double, QString>& entry : sortedScopes) {
        printStatsRow(entry.second, computeStats(scopeValues.value(entry.second)));
    }

    reportOut.insert(QStringLiteral("phases"), phasesJson);
    reportOut.insert(QStringLiteral("scopes"), scopesJson);
    {
        QVector<double> widgetCounts;
        for (const FirstFrameSample& sample : samples) {
            widgetCounts.append(static_cast<double>(sample.widgetCount));
        }
        const Stats stats = computeStats(widgetCounts);
        std::cout << "\nwindow widget count after creation (p50): " << msCell(stats.p50, 0).toStdString()
                  << '\n';
        reportOut.insert(QStringLiteral("widget_count"), statsJsonValue(stats).toObject());
    }
    {
        QMap<QString, QVector<double>> counterValues;
        for (const FirstFrameSample& sample : samples) {
            for (auto iterator = sample.counters.constBegin(); iterator != sample.counters.constEnd();
                 ++iterator) {
                counterValues[iterator.key()].append(static_cast<double>(iterator.value()));
            }
        }
        printScopeStats(QStringLiteral("instrumented counters (per creation)"), counterValues);
        QJsonObject countersJson;
        for (auto iterator = counterValues.constBegin(); iterator != counterValues.constEnd();
             ++iterator) {
            countersJson.insert(iterator.key(),
                                statsJsonValue(computeStats(iterator.value())).toObject());
        }
        reportOut.insert(QStringLiteral("counters"), countersJson);
    }
    QJsonArray rawSamples;
    for (const FirstFrameSample& sample : samples) {
        QJsonObject raw;
        for (auto iterator = sample.phases.constBegin(); iterator != sample.phases.constEnd();
             ++iterator) {
            raw.insert(iterator.key(), iterator.value());
        }
        raw.insert(QStringLiteral("first_paint_ms"),
                   std::isnan(sample.firstPaintMs) ? QJsonValue() : QJsonValue(sample.firstPaintMs));
        raw.insert(QStringLiteral("total_create_to_present_ms"), sample.totalCreateToPresentMs);
        raw.insert(QStringLiteral("widget_count"), static_cast<double>(sample.widgetCount));
        for (auto iterator = sample.counters.constBegin(); iterator != sample.counters.constEnd();
             ++iterator) {
            raw.insert(iterator.key(), static_cast<double>(iterator.value()));
        }
        rawSamples.append(raw);
    }
    reportOut.insert(QStringLiteral("raw_samples"), rawSamples);
}

void reportToolTable(const QString& title, const QMap<QString, ToolAggregate>& aggregates,
                     QJsonArray& reportOut) {
    std::cout << "\n=== " << title.toStdString() << " ===\n";
    std::cout << "columns: cold% = sweeps that hydrated a family; switch/total = setActiveTool"
                 " call / switch start -> compositor present (p50 | p90);\n"
              << "          hydrate/evict/rebind/layout = instrumented scope sums (p50, ms); "
                 "style_paint = first style-panel paint (p50).\n";
    std::cout << "tool            cold%   switch p50    p90   hydrate    evict   rebind   "
                 "layout style_p    settle   compos    total p50    p90  invalid\n";
    for (int index = 0; index < kDrawingToolCount; ++index) {
        const ToolInfo& info = kDrawingTools[index];
        const ToolAggregate& aggregate = aggregates.value(QString::fromLatin1(info.id));
        const QVector<ToolSwitchSample>& samples = aggregate.samples;
        if (samples.isEmpty()) {
            std::cout << QString::fromLatin1(info.id).leftJustified(15).toStdString()
                      << "  (no valid samples; invalid=" << aggregate.invalidCount << ")\n";
            continue;
        }
        const Stats switchStats = computeStats(extract(samples, &ToolSwitchSample::switchMs));
        const Stats totalStats =
            computeStats(extract(samples, &ToolSwitchSample::totalToDisplayMs));
        const Stats hydrate = computeStats(extract(samples, &ToolSwitchSample::hydrateMs));
        const Stats evict = computeStats(extract(samples, &ToolSwitchSample::evictMs));
        const Stats rebind = computeStats(extract(samples, &ToolSwitchSample::rebindMs));
        const Stats layout = computeStats(extract(samples, &ToolSwitchSample::layoutMs));
        const Stats stylePaint =
            computeStats(extract(samples, &ToolSwitchSample::firstStylePaintMs));
        const Stats settle = computeStats(extract(samples, &ToolSwitchSample::settleMs));
        const Stats compositor = computeStats(extract(samples, &ToolSwitchSample::compositorMs));
        int hydratedCount = 0;
        for (const ToolSwitchSample& sample : samples) {
            hydratedCount += sample.hydrated ? 1 : 0;
        }
        const int coldPercent =
            static_cast<int>(std::lround(100.0 * hydratedCount / samples.size()));

        std::cout << QString::fromLatin1(info.id).leftJustified(15).toStdString()
                  << QString::number(coldPercent).rightJustified(5).toStdString() << '%'
                  << msCell(switchStats.p50, 10).toStdString()
                  << msCell(switchStats.p90, 8).toStdString() << msCell(hydrate.p50, 9).toStdString()
                  << msCell(evict.p50, 9).toStdString() << msCell(rebind.p50, 9).toStdString()
                  << msCell(layout.p50, 8).toStdString() << msCell(stylePaint.p50, 8).toStdString()
                  << msCell(settle.p50, 9).toStdString()
                  << msCell(compositor.p50, 8).toStdString()
                  << msCell(totalStats.p50, 10).toStdString()
                  << msCell(totalStats.p90, 8).toStdString()
                  << intCell(aggregate.invalidCount, 7).toStdString() << '\n';

        QJsonObject toolJson;
        toolJson.insert(QStringLiteral("id"), QString::fromLatin1(info.id));
        toolJson.insert(QStringLiteral("samples"), samples.size());
        toolJson.insert(QStringLiteral("invalid"), aggregate.invalidCount);
        toolJson.insert(QStringLiteral("cold_percent"), coldPercent);
        toolJson.insert(QStringLiteral("switch_ms"), statsFieldJson(samples,
                                                                    &ToolSwitchSample::switchMs));
        toolJson.insert(QStringLiteral("total_to_display_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::totalToDisplayMs));
        toolJson.insert(QStringLiteral("hydrate_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::hydrateMs));
        toolJson.insert(QStringLiteral("evict_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::evictMs));
        toolJson.insert(QStringLiteral("rebind_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::rebindMs));
        toolJson.insert(QStringLiteral("layout_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::layoutMs));
        toolJson.insert(QStringLiteral("first_paint_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::firstPaintMs));
        toolJson.insert(QStringLiteral("first_style_paint_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::firstStylePaintMs));
        toolJson.insert(QStringLiteral("settle_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::settleMs));
        toolJson.insert(QStringLiteral("compositor_ms"),
                        statsFieldJson(samples, &ToolSwitchSample::compositorMs));
        QJsonObject scopeJson;
        QMap<QString, QVector<double>> scopeValues;
        for (const ToolSwitchSample& sample : samples) {
            for (auto iterator = sample.scopes.constBegin(); iterator != sample.scopes.constEnd();
                 ++iterator) {
                scopeValues[iterator.key()].append(nsToMs(iterator.value()));
            }
        }
        for (auto iterator = scopeValues.constBegin(); iterator != scopeValues.constEnd();
             ++iterator) {
            scopeJson.insert(iterator.key(), statsJsonValue(computeStats(iterator.value())));
        }
        toolJson.insert(QStringLiteral("scopes"), scopeJson);
        {
            QMap<QString, QVector<double>> counterValues;
            for (const ToolSwitchSample& sample : samples) {
                // Counters are per-switch totals; a sample that never fired the
                // counter contributes zero so means stay per-switch averages.
                for (auto iterator = sample.counters.constBegin();
                     iterator != sample.counters.constEnd(); ++iterator) {
                    counterValues[iterator.key()].append(static_cast<double>(iterator.value()));
                }
            }
            QJsonObject countersJson;
            for (auto iterator = counterValues.constBegin(); iterator != counterValues.constEnd();
                 ++iterator) {
                countersJson.insert(iterator.key(),
                                    statsJsonValue(computeStats(iterator.value())).toObject());
            }
            toolJson.insert(QStringLiteral("counters"), countersJson);
        }
        {
            QVector<double> widgetCounts;
            for (const ToolSwitchSample& sample : samples) {
                widgetCounts.append(static_cast<double>(sample.windowWidgetCount));
            }
            toolJson.insert(QStringLiteral("window_widget_count"),
                            statsJsonValue(computeStats(widgetCounts)).toObject());
        }
        QJsonArray rawSamples;
        for (const ToolSwitchSample& sample : samples) {
            QJsonObject raw;
            raw.insert(QStringLiteral("switch_ms"), sample.switchMs);
            raw.insert(QStringLiteral("settle_ms"), sample.settleMs);
            raw.insert(QStringLiteral("compositor_ms"), sample.compositorMs);
            raw.insert(QStringLiteral("first_paint_ms"),
                       std::isnan(sample.firstPaintMs) ? QJsonValue()
                                                       : QJsonValue(sample.firstPaintMs));
            raw.insert(QStringLiteral("first_style_paint_ms"),
                       std::isnan(sample.firstStylePaintMs)
                           ? QJsonValue()
                           : QJsonValue(sample.firstStylePaintMs));
            raw.insert(QStringLiteral("total_to_display_ms"), sample.totalToDisplayMs);
            raw.insert(QStringLiteral("hydrate_ms"), sample.hydrateMs);
            raw.insert(QStringLiteral("evict_ms"), sample.evictMs);
            raw.insert(QStringLiteral("rebind_ms"), sample.rebindMs);
            raw.insert(QStringLiteral("layout_ms"), sample.layoutMs);
            raw.insert(QStringLiteral("paint_count"), static_cast<double>(sample.paintCount));
            raw.insert(QStringLiteral("style_paint_count"),
                       static_cast<double>(sample.stylePaintCount));
            raw.insert(QStringLiteral("window_widget_count"),
                       static_cast<double>(sample.windowWidgetCount));
            raw.insert(QStringLiteral("hydrated"), sample.hydrated);
            raw.insert(QStringLiteral("style_controls_visible"), sample.styleControlsVisible);
            for (auto iterator = sample.counters.constBegin(); iterator != sample.counters.constEnd();
                 ++iterator) {
                raw.insert(iterator.key(), static_cast<double>(iterator.value()));
            }
            rawSamples.append(raw);
        }
        toolJson.insert(QStringLiteral("raw_samples"), rawSamples);
        reportOut.append(toolJson);
    }

    std::cout << "\ncounters (p50 per switch; layout.commit = synchronous layout commits, "
                 "host.size_sync = host resize passes):\n";
    std::cout << "tool            layout.commit  size_sync  style_noop\n";
    for (int index = 0; index < kDrawingToolCount; ++index) {
        const ToolInfo& info = kDrawingTools[index];
        const QVector<ToolSwitchSample>& samples =
            aggregates.value(QString::fromLatin1(info.id)).samples;
        if (samples.isEmpty()) {
            continue;
        }
        const auto counterP50 = [&samples](const char* name) {
            QVector<double> values;
            for (const ToolSwitchSample& sample : samples) {
                values.append(static_cast<double>(
                    sample.counters.value(QString::fromLatin1(name), 0)));
            }
            return computeStats(values).p50;
        };
        std::cout << QString::fromLatin1(info.id).leftJustified(15).toStdString()
                  << QString::number(counterP50("layout.commit"), 'f', 1).rightJustified(13).toStdString()
                  << QString::number(counterP50("host.size_sync"), 'f', 1).rightJustified(11).toStdString()
                  << QString::number(counterP50("style.state_noop"), 'f', 1).rightJustified(11).toStdString()
                  << '\n';
    }
}

} // namespace

int main(int argc, char* argv[]) {
    BenchmarkApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("snow-shot-toolbar-display-benchmark"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Measures screenshot toolbar creation->first-frame and per-drawing-tool "
                       "sub-toolbar display performance."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption outputOption(
        {QStringLiteral("o"), QStringLiteral("output")},
        QStringLiteral("Directory for report.json (default build/toolbar-display-perf/<stamp>)."),
        QStringLiteral("directory"));
    parser.addOption(outputOption);
    const QCommandLineOption samplesOption(
        {QStringLiteral("s"), QStringLiteral("samples")},
        QStringLiteral("first_frame samples (default 25)."), QStringLiteral("count"), "25");
    parser.addOption(samplesOption);
    const QCommandLineOption warmupsOption(QStringLiteral("warmups"),
                                           QStringLiteral("first_frame warmups (default 6)."),
                                           QStringLiteral("count"), "6");
    parser.addOption(warmupsOption);
    const QCommandLineOption cycleSweepsOption(
        QStringLiteral("cycle-sweeps"), QStringLiteral("tools.cycle sweeps (default 12)."),
        QStringLiteral("count"), "12");
    parser.addOption(cycleSweepsOption);
    const QCommandLineOption cycleWarmupOption(
        QStringLiteral("cycle-warmups"), QStringLiteral("tools.cycle warmup sweeps (default 2)."),
        QStringLiteral("count"), "2");
    parser.addOption(cycleWarmupOption);
    const QCommandLineOption coldSamplesOption(
        QStringLiteral("cold-samples"),
        QStringLiteral("tools.cold samples per tool (default 8)."), QStringLiteral("count"), "8");
    parser.addOption(coldSamplesOption);
    const QCommandLineOption scenarioOption(
        QStringLiteral("scenario"),
        QStringLiteral("Scenario glob to run (first_frame, tools.cycle, tools.cold; default all)."),
        QStringLiteral("pattern"));
    parser.addOption(scenarioOption);
    const QCommandLineOption listOption(QStringLiteral("list-scenarios"),
                                        QStringLiteral("List scenario names and exit."));
    parser.addOption(listOption);
    parser.process(app);

    if (parser.isSet(listOption)) {
        std::cout << "first_frame\ntools.cycle\ntools.cold\n";
        return 0;
    }

    const int firstFrameSamples = parser.value(samplesOption).toInt();
    const int firstFrameWarmups = parser.value(warmupsOption).toInt();
    const int cycleSweeps = parser.value(cycleSweepsOption).toInt();
    const int cycleWarmups = parser.value(cycleWarmupOption).toInt();
    const int coldSamples = parser.value(coldSamplesOption).toInt();
    const int coldWarmups = 1;

    const QString scenarioGlob =
        parser.isSet(scenarioOption) ? parser.value(scenarioOption) : QStringLiteral("*");
    const QRegularExpression scenarioFilter =
        QRegularExpression::fromWildcard(scenarioGlob, Qt::CaseInsensitive);
    const bool runFirstFrame =
        scenarioFilter.match(QStringLiteral("first_frame")).hasMatch();
    const bool runCycle = scenarioFilter.match(QStringLiteral("tools.cycle")).hasMatch();
    const bool runCold = scenarioFilter.match(QStringLiteral("tools.cold")).hasMatch();

    const bool windowsPlatform =
        QGuiApplication::platformName() == QStringLiteral("windows");
    if (!windowsPlatform) {
        std::cerr << "warning: QT_QPA_PLATFORM is '"
                  << QGuiApplication::platformName().toStdString()
                  << "'; the prepared-show sequence and DwmFlush timings require 'windows'.\n";
    }

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        std::cerr << "error: no primary screen available.\n";
        return 1;
    }
    const QRect logicalBounds = screen->availableGeometry();
    const QRect physicalBounds = nativeScreenPhysicalBounds(screen);

    std::cout << "snow-shot toolbar display benchmark\n"
              << "  qt=" << QT_VERSION_STR << " platform="
              << QGuiApplication::platformName().toStdString()
              << " screen=" << screen->name().toStdString()
              << " dpr=" << screen->devicePixelRatio() << "\n"
              << "  bounds=" << logicalBounds.width() << "x" << logicalBounds.height()
              << " samples=" << firstFrameSamples << " sweeps=" << cycleSweeps
              << " cold_samples=" << coldSamples << "\n";

    // The production toolbar floats above the capture overlay; replicate that
    // with a full-screen translucent owner window.
    QWidget owner(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    owner.setObjectName(QLatin1String(kOwnerObjectName));
    owner.setAttribute(Qt::WA_TranslucentBackground, true);
    owner.setGeometry(logicalBounds);
    owner.show();

    NullToolbarCommands commands;
    SinkCollector collector;
    toolbar_perf::setSink(&collector);

    QJsonObject report;
    report.insert(QStringLiteral("schema"), kReportSchemaVersion);
    report.insert(QStringLiteral("benchmark"), QStringLiteral("toolbar-display"));
    report.insert(QStringLiteral("timestamp"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));
    report.insert(QStringLiteral("qt_version"), QString::fromLatin1(QT_VERSION_STR));
    report.insert(QStringLiteral("platform"), QGuiApplication::platformName());
    {
        QJsonObject screenJson;
        screenJson.insert(QStringLiteral("name"), screen->name());
        screenJson.insert(QStringLiteral("dpr"), screen->devicePixelRatio());
        screenJson.insert(QStringLiteral("refresh_rate_hz"), screen->refreshRate());
        screenJson.insert(
            QStringLiteral("geometry"),
            QStringLiteral("%1,%2 %3x%4")
                .arg(logicalBounds.x())
                .arg(logicalBounds.y())
                .arg(logicalBounds.width())
                .arg(logicalBounds.height()));
        report.insert(QStringLiteral("screen"), screenJson);
    }
    {
        QJsonObject envJson;
        const auto stamp = [](const char* key) {
            const QString value = qEnvironmentVariable(key);
            return value.isEmpty() ? QJsonValue() : QJsonValue(value);
        };
        envJson.insert(QStringLiteral("git_commit"), stamp("SNOW_SHOT_PERF_GIT_COMMIT"));
        envJson.insert(QStringLiteral("gpu_driver"), stamp("SNOW_SHOT_PERF_GPU_DRIVER"));
        envJson.insert(QStringLiteral("power_plan"), stamp("SNOW_SHOT_PERF_POWER_PLAN"));
        report.insert(QStringLiteral("environment"), envJson);
    }
    {
        QJsonObject settingsJson;
        settingsJson.insert(QStringLiteral("first_frame_samples"), firstFrameSamples);
        settingsJson.insert(QStringLiteral("first_frame_warmups"), firstFrameWarmups);
        settingsJson.insert(QStringLiteral("cycle_sweeps"), cycleSweeps);
        settingsJson.insert(QStringLiteral("cycle_warmups"), cycleWarmups);
        settingsJson.insert(QStringLiteral("cold_samples"), coldSamples);
        settingsJson.insert(QStringLiteral("cold_warmups"), coldWarmups);
        report.insert(QStringLiteral("settings"), settingsJson);
    }

    // Harness floor calibration: a no-op settle + DwmFlush pair measures what the
    // measurement machinery itself costs. Subtract it from per-sample
    // settle/compositor/total_to_display numbers to isolate real application
    // work; without it, warm-path totals look floor-dominated.
    {
        app.processUntilIdle();
        dwmFlush();
        QVector<double> settleFloor;
        QVector<double> compositorFloor;
        QVector<double> cycleFloor;
        for (int index = 0; index < 6; ++index) {
            const double settle = app.settleMs();
            const double compositor = flushCompositorMs();
            settleFloor.append(settle);
            compositorFloor.append(compositor);
            cycleFloor.append(settle + compositor);
        }
        const Stats settleStats = computeStats(settleFloor);
        const Stats compositorStats = computeStats(compositorFloor);
        QJsonObject floorJson;
        floorJson.insert(QStringLiteral("samples"), settleFloor.size());
        floorJson.insert(QStringLiteral("settle_ms"),
                         statsJsonValue(settleStats).toObject());
        floorJson.insert(QStringLiteral("compositor_flush_ms"),
                         statsJsonValue(compositorStats).toObject());
        floorJson.insert(QStringLiteral("cycle_ms"),
                         statsJsonValue(computeStats(cycleFloor)).toObject());
        floorJson.insert(
            QStringLiteral("note"),
            QStringLiteral("no-op settle + DwmFlush floor; subtract from per-sample "
                           "settle/compositor/total_to_display to isolate real work"));
        report.insert(QStringLiteral("idle_floor"), floorJson);
        std::cout << "idle floor (no-op settle + DwmFlush): settle p50="
                  << msCell(settleStats.p50, 0).toStdString()
                  << " ms, compositor p50=" << msCell(compositorStats.p50, 0).toStdString()
                  << " ms, screen refresh=" << screen->refreshRate() << " Hz\n";
    }

    int invalidTotal = 0;

    if (runFirstFrame) {
        QVector<FirstFrameSample> kept;
        for (int index = 0; index < firstFrameWarmups + firstFrameSamples; ++index) {
            FirstFrameSample sample = runFirstFrameSample(app, collector, commands, screen,
                                                          logicalBounds, physicalBounds);
            if (index >= firstFrameWarmups) {
                kept.append(sample);
            }
        }
        QJsonObject firstFrameJson;
        reportFirstFrame(kept, firstFrameJson);
        report.insert(QStringLiteral("first_frame"), firstFrameJson);
    }

    if (runCycle) {
        double resetMsP50 = std::numeric_limits<double>::quiet_NaN();
        QVector<int> resetBuckets;
        QMap<QString, ToolAggregate> aggregates =
            runToolCycle(app, collector, commands, owner, screen, logicalBounds, physicalBounds,
                         cycleWarmups, cycleSweeps, resetMsP50, resetBuckets);
        QJsonArray toolsJson;
        reportToolTable(QStringLiteral("tools.cycle: sequential switch through all drawing "
                                       "tools"),
                        aggregates, toolsJson);
        report.insert(QStringLiteral("tools_cycle"), toolsJson);
        report.insert(QStringLiteral("cycle_reset"), resetBucketsJson(collector, resetBuckets));
        report.insert(QStringLiteral("cycle_reset_to_move_ms_p50"),
                      std::isnan(resetMsP50) ? QJsonValue() : QJsonValue(resetMsP50));
        std::cout << "\nreset to Move per sweep (p50): " << msCell(resetMsP50, 0).toStdString()
                  << " ms\n";
        printScopeStats(QStringLiteral("reset scopes (per reset)"),
                        bucketScopeValues(collector, resetBuckets));
        for (auto iterator = aggregates.constBegin(); iterator != aggregates.constEnd();
             ++iterator) {
            invalidTotal += iterator->invalidCount;
        }
    }

    if (runCold) {
        double resetMsP50 = std::numeric_limits<double>::quiet_NaN();
        QVector<int> resetBuckets;
        QMap<QString, ToolAggregate> aggregates =
            runToolCold(app, collector, commands, owner, screen, logicalBounds, physicalBounds,
                        coldWarmups, coldSamples, resetMsP50, resetBuckets);
        QJsonArray toolsJson;
        reportToolTable(
            QStringLiteral("tools.cold: isolated first activation per tool (reset between)"),
            aggregates, toolsJson);
        report.insert(QStringLiteral("tools_cold"), toolsJson);
        report.insert(QStringLiteral("cold_reset"), resetBucketsJson(collector, resetBuckets));
        report.insert(QStringLiteral("cold_reset_to_move_ms_p50"),
                      std::isnan(resetMsP50) ? QJsonValue() : QJsonValue(resetMsP50));
        for (auto iterator = aggregates.constBegin(); iterator != aggregates.constEnd();
             ++iterator) {
            invalidTotal += iterator->invalidCount;
        }
    }

    toolbar_perf::setSink(nullptr);

    if (collector.totalScopeCount() == 0) {
        std::cerr << "warning: no toolbar perf scopes received; build this target with "
                     "SNOW_SHOT_TOOLBAR_PERF_INSTRUMENTATION=1 for phase detail.\n";
    }

    const QString outputDir = parser.isSet(outputOption)
                                  ? parser.value(outputOption)
                                  : QDir(QDir::currentPath())
                                        .filePath(QStringLiteral("build/toolbar-display-perf/") +
                                                 QDateTime::currentDateTime().toString(
                                                     QStringLiteral("yyyyMMdd-HHmmss")));
    if (!QDir().mkpath(outputDir)) {
        std::cerr << "error: cannot create output directory "
                  << QDir::toNativeSeparators(outputDir).toStdString() << '\n';
        return 1;
    }
    const QString reportPath = QDir(outputDir).filePath(QStringLiteral("report.json"));
    QFile reportFile(reportPath);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "error: cannot write " << QDir::toNativeSeparators(reportPath).toStdString()
                  << '\n';
        return 1;
    }
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    reportFile.close();
    std::cout << "\nreport written to "
              << QDir::toNativeSeparators(reportPath).toStdString() << '\n';

    if (invalidTotal > 0) {
        std::cerr << "warning: " << invalidTotal
                  << " tool-switch samples were excluded because the expected sub-toolbar was "
                     "not visible after the switch.\n";
    }
    return invalidTotal > 0 ? 3 : 0;
}
