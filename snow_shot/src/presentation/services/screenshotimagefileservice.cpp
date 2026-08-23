#include "snow_shot/presentation/screenshotimagefileservice.h"

#include "snowimageqtcodec.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {
QString filterForFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        return QCoreApplication::translate("ScreenshotImageFileService", "PNG image (*.png)");
    case ScreenshotImageFileFormat::Jpeg:
        return QCoreApplication::translate("ScreenshotImageFileService",
                                           "JPEG image (*.jpg *.jpeg)");
    case ScreenshotImageFileFormat::Webp:
        return QCoreApplication::translate("ScreenshotImageFileService", "WebP image (*.webp)");
    case ScreenshotImageFileFormat::Jxl:
        return QCoreApplication::translate("ScreenshotImageFileService", "JPEG XL image (*.jxl)");
    case ScreenshotImageFileFormat::Avif:
        return QCoreApplication::translate("ScreenshotImageFileService", "AVIF image (*.avif)");
    }
    return {};
}

QString collisionSafePath(const QString& directory, const QString& baseName,
                          const QString& extension) {
    const QDir target(directory);
    QString candidate = target.filePath(QStringLiteral("%1.%2").arg(baseName, extension));
    for (int suffix = 1; QFileInfo::exists(candidate); ++suffix) {
        candidate =
            target.filePath(QStringLiteral("%1_%2.%3").arg(baseName).arg(suffix).arg(extension));
    }
    return candidate;
}

template <typename Encoder>
ScreenshotImageFileSaveResult writeAtomically(const QString& outputPath, Encoder&& encoder) {
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return {{}, file.errorString()};
    }
    QString encodeError;
    if (!encoder(&file, &encodeError)) {
        file.cancelWriting();
        return {{},
                encodeError.isEmpty() ? QStringLiteral("The image could not be encoded")
                                      : encodeError};
    }
    if (!file.commit()) {
        return {{}, file.errorString()};
    }
    return {outputPath, {}};
}
} // namespace

QString ScreenshotImageFileService::suggestedBaseName(const QDateTime& timestamp) {
    return suggestedBaseName(QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), timestamp);
}

QString ScreenshotImageFileService::suggestedBaseName(const QString& filenameFormat,
                                                      const QDateTime& timestamp) {
    QString result = filenameFormat.trimmed();
    static const QRegularExpression placeholder(QStringLiteral("\\{([^{}]+)\\}"));
    QList<QRegularExpressionMatch> matches;
    auto iterator = placeholder.globalMatch(result);
    while (iterator.hasNext()) {
        matches.push_back(iterator.next());
    }
    for (auto match = matches.crbegin(); match != matches.crend(); ++match) {
        QString dateTimeFormat = match->captured(1);
        dateTimeFormat.replace(QStringLiteral("YYYY"), QStringLiteral("yyyy"));
        dateTimeFormat.replace(QStringLiteral("YY"), QStringLiteral("yy"));
        dateTimeFormat.replace(QStringLiteral("DD"), QStringLiteral("dd"));
        result.replace(match->capturedStart(), match->capturedLength(),
                       timestamp.toString(dateTimeFormat));
    }
    return result;
}

QString ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat format) {
    return filterForFormat(format);
}

QString ScreenshotImageFileService::saveDialogFilter() {
    return QStringList{filterForFormat(ScreenshotImageFileFormat::Png),
                       filterForFormat(ScreenshotImageFileFormat::Jpeg),
                       filterForFormat(ScreenshotImageFileFormat::Webp),
                       filterForFormat(ScreenshotImageFileFormat::Jxl),
                       filterForFormat(ScreenshotImageFileFormat::Avif)}
        .join(QStringLiteral(";;"));
}

QString ScreenshotImageFileService::automaticDirectory() {
    const QStringList directories = automaticDirectories();
    return directories.isEmpty() ? QString() : directories.constFirst();
}

QStringList ScreenshotImageFileService::automaticDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::PicturesLocation, QStandardPaths::DocumentsLocation}) {
        const QString root = QStandardPaths::writableLocation(location);
        if (root.isEmpty()) {
            continue;
        }
        const QString directory = QDir(root).filePath(QStringLiteral("SnowShot"));
        if (!directories.contains(directory, Qt::CaseInsensitive)) {
            directories.push_back(directory);
        }
    }
    return directories;
}

QStringList ScreenshotImageFileService::automaticDirectories(const QString& configuredDirectory) {
    QStringList directories;
    const QString configured = QDir::cleanPath(configuredDirectory.trimmed());
    if (!configured.isEmpty() && QFileInfo(configured).isDir()) {
        directories.push_back(configured);
    }
    for (const QString& fallback : automaticDirectories()) {
        if (!directories.contains(fallback, Qt::CaseInsensitive)) {
            directories.push_back(fallback);
        }
    }
    return directories;
}

QString ScreenshotImageFileService::saveDialogDirectory(const QString& lastDirectory,
                                                        const QString& configuredDirectory) {
    const QString remembered = lastDirectory.trimmed();
    if (!remembered.isEmpty()) {
        const QString cleaned = QDir::cleanPath(remembered);
        if (QFileInfo(cleaned).isDir()) {
            return cleaned;
        }
    }

    const QStringList candidates = automaticDirectories(configuredDirectory);
    if (!candidates.isEmpty()) {
        return candidates.constFirst();
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString ScreenshotImageFileService::extension(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        return QStringLiteral("png");
    case ScreenshotImageFileFormat::Jpeg:
        return QStringLiteral("jpg");
    case ScreenshotImageFileFormat::Webp:
        return QStringLiteral("webp");
    case ScreenshotImageFileFormat::Jxl:
        return QStringLiteral("jxl");
    case ScreenshotImageFileFormat::Avif:
        return QStringLiteral("avif");
    }
    return {};
}

ScreenshotImageFileFormat ScreenshotImageFileService::formatForKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("jpeg") || normalized == QStringLiteral("jpg")) {
        return ScreenshotImageFileFormat::Jpeg;
    }
    if (normalized == QStringLiteral("webp")) {
        return ScreenshotImageFileFormat::Webp;
    }
    if (normalized == QStringLiteral("jxl")) {
        return ScreenshotImageFileFormat::Jxl;
    }
    if (normalized == QStringLiteral("avif")) {
        return ScreenshotImageFileFormat::Avif;
    }
    return ScreenshotImageFileFormat::Png;
}

QString ScreenshotImageFileService::normalizedPath(QString path, ScreenshotImageFileFormat format) {
    path = QDir::cleanPath(path.trimmed());
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo information(path);
    const QString fileName = information.fileName();
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    const bool hasExplicitSuffix = dot >= 0 && dot + 1 < fileName.size();
    // The format passed to write() describes the bytes being emitted. Always
    // make the filename agree with it; callers that need suffix inference do
    // that once, through formatForDialogSelection(), before writing.
    if (hasExplicitSuffix && dot > 0) {
        path.chop(fileName.size() - dot);
    } else if (path.endsWith(QLatin1Char('.'))) {
        path.chop(1);
    }
    return path + QStringLiteral(".") + extension(format);
}

std::optional<ScreenshotImageFileFormat>
ScreenshotImageFileService::formatForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("png")) {
        return ScreenshotImageFileFormat::Png;
    }
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) {
        return ScreenshotImageFileFormat::Jpeg;
    }
    if (suffix == QStringLiteral("webp")) {
        return ScreenshotImageFileFormat::Webp;
    }
    if (suffix == QStringLiteral("jxl")) {
        return ScreenshotImageFileFormat::Jxl;
    }
    if (suffix == QStringLiteral("avif")) {
        return ScreenshotImageFileFormat::Avif;
    }
    return std::nullopt;
}

ScreenshotImageFileFormat
ScreenshotImageFileService::formatForDialogSelection(const QString& path,
                                                     const QString& selectedFilter) {
    if (const auto fromPath = formatForPath(path); fromPath.has_value()) {
        return *fromPath;
    }
    for (ScreenshotImageFileFormat format :
         {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Jpeg,
          ScreenshotImageFileFormat::Webp, ScreenshotImageFileFormat::Jxl,
          ScreenshotImageFileFormat::Avif}) {
        if (selectedFilter == filterForFormat(format)) {
            return format;
        }
    }
    return ScreenshotImageFileFormat::Png;
}

snow::image::Format ScreenshotImageFileService::snowImageFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        return snow::image::Format::png;
    case ScreenshotImageFileFormat::Jpeg:
        return snow::image::Format::jpeg;
    case ScreenshotImageFileFormat::Webp:
        return snow::image::Format::webp;
    case ScreenshotImageFileFormat::Jxl:
        return snow::image::Format::jxl;
    case ScreenshotImageFileFormat::Avif:
        return snow::image::Format::avif;
    }
    return snow::image::Format::unknown;
}

snow::image::EncodeOptions
ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat format) {
    snow::image::EncodeOptions options;
    options.format = snowImageFormat(format);
    options.preserve_metadata = false;
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        options.compression_level = 0;
        break;
    case ScreenshotImageFileFormat::Jpeg:
        options.quality = 100;
        break;
    case ScreenshotImageFileFormat::Webp:
        options.lossless = true;
        options.lossless_effort = 0;
        break;
    case ScreenshotImageFileFormat::Jxl:
    case ScreenshotImageFileFormat::Avif:
        options.lossless = true;
        options.effort = 1;
        break;
    }
    return options;
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::write(const QImage& image,
                                                                const QString& path,
                                                                ScreenshotImageFileFormat format) {
    if (image.isNull()) {
        return {{}, QStringLiteral("The screenshot image is empty")};
    }
    const QString outputPath = normalizedPath(path, format);
    if (outputPath.isEmpty()) {
        return {{}, QStringLiteral("No output file was selected")};
    }

    return writeAtomically(outputPath, [image, format](QIODevice* device, QString* error) {
        return snow_shot::image_codec::encodeToDevice(image, device, snowImageFormat(format),
                                                      encodeOptions(format), error);
    });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::write(const ScreenshotImageRowSource& source, const QString& path,
                                  ScreenshotImageFileFormat format) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot image source is empty")};
    }
    const QString outputPath = normalizedPath(path, format);
    if (outputPath.isEmpty()) {
        return {{}, QStringLiteral("No output file was selected")};
    }
    return writeAtomically(outputPath, [&source, format](QIODevice* device, QString* error) {
        return snow_shot::image_codec::encodeToDevice(source, device, snowImageFormat(format),
                                                      encodeOptions(format), error);
    });
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(const QImage& image) {
    return saveAutomatically(image, automaticDirectories(), QDateTime::currentDateTime());
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const QImage& image, const QStringList& candidateDirectories, const QDateTime& timestamp) {
    return saveAutomatically(image, candidateDirectories, ScreenshotImageFileFormat::Png,
                             QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), timestamp);
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const QImage& image, const QStringList& candidateDirectories, ScreenshotImageFileFormat format,
    const QString& filenameFormat, const QDateTime& timestamp) {
    if (image.isNull()) {
        return {{}, QStringLiteral("The screenshot image is empty")};
    }

    const QString baseName = suggestedBaseName(filenameFormat, timestamp);
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\'))) {
        return {{}, QStringLiteral("The screenshot filename format is invalid")};
    }

    QString lastError = QStringLiteral("No automatic screenshot folder is available");
    for (const QString& candidate : candidateDirectories) {
        const QString trimmedCandidate = candidate.trimmed();
        if (trimmedCandidate.isEmpty()) {
            continue;
        }
        const QString directory = QDir::cleanPath(trimmedCandidate);
        if (!QDir().mkpath(directory)) {
            lastError =
                QStringLiteral("The screenshot folder could not be created: %1").arg(directory);
            continue;
        }

        const QString path = collisionSafePath(directory, baseName, extension(format));
        const ScreenshotImageFileSaveResult result = write(image, path, format);
        if (result.succeeded()) {
            return result;
        }
        lastError = result.error;
    }
    return {{}, lastError};
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const ScreenshotImageRowSource& source, const QStringList& candidateDirectories,
    ScreenshotImageFileFormat format, const QString& filenameFormat, const QDateTime& timestamp) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot image source is empty")};
    }

    const QString baseName = suggestedBaseName(filenameFormat, timestamp);
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\'))) {
        return {{}, QStringLiteral("The screenshot filename format is invalid")};
    }

    QString lastError = QStringLiteral("No automatic screenshot folder is available");
    for (const QString& candidate : candidateDirectories) {
        const QString trimmedCandidate = candidate.trimmed();
        if (trimmedCandidate.isEmpty()) {
            continue;
        }
        const QString directory = QDir::cleanPath(trimmedCandidate);
        if (!QDir().mkpath(directory)) {
            lastError =
                QStringLiteral("The screenshot folder could not be created: %1").arg(directory);
            continue;
        }

        const QString path = collisionSafePath(directory, baseName, extension(format));
        const ScreenshotImageFileSaveResult result = write(source, path, format);
        if (result.succeeded()) {
            return result;
        }
        lastError = result.error;
        if (source.cancellationRequested && source.cancellationRequested()) {
            break;
        }
    }
    return {{}, lastError};
}

bool ScreenshotImageFileService::publishFileToClipboard(QClipboard* clipboard,
                                                        const QString& path) {
    if (clipboard == nullptr || path.isEmpty() || !QFileInfo::exists(path)) {
        return false;
    }
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())});
    clipboard->setMimeData(mimeData, QClipboard::Clipboard);
    return true;
}
