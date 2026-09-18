#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace snow_shot::storage {
namespace {

constexpr int kConfigArchiveFormatVersion = 1;
constexpr int kConfigArchiveMaxEntries = 16;
constexpr qint64 kConfigArchiveMaxManifestBytes = 64 * 1024;
constexpr qint64 kConfigArchiveMaxConfigurationBytes = 8 * 1024 * 1024;

QString configArchiveTranslate(const char* source) {
    return QCoreApplication::translate("snow_shot::storage::ConfigurationArchive", source);
}

QByteArray configArchiveEntryBytes(void* reader, qint64 declaredSize, qint64 limit, bool* ok) {
    QByteArray payload;
    if (ok != nullptr) {
        *ok = false;
    }
    if (declaredSize < 0 || declaredSize > limit || mz_zip_reader_entry_open(reader) != MZ_OK) {
        return payload;
    }
    char buffer[65536];
    qint64 total = 0;
    for (;;) {
        const int32_t count = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
        if (count < 0) {
            break;
        }
        if (count == 0) {
            break;
        }
        total += count;
        if (total > limit) {
            break;
        }
        payload.append(buffer, count);
    }
    const bool intact = total == declaredSize && mz_zip_reader_entry_close(reader) == MZ_OK;
    if (!intact) {
        mz_zip_reader_entry_close(reader);
        payload.clear();
        return payload;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return payload;
}

QJsonObject configArchiveJsonObject(const QByteArray& payload, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return document.object();
}

} // namespace

QString ConfigurationArchive::write(const QString& archivePath,
                                    const QMap<QString, QJsonValue>& values, int schemaVersion) {
    const QString failure = configArchiveTranslate(
        QT_TRANSLATE_NOOP("snow_shot::storage::ConfigurationArchive",
                          "The configuration archive could not be created."));
    const QFileInfo target(archivePath);
    if (archivePath.isEmpty() || target.fileName().isEmpty() ||
        !QDir().mkpath(target.absolutePath())) {
        return failure;
    }
    const QString temporary =
        QDir(target.absolutePath())
            .filePath(QStringLiteral(".%1.%2.part")
                          .arg(target.fileName(), QUuid::createUuid().toString(QUuid::Id128)));

    QJsonObject configuration;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() == QLatin1String("storage/schema_version")) {
            continue;
        }
        configuration.insert(it.key(), it.value());
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("snow-shot-configuration"));
    manifest.insert(QStringLiteral("format_version"), kConfigArchiveFormatVersion);
    manifest.insert(QStringLiteral("schema_version"), schemaVersion);
    manifest.insert(QStringLiteral("app_version"), QCoreApplication::applicationVersion());
    manifest.insert(QStringLiteral("created"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    void* writer = mz_zip_writer_create();
    if (writer == nullptr) {
        return failure;
    }
    struct WriterGuard {
        void* writer;
        bool closed = false;
        ~WriterGuard() {
            if (!closed) {
                mz_zip_writer_close(writer);
            }
            mz_zip_writer_delete(&writer);
        }
    } guard{writer};
    if (mz_zip_writer_open_file(writer, QFile::encodeName(temporary).constData(), 0, 0) != MZ_OK) {
        return failure;
    }
    const auto addEntry = [writer](const char* name, const QByteArray& payload) {
        mz_zip_file info{};
        info.filename = name;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        return mz_zip_writer_add_buffer(writer, const_cast<char*>(payload.constData()),
                                        static_cast<int32_t>(payload.size()), &info) == MZ_OK;
    };
    if (!addEntry("manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact)) ||
        !addEntry("config.json", QJsonDocument(configuration).toJson(QJsonDocument::Compact)) ||
        mz_zip_writer_close(writer) != MZ_OK) {
        guard.closed = true;
        QFile::remove(temporary);
        return failure;
    }
    guard.closed = true;
    QFile::remove(archivePath);
    if (!QFile::rename(temporary, archivePath)) {
        QFile::remove(temporary);
        return failure;
    }
    return {};
}

ConfigurationArchiveReadResult ConfigurationArchive::read(const QString& archivePath) {
    ConfigurationArchiveReadResult result;
    const QString invalidArchive =
        configArchiveTranslate(QT_TRANSLATE_NOOP("snow_shot::storage::ConfigurationArchive",
                                                 "The file is not a valid configuration archive."));
    const auto fail = [&result](const QString& error) {
        result.error = error;
        return result;
    };

    void* reader = mz_zip_reader_create();
    if (reader == nullptr || archivePath.isEmpty() ||
        mz_zip_reader_open_file(reader, QFile::encodeName(archivePath).constData()) != MZ_OK) {
        if (reader != nullptr) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
        }
        return fail(invalidArchive);
    }
    struct ReaderGuard {
        void* reader;
        ~ReaderGuard() {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
        }
    } readerGuard{reader};

    QByteArray manifestBytes;
    QByteArray configurationBytes;
    bool haveManifest = false;
    bool haveConfiguration = false;
    int entryCount = 0;
    int32_t code = mz_zip_reader_goto_first_entry(reader);
    while (code == MZ_OK) {
        if (++entryCount > kConfigArchiveMaxEntries) {
            return fail(invalidArchive);
        }
        mz_zip_file* entry = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &entry) != MZ_OK || entry == nullptr ||
            entry->filename == nullptr ||
            (entry->linkname != nullptr && entry->linkname[0] != '\0')) {
            return fail(invalidArchive);
        }
        const auto mode = (entry->external_fa >> 16) & 0170000;
        if (mode != 0 && mode != 0100000) {
            return fail(invalidArchive);
        }
        const QString name = QString::fromUtf8(entry->filename);
        const bool isManifest = name == QLatin1String("manifest.json");
        const bool isConfiguration = name == QLatin1String("config.json");
        const bool duplicate =
            (isManifest && haveManifest) || (isConfiguration && haveConfiguration);
        if ((!isManifest && !isConfiguration) || duplicate) {
            return fail(invalidArchive);
        }
        bool entryOk = false;
        QByteArray& destination = isManifest ? manifestBytes : configurationBytes;
        destination = configArchiveEntryBytes(reader, entry->uncompressed_size,
                                              isManifest ? kConfigArchiveMaxManifestBytes
                                                         : kConfigArchiveMaxConfigurationBytes,
                                              &entryOk);
        if (!entryOk) {
            return fail(invalidArchive);
        }
        if (isManifest) {
            haveManifest = true;
        } else {
            haveConfiguration = true;
        }
        code = mz_zip_reader_goto_next_entry(reader);
    }
    if (code != MZ_END_OF_LIST || !haveManifest || !haveConfiguration) {
        return fail(invalidArchive);
    }

    bool manifestOk = false;
    const QJsonObject manifest = configArchiveJsonObject(manifestBytes, &manifestOk);
    if (!manifestOk) {
        return fail(invalidArchive);
    }
    if (manifest.value(QStringLiteral("format")).toString() !=
        QLatin1String("snow-shot-configuration")) {
        return fail(configArchiveTranslate(
            QT_TRANSLATE_NOOP("snow_shot::storage::ConfigurationArchive",
                              "The file is not a Snow Shot configuration archive.")));
    }
    int formatVersion = 0;
    if (!ConfigurationSchema::parseIntegerVersion(manifest.value(QStringLiteral("format_version")),
                                                  &formatVersion)) {
        return fail(invalidArchive);
    }
    if (formatVersion > kConfigArchiveFormatVersion) {
        return fail(configArchiveTranslate(QT_TRANSLATE_NOOP(
            "snow_shot::storage::ConfigurationArchive",
            "The configuration archive was created by a newer version of Snow Shot.")));
    }
    if (formatVersion != kConfigArchiveFormatVersion) {
        return fail(invalidArchive);
    }
    int schemaVersion = 0;
    if (!ConfigurationSchema::parseIntegerVersion(manifest.value(QStringLiteral("schema_version")),
                                                  &schemaVersion)) {
        return fail(invalidArchive);
    }
    if (schemaVersion > ConfigurationSchema::currentVersion()) {
        return fail(configArchiveTranslate(QT_TRANSLATE_NOOP(
            "snow_shot::storage::ConfigurationArchive",
            "The configuration archive was created by a newer version of Snow Shot.")));
    }
    result.schemaVersion = schemaVersion;

    bool configurationOk = false;
    const QJsonObject configuration = configArchiveJsonObject(configurationBytes, &configurationOk);
    if (!configurationOk) {
        return fail(invalidArchive);
    }
    for (auto it = configuration.begin(); it != configuration.end(); ++it) {
        if (it.key() == QLatin1String("storage/schema_version") ||
            !ConfigurationSchema::contains(it.key())) {
            continue;
        }
        const ConfigurationNormalization normalized =
            ConfigurationSchema::normalize(it.key(), it.value());
        if (normalized.valid) {
            result.values.insert(it.key(), normalized.value);
        }
    }
    if (result.values.isEmpty()) {
        return fail(configArchiveTranslate(
            QT_TRANSLATE_NOOP("snow_shot::storage::ConfigurationArchive",
                              "The configuration archive contains no compatible settings.")));
    }
    return result;
}

} // namespace snow_shot::storage
