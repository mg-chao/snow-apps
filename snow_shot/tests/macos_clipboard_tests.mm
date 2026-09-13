#include "snow_shot/platform/macos/pngmimeconverter.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"

#include <QClipboard>
#include <QColor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMimeData>
#include <QThread>
#include <QTimer>

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using snow_shot::platform::macos::ensurePngMimeConverter;
using snow_shot::platform::macos::PngMimeConverter;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void converterPreservesEncodedBytes() {
    PngMimeConverter converter;
    const QString mime = QStringLiteral("image/png");
    const QString uti = QStringLiteral("public.png");
    // Deliberately opaque bytes ensure conversion is only a format mapping.
    // The export artifact is responsible for encoding and validating its PNG.
    const QByteArray bytes("cached\0png\xff", 11);
    require(converter.utiForMime(mime) == uti && converter.mimeForUti(uti) == mime,
            "PNG must map to the native public.png image type in both directions");
    require(converter.utiForMime(QStringLiteral("image/jpeg")).isEmpty() &&
                converter.mimeForUti(QStringLiteral("public.tiff")).isEmpty(),
            "the PNG converter must not intercept other image formats");
    require(converter.convertFromMime(mime, bytes, uti) == QList<QByteArray>{bytes},
            "outgoing PNG bytes must not be decoded or re-encoded");
    require(converter.convertToMime(mime, {bytes}, uti).toByteArray() == bytes,
            "incoming PNG bytes must remain intact");
    require(converter
                    .convertFromMime(mime, QVariant::fromValue(QImage(2, 2, QImage::Format_ARGB32)),
                                     uti)
                    .isEmpty() &&
                converter.convertFromMime(mime, QByteArray{}, uti).isEmpty(),
            "the converter accepts nonempty encoded bytes, not decoded images");
    require(!converter.convertToMime(mime, {}, uti).isValid() &&
                !converter.convertToMime(mime, {bytes, bytes}, uti).isValid() &&
                !converter.convertToMime(mime, {bytes}, QStringLiteral("public.tiff")).isValid(),
            "unsupported native representations must be rejected");
}

ScreenshotClipboardPayload cachedPayload(const QByteArray& png) {
    ScreenshotImageRowSource rows;
    rows.size = QSize(3, 2);
    rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) -> bool {
        throw std::runtime_error("cached PNG publication must not reread or encode image pixels");
    };
    auto payload = ScreenshotClipboardService::prepare(rows, png);
    require(payload.isValid() && payload.pngBytes() == png,
            "preparation must preserve the cached canonical PNG");
    return payload;
}

void requireClipboardBytes(const QByteArray& png) {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    require(mime != nullptr && mime->data(QStringLiteral("image/png")) == png,
            "clipboard publication must retain the canonical PNG");
    require(!mime->formats().contains(QStringLiteral("application/x-qt-image")),
            "publication must not add a decoded image that Qt re-encodes as TIFF");
}

void publishAndCommitPreserveCachedPng(const QByteArray& png) {
    require(ScreenshotClipboardService::publish(QGuiApplication::clipboard(), cachedPayload(png)),
            "synchronous PNG publication must succeed");
    requireClipboardBytes(png);

    QEventLoop loop;
    bool completed = false;
    ScreenshotClipboardCommitResult result;
    const auto completion = [&](ScreenshotClipboardCommitResult value) {
        result = value;
        completed = true;
        loop.quit();
    };
    auto handle = ScreenshotClipboardService::commit(QGuiApplication::clipboard(), &loop,
                                                     cachedPayload(png), completion);
    require(handle.isValid(), "asynchronous PNG publication must be accepted");
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed && result.succeeded() && result.attempts == 1,
            "asynchronous PNG publication must complete successfully");
    requireClipboardBytes(png);

    auto* mimeData = new QMimeData();
    mimeData->setData(QStringLiteral("image/png"), png);
    mimeData->setText(QStringLiteral("recognized text"));
    completed = false;
    handle = ScreenshotClipboardService::commitMimeData(QGuiApplication::clipboard(), &loop,
                                                        mimeData, completion);
    require(handle.isValid(), "mixed MIME publication must be accepted");
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed && result.succeeded(), "mixed MIME publication must complete successfully");
    requireClipboardBytes(png);
    require(QGuiApplication::clipboard()->text() == QStringLiteral("recognized text"),
            "PNG support must preserve companion clipboard text");
}

void registrationRequiresGuiThread() {
    require(ensurePngMimeConverter() && ensurePngMimeConverter(),
            "registration must be repeatable on the application thread");
    bool registeredFromWorker = true;
    std::unique_ptr<QThread> worker(
        QThread::create([&]() { registeredFromWorker = ensurePngMimeConverter(); }));
    worker->start();
    require(worker->wait(2000), "worker registration check must finish");
    require(!registeredFromWorker, "conversion registration must reject worker threads");
}

void nativeApplicationsReadPng(const QByteArray& png) {
    @autoreleasepool {
        // Never use the general pasteboard: this uniquely named board is
        // independent of the user's clipboard and disappears after the test.
        NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
        PngMimeConverter converter;
        const QString mime = QStringLiteral("image/png");
        const QString uti = converter.utiForMime(mime);
        const QList<QByteArray> nativeData = converter.convertFromMime(mime, png, uti);
        require(nativeData.size() == 1, "one native PNG representation must be produced");
        NSString* type = uti.toNSString();
        const QByteArray& bytes = nativeData.first();
        NSData* data = [NSData dataWithBytes:bytes.constData()
                                      length:static_cast<NSUInteger>(bytes.size())];
        [board declareTypes:@[ type ] owner:nil];
        const bool wrote = [board setData:data forType:type];
        const bool advertised = [board.types containsObject:NSPasteboardTypePNG];
        NSData* readback = [board dataForType:NSPasteboardTypePNG];
        const bool preserved = [readback isEqualToData:data];
        NSImage* nativeImage = [[NSImage alloc] initWithPasteboard:board];
        const bool imageReadable =
            nativeImage != nil && nativeImage.size.width == 3 && nativeImage.size.height == 2;
        [board releaseGlobally];
        require(wrote && advertised && preserved,
                "the native pasteboard must advertise public.png with unchanged bytes");
        require(imageReadable, "AppKit image consumers must recognize the published screenshot");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(!ensurePngMimeConverter(), "registration must require a GUI application");
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication application(argc, argv);
        require(QGuiApplication::platformName() == QStringLiteral("offscreen"),
                "service tests must use an isolated offscreen clipboard");
        QImage image(3, 2, QImage::Format_ARGB32);
        image.fill(QColor(32, 96, 160, 192));
        const QByteArray png = ScreenshotClipboardService::prepareImage(image).pngBytes();
        require(!png.isEmpty(), "the test fixture must encode a valid PNG");
        publishAndCommitPreserveCachedPng(png);
        registrationRequiresGuiThread();
        converterPreservesEncodedBytes();
        nativeApplicationsReadPng(
            QGuiApplication::clipboard()->mimeData()->data(QStringLiteral("image/png")));
        std::cout << "macOS PNG clipboard mapping, publication and native image checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
