#include "snow_shot/diagnostics/diagnostics.h"
#include "diagnosticsbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace snow_shot::diagnostics {
namespace {
constexpr qsizetype kRecordLimit = 16 * 1024;
std::mutex handlerMutex;
DiagnosticsService* installedService = nullptr;
QtMessageHandler previousHandler = nullptr;
bool consoleOutput = true;
thread_local bool insideHandler = false;
thread_local bool insideRecord = false;

bool linked(const QString& path) {
    const QFileInfo info(path);
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return info.isSymLink();
}

bool safePath(const QString& path) {
    QString current = QFileInfo(path).absoluteFilePath();
    for (;;) {
        if (linked(current)) {
            return false;
        }
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) {
            return true;
        }
        current = parent;
    }
}

bool prepareDirectory(const QString& path) {
    return !path.isEmpty() && QDir::isAbsolutePath(path) && safePath(path) && QDir().mkpath(path) &&
           safePath(path);
}

QString severity(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtInfoMsg:
        return QStringLiteral("INFO");
    case QtWarningMsg:
        return QStringLiteral("WARN");
    case QtCriticalMsg:
        return QStringLiteral("ERROR");
    case QtFatalMsg:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtFatalMsg) {
        snow_diag_fatal("qt.fatal");
        snow_diag_panic(reinterpret_cast<const unsigned char*>("qt.fatal"), 8);
        return;
    }
    if (insideHandler) {
        constexpr char failure[] = "{\"level\":\"ERROR\",\"event\":\"logger.reentrant\"}\n";
        snow_diag_emergency(failure, sizeof(failure) - 1);
        return;
    }
    insideHandler = true;
    try {
        std::lock_guard<std::mutex> lock(handlerMutex);
        if (installedService != nullptr) {
            installedService->record(type, QString::fromUtf8(context.category),
                                     QStringLiteral("qt.message"), message, {}, context);
        }
        if (consoleOutput) {
            if (previousHandler != nullptr) {
                previousHandler(type, context, message);
            } else {
                const QByteArray output = qFormatLogMessage(type, context, message).toUtf8();
                std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stderr);
                std::fputc('\n', stderr);
#ifdef Q_OS_WIN
                OutputDebugStringW(reinterpret_cast<LPCWSTR>((message + u'\n').utf16()));
#endif
            }
        }
    } catch (...) {
    }
    insideHandler = false;
}

struct Artifact {
    QString path;
    QDate date;
    qint64 bytes = 0;
    bool snapshot = false;
    bool text = false;
    bool protectedFile = false;
    QString reportId;
    std::shared_ptr<CrashCollector> collector;
    bool liveSession = false;
    QJsonObject reportContext;
    QDateTime reportCreated;
};

qint64 treeBytes(const QString& path) {
    if (linked(path)) {
        return 0;
    }
    qint64 bytes = 0;
    for (const QFileInfo& entry :
         QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!linked(entry.absoluteFilePath())) {
            bytes += entry.isDir() ? treeBytes(entry.absoluteFilePath()) : entry.size();
        }
    }
    return bytes;
}
} // namespace

struct DiagnosticsService::Impl {
    explicit Impl(DiagnosticsService& service) : owner(service) {}
    struct Task {
        QByteArray record;
        QDate date;
        QtMsgType level = QtInfoMsg;
        std::function<void()> command;
    };
    DiagnosticsService& owner;
    DiagnosticsOptions options;
    mutable std::mutex stateMutex;
    DiagnosticsStatus state;
    QStringList roots;
    QString pipe;
    QString collectorError;
    QString emergencyPath;
    QString markerPath;
    QString protectedSnapshot;
    QString pendingSnapshot;
    std::unique_ptr<QLockFile> sessionLock;
    std::unique_ptr<QLockFile> outputLock;
    std::unique_ptr<QLockFile> emergencyLock;
    std::unique_ptr<QLockFile> snapshotLock;
    QDate emergencyDate;
    std::shared_ptr<CrashCollector> collector;
    QSet<QString> reportedCrashes;
    QFile output;
    QDate outputDate;
    quint64 part = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Task> tasks;
    qsizetype queuedBytes = 0;
    qsizetype queuedRecords = 0;
    quint64 sequence = 0;
    quint64 dropped = 0;
    quint64 reportedDrops = 0;
    bool stopping = false;
    bool running = false;
    bool budgetBlocked = false;
    bool ownsHandler = false;
    bool rotated = false;
    bool maintenancePending = false;
    bool maintenanceRunning = false;
    bool writeFailed = false;
    std::chrono::steady_clock::time_point lastFlush;
    std::chrono::steady_clock::time_point lastWake;
    QDateTime lastWallTime;
    std::thread worker;
    std::chrono::steady_clock::time_point lastMaintenance;
    QDate maintenanceDate;
    QMap<QDate, qint64> dailyBytes;

    void notify() {
        emit owner.statusChanged();
    }
    void fail(const QString& error) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            changed = state.lastError != error;
            state.lastError = error;
        }
        if (changed)
            notify();
    }
    bool remove(const QString& path) {
        return safePath(path) &&
               (!QFileInfo::exists(path) ||
                (options.removeFile ? options.removeFile(path) : QFile::remove(path)));
    }
    bool active(const QString& root, const QString& session) const {
        if (root == state.directory && session == state.sessionId) {
            return true;
        }
        QLockFile lock(QDir(root).filePath(QStringLiteral("session-%1.lock").arg(session)));
        lock.setStaleLockTime(0);
        return !lock.tryLock(0);
    }
    bool fileActive(const QString& path) const {
        QLockFile lock(path + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        return !lock.tryLock(0);
    }
    void rotateEmergency(const QDate& date) {
        if (!options.installMessageHandler || emergencyDate == date)
            return;
        const QString directory = QDir(state.directory).filePath(date.toString(Qt::ISODate));
        if (!prepareDirectory(directory))
            return;
        const QString path =
            QDir(directory).filePath(QStringLiteral("emergency-%1.log").arg(state.sessionId));
        auto lock = std::make_unique<QLockFile>(path + QStringLiteral(".lock"));
        lock->setStaleLockTime(0);
        if (!safePath(path) || !lock->tryLock(0))
            return;
        snow_diag_open_emergency(path.toUtf8().constData());
        emergencyLock = std::move(lock);
        emergencyPath = path;
        emergencyDate = date;
    }
    void recoverSessions() {
        static const QRegularExpression name(QStringLiteral("^session-([a-f0-9]{32})\\.open$"));
        for (const QString& root : roots) {
            if (!safePath(root))
                continue;
            for (const QFileInfo& file :
                 QDir(root).entryInfoList({QStringLiteral("session-*.open")}, QDir::Files)) {
                const auto match = name.match(file.fileName());
                if (!match.hasMatch() || active(root, match.captured(1)) ||
                    !safePath(file.absoluteFilePath()))
                    continue;
                internal(QtWarningMsg, QStringLiteral("session.unexpected_termination"),
                         {{QStringLiteral("previous_session"), match.captured(1)}});
                remove(file.absoluteFilePath());
            }
        }
    }
    QByteArray encode(QtMsgType level, const QString& category, const QString& event,
                      const QString& message, const QJsonObject& fields,
                      const QMessageLogContext& context, const QDateTime& now) {
        QJsonObject record{
            {QStringLiteral("time"), now.toString(Qt::ISODateWithMs)},
            {QStringLiteral("level"), severity(level)},
            {QStringLiteral("category"), category.left(128)},
            {QStringLiteral("event"), event.left(128)},
            {QStringLiteral("pid"), QCoreApplication::applicationPid()},
            {QStringLiteral("tid"),
             QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()))},
            {QStringLiteral("session"), state.sessionId},
            {QStringLiteral("sequence"), static_cast<qint64>(++sequence)},
            {QStringLiteral("message"), DiagnosticsService::sanitize(message.left(8192))}};
        QJsonObject safeFields;
        // Callers supply metadata only; deny content-bearing keys as a second boundary.
        static const QSet<QString> allowed{QStringLiteral("operation"),
                                           QStringLiteral("request_kind"),
                                           QStringLiteral("duration_ms"),
                                           QStringLiteral("code"),
                                           QStringLiteral("status"),
                                           QStringLiteral("backend"),
                                           QStringLiteral("version"),
                                           QStringLiteral("revision"),
                                           QStringLiteral("build"),
                                           QStringLiteral("qt"),
                                           QStringLiteral("os"),
                                           QStringLiteral("arch"),
                                           QStringLiteral("count"),
                                           QStringLiteral("child_pid"),
                                           QStringLiteral("exit_code"),
                                           QStringLiteral("outcome"),
                                           QStringLiteral("dump_id"),
                                           QStringLiteral("dump_file"),
                                           QStringLiteral("created"),
                                           QStringLiteral("previous_session"),
                                           QStringLiteral("thread_id"),
                                           QStringLiteral("exception_code"),
                                           QStringLiteral("exception_address")};
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (allowed.contains(it.key()) && !it.value().isObject() && !it.value().isArray()) {
                safeFields.insert(it.key(), it.value().isString()
                                                ? QJsonValue(DiagnosticsService::sanitize(
                                                      it.value().toString().left(512)))
                                                : it.value());
            }
        }
        record.insert(QStringLiteral("fields"), safeFields);
        if (context.file != nullptr) {
            QString source = QString::fromUtf8(context.file).replace(u'\\', u'/');
            const qsizetype project = source.indexOf(QStringLiteral("snow_shot/"));
            source = project >= 0 ? source.mid(project) : QFileInfo(source).fileName();
            record.insert(QStringLiteral("source"), source.left(512));
            record.insert(QStringLiteral("line"), context.line);
        }
        QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
        if (bytes.size() >= kRecordLimit) {
            record.insert(QStringLiteral("message"), QStringLiteral("[record truncated]"));
            record.insert(QStringLiteral("fields"), QJsonObject{});
            record.insert(QStringLiteral("truncated"), true);
            bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
        }
        return bytes + '\n';
    }
    void internal(QtMsgType level, const QString& event, const QJsonObject& fields = {}) {
        owner.record(level, QStringLiteral("snow_shot.diagnostics"), event, {}, fields);
    }
    void write(const Task& task) {
        if (task.record.isEmpty()) {
            return;
        }
        if (budgetBlocked) {
            std::lock_guard<std::mutex> lock(mutex);
            ++dropped;
            std::lock_guard<std::mutex> stateLock(stateMutex);
            state.droppedRecords = dropped;
            return;
        }
        if (owner.status().bytes + task.record.size() > options.totalBytes ||
            dailyBytes.value(task.date) + task.record.size() > options.dailyBytes) {
            maintenance(task.record.size(), task.date);
            if (budgetBlocked)
                return;
        }
        if (!output.isOpen() || outputDate != task.date ||
            output.size() + task.record.size() > options.segmentBytes) {
            output.close();
            outputLock.reset();
            if (!maintenanceRunning)
                maintenance(task.record.size());
            if (budgetBlocked)
                return;
            const QString directory =
                QDir(state.directory).filePath(task.date.toString(Qt::ISODate));
            if (!prepareDirectory(directory)) {
                fail(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("DiagnosticsService", "The log directory is not writable.")));
                return;
            }
            output.setFileName(QDir(directory).filePath(QStringLiteral("snow-shot-%1-%2.log")
                                                            .arg(state.sessionId)
                                                            .arg(++part, 6, 10, u'0')));
            outputLock = std::make_unique<QLockFile>(output.fileName() + QStringLiteral(".lock"));
            outputLock->setStaleLockTime(0);
            if (!safePath(output.fileName()) || !outputLock->tryLock(0) ||
                !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                fail(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("DiagnosticsService", "The log file could not be opened.")));
                std::lock_guard<std::mutex> lock(stateMutex);
                state.loggingAvailable = false;
                return;
            }
            outputDate = task.date;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                state.currentFile = output.fileName();
                state.loggingAvailable = true;
            }
            rotated = true;
        }
        const bool success = options.appendFile ? options.appendFile(output.fileName(), task.record)
                                                : output.write(task.record) == task.record.size();
        if (!success) {
            writeFailed = true;
            snow_diag_emergency(task.record.constData(), static_cast<size_t>(task.record.size()));
            fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                "DiagnosticsService", "Writing the log file failed. Check available disk space.")));
            std::lock_guard<std::mutex> lock(stateMutex);
            state.loggingAvailable = false;
        } else {
            std::lock_guard<std::mutex> lock(stateMutex);
            state.bytes += task.record.size();
            state.loggingAvailable = true;
            dailyBytes[task.date] += task.record.size();
        }
        if (rotated) {
            notify();
        }
    }

    QVector<Artifact> artifacts() {
        QVector<Artifact> result;
        static const QRegularExpression logName(
            QStringLiteral("^snow-shot-([a-f0-9]{32})-[0-9]{6,}\\.log$"));
        static const QRegularExpression emergencyName(
            QStringLiteral("^emergency-([a-f0-9]{32})\\.log$"));
        for (const QString& root : roots) {
            if (!safePath(root) || !QFileInfo(root).isDir()) {
                continue;
            }
            for (const QFileInfo& day :
                 QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QDate date = QDate::fromString(day.fileName(), Qt::ISODate);
                if (!date.isValid() || date.toString(Qt::ISODate) != day.fileName() ||
                    linked(day.absoluteFilePath())) {
                    continue;
                }
                for (const QFileInfo& file :
                     QDir(day.absoluteFilePath()).entryInfoList(QDir::Files)) {
                    const auto match = logName.match(file.fileName());
                    const auto emergency = emergencyName.match(file.fileName());
                    if ((match.hasMatch() || emergency.hasMatch()) &&
                        !linked(file.absoluteFilePath())) {
                        result.push_back({file.absoluteFilePath(),
                                          date,
                                          QFileInfo(file.absoluteFilePath()).size(),
                                          false,
                                          true,
                                          fileActive(file.absoluteFilePath()),
                                          {},
                                          {}});
                        result.back().liveSession = result.back().protectedFile;
                    }
                }
            }
            for (const QFileInfo& file : QDir(root).entryInfoList(QDir::Files)) {
                const auto match = emergencyName.match(file.fileName());
                if (match.hasMatch() && !linked(file.absoluteFilePath())) {
                    result.push_back({file.absoluteFilePath(),
                                      file.lastModified().date(),
                                      QFileInfo(file.absoluteFilePath()).size(),
                                      false,
                                      true,
                                      active(root, match.captured(1)),
                                      {},
                                      {}});
                    result.back().liveSession = result.back().protectedFile;
                }
            }
            const QString exports = QDir(root).filePath(QStringLiteral("exports"));
            QString publishedSnapshot;
            const QString publicationPath =
                QDir(root).filePath(QStringLiteral("clipboard-snapshot.json"));
            if (safePath(publicationPath)) {
                QFile publication(publicationPath);
                if (publication.open(QIODevice::ReadOnly)) {
                    publishedSnapshot = QJsonDocument::fromJson(publication.read(4096))
                                            .object()
                                            .value(QStringLiteral("path"))
                                            .toString();
                }
            }
            if (safePath(exports)) {
                for (const QFileInfo& folder :
                     QDir(exports).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    const QString name = folder.fileName();
                    const QDate date = QDate::fromString(name.left(10), Qt::ISODate);
                    static const QRegularExpression exportName(
                        QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-f0-9]{32}$"));
                    if (!date.isValid() || !exportName.match(name).hasMatch() ||
                        !safePath(folder.absoluteFilePath())) {
                        continue;
                    }
                    const QString file =
                        QDir(folder.absoluteFilePath())
                            .filePath(
                                QStringLiteral("snow-shot-%1.log").arg(date.toString(Qt::ISODate)));
                    if (QFileInfo(file).isFile() && safePath(file)) {
                        result.push_back(
                            {file,
                             date,
                             QFileInfo(file).size(),
                             true,
                             false,
                             file == protectedSnapshot || file == pendingSnapshot ||
                                 QDir(root).relativeFilePath(file) == publishedSnapshot ||
                                 fileActive(file),
                             {},
                             {}});
                    }
                }
            }
            auto reportCollector = collector;
            if (root != state.directory) {
                const QString database = QDir(root).filePath(QStringLiteral("crashes"));
                if (!safePath(database) || !QFileInfo(database).isDir()) {
                    continue;
                }
                reportCollector = makeCrashCollector();
                QString error;
                if (!reportCollector->initialize(database, {}, {}, &error)) {
                    continue;
                }
            }
            if (reportCollector) {
                for (const CrashReport& report : reportCollector->reports()) {
                    if (safePath(report.path)) {
                        result.push_back({report.path, report.created.toLocalTime().date(),
                                          report.bytes, false, false, false, report.id,
                                          reportCollector});
                        result.back().reportContext = report.context;
                        result.back().reportCreated = report.created;
                        if (!reportedCrashes.contains(report.id)) {
                            reportedCrashes.insert(report.id);
                            QJsonObject context = report.context;
                            context.insert(QStringLiteral("dump_id"), report.id);
                            context.insert(QStringLiteral("dump_file"),
                                           QFileInfo(report.path).fileName());
                            context.insert(QStringLiteral("created"),
                                           report.created.toString(Qt::ISODateWithMs));
                            internal(QtCriticalMsg, QStringLiteral("crash.report_available"),
                                     context);
                        }
                    }
                }
            }
        }
        return result;
    }

    void maintenance(qint64 reservation = 0, QDate reservationDate = {}) {
        if (maintenanceRunning)
            return;
        maintenanceRunning = true;
        struct Reset {
            bool& flag;
            ~Reset() {
                flag = false;
            }
        } reset{maintenanceRunning};
        if (output.isOpen())
            output.flush();
        if (output.isOpen() && outputDate != options.clock().date()) {
            output.close();
            outputLock.reset();
        }
        rotateEmergency(options.clock().date());
        QLockFile maintenanceLock(
            QDir(state.directory).filePath(QStringLiteral("maintenance.lock")));
        maintenanceLock.setStaleLockTime(0);
        if (!maintenanceLock.tryLock(0)) {
            return;
        }
        const QDate today = options.clock().date();
        if (collector && !pipe.isEmpty()) {
            const bool available = collector->healthy();
            std::lock_guard<std::mutex> lock(stateMutex);
            state.crashCaptureAvailable = available;
            if (!available && state.lastError.isEmpty())
                state.lastError = QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "DiagnosticsService", "The crash collector could not be started. Check the "
                                          "application installation."));
        }
        auto files = artifacts();
        qint64 total = 0;
        for (const auto& file : files) {
            if (!file.collector)
                total += file.bytes;
        }
        for (const QString& root : roots) {
            const QString database = QDir(root).filePath(QStringLiteral("crashes"));
            if (safePath(database)) {
                total += treeBytes(database);
            }
        }
        std::sort(files.begin(), files.end(), [](const Artifact& a, const Artifact& b) {
            if (a.snapshot != b.snapshot)
                return a.snapshot;
            if (a.date != b.date)
                return a.date < b.date;
            return QFileInfo(a.path).lastModified() < QFileInfo(b.path).lastModified();
        });
        QMap<QDate, qint64> daily;
        for (const auto& file : files) {
            if (file.text)
                daily[file.date] += file.bytes;
        }
        quint64 removedCount = 0;
        bool deletionFailed = false;
        // Age precedes budget eviction, even when an unexpired snapshot is obsolete.
        std::stable_partition(files.begin(), files.end(), [today](const Artifact& file) {
            return file.date < today.addDays(-6);
        });
        for (const auto& file : files) {
            const bool expired = file.date < today.addDays(-6);
            if (file.liveSession || (file.protectedFile && !expired) ||
                (output.isOpen() && file.path == output.fileName()) || file.path == emergencyPath) {
                continue;
            }
            if (!expired && total + reservation <= options.totalBytes &&
                (!file.text ||
                 daily[file.date] + (file.date == reservationDate ? reservation : 0) <=
                     options.dailyBytes)) {
                continue;
            }
            const bool removed =
                file.collector ? file.collector->removeReport(file.reportId) : remove(file.path);
            if (removed) {
                total -= file.bytes;
                if (file.text)
                    daily[file.date] -= file.bytes;
                ++removedCount;
                if (file.collector) {
                    internal(QtWarningMsg, QStringLiteral("retention.dump_removed"),
                             {{QStringLiteral("dump_id"), file.reportId},
                              {QStringLiteral("outcome"),
                               expired ? QStringLiteral("expired") : QStringLiteral("budget")}});
                }
                const QString parent = QFileInfo(file.path).absolutePath();
                if (!file.collector && safePath(parent))
                    QDir().rmdir(parent);
            } else {
                deletionFailed = true;
                fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "DiagnosticsService", "Some expired diagnostics could not be removed.")));
            }
        }
        // Recount database metadata as well as dumps after Crashpad has completed deletion.
        total = 0;
        for (const auto& file : files) {
            if (!file.collector && safePath(file.path))
                total += QFileInfo(file.path).size();
        }
        for (const QString& root : roots) {
            const QString database = QDir(root).filePath(QStringLiteral("crashes"));
            if (safePath(database))
                total += treeBytes(database);
        }
        dailyBytes = daily;
        budgetBlocked = total + reservation > options.totalBytes || deletionFailed ||
                        (reservationDate.isValid() &&
                         daily.value(reservationDate) + reservation > options.dailyBytes) ||
                        daily.value(today) > options.dailyBytes;
        if (budgetBlocked) {
            fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                "DiagnosticsService", "The diagnostics storage limit has been reached.")));
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            state.bytes = std::max<qint64>(total, 0);
            if (!budgetBlocked &&
                (state.lastError ==
                     QStringLiteral("The diagnostics storage limit has been reached.") ||
                 state.lastError ==
                     QStringLiteral("Some expired diagnostics could not be removed.")))
                state.lastError = collectorError;
        }
        lastMaintenance = std::chrono::steady_clock::now();
        maintenanceDate = today;
        if (removedCount > 0 && !budgetBlocked) {
            internal(QtInfoMsg, QStringLiteral("retention.removed"),
                     {{QStringLiteral("count"), static_cast<qint64>(removedCount)}});
        }
        notify();
    }

    LogExportResult exportNow(const QDate& date) {
        if (writeFailed || (output.isOpen() && !output.flush())) {
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP(
                        "DiagnosticsService",
                        "Writing the log file failed. Check available disk space."))};
        }
        QByteArray combined;
        auto files = artifacts();
        std::sort(files.begin(), files.end(),
                  [](const Artifact& a, const Artifact& b) { return a.path < b.path; });
        for (const auto& entry : files) {
            if (!entry.text ||
                (entry.date != date &&
                 !QFileInfo(entry.path).fileName().startsWith(QStringLiteral("emergency-"))) ||
                !safePath(entry.path))
                continue;
            QFile file(entry.path);
            if (!file.open(QIODevice::ReadOnly)) {
                return {false,
                        {},
                        QString::fromUtf8(QT_TRANSLATE_NOOP("DiagnosticsService",
                                                            "A log file could not be read."))};
            }
            const qint64 limit = file.size();
            while (file.pos() < limit) {
                const QByteArray line = file.readLine(kRecordLimit + 1);
                if (line.isEmpty())
                    break;
                if (!line.endsWith('\n') || file.pos() > limit)
                    continue;
                const auto document = QJsonDocument::fromJson(line);
                if (!document.isObject())
                    continue;
                const QDate recordDate =
                    QDateTime::fromString(
                        document.object().value(QStringLiteral("time")).toString(),
                        Qt::ISODateWithMs)
                        .toLocalTime()
                        .date();
                if (recordDate.isValid() && recordDate != date)
                    continue;
                if (combined.size() + line.size() > options.dailyBytes) {
                    return {false,
                            {},
                            QString::fromUtf8(QT_TRANSLATE_NOOP(
                                "DiagnosticsService", "Today's log exceeds the export limit."))};
                }
                combined += line;
            }
        }
        {
            for (const Artifact& report : files) {
                if (!report.collector || report.date != date)
                    continue;
                auto context = report.reportContext;
                context.insert(QStringLiteral("dump_id"), report.reportId);
                context.insert(QStringLiteral("created"),
                               report.reportCreated.toString(Qt::ISODateWithMs));
                std::lock_guard<std::mutex> lock(mutex);
                combined += encode(QtCriticalMsg, QStringLiteral("snow_shot.diagnostics"),
                                   QStringLiteral("crash.summary"), {}, context, {},
                                   report.reportCreated.toLocalTime());
            }
        }
        if (combined.size() > options.dailyBytes + 1024 * 1024) {
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP("DiagnosticsService",
                                                        "Today's log exceeds the export limit."))};
        }
        if (combined.isEmpty()) {
            std::lock_guard<std::mutex> lock(mutex);
            combined = encode(QtInfoMsg, QStringLiteral("snow_shot.diagnostics"),
                              QStringLiteral("export.day_status"), {}, {}, {},
                              QDateTime(date, QTime(0, 0), options.clock().timeZone()));
        }
        pendingSnapshot.clear();
        snapshotLock.reset();
        maintenance(combined.size());
        if (budgetBlocked) {
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP(
                        "DiagnosticsService", "There is not enough space for a log snapshot."))};
        }
        const QString directory =
            QDir(state.directory)
                .filePath(QStringLiteral("exports/%1-%2")
                              .arg(date.toString(Qt::ISODate),
                                   QUuid::createUuid().toString(QUuid::Id128)));
        if (!prepareDirectory(directory)) {
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP(
                        "DiagnosticsService", "The log snapshot directory could not be created."))};
        }
        const QString path = QDir(directory).filePath(
            QStringLiteral("snow-shot-%1.log").arg(date.toString(Qt::ISODate)));
        snapshotLock = std::make_unique<QLockFile>(path + QStringLiteral(".lock"));
        snapshotLock->setStaleLockTime(0);
        if (!snapshotLock->tryLock(0))
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP("DiagnosticsService",
                                                        "The log snapshot could not be saved."))};
        QSaveFile snapshot(path);
        if (!snapshot.open(QIODevice::WriteOnly) || snapshot.write(combined) != combined.size() ||
            !snapshot.commit()) {
            return {false,
                    {},
                    QString::fromUtf8(QT_TRANSLATE_NOOP("DiagnosticsService",
                                                        "The log snapshot could not be saved."))};
        }
        pendingSnapshot = path;
        maintenance();
        return {true, path, {}};
    }

    void loop() {
        try {
            for (;;) {
                Task task;
                quint64 dropCount = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_for(lock, std::chrono::seconds(1),
                                  [this] { return stopping || !tasks.empty(); });
                    if (tasks.empty() && stopping)
                        break;
                    if (!tasks.empty()) {
                        task = std::move(tasks.front());
                        tasks.pop_front();
                        if (!task.record.isEmpty()) {
                            queuedBytes -= task.record.size();
                            --queuedRecords;
                        }
                    }
                    if (!budgetBlocked) {
                        dropCount = dropped - reportedDrops;
                        reportedDrops = dropped;
                    }
                }
                if (task.command)
                    task.command();
                else
                    write(task);
                if (dropCount > 0) {
                    internal(QtWarningMsg, QStringLiteral("logger.records_dropped"),
                             {{QStringLiteral("count"), static_cast<qint64>(dropCount)}});
                }
                const auto steadyNow = std::chrono::steady_clock::now();
                if (output.isOpen() && (task.level == QtWarningMsg || task.level == QtCriticalMsg ||
                                        steadyNow - lastFlush >= std::chrono::seconds(1))) {
                    if (!output.flush()) {
                        writeFailed = true;
                        fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                            "DiagnosticsService",
                            "Writing the log file failed. Check available disk space.")));
                    }
                    lastFlush = steadyNow;
                }
                const auto wallNow = options.clock();
                const bool timeJump =
                    lastWallTime.isValid() &&
                    (steadyNow - lastWake > std::chrono::seconds(5) ||
                     std::abs(
                         lastWallTime.msecsTo(wallNow) -
                         std::chrono::duration_cast<std::chrono::milliseconds>(steadyNow - lastWake)
                             .count()) > 5000);
                lastWake = steadyNow;
                lastWallTime = wallNow;
                const bool newDate = options.clock().date() != maintenanceDate;
                if (newDate && maintenanceDate.isValid()) {
                    internal(QtInfoMsg, QStringLiteral("session.date_changed"));
                }
                if (rotated || timeJump || newDate ||
                    std::chrono::steady_clock::now() - lastMaintenance >=
                        std::chrono::minutes(15) ||
                    (output.isOpen() && output.size() >= options.segmentBytes)) {
                    rotated = false;
                    maintenance();
                }
            }
            if (output.isOpen() && !output.flush())
                writeFailed = true;
            output.close();
            outputLock.reset();
        } catch (...) {
            fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                "DiagnosticsService", "The diagnostics writer stopped unexpectedly.")));
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            tasks.clear();
        }
    }
};

DiagnosticsService::DiagnosticsService(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}
DiagnosticsService::~DiagnosticsService() {
    shutdown();
}
DiagnosticsService& DiagnosticsService::instance() {
    static DiagnosticsService service;
    return service;
}

bool DiagnosticsService::initialize(DiagnosticsOptions options) {
    shutdown();
    m_impl = std::make_unique<Impl>(*this);
    auto& impl = *m_impl;
    impl.options = std::move(options);
    impl.options.segmentBytes = std::max<qint64>(kRecordLimit, impl.options.segmentBytes);
    impl.options.dailyBytes = std::max(impl.options.segmentBytes, impl.options.dailyBytes);
    impl.options.queueBytes = std::max<qsizetype>(kRecordLimit, impl.options.queueBytes);
    impl.options.queueRecords = std::max<qsizetype>(1, impl.options.queueRecords);
    impl.state.sessionId = QUuid::createUuid().toString(QUuid::Id128);
    for (const QString& candidate : impl.options.directories) {
        if (!candidate.isEmpty() && QDir::isAbsolutePath(candidate)) {
            const QString root = QDir::cleanPath(candidate);
            if (!impl.roots.contains(root, Qt::CaseInsensitive))
                impl.roots.append(root);
        }
    }
    for (const QString& root : impl.roots) {
        if (!prepareDirectory(root))
            continue;
        QFile probe(QDir(root).filePath(QStringLiteral("probe-%1").arg(impl.state.sessionId)));
        if (!probe.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            continue;
        probe.close();
        probe.remove();
        impl.state.directory = root;
        break;
    }
    if (impl.state.directory.isEmpty()) {
        impl.fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "DiagnosticsService", "No writable diagnostics directory is available.")));
        return false;
    }
    if (impl.state.directory != impl.roots.value(0)) {
        impl.state.fallbackReason = QString::fromUtf8(
            QT_TRANSLATE_NOOP("DiagnosticsService",
                              "The preferred log directory is unavailable; a fallback is in use."));
    }
    impl.sessionLock = std::make_unique<QLockFile>(
        QDir(impl.state.directory)
            .filePath(QStringLiteral("session-%1.lock").arg(impl.state.sessionId)));
    impl.sessionLock->setStaleLockTime(0);
    if (!impl.sessionLock->tryLock(0))
        return false;
    if (impl.options.installMessageHandler)
        snow_diag_prepare(impl.state.sessionId.toUtf8().constData(),
                          impl.options.version.toUtf8().constData(),
                          impl.options.revision.toUtf8().constData());
    impl.rotateEmergency(impl.options.clock().date());
    if (impl.options.enableCrashCapture) {
        impl.collector =
            impl.options.crashCollector ? impl.options.crashCollector : makeCrashCollector();
        QString error;
        const QString database = QDir(impl.state.directory).filePath(QStringLiteral("crashes"));
        if (prepareDirectory(database)) {
            impl.state.crashCaptureAvailable = impl.collector->initialize(
                database, impl.options.handlerPath, impl.state.sessionId, &error);
        }
        if (!impl.state.crashCaptureAvailable)
            impl.state.lastError = error;
        impl.collectorError = error;
        impl.pipe = impl.collector->pipeName();
    }
    impl.running = true;
    impl.markerPath = QDir(impl.state.directory)
                          .filePath(QStringLiteral("session-%1.open").arg(impl.state.sessionId));
    QFile marker(impl.markerPath);
    if (marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        marker.write(impl.options.clock().toString(Qt::ISODateWithMs).toUtf8());
        marker.close();
    }
    impl.worker = std::thread([&impl] { impl.loop(); });
    if (impl.options.installMessageHandler) {
        std::lock_guard<std::mutex> lock(handlerMutex);
        installedService = this;
        consoleOutput = impl.options.mirrorToConsole;
        previousHandler = qInstallMessageHandler(messageHandler);
        impl.ownsHandler = true;
    }
    record(QtInfoMsg, QStringLiteral("snow_shot.app"), QStringLiteral("session.start"), {},
           {{QStringLiteral("version"), impl.options.version},
            {QStringLiteral("revision"), impl.options.revision},
            {QStringLiteral("build"), impl.options.buildConfiguration},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
            {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()}});
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.tasks.push_back({{}, {}, QtInfoMsg, [&impl] { impl.recoverSessions(); }});
    }
    requestMaintenance();
    static_cast<void>(flush());
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.running && !impl.stopping;
}

void DiagnosticsService::shutdown() {
    auto& impl = *m_impl;
    if (!impl.running)
        return;
    record(QtInfoMsg, QStringLiteral("snow_shot.app"), QStringLiteral("session.clean_shutdown"));
    if (impl.ownsHandler) {
        std::lock_guard<std::mutex> lock(handlerMutex);
        if (installedService == this) {
            qInstallMessageHandler(previousHandler);
            installedService = nullptr;
            previousHandler = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.stopping = true;
    }
    impl.wake.notify_all();
    if (impl.worker.joinable())
        impl.worker.join();
    if (impl.state.loggingAvailable && !impl.writeFailed)
        impl.remove(impl.markerPath);
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.running = false;
    }
    impl.sessionLock.reset();
    if (impl.ownsHandler)
        snow_diag_shutdown();
    impl.emergencyLock.reset();
}

DiagnosticsStatus DiagnosticsService::status() const {
    std::lock_guard<std::mutex> lock(m_impl->stateMutex);
    return m_impl->state;
}
QStringList DiagnosticsService::directories() const {
    return m_impl->roots;
}
QString DiagnosticsService::crashPipeName() const {
    return m_impl->pipe;
}

void DiagnosticsService::record(QtMsgType level, const QString& category, const QString& event,
                                const QString& message, const QJsonObject& fields,
                                const QMessageLogContext& context) noexcept {
    try {
        if (level == QtFatalMsg) {
            snow_diag_fatal("logger.fatal");
            snow_diag_panic(reinterpret_cast<const unsigned char*>("logger.fatal"), 12);
            return;
        }
        if (insideRecord)
            return;
        insideRecord = true;
        struct Reset {
            ~Reset() {
                insideRecord = false;
            }
        } reset;
        auto& impl = *m_impl;
        std::lock_guard<std::mutex> lock(impl.mutex);
        if (!impl.running || impl.stopping || level == QtDebugMsg)
            return;
        const QDateTime now = impl.options.clock();
        QByteArray bytes = impl.encode(level, category, event, message, fields, context, now);
        snow_diag_breadcrumb(bytes.constData(), static_cast<size_t>(bytes.size()));
        const auto full = [&] {
            return impl.queuedBytes + bytes.size() > impl.options.queueBytes ||
                   impl.queuedRecords >= impl.options.queueRecords;
        };
        while (full() && level != QtInfoMsg) {
            const auto expendable =
                std::find_if(impl.tasks.begin(), impl.tasks.end(), [](const Impl::Task& task) {
                    return !task.record.isEmpty() && task.level == QtInfoMsg;
                });
            if (expendable != impl.tasks.end()) {
                impl.queuedBytes -= expendable->record.size();
                --impl.queuedRecords;
                impl.tasks.erase(expendable);
                ++impl.dropped;
            } else
                break;
        }
        if (full()) {
            ++impl.dropped;
        } else {
            impl.queuedBytes += bytes.size();
            ++impl.queuedRecords;
            impl.tasks.push_back({std::move(bytes), now.date(), level, {}});
        }
        {
            std::lock_guard<std::mutex> stateLock(impl.stateMutex);
            impl.state.droppedRecords = impl.dropped;
        }
        impl.wake.notify_one();
    } catch (...) {
        constexpr char failure[] = "{\"level\":\"ERROR\",\"event\":\"logger.failure\"}\n";
        snow_diag_emergency(failure, sizeof(failure) - 1);
    }
}

bool DiagnosticsService::flush(std::chrono::milliseconds timeout) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!m_impl->running || m_impl->stopping || m_impl->tasks.size() >= 4160)
            return false;
        m_impl->tasks.push_back(
            {{}, {}, QtInfoMsg, [this, promise] {
                 const bool flushed = m_impl->output.isOpen() && m_impl->output.flush();
                 promise->set_value(flushed && !m_impl->writeFailed && status().loggingAvailable);
             }});
    }
    m_impl->wake.notify_one();
    if (future.wait_for(timeout) != std::future_status::ready)
        return false;
    try {
        return future.get();
    } catch (...) {
        return false;
    }
}

void DiagnosticsService::requestMaintenance() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->running || m_impl->stopping || m_impl->maintenancePending)
        return;
    m_impl->maintenancePending = true;
    m_impl->tasks.push_back({{}, {}, QtInfoMsg, [this] {
                                 {
                                     std::lock_guard<std::mutex> lock(m_impl->mutex);
                                     m_impl->maintenancePending = false;
                                 }
                                 m_impl->maintenance();
                             }});
    m_impl->wake.notify_one();
}

std::shared_future<LogExportResult> DiagnosticsService::exportDay(const QDate& date) {
    auto promise = std::make_shared<std::promise<LogExportResult>>();
    auto future = promise->get_future().share();
    record(QtInfoMsg, QStringLiteral("snow_shot.diagnostics"), QStringLiteral("export.snapshot"));
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    {
        std::lock_guard<std::mutex> stateLock(m_impl->stateMutex);
        if (!m_impl->running || m_impl->stopping || m_impl->state.exporting || !date.isValid()) {
            promise->set_value(
                {false,
                 {},
                 QString::fromUtf8(QT_TRANSLATE_NOOP(
                     "DiagnosticsService", "Log export is unavailable or already running."))});
            return future;
        }
        m_impl->state.exporting = true;
    }
    m_impl->tasks.push_back({{}, {}, QtInfoMsg, [this, date, promise] {
                                 LogExportResult result;
                                 try {
                                     result = m_impl->exportNow(date);
                                 } catch (...) {
                                     result.error = QString::fromUtf8(
                                         QT_TRANSLATE_NOOP("DiagnosticsService",
                                                           "The log snapshot could not be saved."));
                                 }
                                 {
                                     std::lock_guard<std::mutex> stateLock(m_impl->stateMutex);
                                     m_impl->state.exporting = false;
                                 }
                                 promise->set_value(std::move(result));
                                 m_impl->notify();
                             }});
    m_impl->wake.notify_one();
    m_impl->notify();
    return future;
}

void DiagnosticsService::protectSnapshot(const QString& path) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->running || m_impl->stopping)
        return;
    m_impl->tasks.push_back(
        {{}, {}, QtInfoMsg, [this, path] {
             m_impl->protectedSnapshot = path;
             m_impl->pendingSnapshot.clear();
             QSaveFile publication(
                 QDir(m_impl->state.directory).filePath(QStringLiteral("clipboard-snapshot.json")));
             const QByteArray data =
                 QJsonDocument(QJsonObject{{QStringLiteral("path"),
                                            QDir(m_impl->state.directory).relativeFilePath(path)}})
                     .toJson(QJsonDocument::Compact);
             if (!publication.open(QIODevice::WriteOnly) ||
                 publication.write(data) != data.size() || !publication.commit()) {
                 m_impl->fail(QString::fromUtf8(QT_TRANSLATE_NOOP(
                     "DiagnosticsService", "The log snapshot could not be saved.")));
             }
             m_impl->snapshotLock.reset();
             m_impl->maintenance();
         }});
    m_impl->wake.notify_one();
}

QString DiagnosticsService::sanitize(QString text) {
    static const QRegularExpression secret(QStringLiteral(
        "(?i)\\bauthorization\\s*[:=]\\s*(?:(?:bearer|basic)\\s+)?[^\\s,;\"']+|bearer\\s+[^\\s\"']+"
        "|\\b(?:api[_-]?key|token|password|secret)\\b\\s*[:=]\\s*[^\\s,;\"']+"));
    text.replace(secret, QStringLiteral("[redacted]"));
    static const QRegularExpression quotedSecret(QStringLiteral(
        R"((?i)\b(?:authorization|api[_-]?key|(?:access[_-]?|refresh[_-]?)?token|password|(?:client[_-]?)?secret)\b["']?\s*[:=]\s*(?:"[^"\r\n]*"|'[^'\r\n]*'|[^\s,;}]+))"));
    text.replace(quotedSecret, QStringLiteral("[redacted]"));
    static const QRegularExpression urlCredentials(
        QStringLiteral(R"((https?://)[^\s/@]+:[^\s/@]+@)"));
    text.replace(urlCredentials, QStringLiteral("\\1[redacted]@"));
    static const QRegularExpression urlQuery(QStringLiteral("(https?://[^\\s?]+)\\?[^\\s]+"));
    text.replace(urlQuery, QStringLiteral("\\1?[redacted]"));
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        text.replace(home, QStringLiteral("<home>"), Qt::CaseInsensitive);
        text.replace(QDir::toNativeSeparators(home), QStringLiteral("<home>"), Qt::CaseInsensitive);
    }
    return text;
}

void logEvent(const QString& category, const QString& event, const QJsonObject& fields,
              QtMsgType level) noexcept {
    DiagnosticsService::instance().record(level, category, event, {}, fields);
}
} // namespace snow_shot::diagnostics
