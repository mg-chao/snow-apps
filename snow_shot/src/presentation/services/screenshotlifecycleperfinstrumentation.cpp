#include "screenshotlifecycleperfinstrumentation.h"

#if defined(SNOW_SHOT_SCREENSHOT_LIFECYCLE_PERF_INSTRUMENTATION)

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

namespace snow_shot::presentation::screenshot_lifecycle_perf {
namespace {
QMutex traceMutex;
QFile traceFile;
QElapsedTimer captureTimer;
bool activeCapture = false;
bool captureWasPresented = false;

void writeEvent(const QString& event, QJsonObject values = {}) {
    if (!traceFile.isOpen()) {
        return;
    }

    values.insert(QStringLiteral("schema_version"), 1);
    values.insert(QStringLiteral("event"), event);
    values.insert(QStringLiteral("timestamp_utc"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    values.insert(QStringLiteral("process_id"), QCoreApplication::applicationPid());
    traceFile.write(QJsonDocument(values).toJson(QJsonDocument::Compact));
    traceFile.write("\n");
    traceFile.flush();
}
} // namespace

void configureTrace(const QString& path) {
    QMutexLocker lock(&traceMutex);
    if (traceFile.isOpen()) {
        traceFile.close();
    }
    traceFile.setFileName(path);
    if (!path.isEmpty()) {
        static_cast<void>(traceFile.open(QIODevice::WriteOnly | QIODevice::Append));
    }
    activeCapture = false;
    captureWasPresented = false;
}

void appReady() {
    QMutexLocker lock(&traceMutex);
    writeEvent(QStringLiteral("app_ready"));
}

void beginCapture() {
    QMutexLocker lock(&traceMutex);
    activeCapture = traceFile.isOpen();
    captureWasPresented = false;
    if (!activeCapture) {
        return;
    }
    captureTimer.start();
    writeEvent(QStringLiteral("capture_command_accepted"));
}

bool captureActive() {
    QMutexLocker lock(&traceMutex);
    return activeCapture;
}

void capturePresented() {
    QMutexLocker lock(&traceMutex);
    if (!activeCapture || !captureTimer.isValid()) {
        return;
    }
    captureWasPresented = true;
    writeEvent(QStringLiteral("first_capture_presented"),
               QJsonObject{{QStringLiteral("elapsed_ns"), captureTimer.nsecsElapsed()}});
}

void captureReleased() {
    QMutexLocker lock(&traceMutex);
    if (!activeCapture) {
        return;
    }
    writeEvent(QStringLiteral("capture_released"),
               QJsonObject{{QStringLiteral("capture_presented"), captureWasPresented}});
    activeCapture = false;
    captureWasPresented = false;
}
} // namespace snow_shot::presentation::screenshot_lifecycle_perf

#else

namespace snow_shot::presentation::screenshot_lifecycle_perf {
void configureTrace(const QString&) {}
void appReady() {}
void beginCapture() {}
bool captureActive() {
    return false;
}
void capturePresented() {}
void captureReleased() {}
} // namespace snow_shot::presentation::screenshot_lifecycle_perf

#endif
