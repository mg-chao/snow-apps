#include "snow_shot/diagnostics/diagnostics.h"
#include "diagnosticsbridge.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QTimeZone>

#include <QtEndian>

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include "client/crash_report_database.h"
#include "client/settings.h"
#endif
#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace snow_shot::diagnostics {
namespace {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
base::FilePath nativePath(const QString& path) {
#ifdef Q_OS_WIN
    return base::FilePath(path.toStdWString());
#else
    return base::FilePath(QFile::encodeName(path).toStdString());
#endif
}
QString qtPath(const base::FilePath& path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.value());
#else
    return QFile::decodeName(path.value().c_str());
#endif
}

// Minidumps use the same little-endian wire format on Windows and macOS.
// Read bounded fields rather than host SDK structs (whose layout is platform-specific).
QJsonObject exceptionContext(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray header = file.read(32);
    if (header.size() != 32 || !header.startsWith("MDMP"))
        return {};
    const auto* bytes = reinterpret_cast<const uchar*>(header.constData());
    const quint32 count = qFromLittleEndian<quint32>(bytes + 8);
    const quint32 offset = qFromLittleEndian<quint32>(bytes + 12);
    if (count > 1024 ||
        static_cast<quint64>(offset) + count * 12ULL > static_cast<quint64>(file.size()))
        return {};
    for (quint32 index = 0; index < count; ++index) {
        if (!file.seek(static_cast<qint64>(offset) + index * 12LL))
            return {};
        const QByteArray directory = file.read(12);
        if (directory.size() != 12)
            return {};
        bytes = reinterpret_cast<const uchar*>(directory.constData());
        if (qFromLittleEndian<quint32>(bytes) != 6) // ExceptionStream
            continue;
        const quint32 size = qFromLittleEndian<quint32>(bytes + 4);
        const quint32 rva = qFromLittleEndian<quint32>(bytes + 8);
        if (size < 168 || static_cast<quint64>(rva) + size > static_cast<quint64>(file.size()) ||
            !file.seek(rva))
            return {};
        const QByteArray exception = file.read(32);
        if (exception.size() != 32)
            return {};
        bytes = reinterpret_cast<const uchar*>(exception.constData());
        return {
            {QStringLiteral("exception_code"),
             QStringLiteral("0x%1").arg(qFromLittleEndian<quint32>(bytes + 8), 8, 16, u'0')},
            {QStringLiteral("exception_address"),
             QStringLiteral("0x%1").arg(qFromLittleEndian<quint64>(bytes + 24), 0, 16)},
            {QStringLiteral("thread_id"), static_cast<qint64>(qFromLittleEndian<quint32>(bytes))}};
    }
    return {};
}
#endif
class LocalCrashCollector final : public CrashCollector {
  public:
    bool initialize(const QString& directory, const QString& handler, const QString& session,
                    QString* error) override {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        m_database = crashpad::CrashReportDatabase::Initialize(nativePath(directory));
        if (!m_database || !m_database->GetSettings()->SetUploadsEnabled(false)) {
            *error = QString::fromUtf8(QT_TRANSLATE_NOOP(
                "DiagnosticsService", "The local crash database could not be initialized."));
            return false;
        }
        if (handler.isEmpty() && session.isEmpty())
            return true;
        if (!QFileInfo(handler).isFile() ||
            !snow_diag_start(handler.toUtf8().constData(), directory.toUtf8().constData(),
                             session.toUtf8().constData(), SNOW_DIAGNOSTICS_VERSION,
                             SNOW_DIAGNOSTICS_REVISION)) {
            *error = QString::fromUtf8(QT_TRANSLATE_NOOP(
                "DiagnosticsService",
                "The crash collector could not be started. Check the application installation."));
            return false;
        }
        m_pipe = QString::fromUtf8(snow_diag_pipe());
        return true;
#else
        Q_UNUSED(directory);
        Q_UNUSED(handler);
        Q_UNUSED(session);
        *error = QString::fromUtf8(QT_TRANSLATE_NOOP(
            "DiagnosticsService", "Crash capture is unavailable on this platform."));
        return false;
#endif
    }
    QVector<CrashReport> reports() override {
        QVector<CrashReport> result;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (!m_database)
            return result;
        std::vector<crashpad::CrashReportDatabase::Report> reports;
        std::vector<crashpad::CrashReportDatabase::Report> completed;
        m_database->GetPendingReports(&reports);
        m_database->GetCompletedReports(&completed);
        reports.insert(reports.end(), completed.begin(), completed.end());
        for (const auto& report : reports) {
            result.push_back({QString::fromStdString(report.uuid.ToString()),
                              qtPath(report.file_path),
                              QDateTime::fromSecsSinceEpoch(report.creation_time, QTimeZone::UTC),
                              static_cast<qint64>(report.total_size),
                              exceptionContext(qtPath(report.file_path))});
        }
#endif
        return result;
    }
    bool removeReport(const QString& id) override {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        crashpad::UUID uuid;
        return m_database && uuid.InitializeFromString(id.toStdString()) &&
               m_database->DeleteReport(uuid) == crashpad::CrashReportDatabase::kNoError;
#else
        Q_UNUSED(id);
        return false;
#endif
    }
    QString pipeName() const override {
        return m_pipe;
    }
    bool healthy() const override {
#ifdef Q_OS_WIN
        if (m_pipe.isEmpty())
            return false;
        if (WaitNamedPipeW(reinterpret_cast<LPCWSTR>(m_pipe.utf16()), NMPWAIT_NOWAIT))
            return true;
        const DWORD error = GetLastError();
        return error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY;
#elif defined(Q_OS_MACOS)
        return !m_pipe.isEmpty() && snow_diag_healthy();
#else
        return false;
#endif
    }

  private:
    QString m_pipe;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    std::unique_ptr<crashpad::CrashReportDatabase> m_database;
#endif
};
} // namespace
std::shared_ptr<CrashCollector> makeCrashCollector() {
    return std::make_shared<LocalCrashCollector>();
}
} // namespace snow_shot::diagnostics
