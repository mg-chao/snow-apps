#ifndef SNOW_SHOT_TESTS_LEGACY_THUMBNAIL_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_LEGACY_THUMBNAIL_TEST_SUPPORT_H

#include "snow_shot/storage/storageusagetracker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

namespace thumbnail_tests {

// Both standalone and Store builds must use disposable caches, including when
// ApplicationStorage automatically cleans files during initialization.
class CacheEnvironment final {
  public:
    CacheEnvironment()
        : m_applicationName(QCoreApplication::applicationName()),
          m_organizationName(QCoreApplication::organizationName()),
          m_testMode(QStandardPaths::isTestModeEnabled()),
          m_hadLocalData(qEnvironmentVariableIsSet("LOCALAPPDATA")),
          m_localData(qgetenv("LOCALAPPDATA")) {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("thumbnail-tests-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        m_valid = m_storeData.isValid() && qputenv("LOCALAPPDATA", m_storeData.path().toUtf8());
        m_directory = snow_shot::storage::StorageUsageTracker::defaultThumbnailCacheDirectory();
        m_valid = m_valid && !m_directory.isEmpty();
    }

    ~CacheEnvironment() {
        // The directory belongs to the unique test application or temporary
        // Store data root established above; it cannot contain user caches.
        if (m_valid)
            QDir(m_directory).removeRecursively();
        if (m_hadLocalData)
            qputenv("LOCALAPPDATA", m_localData);
        else
            qunsetenv("LOCALAPPDATA");
        QCoreApplication::setApplicationName(m_applicationName);
        QCoreApplication::setOrganizationName(m_organizationName);
        QStandardPaths::setTestModeEnabled(m_testMode);
    }

    [[nodiscard]] bool isValid() const {
        return m_valid;
    }
    [[nodiscard]] QString directory() const {
        return m_directory;
    }

  private:
    QTemporaryDir m_storeData;
    QString m_applicationName;
    QString m_organizationName;
    bool m_testMode;
    bool m_hadLocalData;
    QByteArray m_localData;
    QString m_directory;
    bool m_valid = false;
};

inline QString legacyPath(const QString& key) {
    const auto digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(snow_shot::storage::StorageUsageTracker::defaultThumbnailCacheDirectory())
        .filePath(QString::fromLatin1(digest) + QStringLiteral(".png"));
}

} // namespace thumbnail_tests

#endif // SNOW_SHOT_TESTS_LEGACY_THUMBNAIL_TEST_SUPPORT_H
