#include "snow_shot/storage/configurationstore.h"

#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/storagelogging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace snow_shot::storage {
namespace {
QJsonValue valueAtPath(const QJsonObject& root, const QString& path, bool* present = nullptr) {
    if (present != nullptr) {
        *present = false;
    }
    const QStringList parts = path.split(u'/');
    if (parts.size() != 2 || !root.value(parts[0]).isObject()) {
        return {};
    }
    const QJsonObject group = root.value(parts[0]).toObject();
    if (!group.contains(parts[1])) {
        return {};
    }
    if (present != nullptr) {
        *present = true;
    }
    return group.value(parts[1]);
}

void insertPath(QJsonObject* root, const QString& path, const QJsonValue& value) {
    const QStringList parts = path.split(u'/');
    if (root == nullptr || parts.size() != 2) {
        return;
    }
    QJsonObject group = root->value(parts[0]).toObject();
    group.insert(parts[1], value);
    root->insert(parts[0], group);
}

void removePath(QJsonObject* root, const QString& path) {
    const QStringList parts = path.split(u'/');
    if (root == nullptr || parts.size() != 2 || !root->value(parts[0]).isObject()) {
        return;
    }
    QJsonObject group = root->value(parts[0]).toObject();
    if (!group.contains(parts[1])) {
        return;
    }
    group.remove(parts[1]);
    root->insert(parts[0], group);
}

QString legacyKeyFor(const QString& key) {
    if (key == QStringLiteral("global_shortcuts/screen_record")) {
        return QStringLiteral("global_shortcuts/video_record");
    }
    if (key == QStringLiteral("global_shortcuts/screen_record_copy")) {
        return QStringLiteral("global_shortcuts/video_record_copy");
    }
    if (!key.startsWith(QStringLiteral("screen_recording/"))) {
        return {};
    }
    if (key == QStringLiteral("screen_recording/clarity")) {
        return QStringLiteral("video_recording/video_clarity");
    }
    return QStringLiteral("video_recording/") +
           key.mid(QStringLiteral("screen_recording/").size());
}

bool hasScreenshotShortcutConfiguration(const QJsonObject& root) {
    for (const QString& key : {QStringLiteral("screenshot_shortcuts/move_tool"),
                               QStringLiteral("screenshot_shortcuts/move_cursor_up"),
                               QStringLiteral("screenshot_shortcuts/move_cursor_down"),
                               QStringLiteral("screenshot_shortcuts/move_cursor_left"),
                               QStringLiteral("screenshot_shortcuts/move_cursor_right")}) {
        bool present = false;
        static_cast<void>(valueAtPath(root, key, &present));
        if (present) {
            return true;
        }
    }
    return false;
}

QJsonValue withoutLegacyDrawingAlias(const QString& key, const QJsonValue& value,
                                     bool* changed) {
    if (!value.isArray()) {
        return value;
    }
    QString alias;
    if (key == QStringLiteral("drawing_shortcuts/shape")) {
        alias = QStringLiteral("S");
    } else if (key == QStringLiteral("drawing_shortcuts/arrow")) {
        alias = QStringLiteral("A");
    } else if (key == QStringLiteral("drawing_shortcuts/watermark")) {
        alias = QStringLiteral("W");
    } else {
        return value;
    }

    QJsonArray migrated;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isString() && item.toString().trimmed().compare(alias, Qt::CaseInsensitive) == 0) {
            if (changed != nullptr) {
                *changed = true;
            }
            continue;
        }
        migrated.push_back(item);
    }
    return migrated;
}

bool integerVersion(const QJsonValue& value, int* version) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() || value.toDouble() < 1.0 ||
        value.toDouble() > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (version != nullptr) {
        *version = value.toInt();
    }
    return true;
}

QString corruptBackupPath(const QString& configurationFile) {
    const QString timestamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
    const QString base = configurationFile + QStringLiteral(".corrupt.") + timestamp;
    QString candidate = base + QStringLiteral(".json");
    for (int suffix = 1; QFileInfo::exists(candidate); ++suffix) {
        candidate = base + QStringLiteral(".%1.json").arg(suffix);
    }
    return candidate;
}

void preserveCorruptFile(const QString& configurationFile) {
    if (!QFileInfo::exists(configurationFile)) {
        return;
    }
    const QString backup = corruptBackupPath(configurationFile);
    if (QFile::copy(configurationFile, backup)) {
        qCWarning(storageLog) << "Preserved invalid configuration at" << backup;
    } else {
        qCWarning(storageLog) << "Unable to preserve invalid configuration" << configurationFile;
    }
}

void cleanupCorruptBackups(const QString& configurationFile) {
    const QFileInfo configurationInfo(configurationFile);
    if (configurationFile.isEmpty() || configurationInfo.absolutePath().isEmpty()) {
        return;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-30);
    const QString pattern = configurationInfo.fileName() + QStringLiteral(".corrupt.*.json");
    const QFileInfoList backups =
        QDir(configurationInfo.absolutePath())
            .entryInfoList({pattern}, QDir::Files | QDir::Hidden | QDir::System, QDir::Time);
    for (const QFileInfo& backup : backups) {
        if (backup.lastModified().toUTC() >= cutoff || QFile::remove(backup.absoluteFilePath())) {
            continue;
        }
        qCWarning(storageLog) << "Unable to remove expired configuration backup"
                              << backup.absoluteFilePath();
    }
}

QByteArray serializedDocument(const QJsonObject& document) {
    QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) {
        bytes.append('\n');
    }
    return bytes;
}
} // namespace

ConfigurationStore::ConfigurationStore(QString configurationFile, bool readAvailable,
                                       bool writeAvailable, int debounceMilliseconds,
                                       QObject* parent)
    : QObject(parent), m_configurationFile(std::move(configurationFile)),
      m_readAvailable(readAvailable), m_writeAvailable(writeAvailable) {
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(std::max(0, debounceMilliseconds));
    connect(&m_flushTimer, &QTimer::timeout, this, [this]() { static_cast<void>(flushNow()); });
    load();
}

QJsonValue ConfigurationStore::value(const QString& key) const {
    QMutexLocker locker(&m_mutex);
    return m_values.value(key, ConfigurationSchema::defaultValue(key));
}

QMap<QString, QJsonValue> ConfigurationStore::snapshot() const {
    QMutexLocker locker(&m_mutex);
    return m_values;
}

QJsonObject ConfigurationStore::documentSnapshot() const {
    QMutexLocker locker(&m_mutex);
    return m_document;
}

bool ConfigurationStore::isDirty() const {
    QMutexLocker locker(&m_mutex);
    return m_dirty;
}

bool ConfigurationStore::isWritable() const {
    QMutexLocker locker(&m_mutex);
    return m_writeAvailable && m_compatibility != ConfigurationCompatibility::FutureVersion;
}

QString ConfigurationStore::lastError() const {
    QMutexLocker locker(&m_mutex);
    return m_lastError;
}

ConfigurationCompatibility ConfigurationStore::compatibility() const {
    QMutexLocker locker(&m_mutex);
    return m_compatibility;
}

int ConfigurationStore::schemaVersion() const {
    QMutexLocker locker(&m_mutex);
    return m_schemaVersion;
}

bool ConfigurationStore::setValue(const QString& key, const QJsonValue& value) {
    return setValues({{key, value}});
}

bool ConfigurationStore::setValues(const QMap<QString, QJsonValue>& values) {
    QMap<QString, QJsonValue> normalizedValues;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!ConfigurationSchema::contains(it.key())) {
            rejectMutation(it.key(), QStringLiteral("The configuration key is not supported"));
            return false;
        }
        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(it.key(), it.value());
        if (!normalized.valid) {
            rejectMutation(it.key(), QStringLiteral("The configuration value is invalid"));
            return false;
        }
        normalizedValues.insert(it.key(), normalized.value);
    }

    QVector<QPair<QString, QJsonValue>> changed;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_writeAvailable || m_compatibility == ConfigurationCompatibility::FutureVersion) {
            locker.unlock();
            rejectMutation(values.isEmpty() ? QString() : values.cbegin().key(),
                           QStringLiteral("Configuration storage is read-only"));
            return false;
        }
        for (auto it = normalizedValues.cbegin(); it != normalizedValues.cend(); ++it) {
            if (m_values.value(it.key()) == it.value()) {
                continue;
            }
            m_values.insert(it.key(), it.value());
            insertPath(&m_document, it.key(), it.value());
            changed.push_back({it.key(), it.value()});
        }
        if (changed.isEmpty()) {
            return true;
        }
        m_dirty = true;
        ++m_revision;
    }

    const auto notify = [this, changed]() {
        for (const auto& item : changed) {
            emit valueChanged(item.first, item.second);
        }
        scheduleFlush();
    };
    if (QThread::currentThread() == thread()) {
        notify();
    } else {
        QMetaObject::invokeMethod(this, notify, Qt::QueuedConnection);
    }
    return true;
}

StorageResult ConfigurationStore::flushNow() {
    if (QThread::currentThread() == thread()) {
        m_flushTimer.stop();
    }
    QMutexLocker ioLocker(&m_ioMutex);
    for (;;) {
        QJsonObject document;
        quint64 revision = 0;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_dirty) {
                return StorageResult::ok();
            }
            if (!m_writeAvailable || m_configurationFile.isEmpty() ||
                m_compatibility == ConfigurationCompatibility::FutureVersion) {
                const QString error = QStringLiteral("Configuration storage is read-only");
                locker.unlock();
                setLastError(error);
                return StorageResult::failure(error);
            }
            document = m_document;
            revision = m_revision;
        }

        const QByteArray bytes = serializedDocument(document);
        QSaveFile file(m_configurationFile);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            file.cancelWriting();
            const QString error = QStringLiteral("Unable to write config.json atomically");
            qCWarning(storageLog) << error << m_configurationFile;
            setLastError(error);
            return StorageResult::failure(error);
        }

        {
            QMutexLocker locker(&m_mutex);
            if (m_revision == revision) {
                m_dirty = false;
                locker.unlock();
                setLastError({});
                return StorageResult::ok();
            }
        }
    }
}

void ConfigurationStore::load() {
    cleanupCorruptBackups(m_configurationFile);
    QMap<QString, QJsonValue> loaded;
    for (const ConfigurationSchemaEntry& entry : ConfigurationSchema::entries()) {
        loaded.insert(entry.key, entry.defaultValue);
    }
    QJsonObject document = ConfigurationSchema::completeDefaultDocument();
    ConfigurationCompatibility compatibility = ConfigurationCompatibility::Current;
    int version = 1;
    bool dirty = false;
    QString error;

    if (!m_readAvailable || m_configurationFile.isEmpty()) {
        compatibility = ConfigurationCompatibility::Unavailable;
        dirty = true;
        error = QStringLiteral("Configuration storage is not readable");
    } else if (!QFileInfo::exists(m_configurationFile)) {
        dirty = true;
        qCInfo(storageLog) << "Configuration file is missing; schema-v1 defaults will be written";
    } else {
        QFile file(m_configurationFile);
        QJsonParseError parseError;
        const bool opened = file.open(QIODevice::ReadOnly);
        const QJsonDocument parsed =
            opened ? QJsonDocument::fromJson(file.readAll(), &parseError) : QJsonDocument();
        if (!opened || parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
            compatibility = ConfigurationCompatibility::RecoveredDefaults;
            dirty = true;
            error = opened ? QStringLiteral("config.json is malformed; defaults were loaded")
                           : QStringLiteral("Unable to read config.json; defaults were loaded");
            preserveCorruptFile(m_configurationFile);
            qCWarning(storageLog) << error;
        } else {
            const QJsonObject parsedObject = parsed.object();
            const QJsonValue versionValue =
                valueAtPath(parsedObject, QStringLiteral("storage/schema_version"));
            if (!integerVersion(versionValue, &version)) {
                compatibility = ConfigurationCompatibility::RecoveredDefaults;
                version = 1;
                dirty = true;
                error = QStringLiteral(
                    "config.json has an invalid schema version; defaults were loaded");
                preserveCorruptFile(m_configurationFile);
                qCWarning(storageLog) << error;
            } else {
                document = parsedObject;
                if (version > 1) {
                    compatibility = ConfigurationCompatibility::FutureVersion;
                    error = QStringLiteral("Configuration schema is newer than this application; "
                                           "storage is read-only");
                    qCWarning(storageLog) << error << version;
                }
                const bool migrateLegacyDrawingAliases =
                    compatibility != ConfigurationCompatibility::FutureVersion &&
                    !hasScreenshotShortcutConfiguration(parsedObject);
                for (const ConfigurationSchemaEntry& entry : ConfigurationSchema::entries()) {
                    if (entry.key == QStringLiteral("storage/schema_version")) {
                        loaded.insert(entry.key, version);
                        continue;
                    }
                    bool present = false;
                    bool migratedLegacy = false;
                    QJsonValue raw = valueAtPath(parsedObject, entry.key, &present);
                    const QString legacyKey = legacyKeyFor(entry.key);
                    if (!present && !legacyKey.isEmpty()) {
                        raw = valueAtPath(parsedObject, legacyKey, &present);
                        migratedLegacy = present;
                    }
                    if (!present) {
                        if (compatibility != ConfigurationCompatibility::FutureVersion) {
                            insertPath(&document, entry.key, entry.defaultValue);
                            dirty = true;
                        }
                        continue;
                    }
                    bool migratedDrawingAlias = false;
                    if (migrateLegacyDrawingAliases) {
                        raw = withoutLegacyDrawingAlias(entry.key, raw, &migratedDrawingAlias);
                    }
                    const ConfigurationNormalization normalized =
                        ConfigurationSchema::normalize(entry.key, raw);
                    if (!normalized.valid) {
                        if (compatibility != ConfigurationCompatibility::FutureVersion) {
                            insertPath(&document, entry.key, entry.defaultValue);
                            if (migratedLegacy) {
                                removePath(&document, legacyKey);
                            }
                            dirty = true;
                        }
                        continue;
                    }
                    loaded.insert(entry.key, normalized.value);
                    if (compatibility != ConfigurationCompatibility::FutureVersion &&
                        (normalized.changed || migratedLegacy || migratedDrawingAlias)) {
                        insertPath(&document, entry.key, normalized.value);
                        if (!legacyKey.isEmpty()) {
                            removePath(&document, legacyKey);
                        }
                        dirty = true;
                    }
                }
            }
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        m_values = std::move(loaded);
        m_document = std::move(document);
        m_compatibility = compatibility;
        m_schemaVersion = version;
        m_dirty = dirty && compatibility != ConfigurationCompatibility::FutureVersion;
        m_lastError = error;
    }
    if (dirty && compatibility != ConfigurationCompatibility::FutureVersion) {
        scheduleFlush();
    }
}

void ConfigurationStore::scheduleFlush() {
    if (isWritable()) {
        m_flushTimer.start();
    }
}

void ConfigurationStore::setLastError(const QString& error) {
    bool changed = false;
    {
        QMutexLocker locker(&m_mutex);
        changed = m_lastError != error;
        m_lastError = error;
    }
    if (changed) {
        emit errorChanged(error);
    }
}

void ConfigurationStore::rejectMutation(const QString& key, const QString& error) {
    setLastError(error);
    if (QThread::currentThread() == thread()) {
        emit mutationRejected(key, error);
    } else {
        QMetaObject::invokeMethod(
            this, [this, key, error]() { emit mutationRejected(key, error); },
            Qt::QueuedConnection);
    }
}
} // namespace snow_shot::storage
