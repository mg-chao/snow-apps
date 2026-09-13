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
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
#include <mutex>
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
    quint64 mappingBytes = 0, mappingGeneration = 0;
    static std::mutex outputMutex;
    const auto reply = [](quint16 kind, quint64 token, const QByteArray& payload = {}) {
        std::lock_guard lock(outputMutex);
        QByteArray frame;
        QDataStream stream(&frame, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << quint32(0x52434f53) << quint16(3) << kind << token << quint32(payload.size());
        frame.append(payload);
        std::fwrite(frame.constData(), 1, frame.size(), stdout);
        std::fflush(stdout);
    };
    const auto event = [](const QByteArray& value) {
        QFile file(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER") +
                   QStringLiteral(".events"));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(value + '\n');
    };
    const auto complete = [reply](quint64 token) {
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << quint8(1) << quint32(0) << quint32(0);
        reply(5, token, payload);
    };
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
        if (magic != 0x52434f53 || version != 3 || size > 1024 * 1024)
            return 2;
        QByteArray payload(size, '\0');
        if (std::fread(payload.data(), 1, size, stdin) != size)
            return 3;
        if (kind == 1) {
            event("start");
            const auto crashPath = QFileInfo(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER"))
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
            std::fputs("ONNX Runtime [Error]: fragmented \xe4", stderr);
            std::fflush(stderr);
            QByteArray ready;
            QDataStream output(&ready, QIODevice::WriteOnly);
            output.setByteOrder(QDataStream::LittleEndian);
            output << quint8(1) << quint8(0) << quint32(0) << quint32(5);
            output.writeRawData("1.0.7", 5);
            output << quint32(3);
            reply(2, 0, ready);
        } else if (kind == 8) {
            event("prepare " + payload.toHex());
            if (qEnvironmentVariableIsSet("SNOW_TEST_OCR_PREPARE_HOLD")) {
                const auto gate = QFileInfo(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER"))
                                      .dir()
                                      .filePath(QStringLiteral("prepared-%1").arg(token));
                std::thread([gate, token, reply]() {
                    while (!QFileInfo::exists(gate))
                        QThread::msleep(1);
                    reply(9, token, QByteArray(1, '\1'));
                }).detach();
            } else {
                reply(9, token,
                      QByteArray(1, qEnvironmentVariableIsSet("SNOW_TEST_OCR_PREPARE_FAIL")
                                        ? '\0'
                                        : '\1'));
            }
        } else if (kind == 10) {
            event("release");
            reply(11, token);
        } else if (kind == 12) {
            QDataStream buffer(payload);
            buffer.setByteOrder(QDataStream::LittleEndian);
            quint32 length = 0;
            buffer >> length;
            QByteArray path(length, '\0');
            buffer.readRawData(path.data(), length);
            mappingPath = QString::fromUtf8(path);
            buffer >> mappingBytes;
            mappingGeneration = token;
            if (QFileInfo(mappingPath).size() != qint64(mappingBytes))
                return 7;
            event("attach " + QByteArray::number(mappingBytes));
            reply(13, token);
        } else if (kind == 16) {
            event("detach");
            mappingPath.clear();
            reply(17, token);
        } else if (kind == 3) {
            QDataStream request(payload);
            request.setByteOrder(QDataStream::LittleEndian);
            quint64 generation = 0, sequence = 0;
            quint32 width = 0, height = 0, stride = 0;
            request >> generation >> width >> height >> stride >> sequence;
            if (generation != mappingGeneration || width == 0 || height == 0 || stride != width * 4)
                return 6;
            QFile mapped(mappingPath);
            if (!mapped.open(QIODevice::ReadOnly) || mapped.size() != qint64(mappingBytes))
                return 8;
            const auto pixels = mapped.readAll().mid(32, stride * height);
            if (pixels.size() != stride * height || pixels != QByteArray(stride * height, '\xff'))
                return 9;
            event("transfer " + QByteArray::number(token));
            QByteArray consumed;
            QDataStream ack(&consumed, QIODevice::WriteOnly);
            ack.setByteOrder(QDataStream::LittleEndian);
            ack << generation << sequence;
            reply(15, token, consumed);
        } else if (kind == 14) {
            event("infer " + QByteArray::number(token));
            if (qEnvironmentVariableIsSet("SNOW_TEST_OCR_LIFECYCLE_COMPLETE")) {
                complete(token);
                continue;
            }
            const QString directory =
                QFileInfo(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER")).absolutePath();
            std::thread([directory, token, complete]() {
                const auto finish =
                    QDir(directory).filePath(QStringLiteral("finish-%1").arg(token));
                while (!QFileInfo::exists(finish)) {
                    if (QFileInfo::exists(QDir(directory).filePath(QStringLiteral("crash")))) {
#ifdef Q_OS_WIN
                        TerminateProcess(GetCurrentProcess(), 0xc0000005);
#else
                        std::abort();
#endif
                    }
                    QThread::msleep(1);
                }
                complete(token);
            }).detach();
            std::fputs(
                "\xb8\xad error\n{\"event\":\"ocr.engine_ready\",\"fields\":{\"operation\":\"1\","
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
        const auto allocation = recordsFor(QStringLiteral("ocr.buffer_allocated"))
                                    .back()
                                    .value(QStringLiteral("fields"))
                                    .toObject();
        require(allocation.value(QStringLiteral("shared_memory_bytes")).toInteger() ==
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
        require(starts.size() == initialStarts + 1,
                "all queued image sizes must share one process");
        const auto fields = recordsFor(QStringLiteral("ocr.buffer_allocated"))
                                .back()
                                .value(QStringLiteral("fields"))
                                .toObject();
        require(fields.value(QStringLiteral("shared_memory_bytes")).toInteger() == 32 * 32 * 4 + 32,
                "replacement mapping must fit the largest queued image exactly");
    }
    const auto events = [&]() {
        QFile file(markerPath + QStringLiteral(".events"));
        require(file.open(QIODevice::ReadOnly), "child lifecycle events must be readable");
        return file.readAll().split('\n');
    };
    const auto countEvent = [&](const QByteArray& prefix) {
        int count = 0;
        for (const auto& event : events())
            if (event.startsWith(prefix))
                ++count;
        return count;
    };
    const auto touch = [&](const QString& name) {
        QFile file(directory.filePath(name));
        require(file.open(QIODevice::WriteOnly), "lifecycle control file must be writable");
    };
    {
        const int initialStarts = countEvent("start");
        const int initialLoads = countEvent("prepare ");
        const int initialTransfers = countEvent("transfer ");
        const int initialBuffers = countEvent("attach ");
        ScreenshotOcrRecognitionService service(options);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        QCoreApplication::processEvents();
        require(service.processId() == 0, "saved hot start cannot activate residency");
        configuration.residentProcess = true;
        configuration.modelHotStart = false;
        service.setRuntimeConfiguration(configuration);
        require(service.processId() == 0, "resident settings must return before startup");
        require(waitUntil([&] { return countEvent("start") == initialStarts + 1; }),
                "resident mode must start without a request");
        const auto pid = service.processId();
        require(waitUntil([&] { return service.processId() != 0; }),
                "resident PID must become visible");
        require(countEvent("prepare ") == initialLoads && countEvent("attach ") == initialBuffers,
                "resident startup must not load a model or create a buffer");
        configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("prepare ") == initialLoads + 1; }),
                "hot start must load a session");
        require(countEvent("transfer ") == initialTransfers &&
                    countEvent("attach ") == initialBuffers,
                "warm-up must have no image or transfer buffer");
        QObject receiver;
        int completed = 0;
        const auto submit = [&]() {
            QImage image(8, 8, QImage::Format_RGBA8888);
            image.fill(Qt::white);
            return service.recognize(
                {image, QRectF(0, 0, 8, 8)}, &receiver, [&](ScreenshotOcrRecognitionResult result) {
                    require(result.error.isEmpty(), "resident recognition must succeed");
                    ++completed;
                });
        };
        require(submit() != 0, "hot recognition must be accepted");
        require(
            waitUntil([&] { return completed == 1 && countEvent("prepare ") == initialLoads + 2; }),
            "idle hot mode must replace the inference-used session with a fresh session");
        require(countEvent("attach ") == initialBuffers + 1 &&
                    countEvent("transfer ") == initialTransfers + 1,
                "one real recognition must perform exactly one transfer");
        const auto warmedPid = service.processId();
        require(pid == 0 || pid == warmedPid, "warm-up must preserve process identity");
        const auto before = events();
        int releaseIndex = -1, lastPrepare = -1;
        for (int i = 0; i < before.size(); ++i) {
            if (before[i] == "release")
                releaseIndex = i;
            if (before[i].startsWith("prepare "))
                lastPrepare = i;
        }
        require(releaseIndex >= 0 && releaseIndex < lastPrepare,
                "the old engine must be released before the next warm session is created");
        configuration.backend = ScreenshotOcrBackendPreference::DirectMl;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("prepare ") == initialLoads + 3; }),
                "backend change must rebuild idle warm-up");
        require(service.processId() == warmedPid, "backend changes must not restart the process");
        configuration.modelHotStart = false;
        const int released = countEvent("release");
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("release") == released + 1; }),
                "hot-off must release its warm session");
        const int loads = countEvent("prepare ");
        require(submit() != 0, "resident-only recognition must be accepted");
        require(waitUntil([&] { return completed == 2 && countEvent("release") == released + 2; }),
                "resident-only recognition must release its model at idle");
        require(countEvent("prepare ") == loads + 1 && service.processId() == warmedPid,
                "resident-only must load once for recognition and retain the process");
        configuration.residentProcess = false;
        configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return service.processId() == 0; }),
                "resident-off must exit even with saved hot start");
    }
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_COMPLETE");
    {
        // Hold real child responses, so transfer growth overlaps an active inference.
        ScreenshotOcrRecognitionService service(options);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = true;
        configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        QObject receiver;
        QList<int> completed;
        const int initialLoads = countEvent("prepare ");
        const int initialInference = countEvent("infer ");
        const int initialTransfers = countEvent("transfer ");
        const auto submit = [&](int edge, ScreenshotOcrRequestPriority priority) {
            QImage image(edge, edge, QImage::Format_RGBA8888);
            image.fill(Qt::white);
            return service.recognize({image, QRectF(0, 0, edge, edge), priority}, &receiver,
                                     [&, edge](ScreenshotOcrRecognitionResult result) {
                                         require(result.error.isEmpty(),
                                                 "controlled resident request must succeed");
                                         completed.append(edge);
                                     });
        };
        const auto first = submit(8, ScreenshotOcrRequestPriority::Interactive);
        require(waitUntil([&] { return countEvent("infer ") == initialInference + 1; }),
                "held inference must begin");
        const auto pid = service.processId();
        const auto second = submit(32, ScreenshotOcrRequestPriority::Prefetch);
        const auto third = submit(16, ScreenshotOcrRequestPriority::Interactive);
        require(waitUntil([&] { return countEvent("transfer ") == initialTransfers + 2; }),
                "a larger next image must transfer while inference is in flight");
        require(countEvent("infer ") == initialInference + 1 && service.processId() == pid,
                "buffer growth must neither start another inference nor replace the process");
        require(events().contains("attach 4128"),
                "growth must use exactly the largest pending image capacity");
        configuration.backend = ScreenshotOcrBackendPreference::DirectMl;
        configuration.modelType = ScreenshotOcrModelType::Medium;
        service.setRuntimeConfiguration(configuration);
        service.cancel(first);
        require(service.processId() == pid, "resident cancellation must preserve its child");
        touch(QStringLiteral("finish-%1").arg(first));
        require(waitUntil([&] { return countEvent("infer ") == initialInference + 2; }),
                "queued prefetch must execute second");
        require(completed.isEmpty(), "canceled inference must not deliver its result");
        require(
            countEvent("prepare ") == initialLoads + 2,
            "queued inference must load the latest configuration after the old inference drains");
        touch(QStringLiteral("finish-%1").arg(second));
        require(waitUntil([&] {
                    return completed == QList<int>({32}) &&
                           countEvent("infer ") == initialInference + 3;
                }),
                "mixed priorities must preserve strict FIFO");
        configuration.residentProcess = false;
        service.setRuntimeConfiguration(configuration);
        require(service.processId() == pid, "resident-off must let outstanding inference finish");
        touch(QStringLiteral("finish-%1").arg(third));
        require(waitUntil(
                    [&] { return completed == QList<int>({32, 16}) && service.processId() == 0; }),
                "resident-off must drain and exit without failing requests");
        for (const auto token : {first, second, third})
            QFile::remove(directory.filePath(QStringLiteral("finish-%1").arg(token)));
    }
    qputenv("SNOW_TEST_OCR_LIFECYCLE_COMPLETE", "1");
    {
        auto crashOptions = options;
        crashOptions.retryTimeUnitMilliseconds = 20;
        ScreenshotOcrRecognitionService service(crashOptions);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = true;
        configuration.modelHotStart = true;
        const int starts = countEvent("start");
        service.setRuntimeConfiguration(configuration);
        require(waitUntil(
                    [&] { return countEvent("start") == starts + 1 && service.processId() != 0; }),
                "crash fixture must start resident child");
        touch(QStringLiteral("crash"));
        require(waitUntil(
                    [&] { return countEvent("start") == starts + 3 && service.processId() == 0; }),
                "three unexpected exits must suspend background residency");
        QFile::remove(directory.filePath(QStringLiteral("crash")));
        QObject receiver;
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        bool prefetched = false;
        service.recognize({image, QRectF(0, 0, 8, 8), ScreenshotOcrRequestPriority::Prefetch},
                          &receiver, [&](ScreenshotOcrRecognitionResult result) {
                              require(result.error.isEmpty(),
                                      "automatic prefetch must still work on demand");
                              prefetched = true;
                          });
        require(waitUntil([&] { return prefetched && service.processId() == 0; }),
                "automatic prefetch must not re-arm suspended residency");
        int completed = 0;
        require(service.recognize({image, QRectF(0, 0, 8, 8)}, &receiver,
                                  [&](ScreenshotOcrRecognitionResult result) {
                                      require(result.error.isEmpty(),
                                              "user retry after crash suspension must succeed");
                                      ++completed;
                                  }) != 0,
                "user retry must be accepted");
        require(waitUntil([&] { return completed == 1 && service.processId() != 0; }),
                "user recognition must re-arm residency after crash backoff");
    }
    {
        QSemaphore entered, resume;
        auto renderOptions = options;
        renderOptions.beforeLocalRender = [&]() {
            entered.release();
            require(resume.tryAcquire(1, 5000), "ordered-render barrier must be released");
        };
        ScreenshotOcrRecognitionService service(renderOptions);
        QObject receiver;
        QList<int> completed;
        auto image = QImage(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        ScreenshotOcrRequest first{image, QRectF(0, 0, 8, 8)};
        first.renderFilteredImage = true;
        const int inferences = countEvent("infer ");
        service.recognize(first, &receiver, [&](ScreenshotOcrRecognitionResult result) {
            require(result.error.isEmpty(), "rendered recognition must succeed");
            completed.append(1);
        });
        QImage oversized(3840, 2161, QImage::Format_RGBA8888);
        service.recognize({oversized, QRectF(0, 0, 3840, 2161)}, &receiver,
                          [&](ScreenshotOcrRecognitionResult result) {
                              require(!result.error.isEmpty(),
                                      "an oversized recognition must fail");
                              completed.append(2);
                          });
        service.recognize({image, QRectF(0, 0, 8, 8)}, &receiver,
                          [&](ScreenshotOcrRecognitionResult result) {
                              require(result.error.isEmpty(), "following recognition must succeed");
                              completed.append(3);
                          });
        require(waitUntil([&] {
                    return entered.available() == 1 && countEvent("infer ") == inferences + 2 &&
                           service.processId() == 0;
                }),
                "local rendering must not retain the OCR process or block the next inference");
        require(completed.isEmpty(), "a later result must wait for the earlier rendered result");
        resume.release();
        require(waitUntil([&] { return completed == QList<int>({1, 2, 3}); }),
                "rendering must preserve callback submission order");
    }
    {
        const int loads = countEvent("prepare ");
        const int buffers = countEvent("attach ");
        auto delayedOptions = options;
        delayedOptions.retryTimeUnitMilliseconds = 10;
        delayedOptions.processPath = directory.filePath(QStringLiteral("later-worker.exe"));
        ScreenshotOcrRecognitionService service(delayedOptions);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = true;
        service.setRuntimeConfiguration(configuration);
        QCoreApplication::processEvents();
        require(service.processId() == 0 && !service.modelFilesReady(),
                "missing resources must postpone resident startup without blocking the caller");
        require(QFile::copy(QCoreApplication::applicationFilePath(), delayedOptions.processPath),
                "delayed runtime fixture must become available");
        require(
            waitUntil([&] { return service.processId() != 0; }),
            "background retries must automatically start the process when resources become ready");
        require(countEvent("prepare ") == loads && countEvent("attach ") == buffers,
                "deferred resident-only activation must not warm a model or allocate pixels");
    }
    {
        const int loads = countEvent("prepare ");
        const int transfers = countEvent("transfer ");
        ScreenshotOcrRecognitionService service(options);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("prepare ") == loads + 1; }),
                "cancellation fixture must warm");
        QObject receiver;
        auto image = QImage(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        const auto token = service.recognize(
            {image, QRectF(0, 0, 8, 8)}, &receiver, [](ScreenshotOcrRecognitionResult) {
                require(false, "canceled queued request must not deliver");
            });
        service.cancel(token);
        require(waitUntil([&] { return countEvent("prepare ") == loads + 2; }),
                "even a canceled recognition cycle must leave a newly created warm session");
        require(countEvent("transfer ") == transfers,
                "queued cancellation must not transfer an image");
    }
    qputenv("SNOW_TEST_OCR_PREPARE_HOLD", "1");
    {
        const int loads = countEvent("prepare ");
        const int releases = countEvent("release");
        const int transfers = countEvent("transfer ");
        ScreenshotOcrRecognitionService service(options);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("prepare ") == loads + 1; }),
                "held background warm-up must begin");
        const auto pid = service.processId();
        configuration.modelHotStart = false;
        service.setRuntimeConfiguration(configuration);
        touch(QStringLiteral("prepared-1"));
        require(waitUntil([&] { return countEvent("release") == releases + 1; }),
                "disabling hot start during loading must discard the completed warm session");
        require(countEvent("transfer ") == transfers && countEvent("prepare ") == loads + 1 &&
                    service.processId() == pid,
                "obsolete warm-up must neither infer nor relaunch nor create another session");
        QFile::remove(directory.filePath(QStringLiteral("prepared-1")));
    }
    qunsetenv("SNOW_TEST_OCR_PREPARE_HOLD");
    qputenv("SNOW_TEST_OCR_PREPARE_FAIL", "1");
    {
        const int loads = countEvent("prepare ");
        ScreenshotOcrRecognitionService service(options);
        ScreenshotOcrRuntimeConfiguration configuration;
        configuration.residentProcess = configuration.modelHotStart = true;
        service.setRuntimeConfiguration(configuration);
        require(waitUntil([&] { return countEvent("prepare ") == loads + 1; }),
                "background warm-up failure fixture must load");
        // Process pending background responses before initiating foreground recognition.
        QCoreApplication::processEvents();
        QObject receiver;
        auto image = QImage(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        int failed = 0;
        service.recognize({image, QRectF(0, 0, 8, 8)}, &receiver,
                          [&](ScreenshotOcrRecognitionResult result) {
                              require(!result.error.isEmpty(),
                                      "foreground session failure must reach its request");
                              ++failed;
                          });
        require(waitUntil([&] { return failed == 1; }),
                "only an actual recognition must expose session failure");
        require(
            service.processId() != 0 &&
                service.assetStatus().phase != ScreenshotOcrAssetPhase::Failed,
            "warm-up failure must preserve resident process and independent resource readiness");
    }
    qunsetenv("SNOW_TEST_OCR_PREPARE_FAIL");
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_COMPLETE");
    diagnostics.shutdown();
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_CHILD");
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_MARKER");
}
