#include "snow_canvas_display_item.h"
#include "snow_canvas_filter_render.h"
#include "snow_canvas_renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QRegion>

#include <algorithm>
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
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

constexpr int kDefaultWarmupIterations = 10;
constexpr int kDefaultMeasuredIterations = 100;

enum class Suite {
    All,
    Kernel,
    Renderer,
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
    std::string effect;
    std::string workload;
    int width = 0;
    int height = 0;
    double devicePixelRatio = 1.0;
    double strength = 0.0;
    int filterCount = 0;
    int exposedWidth = 0;
    int exposedHeight = 0;
    int samples = 0;
    Statistics statistics;
    double framesPerSecond = 0.0;
    double megapixelsPerSecond = 0.0;
    std::uint64_t checksum = 0;
    snow_canvas_renderer::FilterRenderDiagnostics diagnostics;
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
    case Suite::Kernel:
        return "kernel";
    case Suite::Renderer:
        return "renderer";
    case Suite::All:
        return "all";
    }
    return "unknown";
}

std::string_view effectName(std::uint32_t type) {
    switch (type) {
    case 0:
        return "mosaic";
    case 1:
        return "gaussian";
    case 2:
        return "grayscale";
    case 3:
        return "inversion";
    }
    return "unknown";
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
                error = "--suite requires all, kernel, or renderer";
                return std::nullopt;
            }
            const std::string_view value(argv[index]);
            if (value == "all") {
                options.suite = Suite::All;
            } else if (value == "kernel") {
                options.suite = Suite::Kernel;
            } else if (value == "renderer") {
                options.suite = Suite::Renderer;
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
    out << "Detailed Snow Draw Engine filter benchmark\n\n"
        << "Usage: " << program << " [options]\n\n"
        << "  --suite <all|kernel|renderer>  Select a benchmark suite (default: all)\n"
        << "  --scenario <exact-name>        Run one scenario\n"
        << "  --warmup <count>               Warmup iterations (default: 10)\n"
        << "  --iterations <count>           Measured iterations (default: 100)\n"
        << "  --csv <path>                   Write stable CSV results\n"
        << "  --list                         List scenarios without running them\n"
        << "  --help, -h                     Show this help\n";
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

QImage makePatternedImage(int width, int height, qreal devicePixelRatio = 1.0, int seed = 0) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    for (int y = 0; y < height; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int alpha = 96 + ((x * 11 + y * 17 + seed * 7) % 160);
            const int red = (x * 13 + y * 3 + 29 + seed * 11) & 255;
            const int green = (x * 5 + y * 19 + 71 + seed * 17) & 255;
            const int blue = (x * 23 + y * 7 + 113 + seed * 23) & 255;
            line[x] = qPremultiply(qRgba(red, green, blue, alpha));
        }
    }
    return image;
}

std::uint64_t imageChecksum(const QImage& image) {
    if (image.isNull()) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    const int stepX = std::max(1, image.width() / 31);
    const int stepY = std::max(1, image.height() / 29);
    for (int y = 0; y < image.height(); y += stepY) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += stepX) {
            hash ^= line[x];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

bool hasNonTransparentPixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (line[x] != 0) {
                return true;
            }
        }
    }
    return false;
}

void restoreImage(QImage& destination, const QImage& source) {
    std::memcpy(destination.bits(), source.constBits(), source.sizeInBytes());
}

Result finishResult(Result result, const std::vector<double>& samples, std::uint64_t pixels) {
    result.samples = static_cast<int>(samples.size());
    result.statistics = calculateStatistics(samples);
    if (result.statistics.meanMs > 0.0) {
        result.framesPerSecond = 1000.0 / result.statistics.meanMs;
        result.megapixelsPerSecond =
            static_cast<double>(pixels) / (result.statistics.meanMs * 1000.0);
    }
    return result;
}

Runner makeKernelRunner(std::string scenario, std::uint32_t type, int width, int height,
                        double strength, bool blend,
                        snow_canvas_filter_render::ExecutionOptions execution = {}) {
    return [=](const Options& options, std::string& error) -> std::optional<Result> {
        const QImage source = makePatternedImage(width, height);
        QImage working = source.copy();
        QImage blendSource;
        if (blend) {
            blendSource = makePatternedImage(width, height, 1.0, 97);
        }
        snow_canvas_filter_render::Parameters parameters;
        parameters.type = type;
        parameters.strength = strength;
        parameters.logicalBlockSize = 2.0 + strength * 10.0;
        parameters.logicalSigma = 0.5 + strength * 18.0;

        const auto operation = [&] {
            if (blend) {
                snow_canvas_filter_render::blendOverSource(working, blendSource, 0.55, execution);
            } else {
                snow_canvas_filter_render::apply(working, parameters, nullptr, execution);
            }
        };
        for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
            restoreImage(working, source);
            operation();
        }
        std::vector<double> samples;
        samples.reserve(options.measuredIterations);
        for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
            restoreImage(working, source);
            QElapsedTimer timer;
            timer.start();
            operation();
            samples.push_back(timer.nsecsElapsed() / 1'000'000.0);
        }
        Result result;
        result.suite = "kernel";
        result.scenario = scenario;
        result.effect = blend ? "blend" : std::string(effectName(type));
        result.workload = blend ? "blend_over_source" : "apply";
        result.width = width;
        result.height = height;
        result.strength = strength;
        result.checksum = imageChecksum(working);
        if (samples.empty() || result.checksum == imageChecksum(source)) {
            error = "kernel scenario did not transform its input: " + scenario;
            return std::nullopt;
        }
        return finishResult(std::move(result), samples, static_cast<std::uint64_t>(width) * height);
    };
}

Runner makeMaskedMosaicRunner(std::string scenario, int width, int height, int maskAlpha,
                              snow_canvas_filter_render::ExecutionOptions execution = {}) {
    return [=](const Options& options, std::string& error) -> std::optional<Result> {
        const QImage source = makePatternedImage(width, height);
        const QImage originalDestination = makePatternedImage(width, height, 1.0, 97);
        QImage working = originalDestination.copy();
        QImage mask(width, height, QImage::Format_Alpha8);
        mask.fill(maskAlpha);
        snow_canvas_filter_render::Parameters parameters;
        parameters.type = 0;
        parameters.logicalBlockSize = 7.0;
        snow_canvas_filter_render::RenderWorkspace workspace;
        const auto operation = [&] {
            snow_canvas_filter_render::applyMasked(source, working, mask, working.rect(),
                                                   parameters, &workspace, execution);
        };
        for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
            restoreImage(working, originalDestination);
            operation();
        }
        std::vector<double> samples;
        samples.reserve(options.measuredIterations);
        for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
            restoreImage(working, originalDestination);
            QElapsedTimer timer;
            timer.start();
            operation();
            samples.push_back(timer.nsecsElapsed() / 1'000'000.0);
        }
        Result result;
        result.suite = "kernel";
        result.scenario = scenario;
        result.effect = "mosaic";
        result.workload = maskAlpha == 255 ? "masked_opaque" : "masked_partial";
        result.width = width;
        result.height = height;
        result.strength = 0.5;
        result.checksum = imageChecksum(working);
        if (samples.empty() || result.checksum == imageChecksum(originalDestination)) {
            error = "masked mosaic scenario did not transform its input: " + scenario;
            return std::nullopt;
        }
        return finishResult(std::move(result), samples, static_cast<std::uint64_t>(width) * height);
    };
}

Runner makeMaskedColorRunner(std::string scenario, std::uint32_t type, int width, int height,
                             int maskAlpha, double strength,
                             snow_canvas_filter_render::ExecutionOptions execution = {}) {
    return [=](const Options& options, std::string& error) -> std::optional<Result> {
        const QImage source = makePatternedImage(width, height);
        const QImage originalDestination = makePatternedImage(width, height, 1.0, 97);
        QImage working = originalDestination.copy();
        QImage mask(width, height, QImage::Format_Alpha8);
        mask.fill(maskAlpha);
        snow_canvas_filter_render::Parameters parameters;
        parameters.type = type;
        parameters.strength = strength;
        snow_canvas_filter_render::RenderWorkspace workspace;
        const auto operation = [&] {
            snow_canvas_filter_render::applyMasked(source, working, mask, working.rect(),
                                                   parameters, &workspace, execution);
        };
        for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
            restoreImage(working, originalDestination);
            operation();
        }
        std::vector<double> samples;
        samples.reserve(options.measuredIterations);
        for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
            restoreImage(working, originalDestination);
            QElapsedTimer timer;
            timer.start();
            operation();
            samples.push_back(timer.nsecsElapsed() / 1'000'000.0);
        }
        Result result;
        result.suite = "kernel";
        result.scenario = scenario;
        result.effect = effectName(type);
        result.workload = maskAlpha == 255 ? "masked_opaque" : "masked_partial";
        result.width = width;
        result.height = height;
        result.strength = strength;
        result.checksum = imageChecksum(working);
        if (samples.empty() || result.checksum == imageChecksum(originalDestination)) {
            error = "masked color scenario did not transform its input: " + scenario;
            return std::nullopt;
        }
        return finishResult(std::move(result), samples, static_cast<std::uint64_t>(width) * height);
    };
}

struct RendererConfig {
    std::string scenario;
    std::string workload;
    std::uint32_t type = 0;
    int surfaceWidth = 1920;
    int surfaceHeight = 1080;
    qreal devicePixelRatio = 1.0;
    double filterWidth = 256.0;
    double filterHeight = 256.0;
    double strength = 0.5;
    QRegion exposed;
    int filterCount = 1;
    bool alternateTypes = false;
    bool varyStrength = false;
    bool splitLayer = false;
    int offscreenItemCount = 0;
};

Runner makeRendererRunner(RendererConfig config) {
    return [config](const Options& options, std::string& error) -> std::optional<Result> {
        const int physicalWidth = qRound(config.surfaceWidth * config.devicePixelRatio);
        const int physicalHeight = qRound(config.surfaceHeight * config.devicePixelRatio);
        QImage output(physicalWidth, physicalHeight, QImage::Format_ARGB32_Premultiplied);
        output.setDevicePixelRatio(config.devicePixelRatio);
        output.fill(Qt::transparent);
        const QImage background =
            makePatternedImage(physicalWidth, physicalHeight, config.devicePixelRatio);

        std::vector<SnowCanvasSceneItem> items;
        items.reserve(config.filterCount + config.offscreenItemCount + 1);
        for (int index = 0; index < config.offscreenItemCount; ++index) {
            SnowSceneDisplayItem rectangle{};
            rectangle.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
            rectangle.center_x = 10000.0 + index * 3.0;
            rectangle.center_y = 10000.0 + index * 2.0;
            rectangle.width = 8.0;
            rectangle.height = 8.0;
            rectangle.fill = SnowColorRgba8{20, 40, 60, 255};
            rectangle.fill_style = SNOW_FILL_STYLE_SOLID;
            rectangle.opacity = 1.0;
            items.emplace_back(rectangle);
        }
        for (int index = 0; index < config.filterCount; ++index) {
            if (config.splitLayer && index == config.filterCount / 2) {
                SnowSceneDisplayItem separator{};
                separator.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
                separator.width = 32.0;
                separator.height = 32.0;
                separator.fill = SnowColorRgba8{90, 180, 40, 128};
                separator.fill_style = SNOW_FILL_STYLE_SOLID;
                separator.opacity = 1.0;
                items.emplace_back(separator);
            }
            SnowSceneDisplayItem filter{};
            filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
            filter.element_id = SnowElementId{static_cast<std::uint32_t>(index + 1), 1};
            filter.center_x = (index - (config.filterCount - 1) / 2.0) * 8.0;
            filter.center_y = ((index % 3) - 1) * 6.0;
            filter.width = config.filterWidth;
            filter.height = config.filterHeight;
            filter.rotation = index % 2 == 0 ? 0.0 : 0.025;
            const std::uint32_t filterType =
                config.alternateTypes ? static_cast<std::uint32_t>(index % 2 == 0 ? 1 : 3)
                                      : config.type;
            const double strength = config.varyStrength
                                        ? static_cast<double>(index + 1) / config.filterCount
                                        : config.strength;
            filter.filter = snow_filter_render_spec_resolve(filterType, strength);
            filter.opacity = 0.85;
            items.emplace_back(filter);
        }
        SceneDisplayInfo displayInfo{};
        displayInfo.item_count = static_cast<std::uint32_t>(items.size());
        displayInfo.surface_width = config.surfaceWidth;
        displayInfo.surface_height = config.surfaceHeight;
        displayInfo.camera_zoom = 1.0;
        const QRegion exposed = config.exposed;
        std::vector<std::uint32_t> spatialCandidates;
        if (config.offscreenItemCount > 0) {
            spatialCandidates.reserve(items.size() - config.offscreenItemCount);
            for (std::uint32_t index = static_cast<std::uint32_t>(config.offscreenItemCount);
                 index < items.size(); ++index) {
                spatialCandidates.push_back(index);
            }
        }

        const auto render = [&] {
            QPainter painter(&output);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setClipRegion(exposed);
            snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
                &painter,
                &displayInfo,
                items.data(),
                static_cast<std::uint32_t>(items.size()),
                exposed,
                spatialCandidates.empty() ? nullptr : spatialCandidates.data(),
                static_cast<std::uint32_t>(spatialCandidates.size()),
                &background,
            });
            painter.end();
        };
        for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
            render();
        }
        std::vector<double> samples;
        samples.reserve(options.measuredIterations);
        for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
            QElapsedTimer timer;
            timer.start();
            render();
            samples.push_back(timer.nsecsElapsed() / 1'000'000.0);
        }

        Result result;
        result.suite = "renderer";
        result.scenario = config.scenario;
        result.effect =
            config.alternateTypes ? "gaussian+inversion" : std::string(effectName(config.type));
        result.workload = config.workload;
        result.width = config.surfaceWidth;
        result.height = config.surfaceHeight;
        result.devicePixelRatio = config.devicePixelRatio;
        result.strength = config.strength;
        result.filterCount = config.filterCount;
        result.exposedWidth = config.exposed.boundingRect().width();
        result.exposedHeight = config.exposed.boundingRect().height();
        result.checksum = imageChecksum(output);
        result.diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
        const std::size_t expectedLayers = config.splitLayer ? 2u : 1u;
        const std::size_t dispatchesPerLayer =
            config.varyStrength ? static_cast<std::size_t>(config.filterCount) / expectedLayers
                                : (config.alternateTypes ? 2u : 1u);
        const std::size_t expectedDispatches = dispatchesPerLayer * expectedLayers;
        const std::size_t componentCount = result.diagnostics.surfaceComponentCount;
        if (!result.diagnostics.usedFilterPath ||
            result.diagnostics.originalFilterCount !=
                static_cast<std::size_t>(config.filterCount) * componentCount ||
            (!config.varyStrength &&
             result.diagnostics.effectDispatchCount != expectedDispatches * componentCount) ||
            (config.varyStrength &&
             (result.diagnostics.effectDispatchCount == 0 ||
              result.diagnostics.effectDispatchCount > expectedDispatches * componentCount ||
              result.diagnostics.batchedFilterCount == 0)) ||
            result.diagnostics.filterLayerCount != expectedLayers * componentCount ||
            result.diagnostics.workingSurfacePixelCount == 0 ||
            (config.offscreenItemCount > 0 &&
             (result.diagnostics.spatialCandidateCount == 0 ||
              result.diagnostics.spatialCandidateCount >=
                  static_cast<std::size_t>(items.size()) * componentCount)) ||
            !hasNonTransparentPixel(output)) {
            std::ostringstream message;
            message << "renderer scenario failed validation: " << config.scenario
                    << " (dispatches=" << result.diagnostics.effectDispatchCount
                    << ", layers=" << result.diagnostics.filterLayerCount
                    << ", working_pixels=" << result.diagnostics.workingSurfacePixelCount << ')';
            error = message.str();
            return std::nullopt;
        }
        const std::size_t workingPixels = result.diagnostics.workingSurfacePixelCount;
        return finishResult(std::move(result), samples, workingPixels);
    };
}

Runner makePenAppendRunner(std::uint32_t type) {
    return [type](const Options& options, std::string& error) -> std::optional<Result> {
        constexpr int logicalWidth = 1920;
        constexpr int logicalHeight = 1080;
        constexpr qreal devicePixelRatio = 2.0;
        const QSize physicalSize(qRound(logicalWidth * devicePixelRatio),
                                 qRound(logicalHeight * devicePixelRatio));
        QImage output(physicalSize, QImage::Format_ARGB32_Premultiplied);
        output.setDevicePixelRatio(devicePixelRatio);
        output.fill(Qt::transparent);
        const QImage background =
            makePatternedImage(physicalSize.width(), physicalSize.height(), devicePixelRatio);

        std::vector<SnowArrowPoint> basePoints;
        basePoints.reserve(4096);
        for (int row = 0; row < 64; ++row) {
            for (int column = 0; column < 64; ++column) {
                const int serpentineColumn = row % 2 == 0 ? column : 63 - column;
                basePoints.push_back(SnowArrowPoint{
                    -850.0 + serpentineColumn * (1700.0 / 63.0),
                    -420.0 + row * 12.0,
                });
            }
        }
        std::vector<SnowArrowPoint> appendedPoints = basePoints;
        for (int index = 1; index <= 8; ++index) {
            appendedPoints.push_back(SnowArrowPoint{
                basePoints.back().x + index * 15.0,
                basePoints.back().y + (index % 2 == 0 ? 4.0 : -4.0),
            });
        }
        SnowSceneDisplayItem baseFilter{};
        baseFilter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
        baseFilter.element_id = SnowElementId{501, 1};
        baseFilter.center_x = 0.0;
        baseFilter.center_y = -38.0;
        baseFilter.width = 1700.0;
        baseFilter.height = 764.0;
        baseFilter.is_free_draw = 1;
        baseFilter.arrow_points = basePoints.data();
        baseFilter.arrow_point_count = static_cast<std::uint32_t>(basePoints.size());
        baseFilter.stroke_width = 30.0;
        baseFilter.filter = snow_filter_render_spec_resolve(type, type == 1 ? 1.0 : 0.7);
        baseFilter.opacity = 0.85;
        SnowCanvasSceneItem item(baseFilter);

        SceneDisplayInfo info{};
        info.item_count = 1;
        info.surface_width = logicalWidth;
        info.surface_height = logicalHeight;
        info.camera_zoom = 1.0;
        const QPointF endpoint(logicalWidth / 2.0 + basePoints.back().x,
                               logicalHeight / 2.0 + basePoints.back().y);
        const QRegion exposed(QRectF(endpoint.x() - 20.0, endpoint.y() - 24.0, 160.0, 52.0)
                                  .toAlignedRect()
                                  .intersected(QRect(0, 0, logicalWidth, logicalHeight)));
        snow_canvas_filter_render::RenderWorkspace workspace;

        const auto prepareBase = [&] {
            item = baseFilter;
            std::size_t ignoredBuilds = 0;
            std::size_t ignoredReuses = 0;
            item.takePenFilterGeometryDiagnostics(&ignoredBuilds, &ignoredReuses);
        };
        const auto appendAndRender = [&] {
            const std::uint64_t expectedRevision = item.penFilterGeometryRevision();
            if (!item.applyPenFilterGeometryPatch(
                    expectedRevision, expectedRevision + 1,
                    static_cast<std::uint32_t>(basePoints.size()),
                    appendedPoints.data() + basePoints.size(),
                    static_cast<std::uint32_t>(appendedPoints.size() - basePoints.size()), false)) {
                return false;
            }
            QPainter painter(&output);
            painter.setClipRegion(exposed);
            snow_canvas_renderer::SceneRenderRequest request;
            request.painter = &painter;
            request.displayInfo = &info;
            request.sceneItems = &item;
            request.sceneItemCount = 1;
            request.exposedRegion = exposed;
            request.backgroundImage = &background;
            request.workspace = &workspace;
            snow_canvas_renderer::renderSceneItems(request);
            painter.end();
            return true;
        };

        for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
            prepareBase();
            if (!appendAndRender()) {
                error = "Pen Filter append scenario could not apply its append patch";
                return std::nullopt;
            }
        }
        std::vector<double> samples;
        samples.reserve(options.measuredIterations);
        for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
            prepareBase();
            QElapsedTimer timer;
            timer.start();
            if (!appendAndRender()) {
                error = "Pen Filter append scenario could not apply its append patch";
                return std::nullopt;
            }
            samples.push_back(timer.nsecsElapsed() / 1'000'000.0);
        }

        Result result;
        result.suite = "renderer";
        result.scenario = "renderer_pen_append_" + std::string(effectName(type)) + "_dpr2_4k";
        result.effect = std::string(effectName(type));
        result.workload = "pen_4096_serpentine_append_8";
        result.width = logicalWidth;
        result.height = logicalHeight;
        result.devicePixelRatio = devicePixelRatio;
        result.strength = type == 1 ? 1.0 : 0.7;
        result.filterCount = 1;
        result.exposedWidth = exposed.boundingRect().width();
        result.exposedHeight = exposed.boundingRect().height();
        result.checksum = imageChecksum(output);
        result.diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
        if (!result.diagnostics.usedFilterPath || result.diagnostics.effectDispatchCount != 1 ||
            result.diagnostics.penGeometryChunkBuildCount > 2 ||
            result.diagnostics.penGeometryChunkReuseCount < 63 || result.checksum == 0) {
            std::ostringstream message;
            message << "Pen Filter append scenario missed its append-aware path: "
                    << result.scenario << " used=" << result.diagnostics.usedFilterPath
                    << " dispatches=" << result.diagnostics.effectDispatchCount
                    << " builds=" << result.diagnostics.penGeometryChunkBuildCount
                    << " reuses=" << result.diagnostics.penGeometryChunkReuseCount
                    << " mask_bounds=" << result.diagnostics.maskBoundingPixelCount
                    << " mask_covered=" << result.diagnostics.maskCoveredPixelCount
                    << " sparse=" << result.diagnostics.sparseDispatchCount
                    << " dense=" << result.diagnostics.denseDispatchCount
                    << " exposed=" << exposed.boundingRect().x() << ':'
                    << exposed.boundingRect().y() << ':' << exposed.boundingRect().width() << ':'
                    << exposed.boundingRect().height() << " checksum=" << result.checksum;
            error = message.str();
            return std::nullopt;
        }
        const std::size_t workingPixels = result.diagnostics.totalWorkingPixelCount;
        return finishResult(std::move(result), samples, workingPixels);
    };
}

QRect centeredRect(int surfaceWidth, int surfaceHeight, int width, int height) {
    return QRect((surfaceWidth - width) / 2, (surfaceHeight - height) / 2, width, height);
}

std::vector<Scenario> makeScenarios() {
    std::vector<Scenario> scenarios;
    const auto addKernel = [&](std::string name, std::uint32_t type, int width, int height,
                               double strength, bool blend = false,
                               snow_canvas_filter_render::ExecutionOptions execution = {}) {
        const std::string description = (blend ? "blend" : std::string(effectName(type))) + " " +
                                        std::to_string(width) + "x" + std::to_string(height);
        scenarios.push_back(Scenario{
            name,
            Suite::Kernel,
            description,
            makeKernelRunner(name, type, width, height, strength, blend, execution),
        });
    };
    for (const auto [width, height, sizeName] : {
             std::tuple<int, int, const char*>{256, 256, "256x256"},
             std::tuple<int, int, const char*>{1920, 1080, "1920x1080"},
         }) {
        addKernel("kernel_mosaic_" + std::string(sizeName), 0, width, height, 0.5);
        addKernel("kernel_gaussian_low_" + std::string(sizeName), 1, width, height, 0.2);
        addKernel("kernel_gaussian_high_" + std::string(sizeName), 1, width, height, 1.0);
        addKernel("kernel_grayscale_" + std::string(sizeName), 2, width, height, 0.5);
        addKernel("kernel_inversion_" + std::string(sizeName), 3, width, height, 0.5);
        addKernel("kernel_blend_" + std::string(sizeName), 3, width, height, 0.55, true);
    }
    addKernel("kernel_grayscale_scalar_1920x1080", 2, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{true, false});
    addKernel("kernel_grayscale_avx2_1920x1080", 2, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{false, false});
    addKernel("kernel_inversion_scalar_1920x1080", 3, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{true, false});
    addKernel("kernel_inversion_avx2_1920x1080", 3, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{false, false});
    for (std::uint32_t type : {2u, 3u}) {
        for (int maskAlpha : {127, 255}) {
            const std::string name =
                "kernel_masked_" + std::string(effectName(type)) +
                (maskAlpha == 255 ? "_opaque_1920x1080" : "_partial_1920x1080");
            scenarios.push_back(Scenario{
                name,
                Suite::Kernel,
                "masked " + std::string(effectName(type)) + " 1920x1080",
                makeMaskedColorRunner(name, type, 1920, 1080, maskAlpha,
                                      maskAlpha == 255 ? 1.0 : 0.5),
            });
        }
    }
    addKernel("kernel_mosaic_one_thread_1920x1080", 0, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{false, true});
    addKernel("kernel_mosaic_full_pool_1920x1080", 0, 1920, 1080, 0.5, false,
              snow_canvas_filter_render::ExecutionOptions{false, false});
    scenarios.push_back(Scenario{
        "kernel_masked_mosaic_opaque_1920x1080",
        Suite::Kernel,
        "masked opaque mosaic 1920x1080",
        makeMaskedMosaicRunner("kernel_masked_mosaic_opaque_1920x1080", 1920, 1080, 255),
    });
    scenarios.push_back(Scenario{
        "kernel_masked_mosaic_partial_1920x1080",
        Suite::Kernel,
        "masked partial-alpha mosaic 1920x1080",
        makeMaskedMosaicRunner("kernel_masked_mosaic_partial_1920x1080", 1920, 1080, 217),
    });
    addKernel("kernel_gaussian_very_low_1920x1080", 1, 1920, 1080, 0.05);
    addKernel("kernel_gaussian_one_thread_1920x1080", 1, 1920, 1080, 1.0, false,
              snow_canvas_filter_render::ExecutionOptions{false, true});
    addKernel("kernel_gaussian_full_pool_1920x1080", 1, 1920, 1080, 1.0, false,
              snow_canvas_filter_render::ExecutionOptions{false, false});

    const auto addRenderer = [&](RendererConfig config, std::string description) {
        scenarios.push_back(Scenario{
            config.scenario,
            Suite::Renderer,
            std::move(description),
            makeRendererRunner(std::move(config)),
        });
    };
    for (std::uint32_t type = 0; type < 4; ++type) {
        scenarios.push_back(Scenario{
            "renderer_pen_append_" + std::string(effectName(type)) + "_dpr2_4k",
            Suite::Renderer,
            "4096-point Pen Filter with an eight-point late append at physical 4K",
            makePenAppendRunner(type),
        });
    }
    for (std::uint32_t type = 0; type < 4; ++type) {
        RendererConfig local;
        local.scenario = "renderer_local_" + std::string(effectName(type)) + "_4k";
        local.workload = "local_256_on_4k";
        local.type = type;
        local.surfaceWidth = 3840;
        local.surfaceHeight = 2160;
        local.filterWidth = 256;
        local.filterHeight = 256;
        local.strength = type == 1 ? 1.0 : 0.5;
        local.exposed = centeredRect(3840, 2160, 256, 256);
        addRenderer(local, "local 256x256 " + std::string(effectName(type)) + " on 4K");

        RendererConfig full;
        full.scenario = "renderer_full_" + std::string(effectName(type)) + "_1080p";
        full.workload = "full_1080p";
        full.type = type;
        full.filterWidth = 1920;
        full.filterHeight = 1080;
        full.strength = type == 1 ? 1.0 : 0.5;
        full.exposed = QRect(0, 0, 1920, 1080);
        addRenderer(full, "full-frame " + std::string(effectName(type)) + " at 1080p");
    }

    for (std::uint32_t type : {0u, 1u}) {
        RendererConfig dirty;
        dirty.scenario = "renderer_dirty64_" + std::string(effectName(type)) + "_1080p";
        dirty.workload = "dirty_64_filter_512";
        dirty.type = type;
        dirty.filterWidth = 512;
        dirty.filterHeight = 512;
        dirty.strength = 1.0;
        dirty.exposed = centeredRect(1920, 1080, 64, 64);
        addRenderer(dirty, "64x64 dirty region through 512x512 " + std::string(effectName(type)));
    }

    RendererConfig grouped;
    grouped.scenario = "renderer_grouped_gaussian_8_1080p";
    grouped.workload = "overlap_same_type_8";
    grouped.type = 1;
    grouped.filterWidth = 512;
    grouped.filterHeight = 512;
    grouped.strength = 0.7;
    grouped.exposed = centeredRect(1920, 1080, 640, 640);
    grouped.filterCount = 8;
    addRenderer(grouped, "eight overlapping Gaussian filters in one layer");

    RendererConfig groupedMosaic = grouped;
    groupedMosaic.scenario = "renderer_grouped_mosaic_8_1080p";
    groupedMosaic.workload = "overlap_same_mosaic_8";
    groupedMosaic.type = 0;
    groupedMosaic.strength = 0.5;
    addRenderer(groupedMosaic, "eight overlapping mosaic filters in one layer");

    RendererConfig alternating = grouped;
    alternating.scenario = "renderer_alternating_8_1080p";
    alternating.workload = "overlap_alternating_types_8";
    alternating.alternateTypes = true;
    addRenderer(alternating, "eight overlapping alternating filters");

    RendererConfig mixedStrengths = grouped;
    mixedStrengths.scenario = "renderer_mixed_strength_gaussian_8_1080p";
    mixedStrengths.workload = "overlap_mixed_strengths_8";
    mixedStrengths.varyStrength = true;
    addRenderer(mixedStrengths, "eight overlapping Gaussian filters with distinct strengths");

    RendererConfig mixedLayers = alternating;
    mixedLayers.scenario = "renderer_mixed_content_filter_layers_1080p";
    mixedLayers.workload = "mixed_content_two_filter_layers";
    mixedLayers.splitLayer = true;
    addRenderer(mixedLayers, "mixed content separating two adjacent filter layers");

    RendererConfig sparse = grouped;
    sparse.scenario = "renderer_sparse_distant_gaussian_1080p";
    sparse.workload = "two_distant_64_regions";
    sparse.filterCount = 1;
    sparse.filterWidth = 1920;
    sparse.filterHeight = 1080;
    sparse.exposed = QRegion(QRect(8, 8, 64, 64)) + QRegion(QRect(1848, 1008, 64, 64));
    addRenderer(sparse, "two distant 64x64 dirty components through Gaussian");

    RendererConfig culled = grouped;
    culled.scenario = "renderer_10k_mostly_offscreen_gaussian_1080p";
    culled.workload = "10k_mostly_offscreen_scene";
    culled.filterCount = 1;
    culled.offscreenItemCount = 10000;
    culled.exposed = QRect(480, 220, 960, 640);
    addRenderer(culled, "Gaussian layer with 10K mostly offscreen scene elements");

    RendererConfig highDpi;
    highDpi.scenario = "renderer_local_gaussian_dpr2_1080p";
    highDpi.workload = "local_256_dpr2";
    highDpi.type = 1;
    highDpi.devicePixelRatio = 2.0;
    highDpi.filterWidth = 256;
    highDpi.filterHeight = 256;
    highDpi.strength = 1.0;
    highDpi.exposed = centeredRect(1920, 1080, 256, 256);
    addRenderer(highDpi, "local 256x256 Gaussian filter at DPR 2");

    RendererConfig highDpiMosaic = highDpi;
    highDpiMosaic.scenario = "renderer_local_mosaic_dpr2_1080p";
    highDpiMosaic.workload = "local_256_mosaic_dpr2";
    highDpiMosaic.type = 0;
    highDpiMosaic.strength = 0.5;
    addRenderer(highDpiMosaic, "local 256x256 mosaic filter at DPR 2");

    RendererConfig fractionalDpi = highDpi;
    fractionalDpi.scenario = "renderer_local_gaussian_dpr1_25_1080p";
    fractionalDpi.workload = "local_256_dpr1_25";
    fractionalDpi.devicePixelRatio = 1.25;
    addRenderer(fractionalDpi, "local 256x256 Gaussian filter at DPR 1.25");

    return scenarios;
}

bool suiteMatches(Suite requested, Suite scenario) {
    return requested == Suite::All || requested == scenario;
}

void printResults(const std::vector<Result>& results) {
    std::cout << std::left << std::setw(43) << "scenario" << std::right << std::setw(11)
              << "mean_ms" << std::setw(11) << "p50_ms" << std::setw(11) << "p95_ms"
              << std::setw(11) << "p99_ms" << std::setw(11) << "min_ms" << std::setw(11) << "max_ms"
              << std::setw(11) << "stddev" << std::setw(11) << "fps" << std::setw(12) << "MPix/s"
              << '\n';
    std::cout << std::fixed << std::setprecision(3);
    for (const Result& result : results) {
        std::cout << std::left << std::setw(43) << result.scenario << std::right << std::setw(11)
                  << result.statistics.meanMs << std::setw(11) << result.statistics.p50Ms
                  << std::setw(11) << result.statistics.p95Ms << std::setw(11)
                  << result.statistics.p99Ms << std::setw(11) << result.statistics.minimumMs
                  << std::setw(11) << result.statistics.maximumMs << std::setw(11)
                  << result.statistics.standardDeviationMs << std::setw(11)
                  << result.framesPerSecond << std::setw(12) << result.megapixelsPerSecond << '\n';
        if (result.suite == "renderer") {
            std::cout << "  diagnostics working_pixels="
                      << result.diagnostics.totalWorkingPixelCount
                      << " peak_working_pixels=" << result.diagnostics.peakWorkingPixelCount
                      << " peak_effect_pixels=" << result.diagnostics.peakEffectPixelCount
                      << " components=" << result.diagnostics.surfaceComponentCount
                      << " candidates=" << result.diagnostics.spatialCandidateCount
                      << " replayed=" << result.diagnostics.replayedItemCount
                      << " filters=" << result.diagnostics.originalFilterCount
                      << " dispatches=" << result.diagnostics.effectDispatchCount
                      << " batched=" << result.diagnostics.batchedFilterCount
                      << " spatial_groups=" << result.diagnostics.spatialEffectGroupCount
                      << " sparse=" << result.diagnostics.sparseDispatchCount
                      << " dense=" << result.diagnostics.denseDispatchCount
                      << " mask_bounds=" << result.diagnostics.maskBoundingPixelCount
                      << " mask_covered=" << result.diagnostics.maskCoveredPixelCount
                      << " geometry_builds=" << result.diagnostics.penGeometryChunkBuildCount
                      << " geometry_reuses=" << result.diagnostics.penGeometryChunkReuseCount
                      << " pen_queried_chunks=" << result.diagnostics.penQueriedChunkCount
                      << " pen_culled_chunks=" << result.diagnostics.penCulledChunkCount
                      << " pen_raster_tiles=" << result.diagnostics.penRasterizedTileCount
                      << " pen_raster_pixels=" << result.diagnostics.penRasterizedPixelCount
                      << " pen_atlas_hits=" << result.diagnostics.penAtlasHits
                      << " pen_atlas_misses=" << result.diagnostics.penAtlasMisses
                      << " pen_atlas_evictions=" << result.diagnostics.penAtlasEvictions
                      << " pen_reused_after_patch=" << result.diagnostics.penAtlasReusedAfterPatch
                      << " pen_simd_rasters=" << result.diagnostics.penSimdRasterExecutions
                      << " pen_atlas_bytes=" << result.diagnostics.retainedPenAtlasBytes
                      << " source_tile_hits=" << result.diagnostics.sourceTileHits
                      << " source_tile_misses=" << result.diagnostics.sourceTileMisses
                      << " parallel_jobs=" << result.diagnostics.parallelJobs
                      << " retained_bytes=" << result.diagnostics.retainedWorkspaceBytes
                      << " gaussian_passes=" << result.diagnostics.gaussianPasses
                      << " downsample_avx2=" << result.diagnostics.gaussianDownsampleAvx2Executions
                      << " reconstruction_avx2="
                      << result.diagnostics.gaussianReconstructionAvx2Executions
                      << " replay_ms=" << result.diagnostics.sceneReplayNanoseconds / 1.0e6
                      << " path_ms=" << result.diagnostics.pathConstructionNanoseconds / 1.0e6
                      << " mask_ms=" << result.diagnostics.maskConstructionNanoseconds / 1.0e6
                      << " mask_scan_ms=" << result.diagnostics.maskScanNanoseconds / 1.0e6
                      << " downsample_ms=" << result.diagnostics.downsampleNanoseconds / 1.0e6
                      << " reduced_blur_ms=" << result.diagnostics.reducedBlurNanoseconds / 1.0e6
                      << " reconstruction_ms="
                      << result.diagnostics.reconstructionNanoseconds / 1.0e6
                      << " presentation_ms=" << result.diagnostics.presentationNanoseconds / 1.0e6
                      << " simd=" << result.diagnostics.simdBackend << '\n';
        }
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

bool writeCsv(const std::string& path, const std::vector<Result>& results, std::string& error) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        error = "could not open CSV output: " + path;
        return false;
    }
    stream << "format_version,suite,scenario,effect,workload,width,height,dpr,strength,"
              "filter_count,exposed_width,exposed_height,samples,mean_ms,p50_ms,p95_ms,"
              "p99_ms,min_ms,max_ms,stddev_ms,fps,megapixels_per_second,checksum,"
              "exposed_pixels,total_working_pixels,peak_working_pixels,peak_effect_pixels,"
              "surface_components,spatial_candidates,replayed_items,filter_layers,original_filters,"
              "effect_dispatches,batched_filters,mask_pixels,mask_bounding_pixels,"
              "mask_covered_pixels,sparse_dispatches,dense_dispatches,spatial_effect_groups,"
              "pen_geometry_chunk_builds,pen_geometry_chunk_reuses,pen_queried_chunks,"
              "pen_culled_chunks,pen_rasterized_tiles,pen_rasterized_pixels,pen_atlas_hits,"
              "pen_atlas_misses,pen_atlas_evictions,pen_reused_after_patch,pen_simd_rasters,"
              "retained_pen_atlas_bytes,allocated_bytes,copied_bytes,"
              "scratch_reuse,source_tile_hits,source_tile_misses,source_tile_evictions,"
              "parallel_jobs,retained_bytes,"
              "gaussian_passes,gaussian_downsample_avx2,gaussian_reconstruction_avx2,"
              "opaque_rect_dispatches,constant_rect_dispatches,"
              "scene_replay_ns,path_construction_ns,mask_construction_ns,mask_scan_ns,"
              "downsample_ns,reduced_blur_ns,"
              "reconstruction_ns,presentation_ns,simd_backend\n";
    stream << std::fixed << std::setprecision(6);
    for (const Result& result : results) {
        stream << "1," << csvEscape(result.suite) << ',' << csvEscape(result.scenario) << ','
               << csvEscape(result.effect) << ',' << csvEscape(result.workload) << ','
               << result.width << ',' << result.height << ',' << result.devicePixelRatio << ','
               << result.strength << ',' << result.filterCount << ',' << result.exposedWidth << ','
               << result.exposedHeight << ',' << result.samples << ',' << result.statistics.meanMs
               << ',' << result.statistics.p50Ms << ',' << result.statistics.p95Ms << ','
               << result.statistics.p99Ms << ',' << result.statistics.minimumMs << ','
               << result.statistics.maximumMs << ',' << result.statistics.standardDeviationMs << ','
               << result.framesPerSecond << ',' << result.megapixelsPerSecond << ','
               << result.checksum << ',' << result.diagnostics.exposedPixelCount << ','
               << result.diagnostics.totalWorkingPixelCount << ','
               << result.diagnostics.peakWorkingPixelCount << ','
               << result.diagnostics.peakEffectPixelCount << ','
               << result.diagnostics.surfaceComponentCount << ','
               << result.diagnostics.spatialCandidateCount << ','
               << result.diagnostics.replayedItemCount << ',' << result.diagnostics.filterLayerCount
               << ',' << result.diagnostics.originalFilterCount << ','
               << result.diagnostics.effectDispatchCount << ','
               << result.diagnostics.batchedFilterCount << ',' << result.diagnostics.maskPixelCount
               << ',' << result.diagnostics.maskBoundingPixelCount << ','
               << result.diagnostics.maskCoveredPixelCount << ','
               << result.diagnostics.sparseDispatchCount << ','
               << result.diagnostics.denseDispatchCount << ','
               << result.diagnostics.spatialEffectGroupCount << ','
               << result.diagnostics.penGeometryChunkBuildCount << ','
               << result.diagnostics.penGeometryChunkReuseCount << ','
               << result.diagnostics.penQueriedChunkCount << ','
               << result.diagnostics.penCulledChunkCount << ','
               << result.diagnostics.penRasterizedTileCount << ','
               << result.diagnostics.penRasterizedPixelCount << ','
               << result.diagnostics.penAtlasHits << ',' << result.diagnostics.penAtlasMisses << ','
               << result.diagnostics.penAtlasEvictions << ','
               << result.diagnostics.penAtlasReusedAfterPatch << ','
               << result.diagnostics.penSimdRasterExecutions << ','
               << result.diagnostics.retainedPenAtlasBytes << ','
               << result.diagnostics.allocatedBytes << ',' << result.diagnostics.copiedBytes << ','
               << result.diagnostics.scratchReuseCount << ','
               << result.diagnostics.sourceTileHits << ','
               << result.diagnostics.sourceTileMisses << ','
               << result.diagnostics.sourceTileEvictions << ','
               << result.diagnostics.parallelJobs << ','
               << result.diagnostics.retainedWorkspaceBytes << ','
               << result.diagnostics.gaussianPasses << ','
               << result.diagnostics.gaussianDownsampleAvx2Executions << ','
               << result.diagnostics.gaussianReconstructionAvx2Executions << ','
               << result.diagnostics.opaqueRectDispatchCount << ','
               << result.diagnostics.constantOpacityRectDispatchCount << ','
               << result.diagnostics.sceneReplayNanoseconds << ','
               << result.diagnostics.pathConstructionNanoseconds << ','
               << result.diagnostics.maskConstructionNanoseconds << ','
               << result.diagnostics.maskScanNanoseconds << ','
               << result.diagnostics.downsampleNanoseconds << ','
               << result.diagnostics.reducedBlurNanoseconds << ','
               << result.diagnostics.reconstructionNanoseconds << ','
               << result.diagnostics.presentationNanoseconds << ','
               << result.diagnostics.simdBackend << '\n';
    }
    if (!stream) {
        error = "failed while writing CSV output: " + path;
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
    const Options& options = *parsed;
    const std::vector<Scenario> catalog = makeScenarios();
    if (options.help) {
        printUsage(std::cout, argv[0]);
        return 0;
    }
    if (options.list) {
        for (const Scenario& scenario : catalog) {
            if (suiteMatches(options.suite, scenario.suite)) {
                std::cout << std::left << std::setw(43) << scenario.name << " ["
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

    std::cout << "Running filter benchmark: suite=" << suiteName(options.suite)
              << " warmup=" << options.warmupIterations
              << " iterations=" << options.measuredIterations << " scenarios=" << selected.size()
              << '\n';
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
