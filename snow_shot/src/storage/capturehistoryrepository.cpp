#include "snow_shot/storage/capturehistoryrepository.h"

#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/storagelogging.h"
#include "snowimageqtcodec.h"

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
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace snow_shot::storage {
namespace {
constexpr int kManifestFormatVersion = 2;
constexpr int kMaximumDisplays = 32;
constexpr qint64 kMaximumPixelsPerImage = 64'000'000;
constexpr qint64 kMaximumPixelsPerRecord = 128'000'000;
constexpr qint64 kMaximumCanvasBytes = 16 * 1024 * 1024;
constexpr qint64 kBytesPerMiB = 1024 * 1024;
constexpr auto kHistoryDirectoryName = "capture_history_records";
constexpr auto kQuarantineDirectoryName = "capture_history_quarantine";
constexpr auto kManifestFileName = "manifest.json";
constexpr auto kCanvasFileName = "canvas_history.json";
constexpr auto kResultFileName = "capture_result.png";

struct StoredRecord {
    CaptureHistoryRecord record;
    QString directoryName;
    QString canvasFileName;
    std::optional<QString> resultFileName;
    QVector<QString> displayFileNames;
    bool payloadValidated = false;
};

struct EncodedDraft {
    CaptureHistoryRecord record;
    QByteArray canvas;
    QVector<QByteArray> images;
    QVector<QString> imageFileNames;
    std::optional<QByteArray> resultImage;
    std::optional<QString> resultImageFileName;
    QJsonObject manifest;
    QByteArray manifestBytes;
};

QString sourceToManifest(CaptureHistorySource source) {
    switch (source) {
    case CaptureHistorySource::CopiedToClipboard:
        return QStringLiteral("copied_to_clipboard");
    case CaptureHistorySource::PinnedToScreen:
        return QStringLiteral("pinned_to_screen");
    case CaptureHistorySource::CurrentMonitor:
        return QStringLiteral("current_monitor");
    case CaptureHistorySource::FocusedWindow:
        return QStringLiteral("focused_window");
    }
    return QStringLiteral("copied_to_clipboard");
}

bool sourceFromManifest(const QString& value, CaptureHistorySource* source) {
    if (source == nullptr) {
        return false;
    }
    if (value == QStringLiteral("copied_to_clipboard")) {
        *source = CaptureHistorySource::CopiedToClipboard;
        return true;
    }
    if (value == QStringLiteral("pinned_to_screen")) {
        *source = CaptureHistorySource::PinnedToScreen;
        return true;
    }
    if (value == QStringLiteral("current_monitor")) {
        *source = CaptureHistorySource::CurrentMonitor;
        return true;
    }
    if (value == QStringLiteral("focused_window")) {
        *source = CaptureHistorySource::FocusedWindow;
        return true;
    }
    return false;
}

QJsonObject rectangleToJson(const QRect& rectangle) {
    return {
        {QStringLiteral("x"), rectangle.x()},
        {QStringLiteral("y"), rectangle.y()},
        {QStringLiteral("width"), rectangle.width()},
        {QStringLiteral("height"), rectangle.height()},
    };
}

bool jsonInteger(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64* result) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() ||
        value.toDouble() < static_cast<double>(minimum) ||
        value.toDouble() > static_cast<double>(maximum)) {
        return false;
    }
    if (result != nullptr) {
        *result = static_cast<qint64>(value.toDouble());
    }
    return true;
}

bool rectangleFromJson(const QJsonValue& value, QRect* rectangle) {
    if (rectangle == nullptr || !value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    qint64 x = 0;
    qint64 y = 0;
    qint64 width = 0;
    qint64 height = 0;
    if (!jsonInteger(object.value(QStringLiteral("x")), std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::max(), &x) ||
        !jsonInteger(object.value(QStringLiteral("y")), std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::max(), &y) ||
        !jsonInteger(object.value(QStringLiteral("width")), 1, std::numeric_limits<int>::max(),
                     &width) ||
        !jsonInteger(object.value(QStringLiteral("height")), 1, std::numeric_limits<int>::max(),
                     &height)) {
        return false;
    }
    *rectangle = QRect(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                       static_cast<int>(height));
    return true;
}

bool selectionFromJson(const QJsonValue& value, PersistedSelection* selection) {
    if (selection == nullptr) {
        return false;
    }
    const PersistedSelectionNormalization normalized = normalizePersistedSelection(value);
    if (!normalized.valid) {
        return false;
    }
    *selection = normalized.value;
    return true;
}

bool validUuid(const QString& text) {
    const QUuid uuid(text);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text;
}

bool validCanvasFile(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > kMaximumCanvasBytes) {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && !document.isNull() &&
           (document.isObject() || document.isArray());
}

bool safeLocalFileName(const QString& fileName) {
    return !fileName.isEmpty() && !QDir::isAbsolutePath(fileName) && !fileName.contains(u'/') &&
           !fileName.contains(u'\\') && fileName != QStringLiteral(".") &&
           fileName != QStringLiteral("..") && QFileInfo(fileName).fileName() == fileName;
}

QString normalizedAbsolutePath(const QString& path) {
    return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
}

bool pathInsideRoot(const QString& root, const QString& candidate) {
    const QString absoluteRoot = normalizedAbsolutePath(root);
    const QString absoluteCandidate = normalizedAbsolutePath(candidate);
    const QString prefix = absoluteRoot.endsWith(u'/') ? absoluteRoot : absoluteRoot + u'/';
    const Qt::CaseSensitivity sensitivity =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    if (!absoluteCandidate.startsWith(prefix, sensitivity)) {
        return false;
    }
    const QFileInfo candidateInfo(absoluteCandidate);
    if (!candidateInfo.exists()) {
        return true;
    }
    const QString canonicalRoot =
        QDir::fromNativeSeparators(QFileInfo(absoluteRoot).canonicalFilePath());
    const QString canonicalCandidate =
        QDir::fromNativeSeparators(candidateInfo.canonicalFilePath());
    const QString canonicalPrefix =
        canonicalRoot.endsWith(u'/') ? canonicalRoot : canonicalRoot + u'/';
    return !canonicalRoot.isEmpty() && !canonicalCandidate.isEmpty() &&
           canonicalCandidate.startsWith(canonicalPrefix, sensitivity);
}

QByteArray jsonBytes(const QJsonObject& object) {
    QByteArray result = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!result.endsWith('\n')) {
        result.append('\n');
    }
    return result;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

qint64 directoryBytes(const QString& path) {
    const QFileInfo root(path);
    if (!root.exists()) {
        return 0;
    }
    if (root.isFile() && !root.isSymLink()) {
        return root.size();
    }
    qint64 total = 0;
    const QFileInfoList entries = QDir(path).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        total += entry.isDir() ? directoryBytes(entry.absoluteFilePath()) : entry.size();
    }
    return total;
}

template <typename Result> std::shared_future<Result> readyFuture(Result result) {
    auto promise = std::make_shared<std::promise<Result>>();
    std::shared_future<Result> future = promise->get_future().share();
    promise->set_value(std::move(result));
    return future;
}

QJsonObject buildManifest(const CaptureHistoryRecord& record, const QString& canvasFileName,
                          const QVector<QString>& imageFileNames,
                          const std::optional<QString>& resultFileName, qint64 totalBytes) {
    QJsonArray displays;
    for (qsizetype index = 0; index < record.displays.size(); ++index) {
        const CaptureHistoryDisplayRecord& display = record.displays[index];
        displays.push_back(QJsonObject{
            {QStringLiteral("stable_id"), display.stableId},
            {QStringLiteral("display_name"), display.name},
            {QStringLiteral("image_file"), imageFileNames[index]},
            {QStringLiteral("width"), display.imageSize.width()},
            {QStringLiteral("height"), display.imageSize.height()},
            {QStringLiteral("encoded_bytes"), display.encodedBytes},
        });
    }
    QJsonObject manifest{
        {QStringLiteral("format_version"), kManifestFormatVersion},
        {QStringLiteral("id"), record.id},
        {QStringLiteral("created_utc"), record.createdUtc.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("source"), sourceToManifest(record.source)},
        {QStringLiteral("canvas_bounds"), rectangleToJson(record.canvasBounds)},
        {QStringLiteral("selection"), persistedSelectionToJson(record.selection)},
        {QStringLiteral("canvas_history_file"), canvasFileName},
        {QStringLiteral("canvas_byte_size"), record.canvasBytes},
        {QStringLiteral("displays"), displays},
        {QStringLiteral("total_record_size"), totalBytes},
    };
    if (record.result.has_value() && resultFileName.has_value()) {
        manifest.insert(QStringLiteral("result"), QJsonObject{
                                                   {QStringLiteral("image_file"), *resultFileName},
                                                   {QStringLiteral("width"),
                                                    record.result->imageSize.width()},
                                                   {QStringLiteral("height"),
                                                    record.result->imageSize.height()},
                                                   {QStringLiteral("encoded_bytes"),
                                                    record.result->encodedBytes},
                                               });
    }
    return manifest;
}

bool encodeDraft(const CaptureHistoryDraft& draft, qint64 quotaBytes, EncodedDraft* encoded,
                 QString* error) {
    if (encoded == nullptr || !validUuid(draft.id) || !draft.createdUtc.isValid() ||
        draft.createdUtc.timeSpec() != Qt::UTC || draft.canvasBounds.isEmpty() ||
        draft.selection.rectangle.isEmpty() || !draft.selection.shadowColor.isValid() ||
        draft.selection.cornerRadius < 0 || draft.selection.cornerRadius > 256 ||
        draft.selection.shadowWidth < 0 || draft.selection.shadowWidth > 64 ||
        !validCanvasFile(draft.canvasHistory) || draft.displays.isEmpty() ||
        draft.displays.size() > kMaximumDisplays) {
        if (error != nullptr) {
            *error = QStringLiteral("The capture-history draft is invalid");
        }
        return false;
    }

    EncodedDraft result;
    result.record.id = draft.id;
    result.record.createdUtc = draft.createdUtc.toUTC();
    result.record.canvasBounds = draft.canvasBounds;
    result.record.selection = draft.selection;
    result.record.source = draft.source;
    result.record.canvasBytes = draft.canvasHistory.size();
    result.canvas = draft.canvasHistory;

    qint64 pixelCount = 0;
    qint64 payloadBytes = result.canvas.size();
    if (draft.resultImage.has_value()) {
        const QImage& image = *draft.resultImage;
        const qint64 pixels = static_cast<qint64>(image.width()) *
                              static_cast<qint64>(image.height());
        if (image.isNull() || pixels <= 0 || pixels > kMaximumPixelsPerImage ||
            pixelCount > kMaximumPixelsPerRecord - pixels) {
            if (error != nullptr) {
                *error = QStringLiteral("The capture-history result exceeds the image limits");
            }
            return false;
        }
        pixelCount += pixels;
        const QByteArray png = snow_shot::image_codec::encodePng(image);
        if (png.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("The capture-history result could not be encoded");
            }
            return false;
        }
        payloadBytes += png.size();
        result.resultImage = png;
        result.resultImageFileName = QString::fromLatin1(kResultFileName);
        result.record.result = CaptureHistoryResultRecord{image.size(), png.size()};
    }
    for (qsizetype index = 0; index < draft.displays.size(); ++index) {
        const CaptureHistoryDisplayDraft& display = draft.displays[index];
        const qint64 pixels = static_cast<qint64>(display.image.width()) *
                              static_cast<qint64>(display.image.height());
        if (display.image.isNull() || pixels <= 0 || pixels > kMaximumPixelsPerImage ||
            pixelCount > kMaximumPixelsPerRecord - pixels) {
            if (error != nullptr) {
                *error = QStringLiteral("A capture-history display exceeds the image limits");
            }
            return false;
        }
        pixelCount += pixels;
        const QByteArray png = snow_shot::image_codec::encodePng(display.image);
        if (png.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("A capture-history display could not be encoded");
            }
            return false;
        }
        payloadBytes += png.size();
        result.images.push_back(std::move(png));
        result.imageFileNames.push_back(QStringLiteral("display_%1.png").arg(index));
        result.record.displays.push_back({display.stableId, display.name, display.image.size(),
                                          result.images.constLast().size()});
    }
    if (payloadBytes > quotaBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The capture-history record exceeds the disk quota");
        }
        return false;
    }

    qint64 totalBytes = payloadBytes;
    for (int iteration = 0; iteration < 8; ++iteration) {
        result.manifest = buildManifest(result.record, QString::fromLatin1(kCanvasFileName),
                                        result.imageFileNames, result.resultImageFileName,
                                        totalBytes);
        result.manifestBytes = jsonBytes(result.manifest);
        const qint64 nextTotal = payloadBytes + result.manifestBytes.size();
        if (nextTotal == totalBytes) {
            break;
        }
        totalBytes = nextTotal;
    }
    result.record.totalBytes = payloadBytes + result.manifestBytes.size();
    result.manifest = buildManifest(result.record, QString::fromLatin1(kCanvasFileName),
                                    result.imageFileNames, result.resultImageFileName,
                                    result.record.totalBytes);
    result.manifestBytes = jsonBytes(result.manifest);
    result.record.totalBytes = payloadBytes + result.manifestBytes.size();
    if (result.record.totalBytes > quotaBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The capture-history record exceeds the disk quota");
        }
        return false;
    }
    *encoded = std::move(result);
    return true;
}

bool parseManifest(const QJsonObject& object, const QString& directoryName,
                   const QString& directoryPath, bool validatePayloads, StoredRecord* stored,
                   QString* error) {
    qint64 version = 0;
    qint64 canvasBytes = 0;
    qint64 totalBytes = 0;
    if (!jsonInteger(object.value(QStringLiteral("format_version")), 1,
                     std::numeric_limits<int>::max(), &version)) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest has no supported format version");
        }
        return false;
    }
    if (version != kManifestFormatVersion) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest format is unsupported");
        }
        return false;
    }
    if (stored == nullptr || !object.value(QStringLiteral("id")).isString() ||
        !object.value(QStringLiteral("created_utc")).isString() ||
        !object.value(QStringLiteral("canvas_history_file")).isString() ||
        !object.value(QStringLiteral("displays")).isArray() ||
        !jsonInteger(object.value(QStringLiteral("canvas_byte_size")), 1, kMaximumCanvasBytes,
                     &canvasBytes) ||
        !jsonInteger(object.value(QStringLiteral("total_record_size")), 1,
                     std::numeric_limits<qint64>::max(), &totalBytes)) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest is structurally invalid");
        }
        return false;
    }

    StoredRecord result;
    result.record.id = object.value(QStringLiteral("id")).toString();
    const QString createdText = object.value(QStringLiteral("created_utc")).toString();
    result.record.createdUtc = QDateTime::fromString(createdText, Qt::ISODateWithMs);
    result.record.canvasBytes = canvasBytes;
    result.record.totalBytes = totalBytes;
    const QJsonValue sourceValue = object.value(QStringLiteral("source"));
    if (!sourceValue.isUndefined() && !sourceValue.isString()) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest source is invalid");
        }
        return false;
    }
    const QString source = sourceValue.toString(QStringLiteral("copied_to_clipboard"));
    if (!sourceFromManifest(source, &result.record.source)) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest source is invalid");
        }
        return false;
    }
    result.directoryName = directoryName;
    result.canvasFileName = object.value(QStringLiteral("canvas_history_file")).toString();
    if (!validUuid(result.record.id) || !createdText.endsWith(u'Z') ||
        !result.record.createdUtc.isValid() || result.record.createdUtc.timeSpec() != Qt::UTC ||
        directoryName !=
            result.record.createdUtc.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
                u'-' + result.record.id ||
        !rectangleFromJson(object.value(QStringLiteral("canvas_bounds")),
                           &result.record.canvasBounds) ||
        !selectionFromJson(object.value(QStringLiteral("selection")), &result.record.selection) ||
        !safeLocalFileName(result.canvasFileName)) {
        if (error != nullptr) {
            *error = QStringLiteral("The record manifest metadata is invalid");
        }
        return false;
    }

    QSet<QString> expectedFiles{QString::fromLatin1(kManifestFileName), result.canvasFileName};
    const QString canvasPath = QDir(directoryPath).filePath(result.canvasFileName);
    const QFileInfo canvasInfo(canvasPath);
    if (!pathInsideRoot(directoryPath, canvasPath) || !canvasInfo.isFile() ||
        canvasInfo.isSymLink() || canvasInfo.size() != result.record.canvasBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The record canvas payload is invalid");
        }
        return false;
    }
    if (validatePayloads) {
        QFile canvasFile(canvasPath);
        if (!canvasFile.open(QIODevice::ReadOnly) || !validCanvasFile(canvasFile.readAll())) {
            if (error != nullptr) {
                *error = QStringLiteral("The record canvas payload is malformed");
            }
            return false;
        }
    }

    qint64 pixelsInRecord = 0;
    qint64 calculatedBytes = result.record.canvasBytes;
    const QJsonValue resultValue = object.value(QStringLiteral("result"));
    if (!resultValue.isUndefined() && !resultValue.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("The record result descriptor is invalid");
        }
        return false;
    }
    if (resultValue.isObject()) {
        const QJsonObject resultObject = resultValue.toObject();
        qint64 width = 0;
        qint64 height = 0;
        qint64 encodedBytes = 0;
        const QString imageFile = resultObject.value(QStringLiteral("image_file")).toString();
        if (!resultObject.value(QStringLiteral("image_file")).isString() ||
            !safeLocalFileName(imageFile) || expectedFiles.contains(imageFile) ||
            !jsonInteger(resultObject.value(QStringLiteral("width")), 1,
                         std::numeric_limits<int>::max(), &width) ||
            !jsonInteger(resultObject.value(QStringLiteral("height")), 1,
                         std::numeric_limits<int>::max(), &height) ||
            !jsonInteger(resultObject.value(QStringLiteral("encoded_bytes")), 1,
                         std::numeric_limits<qint64>::max(), &encodedBytes)) {
            if (error != nullptr) {
                *error = QStringLiteral("The record result descriptor is invalid");
            }
            return false;
        }
        const qint64 pixels = width * height;
        if (pixels <= 0 || pixels > kMaximumPixelsPerImage ||
            pixelsInRecord > kMaximumPixelsPerRecord - pixels) {
            if (error != nullptr) {
                *error = QStringLiteral("The record result exceeds the image limits");
            }
            return false;
        }
        pixelsInRecord += pixels;
        const QString imagePath = QDir(directoryPath).filePath(imageFile);
        const QFileInfo imageInfo(imagePath);
        if (!pathInsideRoot(directoryPath, imagePath) || !imageInfo.isFile() ||
            imageInfo.isSymLink() || imageInfo.size() != encodedBytes) {
            if (error != nullptr) {
                *error = QStringLiteral("The record result payload is invalid");
            }
            return false;
        }
        if (validatePayloads && !snow_shot::image_codec::inspectFile(
                                    imagePath, snow::image::Format::png,
                                    QSize(static_cast<int>(width), static_cast<int>(height)))) {
            if (error != nullptr) {
                *error = QStringLiteral("The record result payload is invalid");
            }
            return false;
        }
        expectedFiles.insert(imageFile);
        calculatedBytes += encodedBytes;
        result.resultFileName = imageFile;
        result.record.result = CaptureHistoryResultRecord{
            QSize(static_cast<int>(width), static_cast<int>(height)), encodedBytes};
    }

    const QJsonArray displays = object.value(QStringLiteral("displays")).toArray();
    if (displays.isEmpty() || displays.size() > kMaximumDisplays) {
        if (error != nullptr) {
            *error = QStringLiteral("The record display count is invalid");
        }
        return false;
    }
    for (const QJsonValue& displayValue : displays) {
        if (!displayValue.isObject()) {
            return false;
        }
        const QJsonObject displayObject = displayValue.toObject();
        qint64 width = 0;
        qint64 height = 0;
        qint64 encodedBytes = 0;
        const QString imageFile = displayObject.value(QStringLiteral("image_file")).toString();
        if (!displayObject.value(QStringLiteral("stable_id")).isString() ||
            !displayObject.value(QStringLiteral("display_name")).isString() ||
            !displayObject.value(QStringLiteral("image_file")).isString() ||
            !safeLocalFileName(imageFile) || expectedFiles.contains(imageFile) ||
            !jsonInteger(displayObject.value(QStringLiteral("width")), 1,
                         std::numeric_limits<int>::max(), &width) ||
            !jsonInteger(displayObject.value(QStringLiteral("height")), 1,
                         std::numeric_limits<int>::max(), &height) ||
            !jsonInteger(displayObject.value(QStringLiteral("encoded_bytes")), 1,
                         std::numeric_limits<qint64>::max(), &encodedBytes)) {
            if (error != nullptr) {
                *error = QStringLiteral("A record display descriptor is invalid");
            }
            return false;
        }
        const qint64 pixels = width * height;
        if (pixels <= 0 || pixels > kMaximumPixelsPerImage ||
            pixelsInRecord > kMaximumPixelsPerRecord - pixels) {
            if (error != nullptr) {
                *error = QStringLiteral("A record display exceeds the image limits");
            }
            return false;
        }
        pixelsInRecord += pixels;
        const QString imagePath = QDir(directoryPath).filePath(imageFile);
        const QFileInfo imageInfo(imagePath);
        if (!pathInsideRoot(directoryPath, imagePath) || !imageInfo.isFile() ||
            imageInfo.isSymLink() || imageInfo.size() != encodedBytes) {
            if (error != nullptr) {
                *error = QStringLiteral("A record display payload is invalid");
            }
            return false;
        }
        if (validatePayloads && !snow_shot::image_codec::inspectFile(
                                    imagePath, snow::image::Format::png,
                                    QSize(static_cast<int>(width), static_cast<int>(height)))) {
            if (error != nullptr) {
                *error = QStringLiteral("A record display payload is invalid");
            }
            return false;
        }
        expectedFiles.insert(imageFile);
        calculatedBytes += encodedBytes;
        result.displayFileNames.push_back(imageFile);
        result.record.displays.push_back(
            {displayObject.value(QStringLiteral("stable_id")).toString(),
             displayObject.value(QStringLiteral("display_name")).toString(),
             QSize(static_cast<int>(width), static_cast<int>(height)), encodedBytes});
    }

    const QFileInfoList actualEntries =
        QDir(directoryPath)
            .entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                           QDir::System);
    for (const QFileInfo& entry : actualEntries) {
        if (!entry.isFile() || entry.isSymLink() || !expectedFiles.contains(entry.fileName())) {
            if (error != nullptr) {
                *error = QStringLiteral("The record directory contains an unexpected payload");
            }
            return false;
        }
    }
    if (actualEntries.size() != expectedFiles.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("The record directory is incomplete");
        }
        return false;
    }
    calculatedBytes +=
        QFileInfo(QDir(directoryPath).filePath(QString::fromLatin1(kManifestFileName))).size();
    if (calculatedBytes != result.record.totalBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The record size metadata does not match its payloads");
        }
        return false;
    }
    result.payloadValidated = validatePayloads;
    *stored = std::move(result);
    return true;
}

bool validateStoredPayload(const StoredRecord& stored, const QString& directoryPath,
                           QString* error) {
    const QString canvasPath = QDir(directoryPath).filePath(stored.canvasFileName);
    QFile canvas(canvasPath);
    if (!canvas.open(QIODevice::ReadOnly) || !validCanvasFile(canvas.readAll())) {
        if (error != nullptr) {
            *error = QStringLiteral("The record canvas payload is malformed");
        }
        return false;
    }
    for (qsizetype index = 0; index < stored.displayFileNames.size(); ++index) {
        const QString imagePath = QDir(directoryPath).filePath(stored.displayFileNames[index]);
        if (!snow_shot::image_codec::inspectFile(
                imagePath, snow::image::Format::png, stored.record.displays[index].imageSize)) {
            if (error != nullptr) {
                *error = QStringLiteral("A record display payload is invalid");
            }
            return false;
        }
    }
    if (stored.resultFileName.has_value()) {
        const QString imagePath = QDir(directoryPath).filePath(*stored.resultFileName);
        if (!snow_shot::image_codec::inspectFile(
                imagePath, snow::image::Format::png, stored.record.result->imageSize)) {
            if (error != nullptr) {
                *error = QStringLiteral("The record result payload is invalid");
            }
            return false;
        }
    }
    return true;
}
} // namespace

class CaptureHistoryRepositoryImpl final : public CaptureHistoryRepository {
  public:
    CaptureHistoryRepositoryImpl(QString configurationDirectory,
                                 CaptureHistoryRepositoryOptions options)
        : m_configurationDirectory(QDir::cleanPath(std::move(configurationDirectory))),
          m_historyDirectory(
              QDir(m_configurationDirectory).filePath(QString::fromLatin1(kHistoryDirectoryName))),
          m_quarantineDirectory(QDir(m_configurationDirectory)
                                    .filePath(QString::fromLatin1(kQuarantineDirectoryName))),
          m_writeAvailable(options.writeAvailable),
          m_clock(options.clock ? std::move(options.clock)
                                : []() { return QDateTime::currentDateTimeUtc(); }),
          m_callbacks(std::move(options.callbacks)),
          m_maxQueuedPublications(std::max(0, options.maxQueuedPublications)),
          m_policy(options.policy) {
        if (!m_policy.isValid()) {
            m_policy = {};
        }
        reconcile();
        if (m_writeAvailable && m_policy.enabled) {
            static_cast<void>(pruneNow(QString(), false, nullptr));
        }
        refreshUsage(true);
        m_worker = std::thread([this]() { workerLoop(); });
    }

    ~CaptureHistoryRepositoryImpl() override {
        drain();
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stopping = true;
        }
        m_queueCondition.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    QVector<CaptureHistoryRecord> records() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        QVector<CaptureHistoryRecord> result;
        result.reserve(m_records.size());
        for (const StoredRecord& record : m_records) {
            result.push_back(record.record);
        }
        return result;
    }

    CaptureHistoryUsage usage() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_usage;
    }

    CaptureHistoryPolicy policy() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_policy;
    }

    std::shared_future<CaptureHistoryPublishResult> publish(CaptureHistoryDraft draft) override {
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (!m_policy.enabled) {
                return readyFuture(CaptureHistoryPublishResult{
                    StorageResult::failure(QStringLiteral("Capture history is disabled")), {}});
            }
        }
        auto promise = std::make_shared<std::promise<CaptureHistoryPublishResult>>();
        std::shared_future<CaptureHistoryPublishResult> future = promise->get_future().share();
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_stopping) {
                promise->set_value({StorageResult::failure(
                                        QStringLiteral("Capture-history storage is shutting down")),
                                    {}});
                return future;
            }
            const int pendingPublications = static_cast<int>(
                std::count_if(m_queue.cbegin(), m_queue.cend(), [](const Command& command) {
                    return command.kind == CommandKind::Publish;
                }));
            const int publicationsInFlight = pendingPublications + (m_activePublication ? 1 : 0);
            if (publicationsInFlight >= m_maxQueuedPublications + 1) {
                const QString error = QStringLiteral("The capture-history write queue is full");
                promise->set_value({StorageResult::failure(error), {}});
                setError(error);
                return future;
            }
            Command command;
            command.kind = CommandKind::Publish;
            command.draft = std::move(draft);
            command.publishPromise = promise;
            m_queue.push_back(std::move(command));
        }
        m_queueCondition.notify_one();
        return future;
    }

    std::optional<CaptureHistoryPayload> load(const CaptureHistoryRecord& record) const override {
        StoredRecord stored;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(record.id);
            if (index == m_recordIndex.cend() || *index >= m_records.size()) {
                return std::nullopt;
            }
            stored = m_records.at(*index);
        }
        const QString directoryPath = QDir(m_historyDirectory).filePath(stored.directoryName);
        if (!pathInsideRoot(m_historyDirectory, directoryPath)) {
            return std::nullopt;
        }
        if (!stored.payloadValidated) {
            QString validationError;
            if (!validateStoredPayload(stored, directoryPath, &validationError)) {
                const_cast<CaptureHistoryRepositoryImpl*>(this)->setError(validationError);
                const_cast<CaptureHistoryRepositoryImpl*>(this)->enqueueQuarantine(
                    record.id, validationError);
                return std::nullopt;
            }
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(record.id);
            if (index != m_recordIndex.cend() && *index < m_records.size()) {
                m_records[*index].payloadValidated = true;
            }
        }
        QFile canvas(QDir(directoryPath).filePath(stored.canvasFileName));
        CaptureHistoryPayload payload;
        if (!canvas.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        payload.canvasHistory = canvas.readAll();
        if (payload.canvasHistory.size() != stored.record.canvasBytes ||
            !validCanvasFile(payload.canvasHistory)) {
            return std::nullopt;
        }
        for (qsizetype index = 0; index < stored.displayFileNames.size(); ++index) {
            const QString imagePath = QDir(directoryPath).filePath(stored.displayFileNames[index]);
            if (!pathInsideRoot(directoryPath, imagePath)) {
                return std::nullopt;
            }
            const QImage image =
                snow_shot::image_codec::decodeFile(imagePath, snow::image::Format::png);
            if (image.isNull() ||
                image.size() != stored.record.displays[index].imageSize) {
                return std::nullopt;
            }
            payload.displayImages.push_back(image);
        }
        return payload;
    }

    std::optional<CaptureHistoryAssetSet>
    displayAssets(const CaptureHistoryRecord& record) const override {
        StoredRecord stored;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(record.id);
            if (index == m_recordIndex.cend() || *index >= m_records.size()) {
                return std::nullopt;
            }
            const StoredRecord& found = m_records.at(*index);
            if (!(found.record == record) ||
                found.displayFileNames.size() != found.record.displays.size() ||
                found.resultFileName.has_value() != found.record.result.has_value()) {
                return std::nullopt;
            }
            stored = found;
        }

        const QString directoryPath = QDir(m_historyDirectory).filePath(stored.directoryName);
        const QFileInfo directoryInfo(directoryPath);
        if (!pathInsideRoot(m_historyDirectory, directoryPath) || !directoryInfo.isDir() ||
            directoryInfo.isSymLink()) {
            return std::nullopt;
        }
        {
            QString validationError;
            if (!validateStoredPayload(stored, directoryPath, &validationError)) {
                const_cast<CaptureHistoryRepositoryImpl*>(this)->setError(validationError);
                const_cast<CaptureHistoryRepositoryImpl*>(this)->enqueueQuarantine(
                    record.id, validationError);
                return std::nullopt;
            }
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(record.id);
            if (index != m_recordIndex.cend() && *index < m_records.size()) {
                m_records[*index].payloadValidated = true;
            }
        }

        CaptureHistoryAssetSet result;
        result.recordId = stored.record.id;
        if (stored.resultFileName.has_value() && stored.record.result.has_value()) {
            const QString& fileName = *stored.resultFileName;
            if (!safeLocalFileName(fileName)) {
                return std::nullopt;
            }
            const QString imagePath = QDir(directoryPath).filePath(fileName);
            const QFileInfo imageInfo(imagePath);
            if (!pathInsideRoot(directoryPath, imagePath) || !imageInfo.isFile() ||
                imageInfo.isSymLink() || imageInfo.size() != stored.record.result->encodedBytes) {
                return std::nullopt;
            }
            result.result = CaptureHistoryResultAsset{
                stored.record.id, stored.record.result->imageSize,
                QUrl::fromLocalFile(imageInfo.absoluteFilePath())};
        }
        result.displays.reserve(stored.record.displays.size());
        for (qsizetype index = 0; index < stored.record.displays.size(); ++index) {
            const CaptureHistoryDisplayRecord& display = stored.record.displays[index];
            const QString& fileName = stored.displayFileNames[index];
            if (!safeLocalFileName(fileName)) {
                return std::nullopt;
            }

            const QString imagePath = QDir(directoryPath).filePath(fileName);
            const QFileInfo imageInfo(imagePath);
            if (!pathInsideRoot(directoryPath, imagePath) || !imageInfo.isFile() ||
                imageInfo.isSymLink() || imageInfo.size() != display.encodedBytes) {
                return std::nullopt;
            }

            result.displays.push_back({stored.record.id, display.stableId, display.name,
                                       display.imageSize,
                                       QUrl::fromLocalFile(imageInfo.absoluteFilePath())});
        }
        return result;
    }

    std::optional<QImage> loadResultImage(const CaptureHistoryRecord& record) const override {
        StoredRecord stored;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(record.id);
            if (index == m_recordIndex.cend() || *index >= m_records.size()) {
                return std::nullopt;
            }
            const StoredRecord& found = m_records.at(*index);
            if (!(found.record == record) || !found.resultFileName.has_value() ||
                !found.record.result.has_value()) {
                return std::nullopt;
            }
            stored = found;
        }
        const QString directoryPath = QDir(m_historyDirectory).filePath(stored.directoryName);
        const QString imagePath = QDir(directoryPath).filePath(*stored.resultFileName);
        if (!pathInsideRoot(m_historyDirectory, directoryPath) ||
            !pathInsideRoot(directoryPath, imagePath)) {
            return std::nullopt;
        }
        QString validationError;
        if (!stored.payloadValidated &&
            !validateStoredPayload(stored, directoryPath, &validationError)) {
            const_cast<CaptureHistoryRepositoryImpl*>(this)->setError(validationError);
            const_cast<CaptureHistoryRepositoryImpl*>(this)->enqueueQuarantine(
                record.id, validationError);
            return std::nullopt;
        }
        const QImage image =
            snow_shot::image_codec::decodeFile(imagePath, snow::image::Format::png);
        if (image.isNull() || image.size() != stored.record.result->imageSize) {
            return std::nullopt;
        }
        return image;
    }

    std::shared_future<StorageResult> remove(const QString& id) override {
        auto promise = std::make_shared<std::promise<StorageResult>>();
        std::shared_future<StorageResult> future = promise->get_future().share();
        Command command;
        command.kind = CommandKind::Remove;
        command.id = id;
        command.storagePromise = promise;
        if (!enqueue(std::move(command))) {
            promise->set_value(
                StorageResult::failure(QStringLiteral("Capture-history storage is shutting down")));
        }
        return future;
    }

    std::shared_future<StorageResult> updatePolicy(CaptureHistoryPolicy policy) override {
        if (!policy.isValid()) {
            return readyFuture(StorageResult::failure(
                QStringLiteral("The capture-history policy is outside the supported ranges")));
        }
        auto promise = std::make_shared<std::promise<StorageResult>>();
        std::shared_future<StorageResult> future = promise->get_future().share();
        Command command;
        command.kind = CommandKind::Policy;
        command.policy = policy;
        command.storagePromise = promise;
        if (!enqueue(std::move(command))) {
            promise->set_value(
                StorageResult::failure(QStringLiteral("Capture-history storage is shutting down")));
        }
        return future;
    }

    std::shared_future<StorageResult> requestPrune() override {
        auto promise = std::make_shared<std::promise<StorageResult>>();
        std::shared_future<StorageResult> future = promise->get_future().share();
        Command command;
        command.kind = CommandKind::Prune;
        command.storagePromise = promise;
        if (!enqueue(std::move(command))) {
            promise->set_value(
                StorageResult::failure(QStringLiteral("Capture-history storage is shutting down")));
        }
        return future;
    }

    std::shared_future<StorageResult> requestClear() override {
        auto promise = std::make_shared<std::promise<StorageResult>>();
        std::shared_future<StorageResult> future = promise->get_future().share();
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_stopping) {
                promise->set_value(StorageResult::failure(
                    QStringLiteral("Capture-history storage is shutting down")));
                return future;
            }
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (it->kind != CommandKind::Publish) {
                    ++it;
                    continue;
                }
                if (it->publishPromise != nullptr) {
                    it->publishPromise->set_value(
                        {StorageResult::failure(QStringLiteral(
                             "The capture-history publication was cancelled by clear")),
                         {}});
                }
                it = m_queue.erase(it);
            }
            Command command;
            command.kind = CommandKind::Clear;
            command.storagePromise = promise;
            m_queue.push_front(std::move(command));
        }
        m_queueCondition.notify_one();
        return future;
    }

    void drain() override {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_drainCondition.wait(lock, [this]() { return m_queue.empty() && !m_active; });
    }

    QString lastError() const override {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_lastError;
    }

  private:
    enum class CommandKind { Publish, Remove, Policy, Prune, Clear, Quarantine };

    struct Command {
        CommandKind kind = CommandKind::Prune;
        CaptureHistoryDraft draft;
        CaptureHistoryPolicy policy;
        QString id;
        QString reason;
        std::shared_ptr<std::promise<CaptureHistoryPublishResult>> publishPromise;
        std::shared_ptr<std::promise<StorageResult>> storagePromise;
    };

    bool enqueue(Command command) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_stopping) {
                return false;
            }
            m_queue.push_back(std::move(command));
        }
        m_queueCondition.notify_one();
        return true;
    }

    void enqueueQuarantine(const QString& id, const QString& reason) {
        Command command;
        command.kind = CommandKind::Quarantine;
        command.id = id;
        command.reason = reason;
        static_cast<void>(enqueue(std::move(command)));
    }

    void workerLoop() {
        for (;;) {
            Command command;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
                if (m_stopping && m_queue.empty()) {
                    return;
                }
                command = std::move(m_queue.front());
                m_queue.pop_front();
                m_active = true;
                m_activePublication = command.kind == CommandKind::Publish;
            }

            switch (command.kind) {
            case CommandKind::Publish:
                if (command.publishPromise != nullptr) {
                    command.publishPromise->set_value(publishNow(std::move(command.draft)));
                }
                break;
            case CommandKind::Remove:
                if (command.storagePromise != nullptr) {
                    command.storagePromise->set_value(removeNow(command.id));
                }
                break;
            case CommandKind::Policy:
                if (command.storagePromise != nullptr) {
                    const StorageResult result = updatePolicyNow(command.policy);
                    command.storagePromise->set_value(result);
                    if (m_callbacks.policyFinished) {
                        m_callbacks.policyFinished(result.success, result.error);
                    }
                }
                break;
            case CommandKind::Prune:
                if (command.storagePromise != nullptr) {
                    command.storagePromise->set_value(pruneNow(QString(), true, nullptr));
                }
                break;
            case CommandKind::Clear: {
                const StorageResult result = clearNow();
                if (command.storagePromise != nullptr) {
                    command.storagePromise->set_value(result);
                }
                if (m_callbacks.clearFinished) {
                    m_callbacks.clearFinished(result.success, result.error);
                }
                break;
            }
            case CommandKind::Quarantine:
                static_cast<void>(quarantineRecordNow(command.id, command.reason));
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_active = false;
                m_activePublication = false;
                if (m_queue.empty()) {
                    m_drainCondition.notify_all();
                }
            }
        }
    }

    CaptureHistoryPublishResult publishNow(CaptureHistoryDraft draft) {
        if (!m_writeAvailable) {
            return {fail(QStringLiteral("Capture-history storage is read-only")), {}};
        }
        CaptureHistoryPolicy currentPolicy;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            currentPolicy = m_policy;
        }
        if (!currentPolicy.enabled) {
            return {fail(QStringLiteral("Capture history is disabled")), {}};
        }
        EncodedDraft encoded;
        QString validationError;
        const qint64 quotaBytes = static_cast<qint64>(currentPolicy.maxDiskMiB) * kBytesPerMiB;
        if (!encodeDraft(draft, quotaBytes, &encoded, &validationError)) {
            qCWarning(storageLog) << validationError;
            return {fail(validationError), {}};
        }
        if (!QDir().mkpath(m_historyDirectory)) {
            return {fail(QStringLiteral("Unable to create the capture-history directory")), {}};
        }

        const QString finalName =
            encoded.record.createdUtc.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
            u'-' + encoded.record.id;
        const QString temporaryName =
            QStringLiteral(".tmp-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir historyRoot(m_historyDirectory);
        if (historyRoot.exists(finalName) || !historyRoot.mkdir(temporaryName)) {
            return {fail(QStringLiteral("Unable to create a temporary history record")), {}};
        }
        const QString temporaryPath = historyRoot.filePath(temporaryName);
        bool wrote = writeFile(QDir(temporaryPath).filePath(QString::fromLatin1(kCanvasFileName)),
                               encoded.canvas);
        if (wrote && encoded.resultImage.has_value() && encoded.resultImageFileName.has_value()) {
            wrote = writeFile(QDir(temporaryPath).filePath(*encoded.resultImageFileName),
                              *encoded.resultImage);
        }
        for (qsizetype index = 0; wrote && index < encoded.images.size(); ++index) {
            wrote = writeFile(QDir(temporaryPath).filePath(encoded.imageFileNames[index]),
                              encoded.images[index]);
        }
        if (wrote) {
            wrote = writeFile(QDir(temporaryPath).filePath(QString::fromLatin1(kManifestFileName)),
                              encoded.manifestBytes);
        }

        StoredRecord validation;
        QString error;
        if (wrote) {
            wrote = parseManifest(encoded.manifest, finalName, temporaryPath, true, &validation,
                                  &error);
        }
        if (!wrote || !historyRoot.rename(temporaryName, finalName)) {
            static_cast<void>(QDir(temporaryPath).removeRecursively());
            refreshUsage(true);
            return {fail(error.isEmpty()
                             ? QStringLiteral("Unable to publish the capture-history record")
                             : error),
                    {}};
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_records.push_back(validation);
            m_recordBytes += validation.record.totalBytes;
            sortRecordsLocked();
            rebuildRecordIndexLocked();
        }

        const StorageResult pruneResult = pruneNow(encoded.record.id, false, nullptr);
        if (!pruneResult.success) {
            qCWarning(storageLog) << "Capture-history pruning failed after publication:"
                                  << pruneResult.error;
        }
        notifyRecordsChanged();
        refreshUsage(false);
        if (pruneResult.success) {
            setError({});
        }
        return {StorageResult::ok(), encoded.record};
    }

    StorageResult updatePolicyNow(const CaptureHistoryPolicy& policy) {
        if (!m_writeAvailable) {
            return fail(QStringLiteral("Capture-history storage is read-only"));
        }
        if (!policy.isValid()) {
            return fail(QStringLiteral("The capture-history policy is invalid"));
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_policy = policy;
        }
        if (policy.enabled) {
            return pruneNow(QString(), true, nullptr);
        }
        setError({});
        return StorageResult::ok();
    }

    StorageResult pruneNow(const QString& protectedId, bool finalize, bool* changedOut) {
        CaptureHistoryPolicy currentPolicy;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            currentPolicy = m_policy;
        }
        if (!m_writeAvailable) {
            return fail(QStringLiteral("Capture-history storage is read-only"));
        }
        if (!currentPolicy.enabled) {
            return StorageResult::ok();
        }
        const QDateTime cutoff = m_clock().toUTC().addDays(-currentPolicy.retentionDays);
        bool changed = false;
        for (;;) {
            StoredRecord candidate;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                const qint64 bytes = m_recordBytes;
                const int count = m_records.size();
                for (auto it = m_records.crbegin(); it != m_records.crend(); ++it) {
                    if (it->record.id == protectedId) {
                        continue;
                    }
                    const bool expired = it->record.createdUtc < cutoff;
                    const bool countExceeded = count > currentPolicy.maxEntries;
                    const bool bytesExceeded =
                        bytes > static_cast<qint64>(currentPolicy.maxDiskMiB) * kBytesPerMiB;
                    if (expired || countExceeded || bytesExceeded) {
                        candidate = *it;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
            const QString path = QDir(m_historyDirectory).filePath(candidate.directoryName);
            if (!pathInsideRoot(m_historyDirectory, path) || !QDir(path).removeRecursively()) {
                return fail(QStringLiteral("Unable to prune a capture-history record"));
            }
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                const auto foundRecord = std::find_if(
                    m_records.begin(), m_records.end(), [&candidate](const StoredRecord& record) {
                        return record.record.id == candidate.record.id;
                    });
                if (foundRecord != m_records.end()) {
                    m_recordBytes -= foundRecord->record.totalBytes;
                    m_records.erase(foundRecord);
                    rebuildRecordIndexLocked();
                    changed = true;
                }
            }
        }
        if (changedOut != nullptr) {
            *changedOut = changed;
        }
        if (changed && finalize) {
            notifyRecordsChanged();
            refreshUsage(false);
        }
        setError({});
        return StorageResult::ok();
    }

    StorageResult removeNow(const QString& id) {
        StoredRecord record;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(id);
            if (index == m_recordIndex.cend() || *index >= m_records.size()) {
                return StorageResult::ok();
            }
            record = m_records.at(*index);
        }
        const QString path = QDir(m_historyDirectory).filePath(record.directoryName);
        if (!m_writeAvailable || !pathInsideRoot(m_historyDirectory, path) ||
            !QDir(path).removeRecursively()) {
            return fail(QStringLiteral("Unable to remove the capture-history record"));
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(id);
            if (index != m_recordIndex.cend() && *index < m_records.size()) {
                m_recordBytes -= m_records.at(*index).record.totalBytes;
                m_records.removeAt(*index);
                rebuildRecordIndexLocked();
            }
        }
        notifyRecordsChanged();
        refreshUsage(false);
        setError({});
        return StorageResult::ok();
    }

    StorageResult clearNow() {
        if (!m_writeAvailable) {
            return fail(QStringLiteral("Capture-history storage is read-only"));
        }
        bool success = true;
        for (const QString& directory : {m_historyDirectory, m_quarantineDirectory}) {
            if (QDir(directory).exists() && !QDir(directory).removeRecursively()) {
                success = false;
            }
        }
        if (!success) {
            refreshUsage(true);
            return fail(QStringLiteral("Unable to clear all managed capture-history data"));
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_records.clear();
            m_recordIndex.clear();
            m_recordBytes = 0;
            m_quarantineBytes = 0;
            m_temporaryBytes = 0;
        }
        notifyRecordsChanged();
        refreshUsage(false);
        setError({});
        return StorageResult::ok();
    }

    StorageResult quarantineRecordNow(const QString& id, const QString& reason) {
        StoredRecord record;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(id);
            if (index == m_recordIndex.cend() || *index >= m_records.size()) {
                return StorageResult::ok();
            }
            record = m_records.at(*index);
        }
        if (!m_writeAvailable) {
            return fail(QStringLiteral("An invalid history record could not be quarantined"));
        }
        const QString path = QDir(m_historyDirectory).filePath(record.directoryName);
        if (!pathInsideRoot(m_historyDirectory, path) || !QDir(path).exists()) {
            return fail(QStringLiteral("An invalid history record could not be quarantined"));
        }
        quarantine(QFileInfo(path), reason);
        if (QDir(path).exists()) {
            return fail(QStringLiteral("An invalid history record could not be quarantined"));
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto index = m_recordIndex.constFind(id);
            if (index != m_recordIndex.cend() && *index < m_records.size()) {
                m_recordBytes -= m_records.at(*index).record.totalBytes;
                m_records.removeAt(*index);
                rebuildRecordIndexLocked();
            }
        }
        notifyRecordsChanged();
        refreshUsage(false);
        return StorageResult::ok();
    }

    void reconcile() {
        if (m_configurationDirectory.isEmpty()) {
            setError(QStringLiteral("Capture-history storage has no directory"));
            return;
        }
        QDir historyRoot(m_historyDirectory);
        if (historyRoot.exists()) {
            const QFileInfoList directories = historyRoot.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& directory : directories) {
                if (!directory.fileName().startsWith(QStringLiteral(".tmp-"))) {
                    continue;
                }
                if (!QDir(directory.absoluteFilePath()).removeRecursively()) {
                    qCWarning(storageLog) << "Unable to remove temporary history directory"
                                          << directory.absoluteFilePath();
                }
            }
        }

        QVector<StoredRecord> valid;
        QSet<QString> ids;
        if (historyRoot.exists()) {
            const QFileInfoList directories =
                historyRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo& directory : directories) {
                if (directory.fileName().startsWith(QStringLiteral(".tmp-"))) {
                    continue;
                }
                const QString manifestPath = QDir(directory.absoluteFilePath())
                                                 .filePath(QString::fromLatin1(kManifestFileName));
                QJsonObject manifest;
                QFile manifestFile(manifestPath);
                QJsonParseError parseError;
                if (manifestFile.open(QIODevice::ReadOnly)) {
                    const QJsonDocument document =
                        QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
                    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                        manifest = document.object();
                    }
                }
                manifestFile.close();
                StoredRecord record;
                QString error;
                if (manifest.isEmpty() ||
                    !parseManifest(manifest, directory.fileName(), directory.absoluteFilePath(),
                                   false, &record, &error) ||
                    ids.contains(record.record.id)) {
                    quarantine(directory, error.isEmpty()
                                              ? QStringLiteral("The history record is invalid")
                                              : error);
                    continue;
                }
                ids.insert(record.record.id);
                valid.push_back(std::move(record));
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_records = std::move(valid);
            m_recordBytes = 0;
            for (const StoredRecord& record : m_records) {
                m_recordBytes += record.record.totalBytes;
            }
            sortRecordsLocked();
            rebuildRecordIndexLocked();
        }
        cleanupQuarantine();
    }

    void quarantine(const QFileInfo& directory, const QString& reason) {
        qCWarning(storageLog) << "Quarantining capture-history record" << directory.fileName()
                              << reason;
        if (!m_writeAvailable || !QDir().mkpath(m_quarantineDirectory)) {
            setError(QStringLiteral("An invalid history record could not be quarantined"));
            return;
        }
        const QString suffix = m_clock().toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        QString targetName = directory.fileName() + QStringLiteral(".quarantine-") + suffix;
        QString target = QDir(m_quarantineDirectory).filePath(targetName);
        for (int duplicate = 1; QFileInfo::exists(target); ++duplicate) {
            targetName = directory.fileName() + QStringLiteral(".quarantine-") + suffix +
                         QStringLiteral("-%1").arg(duplicate);
            target = QDir(m_quarantineDirectory).filePath(targetName);
        }
        QDir sourceParent = directory.dir();
        if (!sourceParent.rename(directory.fileName(), target)) {
            setError(QStringLiteral("An invalid history record could not be quarantined"));
        }
    }

    void cleanupQuarantine() {
        if (!QDir(m_quarantineDirectory).exists()) {
            return;
        }
        const QDateTime cutoff = m_clock().toUTC().addDays(-30);
        static const QRegularExpression timestampPattern(
            QStringLiteral("\\.quarantine-(\\d{8}-\\d{6}-\\d{3})(?:-\\d+)?$"));
        const QFileInfoList entries =
            QDir(m_quarantineDirectory)
                .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries) {
            QDateTime created = entry.lastModified().toUTC();
            const QRegularExpressionMatch match = timestampPattern.match(entry.fileName());
            if (match.hasMatch()) {
                const QDateTime parsed =
                    QDateTime::fromString(match.captured(1), QStringLiteral("yyyyMMdd-HHmmss-zzz"));
                if (parsed.isValid()) {
                    created = QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC);
                }
            }
            if (created >= cutoff || QDir(entry.absoluteFilePath()).removeRecursively()) {
                continue;
            }
            qCWarning(storageLog) << "Unable to remove expired capture-history quarantine"
                                  << entry.absoluteFilePath();
        }
    }

    void sortRecordsLocked() {
        std::sort(m_records.begin(), m_records.end(),
                  [](const StoredRecord& first, const StoredRecord& second) {
                      return first.record.createdUtc > second.record.createdUtc;
                  });
    }

    void rebuildRecordIndexLocked() {
        m_recordIndex.clear();
        for (qsizetype index = 0; index < m_records.size(); ++index) {
            m_recordIndex.insert(m_records.at(index).record.id, index);
        }
    }

    void refreshUsage(bool scanExternal) {
        CaptureHistoryUsage usage;
        qint64 quarantineBytes = 0;
        qint64 temporaryBytes = 0;
        if (scanExternal) {
            quarantineBytes = directoryBytes(m_quarantineDirectory);
            if (QDir(m_historyDirectory).exists()) {
                const QFileInfoList temporary =
                    QDir(m_historyDirectory)
                        .entryInfoList({QStringLiteral(".tmp-*")},
                                       QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo& entry : temporary) {
                    temporaryBytes += directoryBytes(entry.absoluteFilePath());
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            usage.entryCount = m_records.size();
            usage.recordBytes = m_recordBytes;
            if (scanExternal) {
                m_quarantineBytes = quarantineBytes;
                m_temporaryBytes = temporaryBytes;
            }
            usage.quarantineBytes = m_quarantineBytes;
            usage.temporaryBytes = m_temporaryBytes;
        }
        usage.totalBytes = usage.recordBytes + usage.quarantineBytes + usage.temporaryBytes;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            changed = !(usage == m_usage);
            m_usage = usage;
        }
        if (changed && m_callbacks.usageChanged) {
            m_callbacks.usageChanged(usage);
        }
    }

    void notifyRecordsChanged() {
        if (m_callbacks.recordsChanged) {
            m_callbacks.recordsChanged();
        }
    }

    StorageResult fail(const QString& error) {
        setError(error);
        return StorageResult::failure(error);
    }

    void setError(const QString& error) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            changed = m_lastError != error;
            m_lastError = error;
        }
        if (changed && m_callbacks.errorChanged) {
            m_callbacks.errorChanged(error);
        }
    }

    QString m_configurationDirectory;
    QString m_historyDirectory;
    QString m_quarantineDirectory;
    bool m_writeAvailable = false;
    std::function<QDateTime()> m_clock;
    CaptureHistoryRepositoryCallbacks m_callbacks;
    int m_maxQueuedPublications = 2;

    mutable std::mutex m_stateMutex;
    mutable QVector<StoredRecord> m_records;
    QHash<QString, qsizetype> m_recordIndex;
    CaptureHistoryPolicy m_policy;
    CaptureHistoryUsage m_usage;
    qint64 m_recordBytes = 0;
    qint64 m_quarantineBytes = 0;
    qint64 m_temporaryBytes = 0;
    QString m_lastError;

    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::condition_variable m_drainCondition;
    std::deque<Command> m_queue;
    std::thread m_worker;
    bool m_active = false;
    bool m_activePublication = false;
    bool m_stopping = false;
};

std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory,
                             CaptureHistoryRepositoryOptions options) {
    return std::make_unique<CaptureHistoryRepositoryImpl>(std::move(configurationDirectory),
                                                          std::move(options));
}

std::unique_ptr<CaptureHistoryRepository>
makeCaptureHistoryRepository(QString configurationDirectory, bool writeAvailable,
                             std::function<QDateTime()> clock) {
    CaptureHistoryRepositoryOptions options;
    options.writeAvailable = writeAvailable;
    options.clock = std::move(clock);
    return makeCaptureHistoryRepository(std::move(configurationDirectory), std::move(options));
}
} // namespace snow_shot::storage
