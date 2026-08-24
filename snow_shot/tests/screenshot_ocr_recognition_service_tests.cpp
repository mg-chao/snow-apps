#include "snow_shot/presentation/screenshotocrrecognitionservice.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"

#include "snow_ocr_c.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr int kRecognitionTimeoutMs = 30'000;

QImage whiteImage(int edge = 64) {
    QImage image(edge, edge, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    return image;
}

SnowOcrResourceCountsV1 resourceCounts() {
    SnowOcrResourceCountsV1 counts{};
    counts.struct_size = static_cast<std::uint32_t>(sizeof(SnowOcrResourceCountsV1));
    require(snow_ocr_resource_counts_v1(&counts) != 0, "OCR resource counts should be available");
    return counts;
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs) {
    if (condition()) {
        return true;
    }
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    poll.setInterval(5);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (condition()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMs);
    loop.exec();
    return condition();
}

void runtimePrewarmDoesNotCreateRecognitionResources() {
    ScreenshotOcrRecognitionService service;
    const SnowOcrResourceCountsV1 before = resourceCounts();
    require(service.prewarmRuntime() && service.prewarmRuntime(),
            "OCR runtime prewarm should be successful and idempotent");
    const SnowOcrResourceCountsV1 after = resourceCounts();
    require(service.liveWorkerCount() == 0 && after.engines == before.engines &&
                after.results == before.results && after.owned_images == before.owned_images,
            "OCR runtime prewarm must not create workers, engines, results, or images");
}

void processEventsFor(int durationMs) {
    QEventLoop loop;
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    loop.exec();
}

void isolatedEngineCompletesThroughTheQtWorker(bool directMlEnabled) {
    ScreenshotOcrRecognitionService service(directMlEnabled
                                                ? ScreenshotOcrBackendPreference::DirectMl
                                                : ScreenshotOcrBackendPreference::Cpu);
    require(service.liveWorkerCount() == 0,
            "OCR service construction must not create worker threads eagerly");
    QEventLoop loop;
    ScreenshotOcrRecognitionResult output;
    bool completed = false;
    bool timedOut = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    const QImage image = whiteImage();
    const ScreenshotOcrRecognitionPort::RequestToken token =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &loop, [&](ScreenshotOcrRecognitionResult result) {
                              output = std::move(result);
                              completed = true;
                              loop.quit();
                          });

    require(token != 0, "a valid OCR image should schedule recognition");
    timeout.start(kRecognitionTimeoutMs);
    loop.exec();

    require(!timedOut && completed, "OCR recognition should complete within the test timeout");
    if (!output.error.isEmpty()) {
        std::cerr << "OCR error: " << output.error.toStdString() << '\n';
    }
    require(output.error.isEmpty(), "the isolated OCR engine should not report an error");
    require(output.presentation != nullptr, "OCR recognition should return a presentation");
    require(!output.presentation->filledImage.isNull() &&
                output.presentation->filledImage.size() == image.size(),
            "OCR recognition should copy the filled image into the parent process");
    const SnowOcrResourceCountsV1 counts = resourceCounts();
    require(counts.engines == 0 && counts.results == 0 && counts.owned_images == 0,
            "isolated OCR must not create FFI resources in the parent process");
    require(service.liveWorkerCount() == 0,
            "OCR workers should exit before recognition completion is delivered");
}

void concurrentRequestsCompleteExactlyOnce() {
    constexpr int kRequestCount = 3;
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    int completions = 0;
    std::vector<int> completionOrder;
    std::vector<ScreenshotOcrRecognitionResult> outputs;
    completionOrder.reserve(kRequestCount);
    outputs.reserve(kRequestCount);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    for (int index = 0; index < kRequestCount; ++index) {
        const QImage image = whiteImage();
        const auto token = service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(index, index), QSizeF(image.size()))},
            &receiver, [&, index](ScreenshotOcrRecognitionResult result) {
                ++completions;
                completionOrder.push_back(index);
                outputs.push_back(std::move(result));
                if (completions == kRequestCount) {
                    loop.quit();
                }
            });
        require(token != 0, "every concurrent OCR request should be accepted");
    }

    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(!timedOut, "concurrent OCR requests should finish within the test timeout");
    require(completions == kRequestCount, "every concurrent OCR request should complete once");
    std::sort(completionOrder.begin(), completionOrder.end());
    require(completionOrder == std::vector<int>({0, 1, 2}),
            "every OCR request should complete exactly once");
    for (const auto& output : outputs) {
        require(output.error.isEmpty(), "concurrent OCR should not report an error");
        require(output.presentation != nullptr,
                "every concurrent OCR request should return a presentation");
    }

    require(service.liveWorkerCount() == 0,
            "OCR workers should exit before concurrent recognition completion is delivered");

    SnowOcrResourceCountsV1 counts = resourceCounts();
    require(counts.engines == 0 && counts.results == 0 && counts.owned_images == 0,
            "concurrent isolated OCR must leave parent-process FFI resources untouched");

    outputs.clear();
    counts = resourceCounts();
    require(counts.owned_images == 0,
            "copied presentation images must not use parent-process FFI ownership");
}

void interactiveRequestsPrecedeQueuedPrefetch() {
    ScreenshotOcrRecognitionService::Options options;
    options.workerCount = 1;
    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    bool resourcesRetainedForQueuedTask = false;
    std::vector<int> completionOrder;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    const auto submit = [&](int id, int imageEdge, ScreenshotOcrRequestPriority priority) {
        const QImage image = whiteImage(imageEdge);
        const auto token = service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size())), priority},
            &receiver, [&, id](ScreenshotOcrRecognitionResult result) {
                require(result.error.isEmpty() && result.presentation != nullptr,
                        "priority test OCR requests should succeed");
                completionOrder.push_back(id);
                if (completionOrder.size() == 1) {
                    const SnowOcrResourceCountsV1 counts = resourceCounts();
                    resourcesRetainedForQueuedTask = service.liveWorkerCount() == 1 &&
                                                     counts.engines == 0 && counts.results == 0 &&
                                                     counts.owned_images == 0;
                }
                if (completionOrder.size() == 3) {
                    loop.quit();
                }
            });
        require(token != 0, "priority test OCR requests should be accepted");
    };

    submit(0, 512, ScreenshotOcrRequestPriority::Interactive);
    submit(1, 64, ScreenshotOcrRequestPriority::Prefetch);
    submit(2, 64, ScreenshotOcrRequestPriority::Interactive);

    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(!timedOut, "priority test OCR requests should finish within the timeout");
    require(completionOrder == std::vector<int>({0, 2, 1}),
            "queued interactive OCR must run before queued prefetch OCR");
    require(resourcesRetainedForQueuedTask,
            "the service worker should remain available while queued recognition tasks remain");
}

void resourcesAreReclaimedWhenRecognitionCompletes() {
    require(resourceCounts().engines == 0,
            "the previous OCR service should destroy its engines during shutdown");
    ScreenshotOcrRecognitionService service;
    require(service.liveWorkerCount() == 0,
            "OCR service construction must not create workers eagerly");
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    ScreenshotOcrRecognitionResult output;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    const QImage image = whiteImage();
    const auto token =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
                              output = std::move(result);
                              loop.quit();
                          });
    require(token != 0, "the resource-reclamation OCR request should be accepted");
    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(!timedOut && output.presentation != nullptr && output.error.isEmpty(),
            "the resource-reclamation OCR request should complete successfully");
    require(resourceCounts().engines == 0,
            "a completed OCR request should release its engine before completion delivery");
    require(service.liveWorkerCount() == 0,
            "a completed OCR request should release its worker before completion delivery");
    require(!output.presentation->filledImage.isNull(),
            "the delivered OCR presentation should own a copied image");
    require(resourceCounts().owned_images == 0,
            "the copied presentation image must not retain an FFI owned image");

    output.presentation.reset();
    require(resourceCounts().owned_images == 0,
            "releasing the presentation should leave parent FFI ownership unchanged");
    const SnowOcrResourceCountsV1 counts = resourceCounts();
    require(counts.results == 0 && counts.owned_images == 0,
            "immediate reclamation should leave no live OCR result resources");

    bool recreated = false;
    const auto secondToken =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
                              recreated = result.presentation != nullptr && result.error.isEmpty();
                          });
    require(secondToken != 0, "a request after resource reclamation should be accepted");
    require(waitUntil([&]() { return recreated; }, kRecognitionTimeoutMs),
            "a request after resource reclamation should create a new OCR engine");
    require(resourceCounts().engines == 0,
            "the recreated OCR engine should be released before completion delivery");
}

void idleReleaseWaitsForCancelledRecognition() {
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    bool completionDelivered = false;
    const QImage image = whiteImage(256);
    const auto token = service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult) { completionDelivered = true; });
    require(token != 0, "idle-release OCR request should be accepted");
    require(waitUntil([&]() { return service.liveWorkerCount() > 0; }, kRecognitionTimeoutMs),
            "idle-release OCR request did not initialize its worker");
    service.cancel(token);

    bool released = false;
    require(service.releaseRetainedIdleResources(
                [&](bool resourcesReleased) { released = resourcesReleased; }),
            "OCR idle-resource release should be scheduled");
    require(waitUntil([&]() { return released; }, kRecognitionTimeoutMs),
            "OCR idle-resource release completed before its worker drained");
    require(!completionDelivered && service.liveWorkerCount() == 0 && resourceCounts().engines == 0,
            "OCR idle-resource release retained a worker or delivered cancelled work");
}

void queuedCancellationSkipsExecution() {
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    int completions = 0;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::vector<ScreenshotOcrRecognitionPort::RequestToken> tokens;
    for (int index = 0; index < 3; ++index) {
        const QImage image = whiteImage(256);
        tokens.push_back(
            service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                              &receiver, [&](ScreenshotOcrRecognitionResult) {
                                  ++completions;
                                  if (completions == 2) {
                                      loop.quit();
                                  }
                              }));
        require(tokens.back() != 0, "queued cancellation requests should be accepted");
    }
    service.cancel(tokens.back());
    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(completions == 2, "cancelling the queued third request must suppress delivery");
}

void cancellationSuppressesCompletion() {
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    bool completed = false;
    const QImage image = whiteImage();
    const auto token =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult) { completed = true; });
    require(token != 0, "the cancellable OCR request should be accepted");
    service.cancel(token);
    processEventsFor(250);
    require(!completed, "an immediately cancelled OCR request must not invoke its completion");
}

void receiverDestructionSuppressesCompletion() {
    ScreenshotOcrRecognitionService service;
    auto receiver = std::make_unique<QObject>();
    bool completed = false;
    const QImage image = whiteImage();
    const auto token = service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, receiver.get(),
        [&](ScreenshotOcrRecognitionResult) { completed = true; });
    require(token != 0, "the receiver-guarded OCR request should be accepted");
    receiver.reset();
    processEventsFor(250);
    require(!completed, "destroying the receiver must suppress OCR completion");
}

void serviceDestructionJoinsWorkersAndSuppressesLateDelivery() {
    QObject receiver;
    int completions = 0;
    auto service = std::make_unique<ScreenshotOcrRecognitionService>();
    for (int index = 0; index < 3; ++index) {
        const QImage image = whiteImage(256);
        const auto token =
            service->recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                               &receiver, [&](ScreenshotOcrRecognitionResult) { ++completions; });
        require(token != 0, "requests queued before service shutdown should be accepted");
    }

    require(waitUntil([&]() { return service->liveWorkerCount() > 0; }, kRecognitionTimeoutMs),
            "at least one OCR service worker should initialize before shutdown");
    const int completionsBeforeDestruction = completions;
    service.reset();
    processEventsFor(250);
    require(completions == completionsBeforeDestruction,
            "destroyed OCR services must not deliver queued completions");
    const SnowOcrResourceCountsV1 counts = resourceCounts();
    require(counts.engines == 0 && counts.results == 0 && counts.owned_images == 0,
            "service destruction should synchronously join workers and release FFI resources");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const bool directMlRequested = application.arguments().contains(QStringLiteral("--directml"));
    runtimePrewarmDoesNotCreateRecognitionResources();
    isolatedEngineCompletesThroughTheQtWorker(directMlRequested);
    if (!directMlRequested) {
        concurrentRequestsCompleteExactlyOnce();
        interactiveRequestsPrecedeQueuedPrefetch();
        queuedCancellationSkipsExecution();
        resourcesAreReclaimedWhenRecognitionCompletes();
        idleReleaseWaitsForCancelledRecognition();
        cancellationSuppressesCompletion();
        receiverDestructionSuppressesCompletion();
        serviceDestructionJoinsWorkersAndSuppressesLateDelivery();
    }
    return 0;
}
