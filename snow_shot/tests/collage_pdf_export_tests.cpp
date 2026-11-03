// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagepdfexport.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace snow_shot::presentation;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "export fixture must be readable");
    return file.readAll();
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "failure fixture must be writable");
}

QImage fixture(QSize size = {7, 11}) {
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        uchar* pixels = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            pixels[x * 4] = uchar((x * 31) % 256);
            pixels[x * 4 + 1] = uchar((y * 19) % 256);
            pixels[x * 4 + 2] = uchar(((x + y) * 17) % 256);
            pixels[x * 4 + 3] = uchar((x * 29 + y * 13) % 256);
        }
    }
    return image;
}

QImage flattened(QImage image) {
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        image =
            snowCanvasColorConvertedImage(image, QColorSpace(QColorSpace::SRgb), image.format());
    image = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        uchar* pixels = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x, pixels += 4) {
            const unsigned alpha = pixels[3];
            for (int channel = 0; channel < 3; ++channel)
                pixels[channel] =
                    uchar((unsigned(pixels[channel]) * alpha + 255 * (255 - alpha) + 127) / 255);
            pixels[3] = 255;
        }
    }
    return image;
}

QVector<QImage> pdfImages(const QByteArray& pdf) {
    const QString text = QString::fromLatin1(pdf);
    QRegularExpression expression(QStringLiteral(
        "/Subtype /Image[^>]*?/Width (\\d+) /Height (\\d+)[^>]*?/Length (\\d+) >>\\nstream\\n"));
    auto matches = expression.globalMatch(text);
    QVector<QImage> images;
    while (matches.hasNext()) {
        const auto match = matches.next();
        require(match.captured().contains(QStringLiteral("/FlateDecode")),
                "document image pixels must remain lossless");
        const int width = match.captured(1).toInt();
        const int height = match.captured(2).toInt();
        const int length = match.captured(3).toInt();
        QByteArray prefix(4, '\0');
        qToBigEndian(quint32(width * height * 3), prefix.data());
        const QByteArray rgb = qUncompress(prefix + pdf.mid(match.capturedEnd(), length));
        require(rgb.size() == width * height * 3, "embedded RGB tile must decode completely");
        images.append(QImage(reinterpret_cast<const uchar*>(rgb.constData()), width, height,
                             width * 3, QImage::Format_RGB888)
                          .copy());
    }
    return images;
}

void structure(const QByteArray& pdf, int pages) {
    require(pdf.startsWith("%PDF-1.7") && pdf.endsWith("%%EOF\n"), "PDF output must be finalized");
    require(pdf.count("/Type /Page ") == pages &&
                pdf.contains("/Count " + QByteArray::number(pages) + " /Kids ["),
            "page tree must contain exactly one page per original image");
    require(!pdf.contains("/SMask") && !pdf.contains("/Font") && !pdf.contains(" Tj"),
            "PDF output must be opaque and contain no interface labels");
    require(pdf.count("1 1 1 rg\n0 0 ") == pages,
            "every PDF page must paint an explicit white background");
    const qsizetype start = pdf.lastIndexOf("startxref\n") + 10;
    const qint64 offset = pdf.mid(start, pdf.indexOf('\n', start) - start).toLongLong();
    const auto lines = pdf.mid(offset).split('\n');
    require(lines[0] == "xref", "PDF startxref must identify its cross-reference table");
    const int count = lines[1].split(' ')[1].toInt();
    for (int id = 1; id < count; ++id) {
        const auto position = lines[id + 2].left(10).toLongLong();
        require(pdf.mid(position).startsWith(QByteArray::number(id) + " 0 obj\n"),
                "every cross-reference entry must identify its own object");
    }
}

void publicationReleasesFiles(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("source.png")), "fixture must import");
    for (const auto mode : {CollagePdfExportMode::Combined, CollagePdfExportMode::PerPage}) {
        const QDir output(
            QDir(directory).filePath(QStringLiteral("publication-%1").arg(static_cast<int>(mode))));
        CollagePdfSettings settings;
        settings.exportMode = mode;
        const QString destination = mode == CollagePdfExportMode::Combined
                                        ? output.filePath(QStringLiteral("new.pdf"))
                                        : output.path();
        const auto result = exportCollagePdf(document.items(), settings, {destination});
        require(result.succeeded(), qPrintable(result.errorMessage()));
        require(result.paths.size() == 1 && result.completedPages == QVector<qsizetype>{1},
                "new combined and per-page exports must publish their completed PDF");
        structure(read(result.paths.first()), 1);
        const QString moved = result.paths.first() + QStringLiteral(".moved");
        require(QFile::rename(result.paths.first(), moved) && QFile::remove(moved),
                "published PDFs must have no export-owned handle blocking rename or removal");
        require(output.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
                "successful publication must leave no temporary file or directory");
    }
}

void combinedPublicationConflict(const QString& directory) {
    const QDir output(QDir(directory).filePath(QStringLiteral("combined-conflict")));
    const QString path = output.filePath(QStringLiteral("new.pdf"));
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("source.png")), "fixture must import");
    bool raced = false;
    const auto result = exportCollagePdf(document.items(), {}, {path}, {},
                                         [&](const CollagePdfExportProgress& value) {
                                             if (!raced && value.pageFraction >= 0.99) {
                                                 raced = true;
                                                 write(path, "created during export");
                                             }
                                         });
    require(raced && !result.succeeded() && !result.cancelled && !result.error.isEmpty() &&
                result.paths.isEmpty() && result.completedPages.isEmpty() &&
                read(path) == "created during export",
            "combined publication must preserve a destination created during PDF encoding");
    require(output.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot) ==
                QStringList{QStringLiteral("new.pdf")},
            "failed publication must remove its staging file and directory");
    require(QFile::remove(path) && exportCollagePdf(document.items(), {}, {path}).succeeded(),
            "publication failure must allow export to the same destination on retry");
    structure(read(path), 1);
}

void cancellationCleansNewFiles(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("source.png")), "fixture must import");
    for (const auto mode : {CollagePdfExportMode::Combined, CollagePdfExportMode::PerPage}) {
        const QDir output(
            QDir(directory).filePath(QStringLiteral("cancel-new-%1").arg(static_cast<int>(mode))));
        CollagePdfSettings settings;
        settings.exportMode = mode;
        const QString destination = mode == CollagePdfExportMode::Combined
                                        ? output.filePath(QStringLiteral("new.pdf"))
                                        : output.path();
        bool cancelled = false;
        const auto result = exportCollagePdf(
            document.items(), settings, {destination}, [&] { return cancelled; },
            [&](const CollagePdfExportProgress& value) {
                if (value.pageFraction > 0.2)
                    cancelled = true;
            });
        require(result.cancelled && result.paths.isEmpty() && result.completedPages.isEmpty(),
                "cancellation before publication must not report any completed PDF");
        require(output.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
                "canceled new exports must remove every staging file and directory");
        require(exportCollagePdf(document.items(), settings, {destination}).succeeded(),
                "canceled exports must release their staging handles before retry");
    }
}

#ifdef Q_OS_WIN
void lockedDestinationPreservesOriginal(const QString& directory) {
    const QDir output(QDir(directory).filePath(QStringLiteral("locked-destination")));
    require(QDir().mkpath(output.path()), "locked destination directory must exist");
    const QString path = output.filePath(QStringLiteral("locked.pdf"));
    write(path, "original locked PDF");
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("source.png")), "fixture must import");
    const HANDLE handle =
        CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "fixture must lock the existing destination");
    const auto result = exportCollagePdf(document.items(), {}, {path, true});
    CloseHandle(handle);
    require(!result.succeeded() && !result.cancelled && !result.error.isEmpty() &&
                result.paths.isEmpty() && result.completedPages.isEmpty() &&
                read(path) == "original locked PDF",
            "a real destination lock must report failure and preserve the original bytes");
    require(output.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot) ==
                QStringList{QStringLiteral("locked.pdf")},
            "a destination lock must not leave any temporary file or directory");
    require(exportCollagePdf(document.items(), {}, {path, true}).succeeded(),
            "authorized replacement must succeed after the destination lock is released");
    structure(read(path), 1);
}
#endif

void transformsAndSourceResolution(const QString& directory) {
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (int flip = 0; flip < 4; ++flip) {
            CollageDocument document;
            QImage image = fixture();
            if ((rotation + flip) % 2) {
                image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
            }
            const auto id = document.append(image, QStringLiteral("SECRET_ORIGINAL_NAME.png"));
            require(id != 0, "original image must be accepted");
            for (int count = 0; count < rotation; ++count)
                require(document.rotate(id, true), "rotation fixture must apply");
            if (flip & 1)
                require(document.flip(id, true), "horizontal flip fixture must apply");
            if (flip & 2)
                require(document.flip(id, false), "vertical flip fixture must apply");
            const auto item = document.items().first();
            const auto before = collageResourceUsage().reservedBytes;
            const QString path = QDir(directory).filePath(
                QStringLiteral("transform-%1-%2.pdf").arg(rotation).arg(flip));
            const auto result = exportCollagePdf(document.items(), {}, {path});
            require(result.succeeded(), "all rotation and flip combinations must export");
            require(collageResourceUsage().reservedBytes == before,
                    "PDF scratch reservations must retire after export");
            const auto pdf = read(path);
            structure(pdf, 1);
            require(!pdf.contains("SECRET_ORIGINAL_NAME"),
                    "original image labels must not become PDF metadata or page content");
            const auto tiles = pdfImages(pdf);
            const auto expected = flattened(snowCanvasTransformImage(item.image, item.transform));
            require(tiles.size() == 1 && tiles.first().size() == expected.size(),
                    "export must use transformed original dimensions");
            for (int y = 0; y < expected.height(); ++y)
                for (int x = 0; x < expected.width(); ++x)
                    require(
                        tiles.first().pixelColor(x, y) == expected.pixelColor(x, y),
                        "PDF pixels must match transformed sRGB originals flattened onto white");
            require(document.items().first().transform == item.transform &&
                        document.items().first().revision == item.revision,
                    "export must not change editing state");
        }
    }
}

void combinedOrderAndGeometry(const QString& directory) {
    CollageDocument document;
    QImage red(19, 23, QImage::Format_RGB32);
    red.fill(Qt::red);
    QImage blue(1, 60000, QImage::Format_RGB32);
    blue.fill(Qt::blue);
    require(document.append(red, QStringLiteral("red.png")) &&
                document.append(blue, QStringLiteral("long.png")),
            "combined originals must import");
    auto items = document.items();
    std::reverse(items.begin(), items.end());
    CollagePdfSettings settings;
    settings.marginTopMm = 17;
    settings.marginBottomMm = 13;
    settings.marginLeftMm = 23;
    settings.marginRightMm = 7;
    for (auto orientation : {CollagePdfOrientation::Portrait, CollagePdfOrientation::Landscape}) {
        settings.orientation = orientation;
        const QString path = QDir(directory).filePath(
            QStringLiteral("combined-%1.pdf").arg(static_cast<int>(orientation)));
        qsizetype progressedPage = 0;
        const auto result = exportCollagePdf(
            items, settings, {path}, {}, [&](const CollagePdfExportProgress& value) {
                require(value.pageNumber >= progressedPage && value.totalPages == 2,
                        "progress must visit pages in current document order");
                progressedPage = value.pageNumber;
            });
        require(result.succeeded() && result.completedPages == QVector<qsizetype>({1, 2}) &&
                    result.paths == QStringList{path},
                "combined export must return one finalized file for all completed pages");
        const auto pdf = read(path);
        structure(pdf, 2);
        const QByteArray media = orientation == CollagePdfOrientation::Portrait
                                     ? "/MediaBox [0 0 595.27559055 841.88976378]"
                                     : "/MediaBox [0 0 841.88976378 595.27559055]";
        require(pdf.count(media) == 2, "all pages must share selected physical paper dimensions");
        const auto images = pdfImages(pdf);
        require(images.size() == 31 && images.first().width() == 1 &&
                    images.first().pixelColor(0, 0) == QColor(Qt::blue) &&
                    images.last().pixelColor(0, 0) == QColor(Qt::red),
                "long image tiles must remain on one page and preserve current image order");
        const auto layout = calculateCollagePdfPageLayout(transformedSize(items.first()), settings);
        const auto number = [](qreal value) { return QByteArray::number(value, 'f', 8); };
        constexpr qreal points = 72.0 / 25.4;
        require(pdf.contains(number(layout.imageMillimeters.left() * points)),
                "asymmetric margins must affect actual image centering");
    }
}

void imageSizedPages(const QString& directory) {
    CollageDocument document;
    const auto first = document.append(fixture({96, 192}), QStringLiteral("portrait.png"));
    const auto second = document.append(fixture({48, 96}), QStringLiteral("rotated.png"));
    require(first && second && document.rotate(second, true),
            "image-sized PDF fixtures must import and rotate");
    CollagePdfSettings settings;
    settings.paperSize = CollagePdfPaperSize::ImageSize;
    settings.orientation = CollagePdfOrientation::Landscape;
    const QList<QSize> expected{QSize(96, 192), QSize(96, 48)};
    for (const auto mode : {CollagePdfExportMode::Combined, CollagePdfExportMode::PerPage}) {
        settings.exportMode = mode;
        const bool combined = mode == CollagePdfExportMode::Combined;
        const QString path = QDir(directory).filePath(
            combined ? QStringLiteral("image-sized.pdf") : QStringLiteral("image-sized-pages"));
        const auto result = exportCollagePdf(document.items(), settings, {path});
        require(result.succeeded() && result.paths.size() == (combined ? 1 : 2),
                "combined and per-page export support image-sized paper");
        QByteArray pdf;
        for (const auto& output : result.paths) {
            const auto bytes = read(output);
            structure(bytes, combined ? 2 : 1);
            pdf += bytes;
        }
        QRegularExpression media(QStringLiteral("/MediaBox \\[0 0 ([0-9.]+) ([0-9.]+)\\]"));
        QRegularExpression placement(
            QStringLiteral("q\\n([0-9.]+) 0 0 ([0-9.]+) ([0-9.]+) ([0-9.]+) cm\\n"));
        const QString text = QString::fromLatin1(pdf);
        auto pages = media.globalMatch(text);
        auto images = placement.globalMatch(text);
        for (const auto size : expected) {
            require(pages.hasNext() && images.hasNext(), "each image must produce one page");
            const auto page = pages.next();
            const auto image = images.next();
            const qreal width = size.width() * 0.75;
            const qreal height = size.height() * 0.75;
            require(std::abs(page.captured(1).toDouble() - width) < 1e-8 &&
                        std::abs(page.captured(2).toDouble() - height) < 1e-8 &&
                        std::abs(image.captured(1).toDouble() - width) < 1e-8 &&
                        std::abs(image.captured(2).toDouble() - height) < 1e-8 &&
                        image.captured(3).toDouble() == 0 && image.captured(4).toDouble() == 0,
                    "each exported page matches its transformed image at 96 DPI with no margins");
        }
        require(!pages.hasNext() && !images.hasNext(),
                "image-sized output contains exactly the expected page geometry");
        const auto embedded = pdfImages(pdf);
        require(embedded.size() == expected.size() && embedded[0].size() == expected[0] &&
                    embedded[1].size() == expected[1],
                "image-sized export retains full transformed source resolution");
    }
}

void namesAndConflicts(const QString& directory) {
    const QDir output(QDir(directory).filePath(QStringLiteral("pages")));
    require(QDir().mkpath(output.path()), "per-page fixture directory must exist");
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("C:\\images\\example<name>.png")) &&
                document.append(fixture(), QStringLiteral("CON.png")) &&
                document.append(fixture(), QString(200, QChar(0x96ea)) + QStringLiteral(".png")),
            "named originals must import");
    const QString existing = output.filePath(QStringLiteral("001_example_name_.pdf"));
    write(existing, "existing");
    CollagePdfSettings settings;
    settings.exportMode = CollagePdfExportMode::PerPage;
    bool raced = false;
    const auto result =
        exportCollagePdf(document.items(), settings, {output.path()}, {},
                         [&](const CollagePdfExportProgress& value) {
                             if (!raced && value.pageNumber == 2 && value.pageFraction >= 0.99) {
                                 raced = true;
                                 write(output.filePath(QStringLiteral("002_CON.pdf")), "raced");
                             }
                         });
    require(result.succeeded() && result.paths.size() == 3,
            "per-page export must complete every page independently");
    require(QFileInfo(result.paths[0]).fileName() == QStringLiteral("001_example_name__2.pdf") &&
                QFileInfo(result.paths[1]).fileName() == QStringLiteral("002_CON_2.pdf"),
            "numbered safe filenames must resolve both initial and concurrent conflicts");
    require(read(existing) == "existing" &&
                read(output.filePath(QStringLiteral("002_CON.pdf"))) == "raced",
            "existing and concurrently created files must never be overwritten");
    require(QFileInfo(result.paths[2]).fileName().toUtf8().size() < 150,
            "long Unicode image names must fit filesystem filename limits");
    for (const auto& path : result.paths)
        structure(read(path), 1);
}

void exportNamesPreserveCollagePolicy(const QString& directory) {
    const QDir output(QDir(directory).filePath(QStringLiteral("sanitized-pages")));
    require(QDir().mkpath(output.path()), "filename fixture directory exists");
    const QString emoji = QString::fromUcs4(U"\U0001f4f7");
    const QString exactLimit = QString(116, u'a') + emoji;
    const QString overLimit = QString(118, u'b') + emoji;
    const QString controls = QStringLiteral("name") + QChar(1) + QChar(0x7f) + QChar(0x81);
    const QStringList labels = {QStringLiteral("C:\\folder\\") + controls + QStringLiteral(".png"),
                                QStringLiteral("  ...  .png"), exactLimit + QStringLiteral("x.png"),
                                overLimit + QStringLiteral(".png")};
    CollageDocument document;
    for (const auto& label : labels)
        require(document.append(fixture(), label), "filename originals must import");
    CollagePdfSettings settings;
    settings.exportMode = CollagePdfExportMode::PerPage;
    const auto result = exportCollagePdf(document.items(), settings, {output.path()});
    require(result.succeeded() && result.paths.size() == 4,
            "filename originals must export independently");
    const QStringList expected = {QStringLiteral("001_name___.pdf"),
                                  QStringLiteral("002_Image.pdf"),
                                  QStringLiteral("003_%1.pdf").arg(exactLimit),
                                  QStringLiteral("004_%1.pdf").arg(QString(118, u'b'))};
    for (qsizetype index = 0; index < expected.size(); ++index)
        require(QFileInfo(result.paths[index]).fileName() == expected[index],
                "collage exports remove paths and extensions, replace C1 controls, and cap UTF-8 "
                "safely");
}

void cancellationAndPartialFailure(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture({2051, 2063}), QStringLiteral("first.png")) &&
                document.append(fixture(), QStringLiteral("second.png")),
            "cancellation originals must import");
    const auto snapshot = document.items();
    const QString path = QDir(directory).filePath(QStringLiteral("cancelled.pdf"));
    write(path, "preserved");
    bool cancelled = false;
    const auto combined = exportCollagePdf(
        snapshot, {}, {path, true}, [&] { return cancelled; },
        [&](const CollagePdfExportProgress& value) {
            if (value.pageFraction > 0.2)
                cancelled = true;
        });
    require(combined.cancelled && combined.completedPages.isEmpty() && read(path) == "preserved" &&
                document.items().size() == 2,
            "combined cancellation must preserve the existing file and all editing content");
    require(!exportCollagePdf(snapshot, {}, {path}).succeeded() && read(path) == "preserved",
            "combined replacement requires explicit overwrite authorization");
    CollagePdfSettings settings;
    settings.exportMode = CollagePdfExportMode::PerPage;
    const QDir output(QDir(directory).filePath(QStringLiteral("partial-cancel")));
    cancelled = false;
    const auto partial = exportCollagePdf(
        snapshot, settings, {output.path()}, [&] { return cancelled; },
        [&](const CollagePdfExportProgress& value) {
            if (value.completedPages == 1 && value.pageFraction == 0)
                cancelled = true;
        });
    require(partial.cancelled && partial.completedPages == QVector<qsizetype>{1} &&
                partial.paths.size() == 1 && QFileInfo::exists(partial.paths.first()),
            "per-page cancellation must retain and identify only completed page files");
    require(output.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size() == 1,
            "cancellation must leave no unfinished staging file");
    const QDir failureOutput(QDir(directory).filePath(QStringLiteral("partial-failure")));
    auto invalidSource = snapshot;
    bool invalidated = false;
    const auto failure = exportCollagePdf(invalidSource, settings, {failureOutput.path()}, {},
                                          [&](const CollagePdfExportProgress& value) {
                                              if (!invalidated && value.completedPages == 1 &&
                                                  value.pageFraction == 0) {
                                                  // Invalidate a test-owned source after admission
                                                  // to exercise preparation failure after a
                                                  // successful publication without
                                                  // platform-specific disk permissions.
                                                  invalidSource[1].image = {};
                                                  invalidated = true;
                                              }
                                          });
    require(!failure.succeeded() && !failure.cancelled &&
                failure.completedPages == QVector<qsizetype>{1} && failure.paths.size() == 1 &&
                failure.error.contains(QStringLiteral("Page 2")),
            "per-page failure must report its failing page and preserve completed page paths");
    require(
        failureOutput.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size() == 1,
        "failed export must clean up every incomplete staging file");
    require(exportCollagePdf(snapshot, {}, {QDir(directory).filePath(QStringLiteral("retry.pdf"))})
                .succeeded(),
            "preserved editing content must export successfully on retry");
}

void invalidInput(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("invalid.png")), "fixture must import");
    const QString path = QDir(directory).filePath(QStringLiteral("invalid.pdf"));
    CollagePdfSettings settings;
    settings.marginLeftMm = 300;
    require(!exportCollagePdf(document.items(), settings, {path}).succeeded() &&
                !QFileInfo::exists(path),
            "invalid settings must fail before creating any output file");
    require(!exportCollagePdf({}, {}, {path}).succeeded() &&
                !exportCollagePdf(document.items(), {}, {}).succeeded(),
            "empty document and missing destination must be rejected");
    require(exportCollagePdf(document.items(), {}, {path}, [] { return true; }).cancelled &&
                !QFileInfo::exists(path),
            "pre-canceled exports must not create files");
}

void admittedLongImage(const QString& directory) {
    CollageDocument document;
    QImage image(1, 1000001, QImage::Format_RGB32);
    image.fill(Qt::green);
    require(document.append(image, QStringLiteral("very-long.png")),
            "narrow long image must obey the existing pixel admission policy");
    const QString path = QDir(directory).filePath(QStringLiteral("very-long.pdf"));
    CollagePdfExportProgress finalProgress;
    const auto result =
        exportCollagePdf(document.items(), {}, {path}, {},
                         [&](const CollagePdfExportProgress& value) { finalProgress = value; });
    require(result.succeeded(), "an admitted image must not encounter a second PDF axis limit");
    require(finalProgress.completedPages == 1 && finalProgress.totalPages == 1 &&
                finalProgress.pageNumber == 1 && finalProgress.pageFraction == 1,
            "completed export progress must identify the last page at 100 percent");
    structure(read(path), 1);
}

void overwritePermissionBindsSelectedFilename(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("source.png")), "fixture must import");
    const QDir output(directory);
    const QString selected = output.filePath(QStringLiteral("report.txt"));
    const QString normalized = output.filePath(QStringLiteral("report.txt.pdf"));
    write(selected, "selected file");
    write(normalized, "another file");
    const auto rejected = exportCollagePdf(document.items(), {}, {selected, true});
    require(!rejected.succeeded() && read(selected) == "selected file" &&
                read(normalized) == "another file",
            "adding PDF suffix must not transfer overwrite permission to another filename");
    require(exportCollagePdf(document.items(), {}, {normalized, true}).succeeded(),
            "explicit permission for the actual PDF filename permits replacement");
    structure(read(normalized), 1);
    const QString uppercase = output.filePath(QStringLiteral("uppercase.PDF"));
    write(uppercase, "authorized uppercase extension");
    const auto replaced = exportCollagePdf(document.items(), {}, {uppercase, true});
    require(replaced.succeeded() && replaced.paths == QStringList{uppercase},
            "case-insensitive PDF suffix recognition must retain exact filename permission");
    structure(read(uppercase), 1);
    const QString newSelected = output.filePath(QStringLiteral("new.txt"));
    write(newSelected, "preserved selected filename");
    const auto appended = exportCollagePdf(document.items(), {}, {newSelected, true});
    require(
        appended.succeeded() && read(newSelected) == "preserved selected filename" &&
            appended.paths == QStringList{newSelected + QStringLiteral(".pdf")},
        "extension normalization may publish a new destination without replacing the selection");
}

void positiveTinyPaperKeepsNonzeroGeometry(const QString& directory) {
    CollageDocument document;
    require(document.append(fixture(), QStringLiteral("tiny-paper.png")), "fixture must import");
    CollagePdfSettings settings;
    settings.paperSize = CollagePdfPaperSize::Custom;
    settings.customWidthMm = 1e-10;
    settings.customHeightMm = 2e-10;
    settings.marginTopMm = 0;
    settings.marginBottomMm = 0;
    settings.marginLeftMm = 0;
    settings.marginRightMm = 0;
    const QString path = QDir(directory).filePath(QStringLiteral("tiny-paper.pdf"));
    require(exportCollagePdf(document.items(), settings, {path}).succeeded(),
            "valid positive paper dimensions must export even below fixed decimal precision");
    const auto pdf = read(path);
    structure(pdf, 1);
    const QString text = QString::fromLatin1(pdf);
    const auto media =
        QRegularExpression(QStringLiteral("/MediaBox \\[0 0 ([0-9.]+) ([0-9.]+)\\]")).match(text);
    const auto placement =
        QRegularExpression(QStringLiteral("q\\n([0-9.]+) 0 0 ([0-9.]+) ([0-9.]+) ([0-9.]+) cm\\n"))
            .match(text);
    require(media.hasMatch() && placement.hasMatch(),
            "PDF geometry must use fixed decimal notation without an exponent");
    const qreal pageWidth = media.captured(1).toDouble();
    const qreal pageHeight = media.captured(2).toDouble();
    const qreal imageWidth = placement.captured(1).toDouble();
    const qreal imageHeight = placement.captured(2).toDouble();
    const qreal left = placement.captured(3).toDouble();
    const qreal bottom = placement.captured(4).toDouble();
    constexpr qreal points = 72.0 / 25.4;
    require(pageWidth > 0 && pageHeight > 0 && imageWidth > 0 && imageHeight > 0 &&
                std::abs(pageWidth / (settings.customWidthMm * points) - 1) < 1e-12 &&
                std::abs(pageHeight / (settings.customHeightMm * points) - 1) < 1e-12,
            "small nonzero page and image dimensions must retain their physical values");
    require(std::abs(imageWidth / imageHeight - 7.0 / 11.0) < 1e-12 && left >= 0 && bottom >= 0 &&
                left + imageWidth <= pageWidth * (1 + 1e-12) &&
                bottom + imageHeight <= pageHeight * (1 + 1e-12),
            "tiny paper must retain proportional image placement inside its page");
}
void templateCompositionAndPublication(const QString& directory) {
    CollageDocument document;
    auto settings = document.pdfSettings();
    settings.stitchingMethod = CollagePdfStitchingMethod::TemplateBased;
    settings.templateType = CollagePdfTemplateType::FiveImages;
    settings.imageSpacingMm = 7.5;
    document.setPdfSettings(settings);
    for (int index = 0; index < 7; ++index)
        require(document.append(fixture({9 + index, 13}),
                                QStringLiteral("source-%1.png").arg(index)) != 0,
                "template sources must import");
    const auto first = document.items().front().id;
    require(document.setPdfPlacement(first, {{-30, 20}, 4.5}) && document.rotate(first, true),
            "template fixture must preserve manual scale through rotation");
    QVector<CollagePdfExportProgress> updates;
    const auto combined =
        exportCollagePdf(document.items(), settings,
                         {QDir(directory).filePath(QStringLiteral("template-combined.pdf"))}, {},
                         [&](const CollagePdfExportProgress& update) { updates.append(update); });
    require(combined.succeeded() && combined.totalCount == 2 &&
                combined.completedPages == QVector<qsizetype>({1, 2}),
            "combined template export counts composed pages instead of input images");
    const auto pdf = read(combined.paths.front());
    structure(pdf, 2);
    require(pdf.count(" re W n\n") == 7 && pdfImages(pdf).size() == 7,
            "occupied cells have independent image clips and unused final cells remain blank");
    constexpr qreal points = 72.0 / 25.4;
    const auto number = [](qreal value) { return QByteArray::number(value, 'f', 8); };
    for (qsizetype pageIndex = 0; pageIndex < 2; ++pageIndex) {
        const auto page = calculateCollagePdfComposedPage(document.items(), settings, pageIndex);
        require(page.isValid(), "export geometry must be available to the preview");
        for (const auto& cell : page.cells) {
            if (cell.itemIndex < 0)
                continue;
            const auto& clip = cell.clipMillimeters;
            const QByteArray command =
                number(clip.left() * points) + ' ' +
                number((page.pageMillimeters.height() - clip.bottom()) * points) + ' ' +
                number(clip.width() * points) + ' ' + number(clip.height() * points) + " re W n\n";
            require(pdf.contains(command), "exported cell clip must match shared preview geometry");
            const auto& image = cell.imageMillimeters;
            if (cell.itemId != first) {
                const auto pixels = transformedSize(document.items()[cell.itemIndex]);
                const qreal scale =
                    std::min(clip.width() / pixels.width(), clip.height() / pixels.height());
                require(clip.adjusted(-0.000001, -0.000001, 0.000001, 0.000001).contains(image) &&
                            std::abs(image.width() - pixels.width() * scale) < 0.000001 &&
                            std::abs(image.height() - pixels.height() * scale) < 0.000001 &&
                            (image.center() - clip.center()).manhattanLength() < 0.000001,
                        "automatically placed PDF images must remain fully visible and centered");
            }
            const QByteArray matrix =
                number(image.width() * points) + " 0 0 " + number(image.height() * points) + ' ' +
                number(image.left() * points) + ' ' +
                number((page.pageMillimeters.height() - image.bottom()) * points) + " cm\n";
            require(pdf.contains(matrix),
                    "exported image placement must match shared preview geometry");
        }
    }
    require(!updates.isEmpty() && updates.back().completedPages == 2 &&
                updates.back().totalPages == 2 && updates.back().pageFraction == 1 &&
                std::all_of(updates.cbegin(), updates.cend(),
                            [](const auto& update) {
                                return update.totalPages == 2 && update.pageNumber >= 1 &&
                                       update.pageNumber <= 2;
                            }),
            "all export progress must refer to composed pages");
    settings.exportMode = CollagePdfExportMode::PerPage;
    const auto separate = exportCollagePdf(
        document.items(), settings, {QDir(directory).filePath(QStringLiteral("template-pages"))});
    require(separate.succeeded() && separate.paths.size() == 2 &&
                QFileInfo(separate.paths[0]).fileName() == QStringLiteral("001_page.pdf") &&
                QFileInfo(separate.paths[1]).fileName() == QStringLiteral("002_page.pdf"),
            "per-page template export creates one numbered PDF for every composed page");
    for (qsizetype index = 0; index < separate.paths.size(); ++index) {
        const auto bytes = read(separate.paths[index]);
        structure(bytes, 1);
        require(pdfImages(bytes).size() == (index == 0 ? 5 : 2),
                "per-page files must contain every image assigned to that page");
    }
    bool cancelled = false;
    const auto partial = exportCollagePdf(
        document.items(), settings, {QDir(directory).filePath(QStringLiteral("template-partial"))},
        [&] { return cancelled; },
        [&](const auto& update) {
            if (update.completedPages == 1)
                cancelled = true;
        });
    require(partial.cancelled && partial.totalCount == 2 &&
                partial.completedPages == QVector<qsizetype>({1}) && partial.paths.size() == 1 &&
                QFileInfo::exists(partial.paths.front()),
            "template cancellation retains complete pages and reports composed-page progress");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        require(temporary.isValid(), "PDF fixture directory must exist");
        publicationReleasesFiles(temporary.path());
        combinedPublicationConflict(temporary.path());
        cancellationCleansNewFiles(temporary.path());
#ifdef Q_OS_WIN
        lockedDestinationPreservesOriginal(temporary.path());
#endif
        transformsAndSourceResolution(temporary.path());
        combinedOrderAndGeometry(temporary.path());
        imageSizedPages(temporary.path());
        namesAndConflicts(temporary.path());
        exportNamesPreserveCollagePolicy(temporary.path());
        cancellationAndPartialFailure(temporary.path());
        invalidInput(temporary.path());
        admittedLongImage(temporary.path());
        overwritePermissionBindsSelectedFilename(temporary.path());
        positiveTinyPaperKeepsNonzeroGeometry(temporary.path());
        templateCompositionAndPublication(temporary.path());
        std::cout << "Collage PDF export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
