#include "screenrecordingperfinstrumentation.h"

#if defined(SNOW_SHOT_RECORDING_PERF_INSTRUMENTATION)
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

namespace snow_shot::presentation::recording_perf {
namespace {
QMutex mutex;
QFile traceFile;
Sink* customSink = nullptr;
struct State {
    QElapsedTimer timer;
    QJsonObject spans;
    QJsonObject milestones;
    QJsonObject counters;
    QSet<QString> recordedMilestones;
    QString scenario;
    qint64 width = 0;
    qint64 height = 0;
    bool active = false;
};
State state;

class FileSink final : public Sink {
  public:
    void recordScope(const char* name, qint64 nanoseconds) override {
        if (state.active) {
            const QString key = QString::fromLatin1(name);
            state.spans[key] = state.spans.value(key).toInteger() + nanoseconds;
        }
    }
    void recordMilestone(const char* name, qint64 nanoseconds) override {
        if (state.active)
            state.milestones[QString::fromLatin1(name)] = nanoseconds;
    }
    void recordCounter(const char* name, qint64 value) override {
        if (state.active) {
            const QString key = QString::fromLatin1(name);
            state.counters[key] = state.counters.value(key).toInteger() + value;
        }
    }
    void finish(bool success) override {
        if (!state.active)
            return;
        QJsonObject object{{QStringLiteral("timestamp_utc"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {QStringLiteral("scenario"), state.scenario},
                           {QStringLiteral("width"), state.width},
                           {QStringLiteral("height"), state.height},
                           {QStringLiteral("success"), success},
                           {QStringLiteral("end_to_end_ns"), state.timer.nsecsElapsed()},
                           {QStringLiteral("spans_ns"), state.spans},
                           {QStringLiteral("milestones_ns"), state.milestones},
                           {QStringLiteral("counters"), state.counters},
                           {QStringLiteral("thread"), QString::number(reinterpret_cast<quintptr>(
                                                          QThread::currentThreadId()))}};
        if (traceFile.isOpen()) {
            traceFile.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
            traceFile.write("\n");
            traceFile.flush();
        }
        state.active = false;
    }
};
FileSink fileSink;

Sink* activeSink() {
    return customSink != nullptr ? customSink : &fileSink;
}
} // namespace

void configureTrace(const QString& path) {
    QMutexLocker lock(&mutex);
    if (traceFile.isOpen())
        traceFile.close();
    if (!path.isEmpty()) {
        traceFile.setFileName(path);
        static_cast<void>(traceFile.open(QIODevice::WriteOnly | QIODevice::Append));
    }
}
void installSink(Sink* sink) {
    QMutexLocker lock(&mutex);
    customSink = sink;
}
void beginSample(const char* scenario, qint64 width, qint64 height) {
    QMutexLocker lock(&mutex);
    state = {};
    state.scenario = QString::fromLatin1(scenario);
    state.width = width;
    state.height = height;
    state.timer.start();
    state.active = true;
}
void setSampleDescriptor(const char* scenario, qint64 width, qint64 height) {
    QMutexLocker lock(&mutex);
    if (!state.active)
        return;
    if (scenario != nullptr && scenario[0] != '\0') {
        state.scenario = QString::fromLatin1(scenario);
    }
    if (width > 0)
        state.width = width;
    if (height > 0)
        state.height = height;
}
void milestone(const char* name) {
    QMutexLocker lock(&mutex);
    Sink* sink = activeSink();
    if (sink == nullptr || !state.active)
        return;
    const QString key = QString::fromLatin1(name);
    if (state.recordedMilestones.contains(key))
        return;
    state.recordedMilestones.insert(key);
    sink->recordMilestone(name, state.timer.nsecsElapsed());
}
void counter(const char* name, qint64 value) {
    QMutexLocker lock(&mutex);
    Sink* sink = activeSink();
    if (sink != nullptr && state.active)
        sink->recordCounter(name, value);
}
Scope::~Scope() {
    const qint64 elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - m_started)
                               .count();
    QMutexLocker lock(&mutex);
    Sink* sink = activeSink();
    if (sink != nullptr && state.active)
        sink->recordScope(m_name, elapsed);
}
void finish(bool success) {
    QMutexLocker lock(&mutex);
    Sink* sink = activeSink();
    if (sink != nullptr && state.active)
        sink->finish(success);
}
} // namespace snow_shot::presentation::recording_perf
#else
namespace snow_shot::presentation::recording_perf {
void configureTrace(const QString&) {}
void installSink(Sink*) {}
void beginSample(const char*, qint64, qint64) {}
void setSampleDescriptor(const char*, qint64, qint64) {}
void milestone(const char*) {}
void counter(const char*, qint64) {}
Scope::~Scope() = default;
void finish(bool) {}
} // namespace snow_shot::presentation::recording_perf
#endif
