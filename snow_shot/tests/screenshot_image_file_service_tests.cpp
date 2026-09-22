#include "snow_shot/presentation/screenshotimagefileservice.h"

#include "snowimageqtcodec.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QStandardPaths>
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
        if (source == QStringLiteral("BMP image (*.bmp)")) {
            return QStringLiteral("BMP localized (*.bmp)");
        }
        return {};
    }
};

void namingAndFormatSelection() {
    require(ScreenshotImageFileService::formatForKey(QStringLiteral("PDF")) ==
                    ScreenshotImageFileFormat::Pdf &&
                ScreenshotImageFileService::formatForPath(QStringLiteral("capture.PDF")) ==
                    ScreenshotImageFileFormat::Pdf &&
                ScreenshotImageFileService::formatKey(ScreenshotImageFileFormat::Pdf) ==
                    QStringLiteral("pdf") &&
                ScreenshotImageFileService::normalizedPath(QStringLiteral("capture.png"),
                                                           ScreenshotImageFileFormat::Pdf) ==
                    QStringLiteral("capture.pdf") &&
                ScreenshotImageFileService::saveDialogFilter().contains(
                    ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Pdf)) &&
                ScreenshotImageFileService::formatForDialogSelection(
                    QStringLiteral("capture"),
                    ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Pdf)) ==
                    ScreenshotImageFileFormat::Pdf,
            "PDF must be selectable and use its canonical suffix in both dialogs");

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
    require(ScreenshotImageFileService::extension(ScreenshotImageFileFormat::Bmp) ==
                    QStringLiteral("bmp") &&
                ScreenshotImageFileService::formatForKey(QStringLiteral("bmp")) ==
                    ScreenshotImageFileFormat::Bmp &&
                ScreenshotImageFileService::formatForKey(QStringLiteral("jpeg")) ==
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
    require(ScreenshotImageFileService::formatForDialogSelection(
                QStringLiteral("capture.bmp"), QStringLiteral("PNG image (*.png)")) ==
                ScreenshotImageFileFormat::Bmp,
            "the BMP suffix should select BMP independently of the selected dialog filter");

    SaveDialogTranslator translator;
    require(QCoreApplication::installTranslator(&translator),
            "save-dialog translator must install");
    const QString localizedPng =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png);
    const QString localizedJpeg =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Jpeg);
    const QString localizedBmp =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Bmp);
    require(localizedPng == QStringLiteral("PNG localized (*.png)") &&
                localizedJpeg == QStringLiteral("JPEG localized (*.jpg *.jpeg)") &&
                localizedBmp == QStringLiteral("BMP localized (*.bmp)") &&
                ScreenshotImageFileService::saveDialogFilter().startsWith(
                    localizedPng + QStringLiteral(";;") + localizedJpeg + QStringLiteral(";;") +
                    localizedBmp),
            "save-dialog format descriptions must use the active application translator");
    require(ScreenshotImageFileService::formatForDialogSelection(QStringLiteral("capture.unknown"),
                                                                 localizedJpeg) ==
                ScreenshotImageFileFormat::Jpeg,
            "localized save-dialog filters must still resolve to their image format");
    require(ScreenshotImageFileService::formatForDialogSelection(
                QStringLiteral("capture.unknown"), localizedBmp) == ScreenshotImageFileFormat::Bmp,
            "the localized BMP filter must resolve to BMP output");
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
    require(ScreenshotImageFileService::normalizedPath(QStringLiteral("capture.png"),
                                                       ScreenshotImageFileFormat::Bmp) ==
                QStringLiteral("capture.bmp"),
            "BMP output must replace a mismatched explicit extension");
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
             ScreenshotImageFileFormat::Bmp,
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

void bmpPreservesTransparentPixels() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary BMP directory could not be created");
    QImage source = image();
    source.setPixelColor(1, 1, QColor(10, 20, 30, 40));
    const ScreenshotImageFileSaveResult result = ScreenshotImageFileService::write(
        source, directory.filePath(QStringLiteral("transparent")), ScreenshotImageFileFormat::Bmp);
    const QImage decoded =
        snow_shot::image_codec::decodeFile(result.path, snow::image::Format::bmp);
    require(result.succeeded() && decoded.size() == source.size(),
            "BMP output must be encoded and decoded by snow_image");
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            require(decoded.pixelColor(x, y) == source.pixelColor(x, y),
                    "BMP output must preserve RGBA screenshot pixels");
        }
    }
}

void jpegUsesTheCodecTransparencyPolicy() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary JPEG directory could not be created");
    QImage source(QSize(16, 16), QImage::Format_RGBA8888);
    source.fill(QColor(160, 80, 40, 128));

    const ScreenshotImageFileSaveResult result = ScreenshotImageFileService::write(
        source, directory.filePath(QStringLiteral("transparent")), ScreenshotImageFileFormat::Jpeg);
    const QImage decoded =
        snow_shot::image_codec::decodeFile(result.path, snow::image::Format::jpeg);
    require(result.succeeded() && decoded.size() == source.size(),
            "transparent JPEG output must be encoded and decoded by snow_image");

    const QColor pixel = decoded.pixelColor(decoded.width() / 2, decoded.height() / 2);
    const auto near = [](int actual, int expected) { return std::abs(actual - expected) <= 4; };
    require(near(pixel.red(), 80) && near(pixel.green(), 40) && near(pixel.blue(), 20),
            "the Snow Shot bridge must delegate JPEG alpha removal to the codec's black matte");
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

void automaticDirectoriesUseSystemLocations() {
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QStringList directories = ScreenshotImageFileService::automaticDirectories();
    if (!pictures.isEmpty()) {
        require(!directories.isEmpty() && directories.constFirst() == pictures,
                "automatic saving must prefer the system Pictures directory without a suffix");
    }
    if (!documents.isEmpty()) {
        require(directories.contains(documents, Qt::CaseInsensitive),
                "automatic saving must include the system Documents directory as a fallback");
    }
    for (const QString& directory : directories) {
        require(!directory.isEmpty() && (directory == pictures || directory == documents),
                "default save directories must come directly from the system");
    }
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
        image(), candidates, ScreenshotImageFileFormat::Bmp,
        QStringLiteral("Auto_{yyyyMMdd_HHmmss}"), timestamp);
    require(
        result.succeeded() &&
            result.path ==
                QDir(directory.path()).filePath(QStringLiteral("Auto_20260814_090706.bmp")) &&
            snow_shot::image_codec::inspectFile(result.path, snow::image::Format::bmp, QSize(3, 2)),
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

void codecCapabilitiesAndEncodingOptionsMatchTheOutputContract() {
    for (const auto format : {ScreenshotImageFileFormat::Jpeg, ScreenshotImageFileFormat::Webp,
                              ScreenshotImageFileFormat::Jxl, ScreenshotImageFileFormat::Avif,
                              ScreenshotImageFileFormat::Pdf}) {
        require(ScreenshotImageFileService::supportsQuality(format),
                "quality-supporting format was not advertised");
    }
    for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Bmp}) {
        require(!ScreenshotImageFileService::supportsQuality(format),
                "quality must be hidden for PNG and BMP");
    }
    for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Webp,
                              ScreenshotImageFileFormat::Jxl, ScreenshotImageFileFormat::Avif}) {
        require(ScreenshotImageFileService::supportsCompressionLevel(format),
                "compression-supporting format was not advertised");
    }
    for (const auto format : {ScreenshotImageFileFormat::Jpeg, ScreenshotImageFileFormat::Bmp,
                              ScreenshotImageFileFormat::Pdf}) {
        require(!ScreenshotImageFileService::supportsCompressionLevel(format),
                "compression must be hidden for JPEG, BMP, and PDF");
    }

    const auto options = [](ScreenshotImageFileFormat format, int quality,
                            ScreenshotCompressionLevel compression) {
        return ScreenshotImageFileService::encodeOptions(
            format, ScreenshotImageEncodingOptions{quality, compression});
    };
    require(options(ScreenshotImageFileFormat::Png, 100, ScreenshotCompressionLevel::Low)
                        .compression_level == 0 &&
                options(ScreenshotImageFileFormat::Png, 100, ScreenshotCompressionLevel::Medium)
                        .compression_level == 6 &&
                options(ScreenshotImageFileFormat::Png, 100, ScreenshotCompressionLevel::High)
                        .compression_level == 9,
            "PNG compression must map Low/Medium/High to 0/6/9");
    const auto webpLossyLow =
        options(ScreenshotImageFileFormat::Webp, 85, ScreenshotCompressionLevel::Low);
    const auto webpLossyMedium =
        options(ScreenshotImageFileFormat::Webp, 85, ScreenshotCompressionLevel::Medium);
    const auto webpLossyHigh =
        options(ScreenshotImageFileFormat::Webp, 85, ScreenshotCompressionLevel::High);
    require(!webpLossyLow.lossless && webpLossyLow.effort == 0 && webpLossyMedium.effort == 4 &&
                webpLossyHigh.effort == 6,
            "lossy WebP compression must map Low/Medium/High to effort 0/4/6");
    const auto webpLosslessLow =
        options(ScreenshotImageFileFormat::Webp, 100, ScreenshotCompressionLevel::Low);
    const auto webpLosslessMedium =
        options(ScreenshotImageFileFormat::Webp, 100, ScreenshotCompressionLevel::Medium);
    const auto webpLosslessHigh =
        options(ScreenshotImageFileFormat::Webp, 100, ScreenshotCompressionLevel::High);
    require(webpLosslessLow.lossless && webpLosslessLow.lossless_effort == 0 &&
                webpLosslessMedium.lossless_effort == 6 && webpLosslessHigh.lossless_effort == 9,
            "lossless WebP compression must map Low/Medium/High to effort 0/6/9");
    require(
        options(ScreenshotImageFileFormat::Jxl, 100, ScreenshotCompressionLevel::Low).effort == 1 &&
            options(ScreenshotImageFileFormat::Jxl, 100, ScreenshotCompressionLevel::Medium)
                    .effort == 7 &&
            options(ScreenshotImageFileFormat::Jxl, 100, ScreenshotCompressionLevel::High).effort ==
                10,
        "JPEG XL compression must map Low/Medium/High to effort 1/7/10");
    require(options(ScreenshotImageFileFormat::Avif, 100, ScreenshotCompressionLevel::Low).effort ==
                    1 &&
                options(ScreenshotImageFileFormat::Avif, 100, ScreenshotCompressionLevel::Medium)
                        .effort == 6 &&
                options(ScreenshotImageFileFormat::Avif, 100, ScreenshotCompressionLevel::High)
                        .effort == 9,
            "AVIF compression must map Low/Medium/High to effort 1/6/9");
    require(
        options(ScreenshotImageFileFormat::Jxl, 100, ScreenshotCompressionLevel::Low).lossless &&
            options(ScreenshotImageFileFormat::Avif, 100, ScreenshotCompressionLevel::Low)
                .lossless &&
            options(ScreenshotImageFileFormat::Jpeg, 0, ScreenshotCompressionLevel::High).quality ==
                0,
        "quality 100 must select supported lossless modes and quality 0 must be preserved");

    QTemporaryDir directory;
    require(directory.isValid() &&
                ScreenshotImageFileService::write(
                    image(), directory.filePath(QStringLiteral("quality-zero.jpg")),
                    ScreenshotImageFileFormat::Jpeg, {}, {},
                    ScreenshotImageEncodingOptions{0, ScreenshotCompressionLevel::Low})
                    .succeeded(),
            "the JPEG codec must normalize the preserved quality-zero request when encoding");
}

void encodedFilesPublishAtomically() {
    QTemporaryDir directory;
    require(directory.isValid(), "encoded save directory unavailable");
    const QString encoded = directory.filePath(QStringLiteral("encoded.png"));
    const QString destination = directory.filePath(QStringLiteral("nested/output.png"));
    require(ScreenshotImageFileService::write(image(), encoded, ScreenshotImageFileFormat::Png)
                .succeeded(),
            "encoded save fixture failed");
    require(ScreenshotImageFileService::writeEncodedFile(encoded, destination,
                                                         ScreenshotImageFileFormat::Png)
                .succeeded(),
            "encoded save must create missing directories");
    auto readDestination = [&] {
        QFile file(destination);
        require(file.open(QIODevice::ReadOnly), "encoded output unavailable");
        return file.readAll();
    };
    const QByteArray original = readDestination();
    int cancellationChecks = 0;
    const auto cancelled = ScreenshotImageFileService::writeEncodedFile(
        encoded, destination, ScreenshotImageFileFormat::Png,
        [&] { return ++cancellationChecks >= 3; });
    require(!cancelled.succeeded() && !cancelled.error.isEmpty() && readDestination() == original,
            "cancellation before commit must preserve the existing destination");
    QFile empty(directory.filePath(QStringLiteral("empty.png")));
    require(empty.open(QIODevice::WriteOnly), "empty encoded fixture unavailable");
    empty.close();
    require(!ScreenshotImageFileService::writeEncodedFile(empty.fileName(), destination,
                                                          ScreenshotImageFileFormat::Png)
                    .succeeded() &&
                readDestination() == original,
            "an empty encoded source must never replace an existing file");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        namingAndFormatSelection();
        writesLosslessImageAndPreservesCollisionNames();
        writesEveryAdvertisedFormat();
        bmpPreservesTransparentPixels();
        jpegUsesTheCodecTransparencyPolicy();
        streamsRowsToAtomicFileAndCancelsWithoutPublishing();
        automaticDirectoriesUseSystemLocations();
        configuredAutomaticOutputUsesFormatDirectoryAndFilename();
        saveDialogPrefersTheLastExistingDirectory();
        retriesNextDirectoryAndPublishesFileOnlyClipboardData();
        codecCapabilitiesAndEncodingOptionsMatchTheOutputContract();
        encodedFilesPublishAtomically();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
