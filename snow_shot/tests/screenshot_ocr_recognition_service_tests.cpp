#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
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
#ifdef Q_OS_MACOS
#include <libproc.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr int kRecognitionTimeoutMs = 30'000;

#ifdef Q_OS_MACOS
const QString kWorkerName = QStringLiteral("snow-ocr-process");
#else
const QString kWorkerName = QStringLiteral("snow-ocr-process.exe");
#endif
QString sourceRuntimeDirectory;
#ifdef Q_OS_MACOS
QString sourceWorkerPath;
#endif

ScreenshotOcrRecognitionService::Options sourceRuntimeOptions() {
    require(!sourceRuntimeDirectory.isEmpty(), "the source OCR runtime must be staged first");
    const QDir models(QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("assets/ocr/models/ppocrv6-small-463ea9f")));
    ScreenshotOcrRecognitionService::Options options;
#ifdef Q_OS_MACOS
    options.processPath = sourceWorkerPath;
#else
    options.processPath = QDir(sourceRuntimeDirectory).filePath(kWorkerName);
#endif
    options.detectorModelPath = models.filePath(QStringLiteral("PP-OCRv6_det_small.onnx"));
    options.recognizerModelPath = models.filePath(QStringLiteral("PP-OCRv6_rec_small.onnx"));
    options.dictionaryPath = models.filePath(QStringLiteral("ppocrv6_dict.txt"));
    options.stateDirectory = sourceRuntimeDirectory;
    return options;
}

void stageSourceRuntime(const QString& directory) {
    sourceRuntimeDirectory = directory;
#ifndef Q_OS_MACOS
    const QDir destination(directory);
#endif
    QString executable = QStringLiteral(SNOW_TEST_OCR_EXECUTABLE);
    for (const QString& argument : QCoreApplication::arguments()) {
        if (argument.startsWith(QStringLiteral("--worker="))) {
            executable = QFileInfo(argument.mid(9)).absoluteFilePath();
        }
    }
    const QDir application(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    // Execute deployed code in place: copying a signed worker away from its
    // Frameworks directory would no longer test the packaged dependency closure.
    sourceWorkerPath = executable == QStringLiteral(SNOW_TEST_OCR_EXECUTABLE)
                           ? application.filePath(kWorkerName)
                           : executable;
    require(
        QFileInfo(sourceWorkerPath).isExecutable() &&
            QFileInfo(
                QFileInfo(sourceWorkerPath).dir().filePath(QStringLiteral("libonnxruntime.dylib")))
                .isFile(),
        "the native worker and adjacent ONNX library must be staged");
#else
    require(QFile::copy(executable, destination.filePath(kWorkerName)),
            "the source OCR worker must be copied from the build target into the test fixture");
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
#endif
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
    options.processPath = QDir(directory.path()).filePath(kWorkerName);
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

void modelInitializationFailureIsReportedOnRequestsAndRetries() {
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
                service.assetStatus().phase != ScreenshotOcrAssetPhase::Failed,
            "model initialization failure must reach its request without corrupting resource "
            "readiness");
    submit();
    require(
        waitUntil([&]() { return completions == 2; }, 10'000) &&
            service.assetStatus().phase != ScreenshotOcrAssetPhase::Failed,
        "a later OCR request should retry and report the selected model initialization failure");
}

void oneEngineIsInitializedAfterReadyAndReused() {
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory;
    DiagnosticsOptions logging;
    logging.directories = {QFileInfo(directory.path()).canonicalFilePath()};
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
                require(readyCount == 1, "process readiness must precede model loading");
                require(fields.value(QStringLiteral("operation")) == QStringLiteral("0") &&
                            fields.value(QStringLiteral("outcome")) == QStringLiteral("succeeded"),
                        "the real engine must initialize for a session");
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

void residentRuntimeRebuildsWarmSessions(
    bool directMl, const QImage& fixture = {},
    const ScreenshotOcrRecognitionService::Options* explicitOptions = nullptr) {
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory;
    DiagnosticsOptions logging;
    logging.directories = {QFileInfo(directory.path()).canonicalFilePath()};
    logging.enableCrashCapture = false;
    logging.mirrorToConsole = false;
    auto& diagnostics = DiagnosticsService::instance();
    require(diagnostics.initialize(logging), "resident diagnostics must initialize");
    const auto count = [&](const QString& event) {
        require(diagnostics.flush(), "resident diagnostics must flush");
        QFile log(diagnostics.status().currentFile);
        require(log.open(QIODevice::ReadOnly), "resident diagnostics must be readable");
        int found = 0;
        for (const auto& line : log.readAll().split('\n')) {
            const auto record = QJsonDocument::fromJson(line).object();
            if (record.value(QStringLiteral("event")) == event)
                ++found;
        }
        return found;
    };
    {
        ScreenshotOcrRecognitionService service(
            explicitOptions != nullptr ? *explicitOptions : sourceRuntimeOptions());
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = configuration.modelHotStart = true;
        configuration.backend = directMl ? ScreenshotOcrBackendPreference::DirectMl
                                         : ScreenshotOcrBackendPreference::Cpu;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return count(QStringLiteral("ocr.engine_ready")) == 1; },
                          kRecognitionTimeoutMs),
                "real warm-up must create the selected engine");
        const auto pid = service.processId();
        require(pid != 0 && count(QStringLiteral("ocr.buffer_allocated")) == 0 &&
                    count(QStringLiteral("ocr.worker_finished")) == 0,
                "real warm-up must hold no transfer buffer and perform no inference");
        QObject receiver;
        int completed = 0;
        for (int cycle = 1; cycle <= 2; ++cycle) {
            auto image = fixture.isNull() ? whiteImage() : fixture;
            require(service.recognize(
                        {image, QRectF(0, 0, image.width(), image.height())}, &receiver,
                        [&](ScreenshotOcrRecognitionResult result) {
                            require(result.error.isEmpty() && result.presentation != nullptr,
                                    "warm recognition must succeed");
                            if (!fixture.isNull()) {
                                QString text;
                                for (const auto& line : result.presentation->lines)
                                    text += line.text;
                                text.remove(QLatin1Char(' '));
                                require(
                                    text.contains(QStringLiteral("SnowShot12345")) &&
                                        text.contains(QStringLiteral("\u6587\u5b57\u8bc6\u522b")),
                                    "packaged resident runtime must recognize the English and "
                                    "Chinese fixture");
                            }
                            ++completed;
                        }) != 0,
                    "warm recognition must be accepted");
            require(waitUntil(
                        [&] {
                            return completed == cycle &&
                                   count(QStringLiteral("ocr.engine_ready")) == cycle + 1;
                        },
                        kRecognitionTimeoutMs),
                    "each inference cycle must end with a newly loaded warm engine");
            require(service.processId() == pid &&
                        count(QStringLiteral("ocr.buffer_released")) == cycle,
                    "warm cycles must retain the process and release every transfer mapping");
        }
        configuration.modelHotStart = false;
        service.setRuntimeConfiguration(configuration);
        configuration.residentProcess = false;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return service.processId() == 0; }, 5000),
                "resident shutdown must finish");
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
#ifdef Q_OS_MACOS
        managedRuntime ? QDir(QCoreApplication::applicationDirPath()).filePath(kWorkerName)
                       : options.processPath;
#else
        managedRuntime ? QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("assets/ocr/runtimes/1.0.7/windows-x64/"
                                                      "snow-ocr-process-1.0.7-windows-x64.exe"))
                       : options.processPath;
#endif
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

void renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine(qreal scale = 1) {
    ScreenshotOcrRecognitionService service;
    QObject receiver;
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(QColor(20, 80, 220));
    const QRectF canvasRect(QPointF(-32, 17), QSizeF(image.size()) / scale);
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

void destructionStaysBoundedWhileRendersAreInFlight() {
    ScreenshotOcrRecognitionService::Options options;
    options.shutdownTimeoutMilliseconds = 150;
    QImage image(2560, 1440, QImage::Format_RGBA8888);
    image.fill(QColor(20, 80, 220));
    QObject receiver;
    QElapsedTimer timer;
    {
        ScreenshotOcrRecognitionService service(options);
        for (int index = 0; index < 16; ++index) {
            ScreenshotOcrRequest request;
            request.image = image;
            request.canvasRect = QRectF(QPointF(), QSizeF(image.size()));
            request.presentation = filterPresentation(request.canvasRect.toAlignedRect());
            request.backgroundColor = QColor(30, 40, 50);
            service.render(std::move(request), &receiver, [](ScreenshotOcrRecognitionResult) {});
        }
        timer.start();
    }
    require(timer.elapsed() < 5000, "OCR service destruction stalled on in-flight local renders");
}

void recognitionRenderIntentCanChangeWhileQueued() {
    auto options = sourceRuntimeOptions();
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

void recognitionRequestsKeepSubmissionOrder() {
    auto options = sourceRuntimeOptions();
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
    require(completionOrder == std::vector<int>({0, 1, 2}),
            "recognitions must remain FIFO across interactive and prefetch requests");
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

void modelChangeUsesTheSameProcessForPendingWork() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary OCR model-change directory should be available");
    const QDir assetRoot(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/ocr")));
    auto options = sourceRuntimeOptions();
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
                            "pending OCR work should run in the existing child");
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
                replacementProcessId > 0 && replacementProcessId == originalProcessId,
            "a model change must preserve FIFO and the existing process");
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
        const QDir directory(
            QDir(modelRoot).filePath(model.value(QStringLiteral("id")).toString()));
        auto options = sourceRuntimeOptions();
        options.detectorModelPath =
            directory.filePath(model.value(QStringLiteral("detector")).toString());
        options.recognizerModelPath =
            directory.filePath(model.value(QStringLiteral("recognizer")).toString());
        options.dictionaryPath =
            directory.filePath(model.value(QStringLiteral("dictionary")).toString());
        if (QCoreApplication::arguments().contains(QStringLiteral("--resident-models"))) {
            residentRuntimeRebuildsWarmSessions(directMl, image, &options);
            std::cerr << type.toStdString() << ": resident hot start passed\n";
            ++checked;
            continue;
        }
        ScreenshotOcrRecognitionService service(options,
                                                directMl ? ScreenshotOcrBackendPreference::DirectMl
                                                         : ScreenshotOcrBackendPreference::Cpu);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = true;
        configuration.modelHotStart = true;
        configuration.backend = directMl ? ScreenshotOcrBackendPreference::DirectMl
                                         : ScreenshotOcrBackendPreference::Cpu;
        service.setRuntimeConfiguration(configuration);
        QObject receiver;
        int completed = 0;
        QElapsedTimer timer;
        timer.start();
        qint64 previousMs = 0;
        for (int cycle = 0; cycle < 2; ++cycle) {
            service.recognize(
                ScreenshotOcrRequest{image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
                [&, cycle](ScreenshotOcrRecognitionResult output) {
                    require(completed == cycle, "real inference callbacks must remain FIFO");
                    require(output.error.isEmpty() && output.presentation != nullptr,
                            "versioned model inference must succeed");
                    QString text;
                    for (const auto& line : output.presentation->lines) {
                        text += line.text;
                        require(line.quad.size() == 4 && line.quad.boundingRect().isValid() &&
                                    std::isfinite(line.confidence),
                                "OCR geometry must be valid");
                        for (const auto& point : line.quad)
                            require(std::isfinite(point.x()) && std::isfinite(point.y()),
                                    "OCR geometry must be finite");
                    }
                    text.remove(u' ');
                    std::cerr << type.toStdString() << ": " << text.toStdString() << '\n';
                    require(text.contains(QStringLiteral("SnowShot12345")) &&
                                text.contains(QStringLiteral("文字识别")),
                            "each model must recognize English and Chinese fixture text");
                    const qint64 elapsedMs = timer.elapsed();
                    if (QCoreApplication::arguments().contains(QStringLiteral("--measure"))) {
                        QJsonObject measurement{
                            {QStringLiteral("model"), type},
                            {QStringLiteral("cycle"),
                             cycle == 0 ? QStringLiteral("cold") : QStringLiteral("warm")},
                            {QStringLiteral("elapsed_ms"), elapsedMs - previousMs}};
#ifdef Q_OS_MACOS
                        proc_taskinfo info{};
                        const int size =
                            proc_pidinfo(static_cast<int>(service.processId()), PROC_PIDTASKINFO, 0,
                                         &info, static_cast<int>(sizeof(info)));
                        require(size == static_cast<int>(sizeof(info)),
                                "worker memory measurements must be available");
                        measurement.insert(QStringLiteral("resident_bytes"),
                                           static_cast<qint64>(info.pti_resident_size));
#endif
                        std::cout
                            << QJsonDocument(measurement).toJson(QJsonDocument::Compact).constData()
                            << '\n';
                    }
                    previousMs = elapsedMs;
                    ++completed;
                });
        }
        require(waitUntil([&]() { return completed == 2; }, 90'000),
                "both real model inference cycles must complete");
        ++checked;
    }
    require(checked == 7, "real inference must cover all seven model bundles");
}

void packagedBundleRecognizesText(const QString& bundle) {
    QTemporaryDir temporaryCache;
    ScreenshotOcrRecognitionService::Options options;
    options.offlineRoot = QDir(bundle).filePath(QStringLiteral("Contents/MacOS/assets/ocr"));
    options.cacheRoot = temporaryCache.path();
    for (const auto& argument : QCoreApplication::arguments()) {
        if (argument.startsWith(QStringLiteral("--cache=")))
            options.cacheRoot = argument.mid(8);
        if (argument.startsWith(QStringLiteral("--model=")))
            options.modelType = screenshotOcrModelTypeFromValue(argument.mid(8));
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--offline")))
        options.proxyUrl = QStringLiteral("http://127.0.0.1:1");
    const QImage image(qEnvironmentVariable("SNOW_TEST_OCR_TEXT_FIXTURE"));
    require(!image.isNull(), "the packaged OCR text fixture must be readable");
    ScreenshotOcrRecognitionService service(options);
    ScreenshotOcrRuntimeConfiguration configuration;
    configuration.modelType = options.modelType;
    configuration.backend = ScreenshotOcrBackendPreference::Cpu;
    configuration.residentProcess = true;
    configuration.modelHotStart = false;
    service.setRuntimeConfiguration(configuration);
    QVector<ScreenshotOcrModelType> selected{options.modelType};
    if (QCoreApplication::arguments().contains(QStringLiteral("--all-models"))) {
        selected = {ScreenshotOcrModelType::ExtraSmall, ScreenshotOcrModelType::Small,
                    ScreenshotOcrModelType::Medium,     ScreenshotOcrModelType::SmallV5,
                    ScreenshotOcrModelType::MediumV5,   ScreenshotOcrModelType::SmallV4,
                    ScreenshotOcrModelType::MediumV4};
    }
    QObject receiver;
    qint64 originalPid = 0;
    for (const auto model : selected) {
        service.setModelType(model);
        int completions = 0;
        for (int cycle = 0; cycle < 2; ++cycle) {
            service.recognize(
                {image, QRectF(QPointF(), QSizeF(image.size()))}, &receiver,
                [&, cycle](ScreenshotOcrRecognitionResult result) {
                    if (!result.error.isEmpty())
                        std::cerr << result.error.toStdString() << '\n';
                    require(result.error.isEmpty() && result.presentation != nullptr,
                            "packaged OCR must succeed through managed asset resolution");
                    require(completions == cycle, "packaged callbacks must remain FIFO");
                    QString text;
                    for (const auto& line : result.presentation->lines) {
                        text += line.text;
                        require(line.quad.size() == 4 && line.quad.boundingRect().isValid(),
                                "packaged OCR geometry must be valid");
                        for (const auto& point : line.quad)
                            require(std::isfinite(point.x()) && std::isfinite(point.y()),
                                    "packaged OCR geometry must be finite");
                    }
                    text.remove(u' ');
                    require(text.contains(QStringLiteral("SnowShot12345")) &&
                                text.contains(QStringLiteral("文字识别")),
                            "packaged OCR must recognize bilingual text");
                    const auto pid = service.processId();
                    require(pid != 0 && (originalPid == 0 || pid == originalPid),
                            "model switching must retain the packaged process");
                    originalPid = pid;
                    ++completions;
                });
        }
        require(waitUntil([&] { return completions == 2; }, 180'000),
                "packaged recognition and optional model download must complete");
        std::cerr << screenshotOcrModelTypeValue(model).toStdString()
                  << ": packaged inference passed\n";
    }
    configuration.residentProcess = false;
    service.setRuntimeConfiguration(configuration);
    require(waitUntil([&] { return service.processId() == 0; }, 5000),
            "packaged worker must shut down when residency is disabled");
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
                bytes.contains("ocr.operation_started") && bytes.contains("1.0.7"),
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
    for (const auto& argument : application.arguments()) {
        if (argument.startsWith(QStringLiteral("--bundle="))) {
            packagedBundleRecognizesText(argument.mid(9));
            return 0;
        }
    }
    if (application.arguments().contains(QStringLiteral("--measure"))) {
#ifndef SNOW_TEST_OCR_PERFORMANCE
        require(false, "OCR measurements require the performance preset");
#endif
    }
    QTemporaryDir sourceRuntime;
    require(sourceRuntime.isValid(), "an isolated source OCR runtime directory is required");
    stageSourceRuntime(sourceRuntime.path());
    if (application.arguments().contains(QStringLiteral("--resident-text-fixture"))) {
        const QImage fixture(qEnvironmentVariable("SNOW_TEST_OCR_TEXT_FIXTURE"));
        require(!fixture.isNull(), "resident text fixture must be readable");
        residentRuntimeRebuildsWarmSessions(
            application.arguments().contains(QStringLiteral("--directml")), fixture);
        return 0;
    }
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

    explicitAssetsControlReadiness();
    modelInitializationFailureIsReportedOnRequestsAndRetries();
    renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine();
    renderOnlyWorkRunsOnTheOcrWorkerWithoutAnEngine(2);
    destructionStaysBoundedWhileRendersAreInFlight();
    diskBackedEngineCompletesThroughTheQtWorker(directMlRequested);
    residentRuntimeRebuildsWarmSessions(directMlRequested);
    if (!directMlRequested) {
        oneEngineIsInitializedAfterReadyAndReused();
        recognitionRenderIntentCanChangeWhileQueued();
        concurrentRequestsCompleteExactlyOnce();
        recognitionRequestsKeepSubmissionOrder();
        queuedCancellationSkipsExecution();
        workerRecyclesImmediatelyAndCanBeRecreated();
        modelChangeUsesTheSameProcessForPendingWork();
        cancellationSuppressesCompletion();
        receiverDestructionSuppressesCompletion();
        serviceDestructionJoinsWorkersAndSuppressesLateDelivery();
    }
    return 0;
}
