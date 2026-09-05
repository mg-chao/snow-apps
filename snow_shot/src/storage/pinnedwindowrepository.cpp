#include "snow_shot/storage/pinnedwindowrepository.h"

#include "snowimageqtcodec.h"
#include "snow_shot/storage/storagelogging.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace snow_shot::storage {
namespace {
constexpr int kFormatVersion = 3;
constexpr auto kDefaultGroupId = "default";
constexpr auto kDefaultGroupName = "Default";
constexpr int kMaximumRecords = 128;
constexpr int kMaximumGroups = PinnedWindowRepository::maximumGroupCount();
constexpr qint64 kMaximumImageBytes = 256LL * 1024LL * 1024LL;
constexpr qint64 kMaximumPayloadBytes = 32LL * 1024LL * 1024LL;
constexpr auto kDirectoryName = "pinned_windows_v3";
constexpr auto kManifestName = "index.json";

QString sourceKindToString(PinnedWindowSourceKind kind) {
    switch (kind) {
    case PinnedWindowSourceKind::ImageData:
        return QStringLiteral("image_data");
    case PinnedWindowSourceKind::ClipboardText:
        return QStringLiteral("clipboard_text");
    case PinnedWindowSourceKind::ClipboardImageFile:
        return QStringLiteral("clipboard_image_file");
    }
    return QStringLiteral("image_data");
}

bool sourceKindFromString(const QString& value, PinnedWindowSourceKind* kind) {
    if (kind == nullptr) {
        return false;
    }
    if (value == QStringLiteral("image_data")) {
        *kind = PinnedWindowSourceKind::ImageData;
    } else if (value == QStringLiteral("clipboard_text")) {
        *kind = PinnedWindowSourceKind::ClipboardText;
    } else if (value == QStringLiteral("clipboard_image_file")) {
        *kind = PinnedWindowSourceKind::ClipboardImageFile;
    } else {
        return false;
    }
    return true;
}

QJsonObject rectFToJson(const QRectF& rect) {
    return {{QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}};
}

QJsonObject rectToJson(const QRect& rect) {
    return {{QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}};
}

bool finiteNumber(const QJsonValue& value, double minimum, double maximum, double* result) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < minimum ||
        value.toDouble() > maximum) {
        return false;
    }
    if (result != nullptr) {
        *result = value.toDouble();
    }
    return true;
}

bool rectFromJson(const QJsonValue& value, QRect* result) {
    if (result == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    if (!finiteNumber(object.value(QStringLiteral("x")), std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max(), &x) ||
        !finiteNumber(object.value(QStringLiteral("y")), std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max(), &y) ||
        !finiteNumber(object.value(QStringLiteral("width")), 1.0, std::numeric_limits<int>::max(),
                      &width) ||
        !finiteNumber(object.value(QStringLiteral("height")), 1.0, std::numeric_limits<int>::max(),
                      &height)) {
        return false;
    }
    *result = QRect(qRound(x), qRound(y), qRound(width), qRound(height));
    return result->isValid() && !result->isEmpty();
}

bool rectFFromJson(const QJsonValue& value, QRectF* result) {
    if (result == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    if (!finiteNumber(object.value(QStringLiteral("x")), -1e9, 1e9, &x) ||
        !finiteNumber(object.value(QStringLiteral("y")), -1e9, 1e9, &y) ||
        !finiteNumber(object.value(QStringLiteral("width")), 0.001, 1e9, &width) ||
        !finiteNumber(object.value(QStringLiteral("height")), 0.001, 1e9, &height)) {
        return false;
    }
    *result = QRectF(x, y, width, height);
    return result->isValid() && !result->isEmpty();
}

bool optionalRectFromJson(const QJsonValue& value, QRect* result) {
    if (result == nullptr || !value.isObject()) {
        return false;
    }
    if (rectFromJson(value, result)) {
        return true;
    }
    const QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("x")).toDouble(-1.0) == 0.0 &&
        object.value(QStringLiteral("y")).toDouble(-1.0) == 0.0 &&
        object.value(QStringLiteral("width")).toDouble(-1.0) == 0.0 &&
        object.value(QStringLiteral("height")).toDouble(-1.0) == 0.0) {
        *result = {};
        return true;
    }
    return false;
}

QJsonObject sizeToJson(const QSize& size) {
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}

bool sizeFromJson(const QJsonValue& value, QSize* result) {
    if (result == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    const int width = object.value(QStringLiteral("width")).toInt(-1);
    const int height = object.value(QStringLiteral("height")).toInt(-1);
    if (width < 1 || height < 1) {
        return false;
    }
    *result = QSize(width, height);
    return true;
}

QJsonArray transformToJson(const QTransform& transform) {
    return {transform.m11(), transform.m12(), transform.m13(), transform.m21(), transform.m22(),
            transform.m23(), transform.m31(), transform.m32(), transform.m33()};
}

bool transformFromJson(const QJsonValue& value, QTransform* result) {
    if (result == nullptr || !value.isArray() || value.toArray().size() != 9) {
        return false;
    }
    const QJsonArray values = value.toArray();
    double matrix[9]{};
    for (int index = 0; index < 9; ++index) {
        if (!finiteNumber(values.at(index), -1e6, 1e6, &matrix[index])) {
            return false;
        }
    }
    *result = QTransform(matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
                         matrix[6], matrix[7], matrix[8]);
    return true;
}

bool safeId(const QString& id) {
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

bool safeGroupId(const QString& id) {
    return id == QString::fromLatin1(kDefaultGroupId) || safeId(id);
}

bool safeFileName(const QString& name) {
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           !QDir::isAbsolutePath(name) && QFileInfo(name).fileName() == name &&
           !name.contains(u'/') && !name.contains(u'\\') && !name.contains(u':');
}

void preserveInvalidIndex(const QString& path) {
    if (!QFileInfo::exists(path)) {
        return;
    }
    const QString timestamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
    QString backup = path + QStringLiteral(".corrupt.") + timestamp;
    for (int suffix = 1; QFileInfo::exists(backup); ++suffix) {
        backup = path + QStringLiteral(".corrupt.") + timestamp + QStringLiteral(".%1").arg(suffix);
    }
    if (!QFile::copy(path, backup)) {
        qCWarning(storageLog) << "Failed to preserve invalid pinned-window index" << path;
    }
}

// Fingerprint of the payload-backed fields of a record. Payload identity has
// to survive the demotion that clears those fields from memory once the
// writer has committed them, so the fields cannot be compared directly.
struct PayloadSignature final {
    qint64 imageCacheKey = 0;
    QSize imageSize;
    size_t originalHtmlHash = 0;
    size_t originalTextHash = 0;
    size_t resultStyleHash = 0;
    size_t canvasSessionHash = 0;
    size_t recognitionResultsHash = 0;
};

bool operator==(const PayloadSignature& first, const PayloadSignature& second) {
    return first.imageCacheKey == second.imageCacheKey && first.imageSize == second.imageSize &&
           first.originalHtmlHash == second.originalHtmlHash &&
           first.originalTextHash == second.originalTextHash &&
           first.resultStyleHash == second.resultStyleHash &&
           first.canvasSessionHash == second.canvasSessionHash &&
           first.recognitionResultsHash == second.recognitionResultsHash;
}

size_t payloadHash(const QByteArray& bytes) {
    return bytes.isEmpty() ? 0 : qHashBits(bytes.constData(), bytes.size());
}

PayloadSignature payloadSignature(const PinnedWindowRecord& record) {
    PayloadSignature signature;
    signature.imageCacheKey = record.image.cacheKey();
    signature.imageSize = record.image.size();
    signature.originalHtmlHash = qHash(record.originalHtml);
    signature.originalTextHash = qHash(record.originalText);
    signature.resultStyleHash = payloadHash(record.resultStyle);
    signature.canvasSessionHash = payloadHash(record.canvasSession);
    signature.recognitionResultsHash = payloadHash(record.recognitionResults);
    return signature;
}

PinnedWindowGroup defaultGroup() {
    return {QString::fromLatin1(kDefaultGroupId), QString::fromLatin1(kDefaultGroupName), true};
}

bool groupNameInUse(const QVector<PinnedWindowGroup>& groups, const QString& name) {
    return std::any_of(groups.cbegin(), groups.cend(), [&name](const PinnedWindowGroup& group) {
        return group.name.trimmed().compare(name, Qt::CaseInsensitive) == 0;
    });
}

QJsonObject groupToJson(const PinnedWindowGroup& group) {
    return {{QStringLiteral("id"), group.id},
            {QStringLiteral("name"), group.name},
            {QStringLiteral("built_in"), group.builtIn}};
}

QByteArray encodeImage(const QImage& image) {
    if (image.isNull() || image.size().isEmpty()) {
        return {};
    }
    QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    return buffer.data();
}

QImage decodeImage(const QString& path, const QString& suffix = QStringLiteral("png")) {
    snow::image::Format format = snow::image::Format::png;
    const QString normalized = suffix.toLower();
    if (normalized == QStringLiteral("jpg") || normalized == QStringLiteral("jpeg")) {
        format = snow::image::Format::jpeg;
    } else if (normalized == QStringLiteral("webp")) {
        format = snow::image::Format::webp;
    } else if (normalized == QStringLiteral("jxl")) {
        format = snow::image::Format::jxl;
    } else if (normalized == QStringLiteral("avif")) {
        format = snow::image::Format::avif;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    return snow_shot::image_codec::decodeFileBgra(path, format);
#else
    return snow_shot::image_codec::decodeFile(path, format);
#endif
}

struct StoredRecord final {
    PinnedWindowRecord record;
    quint64 payloadRevision = 1;
    // Reload descriptor for the on-disk payload; empty while the payload is
    // only resident in `record`.
    QJsonObject payloads;
    PayloadSignature signature;
    std::optional<PreparedPngImage> preparedSource;
};

bool samePayload(const StoredRecord& stored, const PinnedWindowRecord& incoming,
                 const PayloadSignature& incomingSignature) {
    return stored.signature == incomingSignature &&
           stored.record.sourceKind == incoming.sourceKind &&
           stored.record.originalFilePath == incoming.originalFilePath &&
           stored.record.originalFileName == incoming.originalFileName;
}

struct Snapshot final {
    QVector<PinnedWindowGroup> groups;
    QString activeGroupId;
    QVector<StoredRecord> records;
    quint64 revision = 0;
};

QString payloadDirectory(const QString& root, const QString& id) {
    return QDir(root).filePath(QStringLiteral("pins/%1").arg(id));
}

QJsonObject payloadsToJson(const PinnedWindowRecord& record) {
    QJsonObject payloads{{QStringLiteral("directory"), record.id}};
    if (record.sourceKind == PinnedWindowSourceKind::ImageData) {
        payloads.insert(QStringLiteral("image"), QStringLiteral("source.png"));
    } else if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
        payloads.insert(QStringLiteral("image"), record.originalFileName);
    }
    if (!record.originalHtml.isEmpty()) {
        payloads.insert(QStringLiteral("html"), QStringLiteral("original.html"));
    }
    if (!record.originalText.isEmpty()) {
        payloads.insert(QStringLiteral("text"), QStringLiteral("original.txt"));
    }
    if (!record.resultStyle.isEmpty()) {
        payloads.insert(QStringLiteral("result_style"), QStringLiteral("result_style.bin"));
    }
    if (!record.canvasSession.isEmpty()) {
        payloads.insert(QStringLiteral("canvas_session"), QStringLiteral("canvas_session.bin"));
    }
    if (!record.recognitionResults.isEmpty()) {
        payloads.insert(QStringLiteral("recognition_results"),
                        QStringLiteral("recognition_results.bin"));
    }
    return payloads;
}

QJsonObject recordToJson(const PinnedWindowRecord& record, const QJsonObject& payloads) {
    return QJsonObject{
        {QStringLiteral("id"), record.id},
        {QStringLiteral("group_id"), record.groupId},
        {QStringLiteral("source_kind"), sourceKindToString(record.sourceKind)},
        {QStringLiteral("canvas_source_rect"), rectFToJson(record.canvasSourceRect)},
        {QStringLiteral("content_canvas_rect"), rectFToJson(record.contentCanvasRect)},
        {QStringLiteral("surface_canvas_rect"), rectFToJson(record.surfaceCanvasRect)},
        {QStringLiteral("initial_physical_size"), sizeToJson(record.initialPhysicalSize)},
        {QStringLiteral("native_geometry"), rectToJson(record.nativeGeometry)},
        {QStringLiteral("screen_name"), record.screenName},
        {QStringLiteral("screen_serial"), record.screenSerial},
        {QStringLiteral("screen_logical_geometry"), rectToJson(record.screenLogicalGeometry)},
        {QStringLiteral("screen_physical_geometry"), rectToJson(record.screenPhysicalGeometry)},
        {QStringLiteral("screen_dpi"), record.screenDpi},
        {QStringLiteral("first_creation_text_dpi"), record.firstCreationTextDpi},
        {QStringLiteral("scale_percent"), record.scalePercent},
        {QStringLiteral("opacity_percent"), record.opacityPercent},
        {QStringLiteral("quarter_turns"), record.quarterTurns},
        {QStringLiteral("image_transform"), transformToJson(record.imageTransform)},
        {QStringLiteral("thumbnail_mode"), record.thumbnailMode},
        {QStringLiteral("pre_thumbnail_geometry"), rectToJson(record.preThumbnailNativeGeometry)},
        {QStringLiteral("original_file_name"), record.originalFileName},
        {QStringLiteral("updated_utc"), record.updatedUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("payloads"), payloads},
    };
}

// The descriptor that reloads a record's payload from disk: the stored one
// once the record has been demoted, otherwise one derived from the resident
// payload fields.
QJsonObject payloadsDescriptor(const StoredRecord& stored) {
    return stored.payloads.isEmpty() ? payloadsToJson(stored.record) : stored.payloads;
}

void clearResidentPayload(PinnedWindowRecord* record) {
    record->image = {};
    record->originalHtml.clear();
    record->originalText.clear();
    record->resultStyle.clear();
    record->canvasSession.clear();
    record->recognitionResults.clear();
}

// Demotes a committed record to its lazy form: the on-disk descriptor and the
// payload signature replace the resident payload data.
void demoteCommittedRecord(StoredRecord& stored) {
    stored.payloads = payloadsDescriptor(stored);
    clearResidentPayload(&stored.record);
    stored.preparedSource.reset();
}

void clearResidentImmutableSource(PinnedWindowRecord* record) {
    record->image = {};
    record->originalHtml.clear();
    record->originalText.clear();
    record->resultStyle.clear();
}

QByteArray jsonBytes(const QJsonObject& object) {
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) {
        bytes.append('\n');
    }
    return bytes;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        file.cancelWriting();
        return false;
    }
    return true;
}

bool writePayload(const QString& root, const StoredRecord& stored) {
    const PinnedWindowRecord& record = stored.record;
    const QString directory = payloadDirectory(root, record.id);
    if (!QDir().mkpath(directory)) {
        return false;
    }
    QSet<QString> retainedFiles;
    for (const QString& key :
         {QStringLiteral("image"), QStringLiteral("html"), QStringLiteral("text"),
          QStringLiteral("result_style"), QStringLiteral("canvas_session"),
          QStringLiteral("recognition_results")}) {
        const QString fileName = stored.payloads.value(key).toString();
        if (safeFileName(fileName)) {
            retainedFiles.insert(fileName);
        }
    }
    if (record.sourceKind == PinnedWindowSourceKind::ImageData) {
        const QString sourcePath = QDir(directory).filePath(QStringLiteral("source.png"));
        QByteArray encoded;
        if (stored.preparedSource.has_value()) {
            encoded = stored.preparedSource->bytes();
        } else if (!record.image.isNull()) {
            encoded = encodeImage(record.image);
        }
        if ((!encoded.isEmpty() &&
             (encoded.size() > kMaximumImageBytes || !writeBytes(sourcePath, encoded))) ||
            (encoded.isEmpty() && !QFileInfo::exists(sourcePath))) {
            return false;
        }
        retainedFiles.insert(QStringLiteral("source.png"));
    } else if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
        const QString fileName = record.originalFileName.isEmpty()
                                     ? QFileInfo(record.originalFilePath).fileName()
                                     : record.originalFileName;
        const QString committedFileName = stored.payloads.value(QStringLiteral("image")).toString();
        if (record.originalFilePath.isEmpty() && safeFileName(committedFileName) &&
            QFileInfo(QDir(directory).filePath(committedFileName)).isFile()) {
            retainedFiles.insert(committedFileName);
        } else if (!safeFileName(fileName) || !QFileInfo(record.originalFilePath).isFile()) {
            return false;
        } else {
            const QString destination = QDir(directory).filePath(fileName);
            if (QDir::cleanPath(record.originalFilePath) != QDir::cleanPath(destination) &&
                !QFileInfo::exists(destination) &&
                !QFile::copy(record.originalFilePath, destination)) {
                return false;
            }
            retainedFiles.insert(fileName);
        }
    }
    if (!record.originalHtml.isEmpty() &&
        !writeBytes(QDir(directory).filePath(QStringLiteral("original.html")),
                    record.originalHtml.toUtf8())) {
        return false;
    }
    if (!record.originalHtml.isEmpty()) {
        retainedFiles.insert(QStringLiteral("original.html"));
    }
    if (!record.originalText.isEmpty() &&
        !writeBytes(QDir(directory).filePath(QStringLiteral("original.txt")),
                    record.originalText.toUtf8())) {
        return false;
    }
    if (!record.originalText.isEmpty()) {
        retainedFiles.insert(QStringLiteral("original.txt"));
    }
    const std::pair<const char*, const QByteArray*> blobs[] = {
        {"result_style.bin", &record.resultStyle},
        {"canvas_session.bin", &record.canvasSession},
        {"recognition_results.bin", &record.recognitionResults},
    };
    for (const auto& blob : blobs) {
        if (blob.second->size() > kMaximumPayloadBytes ||
            (!blob.second->isEmpty() &&
             !writeBytes(QDir(directory).filePath(QString::fromLatin1(blob.first)),
                         *blob.second))) {
            return false;
        }
        if (!blob.second->isEmpty()) {
            retainedFiles.insert(QString::fromLatin1(blob.first));
        }
    }
    const QFileInfoList files =
        QDir(directory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& file : files) {
        if (!retainedFiles.contains(file.fileName()) && !QFile::remove(file.absoluteFilePath())) {
            return false;
        }
    }
    return true;
}

bool readBlob(const QString& path, QByteArray* result) {
    if (result == nullptr) {
        return false;
    }
    QFile file(path);
    if (!file.exists()) {
        result->clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumPayloadBytes) {
        return false;
    }
    *result = file.readAll();
    return result->size() == file.size();
}

bool validatePayloads(const QJsonObject& payloads, const QString& root, const QString& id,
                      PinnedWindowSourceKind sourceKind) {
    if (payloads.value(QStringLiteral("directory")).toString() != id) {
        return false;
    }
    const QString directory = payloadDirectory(root, id);
    for (auto it = payloads.begin(); it != payloads.end(); ++it) {
        if (it.key() == QStringLiteral("directory")) {
            continue;
        }
        if (!it.value().isString()) {
            return false;
        }
        const QString fileName = it.value().toString();
        if (!safeFileName(fileName) || !QFileInfo(QDir(directory).filePath(fileName)).isFile()) {
            return false;
        }
    }
    if ((sourceKind == PinnedWindowSourceKind::ImageData ||
         sourceKind == PinnedWindowSourceKind::ClipboardImageFile) &&
        !payloads.value(QStringLiteral("image")).isString()) {
        return false;
    }
    return true;
}

bool loadPayloads(const QString& root, const QJsonObject& payloads, PinnedWindowRecord* record) {
    if (record == nullptr || !validatePayloads(payloads, root, record->id, record->sourceKind)) {
        return false;
    }
    const QString directory = payloadDirectory(root, record->id);
    if (record->sourceKind == PinnedWindowSourceKind::ImageData) {
        const QString fileName = payloads.value(QStringLiteral("image")).toString();
        record->image = decodeImage(QDir(directory).filePath(fileName));
        if (record->image.isNull() || record->image.sizeInBytes() > kMaximumImageBytes) {
            return false;
        }
    } else if (record->sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
        const QString fileName = payloads.value(QStringLiteral("image")).toString();
        const QString imagePath = QDir(directory).filePath(fileName);
        record->originalFileName = fileName;
        record->originalFilePath = imagePath;
        record->image = decodeImage(imagePath, QFileInfo(imagePath).suffix());
        if (record->image.isNull() || record->image.sizeInBytes() > kMaximumImageBytes) {
            return false;
        }
    }
    const auto readText = [&directory, &payloads](const QString& key, QString* target) {
        if (!payloads.contains(key) || target == nullptr) {
            return true;
        }
        QFile file(QDir(directory).filePath(payloads.value(key).toString()));
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumPayloadBytes) {
            return false;
        }
        const QByteArray bytes = file.readAll();
        if (bytes.size() != file.size()) {
            return false;
        }
        *target = QString::fromUtf8(bytes);
        return true;
    };
    if (!readText(QStringLiteral("html"), &record->originalHtml) ||
        !readText(QStringLiteral("text"), &record->originalText)) {
        return false;
    }
    const std::pair<const char*, QByteArray*> blobs[] = {
        {"result_style", &record->resultStyle},
        {"canvas_session", &record->canvasSession},
        {"recognition_results", &record->recognitionResults},
    };
    for (const auto& blob : blobs) {
        const QString key = QString::fromLatin1(blob.first);
        if (payloads.contains(key) &&
            !readBlob(QDir(directory).filePath(payloads.value(key).toString()), blob.second)) {
            return false;
        }
    }
    return true;
}

bool parseRecord(const QJsonObject& object, const QString& root, PinnedWindowRecord* result,
                 QJsonObject* payloadsResult) {
    if (result == nullptr || !safeId(object.value(QStringLiteral("id")).toString())) {
        return false;
    }
    PinnedWindowRecord record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.groupId = object.value(QStringLiteral("group_id")).toString();
    if (!safeGroupId(record.groupId) ||
        !sourceKindFromString(object.value(QStringLiteral("source_kind")).toString(),
                              &record.sourceKind) ||
        !rectFFromJson(object.value(QStringLiteral("canvas_source_rect")),
                       &record.canvasSourceRect) ||
        !rectFFromJson(object.value(QStringLiteral("content_canvas_rect")),
                       &record.contentCanvasRect) ||
        !rectFFromJson(object.value(QStringLiteral("surface_canvas_rect")),
                       &record.surfaceCanvasRect) ||
        !rectFromJson(object.value(QStringLiteral("native_geometry")), &record.nativeGeometry) ||
        !sizeFromJson(object.value(QStringLiteral("initial_physical_size")),
                      &record.initialPhysicalSize)) {
        return false;
    }
    double number = 0.0;
    if (!finiteNumber(object.value(QStringLiteral("first_creation_text_dpi")), 0.1, 20.0,
                      &record.firstCreationTextDpi) ||
        !finiteNumber(object.value(QStringLiteral("screen_dpi")), 0.1, 20.0, &record.screenDpi) ||
        !finiteNumber(object.value(QStringLiteral("scale_percent")), 1.0, 1000.0,
                      &record.scalePercent) ||
        !finiteNumber(object.value(QStringLiteral("opacity_percent")), 1.0, 100.0, &number)) {
        return false;
    }
    record.opacityPercent = qRound(number);
    record.quarterTurns = object.value(QStringLiteral("quarter_turns")).toInt(-1);
    if (record.quarterTurns < 0 || record.quarterTurns > 3 ||
        !transformFromJson(object.value(QStringLiteral("image_transform")),
                           &record.imageTransform)) {
        return false;
    }
    record.thumbnailMode = object.value(QStringLiteral("thumbnail_mode")).toBool();
    if (record.thumbnailMode &&
        !rectFromJson(object.value(QStringLiteral("pre_thumbnail_geometry")),
                      &record.preThumbnailNativeGeometry)) {
        return false;
    }
    record.screenName = object.value(QStringLiteral("screen_name")).toString();
    record.screenSerial = object.value(QStringLiteral("screen_serial")).toString();
    if (!object.value(QStringLiteral("screen_logical_geometry")).isUndefined() &&
        !optionalRectFromJson(object.value(QStringLiteral("screen_logical_geometry")),
                              &record.screenLogicalGeometry)) {
        return false;
    }
    if (!object.value(QStringLiteral("screen_physical_geometry")).isUndefined() &&
        !optionalRectFromJson(object.value(QStringLiteral("screen_physical_geometry")),
                              &record.screenPhysicalGeometry)) {
        return false;
    }
    record.originalFileName = object.value(QStringLiteral("original_file_name")).toString();
    record.updatedUtc = QDateTime::fromString(
        object.value(QStringLiteral("updated_utc")).toString(), Qt::ISODateWithMs);
    if (!record.updatedUtc.isValid() || !object.value(QStringLiteral("payloads")).isObject()) {
        return false;
    }
    const QJsonObject payloads = object.value(QStringLiteral("payloads")).toObject();
    if (!validatePayloads(payloads, root, record.id, record.sourceKind)) {
        return false;
    }
    if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
        record.originalFileName = payloads.value(QStringLiteral("image")).toString();
    }
    if (payloadsResult != nullptr) {
        *payloadsResult = payloads;
    }
    *result = std::move(record);
    return true;
}

bool snapshotToDisk(const QString& root, const Snapshot& snapshot,
                    QHash<QString, quint64>* committedPayloadRevisions) {
    if (committedPayloadRevisions == nullptr ||
        !QDir().mkpath(QDir(root).filePath(QStringLiteral("pins")))) {
        return false;
    }
    for (const StoredRecord& stored : snapshot.records) {
        if (committedPayloadRevisions->value(stored.record.id, 0) != stored.payloadRevision &&
            !writePayload(root, stored)) {
            return false;
        }
    }
    QJsonArray groups;
    for (const auto& group : snapshot.groups) {
        groups.push_back(groupToJson(group));
    }
    QJsonArray records;
    for (const auto& stored : snapshot.records) {
        records.push_back(recordToJson(stored.record, payloadsDescriptor(stored)));
    }
    const QByteArray bytes = jsonBytes(QJsonObject{
        {QStringLiteral("format_version"), kFormatVersion},
        {QStringLiteral("active_group_id"), snapshot.activeGroupId},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("records"), records},
    });
    if (!writeBytes(QDir(root).filePath(QString::fromLatin1(kManifestName)), bytes)) {
        return false;
    }
    QSet<QString> retainedIds;
    for (const auto& stored : snapshot.records) {
        retainedIds.insert(stored.record.id);
        committedPayloadRevisions->insert(stored.record.id, stored.payloadRevision);
    }
    for (auto it = committedPayloadRevisions->begin(); it != committedPayloadRevisions->end();) {
        if (retainedIds.contains(it.key())) {
            ++it;
            continue;
        }
        const QString obsoleteDirectory = payloadDirectory(root, it.key());
        if (QDir(obsoleteDirectory).removeRecursively() || !QFileInfo::exists(obsoleteDirectory)) {
            it = committedPayloadRevisions->erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

} // namespace

struct PinnedWindowRepository::Impl final {
    QString root;
    bool writeAvailable = false;
    int debounceMilliseconds = 1000;
    mutable std::mutex mutex;
    QString error;
    QHash<QString, StoredRecord> records;
    QVector<PinnedWindowGroup> groups{defaultGroup()};
    QString activeGroupId = QString::fromLatin1(kDefaultGroupId);
    quint64 revision = 0;
    quint64 attemptCount = 0;
    bool dirty = false;
    bool flushRequested = false;
    bool stopping = false;
    bool activeWrite = false;
    std::condition_variable condition;
    std::thread writer;
    QHash<QString, quint64> committedPayloadRevisions;
    quint64 nextPayloadRevision = 1;

    Snapshot snapshotLocked() const {
        Snapshot snapshot;
        snapshot.groups = groups;
        snapshot.activeGroupId = activeGroupId;
        snapshot.records.reserve(records.size());
        for (const auto& stored : records) {
            snapshot.records.push_back(stored);
        }
        std::sort(snapshot.records.begin(), snapshot.records.end(),
                  [](const auto& first, const auto& second) {
                      return first.record.id < second.record.id;
                  });
        snapshot.revision = revision;
        return snapshot;
    }

    void markDirtyLocked() {
        ++revision;
        dirty = true;
        condition.notify_one();
    }
};

PinnedWindowRepository::PinnedWindowRepository(QString configurationDirectory, bool writeAvailable,
                                               int debounceMilliseconds)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->root = QDir(configurationDirectory).filePath(QString::fromLatin1(kDirectoryName));
    m_impl->writeAvailable = writeAvailable && !configurationDirectory.isEmpty();
    m_impl->debounceMilliseconds = std::clamp(debounceMilliseconds, 0, 30000);
    if (!configurationDirectory.isEmpty()) {
        QDir().mkpath(m_impl->root);
    }

    const QString manifestPath = QDir(m_impl->root).filePath(QString::fromLatin1(kManifestName));
    QFile manifest(manifestPath);
    if (!configurationDirectory.isEmpty() && manifest.exists()) {
        QJsonParseError parseError;
        if (!manifest.open(QIODevice::ReadOnly)) {
            m_impl->error = QStringLiteral("Pinned-window index could not be read");
        } else {
            const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
            const QJsonObject object = document.object();
            if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
                object.value(QStringLiteral("format_version")).toInt() != kFormatVersion) {
                m_impl->error = QStringLiteral("Pinned-window index is malformed or unsupported");
                preserveInvalidIndex(manifestPath);
            } else {
                const QJsonArray groups = object.value(QStringLiteral("groups")).toArray();
                for (const QJsonValue& value : groups) {
                    if (!value.isObject() || m_impl->groups.size() >= kMaximumGroups) {
                        continue;
                    }
                    const QJsonObject groupObject = value.toObject();
                    PinnedWindowGroup group;
                    group.id = groupObject.value(QStringLiteral("id")).toString().trimmed();
                    group.name = groupObject.value(QStringLiteral("name")).toString().trimmed();
                    if (!safeGroupId(group.id) ||
                        group.id == QString::fromLatin1(kDefaultGroupId) || group.name.isEmpty() ||
                        group.name.size() > 16 || groupNameInUse(m_impl->groups, group.name) ||
                        std::any_of(
                            m_impl->groups.cbegin(), m_impl->groups.cend(),
                            [&group](const auto& existing) { return existing.id == group.id; })) {
                        continue;
                    }
                    group.builtIn = false;
                    m_impl->groups.push_back(std::move(group));
                }
                const QString active = object.value(QStringLiteral("active_group_id")).toString();
                if (std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                                [&active](const auto& group) { return group.id == active; })) {
                    m_impl->activeGroupId = active;
                }
                const QJsonArray records = object.value(QStringLiteral("records")).toArray();
                for (const QJsonValue& value : records) {
                    if (!value.isObject() || m_impl->records.size() >= kMaximumRecords) {
                        continue;
                    }
                    PinnedWindowRecord record;
                    QJsonObject payloads;
                    if (!parseRecord(value.toObject(), m_impl->root, &record, &payloads) ||
                        !std::any_of(
                            m_impl->groups.cbegin(), m_impl->groups.cend(),
                            [&record](const auto& group) { return group.id == record.groupId; })) {
                        continue;
                    }
                    const QString id = record.id;
                    const PayloadSignature signature = payloadSignature(record);
                    m_impl->records.insert(
                        id, StoredRecord{std::move(record), 1, std::move(payloads), signature, {}});
                    m_impl->committedPayloadRevisions.insert(id, 1);
                }
            }
        }
    }

    if (m_impl->writeAvailable) {
        m_impl->writer = std::thread([impl = m_impl.get()]() {
            std::unique_lock lock(impl->mutex);
            int retryMilliseconds = 0;
            for (;;) {
                if (impl->stopping && !impl->dirty && !impl->activeWrite) {
                    break;
                }
                if (!impl->dirty) {
                    impl->condition.wait(lock, [impl]() { return impl->stopping || impl->dirty; });
                    continue;
                }
                if (!impl->flushRequested && impl->debounceMilliseconds > 0) {
                    const quint64 observedRevision = impl->revision;
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(impl->debounceMilliseconds);
                    if (impl->condition.wait_until(lock, deadline, [impl, observedRevision]() {
                            return impl->stopping || impl->flushRequested || !impl->dirty ||
                                   impl->revision != observedRevision;
                        })) {
                        continue;
                    }
                }
                Snapshot snapshot = impl->snapshotLocked();
                impl->activeWrite = true;
                lock.unlock();
                const bool success =
                    snapshotToDisk(impl->root, snapshot, &impl->committedPayloadRevisions);
                lock.lock();
                impl->activeWrite = false;
                ++impl->attemptCount;
                if (success) {
                    retryMilliseconds = 0;
                    impl->error.clear();
                    // The payloads are on disk now, so the records that were
                    // part of this commit release their resident copies; a
                    // record upserted with a newer payload keeps its data
                    // until that payload is committed.
                    for (auto it = impl->records.begin(); it != impl->records.end(); ++it) {
                        if (impl->committedPayloadRevisions.value(it.key(), 0) ==
                            it->payloadRevision) {
                            demoteCommittedRecord(*it);
                        }
                    }
                    if (impl->revision == snapshot.revision) {
                        impl->dirty = false;
                        impl->flushRequested = false;
                    }
                } else {
                    impl->error = QStringLiteral("Pinned-window index could not be saved");
                    qCWarning(storageLog) << impl->error << "root:" << impl->root;
                    if (impl->stopping) {
                        impl->dirty = false;
                        impl->flushRequested = false;
                        impl->condition.notify_all();
                        break;
                    }
                    retryMilliseconds =
                        retryMilliseconds == 0 ? 100 : std::min(retryMilliseconds * 5, 30000);
                    impl->flushRequested = false;
                    impl->condition.wait_for(
                        lock, std::chrono::milliseconds(retryMilliseconds),
                        [impl]() { return impl->stopping || impl->flushRequested; });
                }
                impl->condition.notify_all();
            }
            impl->condition.notify_all();
        });
    }
}

PinnedWindowRepository::~PinnedWindowRepository() {
    if (m_impl == nullptr) {
        return;
    }
    static_cast<void>(flush());
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
        m_impl->flushRequested = true;
        m_impl->condition.notify_all();
    }
    if (m_impl->writer.joinable()) {
        m_impl->writer.join();
    }
}

QVector<PinnedWindowRecord> PinnedWindowRepository::records() const {
    QVector<PinnedWindowRecord> result;
    if (m_impl == nullptr) {
        return result;
    }
    QVector<StoredRecord> storedRecords;
    {
        std::lock_guard locker(m_impl->mutex);
        storedRecords.reserve(m_impl->records.size());
        for (const auto& stored : m_impl->records) {
            storedRecords.push_back(stored);
        }
    }
    result.reserve(storedRecords.size());
    for (StoredRecord& stored : storedRecords) {
        if (!stored.payloads.isEmpty() &&
            !loadPayloads(m_impl->root, stored.payloads, &stored.record)) {
            continue;
        }
        result.push_back(std::move(stored.record));
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (first.updatedUtc == second.updatedUtc) {
            return first.id < second.id;
        }
        return first.updatedUtc < second.updatedUtc;
    });
    return result;
}

std::optional<PinnedWindowRecord> PinnedWindowRepository::loadRecord(const QString& id) const {
    if (m_impl == nullptr || !safeId(id)) {
        return std::nullopt;
    }
    StoredRecord stored;
    {
        std::lock_guard locker(m_impl->mutex);
        const auto found = m_impl->records.constFind(id);
        if (found == m_impl->records.cend()) {
            return std::nullopt;
        }
        stored = found.value();
    }
    if (!stored.payloads.isEmpty() &&
        !loadPayloads(m_impl->root, stored.payloads, &stored.record)) {
        return std::nullopt;
    }
    return std::move(stored.record);
}

QVector<PinnedWindowSummary> PinnedWindowRepository::summaries() const {
    QVector<PinnedWindowSummary> result;
    if (m_impl == nullptr) {
        return result;
    }
    std::lock_guard locker(m_impl->mutex);
    result.reserve(m_impl->records.size());
    for (const auto& stored : m_impl->records) {
        result.push_back({stored.record.id, stored.record.groupId, stored.record.updatedUtc});
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (first.updatedUtc == second.updatedUtc) {
            return first.id < second.id;
        }
        return first.updatedUtc < second.updatedUtc;
    });
    return result;
}

quint64 PinnedWindowRepository::revision() const {
    if (m_impl == nullptr) {
        return 0;
    }
    std::lock_guard locker(m_impl->mutex);
    return m_impl->revision;
}

QVector<PinnedWindowGroup> PinnedWindowRepository::groups() const {
    if (m_impl == nullptr) {
        return {};
    }
    std::lock_guard locker(m_impl->mutex);
    return m_impl->groups;
}

QString PinnedWindowRepository::activeGroupId() const {
    if (m_impl == nullptr) {
        return QString::fromLatin1(kDefaultGroupId);
    }
    std::lock_guard locker(m_impl->mutex);
    return m_impl->activeGroupId;
}

StorageResult PinnedWindowRepository::setActiveGroup(const QString& groupId) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    std::lock_guard locker(m_impl->mutex);
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&groupId](const auto& group) { return group.id == groupId; })) {
        return StorageResult::failure(QStringLiteral("Pinned-window active group is invalid"));
    }
    if (m_impl->activeGroupId != groupId) {
        m_impl->activeGroupId = groupId;
        m_impl->markDirtyLocked();
    }
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::setGroups(QVector<PinnedWindowGroup> groups,
                                                const QString& activeGroupId) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    QVector<PinnedWindowGroup> normalized{defaultGroup()};
    for (PinnedWindowGroup group : groups) {
        group.id = group.id.trimmed();
        group.name = group.name.trimmed();
        if (group.id.isEmpty() || group.id == QString::fromLatin1(kDefaultGroupId)) {
            continue;
        }
        if (!safeGroupId(group.id) || group.name.isEmpty() || group.name.size() > 16 ||
            groupNameInUse(normalized, group.name) ||
            std::any_of(normalized.cbegin(), normalized.cend(),
                        [&group](const auto& existing) { return existing.id == group.id; })) {
            return StorageResult::failure(
                QStringLiteral("Pinned-window group definition is invalid"));
        }
        if (normalized.size() >= kMaximumGroups) {
            return StorageResult::failure(QStringLiteral("Pinned-window group limit reached"));
        }
        group.builtIn = false;
        normalized.push_back(std::move(group));
    }
    std::lock_guard locker(m_impl->mutex);
    m_impl->groups = std::move(normalized);
    m_impl->activeGroupId =
        std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                    [&activeGroupId](const auto& group) { return group.id == activeGroupId; })
            ? activeGroupId
            : QString::fromLatin1(kDefaultGroupId);
    for (auto it = m_impl->records.begin(); it != m_impl->records.end(); ++it) {
        if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                         [&it](const auto& group) { return group.id == it->record.groupId; })) {
            it->record.groupId = QString::fromLatin1(kDefaultGroupId);
        }
    }
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::removeEmptyGroups() {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    std::lock_guard locker(m_impl->mutex);
    QSet<QString> occupied;
    for (const auto& stored : m_impl->records) {
        occupied.insert(stored.record.groupId);
    }
    QVector<PinnedWindowGroup> kept;
    for (const auto& group : m_impl->groups) {
        if (group.id == QString::fromLatin1(kDefaultGroupId) || occupied.contains(group.id)) {
            kept.push_back(group);
        }
    }
    if (kept.size() == m_impl->groups.size()) {
        return StorageResult::ok();
    }
    m_impl->groups = std::move(kept);
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [this](const auto& group) { return group.id == m_impl->activeGroupId; })) {
        m_impl->activeGroupId = QString::fromLatin1(kDefaultGroupId);
    }
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::setRecordGroup(const QString& recordId,
                                                     const QString& groupId) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    std::lock_guard locker(m_impl->mutex);
    auto record = m_impl->records.find(recordId);
    if (record == m_impl->records.end() ||
        !std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&groupId](const auto& group) { return group.id == groupId; })) {
        return StorageResult::failure(QStringLiteral("Pinned-window group assignment is invalid"));
    }
    if (record->record.groupId != groupId) {
        record->record.groupId = groupId;
        m_impl->markDirtyLocked();
    }
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::create(PinnedWindowRecord record,
                                             PreparedPngImage sourceImage) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (record.id.isEmpty()) {
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!sourceImage.isValid() || !safeId(record.id) || !record.nativeGeometry.isValid() ||
        record.nativeGeometry.isEmpty() || !record.canvasSourceRect.isValid() ||
        record.canvasSourceRect.isEmpty() || !record.contentCanvasRect.isValid() ||
        record.contentCanvasRect.isEmpty() || !record.surfaceCanvasRect.isValid() ||
        record.surfaceCanvasRect.isEmpty() || !record.initialPhysicalSize.isValid() ||
        record.initialPhysicalSize.isEmpty() ||
        record.sourceKind != PinnedWindowSourceKind::ImageData || record.image.isNull() ||
        record.image.size() != sourceImage.pixelSize()) {
        return StorageResult::failure(QStringLiteral("Pinned-window source is invalid"));
    }
    if (record.originalHtml.toUtf8().size() > kMaximumPayloadBytes ||
        record.originalText.toUtf8().size() > kMaximumPayloadBytes ||
        record.resultStyle.size() > kMaximumPayloadBytes ||
        record.canvasSession.size() > kMaximumPayloadBytes ||
        record.recognitionResults.size() > kMaximumPayloadBytes) {
        return StorageResult::failure(QStringLiteral("Pinned-window payload is too large"));
    }
    if (record.updatedUtc.isNull()) {
        record.updatedUtc = QDateTime::currentDateTimeUtc();
    }

    std::lock_guard locker(m_impl->mutex);
    if (m_impl->records.contains(record.id)) {
        return StorageResult::failure(QStringLiteral("Pinned-window record already exists"));
    }
    if (m_impl->records.size() >= kMaximumRecords) {
        return StorageResult::failure(QStringLiteral("Pinned-window record limit reached"));
    }
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&record](const auto& group) { return group.id == record.groupId; })) {
        record.groupId = QString::fromLatin1(kDefaultGroupId);
    }
    const PayloadSignature signature = payloadSignature(record);
    const quint64 payloadRevision = ++m_impl->nextPayloadRevision;
    const QString id = record.id;
    m_impl->records.insert(
        id,
        StoredRecord{std::move(record), payloadRevision, {}, signature, std::move(sourceImage)});
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::create(PinnedWindowRecord record) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (record.id.isEmpty()) {
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!safeId(record.id) || record.sourceKind == PinnedWindowSourceKind::ImageData ||
        !record.nativeGeometry.isValid() || record.nativeGeometry.isEmpty() ||
        !record.canvasSourceRect.isValid() || record.canvasSourceRect.isEmpty() ||
        !record.contentCanvasRect.isValid() || record.contentCanvasRect.isEmpty() ||
        !record.surfaceCanvasRect.isValid() || record.surfaceCanvasRect.isEmpty() ||
        !record.initialPhysicalSize.isValid() || record.initialPhysicalSize.isEmpty()) {
        return StorageResult::failure(QStringLiteral("Pinned-window source is invalid"));
    }
    if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile &&
        record.originalFileName.isEmpty()) {
        record.originalFileName = QFileInfo(record.originalFilePath).fileName();
    }
    if ((record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile &&
         (!safeFileName(record.originalFileName) ||
          !QFileInfo(record.originalFilePath).isFile())) ||
        record.originalHtml.toUtf8().size() > kMaximumPayloadBytes ||
        record.originalText.toUtf8().size() > kMaximumPayloadBytes ||
        record.resultStyle.size() > kMaximumPayloadBytes ||
        record.canvasSession.size() > kMaximumPayloadBytes ||
        record.recognitionResults.size() > kMaximumPayloadBytes) {
        return StorageResult::failure(QStringLiteral("Pinned-window source payload is invalid"));
    }
    if (record.updatedUtc.isNull()) {
        record.updatedUtc = QDateTime::currentDateTimeUtc();
    }

    std::lock_guard locker(m_impl->mutex);
    if (m_impl->records.contains(record.id)) {
        return StorageResult::failure(QStringLiteral("Pinned-window record already exists"));
    }
    if (m_impl->records.size() >= kMaximumRecords) {
        return StorageResult::failure(QStringLiteral("Pinned-window record limit reached"));
    }
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&record](const auto& group) { return group.id == record.groupId; })) {
        record.groupId = QString::fromLatin1(kDefaultGroupId);
    }
    const PayloadSignature signature = payloadSignature(record);
    const quint64 payloadRevision = ++m_impl->nextPayloadRevision;
    const QString id = record.id;
    m_impl->records.insert(id, StoredRecord{std::move(record), payloadRevision, {}, signature, {}});
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::updateState(PinnedWindowRecord record) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (!safeId(record.id) || !record.nativeGeometry.isValid() || record.nativeGeometry.isEmpty() ||
        !record.canvasSourceRect.isValid() || record.canvasSourceRect.isEmpty() ||
        !record.contentCanvasRect.isValid() || record.contentCanvasRect.isEmpty() ||
        !record.surfaceCanvasRect.isValid() || record.surfaceCanvasRect.isEmpty() ||
        !record.initialPhysicalSize.isValid() || record.initialPhysicalSize.isEmpty() ||
        record.canvasSession.size() > kMaximumPayloadBytes ||
        record.recognitionResults.size() > kMaximumPayloadBytes) {
        return StorageResult::failure(QStringLiteral("Pinned-window state is invalid"));
    }
    if (record.updatedUtc.isNull()) {
        record.updatedUtc = QDateTime::currentDateTimeUtc();
    }

    std::lock_guard locker(m_impl->mutex);
    auto existing = m_impl->records.find(record.id);
    if (existing == m_impl->records.end()) {
        return StorageResult::failure(QStringLiteral("Pinned-window record does not exist"));
    }
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&record](const auto& group) { return group.id == record.groupId; })) {
        record.groupId = QString::fromLatin1(kDefaultGroupId);
    }

    // A committed record may have its mutable payloads demoted from memory
    // while their descriptors remain in the manifest. Compare both the
    // resident bytes and descriptor presence so clearing a lazy payload
    // removes its on-disk file instead of treating it as unchanged.
    const bool existingHasCanvasSession =
        !existing->record.canvasSession.isEmpty() ||
        existing->payloads.contains(QStringLiteral("canvas_session"));
    const bool incomingHasCanvasSession = !record.canvasSession.isEmpty();
    const bool existingHasRecognitionResults =
        !existing->record.recognitionResults.isEmpty() ||
        existing->payloads.contains(QStringLiteral("recognition_results"));
    const bool incomingHasRecognitionResults = !record.recognitionResults.isEmpty();
    const bool statePayloadChanged =
        (existingHasCanvasSession != incomingHasCanvasSession) ||
        (existingHasRecognitionResults != incomingHasRecognitionResults) ||
        existing->record.canvasSession != record.canvasSession ||
        existing->record.recognitionResults != record.recognitionResults;
    record.sourceKind = existing->record.sourceKind;
    record.image = existing->record.image;
    record.originalFilePath = existing->record.originalFilePath;
    record.originalFileName = existing->record.originalFileName;
    record.originalHtml = existing->record.originalHtml;
    record.originalText = existing->record.originalText;
    record.resultStyle = existing->record.resultStyle;

    QJsonObject payloads = existing->payloads;
    if (statePayloadChanged) {
        if (record.canvasSession.isEmpty()) {
            payloads.remove(QStringLiteral("canvas_session"));
        } else {
            payloads.insert(QStringLiteral("canvas_session"), QStringLiteral("canvas_session.bin"));
        }
        if (record.recognitionResults.isEmpty()) {
            payloads.remove(QStringLiteral("recognition_results"));
        } else {
            payloads.insert(QStringLiteral("recognition_results"),
                            QStringLiteral("recognition_results.bin"));
        }
    }
    StoredRecord updated{std::move(record),
                         statePayloadChanged ? ++m_impl->nextPayloadRevision
                                             : existing->payloadRevision,
                         std::move(payloads),
                         {},
                         existing->preparedSource};
    updated.signature = payloadSignature(updated.record);
    if (!updated.payloads.isEmpty()) {
        clearResidentImmutableSource(&updated.record);
        if (!statePayloadChanged) {
            updated.record.canvasSession.clear();
            updated.record.recognitionResults.clear();
        }
    }
    const QString id = updated.record.id;
    m_impl->records.insert(id, std::move(updated));
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::upsert(PinnedWindowRecord record) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (record.id.isEmpty()) {
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!safeId(record.id) || !record.nativeGeometry.isValid() || record.nativeGeometry.isEmpty() ||
        !record.canvasSourceRect.isValid() || record.canvasSourceRect.isEmpty() ||
        !record.contentCanvasRect.isValid() || record.contentCanvasRect.isEmpty() ||
        !record.surfaceCanvasRect.isValid() || record.surfaceCanvasRect.isEmpty() ||
        !record.initialPhysicalSize.isValid() || record.initialPhysicalSize.isEmpty()) {
        return StorageResult::failure(QStringLiteral("Pinned-window record is invalid"));
    }
    if (record.updatedUtc.isNull()) {
        record.updatedUtc = QDateTime::currentDateTimeUtc();
    }
    if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile &&
        record.originalFileName.isEmpty()) {
        record.originalFileName = QFileInfo(record.originalFilePath).fileName();
    }
    if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile &&
        !safeFileName(record.originalFileName)) {
        return StorageResult::failure(QStringLiteral("Pinned-window source filename is invalid"));
    }
    if (record.sourceKind == PinnedWindowSourceKind::ImageData && record.image.isNull()) {
        return StorageResult::failure(QStringLiteral("Pinned-window image is missing"));
    }
    if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile &&
        !QFileInfo(record.originalFilePath).isFile()) {
        return StorageResult::failure(QStringLiteral("Pinned-window source file is invalid"));
    }
    if (record.originalHtml.toUtf8().size() > kMaximumPayloadBytes ||
        record.originalText.toUtf8().size() > kMaximumPayloadBytes ||
        record.resultStyle.size() > kMaximumPayloadBytes ||
        record.canvasSession.size() > kMaximumPayloadBytes ||
        record.recognitionResults.size() > kMaximumPayloadBytes) {
        return StorageResult::failure(QStringLiteral("Pinned-window payload is too large"));
    }
    std::lock_guard locker(m_impl->mutex);
    if (!std::any_of(m_impl->groups.cbegin(), m_impl->groups.cend(),
                     [&record](const auto& group) { return group.id == record.groupId; })) {
        record.groupId = QString::fromLatin1(kDefaultGroupId);
    }
    auto existing = m_impl->records.find(record.id);
    const bool isNew = existing == m_impl->records.end();
    if (isNew && m_impl->records.size() >= kMaximumRecords) {
        return StorageResult::failure(QStringLiteral("Pinned-window record limit reached"));
    }
    const PayloadSignature signature = payloadSignature(record);
    const bool payloadChanged = isNew || !samePayload(*existing, record, signature);
    const quint64 payloadRevision =
        payloadChanged ? ++m_impl->nextPayloadRevision : existing->payloadRevision;
    const QString id = record.id;
    if (!payloadChanged && !existing->payloads.isEmpty()) {
        // The stored descriptor already describes this exact committed
        // payload, so the update keeps the lazy form instead of holding a
        // fresh resident copy until the writer runs.
        StoredRecord stored{std::move(record), payloadRevision, existing->payloads, signature};
        clearResidentPayload(&stored.record);
        m_impl->records.insert(id, std::move(stored));
    } else {
        m_impl->records.insert(id,
                               StoredRecord{std::move(record), payloadRevision, {}, signature, {}});
    }
    m_impl->markDirtyLocked();
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::remove(const QString& id) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (!safeId(id)) {
        return StorageResult::failure(QStringLiteral("Pinned-window id is invalid"));
    }
    {
        std::lock_guard locker(m_impl->mutex);
        const bool existed = m_impl->records.contains(id);
        m_impl->records.remove(id);
        if (existed) {
            m_impl->markDirtyLocked();
        }
    }
    return StorageResult::ok();
}

StorageResult PinnedWindowRepository::flush() {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->dirty) {
        return StorageResult::ok();
    }
    const quint64 initialAttemptCount = m_impl->attemptCount;
    m_impl->flushRequested = true;
    m_impl->condition.notify_one();
    m_impl->condition.wait(lock, [this, initialAttemptCount]() {
        return (!m_impl->dirty && !m_impl->activeWrite) ||
               (m_impl->attemptCount > initialAttemptCount && !m_impl->error.isEmpty());
    });
    return m_impl->error.isEmpty() ? StorageResult::ok() : StorageResult::failure(m_impl->error);
}

QString PinnedWindowRepository::lastError() const {
    if (m_impl == nullptr) {
        return QStringLiteral("Pinned-window storage unavailable");
    }
    std::lock_guard locker(m_impl->mutex);
    return m_impl->error;
}

} // namespace snow_shot::storage
