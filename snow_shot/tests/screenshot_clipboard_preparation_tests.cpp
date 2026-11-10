#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snowimageqtcodec.h"
#include "../src/presentation/services/screenshotclipboarddibwriter.h"

#include <QCoreApplication>
#include <QColorSpace>
#include <QImage>
#include <QDir>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>

struct ScreenshotClipboardPayloadTestAccess {
    static QByteArray fileBytes(const ScreenshotClipboardPayload& payload) {
        const auto handle = static_cast<HGLOBAL>(payload.m_fileHandle);
        const auto* memory = static_cast<const char*>(GlobalLock(handle));
        const QByteArray bytes = memory ? QByteArray(memory, GlobalSize(handle)) : QByteArray{};
        if (memory)
            GlobalUnlock(handle);
        return bytes;
    }
    static bool independentFiles(const ScreenshotClipboardPayload& first,
                                 const ScreenshotClipboardPayload& second) {
        return first.m_fileHandle != second.m_fileHandle;
    }
};
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void bitmapPixelsAndBounds() {
    using snow_shot::presentation::clipboard_detail::writeBgr;
    // Exhaust every channel/alpha combination, including vector lanes and rounding boundaries.
    std::vector<std::uint8_t> rgba(256 * 256 * 4);
    std::vector<std::uint8_t> bgr(256 * 256 * 3);
    for (int alpha = 0; alpha < 256; ++alpha)
        for (int value = 0; value < 256; ++value) {
            const auto offset = size_t(alpha * 256 + value) * 4;
            rgba[offset] = static_cast<std::uint8_t>(value);
            rgba[offset + 1] = static_cast<std::uint8_t>(255 - value);
            rgba[offset + 2] = static_cast<std::uint8_t>(value ^ 0x55);
            rgba[offset + 3] = static_cast<std::uint8_t>(alpha);
        }
    writeBgr(rgba.data(), bgr.data(), 256 * 256, false);
    for (int pixel = 0; pixel < 256 * 256; ++pixel)
        for (int channel = 0; channel < 3; ++channel) {
            const unsigned alpha = rgba[size_t(pixel) * 4 + 3];
            const unsigned color = rgba[size_t(pixel) * 4 + size_t(2 - channel)];
            require(bgr[size_t(pixel) * 3 + size_t(channel)] ==
                        (color * alpha + 255U * (255U - alpha) + 127U) / 255U,
                    "DIB colors must exactly flatten straight alpha onto white");
        }
    for (bool opaque : {false, true})
        for (int width : {0, 1, 3, 4, 5, 15, 16, 17, 31, 32, 33})
            for (int alignment = 0; alignment < 16; ++alignment) {
                std::array<std::uint8_t, 192> source{};
                std::array<std::uint8_t, 160> output;
                output.fill(0xa5);
                for (int x = 0; x < width; ++x) {
                    auto* pixel = source.data() + alignment + x * 4;
                    pixel[0] = static_cast<std::uint8_t>(x * 7);
                    pixel[1] = static_cast<std::uint8_t>(x * 11);
                    pixel[2] = static_cast<std::uint8_t>(x * 3);
                    pixel[3] = opaque ? 255 : static_cast<std::uint8_t>(x * 13);
                }
                writeBgr(source.data() + alignment, output.data() + alignment, width, opaque);
                for (size_t i = 0; i < output.size(); ++i) {
                    if (i < size_t(alignment) || i >= size_t(alignment + width * 3)) {
                        require(output[i] == 0xa5, "DIB stores must not overwrite adjacent bytes");
                        continue;
                    }
                    const size_t local = i - size_t(alignment);
                    const auto* pixel = source.data() + alignment + local / 3 * 4;
                    const unsigned alpha = pixel[3];
                    require(output[i] ==
                                (pixel[2 - local % 3] * alpha + 255U * (255U - alpha) + 127U) /
                                    255U,
                            "unaligned SIMD and tail pixels must retain exact colors");
                }
            }
#ifdef Q_OS_WIN
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = size_t(info.dwPageSize);
    struct Guard final {
        void* allocation;
        ~Guard() {
            if (allocation)
                VirtualFree(allocation, 0, MEM_RELEASE);
        }
    };
    Guard input{VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
    Guard output{VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
    require(input.allocation && output.allocation, "guarded pixel allocations");
    DWORD oldProtection = 0;
    require(VirtualProtect(static_cast<uchar*>(input.allocation) + page, page, PAGE_NOACCESS,
                           &oldProtection) &&
                VirtualProtect(static_cast<uchar*>(output.allocation) + page, page, PAGE_NOACCESS,
                               &oldProtection),
            "protect bytes immediately following SIMD spans");
    for (int width = 1; width <= 33; ++width) {
        auto* source = static_cast<uchar*>(input.allocation) + page - size_t(width) * 4;
        auto* destination = static_cast<uchar*>(output.allocation) + page - size_t(width) * 3;
        std::memset(source, 255, size_t(width) * 4);
        writeBgr(source, destination, width, true);
        writeBgr(source, destination, width, false);
    }
#endif
}

void opacitySurvivesSnapshotsAndEncoding() {
    for (const auto format : {QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied})
        for (int width : {1, 3, 4, 7, 16, 17, 33})
            for (bool opaque : {true, false}) {
                QImage original(width, 3, QImage::Format_ARGB32);
                original.fill(QColor(23, 117, 209, 255));
                if (!opaque)
                    original.setPixelColor(width - 1, 2, QColor(97, 171, 53, 127));
                original = original.convertToFormat(format);
                original.setColorSpace(QColorSpace::SRgb);
                QImage caller = original;
                auto source = snow_shot::image_codec::srgbRowSource(caller);
                require(!source.isKnownOpaque && source.classifyAlpha,
                        "alpha-capable snapshots defer pixel verification");
                caller.fill(Qt::transparent);
                std::array<ScreenshotImageAlphaContent, 4> verified{};
                std::vector<std::thread> threads;
                for (size_t i = 0; i < verified.size(); ++i)
                    threads.emplace_back(
                        [source, &verified, i] { verified[i] = source.verifiedAlphaContent(); });
                for (auto& thread : threads)
                    thread.join();
                for (auto value : verified)
                    require(value == (opaque ? ScreenshotImageAlphaContent::Opaque
                                             : ScreenshotImageAlphaContent::NonOpaque),
                            "shared opacity verification must inspect the immutable source");
                const auto payload = ScreenshotClipboardService::prepare(source);
                require(payload.isValid() && payload.pngBytes().size() > 25,
                        "prepare both clipboard representations without publication");
                require(static_cast<uchar>(payload.pngBytes()[25]) == (opaque ? 2 : 6),
                        "opaque screenshots use RGB PNG; transparent output retains alpha");
                const auto actual =
                    snow_shot::image_codec::decode(payload.pngBytes(), snow::image::Format::png,
                                                   "clipboard.png")
                        .convertToFormat(QImage::Format_RGBA8888);
                const auto expected = original.convertToFormat(QImage::Format_RGBA8888);
                require(actual.size() == expected.size(), "PNG retains screenshot dimensions");
                for (int y = 0; y < expected.height(); ++y)
                    require(std::memcmp(actual.constScanLine(y), expected.constScanLine(y),
                                        size_t(width) * 4) == 0,
                            "opaque optimization must preserve every pixel including alpha");
            }
    QImage wide(7, 3, QImage::Format_RGBA64);
    wide.fill(Qt::transparent);
    require(snow_shot::image_codec::srgbRowSource(wide).verifiedAlphaContent() ==
                ScreenshotImageAlphaContent::Unknown,
            "unsupported alpha layouts must remain conservative");
    require(snow_shot::image_codec::srgbRowSource({}).verifiedAlphaContent() ==
                ScreenshotImageAlphaContent::Unknown,
            "empty images cannot assert opacity");
}

void unknownOpacityPreservesSinglePassRows() {
    QImage image(19, 7, QImage::Format_RGBA8888);
    image.fill(QColor(23, 117, 209));
    auto source = snow_shot::image_codec::srgbRowSource(image);
    source.classifyAlpha = {};
    source.backingImage = {};
    int rowsRead = 0;
    source.readRows = [read = source.readRows, &rowsRead](int first, int count, qsizetype stride,
                                                          uchar* target, qsizetype capacity) {
        rowsRead += count;
        return read(first, count, stride, target, capacity);
    };
    const auto unknown = ScreenshotClipboardService::prepare(source);
    require(unknown.isValid() && rowsRead == image.height() &&
                static_cast<uchar>(unknown.pngBytes()[25]) == 6,
            "unknown row producers retain alpha without scanning or materializing extra rows");
    const auto verified = ScreenshotClipboardService::prepareImage(image);
    require(verified.isValid() && static_cast<uchar>(verified.pngBytes()[25]) == 2,
            "the same opaque pixels can be encoded as RGB when verified");
    const auto decode = [](const QByteArray& png) {
        return snow_shot::image_codec::decode(png, snow::image::Format::png, "clipboard.png")
            .convertToFormat(QImage::Format_RGBA8888);
    };
    const auto first = decode(unknown.pngBytes());
    const auto second = decode(verified.pngBytes());
    require(first.size() == image.size() && second.size() == image.size(),
            "both opacity contracts retain dimensions");
    for (int y = 0; y < image.height(); ++y)
        require(std::memcmp(first.constScanLine(y), image.constScanLine(y),
                            size_t(image.width()) * 4) == 0 &&
                    std::memcmp(second.constScanLine(y), image.constScanLine(y),
                                size_t(image.width()) * 4) == 0,
                "RGB and RGBA encodings preserve identical opaque pixels");
}

void combinedPayloadOwnsImageAndFile() {
    QImage image(3, 2, QImage::Format_RGBA8888);
    image.fill(QColor(23, 117, 209, 127));
    const QString path = QDir::temp().filePath(QStringLiteral("Snow Shot \u56fe\u50cf.png"));
    auto original = ScreenshotClipboardService::prepareImage(image);
    require(original.isValid() && original.filePath().isEmpty(),
            "image-only copies must not acquire a file reference");
    const QByteArray png = original.pngBytes();
    auto combined = ScreenshotClipboardService::withFile(std::move(original), path);
    require(combined.isValid() && !original.isValid() && combined.filePath() == path &&
                combined.pngBytes() == png,
            "adding a file must preserve image bytes and transfer ownership");
    auto clone = ScreenshotClipboardService::clonePrepared(combined);
    require(clone.isValid() && clone.filePath() == path && clone.pngBytes() == png,
            "cloned publications must retain both representations");
#ifdef Q_OS_WIN
    require(ScreenshotClipboardPayloadTestAccess::independentFiles(combined, clone),
            "cloned publications need independent native file ownership");
    const QByteArray bytes = ScreenshotClipboardPayloadTestAccess::fileBytes(clone);
    const QString native = QDir::toNativeSeparators(path);
    const auto pathBytes = static_cast<qsizetype>(native.size() * sizeof(wchar_t));
    require(bytes.size() >= qsizetype(sizeof(DROPFILES)) + pathBytes + 4,
            "file publication must allocate a header and terminated UTF-16 path");
    DROPFILES header{};
    std::memcpy(&header, bytes.constData(), sizeof(header));
    require(header.pFiles == sizeof(DROPFILES) && header.fWide && !header.fNC && header.pt.x == 0 &&
                header.pt.y == 0 &&
                std::memcmp(bytes.constData() + header.pFiles, native.utf16(),
                            static_cast<size_t>(pathBytes)) == 0,
            "CF_HDROP must preserve spaces and Unicode in the native file path");
    for (qsizetype i = sizeof(DROPFILES) + pathBytes; i < bytes.size(); ++i)
        require(bytes[i] == 0, "file terminators and allocator padding must be initialized");
#endif
    combined = {};
    require(clone.isValid(), "destroying one payload must leave its clone usable");
    require(!ScreenshotClipboardService::withFile(std::move(clone), QStringLiteral("relative.png"))
                    .isValid() &&
                !ScreenshotClipboardService::withFile({}, path).isValid(),
            "combined publication requires a valid image and an absolute file path");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        bitmapPixelsAndBounds();
        opacitySurvivesSnapshotsAndEncoding();
        unknownOpacityPreservesSinglePassRows();
        combinedPayloadOwnsImageAndFile();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
