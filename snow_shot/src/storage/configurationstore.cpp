#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/customaimodelconfiguration.h"

#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/storagelogging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <utility>

namespace snow_shot::storage {
namespace {
const QString kSchemaVersionKey = QStringLiteral("storage/schema_version");
const QString kCustomModelsKey = QStringLiteral("api_configuration/custom_models");

enum class ConfigurationOverlayPolicy {
    MergeFromDisk,
    ReplaceAll,
};

struct MaterializedConfiguration {
    QMap<QString, QJsonValue> values;
    QJsonObject document;
    bool dirty = false;
    bool customModelsRepaired = false;
};

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

QMap<QString, QJsonValue> overlayFromDocument(const QJsonObject& document) {
    QMap<QString, QJsonValue> overlay;
    for (const ConfigurationSchemaEntry& entry : ConfigurationSchema::entries()) {
        if (entry.key == kSchemaVersionKey) {
            continue;
        }
        bool present = false;
        const QJsonValue raw = valueAtPath(document, entry.key, &present);
        if (present) {
            overlay.insert(entry.key, raw);
        }
    }
    return overlay;
}

MaterializedConfiguration materializeConfiguration(const QMap<QString, QJsonValue>& overlay,
                                                   QJsonObject document, int schemaVersion,
                                                   ConfigurationCompatibility compatibility,
                                                   ConfigurationOverlayPolicy policy) {
    MaterializedConfiguration result;
    result.document = std::move(document);
    const bool mutateDocument = compatibility != ConfigurationCompatibility::FutureVersion;
    const bool replaceAll = policy == ConfigurationOverlayPolicy::ReplaceAll;

    for (const ConfigurationSchemaEntry& entry : ConfigurationSchema::entries()) {
        if (entry.key == kSchemaVersionKey) {
            continue;
        }

        const bool present = overlay.contains(entry.key);
        if (!present) {
            result.values.insert(entry.key, entry.defaultValue);
            if (mutateDocument) {
                insertPath(&result.document, entry.key, entry.defaultValue);
                if (!replaceAll) {
                    result.dirty = true;
                }
            }
            continue;
        }

        QJsonValue raw = overlay.value(entry.key);
        // Upgrade only untouched tray defaults from before each action was introduced.
        // Customized menus retain their selected commands and order, including on import.
        if (entry.key == QStringLiteral("tray/menu_options") && mutateDocument) {
            QJsonArray previousDefault = entry.defaultValue.toArray();
            const auto removeNewAction = [&](const QString& key, const QString& action) {
                if (overlay.contains(key) || raw.toArray().contains(action)) {
                    return;
                }
                for (qsizetype i = previousDefault.size(); i > 0; --i) {
                    if (previousDefault.at(i - 1).toString() == action) {
                        previousDefault.removeAt(i - 1);
                    }
                }
            };
            removeNewAction(QStringLiteral("global_shortcuts/restore_last_closed_windows"),
                            QStringLiteral("quick.restore-last-closed-windows"));
            removeNewAction(QStringLiteral("global_shortcuts/fullscreen_canvas"),
                            QStringLiteral("quick.fullscreen-canvas"));
            if (previousDefault != entry.defaultValue.toArray() && raw.isArray() &&
                raw.toArray() == previousDefault) {
                raw = entry.defaultValue;
                insertPath(&result.document, entry.key, raw);
                result.dirty = true;
            }
        }
        bool migratedDestroyShortcut = false;
        if (entry.key == QStringLiteral("pin_to_screen_shortcuts/destroy_window") &&
            mutateDocument && schemaVersion < 3) {
            const auto previousDefault =
                ConfigurationSchema::normalize(entry.key, QJsonArray{QStringLiteral("Ctrl+Esc")});
            const auto stored = ConfigurationSchema::normalize(entry.key, raw);
            if (stored.valid && stored.value == previousDefault.value) {
                raw = entry.defaultValue;
                migratedDestroyShortcut = true;
            }
        }
        if (entry.key == kCustomModelsKey) {
            bool valid = false;
            const QJsonValue canonical = customAiModelsToJson(customAiModelsFromJson(raw, &valid));
            result.values.insert(entry.key, canonical);
            result.customModelsRepaired = result.customModelsRepaired || !valid;
            if (replaceAll) {
                insertPath(&result.document, entry.key, canonical);
            }
            continue;
        }

        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(entry.key, raw);
        if (!normalized.valid) {
            result.values.insert(entry.key, entry.defaultValue);
            if (mutateDocument) {
                insertPath(&result.document, entry.key, entry.defaultValue);
                if (!replaceAll) {
                    result.dirty = true;
                }
            }
            continue;
        }

        result.values.insert(entry.key, normalized.value);
        if (mutateDocument && (replaceAll || normalized.changed || migratedDestroyShortcut)) {
            insertPath(&result.document, entry.key, normalized.value);
            if ((normalized.changed || migratedDestroyShortcut) && !replaceAll) {
                result.dirty = true;
            }
        }
    }

    int resolvedVersion = schemaVersion;
    const int currentVersion = ConfigurationSchema::currentVersion();
    if (mutateDocument && resolvedVersion < currentVersion) {
        resolvedVersion = currentVersion;
        if (!replaceAll) {
            result.dirty = true;
        }
    }
    result.values.insert(kSchemaVersionKey, resolvedVersion);
    if (mutateDocument && (replaceAll || resolvedVersion != schemaVersion)) {
        insertPath(&result.document, kSchemaVersionKey, resolvedVersion);
    }
    return result;
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

int ConfigurationStore::currentSchemaVersion() {
    return ConfigurationSchema::currentVersion();
}

QJsonValue ConfigurationStore::value(const QString& key) const {
    QMutexLocker locker(&m_mutex);
    return m_values.value(key, ConfigurationSchema::defaultValue(key));
}

QMap<QString, QJsonValue> ConfigurationStore::snapshot() const {
    QMutexLocker locker(&m_mutex);
    return m_values;
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

    announceChanges(std::move(changed));
    return true;
}

void ConfigurationStore::announceChanges(QVector<QPair<QString, QJsonValue>> changed) {
    const auto notify = [this, changed = std::move(changed)]() {
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
}

bool ConfigurationStore::applySnapshot(const QMap<QString, QJsonValue>& values, int schemaVersion) {
    const int currentVersion = ConfigurationSchema::currentVersion();
    if (schemaVersion <= 0) {
        schemaVersion = currentVersion;
    } else if (schemaVersion > currentVersion) {
        rejectMutation({}, QStringLiteral("Configuration schema is newer than this application"));
        return false;
    }

    MaterializedConfiguration materialized = materializeConfiguration(
        values, ConfigurationSchema::completeDefaultDocument(), schemaVersion,
        ConfigurationCompatibility::Current, ConfigurationOverlayPolicy::ReplaceAll);
    const QString customModelsError =
        materialized.customModelsRepaired
            ? tr("Some custom AI model configurations are invalid and were ignored")
            : QString();

    QVector<QPair<QString, QJsonValue>> changed;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_writeAvailable || m_compatibility == ConfigurationCompatibility::FutureVersion) {
            locker.unlock();
            rejectMutation({}, QStringLiteral("Configuration storage is read-only"));
            return false;
        }
        for (auto it = materialized.values.cbegin(); it != materialized.values.cend(); ++it) {
            if (m_values.value(it.key()) == it.value()) {
                continue;
            }
            changed.push_back({it.key(), it.value()});
        }
        if (changed.isEmpty()) {
            locker.unlock();
            if (!customModelsError.isEmpty()) {
                qCWarning(storageLog) << customModelsError;
                setLastError(customModelsError);
            }
            return true;
        }
        m_values = std::move(materialized.values);
        m_document = std::move(materialized.document);
        m_dirty = true;
        ++m_revision;
    }

    if (!customModelsError.isEmpty()) {
        qCWarning(storageLog) << customModelsError;
        setLastError(customModelsError);
    } else {
        setLastError({});
    }
    announceChanges(std::move(changed));
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
        qCInfo(storageLog) << "Configuration file is missing; schema defaults will be written";
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
            const QJsonValue versionValue = valueAtPath(parsedObject, kSchemaVersionKey);
            if (!ConfigurationSchema::parseIntegerVersion(versionValue, &version)) {
                compatibility = ConfigurationCompatibility::RecoveredDefaults;
                version = 1;
                dirty = true;
                error = QStringLiteral(
                    "config.json has an invalid schema version; defaults were loaded");
                preserveCorruptFile(m_configurationFile);
                qCWarning(storageLog) << error;
            } else {
                document = parsedObject;
                if (version > ConfigurationSchema::currentVersion()) {
                    compatibility = ConfigurationCompatibility::FutureVersion;
                    error = QStringLiteral("Configuration schema is newer than this application; "
                                           "storage is read-only");
                    qCWarning(storageLog) << error << version;
                }
                const MaterializedConfiguration materialized = materializeConfiguration(
                    overlayFromDocument(parsedObject), parsedObject, version, compatibility,
                    ConfigurationOverlayPolicy::MergeFromDisk);
                loaded = materialized.values;
                document = materialized.document;
                dirty = materialized.dirty;
                if (materialized.customModelsRepaired) {
                    error = tr("Some custom AI model configurations are invalid and were ignored");
                    qCWarning(storageLog) << error;
                }
            }
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        m_values = std::move(loaded);
        m_document = std::move(document);
        m_compatibility = compatibility;
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
