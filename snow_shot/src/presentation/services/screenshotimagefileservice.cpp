#include "snow_shot/presentation/screenshotimagefileservice.h"

#include "snowimageqtcodec.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/storage/timevariables.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QMimeData>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <utility>

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
    case ScreenshotImageFileFormat::Tiff:
        return QCoreApplication::translate("ScreenshotImageFileService",
                                           "TIFF image (*.tif *.tiff)");
    }
    return {};
}

QString collisionSafePath(const QString& directory, const QString& baseName,
                          const QString& extension) {
    const QDir target(directory);
    QString candidate = target.filePath(
        snow_shot::storage::composeExportFileName(baseName, QStringLiteral(".%1").arg(extension)));
    for (int suffix = 1; QFileInfo::exists(candidate); ++suffix) {
        candidate = target.filePath(snow_shot::storage::composeExportFileName(
            baseName, QStringLiteral("_%1.%2").arg(suffix).arg(extension)));
    }
    return candidate;
}

// Finalized image and PDF bytes pass sequentially to the atomic file. TIFF encoders
// stage their seekable output first so backpatches are included in this digest.
class HashedOutput final : public QIODevice {
  public:
    explicit HashedOutput(QIODevice& output) : m_output(output) {
        open(QIODevice::WriteOnly);
    }
    bool isSequential() const override {
        return true;
    }
    qint64 pos() const override {
        return m_count;
    }
    QByteArray sha256() const {
        return m_hash.result().toHex();
    }
    qint64 byteCount() const {
        return m_count;
    }

  protected:
    qint64 readData(char*, qint64) override {
        return -1;
    }
    qint64 writeData(const char* data, qint64 size) override {
        const qint64 written = m_output.write(data, size);
        if (written > 0) {
            m_hash.addData(QByteArrayView(data, written));
            m_count += written;
        }
        if (written != size)
            setErrorString(m_output.errorString());
        return written;
    }

  private:
    QIODevice& m_output;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_count = 0;
};

template <typename Encoder>
ScreenshotImageFileSaveResult writeAtomically(const QString& outputPath, Encoder&& encoder) {
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return {{}, file.errorString()};
    }
    HashedOutput output(file);
    QString encodeError;
    if (!encoder(&output, &encodeError)) {
        file.cancelWriting();
        return {{},
                encodeError.isEmpty() ? QStringLiteral("The image could not be encoded")
                                      : encodeError};
    }
    if (!file.commit()) {
        return {{}, file.errorString()};
    }
    return {outputPath, {}, output.sha256(), output.byteCount()};
}

template <typename Writer>
ScreenshotImageFileSaveResult
savePreparedAutomatically(const QStringList& directories, ScreenshotImageFileFormat format,
                          const QString& filenameFormat, const QDateTime& timestamp,
                          const std::function<bool()>& cancelled,
                          const snow_shot::storage::ScreenshotWindowNames& names, Writer&& writer) {
    const QString baseName =
        ScreenshotImageFileService::suggestedBaseName(filenameFormat, timestamp, names);
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\')))
        return {{}, QStringLiteral("The screenshot filename format is invalid")};
    QString lastError = QStringLiteral("No automatic screenshot folder is available");
    for (const QString& candidate : directories) {
        if (cancelled && cancelled())
            return {{}, QStringLiteral("The screenshot save was cancelled")};
        const QString directory = QDir::cleanPath(candidate.trimmed());
        if (candidate.trimmed().isEmpty())
            continue;
        if (!QDir().mkpath(directory)) {
            lastError =
                QStringLiteral("The screenshot folder could not be created: %1").arg(directory);
            continue;
        }
        auto result = writer(
            collisionSafePath(directory, baseName, ScreenshotImageFileService::extension(format)));
        if (result.succeeded())
            return result;
        lastError = result.error;
    }
    return {{}, lastError};
}
} // namespace

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveEncodedAutomatically(
    const QString& encodedFile, const QStringList& directories, ScreenshotImageFileFormat format,
    const QString& filenameFormat, const QDateTime& timestamp, std::function<bool()> cancelled,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    return savePreparedAutomatically(
        directories, format, filenameFormat, timestamp, cancelled, names, [&](const QString& path) {
            return writeEncodedFile(encodedFile, path, format, cancelled);
        });
}

ScreenshotImageFileSaveResult
ScreenshotImageFileService::writeEncodedBytes(const QByteArray& bytes, const QString& path,
                                              ScreenshotImageFileFormat format,
                                              std::function<bool()> cancelled) {
    const QString outputPath = normalizedPath(path, format);
    if (bytes.isEmpty())
        return {{},
                QCoreApplication::translate("ScreenshotImageFileService",
                                            "The encoded image is empty")};
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
    return writeAtomically(outputPath, [&](QIODevice* device, QString* error) {
        for (qsizetype offset = 0; offset < bytes.size(); offset += 1024 * 1024) {
            if (cancelled && cancelled()) {
                *error = QCoreApplication::translate("ScreenshotImageFileService",
                                                     "The save was cancelled");
                return false;
            }
            const qsizetype count = qMin(qsizetype{1024 * 1024}, bytes.size() - offset);
            if (device->write(bytes.constData() + offset, count) != count) {
                *error = device->errorString();
                return false;
            }
        }
        if (cancelled && cancelled()) {
            *error =
                QCoreApplication::translate("ScreenshotImageFileService", "The save was cancelled");
            return false;
        }
        return true;
    });
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::saveEncodedBytesAutomatically(
    const QByteArray& bytes, const QStringList& directories, ScreenshotImageFileFormat format,
    const QString& filenameFormat, const QDateTime& timestamp, std::function<bool()> cancelled,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    return savePreparedAutomatically(
        directories, format, filenameFormat, timestamp, cancelled, names,
        [&](const QString& path) { return writeEncodedBytes(bytes, path, format, cancelled); });
}

ScreenshotImageFileSaveResult ScreenshotImageFileService::savePdfAutomatically(
    const screenshot_pdf::Payload& payload, const QStringList& directories,
    const QString& filenameFormat, const QDateTime& timestamp, ScreenshotPdfOptions options,
    std::function<bool()> cancelled, const snow_shot::storage::ScreenshotWindowNames& names) {
    return savePreparedAutomatically(
        directories, ScreenshotImageFileFormat::Pdf, filenameFormat, timestamp, cancelled, names,
        [&](const QString& path) { return writePdf(payload, path, options, cancelled); });
}

QString ScreenshotImageFileService::suggestedBaseName(const QDateTime& timestamp) {
    return suggestedBaseName(QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), timestamp);
}

QString ScreenshotImageFileService::suggestedBaseName(
    const QString& filenameFormat, const QDateTime& timestamp,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    return snow_shot::storage::expandScreenshotExportVariables(filenameFormat, timestamp, names);
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
                       filterForFormat(ScreenshotImageFileFormat::Tiff),
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

QStringList ScreenshotImageFileService::automaticDirectories(
    const QString& configuredDirectory, const QDateTime& timestamp,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    QStringList directories;
    const QString expanded =
        snow_shot::storage::expandScreenshotExportVariables(configuredDirectory, timestamp, names);
    const QString configured = expanded.isEmpty() ? QString() : QDir::cleanPath(expanded);
    if (!configured.isEmpty() && (QFileInfo(configured).isDir() ||
                                  snow_shot::storage::containsTimeVariables(configuredDirectory))) {
        directories.push_back(configured);
    }
    for (const QString& fallback : automaticDirectories()) {
        if (!directories.contains(fallback, Qt::CaseInsensitive)) {
            directories.push_back(fallback);
        }
    }
    return directories;
}

QString ScreenshotImageFileService::saveDialogDirectory(
    const QString& lastDirectory, const QString& configuredDirectory, const QDateTime& timestamp,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    const QString remembered = lastDirectory.trimmed();
    if (!remembered.isEmpty()) {
        const QString cleaned = QDir::cleanPath(remembered);
        if (QFileInfo(cleaned).isDir()) {
            return cleaned;
        }
    }

    const QStringList candidates = automaticDirectories(configuredDirectory, timestamp, names);
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
    case ScreenshotImageFileFormat::Tiff:
        return QStringLiteral("tiff");
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
    if (normalized == QStringLiteral("tif") || normalized == QStringLiteral("tiff"))
        return ScreenshotImageFileFormat::Tiff;
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
    if (format == ScreenshotImageFileFormat::Tiff &&
        information.suffix().compare(QStringLiteral("tif"), Qt::CaseInsensitive) == 0)
        return path + QStringLiteral(".tif");
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
    if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff"))
        return ScreenshotImageFileFormat::Tiff;
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
          ScreenshotImageFileFormat::Tiff, ScreenshotImageFileFormat::Pdf}) {
        if (selectedFilter == filterForFormat(format)) {
            return format;
        }
    }
    return ScreenshotImageFileFormat::Png;
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
    if (format == ScreenshotImageFileFormat::Pdf || format == ScreenshotImageFileFormat::Tiff ||
        cancelled) {
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
    if (format == ScreenshotImageFileFormat::Tiff) {
        if (source.cancellationRequested && source.cancellationRequested())
            return {{},
                    QCoreApplication::translate("ScreenshotImageFileService",
                                                "The save was cancelled")};
        QTemporaryDir directory(QDir::temp().filePath(QStringLiteral("snow-shot-tiff-XXXXXX")));
        if (!directory.isValid())
            return {{}, directory.errorString()};
        const QString stagedPath = directory.filePath(QStringLiteral("encoded.tiff"));
        QFile staged(stagedPath);
        if (!staged.open(QIODevice::WriteOnly))
            return {{}, staged.errorString()};
        QString error;
        if (!snow_shot::image_codec::encodeToDevice(source, &staged, snowImageFormat(format),
                                                    encodeOptions(format, encoding), &error))
            return {{}, error};
        if (!staged.flush())
            return {{}, staged.errorString()};
        staged.close();
        return writeEncodedFile(stagedPath, outputPath, format, source.cancellationRequested);
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
    ScreenshotImageEncodingOptions encoding,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    if (image.isNull()) {
        return {{}, QStringLiteral("The screenshot image is empty")};
    }

    pdf.quality = qBound(0, encoding.quality, 100);
    const QString baseName = suggestedBaseName(filenameFormat, timestamp, names);
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
    ScreenshotPdfOptions pdf, ScreenshotImageEncodingOptions encoding,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot image source is empty")};
    }

    pdf.quality = qBound(0, encoding.quality, 100);
    const QString baseName = suggestedBaseName(filenameFormat, timestamp, names);
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
    const QString& filenameFormat, const QDateTime& timestamp, std::function<bool()> cancelled,
    const snow_shot::storage::ScreenshotWindowNames& names) {
    if (!png.isValid())
        return {{}, QStringLiteral("The screenshot image is empty")};
    return savePreparedAutomatically(
        candidateDirectories, ScreenshotImageFileFormat::Png, filenameFormat, timestamp, cancelled,
        names, [&](const QString& path) { return write(png, path, cancelled); });
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
