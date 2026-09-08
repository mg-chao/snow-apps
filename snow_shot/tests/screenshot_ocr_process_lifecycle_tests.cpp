#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
#ifdef Q_OS_WIN
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

QList<QJsonObject> processExits() {
    auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
    require(diagnostics.flush(), "lifecycle diagnostics must flush");
    QFile file(diagnostics.status().currentFile);
    require(file.open(QIODevice::ReadOnly), "lifecycle log must be readable");
    QList<QJsonObject> result;
    for (const auto& line : file.readAll().split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        if (record.value(QStringLiteral("event")) == QStringLiteral("ocr.process_exit"))
            result.append(record);
    }
    return result;
}
} // namespace

// A real subprocess with the OCR wire handshake, but no model or inference timing.
// Each submit is acknowledged on disk and held until the parent ends the process.
int runOcrLifecycleChild() {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
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
            QByteArray ready;
            QDataStream output(&ready, QIODevice::WriteOnly);
            output.setByteOrder(QDataStream::LittleEndian);
            output << quint32(0x52434f53) << quint16(2) << quint16(2) << quint64(0) << quint32(19)
                   << quint8(1) << quint8(0) << quint32(0) << quint32(5);
            output.writeRawData("1.0.4", 5);
            output << quint32(2);
            if (std::fwrite(ready.constData(), 1, ready.size(), stdout) != size_t(ready.size()))
                return 4;
            std::fflush(stdout);
        } else if (kind == 3) {
            QFile marker(qEnvironmentVariable("SNOW_TEST_OCR_LIFECYCLE_MARKER"));
            if (!marker.open(QIODevice::WriteOnly | QIODevice::Append))
                return 5;
            marker.write(QByteArray::number(token) + '\n');
        } else if (kind == 6) {
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
        require(first != 0 && waitUntil([&] { return submitted(first); }),
                "first inference must reach the controlled child");
        QPointer<QProcess> original = service.findChild<QProcess*>();
        service.cancel(first);
        service.cancel(first);
        const auto second = submit();
        require(second != 0 && waitUntil([&] { return submitted(second) && original.isNull(); }),
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
        auto* child = service.findChild<QProcess*>();
        require(child != nullptr && child->state() == QProcess::Running,
                "replacement child must be running");
        // Identical OS exit code, but no service cancellation: this remains an error.
        child->kill();
        require(waitUntil([&] { return completions == 1; }),
                "unexpected child death must complete the pending request with an error");
        const auto allExits = processExits();
        require(allExits.size() == 2 &&
                    allExits.back().value(QStringLiteral("level")) == QStringLiteral("ERROR") &&
                    allExits.back()
                            .value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("outcome")) == QStringLiteral("crashed"),
                "unrequested termination must remain observable as a crash");
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
    diagnostics.shutdown();
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_CHILD");
    qunsetenv("SNOW_TEST_OCR_LIFECYCLE_MARKER");
}
