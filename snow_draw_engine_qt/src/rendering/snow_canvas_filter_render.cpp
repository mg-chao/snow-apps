#include "snow_canvas_filter_render.h"
#include "snow_canvas_filter_avx2.h"

#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace snow_canvas_filter_render {
namespace {

constexpr std::size_t kParallelPixelThreshold = 64u * 1024u;
constexpr std::size_t kTargetPixelsPerJob = 64u * 1024u;
thread_local bool g_insideFilterWorker = false;

class FilterWorkerPool {
  public:
    struct Barrier {
        std::mutex mutex;
        std::condition_variable ready;
        int remaining = 0;
    };

    struct Task {
        void* context = nullptr;
        void (*invoke)(void*, int, int) = nullptr;
        int begin = 0;
        int end = 0;
        Barrier* barrier = nullptr;
    };

    FilterWorkerPool() {
        const int ideal = QThread::idealThreadCount();
        const int count = std::min(8, std::max(1, ideal > 0 ? ideal - 1 : 1));
        m_threads.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            m_threads.emplace_back([this] { workerLoop(); });
        }
    }

    ~FilterWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_all();
        for (std::thread& thread : m_threads) {
            thread.join();
        }
    }

    std::size_t workerCount() const {
        return m_threads.size();
    }

    void submit(Task task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(task);
        }
        m_ready.notify_one();
    }

    void wait(Barrier& barrier) {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        barrier.ready.wait(lock, [&barrier] { return barrier.remaining == 0; });
    }

  private:
    void workerLoop() {
        g_insideFilterWorker = true;
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
                if (m_stopping && m_tasks.empty()) {
                    return;
                }
                task = m_tasks.front();
                m_tasks.pop_front();
            }
            task.invoke(task.context, task.begin, task.end);
            {
                std::lock_guard<std::mutex> lock(task.barrier->mutex);
                --task.barrier->remaining;
            }
            task.barrier->ready.notify_one();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Task> m_tasks;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

FilterWorkerPool& workerPool() {
    static FilterWorkerPool pool;
    return pool;
}

template <typename Function>
std::size_t parallelRows(int rowCount, int width, bool singleThreaded, Function&& function) {
    const std::size_t pixels = static_cast<std::size_t>(std::max(0, rowCount)) *
                               static_cast<std::size_t>(std::max(0, width));
    if (rowCount <= 1 || singleThreaded || g_insideFilterWorker ||
        pixels <= kParallelPixelThreshold) {
        function(0, rowCount);
        return 0;
    }

    FilterWorkerPool& pool = workerPool();
    const int usefulJobs =
        static_cast<int>((pixels + kTargetPixelsPerJob - 1) / kTargetPixelsPerJob);
    const int jobCount =
        std::min<int>(rowCount, std::min(usefulJobs, static_cast<int>(pool.workerCount()) + 1));
    if (jobCount <= 1) {
        function(0, rowCount);
        return 0;
    }
    const int rowsPerJob = (rowCount + jobCount - 1) / jobCount;
    FilterWorkerPool::Barrier barrier;
    barrier.remaining = jobCount - 1;
    using FunctionType = std::remove_reference_t<Function>;
    const auto invoke = [](void* context, int begin, int end) {
        (*static_cast<FunctionType*>(context))(begin, end);
    };
    for (int job = 0; job + 1 < jobCount; ++job) {
        const int begin = job * rowsPerJob;
        const int end = std::min(rowCount, begin + rowsPerJob);
        pool.submit(FilterWorkerPool::Task{&function, invoke, begin, end, &barrier});
    }
    const int callerBegin = (jobCount - 1) * rowsPerJob;
    function(callerBegin, rowCount);
    pool.wait(barrier);
    return static_cast<std::size_t>(jobCount - 1);
}

std::array<int, 3> gaussianBoxRadii(double sigma) {
    if (!(sigma > 0.0)) {
        return {0, 0, 0};
    }
    constexpr int passCount = 3;
    const double idealWidth = std::sqrt((12.0 * sigma * sigma / passCount) + 1.0);
    int lowerWidth = static_cast<int>(std::floor(idealWidth));
    if ((lowerWidth & 1) == 0) {
        --lowerWidth;
    }
    lowerWidth = qMax(1, lowerWidth);
    const int upperWidth = lowerWidth + 2;
    const double numerator = 12.0 * sigma * sigma - passCount * lowerWidth * lowerWidth -
                             4.0 * passCount * lowerWidth - 3.0 * passCount;
    const double denominator = -4.0 * lowerWidth - 4.0;
    const int lowerPasses = qBound(0, qRound(numerator / denominator), passCount);
    std::array<int, 3> radii{};
    for (int index = 0; index < passCount; ++index) {
        radii[index] = ((index < lowerPasses) ? lowerWidth : upperWidth) / 2;
    }
    return radii;
}

struct BoxAverage {
    int count = 1;
    std::uint32_t reciprocal = 1u << 24;

    int operator()(int sum) const {
        return std::min(
            255,
            static_cast<int>((static_cast<std::uint64_t>(sum) * reciprocal + (1u << 23)) >> 24));
    }
};

BoxAverage boxAverage(int count) {
    if (count <= 1) {
        return {};
    }
    constexpr std::uint32_t scale = 1u << 24;
    return {count,
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(scale) + count / 2) / count)};
}

inline QRgb averagePixel(int alpha, int red, int green, int blue, const BoxAverage& average) {
    return qRgba(average(red), average(green), average(blue), average(alpha));
}

struct MosaicGrid {
    int block = 1;
    int firstX = 0;
    int firstY = 0;
    int columnCount = 0;
    int rowCount = 0;
};

MosaicGrid mosaicGrid(const QSize& size, const Parameters& parameters) {
    MosaicGrid grid;
    grid.block = qMax(1, qRound(parameters.logicalBlockSize * parameters.devicePixelRatio));
    const int originX = qRound(parameters.gridOriginInImage.x());
    const int originY = qRound(parameters.gridOriginInImage.y());
    grid.firstX =
        originX +
        static_cast<int>(std::floor(-originX / static_cast<double>(grid.block))) * grid.block;
    grid.firstY =
        originY +
        static_cast<int>(std::floor(-originY / static_cast<double>(grid.block))) * grid.block;
    grid.columnCount = (size.width() - grid.firstX + grid.block - 1) / grid.block;
    grid.rowCount = (size.height() - grid.firstY + grid.block - 1) / grid.block;
    return grid;
}

void collectMosaicSamples(const ConstImageView& source, const MosaicGrid& grid, int firstColumn,
                          int columnCount, int firstRow, int rowCount, std::vector<QRgb>& samples) {
    for (int localRow = 0; localRow < rowCount; ++localRow) {
        const int row = firstRow + localRow;
        const int sampleY =
            qBound(0, grid.firstY + row * grid.block + grid.block / 2, source.height - 1);
        const auto* sampleLine = reinterpret_cast<const QRgb*>(
            source.data + static_cast<qsizetype>(sampleY) * source.stride);
        for (int localColumn = 0; localColumn < columnCount; ++localColumn) {
            const int column = firstColumn + localColumn;
            const int sampleX =
                qBound(0, grid.firstX + column * grid.block + grid.block / 2, source.width - 1);
            samples[static_cast<std::size_t>(localRow) * columnCount + localColumn] =
                sampleLine[sampleX];
        }
    }
}

inline QRgb blendPremultiplied(QRgb current, QRgb to, int mix) {
    const std::uint64_t inverse = static_cast<std::uint64_t>(255 - mix);
    const auto blendPair = [inverse, mix](std::uint32_t first, std::uint32_t second) {
        std::uint64_t value = static_cast<std::uint64_t>(first) * inverse +
                              static_cast<std::uint64_t>(second) * static_cast<std::uint64_t>(mix) +
                              0x007f007full;
        value += 0x00010001ull + ((value >> 8) & 0x00ff00ffull);
        return static_cast<std::uint32_t>((value >> 8) & 0x00ff00ffull);
    };
    const std::uint32_t redBlue = blendPair(current & 0x00ff00ffu, to & 0x00ff00ffu);
    const std::uint32_t alphaGreen =
        blendPair((current >> 8) & 0x00ff00ffu, (to >> 8) & 0x00ff00ffu);
    return redBlue | (alphaGreen << 8);
}

int normalizedStrengthMix(double strength) {
    if (std::isnan(strength)) {
        strength = 1.0;
    } else if (!std::isfinite(strength)) {
        strength = strength < 0.0 ? 0.0 : 1.0;
    }
    return qBound(0, qRound(qBound(0.0, strength, 1.0) * 255.0), 255);
}

inline int combineCoverage(int first, int second) {
    return (first * second + 127) / 255;
}

inline QRgb transformedColor(QRgb pixel, std::uint32_t type) {
    const int alpha = qAlpha(pixel);
    if (type == 2) {
        const int luminance =
            qMin(alpha, (qRed(pixel) * 54 + qGreen(pixel) * 183 + qBlue(pixel) * 19 + 128) >> 8);
        return qRgba(luminance, luminance, luminance, alpha);
    }
    if (type == 3) {
        return qRgba(alpha - qRed(pixel), alpha - qGreen(pixel), alpha - qBlue(pixel), alpha);
    }
    return pixel;
}

std::size_t horizontalBoxBlur(const QImage& source, QImage& destination, int radius,
                              const BoxAverage& average, bool singleThreaded) {
    const int width = source.width();
    const int height = source.height();
    if (radius <= 0) {
        std::memcpy(destination.bits(), source.constBits(), source.sizeInBytes());
        return 0;
    }
    return parallelRows(height, width, singleThreaded, [&](int begin, int end) {
        for (int y = begin; y < end; ++y) {
            const auto* input = reinterpret_cast<const QRgb*>(source.constScanLine(y));
            auto* output = reinterpret_cast<QRgb*>(destination.scanLine(y));
            int alpha = 0;
            int red = 0;
            int green = 0;
            int blue = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const QRgb pixel = input[qBound(0, offset, width - 1)];
                alpha += qAlpha(pixel);
                red += qRed(pixel);
                green += qGreen(pixel);
                blue += qBlue(pixel);
            }
            for (int x = 0; x < width; ++x) {
                output[x] = averagePixel(alpha, red, green, blue, average);
                const QRgb removed = input[qBound(0, x - radius, width - 1)];
                const QRgb added = input[qBound(0, x + radius + 1, width - 1)];
                alpha += qAlpha(added) - qAlpha(removed);
                red += qRed(added) - qRed(removed);
                green += qGreen(added) - qGreen(removed);
                blue += qBlue(added) - qBlue(removed);
            }
        }
    });
}

std::size_t verticalBoxBlur(const QImage& source, QImage& destination, int radius,
                            const BoxAverage& average, bool singleThreaded) {
    const int width = source.width();
    const int height = source.height();
    if (radius <= 0) {
        std::memcpy(destination.bits(), source.constBits(), source.sizeInBytes());
        return 0;
    }
    return parallelRows(height, width, singleThreaded, [&](int begin, int end) {
        thread_local std::vector<int> sums;
        sums.assign(static_cast<std::size_t>(width) * 4u, 0);
        auto* alpha = sums.data();
        auto* red = alpha + width;
        auto* green = red + width;
        auto* blue = green + width;
        for (int offset = -radius; offset <= radius; ++offset) {
            const auto* line = reinterpret_cast<const QRgb*>(
                source.constScanLine(qBound(0, begin + offset, height - 1)));
            for (int x = 0; x < width; ++x) {
                alpha[x] += qAlpha(line[x]);
                red[x] += qRed(line[x]);
                green[x] += qGreen(line[x]);
                blue[x] += qBlue(line[x]);
            }
        }
        for (int y = begin; y < end; ++y) {
            auto* output = reinterpret_cast<QRgb*>(destination.scanLine(y));
            for (int x = 0; x < width; ++x) {
                output[x] = averagePixel(alpha[x], red[x], green[x], blue[x], average);
            }
            const auto* removed = reinterpret_cast<const QRgb*>(
                source.constScanLine(qBound(0, y - radius, height - 1)));
            const auto* added = reinterpret_cast<const QRgb*>(
                source.constScanLine(qBound(0, y + radius + 1, height - 1)));
            for (int x = 0; x < width; ++x) {
                alpha[x] += qAlpha(added[x]) - qAlpha(removed[x]);
                red[x] += qRed(added[x]) - qRed(removed[x]);
                green[x] += qGreen(added[x]) - qGreen(removed[x]);
                blue[x] += qBlue(added[x]) - qBlue(removed[x]);
            }
        }
    });
}

std::size_t downsample(const QImage& source, const QRect& sourcePixels, QImage& destination,
                       int factor, bool singleThreaded, bool useAvx2, bool* avx2Executed) {
    std::atomic_bool executed = false;
    if (factor <= 1) {
        const std::size_t jobs = parallelRows(
            destination.height(), destination.width(), singleThreaded, [&](int begin, int end) {
                if (useAvx2 && sourcePixels == source.rect() &&
                    detail::copyRowsAvx2(view(source), view(destination), begin, end)) {
                    executed.store(true, std::memory_order_relaxed);
                    return;
                }
                const std::size_t rowBytes =
                    static_cast<std::size_t>(destination.width()) * sizeof(QRgb);
                for (int y = begin; y < end; ++y) {
                    std::memcpy(destination.scanLine(y),
                                source.constScanLine(sourcePixels.top() + y) +
                                    static_cast<qsizetype>(sourcePixels.left()) * sizeof(QRgb),
                                rowBytes);
                }
            });
        if (avx2Executed != nullptr) {
            *avx2Executed = executed.load(std::memory_order_relaxed);
        }
        return jobs;
    }
    const std::size_t jobs = parallelRows(
        destination.height(), destination.width(), singleThreaded, [&](int begin, int end) {
            if (useAvx2 &&
                detail::downsampleFourTapAvx2(view(source), sourcePixels.left(), sourcePixels.top(),
                                              sourcePixels.right() + 1, sourcePixels.bottom() + 1,
                                              view(destination), factor, begin, end)) {
                executed.store(true, std::memory_order_relaxed);
                return;
            }
            for (int y = begin; y < end; ++y) {
                auto* output = reinterpret_cast<QRgb*>(destination.scanLine(y));
                const int top = sourcePixels.top() + y * factor;
                const int bottom = std::min(sourcePixels.bottom() + 1, top + factor);
                for (int x = 0; x < destination.width(); ++x) {
                    const int left = sourcePixels.left() + x * factor;
                    const int right = std::min(sourcePixels.right() + 1, left + factor);
                    // Four stratified taps cap reduction cost independently of the factor;
                    // the following blur and bilinear reconstruction suppress the aliasing.
                    const int sampleX[] = {
                        left + (right - left) / 4,
                        left + ((right - left) * 3) / 4,
                    };
                    const int sampleY[] = {
                        top + (bottom - top) / 4,
                        top + ((bottom - top) * 3) / 4,
                    };
                    const int xCount = sampleX[0] == sampleX[1] ? 1 : 2;
                    const int yCount = sampleY[0] == sampleY[1] ? 1 : 2;
                    int alpha = 0;
                    int red = 0;
                    int green = 0;
                    int blue = 0;
                    for (int sy = 0; sy < yCount; ++sy) {
                        const auto* input =
                            reinterpret_cast<const QRgb*>(source.constScanLine(sampleY[sy]));
                        for (int sx = 0; sx < xCount; ++sx) {
                            const QRgb pixel = input[sampleX[sx]];
                            alpha += qAlpha(pixel);
                            red += qRed(pixel);
                            green += qGreen(pixel);
                            blue += qBlue(pixel);
                        }
                    }
                    const int count = xCount * yCount;
                    if (count == 4) {
                        output[x] = qRgba(red >> 2, green >> 2, blue >> 2, alpha >> 2);
                    } else if (count == 2) {
                        output[x] = qRgba(red >> 1, green >> 1, blue >> 1, alpha >> 1);
                    } else {
                        output[x] = qRgba(red, green, blue, alpha);
                    }
                }
            }
        });
    if (avx2Executed != nullptr) {
        *avx2Executed = executed.load(std::memory_order_relaxed);
    }
    return jobs;
}

std::size_t downsample(const QImage& source, QImage& destination, int factor, bool singleThreaded,
                       bool useAvx2, bool* avx2Executed) {
    return downsample(source, source.rect(), destination, factor, singleThreaded, useAvx2,
                      avx2Executed);
}

struct AxisSample {
    int first = 0;
    int second = 0;
    int weight = 0;
};

template <int Factor> AxisSample axisSample(int coordinate, int extent) {
    constexpr int denominator = Factor * 2;
    const int numerator = coordinate * 2 + 1 - Factor;
    const int base =
        numerator >= 0 ? numerator / denominator : -((-numerator + denominator - 1) / denominator);
    const int remainder = numerator - base * denominator;
    return {
        qBound(0, base, extent - 1),
        qBound(0, base + 1, extent - 1),
        (remainder * 256 + Factor) / denominator,
    };
}

inline QRgb interpolatePixel(QRgb first, QRgb second, int weight) {
    if (weight <= 0 || first == second) {
        return first;
    }
    if (weight >= 256) {
        return second;
    }
    constexpr std::uint32_t lanes = 0x00ff00ffu;
    constexpr std::uint32_t rounding = 0x00800080u;
    const std::uint32_t inverse = static_cast<std::uint32_t>(256 - weight);
    const std::uint32_t mix = static_cast<std::uint32_t>(weight);
    const std::uint32_t redBlue =
        ((((first & lanes) * inverse) + ((second & lanes) * mix) + rounding) >> 8) & lanes;
    const std::uint32_t alphaGreen =
        (((((first >> 8) & lanes) * inverse) + (((second >> 8) & lanes) * mix) + rounding) >> 8) &
        lanes;
    return redBlue | (alphaGreen << 8);
}

template <int Factor>
void prepareAxisSamples(std::vector<AxisSample>& samples, int firstCoordinate, int count,
                        int extent) {
    samples.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        samples[static_cast<std::size_t>(index)] =
            axisSample<Factor>(firstCoordinate + index, extent);
    }
}

template <int Factor>
std::size_t upsampleBilinearImpl(const QImage& source, QImage& destination, bool singleThreaded,
                                 bool useAvx2, bool* avx2Executed) {
    thread_local std::vector<AxisSample> xSamples;
    thread_local std::vector<AxisSample> ySamples;
    prepareAxisSamples<Factor>(xSamples, 0, destination.width(), source.width());
    prepareAxisSamples<Factor>(ySamples, 0, destination.height(), source.height());
    const AxisSample* const horizontalSamples = xSamples.data();
    const AxisSample* const verticalSamples = ySamples.data();
    std::atomic_bool executed = false;
    const std::size_t jobs = parallelRows(
        destination.height(), destination.width(), singleThreaded, [&](int begin, int end) {
            thread_local std::vector<QRgb> firstExpanded;
            thread_local std::vector<QRgb> secondExpanded;
            firstExpanded.resize(static_cast<std::size_t>(destination.width()));
            secondExpanded.resize(static_cast<std::size_t>(destination.width()));
            int firstSourceRow = -1;
            int secondSourceRow = -1;
            const auto expandRow = [&](int sourceRow, std::vector<QRgb>& expanded) {
                const auto* line = reinterpret_cast<const QRgb*>(source.constScanLine(sourceRow));
                for (int x = 0; x < destination.width(); ++x) {
                    const AxisSample horizontal = horizontalSamples[x];
                    expanded[static_cast<std::size_t>(x)] = interpolatePixel(
                        line[horizontal.first], line[horizontal.second], horizontal.weight);
                }
            };
            for (int y = begin; y < end; ++y) {
                auto* output = reinterpret_cast<QRgb*>(destination.scanLine(y));
                const AxisSample vertical = verticalSamples[y];
                if (vertical.first == secondSourceRow) {
                    firstExpanded.swap(secondExpanded);
                    firstSourceRow = secondSourceRow;
                    secondSourceRow = -1;
                }
                if (vertical.first != firstSourceRow) {
                    expandRow(vertical.first, firstExpanded);
                    firstSourceRow = vertical.first;
                }
                if (vertical.second != firstSourceRow && vertical.second != secondSourceRow) {
                    expandRow(vertical.second, secondExpanded);
                    secondSourceRow = vertical.second;
                }
                const QRgb* const line0 = firstExpanded.data();
                const QRgb* const line1 = vertical.second == firstSourceRow ? firstExpanded.data()
                                                                            : secondExpanded.data();
                int x = useAvx2 ? detail::interpolateAndBlendConstantAvx2(line0, line1, output,
                                                                          destination.width(),
                                                                          vertical.weight, 255)
                                : 0;
                if (x > 0) {
                    executed.store(true, std::memory_order_relaxed);
                }
                for (; x < destination.width(); ++x) {
                    output[x] = interpolatePixel(line0[x], line1[x], vertical.weight);
                }
            }
        });
    if (avx2Executed != nullptr && executed.load(std::memory_order_relaxed)) {
        *avx2Executed = true;
    }
    return jobs;
}

std::size_t upsampleBilinear(const QImage& source, QImage& destination, int factor,
                             bool singleThreaded, bool useAvx2, bool* avx2Executed) {
    if (factor <= 1) {
        std::memcpy(destination.bits(), source.constBits(), destination.sizeInBytes());
        return 0;
    }
    switch (factor) {
    case 2:
        return upsampleBilinearImpl<2>(source, destination, singleThreaded, useAvx2, avx2Executed);
    case 4:
        return upsampleBilinearImpl<4>(source, destination, singleThreaded, useAvx2, avx2Executed);
    case 8:
        return upsampleBilinearImpl<8>(source, destination, singleThreaded, useAvx2, avx2Executed);
    case 16:
        return upsampleBilinearImpl<16>(source, destination, singleThreaded, useAvx2, avx2Executed);
    case 32:
        return upsampleBilinearImpl<32>(source, destination, singleThreaded, useAvx2, avx2Executed);
    default:
        return upsampleBilinearImpl<64>(source, destination, singleThreaded, useAvx2, avx2Executed);
    }
}

template <int Factor>
std::size_t upsampleBilinearCompositedImpl(const QImage& source, QImage& destination,
                                           AlphaView maskView, const QPoint& maskOrigin,
                                           const QRect& destinationPixels,
                                           const QRect& sourcePixels, int constantMix,
                                           bool singleThreaded, bool useAvx2, bool* avx2Executed) {
    const ConstImageView sourceView = view(source);
    const ImageView destinationView = view(destination);
    thread_local std::vector<AxisSample> xSamples;
    thread_local std::vector<AxisSample> ySamples;
    prepareAxisSamples<Factor>(xSamples, destinationPixels.left() - sourcePixels.left(),
                               destinationPixels.width(), source.width());
    prepareAxisSamples<Factor>(ySamples, destinationPixels.top() - sourcePixels.top(),
                               destinationPixels.height(), source.height());
    const AxisSample* const horizontalSamples = xSamples.data();
    const AxisSample* const verticalSamples = ySamples.data();
    std::atomic_bool executed = false;
    const std::size_t jobs = parallelRows(
        destinationPixels.height(), destinationPixels.width(), singleThreaded,
        [&](int begin, int end) {
            thread_local std::vector<QRgb> firstExpanded;
            thread_local std::vector<QRgb> secondExpanded;
            firstExpanded.resize(static_cast<std::size_t>(destinationPixels.width()));
            secondExpanded.resize(static_cast<std::size_t>(destinationPixels.width()));
            int firstSourceRow = -1;
            int secondSourceRow = -1;
            const auto expandRow = [&](int sourceRow, std::vector<QRgb>& expanded) {
                const auto* line = reinterpret_cast<const QRgb*>(
                    sourceView.data + static_cast<qsizetype>(sourceRow) * sourceView.stride);
                for (int localX = 0; localX < destinationPixels.width(); ++localX) {
                    const AxisSample horizontal = horizontalSamples[localX];
                    expanded[static_cast<std::size_t>(localX)] = interpolatePixel(
                        line[horizontal.first], line[horizontal.second], horizontal.weight);
                }
            };
            for (int localY = begin; localY < end; ++localY) {
                const int y = destinationPixels.top() + localY;
                auto* output = reinterpret_cast<QRgb*>(
                    destinationView.data + static_cast<qsizetype>(y) * destinationView.stride);
                const auto* alphaLine =
                    maskView.data == nullptr
                        ? nullptr
                        : maskView.data +
                              static_cast<qsizetype>(y - maskOrigin.y()) * maskView.stride;
                const AxisSample vertical = verticalSamples[localY];
                if (vertical.first == secondSourceRow) {
                    firstExpanded.swap(secondExpanded);
                    firstSourceRow = secondSourceRow;
                    secondSourceRow = -1;
                }
                if (vertical.first != firstSourceRow) {
                    expandRow(vertical.first, firstExpanded);
                    firstSourceRow = vertical.first;
                }
                if (vertical.second != firstSourceRow && vertical.second != secondSourceRow) {
                    expandRow(vertical.second, secondExpanded);
                    secondSourceRow = vertical.second;
                }
                const QRgb* const line0 = firstExpanded.data();
                const QRgb* const line1 = vertical.second == firstSourceRow ? firstExpanded.data()
                                                                            : secondExpanded.data();
                int localX = 0;
                if (useAvx2) {
                    localX = alphaLine == nullptr
                                 ? detail::interpolateAndBlendConstantAvx2(
                                       line0, line1, output + destinationPixels.left(),
                                       destinationPixels.width(), vertical.weight, constantMix)
                                 : detail::interpolateAndBlendMaskedAvx2(
                                       line0, line1, output + destinationPixels.left(),
                                       alphaLine + destinationPixels.left() - maskOrigin.x(),
                                       destinationPixels.width(), vertical.weight);
                    if (localX > 0) {
                        executed.store(true, std::memory_order_relaxed);
                    }
                }
                for (; localX < destinationPixels.width(); ++localX) {
                    const int x = destinationPixels.left() + localX;
                    const int mix =
                        alphaLine == nullptr ? constantMix : alphaLine[x - maskOrigin.x()];
                    if (mix == 0) {
                        continue;
                    }
                    const QRgb effect =
                        interpolatePixel(line0[localX], line1[localX], vertical.weight);
                    output[x] = mix == 255 ? effect : blendPremultiplied(output[x], effect, mix);
                }
            }
        });
    if (avx2Executed != nullptr && executed.load(std::memory_order_relaxed)) {
        *avx2Executed = true;
    }
    return jobs;
}

std::size_t upsampleBilinearComposited(const QImage& source, QImage& destination, AlphaView mask,
                                       const QPoint& maskOrigin, const QRect& destinationPixels,
                                       const QRect& sourcePixels, int factor, int constantMix,
                                       bool singleThreaded, bool useAvx2, bool* avx2Executed) {
    switch (factor) {
    case 1:
        return upsampleBilinearCompositedImpl<1>(source, destination, mask, maskOrigin,
                                                 destinationPixels, sourcePixels, constantMix,
                                                 singleThreaded, useAvx2, avx2Executed);
    case 2:
        return upsampleBilinearCompositedImpl<2>(source, destination, mask, maskOrigin,
                                                 destinationPixels, sourcePixels, constantMix,
                                                 singleThreaded, useAvx2, avx2Executed);
    case 4:
        return upsampleBilinearCompositedImpl<4>(source, destination, mask, maskOrigin,
                                                 destinationPixels, sourcePixels, constantMix,
                                                 singleThreaded, useAvx2, avx2Executed);
    case 8:
        return upsampleBilinearCompositedImpl<8>(source, destination, mask, maskOrigin,
                                                 destinationPixels, sourcePixels, constantMix,
                                                 singleThreaded, useAvx2, avx2Executed);
    case 16:
        return upsampleBilinearCompositedImpl<16>(source, destination, mask, maskOrigin,
                                                  destinationPixels, sourcePixels, constantMix,
                                                  singleThreaded, useAvx2, avx2Executed);
    case 32:
        return upsampleBilinearCompositedImpl<32>(source, destination, mask, maskOrigin,
                                                  destinationPixels, sourcePixels, constantMix,
                                                  singleThreaded, useAvx2, avx2Executed);
    default:
        return upsampleBilinearCompositedImpl<64>(source, destination, mask, maskOrigin,
                                                  destinationPixels, sourcePixels, constantMix,
                                                  singleThreaded, useAvx2, avx2Executed);
    }
}

GaussianBlurPlan makeGaussianBlurPlan(const Parameters& parameters) {
    const double sigma =
        std::max(0.0, parameters.logicalSigma * static_cast<double>(parameters.devicePixelRatio));
    // Keep the reduced kernel compact. Bilinear reconstruction supplies the final
    // low-pass stage, so product rendering can reduce one level more aggressively
    // than the reference-quality plan without exposing block boundaries.
    constexpr std::array<std::pair<double, int>, 7> bands{{
        {2.0, 1},
        {4.0, 2},
        {8.0, 4},
        {16.0, 8},
        {32.0, 16},
        {128.0, 32},
        {std::numeric_limits<double>::infinity(), 64},
    }};
    int factor = 1;
    for (const auto& [upperSigma, reduction] : bands) {
        if (sigma < upperSigma) {
            factor = reduction;
            break;
        }
    }

    GaussianBlurPlan plan;
    plan.reductionFactor = factor;
    plan.passCount = 3;
    const std::array<int, 3> radii = gaussianBoxRadii(sigma / factor);
    for (int index = 0; index < plan.passCount; ++index) {
        plan.radii[index] = radii[index];
    }
    const int reducedSupport = plan.radii[0] + plan.radii[1] + plan.radii[2];
    // One reduced pixel covers the bilinear neighbor and one covers the farthest
    // stratified downsample tap. Box support itself is the sum of pass radii.
    plan.physicalSupportRadius = reducedSupport * factor + (factor > 1 ? 2 * factor : 0);
    return plan;
}

template <typename Function> void measureStage(std::uint64_t& destination, Function&& function) {
    const auto begin = std::chrono::steady_clock::now();
    function();
    destination += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - begin)
                                                  .count());
}

bool blur(QImage& image, const Parameters& parameters, RenderWorkspace& workspace,
          const ExecutionOptions& options) {
    const GaussianBlurPlan plan = makeGaussianBlurPlan(parameters);
    const int factor = plan.reductionFactor;
    KernelDiagnostics& diagnostics = const_cast<KernelDiagnostics&>(workspace.diagnostics());
    diagnostics.adaptiveBlurFactor = factor;
    const bool useAvx2 = !options.forceScalar && selectedSimdBackend() == SimdBackend::Avx2;
    if (factor == 1) {
        QImage& scratch = workspace.argbScratchB(image.size(), image.devicePixelRatio());
        if (scratch.isNull()) {
            return false;
        }
        measureStage(diagnostics.reducedBlurNanoseconds, [&] {
            for (int index = 0; index < plan.passCount; ++index) {
                const int radius = plan.radii[index];
                if (radius <= 0) {
                    continue;
                }
                const BoxAverage average = boxAverage(radius * 2 + 1);
                diagnostics.parallelJobs +=
                    horizontalBoxBlur(image, scratch, radius, average, options.singleThreaded);
                diagnostics.parallelJobs +=
                    verticalBoxBlur(scratch, image, radius, average, options.singleThreaded);
                ++diagnostics.gaussianPasses;
            }
        });
        return true;
    }
    const QSize reducedSize((image.width() + factor - 1) / factor,
                            (image.height() + factor - 1) / factor);
    QImage& a = workspace.argbScratchA(reducedSize, image.devicePixelRatio());
    QImage& b = workspace.argbScratchB(reducedSize, image.devicePixelRatio());
    if (a.isNull() || b.isNull()) {
        return false;
    }
    bool downsampleAvx2Executed = false;
    bool reconstructionAvx2Executed = false;
    measureStage(diagnostics.downsampleNanoseconds, [&] {
        diagnostics.parallelJobs +=
            downsample(image, a, factor, options.singleThreaded, useAvx2, &downsampleAvx2Executed);
    });
    diagnostics.copiedBytes += image.sizeInBytes() + a.sizeInBytes();

    measureStage(diagnostics.reducedBlurNanoseconds, [&] {
        for (int index = 0; index < plan.passCount; ++index) {
            const int radius = plan.radii[index];
            if (radius <= 0) {
                continue;
            }
            const BoxAverage average = boxAverage(radius * 2 + 1);
            diagnostics.parallelJobs +=
                horizontalBoxBlur(a, b, radius, average, options.singleThreaded);
            diagnostics.parallelJobs +=
                verticalBoxBlur(b, a, radius, average, options.singleThreaded);
            ++diagnostics.gaussianPasses;
        }
    });
    measureStage(diagnostics.reconstructionNanoseconds, [&] {
        diagnostics.parallelJobs += upsampleBilinear(a, image, factor, options.singleThreaded,
                                                     useAvx2, &reconstructionAvx2Executed);
    });
    diagnostics.copiedBytes += image.sizeInBytes();
    if (downsampleAvx2Executed) {
        ++diagnostics.gaussianDownsampleAvx2Executions;
    }
    if (reconstructionAvx2Executed) {
        ++diagnostics.gaussianReconstructionAvx2Executions;
    }
    if (downsampleAvx2Executed || reconstructionAvx2Executed) {
        ++diagnostics.gaussianAvx2Executions;
        diagnostics.backend = SimdBackend::Avx2;
    }
    return true;
}

bool blurMasked(const QImage& source, QImage& destination, AlphaView mask, const QPoint& maskOrigin,
                const QRegion& destinationRegion, int constantMix, const Parameters& parameters,
                RenderWorkspace& workspace, const ExecutionOptions& options) {
    const QRect destinationPixels = destinationRegion.boundingRect();
    const GaussianBlurPlan plan = makeGaussianBlurPlan(parameters);
    const int support = plan.physicalSupportRadius;
    const int factor = plan.reductionFactor;
    const QRect requestedSourcePixels =
        destinationPixels.adjusted(-support, -support, support, support).intersected(source.rect());
    if (requestedSourcePixels.isEmpty()) {
        return true;
    }
    const auto alignDown = [factor](int value, int origin) {
        int remainder = (value - origin) % factor;
        if (remainder < 0) {
            remainder += factor;
        }
        return value - remainder;
    };
    const auto alignUp = [&](int value, int origin) {
        const int aligned = alignDown(value, origin);
        return aligned == value ? value : aligned + factor;
    };
    const int originX = qRound(parameters.gridOriginInImage.x());
    const int originY = qRound(parameters.gridOriginInImage.y());
    const int left = alignUp(requestedSourcePixels.left(), originX);
    const int top = alignUp(requestedSourcePixels.top(), originY);
    const int rightExclusive = alignDown(requestedSourcePixels.right() + 1, originX);
    const int bottomExclusive = alignDown(requestedSourcePixels.bottom() + 1, originY);
    const QRect alignedSourcePixels(left, top, rightExclusive - left, bottomExclusive - top);
    const QRect sourcePixels =
        alignedSourcePixels.isEmpty() ? requestedSourcePixels : alignedSourcePixels;
    const QSize reducedSize((sourcePixels.width() + factor - 1) / factor,
                            (sourcePixels.height() + factor - 1) / factor);
    QImage& a = workspace.argbScratchA(reducedSize, source.devicePixelRatio());
    QImage& b = workspace.argbScratchB(reducedSize, source.devicePixelRatio());
    if (a.isNull() || b.isNull()) {
        return false;
    }
    KernelDiagnostics& diagnostics = const_cast<KernelDiagnostics&>(workspace.diagnostics());
    diagnostics.adaptiveBlurFactor = factor;
    bool downsampleAvx2Executed = false;
    bool reconstructionAvx2Executed = false;
    const bool useAvx2 = !options.forceScalar && selectedSimdBackend() == SimdBackend::Avx2;
    measureStage(diagnostics.downsampleNanoseconds, [&] {
        diagnostics.parallelJobs +=
            downsample(source, sourcePixels, a, factor, options.singleThreaded, useAvx2,
                       &downsampleAvx2Executed);
    });
    diagnostics.copiedBytes += static_cast<std::size_t>(sourcePixels.width()) *
                                   static_cast<std::size_t>(sourcePixels.height()) * sizeof(QRgb) +
                               a.sizeInBytes();
    measureStage(diagnostics.reducedBlurNanoseconds, [&] {
        for (int index = 0; index < plan.passCount; ++index) {
            const int radius = plan.radii[index];
            if (radius <= 0) {
                continue;
            }
            const BoxAverage average = boxAverage(radius * 2 + 1);
            diagnostics.parallelJobs +=
                horizontalBoxBlur(a, b, radius, average, options.singleThreaded);
            diagnostics.parallelJobs +=
                verticalBoxBlur(b, a, radius, average, options.singleThreaded);
            ++diagnostics.gaussianPasses;
        }
    });
    measureStage(diagnostics.reconstructionNanoseconds, [&] {
        for (const QRect& rect : destinationRegion) {
            diagnostics.parallelJobs += upsampleBilinearComposited(
                a, destination, mask, maskOrigin, rect, sourcePixels, factor, constantMix,
                options.singleThreaded, useAvx2, &reconstructionAvx2Executed);
        }
    });
    for (const QRect& rect : destinationRegion) {
        diagnostics.copiedBytes += static_cast<std::size_t>(rect.width()) *
                                   static_cast<std::size_t>(rect.height()) * sizeof(QRgb);
    }
    if (downsampleAvx2Executed) {
        ++diagnostics.gaussianDownsampleAvx2Executions;
    }
    if (reconstructionAvx2Executed) {
        ++diagnostics.gaussianReconstructionAvx2Executions;
    }
    if (downsampleAvx2Executed || reconstructionAvx2Executed) {
        ++diagnostics.gaussianAvx2Executions;
        diagnostics.backend = SimdBackend::Avx2;
    }
    return true;
}

std::size_t mosaic(QImage& image, const Parameters& parameters, RenderWorkspace& workspace,
                   bool singleThreaded) {
    const MosaicGrid grid = mosaicGrid(image.size(), parameters);
    std::vector<QRgb>& samples =
        workspace.mosaicSampleScratch(static_cast<std::size_t>(grid.columnCount) * grid.rowCount);
    collectMosaicSamples(view(static_cast<const QImage&>(image)), grid, 0, grid.columnCount, 0,
                         grid.rowCount, samples);
    return parallelRows(image.height(), image.width(), singleThreaded, [&](int begin, int end) {
        for (int py = begin; py < end; ++py) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(py));
            const int row = (py - grid.firstY) / grid.block;
            const std::size_t sampleOffset = static_cast<std::size_t>(row) * grid.columnCount;
            for (int column = 0; column < grid.columnCount; ++column) {
                const int left = qMax(0, grid.firstX + column * grid.block);
                const int right = qMin(image.width(), grid.firstX + (column + 1) * grid.block);
                std::fill(line + left, line + right, samples[sampleOffset + column]);
            }
        }
    });
}

std::size_t colorEffect(QImage& image, std::uint32_t type, int mix, bool singleThreaded,
                        bool forceScalar, bool* usedSimd) {
    if (mix <= 0) {
        return 0;
    }
    std::atomic_bool simdExecuted{false};
    const ImageView imageView = view(image);
    const std::size_t jobs =
        parallelRows(image.height(), image.width(), singleThreaded, [&](int begin, int end) {
            if (!forceScalar && selectedSimdBackend() == SimdBackend::Avx2) {
                const bool executed = type == 2 ? detail::grayscaleAvx2(imageView, begin, end, mix)
                                                : detail::invertAvx2(imageView, begin, end, mix);
                if (executed) {
                    simdExecuted.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            for (int y = begin; y < end; ++y) {
                auto* line = reinterpret_cast<QRgb*>(imageView.data +
                                                     static_cast<qsizetype>(y) * imageView.stride);
                for (int x = 0; x < imageView.width; ++x) {
                    const QRgb pixel = line[x];
                    const QRgb transformed = transformedColor(pixel, type);
                    line[x] =
                        mix == 255 ? transformed : blendPremultiplied(pixel, transformed, mix);
                }
            }
        });
    if (usedSimd != nullptr && simdExecuted.load(std::memory_order_relaxed)) {
        *usedSimd = true;
    }
    return jobs;
}

std::size_t colorEffectRect(const QImage& source, QImage& destination, const QRect& pixels,
                            std::uint32_t type, int mix, bool singleThreaded, bool forceScalar,
                            bool* usedSimd) {
    if (pixels.isEmpty() || mix <= 0) {
        return 0;
    }
    const ConstImageView sourceView = view(source);
    const ImageView destinationView = view(destination);
    std::atomic_bool simdExecuted{false};
    const std::size_t jobs =
        parallelRows(pixels.height(), pixels.width(), singleThreaded, [&](int begin, int end) {
            if (!forceScalar && selectedSimdBackend() == SimdBackend::Avx2) {
                const bool executed =
                    type == 2
                        ? detail::grayscaleRectAvx2(sourceView, destinationView, pixels.left(),
                                                    pixels.top() + begin, pixels.right() + 1,
                                                    pixels.top() + end, mix)
                        : detail::invertRectAvx2(sourceView, destinationView, pixels.left(),
                                                 pixels.top() + begin, pixels.right() + 1,
                                                 pixels.top() + end, mix);
                if (executed) {
                    simdExecuted.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            for (int localY = begin; localY < end; ++localY) {
                const int y = pixels.top() + localY;
                const auto* sourceLine = reinterpret_cast<const QRgb*>(
                    sourceView.data + static_cast<qsizetype>(y) * sourceView.stride);
                auto* destinationLine = reinterpret_cast<QRgb*>(
                    destinationView.data + static_cast<qsizetype>(y) * destinationView.stride);
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const QRgb transformed = transformedColor(sourceLine[x], type);
                    destinationLine[x] =
                        mix == 255 ? transformed
                                   : blendPremultiplied(destinationLine[x], transformed, mix);
                }
            }
        });
    if (usedSimd != nullptr && simdExecuted.load(std::memory_order_relaxed)) {
        *usedSimd = true;
    }
    return jobs;
}

} // namespace

struct RenderWorkspace::PoolEntry {
    QImage storage;
    QImage::Format format = QImage::Format_Invalid;
    std::uint64_t lastUsed = 0;
    int lease = -1;
};

RenderWorkspace::RenderWorkspace(std::size_t retainedByteLimit)
    : m_retainedByteLimit(retainedByteLimit) {}

RenderWorkspace::~RenderWorkspace() = default;

void RenderWorkspace::releaseLease(PoolEntry*& entry, QImage& image) {
    image = {};
    if (entry != nullptr) {
        entry->lease = -1;
        entry->lastUsed = ++m_poolClock;
        entry = nullptr;
    }
}

QImage& RenderWorkspace::ensureImage(QImage& image, PoolEntry*& entry, int lease, const QSize& size,
                                     QImage::Format format, qreal dpr) {
    if (size.isEmpty() || m_failAllocationsForTests) {
        releaseLease(entry, image);
        return image;
    }
    const auto bucket = [](int value) { return ((std::max(1, value) + 63) / 64) * 64; };
    const QSize bucketSize =
        m_retainedByteLimit == 0 ? size : QSize(bucket(size.width()), bucket(size.height()));
    if (entry != nullptr && entry->storage.size() == bucketSize && entry->format == format) {
        ++m_diagnostics.scratchReuseCount;
    } else {
        releaseLease(entry, image);
        auto found = std::find_if(m_pool.begin(), m_pool.end(), [&](const PoolEntry& candidate) {
            return candidate.lease < 0 && candidate.format == format &&
                   candidate.storage.size() == bucketSize;
        });
        if (found == m_pool.end()) {
            QImage storage(bucketSize, format);
            if (storage.isNull()) {
                return image;
            }
            m_diagnostics.allocatedBytes += storage.sizeInBytes();
            m_pool.push_back(PoolEntry{std::move(storage), format, ++m_poolClock, lease});
            entry = &m_pool.back();
        } else {
            entry = &*found;
            entry->lease = lease;
            entry->lastUsed = ++m_poolClock;
            ++m_diagnostics.scratchReuseCount;
        }
    }
    entry->lease = lease;
    entry->lastUsed = ++m_poolClock;
    image = QImage(entry->storage.bits(), size.width(), size.height(),
                   entry->storage.bytesPerLine(), format);
    image.setDevicePixelRatio(dpr);
    return image;
}

QImage& RenderWorkspace::argbScratchA(const QSize& size, qreal devicePixelRatio) {
    return ensureImage(m_argbA, m_argbAEntry, 0, size, QImage::Format_ARGB32_Premultiplied,
                       devicePixelRatio);
}

QImage& RenderWorkspace::argbScratchB(const QSize& size, qreal devicePixelRatio) {
    return ensureImage(m_argbB, m_argbBEntry, 1, size, QImage::Format_ARGB32_Premultiplied,
                       devicePixelRatio);
}

QImage& RenderWorkspace::sceneScratch(const QSize& size, qreal devicePixelRatio) {
    return ensureImage(m_scene, m_sceneEntry, 3, size, QImage::Format_ARGB32_Premultiplied,
                       devicePixelRatio);
}

QImage& RenderWorkspace::preLayerScratch(const QSize& size, qreal devicePixelRatio) {
    return ensureImage(m_preLayer, m_preLayerEntry, 4, size, QImage::Format_ARGB32_Premultiplied,
                       devicePixelRatio);
}

QImage& RenderWorkspace::alphaScratch(const QSize& size, qreal devicePixelRatio) {
    return ensureImage(m_alpha, m_alphaEntry, 2, size, QImage::Format_Alpha8, devicePixelRatio);
}

std::vector<QRgb>& RenderWorkspace::mosaicSampleScratch(std::size_t count) {
    if (m_mosaicSamples.capacity() < count) {
        const std::size_t previousCapacity = m_mosaicSamples.capacity();
        m_mosaicSamples.reserve(count);
        m_diagnostics.allocatedBytes +=
            (m_mosaicSamples.capacity() - previousCapacity) * sizeof(QRgb);
    } else {
        ++m_diagnostics.scratchReuseCount;
    }
    m_mosaicSamples.resize(count);
    return m_mosaicSamples;
}

void RenderWorkspace::clear() {
    releaseLease(m_argbAEntry, m_argbA);
    releaseLease(m_argbBEntry, m_argbB);
    releaseLease(m_sceneEntry, m_scene);
    releaseLease(m_preLayerEntry, m_preLayer);
    releaseLease(m_alphaEntry, m_alpha);
    m_pool.clear();
    std::vector<QRgb>().swap(m_mosaicSamples);
    m_poolClock = 0;
    m_diagnostics = {};
}

void RenderWorkspace::finishFrame(bool releaseAll) {
    constexpr qsizetype kMaximumRetainedScratchBytes = 16 * 1024 * 1024;
    releaseLease(m_argbAEntry, m_argbA);
    releaseLease(m_argbBEntry, m_argbB);
    releaseLease(m_sceneEntry, m_scene);
    releaseLease(m_preLayerEntry, m_preLayer);
    releaseLease(m_alphaEntry, m_alpha);
    if (releaseAll) {
        m_pool.clear();
        std::vector<QRgb>().swap(m_mosaicSamples);
    } else {
        for (auto iterator = m_pool.begin(); iterator != m_pool.end();) {
            if (iterator->lease < 0 &&
                iterator->storage.sizeInBytes() > kMaximumRetainedScratchBytes) {
                iterator = m_pool.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (m_mosaicSamples.capacity() * sizeof(QRgb) >
            static_cast<std::size_t>(kMaximumRetainedScratchBytes)) {
            std::vector<QRgb>().swap(m_mosaicSamples);
        }
        while (retainedBytes() > m_retainedByteLimit && !m_pool.empty()) {
            const auto victim = std::min_element(m_pool.begin(), m_pool.end(),
                                                 [](const PoolEntry& left, const PoolEntry& right) {
                                                     return left.lastUsed < right.lastUsed;
                                                 });
            m_pool.erase(victim);
        }
        if (retainedBytes() > m_retainedByteLimit) {
            std::vector<QRgb>().swap(m_mosaicSamples);
        }
    }
    m_diagnostics.retainedBytes = retainedBytes();
}

const KernelDiagnostics& RenderWorkspace::diagnostics() const {
    return m_diagnostics;
}
void RenderWorkspace::resetDiagnostics() {
    m_diagnostics = {};
}

std::size_t RenderWorkspace::retainedBytes() const {
    std::size_t retained = m_mosaicSamples.capacity() * sizeof(QRgb);
    for (const PoolEntry& entry : m_pool) {
        retained += static_cast<std::size_t>(entry.storage.sizeInBytes());
    }
    return retained;
}

void RenderWorkspace::setAllocationFailureForTests(bool fail) {
    m_failAllocationsForTests = fail;
}

ConstImageView view(const QImage& image) {
    return {image.constBits(), image.width(), image.height(), image.bytesPerLine()};
}

ImageView view(QImage& image) {
    return {image.bits(), image.width(), image.height(), image.bytesPerLine()};
}

AlphaView alphaView(const QImage& image) {
    return {image.constBits(), image.width(), image.height(), image.bytesPerLine()};
}

SimdBackend selectedSimdBackend() {
    static const SimdBackend backend = [] {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int registers[4]{};
        __cpuid(registers, 1);
        const bool osxsave = (registers[2] & (1 << 27)) != 0;
        const bool avx = (registers[2] & (1 << 28)) != 0;
        if (!osxsave || !avx || (_xgetbv(0) & 0x6) != 0x6) {
            return SimdBackend::Scalar;
        }
        __cpuidex(registers, 7, 0);
        return (registers[1] & (1 << 5)) != 0 ? SimdBackend::Avx2 : SimdBackend::Scalar;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
        return __builtin_cpu_supports("avx2") ? SimdBackend::Avx2 : SimdBackend::Scalar;
#else
        return SimdBackend::Scalar;
#endif
    }();
    return backend;
}

const char* simdBackendName(SimdBackend backend) {
    return backend == SimdBackend::Avx2 ? "avx2" : "scalar";
}

int samplingRadiusPixels(const Parameters& parameters) {
    if (parameters.type == 1) {
        return makeGaussianBlurPlan(parameters).physicalSupportRadius;
    }
    return qMax(0, qCeil(parameters.logicalSamplingRadius * parameters.devicePixelRatio));
}

GaussianBlurPlan gaussianBlurPlan(const Parameters& parameters) {
    return makeGaussianBlurPlan(parameters);
}

void apply(QImage& image, const Parameters& parameters, RenderWorkspace* workspace,
           const ExecutionOptions& options) {
    if (image.isNull()) {
        return;
    }
    if ((parameters.type == 2 || parameters.type == 3) &&
        normalizedStrengthMix(parameters.strength) == 0) {
        return;
    }
    if (image.format() != QImage::Format_ARGB32_Premultiplied) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    image.detach();
    RenderWorkspace localWorkspace(0);
    RenderWorkspace& activeWorkspace = workspace != nullptr ? *workspace : localWorkspace;
    KernelDiagnostics& diagnostics = const_cast<KernelDiagnostics&>(activeWorkspace.diagnostics());
    switch (parameters.type) {
    case 0:
        diagnostics.parallelJobs +=
            mosaic(image, parameters, activeWorkspace, options.singleThreaded);
        break;
    case 1:
        (void)blur(image, parameters, activeWorkspace, options);
        break;
    case 2:
    case 3: {
        bool usedSimd = false;
        diagnostics.parallelJobs +=
            colorEffect(image, parameters.type, normalizedStrengthMix(parameters.strength),
                        options.singleThreaded, options.forceScalar, &usedSimd);
        if (usedSimd) {
            diagnostics.backend = SimdBackend::Avx2;
        }
        break;
    }
    default:
        break;
    }
    if (workspace == nullptr) {
        localWorkspace.finishFrame(true);
    }
}

bool applyMasked(const QImage& source, QImage& destination, const QImage& mask,
                 const QRect& destinationPixels, const Parameters& parameters,
                 RenderWorkspace* workspace, const ExecutionOptions& options) {
    return applyMasked(source, destination, mask, QPoint(), destinationPixels, parameters,
                       workspace, options);
}

bool applyMasked(const QImage& source, QImage& destination, const QImage& mask,
                 const QPoint& maskOriginPixels, const QRect& destinationPixels,
                 const Parameters& parameters, RenderWorkspace* workspace,
                 const ExecutionOptions& options) {
    if (source.size() != destination.size() ||
        source.format() != QImage::Format_ARGB32_Premultiplied ||
        destination.format() != QImage::Format_ARGB32_Premultiplied ||
        mask.format() != QImage::Format_Alpha8) {
        return false;
    }
    const QRect pixels = destinationPixels.intersected(destination.rect());
    if (pixels.isEmpty()) {
        return true;
    }
    if (!QRect(maskOriginPixels, mask.size()).contains(pixels)) {
        return false;
    }
    const bool colorEffectType = parameters.type == 2 || parameters.type == 3;
    const int strengthMix = colorEffectType ? normalizedStrengthMix(parameters.strength) : 255;
    if (colorEffectType && strengthMix == 0) {
        return true;
    }
    destination.detach();
    RenderWorkspace localWorkspace(0);
    RenderWorkspace& activeWorkspace = workspace != nullptr ? *workspace : localWorkspace;
    KernelDiagnostics& diagnostics = const_cast<KernelDiagnostics&>(activeWorkspace.diagnostics());
    if (parameters.type == 1) {
        const bool succeeded =
            blurMasked(source, destination, alphaView(mask), maskOriginPixels, QRegion(pixels), -1,
                       parameters, activeWorkspace, options);
        if (workspace == nullptr) {
            localWorkspace.finishFrame(true);
        }
        return succeeded;
    }
    const ConstImageView sourceView = view(source);
    const ImageView destinationView = view(destination);
    const AlphaView maskView = alphaView(mask);
    if (parameters.type == 0) {
        const MosaicGrid grid = mosaicGrid(source.size(), parameters);
        const int firstColumn = (pixels.left() - grid.firstX) / grid.block;
        const int lastColumn = (pixels.right() - grid.firstX) / grid.block;
        const int firstRow = (pixels.top() - grid.firstY) / grid.block;
        const int lastRow = (pixels.bottom() - grid.firstY) / grid.block;
        const int sampleColumnCount = lastColumn - firstColumn + 1;
        const int sampleRowCount = lastRow - firstRow + 1;
        std::vector<QRgb>& samples = activeWorkspace.mosaicSampleScratch(
            static_cast<std::size_t>(sampleColumnCount) * sampleRowCount);
        collectMosaicSamples(sourceView, grid, firstColumn, sampleColumnCount, firstRow,
                             sampleRowCount, samples);
        const std::size_t jobs = parallelRows(
            pixels.height(), pixels.width(), options.singleThreaded, [&](int begin, int end) {
                for (int localY = begin; localY < end; ++localY) {
                    const int y = pixels.top() + localY;
                    auto* destinationLine = reinterpret_cast<QRgb*>(
                        destinationView.data + static_cast<qsizetype>(y) * destinationView.stride);
                    const auto* alphaLine =
                        maskView.data +
                        static_cast<qsizetype>(y - maskOriginPixels.y()) * maskView.stride;
                    const int sampleRow = (y - grid.firstY) / grid.block - firstRow;
                    const std::size_t sampleOffset =
                        static_cast<std::size_t>(sampleRow) * sampleColumnCount;
                    int column = firstColumn;
                    int x = pixels.left();
                    while (x <= pixels.right()) {
                        const int spanEnd =
                            qMin(pixels.right() + 1, grid.firstX + (column + 1) * grid.block);
                        const QRgb sample =
                            samples[sampleOffset + static_cast<std::size_t>(column - firstColumn)];
                        while (x < spanEnd) {
                            const int maskX = x - maskOriginPixels.x();
                            if (alphaLine[maskX] == 0) {
                                ++x;
                                continue;
                            }
                            if (alphaLine[maskX] == 255) {
                                const int opaqueBegin = x;
                                do {
                                    ++x;
                                } while (x < spanEnd && alphaLine[x - maskOriginPixels.x()] == 255);
                                std::fill(destinationLine + opaqueBegin, destinationLine + x,
                                          sample);
                                continue;
                            }
                            destinationLine[x] = blendPremultiplied(
                                destinationLine[x], sample, alphaLine[x - maskOriginPixels.x()]);
                            ++x;
                        }
                        ++column;
                    }
                }
            });
        diagnostics.parallelJobs += jobs;
        if (workspace == nullptr) {
            localWorkspace.finishFrame(true);
        }
        return true;
    }
    std::atomic_bool simdExecuted{false};
    const std::size_t jobs = parallelRows(
        pixels.height(), pixels.width(), options.singleThreaded, [&](int begin, int end) {
            if (colorEffectType && !options.forceScalar &&
                selectedSimdBackend() == SimdBackend::Avx2) {
                const bool executed =
                    parameters.type == 2
                        ? detail::grayscaleMaskedAvx2(
                              sourceView, destinationView, maskView, maskOriginPixels.x(),
                              maskOriginPixels.y(), pixels.left(), pixels.top() + begin,
                              pixels.right() + 1, pixels.top() + end, strengthMix)
                        : detail::invertMaskedAvx2(
                              sourceView, destinationView, maskView, maskOriginPixels.x(),
                              maskOriginPixels.y(), pixels.left(), pixels.top() + begin,
                              pixels.right() + 1, pixels.top() + end, strengthMix);
                if (executed) {
                    simdExecuted.store(true, std::memory_order_relaxed);
                    return;
                }
            }
            for (int localY = begin; localY < end; ++localY) {
                const int y = pixels.top() + localY;
                const auto* sourceLine = reinterpret_cast<const QRgb*>(
                    sourceView.data + static_cast<qsizetype>(y) * sourceView.stride);
                auto* destinationLine = reinterpret_cast<QRgb*>(
                    destinationView.data + static_cast<qsizetype>(y) * destinationView.stride);
                const auto* alphaLine =
                    maskView.data +
                    static_cast<qsizetype>(y - maskOriginPixels.y()) * maskView.stride;
                for (int x = pixels.left(); x <= pixels.right(); ++x) {
                    const int mix =
                        combineCoverage(alphaLine[x - maskOriginPixels.x()], strengthMix);
                    if (mix == 0) {
                        continue;
                    }
                    const QRgb from = sourceLine[x];
                    const QRgb to = transformedColor(from, parameters.type);
                    destinationLine[x] =
                        mix == 255 ? to : blendPremultiplied(destinationLine[x], to, mix);
                }
            }
        });
    diagnostics.parallelJobs += jobs;
    if (simdExecuted.load(std::memory_order_relaxed)) {
        diagnostics.backend = SimdBackend::Avx2;
    }
    if (workspace == nullptr) {
        localWorkspace.finishFrame(true);
    }
    return true;
}

bool applyMaskedSparse(const QImage& source, QImage& destination, const QImage& mask,
                       const QPoint& maskOriginPixels, const QRect& destinationPixels,
                       const std::vector<MaskSpan>& spans, const std::vector<QRect>& occupiedBlocks,
                       const Parameters& parameters, RenderWorkspace* workspace,
                       const ExecutionOptions& options) {
    if (source.size() != destination.size() ||
        source.format() != QImage::Format_ARGB32_Premultiplied ||
        destination.format() != QImage::Format_ARGB32_Premultiplied ||
        mask.format() != QImage::Format_Alpha8) {
        return false;
    }
    const QRect pixels = destinationPixels.intersected(destination.rect());
    if (pixels.isEmpty() || spans.empty()) {
        return true;
    }
    if (!QRect(maskOriginPixels, mask.size()).contains(pixels)) {
        return false;
    }
    const bool colorEffectType = parameters.type == 2 || parameters.type == 3;
    const int strengthMix = colorEffectType ? normalizedStrengthMix(parameters.strength) : 255;
    if (colorEffectType && strengthMix == 0) {
        return true;
    }

    destination.detach();
    RenderWorkspace localWorkspace(0);
    RenderWorkspace& activeWorkspace = workspace != nullptr ? *workspace : localWorkspace;
    KernelDiagnostics& diagnostics = const_cast<KernelDiagnostics&>(activeWorkspace.diagnostics());
    try {
        if (parameters.type == 1) {
            struct Cluster {
                QRegion blocks;
                QRect expandedBounds;
            };
            const int support =
                makeGaussianBlurPlan(parameters).physicalSupportRadius;
            std::vector<Cluster> clusters;
            for (const QRect& block : occupiedBlocks) {
                Cluster next{
                    QRegion(block),
                    block.adjusted(-support, -support, support, support),
                };
                for (std::size_t index = 0; index < clusters.size();) {
                    if (!clusters[index].expandedBounds.intersects(next.expandedBounds)) {
                        ++index;
                        continue;
                    }
                    next.blocks += clusters[index].blocks;
                    next.expandedBounds =
                        next.expandedBounds.united(clusters[index].expandedBounds);
                    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(index));
                    index = 0;
                }
                clusters.push_back(std::move(next));
            }
            bool succeeded = true;
            for (const Cluster& cluster : clusters) {
                QRegion sparseRegion;
                for (const MaskSpan& span : spans) {
                    if (span.y < pixels.top() || span.y > pixels.bottom()) {
                        continue;
                    }
                    const int begin = qMax(span.beginX, pixels.left());
                    const int end = qMin(span.endX, pixels.right() + 1);
                    if (begin >= end) {
                        continue;
                    }
                    const QRegion row =
                        cluster.blocks.intersected(QRegion(QRect(begin, span.y, end - begin, 1)));
                    sparseRegion += row;
                }
                if (sparseRegion.isEmpty()) {
                    continue;
                }
                succeeded = blurMasked(source, destination, alphaView(mask), maskOriginPixels,
                                       sparseRegion, -1, parameters, activeWorkspace, options) &&
                            succeeded;
                if (!succeeded) {
                    break;
                }
            }
            if (workspace == nullptr) {
                localWorkspace.finishFrame(true);
            }
            return succeeded;
        }

        const ConstImageView sourceView = view(source);
        const ImageView destinationView = view(destination);
        const AlphaView maskView = alphaView(mask);
        if (parameters.type == 0) {
            const MosaicGrid grid = mosaicGrid(source.size(), parameters);
            const int firstColumn = (pixels.left() - grid.firstX) / grid.block;
            const int lastColumn = (pixels.right() - grid.firstX) / grid.block;
            const int firstRow = (pixels.top() - grid.firstY) / grid.block;
            const int lastRow = (pixels.bottom() - grid.firstY) / grid.block;
            const int sampleColumnCount = lastColumn - firstColumn + 1;
            const int sampleRowCount = lastRow - firstRow + 1;
            std::vector<QRgb>& samples = activeWorkspace.mosaicSampleScratch(
                static_cast<std::size_t>(sampleColumnCount) * sampleRowCount);
            collectMosaicSamples(sourceView, grid, firstColumn, sampleColumnCount, firstRow,
                                 sampleRowCount, samples);
            for (const MaskSpan& span : spans) {
                if (span.y < pixels.top() || span.y > pixels.bottom()) {
                    continue;
                }
                const int begin = qMax(span.beginX, pixels.left());
                const int end = qMin(span.endX, pixels.right() + 1);
                auto* destinationLine = reinterpret_cast<QRgb*>(
                    destinationView.data + static_cast<qsizetype>(span.y) * destinationView.stride);
                const auto* alphaLine =
                    maskView.data +
                    static_cast<qsizetype>(span.y - maskOriginPixels.y()) * maskView.stride;
                const int sampleRow = (span.y - grid.firstY) / grid.block - firstRow;
                const std::size_t sampleOffset =
                    static_cast<std::size_t>(sampleRow) * sampleColumnCount;
                for (int x = begin; x < end; ++x) {
                    const int sampleColumn = (x - grid.firstX) / grid.block - firstColumn;
                    const QRgb sample = samples[sampleOffset + sampleColumn];
                    const int mix = alphaLine[x - maskOriginPixels.x()];
                    destinationLine[x] =
                        mix == 255 ? sample : blendPremultiplied(destinationLine[x], sample, mix);
                }
            }
            if (workspace == nullptr) {
                localWorkspace.finishFrame(true);
            }
            return true;
        }

        bool simdExecuted = false;
        const bool useAvx2 =
            colorEffectType && !options.forceScalar && selectedSimdBackend() == SimdBackend::Avx2;
        for (const MaskSpan& span : spans) {
            if (span.y < pixels.top() || span.y > pixels.bottom()) {
                continue;
            }
            const int begin = qMax(span.beginX, pixels.left());
            const int end = qMin(span.endX, pixels.right() + 1);
            if (begin >= end) {
                continue;
            }
            if (useAvx2) {
                const bool executed =
                    parameters.type == 2
                        ? detail::grayscaleMaskedAvx2(sourceView, destinationView, maskView,
                                                      maskOriginPixels.x(), maskOriginPixels.y(),
                                                      begin, span.y, end, span.y + 1, strengthMix)
                        : detail::invertMaskedAvx2(sourceView, destinationView, maskView,
                                                   maskOriginPixels.x(), maskOriginPixels.y(),
                                                   begin, span.y, end, span.y + 1, strengthMix);
                if (executed) {
                    simdExecuted = true;
                    continue;
                }
            }
            const auto* sourceLine = reinterpret_cast<const QRgb*>(
                sourceView.data + static_cast<qsizetype>(span.y) * sourceView.stride);
            auto* destinationLine = reinterpret_cast<QRgb*>(
                destinationView.data + static_cast<qsizetype>(span.y) * destinationView.stride);
            const auto* alphaLine =
                maskView.data +
                static_cast<qsizetype>(span.y - maskOriginPixels.y()) * maskView.stride;
            for (int x = begin; x < end; ++x) {
                const int mix = combineCoverage(alphaLine[x - maskOriginPixels.x()], strengthMix);
                if (mix == 0) {
                    continue;
                }
                const QRgb to = transformedColor(sourceLine[x], parameters.type);
                destinationLine[x] =
                    mix == 255 ? to : blendPremultiplied(destinationLine[x], to, mix);
            }
        }
        if (simdExecuted) {
            diagnostics.backend = SimdBackend::Avx2;
        }
        if (workspace == nullptr) {
            localWorkspace.finishFrame(true);
        }
        return true;
    } catch (const std::bad_alloc&) {
        if (workspace == nullptr) {
            localWorkspace.finishFrame(true);
        }
        return false;
    }
}

bool applyRect(const QImage& source, QImage& destination, const QRect& destinationPixels,
               double opacity, const Parameters& parameters, RenderWorkspace* workspace,
               const ExecutionOptions& options) {
    const bool supported = parameters.type == 1 || parameters.type == 2 || parameters.type == 3;
    if (!supported || source.size() != destination.size() ||
        source.format() != QImage::Format_ARGB32_Premultiplied ||
        destination.format() != QImage::Format_ARGB32_Premultiplied) {
        return false;
    }
    const QRect pixels = destinationPixels.intersected(destination.rect());
    int mix = qBound(0, qRound(opacity * 255.0), 255);
    if (parameters.type == 2 || parameters.type == 3) {
        mix = combineCoverage(mix, normalizedStrengthMix(parameters.strength));
    }
    if (pixels.isEmpty() || mix == 0) {
        return true;
    }
    destination.detach();
    RenderWorkspace localWorkspace(0);
    RenderWorkspace& activeWorkspace = workspace != nullptr ? *workspace : localWorkspace;
    bool succeeded = true;
    if (parameters.type == 1) {
        succeeded = blurMasked(source, destination, {}, {}, QRegion(pixels), mix, parameters,
                               activeWorkspace, options);
    } else {
        bool usedSimd = false;
        KernelDiagnostics& diagnostics =
            const_cast<KernelDiagnostics&>(activeWorkspace.diagnostics());
        diagnostics.parallelJobs +=
            colorEffectRect(source, destination, pixels, parameters.type, mix,
                            options.singleThreaded, options.forceScalar, &usedSimd);
        if (usedSimd) {
            diagnostics.backend = SimdBackend::Avx2;
        }
    }
    if (workspace == nullptr) {
        localWorkspace.finishFrame(true);
    }
    return succeeded;
}

bool applyRegion(const QImage& source, QImage& destination, const QRegion& destinationPixels,
                 const Parameters& parameters, RenderWorkspace* workspace,
                 const ExecutionOptions& options) {
    const bool supported = parameters.type == 1 || parameters.type == 2 || parameters.type == 3;
    if (!supported || source.size() != destination.size() ||
        source.format() != QImage::Format_ARGB32_Premultiplied ||
        destination.format() != QImage::Format_ARGB32_Premultiplied) {
        return false;
    }
    const QRegion pixels = destinationPixels.intersected(destination.rect());
    if (pixels.isEmpty()) {
        return true;
    }
    const int colorMix = parameters.type == 2 || parameters.type == 3
                             ? normalizedStrengthMix(parameters.strength)
                             : 255;
    if (colorMix == 0) {
        return true;
    }
    destination.detach();
    RenderWorkspace localWorkspace(0);
    RenderWorkspace& activeWorkspace = workspace != nullptr ? *workspace : localWorkspace;
    bool succeeded = true;
    if (parameters.type == 1) {
        succeeded = blurMasked(source, destination, {}, {}, pixels, 255, parameters,
                               activeWorkspace, options);
    } else {
        bool usedSimd = false;
        KernelDiagnostics& diagnostics =
            const_cast<KernelDiagnostics&>(activeWorkspace.diagnostics());
        for (const QRect& rect : pixels) {
            diagnostics.parallelJobs +=
                colorEffectRect(source, destination, rect, parameters.type, colorMix,
                                options.singleThreaded, options.forceScalar, &usedSimd);
        }
        if (usedSimd) {
            diagnostics.backend = SimdBackend::Avx2;
        }
    }
    if (workspace == nullptr) {
        localWorkspace.finishFrame(true);
    }
    return succeeded;
}

void blendOverSource(QImage& filtered, const QImage& source, double opacity,
                     const ExecutionOptions& options, KernelDiagnostics* diagnostics) {
    if (filtered.size() != source.size() ||
        filtered.format() != QImage::Format_ARGB32_Premultiplied ||
        source.format() != QImage::Format_ARGB32_Premultiplied) {
        return;
    }
    const int mix = qBound(0, qRound(opacity * 256.0), 256);
    const std::size_t jobs = parallelRows(
        filtered.height(), filtered.width(), options.singleThreaded, [&](int begin, int end) {
            for (int y = begin; y < end; ++y) {
                auto* destination = reinterpret_cast<QRgb*>(filtered.scanLine(y));
                const auto* base = reinterpret_cast<const QRgb*>(source.constScanLine(y));
                for (int x = 0; x < filtered.width(); ++x) {
                    const QRgb from = base[x];
                    const QRgb to = destination[x];
                    const auto interpolate = [mix](int first, int second) {
                        const int scaled = (second - first) * mix;
                        return first + (scaled + (scaled >= 0 ? 128 : -128)) / 256;
                    };
                    const int alpha = interpolate(qAlpha(from), qAlpha(to));
                    destination[x] = qRgba(qMin(alpha, interpolate(qRed(from), qRed(to))),
                                           qMin(alpha, interpolate(qGreen(from), qGreen(to))),
                                           qMin(alpha, interpolate(qBlue(from), qBlue(to))), alpha);
                }
            }
        });
    if (diagnostics != nullptr) {
        diagnostics->parallelJobs += jobs;
        diagnostics->copiedBytes += filtered.sizeInBytes() + source.sizeInBytes();
    }
}

} // namespace snow_canvas_filter_render
