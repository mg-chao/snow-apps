#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QScreen>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <dwmapi.h>
#include <qt_windows.h>
#endif

namespace {
constexpr double kFrameBudgetMilliseconds = 16.67;
constexpr int kDefaultIterations = 240;
constexpr int kDefaultWarmup = 30;

class PaintProbe final : public QObject {
  public:
    void begin() {
        m_region = {};
    }

    [[nodiscard]] QRegion region() const {
        return m_region;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (event != nullptr && event->type() == QEvent::Paint) {
            m_region += static_cast<QPaintEvent*>(event)->region();
        }
        return false;
    }

  private:
    QRegion m_region;
};

struct BenchmarkFixture {
    explicit BenchmarkFixture(const QSize& size)
        : canvas(std::make_unique<SnowCanvasWidget>(&window)),
          renderer(std::make_unique<ScreenshotCanvasRenderer>(*canvas)) {
        window.setWindowTitle(QStringLiteral("Snow Shot selection benchmark"));
        window.setAttribute(Qt::WA_NativeWindow, true);
        window.resize(size);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(canvas.get());
        canvas->setClearBackgroundEnabled(false);
        canvas->setCustomRenderer(renderer.get());
        static_cast<void>(canvas->setViewportCamera(0.0, 0.0, 1.0));

        QImage screenshot(size, QImage::Format_RGBA8888);
        screenshot.fill(QColor(0, 80, 240));
        renderer->setImage(std::move(screenshot), QRectF(-size.width() / 2.0, -size.height() / 2.0,
                                                         size.width(), size.height()));
        renderer->setMaskVisible(true);
        renderer->setSelection(QRectF(-960.0, -540.0, 1920.0, 1080.0), true, 0, 16,
                               QColor(0x59, 0x59, 0x59));
        canvas->installEventFilter(&paintProbe);
        window.show();
        QApplication::processEvents();
    }

    ~BenchmarkFixture() {
        canvas->removeEventFilter(&paintProbe);
        canvas->setCustomRenderer(nullptr);
    }

    QWidget window;
    std::unique_ptr<SnowCanvasWidget> canvas;
    std::unique_ptr<ScreenshotCanvasRenderer> renderer;
    PaintProbe paintProbe;
};

struct FrameSample {
    double milliseconds = 0.0;
    double requestedPaintRegionRatio = 0.0;
    double paintedPaintRegionRatio = 0.0;
    double selectionDamageRegionRatio = 0.0;
    std::size_t tileCandidates = 0;
    std::size_t tileVisits = 0;
    std::size_t shadowCacheHits = 0;
    std::size_t shadowCacheBuilds = 0;
    std::size_t shadowRetainedBytes = 0;
    std::size_t shadowTransientAllocations = 0;
    std::size_t selectionDamagePathFallbacks = 0;
};

struct ScenarioResult {
    QString name;
    bool available = true;
    std::vector<FrameSample> samples;
};

struct DwmSnapshot {
    bool available = false;
    quint64 refreshCount = 0;
    quint64 composeCount = 0;
};

DwmSnapshot dwmSnapshot(const QWidget& widget) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND windowHandle = reinterpret_cast<HWND>(widget.winId());
    if (windowHandle == nullptr) {
        return {};
    }
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (DwmGetCompositionTimingInfo(windowHandle, &timing) != S_OK) {
        return {};
    }
    return DwmSnapshot{
        true,
        timing.cRefresh,
        timing.cFrame,
    };
#else
    Q_UNUSED(widget);
    return {};
#endif
}

double paintRegionRatio(const QRegion& region, const QSize& size) {
    if (region.isEmpty() || size.isEmpty()) {
        return 0.0;
    }
    qint64 pixels = 0;
    for (const QRect& rect : region) {
        pixels += static_cast<qint64>(rect.width()) * rect.height();
    }
    return static_cast<double>(pixels) / static_cast<double>(size.width()) /
           static_cast<double>(size.height());
}

FrameSample measureFrame(BenchmarkFixture& fixture, const std::function<void()>& mutation) {
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    snow_canvas_renderer::resetFilterRenderDiagnosticsForCurrentThread();
    resetSelectionRenderDiagnosticsForCurrentThread();
    fixture.paintProbe.begin();
    QElapsedTimer timer;
    timer.start();
    mutation();
    QApplication::processEvents();
    const qint64 elapsedNanoseconds = timer.nsecsElapsed();
    const QRegion requested = fixture.paintProbe.region();
    const QRegion painted = requested.intersected(fixture.canvas->rect());
    const auto rendererDiagnostics =
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    const auto shadowDiagnostics = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    const auto selectionDiagnostics = selectionRenderDiagnosticsForCurrentThread();
    return FrameSample{
        elapsedNanoseconds / 1'000'000.0,
        paintRegionRatio(requested, fixture.canvas->size()),
        paintRegionRatio(painted, fixture.canvas->size()),
        static_cast<double>(selectionDiagnostics.requestedDamagePixels) /
            static_cast<double>(fixture.canvas->width()) /
            static_cast<double>(fixture.canvas->height()),
        rendererDiagnostics.tileCandidates,
        rendererDiagnostics.tileVisits,
        shadowDiagnostics.cacheHits,
        shadowDiagnostics.cacheBuilds,
        shadowDiagnostics.retainedBytes,
        shadowDiagnostics.selectionSizedTransientAllocations,
        selectionDiagnostics.pathFallbacks,
    };
}

ScenarioResult runScenario(BenchmarkFixture& fixture, const QString& name, int warmup,
                           int iterations, const std::function<void(int)>& mutation,
                           bool available = true) {
    for (int index = 0; index < warmup; ++index) {
        static_cast<void>(measureFrame(fixture, [&]() { mutation(index); }));
    }

    ScenarioResult result;
    result.name = name;
    result.available = available;
    result.samples.reserve(static_cast<std::size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        result.samples.push_back(measureFrame(fixture, [&]() { mutation(index + warmup); }));
    }
    return result;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(
        values.size() - 1, static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1);
    return values[index];
}

double mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

QJsonObject summarize(const ScenarioResult& result, const DwmSnapshot& beforeDwm,
                      const DwmSnapshot& afterDwm) {
    std::vector<double> milliseconds;
    std::vector<double> requestedRegionRatios;
    std::vector<double> paintedRegionRatios;
    std::vector<double> selectionDamageRatios;
    milliseconds.reserve(result.samples.size());
    requestedRegionRatios.reserve(result.samples.size());
    paintedRegionRatios.reserve(result.samples.size());
    selectionDamageRatios.reserve(result.samples.size());
    std::size_t tileCandidates = 0;
    std::size_t tileVisits = 0;
    std::size_t shadowHits = 0;
    std::size_t shadowBuilds = 0;
    std::size_t shadowRetainedBytes = 0;
    std::size_t shadowTransientAllocations = 0;
    std::size_t selectionDamagePathFallbacks = 0;
    for (const FrameSample& sample : result.samples) {
        milliseconds.push_back(sample.milliseconds);
        requestedRegionRatios.push_back(sample.requestedPaintRegionRatio);
        paintedRegionRatios.push_back(sample.paintedPaintRegionRatio);
        selectionDamageRatios.push_back(sample.selectionDamageRegionRatio);
        tileCandidates += sample.tileCandidates;
        tileVisits += sample.tileVisits;
        shadowHits += sample.shadowCacheHits;
        shadowBuilds += sample.shadowCacheBuilds;
        shadowRetainedBytes = std::max(shadowRetainedBytes, sample.shadowRetainedBytes);
        shadowTransientAllocations += sample.shadowTransientAllocations;
        selectionDamagePathFallbacks += sample.selectionDamagePathFallbacks;
    }

    const double p95 = percentile(milliseconds, 0.95);
    QJsonObject object;
    object.insert(QStringLiteral("name"), result.name);
    object.insert(QStringLiteral("available"), result.available);
    object.insert(QStringLiteral("sampleCount"), static_cast<qint64>(result.samples.size()));
    object.insert(QStringLiteral("p50Ms"), percentile(milliseconds, 0.50));
    object.insert(QStringLiteral("p95Ms"), p95);
    object.insert(QStringLiteral("p99Ms"), percentile(milliseconds, 0.99));
    object.insert(QStringLiteral("meanRequestedPaintRegionRatio"), mean(requestedRegionRatios));
    object.insert(QStringLiteral("meanPaintedPaintRegionRatio"), mean(paintedRegionRatios));
    object.insert(QStringLiteral("meanSelectionDamageRegionRatio"), mean(selectionDamageRatios));
    object.insert(QStringLiteral("meanPaintRegionRatio"), mean(paintedRegionRatios));
    object.insert(
        QStringLiteral("tileCandidatesPerFrame"),
        result.samples.empty() ? 0.0 : static_cast<double>(tileCandidates) / result.samples.size());
    object.insert(QStringLiteral("tileVisitsPerFrame"),
                  result.samples.empty() ? 0.0
                                         : static_cast<double>(tileVisits) / result.samples.size());
    object.insert(QStringLiteral("shadowCacheHits"), static_cast<qint64>(shadowHits));
    object.insert(QStringLiteral("shadowCacheBuilds"), static_cast<qint64>(shadowBuilds));
    object.insert(QStringLiteral("shadowRetainedBytes"), static_cast<qint64>(shadowRetainedBytes));
    object.insert(QStringLiteral("shadowTransientAllocations"),
                  static_cast<qint64>(shadowTransientAllocations));
    object.insert(QStringLiteral("selectionDamagePathFallbacks"),
                  static_cast<qint64>(selectionDamagePathFallbacks));
    object.insert(QStringLiteral("targetMs"), kFrameBudgetMilliseconds);
    object.insert(QStringLiteral("classification"), p95 <= kFrameBudgetMilliseconds
                                                        ? QStringLiteral("within-target")
                                                        : QStringLiteral("over-target"));
    object.insert(QStringLiteral("hardwareSensitive"), true);
    object.insert(QStringLiteral("ciFailure"), false);
    object.insert(QStringLiteral("dwmPresentationAvailable"),
                  beforeDwm.available && afterDwm.available);
    if (beforeDwm.available && afterDwm.available) {
        object.insert(QStringLiteral("dwmRefreshDelta"),
                      static_cast<qint64>(afterDwm.refreshCount - beforeDwm.refreshCount));
        object.insert(QStringLiteral("dwmComposeDelta"),
                      static_cast<qint64>(afterDwm.composeCount - beforeDwm.composeCount));
    }
    return object;
}

void createSpotlightCutout(SnowCanvasWidget& canvas) {
    static_cast<void>(canvas.setCanvasTool(SnowCanvasTool::Spotlight));
    const QPointF start(160.0, 160.0);
    const QPointF end(800.0, 600.0);
    QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &release);
    QApplication::processEvents();
    static_cast<void>(canvas.setCanvasTool(SnowCanvasTool::Select));
}

QJsonObject writeReports(const QList<QJsonObject>& objects, const QString& jsonlPath,
                         const QString& summaryPath, const QString& htmlPath,
                         const QSize& surfaceSize, qreal devicePixelRatio) {
    QFile jsonl(jsonlPath);
    if (!jsonl.open(QIODevice::WriteOnly | QIODevice::Text)) {
        throw std::runtime_error("unable to open JSONL output");
    }
    QTextStream jsonlStream(&jsonl);
    for (const QJsonObject& object : objects) {
        jsonlStream << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
    }
    jsonl.close();

    QJsonArray scenarios;
    for (const QJsonObject& object : objects) {
        scenarios.append(object);
    }
    QJsonObject summary;
    summary.insert(QStringLiteral("schemaVersion"), 1);
    summary.insert(QStringLiteral("surfaceWidth"), surfaceSize.width());
    summary.insert(QStringLiteral("surfaceHeight"), surfaceSize.height());
    summary.insert(QStringLiteral("devicePixelRatio"), devicePixelRatio);
    summary.insert(QStringLiteral("targetMs"), kFrameBudgetMilliseconds);
    summary.insert(QStringLiteral("timingClassificationIsInformational"), true);
    summary.insert(QStringLiteral("scenarios"), scenarios);

    QFile summaryFile(summaryPath);
    if (!summaryFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        throw std::runtime_error("unable to open JSON summary output");
    }
    summaryFile.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    summaryFile.close();

    QFile html(htmlPath);
    if (!html.open(QIODevice::WriteOnly | QIODevice::Text)) {
        throw std::runtime_error("unable to open HTML summary output");
    }
    const QByteArray summaryJson = QJsonDocument(summary).toJson(QJsonDocument::Indented);
    const QByteArray htmlContent =
        "<!doctype html><meta charset=\"utf-8\"><title>Snow Shot selection render benchmark</title>"
        "<style>body{font:14px system-ui;margin:24px}pre{white-space:pre-wrap}</style>"
        "<h1>Selection render benchmark</h1><pre>" +
        summaryJson + "</pre>";
    html.write(htmlContent);
    html.close();
    return summary;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native Windows Snow Shot selection rendering benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("jsonl"), QStringLiteral("JSONL output path"),
                      QStringLiteral("path"), QStringLiteral("selection-render-benchmark.jsonl")});
    parser.addOption({QStringLiteral("summary"), QStringLiteral("JSON summary output path"),
                      QStringLiteral("path"), QStringLiteral("selection-render-benchmark.json")});
    parser.addOption({QStringLiteral("html"), QStringLiteral("HTML summary output path"),
                      QStringLiteral("path"), QStringLiteral("selection-render-benchmark.html")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Measured frames per scenario"),
                      QStringLiteral("count"), QString::number(kDefaultIterations)});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("Warmup frames per scenario"),
                      QStringLiteral("count"), QString::number(kDefaultWarmup)});
    parser.addOption(
        {QStringLiteral("list"), QStringLiteral("List benchmark scenarios and exit")});
    parser.process(application);

    const QStringList scenarioNames{
        QStringLiteral("one-pixel-move"),
        QStringLiteral("one-pixel-resize"),
        QStringLiteral("smart-selection-animation"),
        QStringLiteral("rounded-corners"),
        QStringLiteral("hover-entry-exit"),
        QStringLiteral("shadow-width-sweep"),
        QStringLiteral("cursor-and-monitor-guide-lines"),
        QStringLiteral("monitor-center-guide-line-only"),
        QStringLiteral("active-spotlight"),
        QStringLiteral("live-reanchored-watermark"),
        QStringLiteral("fractional-dpr"),
        QStringLiteral("cross-monitor-selection"),
    };
    if (parser.isSet(QStringLiteral("list"))) {
        QTextStream(stdout) << scenarioNames.join('\n') << '\n';
        return 0;
    }

    bool iterationsOk = false;
    bool warmupOk = false;
    const int iterations = parser.value(QStringLiteral("iterations")).toInt(&iterationsOk);
    const int warmup = parser.value(QStringLiteral("warmup")).toInt(&warmupOk);
    if (!iterationsOk || !warmupOk || iterations <= 0 || warmup < 0) {
        QTextStream(stderr) << "iterations must be positive and warmup non-negative\n";
        return 2;
    }

    const QSize surfaceSize(3840, 2160);
    BenchmarkFixture fixture(surfaceSize);
    auto& canvas = *fixture.canvas;
    auto& renderer = *fixture.renderer;
    const QRectF baseSelection(-960.0, -540.0, 1920.0, 1080.0);
    const QColor shadowColor(0x59, 0x59, 0x59);
    QList<QJsonObject> reports;

    const auto run = [&](const QString& name, const std::function<void(int)>& mutation,
                         bool available = true) {
        const DwmSnapshot before = dwmSnapshot(fixture.window);
        const ScenarioResult result =
            runScenario(fixture, name, warmup, iterations, mutation, available);
        const DwmSnapshot after = dwmSnapshot(fixture.window);
        reports.append(summarize(result, before, after));
    };

    run(QStringLiteral("one-pixel-move"), [&](int index) {
        renderer.setSelection(QRectF(baseSelection.left() + (index & 1), baseSelection.top(),
                                     baseSelection.width(), baseSelection.height()),
                              true, 0, 16, shadowColor);
    });
    run(QStringLiteral("one-pixel-resize"), [&](int index) {
        renderer.setSelection(QRectF(baseSelection.left(), baseSelection.top(),
                                     baseSelection.width() + (index & 1),
                                     baseSelection.height() + ((index >> 1) & 1)),
                              true, 0, 16, shadowColor);
    });
    run(QStringLiteral("smart-selection-animation"), [&](int index) {
        const qreal amount = (index % 120) / 119.0;
        const QRectF target(-1200.0, -720.0, 2400.0, 1440.0);
        renderer.setSelection(
            QRectF(baseSelection.left() * (1.0 - amount) + target.left() * amount,
                   baseSelection.top() * (1.0 - amount) + target.top() * amount,
                   baseSelection.width() * (1.0 - amount) + target.width() * amount,
                   baseSelection.height() * (1.0 - amount) + target.height() * amount),
            true, 0, 16, shadowColor);
    });
    run(QStringLiteral("rounded-corners"), [&](int index) {
        renderer.setSelection(baseSelection, true, (index * 3) % 96, 16, shadowColor);
    });
    run(QStringLiteral("hover-entry-exit"), [&](int index) {
        renderer.setSelection(baseSelection, true, 12, 16, shadowColor);
        renderer.setSelectionToolbarHovered((index & 1) != 0);
    });
    run(QStringLiteral("shadow-width-sweep"), [&](int index) {
        static constexpr std::array<int, 8> widths = {1, 4, 16, 32, 64, 32, 16, 4};
        renderer.setSelection(baseSelection, true, 16, widths[index % widths.size()], shadowColor);
        renderer.setSelectionToolbarHovered(true);
    });

    const QPointF guideLineCenter(surfaceSize.width() / 2.0, surfaceSize.height() / 2.0);
    const QColor cursorGuideLineColor(220, 30, 40);
    const QColor monitorGuideLineColor(30, 80, 220);
    renderer.setSelectionToolbarHovered(false);
    renderer.setGuideLines(guideLineCenter, cursorGuideLineColor, monitorGuideLineColor);
    QApplication::processEvents();
    run(QStringLiteral("cursor-and-monitor-guide-lines"), [&](int index) {
        renderer.setGuideLines(guideLineCenter + QPointF(index & 1, (index >> 1) & 1),
                               cursorGuideLineColor, monitorGuideLineColor);
    });
    renderer.setGuideLines(guideLineCenter, Qt::transparent, monitorGuideLineColor);
    QApplication::processEvents();
    run(QStringLiteral("monitor-center-guide-line-only"), [&](int index) {
        renderer.setGuideLines(guideLineCenter + QPointF(index & 1, (index >> 1) & 1),
                               Qt::transparent, monitorGuideLineColor);
    });
    renderer.clearGuideLines();
    QApplication::processEvents();

    createSpotlightCutout(canvas);
    run(QStringLiteral("active-spotlight"), [&](int index) {
        const qreal offset = static_cast<qreal>(index & 63);
        canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
            std::nullopt,
            std::optional<QRectF>(QRectF(-960.0 + offset, -540.0, 1920.0, 1080.0)),
        });
    });

    SnowCanvasWatermarkConfig watermark;
    watermark.text = QStringLiteral("SNOW SHOT");
    watermark.color = Qt::white;
    watermark.fontSize = 28.0;
    watermark.opacity = 1.0;
    static_cast<void>(canvas.setCanvasWatermarkConfig(watermark));
    run(QStringLiteral("live-reanchored-watermark"), [&](int index) {
        const qreal offset = static_cast<qreal>(index & 63);
        canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
            std::optional<QRectF>(QRectF(-960.0 + offset, -540.0, 1920.0, 1080.0)),
            std::nullopt,
        });
    });
    run(QStringLiteral("fractional-dpr"), [&](int index) {
        renderer.setSelection(QRectF(-960.25 + (index & 1) * 0.5, -540.25, 1920.5, 1080.5), true,
                              18, 16, shadowColor);
    });

    const QList<QScreen*> screens = QGuiApplication::screens();
    const bool hasSecondScreen = screens.size() > 1;
    if (hasSecondScreen) {
        fixture.window.move(screens.at(1)->availableGeometry().center() -
                            QPoint(surfaceSize.width() / 2, surfaceSize.height() / 2));
        QApplication::processEvents();
    }
    run(
        QStringLiteral("cross-monitor-selection"),
        [&](int index) {
            renderer.setSelection(QRectF(-960.0 + (index & 1), -540.0, 1920.0, 1080.0), true, 12,
                                  16, shadowColor);
        },
        hasSecondScreen);

    try {
        const QJsonObject summary = writeReports(
            reports, parser.value(QStringLiteral("jsonl")), parser.value(QStringLiteral("summary")),
            parser.value(QStringLiteral("html")), surfaceSize, canvas.devicePixelRatioF());
        QTextStream(stdout) << QJsonDocument(summary).toJson(QJsonDocument::Indented);
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 3;
    }
}
