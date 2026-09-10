#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/diagnostics/diagnostics.h"

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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QSemaphore>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr int kRecognitionTimeoutMs = 30'000;

QString sourceRuntimeDirectory;

ScreenshotOcrRecognitionService::Options sourceRuntimeOptions() {
    require(!sourceRuntimeDirectory.isEmpty(), "the source OCR runtime must be staged first");
    const QDir models(QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("assets/ocr/models/ppocrv6-small-463ea9f")));
    ScreenshotOcrRecognitionService::Options options;
    options.processPath =
        QDir(sourceRuntimeDirectory).filePath(QStringLiteral("snow-ocr-process.exe"));
    options.detectorModelPath = models.filePath(QStringLiteral("PP-OCRv6_det_small.onnx"));
    options.recognizerModelPath = models.filePath(QStringLiteral("PP-OCRv6_rec_small.onnx"));
    options.dictionaryPath = models.filePath(QStringLiteral("ppocrv6_dict.txt"));
    options.stateDirectory = sourceRuntimeDirectory;
    return options;
}

void stageSourceRuntime(const QString& directory) {
    sourceRuntimeDirectory = directory;
    const QDir destination(directory);
    QString executable = QStringLiteral(SNOW_TEST_OCR_EXECUTABLE);
    for (const QString& argument : QCoreApplication::arguments()) {
        if (argument.startsWith(QStringLiteral("--worker="))) {
            executable = QFileInfo(argument.mid(9)).absoluteFilePath();
        }
    }
    require(QFile::copy(executable, destination.filePath(QStringLiteral("snow-ocr-process.exe"))),
            "the source OCR worker must be copied from the build target into the test fixture");
    const QDir application(QCoreApplication::applicationDirPath());
    // The isolated worker still needs the transitive DLLs staged beside the
    // test executable (for example, the dynamic ONNX Runtime dependencies).
    require(qputenv("PATH", QFile::encodeName(application.absolutePath()) + ';' + qgetenv("PATH")),
            "the source OCR worker must inherit the staged runtime dependency directory");
    for (const QString& library :
         {QStringLiteral("DirectML.dll"), QStringLiteral("onnxruntime.dll")}) {
        const QString source = application.filePath(library);
        if (QFileInfo::exists(source)) {
            require(QFile::copy(source, destination.filePath(library)),
                    "the source OCR runtime dependencies must be copied into the fixture");
        }
    }
}

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
    require(service.modelFilesReady(), "a complete explicit OCR asset set must report ready");
}

void modelInitializationFailureExposesAssetErrorAndRetries() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary OCR initialization directory should be available");
    ScreenshotOcrRecognitionService::Options options;
    options.processPath = sourceRuntimeOptions().processPath;
    options.detectorModelPath = QDir(directory.path()).filePath(QStringLiteral("invalid-det.onnx"));
    options.recognizerModelPath =
        QDir(directory.path()).filePath(QStringLiteral("invalid-rec.onnx"));
    options.dictionaryPath = QDir(directory.path()).filePath(QStringLiteral("invalid-dict.txt"));
    options.stateDirectory = QDir(directory.path()).filePath(QStringLiteral("state"));
    require(QFileInfo(options.processPath).isFile() && QDir().mkpath(options.stateDirectory),
            "the staged OCR runtime and temporary state directory should be available");
    for (const QString& path :
         {options.detectorModelPath, options.recognizerModelPath, options.dictionaryPath}) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly),
                "invalid OCR initialization fixture should be writable");
        require(file.write(QByteArrayLiteral("invalid model fixture")) > 0,
                "invalid OCR initialization fixture write should complete");
    }

    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    const QImage image = whiteImage();
    int completions = 0;
    const auto submit = [&]() {
        const auto token = service.recognize(
            ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
            [&](ScreenshotOcrRecognitionResult result) {
                require(!result.error.isEmpty(),
                        "invalid selected model initialization should fail recognition");
                ++completions;
            });
        require(token != 0, "an initialization-failure request should be accepted");
    };

    submit();
    require(waitUntil([&]() { return completions == 1; }, 10'000) &&
                service.assetStatus().phase == ScreenshotOcrAssetPhase::Failed,
            "model initialization failure should expose the OCR asset error state");
    submit();
    require(
        waitUntil([&]() { return completions == 2; }, 10'000) &&
            service.assetStatus().phase == ScreenshotOcrAssetPhase::Failed,
        "a later OCR request should retry and report the selected model initialization failure");
}

void oneEngineIsInitializedBeforeReadyAndReused() {
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory;
    DiagnosticsOptions logging;
    logging.directories = {directory.path()};
    logging.enableCrashCapture = false;
    logging.mirrorToConsole = false;
    auto& diagnostics = DiagnosticsService::instance();
    require(diagnostics.initialize(logging), "engine reuse diagnostics must initialize");
    {
        ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
        QObject receiver;
        const QImage image = whiteImage();
        int completions = 0;
        const auto request =
            ScreenshotOcrRequest{image, QRectF(0, 0, image.width(), image.height())};
        service.recognize(request, &receiver, [&](ScreenshotOcrRecognitionResult first) {
            require(first.error.isEmpty(), "first initialized-engine request must succeed");
            ++completions;
            service.recognize(request, &receiver, [&](ScreenshotOcrRecognitionResult second) {
                require(second.error.isEmpty(), "reused-engine request must succeed");
                ++completions;
            });
        });
        require(waitUntil([&] { return completions == 2 && service.processId() == 0; },
                          kRecognitionTimeoutMs),
                "both requests must finish and release the engine");
        require(diagnostics.flush(), "engine reuse diagnostics must flush");
        QFile log(diagnostics.status().currentFile);
        require(log.open(QIODevice::ReadOnly), "engine reuse log must be readable");
        int starts = 0, initializations = 0, workers = 0;
        int readyCount = 0;
        for (const auto& line : log.readAll().split('\n')) {
            const auto record = QJsonDocument::fromJson(line).object();
            const auto event = record.value(QStringLiteral("event")).toString();
            const auto fields = record.value(QStringLiteral("fields")).toObject();
            if (event == QStringLiteral("ocr.process_started"))
                ++starts;
            if (event == QStringLiteral("ocr.engine_ready")) {
                ++initializations;
                require(fields.value(QStringLiteral("operation")) == QStringLiteral("0") &&
                            fields.value(QStringLiteral("outcome")) == QStringLiteral("succeeded"),
                        "the real engine must initialize during startup");
            }
            if (event == QStringLiteral("ocr.process_ready"))
                ++readyCount;
            if (event == QStringLiteral("ocr.worker_finished")) {
                ++workers;
                require(fields.value(QStringLiteral("initialization_ms")).toInteger() == 0,
                        "requests must reuse the initialized engine");
            }
        }
        require(starts == 1 && initializations == 1 && workers == 2 && readyCount == 1,
                "one worker engine must serve both requests after successful readiness");
    }
    diagnostics.shutdown();
}

void diskBackedEngineCompletesThroughTheQtWorker(bool directMlEnabled,
                                                 bool managedRuntime = false) {
    QTemporaryDir cache;
    require(cache.isValid(), "an isolated OCR cache is required");
    auto options =
        managedRuntime ? ScreenshotOcrRecognitionService::Options{} : sourceRuntimeOptions();
    const QString expectedProcess =
        managedRuntime ? QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("assets/ocr/runtimes/1.0.6/windows-x64/"
                                                      "snow-ocr-process-1.0.6-windows-x64.exe"))
                       : options.processPath;
    if (managedRuntime) {
        // Exercise the same trusted offline selection as the app, without a
        // pre-existing user cache or a successful network fallback.
        options.cacheRoot = cache.path();
        options.proxyUrl = QStringLiteral("http://127.0.0.1:1");
        require(QFileInfo::exists(expectedProcess), "the managed OCR runtime must be staged");
    }
    ScreenshotOcrRecognitionService service(options, directMlEnabled
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
                              require(service.processId() != 0 &&
                                          QFileInfo(service.processPath()).canonicalFilePath() ==
                                              QFileInfo(expectedProcess).canonicalFilePath(),
                                      "OCR integration must execute the selected runtime");
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
    if (managedRuntime) {
        require(service.assetStatus().phase == ScreenshotOcrAssetPhase::ReadyOffline,
                "managed OCR must use the verified offline payload without downloading");
    }
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
    const auto token =
        service.render(std::move(request), &receiver, [&](ScreenshotOcrRecognitionResult result) {
            output = std::move(result);
            completed = true;
        });
    require(token != 0, "a valid render-only OCR request should be accepted");
    require(waitUntil([&]() { return completed; }, kRecognitionTimeoutMs),
            "render-only OCR work should complete on the worker");
    require(output.error.isEmpty() && output.presentation == nullptr &&
                !output.filteredImage.isNull(),
            "render-only OCR work should return only its transient filtered image");
    require(output.filteredImageCanvasRect.isValid() && !output.filteredImageCanvasRect.isEmpty() &&
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
    auto options = sourceRuntimeOptions();
    options.workerCount = 1;
    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    const QImage blocker = whiteImage(768);
    bool blockerCompleted = false;
    const auto blockerToken =
        service.recognize(ScreenshotOcrRequest{blocker, QRectF(QPointF(), QSizeF(blocker.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
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
    const auto promotedToken = service.recognize(std::move(promotedRequest), &receiver,
                                                 [&](ScreenshotOcrRecognitionResult result) {
                                                     promotedOutput = std::move(result);
                                                     promotedCompleted = true;
                                                 });
    require(promotedToken != 0 &&
                service.setRenderFilteredImage(promotedToken, true, QColor(Qt::white)),
            "a queued prefetch should accept interactive render promotion");
    require(
        waitUntil([&]() { return blockerCompleted && promotedCompleted; }, kRecognitionTimeoutMs),
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
    const auto suppressedToken = service.recognize(std::move(suppressedRequest), &receiver,
                                                   [&](ScreenshotOcrRecognitionResult result) {
                                                       suppressedOutput = std::move(result);
                                                       suppressedCompleted = true;
                                                   });
    require(suppressedToken != 0 && service.setRenderFilteredImage(suppressedToken, false),
            "an abandoned recognition should accept render suppression");
    require(waitUntil([&]() { return suppressedCompleted; }, kRecognitionTimeoutMs),
            "render-suppressed recognition should still complete and remain cacheable");
    require(suppressedOutput.error.isEmpty() && suppressedOutput.presentation != nullptr &&
                suppressedOutput.filteredImage.isNull(),
            "render suppression should preserve OCR output without retaining filtered pixels");
}

void concurrentRequestsCompleteExactlyOnce() {
    constexpr int kRequestCount = 3;
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
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

    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "OCR workers should exit once a concurrent burst is drained");

    outputs.clear();
}

void interactiveRequestsPrecedeQueuedPrefetch() {
    auto options = sourceRuntimeOptions();
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
            &receiver, [&, id](ScreenshotOcrRecognitionResult result) {
                require(result.error.isEmpty() && result.presentation != nullptr,
                        "priority test OCR requests should succeed");
                completionOrder.push_back(id);
                if (completionOrder.size() == 3) {
                    loop.quit();
                }
            });
        require(token != 0, "priority test OCR requests should be accepted");
    };

    // Cold ONNX startup can exceed the scheduler's prefetch-aging threshold.
    // Queue the priority comparison while a warmed child is still alive.
    const QImage warmup = whiteImage(512);
    service.recognize(ScreenshotOcrRequest{warmup, QRectF(QPointF(), QSizeF(warmup.size()))},
                      &receiver, [&](ScreenshotOcrRecognitionResult result) {
                          require(result.error.isEmpty(), "priority-test engine warms up");
                          submit(0, 512, ScreenshotOcrRequestPriority::Interactive);
                          submit(1, 64, ScreenshotOcrRequestPriority::Prefetch);
                          submit(2, 64, ScreenshotOcrRequestPriority::Interactive);
                      });

    timeout.start(kRecognitionTimeoutMs);
    loop.exec();
    require(!timedOut, "priority test OCR requests should finish within the timeout");
    require(completionOrder == std::vector<int>({0, 2, 1}),
            "queued interactive OCR must run before queued prefetch OCR");
}

void workerRecyclesImmediatelyAndCanBeRecreated() {
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
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
    const auto token =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
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
    const auto secondToken =
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
                              recreated = result.presentation != nullptr && result.error.isEmpty();
                          });
    require(secondToken != 0, "a request after immediate recycling should be accepted");
    require(waitUntil([&]() { return recreated; }, kRecognitionTimeoutMs),
            "a request after immediate recycling should recreate the OCR engine");
    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "the recreated OCR worker should exit after completing the request");
}

void modelChangeDrainsSubmittedWorkBeforeRestartingPendingWork() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary OCR model-change directory should be available");
    const QDir assetRoot(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/ocr")));
    auto options = sourceRuntimeOptions();
    options.workerCount = 1;
    options.processPath = sourceRuntimeOptions().processPath;
    options.detectorModelPath =
        assetRoot.filePath(QStringLiteral("models/ppocrv6-small-463ea9f/PP-OCRv6_det_small.onnx"));
    options.recognizerModelPath =
        assetRoot.filePath(QStringLiteral("models/ppocrv6-small-463ea9f/PP-OCRv6_rec_small.onnx"));
    options.dictionaryPath =
        assetRoot.filePath(QStringLiteral("models/ppocrv6-small-463ea9f/ppocrv6_dict.txt"));
    options.stateDirectory = QDir(directory.path()).filePath(QStringLiteral("state"));
    require(QFileInfo(options.processPath).isFile() &&
                QFileInfo(options.detectorModelPath).isFile() &&
                QFileInfo(options.recognizerModelPath).isFile() &&
                QFileInfo(options.dictionaryPath).isFile() && QDir().mkpath(options.stateDirectory),
            "the packaged explicit OCR assets should be available for model-change testing");

    ScreenshotOcrRecognitionService service(options);
    QObject receiver;
    std::vector<int> completionOrder;
    qint64 originalProcessId = 0;
    qint64 replacementProcessId = 0;
    bool workScheduled = false;
    const QImage warmup = whiteImage(768);
    const auto warmupToken = service.recognize(
        ScreenshotOcrRequest{warmup, QRectF(QPointF(), QSizeF(warmup.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult result) {
            require(result.error.isEmpty() && result.presentation != nullptr,
                    "the model-change OCR child should warm successfully");
            require(service.processId() != 0, "the warmed OCR child should still be running");
            originalProcessId = service.processId();

            const QImage submitted = whiteImage(768);
            const auto submittedToken = service.recognize(
                ScreenshotOcrRequest{submitted, QRectF(QPointF(), QSizeF(submitted.size()))},
                &receiver, [&](ScreenshotOcrRecognitionResult submittedResult) {
                    require(submittedResult.error.isEmpty() &&
                                submittedResult.presentation != nullptr,
                            "work submitted before a model change should finish successfully");
                    completionOrder.push_back(1);
                });
            require(submittedToken != 0, "pre-change OCR work should be submitted");

            service.setModelType(ScreenshotOcrModelType::Medium);
            const QImage pending = whiteImage();
            const auto pendingToken = service.recognize(
                ScreenshotOcrRequest{pending, QRectF(QPointF(), QSizeF(pending.size()))}, &receiver,
                [&](ScreenshotOcrRecognitionResult pendingResult) {
                    require(pendingResult.error.isEmpty() && pendingResult.presentation != nullptr,
                            "work queued after a model change should finish successfully");
                    require(service.processId() != 0,
                            "pending OCR work should run in a replacement child");
                    replacementProcessId = service.processId();
                    completionOrder.push_back(2);
                });
            require(pendingToken != 0, "post-change OCR work should remain queued");
            workScheduled = true;
        });
    require(warmupToken != 0, "the model-change warmup should be accepted");
    require(waitUntil([&]() { return workScheduled && completionOrder.size() == 2; },
                      kRecognitionTimeoutMs),
            "submitted and pending OCR work should complete across the model change");
    require(completionOrder == std::vector<int>({1, 2}) && originalProcessId > 0 &&
                replacementProcessId > 0 && replacementProcessId != originalProcessId,
            "a model change must drain submitted work before starting pending work in a new child");
}

void queuedCancellationSkipsExecution() {
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
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
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
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
    require(waitUntil([&]() { return service.liveWorkerCount() == 0; }, 1'000),
            "canceling the only OCR request must retire the child process");
}

void receiverDestructionSuppressesCompletion() {
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
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
    auto service = std::make_unique<ScreenshotOcrRecognitionService>(sourceRuntimeOptions());
    for (int index = 0; index < 3; ++index) {
        const QImage image = whiteImage(256);
        const auto token =
            service->recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                               &receiver, [&](ScreenshotOcrRecognitionResult) { ++completions; });
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

QJsonObject assetFile(const QString& name, const QByteArray& contents, const QString& url = {}) {
    QJsonObject result{
        {QStringLiteral("name"), name},
        {QStringLiteral("size"), contents.size()},
        {QStringLiteral("sha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex())}};
    if (!url.isEmpty())
        result.insert(QStringLiteral("url"), url);
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
    const QString runtimeDirectory =
        QDir(root).filePath(QStringLiteral("runtimes/1.0.6/windows-x64"));
    const QString modelDirectory =
        QDir(root).filePath(QStringLiteral("models/ppocrv6-small-463ea9f"));
    if (completePayload) {
        writeFixture(QDir(runtimeDirectory)
                         .filePath(QStringLiteral("snow-ocr-process-1.0.6-windows-x64.exe")),
                     process);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("DirectML.dll")), directMl);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral("runtime-manifest.json")),
                     runtimeManifest);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_det_small.onnx")),
                     detector);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("PP-OCRv6_rec_small.onnx")),
                     recognizer);
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral("ppocrv6_dict.txt")), dictionary);
        writeFixture(QDir(runtimeDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"1.0.6"})");
        writeFixture(QDir(modelDirectory).filePath(QStringLiteral(".complete.json")),
                     R"({"schema":1,"component":"ppocrv6-small-463ea9f"})");
    }
    const QJsonArray runtimeFiles{
        assetFile(QStringLiteral("snow-ocr-process-1.0.6-windows-x64.exe"), process),
        assetFile(QStringLiteral("DirectML.dll"), directMl),
        assetFile(QStringLiteral("runtime-manifest.json"), runtimeManifest)};
    const auto model = [](const QString& type, const QString& id, const QString& detectorName,
                          const QByteArray& detectorContents, const QString& recognizerName,
                          const QByteArray& recognizerContents, const QString& dictionaryName,
                          const QByteArray& dictionaryContents) {
        return QJsonObject{
            {QStringLiteral("type"), type},
            {QStringLiteral("id"), id},
            {QStringLiteral("detector"), detectorName},
            {QStringLiteral("recognizer"), recognizerName},
            {QStringLiteral("dictionary"), dictionaryName},
            {QStringLiteral("files"),
             QJsonArray{
                 assetFile(detectorName, detectorContents,
                           QStringLiteral("https://example.invalid/") + detectorName),
                 assetFile(recognizerName, recognizerContents,
                           QStringLiteral("https://example.invalid/") + recognizerName),
                 assetFile(dictionaryName, dictionaryContents,
                           QStringLiteral("https://example.invalid/") + dictionaryName),
             }},
        };
    };
    const QByteArray archive("archive");
    const QJsonObject manifest{
        {QStringLiteral("schema"), 2},
        {QStringLiteral("default_model"), QStringLiteral("small")},
        {QStringLiteral("runtime"),
         QJsonObject{{QStringLiteral("version"), QStringLiteral("1.0.6")},
                     {QStringLiteral("platform"), QStringLiteral("windows-x64")},
                     {QStringLiteral("archive"),
                      assetFile(QStringLiteral("snow-ocr-runtime-1.0.6-windows-x64.zip"), archive,
                                QStringLiteral("https://example.invalid/runtime"))},
                     {QStringLiteral("files"), runtimeFiles}}},
        {QStringLiteral("models"),
         QJsonArray{
             model(QStringLiteral("extra_small"), QStringLiteral("ppocrv6-tiny-cd609a1"),
                   QStringLiteral("PP-OCRv6_det_tiny.onnx"), QByteArray("tiny-detector"),
                   QStringLiteral("PP-OCRv6_rec_tiny.onnx"), QByteArray("tiny-recognizer"),
                   QStringLiteral("ppocrv6_tiny_dict.txt"), QByteArray("tiny-dictionary")),
             model(QStringLiteral("small"), QStringLiteral("ppocrv6-small-463ea9f"),
                   QStringLiteral("PP-OCRv6_det_small.onnx"), detector,
                   QStringLiteral("PP-OCRv6_rec_small.onnx"), recognizer,
                   QStringLiteral("ppocrv6_dict.txt"), dictionary),
             model(QStringLiteral("medium"), QStringLiteral("ppocrv6-medium-f5063c6"),
                   QStringLiteral("PP-OCRv6_det_medium.onnx"), QByteArray("medium-detector"),
                   QStringLiteral("PP-OCRv6_rec_medium.onnx"), QByteArray("medium-recognizer"),
                   QStringLiteral("ppocrv6_dict.txt"), dictionary),
             model(QStringLiteral("small_v5"), QStringLiteral("ppocrv5-small-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv5_det_mobile.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv5_rec_mobile.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocrv5_dict.txt"), dictionary),
             model(QStringLiteral("medium_v5"), QStringLiteral("ppocrv5-medium-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv5_det_server.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv5_rec_server.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocrv5_dict.txt"), dictionary),
             model(QStringLiteral("small_v4"), QStringLiteral("ppocrv4-small-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv4_det_mobile.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv4_rec_mobile.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocr_keys_v1.txt"), dictionary),
             model(QStringLiteral("medium_v4"), QStringLiteral("ppocrv4-medium-7b2a75a"),
                   QStringLiteral("ch_PP-OCRv4_det_server.onnx"), QByteArray("detector"),
                   QStringLiteral("ch_PP-OCRv4_rec_server.onnx"), QByteArray("recognizer"),
                   QStringLiteral("ppocr_keys_v1.txt"), dictionary),
         }}};
    writeFixture(QDir(root).filePath(QStringLiteral("asset-manifest.json")),
                 QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

QByteArray modelFixtureContents(const QString& name) {
    if (name.startsWith(QStringLiteral("ch_PP-OCRv")))
        return name.contains(QStringLiteral("_det_")) ? QByteArray("detector")
                                                      : QByteArray("recognizer");
    if (name == QStringLiteral("ppocrv5_dict.txt") || name == QStringLiteral("ppocr_keys_v1.txt"))
        return QByteArray("dictionary");
    if (name == QStringLiteral("PP-OCRv6_det_tiny.onnx"))
        return QByteArray("tiny-detector");
    if (name == QStringLiteral("PP-OCRv6_rec_tiny.onnx"))
        return QByteArray("tiny-recognizer");
    if (name == QStringLiteral("ppocrv6_tiny_dict.txt"))
        return QByteArray("tiny-dictionary");
    if (name == QStringLiteral("PP-OCRv6_det_small.onnx"))
        return QByteArray("detector");
    if (name == QStringLiteral("PP-OCRv6_rec_small.onnx"))
        return QByteArray("recognizer");
    if (name == QStringLiteral("PP-OCRv6_det_medium.onnx"))
        return QByteArray("medium-detector");
    if (name == QStringLiteral("PP-OCRv6_rec_medium.onnx"))
        return QByteArray("medium-recognizer");
    if (name == QStringLiteral("ppocrv6_dict.txt"))
        return QByteArray("dictionary");
    return {};
}

bool writeDownloadedModelFixture(const QString& destination, QString* error) {
    const QByteArray contents = modelFixtureContents(QFileInfo(destination).fileName());
    if (contents.isEmpty()) {
        *error = QStringLiteral("unexpected fixture download");
        return false;
    }
    writeFixture(destination, contents);
    return true;
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
    require(offline.isValid() && cache.isValid(),
            "temporary OCR fallback roots should be available");
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

void everyModelMapsToItsOwnFilesAndCachesAreRetained() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR model roots should be available");
    writeAssetManifest(offline.path(), true);
    QSet<QString> downloads;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::Medium;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        downloads.insert(QFileInfo(destination).fileName());
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    ScreenshotOcrResolvedAssets resolved;
    int readyCount = 0;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ++readyCount;
                     });
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 1; }, 5'000),
            "the Medium model fixture should be acquired");
    require(resolved.modelType == ScreenshotOcrModelType::Medium &&
                resolved.modelId == QStringLiteral("ppocrv6-medium-f5063c6") &&
                resolved.detectorModelPath.endsWith(QStringLiteral("PP-OCRv6_det_medium.onnx")) &&
                resolved.recognizerModelPath.endsWith(QStringLiteral("PP-OCRv6_rec_medium.onnx")) &&
                resolved.dictionaryPath.endsWith(QStringLiteral("ppocrv6_dict.txt")) &&
                !resolved.offline,
            "Medium must combine the bundled runtime with only its cached model files");
    require(downloads == QSet<QString>{QStringLiteral("PP-OCRv6_det_medium.onnx"),
                                       QStringLiteral("PP-OCRv6_rec_medium.onnx"),
                                       QStringLiteral("ppocrv6_dict.txt")},
            "Medium acquisition must download exactly its three role files");

    downloads.clear();
    assets.setModelType(ScreenshotOcrModelType::ExtraSmall);
    processEventsFor(100);
    require(downloads.isEmpty(),
            "changing the OCR model selection must not start acquisition without demand");
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 2; }, 5'000),
            "the Extra Small model fixture should be acquired");
    require(resolved.modelType == ScreenshotOcrModelType::ExtraSmall &&
                resolved.modelId == QStringLiteral("ppocrv6-tiny-cd609a1") &&
                resolved.detectorModelPath.endsWith(QStringLiteral("PP-OCRv6_det_tiny.onnx")) &&
                resolved.recognizerModelPath.endsWith(QStringLiteral("PP-OCRv6_rec_tiny.onnx")) &&
                resolved.dictionaryPath.endsWith(QStringLiteral("ppocrv6_tiny_dict.txt")),
            "Extra Small must resolve its own detector, recognizer, and dictionary");
    require(
        QDir(cache.path()).exists(QStringLiteral("models/ppocrv6-medium-f5063c6/.complete.json")) &&
            QDir(cache.path()).exists(QStringLiteral("models/ppocrv6-tiny-cd609a1/.complete.json")),
        "switching models must retain every manifest-approved cached model");

    downloads.clear();
    assets.setModelType(ScreenshotOcrModelType::Small);
    assets.prepare();
    require(waitUntil([&]() { return readyCount == 3; }, 5'000),
            "the bundled Small model should be selected");
    require(resolved.modelType == ScreenshotOcrModelType::Small && resolved.offline &&
                downloads.isEmpty(),
            "Small must continue to resolve entirely offline without downloading");
    struct ExpectedModel {
        ScreenshotOcrModelType type;
        const char* value;
        const char* id;
        const char* detector;
        const char* recognizer;
        const char* dictionary;
    };
    const ExpectedModel versionedModels[] = {
        {ScreenshotOcrModelType::SmallV5, "small_v5", "ppocrv5-small-7b2a75a",
         "ch_PP-OCRv5_det_mobile.onnx", "ch_PP-OCRv5_rec_mobile.onnx", "ppocrv5_dict.txt"},
        {ScreenshotOcrModelType::MediumV5, "medium_v5", "ppocrv5-medium-7b2a75a",
         "ch_PP-OCRv5_det_server.onnx", "ch_PP-OCRv5_rec_server.onnx", "ppocrv5_dict.txt"},
        {ScreenshotOcrModelType::SmallV4, "small_v4", "ppocrv4-small-7b2a75a",
         "ch_PP-OCRv4_det_mobile.onnx", "ch_PP-OCRv4_rec_mobile.onnx", "ppocr_keys_v1.txt"},
        {ScreenshotOcrModelType::MediumV4, "medium_v4", "ppocrv4-medium-7b2a75a",
         "ch_PP-OCRv4_det_server.onnx", "ch_PP-OCRv4_rec_server.onnx", "ppocr_keys_v1.txt"},
    };
    for (const auto& expected : versionedModels) {
        downloads.clear();
        assets.setModelType(expected.type);
        const int previous = readyCount;
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 1; }, 5'000),
                "versioned model acquisition completes");
        const QString detector = QString::fromLatin1(expected.detector);
        const QString recognizer = QString::fromLatin1(expected.recognizer);
        const QString dictionary = QString::fromLatin1(expected.dictionary);
        require(resolved.modelType == expected.type &&
                    resolved.modelId == QString::fromLatin1(expected.id) &&
                    resolved.detectorModelPath.endsWith(detector) &&
                    resolved.recognizerModelPath.endsWith(recognizer) &&
                    resolved.dictionaryPath.endsWith(dictionary) &&
                    downloads == QSet<QString>{detector, recognizer, dictionary},
                "versioned model downloads and resolves exactly its matching role files");
        require(screenshotOcrModelTypeValue(expected.type) == QString::fromLatin1(expected.value) &&
                    screenshotOcrModelTypeFromValue(QString::fromLatin1(expected.value)) ==
                        expected.type,
                "versioned model identifiers round trip");
        downloads.clear();
        assets.setModelType(ScreenshotOcrModelType::Small);
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 2; }, 5'000),
                "switching to bundled V6 succeeds");
        assets.setModelType(expected.type);
        assets.prepare();
        require(waitUntil([&]() { return readyCount == previous + 3; }, 5'000) &&
                    downloads.isEmpty(),
                "switching back reuses the versioned model cache without downloading");
    }
}

void selectedModelFailureNeverFallsBackToSmallAndCanRetry() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(),
            "temporary OCR failure roots should be available");
    writeAssetManifest(offline.path(), true);
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::MediumV4;
    bool allowDownload = false;
    int downloadAttempts = 0;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        ++downloadAttempts;
        if (!allowDownload) {
            *error = QStringLiteral("selected model unavailable");
            return false;
        }
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    bool ready = false;
    int failureCount = 0;
    ScreenshotOcrResolvedAssets resolved;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ready = true;
                     });
    QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets, [&](const QString&) {
        ++failureCount;
        if (failureCount == 1) {
            allowDownload = true;
            assets.prepare();
        }
    });
    assets.prepare();
    require(waitUntil([&]() { return ready; }, 5'000),
            "a request arriving from the failure callback should retry model acquisition");
    require(failureCount == 1 && resolved.modelType == ScreenshotOcrModelType::MediumV4 &&
                !resolved.offline && downloadAttempts == 4,
            "a failed Medium acquisition must retry safely without falling back to bundled Small");
}

void invalidSchemaTwoManifestsAreRejectedBeforeDownloading() {
    const auto rejectMutation = [](const std::function<void(QJsonObject*)>& mutate) {
        QTemporaryDir offline;
        QTemporaryDir cache;
        require(offline.isValid() && cache.isValid(),
                "temporary invalid-manifest roots should be available");
        writeAssetManifest(offline.path(), false);
        const QString manifestPath =
            QDir(offline.path()).filePath(QStringLiteral("asset-manifest.json"));
        QFile input(manifestPath);
        require(input.open(QIODevice::ReadOnly), "valid fixture manifest should be readable");
        QJsonObject manifest = QJsonDocument::fromJson(input.readAll()).object();
        input.close();
        mutate(&manifest);
        writeFixture(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        int downloads = 0;
        bool failed = false;
        ScreenshotOcrAssets::Options options;
        options.offlineRoot = offline.path();
        options.cacheRoot = cache.path();
        options.downloadOverride = [&](const QString&, const QString&, QString*) {
            ++downloads;
            return false;
        };
        ScreenshotOcrAssets assets(options);
        QObject::connect(&assets, &ScreenshotOcrAssets::failed, &assets,
                         [&](const QString&) { failed = true; });
        assets.prepare();
        require(waitUntil([&]() { return failed; }, 5'000),
                "invalid schema-2 manifest should be rejected");
        require(downloads == 0, "an invalid trusted manifest must never initiate downloads");
    };

    rejectMutation([](QJsonObject* manifest) { manifest->insert(QStringLiteral("schema"), 1); });
    rejectMutation([](QJsonObject* manifest) {
        manifest->insert(QStringLiteral("default_model"), QStringLiteral("medium"));
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        models.removeAt(2);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("type"), QStringLiteral("small"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("id"), QStringLiteral("ppocrv6-tiny-cd609a1"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject medium = models.at(2).toObject();
        medium.insert(QStringLiteral("type"), QStringLiteral("large"));
        models.replace(2, medium);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        smallModel.insert(QStringLiteral("detector"), QStringLiteral("missing.onnx"));
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject recognizer = files.at(1).toObject();
        recognizer.insert(QStringLiteral("name"),
                          files.at(0).toObject().value(QStringLiteral("name")));
        files.replace(1, recognizer);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("size"), 0);
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("sha256"), QStringLiteral("not-a-sha256"));
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
    rejectMutation([](QJsonObject* manifest) {
        QJsonArray models = manifest->value(QStringLiteral("models")).toArray();
        QJsonObject smallModel = models.at(1).toObject();
        QJsonArray files = smallModel.value(QStringLiteral("files")).toArray();
        QJsonObject detector = files.at(0).toObject();
        detector.insert(QStringLiteral("url"), QStringLiteral("http://example.invalid/model"));
        files.replace(0, detector);
        smallModel.insert(QStringLiteral("files"), files);
        models.replace(1, smallModel);
        manifest->insert(QStringLiteral("models"), models);
    });
}

void modelSelectionDuringAcquisitionIsLastSelectionWins() {
    QTemporaryDir offline;
    QTemporaryDir cache;
    require(offline.isValid() && cache.isValid(), "temporary OCR race roots should be available");
    writeAssetManifest(offline.path(), true);
    QSemaphore mediumStarted;
    QSemaphore releaseMedium;
    ScreenshotOcrAssets::Options options;
    options.offlineRoot = offline.path();
    options.cacheRoot = cache.path();
    options.modelType = ScreenshotOcrModelType::MediumV5;
    options.downloadOverride = [&](const QString&, const QString& destination, QString* error) {
        if (QFileInfo(destination).fileName() == QStringLiteral("ch_PP-OCRv5_det_server.onnx")) {
            mediumStarted.release();
            if (!releaseMedium.tryAcquire(1, 5'000)) {
                *error = QStringLiteral("timed out waiting for model switch");
                return false;
            }
        }
        return writeDownloadedModelFixture(destination, error);
    };
    ScreenshotOcrAssets assets(options);
    ScreenshotOcrResolvedAssets resolved;
    int readyCount = 0;
    QObject::connect(&assets, &ScreenshotOcrAssets::ready, &assets,
                     [&](const ScreenshotOcrResolvedAssets& result) {
                         resolved = result;
                         ++readyCount;
                     });
    assets.prepare();
    require(waitUntil([&]() { return mediumStarted.available() > 0; }, 5'000),
            "Medium acquisition should reach the controlled download");
    require(mediumStarted.tryAcquire(), "the controlled Medium download should be observed");
    assets.setModelType(ScreenshotOcrModelType::SmallV4);
    assets.prepare();
    releaseMedium.release();
    require(waitUntil([&]() { return readyCount == 1; }, 10'000),
            "the replacement Small V4 acquisition should complete");
    require(resolved.modelType == ScreenshotOcrModelType::SmallV4 &&
                QDir(cache.path())
                    .exists(QStringLiteral("models/ppocrv5-medium-7b2a75a/.complete.json")),
            "a stale Medium V5 download may remain cached but must never become active");
}

// Opt-in real-model coverage. Model payloads are supplied explicitly so the
// deterministic asset tests never need network access or large model fixtures.
void versionedModelsRecognizeText(const QString& modelRoot, const QString& fixturePath,
                                  bool directMl) {
    QFile manifest(QDir(QCoreApplication::applicationDirPath())
                       .filePath(QStringLiteral("assets/ocr/asset-manifest.json")));
    require(manifest.open(QIODevice::ReadOnly), "the staged model manifest must be readable");
    const QJsonArray models = QJsonDocument::fromJson(manifest.readAll())
                                  .object()
                                  .value(QStringLiteral("models"))
                                  .toArray();
    const QImage image(fixturePath);
    require(!image.isNull(), "the bilingual OCR fixture must be readable");
    int checked = 0;
    for (const QJsonValue& value : models) {
        const QJsonObject model = value.toObject();
        const QString type = model.value(QStringLiteral("type")).toString();
        if (!type.endsWith(QStringLiteral("_v4")) && !type.endsWith(QStringLiteral("_v5")))
            continue;
        const QDir directory(
            QDir(modelRoot).filePath(model.value(QStringLiteral("id")).toString()));
        auto options = sourceRuntimeOptions();
        options.detectorModelPath =
            directory.filePath(model.value(QStringLiteral("detector")).toString());
        options.recognizerModelPath =
            directory.filePath(model.value(QStringLiteral("recognizer")).toString());
        options.dictionaryPath =
            directory.filePath(model.value(QStringLiteral("dictionary")).toString());
        ScreenshotOcrRecognitionService service(options,
                                                directMl ? ScreenshotOcrBackendPreference::DirectMl
                                                         : ScreenshotOcrBackendPreference::Cpu);
        QObject receiver;
        bool completed = false;
        ScreenshotOcrRecognitionResult output;
        service.recognize(ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
                              output = std::move(result);
                              completed = true;
                          });
        require(waitUntil([&]() { return completed; }, 90'000),
                "versioned model inference must complete");
        std::cerr << type.toStdString() << ": " << output.error.toStdString() << '\n';
        require(output.error.isEmpty() && output.presentation != nullptr,
                "versioned model inference must succeed");
        QString text;
        for (const auto& line : output.presentation->lines)
            text += line.text;
        std::cerr << text.toStdString() << '\n';
        // CTC models differ in whether they retain spaces between Latin words.
        text.remove(u' ');
        require(text.contains(QStringLiteral("SnowShot12345")) &&
                    text.contains(QStringLiteral("文字识别")),
                "each model must recognize English and Chinese fixture text");
        ++checked;
    }
    require(checked == 4, "real inference must cover all four added model bundles");
}

void actualOcrCrashAfterInference() {
#ifdef Q_OS_WIN
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory;
    auto& diagnostics = DiagnosticsService::instance();
    DiagnosticsOptions diagnosticsOptions;
    diagnosticsOptions.directories = {directory.path()};
    diagnosticsOptions.handlerPath = QStringLiteral(SNOW_TEST_CRASHPAD_HANDLER);
    diagnosticsOptions.mirrorToConsole = false;
    require(diagnostics.initialize(diagnosticsOptions) &&
                diagnostics.status().crashCaptureAvailable,
            "actual OCR crash collector starts");
    ScreenshotOcrRecognitionService service(sourceRuntimeOptions());
    QObject receiver;
    bool crashed = false;
    const auto image = whiteImage();
    service.recognize(
        ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
        [&](ScreenshotOcrRecognitionResult result) {
            require(result.error.isEmpty(), "actual OCR inference succeeds before crash");
            require(service.processId() != 0, "the test owns a running OCR child");
            HANDLE process =
                OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                PROCESS_QUERY_INFORMATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, static_cast<DWORD>(service.processId()));
            require(process != nullptr, "open the test-owned OCR child");
            void* inaccessible =
                VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
            require(inaccessible != nullptr, "reserve inaccessible crash fixture page");
            HANDLE thread = CreateRemoteThread(
                process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(inaccessible),
                nullptr, 0, nullptr);
            require(thread != nullptr && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0,
                    "isolated OCR native crash terminates");
            CloseHandle(thread);
            CloseHandle(process);
            crashed = true;
        });
    require(waitUntil([&] { return crashed; }, kRecognitionTimeoutMs),
            "actual OCR crash completes");
    require(waitUntil([&] { return service.processId() == 0; }, 5000),
            "parent observes the abnormal OCR exit");
    auto collector = makeCrashCollector();
    QString error;
    require(collector->initialize(QDir(directory.path()).filePath(QStringLiteral("crashes")), {},
                                  {}, &error),
            "open actual OCR crash database");
    require(waitUntil([&] { return !collector->reports().isEmpty(); }, 5000),
            "actual OCR dump arrives");
    const auto reports = collector->reports();
    require(reports.size() == 1 && reports.front().context.value(QStringLiteral(
                                       "exception_code")) == QStringLiteral("0xc0000005"),
            "actual OCR dump has native exception context");
    QFile dump(reports.front().path);
    require(dump.open(QIODevice::ReadOnly), "actual OCR dump is readable");
    const auto bytes = dump.readAll();
    dump.close();
    require(bytes.contains(diagnostics.status().sessionId.toUtf8()) &&
                bytes.contains("ocr.operation_started") && bytes.contains("1.0.6"),
            "actual OCR dump retains parent session, operation and runtime version");
    require(diagnostics.flush(), "actual OCR final diagnostics flush");
    diagnostics.shutdown();
#endif
}
} // namespace

int runOcrLifecycleChild();
void ocrProcessLifecycleTests();

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("SNOW_TEST_OCR_LIFECYCLE_CHILD"))
        return runOcrLifecycleChild();
    if (application.arguments().contains(QStringLiteral("--process-lifecycle"))) {
        ocrProcessLifecycleTests();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--managed-runtime-only"))) {
        diskBackedEngineCompletesThroughTheQtWorker(
            application.arguments().contains(QStringLiteral("--directml")), true);
        return 0;
    }
    QTemporaryDir sourceRuntime;
    require(sourceRuntime.isValid(), "an isolated source OCR runtime directory is required");
    stageSourceRuntime(sourceRuntime.path());
    for (const QString& argument : application.arguments()) {
        if (argument.startsWith(QStringLiteral("--model-root="))) {
            const QString fixturePath = qEnvironmentVariable("SNOW_TEST_OCR_TEXT_FIXTURE");
            versionedModelsRecognizeText(
                argument.mid(13), fixturePath,
                application.arguments().contains(QStringLiteral("--directml")));
            return 0;
        }
    }
    if (application.arguments().contains(QStringLiteral("--native-crash"))) {
        actualOcrCrashAfterInference();
        return 0;
    }
    const bool directMlRequested = application.arguments().contains(QStringLiteral("--directml"));
    validOfflineAssetsAreSelectedWithoutNetwork();
    incompleteOfflineAssetsFallBackToOnlineAcquisition();
    everyModelMapsToItsOwnFilesAndCachesAreRetained();
    selectedModelFailureNeverFallsBackToSmallAndCanRetry();
    invalidSchemaTwoManifestsAreRejectedBeforeDownloading();
    modelSelectionDuringAcquisitionIsLastSelectionWins();
    explicitAssetsControlReadiness();
    modelInitializationFailureExposesAssetErrorAndRetries();
    renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine();
    diskBackedEngineCompletesThroughTheQtWorker(directMlRequested);
    if (!directMlRequested) {
        oneEngineIsInitializedBeforeReadyAndReused();
        recognitionRenderIntentCanChangeWhileQueued();
        concurrentRequestsCompleteExactlyOnce();
        interactiveRequestsPrecedeQueuedPrefetch();
        queuedCancellationSkipsExecution();
        workerRecyclesImmediatelyAndCanBeRecreated();
        modelChangeDrainsSubmittedWorkBeforeRestartingPendingWork();
        cancellationSuppressesCompletion();
        receiverDestructionSuppressesCompletion();
        serviceDestructionJoinsWorkersAndSuppressesLateDelivery();
    }
    return 0;
}
