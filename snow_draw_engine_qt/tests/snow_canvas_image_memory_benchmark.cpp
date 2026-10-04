// SPDX-License-Identifier: Apache-2.0
// Compile this same source against the pre-optimization Qt code with
// SNOW_MEMORY_BENCHMARK_BASELINE to compare identical scenarios and fixtures.
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
#include "snow_draw_engine_qt/snow_canvas_image.h"
#endif
#include "../../test-support/memorysnapshot.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QTransform>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using snow::test_support::memorySnapshot;
std::uint64_t sink = 0;

struct Options {
    bool managed = true;
    bool verifyOnly = false;
    std::string scenario = "mixed-allocate";
    int width = 3840;
    int height = 2160;
    int warmup = 3;
    int count = 60;
};

int positiveArgument(std::string_view argument, bool zeroAllowed = false) {
    int value = 0;
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
        value < (zeroAllowed ? 0 : 1))
        throw std::runtime_error("invalid numeric argument");
    return value;
}

Options parseOptions(int argc, char** argv) {
    Options options;
#if defined(SNOW_MEMORY_BENCHMARK_BASELINE)
    options.managed = false;
#endif
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--qt")
            options.managed = false;
        else if (argument == "--pages")
            options.managed = true;
        else if (argument == "--verify-only")
            options.verifyOnly = true;
        else {
            if (index + 1 == argc)
                throw std::runtime_error("missing argument value");
            const std::string_view value(argv[++index]);
            if (argument == "--scenario")
                options.scenario = value;
            else if (argument == "--width")
                options.width = positiveArgument(value);
            else if (argument == "--height")
                options.height = positiveArgument(value);
            else if (argument == "--warmup")
                options.warmup = positiveArgument(value, true);
            else if (argument == "--count" || argument == "--repeat")
                options.count = positiveArgument(value);
            else
                throw std::runtime_error("unknown argument");
        }
    }
#if defined(SNOW_MEMORY_BENCHMARK_BASELINE)
    if (options.managed)
        throw std::runtime_error("--pages requires the optimized image helpers");
#endif
    if ((options.scenario == "crop" || options.scenario == "scale_half_smooth") &&
        (options.width < 2 || options.height < 2))
        throw std::runtime_error("crop and half-scale require dimensions of at least 2");
    return options;
}

QImage allocate(const QSize& size, QImage::Format format, bool managed) {
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
    if (managed)
        return snowCanvasAllocateImage(size, format);
#else
    static_cast<void>(managed);
#endif
    return QImage(size, format);
}

QImage seedImage(const QSize& size, bool managed) {
    QImage image = allocate(size, QImage::Format_RGBA8888, managed);
    if (image.isNull())
        throw std::runtime_error("fixture allocation failed");
    for (int y = 0; y < size.height(); ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const auto offset = static_cast<qsizetype>(x) * 4;
            const auto value =
                static_cast<unsigned>(x) * 1664525U + static_cast<unsigned>(y) * 1013904223U;
            row[offset] = static_cast<uchar>((value >> 3) ^ static_cast<unsigned>(x / 19));
            row[offset + 1] = static_cast<uchar>((value >> 11) ^ static_cast<unsigned>(y / 13));
            row[offset + 2] = static_cast<uchar>(
                (value >> 19) ^ ((static_cast<unsigned>(x) + static_cast<unsigned>(y)) / 29));
            row[offset + 3] = static_cast<uchar>(32 + value % 224);
        }
    }
    image.setColorSpace(QColorSpace::DisplayP3);
    return image;
}

std::uint64_t checksum(const QImage& image) {
    if (image.isNull())
        throw std::runtime_error("operation returned a null image");
    std::uint64_t hash = 1469598103934665603ULL;
    const auto rowBytes = (static_cast<qsizetype>(image.width()) * image.depth() + 7) / 8;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (qsizetype x = 0; x < rowBytes; ++x) {
            hash ^= row[x];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void touch(const QImage& image) {
    if (image.isNull())
        throw std::runtime_error("operation returned a null image");
    sink += static_cast<std::uint64_t>(image.sizeInBytes()) + image.constBits()[0] +
            image.constScanLine(image.height() / 2)[image.bytesPerLine() / 2];
}

QImage apply(const Options& options, const QImage& source) {
    const bool managed = options.managed;
    const auto& operation = options.scenario;
    if (operation == "fresh_allocate_zero_release") {
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasAllocateZeroedImage(QSize(options.width, options.height),
                                                 QImage::Format_RGBA8888);
#endif
        QImage output(QSize(options.width, options.height), QImage::Format_RGBA8888);
        if (!output.isNull())
            std::memset(output.bits(), 0, static_cast<std::size_t>(output.sizeInBytes()));
        return output;
    }
    if (operation == "fresh_allocate_fill_release" || operation == "mixed-allocate") {
        QImage output =
            allocate(QSize(options.width, options.height), QImage::Format_RGBA8888, managed);
        if (!output.isNull())
            output.fill(qRgba(11, 22, 33, 255));
        return output;
    }
    if (operation == "copy" || operation == "crop") {
        const QRect crop = operation == "crop" ? QRect(options.width / 4, options.height / 4,
                                                       options.width / 2, options.height / 2)
                                               : QRect{};
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasCopyImage(source, crop);
#endif
        return source.copy(crop);
    }
    if (operation == "cow") {
        QImage output = source;
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed) {
            if (!snowCanvasDetachImage(output))
                throw std::runtime_error("detach failed");
        } else
#endif
            output.detach();
        output.scanLine(0)[0] ^= 0xff;
        return output;
    }
    if (operation == "convert_rgba_premul") {
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasConvertImage(source, QImage::Format_ARGB32_Premultiplied);
#endif
        return source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    if (operation == "convert_rgba64") {
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasConvertImage(source, QImage::Format_RGBA64);
#endif
        return source.convertToFormat(QImage::Format_RGBA64);
    }
    if (operation == "color_p3_srgb") {
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasColorConvertedImage(source, QColorSpace::SRgb,
                                                 QImage::Format_RGBA8888);
#endif
        return source.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888);
    }
    if (operation == "rotate90" || operation == "rotate17_smooth" ||
        operation == "flip_horizontal" || operation == "flip_horizontal_smooth") {
        QTransform transform;
        if (operation == "rotate90")
            transform.rotate(90);
        else if (operation == "rotate17_smooth")
            transform.rotate(17);
        else
            transform.scale(-1, 1);
        const auto mode = operation == "flip_horizontal_smooth" || operation == "rotate17_smooth"
                              ? Qt::SmoothTransformation
                              : Qt::FastTransformation;
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasTransformImage(source, transform, mode);
#endif
        return source.transformed(transform, mode);
    }
    if (operation == "scale_half_smooth") {
        const QSize target(options.width / 2, options.height / 2);
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
        if (managed)
            return snowCanvasScaleImage(source, target, Qt::IgnoreAspectRatio,
                                        Qt::SmoothTransformation);
#endif
        return source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    throw std::runtime_error("unknown scenario: " + operation);
}

bool borrowedRowsScenario(const Options& options) {
    return options.scenario == "copy_rgba_rows_reuse" ||
           options.scenario == "convert_premul_rgba_rows_reuse" ||
           options.scenario == "color_p3_srgb_rows_reuse";
}

void copyBorrowedRows(const Options& options, const QImage& source, QImage& destination) {
    const QColorSpace target = options.scenario == "color_p3_srgb_rows_reuse"
                                   ? QColorSpace(QColorSpace::SRgb)
                                   : QColorSpace{};
#if !defined(SNOW_MEMORY_BENCHMARK_BASELINE)
    if (options.managed) {
        if (!snowCanvasCopyRgba8888Rows(source, 0, source.height(), destination.bits(),
                                        destination.sizeInBytes(), destination.bytesPerLine(),
                                        target))
            throw std::runtime_error("borrowed row conversion failed");
        return;
    }
#endif
    const QImage converted = target.isValid()
                                 ? source.convertedToColorSpace(target, QImage::Format_RGBA8888)
                                 : source.convertToFormat(QImage::Format_RGBA8888);
    if (converted.isNull())
        throw std::runtime_error("native borrowed row conversion failed");
    for (int row = 0; row < source.height(); ++row)
        std::memcpy(destination.scanLine(row), converted.constScanLine(row),
                    static_cast<std::size_t>(source.width()) * 4);
}

void report(const char* record, const Options& options, int iteration, std::int64_t elapsed = 0,
            std::uint64_t hash = 0, std::uint64_t logicalBytes = 0) {
    const auto memory = memorySnapshot();
    std::cout << record << ',' << (options.managed ? "pages" : "qt") << ',' << options.scenario
              << ',' << options.width << ',' << options.height << ',' << iteration << ',' << elapsed
              << ',' << hash << ',' << logicalBytes << ',' << memory.residentBytes << ','
              << memory.footprintBytes << ',' << memory.peakResidentBytes << '\n';
}
} // namespace

// Run one scenario/size/mode per fresh process. Native reference verification is
// a separate invocation so its heap allocations cannot contaminate memory data.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto options = parseOptions(argc, argv);
        std::cout
            << "record,mode,scenario,width,height,iteration,elapsed_ns,checksum,logical_bytes,"
               "rss_bytes,footprint_bytes,peak_rss_bytes\n";
        report("process_baseline", options, -1);
        QImage source;
        QImage reused;
        if (options.scenario == "reuse_fill") {
            reused = allocate(QSize(options.width, options.height), QImage::Format_RGBA8888,
                              options.managed);
            reused.fill(qRgba(11, 22, 33, 255));
            touch(reused);
        } else if (options.scenario != "fresh_allocate_fill_release" &&
                   options.scenario != "fresh_allocate_zero_release" &&
                   options.scenario != "mixed-allocate") {
            source = seedImage(QSize(options.width, options.height), options.managed);
        }
        if (borrowedRowsScenario(options)) {
            if (options.scenario == "convert_premul_rgba_rows_reuse")
                source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            reused = allocate(source.size(), QImage::Format_RGBA8888, options.managed);
            if (reused.isNull())
                throw std::runtime_error("borrowed row destination allocation failed");
            reused.fill(0);
            reused.setColorSpace(options.scenario == "color_p3_srgb_rows_reuse"
                                     ? QColorSpace(QColorSpace::SRgb)
                                     : source.colorSpace());
        }
        report("fixture_baseline", options, -1);
        if (options.verifyOnly) {
            if (options.scenario == "reuse_fill") {
                report("verified", options, -1, 0, checksum(reused));
                return 0;
            }
            if (borrowedRowsScenario(options)) {
                const QImage reference =
                    options.scenario == "color_p3_srgb_rows_reuse"
                        ? source.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888)
                        : source.convertToFormat(QImage::Format_RGBA8888);
                copyBorrowedRows(options, source, reused);
                const auto hash = checksum(reused);
                if (reference.isNull() || hash != checksum(reference) ||
                    reused.colorSpace() != reference.colorSpace())
                    throw std::runtime_error("borrowed row Qt equivalence failed");
                report("verified", options, -1, 0, hash);
                return 0;
            }
            Options native = options;
            native.managed = false;
            const QImage reference = apply(native, source);
            const QImage actual = apply(options, source);
            const auto hash = checksum(actual);
            if (actual.size() != reference.size() || actual.format() != reference.format() ||
                actual.colorSpace() != reference.colorSpace() || hash != checksum(reference))
                throw std::runtime_error("native Qt equivalence failed");
            report("verified", options, -1, 0, hash);
            return 0;
        }
        std::vector<std::int64_t> samples;
        samples.reserve(static_cast<std::size_t>(options.count));
        for (int iteration = -options.warmup; iteration < options.count; ++iteration) {
            const auto start = Clock::now();
            if (options.scenario == "reuse_fill") {
                reused.fill(qRgba(11, 22, 33, 255));
                touch(reused);
            } else if (borrowedRowsScenario(options)) {
                copyBorrowedRows(options, source, reused);
                touch(reused);
            } else if (options.scenario == "mixed-allocate") {
                std::array<QImage, 3> images;
                const std::array<QSize, 3> sizes{QSize(options.width, options.height),
                                                 QSize(1600, 900), QSize(1664, 964)};
                for (std::size_t index = 0; index < images.size(); ++index) {
                    images[index] =
                        allocate(sizes[index], QImage::Format_RGBA8888, options.managed);
                    images[index].fill(qRgba(11, 22, 33, 255));
                    touch(images[index]);
                }
            } else {
                const QImage output = apply(options, source);
                touch(output);
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            if (iteration >= 0) {
                samples.push_back(elapsed);
                report("sample", options, iteration, elapsed);
            }
        }
        report("after_cycles_released", options, -1);
        // One separate space pass observes fully touched live output and its
        // release. This avoids process-memory queries inside timed operations.
        if (options.scenario == "mixed-allocate") {
            std::array<QImage, 3> images;
            const std::array<QSize, 3> sizes{QSize(options.width, options.height), QSize(1600, 900),
                                             QSize(1664, 964)};
            std::uint64_t logicalBytes = 0;
            std::uint64_t hash = 0;
            for (std::size_t index = 0; index < images.size(); ++index) {
                images[index] = allocate(sizes[index], QImage::Format_RGBA8888, options.managed);
                images[index].fill(qRgba(11, 22, 33, 255));
                hash ^= checksum(images[index]);
                logicalBytes += static_cast<std::uint64_t>(images[index].sizeInBytes());
            }
            report("output_live", options, -1, 0, hash, logicalBytes);
        } else {
            if (borrowedRowsScenario(options))
                copyBorrowedRows(options, source, reused);
            const QImage output = options.scenario == "reuse_fill" || borrowedRowsScenario(options)
                                      ? reused
                                      : apply(options, source);
            report("output_live", options, -1, 0, checksum(output),
                   static_cast<std::uint64_t>(output.sizeInBytes()));
        }
        report("output_released", options, -1);
        source = {};
        reused = {};
        report("all_released", options, -1);
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&samples](std::size_t percent) {
            return samples[(samples.size() * percent + 99) / 100 - 1];
        };
        std::cerr << (options.managed ? "pages" : "qt") << ',' << options.scenario
                  << ",p50_ns=" << percentile(50) << ",p95_ns=" << percentile(95)
                  << ",sink=" << sink << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
