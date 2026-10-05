#include "pinnedwindowselectiongeometry.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace cpp_allocation_tracking {
// Benchmark-only replacements count successful scalar/array/aligned C++
// allocations on the sampling thread. Qt containers allocate through malloc
// directly, so these counters do not represent all heap activity.
thread_local bool enabled = false;
thread_local std::uint64_t calls = 0;
thread_local std::uint64_t requestedBytes = 0;

void record(std::size_t size) {
    if (enabled) {
        ++calls;
        requestedBytes += size;
    }
}

void* allocate(std::size_t size) {
    for (;;) {
        if (void* result = std::malloc(std::max(std::size_t(1), size))) {
            record(size);
            return result;
        }
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}

void* allocateAligned(std::size_t size, std::size_t alignment) {
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        throw std::bad_alloc();
    const auto rounded = std::max(alignment, ((size + alignment - 1) / alignment) * alignment);
    for (;;) {
#if defined(_WIN32)
        void* result = _aligned_malloc(rounded, alignment);
#else
        void* result = std::aligned_alloc(alignment, rounded);
#endif
        if (result) {
            record(size);
            return result;
        }
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}

void freeAligned(void* pointer) {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

class Sample final {
  public:
    Sample() {
        calls = 0;
        requestedBytes = 0;
        enabled = true;
    }
    ~Sample() {
        enabled = false;
    }
    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;
};
} // namespace cpp_allocation_tracking

void* operator new(std::size_t size) {
    return cpp_allocation_tracking::allocate(size);
}
void* operator new[](std::size_t size) {
    return cpp_allocation_tracking::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return cpp_allocation_tracking::allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return cpp_allocation_tracking::allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return cpp_allocation_tracking::allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return cpp_allocation_tracking::allocateAligned(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
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
void operator delete(void* pointer, std::align_val_t) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    cpp_allocation_tracking::freeAligned(pointer);
}

namespace {
namespace geometry = pinned_window_selection_geometry;
using Alignment = SnowCanvasSelectionAlignment;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    QList<QRectF> rectangles;
    std::vector<double> scalePercents;
};

Fixture fixture(int count) {
    Fixture result;
    result.rectangles.reserve(count);
    result.scalePercents.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        result.rectangles.append({-1200.25 + (index * 317) % 2600, -380.5 + (index * 173) % 1400,
                                  101.0 + (index % 9) * 23.0, 79.0 + (index % 7) * 17.0});
        result.scalePercents.push_back(30.0 + (index % 11) * 35.0);
    }
    return result;
}

struct SampleResult {
    std::vector<double> nanoseconds;
    double checksum = 0.0;
    std::uint64_t cppAllocationCalls = 0;
    std::uint64_t cppRequestedBytes = 0;
};

template <typename Operation>
SampleResult measure(int samples, int iterations, Operation operation) {
    SampleResult result;
    result.nanoseconds.reserve(static_cast<std::size_t>(samples));
    for (int warmup = 0; warmup < 100; ++warmup)
        result.checksum += operation();
    QElapsedTimer timer;
    for (int sample = 0; sample < samples; ++sample) {
        timer.start();
        for (int iteration = 0; iteration < iterations; ++iteration)
            result.checksum += operation();
        const qint64 nanoseconds = timer.nsecsElapsed();
        result.nanoseconds.push_back(static_cast<double>(nanoseconds) / iterations);
    }
    // Measure allocations in a separate pass so successful-allocation counter
    // writes do not contaminate the elapsed-time samples.
    {
        const cpp_allocation_tracking::Sample allocationSample;
        for (int iteration = 0; iteration < iterations; ++iteration)
            result.checksum += operation();
    }
    result.cppAllocationCalls = cpp_allocation_tracking::calls;
    result.cppRequestedBytes = cpp_allocation_tracking::requestedBytes;
    std::sort(result.nanoseconds.begin(), result.nanoseconds.end());
    require(std::isfinite(result.checksum) && result.checksum != 0.0,
            "benchmark results must be consumed");
    return result;
}

QJsonObject report(int count, const QString& operation, int samples, int iterations,
                   const SampleResult& result) {
    const auto percentile = [&](double fraction) {
        const auto index = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(result.nanoseconds.size())) - 1.0);
        return result.nanoseconds[std::min(index, result.nanoseconds.size() - 1)];
    };
    return {{QStringLiteral("window_count"), count},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("iterations_per_sample"), iterations},
            {QStringLiteral("p50_ns_per_call"), percentile(0.5)},
            {QStringLiteral("p95_ns_per_call"), percentile(0.95)},
            {QStringLiteral("allocation_sampling_calls"), iterations},
            {QStringLiteral("cpp_allocation_calls_per_call"),
             static_cast<double>(result.cppAllocationCalls) / iterations},
            {QStringLiteral("cpp_requested_bytes_per_call"),
             static_cast<double>(result.cppRequestedBytes) / iterations},
            {QStringLiteral("checksum"), result.checksum}};
}

double consume(const std::optional<QList<QRectF>>& rectangles) {
    require(rectangles.has_value() && !rectangles->isEmpty(), "geometry operation failed");
    const auto& first = rectangles->first();
    const auto& middle = rectangles->at(rectangles->size() / 2);
    const auto& last = rectangles->last();
    return first.x() + first.y() + middle.x() + middle.width() + last.y() + last.height();
}
} // namespace

int main(int argc, char* argv[]) {
#ifndef NDEBUG
    std::cerr << "Run this benchmark with a Release performance preset.\n";
    return 2;
#endif
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured samples per operation"),
                      QStringLiteral("count"), QStringLiteral("21")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Calls per measured sample"),
                      QStringLiteral("count"), QStringLiteral("1000")});
    parser.process(application);
    try {
        bool validSamples = false;
        bool validIterations = false;
        const int samples = parser.value(QStringLiteral("samples")).toInt(&validSamples);
        const int iterations = parser.value(QStringLiteral("iterations")).toInt(&validIterations);
        require(validSamples && samples >= 5 && samples <= 200,
                "samples must be between 5 and 200");
        require(validIterations && iterations >= 1 && iterations <= 100000,
                "iterations must be between 1 and 100000");
        struct AlignmentCase {
            Alignment operation;
            const char* name;
            bool distribute;
        };
        const std::array alignments{
            AlignmentCase{Alignment::AlignLeft, "align-left", false},
            AlignmentCase{Alignment::AlignCenterHorizontally, "center-horizontally", false},
            AlignmentCase{Alignment::AlignRight, "align-right", false},
            AlignmentCase{Alignment::DistributeHorizontally, "distribute-horizontally", true},
            AlignmentCase{Alignment::AlignTop, "align-top", false},
            AlignmentCase{Alignment::AlignCenterVertically, "center-vertically", false},
            AlignmentCase{Alignment::AlignBottom, "align-bottom", false},
            AlignmentCase{Alignment::DistributeVertically, "distribute-vertically", true}};
        QJsonArray scenarios;
        for (const int count : {2, 10, 50}) {
            const auto input = fixture(count);
            for (const auto& alignment : alignments) {
                if (alignment.distribute && count < 3)
                    continue;
                const auto result = measure(samples, iterations, [&] {
                    return consume(
                        geometry::alignmentTargets(input.rectangles, alignment.operation));
                });
                scenarios.append(report(count, QString::fromLatin1(alignment.name), samples,
                                        iterations, result));
            }
            for (const double requestedFactor : {0.5, 1.2}) {
                const auto result = measure(samples, iterations, [&] {
                    const auto factor =
                        geometry::sharedScaleFactor(input.scalePercents, requestedFactor);
                    require(factor.has_value(), "shared scale factor failed");
                    return consume(geometry::scaledTargets(input.rectangles, *factor));
                });
                scenarios.append(report(count,
                                        requestedFactor < 1.0 ? QStringLiteral("scale-down")
                                                              : QStringLiteral("scale-up"),
                                        samples, iterations, result));
            }
        }
        const QJsonObject results{
            {QStringLiteral("configuration"), QStringLiteral("Release")},
            {QStringLiteral("measurement"), QStringLiteral("geometry-helper")},
            {QStringLiteral("allocation_measurement"),
             QStringLiteral("successful C++ operator new/new[] calls on the sampling thread, "
                            "including aligned and nothrow forms; direct malloc and Qt container "
                            "allocations excluded")},
            {QStringLiteral("timing_includes_allocation_tracking"), false},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("scenarios"), scenarios}};
        QTextStream(stdout) << QJsonDocument(results).toJson(QJsonDocument::Indented);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
