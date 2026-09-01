#include "snow_canvas_spotlight_renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QSysInfo>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kDefaultWarmup = 5;
constexpr int kDefaultIterations = 100;

enum class Mutation {
    None,
    Opacity,
    Color,
    Camera,
    RenderArea,
    Geometry,
};

struct Options {
    std::string scenario;
    std::string csvPath;
    int warmup = kDefaultWarmup;
    int iterations = kDefaultIterations;
    bool list = false;
    bool help = false;
};

struct Scenario {
    std::string name;
    int width = 1920;
    int height = 1080;
    double dpr = 1.0;
    int cutoutCount = 1;
    Mutation mutation = Mutation::None;
    bool fragmentedExposure = false;
    bool boundedArea = false;
    bool zeroVisibleCutouts = false;
    bool fractionalCoordinates = false;
    bool compactExposure = false;
};

struct Statistics {
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
};

struct Result {
    Scenario scenario;
    Statistics timing;
    std::uint64_t checksum = 0;
    snow_canvas_spotlight_renderer::RenderDiagnostics diagnostics;
};

bool parsePositive(const char* text, int* out) {
    try {
        std::size_t consumed = 0;
        const long value = std::stol(text, &consumed);
        if (consumed != std::strlen(text) || value <= 0 ||
            value > std::numeric_limits<int>::max()) {
            return false;
        }
        *out = static_cast<int>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Options> parseOptions(int argc, char** argv, std::string* error) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--suite") {
            if (++index >= argc || (std::string_view(argv[index]) != "all" &&
                                    std::string_view(argv[index]) != "renderer")) {
                *error = "--suite requires all or renderer";
                return std::nullopt;
            }
        } else if (argument == "--scenario") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                *error = "--scenario requires an exact scenario name";
                return std::nullopt;
            }
            options.scenario = argv[index];
        } else if (argument == "--warmup") {
            if (++index >= argc || !parsePositive(argv[index], &options.warmup)) {
                *error = "--warmup requires a positive integer";
                return std::nullopt;
            }
        } else if (argument == "--iterations") {
            if (++index >= argc || !parsePositive(argv[index], &options.iterations)) {
                *error = "--iterations requires a positive integer";
                return std::nullopt;
            }
        } else if (argument == "--csv") {
            if (++index >= argc || std::string_view(argv[index]).empty()) {
                *error = "--csv requires a path";
                return std::nullopt;
            }
            options.csvPath = argv[index];
        } else {
            *error = "unknown argument: " + std::string(argument);
            return std::nullopt;
        }
    }
    return options;
}

void usage(std::ostream& out, const char* program) {
    out << "Snow Canvas spotlight benchmark\n\n"
        << "Usage: " << program << " [options]\n\n"
        << "  --suite <all|renderer>  Select suite (default: all)\n"
        << "  --scenario <name>       Run one exact scenario\n"
        << "  --warmup <count>        Warmup samples (default: 5)\n"
        << "  --iterations <count>    Measured samples (default: 100)\n"
        << "  --csv <path>            Write CSV results\n"
        << "  --list                  List scenarios\n"
        << "  --help, -h              Show help\n";
}

std::string dprLabel(double dpr) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << dpr;
    std::string value = stream.str();
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    std::replace(value.begin(), value.end(), '.', '_');
    return value;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;
    for (const auto [width, height] : {std::pair{1920, 1080}, std::pair{3840, 2160}}) {
        for (double dpr : {1.0, 1.25, 2.0}) {
            for (int count : {1, 16, 128}) {
                Scenario scenario;
                scenario.name = std::string("renderer_") + std::to_string(width) + "x" +
                                std::to_string(height) + "_dpr" + dprLabel(dpr) + "_cutouts" +
                                std::to_string(count);
                scenario.width = width;
                scenario.height = height;
                scenario.dpr = dpr;
                scenario.cutoutCount = count;
                scenario.compactExposure = width == 3840 && dpr == 2.0;
                out.push_back(scenario);
            }
        }
    }
    out.push_back(Scenario{"renderer_fragmented_exposure_1920x1080", 1920, 1080, 1.0, 128,
                           Mutation::None, true, false});
    out.push_back(Scenario{"renderer_bounded_area_3840x2160", 3840, 2160, 1.0, 128, Mutation::None,
                           false, true});
    out.push_back(Scenario{"renderer_opacity_preview_burst_1920x1080", 1920, 1080, 1.0, 128,
                           Mutation::Opacity, true, false});
    out.push_back(Scenario{"renderer_color_preview_burst_1920x1080", 1920, 1080, 1.0, 128,
                           Mutation::Color, true, false});
    out.push_back(Scenario{"renderer_zero_visible_cutouts_1920x1080", 1920, 1080, 1.0, 128,
                           Mutation::None, false, false, true});
    out.push_back(Scenario{"renderer_fractional_geometry_dpr125_1920x1080", 1920, 1080, 1.25,
                           16, Mutation::None, false, false, false, true});
    out.push_back(Scenario{"renderer_geometry_change_1920x1080", 1920, 1080, 1.0, 128,
                           Mutation::Geometry, false, false});
    out.push_back(Scenario{"renderer_camera_change_3840x2160", 3840, 2160, 1.0, 128,
                           Mutation::Camera, false, false});
    out.push_back(Scenario{"renderer_render_area_change_3840x2160", 3840, 2160, 1.0, 128,
                           Mutation::RenderArea, false, false});
    return out;
}

std::vector<SnowSpotlightCutout> makeCutouts(const Scenario& scenario) {
    std::vector<SnowSpotlightCutout> out;
    out.reserve(scenario.cutoutCount);
    const int columns = static_cast<int>(std::ceil(std::sqrt(scenario.cutoutCount)));
    for (int index = 0; index < scenario.cutoutCount; ++index) {
        const int column = index % columns;
        const int row = index / columns;
        const double spacingX = scenario.width / static_cast<double>(columns + 1);
        const double spacingY = scenario.height / static_cast<double>(columns + 1);
        double centerX = spacingX * (column + 1);
        double centerY = spacingY * (row + 1);
        double width = std::max(48.0, spacingX * 0.8);
        double height = std::max(36.0, spacingY * 0.7);
        if (scenario.zeroVisibleCutouts) {
            centerX = -width - 10.0 - index;
            centerY = -height - 10.0 - index;
        }
        if (scenario.fractionalCoordinates) {
            centerX += 0.37 + (index % 5) * 0.07;
            centerY += 0.23 + (index % 3) * 0.11;
            width += 0.29;
            height += 0.17;
        }
        out.push_back(SnowSpotlightCutout{
            centerX,
            centerY,
            width,
            height,
            (index % 7 - 3) * 0.11,
        });
    }
    return out;
}

Statistics statistics(const std::vector<double>& samples) {
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double fraction) {
        const std::size_t index = std::min(
            sorted.size() - 1, static_cast<std::size_t>(std::ceil(sorted.size() * fraction) - 1.0));
        return sorted[index];
    };
    return Statistics{
        std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size(),
        percentile(0.50),
        percentile(0.95),
        percentile(0.99),
        sorted.front(),
        sorted.back(),
    };
}

void accumulate(snow_canvas_spotlight_renderer::RenderDiagnostics* target,
                const snow_canvas_spotlight_renderer::RenderDiagnostics& source) {
    target->processedCutoutCount += source.processedCutoutCount;
    target->locallyCulledCutoutCount += source.locallyCulledCutoutCount;
    target->earlyExitCount += source.earlyExitCount;
    target->zeroCutoutFastPathCount += source.zeroCutoutFastPathCount;
    target->renderedPixelCount += source.renderedPixelCount;
    target->renderedRegionCount += source.renderedRegionCount;
}

std::optional<Result> run(const Scenario& scenario, const Options& options, std::string* error) {
    constexpr std::size_t targetCount = 1;
    std::vector<QImage> targets;
    targets.reserve(targetCount);
    for (std::size_t index = 0; index < targetCount; ++index) {
        QImage image(qRound(scenario.width * scenario.dpr), qRound(scenario.height * scenario.dpr),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(scenario.dpr);
        if (image.isNull()) {
            *error = scenario.name + ": render target allocation failed";
            return std::nullopt;
        }
        targets.push_back(std::move(image));
    }
    std::vector<SnowSpotlightCutout> cutouts = makeCutouts(scenario);
    SceneDisplayInfo sceneInfo;
    sceneInfo.surface_width = scenario.width;
    sceneInfo.surface_height = scenario.height;
    sceneInfo.camera_center_x = scenario.width / 2.0;
    sceneInfo.camera_center_y = scenario.height / 2.0;
    sceneInfo.camera_zoom = 1.0;
    SpotlightDisplayInfo spotlightInfo;
    spotlightInfo.active = true;
    const QRectF fullArea(0.0, 0.0, scenario.width, scenario.height);

    const auto makeExposure = [&](const QRectF& area) {
        if (scenario.compactExposure) {
            return QRegion(QRect(scenario.width / 4, scenario.height / 4, scenario.width / 2,
                                 scenario.height / 2));
        }
        QRegion exposure(area.toAlignedRect());
        if (!scenario.fragmentedExposure) {
            return exposure;
        }
        exposure = QRegion();
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                if ((row + column) % 2 == 0) {
                    exposure += QRect(column * scenario.width / 4, row * scenario.height / 4,
                                      scenario.width / 4, scenario.height / 4);
                }
            }
        }
        return exposure.intersected(area.toAlignedRect());
    };

    const auto sample = [&](int sequence, bool measured, double* elapsed,
                            snow_canvas_spotlight_renderer::RenderDiagnostics* diagnostics) {
        if (scenario.mutation == Mutation::Geometry && !cutouts.empty()) {
            cutouts.front().center_x += (sequence & 1) != 0 ? 1.5 : -1.5;
            cutouts.front().center_y += (sequence % 3) == 0 ? 0.75 : -0.5;
        }

        spotlightInfo.opacity =
            scenario.mutation == Mutation::Opacity ? ((sequence & 1) != 0 ? 0.35 : 0.75) : 0.64;
        spotlightInfo.color = scenario.mutation == Mutation::Color
                                  ? ((sequence & 1) != 0 ? SnowColorRgba8{24, 96, 192, 220}
                                                         : SnowColorRgba8{192, 72, 32, 220})
                                  : SnowColorRgba8{0, 0, 0, 255};
        sceneInfo.camera_center_x =
            scenario.width / 2.0 + (scenario.mutation == Mutation::Camera ? sequence * 4.0 : 0.0);

        QRectF renderArea = fullArea;
        if (scenario.boundedArea) {
            renderArea.adjust(scenario.width * 0.2, scenario.height * 0.2, -scenario.width * 0.2,
                              -scenario.height * 0.2);
        }
        if (scenario.mutation == Mutation::RenderArea && (sequence & 1) != 0) {
            renderArea = QRectF(scenario.width * 0.1, scenario.height * 0.1, scenario.width * 0.8,
                                scenario.height * 0.8);
        }
        const QRegion exposure = makeExposure(renderArea);
        for (QImage& target : targets) {
            target.fill(Qt::white);
        }

        snow_canvas_spotlight_renderer::resetDiagnosticsForCurrentThread();
        QElapsedTimer timer;
        timer.start();
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            QPainter painter(&targets[targetIndex]);
            painter.setClipRegion(exposure);
            snow_canvas_spotlight_renderer::render(
                painter, sceneInfo, spotlightInfo, cutouts.data(),
                static_cast<std::uint32_t>(cutouts.size()), renderArea, exposure);
            painter.end();
        }
        const qint64 nanoseconds = timer.nsecsElapsed();
        if (measured) {
            *elapsed = nanoseconds / 1'000'000.0;
            *diagnostics = snow_canvas_spotlight_renderer::diagnosticsForCurrentThread();
        }
    };

    for (int index = 0; index < options.warmup; ++index) {
        double ignored = 0.0;
        snow_canvas_spotlight_renderer::RenderDiagnostics ignoredDiagnostics;
        sample(index, false, &ignored, &ignoredDiagnostics);
    }
    std::vector<double> samples;
    samples.reserve(options.iterations);
    Result result;
    result.scenario = scenario;
    for (int index = 0; index < options.iterations; ++index) {
        double elapsed = 0.0;
        snow_canvas_spotlight_renderer::RenderDiagnostics diagnostics;
        sample(index, true, &elapsed, &diagnostics);
        samples.push_back(elapsed);
        accumulate(&result.diagnostics, diagnostics);
    }
    result.timing = statistics(samples);
    const auto pixel =
        targets.front().pixelColor(targets.front().width() / 2, targets.front().height() / 2);
    result.checksum = static_cast<std::uint64_t>(pixel.rgba());

    return result;
}

void print(const std::vector<Result>& results, int samples) {
    std::cout << std::left << std::setw(55) << "scenario" << std::right << std::setw(10) << "p50 ms"
              << std::setw(10) << "p95 ms" << std::setw(10) << "p99 ms" << std::setw(12)
              << "cutouts" << '\n';
    for (const Result& result : results) {
        std::cout << std::left << std::setw(55) << result.scenario.name << std::right << std::fixed
                  << std::setprecision(3) << std::setw(10) << result.timing.p50 << std::setw(10)
                  << result.timing.p95 << std::setw(10) << result.timing.p99 << std::setw(12)
                  << result.diagnostics.processedCutoutCount / samples << '\n';
    }
}

bool writeCsv(const std::string& path, const std::vector<Result>& results, int samples) {
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << "suite,scenario,operation,logical_width,logical_height,dpr,cutouts,"
              "samples,mean_ms,p50_ms,p95_ms,p99_ms,min_ms,max_ms,processed_cutouts,"
              "locally_culled_cutouts,early_exits,zero_cutout_fast_paths,rendered_pixels,"
              "rendered_regions,checksum,"
              "qt_version,platform,architecture\n";
    for (const Result& result : results) {
        const auto& d = result.diagnostics;
        stream << "renderer," << result.scenario.name << ",paint," << result.scenario.width << ','
               << result.scenario.height << ',' << result.scenario.dpr << ','
               << result.scenario.cutoutCount << ',' << samples << ',' << result.timing.mean << ','
               << result.timing.p50 << ',' << result.timing.p95 << ',' << result.timing.p99 << ','
               << result.timing.minimum << ',' << result.timing.maximum << ','
               << d.processedCutoutCount << ','
               << d.locallyCulledCutoutCount << ',' << d.earlyExitCount << ','
               << d.zeroCutoutFastPathCount << ',' << d.renderedPixelCount << ','
               << d.renderedRegionCount << ',' << result.checksum
               << ',' << qVersion() << ',' << QSysInfo::prettyProductName().toStdString() << ','
               << QSysInfo::currentCpuArchitecture().toStdString() << '\n';
    }
    return static_cast<bool>(stream);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    std::string error;
    const auto options = parseOptions(argc, argv, &error);
    if (!options) {
        std::cerr << "error: " << error << "\n\n";
        usage(std::cerr, argv[0]);
        return 2;
    }
    const auto catalog = scenarios();
    if (options->help) {
        usage(std::cout, argv[0]);
        return 0;
    }
    if (options->list) {
        for (const Scenario& scenario : catalog) {
            std::cout << scenario.name << '\n';
        }
        return 0;
    }
    std::vector<Result> results;
    for (const Scenario& scenario : catalog) {
        if (!options->scenario.empty() && options->scenario != scenario.name) {
            continue;
        }
        std::cout << "Benchmarking " << scenario.name << "...\n";
        auto result = run(scenario, *options, &error);
        if (!result) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        results.push_back(std::move(*result));
    }
    if (results.empty()) {
        std::cerr << "error: no scenario matched; use --list\n";
        return 2;
    }
    print(results, options->iterations);
    if (!options->csvPath.empty() && !writeCsv(options->csvPath, results, options->iterations)) {
        std::cerr << "error: could not write CSV " << options->csvPath << '\n';
        return 1;
    }
    return 0;
}
