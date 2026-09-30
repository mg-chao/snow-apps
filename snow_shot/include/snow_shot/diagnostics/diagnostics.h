#ifndef SNOW_SHOT_DIAGNOSTICS_DIAGNOSTICS_H
#define SNOW_SHOT_DIAGNOSTICS_DIAGNOSTICS_H

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace snow_shot::diagnostics {

struct CrashReport {
    QString id;
    QString path;
    QDateTime created;
    qint64 bytes = 0;
    QJsonObject context;
};

class CrashCollector {
  public:
    virtual ~CrashCollector() = default;
    virtual bool initialize(const QString& directory, const QString& handler,
                            const QString& session, QString* error) = 0;
    virtual QVector<CrashReport> reports() = 0;
    virtual bool removeReport(const QString& id) = 0;
    virtual QString pipeName() const = 0;
    virtual QString databaseDirectory() const {
        return {};
    }
    virtual bool healthy() const {
        return true;
    }
};

struct DiagnosticsOptions {
    QStringList directories;
    QString handlerPath;
    // Stable Crashpad database, independent of the relocatable log directories.
    // The first successful process registration owns this path until process exit.
    QString crashCaptureDirectory;
    QString version;
    QString revision;
    QString buildConfiguration;
    bool installMessageHandler = true;
    bool enableCrashCapture = true;
    bool mirrorToConsole = true;
    qsizetype queueBytes = 4 * 1024 * 1024;
    qsizetype queueRecords = 4096;
    qint64 segmentBytes = 10 * 1024 * 1024;
    qint64 dailyBytes = 50 * 1024 * 1024;
    qint64 totalBytes = 256 * 1024 * 1024;
    std::function<QDateTime()> clock = [] { return QDateTime::currentDateTime(); };
    std::function<bool(const QString&)> removeFile;
    std::function<bool(const QString&, const QByteArray&)> appendFile;
    std::shared_ptr<CrashCollector> crashCollector;
};

struct DiagnosticsStatus {
    QString directory;
    QString currentFile;
    QString fallbackReason;
    QString lastError;
    QString sessionId;
    bool loggingAvailable = false;
    bool crashCaptureAvailable = false;
    bool exporting = false;
    qint64 bytes = 0;
    quint64 droppedRecords = 0;

    friend bool operator==(const DiagnosticsStatus& a, const DiagnosticsStatus& b) {
        return a.directory == b.directory && a.currentFile == b.currentFile &&
               a.fallbackReason == b.fallbackReason && a.lastError == b.lastError &&
               a.sessionId == b.sessionId && a.loggingAvailable == b.loggingAvailable &&
               a.crashCaptureAvailable == b.crashCaptureAvailable && a.exporting == b.exporting &&
               a.bytes == b.bytes && a.droppedRecords == b.droppedRecords;
    }
};

struct LogExportResult {
    bool success = false;
    QString path;
    QString error;
};

class DiagnosticsService final : public QObject {
    Q_OBJECT
  public:
    explicit DiagnosticsService(QObject* parent = nullptr);
    ~DiagnosticsService() override;
    static DiagnosticsService& instance();
    bool initialize(DiagnosticsOptions options);
    void shutdown();
    DiagnosticsOptions options() const;
    DiagnosticsStatus status() const;
    QStringList directories() const;
    QString crashPipeName() const;
    QString crashCaptureDirectory() const;
    void record(QtMsgType level, const QString& category, const QString& event,
                const QString& message = {}, const QJsonObject& fields = {},
                const QMessageLogContext& context = {}) noexcept;
    bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(2));
    void requestMaintenance();
    std::shared_future<LogExportResult> exportDay(const QDate& date);
    void protectSnapshot(const QString& path);
    static QString sanitize(QString text);

  signals:
    void statusChanged();

  private:
    struct Impl;
    // Readers retain their session while a storage migration publishes its replacement.
    std::shared_ptr<Impl> sessionSnapshot() const;
    void publishSession(std::shared_ptr<Impl> session);
    mutable std::mutex m_sessionMutex;
    std::shared_ptr<Impl> m_impl;
};

void logEvent(const QString& category, const QString& event, const QJsonObject& fields = {},
              QtMsgType level = QtInfoMsg) noexcept;
std::shared_ptr<CrashCollector> makeCrashCollector();

} // namespace snow_shot::diagnostics

Q_DECLARE_METATYPE(snow_shot::diagnostics::DiagnosticsStatus)
#endif
