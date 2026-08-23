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
#include <QVector>

#include <utility>

namespace snow_shot::presentation::screenshot_lifecycle_perf {
namespace {
constexpr qsizetype kExpectedPhaseEventCount = 32;

struct BufferedPhaseEvent {
    QString event;
    qint64 elapsedNanoseconds = 0;
    qint64 sequence = 0;
};

QMutex traceMutex;
QFile traceFile;
QElapsedTimer captureTimer;
QDateTime captureStartedUtc;
QVector<BufferedPhaseEvent> pendingPhaseEvents;
qint64 nextEventSequence = 0;
bool activeCapture = false;
bool captureWasPresented = false;

QByteArray serializedEvent(const QString& event, qint64 sequence, const QDateTime& timestampUtc,
                           QJsonObject values = {}) {
    values.insert(QStringLiteral("schema_version"), 1);
    values.insert(QStringLiteral("event"), event);
    values.insert(QStringLiteral("event_sequence"), sequence);
    values.insert(QStringLiteral("timestamp_utc"), timestampUtc.toString(Qt::ISODateWithMs));
    values.insert(QStringLiteral("process_id"), QCoreApplication::applicationPid());
    QByteArray line = QJsonDocument(values).toJson(QJsonDocument::Compact);
    line.append('\n');
    return line;
}

void writeSynchronizationEvent(const QString& event, QJsonObject values = {}) {
    if (!traceFile.isOpen()) {
        return;
    }

    traceFile.write(serializedEvent(event, nextEventSequence++, QDateTime::currentDateTimeUtc(),
                                    std::move(values)));
    traceFile.flush();
}

void flushPendingPhaseEvents() {
    if (pendingPhaseEvents.isEmpty() || !traceFile.isOpen()) {
        pendingPhaseEvents.clear();
        return;
    }

    QByteArray lines;
    lines.reserve(pendingPhaseEvents.size() * 192);
    for (const BufferedPhaseEvent& phase : std::as_const(pendingPhaseEvents)) {
        const QDateTime timestampUtc =
            captureStartedUtc.addMSecs(phase.elapsedNanoseconds / (1000 * 1000));
        lines.append(serializedEvent(
            phase.event, phase.sequence, timestampUtc,
            QJsonObject{{QStringLiteral("elapsed_ns"), phase.elapsedNanoseconds},
                        {QStringLiteral("write_deferred"), true}}));
    }
    traceFile.write(lines);
    traceFile.flush();
    pendingPhaseEvents.clear();
}
} // namespace

void configureTrace(const QString& path) {
    QMutexLocker lock(&traceMutex);
    if (traceFile.isOpen()) {
        flushPendingPhaseEvents();
        traceFile.close();
    }
    traceFile.setFileName(path);
    if (!path.isEmpty()) {
        static_cast<void>(traceFile.open(QIODevice::WriteOnly | QIODevice::Append));
    }
    pendingPhaseEvents.clear();
    pendingPhaseEvents.reserve(kExpectedPhaseEventCount);
    nextEventSequence = 0;
    activeCapture = false;
    captureWasPresented = false;
}

void appReady() {
    QMutexLocker lock(&traceMutex);
    writeSynchronizationEvent(QStringLiteral("app_ready"));
}

void beginCapture() {
    QMutexLocker lock(&traceMutex);
    flushPendingPhaseEvents();
    activeCapture = traceFile.isOpen();
    captureWasPresented = false;
    if (!activeCapture) {
        return;
    }
    captureStartedUtc = QDateTime::currentDateTimeUtc();
    captureTimer.start();
    writeSynchronizationEvent(QStringLiteral("capture_command_accepted"));
}

void mark(const QString& event) {
    QMutexLocker lock(&traceMutex);
    if (!activeCapture || !captureTimer.isValid()) {
        return;
    }
    pendingPhaseEvents.append(
        BufferedPhaseEvent{event, captureTimer.nsecsElapsed(), nextEventSequence++});
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
    const qint64 elapsedNanoseconds = captureTimer.nsecsElapsed();
    captureWasPresented = true;
    writeSynchronizationEvent(
        QStringLiteral("first_capture_presented"),
        QJsonObject{{QStringLiteral("elapsed_ns"), elapsedNanoseconds},
                    {QStringLiteral("deferred_phase_event_count"), pendingPhaseEvents.size()}});
    // The synchronization record must reach the benchmark before phase serialization or I/O.
    // event_sequence remains the canonical chronological order for the deferred JSONL records.
    flushPendingPhaseEvents();
}

void captureInteractionReady() {
    QMutexLocker lock(&traceMutex);
    if (!activeCapture || !captureWasPresented || !captureTimer.isValid()) {
        return;
    }
    writeSynchronizationEvent(
        QStringLiteral("capture_interaction_ready"),
        QJsonObject{{QStringLiteral("elapsed_ns"), captureTimer.nsecsElapsed()}});
}

void captureReleased() {
    QMutexLocker lock(&traceMutex);
    if (!activeCapture) {
        return;
    }
    const qsizetype deferredPhaseEventCount = pendingPhaseEvents.size();
    flushPendingPhaseEvents();
    writeSynchronizationEvent(
        QStringLiteral("capture_released"),
        QJsonObject{{QStringLiteral("capture_presented"), captureWasPresented},
                    {QStringLiteral("deferred_phase_event_count"), deferredPhaseEventCount}});
    activeCapture = false;
    captureWasPresented = false;
}

} // namespace snow_shot::presentation::screenshot_lifecycle_perf

#else

namespace snow_shot::presentation::screenshot_lifecycle_perf {
void configureTrace(const QString&) {}
void appReady() {}
void beginCapture() {}
void mark(const QString&) {}
bool captureActive() {
    return false;
}
void capturePresented() {}
void captureInteractionReady() {}
void captureReleased() {}
} // namespace snow_shot::presentation::screenshot_lifecycle_perf

#endif
