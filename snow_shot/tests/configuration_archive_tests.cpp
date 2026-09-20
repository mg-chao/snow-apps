#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/platform/minizippath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <iostream>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeZip(const QString& path, const QMap<QString, QByteArray>& entries) {
    void* writer = mz_zip_writer_create();
    require(writer != nullptr, "test zip writer could not be created");
    require(mz_zip_writer_open_file(writer, snow_shot::platform::minizipPath(path).constData(), 0,
                                    0) == MZ_OK,
            "test zip could not be opened");
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QByteArray name = it.key().toUtf8();
        mz_zip_file info{};
        info.filename = name.constData();
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        require(mz_zip_writer_add_buffer(writer, const_cast<char*>(it.value().constData()),
                                         static_cast<int32_t>(it.value().size()), &info) == MZ_OK,
                "test zip entry could not be written");
    }
    require(mz_zip_writer_close(writer) == MZ_OK, "test zip must finalize successfully");
    mz_zip_writer_delete(&writer);
}

QByteArray manifestBytes(int schemaVersion, const QByteArray& format = "snow-shot-configuration",
                         int formatVersion = 1) {
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), QString::fromUtf8(format)},
                                     {QStringLiteral("format_version"), formatVersion},
                                     {QStringLiteral("schema_version"), schemaVersion}})
        .toJson(QJsonDocument::Compact);
}

QByteArray configurationBytes(const QJsonObject& configuration) {
    return QJsonDocument(configuration).toJson(QJsonDocument::Compact);
}

QJsonObject sampleConfiguration() {
    QJsonObject configuration;
    const auto& entries = storage::ConfigurationSchema::entries();
    int added = 0;
    for (const storage::ConfigurationSchemaEntry& entry : entries) {
        if (added >= 12) {
            break;
        }
        configuration.insert(entry.key, entry.defaultValue);
        ++added;
    }
    return configuration;
}

void roundTripPreservesValuesAndSchemaVersion(const QTemporaryDir& temporary) {
    const QJsonObject configuration = sampleConfiguration();
    QMap<QString, QJsonValue> values;
    for (auto it = configuration.begin(); it != configuration.end(); ++it) {
        values.insert(it.key(), it.value());
    }
    values.insert(QStringLiteral("storage/schema_version"),
                  storage::ConfigurationStore::currentSchemaVersion());

    const QString path = temporary.filePath(QStringLiteral("config-export.zip"));
    require(storage::ConfigurationArchive::write(
                path, values, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "writing a configuration archive must succeed");

    const storage::ConfigurationArchiveReadResult read = storage::ConfigurationArchive::read(path);
    require(read.isValid(), "reading a just-written archive must succeed");
    require(read.schemaVersion == storage::ConfigurationStore::currentSchemaVersion(),
            "the schema version must round trip");
    require(!read.values.contains(QStringLiteral("storage/schema_version")),
            "the store-managed schema version must not be importable");
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() == QStringLiteral("storage/schema_version")) {
            continue;
        }
        require(read.values.value(it.key()) == it.value(),
                "exported values must survive the round trip");
    }
}

void readRejectsInvalidArchives(const QTemporaryDir& temporary) {
    const QString garbage = temporary.filePath(QStringLiteral("garbage.zip"));
    {
        QFile file(garbage);
        require(file.open(QIODevice::WriteOnly), "garbage fixture could not be written");
        file.write("this is not a zip archive");
    }
    require(!storage::ConfigurationArchive::read(garbage).isValid(),
            "random bytes must not read as a configuration archive");
    require(!storage::ConfigurationArchive::read(temporary.filePath(QStringLiteral("missing.zip")))
                 .isValid(),
            "a missing file must not read as a configuration archive");

    const QJsonObject configuration = sampleConfiguration();
    const QString withoutManifest = temporary.filePath(QStringLiteral("no-manifest.zip"));
    writeZip(withoutManifest, {{QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(withoutManifest).isValid(),
            "an archive without a manifest must be rejected");

    const QString withoutConfiguration = temporary.filePath(QStringLiteral("no-config.zip"));
    writeZip(withoutConfiguration,
             {{QStringLiteral("manifest.json"),
               manifestBytes(storage::ConfigurationStore::currentSchemaVersion())}});
    require(!storage::ConfigurationArchive::read(withoutConfiguration).isValid(),
            "an archive without configuration values must be rejected");

    const QString foreignFormat = temporary.filePath(QStringLiteral("foreign.zip"));
    writeZip(foreignFormat, {{QStringLiteral("manifest.json"),
                              manifestBytes(storage::ConfigurationStore::currentSchemaVersion(),
                                            "some-other-format")},
                             {QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(foreignFormat).isValid(),
            "an archive with a foreign format marker must be rejected");

    const QString futureFormat = temporary.filePath(QStringLiteral("future-format.zip"));
    writeZip(futureFormat, {{QStringLiteral("manifest.json"),
                             manifestBytes(storage::ConfigurationStore::currentSchemaVersion(),
                                           "snow-shot-configuration", 2)},
                            {QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(futureFormat).isValid(),
            "an archive with a newer archive format must be rejected");

    const QString fractionalFormat = temporary.filePath(QStringLiteral("fractional-format.zip"));
    writeZip(fractionalFormat,
             {{QStringLiteral("manifest.json"),
               QJsonDocument(QJsonObject{{QStringLiteral("format"),
                                          QStringLiteral("snow-shot-configuration")},
                                         {QStringLiteral("format_version"), 1.5},
                                         {QStringLiteral("schema_version"),
                                          storage::ConfigurationStore::currentSchemaVersion()}})
                   .toJson(QJsonDocument::Compact)},
              {QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(fractionalFormat).isValid(),
            "a non-integer archive format version must be rejected");

    const QString missingFormat = temporary.filePath(QStringLiteral("missing-format.zip"));
    writeZip(missingFormat,
             {{QStringLiteral("manifest.json"),
               QJsonDocument(QJsonObject{{QStringLiteral("format"),
                                          QStringLiteral("snow-shot-configuration")},
                                         {QStringLiteral("schema_version"),
                                          storage::ConfigurationStore::currentSchemaVersion()}})
                   .toJson(QJsonDocument::Compact)},
              {QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(missingFormat).isValid(),
            "a manifest without a format version must be rejected");

    const QString futureSchema = temporary.filePath(QStringLiteral("future-schema.zip"));
    writeZip(futureSchema,
             {{QStringLiteral("manifest.json"),
               manifestBytes(storage::ConfigurationStore::currentSchemaVersion() + 1)},
              {QStringLiteral("config.json"), configurationBytes(configuration)}});
    require(!storage::ConfigurationArchive::read(futureSchema).isValid(),
            "an archive from a newer application version must be rejected");

    const QString invalidJson = temporary.filePath(QStringLiteral("invalid-json.zip"));
    writeZip(invalidJson, {{QStringLiteral("manifest.json"),
                            manifestBytes(storage::ConfigurationStore::currentSchemaVersion())},
                           {QStringLiteral("config.json"), QByteArray("{ not json")}});
    require(!storage::ConfigurationArchive::read(invalidJson).isValid(),
            "malformed configuration JSON must be rejected");

    const QString unknownKeys = temporary.filePath(QStringLiteral("unknown-keys.zip"));
    writeZip(unknownKeys,
             {{QStringLiteral("manifest.json"),
               manifestBytes(storage::ConfigurationStore::currentSchemaVersion())},
              {QStringLiteral("config.json"),
               configurationBytes(QJsonObject{{QStringLiteral("not/a_key"), 1},
                                              {QStringLiteral("also/unknown"), true}})}});
    require(!storage::ConfigurationArchive::read(unknownKeys).isValid(),
            "an archive without any compatible settings must be rejected");

    const QString oversized = temporary.filePath(QStringLiteral("oversized.zip"));
    writeZip(oversized, {{QStringLiteral("manifest.json"),
                          manifestBytes(storage::ConfigurationStore::currentSchemaVersion())},
                         {QStringLiteral("config.json"), QByteArray(9 * 1024 * 1024, ' ')}});
    require(!storage::ConfigurationArchive::read(oversized).isValid(),
            "an oversized configuration entry must be rejected");

    QMap<QString, QByteArray> manyEntries;
    manyEntries.insert(QStringLiteral("manifest.json"),
                       manifestBytes(storage::ConfigurationStore::currentSchemaVersion()));
    for (int index = 0; index < 20; ++index) {
        manyEntries.insert(QStringLiteral("extra-%1.bin").arg(index), QByteArray("x"));
    }
    const QString tooManyEntries = temporary.filePath(QStringLiteral("too-many-entries.zip"));
    writeZip(tooManyEntries, manyEntries);
    require(!storage::ConfigurationArchive::read(tooManyEntries).isValid(),
            "archives with unexpected entries must be rejected");
}

void writeRejectsUnwritableTargets(const QTemporaryDir& temporary) {
    const QString blocker = temporary.filePath(QStringLiteral("blocker"));
    {
        QFile file(blocker);
        require(file.open(QIODevice::WriteOnly), "blocker fixture could not be written");
    }
    const QString nested = QDir(blocker).filePath(QStringLiteral("nested/export.zip"));
    require(!storage::ConfigurationArchive::write(nested, {}, 1).isEmpty(),
            "writing under a regular file must fail");
    require(!storage::ConfigurationArchive::write(QString(), {}, 1).isEmpty(),
            "writing without a target path must fail");

    const QString directoryTarget = temporary.filePath(QStringLiteral("directory.zip"));
    require(QDir().mkpath(directoryTarget), "a directory target must be available");
    require(!storage::ConfigurationArchive::write(directoryTarget, {}, 1).isEmpty(),
            "publishing an archive over a directory must fail");
    require(QFileInfo(directoryTarget).isDir(), "failed publication must preserve the directory");
    require(QDir(temporary.path())
                .entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden)
                .isEmpty(),
            "failed publication must remove temporary archives");
}

void unicodePathRoundTrip(const QString& name) {
    QTemporaryDir unicodeDirectory(QDir::tempPath() + QStringLiteral("/snow-%1-XXXXXX").arg(name));
    require(unicodeDirectory.isValid(), "a unicode temporary directory must be available");
    const QJsonObject configuration = sampleConfiguration();
    QMap<QString, QJsonValue> values;
    for (auto it = configuration.begin(); it != configuration.end(); ++it) {
        values.insert(it.key(), it.value());
    }

    const QString path = QDir(unicodeDirectory.path()).filePath(name + QStringLiteral(".zip"));
    require(storage::ConfigurationArchive::write(
                path, values, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "writing a configuration archive under a unicode path must succeed");
    require(QFileInfo::exists(path), "the archive must exist at the exact requested Unicode path");
    const storage::ConfigurationArchiveReadResult read = storage::ConfigurationArchive::read(path);
    require(read.isValid(), "reading a configuration archive from a unicode path must succeed");
    require(read.schemaVersion == storage::ConfigurationStore::currentSchemaVersion(),
            "unicode-path archives must preserve the schema version");
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() == QStringLiteral("storage/schema_version")) {
            continue;
        }
        require(read.values.value(it.key()) == it.value(),
                "unicode-path archives must preserve values");
    }
}

void unicodePathsRoundTrip() {
    // Test both ANSI-representable characters whose bytes differ from UTF-8 and
    // characters outside legacy code pages, in both directory and archive names.
    unicodePathRoundTrip(QStringLiteral("caf\u00e9"));
    unicodePathRoundTrip(QStringLiteral("配置归档-\U0001F9CA"));
    unicodePathRoundTrip(QStringLiteral("cafe\u0301"));
}

void applySnapshotReplacesConfiguration(const QTemporaryDir& temporary) {
    const QString file = temporary.filePath(QStringLiteral("snapshot-config.json"));
    storage::ConfigurationStore store(file, true, true);
    require(store.isWritable(), "a writable store must accept snapshots");

    QString archivedKey;
    QJsonValue archivedValue;
    QString revertedKey;
    QJsonValue revertedValue;
    for (const storage::ConfigurationSchemaEntry& entry : storage::ConfigurationSchema::entries()) {
        if (entry.key == QStringLiteral("storage/schema_version") ||
            entry.valueKind != storage::ConfigurationValueKind::Boolean) {
            continue;
        }
        const QJsonValue flipped = !entry.defaultValue.toBool();
        if (archivedKey.isEmpty()) {
            archivedKey = entry.key;
            archivedValue = flipped;
        } else {
            revertedKey = entry.key;
            revertedValue = flipped;
            break;
        }
    }
    require(!archivedKey.isEmpty() && !revertedKey.isEmpty(),
            "the schema must expose at least two boolean settings");
    require(store.setValue(archivedKey, archivedValue) &&
                store.setValue(revertedKey, revertedValue),
            "seeding the store must succeed");

    QStringList changedKeys;
    QObject::connect(
        &store, &storage::ConfigurationStore::valueChanged,
        [&changedKeys](const QString& key, const QJsonValue&) { changedKeys.push_back(key); });

    require(store.applySnapshot({{archivedKey, archivedValue}}),
            "applying a snapshot must succeed");
    require(store.value(archivedKey) == archivedValue, "snapshot values must be applied");
    require(store.value(revertedKey) == storage::ConfigurationSchema::defaultValue(revertedKey),
            "keys absent from the snapshot must revert to schema defaults");
    require(changedKeys == QStringList{revertedKey},
            "only settings that actually differ must announce changes");

    require(store.applySnapshot({{archivedKey, QStringLiteral("not a boolean")}}),
            "invalid snapshot values must be tolerated");
    require(store.value(archivedKey) == storage::ConfigurationSchema::defaultValue(archivedKey),
            "invalid snapshot values must revert to schema defaults");

    require(store.applySnapshot(
                {{QStringLiteral("not/a_key"), 1}, {QStringLiteral("storage/schema_version"), 99}}),
            "unknown snapshot keys must be ignored");
    require(store.value(QStringLiteral("storage/schema_version")).toInt() ==
                storage::ConfigurationStore::currentSchemaVersion(),
            "the store-managed schema version must be preserved");

    require(!store.applySnapshot({{archivedKey, archivedValue}},
                                 storage::ConfigurationSchema::currentVersion() + 1),
            "snapshots from a newer schema must be rejected");
    require(store.value(archivedKey) == storage::ConfigurationSchema::defaultValue(archivedKey),
            "rejecting a future schema must leave the current configuration unchanged");

    require(store.setValue(archivedKey, archivedValue) &&
                store.setValue(revertedKey, revertedValue),
            "reseeding after a future-schema rejection must succeed");
    require(store.applySnapshot({{archivedKey, archivedValue}}, 1),
            "older schema snapshots must be accepted");
    require(store.value(archivedKey) == archivedValue, "older snapshots must apply carried values");
    require(store.value(revertedKey) == storage::ConfigurationSchema::defaultValue(revertedKey),
            "older snapshots must restore unspecified keys");
    require(store.value(QStringLiteral("storage/schema_version")).toInt() ==
                storage::ConfigurationStore::currentSchemaVersion(),
            "older snapshots must upgrade to the current schema version");

    require(store.flushNow().success, "flushing an applied snapshot must succeed");
    storage::ConfigurationStore reloaded(file, true, true);
    require(reloaded.value(archivedKey) == archivedValue &&
                reloaded.value(revertedKey) ==
                    storage::ConfigurationSchema::defaultValue(revertedKey) &&
                reloaded.value(QStringLiteral("storage/schema_version")).toInt() ==
                    storage::ConfigurationStore::currentSchemaVersion(),
            "applied snapshots must persist across reloads");

    storage::ConfigurationStore readOnly(
        temporary.filePath(QStringLiteral("snapshot-read-only.json")), true, false);
    require(!readOnly.applySnapshot({{archivedKey, archivedValue}}),
            "read-only stores must reject snapshots");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("configuration-archive-tests"));

    require(storage::ConfigurationStore::currentSchemaVersion() >= 1,
            "the current schema version must be positive");

    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    roundTripPreservesValuesAndSchemaVersion(temporary);
    readRejectsInvalidArchives(temporary);
    writeRejectsUnwritableTargets(temporary);
    unicodePathsRoundTrip();
    applySnapshotReplacesConfiguration(temporary);
    return 0;
}
