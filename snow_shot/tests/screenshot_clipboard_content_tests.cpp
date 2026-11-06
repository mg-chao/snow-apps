#include "snow_shot/app/supportedpinfiles.h"
#include "snow_shot/presentation/screenshotcontentdrop.h"
#include <QTextTable>
#include <QFontDatabase>
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "../src/presentation/services/screenshotclipboardcontentsnapshot.h"
#include "snowimageqtcodec.h"
#include "snowimagecodecbridge.h"
#include "pinned_animation_fixtures.h"
#include "pinned_animation_native_fixtures.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/image/supportedimagefiles.h"
#include "bmp_import_fixtures.h"
#include "../../test-support/virtualmemory.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QImage>
#include <QFile>
#include <QMimeData>
#include <QPalette>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextFrame>
#include <QThread>
#include <QUrl>
#include <QtEndian>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void imageFileExtensionsMatchShellRegistration() {
    const auto images = snow_shot::image::supportedImageFileExtensions();
    require(ScreenshotClipboardContentReader::supportedImageFileExtensions() == images,
            "image reader and shell registration share the authoritative image suffixes");
    auto allFiles = images;
    allFiles.append({QStringLiteral("txt"), QStringLiteral("html"), QStringLiteral("htm"),
                     QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("css")});
    require(ScreenshotClipboardContentReader::supportedFileExtensions() == allFiles,
            "existing file pinning retains its additional formatted-text formats");
}

void clipboardImagesTakePriorityOverAccompanyingFiles() {
    QTemporaryDir directory;
    require(directory.isValid(), "mixed clipboard fixtures need temporary storage");
    QImage pixels(3, 2, QImage::Format_RGBA8888);
    pixels.fill(QColor(23, 117, 209, 127));
    pixels.setColorSpace(QColorSpace::SRgb);
    QImage filePixels(pixels.size(), pixels.format());
    filePixels.fill(Qt::red);
    const QString imagePath = directory.filePath(QStringLiteral("export.png"));
    const QString textPath = directory.filePath(QStringLiteral("document.txt"));
    require(filePixels.save(imagePath, "PNG"), "mixed clipboard file must be saved");
    const QStringList paths{imagePath, textPath, imagePath};
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(imagePath), QUrl::fromLocalFile(textPath),
                  QUrl::fromLocalFile(imagePath)});
    mime.setText(imagePath);
    require(ScreenshotClipboardContentReader::filePathsForImport(&mime) == paths &&
                ScreenshotClipboardContentReader::localImageFilePaths(&mime) ==
                    QStringList{imagePath, imagePath},
            "file-only imports preserve order and duplicates independently of accompanying text");

    ScreenshotClipboardAppearance appearance;
    appearance.rasterSize = pixels.size();
    appearance.checkerboardEnabled = true;
    appearance.showBorder = false;
    setScreenshotClipboardAppearance(mime, appearance);
    for (bool encoded : {false, true}) {
        if (encoded) {
            mime.removeFormat(QStringLiteral("application/x-qt-image"));
            mime.setData(QStringLiteral("image/png"), snow_shot::image_codec::encodePng(pixels));
        } else {
            mime.setImageData(pixels);
        }
        require(ScreenshotClipboardContentReader::filePathsForImport(&mime).isEmpty() &&
                    ScreenshotClipboardContentReader::localImageFilePaths(&mime).isEmpty() &&
                    ScreenshotClipboardContentReader::localFilePaths(&mime) == paths,
                "clipboard imports must prefer inline images while retaining the exported URLs");
        const auto decoded = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(decoded &&
                    decoded->image.convertToFormat(QImage::Format_ARGB32_Premultiplied) ==
                        pixels.convertToFormat(QImage::Format_ARGB32_Premultiplied) &&
                    decoded->appearance && decoded->appearance->checkerboardEnabled &&
                    decoded->appearance->showBorder == false &&
                    decoded->originalContent.localFilePath.isEmpty(),
                "mixed clipboard imports retain canonical image pixels and image-bound appearance");
    }
    require(ScreenshotClipboardContentReader::filePathsForImport(nullptr).isEmpty(),
            "missing clipboard MIME data must not offer file imports");
}

void imageFilesDecodeByContent() {
    QTemporaryDir directory;
    require(directory.isValid(), "content detection fixture directory must exist");
    QImage source(32, 24, QImage::Format_RGB888);
    source.fill(Qt::green);
    source.setPixelColor(0, 0, Qt::red);
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    for (const auto* encoding : {"JPEG", "PNG"}) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        require(buffer.open(QIODevice::WriteOnly) && source.save(&buffer, encoding),
                "encode content detection fixture");
        const auto format =
            QByteArray(encoding) == "JPEG" ? snow::image::Format::jpeg : snow::image::Format::png;
        const auto reference = snow_shot::image_codec::decode(bytes, format, nullptr);
        require(!reference.isNull(), "correctly identified reference must decode");
        const auto wrongFormat = format == snow::image::Format::jpeg ? snow::image::Format::png
                                                                     : snow::image::Format::jpeg;
        require(snow_shot::image_codec::decode(bytes, wrongFormat, nullptr).isNull() &&
                    snow_shot::image_codec::inspectSize(bytes, wrongFormat).isEmpty(),
                "explicit format constraints must continue to reject mismatched payloads");
        require(snow_shot::image_codec::inspectSize(bytes, snow::image::Format::unknown) ==
                    reference.size(),
                "automatic inspection must report the encoded image size");
        const auto automatic =
            snow_shot::image_codec::decode(bytes, snow::image::Format::unknown, nullptr);
        require(automatic == reference, "automatic packed decoding must preserve pixels");
        for (const auto& suffix :
             ScreenshotClipboardContentReader::supportedImageFileExtensions()) {
            const auto path = directory.filePath(QStringLiteral("image.") + suffix);
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                    "write image with a potentially misleading extension");
            file.close();
            const auto bgra =
                snow_shot::image_codec::decodeFileBgra(path, snow::image::Format::unknown);
            require(bgra.convertToFormat(reference.format()) == reference &&
                        bgra.colorSpace() == reference.colorSpace(),
                    "automatic BGRA file decoding must preserve pixels and profiles");
            const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
            require(files.size() == 1, "supported image suffix must reach content detection");
            ScreenshotClipboardContentSnapshot snapshot;
            snapshot.localImage = files.first();
            for (const bool budgeted : {false, true}) {
                const auto content = ScreenshotClipboardContentReader::decode(
                    snapshot, {},
                    budgeted ? ScreenshotClipboardContentReader::AllocationCheck(
                                   [](qint64) { return true; })
                             : ScreenshotClipboardContentReader::AllocationCheck{});
                require(content && content->image == reference &&
                            content->image.colorSpace() == reference.colorSpace() &&
                            content->sourceIdentity == files.first().sourceIdentity &&
                            content->originalContent.localFilePath == path,
                        "file content determines decoding and admission regardless of suffix");
            }
            const auto preview = ScreenshotClipboardContentReader::decodeImageFile(files.first());
            require(preview == reference && preview.colorSpace() == reference.colorSpace(),
                    "first-frame import must also detect the encoded format");
            require(!ScreenshotClipboardContentReader::decode(snapshot, {},
                                                              [](qint64) { return false; }) &&
                        ScreenshotClipboardContentReader::decodeImageFile(files.first(),
                                                                          [] { return true; })
                            .isNull(),
                    "content detection must preserve admission and cancellation checks");
            require(
                !ScreenshotClipboardContentReader::decode(
                    snapshot, {}, [&](qint64 requested) { return requested <= bytes.size() + 1; }),
                "mislabeled files must pass decoded-size admission before raster allocation");
        }
    }
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        const auto reference = snow_shot::image_codec::decodeAnimationSource(fixture.bytes);
        require(reference && reference->isAnimated(), "animation fixture must decode");
        const auto path = directory.filePath(QStringLiteral("animation.jpg"));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "write animation with a still-image suffix");
        file.close();
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "snapshot mislabeled animation");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        const auto content =
            ScreenshotClipboardContentReader::decode(snapshot, {}, [](qint64) { return true; });
        require(content && content->animationSource &&
                    content->animationSource->encoded == fixture.bytes &&
                    content->image == reference->firstFrame &&
                    ScreenshotClipboardContentReader::decodeImageFile(files.first()) ==
                        reference->firstFrame,
                "mislabeled animations must retain playback and composited preview semantics");
    }
}

void tiffFilesAndMimeDataPreservePixels() {
    QTemporaryDir directory;
    require(directory.isValid(), "TIFF imports need temporary storage");
    QImage pixels(3, 2, QImage::Format_RGBA8888);
    pixels.fill(QColor(23, 117, 209, 127));
    pixels.setPixelColor(0, 0, QColor(87, 42, 19, 0));
    pixels.setPixelColor(2, 1, QColor(9, 33, 71, 255));
    pixels.setColorSpace(QColorSpace::SRgb);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    snow::image::EncodeOptions options;
    options.format = snow::image::Format::tiff;
    QString error;
    require(buffer.open(QIODevice::WriteOnly) &&
                snow_shot::image_codec::encodeToDevice(pixels, &buffer, options.format, options,
                                                       &error),
            "encode a TIFF import fixture with straight alpha");

    QMimeData mime;
    mime.setData(QStringLiteral("image/tiff"), bytes);
    const auto decoded = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(decoded && decoded->image.convertToFormat(pixels.format()) == pixels &&
                !decoded->animationSource &&
                ScreenshotClipboardContentReader::filePathsForImport(&mime).isEmpty(),
            "TIFF MIME data must import as one still image with exact RGBA pixels");
    for (const QString& suffix :
         {QStringLiteral("TIF"), QStringLiteral("tiff"), QStringLiteral("png")}) {
        const QString path = directory.filePath(QStringLiteral("scan.") + suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                "write TIFF with an accepted or misleading suffix");
        file.close();
        require(snow_shot::image_codec::decodeFileBgra(path, snow::image::Format::unknown)
                        .convertToFormat(pixels.format()) == pixels,
                "TIFF file decoding must support the app's BGRA import path");
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "both TIFF suffixes must reach file import");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        const auto imported = ScreenshotClipboardContentReader::decode(snapshot);
        require(imported && imported->image.convertToFormat(pixels.format()) == pixels &&
                    !imported->animationSource && imported->originalContent.localFilePath == path,
                "TIFF file imports must detect content and retain source identity");
        require(ScreenshotClipboardContentReader::decodeImageFile(files.first())
                            .convertToFormat(pixels.format()) == pixels &&
                    ScreenshotClipboardContentReader::decodeImageFile(files.first(),
                                                                      [] { return true; })
                        .isNull(),
                "first-page TIFF previews must preserve pixels and cancellation");
    }
    mime.setData(QStringLiteral("image/tiff"), QByteArrayLiteral("truncated TIFF"));
    mime.setText(QStringLiteral("text fallback"));
    const auto fallback = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(fallback && fallback->isFormattedText() &&
                fallback->plainText == QStringLiteral("text fallback"),
            "an invalid TIFF offer must allow the existing text fallback");
}

void bmpFilesPreservePixelsAndIdentity() {
    QTemporaryDir directory;
    require(directory.isValid(), "BMP import fixture directory must exist");
    QImage opaque(3, 2, QImage::Format_RGB888);
    opaque.fill(Qt::green);
    opaque.setPixelColor(0, 0, Qt::red);
    opaque.setPixelColor(2, 1, Qt::blue);
    QImage alpha = opaque.convertToFormat(QImage::Format_RGBA8888);
    alpha.setPixelColor(1, 0, QColor(10, 20, 30, 40));
    alpha.setPixelColor(0, 1, QColor(90, 80, 70, 0));
    const QStringList paths{directory.filePath(QStringLiteral("opaque.bmp")),
                            directory.filePath(QStringLiteral("透明 image.BMP"))};
    require(opaque.save(paths[0], "BMP") && bmp_import_fixtures::write(paths[1], alpha),
            "write 24-bit opaque and 32-bit alpha BMP fixtures");
    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles(paths);
    require(files.size() == 2 && files[0].suffix == QStringLiteral("bmp") &&
                files[1].suffix == QStringLiteral("bmp"),
            "BMP snapshots accept lowercase and uppercase suffixes in source order");
    for (qsizetype index = 0; index < paths.size(); ++index) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(paths[index])});
        const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && !content->isFormattedText() && !content->animationSource &&
                    bmp_import_fixtures::samePixels(content->image, index == 0 ? opaque : alpha) &&
                    content->originalContent.localFilePath == paths[index] &&
                    content->sourceIdentity == files[index].sourceIdentity,
                "BMP file imports preserve dimensions, RGBA pixels and original file identity");
    }
    const auto corrupt = directory.filePath(QStringLiteral("corrupt.bmp"));
    QFile file(corrupt);
    require(file.open(QIODevice::WriteOnly) && file.write("not a bitmap") > 0,
            "write malformed BMP fixture");
    file.close();
    const auto corruptFiles = ScreenshotClipboardContentReader::snapshotLocalFiles({corrupt});
    require(corruptFiles.size() == 1, "malformed BMP fixture must reach the decoder");
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.localImage = corruptFiles.first();
    snapshot.devicePixelRatio = 1;
    require(!ScreenshotClipboardContentReader::decode(std::move(snapshot)),
            "malformed BMP files must fail decoding");
}

void animatedSourcesSurviveImport() {
    QTemporaryDir directory;
    require(directory.isValid(), "animated image fixture directory must exist");
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        QMimeData mime;
        mime.setData(fixture.mimeType, fixture.bytes);
        const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->animationSource && content->animationSource->isAnimated() &&
                    content->animationSource->encoded == fixture.bytes &&
                    content->image == content->animationSource->firstFrame &&
                    content->preparedDisplayImage.constBits() ==
                        content->animationSource->firstDisplayFrame.constBits() &&
                    content->preparedImageTransform.isIdentity() &&
                    content->image.size() == QSize(2, 2),
                "encoded animation import must retain exact bytes and its initial frame");

        const auto path = directory.filePath(QStringLiteral("source.") + fixture.suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "animated file fixture must be written");
        file.close();
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "GIF, WebP and APNG extensions must be admitted");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        const auto decoded = ScreenshotClipboardContentReader::decode(snapshot);
        require(decoded && decoded->animationSource &&
                    decoded->animationSource->encoded == fixture.bytes &&
                    decoded->originalContent.localFilePath == path &&
                    decoded->sourceIdentity == files.first().sourceIdentity,
                "file animation import must retain source bytes and original file identity");
        require(file.open(QIODevice::Append) && file.write("changed") == 7,
                "an admitted animation's original file may change afterward");
        file.close();
        require(decoded->animationSource->encoded == fixture.bytes &&
                    !ScreenshotClipboardContentReader::decode(snapshot),
                "source changes preserve imported bytes but invalidate an older file snapshot");
        require(file.remove() && decoded->animationSource->encoded == fixture.bytes,
                "removing an imported file must not invalidate retained animation bytes");
        require(!ScreenshotClipboardContentReader::decode(snapshot),
                "deleting an unsnapshotted file source must reject its later decode");
        ScreenshotClipboardContentSnapshot encodedSnapshot;
        encodedSnapshot.encodedImages.append({fixture.bytes, fixture.mimeType});
        require(!ScreenshotClipboardContentReader::decode(encodedSnapshot, [] { return true; }),
                "animated image import must honor cancellation");
        require(!ScreenshotClipboardContentReader::decode(encodedSnapshot, {},
                                                          [](qint64) { return false; }),
                "animated image import must honor allocation admission");
    }
}

QByteArray profiledApng(const QColorSpace& profile) {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    image.setColorSpace(profile);
    QByteArray png;
    QBuffer buffer(&png);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "encode replacement profile fixture");
    QByteArray metadata;
    for (qsizetype position = 8; position <= png.size() - 12;) {
        const auto length =
            static_cast<qsizetype>(qFromBigEndian<quint32>(png.constData() + position));
        require(length <= png.size() - position - 12, "complete profile metadata chunk");
        const auto type = png.mid(position + 4, 4);
        if (type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" || type == "cICP")
            metadata.append(png.mid(position, length + 12));
        position += length + 12;
    }
    require(!metadata.isEmpty(), "replacement fixture carries an encoded color profile");
    auto result = pinned_animation_fixtures::images().back().bytes;
    result.insert(33, metadata);
    return result;
}

void firstFrameFilesKeepCompositionProfilesAndRevisionChecks() {
    QTemporaryDir directory;
    require(directory.isValid(), "first-frame fixture directory must exist");
    auto fixtures = pinned_animation_native_fixtures::images();
    fixtures.append(pinned_animation_fixtures::hiddenPosterAnimation());
    fixtures.append(pinned_animation_fixtures::singleFrameImages());
    fixtures.append({QStringLiteral("apng"), QStringLiteral("image/apng"),
                     profiledApng(QColorSpace(QColorSpace::DisplayP3))});
    for (qsizetype index = 0; index < fixtures.size(); ++index) {
        const auto& fixture = fixtures[index];
        const auto path =
            directory.filePath(QStringLiteral("first-%1.%2").arg(index).arg(fixture.suffix));
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "write first-frame file fixture");
        file.close();
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "snapshot first-frame file fixture");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        const auto complete = ScreenshotClipboardContentReader::decode(snapshot);
        const auto budgeted =
            ScreenshotClipboardContentReader::decode(snapshot, {}, [](qint64) { return true; });
        const auto image = ScreenshotClipboardContentReader::decodeImageFile(
            files.first(), {}, [](qint64) { return true; });
        require(
            complete && budgeted && budgeted->image == complete->image &&
                image == complete->image && image.colorSpace() == complete->image.colorSpace(),
            "budgeted full/first-frame files preserve animated canvas composition and profiles");
        require(
            ScreenshotClipboardContentReader::decodeImageFile(files.first(), [] { return true; })
                .isNull(),
            "first-frame file decode honors cancellation before reading");
        int calls = 0;
        require(ScreenshotClipboardContentReader::decodeImageFile(
                    files.first(), {},
                    [&](qint64) {
                        ++calls;
                        return false;
                    }).isNull() &&
                    calls == 1,
                "first-frame file decoding admits encoded bytes before allocation");
        require(file.open(QIODevice::Append) && file.write("changed") == 7,
                "mutate a captured first-frame source");
        file.close();
        require(ScreenshotClipboardContentReader::decodeImageFile(files.first()).isNull() &&
                    !image.isNull(),
                "changed file revisions fail while previously decoded first frames remain valid");
    }
    const auto malformed = directory.filePath(QStringLiteral("malformed.apng"));
    QFile file(malformed);
    require(file.open(QIODevice::WriteOnly) && file.write("not an image") > 0,
            "write malformed first-frame fixture");
    file.close();
    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({malformed});
    require(files.size() == 1 &&
                ScreenshotClipboardContentReader::decodeImageFile(files.first()).isNull(),
            "first-frame-only decoding rejects malformed image files");
}

void animationReplacementPreparationIsAtomicAndWorkerSafe() {
    auto fixtures = pinned_animation_fixtures::images();
    for (const auto& profile :
         {QColorSpace(QColorSpace::SRgb), QColorSpace(QColorSpace::DisplayP3)})
        fixtures.append(
            {QStringLiteral("apng"), QStringLiteral("image/apng"), profiledApng(profile)});
    for (const auto& fixture : fixtures) {
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.encodedImages.append({fixture.bytes, fixture.mimeType});
        auto content = ScreenshotClipboardContentReader::decode(std::move(snapshot));
        require(content && content->animationSource, "decode preparation fixture");
        const auto rawOwner = content->image.constBits();
        const auto rawProfile = content->image.colorSpace();
        const auto& display = content->animationSource->firstDisplayFrame;
        const qint64 retainedBytes = fixture.bytes.size() + content->image.sizeInBytes() +
                                     (display.constBits() == rawOwner ? 0 : display.sizeInBytes());
        qint64 admitted = 0;
        require(ScreenshotClipboardContentReader::prepareAnimationDisplay(*content, {}, {},
                                                                          [&](qint64 bytes) {
                                                                              admitted = bytes;
                                                                              return true;
                                                                          }) &&
                    content->preparedDisplayImage.constBits() ==
                        content->animationSource->firstDisplayFrame.constBits() &&
                    admitted == retainedBytes,
                "identity preparation shares its display owner and admits each distinct raster "
                "once");
        QTransform requested;
        requested.translate(123, -77);
        requested.rotate(90);
        requested.scale(-1, 1);
        const QRectF bounds = requested.mapRect(QRectF(QPointF(), QSizeF(content->image.size())));
        QTransform normalized;
        normalized.setMatrix(requested.m11(), requested.m12(), requested.m13(), requested.m21(),
                             requested.m22(), requested.m23(), requested.dx() - bounds.left(),
                             requested.dy() - bounds.top(), requested.m33());
        const QImage expected = snowCanvasTransformImage(
            content->animationSource->firstDisplayFrame, normalized, Qt::SmoothTransformation);
        auto worker = std::async(std::launch::async, [content = *content, requested]() mutable {
            require(ScreenshotClipboardContentReader::prepareAnimationDisplay(content, requested),
                    "replacement transformation must prepare on an import worker");
            return content;
        });
        *content = worker.get();
        require(
            content->preparedDisplayImage == expected &&
                content->preparedImageTransform == normalized &&
                content->image.constBits() == rawOwner &&
                content->image.colorSpace() == rawProfile &&
                content->animationSource->encoded == fixture.bytes,
            "prepared replacement preserves raw profile and bytes with a normalized fingerprint");
        const auto preparedOwner = content->preparedDisplayImage.constBits();
        const auto preparedTransform = content->preparedImageTransform;
        require(!ScreenshotClipboardContentReader::prepareAnimationDisplay(
                    *content, {}, {}, [](qint64) { return false; }) &&
                    content->preparedDisplayImage.constBits() == preparedOwner &&
                    content->preparedImageTransform == preparedTransform,
                "denied preparation leaves the committed replacement pixels unchanged");
        bool cancelled = false;
        require(!ScreenshotClipboardContentReader::prepareAnimationDisplay(
                    *content, {}, [&] { return cancelled; },
                    [&](qint64) {
                        cancelled = true;
                        return true;
                    }) &&
                    content->preparedDisplayImage.constBits() == preparedOwner &&
                    content->preparedImageTransform == preparedTransform,
                "cancellation after admission cannot publish an incomplete replacement");
        QTransform oversized;
        oversized.scale(100000, 100000);
        require(!ScreenshotClipboardContentReader::prepareAnimationDisplay(*content, oversized),
                "transformed animation admission rejects oversized rasters before allocation");
    }
    ScreenshotClipboardContent nativeContent;
    nativeContent.animationSource = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_native_fixtures::images().first().bytes);
    require(nativeContent.animationSource &&
                nativeContent.animationSource->canvasSize == QSize(320, 180),
            "native-sized overflow fixture must decode");
    nativeContent.image = nativeContent.animationSource->firstFrame;
    nativeContent.preparedDisplayImage = nativeContent.animationSource->firstDisplayFrame;
    const auto nativeDisplayOwner = nativeContent.preparedDisplayImage.constBits();
    QTransform overflowing;
    overflowing.scale(1e9, 1e9);
    int allocationAttempts = 0;
    require(!ScreenshotClipboardContentReader::prepareAnimationDisplay(nativeContent, overflowing,
                                                                       {},
                                                                       [&](qint64) {
                                                                           ++allocationAttempts;
                                                                           return true;
                                                                       }) &&
                allocationAttempts == 0 &&
                nativeContent.preparedDisplayImage.constBits() == nativeDisplayOwner &&
                nativeContent.preparedImageTransform.isIdentity(),
            "native-sized transforms exceeding Qt integer bounds must reject before raster "
            "preparation");
    QTransform largeTranslation;
    largeTranslation.translate(static_cast<qreal>(std::numeric_limits<int>::min()),
                               static_cast<qreal>(std::numeric_limits<int>::max()));
    require(ScreenshotClipboardContentReader::prepareAnimationDisplay(nativeContent,
                                                                      largeTranslation) &&
                nativeContent.preparedDisplayImage.constBits() == nativeDisplayOwner &&
                nativeContent.preparedImageTransform.isIdentity(),
            "preparation normalizes large translations before Qt integer raster bounds are used");
    ScreenshotClipboardContent staticContent;
    staticContent.image = QImage(QSize(2, 3), QImage::Format_ARGB32_Premultiplied);
    staticContent.image.fill(Qt::blue);
    const auto staticOwner = staticContent.image.constBits();
    require(ScreenshotClipboardContentReader::prepareAnimationDisplay(
                staticContent, QTransform(-1, 0, 0, 1, 0, 0)) &&
                staticContent.image.constBits() == staticOwner &&
                staticContent.preparedDisplayImage.isNull(),
            "static replacements retain their existing preparation path");
}

void animationOffersPrecedeStaticThumbnails() {
    QImage thumbnail(3, 3, QImage::Format_ARGB32);
    thumbnail.fill(Qt::blue);
    QByteArray png;
    QBuffer buffer(&png);
    require(buffer.open(QIODevice::WriteOnly) && thumbnail.save(&buffer, "PNG"),
            "static clipboard thumbnail must encode");
    QByteArray jpeg;
    QBuffer jpegBuffer(&jpeg);
    require(jpegBuffer.open(QIODevice::WriteOnly) && thumbnail.save(&jpegBuffer, "JPEG"),
            "a higher-priority static JPEG clipboard offer must encode");
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        QMimeData mime;
        mime.setData(QStringLiteral("image/png"), png);
        mime.setData(fixture.mimeType, fixture.bytes);
        mime.setData(QStringLiteral("image/jpeg"), jpeg);
        mime.setImageData(thumbnail);
        const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->animationSource &&
                    content->animationSource->encoded == fixture.bytes &&
                    content->image.size() == QSize(2, 2),
                "an original animation must precede a provider's static thumbnail");
        for (bool animatedFirst : {false, true}) {
            ScreenshotClipboardContentSnapshot snapshot;
            const ScreenshotClipboardEncodedImage staticOffer{png, QStringLiteral("image/png")};
            const ScreenshotClipboardEncodedImage animatedOffer{fixture.bytes, fixture.mimeType};
            snapshot.encodedImages = animatedFirst ? QList{animatedOffer, staticOffer}
                                                   : QList{staticOffer, animatedOffer};
            const auto decoded = ScreenshotClipboardContentReader::decode(std::move(snapshot));
            require(decoded && decoded->animationSource &&
                        decoded->animationSource->encoded == fixture.bytes,
                    "mixed encoded snapshot order cannot flatten an offered animation");
        }
    }
    QMimeData mime;
    mime.setData(QStringLiteral("image/png"), png);
    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content && !content->animationSource && content->image.size() == thumbnail.size(),
            "static PNGs must keep normal static-image capabilities");
}

void singleFrameAnimationContainersStayStatic() {
    QTemporaryDir directory;
    require(directory.isValid(), "single-frame image fixture directory must exist");
    for (const auto& fixture : pinned_animation_fixtures::singleFrameImages()) {
        const auto verify = [&](const std::optional<ScreenshotClipboardContent>& content) {
            require(content && !content->animationSource && content->image.size() == QSize(2, 2) &&
                        content->image.pixelColor(0, 0) == QColor(Qt::red),
                    "one-frame GIF and APNG remain static pins using their actual frame");
            if (fixture.suffix == QStringLiteral("apng"))
                require(content->image.pixelColor(1, 1).alpha() == 0,
                        "single-frame APNG import excludes its separate blue poster");
        };
        QMimeData mime;
        mime.setData(fixture.mimeType, fixture.bytes);
        verify(ScreenshotClipboardContentReader::readMimeData(&mime, 1.0));
        const auto path = directory.filePath(QStringLiteral("single.") + fixture.suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) &&
                    file.write(fixture.bytes) == fixture.bytes.size(),
                "single-frame image file fixture must be written");
        file.close();
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "single-frame GIF and APNG file containers must be admitted");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        verify(ScreenshotClipboardContentReader::decode(std::move(snapshot)));
    }
}

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly), "PNG buffer should open");
    require(image.save(&buffer, "PNG"), "PNG image should encode");
    return bytes;
}

bool imageContainsColor(const QImage& image, const QColor& expected) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == expected) {
                return true;
            }
        }
    }
    return false;
}

void embeddedImagesUseManagedCachedPixels() {
    QImage source(QSize(1025, 513), QImage::Format_ARGB32);
    std::uint32_t random = 0x12345678U;
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            row[x] = random;
        }
    }
    source.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QByteArray png = pngBytes(source);
    const QImage expected = QImage::fromData(png, "PNG");
    const QByteArray base64 = png.toBase64();
    require(base64.size() > 2 * 1024 * 1024,
            "the default embedded-image fixture must exceed the automation-only URL limit");
    const std::array urls{
        QByteArray("data:image/png;base64,") + base64,
        QByteArray("data:image/png;%42ASE64%20,") +
            base64.toPercentEncoding(QByteArray{}, QByteArray("+/=")),
        QByteArray("data:image/png,") + png.toPercentEncoding(),
    };
    for (const QByteArray& encodedUrl : urls) {
        const QUrl url = QUrl::fromEncoded(encodedUrl);
        QMimeData mime;
        mime.setHtml(QStringLiteral("before <img width=\"17\" height=\"11\" src=\"%1\"> after")
                         .arg(QString::fromLatin1(encodedUrl)));
        auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->isFormattedText(),
                "default clipboard data URLs must decode without automation-only limits");
        QImage retained =
            content->formattedDocument->resource(QTextDocument::ImageResource, url).value<QImage>();
        require(retained == expected && retained.colorSpace() == expected.colorSpace(),
                "embedded images must retain exact pixels and source color metadata");
        {
            const QImage again =
                content->formattedDocument->resource(QTextDocument::ImageResource, url)
                    .value<QImage>();
            require(again.constBits() == retained.constBits(),
                    "document resources must share cached pixels instead of decoding again");
        }
        const void* midpoint = retained.constBits() + retained.sizeInBytes() / 2;
        content.reset();
        require(snow::test_support::virtualMemoryMapped(midpoint),
                "a retained resource image must outlive its formatted document");
        retained = {};
        require(!snow::test_support::virtualMemoryMapped(midpoint),
                "embedded pixels must unmap after their document and image owners are dropped");
    }
}

void localTextFilesAreSupported() {
    QTemporaryDir directory;
    require(directory.isValid(), "text file directory exists");
    for (const auto& suffix :
         {QStringLiteral("TXT"), QStringLiteral("html"), QStringLiteral("htm"),
          QStringLiteral("MD"), QStringLiteral("markdown"), QStringLiteral("CSS")}) {
        const auto format = snow_shot::app::pinDocumentFileFormat(suffix);
        const bool html = format == snow_shot::app::PinDocumentFormat::Html;
        const bool markdown = format == snow_shot::app::PinDocumentFormat::Markdown;
        const QString source = html       ? QStringLiteral("<b>Unicode \u4e2d\u6587</b>")
                               : markdown ? QStringLiteral("**Unicode \u4e2d\u6587**")
                                          : QStringLiteral("Unicode \u4e2d\u6587");
        const QString path = directory.filePath(QStringLiteral("content.") + suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "text file opens");
        file.write(source.toUtf8());
        file.close();
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
        require(files.size() == 1, "text and HTML extensions are admitted case-insensitively");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = files.first();
        snapshot.devicePixelRatio = 2.0;
        snapshot.baseColor = Qt::white;
        auto decoded = ScreenshotClipboardContentReader::decode(snapshot);
        require(decoded && decoded->isFormattedText() &&
                    decoded->originalContent.textFormat ==
                        (markdown ? snow_shot::storage::PinnedTextFormat::Markdown
                                  : snow_shot::storage::PinnedTextFormat::PlainText) &&
                    decoded->plainText == QStringLiteral("Unicode \u4e2d\u6587") &&
                    decoded->formattedTextDevicePixelRatio == 2.0 &&
                    decoded->sourceIdentity == files.first().sourceIdentity &&
                    decoded->originalContent.localFilePath.isEmpty() &&
                    (html ? decoded->originalContent.html : decoded->originalContent.text) ==
                        source,
                "local text renders at requested density with original source and file identity");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        require(acceptsScreenshotDrop(&mime) && screenshotDropFilePaths(&mime) == QStringList{path},
                "all document suffixes are admitted for file drops");
        const auto clipboard = ScreenshotClipboardContentReader::readMimeData(&mime, 2.0);
        require(clipboard && clipboard->plainText == decoded->plainText &&
                    clipboard->originalContent.textFormat == decoded->originalContent.textFormat,
                "clipboard file URLs use the same document decoder");
        require(ScreenshotClipboardContentReader::localImageFilePaths(&mime).isEmpty() &&
                    ScreenshotClipboardContentReader::decodeImageFile(files.first()).isNull(),
                "image-only imports must continue rejecting document files");
        require(!ScreenshotClipboardContentReader::decode(snapshot, [] { return true; }),
                "text file decode honors cancellation");
        require(
            !ScreenshotClipboardContentReader::decode(snapshot, {}, [](qint64) { return false; }),
            "text file decode honors allocation admission");
        require(file.open(QIODevice::Append), "text file reopens");
        file.write("changed");
        file.close();
        require(!ScreenshotClipboardContentReader::decode(snapshot),
                "text file mutation after snapshot is rejected");
    }
    for (const auto& format : snow_shot::app::kPinDocumentFileFormats) {
        for (const bool utf16 : {false, true}) {
            const auto path =
                directory.filePath(QStringLiteral("bom.") + QLatin1String(format.suffix));
            QFile file(path);
            require(file.open(QIODevice::WriteOnly), "BOM file opens");
            file.write(utf16 ? QByteArray::fromHex("fffe4800690020002d4e8765")
                             : QByteArray::fromHex("efbbbf") +
                                   QStringLiteral("Hi \u4e2d\u6587").toUtf8());
            file.close();
            ScreenshotClipboardContentSnapshot snapshot;
            snapshot.localImage =
                ScreenshotClipboardContentReader::snapshotLocalFiles({path}).first();
            const auto decoded = ScreenshotClipboardContentReader::decode(snapshot);
            require(decoded && decoded->plainText == QStringLiteral("Hi \u4e2d\u6587"),
                    "BOM-marked UTF-8 and UTF-16 decode for every document format");
        }
    }
}

void documentFilesPreserveLiteralAndRenderedContent() {
    using snow_shot::storage::PinnedTextFormat;
    QTemporaryDir directory;
    const auto imagePath = directory.filePath(QStringLiteral("external.png"));
    QImage externalImage(8, 8, QImage::Format_RGB32);
    externalImage.fill(Qt::red);
    require(externalImage.save(imagePath), "write readable external image fixture");
    const QString externalUrl = QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded);
    const QString markdown =
        QStringLiteral("# Heading\n\nA **bold** paragraph with [link](https://example.com).\n\n"
                       "- item\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n"
                       "```css\n  body { color: red; }\n  <b>literal</b>\n```\n\n"
                       "![blocked](%1)\n")
            .arg(externalUrl);
    snow_shot::storage::PinnedTextStyle style;
    style.fontFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    style.maximumWidth = 320;
    style.fontSize = 20;
    style.textColor = QColor(230, 220, 210);
    style.backgroundColor = QColor(20, 30, 40);
    for (const auto& suffix : {QStringLiteral("txt"), QStringLiteral("css"), QStringLiteral("md"),
                               QStringLiteral("markdown")}) {
        const auto path = directory.filePath(QStringLiteral("document.") + suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(markdown.toUtf8()) > 0,
                "write document fixture");
        file.close();
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = ScreenshotClipboardContentReader::snapshotLocalFiles({path}).first();
        snapshot.textStyle = style;
        snapshot.devicePixelRatio = 1.5;
        const auto content = ScreenshotClipboardContentReader::decode(snapshot);
        const bool formatted =
            suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown");
        require(content && content->originalContent.text == markdown &&
                    content->originalContent.textStyle == style && content->image.width() <= 480 &&
                    qFuzzyCompare(content->image.devicePixelRatio(), 1.5),
                "documents retain exact source, captured style and fractional DPI");
        if (formatted) {
            auto* document = content->formattedDocument.get();
            bool heading = false, bold = false, list = false, table = false, code = false;
            bool link = false;
            for (auto block = document->begin(); block.isValid(); block = block.next()) {
                heading = heading || block.blockFormat().headingLevel() == 1;
                list = list || block.textList() != nullptr;
                table = table || QTextCursor(block).currentTable() != nullptr;
                if (block.text().contains(QStringLiteral("body {"))) {
                    const auto characters = block.begin().fragment().charFormat();
                    code = characters.fontFamilies().toStringList().contains(
                               QFontDatabase::systemFont(QFontDatabase::FixedFont).family()) &&
                           block.blockFormat().background().style() != Qt::NoBrush;
                }
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    if (it.fragment().text() == QStringLiteral("bold"))
                        bold = it.fragment().charFormat().fontWeight() >= QFont::Bold;
                    if (it.fragment().text() == QStringLiteral("link")) {
                        const auto format = it.fragment().charFormat();
                        link = format.isAnchor() &&
                               format.anchorHref() == QStringLiteral("https://example.com") &&
                               format.foreground().color() == QColor(105, 177, 255);
                    }
                }
            }
            require(heading && bold && list && table && code && link &&
                        !content->plainText.contains(QStringLiteral("**bold**")) &&
                        content->plainText.contains(QStringLiteral("<b>literal</b>")),
                    "Markdown renders headings, emphasis, lists, tables and literal fenced code");
            for (const auto& url : {externalUrl, QStringLiteral("https://example.com/image.png")})
                require(document->resource(QTextDocument::ImageResource, QUrl(url))
                            .value<QImage>()
                            .isNull(),
                        "Markdown resources cannot read local files or access the network");
        } else {
            require(content->plainText == markdown &&
                        content->originalContent.textFormat == PinnedTextFormat::PlainText,
                    "TXT and CSS preserve markup, indentation and line breaks literally");
        }
        require(QFile::remove(path), "remove the external source after import");
        const auto restored = ScreenshotClipboardContentReader::renderOriginalText(
            content->originalContent, 1.5, Qt::red);
        require(restored && restored->plainText == content->plainText &&
                    restored->image == content->image,
                "restoring captured documents preserves rendering without the external file");
    }
    for (const auto& suffix : {QStringLiteral("txt"), QStringLiteral("css"), QStringLiteral("md"),
                               QStringLiteral("html")}) {
        const auto path = directory.filePath(QStringLiteral("invalid.") + suffix);
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "open empty document");
        file.close();
        require(ScreenshotClipboardContentReader::snapshotLocalFiles({path}).isEmpty(),
                "empty documents retain existing rejection behavior");
        require(file.open(QIODevice::WriteOnly) && file.write(QByteArray(1024 * 1024 + 1, 'x')) > 0,
                "write oversized document");
        file.close();
        require(ScreenshotClipboardContentReader::snapshotLocalFiles({path}).isEmpty(),
                "all document formats retain the one MiB source limit");
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    file.write(QByteArray::fromHex("ff")) == 1,
                "write invalid UTF-8");
        file.close();
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.localImage = ScreenshotClipboardContentReader::snapshotLocalFiles({path}).first();
        require(!ScreenshotClipboardContentReader::decode(snapshot),
                "malformed document encoding is rejected");
    }
}

void directImageSharesImmutableOwner() {
    std::optional<ScreenshotClipboardContent> content;
    const QColor original(200, 100, 50);
    const QColorSpace colorSpace(QColorSpace::DisplayP3);
    const uchar* pixels = nullptr;
    {
        QMimeData mime;
        QImage source(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied);
        source.fill(original);
        source.setColorSpace(colorSpace);
        source.setDotsPerMeterX(4321);
        source.setText(QStringLiteral("Author"), QStringLiteral("clipboard owner"));
        pixels = source.constBits();
        mime.setImageData(source);
        content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->image.constBits() == pixels && content->image == source &&
                    content->image.colorSpace() == colorSpace &&
                    content->image.dotsPerMeterX() == 4321 &&
                    content->image.text(QStringLiteral("Author")) ==
                        QStringLiteral("clipboard owner"),
                "immutable clipboard imports must share the existing owner and metadata");
        source.fill(Qt::blue);
        require(content->image.constBits() == pixels &&
                    content->image.pixelColor(0, 0) == original &&
                    source.pixelColor(0, 0) == QColor(Qt::blue),
                "source mutation must detach without changing the clipboard snapshot");
        QImage writable = content->image;
        require(snowCanvasDetachImage(writable), "clipboard consumers must be able to detach");
        writable.fill(Qt::green);
        require(writable.constBits() != pixels && content->image.pixelColor(0, 0) == original &&
                    mime.imageData().value<QImage>().constBits() == pixels,
                "consumer mutation must preserve the shared clipboard provider and content");
    }
    require(content->image.constBits() == pixels && content->image.pixelColor(0, 0) == original,
            "clipboard content must outlive its source and MIME provider");
}

void directManagedImageKeepsSharedStorage() {
    std::optional<ScreenshotClipboardContent> content;
    const uchar* pixels = nullptr;
    const void* middle = nullptr;
    {
        QMimeData mime;
        QImage source =
            snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied);
        require(!source.isNull(), "managed clipboard fixture must allocate");
        source.fill(Qt::red);
        pixels = source.constBits();
        middle = pixels + source.sizeInBytes() / 2;
        mime.setImageData(source);
        content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->image.constBits() == pixels,
                "managed clipboard images must retain their existing allocation");
    }
    require(snow::test_support::virtualMemoryMapped(middle) &&
                content->image.pixelColor(0, 0) == QColor(Qt::red),
            "the imported content must retain managed pixels after provider destruction");
    content.reset();
    require(!snow::test_support::virtualMemoryMapped(middle),
            "managed clipboard pixels must unmap after their final shared owner drops");
}

void directReadOnlyImageRetainsCleanupOwner() {
    struct Owner {
        QImage image;
        bool* released;
    };
    bool released = false;
    std::optional<ScreenshotClipboardContent> content;
    const uchar* pixels = nullptr;
    const void* middle = nullptr;
    {
        QMimeData mime;
        auto* owner = new Owner{
            snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied),
            &released};
        require(!owner->image.isNull(), "read-only clipboard fixture must allocate");
        owner->image.fill(Qt::red);
        pixels = owner->image.constBits();
        middle = pixels + owner->image.sizeInBytes() / 2;
        QImage source(
            pixels, owner->image.width(), owner->image.height(), owner->image.bytesPerLine(),
            owner->image.format(),
            [](void* raw) {
                auto* retained = static_cast<Owner*>(raw);
                *retained->released = true;
                delete retained;
            },
            owner);
        mime.setImageData(source);
        content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
        require(content && content->image.constBits() == pixels && !released,
                "read-only external clipboard images must share their cleanup owner");
    }
    require(!released && snow::test_support::virtualMemoryMapped(middle),
            "clipboard content must retain a read-only external image's cleanup owner");
    QImage writable = content->image;
    require(snowCanvasDetachImage(writable), "read-only clipboard images must detach for writing");
    writable.fill(Qt::blue);
    require(writable.constBits() != pixels && content->image.pixelColor(0, 0) == QColor(Qt::red) &&
                !released,
            "writing a detached image must preserve the external clipboard snapshot");
    content.reset();
    require(released && !snow::test_support::virtualMemoryMapped(middle),
            "external clipboard cleanup must run after the final original owner drops");
}

void directImageWinsOverRichText() {
    QMimeData mime;
    QImage image(QSize(3, 2), QImage::Format_RGBA8888);
    image.fill(Qt::red);
    mime.setImageData(image);
    mime.setHtml(QStringLiteral("<b>ignored</b>"));

    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content.has_value() && !content->isFormattedText() &&
                content->image.size() == QSize(3, 2),
            "direct clipboard images should have priority over HTML");
}

void imageSourceDensitySurvivesDecode() {
    QImage image(QSize(1025, 513), QImage::Format_RGB32);
    image.fill(Qt::red);
    image.setDevicePixelRatio(2);
    QMimeData mime;
    mime.setImageData(image);
    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content && content->image.size() == image.size(),
            "clipboard decode must retain the raster");
#if defined(Q_OS_MACOS)
    require(content->image.devicePixelRatio() == 2. &&
                content->image.constBits() == image.constBits(),
            "macOS pin imports need the source's logical size");
#else
    require(content->image.devicePixelRatio() == 1. && image.devicePixelRatio() == 2. &&
                content->image.constBits() != image.constBits(),
            "normalizing clipboard density must detach without changing the provider");
#endif
}

void oversizedDirectImagesAreIgnored() {
    QMimeData mime;
    QImage image(QSize(8192, 8192), QImage::Format_Mono);
    require(!image.isNull(), "oversized clipboard fixture should allocate");
    mime.setImageData(image);
    require(!ScreenshotClipboardContentReader::readMimeData(&mime, 1.0).has_value(),
            "oversized direct clipboard images should be ignored");
}

void automationAdmissionPrecedesDecodeAndRasterization() {
    QImage image(QSize(64, 32), QImage::Format_RGB32);
    image.fill(Qt::red);
    const auto encoded = pngBytes(image);
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.encodedImages.append({encoded, QStringLiteral("image/png")});
    qint64 maximum = 0;
    int calls = 0;
    const auto decoded = ScreenshotClipboardContentReader::decode(snapshot, {}, [&](qint64 bytes) {
        maximum = std::max(maximum, bytes);
        ++calls;
        return true;
    });
    require(decoded && decoded->image.size() == image.size() && calls >= 2 &&
                maximum >= encoded.size() + 64 * 32 * 8,
            "encoded clipboard admission includes retained bytes and decoded peak");
    calls = 0;
    require(!ScreenshotClipboardContentReader::decode(snapshot, {},
                                                      [&](qint64 bytes) {
                                                          ++calls;
                                                          return bytes <= encoded.size();
                                                      }) &&
                calls == 2,
            "valid encoded clipboard source is rejected before raster decoding");

    QMimeData mime;
    mime.setData(QStringLiteral("image/png"), encoded);
    calls = 0;
    require(!ScreenshotClipboardContentReader::snapshotMimeData(&mime, 1.0, {},
                                                                [&](qint64) {
                                                                    ++calls;
                                                                    return false;
                                                                }) &&
                calls == 1,
            "lazy MIME data admission precedes requesting its encoded payload");

    ScreenshotClipboardOriginalContent text;
    text.text = QStringLiteral("A source with a bounded raster");
    const auto layoutBytes = text.text.size() * 34;
    maximum = 0;
    require(!ScreenshotClipboardContentReader::renderOriginalText(text, 1.0, {},
                                                                  [&](qint64 bytes) {
                                                                      maximum =
                                                                          std::max(maximum, bytes);
                                                                      return bytes <= layoutBytes;
                                                                  }) &&
                maximum > layoutBytes,
            "text layout and raster allocation have separate admission boundaries");

    text.html = QStringLiteral("before <img src=\"data:image/png;base64,%1\"> after")
                    .arg(QString::fromLatin1(encoded.toBase64()));
    maximum = 0;
    require(ScreenshotClipboardContentReader::renderOriginalText(
                text, 1.0, {},
                [&](qint64 bytes) {
                    maximum = std::max(maximum, bytes);
                    return true;
                }).has_value() &&
                maximum >= 64 * 32 * 8,
            "embedded data images participate in formatted-text admission");
    require(!ScreenshotClipboardContentReader::renderOriginalText(
                text, 1.0, {}, [](qint64 bytes) { return bytes < 64 * 32 * 8; }),
            "embedded image admission failure does not silently render incomplete HTML");
}

void encodedImageAndTextAreSupported() {
    QMimeData mime;
    QImage image(QSize(4, 5), QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    mime.setData(QStringLiteral("image/png"), pngBytes(image));

    const auto imageContent = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(imageContent.has_value() && !imageContent->isFormattedText() &&
                imageContent->image.size() == QSize(4, 5),
            "encoded PNG clipboard data should decode");

    QMimeData textMime;
    const QString longText(4096, u'W');
    textMime.setText(longText);
    const auto textContent = ScreenshotClipboardContentReader::readMimeData(&textMime, 1.0);
    require(textContent.has_value() && textContent->isFormattedText() &&
                textContent->formattedDocument != nullptr && textContent->plainText == longText &&
                textContent->originalContent.html.isEmpty() &&
                textContent->originalContent.text == longText &&
                textContent->image.width() == 1024 && !textContent->image.isNull(),
            "plain clipboard text should be rendered with a bounded image");
}

void imageColorProfilesSurviveImport() {
    QTemporaryDir directory;
    require(directory.isValid(), "profile fixture directory should exist");
    const QColorSpace custom(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Gamma,
                             2.4f);
    for (const QColorSpace& space :
         {QColorSpace{}, QColorSpace(QColorSpace::SRgb), QColorSpace(QColorSpace::DisplayP3),
          QColorSpace(QColorSpace::AdobeRgb), QColorSpace(QColorSpace::SRgbLinear), custom}) {
        QImage source(4, 3, QImage::Format_RGBA8888);
        source.fill(QColor(200, 100, 50));
        source.setPixelColor(1, 1, QColor(80, 120, 160, 128));
        source.setColorSpace(space);
        const QByteArray encoded = pngBytes(source);
        require(QImage::fromData(encoded, "PNG").colorSpace() == space,
                "PNG fixture must retain its intended profile");
        const auto requireOriginalImage = [&](const QImage& decoded) {
            require(!decoded.isNull() && decoded.colorSpace() == space,
                    "image import must retain the source color space");
            require(decoded.convertToFormat(QImage::Format_RGBA8888) == source,
                    "image import must preserve the source pixels and alpha");
        };
        requireOriginalImage(
            snow_shot::image_codec::decode(encoded, snow::image::Format::png, "image/png"));

        const QString path = directory.filePath(QStringLiteral("profile.png"));
        require(source.save(path, "PNG"), "profiled file fixture must encode");
        requireOriginalImage(snow_shot::image_codec::decodeFile(path, snow::image::Format::png));
        requireOriginalImage(
            snow_shot::image_codec::decodeFileBgra(path, snow::image::Format::png));

        QMimeData encodedMime;
        encodedMime.setData(QStringLiteral("image/png"), encoded);
        const auto clipboard = ScreenshotClipboardContentReader::readMimeData(&encodedMime, 1.0);
        require(clipboard.has_value(), "profiled clipboard image must decode");
        requireOriginalImage(clipboard->image);

        QMimeData fileMime;
        fileMime.setUrls({QUrl::fromLocalFile(path)});
        const auto file = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
        require(file.has_value(), "profiled file URL must decode");
        requireOriginalImage(file->image);
    }

    QImage srgb(4, 3, QImage::Format_RGBA8888);
    srgb.setColorSpace(QColorSpace::SRgb);
    srgb.fill(QColor(200, 100, 50));
    const auto canonical = snow_shot::image_codec::encodePng(srgb);
    require(snow_shot::image_codec::decode(canonical, snow::image::Format::png, "image/png") ==
                srgb,
            "the standard PNG sRGB declaration must survive import without an ICC profile");

    QImage p3 = srgb;
    p3.setColorSpace(QColorSpace::DisplayP3);
    QByteArray jpeg;
    QBuffer jpegBuffer(&jpeg);
    require(jpegBuffer.open(QIODevice::WriteOnly) && p3.save(&jpegBuffer, "JPEG", 100),
            "profiled JPEG fixture must encode");
    require(QImage::fromData(jpeg, "JPEG").colorSpace() == p3.colorSpace(),
            "JPEG fixture must retain its intended profile");
    const auto jpegImage =
        snow_shot::image_codec::decode(jpeg, snow::image::Format::jpeg, "image/jpeg");
    require(!jpegImage.isNull() && jpegImage.colorSpace() == p3.colorSpace(),
            "JPEG import must preserve its embedded ICC profile");

    QColorSpace retainedSpace;
    {
        const auto decoded =
            snow_shot::image_codec::decode(pngBytes(p3), snow::image::Format::png, "image/png");
        retainedSpace = decoded.colorSpace();
    }
    require(QColorSpace::fromIccProfile(retainedSpace.iccProfile()) == p3.colorSpace(),
            "a retained color space must own its ICC bytes after the pixel buffer is released");
}

void decodedBufferOwnsItsProfile() {
    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace::DisplayP3);
    source.fill(QColor(200, 100, 50));
    const QByteArray encoded = pngBytes(source);
    const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.constData());
    const auto size = static_cast<uint64_t>(encoded.size());
    SnowShotImageCodecBuffer output{};
    std::array<char, 512> error{};
    require(snow_shot_image_codec_decode_rgba8(bytes, size, SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG,
                                               &output, error.data(), error.size()) != 0,
            "backend profile fixture must decode");
    require(output.color.icc_profile != nullptr && output.color.icc_profile_size != 0,
            "native decode must publish the owned color profile alongside its pixels");
    const QByteArray profile(reinterpret_cast<const char*>(output.color.icc_profile),
                             static_cast<qsizetype>(output.color.icc_profile_size));
    require(QColorSpace::fromIccProfile(profile) == source.colorSpace(),
            "the bridge must carry the original ICC profile");
    const auto* pixels = output.data;
    const auto* icc = output.color.icc_profile;
    require(snow_shot_image_codec_decode_rgba8(bytes, size, SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG,
                                               &output, error.data(), error.size()) == 0 &&
                output.data == pixels && output.color.icc_profile == icc,
            "unreleased decode buffers must retain both allocations when reuse is rejected");
    snow_shot_image_codec_release_buffer(&output);
    require(output.data == nullptr && output.color.icc_profile == nullptr &&
                output.color.icc_profile_size == 0 && output.color.primaries == 0 &&
                output.color.transfer == 0,
            "releasing a native decode buffer must clear pixels and color metadata");
    snow_shot_image_codec_release_buffer(&output);
}

void formattedTextRetainsOriginalClipboardInput() {
    QMimeData mime;
    const QString html =
        QStringLiteral("<article data-source=\"clipboard\"><b>Original HTML</b></article>");
    const QString text = QStringLiteral("Original HTML");
    mime.setHtml(html);
    mime.setText(text);

    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content.has_value() && content->isFormattedText() &&
                content->originalContent.html == html && content->originalContent.text == text,
            "formatted clipboard content should retain its original HTML and text inputs");
}

void encodedImagesPrecedeDetachedImagesAndCorruptionFallsBack() {
    QMimeData mime;
    QImage detached(QSize(3, 2), QImage::Format_RGBA8888);
    detached.fill(Qt::red);
    QImage encoded(QSize(4, 3), QImage::Format_RGBA8888);
    encoded.fill(Qt::blue);
    mime.setImageData(detached);
    mime.setData(QStringLiteral("image/png"), pngBytes(encoded));

    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content.has_value() && content->image.size() == encoded.size() &&
                content->image.pixelColor(0, 0) == QColor(Qt::blue),
            "encoded image MIME data should precede the detached image fallback");

    mime.setData(QStringLiteral("image/png"), QByteArrayLiteral("corrupt"));
    const auto fallback = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(fallback.has_value() && fallback->image.size() == detached.size() &&
                fallback->image.pixelColor(0, 0) == QColor(Qt::red),
            "corrupt encoded image data should fall back to the detached image snapshot");
}

class CountingEncodedMimeData final : public QMimeData {
  public:
    mutable int pngRequests = 0;

  protected:
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override {
        if (mimeType == QStringLiteral("image/png"))
            ++pngRequests;
        return QMimeData::retrieveData(mimeType, type);
    }
};

ScreenshotClipboardContentSnapshot nativeBitmapFallback() {
    const QSize size(2, 1);
    QByteArray bytes(40 + size.width() * size.height() * 4, '\0');
    qToLittleEndian(quint32(40), bytes.data());
    qToLittleEndian(qint32(size.width()), bytes.data() + 4);
    qToLittleEndian(qint32(-size.height()), bytes.data() + 8);
    qToLittleEndian(quint16(1), bytes.data() + 12);
    qToLittleEndian(quint16(32), bytes.data() + 14);
    for (int x = 0; x < size.width(); ++x)
        qToLittleEndian(quint32(0x00ff0000), bytes.data() + 40 + x * 4);
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.nativeDib = ScreenshotClipboardNativeDib{std::move(bytes), size,
                                                      ScreenshotClipboardNativeDibFormat::Dib};
    return snapshot;
}

void nativeSnapshotRetainsPreferredEncodedImages() {
    QImage original(QSize(4, 3), QImage::Format_RGBA8888);
    original.fill(QColor(30, 90, 160, 127));
    const auto png = pngBytes(original);
    CountingEncodedMimeData mime;
    mime.setData(QStringLiteral("image/png"), png);
    auto snapshot = snow_shot::presentation::detail::snapshotClipboardMimeData(
        &mime, 1.0, Qt::white, false, {}, nativeBitmapFallback());
    require(snapshot && snapshot->encodedImages.size() == 1 &&
                snapshot->encodedImages.first().bytes == png && snapshot->nativeDib &&
                snapshot->nativeDib->size == QSize(2, 1) && mime.pngRequests == 1,
            "a native bitmap must not discard the original encoded image");
    const auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
    require(content && content->image.size() == original.size() &&
                content->image.pixelColor(0, 0).alpha() == 127,
            "encoded pixels must retain their resolution and transparency ahead of native bitmaps");

    QImage secondary(QSize(5, 4), QImage::Format_RGB32);
    secondary.fill(Qt::green);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    require(buffer.open(QIODevice::WriteOnly) &&
                snow_shot::image_codec::encodeToDevice(secondary, &buffer,
                                                       snow::image::Format::jpeg, {}),
            "secondary encoded image should encode");
    mime.setData(QStringLiteral("image/jpeg"), jpeg);
    for (const bool corrupt : {false, true}) {
        auto native = nativeBitmapFallback();
        const auto capturedPng = corrupt ? QByteArrayLiteral("corrupt") : png;
        native.encodedImages.append({capturedPng, QStringLiteral("image/png")});
        mime.pngRequests = 0;
        snapshot = snow_shot::presentation::detail::snapshotClipboardMimeData(
            &mime, 1.0, Qt::white, false, {}, std::move(native));
        require(snapshot && snapshot->encodedImages.size() == 2 &&
                    snapshot->encodedImages[0].bytes == capturedPng &&
                    snapshot->encodedImages[1].bytes == jpeg && mime.pngRequests == 0,
                "native PNG bytes must be reused while retaining other encoded formats");
        const auto decoded = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
        require(decoded && decoded->image.size() == (corrupt ? secondary.size() : original.size()),
                "encoded format priority and corruption fallback must precede native bitmaps");
    }
}

class ImageDataOnlyMimeData final : public QMimeData {
  public:
    bool hasFormat(const QString& mimeType) const override {
        return mimeType != QStringLiteral("application/x-qt-image") &&
               QMimeData::hasFormat(mimeType);
    }

    QStringList formats() const override {
        QStringList result = QMimeData::formats();
        result.removeAll(QStringLiteral("application/x-qt-image"));
        return result;
    }
};

void liveSnapshotRetainsBitmapFallback() {
    QClipboard* clipboard = QApplication::clipboard();
    QImage bitmap(QSize(3, 2), QImage::Format_RGB32);
    bitmap.fill(Qt::red);
    QImage encoded(QSize(4, 3), QImage::Format_RGB32);
    encoded.fill(Qt::blue);
    for (bool corrupt : {false, true}) {
        // Keep imageData() available without advertising a native bitmap format.
        auto* mime = new ImageDataOnlyMimeData;
        mime->setImageData(bitmap);
        mime->setData(QStringLiteral("image/png"),
                      corrupt ? QByteArrayLiteral("corrupt") : pngBytes(encoded));
        mime->setText(QStringLiteral("text fallback"));
        clipboard->setMimeData(mime);
        auto snapshot = ScreenshotClipboardContentReader::snapshot(clipboard, 1.0);
        clipboard->clear();
        require(snapshot.has_value(), "live clipboard image should be snapshotted");
        require(!snapshot->nativeDib.has_value() && !snapshot->detachedImage.isNull(),
                "an imageData-only provider must retain its detached bitmap fallback");
        const auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
        const QImage& expected = corrupt ? bitmap : encoded;
        require(content.has_value() && !content->isFormattedText() &&
                    content->image.size() == expected.size() &&
                    content->image.pixelColor(0, 0) == expected.pixelColor(0, 0),
                "live snapshot must retain an owned bitmap when encoded decoding fails");
    }
}

void localImageFilesAndPlainTextFallbackAreSupported() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary image directory should be available");
    const QString path = directory.filePath(QStringLiteral("clipboard.png"));
    QImage image(QSize(7, 6), QImage::Format_RGBA8888);
    image.fill(Qt::green);
    require(image.save(path, "PNG"), "temporary clipboard image should encode");

    QMimeData fileMime;
    fileMime.setUrls({QUrl::fromLocalFile(path)});
    const auto fileContent = ScreenshotClipboardContentReader::readMimeData(&fileMime, 1.0);
    require(fileContent.has_value() && !fileContent->isFormattedText() &&
                fileContent->image.size() == QSize(7, 6),
            "supported local image-file URLs should decode");

    QMimeData fallbackMime;
    fallbackMime.setHtml(QString());
    fallbackMime.setText(QStringLiteral("plain fallback"));
    const auto fallbackContent = ScreenshotClipboardContentReader::readMimeData(&fallbackMime, 1.0);
    require(fallbackContent.has_value() && fallbackContent->isFormattedText() &&
                fallbackContent->plainText == QStringLiteral("plain fallback"),
            "plain text should be used when an advertised HTML payload is empty");
}

void changedLocalFilesAreRejectedAndTextFallbackRemainsAvailable() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary image directory should be available");
    const QString path = directory.filePath(QStringLiteral("changing.png"));
    QImage original(QSize(5, 4), QImage::Format_RGBA8888);
    original.fill(Qt::green);
    require(original.save(path, "PNG"), "changing clipboard image should encode");

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    mime.setText(QStringLiteral("file changed"));
    auto snapshot = ScreenshotClipboardContentReader::snapshotMimeData(
        &mime, 1.0, QApplication::palette().color(QPalette::Base));
    require(snapshot.has_value(), "local image clipboard snapshot should be captured");

    QImage replacement(QSize(8, 7), QImage::Format_RGBA8888);
    replacement.fill(Qt::yellow);
    require(replacement.save(path, "PNG"), "changing clipboard image should be replaced");
    const auto content = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
    require(content.has_value() && content->isFormattedText() &&
                content->plainText == QStringLiteral("file changed"),
            "a changed local image file should be rejected without losing the text fallback");
}

void decodeIsCancellableAndReturnsGuiAffineDocuments() {
    QMimeData mime;
    mime.setHtml(QStringLiteral("<p><b>Worker</b> document</p>"));
    auto cancelledSnapshot = ScreenshotClipboardContentReader::snapshotMimeData(
        &mime, 1.0, QApplication::palette().color(QPalette::Base));
    require(cancelledSnapshot.has_value() &&
                !ScreenshotClipboardContentReader::decode(std::move(*cancelledSnapshot),
                                                          []() { return true; })
                     .has_value(),
            "clipboard content decoding should honor cancellation before doing work");

    auto workerSnapshot = ScreenshotClipboardContentReader::snapshotMimeData(
        &mime, 1.0, QApplication::palette().color(QPalette::Base));
    require(workerSnapshot.has_value(), "worker clipboard snapshot should be captured");
    auto future = std::async(std::launch::async, [snapshot = std::move(*workerSnapshot)]() mutable {
        return ScreenshotClipboardContentReader::decode(std::move(snapshot));
    });
    auto content = future.get();
    require(content.has_value() && content->formattedDocument != nullptr &&
                content->formattedDocument->thread() == QApplication::instance()->thread(),
            "worker-rendered clipboard documents should return with GUI thread affinity");
}

void htmlIsSelectableAndExternalResourcesAreBlocked() {
    QMimeData mime;
    mime.setHtml(QStringLiteral(
        "<p><b>Bold</b> and <i>italic</i></p><img src=\"https://example.invalid/a.png\">"));
    const auto content = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    require(content.has_value() && content->isFormattedText() &&
                content->formattedDocument != nullptr && content->image.width() <= 1024,
            "HTML clipboard data should produce a formatted document and raster");
    bool foundBold = false;
    for (QTextBlock block = content->formattedDocument->begin(); block.isValid();
         block = block.next()) {
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (fragment.isValid() && fragment.text().contains(QStringLiteral("Bold"))) {
                foundBold = fragment.charFormat().fontWeight() >= QFont::Bold;
            }
        }
    }
    require(foundBold, "HTML formatting should be retained by the Qt document");
    require(content->formattedDocument
                ->resource(QTextDocument::ImageResource,
                           QUrl(QStringLiteral("https://example.invalid/a.png")))
                .value<QImage>()
                .isNull(),
            "external HTML resources must not be fetched");
}

void formattedTextHasPaddingOnEverySide() {
    const auto requirePadding = [](const ScreenshotClipboardContent& content, const char* message) {
        constexpr qreal expectedPadding = 16.0;
        require(content.formattedDocument != nullptr, message);
        const QTextDocument& document = *content.formattedDocument;
        const QRectF blockBounds = document.documentLayout()->blockBoundingRect(document.begin());
        const QSizeF documentSize = document.documentLayout()->documentSize();
        require(qFuzzyCompare(document.documentMargin(), expectedPadding) &&
                    blockBounds.left() >= expectedPadding && blockBounds.top() >= expectedPadding &&
                    documentSize.width() - blockBounds.right() >= expectedPadding &&
                    documentSize.height() - blockBounds.bottom() >= expectedPadding,
                message);
    };

    QMimeData plainTextMime;
    plainTextMime.setText(QStringLiteral("Padded plain text"));
    const auto plainTextContent =
        ScreenshotClipboardContentReader::readMimeData(&plainTextMime, 1.0);
    require(plainTextContent.has_value(), "plain text padding fixture should render");
    requirePadding(*plainTextContent, "plain text should have padding on every side");

    QMimeData htmlMime;
    htmlMime.setHtml(QStringLiteral("<p style=\"margin: 0\"><b>Padded HTML</b></p>"));
    const auto htmlContent = ScreenshotClipboardContentReader::readMimeData(&htmlMime, 1.0);
    require(htmlContent.has_value(), "HTML padding fixture should render");
    requirePadding(*htmlContent, "HTML should have padding on every side");
}

void formattedTextBackgroundFollowsApplicationTheme() {
    const QPalette originalPalette = QApplication::palette();
    QPalette themedPalette = originalPalette;
    const QColor themedBackground(QStringLiteral("#19324a"));
    themedPalette.setColor(QPalette::Base, themedBackground);
    QApplication::setPalette(themedPalette);

    QMimeData plainTextMime;
    plainTextMime.setText(QStringLiteral("Themed plain text"));
    const auto plainTextContent =
        ScreenshotClipboardContentReader::readMimeData(&plainTextMime, 1.0);
    require(plainTextContent.has_value() &&
                imageContainsColor(plainTextContent->image, themedBackground),
            "plain text raster background should follow the application theme");

    QMimeData htmlMime;
    htmlMime.setHtml(QStringLiteral("<p><b>Themed</b> HTML text</p>"));
    const auto htmlContent = ScreenshotClipboardContentReader::readMimeData(&htmlMime, 1.0);
    require(htmlContent.has_value() && imageContainsColor(htmlContent->image, themedBackground),
            "HTML raster background should follow the application theme");

    QApplication::setPalette(originalPalette);
}

void htmlSourceBackgroundOverridesApplicationTheme() {
    const QPalette originalPalette = QApplication::palette();
    QPalette themedPalette = originalPalette;
    const QColor themedBackground(QStringLiteral("#19324a"));
    themedPalette.setColor(QPalette::Base, themedBackground);
    QApplication::setPalette(themedPalette);

    const QColor bodyBackground(QStringLiteral("#f3d7a4"));
    QMimeData bodyMime;
    bodyMime.setHtml(
        QStringLiteral("<html><head><style>body { background-color: #f3d7a4; }</style></head>"
                       "<body><p>Body background</p></body></html>"));
    const auto bodyContent = ScreenshotClipboardContentReader::readMimeData(&bodyMime, 1.0);
    require(bodyContent.has_value() && bodyContent->formattedDocument != nullptr &&
                bodyContent->formattedDocument->rootFrame()->frameFormat().background().color() ==
                    bodyBackground &&
                imageContainsColor(bodyContent->image, bodyBackground),
            "an HTML body background should override the application theme");

    const QColor inlineBackground(QStringLiteral("#d6ebff"));
    QMimeData inlineMime;
    inlineMime.setHtml(
        QStringLiteral("<p style=\"margin: 0\"><span style=\"background-color: #d6ebff\">"
                       "Browser clipboard background</span></p>"));
    const auto inlineContent = ScreenshotClipboardContentReader::readMimeData(&inlineMime, 1.0);
    require(inlineContent.has_value() && inlineContent->formattedDocument != nullptr &&
                inlineContent->formattedDocument->rootFrame()->frameFormat().background().color() ==
                    inlineBackground &&
                inlineContent->image.pixelColor(0, inlineContent->image.height() - 1) ==
                    inlineBackground,
            "a uniform clipboard text background should fill the formatted-text canvas");

    QMimeData mixedMime;
    mixedMime.setHtml(QStringLiteral(
        "<p><span style=\"background-color: #ffff00\">Highlighted</span> plain</p>"));
    const auto mixedContent = ScreenshotClipboardContentReader::readMimeData(&mixedMime, 1.0);
    require(mixedContent.has_value() && mixedContent->formattedDocument != nullptr &&
                mixedContent->formattedDocument->rootFrame()->frameFormat().background().style() ==
                    Qt::NoBrush &&
                imageContainsColor(mixedContent->image, themedBackground),
            "a partial text highlight must not replace the application canvas background");

    QApplication::setPalette(originalPalette);
}

void pinnedTextStylesPreserveFormattingAndSnapshots() {
    snow_shot::storage::PinnedTextStyle style;
    style.maximumWidth = 320;
    style.fontFamily = QApplication::font().family();
    style.fontSize = 28;
    style.textColor = QColor(255, 0, 255, 128);
    style.backgroundColor = Qt::transparent;
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.text = QStringLiteral("Pinned text ").repeated(100);
    snapshot.textStyle = style;
    snapshot.devicePixelRatio = 2.0;
    const auto content = ScreenshotClipboardContentReader::decode(snapshot);
    require(content && content->formattedDocument->defaultFont().pixelSize() == 28 &&
                content->formattedDocument->defaultFont().family() == style.fontFamily &&
                content->image.width() == 640 && content->image.devicePixelRatio() == 2.0 &&
                content->image.pixelColor(0, 0).alpha() == 0 &&
                content->originalContent.textStyle == style,
            "pinned text uses captured dimensions, font, transparent background, and DPI");
    require(
        content->formattedDocument->begin().begin().fragment().charFormat().foreground().color() ==
            style.textColor,
        "unstyled text materializes the captured foreground including alpha");
    const auto restored = ScreenshotClipboardContentReader::renderOriginalText(
        content->originalContent, 2.0, Qt::red);
    require(restored && restored->image == content->image &&
                restored->originalContent.text == snapshot.text,
            "restoring captured text ignores later palette defaults and retains original source");
    for (const int width : {100, 2000}) {
        snapshot.textStyle->maximumWidth = width;
        const auto wrapped = ScreenshotClipboardContentReader::decode(snapshot);
        require(wrapped && wrapped->image.width() == width * 2,
                "configured width boundaries cap long text in logical pixels");
    }
    for (const int size : {8, 96}) {
        snapshot.text = QStringLiteral("Text");
        snapshot.textStyle->fontSize = size;
        const auto sized = ScreenshotClipboardContentReader::decode(snapshot);
        require(sized && sized->formattedDocument->defaultFont().pixelSize() == size,
                "configured font-size boundaries render successfully");
    }
    snapshot.textStyle->maximumWidth = 99;
    require(!ScreenshotClipboardContentReader::decode(snapshot),
            "invalid captured styles cannot reach raster allocation");

    snapshot.textStyle = style;
    snapshot.html =
        QStringLiteral("<p>Default <span style=\"font-family: serif; font-size: 11px; color: "
                       "#00ff00; background-color: #ffff00\">Explicit</span></p>");
    const auto html = ScreenshotClipboardContentReader::decode(snapshot);
    require(html && html->originalContent.html == snapshot.html, "HTML keeps its original source");
    bool inherited = false, explicitStyle = false;
    for (auto block = html->formattedDocument->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.text().contains(QStringLiteral("Default")))
                inherited = fragment.charFormat().foreground().color() == style.textColor;
            if (fragment.text().contains(QStringLiteral("Explicit"))) {
                const auto format = fragment.charFormat();
                explicitStyle =
                    format.foreground().color() == QColor(Qt::green) &&
                    format.background().color() == QColor(Qt::yellow) &&
                    format.intProperty(QTextFormat::FontPixelSize) == 11 &&
                    format.fontFamilies().toStringList().contains(QStringLiteral("serif"));
            }
        }
    }
    require(inherited && explicitStyle,
            "explicit HTML styles override inherited defaults without affecting unstyled text");
    snapshot.html =
        QStringLiteral("<body style=\"background-color: #123456\"><p>Source background</p></body>");
    const auto body = ScreenshotClipboardContentReader::decode(snapshot);
    require(body && body->image.pixelColor(0, 0) == QColor(QStringLiteral("#123456")),
            "HTML body background overrides a configured default");

    snapshot.html.clear();
    snapshot.text = QStringLiteral("Worker captured text");
    auto future = std::async(std::launch::async, [snapshot] {
        return ScreenshotClipboardContentReader::decode(snapshot);
    });
    const auto worker = future.get();
    require(worker && worker->originalContent.textStyle == style &&
                worker->formattedDocument->thread() == qApp->thread(),
            "workers consume immutable style snapshots and return GUI-affine documents");
}

void formattedTextUsesOwningDisplayDevicePixelRatio() {
    QMimeData mime;
    mime.setHtml(
        QStringLiteral("<p style=\"font-size: 24px; margin: 0\"><b>Display DPI</b> text</p>"));

    const auto standard = ScreenshotClipboardContentReader::readMimeData(&mime, 1.0);
    const auto highDpi = ScreenshotClipboardContentReader::readMimeData(&mime, 2.0);
    require(standard.has_value() && highDpi.has_value() && standard->isFormattedText() &&
                highDpi->isFormattedText(),
            "formatted text should render at both standard and high display DPRs");
    require(highDpi->image.size() == standard->image.size() * 2 &&
                qFuzzyCompare(highDpi->image.devicePixelRatio(), 2.0) &&
                qFuzzyCompare(highDpi->formattedTextDevicePixelRatio, 2.0),
            "formatted text should allocate its physical backing image from the owning display "
            "DPR");
    require(qFuzzyCompare(highDpi->image.deviceIndependentSize().width(),
                          standard->image.deviceIndependentSize().width()) &&
                qFuzzyCompare(highDpi->image.deviceIndependentSize().height(),
                              standard->image.deviceIndependentSize().height()) &&
                qFuzzyCompare(highDpi->formattedDocument->textWidth(),
                              standard->formattedDocument->textWidth()),
            "display DPR should not change the formatted document's logical layout size");
}

void intrinsicHtmlWidthPreservesTheEntireRaster() {
    QImage source(QSize(1400, 20), QImage::Format_ARGB32);
    source.fill(Qt::red);
    const QStringList fixtures{
        QStringLiteral("<table width=\"1400\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                       "<td>Left</td><td align=\"right\" bgcolor=\"#00ff00\">Right edge</td>"
                       "</tr></table>"),
        QStringLiteral("<pre>%1<span style=\"background-color: #00ff00\">Right edge</span></pre>")
            .arg(QStringLiteral("wide line ").repeated(20)),
        QStringLiteral("<img src=\"data:image/png;base64,%1\">")
            .arg(QString::fromLatin1(pngBytes(source).toBase64())),
    };
    for (const QString& html : fixtures) {
        for (const qreal dpr : {1.0, 1.25, 2.0}) {
            ScreenshotClipboardContentSnapshot snapshot;
            snapshot.html = html;
            snapshot.textStyle = snow_shot::storage::PinnedTextStyle{};
            snapshot.textStyle->fontFamily = QApplication::font().family();
            snapshot.textStyle->maximumWidth = 2000;
            snapshot.devicePixelRatio = dpr;
            const auto reference = ScreenshotClipboardContentReader::decode(snapshot);
            require(reference && reference->formattedDocument->size().width() > 1024,
                    "intrinsic HTML fixture must exceed the default text width");
            require(reference->image.deviceIndependentSize().width() >=
                        reference->formattedDocument->size().width(),
                    "the HTML raster must include the full laid-out width");
            for (const int maximumWidth : {100, 500}) {
                snapshot.textStyle->maximumWidth = maximumWidth;
                const auto content = ScreenshotClipboardContentReader::decode(snapshot);
                require(content && content->image == reference->image,
                        "fixed tables, preformatted lines, and images must ignore a narrower "
                        "text width without clipping their right edge");
                const QSizeF documentSize = content->formattedDocument->size();
                require(content->image.deviceIndependentSize().width() >= documentSize.width() &&
                            content->image.deviceIndependentSize().height() >=
                                documentSize.height(),
                        "the raster must contain the entire selectable document at every DPI");
                const auto restored = ScreenshotClipboardContentReader::renderOriginalText(
                    content->originalContent, dpr);
                require(restored && restored->image == content->image,
                        "restoring and exporting intrinsic HTML must preserve its full width");
            }
            snapshot.textStyle.reset();
            const auto legacy = ScreenshotClipboardContentReader::decode(snapshot);
            require(legacy && legacy->image.size() == reference->image.size(),
                    "HTML without captured settings must also retain its intrinsic width");
        }
    }
}

void flowingHtmlStillUsesTheConfiguredWidth() {
    for (const QString& html :
         {QStringLiteral("<p>%1</p>"),
          QStringLiteral("<table width=\"100%\"><tr><td>%1</td></tr></table>")}) {
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.html = html.arg(QStringLiteral("Flow text ").repeated(200));
        snapshot.textStyle = snow_shot::storage::PinnedTextStyle{};
        snapshot.textStyle->fontFamily = QApplication::font().family();
        snapshot.devicePixelRatio = 1.25;
        int previousHeight = std::numeric_limits<int>::max();
        for (const int maximumWidth : {100, 500, 2000}) {
            snapshot.textStyle->maximumWidth = maximumWidth;
            const auto content = ScreenshotClipboardContentReader::decode(snapshot);
            require(content && content->image.width() == maximumWidth * 1.25 &&
                        content->image.height() < previousHeight,
                    "flowing HTML and percentage-width tables must wrap at the configured width");
            previousHeight = content->image.height();
        }
    }
}

void intrinsicHtmlWidthRespectsRasterLimits() {
    QImage source(QSize(1, 1), QImage::Format_ARGB32);
    source.fill(Qt::red);
    const QString sourceUrl = QStringLiteral("data:image/png;base64,%1")
                                  .arg(QString::fromLatin1(pngBytes(source).toBase64()));
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.html =
        QStringLiteral("<img width=\"100000\" height=\"1000\" src=\"%1\">").arg(sourceUrl);
    require(!ScreenshotClipboardContentReader::decode(snapshot),
            "the full intrinsic width must count toward the raster pixel limit");

    snapshot.html = QStringLiteral("<img width=\"1400\" height=\"20\" src=\"%1\">").arg(sourceUrl);
    const auto content = ScreenshotClipboardContentReader::decode(snapshot);
    require(content && content->image.width() >= 1400,
            "the allocation fixture must include the full image width");
    const qint64 rasterBytes =
        static_cast<qint64>(content->image.width()) * content->image.height() * 8;
    bool rejectedRaster = false;
    const auto admit = [&](qint64 bytes) {
        if (bytes >= rasterBytes) {
            rejectedRaster = true;
            return false;
        }
        return true;
    };
    require(!ScreenshotClipboardContentReader::decode(snapshot, {}, admit) && rejectedRaster,
            "allocation admission must account for the full intrinsic raster width");
    rejectedRaster = false;
    require(!ScreenshotClipboardContentReader::renderOriginalText(content->originalContent, 1.0, {},
                                                                  admit) &&
                rejectedRaster,
            "restoring intrinsic HTML must enforce the same raster admission");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
ScreenshotClipboardContentSnapshot nativeDibSnapshot(QByteArray bytes, QSize size) {
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.devicePixelRatio = 1.0;
    snapshot.nativeDib = ScreenshotClipboardNativeDib{std::move(bytes), size,
                                                      ScreenshotClipboardNativeDibFormat::Dib};
    return snapshot;
}

QByteArray makeRgbDib(int width, int height, bool topDown) {
    const int absoluteHeight = std::abs(height);
    QByteArray bytes(static_cast<int>(sizeof(BITMAPINFOHEADER)) + width * absoluteHeight * 4, 0);
    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(bytes.data());
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = topDown ? -absoluteHeight : absoluteHeight;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    auto* pixels = reinterpret_cast<std::uint32_t*>(bytes.data() + sizeof(BITMAPINFOHEADER));
    for (int y = 0; y < absoluteHeight; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint32_t red = static_cast<std::uint32_t>(x * 31 + 16) & 0xffu;
            const std::uint32_t green = static_cast<std::uint32_t>(y * 47 + 32) & 0xffu;
            const std::uint32_t blue = static_cast<std::uint32_t>(x * 13 + y * 17) & 0xffu;
            const int storageY = topDown ? y : absoluteHeight - 1 - y;
            pixels[storageY * width + x] = (blue << 16) | (green << 8) | red;
        }
    }
    return bytes;
}

void standardRgbDibOrientationAndAlphaAreHandled() {
    const QSize size(3, 2);
    auto topDown = nativeDibSnapshot(makeRgbDib(size.width(), size.height(), true), size);
    auto bottomUp = nativeDibSnapshot(makeRgbDib(size.width(), size.height(), false), size);
    const auto top = ScreenshotClipboardContentReader::decode(std::move(topDown));
    const auto bottom = ScreenshotClipboardContentReader::decode(std::move(bottomUp));
    require(top.has_value() && bottom.has_value() && top->image == bottom->image,
            "BI_RGB DIB orientation should decode to the same logical image");
    require(top->image.pixelColor(0, 0).alpha() == 255 &&
                top->image.pixelColor(size.width() - 1, size.height() - 1).alpha() == 255,
            "BI_RGB DIB pixels should be treated as opaque");
}

void unalignedDibStorageAndPixelOffsetsDecodeCorrectly() {
    const QSize size(17, 2);
    for (const bool bitfields : {false, true}) {
        QByteArray bytes = makeRgbDib(size.width(), size.height(), true);
        if (bitfields) {
            BITMAPINFOHEADER header{};
            std::memcpy(&header, bytes.constData(), sizeof(header));
            header.biCompression = BI_BITFIELDS;
            std::memcpy(bytes.data(), &header, sizeof(header));
            const DWORD masks[]{0x000000ff, 0x0000ff00, 0x00ff0000};
            bytes.insert(sizeof(header), reinterpret_cast<const char*>(masks), sizeof(masks));
        }
        const auto expected =
            ScreenshotClipboardContentReader::decode(nativeDibSnapshot(bytes, size));
        require(expected.has_value(), "the aligned DIB reference decodes");
        for (int offset = 1; offset < 4; ++offset) {
            const QByteArray backing = QByteArray(offset, 0) + bytes;
            const QByteArray borrowed =
                QByteArray::fromRawData(backing.constData() + offset, bytes.size());
            const auto decoded =
                ScreenshotClipboardContentReader::decode(nativeDibSnapshot(borrowed, size));
            require(decoded.has_value() && decoded->image == expected->image,
                    "borrowed DIB headers, masks and pixel rows need no alignment");
        }
        if (!bitfields) {
            // The decoder accepts extended header sizes. Their pixel arrays need
            // not begin on the same alignment as the owning QByteArray.
            bytes.insert(sizeof(BITMAPINFOHEADER), 1, '\0');
            const DWORD headerSize = sizeof(BITMAPINFOHEADER) + 1;
            std::memcpy(bytes.data(), &headerSize, sizeof(headerSize));
            const auto decoded =
                ScreenshotClipboardContentReader::decode(nativeDibSnapshot(bytes, size));
            require(decoded.has_value() && decoded->image == expected->image,
                    "unaligned pixel offsets preserve the DIB image");
        }
    }
}

void malformedAndLargeDibsAreHandled() {
    QByteArray malformed(static_cast<int>(sizeof(BITMAPINFOHEADER)), 0);
    auto* malformedHeader = reinterpret_cast<BITMAPINFOHEADER*>(malformed.data());
    malformedHeader->biSize = sizeof(BITMAPINFOHEADER);
    malformedHeader->biWidth = 0x7fffffff;
    malformedHeader->biHeight = 0x7fffffff;
    malformedHeader->biPlanes = 1;
    malformedHeader->biBitCount = 32;
    malformedHeader->biCompression = BI_RGB;
    require(!ScreenshotClipboardContentReader::decode(
                 nativeDibSnapshot(std::move(malformed), QSize(1, 1)))
                 .has_value(),
            "malformed DIB dimensions should be rejected");

    const QSize largeSize(1024, 1024);
    auto decoded = ScreenshotClipboardContentReader::decode(
        nativeDibSnapshot(makeRgbDib(largeSize.width(), largeSize.height(), true), largeSize));
    require(decoded.has_value() && decoded->image.size() == largeSize,
            "large DIBs should decode through the parallel conversion path");
}

void nativeDibSnapshotDecodesOffClipboard() {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = 2;
    header.bV5Height = 2;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    QByteArray bytes(static_cast<int>(sizeof(BITMAPV5HEADER) + 16), 0);
    std::memcpy(bytes.data(), &header, sizeof(header));
    auto* pixels = reinterpret_cast<std::uint32_t*>(bytes.data() + sizeof(header));
    pixels[0] = 0xff0000ff; // bottom row: blue, red
    pixels[1] = 0xffff0000;
    pixels[2] = 0xff00ff00; // top row: green, white
    pixels[3] = 0xffffffff;

    for (const int maskOrder : {0, 1, 2}) {
        QByteArray payload = bytes;
        if (maskOrder != 0) {
            const DWORD masks[3]{maskOrder == 1 ? header.bV5RedMask : header.bV5BlueMask,
                                 header.bV5GreenMask,
                                 maskOrder == 1 ? header.bV5BlueMask : header.bV5RedMask};
            payload.insert(sizeof(header), reinterpret_cast<const char*>(masks), sizeof(masks));
        }
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.devicePixelRatio = 1.0;
        snapshot.nativeDib = ScreenshotClipboardNativeDib{
            payload, QSize(2, 2), ScreenshotClipboardNativeDibFormat::DibV5};
        const auto decoded = ScreenshotClipboardContentReader::decode(snapshot);
        require(decoded.has_value() && decoded->image.size() == QSize(2, 2),
                "native DIB snapshot should decode into an image");
        require(decoded->image.pixelColor(0, 0) == QColor(Qt::green) &&
                    decoded->image.pixelColor(1, 0) == QColor(Qt::white) &&
                    decoded->image.pixelColor(0, 1) == QColor(Qt::blue) &&
                    decoded->image.pixelColor(1, 1) == QColor(Qt::red),
                "native DIB snapshot should preserve orientation and channels");
        if (maskOrder != 0) {
            // Declare the table so truncation is unambiguous: an undeclared
            // partial table is indistinguishable from pixels and trailing data.
            auto* declaredHeader =
                reinterpret_cast<BITMAPV5HEADER*>(snapshot.nativeDib->bytes.data());
            declaredHeader->bV5ClrUsed = 3;
            const auto declared = ScreenshotClipboardContentReader::decode(snapshot);
            require(declared.has_value() && declared->image == decoded->image,
                    "a declared color table must precede the bitmap pixels");
            snapshot.nativeDib->bytes.chop(1);
            require(!ScreenshotClipboardContentReader::decode(std::move(snapshot)).has_value(),
                    "a truncated canonical DIBV5 must not decode its mask table as pixels");
        }
    }
}

void nativeDibMaskColoredPixelsRemainPixels() {
    for (DWORD headerSize : {DWORD(sizeof(BITMAPV5HEADER)), DWORD(sizeof(BITMAPV4HEADER))}) {
        for (int trailingBytes : {0, 1, 11, 12, 13, 16}) {
            if (headerSize == sizeof(BITMAPV4HEADER) && trailingBytes == 12)
                continue;
            BITMAPV5HEADER header{};
            header.bV5Size = headerSize;
            header.bV5Width = 3;
            header.bV5Height = -1;
            header.bV5Planes = 1;
            header.bV5BitCount = 32;
            header.bV5Compression = BI_BITFIELDS;
            header.bV5RedMask = 0x00ff0000;
            header.bV5GreenMask = 0x0000ff00;
            header.bV5BlueMask = 0x000000ff;
            const DWORD pixels[]{header.bV5RedMask, header.bV5GreenMask, header.bV5BlueMask};
            if (trailingBytes == 12) {
                header.bV5CSType = PROFILE_EMBEDDED;
                header.bV5ProfileData = headerSize + sizeof(pixels);
                header.bV5ProfileSize = trailingBytes;
            }
            QByteArray bytes(static_cast<qsizetype>(headerSize + sizeof(pixels) + trailingBytes),
                             0);
            std::memcpy(bytes.data(), &header, headerSize);
            std::memcpy(bytes.data() + headerSize, pixels, sizeof(pixels));
            const auto decoded = ScreenshotClipboardContentReader::decode(
                nativeDibSnapshot(std::move(bytes), QSize(3, 1)));
            require(decoded.has_value() && decoded->image.pixelColor(0, 0) == QColor(Qt::red) &&
                        decoded->image.pixelColor(1, 0) == QColor(Qt::green) &&
                        decoded->image.pixelColor(2, 0) == QColor(Qt::blue),
                    "V4/V5 pixels equal to masks must not be skipped as an extra mask table");
        }
    }
}
#endif
#include "pinned_color_swatch_content_tests.h"
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
#if defined(Q_OS_WIN)
    require(
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0 &&
            QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/consola.ttf")) >= 0,
        "offscreen document tests require proportional and monospace fonts");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    if (application.arguments().contains(QStringLiteral("--color-swatch-only"))) {
        pinnedColorSwatchContentTests();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--first-frame-only"))) {
        firstFrameFilesKeepCompositionProfilesAndRevisionChecks();
        bmpFilesPreservePixelsAndIdentity();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--import-routing-only"))) {
        imageFilesDecodeByContent();
        tiffFilesAndMimeDataPreservePixels();
        clipboardImagesTakePriorityOverAccompanyingFiles();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--tiff-only"))) {
        imageFileExtensionsMatchShellRegistration();
        tiffFilesAndMimeDataPreservePixels();
        return EXIT_SUCCESS;
    }
    if (application.arguments().contains(QStringLiteral("--text-layout-only"))) {
        intrinsicHtmlWidthPreservesTheEntireRaster();
        flowingHtmlStillUsesTheConfiguredWidth();
        intrinsicHtmlWidthRespectsRasterLimits();
        formattedTextHasPaddingOnEverySide();
        pinnedTextStylesPreserveFormattingAndSnapshots();
        formattedTextUsesOwningDisplayDevicePixelRatio();
        return EXIT_SUCCESS;
    }
    imageFileExtensionsMatchShellRegistration();
    imageFilesDecodeByContent();
    tiffFilesAndMimeDataPreservePixels();
    clipboardImagesTakePriorityOverAccompanyingFiles();
    embeddedImagesUseManagedCachedPixels();
    if (application.arguments().contains(QStringLiteral("--embedded-images-only"))) {
        return EXIT_SUCCESS;
    }
    if (!application.arguments().contains(QStringLiteral("--mime-data-only"))) {
        liveSnapshotRetainsBitmapFallback();
    }
    localTextFilesAreSupported();
    documentFilesPreserveLiteralAndRenderedContent();
    bmpFilesPreservePixelsAndIdentity();
    animatedSourcesSurviveImport();
    firstFrameFilesKeepCompositionProfilesAndRevisionChecks();
    animationReplacementPreparationIsAtomicAndWorkerSafe();
    animationOffersPrecedeStaticThumbnails();
    singleFrameAnimationContainersStayStatic();
    imageSourceDensitySurvivesDecode();
    directImageWinsOverRichText();
    directImageSharesImmutableOwner();
    directManagedImageKeepsSharedStorage();
    directReadOnlyImageRetainsCleanupOwner();
    oversizedDirectImagesAreIgnored();
    automationAdmissionPrecedesDecodeAndRasterization();
    encodedImageAndTextAreSupported();
    imageColorProfilesSurviveImport();
    decodedBufferOwnsItsProfile();
    formattedTextRetainsOriginalClipboardInput();
    encodedImagesPrecedeDetachedImagesAndCorruptionFallsBack();
    nativeSnapshotRetainsPreferredEncodedImages();
    localImageFilesAndPlainTextFallbackAreSupported();
    changedLocalFilesAreRejectedAndTextFallbackRemainsAvailable();
    decodeIsCancellableAndReturnsGuiAffineDocuments();
    htmlIsSelectableAndExternalResourcesAreBlocked();
    formattedTextHasPaddingOnEverySide();
    formattedTextBackgroundFollowsApplicationTheme();
    htmlSourceBackgroundOverridesApplicationTheme();
    intrinsicHtmlWidthPreservesTheEntireRaster();
    flowingHtmlStillUsesTheConfiguredWidth();
    intrinsicHtmlWidthRespectsRasterLimits();
    pinnedTextStylesPreserveFormattingAndSnapshots();
    formattedTextUsesOwningDisplayDevicePixelRatio();
#if defined(Q_OS_WIN) || defined(_WIN32)
    standardRgbDibOrientationAndAlphaAreHandled();
    unalignedDibStorageAndPixelOffsetsDecodeCorrectly();
    malformedAndLargeDibsAreHandled();
    nativeDibSnapshotDecodesOffClipboard();
    nativeDibMaskColoredPixelsRemainPixels();
#endif
    return 0;
}
