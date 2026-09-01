#include "screenshotpintoperfinstrumentation.h"

#if defined(SNOW_SHOT_PIN_PERF_INSTRUMENTATION)
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
namespace snow_shot::presentation::pin_perf {
namespace {
QMutex mutex;
QFile traceFile;
Sink* activeSink = nullptr;
struct State {
    QElapsedTimer timer;
    QJsonObject spans;
    QJsonObject milestones;
    QJsonObject counters;
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
        if (state.active) state.milestones[QString::fromLatin1(name)] = nanoseconds;
    }
    void recordCounter(const char* name, qint64 value) override {
        if (state.active) {
            const QString key = QString::fromLatin1(name);
            state.counters[key] = state.counters.value(key).toInteger() + value;
        }
    }
    void finish(bool success) override {
        if (!state.active) return;
        QJsonObject object{{QStringLiteral("timestamp_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {QStringLiteral("scenario"), state.scenario},
                           {QStringLiteral("width"), state.width},
                           {QStringLiteral("height"), state.height},
                           {QStringLiteral("success"), success},
                           {QStringLiteral("end_to_end_ns"), state.timer.nsecsElapsed()},
                           {QStringLiteral("spans_ns"), state.spans},
                           {QStringLiteral("milestones_ns"), state.milestones},
                           {QStringLiteral("counters"), state.counters},
                           {QStringLiteral("thread"), QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()))}};
        if (traceFile.isOpen()) {
            traceFile.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
            traceFile.write("\n");
            traceFile.flush();
        }
        state.active = false;
    }
};
FileSink fileSink;
} // namespace

void setSink(Sink* sink) { QMutexLocker lock(&mutex); activeSink = sink; }
void configureTrace(const QString& path) {
    QMutexLocker lock(&mutex);
    if (traceFile.isOpen()) traceFile.close();
    if (!path.isEmpty()) {
        traceFile.setFileName(path);
        static_cast<void>(traceFile.open(QIODevice::WriteOnly | QIODevice::Append));
    }
    activeSink = &fileSink;
}
void beginSample(const char* scenario, qint64 width, qint64 height) {
    if (state.active) fileSink.finish(false);
    QMutexLocker lock(&mutex);
    state = {};
    state.scenario = QString::fromLatin1(scenario);
    state.width = width; state.height = height; state.timer.start(); state.active = true;
}
void setSampleDescriptor(const char* scenario, qint64 width, qint64 height) {
    QMutexLocker lock(&mutex);
    if (!state.active) return;
    if (scenario != nullptr && scenario[0] != '\0') {
        state.scenario = QString::fromLatin1(scenario);
    }
    if (width > 0) state.width = width;
    if (height > 0) state.height = height;
}
void milestone(const char* name) { QMutexLocker lock(&mutex); if (activeSink && state.active) { const qint64 elapsed = state.timer.nsecsElapsed(); activeSink->recordMilestone(name, elapsed); } }
void counter(const char* name, qint64 value) { QMutexLocker lock(&mutex); if (activeSink && state.active) activeSink->recordCounter(name, value); }
Scope::~Scope() {
    const qint64 elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_started).count();
    QMutexLocker lock(&mutex); if (activeSink && state.active) activeSink->recordScope(m_name, elapsed);
}
void finish(bool success) { QMutexLocker lock(&mutex); if (activeSink && state.active) activeSink->finish(success); }
} // namespace snow_shot::presentation::pin_perf
#else
namespace snow_shot::presentation::pin_perf {
void setSink(Sink*) {}
void configureTrace(const QString&) {}
void beginSample(const char*, qint64, qint64) {}
void setSampleDescriptor(const char*, qint64, qint64) {}
void milestone(const char*) {}
void counter(const char*, qint64) {}
Scope::~Scope() = default;
void finish(bool) {}
} // namespace snow_shot::presentation::pin_perf
#endif
