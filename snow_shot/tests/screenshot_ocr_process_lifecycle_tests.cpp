#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
#ifdef Q_OS_WIN
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::_Exit(1);
    }
}

bool waitUntil(const std::function<bool()>& condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

QList<QJsonObject> recordsFor(const QString& event) {
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    require(diagnostics.flush(), "lifecycle diagnostics must flush");
    QFile file(diagnostics.status().currentFile);
    require(file.open(QIODevice::ReadOnly), "lifecycle log must be readable");
    QList<QJsonObject> result;
    for (const auto& line : file.readAll().split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        if (record.value(QStringLiteral("event")) == event)
            result.append(record);
    }
    return result;
}
QList<QJsonObject> processExits() {
    return recordsFor(QStringLiteral("ocr.process_exit"));
}
} // namespace

// A real subprocess with the OCR wire handshake, but no model or inference timing.
// Each submit is acknowledged on disk and held until the parent ends the process.
int runOcrLifecycleChild() {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QString mappingPath;
    quint64 mappingBytes = 0;
    while (true) {
        QByteArray header(20, '\0');
        if (std::fread(header.data(), 1, 20, stdin) != 20)
            return 0;
        QDataStream input(header);
        input.setByteOrder(QDataStream::LittleEndian);
        quint32 magic = 0, size = 0;
        quint16 version = 0, kind = 0;
        quint64 token = 0;
        input >> magic >> version >> kind >> token >> size;
        if (magic != 0x52434f53 || version != 2 || size > 1024 * 1024)
            return 2;
        QByteArray payload(size, '\0');
        if (std::fread(payload.data(), 1, size, stdin) != size)
            return 3;
        if (kind == 1) {
            QDataStream hello(payload);
            hello.setByteOrder(QDataStream::LittleEndian);
            quint32 workers = 0;
            quint8 backend = 0;
            hello >> workers >> backend;
            for (int i = 0; i < 5; ++i) {
                quint32 length = 0;
                hello >> length;
                QByteArray value(length, '\0');
                hello.readRawData(value.data(), length);
                if (i == 4)
                    mappingPath = QString::fromUtf8(value);
            }
            quint32 slotCount = 0;
            hello >> mappingBytes >> slotCount;
            if (workers != 1 || slotCount != 1 ||
                QFileInfo(mappingPath).size() != qint64(mappingBytes))
                return 7;
            std::fputs("ONNX Runtime [Error]: fragmented \xe4", stderr);
            std::fflush(stderr);
            QByteArray ready;
            QDataStream output(&ready, QIODevice::WriteOnly);
            output.setByteOrder(QDataStream::LittleEndian);
            output << quint32(0x52434f53) << quint16(2) << quint16(2) << quint64(0) << quint32(19)
                   << quint8(1) << quint8(0) << quint32(0) << quint32(5);
            output.writeRawData("1.0.6", 5);
            output << quint32(2);
            if (std::fwrite(ready.constData(), 1, ready.size(), stdout) != size_t(ready.size()))
                return 4;
            std::fflush(stdout);
        } else if (kind == 3) {
            if (qEnvironmentVariableIsSet("SNOW_TEST_OCR_LIFECYCLE_COMPLETE")) {
                // Verify the request-sized mapping and its pixel contents in the real child.
                QDataStream request(payload);
                request.setByteOrder(QDataStream::LittleEndian);
                quint32 slot = 0, width = 0, height = 0, stride = 0;
                quint64 sequence = 0;
                quint8 priority = 0;
                request >> slot >> width >> height >> stride >> sequence >> priority;
                if (slot != 0 || width == 0 || height == 0 || stride != width * 4)
                    return 6;
                QFile mapped(mappingPath);
                if (!mapped.open(QIODevice::ReadOnly) || mapped.size() != qint64(mappingBytes))
                    return 8;
                const auto pixels = mapped.readAll().mid(32, stride * height);
                if (pixels.size() != stride * height ||
                    pixels != QByteArray(stride * height, '\xff'))
                    return 9;
                QByteArray complete;
                QDataStream response(&complete, QIODevice::WriteOnly);
                response.setByteOrder(QDataStream::LittleEndian);
                response << quint32(0x52434f53) << quint16(2) << quint16(5) << token << quint32(9)
                         << quint8(1) << quint32(0) << quint32(0);
                std::fwrite(complete.constData(), 1, complete.size(), stdout);
                std::fflush(stdout);
                continue;
            }
            // A dedicated watcher makes unexpected termination independent of parent internals.
            const QString crashPath =
                QFileInfo(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER"))
                    .dir()
                    .filePath(QStringLiteral("crash"));
            std::thread([crashPath]() {
                while (!QFileInfo::exists(crashPath))
                    QThread::msleep(1);
#ifdef Q_OS_WIN
                TerminateProcess(GetCurrentProcess(), 0xc0000005);
#else
                std::abort();
#endif
            }).detach();
            std::fputs("\xb8\xad "
                       "error\n{\"event\":\"ocr.engine_ready\",\"fields\":{\"operation\":\"1\","
                       "\"stage\":\"initialization\",\"backend\":\"cpu\",\"outcome\":\"succeeded\","
                       "\"duration_ms\":7}}\n",
                       stderr);
            std::fflush(stderr);
            QFile marker(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER"));
            if (!marker.open(QIODevice::WriteOnly | QIODevice::Append))
                return 5;
            marker.write(QByteArray::number(token) + '\n');
        } else if (kind == 6) {
            if (qEnvironmentVariableIsSet("SNOW_TEST_OCR_LIFECYCLE_COMPLETE"))
                return 0;
            return QCoreApplication::exec();
        }
    }
}

void ocrProcessLifecycleTests() {
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory;
    require(directory.isValid(), "lifecycle fixture directory must exist");
    const auto markerPath = directory.filePath(QStringLiteral("submitted"));
    qputenv("SNOW_TEST_OCR_LIFECYCLE_CHILD", "1");
    qputenv("SNOW_TEST_OCR_LIFECYCLE_MARKER", markerPath.toUtf8());
    DiagnosticsOptions logging;
    logging.directories = {directory.filePath(QStringLiteral("logs"))};
    logging.enableCrashCapture = false;
    logging.mirrorToConsole = false;
    auto& diagnostics = DiagnosticsService::instance();
    require(diagnostics.initialize(logging), "lifecycle diagnostics must initialize");
    ScreenshotOcrRecognitionService::Options options;
    options.processPath = QCoreApplication::applicationFilePath();
    options.detectorModelPath = options.processPath;
    options.recognizerModelPath = options.processPath;
    options.dictionaryPath = options.processPath;
    options.stateDirectory = directory.path();
    {
        QObject receiver;
        ScreenshotOcrRecognitionService service(options);
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        int completions = 0;
        auto submit = [&]() {
            return service.recognize(ScreenshotOcrRequest{image, QRectF(0, 0, 8, 8)}, &receiver,
                                     [&](ScreenshotOcrRecognitionResult result) {
                                         require(!result.error.isEmpty(),
                                                 "unexpected child death must fail inference");
                                         ++completions;
                                     });
        };
        auto submitted = [&](quint64 token) {
            QFile marker(markerPath);
            return marker.open(QIODevice::ReadOnly) &&
                   marker.readAll().split('\n').contains(QByteArray::number(token));
        };
        const auto first = submit();
        require(service.processId() == 0,
                "recognize must return before asynchronous process startup is delivered");
        require(first != 0 && waitUntil([&] { return submitted(first); }),
                "first inference must reach the controlled child");
        require(
            waitUntil([&] { return !recordsFor(QStringLiteral("ocr.engine_ready")).isEmpty(); }),
            "worker stage events must be relayed as structured records");
        const auto stderrRecords = recordsFor(QStringLiteral("ocr.stderr"));
        require(stderrRecords.size() == 1 &&
                    stderrRecords.front().value(QStringLiteral("message")) ==
                        QString::fromUtf8("ONNX Runtime [Error]: fragmented \xe4\xb8\xad error"),
                "stderr must preserve a UTF-8 error fragmented across pipe writes");
        const auto submittedFields = recordsFor(QStringLiteral("ocr.submitted"))
                                         .front()
                                         .value(QStringLiteral("fields"))
                                         .toObject();
        require(submittedFields.value(QStringLiteral("width")).toInt() == 8 &&
                    submittedFields.value(QStringLiteral("height")).toInt() == 8 &&
                    submittedFields.contains(QStringLiteral("queue_ms")) &&
                    submittedFields.contains(QStringLiteral("pending_count")),
                "submission logs must retain image geometry and queue context");
        const auto allocation = recordsFor(QStringLiteral("ocr.process_started"))
                                    .back()
                                    .value(QStringLiteral("fields"))
                                    .toObject();
        require(allocation.value(QStringLiteral("slot_count")).toInt() == 1 &&
                    allocation.value(QStringLiteral("shared_memory_bytes")).toInteger() ==
                        8 * 8 * 4 + 32,
                "an 8x8 request must allocate one 288-byte slot, not four 4K slots");
        const qint64 original = service.processId();
        service.cancel(first);
        service.cancel(first);
        require(recordsFor(QStringLiteral("ocr.cancelled")).size() == 1,
                "repeated cancellation must produce one request cancellation record");
        const auto second = submit();
        require(second != 0 &&
                    waitUntil([&] { return submitted(second) && service.processId() != original; }),
                "a request queued during cancellation must reach a fresh child");
        require(completions == 0, "canceling one request must not fail its replacement");
        const auto exits = processExits();
        require(exits.size() == 1, "cancellation must record exactly one child exit");
        std::cout << QJsonDocument(exits.front()).toJson(QJsonDocument::Compact).constData()
                  << '\n';
        require(exits.front().value(QStringLiteral("level")) == QStringLiteral("INFO") &&
                    exits.front()
                            .value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("outcome")) == QStringLiteral("cancelled"),
                "intentional OCR cancellation must not be reported as a crash");
        require(service.processId() != 0, "replacement child must be running");
        // Identical OS exit code, but no service cancellation: this remains an error.
        QFile crashMarker(directory.filePath(QStringLiteral("crash")));
        require(crashMarker.open(QIODevice::WriteOnly), "the child crash trigger must be writable");
        crashMarker.close();
        require(waitUntil([&] { return completions == 1; }),
                "unexpected child death must complete the pending request with an error");
        const auto failures = recordsFor(QStringLiteral("ocr.process_failed"));
        require(failures.size() == 1 &&
                    failures.front()
                            .value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("stage")) == QStringLiteral("process_exit"),
                "unexpected child death must identify the failed process stage");
        const auto finished = recordsFor(QStringLiteral("ocr.finished"));
        require(!finished.isEmpty(), "process failure must retain request completion diagnostics");
        const auto failedFields = finished.back().value(QStringLiteral("fields")).toObject();
        require(failedFields.value(QStringLiteral("outcome")) == QStringLiteral("failed") &&
                    failedFields.value(QStringLiteral("child_pid")).toInteger() > 0 &&
                    failedFields.contains(QStringLiteral("queue_ms")) &&
                    failedFields.contains(QStringLiteral("worker_ms")),
                "process failure must preserve the affected request PID and timings");
        const auto allExits = processExits();
        require(allExits.size() == 2 &&
                    allExits.back().value(QStringLiteral("level")) == QStringLiteral("ERROR") &&
                    allExits.back()
                            .value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("outcome")) == QStringLiteral("crashed"),
                "unrequested termination must remain observable as a crash");
        require(crashMarker.remove(), "remove the consumed crash trigger");
        auto transientReceiver = std::make_unique<QObject>();
        const auto cancelledByReceiver =
            service.recognize(ScreenshotOcrRequest{image, QRectF(0, 0, 8, 8)},
                              transientReceiver.get(), [&](ScreenshotOcrRecognitionResult) {
                                  require(false, "destroyed receivers must not receive completion");
                              });
        require(waitUntil([&] { return submitted(cancelledByReceiver); }),
                "receiver-owned inference must reach a restarted child");
        transientReceiver.reset();
        require(waitUntil([&] { return service.liveWorkerCount() == 0; }),
                "receiver destruction must retire its only child job");
        require(waitUntil([&] { return processExits().size() == 3; }),
                "receiver destruction must record its child exit");
        require(processExits()
                        .back()
                        .value(QStringLiteral("fields"))
                        .toObject()
                        .value(QStringLiteral("outcome")) == QStringLiteral("cancelled"),
                "receiver destruction must use expected cancellation semantics");
        const auto third = submit();
        require(third != 0 && waitUntil([&] { return submitted(third); }),
                "recognition must be restartable after an unexpected child exit");
        // The controlled child ignores graceful shutdown, exercising the
        // destructor's forced-stop fallback without model-dependent timing.
    }
    const auto finalExits = processExits();
    require(finalExits.size() == 4 &&
                finalExits.back().value(QStringLiteral("level")) == QStringLiteral("INFO") &&
                finalExits.back()
                        .value(QStringLiteral("fields"))
                        .toObject()
                        .value(QStringLiteral("outcome")) == QStringLiteral("shutdown"),
            "forced service shutdown must be recorded as expected termination");
    qputenv("SNOW_TEST_OCR_LIFECYCLE_COMPLETE", "1");
    {
        ScreenshotOcrRecognitionService service(options);
        QObject receiver;
        QList<int> completed;
        const auto submitImage = [&](int edge) {
            // RGB input exercises worker-side conversion as well as mapping growth.
            QImage image(edge, edge, QImage::Format_RGB888);
            image.fill(Qt::white);
            require(service.recognize(
                        ScreenshotOcrRequest{image, QRectF(0, 0, edge, edge)}, &receiver,
                        [&, edge](ScreenshotOcrRecognitionResult result) {
                            require(QThread::currentThread() == receiver.thread(),
                                    "completion must return to the receiver's application thread");
                            require(result.error.isEmpty() && result.presentation != nullptr,
                                    "small and larger queued images must both complete");
                            completed.append(edge);
                        }) != 0,
                    "growth fixture request must be accepted");
        };
        const auto initialStarts = recordsFor(QStringLiteral("ocr.process_started")).size();
        submitImage(8);
        submitImage(32);
        submitImage(16);
        require(waitUntil([&] { return completed.size() == 3 && service.processId() == 0; }),
                "queued growth must drain and release the child without losing requests");
        require(completed == QList<int>({8, 32, 16}),
                "buffer growth must preserve FIFO order for equal-priority requests");
        const auto starts = recordsFor(QStringLiteral("ocr.process_started"));
        require(starts.size() == initialStarts + 2,
                "a larger queued image grows the transport once; smaller images reuse it");
        const auto fields = starts.back().value(QStringLiteral("fields")).toObject();
        require(fields.value(QStringLiteral("shared_memory_bytes")).toInteger() == 32 * 32 * 4 + 32,
                "replacement mapping must fit the largest queued image exactly");
    }
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_COMPLETE");
    diagnostics.shutdown();
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_CHILD");
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_MARKER");
}
