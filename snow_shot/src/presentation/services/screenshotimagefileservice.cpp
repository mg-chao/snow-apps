#include "snow_shot/presentation/screenshotimagefileservice.h"

#include "snowimageqtcodec.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QMimeData>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace {
QString filterForFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Pdf:
        return QCoreApplication::translate("ScreenshotImageFileService", "PDF document (*.pdf)");
    case ScreenshotImageFileFormat::Png:
        return QCoreApplication::translate("ScreenshotImageFileService", "PNG image (*.png)");
    case ScreenshotImageFileFormat::Jpeg:
        return QCoreApplication::translate("ScreenshotImageFileService",
                                           "JPEG image (*.jpg *.jpeg)");
    case ScreenshotImageFileFormat::Bmp:
        return QCoreApplication::translate("ScreenshotImageFileService", "BMP image (*.bmp)");
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

int compressionValue(const snow::image::EncoderOptionRange& range,
                     ScreenshotCompressionLevel level) {
    switch (level) {
    case ScreenshotCompressionLevel::Low:
        return range.minimum;
    case ScreenshotCompressionLevel::Medium:
        return range.default_value;
    case ScreenshotCompressionLevel::High:
        return range.maximum;
    }
    return range.minimum;
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
                       filterForFormat(ScreenshotImageFileFormat::Bmp),
                       filterForFormat(ScreenshotImageFileFormat::Webp),
                       filterForFormat(ScreenshotImageFileFormat::Jxl),
                       filterForFormat(ScreenshotImageFileFormat::Avif),
                       filterForFormat(ScreenshotImageFileFormat::Pdf)}
        .join(QStringLiteral(";;"));
}

QStringList ScreenshotImageFileService::automaticDirectories() {
    QStringList directories;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::PicturesLocation, QStandardPaths::DocumentsLocation}) {
        const QString directory = QStandardPaths::writableLocation(location);
        if (directory.isEmpty()) {
            continue;
        }
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
    case ScreenshotImageFileFormat::Pdf:
        return QStringLiteral("pdf");
    case ScreenshotImageFileFormat::Png:
        return QStringLiteral("png");
    case ScreenshotImageFileFormat::Jpeg:
        return QStringLiteral("jpg");
    case ScreenshotImageFileFormat::Bmp:
        return QStringLiteral("bmp");
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
    if (normalized == QStringLiteral("pdf"))
        return ScreenshotImageFileFormat::Pdf;
    if (normalized == QStringLiteral("jpeg") || normalized == QStringLiteral("jpg")) {
        return ScreenshotImageFileFormat::Jpeg;
    }
    if (normalized == QStringLiteral("bmp")) {
        return ScreenshotImageFileFormat::Bmp;
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

QString ScreenshotImageFileService::formatKey(ScreenshotImageFileFormat format) {
    return format == ScreenshotImageFileFormat::Jpeg ? QStringLiteral("jpeg") : extension(format);
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
    if (suffix == QStringLiteral("pdf"))
        return ScreenshotImageFileFormat::Pdf;
    if (suffix == QStringLiteral("png")) {
        return ScreenshotImageFileFormat::Png;
    }
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) {
        return ScreenshotImageFileFormat::Jpeg;
    }
    if (suffix == QStringLiteral("bmp")) {
        return ScreenshotImageFileFormat::Bmp;
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
          ScreenshotImageFileFormat::Bmp, ScreenshotImageFileFormat::Webp,
          ScreenshotImageFileFormat::Jxl, ScreenshotImageFileFormat::Avif,
          ScreenshotImageFileFormat::Pdf}) {
        if (selectedFilter == filterForFormat(format)) {
            return format;
        }
    }
    return ScreenshotImageFileFormat::Png;
}

snow::image::Format ScreenshotImageFileService::snowImageFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Pdf:
        return snow::image::Format::unknown;
    case ScreenshotImageFileFormat::Png:
        return snow::image::Format::png;
    case ScreenshotImageFileFormat::Jpeg:
        return snow::image::Format::jpeg;
    case ScreenshotImageFileFormat::Bmp:
        return snow::image::Format::bmp;
    case ScreenshotImageFileFormat::Webp:
        return snow::image::Format::webp;
    case ScreenshotImageFileFormat::Jxl:
        return snow::image::Format::jxl;
    case ScreenshotImageFileFormat::Avif:
        return snow::image::Format::avif;
    }
    return snow::image::Format::unknown;
}

bool ScreenshotImageFileService::supportsQuality(ScreenshotImageFileFormat format) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return true;
    const auto encoder = snow_shot::image_codec::encoderInfo(snowImageFormat(format));
    return encoder.has_value() &&
           snow::image::has_feature(encoder->features, snow::image::EncoderFeature::quality);
}

bool ScreenshotImageFileService::supportsCompressionLevel(ScreenshotImageFileFormat format) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return false;
    const auto encoder = snow_shot::image_codec::encoderInfo(snowImageFormat(format));
    if (!encoder.has_value())
        return false;
    return snow::image::has_feature(encoder->features,
                                    snow::image::EncoderFeature::compression_level) ||
           snow::image::has_feature(encoder->features, snow::image::EncoderFeature::effort) ||
           (snow::image::has_feature(encoder->features, snow::image::EncoderFeature::lossless) &&
            encoder->lossless_effort.maximum > encoder->lossless_effort.minimum);
}

QString ScreenshotImageFileService::compressionLevelKey(ScreenshotCompressionLevel level) {
    switch (level) {
    case ScreenshotCompressionLevel::Low:
        return QStringLiteral("low");
    case ScreenshotCompressionLevel::Medium:
        return QStringLiteral("medium");
    case ScreenshotCompressionLevel::High:
        return QStringLiteral("high");
    }
    return QStringLiteral("low");
}

ScreenshotCompressionLevel ScreenshotImageFileService::compressionLevelForKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("medium"))
        return ScreenshotCompressionLevel::Medium;
    if (normalized == QStringLiteral("high"))
        return ScreenshotCompressionLevel::High;
    return ScreenshotCompressionLevel::Low;
}

snow::image::EncodeOptions
ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat format,
                                          ScreenshotImageEncodingOptions encoding) {
    snow::image::EncodeOptions options;
    options.format = snowImageFormat(format);
    options.preserve_metadata = false;
    options.quality = qBound(0, encoding.quality, 100);
    const auto encoder = snow_shot::image_codec::encoderInfo(options.format);
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        if (encoder.has_value())
            options.compression_level =
                compressionValue(encoder->compression_level, encoding.compressionLevel);
        break;
    case ScreenshotImageFileFormat::Pdf:
    case ScreenshotImageFileFormat::Jpeg:
    case ScreenshotImageFileFormat::Bmp:
        break;
    case ScreenshotImageFileFormat::Webp:
        options.lossless = options.quality == 100;
        if (encoder.has_value()) {
            if (options.lossless) {
                options.lossless_effort =
                    compressionValue(encoder->lossless_effort, encoding.compressionLevel);
            } else {
                options.effort = compressionValue(encoder->effort, encoding.compressionLevel);
            }
        }
        break;
    case ScreenshotImageFileFormat::Jxl:
    case ScreenshotImageFileFormat::Avif:
        options.lossless = options.quality == 100;
        if (encoder.has_value())
            options.effort = compressionValue(encoder->effort, encoding.compressionLevel);
        break;
    }
    return options;
}

snow::image::EncodeOptions
ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat format, int quality) {
    return encodeOptions(format, ScreenshotImageEncodingOptions{quality});
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::writeEncodedFile(const QString& encodedFile, const QString& path,
                                             ScreenshotImageFileFormat format,
                                             std::function<bool()> cancelled) {
    const QString outputPath = normalizedPath(path, format);
    if (outputPath.isEmpty())
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "No output file was selected")};
    const QString cancelledMessage =
        QCoreApplication::translate("ScreenshotImageFileService", "The save was cancelled");
    if (cancelled && cancelled())
        return {{}, cancelledMessage};
    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "The output directory could not be created")};
    return writeAtomically(outputPath, [&](QIODevice* device, QString* error) {
        QFile input(encodedFile);
        if (!input.open(QIODevice::ReadOnly)) {
            *error = input.errorString();
            return false;
        }
        if (input.size() == 0) {
            *error = QCoreApplication::translate("ScreenshotImageFileService",
                                                 "The encoded image is empty");
            return false;
        }
        while (!input.atEnd()) {
            if (cancelled && cancelled()) {
                *error = cancelledMessage;
                return false;
            }
            const QByteArray bytes = input.read(1024 * 1024);
            if (bytes.isEmpty() || device->write(bytes) != bytes.size()) {
                *error = input.error() != QFileDevice::NoError ? input.errorString()
                                                               : device->errorString();
                return false;
            }
        }
        if (cancelled && cancelled()) {
            *error = cancelledMessage;
            return false;
        }
        return true;
    });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::writePdf(const screenshot_pdf::Payload& payload, const QString& path,
                                     ScreenshotPdfOptions options,
                                     std::function<bool()> cancelled) {
    const QString outputPath = normalizedPath(path, ScreenshotImageFileFormat::Pdf);
    if (outputPath.isEmpty())
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "No output file was selected")};
    if (cancelled && cancelled())
        return {
            {},
            QCoreApplication::translate("ScreenshotImageFileService", "The save was cancelled")};
    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "The output directory could not be created")};
    options.title = QFileInfo(outputPath).completeBaseName();
    if (!options.creationTime.isValid())
        options.creationTime = QDateTime::currentDateTimeUtc();
    return writeAtomically(outputPath, [&](QIODevice* device, QString* error) {
        return screenshot_pdf::write(payload, device, options, error, cancelled);
    });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::write(const QImage& image, const QString& path,
                                  ScreenshotImageFileFormat format, ScreenshotPdfOptions pdf,
                                  std::function<bool()> cancelled,
                                  ScreenshotImageEncodingOptions encoding) {
    if (format == ScreenshotImageFileFormat::Pdf || cancelled) {
        auto source = snow_shot::image_codec::srgbRowSource(image);
        source.cancellationRequested = std::move(cancelled);
        return write(source, path, format, std::move(pdf), encoding);
    }
    if (image.isNull()) {
        return {{}, QStringLiteral("The screenshot image is empty")};
    }
    const QString outputPath = normalizedPath(path, format);
    if (outputPath.isEmpty()) {
        return {{}, QStringLiteral("No output file was selected")};
    }

    return writeAtomically(
        outputPath, [image, format, encoding](QIODevice* device, QString* error) {
            return snow_shot::image_codec::encodeToDevice(image, device, snowImageFormat(format),
                                                          encodeOptions(format, encoding), error);
        });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::write(const snow_shot::storage::PreparedPngImage& png,
                                  const QString& path, std::function<bool()> cancelled) {
    const QString outputPath = normalizedPath(path, ScreenshotImageFileFormat::Png);
    if (!png.isValid())
        return {{}, QStringLiteral("The screenshot image is empty")};
    if (outputPath.isEmpty())
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "No output file was selected")};
    const QString cancelledMessage =
        QCoreApplication::translate("ScreenshotImageFileService", "The save was cancelled");
    if (cancelled && cancelled())
        return {{}, cancelledMessage};
    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "The output directory could not be created")};
    return writeAtomically(
        outputPath, [&png, &cancelled, &cancelledMessage](QIODevice* device, QString* error) {
            if (cancelled && cancelled()) {
                *error = cancelledMessage;
                return false;
            }
            const QByteArray& bytes = png.bytes();
            if (device->write(bytes) != bytes.size()) {
                *error = device->errorString();
                return false;
            }
            if (cancelled && cancelled()) {
                *error = cancelledMessage;
                return false;
            }
            return true;
        });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::write(const ScreenshotImageRowSource& source, const QString& path,
                                  ScreenshotImageFileFormat format, ScreenshotPdfOptions pdf,
                                  ScreenshotImageEncodingOptions encoding) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot image source is empty")};
    }
    const QString outputPath = normalizedPath(path, format);
    if (outputPath.isEmpty()) {
        return {{}, QStringLiteral("No output file was selected")};
    }
    if (format == ScreenshotImageFileFormat::Pdf) {
        QString error;
        pdf.quality = qBound(0, encoding.quality, 100);
        auto payload = screenshot_pdf::prepare(source, pdf.quality, &error);
        return payload
                   ? writePdf(*payload, outputPath, std::move(pdf), source.cancellationRequested)
                   : ScreenshotImageFileSaveResult{{}, error};
    }
    return writeAtomically(
        outputPath, [&source, format, encoding](QIODevice* device, QString* error) {
            return snow_shot::image_codec::encodeToDevice(source, device, snowImageFormat(format),
                                                          encodeOptions(format, encoding), error);
        });
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const QImage& image, const QStringList& candidateDirectories, const QDateTime& timestamp) {
    return saveAutomatically(image, candidateDirectories, ScreenshotImageFileFormat::Png,
                             QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), timestamp);
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const QImage& image, const QStringList& candidateDirectories, ScreenshotImageFileFormat format,
    const QString& filenameFormat, const QDateTime& timestamp, ScreenshotPdfOptions pdf,
    ScreenshotImageEncodingOptions encoding) {
    if (image.isNull()) {
        return {{}, QStringLiteral("The screenshot image is empty")};
    }

    pdf.quality = qBound(0, encoding.quality, 100);
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
        const ScreenshotImageFileSaveResult result = write(image, path, format, pdf, {}, encoding);
        if (result.succeeded()) {
            return result;
        }
        lastError = result.error;
    }
    return {{}, lastError};
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const ScreenshotImageRowSource& source, const QStringList& candidateDirectories,
    ScreenshotImageFileFormat format, const QString& filenameFormat, const QDateTime& timestamp,
    ScreenshotPdfOptions pdf, ScreenshotImageEncodingOptions encoding) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot image source is empty")};
    }

    pdf.quality = qBound(0, encoding.quality, 100);
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
        const ScreenshotImageFileSaveResult result = write(source, path, format, pdf, encoding);
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

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveAutomatically(
    const snow_shot::storage::PreparedPngImage& png, const QStringList& candidateDirectories,
    const QString& filenameFormat, const QDateTime& timestamp) {
    if (!png.isValid()) {
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

        const QString path = collisionSafePath(directory, baseName, QStringLiteral("png"));
        const ScreenshotImageFileSaveResult result = write(png, path);
        if (result.succeeded()) {
            return result;
        }
        lastError = result.error;
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
