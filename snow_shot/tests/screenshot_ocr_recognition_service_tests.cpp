#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
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

void processEventsFor(int durationMs) {
    QEventLoop loop;
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    loop.exec();
}

void explicitAssetsControlReadiness() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary OCR asset directory should be available");

    ScreenshotOcrRecognitionService::Options options;
    options.processPath = QDir(directory.path()).filePath(QStringLiteral("snow-ocr-process.exe"));
    options.detectorModelPath = QDir(directory.path()).filePath(QStringLiteral("det.onnx"));
    options.recognizerModelPath = QDir(directory.path()).filePath(QStringLiteral("rec.onnx"));
    options.dictionaryPath = QDir(directory.path()).filePath(QStringLiteral("dict.txt"));
    ScreenshotOcrRecognitionService service(options);
    require(!service.modelFilesReady(),
            "missing explicit OCR assets must report that components are unavailable");

    for (const QString& path : {options.processPath, options.detectorModelPath,
                                options.recognizerModelPath, options.dictionaryPath}) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "OCR asset fixture file should be writable");
        file.write("fixture");
    }
    require(service.modelFilesReady(),
            "a complete explicit OCR asset set must report ready");
}

void diskBackedEngineCompletesThroughTheQtWorker(bool directMlEnabled) {
    ScreenshotOcrRecognitionService service(
        directMlEnabled ? ScreenshotOcrBackendPreference::DirectMl
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
        service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &loop,
                          [&](ScreenshotOcrRecognitionResult result) {
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
    require(output.error.isEmpty(), "the disk-backed OCR engine should not report an error");
    require(output.presentation != nullptr, "OCR recognition should return a presentation");
}

std::shared_ptr<ScreenshotOcrPresentation> filterPresentation(const QRect& selection) {
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = selection;
    ScreenshotOcrLine line;
    line.text = QStringLiteral("OCR");
    line.quad = QPolygonF({QPointF(selection.left() + 8.0, selection.top() + 8.0),
                           QPointF(selection.right() - 8.0, selection.top() + 8.0),
                           QPointF(selection.right() - 8.0, selection.bottom() - 8.0),
                           QPointF(selection.left() + 8.0, selection.bottom() - 8.0)});
    presentation->lines.push_back(std::move(line));
    presentation->prepareForRendering();
    return presentation;
}

void renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine() {
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(QColor(20, 80, 220));
    const QRectF canvasRect(QPointF(), QSizeF(image.size()));
    ScreenshotOcrRequest request;
    request.image = image;
    request.canvasRect = canvasRect;
    request.presentation = filterPresentation(canvasRect.toAlignedRect());
    request.backgroundColor = QColor(30, 40, 50);

    ScreenshotOcrRecognitionResult output;
    bool completed = false;
    const auto token = service.render(
        std::move(request), &receiver, [&](ScreenshotOcrRecognitionResult result) {
            output = std::move(result);
            completed = true;
        });
    require(token != 0, "a valid render-only OCR request should be accepted");
    require(waitUntil([&]() { return completed; }, kRecognitionTimeoutMs),
            "render-only OCR work should complete on the worker");
    require(output.error.isEmpty() && output.presentation == nullptr &&
                !output.filteredImage.isNull(),
            "render-only OCR work should return only its transient filtered image");
    require(output.filteredImageCanvasRect.isValid() &&
                !output.filteredImageCanvasRect.isEmpty() &&
                canvasRect.contains(output.filteredImageCanvasRect),
            "render-only OCR work should report the canvas rect covered by its filtered crop");
    const qreal renderScale = image.width() / canvasRect.width();
    require(std::abs(output.filteredImage.width() -
                     output.filteredImageCanvasRect.width() * renderScale) <= 1.0 &&
                std::abs(output.filteredImage.height() -
                         output.filteredImageCanvasRect.height() * renderScale) <= 1.0,
            "the filtered image should be sized to match its canvas rect at source resolution");
    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "the render-only OCR worker should retire after its queue drains");
}

void recognitionRenderIntentCanChangeWhileQueued() {
    ScreenshotOcrRecognitionService::Options options;
    options.workerCount = 1;
    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    const QImage blocker = whiteImage(768);
    bool blockerCompleted = false;
    const auto blockerToken = service.recognize(
        ScreenshotOcrRequest{blocker, QRectF(QPointF(), QSizeF(blocker.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult result) {
            require(result.error.isEmpty() && result.presentation != nullptr,
                    "the render-intent blocker recognition should succeed");
            blockerCompleted = true;
        });
    require(blockerToken != 0, "the render-intent blocker should be accepted");

    QImage promotedImage(96, 64, QImage::Format_RGBA8888);
    promotedImage.fill(QColor(20, 80, 220));
    ScreenshotOcrRequest promotedRequest;
    promotedRequest.image = promotedImage;
    promotedRequest.canvasRect = QRectF(QPointF(), QSizeF(promotedImage.size()));
    promotedRequest.priority = ScreenshotOcrRequestPriority::Prefetch;
    bool promotedCompleted = false;
    ScreenshotOcrRecognitionResult promotedOutput;
    const auto promotedToken = service.recognize(
        std::move(promotedRequest), &receiver, [&](ScreenshotOcrRecognitionResult result) {
            promotedOutput = std::move(result);
            promotedCompleted = true;
        });
    require(promotedToken != 0 &&
                service.setRenderFilteredImage(promotedToken, true, QColor(Qt::white)),
            "a queued prefetch should accept interactive render promotion");
    require(waitUntil([&]() { return blockerCompleted && promotedCompleted; },
                      kRecognitionTimeoutMs),
            "promoted recognition should finish within the timeout");
    require(promotedOutput.error.isEmpty() && promotedOutput.presentation != nullptr &&
                !promotedOutput.filteredImage.isNull(),
            "a promoted prefetch should render its transient effect in the recognition worker");

    ScreenshotOcrRequest suppressedRequest;
    suppressedRequest.image = promotedImage;
    suppressedRequest.canvasRect = QRectF(QPointF(), QSizeF(promotedImage.size()));
    suppressedRequest.renderFilteredImage = true;
    bool suppressedCompleted = false;
    ScreenshotOcrRecognitionResult suppressedOutput;
    const auto suppressedToken = service.recognize(
        std::move(suppressedRequest), &receiver, [&](ScreenshotOcrRecognitionResult result) {
            suppressedOutput = std::move(result);
            suppressedCompleted = true;
        });
    require(suppressedToken != 0 &&
                service.setRenderFilteredImage(suppressedToken, false),
            "an abandoned recognition should accept render suppression");
    require(waitUntil([&]() { return suppressedCompleted; }, kRecognitionTimeoutMs),
            "render-suppressed recognition should still complete and remain cacheable");
    require(suppressedOutput.error.isEmpty() && suppressedOutput.presentation != nullptr &&
                suppressedOutput.filteredImage.isNull(),
            "render suppression should preserve OCR output without retaining filtered pixels");
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
            ScreenshotOcrRequest{
                image, QRectF(QPointF(index, index), QSizeF(image.size()))},
            &receiver,
            [&, index](ScreenshotOcrRecognitionResult result) {
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

    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "OCR workers should exit once a concurrent burst is drained");

    outputs.clear();
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
    std::vector<int> completionOrder;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    const auto submit = [&](int id, int imageEdge, ScreenshotOcrRequestPriority priority) {
        const QImage image = whiteImage(imageEdge);
        const auto token = service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size())), priority},
            &receiver,
            [&, id](ScreenshotOcrRecognitionResult result) {
                require(result.error.isEmpty() && result.presentation != nullptr,
                        "priority test OCR requests should succeed");
                completionOrder.push_back(id);
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
}

void workerRecyclesImmediatelyAndCanBeRecreated() {
    ScreenshotOcrRecognitionService service;
    require(service.liveWorkerCount() == 0,
            "OCR service construction must not create worker threads eagerly");
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
    const auto token = service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult result) {
            output = std::move(result);
            loop.quit();
        });
    require(token != 0, "the immediate-retirement OCR request should be accepted");
    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(!timedOut && output.presentation != nullptr && output.error.isEmpty(),
            "the OCR request should complete successfully");
    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "an OCR worker thread should exit as soon as its queue is empty");
    output.presentation.reset();

    bool recreated = false;
    const auto secondToken = service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult result) {
            recreated = result.presentation != nullptr && result.error.isEmpty();
        });
    require(secondToken != 0, "a request after immediate recycling should be accepted");
    require(waitUntil([&]() { return recreated; }, kRecognitionTimeoutMs),
            "a request after immediate recycling should recreate the OCR engine");
    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "the recreated OCR worker should exit after completing the request");
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
        tokens.push_back(service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
            [&](ScreenshotOcrRecognitionResult) {
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
    const auto token = service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult) { completed = true; });
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
        const auto token = service->recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
            [&](ScreenshotOcrRecognitionResult) { ++completions; });
        require(token != 0, "requests queued before service shutdown should be accepted");
    }

    require(waitUntil([&]() { return service->liveWorkerCount() > 0; }, kRecognitionTimeoutMs),
            "at least one OCR worker should initialize before shutdown");
    const int completionsBeforeDestruction = completions;
    service.reset();
    processEventsFor(250);
    require(completions == completionsBeforeDestruction,
            "destroyed OCR services must not deliver queued completions");
}

QJsonObject assetFile(const QString& name, const QByteArray& contents,
                      const QString& url = {}) {
    QJsonObject result{{QStringLiteral("name"), name},
                       {QStringLiteral("size"), contents.size()},
                       {QStringLiteral("sha256"),
                        QString::fromLatin1(QCryptographicHash::hash(
                                                contents, QCryptographicHash::Sha256)
                                                .toHex())}};
    if (!url.isEmpty()) result.insert(QStringLiteral("url"), url);
    return result;
}

void writeFixture(const QString& path, const QByteArray& contents) {
    require(QDir().mkpath(QFileInfo(path).dir().absolutePath()),
            "OCR asset fixture directory should be writable");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "OCR asset fixture should be writable");
    require(file.write(contents) == contents.size(), "OCR asset fixture write should complete");
}

void writeAssetManifest(const QString& root, bool completePayload) {
    const QByteArray process("process");
    const QByteArray directMl("directml");
    const QByteArray runtimeManifest("runtime");
    const QByteArray detector("detector");
    const QByteArray recognizer("recognizer");
    const QByteArray dictionary("dictionary");
    const QString runtimeDirectory = QDir(root).filePath(QStringLiteral("runtimes/1.0.0/windows-x64"));
    const QString modelDirectory = QDir(root).filePath(QStringLiteral("models/ppocrv6-small-463ea9f"));
    if (completePayload) {
        writeFixture(QDir(runtimeDirectory).filePath(
                         QStringLiteral("snow-ocr-process-1.0.0-windows-x64.exe")), process);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("DirectML.dll")), directMl);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("runtime-manifest.json")),
                     runtimeManifest);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_det_small.onnx")),
                     detector);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_rec_small.onnx")),
                     recognizer);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("ppocrv6_dict.txt")), dictionary);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"1.0.0"})");
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"ppocrv6-small-463ea9f"})");
    }
    const QJsonArray runtimeFiles{
        assetFile(QStringLiteral("snow-ocr-process-1.0.0-windows-x64.exe"), process),
        assetFile(QStringLiteral("DirectML.dll"), directMl),
        assetFile(QStringLiteral("runtime-manifest.json"), runtimeManifest)};
    const QJsonArray modelFiles{
        assetFile(QStringLiteral("PP-OCRv6_det_small.onnx"), detector,
                  QStringLiteral("https://example.invalid/det")),
        assetFile(QStringLiteral("PP-OCRv6_rec_small.onnx"), recognizer,
                  QStringLiteral("https://example.invalid/rec")),
        assetFile(QStringLiteral("ppocrv6_dict.txt"), dictionary,
                  QStringLiteral("https://example.invalid/dict"))};
    const QByteArray archive("archive");
    const QJsonObject manifest{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0.0")},
                     {QStringLiteral("platform"), QStringLiteral("windows-x64")},
                     {QStringLiteral("archive"),
                      assetFile(QStringLiteral("snow-ocr-runtime-1.0.0-windows-x64.zip"),
                                archive, QStringLiteral("https://example.invalid/runtime"))},
                     {QStringLiteral("files"), runtimeFiles}}},
        {QStringLiteral("model"),
         QJsonObject{{QStringLiteral("id"), QStringLiteral("ppocrv6-small-463ea9f")},
                     {QStringLiteral("files"), modelFiles}}}};
    writeFixture(QDir(root).filePath(QStringLiteral("asset-manifest.json")),
                 QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

void validOfflineAssetsAreSelectedWithoutNetwork() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR asset roots should be available");
    writeAssetManifest(offline.path(), true);
    int downloads = 0;
    bool ready = false;
    bool failed = false;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.cacheRoot = cache.path();
    options.downloadOverride = [&](const QString&, const QString&, QString*) {
        ++downloads;
        return false;
    };
    ScreenshotOcrAssets assets(options);
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         ready = result.offline && result.valid();
                     });
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                     [&](const QString&) { failed = true; });
    assets.prepare();
    require(waitUntil([&]() { return ready || failed; }, 5'000),
            "offline OCR asset validation should complete");
    require(ready && !failed, "a complete hash-valid offline payload should be selected");
    require(downloads == 0, "valid offline OCR assets must not use the network");
}

void incompleteOfflineAssetsFallBackToOnlineAcquisition() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR fallback roots should be available");
    writeAssetManifest(offline.path(), false);
    int downloads = 0;
    bool finished = false;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.cacheRoot = cache.path();
    options.downloadOverride = [&](const QString&, const QString&, QString* error) {
        ++downloads;
        *error = QStringLiteral("fixture download stopped");
        return false;
    };
    ScreenshotOcrAssets assets(options);
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                     [&](const QString&) { finished = true; });
    assets.prepare();
    require(waitUntil([&]() { return finished; }, 5'000),
            "invalid offline OCR assets should attempt online acquisition");
    require(downloads == 1, "incomplete offline assets must enter online acquisition once");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const bool directMlRequested = application.arguments().contains(QStringLiteral("--directml"));
    validOfflineAssetsAreSelectedWithoutNetwork();
    incompleteOfflineAssetsFallBackToOnlineAcquisition();
    explicitAssetsControlReadiness();
    renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine();
    diskBackedEngineCompletesThroughTheQtWorker(directMlRequested);
    if (!directMlRequested) {
        recognitionRenderIntentCanChangeWhileQueued();
        concurrentRequestsCompleteExactlyOnce();
        interactiveRequestsPrecedeQueuedPrefetch();
        queuedCancellationSkipsExecution();
        workerRecyclesImmediatelyAndCanBeRecreated();
        cancellationSuppressesCompletion();
        receiverDestructionSuppressesCompletion();
        serviceDestructionJoinsWorkersAndSuppressesLateDelivery();
    }
    return 0;
}
