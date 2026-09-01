#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kDefaultWarmupIterations = 10;
constexpr int kDefaultMeasuredIterations = 300;
constexpr int kPreviewBurstSize = 16;

enum class Suite {
    All,
    Renderer,
    Workflow,
};

enum class RendererMutation {
    None,
    ColdCache,
    Color,
    Angle,
    Text,
    FontSize,
    Gap,
};

struct Options {
    Suite suite = Suite::All;
    std::string scenario;
    int warmupIterations = kDefaultWarmupIterations;
    int measuredIterations = kDefaultMeasuredIterations;
    std::string csvPath;
    bool list = false;
    bool help = false;
};

struct Statistics {
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double minimumMs = 0.0;
    double maximumMs = 0.0;
    double standardDeviationMs = 0.0;
};

struct Result {
    std::string suite;
    std::string scenario;
    std::string operation;
    std::string cacheMode;
    int logicalWidth = 0;
    int logicalHeight = 0;
    int physicalWidth = 0;
    int physicalHeight = 0;
    double devicePixelRatio = 1.0;
    int renderAreaWidth = 0;
    int renderAreaHeight = 0;
    int textBytes = 0;
    double fontSize = 0.0;
    double angle = 0.0;
    double gap = 0.0;
    int batchSize = 1;
    int samples = 0;
    Statistics statistics;
    double operationsPerSecond = 0.0;
    double framesPerSecond = 0.0;
    double megapixelsPerSecond = 0.0;
    std::uint64_t checksum = 0;
    std::string resolvedFont;
    snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
};

struct RendererCase {
    std::string name;
    std::string description;
    std::string cacheMode;
    int logicalWidth = 1920;
    int logicalHeight = 1080;
    double devicePixelRatio = 1.0;
    QRectF renderArea;
    QString text = QStringLiteral("SNOW SHOT");
    double fontSize = 18.0;
    double angle = 30.0;
    double gap = 56.0;
    double opacity = 0.30;
    RendererMutation mutation = RendererMutation::None;
    bool expectVisible = true;
    bool expectFallback = false;
    bool expectSegmented = false;
};

using Runner = std::function<std::optional<Result>(const Options&, std::string&)>;

struct Scenario {
    std::string name;
    Suite suite;
    std::string description;
    Runner run;
};

std::string_view suiteName(Suite suite) {
    switch (suite) {
    case Suite::All:
        return "all";
    case Suite::Renderer:
        return "renderer";
    case Suite::Workflow:
        return "workflow";
    }
    return "unknown";
}

std::string_view strategyName(snow_canvas_renderer::WatermarkRenderStrategy strategy) {
    using Strategy = snow_canvas_renderer::WatermarkRenderStrategy;
    switch (strategy) {
    case Strategy::None:
        return "none";
    case Strategy::SparsePixmap:
        return "sparse_pixmap";
    case Strategy::SparseImage:
        return "sparse_image";
    case Strategy::DenseCell:
        return "dense_cell";
    case Strategy::SegmentedSparse:
        return "segmented_sparse";
    case Strategy::GlyphFallback:
        return "glyph_fallback";
    }
    return "unknown";
}

bool suiteMatches(Suite selected, Suite scenario) {
    return selected == Suite::All || selected == scenario;
}

bool parsePositiveInt(const char* text, int& value) {
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(text, &consumed);
        if (consumed != std::strlen(text) || parsed <= 0 ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Options> parseOptions(int argc, char** argv, std::string& error) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--suite") {
            if (++index >= argc) {
                error = "--suite requires all, renderer, or workflow";
                return std::nullopt;
            }
            const std::string_view value(argv[index]);
            if (value == "all") {
                options.suite = Suite::All;
            } else if (value == "renderer") {
                options.suite = Suite::Renderer;
            } else if (value == "workflow") {
                options.suite = Suite::Workflow;
            } else {
                error = "invalid --suite value: " + std::string(value);
                return std::nullopt;
            }
        } else if (argument == "--scenario") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                error = "--scenario requires an exact scenario name";
                return std::nullopt;
            }
            options.scenario = argv[index];
        } else if (argument == "--warmup") {
            if (++index >= argc || !parsePositiveInt(argv[index], options.warmupIterations)) {
                error = "--warmup requires a positive integer";
                return std::nullopt;
            }
        } else if (argument == "--iterations") {
            if (++index >= argc || !parsePositiveInt(argv[index], options.measuredIterations)) {
                error = "--iterations requires a positive integer";
                return std::nullopt;
            }
        } else if (argument == "--csv") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                error = "--csv requires a path";
                return std::nullopt;
            }
            options.csvPath = argv[index];
        } else {
            error = "unknown argument: " + std::string(argument);
            return std::nullopt;
        }
    }
    return options;
}

void printUsage(std::ostream& out, const char* program) {
    out << "Detailed Snow Draw Engine watermark benchmark\n\n"
        << "Usage: " << program << " [options]\n\n"
        << "  --suite <all|renderer|workflow>  Select a suite (default: all)\n"
        << "  --scenario <exact-name>          Run one scenario\n"
        << "  --warmup <count>                 Warmup samples (default: 10)\n"
        << "  --iterations <count>             Measured samples (default: 300)\n"
        << "  --csv <path>                     Write versioned CSV results\n"
        << "  --list                           List scenarios without running them\n"
        << "  --help, -h                       Show this help\n";
}

Statistics calculateStatistics(const std::vector<double>& samples) {
    Statistics result;
    if (samples.empty()) {
        return result;
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double fraction) {
        const std::size_t index = std::min(
            sorted.size() - 1, static_cast<std::size_t>(std::ceil(sorted.size() * fraction) - 1.0));
        return sorted[index];
    };
    result.meanMs =
        std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    result.p50Ms = percentile(0.50);
    result.p95Ms = percentile(0.95);
    result.p99Ms = percentile(0.99);
    result.minimumMs = sorted.front();
    result.maximumMs = sorted.back();
    double squaredDifferenceSum = 0.0;
    for (double sample : samples) {
        const double difference = sample - result.meanMs;
        squaredDifferenceSum += difference * difference;
    }
    result.standardDeviationMs =
        std::sqrt(squaredDifferenceSum / static_cast<double>(samples.size()));
    return result;
}

double operationsPerSecond(double milliseconds, int operations = 1) {
    return milliseconds > 0.0 ? operations * 1000.0 / milliseconds : 0.0;
}

std::uint64_t imageChecksum(const QImage& image) {
    std::uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < image.height(); ++y) {
        const unsigned char* line = image.constScanLine(y);
        for (qsizetype index = 0; index < image.bytesPerLine(); ++index) {
            hash ^= line[index];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool hasVisiblePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 0) {
                return true;
            }
        }
    }
    return false;
}

std::uint64_t byteChecksum(const QByteArray& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (char value : bytes) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool waitForPreviewApplication(int& appliedCount, int expectedCount) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (appliedCount < expectedCount && elapsed.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return appliedCount == expectedCount;
}

void accumulateDiagnostics(snow_canvas_renderer::WatermarkRenderDiagnostics& target,
                           const snow_canvas_renderer::WatermarkRenderDiagnostics& source) {
    target.renderCallCount += source.renderCallCount;
    target.earlyExitCount += source.earlyExitCount;
    target.cacheEvictionCount += source.cacheEvictionCount;
    target.shapeHitCount += source.shapeHitCount;
    target.shapeMissCount += source.shapeMissCount;
    target.unitHitCount += source.unitHitCount;
    target.unitMissCount += source.unitMissCount;
    target.tintBuildCount += source.tintBuildCount;
    target.repeatCellBuildCount += source.repeatCellBuildCount;
    target.sparseBatchCount += source.sparseBatchCount;
    target.submittedFragmentCount += source.submittedFragmentCount;
    target.culledFragmentCount += source.culledFragmentCount;
    target.segmentedChunkCount += source.segmentedChunkCount;
    target.denseFillCount += source.denseFillCount;
    target.fallbackGlyphDrawCount += source.fallbackGlyphDrawCount;
    target.cacheBytes = std::max(target.cacheBytes, source.cacheBytes);
    target.fragmentCoverage += source.fragmentCoverage;
    target.shapeMilliseconds += source.shapeMilliseconds;
    target.rasterMilliseconds += source.rasterMilliseconds;
    target.tintMilliseconds += source.tintMilliseconds;
    target.placementMilliseconds += source.placementMilliseconds;
    target.compositionMilliseconds += source.compositionMilliseconds;
    target.selectedStrategy = source.selectedStrategy;
    target.renderedLogicalBounds = source.renderedLogicalBounds;
    target.renderedDeviceBounds = source.renderedDeviceBounds;
}

QString resolvedBenchmarkFont() {
    QFont font(QStringLiteral("Segoe UI"));
    return QFontInfo(font).family();
}

void copyUtf8(const QString& value, std::array<char, SNOW_WATERMARK_TEXT_CAPACITY>& target,
              std::uint16_t& length) {
    const QByteArray utf8 = value.toUtf8().left(static_cast<int>(target.size()));
    target.fill(0);
    std::copy(utf8.begin(), utf8.end(), target.begin());
    length = static_cast<std::uint16_t>(utf8.size());
}

void copyFontFamily(const QString& value, WatermarkDisplayInfo& info) {
    const QByteArray utf8 =
        value.toUtf8().left(static_cast<int>(info.watermark_font_family.size()));
    info.watermark_font_family.fill(0);
    std::copy(utf8.begin(), utf8.end(), info.watermark_font_family.begin());
    info.watermark_font_family_len = static_cast<std::uint16_t>(utf8.size());
}

WatermarkDisplayInfo makeDisplayInfo(const RendererCase& testCase) {
    WatermarkDisplayInfo info{};
    info.surface_width = testCase.logicalWidth;
    info.surface_height = testCase.logicalHeight;
    info.watermark_color = SnowColorRgba8{30, 80, 160, 255};
    copyUtf8(testCase.text, info.watermark_text, info.watermark_text_len);
    info.watermark_font_size = testCase.fontSize;
    copyFontFamily(resolvedBenchmarkFont(), info);
    info.watermark_angle = testCase.angle;
    info.watermark_gap = testCase.gap;
    info.watermark_opacity = testCase.opacity;
    return info;
}

void applyRendererMutation(WatermarkDisplayInfo& info, const RendererCase& testCase, int sequence) {
    const bool alternate = (sequence & 1) != 0;
    switch (testCase.mutation) {
    case RendererMutation::None:
    case RendererMutation::ColdCache:
        break;
    case RendererMutation::Color:
        info.watermark_color =
            alternate ? SnowColorRgba8{190, 35, 70, 255} : SnowColorRgba8{30, 80, 160, 255};
        info.watermark_opacity = alternate ? 0.48 : 0.30;
        break;
    case RendererMutation::Angle:
        info.watermark_angle = alternate ? -45.0 : 15.0;
        break;
    case RendererMutation::Text:
        copyUtf8(alternate ? QStringLiteral("SNOW SHOT B") : QStringLiteral("SNOW SHOT A"),
                 info.watermark_text, info.watermark_text_len);
        break;
    case RendererMutation::FontSize:
        info.watermark_font_size = alternate ? 30.0 : 14.0;
        break;
    case RendererMutation::Gap:
        info.watermark_gap = alternate ? 90.0 : 20.0;
        break;
    }
}

bool validateRendererDiagnostics(const RendererCase& testCase, const Result& result,
                                 std::string& error) {
    const std::size_t samples = static_cast<std::size_t>(result.samples);
    const auto& diagnostics = result.diagnostics;
    if (diagnostics.renderCallCount != samples) {
        error = testCase.name + ": renderer call count did not match measured samples";
        return false;
    }
    if (!testCase.expectVisible) {
        if (diagnostics.earlyExitCount != samples ||
            diagnostics.selectedStrategy != snow_canvas_renderer::WatermarkRenderStrategy::None) {
            error = testCase.name + ": hidden watermark did not remain on the early-exit path";
            return false;
        }
        return true;
    }
    if (testCase.expectFallback) {
        if (diagnostics.fallbackGlyphDrawCount == 0 ||
            diagnostics.selectedStrategy !=
                snow_canvas_renderer::WatermarkRenderStrategy::GlyphFallback) {
            error = testCase.name + ": expected direct-text fallback diagnostics were not observed";
            return false;
        }
        return true;
    }
    if (testCase.expectSegmented &&
        (diagnostics.segmentedChunkCount == 0 ||
         diagnostics.selectedStrategy !=
             snow_canvas_renderer::WatermarkRenderStrategy::SegmentedSparse)) {
        error = testCase.name + ": expected segmented-unit diagnostics were not observed";
        return false;
    }
    if (diagnostics.denseFillCount == 0 && diagnostics.sparseBatchCount == 0) {
        error = testCase.name + ": no adaptive composition strategy was recorded";
        return false;
    }
    const bool rebuildEverySample = testCase.mutation == RendererMutation::ColdCache ||
                                    testCase.mutation == RendererMutation::Text ||
                                    testCase.mutation == RendererMutation::FontSize;
    if (rebuildEverySample) {
        if (diagnostics.shapeMissCount != samples || diagnostics.unitMissCount != samples ||
            diagnostics.tintBuildCount != samples) {
            error =
                testCase.name + ": cache-invalidating samples did not rebuild every cache layer";
            return false;
        }
    } else if (testCase.mutation == RendererMutation::Color) {
        if (diagnostics.shapeMissCount != 0 || diagnostics.unitMissCount != 0 ||
            diagnostics.tintBuildCount != samples) {
            error = testCase.name + ": recoloring did not rebuild only the tint cache";
            return false;
        }
    } else if (diagnostics.shapeMissCount != 0 || diagnostics.unitMissCount != 0 ||
               diagnostics.tintBuildCount != 0) {
        error = testCase.name + ": warm samples unexpectedly rebuilt a watermark cache";
        return false;
    }
    return true;
}

std::optional<Result> runRendererCase(const Options& options, const RendererCase& testCase,
                                      std::string& error) {
    const QSize physicalSize(qRound(testCase.logicalWidth * testCase.devicePixelRatio),
                             qRound(testCase.logicalHeight * testCase.devicePixelRatio));
    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(testCase.devicePixelRatio);
    if (image.isNull()) {
        error = testCase.name + ": could not allocate render target";
        return std::nullopt;
    }
    const QRectF renderArea = testCase.renderArea.isValid() && !testCase.renderArea.isEmpty()
                                  ? testCase.renderArea
                                  : QRectF(0.0, 0.0, testCase.logicalWidth, testCase.logicalHeight);
    WatermarkDisplayInfo info = makeDisplayInfo(testCase);
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();

    const auto renderSample = [&](int sequence, bool measured, double* milliseconds,
                                  snow_canvas_renderer::WatermarkRenderDiagnostics* diagnostics) {
        applyRendererMutation(info, testCase, sequence);
        if (testCase.mutation == RendererMutation::ColdCache ||
            testCase.mutation == RendererMutation::Text ||
            testCase.mutation == RendererMutation::FontSize) {
            snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
        }
        image.fill(Qt::transparent);
        QPainter painter(&image);
        snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        snow_canvas_renderer::renderWatermark(painter, info, renderArea);
        const qint64 elapsed = timer.nsecsElapsed();
        painter.end();
        if (measured) {
            *milliseconds = elapsed / 1'000'000.0;
            *diagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
        }
    };

    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignoredMilliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics ignoredDiagnostics;
        renderSample(iteration, false, &ignoredMilliseconds, &ignoredDiagnostics);
    }

    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    snow_canvas_renderer::WatermarkRenderDiagnostics aggregateDiagnostics;
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
        renderSample(options.warmupIterations + iteration, true, &milliseconds, &diagnostics);
        samples.push_back(milliseconds);
        accumulateDiagnostics(aggregateDiagnostics, diagnostics);
    }

    if (hasVisiblePixel(image) != testCase.expectVisible) {
        error = testCase.name + ": rendered visibility did not match the scenario contract";
        return std::nullopt;
    }
    Result result;
    result.suite = "renderer";
    result.scenario = testCase.name;
    result.operation = "paint";
    result.cacheMode = testCase.cacheMode;
    result.logicalWidth = testCase.logicalWidth;
    result.logicalHeight = testCase.logicalHeight;
    result.physicalWidth = physicalSize.width();
    result.physicalHeight = physicalSize.height();
    result.devicePixelRatio = testCase.devicePixelRatio;
    result.renderAreaWidth = qRound(renderArea.width());
    result.renderAreaHeight = qRound(renderArea.height());
    result.textBytes = info.watermark_text_len;
    result.fontSize = info.watermark_font_size;
    result.angle = info.watermark_angle;
    result.gap = info.watermark_gap;
    result.samples = options.measuredIterations;
    result.statistics = calculateStatistics(samples);
    result.operationsPerSecond = operationsPerSecond(result.statistics.p50Ms);
    result.framesPerSecond = result.operationsPerSecond;
    const double renderedMegapixels = renderArea.width() * renderArea.height() *
                                      testCase.devicePixelRatio * testCase.devicePixelRatio /
                                      1'000'000.0;
    result.megapixelsPerSecond = renderedMegapixels * result.framesPerSecond;
    result.checksum = imageChecksum(image);
    result.resolvedFont = resolvedBenchmarkFont().toStdString();
    result.diagnostics = aggregateDiagnostics;
    if (!validateRendererDiagnostics(testCase, result, error)) {
        return std::nullopt;
    }
    return result;
}

SnowCanvasWatermarkConfig makeWorkflowConfig(const QString& text) {
    SnowCanvasWatermarkConfig config;
    config.color = QColor(30, 80, 160, 255);
    config.text = text;
    config.fontSize = 18.0;
    config.fontFamily = resolvedBenchmarkFont();
    config.angle = 30.0;
    config.gap = 56.0;
    config.opacity = 0.30;
    return config;
}

Result makeWorkflowResult(const Options& options, const std::string& scenario,
                          const std::string& operation, const std::vector<double>& samples,
                          int batchSize = 1) {
    Result result;
    result.suite = "workflow";
    result.scenario = scenario;
    result.operation = operation;
    result.cacheMode = "engine";
    result.batchSize = batchSize;
    result.samples = options.measuredIterations;
    result.statistics = calculateStatistics(samples);
    result.operationsPerSecond = operationsPerSecond(result.statistics.p50Ms, batchSize);
    result.resolvedFont = resolvedBenchmarkFont().toStdString();
    return result;
}

std::optional<Result> runCommitWorkflow(const Options& options, std::string& error) {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    if (!runtime.isValid()) {
        error = "workflow_commit_alternating: could not initialize runtime";
        return std::nullopt;
    }
    const SnowCanvasWatermarkConfig configs[] = {
        makeWorkflowConfig(QStringLiteral("COMMIT A")),
        makeWorkflowConfig(QStringLiteral("COMMIT B")),
    };
    const auto sample = [&](int sequence, double* milliseconds) {
        QElapsedTimer timer;
        timer.start();
        const bool success = canvas.setCanvasWatermarkConfig(configs[sequence & 1]);
        *milliseconds = timer.nsecsElapsed() / 1'000'000.0;
        return success;
    };
    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignored = 0.0;
        if (!sample(iteration, &ignored)) {
            error = "workflow_commit_alternating: warmup commit failed";
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        if (!sample(options.warmupIterations + iteration, &milliseconds)) {
            error = "workflow_commit_alternating: measured commit failed";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
    }
    Result result = makeWorkflowResult(options, "workflow_commit_alternating", "commit", samples);
    result.textBytes = canvas.canvasWatermarkConfig().text.toUtf8().size();
    result.checksum = byteChecksum(canvas.canvasWatermarkConfig().text.toUtf8());
    return result;
}

std::optional<Result> runPreviewBurstWorkflow(const Options& options, std::string& error) {
    SnowCanvasWidget canvas;
    int appliedCount = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::watermarkPreviewApplied,
                     [&appliedCount]() { ++appliedCount; });
    const auto sample = [&](int sequence, double* milliseconds) {
        const int before = appliedCount;
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < kPreviewBurstSize; ++index) {
            SnowCanvasWatermarkConfig config =
                makeWorkflowConfig(QStringLiteral("PREVIEW %1 %2").arg(sequence & 1).arg(index));
            config.angle = 10.0 + index;
            canvas.previewCanvasWatermarkConfig(config);
        }
        const qint64 enqueueElapsed = timer.nsecsElapsed();
        const bool delivered = waitForPreviewApplication(appliedCount, before + 1);
        *milliseconds = enqueueElapsed / 1'000'000.0;
        return delivered;
    };
    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignored = 0.0;
        if (!sample(iteration, &ignored)) {
            error = "workflow_preview_burst_16: warmup burst was not coalesced once";
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        if (!sample(options.warmupIterations + iteration, &milliseconds)) {
            error = "workflow_preview_burst_16: measured burst was not coalesced once";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
    }
    Result result = makeWorkflowResult(options, "workflow_preview_burst_16", "preview_apply",
                                       samples, kPreviewBurstSize);
    result.textBytes = QStringLiteral("PREVIEW 0 15").toUtf8().size();
    result.fontSize = 18.0;
    result.angle = 25.0;
    result.gap = 56.0;
    result.checksum = static_cast<std::uint64_t>(appliedCount);
    return result;
}

std::optional<Result> runPreviewPaintWorkflow(const Options& options, int width, int height,
                                              const QRectF& watermarkArea,
                                              const std::string& scenario, std::string& error) {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(width, height);
    if (watermarkArea.isValid() && !watermarkArea.isEmpty()) {
        canvas.setWatermarkRenderArea(watermarkArea);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    int appliedCount = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::watermarkPreviewApplied,
                     [&appliedCount]() { ++appliedCount; });
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    const auto sample = [&](int sequence, bool measured, double* milliseconds,
                            snow_canvas_renderer::WatermarkRenderDiagnostics* diagnostics) {
        const int before = appliedCount;
        SnowCanvasWatermarkConfig config = makeWorkflowConfig(QStringLiteral("PREVIEW PAINT"));
        config.color = (sequence & 1) != 0 ? QColor(190, 35, 70, 255) : QColor(30, 80, 160, 255);
        image.fill(Qt::transparent);
        snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
        for (int index = 0; index < kPreviewBurstSize; ++index) {
            config.angle = 30.0 + index * 0.25;
            canvas.previewCanvasWatermarkConfig(config);
        }
        const bool delivered = waitForPreviewApplication(appliedCount, before + 1);
        QElapsedTimer timer;
        timer.start();
        canvas.render(&image);
        const qint64 elapsed = timer.nsecsElapsed();
        if (measured) {
            *milliseconds = elapsed / 1'000'000.0;
            *diagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
        }
        return delivered;
    };
    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignoredMilliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics ignoredDiagnostics;
        if (!sample(iteration, false, &ignoredMilliseconds, &ignoredDiagnostics)) {
            error = scenario + ": warmup preview paint was not coalesced once";
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    snow_canvas_renderer::WatermarkRenderDiagnostics aggregateDiagnostics;
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
        if (!sample(options.warmupIterations + iteration, true, &milliseconds, &diagnostics)) {
            error = scenario + ": measured preview paint was not coalesced once";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
        accumulateDiagnostics(aggregateDiagnostics, diagnostics);
    }
    if (!hasVisiblePixel(image) || aggregateDiagnostics.renderCallCount == 0) {
        error = scenario + ": widget paint did not render a visible watermark";
        return std::nullopt;
    }
    Result result =
        makeWorkflowResult(options, scenario, "preview_paint", samples, kPreviewBurstSize);
    result.logicalWidth = width;
    result.logicalHeight = height;
    result.physicalWidth = width;
    result.physicalHeight = height;
    result.renderAreaWidth =
        watermarkArea.isValid() && !watermarkArea.isEmpty() ? qRound(watermarkArea.width()) : width;
    result.renderAreaHeight = watermarkArea.isValid() && !watermarkArea.isEmpty()
                                  ? qRound(watermarkArea.height())
                                  : height;
    result.textBytes = QByteArray("PREVIEW PAINT").size();
    result.fontSize = 18.0;
    result.angle = 30.0 + (kPreviewBurstSize - 1) * 0.25;
    result.gap = 56.0;
    result.framesPerSecond = operationsPerSecond(result.statistics.p50Ms);
    result.megapixelsPerSecond = width * height / 1'000'000.0 * result.framesPerSecond;
    result.checksum = imageChecksum(image);
    result.diagnostics = aggregateDiagnostics;
    return result;
}

using WidgetWorkflowMutation = std::function<bool(int, SnowCanvasWidget&)>;

std::optional<Result> runWidgetPaintWorkflow(const Options& options, int width, int height,
                                             const std::string& scenario,
                                             const std::string& operation,
                                             WidgetWorkflowMutation mutation, std::string& error) {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(width, height);
    if (!canvas.setCanvasWatermarkConfig(makeWorkflowConfig(QStringLiteral("WIDGET PAINT")))) {
        error = scenario + ": could not configure the runtime watermark";
        return std::nullopt;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();

    const auto sample = [&](int sequence, bool measured, double* milliseconds,
                            snow_canvas_renderer::WatermarkRenderDiagnostics* diagnostics) {
        image.fill(Qt::transparent);
        snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        const bool mutationApplied = mutation(sequence, canvas);
        canvas.render(&image);
        const qint64 elapsed = timer.nsecsElapsed();
        if (measured) {
            *milliseconds = elapsed / 1'000'000.0;
            *diagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
        }
        return mutationApplied;
    };

    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignoredMilliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics ignoredDiagnostics;
        if (!sample(iteration, false, &ignoredMilliseconds, &ignoredDiagnostics)) {
            error = scenario + ": warmup widget mutation failed";
            return std::nullopt;
        }
    }

    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    snow_canvas_renderer::WatermarkRenderDiagnostics aggregateDiagnostics;
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
        if (!sample(options.warmupIterations + iteration, true, &milliseconds, &diagnostics)) {
            error = scenario + ": measured widget mutation failed";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
        accumulateDiagnostics(aggregateDiagnostics, diagnostics);
    }
    if (!hasVisiblePixel(image) || aggregateDiagnostics.renderCallCount !=
                                       static_cast<std::size_t>(options.measuredIterations)) {
        error = scenario + ": widget paint did not render one visible watermark per sample";
        return std::nullopt;
    }

    Result result = makeWorkflowResult(options, scenario, operation, samples);
    result.logicalWidth = width;
    result.logicalHeight = height;
    result.physicalWidth = width;
    result.physicalHeight = height;
    result.renderAreaWidth = width;
    result.renderAreaHeight = height;
    result.textBytes = QByteArray("WIDGET PAINT").size();
    result.fontSize = 18.0;
    result.angle = 30.0;
    result.gap = 56.0;
    result.framesPerSecond = operationsPerSecond(result.statistics.p50Ms);
    result.megapixelsPerSecond = width * height / 1'000'000.0 * result.framesPerSecond;
    result.checksum = imageChecksum(image);
    result.diagnostics = aggregateDiagnostics;
    return result;
}

std::optional<Result> runMultiCanvasWorkflow(const Options& options, int width, int height,
                                             std::string& error) {
    SnowCanvasRuntime firstRuntime;
    SnowCanvasRuntime secondRuntime;
    SnowCanvasWidget first(firstRuntime);
    SnowCanvasWidget second(secondRuntime);
    first.resize(width, height);
    second.resize(width, height);
    const SnowCanvasWatermarkConfig config = makeWorkflowConfig(QStringLiteral("MULTI CANVAS"));
    if (!first.setCanvasWatermarkConfig(config) || !second.setCanvasWatermarkConfig(config)) {
        error = "workflow_multi_canvas_1920x1080: could not configure both watermarks";
        return std::nullopt;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QImage firstImage(width, height, QImage::Format_ARGB32_Premultiplied);
    QImage secondImage(width, height, QImage::Format_ARGB32_Premultiplied);
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();

    const auto sample = [&](bool measured, double* milliseconds,
                            snow_canvas_renderer::WatermarkRenderDiagnostics* diagnostics) {
        firstImage.fill(Qt::transparent);
        secondImage.fill(Qt::transparent);
        snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        first.render(&firstImage);
        second.render(&secondImage);
        const qint64 elapsed = timer.nsecsElapsed();
        if (measured) {
            *milliseconds = elapsed / 1'000'000.0;
            *diagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
        }
        return hasVisiblePixel(firstImage) && hasVisiblePixel(secondImage);
    };
    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignoredMilliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics ignoredDiagnostics;
        if (!sample(false, &ignoredMilliseconds, &ignoredDiagnostics)) {
            error = "workflow_multi_canvas_1920x1080: warmup paint failed";
            return std::nullopt;
        }
    }

    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    snow_canvas_renderer::WatermarkRenderDiagnostics aggregateDiagnostics;
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
        if (!sample(true, &milliseconds, &diagnostics)) {
            error = "workflow_multi_canvas_1920x1080: measured paint failed";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
        accumulateDiagnostics(aggregateDiagnostics, diagnostics);
    }
    Result result = makeWorkflowResult(options, "workflow_multi_canvas_1920x1080",
                                       "multi_canvas_paint", samples, 2);
    result.logicalWidth = width;
    result.logicalHeight = height;
    result.physicalWidth = width;
    result.physicalHeight = height;
    result.renderAreaWidth = width;
    result.renderAreaHeight = height;
    result.textBytes = config.text.toUtf8().size();
    result.fontSize = config.fontSize;
    result.angle = config.angle;
    result.gap = config.gap;
    result.framesPerSecond = operationsPerSecond(result.statistics.p50Ms);
    result.megapixelsPerSecond = width * height / 1'000'000.0 * result.framesPerSecond;
    result.checksum = imageChecksum(firstImage) ^ imageChecksum(secondImage);
    result.diagnostics = aggregateDiagnostics;
    if (aggregateDiagnostics.renderCallCount !=
            static_cast<std::size_t>(options.measuredIterations * 2) ||
        aggregateDiagnostics.denseFillCount >
            static_cast<std::size_t>(options.measuredIterations * 2)) {
        error =
            "workflow_multi_canvas_1920x1080: expected one render and at most one fill per canvas";
        return std::nullopt;
    }
    return result;
}

std::optional<Result> runExportWorkflow(const Options& options, int width, int height,
                                        bool visibleWatermark, const std::string& scenario,
                                        std::string& error) {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    SnowCanvasWatermarkConfig config =
        makeWorkflowConfig(visibleWatermark ? QStringLiteral("EXPORT WATERMARK") : QString());
    if (!canvas.setCanvasWatermarkConfig(config)) {
        error = scenario + ": could not configure the runtime watermark";
        return std::nullopt;
    }
    const QRectF selection(-width / 2.0, -height / 2.0, static_cast<double>(width),
                           static_cast<double>(height));
    QImage background(width, height, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);
    const QList<CanvasExportSource> sources{CanvasExportSource{background, selection}};
    QImage output;
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    const auto sample = [&](bool measured, double* milliseconds,
                            snow_canvas_renderer::WatermarkRenderDiagnostics* diagnostics) {
        snow_canvas_renderer::resetWatermarkRenderDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        output = runtime.renderToImage(selection, QSize(width, height), sources);
        const qint64 elapsed = timer.nsecsElapsed();
        if (measured) {
            *milliseconds = elapsed / 1'000'000.0;
            *diagnostics = snow_canvas_renderer::watermarkRenderDiagnosticsForCurrentThread();
        }
        return !output.isNull();
    };
    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        double ignoredMilliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics ignoredDiagnostics;
        if (!sample(false, &ignoredMilliseconds, &ignoredDiagnostics)) {
            error = scenario + ": warmup export failed";
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(options.measuredIterations);
    snow_canvas_renderer::WatermarkRenderDiagnostics aggregateDiagnostics;
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        double milliseconds = 0.0;
        snow_canvas_renderer::WatermarkRenderDiagnostics diagnostics;
        if (!sample(true, &milliseconds, &diagnostics)) {
            error = scenario + ": measured export failed";
            return std::nullopt;
        }
        samples.push_back(milliseconds);
        accumulateDiagnostics(aggregateDiagnostics, diagnostics);
    }
    if (aggregateDiagnostics.renderCallCount !=
            static_cast<std::size_t>(options.measuredIterations) ||
        (visibleWatermark && aggregateDiagnostics.denseFillCount == 0 &&
         aggregateDiagnostics.sparseBatchCount == 0 &&
         aggregateDiagnostics.fallbackGlyphDrawCount == 0) ||
        (!visibleWatermark && aggregateDiagnostics.earlyExitCount !=
                                  static_cast<std::size_t>(options.measuredIterations))) {
        error = scenario + ": export did not use the expected watermark renderer path";
        return std::nullopt;
    }
    Result result = makeWorkflowResult(options, scenario, "export", samples);
    result.cacheMode = visibleWatermark ? "warm" : "hidden_control";
    result.logicalWidth = width;
    result.logicalHeight = height;
    result.physicalWidth = width;
    result.physicalHeight = height;
    result.renderAreaWidth = width;
    result.renderAreaHeight = height;
    result.textBytes = config.text.toUtf8().size();
    result.fontSize = config.fontSize;
    result.angle = config.angle;
    result.gap = config.gap;
    result.framesPerSecond = operationsPerSecond(result.statistics.p50Ms);
    result.megapixelsPerSecond = width * height / 1'000'000.0 * result.framesPerSecond;
    result.checksum = imageChecksum(output);
    result.diagnostics = aggregateDiagnostics;
    return result;
}

std::vector<RendererCase> makeRendererCases() {
    std::vector<RendererCase> cases;
    RendererCase value;

    value.name = "renderer_hidden_1920x1080";
    value.description = "Empty text early-exit overhead at 1080p";
    value.cacheMode = "hidden";
    value.text.clear();
    value.expectVisible = false;
    cases.push_back(value);

    value = {};
    value.name = "renderer_cold_short_1920x1080";
    value.description = "Cold layout, alpha tile, tint, and paint at 1080p";
    value.cacheMode = "cold";
    value.mutation = RendererMutation::ColdCache;
    cases.push_back(value);

    value = {};
    value.name = "renderer_warm_short_1920x1080";
    value.description = "Warm cached short watermark at 1080p";
    value.cacheMode = "warm";
    cases.push_back(value);

    value = {};
    value.name = "renderer_warm_short_3840x2160";
    value.description = "Warm cached short watermark at 4K";
    value.cacheMode = "warm";
    value.logicalWidth = 3840;
    value.logicalHeight = 2160;
    cases.push_back(value);

    value = {};
    value.name = "renderer_warm_short_1920x1080_dpr2";
    value.description = "Warm 1080p logical watermark on a DPR 2 target";
    value.cacheMode = "warm";
    value.devicePixelRatio = 2.0;
    cases.push_back(value);

    value = {};
    value.name = "renderer_dense_1920x1080";
    value.description = "Warm minimum-gap dense watermark at 1080p";
    value.cacheMode = "warm_dense";
    value.fontSize = 32.0;
    value.gap = 10.0;
    cases.push_back(value);

    value = {};
    value.name = "renderer_clipped_640x360_on_3840x2160";
    value.description = "Warm 640x360 clipped area on a 4K surface";
    value.cacheMode = "warm_clipped";
    value.logicalWidth = 3840;
    value.logicalHeight = 2160;
    value.renderArea = QRectF(1600.0, 900.0, 640.0, 360.0);
    cases.push_back(value);

    value = {};
    value.name = "renderer_recolor_1920x1080";
    value.description = "Alternating color and opacity with tint-only rebuilds";
    value.cacheMode = "retint";
    value.mutation = RendererMutation::Color;
    cases.push_back(value);

    value = {};
    value.name = "renderer_angle_change_1920x1080";
    value.description = "Alternating angles with complete cache reuse";
    value.cacheMode = "warm_angle";
    value.mutation = RendererMutation::Angle;
    cases.push_back(value);

    value = {};
    value.name = "renderer_text_change_1920x1080";
    value.description = "Alternating text with full cache invalidation";
    value.cacheMode = "text_invalidation";
    value.mutation = RendererMutation::Text;
    cases.push_back(value);

    value = {};
    value.name = "renderer_font_size_change_1920x1080";
    value.description = "Alternating font size with full cache invalidation";
    value.cacheMode = "font_invalidation";
    value.mutation = RendererMutation::FontSize;
    cases.push_back(value);

    value = {};
    value.name = "renderer_gap_change_1920x1080";
    value.description = "Alternating tile gap with full cache invalidation";
    value.cacheMode = "gap_invalidation";
    value.mutation = RendererMutation::Gap;
    cases.push_back(value);

    value = {};
    value.name = "renderer_unicode_1920x1080";
    value.description = "Warm UTF-8 CJK watermark shaping and painting";
    value.cacheMode = "warm_unicode";
    value.text = QString::fromUtf8("Snow \xE6\xB0\xB4\xE5\x8D\xB0");
    cases.push_back(value);

    value = {};
    value.name = "renderer_segmented_maximum_text_1920x1080";
    value.description = "Oversized maximum-length text on the segmented sparse path";
    value.cacheMode = "segmented_sparse";
    value.text = QString(255, QLatin1Char('W'));
    value.fontSize = 42.0;
    value.gap = 10.0;
    value.angle = 0.0;
    value.expectSegmented = true;
    cases.push_back(value);

    value = {};
    value.name = "renderer_glyph_fallback_1920x1080";
    value.description = "A single glyph exceeding the physical chunk limit";
    value.cacheMode = "glyph_fallback";
    value.text = QStringLiteral("W");
    value.fontSize = 2000.0;
    value.gap = 10.0;
    value.angle = 0.0;
    value.expectFallback = true;
    cases.push_back(value);

    return cases;
}

std::vector<Scenario> makeScenarios() {
    std::vector<Scenario> scenarios;
    for (const RendererCase& testCase : makeRendererCases()) {
        scenarios.push_back(Scenario{
            testCase.name,
            Suite::Renderer,
            testCase.description,
            [testCase](const Options& options, std::string& error) {
                return runRendererCase(options, testCase, error);
            },
        });
    }
    scenarios.push_back(Scenario{
        "workflow_commit_alternating",
        Suite::Workflow,
        "Alternating persistent watermark commits without painting",
        runCommitWorkflow,
    });
    scenarios.push_back(Scenario{
        "workflow_preview_burst_16",
        Suite::Workflow,
        "Sixteen preview writes coalesced into one application",
        runPreviewBurstWorkflow,
    });
    scenarios.push_back(Scenario{
        "workflow_preview_paint_1920x1080",
        Suite::Workflow,
        "Coalesced preview application followed by full 1080p widget paint",
        [](const Options& options, std::string& error) {
            return runPreviewPaintWorkflow(options, 1920, 1080, {},
                                           "workflow_preview_paint_1920x1080", error);
        },
    });
    scenarios.push_back(Scenario{
        "workflow_preview_paint_clipped_640x360_on_3840x2160",
        Suite::Workflow,
        "Coalesced preview followed by clipped 640x360 paint on a 4K widget",
        [](const Options& options, std::string& error) {
            return runPreviewPaintWorkflow(
                options, 3840, 2160, QRectF(-320.0, -180.0, 640.0, 360.0),
                "workflow_preview_paint_clipped_640x360_on_3840x2160", error);
        },
    });
    scenarios.push_back(Scenario{
        "workflow_steady_widget_paint_1920x1080",
        Suite::Workflow,
        "Steady warm widget repaint with one watermark decoration pass",
        [](const Options& options, std::string& error) {
            return runWidgetPaintWorkflow(
                options, 1920, 1080, "workflow_steady_widget_paint_1920x1080", "widget_paint",
                [](int, SnowCanvasWidget&) { return true; }, error);
        },
    });
    scenarios.push_back(Scenario{
        "workflow_render_area_movement_1920x1080",
        Suite::Workflow,
        "Alternating watermark render areas with stable scene content",
        [](const Options& options, std::string& error) {
            return runWidgetPaintWorkflow(
                options, 1920, 1080, "workflow_render_area_movement_1920x1080", "render_area_move",
                [](int sequence, SnowCanvasWidget& canvas) {
                    const QRectF area = (sequence & 1) != 0 ? QRectF(-480.0, -270.0, 960.0, 540.0)
                                                            : QRectF(-720.0, -405.0, 1440.0, 810.0);
                    canvas.setWatermarkRenderArea(area);
                    return true;
                },
                error);
        },
    });
    scenarios.push_back(Scenario{
        "workflow_camera_movement_1920x1080",
        Suite::Workflow,
        "Alternating camera positions while keeping watermark viewport anchored",
        [](const Options& options, std::string& error) {
            return runWidgetPaintWorkflow(
                options, 1920, 1080, "workflow_camera_movement_1920x1080", "camera_move",
                [](int sequence, SnowCanvasWidget& canvas) {
                    return canvas.setViewportCamera((sequence & 1) != 0 ? 180.0 : -180.0,
                                                    (sequence & 1) != 0 ? -90.0 : 90.0,
                                                    (sequence & 1) != 0 ? 1.25 : 0.85);
                },
                error);
        },
    });
    scenarios.push_back(Scenario{
        "workflow_multi_canvas_1920x1080",
        Suite::Workflow,
        "Two same-thread canvases sharing shaped and tinted pattern-cache entries",
        [](const Options& options, std::string& error) {
            return runMultiCanvasWorkflow(options, 1920, 1080, error);
        },
    });
    for (const auto& [width, height] :
         {std::pair<int, int>{1920, 1080}, std::pair<int, int>{3840, 2160}}) {
        for (bool visible : {false, true}) {
            const std::string scenario = std::string("workflow_export_") +
                                         (visible ? "watermark_" : "hidden_") +
                                         std::to_string(width) + "x" + std::to_string(height);
            const std::string description = std::string(visible ? "Watermarked" : "No-watermark") +
                                            " runtime export at " + std::to_string(width) + "x" +
                                            std::to_string(height);
            scenarios.push_back(Scenario{
                scenario,
                Suite::Workflow,
                description,
                [width, height, visible, scenario](const Options& options, std::string& error) {
                    return runExportWorkflow(options, width, height, visible, scenario, error);
                },
            });
        }
    }
    return scenarios;
}

void printResults(const std::vector<Result>& results) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << std::left << std::setw(55) << "scenario" << std::right << std::setw(13) << "p50_ms"
              << std::setw(13) << "p95_ms" << std::setw(13) << "p99_ms" << std::setw(16) << "ops/s"
              << std::setw(16) << "MP/s" << '\n';
    for (const Result& result : results) {
        std::cout << std::left << std::setw(55) << result.scenario << std::right << std::setw(13)
                  << result.statistics.p50Ms << std::setw(13) << result.statistics.p95Ms
                  << std::setw(13) << result.statistics.p99Ms << std::setw(16)
                  << result.operationsPerSecond << std::setw(16) << result.megapixelsPerSecond
                  << '\n'
                  << "  diagnostics calls=" << result.diagnostics.renderCallCount
                  << " exits=" << result.diagnostics.earlyExitCount
                  << " strategy=" << strategyName(result.diagnostics.selectedStrategy)
                  << " shape_hits=" << result.diagnostics.shapeHitCount
                  << " shape_misses=" << result.diagnostics.shapeMissCount
                  << " unit_hits=" << result.diagnostics.unitHitCount
                  << " unit_misses=" << result.diagnostics.unitMissCount
                  << " cache_evictions=" << result.diagnostics.cacheEvictionCount
                  << " tints=" << result.diagnostics.tintBuildCount
                  << " repeat_cells=" << result.diagnostics.repeatCellBuildCount
                  << " sparse_batches=" << result.diagnostics.sparseBatchCount
                  << " fragments=" << result.diagnostics.submittedFragmentCount
                  << " dense_fills=" << result.diagnostics.denseFillCount
                  << " fallback_draws=" << result.diagnostics.fallbackGlyphDrawCount
                  << '\n';
    }
}

std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (char character : value) {
        escaped += character == '"' ? "\"\"" : std::string(1, character);
    }
    return escaped + '"';
}

double perSample(std::size_t value, int samples) {
    return samples > 0 ? static_cast<double>(value) / samples : 0.0;
}

std::string buildType() {
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

std::string compilerName() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}

bool writeCsv(const std::string& path, const std::vector<Result>& results, std::string& error) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        error = "could not open CSV output: " + path;
        return false;
    }
    stream << "format_version,suite,scenario,operation,cache_mode,logical_width,logical_height,"
              "physical_width,physical_height,dpr,render_area_width,render_area_height,text_bytes,"
              "font_size,angle,gap,batch_size,samples,mean_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,"
              "stddev_ms,operations_per_second,frames_per_second,megapixels_per_second,checksum,"
              "selected_strategy,cache_bytes,fragment_coverage,"
              "render_calls_per_sample,early_exits_per_sample,shape_hits_per_sample,"
              "shape_misses_per_sample,unit_hits_per_sample,unit_misses_per_sample,"
              "tint_builds_per_sample,repeat_cell_builds_per_sample,sparse_batches_per_sample,"
              "submitted_fragments_per_sample,culled_fragments_per_sample,"
              "segmented_chunks_per_sample,dense_fills_per_sample,fallback_glyph_draws_per_sample,"
              "shape_ms_per_sample,raster_ms_per_sample,tint_ms_per_sample,"
              "placement_ms_per_sample,composition_ms_per_sample,"
              "rendered_logical_left,rendered_logical_top,rendered_logical_right,"
              "rendered_logical_bottom,rendered_device_left,rendered_device_top,"
              "rendered_device_right,rendered_device_bottom,"
              "resolved_font,qt_version,platform,cpu_architecture,compiler,build_type\n";
    stream << std::fixed << std::setprecision(6);
    const std::string qtVersion = qVersion();
    const std::string platform = QSysInfo::prettyProductName().toStdString();
    const std::string architecture = QSysInfo::currentCpuArchitecture().toStdString();
    const std::string compiler = compilerName();
    for (const Result& result : results) {
        const auto& diagnostics = result.diagnostics;
        stream << "1," << csvEscape(result.suite) << ',' << csvEscape(result.scenario) << ','
               << csvEscape(result.operation) << ',' << csvEscape(result.cacheMode) << ','
               << result.logicalWidth << ',' << result.logicalHeight << ',' << result.physicalWidth
               << ',' << result.physicalHeight << ',' << result.devicePixelRatio << ','
               << result.renderAreaWidth << ',' << result.renderAreaHeight << ','
               << result.textBytes << ',' << result.fontSize << ',' << result.angle << ','
               << result.gap << ',' << result.batchSize << ',' << result.samples << ','
               << result.statistics.meanMs << ',' << result.statistics.p50Ms << ','
               << result.statistics.p95Ms << ',' << result.statistics.p99Ms << ','
               << result.statistics.minimumMs << ',' << result.statistics.maximumMs << ','
               << result.statistics.standardDeviationMs << ',' << result.operationsPerSecond << ','
               << result.framesPerSecond << ',' << result.megapixelsPerSecond << ','
               << result.checksum << ',' << strategyName(diagnostics.selectedStrategy) << ','
               << diagnostics.cacheBytes << ','
               << (result.samples > 0 ? diagnostics.fragmentCoverage / result.samples : 0.0) << ','
               << perSample(diagnostics.renderCallCount, result.samples) << ','
               << perSample(diagnostics.earlyExitCount, result.samples) << ','
               << perSample(diagnostics.shapeHitCount, result.samples) << ','
               << perSample(diagnostics.shapeMissCount, result.samples) << ','
               << perSample(diagnostics.unitHitCount, result.samples) << ','
               << perSample(diagnostics.unitMissCount, result.samples) << ','
               << perSample(diagnostics.tintBuildCount, result.samples) << ','
               << perSample(diagnostics.repeatCellBuildCount, result.samples) << ','
               << perSample(diagnostics.sparseBatchCount, result.samples) << ','
               << perSample(diagnostics.submittedFragmentCount, result.samples) << ','
               << perSample(diagnostics.culledFragmentCount, result.samples) << ','
               << perSample(diagnostics.segmentedChunkCount, result.samples) << ','
               << perSample(diagnostics.denseFillCount, result.samples) << ','
               << perSample(diagnostics.fallbackGlyphDrawCount, result.samples) << ','
               << diagnostics.shapeMilliseconds / result.samples << ','
               << diagnostics.rasterMilliseconds / result.samples << ','
               << diagnostics.tintMilliseconds / result.samples << ','
               << diagnostics.placementMilliseconds / result.samples << ','
               << diagnostics.compositionMilliseconds / result.samples << ','
               << diagnostics.renderedLogicalBounds.left() << ','
               << diagnostics.renderedLogicalBounds.top() << ','
               << diagnostics.renderedLogicalBounds.right() << ','
               << diagnostics.renderedLogicalBounds.bottom() << ','
               << diagnostics.renderedDeviceBounds.left() << ','
               << diagnostics.renderedDeviceBounds.top() << ','
               << diagnostics.renderedDeviceBounds.right() << ','
               << diagnostics.renderedDeviceBounds.bottom() << ','
               << csvEscape(result.resolvedFont) << ',' << csvEscape(qtVersion) << ','
               << csvEscape(platform) << ',' << csvEscape(architecture) << ','
               << csvEscape(compiler) << ',' << buildType() << '\n';
    }
    if (!stream) {
        error = "failed while writing CSV output: " + path;
        return false;
    }
    return true;
}

bool registerBenchmarkFont(std::string& error) {
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0) {
        error = "could not load C:/Windows/Fonts/segoeui.ttf";
        return false;
    }
#endif
    if (resolvedBenchmarkFont().isEmpty()) {
        error = "could not resolve a benchmark font";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    std::string error;
    const std::optional<Options> parsed = parseOptions(argc, argv, error);
    if (!parsed) {
        std::cerr << "error: " << error << "\n\n";
        printUsage(std::cerr, argv[0]);
        return 2;
    }
    if (!registerBenchmarkFont(error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    const Options& options = *parsed;
    const std::vector<Scenario> catalog = makeScenarios();
    if (options.help) {
        printUsage(std::cout, argv[0]);
        return 0;
    }
    if (options.list) {
        for (const Scenario& scenario : catalog) {
            if (suiteMatches(options.suite, scenario.suite)) {
                std::cout << std::left << std::setw(58) << scenario.name << " ["
                          << suiteName(scenario.suite) << "] " << scenario.description << '\n';
            }
        }
        return 0;
    }

    std::vector<const Scenario*> selected;
    for (const Scenario& scenario : catalog) {
        if (suiteMatches(options.suite, scenario.suite) &&
            (options.scenario.empty() || options.scenario == scenario.name)) {
            selected.push_back(&scenario);
        }
    }
    if (selected.empty()) {
        std::cerr << "error: no scenario matched suite=" << suiteName(options.suite);
        if (!options.scenario.empty()) {
            std::cerr << " name=" << options.scenario;
        }
        std::cerr << "; use --list to see available scenarios\n";
        return 2;
    }

    std::cout << "Running watermark benchmark: suite=" << suiteName(options.suite)
              << " warmup=" << options.warmupIterations
              << " iterations=" << options.measuredIterations << " scenarios=" << selected.size()
              << " font=\"" << resolvedBenchmarkFont().toStdString() << "\"\n";
    std::vector<Result> results;
    results.reserve(selected.size());
    for (const Scenario* scenario : selected) {
        std::cout << "Benchmarking " << scenario->name << "...\n";
        std::optional<Result> result = scenario->run(options, error);
        if (!result) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        results.push_back(std::move(*result));
    }
    printResults(results);
    if (!options.csvPath.empty()) {
        if (!writeCsv(options.csvPath, results, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        std::cout << "Wrote CSV results to " << options.csvPath << '\n';
    }
    return 0;
}
