#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snowimageqtcodec.h"
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QEventLoop>
#include <QMimeData>
#include <QTimer>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

struct ScreenshotClipboardPayloadTestAccess {
    static QByteArray dib(const ScreenshotClipboardPayload& payload) {
        auto handle = static_cast<HGLOBAL>(payload.m_dibHandle);
        const auto* memory = static_cast<const char*>(GlobalLock(handle));
        QByteArray bytes =
            memory == nullptr ? QByteArray{} : QByteArray(memory, GlobalSize(handle));
        if (memory != nullptr)
            GlobalUnlock(handle);
        return bytes;
    }
};

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage randomImage(int width, int height, bool alpha) {
    QImage image(width, height, QImage::Format_ARGB32);
    std::mt19937 random(0x534e4f57U);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x)
            row[x] = static_cast<QRgb>(random()) | (alpha ? 0U : 0xff000000U);
    }
    return image;
}

void comparePixels(const QImage& expected, const QImage& actual) {
    require(actual.size() == expected.size(), "readback dimensions differ");
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            if (expected.pixel(x, y) != actual.pixel(x, y)) {
                std::cerr << "pixel mismatch at " << x << ',' << y << ": expected=0x" << std::hex
                          << expected.pixel(x, y) << " actual=0x" << actual.pixel(x, y) << std::dec
                          << '\n';
                throw std::runtime_error("readback pixels differ");
            }
        }
    }
}

void verifyDib(const QImage& source, const QByteArray& bytes) {
    require(bytes.size() >= sizeof(BITMAPINFOHEADER), "DIB header missing");
    BITMAPINFOHEADER header{};
    std::memcpy(&header, bytes.constData(), sizeof(header));
    const int stride = (source.width() * 3 + 3) & ~3;
    require(header.biSize == sizeof(header) && header.biWidth == source.width() &&
                header.biHeight == source.height() && header.biPlanes == 1 &&
                header.biBitCount == 24 && header.biCompression == BI_RGB &&
                header.biSizeImage == static_cast<DWORD>(stride * source.height()) &&
                bytes.size() >= static_cast<qsizetype>(sizeof(header) + header.biSizeImage),
            "fallback must be a bottom-up 24-bit BI_RGB DIB");
    for (int y = 0; y < source.height(); ++y) {
        const auto* row = reinterpret_cast<const uchar*>(bytes.constData() + sizeof(header)) +
                          (source.height() - 1 - y) * stride;
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = source.pixel(x, y);
            const int alpha = qAlpha(pixel);
            const int channels[]{qBlue(pixel), qGreen(pixel), qRed(pixel)};
            for (int c = 0; c < 3; ++c) {
                const int expected = (channels[c] * alpha + 255 * (255 - alpha) + 127) / 255;
                require(row[x * 3 + c] == expected, "DIB fallback did not composite onto white");
            }
        }
        for (int pad = source.width() * 3; pad < stride; ++pad)
            require(row[pad] == 0, "DIB row padding must be initialized");
    }
}

QByteArray nativeBytes(UINT format) {
    // The OS clipboard is a shared resource; the clipboard history service or
    // a neighbouring application can hold it briefly, so the open needs
    // bounded retries before the payload can be judged.
    bool opened = false;
    for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) {
            Sleep(20);
        }
    }
    require(opened, "could not open clipboard");
    const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
    const auto* memory = handle == nullptr ? nullptr : static_cast<const char*>(GlobalLock(handle));
    QByteArray bytes = memory == nullptr ? QByteArray{} : QByteArray(memory, GlobalSize(handle));
    if (memory != nullptr)
        GlobalUnlock(handle);
    CloseClipboard();
    require(!bytes.isEmpty(), "clipboard representation missing");
    return bytes;
}

void payloadsPreservePixels() {
    require(!ScreenshotClipboardService::prepareImage({}).isValid(), "empty image was accepted");
    // All DWORD padding cases, single-row images, and odd multi-batch heights.
    for (int width : {1, 2, 3, 4, 257}) {
        for (int height : {1, 193}) {
            for (bool alpha : {false, true}) {
                const QImage original = randomImage(width, height, alpha);
                for (auto format : {QImage::Format_ARGB32, QImage::Format_RGBA8888,
                                    QImage::Format_ARGB32_Premultiplied}) {
                    const QImage source = original.convertToFormat(format);
                    const QImage expected = source.convertToFormat(QImage::Format_ARGB32);
                    auto rows = snow_shot::image_codec::srgbRowSource(source);
                    const QByteArray png = snow_shot::image_codec::encodePng(rows);
                    auto direct = ScreenshotClipboardService::prepareImage(source);
                    auto reused = ScreenshotClipboardService::prepare(rows, png);
                    require(direct.isValid() && reused.isValid(), "payload preparation failed");
                    require(reused.pngBytes().constData() == png.constData(),
                            "pre-encoded PNG was copied or re-encoded");
                    comparePixels(expected, QImage::fromData(direct.pngBytes(), "PNG"));
                    comparePixels(expected, QImage::fromData(reused.pngBytes(), "PNG"));
                    verifyDib(expected, ScreenshotClipboardPayloadTestAccess::dib(direct));
                    verifyDib(expected, ScreenshotClipboardPayloadTestAccess::dib(reused));
                    auto moved = std::move(reused);
                    require(moved.isValid() && !reused.isValid(), "payload move lost ownership");
                }
            }
        }
    }
    QImage tagged = randomImage(31, 17, true);
    tagged.setColorSpace(QColorSpace::SRgbLinear);
    auto payload = ScreenshotClipboardService::prepareImage(tagged);
    const QImage expected = tagged.convertedToColorSpace(QColorSpace::SRgb);
    comparePixels(expected, QImage::fromData(payload.pngBytes(), "PNG"));
    verifyDib(expected, ScreenshotClipboardPayloadTestAccess::dib(payload));
    auto rows = snow_shot::image_codec::srgbRowSource(tagged);
    rows.cancellationRequested = [] { return true; };
    require(!ScreenshotClipboardService::prepare(rows).isValid(), "cancelled source was accepted");
    rows.cancellationRequested = {};
    rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
    require(!ScreenshotClipboardService::prepare(rows).isValid(), "failed encoding was accepted");
    require(!ScreenshotClipboardService::prepare(rows, payload.pngBytes()).isValid(),
            "failed fallback read was accepted");
}

QImage browserFixture() {
    QImage source = randomImage(257, 193, false);
    // Binary channels avoid Qt's unavoidable premultiplication quantization.
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            if (x < 7 || y >= source.height() - 5)
                source.setPixel(x, y, 0);
            else if (x > source.width() / 2)
                source.setPixel(x, y, qRgba(255, 0, 255, 128));
        }
    }
    return source;
}

void publishAndReadThroughQt() {
    const QImage source = browserFixture();
    auto payload = ScreenshotClipboardService::prepareImage(source);
    const QByteArray expectedPng = payload.pngBytes();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool succeeded = false;
    auto handle =
        ScreenshotClipboardService::commit(QApplication::clipboard(), &loop, std::move(payload),
                                           [&](ScreenshotClipboardCommitResult result) {
                                               succeeded = result.succeeded();
                                               loop.quit();
                                           });
    require(handle.isValid(), "commit was not scheduled");
    timeout.start(2000);
    loop.exec();
    handle.cancel();
    require(succeeded, "clipboard commit failed or timed out");
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    require(nativeBytes(pngFormat).startsWith(expectedPng),
            "published PNG differs from prepared bytes");
    verifyDib(source, nativeBytes(CF_DIB));
    require(OpenClipboard(nullptr) != FALSE, "could not inspect format order");
    const UINT first = EnumClipboardFormats(0);
    CloseClipboard();
    require(first == pngFormat, "PNG must be published first");
    QApplication::processEvents();
    comparePixels(source,
                  QApplication::clipboard()->image().convertToFormat(QImage::Format_ARGB32));
    auto snapshot = ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1.0);
    require(snapshot.has_value() && !snapshot->encodedImages.isEmpty(),
            "Snow Shot must capture encoded PNG before considering a bitmap");
    const auto decoded = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
    require(decoded.has_value(), "Snow Shot PNG readback failed");
    comparePixels(source, decoded->image.convertToFormat(QImage::Format_ARGB32));
}

void corruptEncodedImageRetainsNativeBitmap() {
    QClipboard* clipboard = QApplication::clipboard();
    const QImage source = randomImage(3, 2, false);
    auto* mime = new QMimeData;
    mime->setImageData(source);
    mime->setData(QStringLiteral("image/png"), QByteArrayLiteral("corrupt"));
    mime->setText(QStringLiteral("text fallback"));
    clipboard->setMimeData(mime);
    auto snapshot = ScreenshotClipboardContentReader::snapshot(clipboard, 1.0);
    clipboard->clear();
    require(snapshot.has_value() && !snapshot->encodedImages.isEmpty() &&
                snapshot->nativeDib.has_value(),
            "live snapshot must retain a native bitmap alongside encoded image bytes");
    const auto decoded = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
    require(decoded.has_value() && !decoded->isFormattedText(),
            "corrupt encoded data must fall back to the owned native bitmap");
    comparePixels(source, decoded->image.convertToFormat(QImage::Format_ARGB32));
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--browser-fixture"))) {
            require(ScreenshotClipboardService::publishImage(QApplication::clipboard(),
                                                             browserFixture()),
                    "browser fixture publication failed");
            std::cout << "Browser clipboard fixture ready" << std::endl;
            QTimer::singleShot(120000, &application, &QApplication::quit);
            return application.exec();
        }
        payloadsPreservePixels();
        publishAndReadThroughQt();
        corruptEncodedImageRetainsNativeBitmap();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
