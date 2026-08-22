#include "snow_shot/presentation/screenshotpinnedcopyservice.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void processUntil(QCoreApplication& application, const std::function<bool()>& predicate) {
    for (int iteration = 0; iteration < 500 && !predicate(); ++iteration) {
        application.processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

void originalCopyCoalescesDuplicates() {
    ScreenshotPinnedCopyService service;
    QObject receiver;
    QImage source(QSize(4, 3), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 60, 90, 255));
    int callbackCount = 0;
    bool callbackPayloadValid = false;
    require(service.requestOriginalImage(
                source, &receiver,
                [&callbackCount, &callbackPayloadValid](ScreenshotClipboardPayload payload) {
                    ++callbackCount;
                    callbackPayloadValid = payload.isValid();
                }),
            "original pinned copy was not scheduled");
    require(!service.requestOriginalImage(source, &receiver, [](ScreenshotClipboardPayload) {}),
            "duplicate pinned copy was not coalesced");
    processUntil(*QCoreApplication::instance(), [&callbackCount]() { return callbackCount == 1; });
    require(callbackCount == 1 && callbackPayloadValid,
            "original pinned copy did not deliver one valid payload");
}

void invalidatedViewportCopyIsSuppressed() {
    SnowCanvasRuntime runtime;
    const QByteArray session = runtime.serializeDocumentSession();
    require(!session.isEmpty(), "runtime session could not be serialized");
    ScreenshotPinnedCopyService service;
    QObject receiver;
    int callbackCount = 0;
    QImage background(QSize(64, 48), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(12, 24, 36, 255));
    ScreenshotPinnedViewportCopyRequest request{
        session, std::move(background), QRectF(0.0, 0.0, 64.0, 48.0), QSize(64, 48), {},
    };
    require(service.requestCurrentViewport(
                std::move(request), &receiver,
                [&callbackCount](ScreenshotClipboardPayload) { ++callbackCount; }),
            "viewport pinned copy was not scheduled");
    service.invalidate();
    processUntil(*QCoreApplication::instance(), [&callbackCount]() { return callbackCount != 0; });
    require(callbackCount == 0, "invalidated pinned copy reached its callback");
}

void currentImageRequestReturnsRenderedPixels() {
    SnowCanvasRuntime runtime;
    const QByteArray session = runtime.serializeDocumentSession();
    require(!session.isEmpty(), "runtime session could not be serialized");
    ScreenshotPinnedCopyService service;
    QObject receiver;
    QImage rendered;
    QImage background(QSize(64, 48), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(12, 24, 36, 255));
    ScreenshotPinnedViewportCopyRequest request{
        session, std::move(background), QRectF(0.0, 0.0, 64.0, 48.0), QSize(64, 48), {},
    };
    require(service.requestCurrentImage(
                std::move(request), &receiver,
                [&rendered](QImage image) { rendered = std::move(image); }),
            "pinned image export was not scheduled");
    processUntil(*QCoreApplication::instance(), [&rendered]() { return !rendered.isNull(); });
    require(rendered.size() == QSize(64, 48) &&
                rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) ==
                    QColor(12, 24, 36, 255),
            "pinned image export did not return the rendered pixels");
}

void destructionSuppressesQueuedCompletion() {
    QObject receiver;
    int callbackCount = 0;
    QImage source(QSize(512, 512), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(25, 50, 75, 255));
    auto service = std::make_unique<ScreenshotPinnedCopyService>();
    require(
        service->requestOriginalImage(
            source, &receiver, [&callbackCount](ScreenshotClipboardPayload) { ++callbackCount; }),
        "destruction-lifetime pinned copy was not scheduled");
    service.reset();
    processUntil(*QCoreApplication::instance(), [&callbackCount]() { return callbackCount != 0; });
    require(callbackCount == 0, "destroyed pinned copy service received a queued completion");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        originalCopyCoalescesDuplicates();
        invalidatedViewportCopyIsSuppressed();
        currentImageRequestReturnsRenderedPixels();
        destructionSuppressesQueuedCompletion();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
