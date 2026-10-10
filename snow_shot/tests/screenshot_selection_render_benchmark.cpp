#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_canvas_render_diagnostics.h"
#include "snow_shot/image/screenshotregionpoints.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
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

QRectF baseSelectionForSize(const QSize& size) {
    return QRectF(-size.width() / 4.0, -size.height() / 4.0, size.width() / 2.0,
                  size.height() / 2.0);
}

QImage screenshotTexture(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = qRgb((x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255, (x * 11 + y * 7) & 255);
        }
    }
    return image;
}

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
    explicit BenchmarkFixture(const QSize& size, bool translucent = false)
        : canvas(std::make_unique<SnowCanvasWidget>(runtime, &window)),
          renderer(std::make_unique<ScreenshotCanvasRenderer>(*canvas)) {
        window.setWindowTitle(QStringLiteral("Snow Shot selection benchmark"));
        // Match the screenshot overlay's native backing-store configuration before
        // WA_NativeWindow creates a platform window. An opaque child would bypass
        // the parent alpha surface and invalidate the presentation comparison.
        if (translucent) {
            window.setWindowFlag(Qt::FramelessWindowHint);
            window.setAttribute(Qt::WA_TranslucentBackground);
            canvas->setAttribute(Qt::WA_OpaquePaintEvent, false);
        }
        window.setAttribute(Qt::WA_NativeWindow, true);
        window.resize(size);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(canvas.get());
        canvas->setClearBackgroundEnabled(false);
        canvas->setCustomRenderer(renderer.get());
        static_cast<void>(canvas->setViewportCamera(0.0, 0.0, 1.0));

        const qreal dpr = window.devicePixelRatioF();
        const QSize pixelSize(static_cast<int>(std::ceil(size.width() * dpr)),
                              static_cast<int>(std::ceil(size.height() * dpr)));
        QImage screenshot = screenshotTexture(pixelSize);
        renderer->setImage(std::move(screenshot), QRectF(-size.width() / 2.0, -size.height() / 2.0,
                                                         size.width(), size.height()));
        renderer->setMaskVisible(true);
        renderer->setSelection(baseSelectionForSize(size), true, 0, 16, QColor(0x59, 0x59, 0x59));
        canvas->installEventFilter(&paintProbe);
        window.show();
        QApplication::processEvents();
    }

    ~BenchmarkFixture() {
        canvas->removeEventFilter(&paintProbe);
        canvas->setCustomRenderer(nullptr);
    }

    SnowCanvasRuntime runtime;
    QWidget window;
    std::unique_ptr<SnowCanvasWidget> canvas;
    std::unique_ptr<ScreenshotCanvasRenderer> renderer;
    PaintProbe paintProbe;
};

struct FrameSample {
    double milliseconds = 0.0;
    double requestedPaintRegionRatio = 0.0;
    double paintedPaintRegionRatio = 0.0;
    double paintedBoundingRectRatio = 0.0;
    double selectionDamageRegionRatio = 0.0;
    std::size_t shadowCacheHits = 0;
    std::size_t shadowCacheBuilds = 0;
    std::size_t shadowRetainedBytes = 0;
    std::size_t shadowTransientAllocations = 0;
    std::size_t selectionDamagePathFallbacks = 0;
    double mutationMs = 0;
    double eventProcessingMs = 0;
    std::size_t regionMaskBuilds = 0;
    std::size_t regionShadowBuilds = 0;
    std::size_t regionScratchPeakBytes = 0;
    snow_canvas_renderer::FilterRenderDiagnostics filter;
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
    resetSelectionRenderDiagnosticsForCurrentThread();
    snow_canvas_renderer::resetFilterRenderDiagnosticsForCurrentThread();
    fixture.paintProbe.begin();
    QElapsedTimer timer;
    timer.start();
    mutation();
    const auto mutationNs = timer.nsecsElapsed();
    QApplication::processEvents();
    const qint64 elapsedNanoseconds = timer.nsecsElapsed();
    const QRegion requested = fixture.paintProbe.region();
    const QRegion painted = requested.intersected(fixture.canvas->rect());
    const auto shadowDiagnostics = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    const auto selectionDiagnostics = selectionRenderDiagnosticsForCurrentThread();
    return FrameSample{
        static_cast<double>(elapsedNanoseconds) / 1'000'000.0,
        paintRegionRatio(requested, fixture.canvas->size()),
        paintRegionRatio(painted, fixture.canvas->size()),
        paintRegionRatio(QRegion(painted.boundingRect()), fixture.canvas->size()),
        static_cast<double>(selectionDiagnostics.requestedDamagePixels) /
            static_cast<double>(fixture.canvas->width()) /
            static_cast<double>(fixture.canvas->height()),
        shadowDiagnostics.cacheHits,
        shadowDiagnostics.cacheBuilds,
        shadowDiagnostics.retainedBytes,
        shadowDiagnostics.selectionSizedTransientAllocations,
        selectionDiagnostics.pathFallbacks,
        static_cast<double>(mutationNs) / 1'000'000.0,
        static_cast<double>(elapsedNanoseconds - mutationNs) / 1'000'000.0,
        shadowDiagnostics.regionMaskBuilds,
        shadowDiagnostics.regionShadowBuilds,
        shadowDiagnostics.regionScratchPeakBytes,
        snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread(),
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
        values.size() - 1,
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
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
    std::vector<double> milliseconds, mutationMs, eventProcessingMs;
    std::vector<double> requestedRegionRatios;
    std::vector<double> paintedRegionRatios;
    std::vector<double> paintedBoundingRectRatios;
    std::vector<double> selectionDamageRatios;
    milliseconds.reserve(result.samples.size());
    requestedRegionRatios.reserve(result.samples.size());
    paintedRegionRatios.reserve(result.samples.size());
    paintedBoundingRectRatios.reserve(result.samples.size());
    selectionDamageRatios.reserve(result.samples.size());
    std::size_t shadowHits = 0;
    std::size_t shadowBuilds = 0;
    std::size_t shadowRetainedBytes = 0;
    std::size_t shadowTransientAllocations = 0;
    std::size_t selectionDamagePathFallbacks = 0;
    std::size_t regionMaskBuilds = 0, regionShadowBuilds = 0, regionScratchPeakBytes = 0;
    snow_canvas_renderer::FilterRenderDiagnostics filterTotals;
    std::size_t peakFilterDispatchCount = 0;
    for (const FrameSample& sample : result.samples) {
        regionMaskBuilds += sample.regionMaskBuilds;
        regionShadowBuilds += sample.regionShadowBuilds;
        regionScratchPeakBytes = std::max(regionScratchPeakBytes, sample.regionScratchPeakBytes);
        milliseconds.push_back(sample.milliseconds);
        mutationMs.push_back(sample.mutationMs);
        eventProcessingMs.push_back(sample.eventProcessingMs);
        snow_canvas_renderer::accumulateFilterRenderDiagnostics(filterTotals, sample.filter);
        peakFilterDispatchCount =
            std::max(peakFilterDispatchCount, sample.filter.effectDispatchCount);
        requestedRegionRatios.push_back(sample.requestedPaintRegionRatio);
        paintedRegionRatios.push_back(sample.paintedPaintRegionRatio);
        paintedBoundingRectRatios.push_back(sample.paintedBoundingRectRatio);
        selectionDamageRatios.push_back(sample.selectionDamageRegionRatio);
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
    object.insert(QStringLiteral("mutationP95Ms"), percentile(mutationMs, 0.95));
    object.insert(QStringLiteral("eventProcessingP95Ms"), percentile(eventProcessingMs, 0.95));
    object.insert(QStringLiteral("p95Ms"), p95);
    object.insert(QStringLiteral("p99Ms"), percentile(milliseconds, 0.99));
    object.insert(QStringLiteral("meanRequestedPaintRegionRatio"), mean(requestedRegionRatios));
    object.insert(QStringLiteral("meanPaintedPaintRegionRatio"), mean(paintedRegionRatios));
    // Windows' translucent backing store presents the dirty region's bounding
    // rectangle. This metric estimates that amplification; it does not measure
    // compositor latency or prove the pixels were displayed.
    object.insert(QStringLiteral("meanPaintedBoundingRectRatio"), mean(paintedBoundingRectRatios));
    object.insert(QStringLiteral("meanSelectionDamageRegionRatio"), mean(selectionDamageRatios));
    object.insert(QStringLiteral("meanPaintRegionRatio"), mean(paintedRegionRatios));
    object.insert(QStringLiteral("shadowCacheHits"), static_cast<qint64>(shadowHits));
    object.insert(QStringLiteral("shadowCacheBuilds"), static_cast<qint64>(shadowBuilds));
    object.insert(QStringLiteral("shadowRetainedBytes"), static_cast<qint64>(shadowRetainedBytes));
    object.insert(QStringLiteral("shadowTransientAllocations"),
                  static_cast<qint64>(shadowTransientAllocations));
    object.insert(QStringLiteral("selectionDamagePathFallbacks"),
                  static_cast<qint64>(selectionDamagePathFallbacks));
    object.insert(QStringLiteral("regionMaskBuilds"), qint64(regionMaskBuilds));
    object.insert(QStringLiteral("regionShadowBuilds"), qint64(regionShadowBuilds));
    object.insert(QStringLiteral("regionScratchPeakBytes"), qint64(regionScratchPeakBytes));
    object.insert(QStringLiteral("filterEffectDispatchCount"),
                  static_cast<qint64>(filterTotals.effectDispatchCount));
    object.insert(QStringLiteral("meanFilterEffectDispatchCount"),
                  static_cast<double>(filterTotals.effectDispatchCount) /
                      static_cast<double>(result.samples.size()));
    object.insert(QStringLiteral("peakFilterEffectDispatchCount"),
                  static_cast<qint64>(peakFilterDispatchCount));
    object.insert(QStringLiteral("filterExposedPixelCount"),
                  static_cast<qint64>(filterTotals.exposedPixelCount));
    object.insert(QStringLiteral("filterTotalWorkingPixelCount"),
                  static_cast<qint64>(filterTotals.totalWorkingPixelCount));
    object.insert(QStringLiteral("meanFilterWorkingPixelCount"),
                  static_cast<double>(filterTotals.totalWorkingPixelCount) /
                      static_cast<double>(result.samples.size()));
    object.insert(QStringLiteral("filterPeakWorkingPixelCount"),
                  static_cast<qint64>(filterTotals.peakWorkingPixelCount));
    object.insert(QStringLiteral("filterAllocatedBytes"),
                  static_cast<qint64>(filterTotals.allocatedBytes));
    object.insert(QStringLiteral("filterCopiedBytes"),
                  static_cast<qint64>(filterTotals.copiedBytes));
    object.insert(QStringLiteral("filterSourceTileCandidates"),
                  static_cast<qint64>(filterTotals.sourceTileCandidates));
    object.insert(QStringLiteral("filterSourceTileVisits"),
                  static_cast<qint64>(filterTotals.sourceTileVisits));
    object.insert(QStringLiteral("filterSourceTileHits"),
                  static_cast<qint64>(filterTotals.sourceTileHits));
    object.insert(QStringLiteral("filterSourceTileMisses"),
                  static_cast<qint64>(filterTotals.sourceTileMisses));
    QJsonObject filterStageTotalMs;
    const auto stage = [&](const QString& name, std::uint64_t nanoseconds) {
        filterStageTotalMs.insert(name, static_cast<double>(nanoseconds) / 1'000'000.0);
    };
    stage(QStringLiteral("planning"), filterTotals.planningNanoseconds);
    stage(QStringLiteral("sceneReplay"), filterTotals.sceneReplayNanoseconds);
    stage(QStringLiteral("maskConstruction"), filterTotals.maskConstructionNanoseconds);
    stage(QStringLiteral("maskScan"), filterTotals.maskScanNanoseconds);
    stage(QStringLiteral("downsample"), filterTotals.downsampleNanoseconds);
    stage(QStringLiteral("reducedBlur"), filterTotals.reducedBlurNanoseconds);
    stage(QStringLiteral("reconstruction"), filterTotals.reconstructionNanoseconds);
    stage(QStringLiteral("presentation"), filterTotals.presentationNanoseconds);
    object.insert(QStringLiteral("filterStageTotalMs"), filterStageTotalMs);
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

void applyResizeSelection(BenchmarkFixture& fixture, const QRectF& selection) {
    ScreenshotSelectionVisualState state;
    state.bounds = selection;
    state.present = true;
    state.shadowWidth = 16;
    state.shadowColor = QColor(0x59, 0x59, 0x59);
    fixture.renderer->applySelectionState(state);
    fixture.canvas->setDecorationRenderAreas({selection, selection});
}

void createRectangleFilter(BenchmarkFixture& fixture, SnowCanvasFilterType type,
                           const QRectF& canvasRect) {
    auto& canvas = *fixture.canvas;
    if (!fixture.runtime.setQuickSelectionDisabledTools({SnowCanvasTool::RectangleFilter}) ||
        !canvas.setCanvasFilterStyle({type, 0.65, 1.0, 2.0},
                                     SnowCanvasFilterStylePropertyType |
                                         SnowCanvasFilterStylePropertyStrength |
                                         SnowCanvasFilterStylePropertyOpacity) ||
        !canvas.setCanvasTool(SnowCanvasTool::RectangleFilter)) {
        throw std::runtime_error("unable to configure benchmark filter");
    }
    const QTransform transform = canvas.canvasToViewTransform();
    const QPointF start = transform.map(canvasRect.topLeft());
    const QPointF end = transform.map(canvasRect.bottomRight());
    QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &release);
    if (!canvas.resetEditingState() || !canvas.setCanvasTool(SnowCanvasTool::Select) ||
        !canvas.canvasHistoryState().canUndo) {
        throw std::runtime_error("unable to create benchmark filter annotation");
    }
    QApplication::processEvents();
}

void createGuideTextAnnotations(BenchmarkFixture& fixture, int count) {
    if (count == 0) {
        return;
    }
    auto& canvas = *fixture.canvas;
    if (!fixture.runtime.setQuickSelectionDisabledTools({SnowCanvasTool::Text})) {
        throw std::runtime_error("unable to configure benchmark text annotations");
    }
    const int columns = std::min(count, 10);
    const int rows = (count + columns - 1) / columns;
    for (int index = 0; index < count; ++index) {
        if (!canvas.setCanvasTool(SnowCanvasTool::Text)) {
            throw std::runtime_error("unable to select benchmark text tool");
        }
        const QPointF position(canvas.width() * (0.2 + 0.6 * (index % columns) / columns),
                               canvas.height() * (0.2 + 0.6 * (index / columns) / rows));
        QMouseEvent press(QEvent::MouseButtonPress, position, position, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, position, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&canvas, &release);
        QInputMethodEvent text;
        text.setCommitString(QStringLiteral("Guide benchmark annotation %1").arg(index));
        QCoreApplication::sendEvent(&canvas, &text);
        if (!canvas.resetEditingState() || !canvas.canvasHistoryState().canUndo) {
            throw std::runtime_error("unable to commit benchmark text annotation");
        }
    }
    QApplication::processEvents();
}

QJsonObject writeReports(const QList<QJsonObject>& objects, const QString& jsonlPath,
                         const QString& summaryPath, const QString& htmlPath,
                         const QSize& surfaceSize, const QSize& actualCanvasSize,
                         qreal devicePixelRatio, bool translucent) {
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
    summary.insert(QStringLiteral("schemaVersion"), 4);
#if defined(SNOW_SHOT_SELECTION_RENDER_BASELINE)
    summary.insert(QStringLiteral("implementation"), QStringLiteral("baseline"));
#else
    summary.insert(QStringLiteral("implementation"), QStringLiteral("candidate"));
#endif
    summary.insert(QStringLiteral("requestedSurfaceWidth"), surfaceSize.width());
    summary.insert(QStringLiteral("requestedSurfaceHeight"), surfaceSize.height());
    summary.insert(QStringLiteral("surfaceWidth"), actualCanvasSize.width());
    summary.insert(QStringLiteral("surfaceHeight"), actualCanvasSize.height());
    summary.insert(QStringLiteral("devicePixelRatio"), devicePixelRatio);
    summary.insert(QStringLiteral("physicalSurfaceWidth"),
                   static_cast<int>(std::ceil(actualCanvasSize.width() * devicePixelRatio)));
    summary.insert(QStringLiteral("physicalSurfaceHeight"),
                   static_cast<int>(std::ceil(actualCanvasSize.height() * devicePixelRatio)));
    summary.insert(QStringLiteral("qtPlatform"), QGuiApplication::platformName());
    summary.insert(QStringLiteral("qtScaleFactor"), qEnvironmentVariable("QT_SCALE_FACTOR"));
    summary.insert(QStringLiteral("translucentWindow"), translucent);
    summary.insert(
        QStringLiteral("timingScope"),
        QStringLiteral("Default: API mutation plus QApplication::processEvents, including "
                       "synchronous backing-store flush; excludes compositor latency "
                       "and the screenshot presenter/input path. A scenario's "
                       "measurement field overrides this default scope."));
    summary.insert(QStringLiteral("paintedBoundingRectScope"),
                   QStringLiteral("bounding rectangle of observed canvas paint events; proxy "
                                  "for Windows translucent backing-store presentation area"));
    summary.insert(QStringLiteral("screenshotTexture"),
                   QStringLiteral("deterministic-rgb-pattern"));
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
    snow_canvas_render_diagnostics::setEnabled(true);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Snow Shot selection rendering benchmark"));
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
    parser.addOption({QStringLiteral("surface-width"), QStringLiteral("Logical surface width"),
                      QStringLiteral("pixels"), QStringLiteral("3840")});
    parser.addOption({QStringLiteral("surface-height"), QStringLiteral("Logical surface height"),
                      QStringLiteral("pixels"), QStringLiteral("2160")});
    parser.addOption({QStringLiteral("translucent"),
                      QStringLiteral("Use the screenshot overlay's translucent native surface")});
    parser.addOption({QStringLiteral("guide-text-annotations"),
                      QStringLiteral("Text annotation count in guide and snap-target scenarios"),
                      QStringLiteral("count"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("list"), QStringLiteral("List benchmark scenarios and exit")});
    parser.addOption({QStringLiteral("scenario"), QStringLiteral("Run one scenario (repeatable)"),
                      QStringLiteral("name")});
    parser.process(application);

    const QStringList scenarioNames{
        QStringLiteral("custom-freehand"),
        QStringLiteral("custom-curve"),
        QStringLiteral("custom-mixed-operations"),
        QStringLiteral("custom-growing-draft"),
        QStringLiteral("rounded-many-regions"),
        QStringLiteral("sparse-region-export"),
        QStringLiteral("dense-region-export"),
        QStringLiteral("custom-alternating-scale"),
        QStringLiteral("region-one-pixel-move"),
        QStringLiteral("region-hover-one-pixel-move"),
        QStringLiteral("region-two-stage-presentation"),
        QStringLiteral("region-one-pass-presentation"),
        QStringLiteral("one-pixel-move"),
        QStringLiteral("one-pixel-resize"),
        QStringLiteral("resize-filter-interior"),
        QStringLiteral("resize-filter-edge"),
        QStringLiteral("smart-selection-animation"),
        QStringLiteral("rectangular-overlay-pass-aligned"),
        QStringLiteral("rectangular-overlay-pass-fractional"),
        QStringLiteral("shared-source-startup"),
        QStringLiteral("separate-source-startup"),
        QStringLiteral("rounded-corners"),
        QStringLiteral("hover-entry-exit"),
        QStringLiteral("shadow-width-sweep"),
        QStringLiteral("rounded-shadow-toggle"),
        QStringLiteral("cursor-and-monitor-guide-lines"),
        QStringLiteral("cursor-guide-horizontal-move"),
        QStringLiteral("cursor-guide-vertical-move"),
        QStringLiteral("cursor-guide-diagonal-move"),
        QStringLiteral("cursor-guide-complex-selection"),
        QStringLiteral("monitor-center-guide-line-only"),
        QStringLiteral("snap-guide-targets-unchanged"),
        QStringLiteral("snap-guide-targets-changed"),
        QStringLiteral("selection-center-guide-horizontal-move"),
        QStringLiteral("selection-center-guide-vertical-move"),
        QStringLiteral("selection-center-guide-fixed-center-resize"),
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
    bool guideTextAnnotationsOk = false;
    const int guideTextAnnotations =
        parser.value(QStringLiteral("guide-text-annotations")).toInt(&guideTextAnnotationsOk);
    if (!guideTextAnnotationsOk || guideTextAnnotations < 0 || guideTextAnnotations > 1000) {
        QTextStream(stderr) << "guide text annotation count must be between 0 and 1000\n";
        return 2;
    }

    bool widthOk = false;
    bool heightOk = false;
    const int surfaceWidth = parser.value(QStringLiteral("surface-width")).toInt(&widthOk);
    const int surfaceHeight = parser.value(QStringLiteral("surface-height")).toInt(&heightOk);
    if (!widthOk || !heightOk || surfaceWidth < 640 || surfaceHeight < 480) {
        QTextStream(stderr) << "surface width must be at least 640 and height at least 480\n";
        return 2;
    }
    for (const QString& requested : parser.values(QStringLiteral("scenario"))) {
        if (!scenarioNames.contains(requested)) {
            QTextStream(stderr) << "unknown scenario: " << requested << '\n';
            return 2;
        }
    }
    const QSize surfaceSize(surfaceWidth, surfaceHeight);
    const bool translucent = parser.isSet(QStringLiteral("translucent"));
    BenchmarkFixture fixture(surfaceSize, translucent);
    auto& canvas = *fixture.canvas;
    auto& renderer = *fixture.renderer;
    const QRectF baseSelection = baseSelectionForSize(surfaceSize);
    const QColor shadowColor(0x59, 0x59, 0x59);
    const ScreenshotRegionGeometry compoundRegion(
        QRegion(QRect(-960, -540, 1920, 1080)).subtracted(QRect(-120, -120, 240, 240)));
    QList<QJsonObject> reports;

    const auto run = [&](const QString& name, const std::function<void(int)>& mutation,
                         bool available = true) {
        if (parser.isSet(QStringLiteral("scenario")) &&
            !parser.values(QStringLiteral("scenario")).contains(name))
            return;
        // Each scenario starts with the same selection, including draft and hover
        // state. Otherwise the result depends on which scenarios ran before it.
        ScreenshotSelectionVisualState initial;
        initial.bounds = baseSelection;
        initial.present = true;
        initial.shadowWidth = 16;
        initial.shadowColor = shadowColor;
        renderer.applySelectionState(initial);
        QApplication::processEvents();
        const DwmSnapshot before = dwmSnapshot(fixture.window);
        const ScenarioResult result =
            runScenario(fixture, name, warmup, iterations, mutation, available);
        const DwmSnapshot after = dwmSnapshot(fixture.window);
        auto report = summarize(result, before, after);
        report.insert(QStringLiteral("outlineCacheRetainedBytes"),
                      renderer.selectionOutlineCacheBytes());
        report.insert(QStringLiteral("maskCacheRetainedBytes"), renderer.selectionMaskCacheBytes());
        report.insert(QStringLiteral("canvasWidth"), fixture.canvas->width());
        report.insert(QStringLiteral("canvasHeight"), fixture.canvas->height());
        report.insert(QStringLiteral("devicePixelRatio"), fixture.canvas->devicePixelRatioF());
        reports.append(report);
    };

    const auto selectedScenario = [&](const QString& name) {
        return !parser.isSet(QStringLiteral("scenario")) ||
               parser.values(QStringLiteral("scenario")).contains(name);
    };
    const auto runOverlayPass = [&](const QString& name, bool fractional) {
        if (!selectedScenario(name))
            return;
        const qreal dpr = canvas.devicePixelRatioF();
        QImage target(QSize(qCeil(surfaceWidth * dpr), qCeil(surfaceHeight * dpr)),
                      QImage::Format_ARGB32_Premultiplied);
        target.setDevicePixelRatio(dpr);
        const QTransform transform =
            QTransform::fromTranslate(surfaceWidth / 2.0, surfaceHeight / 2.0);
        ScenarioResult result;
        result.name = name;
        for (int frame = 0; frame < warmup + iterations; ++frame) {
            target.fill(QColor(37, 83, 129));
            resetSelectionRenderDiagnosticsForCurrentThread();
            resetGuideLineRenderDiagnosticsForCurrentThread();
            const qreal x = 20.0 + frame % 97;
            const qreal y = 20.0 + frame % 53;
            const qreal phase = fractional ? 0.271 : 0.0;
            const QRectF hole((std::round(x * dpr) + phase) / dpr,
                              (std::round(y * dpr) + phase) / dpr,
                              std::round(surfaceWidth * 0.55 * dpr) / dpr,
                              std::round(surfaceHeight * 0.55 * dpr) / dpr);
            QElapsedTimer timer;
            timer.start();
            renderer.setSelection(hole.translated(-surfaceWidth / 2.0, -surfaceHeight / 2.0),
                                  false);
            const qint64 mutationNs = timer.nsecsElapsed();
            {
                QPainter painter(&target);
                painter.setRenderHint(QPainter::Antialiasing);
                renderer.renderAfterCanvas(painter,
                                           {canvas.rect(), QRegion(canvas.rect()), transform, dpr});
            }
            if (frame >= warmup) {
                FrameSample sample;
                sample.milliseconds = static_cast<double>(timer.nsecsElapsed()) / 1e6;
                sample.mutationMs = static_cast<double>(mutationNs) / 1e6;
                sample.eventProcessingMs = sample.milliseconds - sample.mutationMs;
                result.samples.push_back(sample);
            }
        }
        auto report = summarize(result, {}, {});
        report.insert(
            QStringLiteral("measurement"),
            QStringLiteral("Selection mutation plus direct overlay mask/border paint "
                           "into QImage; excludes background, Qt event delivery, "
                           "backing-store flush, and compositor presentation. "
                           "eventProcessingP95Ms denotes direct paint work in this case."));
        report.insert(QStringLiteral("devicePixelRatio"), dpr);
        report.insert(QStringLiteral("canvasWidth"), surfaceWidth);
        report.insert(QStringLiteral("canvasHeight"), surfaceHeight);
        reports.append(report);
    };
    runOverlayPass(QStringLiteral("rectangular-overlay-pass-aligned"), false);
    runOverlayPass(QStringLiteral("rectangular-overlay-pass-fractional"), true);

    const auto runSourceStartup = [&](const QString& name, bool shared) {
        if (!selectedScenario(name))
            return;
        constexpr int kOverlayCount = 3;
        const qreal dpr = canvas.devicePixelRatioF();
        const QSize pixels(qCeil(surfaceWidth * dpr), qCeil(surfaceHeight * dpr));
        const QRectF bounds(-surfaceWidth / 2.0, -surfaceHeight / 2.0, surfaceWidth, surfaceHeight);
        QList<ScreenshotImageLayer> layers;
        std::array<std::unique_ptr<SnowCanvasWidget>, kOverlayCount> canvases;
        std::array<std::unique_ptr<ScreenshotCanvasRenderer>, kOverlayCount> renderers;
        for (int overlay = 0; overlay < kOverlayCount; ++overlay) {
            QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(37 + overlay, 83, 129));
            layers.append({std::move(image), bounds, bounds});
            canvases[overlay] = std::make_unique<SnowCanvasWidget>(fixture.runtime);
            canvases[overlay]->resize(surfaceSize);
            renderers[overlay] = std::make_unique<ScreenshotCanvasRenderer>(*canvases[overlay]);
        }
        ScenarioResult result;
        result.name = name;
        for (int frame = 0; frame < warmup + iterations; ++frame) {
            QElapsedTimer timer;
            timer.start();
            const ScreenshotImageSource source = ScreenshotImageSource::fromLayers(layers);
            for (auto& current : renderers)
                current->setImageSource(shared ? source
                                               : ScreenshotImageSource::fromLayers(layers));
            if (frame >= warmup) {
                FrameSample sample;
                sample.milliseconds = static_cast<double>(timer.nsecsElapsed()) / 1e6;
                sample.mutationMs = sample.milliseconds;
                result.samples.push_back(sample);
            }
        }
        auto report = summarize(result, {}, {});
        report.insert(
            QStringLiteral("measurement"),
            QStringLiteral("Source creation plus setImageSource for three preconstructed "
                           "overlays sharing three opaque alpha-format capture buffers; "
                           "excludes capture, widget creation, event delivery, and paint."));
        report.insert(QStringLiteral("devicePixelRatio"), dpr);
        report.insert(QStringLiteral("overlayCount"), kOverlayCount);
        report.insert(QStringLiteral("sourceBufferCount"), kOverlayCount);
        reports.append(report);
    };
    runSourceStartup(QStringLiteral("shared-source-startup"), true);
    runSourceStartup(QStringLiteral("separate-source-startup"), false);

    QVector<QPointF> freehand;
    freehand.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        const qreal angle = i * 6.283185307179586 / 9999.0;
        freehand.append(
            QPointF(700 * std::cos(angle), 450 * std::sin(angle) + 4 * std::sin(angle * 75)));
    }
    const auto simplified = simplifyScreenshotRegionPoints(freehand, 0.25);
    QPainterPath freehandPath;
    freehandPath.addPolygon(QPolygonF(simplified));
    freehandPath.closeSubpath();
    const auto freehandRegion =
        ScreenshotRegionGeometry::fromPath(freehandPath, ScreenshotRegionType::Freehand);
    QVector<QPointF> curveVertices;
    for (int i = 0; i < 128; ++i) {
        const qreal angle = i * 6.283185307179586 / 128.0;
        const qreal radius = 500 + 60 * std::sin(angle * 9);
        curveVertices.append(QPointF(radius * std::cos(angle), radius * std::sin(angle)));
    }
    const auto curveRegion = ScreenshotRegionGeometry::fromPath(
        snowCanvasCatmullRomPath(curveVertices, true), ScreenshotRegionType::Curve);
    run(QStringLiteral("custom-freehand"), [&](int index) {
        auto points = simplified;
        points.last() += QPointF(index & 1, 0);
        QPainterPath path;
        path.addPolygon(QPolygonF(points));
        path.closeSubpath();
        renderer.setSelectionRegion(
            ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Freehand), {}, {}, false,
            Qt::red);
    });
    run(QStringLiteral("custom-curve"), [&](int index) {
        auto points = curveVertices;
        points.last() += QPointF(index & 1, 0);
        renderer.setSelectionRegion(
            ScreenshotRegionGeometry::fromPath(snowCanvasCatmullRomPath(points, true),
                                               ScreenshotRegionType::Curve),
            {}, {}, false, Qt::red);
    });
    auto mixed = curveRegion;
    for (int i = 0; i < 12; ++i)
        mixed = mixed.subtracted(QRect(-300 + i * 45, -250, 20, 400));
    run(QStringLiteral("custom-mixed-operations"), [&](int index) {
        const auto candidate = mixed.united(freehandRegion.translated(index & 1, 0));
        renderer.setSelectionRegion(candidate, mixed, {}, false, Qt::red);
    });
    QVector<QPointF> growingVertices;
    growingVertices.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        const qreal angle = i * 6.283185307179586 / 4096.0;
        growingVertices.append(QPointF(620 * std::cos(angle), 390 * std::sin(angle)));
    }
    run(QStringLiteral("custom-growing-draft"), [&](int index) {
        const int count = 2048 + index % 2048;
        const auto path = snowCanvasCatmullRomPath(growingVertices.mid(0, count), true);
        ScreenshotSelectionVisualState state;
        state.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Freehand);
        state.bounds = state.region->boundingRect();
        state.present = true;
        state.draftPath = path;
        renderer.applySelectionState(state);
    });
    QRegion manyRegions;
    for (int row = 0; row < 10; ++row)
        for (int column = 0; column < 20; ++column)
            manyRegions += QRect(-1250 + column * 125, -650 + row * 130, 82, 82);
    run(QStringLiteral("rounded-many-regions"), [&](int index) {
        ScreenshotSelectionVisualState state;
        const ScreenshotRegionGeometry region(manyRegions.translated(index & 1, 0));
        state.region = region;
        state.confirmedRegion = region;
        state.bounds = region.boundingRect();
        state.present = true;
        state.cornerRadius = 8;
        renderer.applySelectionState(state);
    });
    QImage sparseContent(surfaceSize, QImage::Format_ARGB32_Premultiplied);
    sparseContent.fill(Qt::white);
    ScreenshotResultStyle sparseStyle;
    sparseStyle.region =
        QRegion(QRect(40, 40, 120, 120)) +
        QRegion(QRect(surfaceSize.width() - 160, surfaceSize.height() - 160, 120, 120));
    sparseStyle.shadowWidth = 16;
    run(QStringLiteral("sparse-region-export"), [&](int index) {
        sparseStyle.shadowColor = (index & 1) ? QColor(0x33, 0x33, 0x33) : QColor(0x34, 0x33, 0x33);
        const QImage result = ScreenshotResultCompositor::compose(sparseContent, sparseStyle);
        if (result.isNull())
            throw std::runtime_error("sparse region export failed");
    });
    ScreenshotResultStyle denseStyle;
    QPainterPath densePath;
    densePath.addEllipse(QRectF(0, 0, surfaceSize.width(), surfaceSize.height()));
    denseStyle.region = ScreenshotRegionGeometry::fromPath(densePath, ScreenshotRegionType::Curve);
    denseStyle.shadowWidth = 16;
    run(QStringLiteral("dense-region-export"), [&](int index) {
        denseStyle.shadowColor = (index & 1) ? QColor(0x33, 0x33, 0x33) : QColor(0x34, 0x33, 0x33);
        if (ScreenshotResultCompositor::compose(sparseContent, denseStyle).isNull())
            throw std::runtime_error("dense region export failed");
    });
    run(QStringLiteral("custom-alternating-scale"), [&](int) {
        for (const qreal scale : {1.0, 1.5, 2.0}) {
            if (mixed.path(scale).isEmpty())
                throw std::runtime_error("scaled region contour failed");
        }
    });
    run(QStringLiteral("region-one-pixel-move"), [&](int index) {
        const auto moved = compoundRegion.translated(index & 1, 0);
        renderer.setSelectionRegion(moved, moved, {}, false, Qt::red);
    });
    run(QStringLiteral("region-hover-one-pixel-move"), [&](int index) {
        const auto moved = compoundRegion.translated(index & 1, 0);
        renderer.setSelectionRegion(moved, moved, {}, false, Qt::red);
        renderer.setSelectionToolbarHovered(true);
    });
    run(QStringLiteral("region-two-stage-presentation"), [&](int index) {
        const auto moved = compoundRegion.translated(index & 1, 0);
        renderer.setSelection(QRectF(moved.boundingRect()), false, 0, 16, shadowColor);
        renderer.setSelectionRegion(moved, moved, {}, false, Qt::red);
        renderer.setSelectionToolbarHovered(false);
    });
    run(QStringLiteral("region-one-pass-presentation"), [&](int index) {
        const auto moved = compoundRegion.translated(index & 1, 0);
        ScreenshotSelectionVisualState state;
        state.bounds = QRectF(moved.boundingRect());
        state.present = true;
        state.handlesVisible = false;
        state.shadowWidth = 16;
        state.shadowColor = shadowColor;
        state.region = moved;
        state.confirmedRegion = moved;
        state.dangerColor = Qt::red;
        renderer.applySelectionState(state);
    });
    QElapsedTimer commitTimer;
    commitTimer.start();
    const auto committed = mixed.united(freehandRegion);
    const auto commitPath = committed.path();
    const double commitMs = static_cast<double>(commitTimer.nsecsElapsed()) / 1'000'000.0;
    for (auto& report : reports) {
        report.insert(QStringLiteral("freehandInputPoints"), freehand.size());
        report.insert(QStringLiteral("freehandRetainedPoints"), simplified.size());
        report.insert(QStringLiteral("mixedCommitMs"), commitMs);
        report.insert(QStringLiteral("mixedSerializedBytes"),
                      QJsonDocument(committed.toJson()).toJson(QJsonDocument::Compact).size());
        report.insert(QStringLiteral("mixedDerivedPathElements"), commitPath.elementCount());
        report.insert(QStringLiteral("geometryRetainedBytesEstimate"),
                      committed.retainedBytesEstimate());
    }
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
    const auto runFilterResize = [&](const QString& name, SnowCanvasFilterType type,
                                     bool intersectsSelectionEdge) {
        if (parser.isSet(QStringLiteral("scenario")) &&
            !parser.values(QStringLiteral("scenario")).contains(name)) {
            return;
        }
        // Fresh runtime, renderer, and canvas keep annotations and decoration state
        // independent of the legacy spotlight/watermark scenarios and each other.
        BenchmarkFixture filterFixture(surfaceSize, translucent);
        applyResizeSelection(filterFixture, baseSelection);
        const qreal dpr = filterFixture.canvas->devicePixelRatioF();
        const qreal tileSize = snow_canvas_filter_tile_cache::kTilePhysicalSize / dpr;
        const QRectF safeInterior = filterFixture.canvas->canvasToViewTransform()
                                        .mapRect(baseSelection)
                                        .adjusted(32.0, 32.0, -32.0, -32.0);
        // Fit entirely inside tiles untouched by the selection border/handles. The
        // interior scenario can therefore verify zero dispatches after tile culling.
        const qreal left = std::ceil(safeInterior.left() / tileSize) * tileSize;
        const qreal top = std::ceil(safeInterior.top() / tileSize) * tileSize;
        const qreal right = std::floor(safeInterior.right() / tileSize) * tileSize;
        const qreal bottom = std::floor(safeInterior.bottom() / tileSize) * tileSize;
        if (!intersectsSelectionEdge && (right <= left || bottom <= top)) {
            reports.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("available"), false},
                {QStringLiteral("sampleCount"), 0},
                {QStringLiteral("unavailableReason"),
                 QStringLiteral("selection has no interior tile separated from border damage")},
            });
            return;
        }
        const QRectF filterRect =
            intersectsSelectionEdge
                ? QRectF(baseSelection.right() - baseSelection.width() * 0.15,
                         baseSelection.top() + baseSelection.height() * 0.15,
                         baseSelection.width() * 0.3, baseSelection.height() * 0.7)
                : filterFixture.canvas->canvasToViewTransform().inverted().mapRect(
                      QRectF(QPointF(left, top), QPointF(right, bottom))
                          .adjusted(16.0 / dpr, 16.0 / dpr, -16.0 / dpr, -16.0 / dpr));
        createRectangleFilter(filterFixture, type, filterRect);
        applyResizeSelection(filterFixture, baseSelection);
        QApplication::processEvents();
        const DwmSnapshot before = dwmSnapshot(filterFixture.window);
        const ScenarioResult result =
            runScenario(filterFixture, name, warmup, iterations, [&](int index) {
                applyResizeSelection(filterFixture,
                                     QRectF(baseSelection.left(), baseSelection.top(),
                                            baseSelection.width() + (index & 1),
                                            baseSelection.height() + ((index >> 1) & 1)));
            });
        auto report = summarize(result, before, dwmSnapshot(filterFixture.window));
        report.insert(QStringLiteral("filterType"), intersectsSelectionEdge
                                                        ? QStringLiteral("gaussian-blur")
                                                        : QStringLiteral("mosaic"));
        report.insert(QStringLiteral("filterIntersectsSelectionEdge"), intersectsSelectionEdge);
        report.insert(QStringLiteral("selectionWidth"), baseSelection.width());
        report.insert(QStringLiteral("selectionHeight"), baseSelection.height());
        report.insert(QStringLiteral("filterWidth"), filterRect.width());
        report.insert(QStringLiteral("filterHeight"), filterRect.height());
        report.insert(QStringLiteral("devicePixelRatio"),
                      filterFixture.canvas->devicePixelRatioF());
        report.insert(QStringLiteral("canvasWidth"), filterFixture.canvas->width());
        report.insert(QStringLiteral("canvasHeight"), filterFixture.canvas->height());
        reports.append(report);
    };
    runFilterResize(QStringLiteral("resize-filter-interior"), SnowCanvasFilterType::Mosaic, false);
    runFilterResize(QStringLiteral("resize-filter-edge"), SnowCanvasFilterType::GaussianBlur, true);

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
        renderer.setSelection(baseSelection, true, 16,
                              widths[static_cast<std::size_t>(index) % widths.size()], shadowColor);
        renderer.setSelectionToolbarHovered(true);
    });
    run(QStringLiteral("rounded-shadow-toggle"), [&](int index) {
        renderer.setSelection(baseSelection, false, 18, (index & 1) ? 10 : 0, shadowColor);
        renderer.setSelectionToolbarHovered(true);
    });

    const QPointF guideLineCenter(surfaceSize.width() / 2.0, surfaceSize.height() / 2.0);
    const QColor cursorGuideLineColor(220, 30, 40);
    const QColor monitorGuideLineColor(30, 80, 220);
    const auto runGuide = [&](const QString& name,
                              const std::function<void(BenchmarkFixture&)>& prepare,
                              const std::function<void(BenchmarkFixture&, int)>& mutation) {
        if (parser.isSet(QStringLiteral("scenario")) &&
            !parser.values(QStringLiteral("scenario")).contains(name)) {
            return;
        }
        // Dedicated fixtures prevent guide settings and annotation/layout caches
        // leaking between scenarios, even when only one scenario is requested.
        BenchmarkFixture guideFixture(surfaceSize, translucent);
        createGuideTextAnnotations(guideFixture, guideTextAnnotations);
        prepare(guideFixture);
        mutation(guideFixture, 3);
        QApplication::processEvents();
        const DwmSnapshot before = dwmSnapshot(guideFixture.window);
        const ScenarioResult result =
            runScenario(guideFixture, name, warmup, iterations,
                        [&](int index) { mutation(guideFixture, index + 4); });
        auto report = summarize(result, before, dwmSnapshot(guideFixture.window));
        report.insert(QStringLiteral("canvasWidth"), guideFixture.canvas->width());
        report.insert(QStringLiteral("canvasHeight"), guideFixture.canvas->height());
        report.insert(QStringLiteral("devicePixelRatio"), guideFixture.canvas->devicePixelRatioF());
        report.insert(QStringLiteral("guideTextAnnotations"), guideTextAnnotations);
        report.insert(QStringLiteral("translucentWindow"), translucent);
        reports.append(report);
    };
    const auto noPreparation = [](BenchmarkFixture&) {};
    runGuide(QStringLiteral("cursor-and-monitor-guide-lines"), noPreparation,
             [&](BenchmarkFixture& guideFixture, int index) {
                 guideFixture.renderer->setGuideLines(guideLineCenter +
                                                          QPointF(index & 1, (index >> 1) & 1),
                                                      cursorGuideLineColor, monitorGuideLineColor);
             });
    // Moving away from the fixed monitor center exposes the full dirty strips.
    // The legacy one-pixel scenario above overlaps the two crosshairs and hides
    // whether the native presentation area expands for diagonal motion.
    const auto largeMotion = [&](const QString& name, bool moveX, bool moveY) {
        runGuide(name, noPreparation, [&](BenchmarkFixture& guideFixture, int index) {
            const qreal amount = (index & 1) ? 0.75 : 0.25;
            const QPointF position(surfaceSize.width() * (moveX ? amount : 0.35),
                                   surfaceSize.height() * (moveY ? amount : 0.35));
            guideFixture.renderer->setGuideLines(position, cursorGuideLineColor,
                                                 monitorGuideLineColor);
        });
    };
    largeMotion(QStringLiteral("cursor-guide-horizontal-move"), true, false);
    largeMotion(QStringLiteral("cursor-guide-vertical-move"), false, true);
    largeMotion(QStringLiteral("cursor-guide-diagonal-move"), true, true);
    runGuide(
        QStringLiteral("cursor-guide-complex-selection"),
        [&](BenchmarkFixture& guideFixture) {
            const auto region = mixed.united(freehandRegion);
            guideFixture.renderer->setSelectionRegion(region, region, {}, false, Qt::red);
        },
        [&](BenchmarkFixture& guideFixture, int index) {
            const qreal amount = (index & 1) ? 0.75 : 0.25;
            guideFixture.renderer->setGuideLines(
                QPointF(surfaceSize.width() * amount, surfaceSize.height() * amount),
                cursorGuideLineColor, monitorGuideLineColor);
        });
    runGuide(QStringLiteral("monitor-center-guide-line-only"), noPreparation,
             [&](BenchmarkFixture& guideFixture, int index) {
                 guideFixture.renderer->setGuideLines(guideLineCenter +
                                                          QPointF(index & 1, (index >> 1) & 1),
                                                      Qt::transparent, monitorGuideLineColor);
             });
    const auto prepareSnap = [](BenchmarkFixture& guideFixture) {
        SnowCanvasSnapConfig config;
        config.enabled = true;
        if (!guideFixture.canvas->setCanvasSnapConfig(config)) {
            throw std::runtime_error("unable to configure benchmark snap guides");
        }
    };
    const auto snapTargets = [&](int index) {
        const qreal offset = index & 1;
        return SnowCanvasSnapGuideTargets{{baseSelection.center().x() + offset, 240.0},
                                          {baseSelection.center().y() + offset, 180.0}};
    };
    runGuide(QStringLiteral("snap-guide-targets-unchanged"), prepareSnap,
             [&](BenchmarkFixture& guideFixture, int) {
                 if (!guideFixture.canvas->setCanvasSnapGuideTargets(snapTargets(0))) {
                     throw std::runtime_error("unable to apply unchanged benchmark snap targets");
                 }
             });
    runGuide(QStringLiteral("snap-guide-targets-changed"), prepareSnap,
             [&](BenchmarkFixture& guideFixture, int index) {
                 if (!guideFixture.canvas->setCanvasSnapGuideTargets(snapTargets(index))) {
                     throw std::runtime_error("unable to apply changed benchmark snap targets");
                 }
             });

    const auto prepareSelectionCenterGuide = [](BenchmarkFixture& guideFixture) {
        guideFixture.renderer->setSelectionCenterGuideLineColor(QColor(0x40, 0x96, 0xff));
    };
    runGuide(QStringLiteral("selection-center-guide-horizontal-move"), prepareSelectionCenterGuide,
             [&](BenchmarkFixture& guideFixture, int index) {
                 guideFixture.renderer->setSelection(baseSelection.translated(index & 1, 0), false);
             });
    runGuide(QStringLiteral("selection-center-guide-vertical-move"), prepareSelectionCenterGuide,
             [&](BenchmarkFixture& guideFixture, int index) {
                 guideFixture.renderer->setSelection(baseSelection.translated(0, index & 1), false);
             });
    runGuide(QStringLiteral("selection-center-guide-fixed-center-resize"),
             prepareSelectionCenterGuide, [&](BenchmarkFixture& guideFixture, int index) {
                 const qreal offset = index & 1;
                 guideFixture.renderer->setSelection(
                     baseSelection.adjusted(-offset, -offset, offset, offset), false);
             });

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
    const bool crossMonitorRequested = !parser.isSet(QStringLiteral("scenario")) ||
                                       parser.values(QStringLiteral("scenario"))
                                           .contains(QStringLiteral("cross-monitor-selection"));
    if (hasSecondScreen && crossMonitorRequested) {
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
            parser.value(QStringLiteral("html")), surfaceSize, canvas.size(),
            canvas.devicePixelRatioF(), translucent);
        QTextStream(stdout) << QJsonDocument(summary).toJson(QJsonDocument::Indented);
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 3;
    }
}
