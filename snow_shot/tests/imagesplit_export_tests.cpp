// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"
#include "imagesplit_guarded_image.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTranslator>

#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage fixture() {
    QImage image(7, 5, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.setDevicePixelRatio(2);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(17 + 23 * x, 31 + 29 * y, 41 + x + y, (x + y) * 23));
    return image;
}

void verifyOutputs(const QImage& image, const ImageSplitLayout& layout,
                   const ImageSplitExportResult& result) {
    require(result.succeeded() && result.savedCount == layout.outputCount(),
            "all split outputs are saved successfully");
    QImage rebuilt(image.size(), QImage::Format_RGBA8888);
    rebuilt.fill(Qt::transparent);
    const QDir directory(result.directory);
    const auto files = directory.entryList(QDir::Files);
    require(files.size() == layout.outputCount(), "output contains exactly the numbered PNG files");
    for (qint64 index = 0; index < layout.outputCount(); ++index) {
        const QString name = QStringLiteral("%1.png").arg(index + 1);
        const QString path = directory.filePath(name);
        require(files.contains(name), "PNG names are consecutive one-based export ordinals");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) &&
                    file.read(8) == QByteArray::fromHex("89504e470d0a1a0a"),
                "each output is a PNG file");
        const QImage decoded = snow_shot::image_codec::decodeFile(path, snow::image::Format::png)
                                   .convertToFormat(QImage::Format_RGBA8888);
        const QRect region = layout.regionAt(index);
        require(!decoded.isNull() && decoded.size() == region.size(),
                "output dimensions match full-resolution region pixels");
        for (int y = 0; y < decoded.height(); ++y) {
            std::memcpy(rebuilt.scanLine(region.y() + y) + qsizetype(region.x()) * 4,
                        decoded.constScanLine(y), size_t(decoded.width()) * 4);
            for (int x = 0; x < decoded.width(); ++x) {
                const QColor actual = decoded.pixelColor(x, y);
                const QColor expected = image.pixelColor(region.x() + x, region.y() + y);
                if (actual != expected)
                    throw std::runtime_error(
                        QStringLiteral("%1: pixel (%2, %3) is %4, expected %5")
                            .arg(path)
                            .arg(x)
                            .arg(y)
                            .arg(actual.name(QColor::HexArgb), expected.name(QColor::HexArgb))
                            .toStdString());
            }
        }
    }
    for (int y = 0; y < rebuilt.height(); ++y)
        require(std::memcmp(rebuilt.constScanLine(y), image.constScanLine(y),
                            size_t(image.width()) * 4) == 0,
                "reassembled regions preserve every straight-alpha pixel exactly");
}

void orderingAndUniqueDirectories() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "export temporary directory exists");
    const QImage image = fixture();
    const QByteArray original(reinterpret_cast<const char*>(image.constBits()),
                              image.sizeInBytes());
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 3;
    const auto layout = calculateImageSplitLayout(image.size(), settings);
    std::vector<qint64> progress;
    const auto first = exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"),
                                         {}, [&progress, &layout](qint64 count, qint64 total) {
                                             require(total == layout.outputCount(),
                                                     "progress includes actual output count");
                                             progress.push_back(count);
                                         });
    require(QFileInfo(first.directory).fileName() == QStringLiteral("Split Images_sample"),
            "base output directory follows the requested filename pattern");
    require(progress == std::vector<qint64>{0, 1, 2, 3, 4, 5, 6},
            "progress begins before writing and advances once per saved image");
    verifyOutputs(image, layout, first);
    const auto second =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"));
    const auto third = exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"));
    require(QFileInfo(second.directory).fileName() == QStringLiteral("Split Images_sample (2)") &&
                QFileInfo(third.directory).fileName() == QStringLiteral("Split Images_sample (3)"),
            "existing output folders produce unique numbered directories");
    verifyOutputs(image, layout, second);
    verifyOutputs(image, layout, third);
    const QString longStem(200, u's');
    const auto longName = exportSplitImages(image, layout, temporary.path(), longStem);
    require(longName.succeeded() && QFileInfo(longName.directory).fileName() ==
                                        QStringLiteral("Split Images_%1").arg(longStem),
            "valid long filename stems retain their complete name without arbitrary truncation");
    verifyOutputs(image, layout, longName);
    require(QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()) ==
                original,
            "batch export never changes source pixels");
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = 3;
    const auto vertical = calculateImageSplitLayout(image.size(), settings);
    verifyOutputs(image, vertical,
                  exportSplitImages(image, vertical, temporary.path(), QStringLiteral("vertical")));
    settings.direction = ImageSplitDirection::Horizontal;
    const auto horizontal = calculateImageSplitLayout(image.size(), settings);
    verifyOutputs(
        image, horizontal,
        exportSplitImages(image, horizontal, temporary.path(), QStringLiteral("horizontal")));
}

void tiffImportsSplitWithoutLosingPixels() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "TIFF split import fixture directory exists");
    QImage original = fixture();
    original.setDevicePixelRatio(1);
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 3;
    const auto layout = calculateImageSplitLayout(original.size(), settings);
    for (const auto* suffix : {"tif", "tiff"}) {
        const auto saved = ScreenshotImageFileService::write(
            original, temporary.filePath(QStringLiteral("source.") + QLatin1String(suffix)),
            ScreenshotImageFileFormat::Tiff);
        require(saved.succeeded(), "TIFF split input is encoded");
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({saved.path});
        require(files.size() == 1, "both TIFF suffixes reach image splitting imports");
        const QImage imported = ScreenshotClipboardContentReader::decodeImageFile(files.first());
        require(imported == original,
                "TIFF import preserves straight alpha, color, and dimensions");
        verifyOutputs(original, layout,
                      exportSplitImages(imported, layout, temporary.path(),
                                        QStringLiteral("tiff-import-") + QLatin1String(suffix)));
    }
}

void exportNamesPreserveSplitPolicy() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "filename fixture directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const QString c1 = QString(QChar(0x7f)) + QChar(0x81);
    struct NameCase {
        QString input;
        QString expected;
    };
    const NameCase cases[] = {
        {QStringLiteral("  path/part.name<>:\"\\|?*") + QChar(1) + QStringLiteral(". "),
         QStringLiteral("path_part.name_________")},
        {QStringLiteral("control") + c1, QStringLiteral("control") + c1},
        {QStringLiteral("  ...  "), QStringLiteral("Screenshot")},
    };
    for (const auto& entry : cases) {
        const auto result = exportSplitImages(image, layout, temporary.path(), entry.input);
        require(result.succeeded() && QFileInfo(result.directory).fileName() ==
                                          QStringLiteral("Split Images_%1").arg(entry.expected),
                "split exports keep complete stems, C1 characters, and their existing fallback");
    }
}

void failureAndCancellation() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failure temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto initialEntries =
        QDir(temporary.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    const auto canceledBefore = exportSplitImages(image, layout, temporary.path(),
                                                  QStringLiteral("canceled"), [] { return true; });
    require(!canceledBefore.succeeded() && canceledBefore.directory.isEmpty() &&
                canceledBefore.savedCount == 0 && !canceledBefore.error.isEmpty() &&
                QDir(temporary.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries) ==
                    initialEntries,
            "cancellation before export creates no filesystem content");
    auto invalid = layout;
    invalid.verticalLines = {0};
    const auto invalidExport =
        exportSplitImages(image, invalid, temporary.path(), QStringLiteral("invalid"));
    require(!invalidExport.succeeded() && invalidExport.directory.isEmpty(),
            "invalid geometry is rejected before directory creation");
    const auto missingParent = exportSplitImages(
        image, layout, temporary.filePath(QStringLiteral("absent")), QStringLiteral("missing"));
    require(!missingParent.succeeded() && !missingParent.error.isEmpty() &&
                missingParent.directory.isEmpty(),
            "unavailable destination reports failure without creating parent paths");
    bool canceled = false;
    const auto partial = exportSplitImages(
        image, layout, temporary.path(), QStringLiteral("partial"),
        [&canceled] { return canceled; },
        [&canceled](qint64 saved, qint64) { canceled = saved == 1; });
    require(!partial.succeeded() && partial.savedCount == 1 && !partial.error.isEmpty() &&
                QFileInfo::exists(QDir(partial.directory).filePath(QStringLiteral("1.png"))) &&
                !QFileInfo::exists(QDir(partial.directory).filePath(QStringLiteral("2.png"))),
            "cancellation retains completed outputs and does not publish a partial next PNG");
    const QString failureDirectory = temporary.filePath(QStringLiteral("Split Images_failure"));
    const auto failed =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("failure"), {},
                          [&failureDirectory](qint64 saved, qint64) {
                              if (saved == 1)
                                  require(QDir(failureDirectory).mkdir(QStringLiteral("2.png")),
                                          "failure fixture obstructs the second PNG");
                          });
    require(!failed.succeeded() && failed.savedCount == 1 && !failed.error.isEmpty() &&
                QFileInfo::exists(QDir(failed.directory).filePath(QStringLiteral("1.png"))) &&
                QFileInfo(QDir(failed.directory).filePath(QStringLiteral("2.png"))).isDir(),
            "write failure preserves completed files and identifies failed output");
    QFile collision(temporary.filePath(QStringLiteral("Split Images_collision")));
    require(collision.open(QIODevice::WriteOnly) && collision.write("keep") == 4,
            "file collision fixture exists");
    collision.close();
    const auto fileCollision =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("collision"));
    require(fileCollision.succeeded() &&
                QFileInfo(fileCollision.directory).fileName() ==
                    QStringLiteral("Split Images_collision (2)") &&
                collision.open(QIODevice::ReadOnly) && collision.readAll() == QByteArray("keep"),
            "an existing file also forces a unique directory without overwriting it");
}

void concurrentDirectoryCreationAndNormalization() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "concurrent temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto exportWork = [&] {
        return exportSplitImages(image, layout, temporary.path(), QStringLiteral("concurrent"));
    };
    auto first = std::async(std::launch::async, exportWork);
    auto second = std::async(std::launch::async, exportWork);
    const auto firstResult = first.get();
    const auto secondResult = second.get();
    require(firstResult.succeeded() && secondResult.succeeded() &&
                firstResult.directory != secondResult.directory,
            "simultaneous exports reserve different directories atomically");
    verifyOutputs(image, layout, firstResult);
    verifyOutputs(image, layout, secondResult);
    const QImage premultiplied = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto normalized = premultiplied.convertToFormat(QImage::Format_RGBA8888);
    verifyOutputs(
        normalized, layout,
        exportSplitImages(premultiplied, layout, temporary.path(), QStringLiteral("normalized")));
}

QImage normalizedForExport(QImage image) {
    const QColorSpace srgb(QColorSpace::SRgb);
    if (image.colorSpace().isValid() && image.colorSpace() != srgb)
        image = image.convertedToColorSpace(srgb);
    return image.convertToFormat(QImage::Format_RGBA8888);
}

void croppedNativeRowsRespectAllocationBoundary() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "guarded export directory exists");
    int ordinal = 0;
    for (const auto format :
         {QImage::Format_RGB888, QImage::Format_BGR888, QImage::Format_RGB16,
          QImage::Format_Grayscale8, QImage::Format_Indexed8, QImage::Format_Mono,
          QImage::Format_MonoLSB, QImage::Format_ARGB32_Premultiplied,
          QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        for (const QSize size : {QSize(37, 1), QSize(37, 9), QSize(16421, 9)}) {
            QImage source = fixture()
                                .scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                                .convertToFormat(format);
            source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
            const QImage expected = normalizedForExport(source);
            require(!expected.isNull(), "guarded source supports full-image normalization");
            image_split_test::GuardedImage guarded(source);
            const auto* pixels = guarded.image().constBits();
            // The rightmost seven RGB888 pixels occupy 21 bytes, but Qt's aligned clone
            // needs 24. A borrowed view at x=30 has only 22 bytes left in the final row.
            // Other cuts also exercise bit offsets, row caching, and the 16K-column tile edge.
            ImageSplitLayout layout;
            layout.size = source.size();
            layout.verticalLines = {1, 8, 15, size.width() - 7};
            if (size.height() > 1)
                layout.horizontalLines = {2, 5};
            verifyOutputs(expected, layout,
                          exportSplitImages(guarded.image(), layout, temporary.path(),
                                            QStringLiteral("guarded_%1").arg(++ordinal)));
            require(guarded.image().constBits() == pixels && guarded.image() == source,
                    "export preserves native source pixels and metadata");
        }
    }
}

void boundedNativeFormatAndColorConversion() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "native-format temporary directory exists");
    const QImage reference =
        fixture().scaled(37, 41, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 3;
    settings.columns = 5;
    const auto layout = calculateImageSplitLayout(reference.size(), settings);
    int ordinal = 0;
    for (const auto format :
         {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied,
          QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGB888, QImage::Format_RGB32,
          QImage::Format_RGB16, QImage::Format_RGBA64, QImage::Format_RGBA64_Premultiplied,
          QImage::Format_RGBA16FPx4, QImage::Format_RGBA32FPx4, QImage::Format_Indexed8,
          QImage::Format_Mono, QImage::Format_MonoLSB}) {
        for (const auto& space :
             {QColorSpace{}, QColorSpace(QColorSpace::SRgb), QColorSpace(QColorSpace::DisplayP3),
              QColorSpace(QColorSpace::SRgbLinear)}) {
            QImage native = reference.convertToFormat(format);
            native.setColorSpace(space);
            require(!native.isNull(), "native-format fixture conversion succeeds");
            const auto* pixels = native.constBits();
            const QByteArray original(reinterpret_cast<const char*>(pixels), native.sizeInBytes());
            const auto palette = native.colorTable();
            const QImage expected = normalizedForExport(native);
            require(!expected.isNull(), "legacy full-image normalization succeeds");
            verifyOutputs(expected, layout,
                          exportSplitImages(native, layout, temporary.path(),
                                            QStringLiteral("native_%1").arg(++ordinal)));
            require(native.constBits() == pixels && native.format() == format &&
                        native.colorSpace() == space && native.colorTable() == palette &&
                        QByteArray(reinterpret_cast<const char*>(pixels), native.sizeInBytes()) ==
                            original,
                    "bounded conversion preserves source storage, profile, palette and pixels");
        }
    }

    // Cross both the conversion tile width and the converted-row cache limit.
    QImage wide(18001, 37, QImage::Format_ARGB32_Premultiplied);
    wide.fill(QColor(103, 71, 211, 97));
    wide.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    settings.direction = ImageSplitDirection::Horizontal;
    settings.count = 1;
    const auto wideLayout = calculateImageSplitLayout(wide.size(), settings);
    verifyOutputs(normalizedForExport(wide), wideLayout,
                  exportSplitImages(wide, wideLayout, temporary.path(), QStringLiteral("wide")));
}

void largeProfileConversionMatchesFullNormalization() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "large-profile temporary directory exists");
    QImage reference(1025, 1027, QImage::Format_RGBA8888);
    quint32 random = 0x513bc29u;
    for (int y = 0; y < reference.height(); ++y) {
        auto* row = reference.scanLine(y);
        for (int x = 0; x < reference.width(); ++x) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            row[qsizetype(x) * 4] = uchar(random);
            row[qsizetype(x) * 4 + 1] = uchar(random >> 8);
            row[qsizetype(x) * 4 + 2] = uchar(random >> 16);
            row[qsizetype(x) * 4 + 3] = uchar(random >> 24);
        }
    }
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 2;
    const auto layout = calculateImageSplitLayout(reference.size(), settings);
    int ordinal = 0;
    for (const auto format : {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        QImage native = reference.convertToFormat(format);
        native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        // Each crop exceeds Qt's 64K-pixel threshold. Noisy low-alpha colors expose differences
        // in native precision, parallel conversion rounding and cached-transform tile boundaries.
        const QImage expected = normalizedForExport(native);
        require(!native.isNull() && !expected.isNull(), "large-profile normalization succeeds");
        verifyOutputs(expected, layout,
                      exportSplitImages(native, layout, temporary.path(),
                                        QStringLiteral("large_profile_%1").arg(++ordinal)));
    }
}

void structurallyOpaqueSourcesWriteRgbPng() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "opaque temporary directory exists");
    const QImage native = fixture().convertToFormat(QImage::Format_RGB888);
    const auto layout = calculateImageSplitLayout(native.size(), {});
    const auto result =
        exportSplitImages(native, layout, temporary.path(), QStringLiteral("opaque"));
    verifyOutputs(normalizedForExport(native), layout, result);
    for (qint64 index = 0; index < layout.outputCount(); ++index) {
        QFile file(QDir(result.directory).filePath(QStringLiteral("%1.png").arg(index + 1)));
        require(file.open(QIODevice::ReadOnly), "opaque PNG can be inspected");
        const auto header = file.read(26);
        require(header.size() == 26 && static_cast<uchar>(header.at(25)) == 2,
                "verified opaque source omits the redundant PNG alpha channel");
    }
}

void cancellationDuringEncodingRetainsCompletedPrefix() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "mid-encode temporary directory exists");
    QImage native = fixture()
                        .scaled(257, 257, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    native.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = 2;
    const auto layout = calculateImageSplitLayout(native.size(), settings);
    qint64 completed = 0;
    int secondOutputPolls = 0;
    const auto result = exportSplitImages(
        native, layout, temporary.path(), QStringLiteral("mid_encode"),
        [&] { return completed == 1 && ++secondOutputPolls >= 64; },
        [&](qint64 count, qint64) { completed = count; });
    require(!result.succeeded() && result.savedCount == 1 && secondOutputPolls >= 64 &&
                result.errorSource == QStringLiteral("Export was canceled") &&
                QDir(result.directory).entryList(QDir::Files | QDir::Hidden) ==
                    QStringList{QStringLiteral("1.png")},
            "cancellation inside the second encoder preserves only its completed prefix");
    const QImage decoded = snow_shot::image_codec::decodeFile(
        QDir(result.directory).filePath(QStringLiteral("1.png")), snow::image::Format::png);
    require(decoded == normalizedForExport(native).copy(layout.regionAt(0)),
            "the PNG saved before mid-encode cancellation preserves normalized pixels");
}

void failuresRetranslate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "language-switch temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto canceled = exportSplitImages(image, layout, temporary.path(),
                                            QStringLiteral("language"), [] { return true; });
    require(canceled.errorSource == QStringLiteral("Export was canceled") &&
                canceled.errorArguments.isEmpty() && canceled.errorMessage() == canceled.error,
            "helper failures retain their original translation key and initial display message");
    ImageSplitExportResult indexed;
    indexed.errorSource = QStringLiteral("Image %1 could not be saved: %2");
    indexed.errorArguments = {QStringLiteral("7"), QStringLiteral("native detail")};
    indexed.error = indexed.errorMessage();
    for (const auto* catalog : {":/i18n/snow_shot_zh_CN.qm", ":/i18n/snow_shot_zh_TW.qm"}) {
        QTranslator translator;
        require(translator.load(QString::fromLatin1(catalog)),
                "export failures require complete Chinese translation catalogs");
        QCoreApplication::installTranslator(&translator);
        const QString expected =
            QCoreApplication::translate("ImageSplitExport", "Export was canceled");
        require(canceled.errorMessage() == expected && canceled.errorMessage() != canceled.error,
                "cached export failures retranslate when the runtime language changes");
        require(
            indexed.errorMessage() ==
                QCoreApplication::translate("ImageSplitExport", "Image %1 could not be saved: %2")
                    .arg(QStringLiteral("7"), QStringLiteral("native detail")),
            "runtime retranslation preserves ordinal and native error-detail arguments");
        QCoreApplication::removeTranslator(&translator);
    }
    require(canceled.errorMessage() == canceled.error && indexed.errorMessage() == indexed.error,
            "returning to English restores the initial error text");
    const ImageSplitExportResult external{{}, QStringLiteral("external failure"), 0, 1};
    require(external.errorMessage() == external.error,
            "existing four-field aggregate service results retain their error text");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        orderingAndUniqueDirectories();
        tiffImportsSplitWithoutLosingPixels();
        exportNamesPreserveSplitPolicy();
        std::cout << "Image split filename policy tests passed\n";
        failureAndCancellation();
        concurrentDirectoryCreationAndNormalization();
        boundedNativeFormatAndColorConversion();
        croppedNativeRowsRespectAllocationBoundary();
        largeProfileConversionMatchesFullNormalization();
        structurallyOpaqueSourcesWriteRgbPng();
        cancellationDuringEncodingRetainsCompletedPrefix();
        failuresRetranslate();
        std::cout << "Image split export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
