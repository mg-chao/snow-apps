#ifndef SNOW_SHOT_PRINT_DIAGNOSTICS_TEST_SUPPORT_H
#define SNOW_SHOT_PRINT_DIAGNOSTICS_TEST_SUPPORT_H

#include "snow_shot/diagnostics/diagnostics.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <stdexcept>

namespace print_tests {
class LogSession {
  public:
    LogSession()
        : m_directory(QDir(QDir::tempPath()).canonicalPath() +
                      QStringLiteral("/snow-print-log-XXXXXX")) {
        snow_shot::diagnostics::DiagnosticsOptions options;
        options.directories = {m_directory.path()};
        options.enableCrashCapture = false;
        options.installMessageHandler = false;
        options.mirrorToConsole = false;
        if (!m_directory.isValid() || !service().initialize(options))
            throw std::runtime_error("print diagnostic logger must initialize");
    }

    ~LogSession() {
        service().shutdown();
    }

    QList<QJsonObject> records() {
        const auto exported = service().exportDay(QDate::currentDate()).get();
        if (!exported.success)
            throw std::runtime_error("print diagnostic log must export");
        QFile file(exported.path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("exported print log must be readable");
        QList<QJsonObject> records;
        for (const auto& line : file.readAll().split('\n')) {
            const auto record = QJsonDocument::fromJson(line).object();
            if (record.value(QStringLiteral("category")) == QStringLiteral("snow_shot.print"))
                records.append(record);
        }
        return records;
    }

  private:
    static snow_shot::diagnostics::DiagnosticsService& service() {
        return snow_shot::diagnostics::DiagnosticsService::instance();
    }
    QTemporaryDir m_directory;
};
} // namespace print_tests

#endif
