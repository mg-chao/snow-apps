#include "snow_canvas_eraser_render_fixture.h"

#include <QGuiApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

std::vector<SnowArrowPoint> serpentine(int count) {
    std::vector<SnowArrowPoint> points;
    points.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        points.push_back({100 + i * 0.4, 540 + std::sin(i * 0.012) * 320});
    }
    return points;
}

void measure(const std::string& scenario, int iterations) {
    constexpr qreal dpr = 2;
    RenderFixture fixture({1920, 1080}, dpr);
    const auto points = serpentine(4104);
    const bool rectangleCase = scenario == "rectangle";
    const bool appendCase = scenario == "append";
    const bool dirtyCase = scenario == "dirty64";
    if (rectangleCase || dirtyCase) {
        fixture.items.push_back(rectangle({100, 100, 1700, 880}, 2));
    } else {
        fixture.items.push_back(
            brush(appendCase ? std::vector<SnowArrowPoint>(points.begin(), points.begin() + 4096)
                             : points,
                  24, 2));
    }
    const QRegion exposed = dirtyCase ? QRegion(QRect(944, 524, 64, 64)) : QRegion();
    for (int i = 0; i < 3; ++i) {
        fixture.render(dpr, true, exposed, true);
    }
    std::vector<double> times;
    snow_canvas_renderer::FilterRenderDiagnostics diagnostics;
    for (int i = 0; i < iterations; ++i) {
        if (appendCase) {
            fixture.items.back() =
                brush(std::vector<SnowArrowPoint>(points.begin(), points.begin() + 4096), 24, 2);
            fixture.atlas.clear();
            fixture.render(dpr, true);
            require(fixture.items.back().applyPenFilterGeometryPatch(
                        0, 1, 4096, points.data() + 4096, 8, false),
                    "append benchmark patch");
            fixture.atlas.invalidate(
                {{2, 1}, 0, 1, {1720, 100, 80, 900}, {1720, 100, 80, 900}, false, false});
        }
        QElapsedTimer timer;
        timer.start();
        const auto image = fixture.render(dpr, true, exposed, !appendCase);
        times.push_back(timer.nsecsElapsed() / 1'000'000.0);
        require(!image.isNull(), "benchmark output");
        diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    const double p95 = times[std::min(times.size() - 1, times.size() * 95 / 100)];
    std::cout << "scenario=" << scenario << " p50_ms=" << median << " p95_ms=" << p95
              << " restored_pixels=" << diagnostics.restoredPixelCount
              << " edge_blend_pixels=" << diagnostics.restorationBlendPixelCount
              << " working_pixels=" << diagnostics.totalWorkingPixelCount
              << " pristine_hits=" << diagnostics.pristineTileHits
              << " pristine_misses=" << diagnostics.pristineTileMisses
              << " chunk_builds=" << diagnostics.penGeometryChunkBuildCount
              << " chunk_reuses=" << diagnostics.penGeometryChunkReuseCount
              << " rasterized_tiles=" << diagnostics.penRasterizedTileCount
              << " copied_bytes=" << diagnostics.copiedBytes
              << " allocated_bytes=" << diagnostics.allocatedBytes
              << " atlas_bytes=" << fixture.atlas.retainedBytes()
              << " source_bytes=" << snow_canvas_filter_tile_cache::retainedBytes() << '\n';
    require(diagnostics.effectDispatchCount == 0,
            "erasers must not dispatch current-composite filter kernels");
    require(diagnostics.pristineTileMisses == 0 && diagnostics.allocatedBytes == 0,
            "warm erasers must reuse pristine source and bounded scratch");
    if (rectangleCase || dirtyCase) {
        require(diagnostics.maskPixelCount == 0, "aligned rectangle uses no mask");
    } else if (!appendCase) {
        require(diagnostics.penGeometryChunkBuildCount == 0 &&
                    diagnostics.penRasterizedTileCount == 0,
                "warm brush masks remain immutable");
    }
    if (appendCase) {
        require(diagnostics.penGeometryChunkBuildCount <= 2 &&
                    diagnostics.penGeometryChunkReuseCount >= 63,
                "append work is bounded");
    }
    if (dirtyCase) {
        require(diagnostics.totalWorkingPixelCount <= 4u * 256u * 256u,
                "dirty64 must not allocate a full 4K working surface");
    }
    require(fixture.atlas.retainedBytes() <= fixture.atlas.byteBudget() &&
                snow_canvas_filter_tile_cache::retainedBytes() <=
                    snow_canvas_filter_tile_cache::kByteLimit,
            "cache budgets remain bounded");
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    std::string scenario;
    int iterations = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--scenario" && i + 1 < argc) {
            scenario = argv[++i];
        } else if (argument == "--iterations" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else {
            std::cerr << "Use --scenario rectangle|brush|append|dirty64 --iterations <count>\n";
            return 1;
        }
    }
    require(iterations > 0, "iterations must be positive");
    if (scenario.empty()) {
        for (const auto* name : {"rectangle", "brush", "append", "dirty64"}) {
            measure(name, iterations);
        }
    } else {
        require(scenario == "rectangle" || scenario == "brush" || scenario == "append" ||
                    scenario == "dirty64",
                "unknown eraser benchmark scenario");
        measure(scenario, iterations);
    }
    return 0;
}
