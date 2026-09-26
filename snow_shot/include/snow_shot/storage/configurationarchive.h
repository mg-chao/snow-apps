#ifndef SNOW_SHOT_STORAGE_CONFIGURATIONARCHIVE_H
#define SNOW_SHOT_STORAGE_CONFIGURATIONARCHIVE_H

#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>

namespace snow_shot::storage {

struct ConfigurationArchiveReadResult {
    QMap<QString, QJsonValue> values;
    int schemaVersion = 0;
    QString error;
    QStringList redactedCredentialIds;
    void preserveOmittedCredentials(const QMap<QString, QJsonValue>& current);

    [[nodiscard]] bool isValid() const {
        return error.isEmpty();
    }
};

// Creates and validates zip archives that carry a full configuration snapshot.
// An archive holds a manifest describing the format plus a flat
// "group/key" -> value document.  Applying imported values is left to
// ConfigurationStore, so reading an archive never mutates live configuration.
class ConfigurationArchive final {
  public:
    // Writes `values` (excluding the store-managed schema version) together
    // with a manifest stamped with `schemaVersion`.  Returns an empty string
    // on success and a translated error otherwise.
    [[nodiscard]] static QString write(const QString& archivePath,
                                       const QMap<QString, QJsonValue>& values, int schemaVersion,
                                       bool redactCredentials = false);

    // Reads and validates an archive.  Unknown or invalid keys are dropped;
    // on success `values` holds only keys the current schema accepts.
    [[nodiscard]] static ConfigurationArchiveReadResult read(const QString& archivePath);
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CONFIGURATIONARCHIVE_H
