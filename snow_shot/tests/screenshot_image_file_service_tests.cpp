#include "snow_shot/presentation/screenshotimagefileservice.h"

#include "snowimageqtcodec.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTranslator>
#include <QUrl>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QImage image() {
    QImage result(QSize(3, 2), QImage::Format_RGBA8888);
    result.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    result.setPixelColor(1, 0, QColor(0, 255, 0, 255));
    result.setPixelColor(2, 0, QColor(0, 0, 255, 255));
    result.setPixelColor(0, 1, QColor(255, 255, 255, 255));
    result.setPixelColor(1, 1, QColor(0, 0, 0, 255));
    result.setPixelColor(2, 1, QColor(80, 100, 120, 255));
    return result;
}

class SaveDialogTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QString::fromLatin1(context) != QStringLiteral("ScreenshotImageFileService")) {
            return {};
        }
        const QString source = QString::fromUtf8(sourceText);
        if (source == QStringLiteral("PNG image (*.png)")) {
            return QStringLiteral("PNG localized (*.png)");
        }
        if (source == QStringLiteral("JPEG image (*.jpg *.jpeg)")) {
            return QStringLiteral("JPEG localized (*.jpg *.jpeg)");
        }
        return {};
    }
};

void namingAndFormatSelection() {
    const QDateTime timestamp(QDate(2026, 8, 14), QTime(9, 7, 6), QTimeZone::UTC);
    require(ScreenshotImageFileService::suggestedBaseName(timestamp) ==
                QStringLiteral("SnowShot_2026-08-14_09-07-06"),
            "automatic screenshot names must use the documented timestamp format");
    require(ScreenshotImageFileService::suggestedBaseName(
                QStringLiteral("Capture_{yyyyMMdd}_{HHmmss}_{zzz}"), timestamp) ==
                QStringLiteral("Capture_20260814_090706_000"),
            "filename formats must expand arbitrary date-time patterns inside braces");
    require(ScreenshotImageFileService::suggestedBaseName(
                QStringLiteral("SnowShot_{YYYY-MM-DD_HH-mm-ss}"), timestamp) ==
                QStringLiteral("SnowShot_2026-08-14_09-07-06"),
            "documented uppercase date tokens must map to Qt date-time fields");
    require(ScreenshotImageFileService::extension(ScreenshotImageFileFormat::Jpeg) ==
                QStringLiteral("jpg"),
            "JPEG should use the canonical jpg extension");
    require(ScreenshotImageFileService::formatForKey(QStringLiteral("jpeg")) ==
                    ScreenshotImageFileFormat::Jpeg &&
                ScreenshotImageFileService::formatForKey(QStringLiteral("webp")) ==
                    ScreenshotImageFileFormat::Webp &&
                ScreenshotImageFileService::formatForKey(QStringLiteral("unsupported")) ==
                    ScreenshotImageFileFormat::Png,
            "persisted image format keys must resolve to supported codecs with PNG fallback");
    require(ScreenshotImageFileService::formatForDialogSelection(
                QStringLiteral("capture.unknown"), QStringLiteral("JPEG image (*.jpg *.jpeg)")) ==
                ScreenshotImageFileFormat::Jpeg,
            "an unrecognized suffix should defer to the selected save-dialog filter");

    SaveDialogTranslator translator;
    require(QCoreApplication::installTranslator(&translator),
            "save-dialog translator must install");
    const QString localizedPng =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png);
    const QString localizedJpeg =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Jpeg);
    require(localizedPng == QStringLiteral("PNG localized (*.png)") &&
                localizedJpeg == QStringLiteral("JPEG localized (*.jpg *.jpeg)") &&
                ScreenshotImageFileService::saveDialogFilter().startsWith(
                    localizedPng + QStringLiteral(";;") + localizedJpeg),
            "save-dialog format descriptions must use the active application translator");
    require(ScreenshotImageFileService::formatForDialogSelection(QStringLiteral("capture.unknown"),
                                                                 localizedJpeg) ==
                ScreenshotImageFileFormat::Jpeg,
            "localized save-dialog filters must still resolve to their image format");
    QCoreApplication::removeTranslator(&translator);

    require(ScreenshotImageFileService::normalizedPath(QStringLiteral("capture.unknown"),
                                                       ScreenshotImageFileFormat::Png) ==
                QStringLiteral("capture.png"),
            "unsupported suffixes must be replaced by the selected format extension");
    require(ScreenshotImageFileService::normalizedPath(QStringLiteral("capture.jpg"),
                                                       ScreenshotImageFileFormat::Png) ==
                QStringLiteral("capture.png"),
            "recognized suffixes must still agree with the requested output format");
    require(ScreenshotImageFileService::normalizedPath(QStringLiteral("capture"),
                                                       ScreenshotImageFileFormat::Webp) ==
                QStringLiteral("capture.webp"),
            "paths without a suffix must receive the selected format extension");
    require(ScreenshotImageFileService::normalizedPath(QStringLiteral(".capture"),
                                                       ScreenshotImageFileFormat::Png) ==
                QStringLiteral(".capture.png"),
            "dot-prefixed names must retain their stem when an extension is added");
}

void writesLosslessImageAndPreservesCollisionNames() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory could not be created");
    const QDateTime timestamp(QDate(2026, 8, 14), QTime(9, 7, 6), QTimeZone::UTC);
    const QString base = ScreenshotImageFileService::suggestedBaseName(timestamp);
    const QString firstPath = QDir(directory.path()).filePath(base + QStringLiteral(".png"));
    QFile collision(firstPath);
    require(collision.open(QIODevice::WriteOnly) && collision.write("existing") == 8,
            "collision fixture could not be created");
    collision.close();

    const ScreenshotImageFileSaveResult result = ScreenshotImageFileService::saveAutomatically(
        image(), QStringList{directory.path()}, timestamp);
    require(result.succeeded(), "automatic PNG save should succeed");
    require(result.path == QDir(directory.path()).filePath(base + QStringLiteral("_1.png")),
            "automatic saves must preserve existing files and add a numeric suffix");
    require(snow_shot::image_codec::inspectFile(result.path, snow::image::Format::png, QSize(3, 2)),
            "the automatic PNG must be encoded by snow_image");
}

void writesEveryAdvertisedFormat() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary format directory could not be created");

    for (const ScreenshotImageFileFormat format : {
             ScreenshotImageFileFormat::Png,
             ScreenshotImageFileFormat::Jpeg,
             ScreenshotImageFileFormat::Webp,
             ScreenshotImageFileFormat::Jxl,
             ScreenshotImageFileFormat::Avif,
         }) {
        const QString path = QDir(directory.path())
                                 .filePath(QStringLiteral("encoded.%1")
                                               .arg(ScreenshotImageFileService::extension(format)));
        const ScreenshotImageFileSaveResult result =
            ScreenshotImageFileService::write(image(), path, format);
        require(result.succeeded(), "an advertised Save As format could not be encoded");
        require(snow_shot::image_codec::inspectFile(
                    result.path, ScreenshotImageFileService::snowImageFormat(format), QSize(3, 2)),
                "an advertised Save As output could not be inspected by snow_image");
    }
}

void streamsRowsToAtomicFileAndCancelsWithoutPublishing() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary streaming directory could not be created");
    const QImage pixels = image();
    int rowRequests = 0;
    ScreenshotImageRowSource source;
    source.size = pixels.size();
    source.readRows = [&pixels, &rowRequests](int firstRow, int rowCount,
                                              qsizetype destinationStride, uchar* destination,
                                              qsizetype destinationSize) {
        const qsizetype rowBytes = pixels.width() * 4;
        if (firstRow < 0 || rowCount <= 0 || firstRow + rowCount > pixels.height() ||
            destinationStride < rowBytes ||
            destinationSize < destinationStride * (rowCount - 1) + rowBytes) {
            return false;
        }
        ++rowRequests;
        for (int row = 0; row < rowCount; ++row) {
            std::memcpy(destination + row * destinationStride, pixels.constScanLine(firstRow + row),
                        static_cast<std::size_t>(rowBytes));
        }
        return true;
    };

    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("rows.png"));
    const ScreenshotImageFileSaveResult saved =
        ScreenshotImageFileService::write(source, outputPath, ScreenshotImageFileFormat::Png);
    require(saved.succeeded() && rowRequests > 0 &&
                snow_shot::image_codec::inspectFile(saved.path, snow::image::Format::png,
                                                    pixels.size()),
            "row-source save must stream a valid PNG through the atomic file");

    source.cancellationRequested = []() { return true; };
    const QString cancelledPath = QDir(directory.path()).filePath(QStringLiteral("cancelled.png"));
    const ScreenshotImageFileSaveResult cancelled =
        ScreenshotImageFileService::write(source, cancelledPath, ScreenshotImageFileFormat::Png);
    require(!cancelled.succeeded() && !QFileInfo::exists(cancelledPath),
            "a cancelled row-source save must not publish its temporary file");
}

void configuredAutomaticOutputUsesFormatDirectoryAndFilename() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary configured-output directory could not be created");
    const QDateTime timestamp(QDate(2026, 8, 14), QTime(9, 7, 6), QTimeZone::UTC);
    const QStringList candidates =
        ScreenshotImageFileService::automaticDirectories(directory.path());
    require(!candidates.isEmpty() && candidates.constFirst() == directory.path(),
            "an existing configured directory must precede platform fallbacks");

    const ScreenshotImageFileSaveResult result = ScreenshotImageFileService::saveAutomatically(
        image(), candidates, ScreenshotImageFileFormat::Webp,
        QStringLiteral("Auto_{yyyyMMdd_HHmmss}"), timestamp);
    require(result.succeeded() &&
                result.path ==
                    QDir(directory.path()).filePath(QStringLiteral("Auto_20260814_090706.webp")) &&
                snow_shot::image_codec::inspectFile(result.path, snow::image::Format::webp,
                                                    QSize(3, 2)),
            "configured automatic output must apply its directory, filename, and image format");

    const QString missing = QDir(directory.path()).filePath(QStringLiteral("missing"));
    const QStringList fallbackCandidates =
        ScreenshotImageFileService::automaticDirectories(missing);
    require(!fallbackCandidates.contains(missing, Qt::CaseInsensitive),
            "a missing configured directory must be skipped in favor of platform fallbacks");
}

void saveDialogPrefersTheLastExistingDirectory() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary Save As directory could not be created");
    const QString configured = QDir(temporary.path()).filePath(QStringLiteral("configured"));
    const QString remembered = QDir(temporary.path()).filePath(QStringLiteral("remembered"));
    require(QDir().mkpath(configured) && QDir().mkpath(remembered),
            "Save As directory fixtures could not be created");

    require(ScreenshotImageFileService::saveDialogDirectory(remembered, configured) == remembered,
            "Save As must prefer the last selected directory");
    require(ScreenshotImageFileService::saveDialogDirectory(
                QDir(temporary.path()).filePath(QStringLiteral("missing")), configured) ==
                configured,
            "Save As must fall back when the remembered directory no longer exists");
}

void retriesNextDirectoryAndPublishesFileOnlyClipboardData() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory could not be created");
    const QString blockedPath = QDir(directory.path()).filePath(QStringLiteral("blocked"));
    QFile blocked(blockedPath);
    require(blocked.open(QIODevice::WriteOnly) && blocked.write("x") == 1,
            "blocked-directory fixture could not be created");
    blocked.close();
    const QString fallback = QDir(directory.path()).filePath(QStringLiteral("fallback"));

    const ScreenshotImageFileSaveResult result = ScreenshotImageFileService::saveAutomatically(
        image(), QStringList{blockedPath, fallback},
        QDateTime(QDate(2026, 8, 14), QTime(9, 7, 6), QTimeZone::UTC));
    require(result.succeeded() && result.path.startsWith(fallback),
            "automatic saving should retry the next candidate directory after a failure");

    QClipboard* clipboard = QGuiApplication::clipboard();
    require(ScreenshotImageFileService::publishFileToClipboard(clipboard, result.path),
            "file clipboard publication should succeed for an existing file");
    const QMimeData* mime = clipboard->mimeData();
    require(mime != nullptr && mime->urls().size() == 1 && mime->hasUrls() &&
                mime->urls().constFirst().isLocalFile() &&
                mime->urls().constFirst().toLocalFile() ==
                    QFileInfo(result.path).absoluteFilePath() &&
                !mime->hasImage(),
            "file clipboard mode must publish a local file URL without image data");
}

void codecOptionsUseFastLosslessAndMaximumJpegQuality() {
    const auto jpeg = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Jpeg);
    require(jpeg.format == snow::image::Format::jpeg && jpeg.quality == 100,
            "JPEG saves must use quality 100");
    const auto png = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png);
    require(png.format == snow::image::Format::png && png.compression_level == 0,
            "PNG saves must use the fastest compression setting");
    const auto webp = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Webp);
    require(webp.lossless && webp.lossless_effort == 0,
            "lossless WebP saves must use the fastest lossless effort");
    const auto jxl = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Jxl);
    const auto avif = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Avif);
    require(jxl.lossless && jxl.effort == 1 && avif.lossless && avif.effort == 1,
            "lossless JXL and AVIF saves must use the fastest effort");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        namingAndFormatSelection();
        writesLosslessImageAndPreservesCollisionNames();
        writesEveryAdvertisedFormat();
        streamsRowsToAtomicFileAndCancelsWithoutPublishing();
        configuredAutomaticOutputUsesFormatDirectoryAndFilename();
        saveDialogPrefersTheLastExistingDirectory();
        retriesNextDirectoryAndPublishesFileOnlyClipboardData();
        codecOptionsUseFastLosslessAndMaximumJpegQuality();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
