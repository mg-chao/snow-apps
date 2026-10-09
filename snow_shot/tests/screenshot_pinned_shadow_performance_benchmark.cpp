#include "screenshotpinneddecorationrenderer.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <QLibrary>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace allocations {
// Count C++ allocations on the benchmark thread. Qt's direct malloc allocations are excluded;
// asset builds and retained vector/image capacities are reported separately.
thread_local bool enabled = false;
thread_local std::size_t count = 0;
thread_local std::size_t bytes = 0;
#ifdef Q_OS_WIN
struct StackSample {
    std::array<void*, 12> frames{};
    unsigned short depth = 0;
    std::size_t calls = 0;
    std::size_t bytes = 0;
};
thread_local bool traceEnabled = false;
thread_local std::array<StackSample, 256> stackSamples;
thread_local std::size_t stackSampleCount = 0;

void trace(std::size_t size) {
    StackSample sample;
    sample.depth = CaptureStackBackTrace(2, static_cast<DWORD>(sample.frames.size()),
                                         sample.frames.data(), nullptr);
    for (std::size_t index = 0; index < stackSampleCount; ++index) {
        auto& existing = stackSamples[index];
        if (sample.depth == existing.depth &&
            std::memcmp(sample.frames.data(), existing.frames.data(),
                        sample.depth * sizeof(void*)) == 0) {
            ++existing.calls;
            existing.bytes += size;
            return;
        }
    }
    if (stackSampleCount < stackSamples.size()) {
        sample.calls = 1;
        sample.bytes = size;
        stackSamples[stackSampleCount++] = sample;
    }
}

void reportStacks() {
    traceEnabled = false;
    QLibrary debugHelp(QStringLiteral("dbghelp"));
    const auto initialize =
        reinterpret_cast<decltype(&SymInitialize)>(debugHelp.resolve("SymInitialize"));
    const auto fromAddress =
        reinterpret_cast<decltype(&SymFromAddr)>(debugHelp.resolve("SymFromAddr"));
    const auto cleanup = reinterpret_cast<decltype(&SymCleanup)>(debugHelp.resolve("SymCleanup"));
    const HANDLE process = GetCurrentProcess();
    const bool initialized =
        initialize && fromAddress && cleanup && initialize(process, nullptr, true);
    std::sort(stackSamples.begin(), stackSamples.begin() + stackSampleCount,
              [](const auto& left, const auto& right) { return left.calls > right.calls; });
    QTextStream output(stderr);
    output << "image_base=0x"
           << QString::number(reinterpret_cast<quintptr>(GetModuleHandleW(nullptr)), 16) << '\n';
    for (std::size_t index = 0; index < std::min<std::size_t>(8, stackSampleCount); ++index) {
        const auto& sample = stackSamples[index];
        output << "allocation_stack calls=" << sample.calls << " bytes=" << sample.bytes << '\n';
        for (unsigned short frame = 0; frame < sample.depth; ++frame) {
            alignas(SYMBOL_INFO) std::array<unsigned char, sizeof(SYMBOL_INFO) + 1024> storage{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 1024;
            DWORD64 displacement = 0;
            const auto address = reinterpret_cast<DWORD64>(sample.frames[frame]);
            if (initialized && fromAddress(process, address, &displacement, symbol))
                output << "  " << symbol->Name << " + " << displacement << '\n';
            else
                output << "  0x" << QString::number(address, 16) << '\n';
        }
    }
    if (initialized)
        cleanup(process);
}
#endif

void* allocate(std::size_t size) {
    for (;;) {
        if (void* pointer = std::malloc(std::max<std::size_t>(1, size))) {
            if (enabled) {
                ++count;
                bytes += size;
#ifdef Q_OS_WIN
                if (traceEnabled)
                    trace(size);
#endif
            }
            return pointer;
        }
        if (const auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}
} // namespace allocations

void* operator new(std::size_t size) {
    return allocations::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocations::allocate(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {
struct Scenario {
    const char* name;
    QSizeF radii;
    int width = 16;
    bool enabled = true;
    bool cold = false;
    bool stateChanges = false;
    bool resize = false;
};

qreal percentile(const std::vector<qreal>& sorted, qreal fraction) {
    return sorted[static_cast<std::size_t>(
        std::ceil(fraction * static_cast<qreal>(sorted.size() - 1)))];
}

QJsonObject benchmark(const Scenario& scenario, int iterations, bool traceAllocations) {
#ifndef Q_OS_WIN
    Q_UNUSED(traceAllocations)
#endif
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    ScreenshotPinnedShadowCache cache;
    QImage surface(QSize(1920, 1080), QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    const QRect originalOutline = surface.rect().adjusted(24, 24, -24, -24);
    const std::array<QColor, 3> colors = {QColor(0, 0, 0, 255), QColor(105, 177, 255, 255),
                                          QColor(250, 173, 20, 160)};
    QPainter painter(&surface);
    const auto draw = [&](int frame) {
        if (!scenario.enabled)
            return;
        const QRect outline =
            scenario.resize ? originalOutline.adjusted(0, 0, -(frame % 2), 0) : originalOutline;
        const QColor& color =
            colors[scenario.stateChanges ? static_cast<std::size_t>(frame % 3) : std::size_t(0)];
        ScreenshotPinnedDecorationRenderer::renderShadow(
            painter, outline, scenario.radii, scenario.width, color, surface.rect(), cache);
    };
    for (int frame = 0; frame < 24; ++frame)
        draw(frame);
    const auto startGeometry = cache.diagnostics();
    const auto startAssets = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    std::vector<qreal> elapsed;
    elapsed.reserve(static_cast<std::size_t>(iterations));
    std::size_t geometryBuilds = 0;
    std::size_t assetBuilds = 0;
    std::size_t paletteBuilds = 0;
    std::size_t allocationCount = 0;
    std::size_t allocationBytes = 0;
    QElapsedTimer timer;
    for (int frame = 0; frame < iterations; ++frame) {
        if (scenario.cold) {
            cache.clear();
            ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
            ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
        }
        allocations::count = 0;
        allocations::bytes = 0;
        allocations::enabled = true;
#ifdef Q_OS_WIN
        allocations::traceEnabled =
            traceAllocations && frame == 0 && std::strcmp(scenario.name, "elliptical_warm") == 0;
#endif
        timer.start();
        draw(frame);
        const qreal microseconds = static_cast<qreal>(timer.nsecsElapsed()) / 1000.0;
        allocations::enabled = false;
#ifdef Q_OS_WIN
        if (allocations::traceEnabled)
            allocations::reportStacks();
#endif
        allocationCount += allocations::count;
        allocationBytes += allocations::bytes;
        elapsed.push_back(microseconds);
        if (scenario.cold) {
            geometryBuilds += cache.diagnostics().geometryBuilds;
            paletteBuilds += cache.diagnostics().paletteBuilds;
            assetBuilds +=
                ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread().cacheBuilds;
        }
    }
    const auto geometry = cache.diagnostics();
    const auto assets = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    if (!scenario.cold) {
        geometryBuilds = geometry.geometryBuilds - startGeometry.geometryBuilds;
        paletteBuilds = geometry.paletteBuilds - startGeometry.paletteBuilds;
        assetBuilds = assets.cacheBuilds - startAssets.cacheBuilds;
    }
    if (!scenario.cold && !scenario.resize &&
        (geometryBuilds != 0 || assetBuilds != 0 || paletteBuilds != 0))
        throw std::runtime_error("warm repaint unexpectedly rebuilt shadow preparation");
    std::sort(elapsed.begin(), elapsed.end());
    return {
        {QStringLiteral("scenario"), QString::fromLatin1(scenario.name)},
        {QStringLiteral("iterations"), iterations},
        {QStringLiteral("median_us"), percentile(elapsed, 0.5)},
        {QStringLiteral("p95_us"), percentile(elapsed, 0.95)},
        {QStringLiteral("p99_us"), percentile(elapsed, 0.99)},
        {QStringLiteral("max_us"), elapsed.back()},
        {QStringLiteral("geometry_builds"), static_cast<qint64>(geometryBuilds)},
        {QStringLiteral("asset_builds"), static_cast<qint64>(assetBuilds)},
        {QStringLiteral("geometry_cache_hits"), static_cast<qint64>(geometry.geometryHits)},
        {QStringLiteral("asset_cache_hits"), static_cast<qint64>(assets.cacheHits)},
        {QStringLiteral("retained_geometry_bytes"), static_cast<qint64>(geometry.retainedBytes)},
        {QStringLiteral("retained_palette_handle_bytes"),
         static_cast<qint64>(geometry.retainedPaletteBytes)},
        {QStringLiteral("retained_total_known_bytes"),
         static_cast<qint64>(sizeof(ScreenshotPinnedShadowCache) + geometry.retainedBytes +
                             geometry.retainedPaletteBytes + assets.retainedBytes)},
        {QStringLiteral("retained_span_rectangles"),
         static_cast<qint64>(geometry.retainedSpanRectangles)},
        {QStringLiteral("palette_builds"), static_cast<qint64>(paletteBuilds)},
        {QStringLiteral("palette_cache_hits"), static_cast<qint64>(geometry.paletteHits)},
        {QStringLiteral("retained_asset_bytes"), static_cast<qint64>(assets.retainedBytes)},
        {QStringLiteral("cpp_allocation_calls"), static_cast<qint64>(allocationCount)},
        {QStringLiteral("cpp_requested_bytes"), static_cast<qint64>(allocationBytes)}};
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption iterationsOption(QStringLiteral("iterations"),
                                              QStringLiteral("Samples per scenario"),
                                              QStringLiteral("count"), QStringLiteral("180"));
    parser.addOption(iterationsOption);
    const QCommandLineOption traceOption(
        QStringLiteral("trace-allocations"),
        QStringLiteral("Diagnose one Windows vector-paint allocation sample"));
    parser.addOption(traceOption);
    parser.process(application);
    const int iterations = parser.value(iterationsOption).toInt();
    if (iterations < 1 || iterations > 100000)
        return 2;
    try {
        QJsonArray results;
        const std::array scenarios = {
            Scenario{"disabled", QSizeF(300, 120), 16, false},
            Scenario{"square_warm", {}, 16},
            Scenario{"rounded_warm", QSizeF(128, 128), 16},
            Scenario{"rounded_cold", QSizeF(128, 128), 16, true, true},
            Scenario{"rounded_state_changes", QSizeF(128, 128), 16, true, false, true},
            Scenario{"large_radius_warm", QSizeF(300, 300), 16},
            Scenario{"large_radius_cold", QSizeF(300, 300), 16, true, true},
            Scenario{"elliptical_warm", QSizeF(300, 120), 16},
            Scenario{"elliptical_state_changes", QSizeF(300, 120), 16, true, false, true},
            Scenario{"elliptical_resize", QSizeF(300, 120), 16, true, false, false, true},
            Scenario{"macos_2x_warm", QSizeF(128, 128), 24},
            Scenario{"macos_2x_elliptical", QSizeF(300, 120), 24},
        };
        for (const auto& scenario : scenarios)
            results.append(benchmark(scenario, iterations, parser.isSet(traceOption)));
        QTextStream(stdout)
            << QJsonDocument(
                   QJsonObject{
                       {QStringLiteral("physical_surface"), QStringLiteral("1920x1080")},
                       {QStringLiteral("allocation_stack_diagnostic"), parser.isSet(traceOption)},
                       {QStringLiteral("allocation_coverage"),
                        QStringLiteral("C++ scalar/array new on the sample thread; "
                                       "Qt malloc and aligned allocations excluded")},
                       {QStringLiteral("retention_coverage"),
                        QStringLiteral("Cache objects, vector capacities, palette brush "
                                       "handles and shared image pixels; Qt-private "
                                       "brush data and allocator overhead excluded")},
                       {QStringLiteral("results"), results}})
                   .toJson(QJsonDocument::Indented);
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
