// SPDX-License-Identifier: GPL-3.0-only

#include "snowimageqtcodec.h"
#include "snowimageqtsrgbrowreader.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/memorysnapshot.h"
#include "../../test-support/virtualmemory.h"

#include <QBuffer>
#include <QColorSpace>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

using snow_shot::image_codec::detail::BoundedSrgbRowReader;

// Keep the rejected direct-per-request strategy explicit for reproducible
// comparisons after the accepted reader becomes the production factory.
ScreenshotImageRowSource lazyRows(const QImage& image) {
    if (image.isNull())
        return {};
    const QColorSpace srgb(QColorSpace::SRgb);
    const bool convertColor = image.colorSpace().isValid() && image.colorSpace() != srgb;
    ScreenshotImageRowSource source;
    source.size = image.size();
    if (!convertColor && image.format() == QImage::Format_RGBA8888)
        source.backingImage = image;
    source.readRows = [image, target = convertColor ? srgb : QColorSpace{}](
                          int first, int count, qsizetype stride, uchar* destination,
                          qsizetype capacity) {
        return snowCanvasCopyRgba8888Rows(image, first, count, destination, capacity, stride,
                                          target);
    };
    return source;
}

ScreenshotImageRowSource eagerRows(const QImage& image) {
    const QColorSpace srgb(QColorSpace::SRgb);
    QImage rgba = image.colorSpace().isValid() && image.colorSpace() != srgb
                      ? snowCanvasColorConvertedImage(image, srgb, QImage::Format_RGBA8888)
                      : snowCanvasConvertImage(image, QImage::Format_RGBA8888);
    if (rgba.isNull() || (rgba.colorSpace() != srgb && !snowCanvasDetachImage(rgba)))
        return {};
    rgba.setColorSpace(srgb);
    ScreenshotImageRowSource rows;
    rows.size = rgba.size();
    rows.backingImage = rgba;
    rows.readRows = [rgba](int first, int count, qsizetype stride, uchar* destination,
                           qsizetype capacity) {
        // Reproduce the previous eager callback, including its simple row copy,
        // so the gate measures the complete old export path.
        const qsizetype rowBytes = static_cast<qsizetype>(rgba.width()) * 4;
        if (first < 0 || count <= 0 || first > rgba.height() || count > rgba.height() - first ||
            destination == nullptr || stride < rowBytes || capacity < rowBytes ||
            count - 1 > (capacity - rowBytes) / stride)
            return false;
        for (int row = 0; row < count; ++row)
            std::memcpy(destination + row * stride, rgba.constScanLine(first + row),
                        static_cast<std::size_t>(rowBytes));
        return true;
    };
    return rows;
}

QImage fixtureImage(const QSize& size, const QString& kind) {
    QImage image = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
    require(!image.isNull(), "export row benchmark fixture must allocate");
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const auto column = static_cast<unsigned>(x);
            const auto line = static_cast<unsigned>(y);
            row[x * 4] = static_cast<uchar>((column * 29 + line * 37) % 256);
            row[x * 4 + 1] = static_cast<uchar>((column * 47 + line * 19) % 256);
            row[x * 4 + 2] = static_cast<uchar>((column * 13 + line * 53) % 256);
            row[x * 4 + 3] = static_cast<uchar>(32 + (column + line * 3) % 224);
        }
    }
    image.setColorSpace(kind == QStringLiteral("p3") ? QColorSpace(QColorSpace::DisplayP3)
                                                     : QColorSpace(QColorSpace::SRgb));
    if (kind == QStringLiteral("premul"))
        image = snowCanvasConvertImage(image, QImage::Format_ARGB32_Premultiplied);
    return image;
}

std::uint64_t checksum(const QByteArray& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char byte : bytes) {
        hash ^= static_cast<uchar>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

ScreenshotImageRowSource rowsForMode(const QImage& image, const QString& mode) {
    if (mode == QStringLiteral("eager"))
        return eagerRows(image);
    if (mode == QStringLiteral("lazy"))
        return lazyRows(image);
    if (mode == QStringLiteral("cached"))
        return snow_shot::image_codec::srgbRowSource(image);
    throw std::runtime_error("invalid export row benchmark mode");
}

QByteArray encodeRows(const ScreenshotImageRowSource& rows, const QString& format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    snow::image::EncodeOptions options;
    options.compression_level = 0;
    options.quality = 85;
    require(buffer.open(QIODevice::WriteOnly), "export benchmark output must open");
    require(snow_shot::image_codec::encodeToDevice(rows, &buffer,
                                                   format == QStringLiteral("png")
                                                       ? snow::image::Format::png
                                                       : snow::image::Format::jpeg,
                                                   options),
            "export row benchmark encoding must succeed");
    return bytes;
}
} // namespace

void boundedExportRowsTests() {
    for (const QString& kind : {QStringLiteral("premul"), QStringLiteral("p3")}) {
        QImage image = fixtureImage(QSize(1031, 517), kind);
        const QImage reference = image.copy();
        const QImage expected =
            reference.colorSpace() == QColorSpace(QColorSpace::SRgb)
                ? reference.convertToFormat(QImage::Format_RGBA8888)
                : reference.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888);
        const uchar* original = image.constBits();
        const uchar* middle = original + image.sizeInBytes() / 2;
        auto rows = snow_shot::image_codec::srgbRowSource(image);
        const qsizetype stride = qsizetype(image.width()) * 4 + 17;
        constexpr uchar sentinel = 0xad;
        std::vector<uchar> output(static_cast<std::size_t>(stride * 41), sentinel);
        for (const int first : {0, 516, 1, 64, 7, 509}) {
            const int count = std::min(41, image.height() - first);
            require(rows.readRows(first, count, stride, output.data(),
                                  static_cast<qsizetype>(output.size())),
                    "cached rows must support one-row and out-of-order reads");
            for (int y = 0; y < count; ++y) {
                const uchar* pixels = output.data() + qsizetype(y) * stride;
                require(std::memcmp(pixels, expected.constScanLine(first + y),
                                    static_cast<std::size_t>(image.width()) * 4) == 0,
                        "cached conversion must match native Qt bytes");
                require(std::all_of(pixels + image.width() * 4, pixels + stride,
                                    [](uchar value) { return value == sentinel; }),
                        "cached rows must leave destination padding unchanged");
            }
        }
        const auto* reader = rows.readRows.target<BoundedSrgbRowReader>();
        require(reader != nullptr &&
                    reader->cachedBytes() <= BoundedSrgbRowReader::maximumCacheBytes,
                "each converted reader cache must remain within its byte budget");
        const qsizetype cachedBeforeFullRead = reader->cachedBytes();
        std::vector<uchar> full(static_cast<std::size_t>(expected.sizeInBytes()));
        require(rows.readRows(0, image.height(), expected.bytesPerLine(), full.data(),
                              static_cast<qsizetype>(full.size())) &&
                    std::memcmp(full.data(), expected.constBits(), full.size()) == 0 &&
                    reader->cachedBytes() == cachedBeforeFullRead,
                "large row requests must retain direct bounded conversion and exact native bytes");
        auto copy = rows;
        const auto* copiedReader = copy.readRows.target<BoundedSrgbRowReader>();
        require(copiedReader != nullptr && copiedReader->cachedBytes() == 0,
                "reader copies must start with independent empty caches");
        require(copy.readRows(0, 1, stride, output.data(), static_cast<qsizetype>(output.size())) &&
                    copiedReader->cachedPixels() != reader->cachedPixels(),
                "reader copies must own independent converted cache pixels");

        std::atomic<bool> correct{true};
        const auto concurrent = [&](int first) {
            std::vector<uchar> pixels(static_cast<std::size_t>(image.width()) * 4);
            for (int step = 0; step < 15; ++step) {
                const int row = (first + step * 29) % image.height();
                if (!rows.readRows(row, 1, image.width() * 4, pixels.data(),
                                   static_cast<qsizetype>(pixels.size())) ||
                    std::memcmp(pixels.data(), expected.constScanLine(row), pixels.size()) != 0)
                    correct.store(false);
            }
        };
        std::thread first(concurrent, 0);
        std::thread second(concurrent, 257);
        first.join();
        second.join();
        require(correct.load(), "concurrent calls to one cached reader must preserve exact bytes");
        const uchar* cache = reader->cachedPixels();
        const qsizetype cacheBytes = reader->cachedBytes();
        const std::vector<uchar> cacheBefore(cache, cache + cacheBytes);
        require(!rows.readRows(0, 1, stride, const_cast<uchar*>(cache), cacheBytes) &&
                    !rows.readRows(image.height() - 1, 1, stride, const_cast<uchar*>(cache),
                                   cacheBytes) &&
                    reader->cachedPixels() == cache &&
                    std::memcmp(cache, cacheBefore.data(), cacheBefore.size()) == 0,
                "cache aliases must be rejected before either refill or destination writes");
        const auto unchanged = output;
        require(
            !rows.readRows(-1, 1, stride, output.data(), static_cast<qsizetype>(output.size())) &&
                !rows.readRows(0, 1, stride, output.data(), image.width() * 4 - 1) &&
                !rows.readRows(0, 2, std::numeric_limits<qsizetype>::max(), output.data(),
                               static_cast<qsizetype>(output.size())) &&
                !rows.readRows(0, 1, stride, const_cast<uchar*>(original), image.sizeInBytes()) &&
                output == unchanged && image == reference && image.constBits() == original,
            "cached readers must reject invalid/alias destinations before writing");
        const QByteArray eagerPng = encodeRows(eagerRows(image), QStringLiteral("png"));
        require(encodeRows(rows, QStringLiteral("png")) == eagerPng &&
                    encodeRows(rows, QStringLiteral("jpeg")) ==
                        encodeRows(eagerRows(image), QStringLiteral("jpeg")),
                "actual cached PNG/JPEG output must exactly match eager conversion");
        image.fill(Qt::transparent);
        require(image.constBits() != original &&
                    rows.readRows(516, 1, stride, output.data(),
                                  static_cast<qsizetype>(output.size())) &&
                    std::memcmp(output.data(), expected.constScanLine(516),
                                static_cast<std::size_t>(image.width()) * 4) == 0,
                "caller image mutation must leave captured source and cached rows unchanged");
        image = {};
        require(snow::test_support::virtualMemoryMapped(middle),
                "cached callbacks must retain their source after caller release");
        require(copy.readRows(0, 1, stride, output.data(), static_cast<qsizetype>(output.size())),
                "copied cached readers must remain usable after caller release");
        rows = {};
        copy = {};
        require(!snow::test_support::virtualMemoryMapped(middle),
                "last cached reader release must return original managed pages");
    }
    const int wide = static_cast<int>(BoundedSrgbRowReader::maximumCacheBytes / 4) + 1;
    const QImage image = fixtureImage(QSize(wide, 1), QStringLiteral("premul"));
    const QImage expected = image.copy().convertToFormat(QImage::Format_RGBA8888);
    const auto rows = snow_shot::image_codec::srgbRowSource(image);
    const auto* reader = rows.readRows.target<BoundedSrgbRowReader>();
    const qsizetype stride = qsizetype(wide) * 4 + 13;
    std::vector<uchar> pixels(static_cast<std::size_t>(stride), 0xad);
    require(reader != nullptr && rows.readRows(0, 1, stride, pixels.data(), stride) &&
                std::memcmp(pixels.data(), expected.constBits(),
                            static_cast<std::size_t>(wide) * 4) == 0 &&
                std::all_of(pixels.data() + qsizetype(wide) * 4, pixels.data() + stride,
                            [](uchar value) { return value == 0xad; }) &&
                reader->cachedBytes() == 0,
            "rows wider than the cache budget must use exact bounded conversion without a cache");
}

int runExportRowBenchmark(const QStringList& arguments) {
    try {
#ifndef NDEBUG
        throw std::runtime_error("export row benchmark requires Release");
#endif
        const auto value = [&arguments](const QString& name, const QString& fallback) {
            const int index = static_cast<int>(arguments.indexOf(name));
            return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : fallback;
        };
        const QString mode = value(QStringLiteral("--mode"), QStringLiteral("lazy"));
        const QString kind = value(QStringLiteral("--source"), QStringLiteral("premul"));
        const QString format = value(QStringLiteral("--format"), QStringLiteral("png"));
        const int width = value(QStringLiteral("--width"), QStringLiteral("3840")).toInt();
        const int height = value(QStringLiteral("--height"), QStringLiteral("2160")).toInt();
        const int count = value(QStringLiteral("--samples"), QStringLiteral("7")).toInt();
        require(width > 0 && width <= std::numeric_limits<int>::max() / 4 && height > 0 &&
                    count > 0 && count <= 31,
                "export row benchmark dimensions/sample count are invalid");
        require(kind == QStringLiteral("premul") || kind == QStringLiteral("p3") ||
                    kind == QStringLiteral("rgba"),
                "export row benchmark source is invalid");
        require(format == QStringLiteral("png") || format == QStringLiteral("jpeg"),
                "export row benchmark format is invalid");
        const QString scenario = QStringLiteral("export-row-%1-%2").arg(kind, format);
        const auto report = [&](const char* record, int iteration, std::int64_t elapsed = 0,
                                std::uint64_t hash = 0, std::uint64_t logical = 0) {
            const auto memory = snow::test_support::memorySnapshot();
            std::cout << record << ',' << mode.toStdString() << ',' << scenario.toStdString() << ','
                      << width << ',' << height << ',' << iteration << ',' << elapsed << ',' << hash
                      << ',' << logical << ',' << memory.residentBytes << ','
                      << memory.footprintBytes << ',' << memory.peakResidentBytes << '\n';
        };
        std::cout
            << "record,mode,scenario,width,height,iteration,elapsed_ns,checksum,logical_bytes,"
               "rss_bytes,footprint_bytes,peak_rss_bytes\n";
        report("process_baseline", -1);
        QImage image = fixtureImage(QSize(width, height), kind);
        report("fixture_baseline", -1);
        if (arguments.contains(QStringLiteral("--verify-only"))) {
            const QByteArray expected = encodeRows(eagerRows(image), format);
            const QByteArray actual = encodeRows(rowsForMode(image, mode), format);
            require(actual == expected, "export row mode must produce identical encoded bytes");
            report("verified", -1, 0, checksum(actual), static_cast<std::uint64_t>(actual.size()));
            return 0;
        }
        std::uint64_t sink = 0;
        for (int iteration = -3; iteration < count; ++iteration) {
            const auto start = std::chrono::steady_clock::now();
            {
                const auto rows = rowsForMode(image, mode);
                const QByteArray bytes = encodeRows(rows, format);
                require(!bytes.isEmpty(), "encoded export output must not be empty");
                sink += static_cast<std::uint64_t>(bytes.size()) + static_cast<uchar>(bytes[0]);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (iteration >= 0)
                report("sample", iteration, elapsed);
        }
        report("after_cycles_released", -1);
        {
            const auto rows = rowsForMode(image, mode);
            const QByteArray bytes = encodeRows(rows, format);
            report("output_live", -1, 0, checksum(bytes), static_cast<std::uint64_t>(bytes.size()));
        }
        report("output_released", -1);
        image = {};
        report("all_released", -1);
        std::cerr << "sink=" << sink << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
