#include "snow_shot/storage/pinnedwindowrepository.h"

#include "snowimageqtcodec.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUuid>

#include <snow/image/format.h>

#include <cmath>
#include <limits>

namespace snow_shot::storage {
namespace {
constexpr int kFormatVersion = 1;
constexpr int kMaximumRecords = 128;
constexpr qint64 kMaximumImageBytes = 256LL * 1024LL * 1024LL;
constexpr qint64 kMaximumPayloadBytes = 32LL * 1024LL * 1024LL;
constexpr auto kDirectoryName = "pinned_windows";
constexpr auto kManifestName = "manifest.json";

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
    return {{QStringLiteral("x"), rect.x()}, {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()}, {QStringLiteral("height"), rect.height()}};
}

QJsonObject rectToJson(const QRect& rect) {
    return {{QStringLiteral("x"), rect.x()}, {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()}, {QStringLiteral("height"), rect.height()}};
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
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
    if (!finiteNumber(object.value(QStringLiteral("x")), std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max(), &x) ||
        !finiteNumber(object.value(QStringLiteral("y")), std::numeric_limits<int>::min(),
                      std::numeric_limits<int>::max(), &y) ||
        !finiteNumber(object.value(QStringLiteral("width")), 1.0, std::numeric_limits<int>::max(),
                      &width) ||
        !finiteNumber(object.value(QStringLiteral("height")), 1.0,
                      std::numeric_limits<int>::max(), &height)) {
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
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
    if (!finiteNumber(object.value(QStringLiteral("x")), -1e9, 1e9, &x) ||
        !finiteNumber(object.value(QStringLiteral("y")), -1e9, 1e9, &y) ||
        !finiteNumber(object.value(QStringLiteral("width")), 0.001, 1e9, &width) ||
        !finiteNumber(object.value(QStringLiteral("height")), 0.001, 1e9, &height)) {
        return false;
    }
    *result = QRectF(x, y, width, height);
    return result->isValid() && !result->isEmpty();
}

QJsonObject sizeToJson(const QSize& size) {
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}

bool sizeFromJson(const QJsonValue& value, QSize* result) {
    QRect rect;
    if (!rectFromJson(QJsonObject{{QStringLiteral("x"), 0}, {QStringLiteral("y"), 0},
                                  {QStringLiteral("width"), value.toObject().value(QStringLiteral("width"))},
                                  {QStringLiteral("height"), value.toObject().value(QStringLiteral("height"))}},
                      &rect)) {
        return false;
    }
    *result = rect.size();
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
    double m[9];
    for (int i = 0; i < 9; ++i) {
        if (!finiteNumber(values.at(i), -1e6, 1e6, &m[i])) {
            return false;
        }
    }
    *result = QTransform(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    return true;
}

bool safeId(const QString& id) {
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
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

} // namespace

struct PinnedWindowRepository::Impl {
    QString root;
    bool writeAvailable = false;
    QString error;
    QHash<QString, PinnedWindowRecord> records;
};

PinnedWindowRepository::PinnedWindowRepository(QString configurationDirectory, bool writeAvailable)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->root = QDir(configurationDirectory).filePath(QString::fromLatin1(kDirectoryName));
    m_impl->writeAvailable = writeAvailable && !configurationDirectory.isEmpty();
    QDir().mkpath(m_impl->root);

    QFile manifest(QDir(m_impl->root).filePath(QString::fromLatin1(kManifestName)));
    if (!manifest.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_impl->error = QStringLiteral("Pinned-window manifest is malformed");
        return;
    }
    if (document.object().value(QStringLiteral("format_version")).toInt() != kFormatVersion) {
        m_impl->error = QStringLiteral("Pinned-window manifest version is unsupported");
        return;
    }
    const QJsonArray records = document.object().value(QStringLiteral("records")).toArray();
    for (const QJsonValue& value : records) {
        if (!value.isObject() || m_impl->records.size() >= kMaximumRecords) {
            continue;
        }
        const QJsonObject object = value.toObject();
        PinnedWindowRecord record;
        record.id = object.value(QStringLiteral("id")).toString();
        if (!safeId(record.id) || !sourceKindFromString(object.value(QStringLiteral("source_kind")).toString(),
                                                         &record.sourceKind)) {
            continue;
        }
        if (!rectFFromJson(object.value(QStringLiteral("canvas_source_rect")),
                           &record.canvasSourceRect) ||
            !rectFFromJson(object.value(QStringLiteral("content_canvas_rect")),
                           &record.contentCanvasRect) ||
            !rectFFromJson(object.value(QStringLiteral("surface_canvas_rect")),
                           &record.surfaceCanvasRect) ||
            !rectFromJson(object.value(QStringLiteral("native_geometry")),
                          &record.nativeGeometry) ||
            !sizeFromJson(object.value(QStringLiteral("initial_physical_size")),
                          &record.initialPhysicalSize)) {
            continue;
        }
        if (!object.value(QStringLiteral("screen_logical_geometry")).isUndefined() &&
            !rectFromJson(object.value(QStringLiteral("screen_logical_geometry")),
                          &record.screenLogicalGeometry)) {
            continue;
        }
        if (!object.value(QStringLiteral("screen_physical_geometry")).isUndefined() &&
            !rectFromJson(object.value(QStringLiteral("screen_physical_geometry")),
                          &record.screenPhysicalGeometry)) {
            continue;
        }
        double number = 0.0;
        if (!finiteNumber(object.value(QStringLiteral("first_creation_text_dpi")), 0.1, 20.0,
                          &record.firstCreationTextDpi) ||
            !finiteNumber(object.value(QStringLiteral("screen_dpi")), 0.1, 20.0,
                          &record.screenDpi) ||
            !finiteNumber(object.value(QStringLiteral("scale_percent")), 1.0, 1000.0,
                          &record.scalePercent) ||
            !finiteNumber(object.value(QStringLiteral("opacity_percent")), 1.0, 100.0,
                          &number)) {
            continue;
        }
        record.opacityPercent = qRound(number);
        record.quarterTurns = object.value(QStringLiteral("quarter_turns")).toInt();
        if (record.quarterTurns < 0 || record.quarterTurns > 3 ||
            !transformFromJson(object.value(QStringLiteral("image_transform")),
                               &record.imageTransform)) {
            continue;
        }
        record.thumbnailMode = object.value(QStringLiteral("thumbnail_mode")).toBool();
        if (record.thumbnailMode &&
            !rectFromJson(object.value(QStringLiteral("pre_thumbnail_geometry")),
                          &record.preThumbnailNativeGeometry)) {
            continue;
        }
        record.screenName = object.value(QStringLiteral("screen_name")).toString();
        record.screenSerial = object.value(QStringLiteral("screen_serial")).toString();
        record.originalFileName = object.value(QStringLiteral("original_file_name")).toString();
        record.originalHtml = QString::fromUtf8(
            QByteArray::fromBase64(object.value(QStringLiteral("original_html")).toString().toUtf8()));
        record.originalText = QString::fromUtf8(
            QByteArray::fromBase64(object.value(QStringLiteral("original_text")).toString().toUtf8()));
        record.resultStyle = QByteArray::fromBase64(
            object.value(QStringLiteral("result_style")).toString().toUtf8());
        record.canvasSession = QByteArray::fromBase64(
            object.value(QStringLiteral("canvas_session")).toString().toUtf8());
        record.recognitionResults = QByteArray::fromBase64(
            object.value(QStringLiteral("recognition_results")).toString().toUtf8());
        record.updatedUtc = QDateTime::fromString(object.value(QStringLiteral("updated_utc")).toString(),
                                                   Qt::ISODateWithMs);
        if (record.resultStyle.size() > kMaximumPayloadBytes ||
            record.canvasSession.size() > kMaximumPayloadBytes ||
            record.recognitionResults.size() > kMaximumPayloadBytes) {
            continue;
        }
        const QString directory = QDir(m_impl->root).filePath(record.id);
        if (record.sourceKind == PinnedWindowSourceKind::ImageData) {
            const QString imagePath = QDir(directory).filePath(QStringLiteral("source.png"));
            if (!QFileInfo::exists(imagePath)) {
                continue;
            }
            record.image = decodeImage(imagePath);
            if (record.image.isNull() || record.image.sizeInBytes() > kMaximumImageBytes) {
                continue;
            }
        } else if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
            if (record.originalFileName.isEmpty() ||
                QFileInfo(record.originalFileName).fileName() != record.originalFileName ||
                record.originalFileName == QStringLiteral(".") ||
                record.originalFileName == QStringLiteral("..")) {
                continue;
            }
            const QString filePath = QDir(directory).filePath(record.originalFileName);
            if (!QFileInfo(filePath).isFile()) {
                continue;
            }
            record.originalFilePath = filePath;
            record.image = decodeImage(filePath, QFileInfo(filePath).suffix());
            if (record.image.isNull()) {
                continue;
            }
        }
        m_impl->records.insert(record.id, std::move(record));
    }
}

PinnedWindowRepository::~PinnedWindowRepository() = default;

QVector<PinnedWindowRecord> PinnedWindowRepository::records() const {
    QVector<PinnedWindowRecord> result;
    if (m_impl == nullptr) {
        return result;
    }
    result.reserve(m_impl->records.size());
    for (const auto& record : m_impl->records) {
        result.push_back(record);
    }
    std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        return first.updatedUtc < second.updatedUtc;
    });
    return result;
}

StorageResult PinnedWindowRepository::upsert(PinnedWindowRecord record) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (record.id.isEmpty()) {
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!safeId(record.id) || !record.nativeGeometry.isValid() || record.nativeGeometry.isEmpty() ||
        !record.canvasSourceRect.isValid() || !record.contentCanvasRect.isValid() ||
        !record.surfaceCanvasRect.isValid() || !record.initialPhysicalSize.isValid()) {
        return StorageResult::failure(QStringLiteral("Pinned-window record is invalid"));
    }
    if (record.updatedUtc.isNull()) {
        record.updatedUtc = QDateTime::currentDateTimeUtc();
    }

    const QString directory = QDir(m_impl->root).filePath(record.id);
    if (!QDir().mkpath(directory)) {
        return StorageResult::failure(QStringLiteral("Pinned-window directory could not be created"));
    }
    if (record.sourceKind == PinnedWindowSourceKind::ImageData) {
        const QByteArray encoded = encodeImage(record.image);
        if (encoded.isEmpty() || encoded.size() > kMaximumImageBytes) {
            return StorageResult::failure(QStringLiteral("Pinned-window image could not be encoded"));
        }
        QSaveFile file(QDir(directory).filePath(QStringLiteral("source.png")));
        if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() ||
            !file.commit()) {
            return StorageResult::failure(QStringLiteral("Pinned-window image could not be saved"));
        }
    } else if (record.sourceKind == PinnedWindowSourceKind::ClipboardImageFile) {
        QString fileName = record.originalFileName;
        if (fileName.isEmpty()) {
            fileName = QFileInfo(record.originalFilePath).fileName();
        }
        if (fileName.isEmpty() || QFileInfo(fileName).fileName() != fileName ||
            !QFileInfo(record.originalFilePath).isFile()) {
            return StorageResult::failure(QStringLiteral("Pinned-window source file is invalid"));
        }
        const QString destination = QDir(directory).filePath(fileName);
        if (QDir::cleanPath(record.originalFilePath) != QDir::cleanPath(destination)) {
            static_cast<void>(QFile::remove(destination));
            if (!QFile::copy(record.originalFilePath, destination)) {
                return StorageResult::failure(
                    QStringLiteral("Pinned-window source file could not be copied"));
            }
        }
        record.originalFileName = fileName;
        record.originalFilePath = destination;
    }

    QJsonObject object{
        {QStringLiteral("id"), record.id},
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
        {QStringLiteral("original_html"), QString::fromUtf8(record.originalHtml.toUtf8().toBase64())},
        {QStringLiteral("original_text"), QString::fromUtf8(record.originalText.toUtf8().toBase64())},
        {QStringLiteral("result_style"), QString::fromUtf8(record.resultStyle.toBase64())},
        {QStringLiteral("canvas_session"), QString::fromUtf8(record.canvasSession.toBase64())},
        {QStringLiteral("recognition_results"), QString::fromUtf8(record.recognitionResults.toBase64())},
        {QStringLiteral("updated_utc"), record.updatedUtc.toUTC().toString(Qt::ISODateWithMs)},
    };
    m_impl->records.insert(record.id, record);
    const StorageResult result = flush();
    if (!result.success) {
        m_impl->records.remove(record.id);
    }
    return result;
}

StorageResult PinnedWindowRepository::remove(const QString& id) {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    if (!safeId(id)) {
        return StorageResult::failure(QStringLiteral("Pinned-window id is invalid"));
    }
    m_impl->records.remove(id);
    const QString directory = QDir(m_impl->root).filePath(id);
    QDir(directory).removeRecursively();
    return flush();
}

StorageResult PinnedWindowRepository::flush() {
    if (m_impl == nullptr || !m_impl->writeAvailable) {
        return StorageResult::failure(QStringLiteral("Pinned-window storage is not writable"));
    }
    QJsonArray array;
    for (const PinnedWindowRecord& record : m_impl->records) {
        QJsonObject object{
            {QStringLiteral("id"), record.id},
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
            {QStringLiteral("original_html"), QString::fromUtf8(record.originalHtml.toUtf8().toBase64())},
            {QStringLiteral("original_text"), QString::fromUtf8(record.originalText.toUtf8().toBase64())},
            {QStringLiteral("result_style"), QString::fromUtf8(record.resultStyle.toBase64())},
            {QStringLiteral("canvas_session"), QString::fromUtf8(record.canvasSession.toBase64())},
            {QStringLiteral("recognition_results"), QString::fromUtf8(record.recognitionResults.toBase64())},
            {QStringLiteral("updated_utc"), record.updatedUtc.toUTC().toString(Qt::ISODateWithMs)},
        };
        array.push_back(std::move(object));
    }
    QSaveFile file(QDir(m_impl->root).filePath(QString::fromLatin1(kManifestName)));
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("format_version"), kFormatVersion},
                                                        {QStringLiteral("records"), array}})
                                 .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_impl->error = QStringLiteral("Pinned-window manifest could not be saved");
        return StorageResult::failure(m_impl->error);
    }
    m_impl->error.clear();
    return StorageResult::ok();
}

QString PinnedWindowRepository::lastError() const {
    return m_impl != nullptr ? m_impl->error : QStringLiteral("Pinned-window storage unavailable");
}

} // namespace snow_shot::storage
