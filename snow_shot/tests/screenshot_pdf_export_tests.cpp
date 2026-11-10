#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "PDF fixture must be readable");
    return file.readAll();
}
QImage fixture(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar* pixels = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            pixels[x * 4] = uchar(x % 256);
            pixels[x * 4 + 1] = uchar(y % 256);
            pixels[x * 4 + 2] = uchar((x + y) % 256);
            pixels[x * 4 + 3] = x > size.width() / 2 ? 0 : uchar((x * 13 + y * 7) % 256);
        }
    }
    return image;
}
void validateStructure(const QByteArray& pdf, int pages = 1) {
    require(pdf.startsWith("%PDF-1.7") && pdf.endsWith("%%EOF\n"), "PDF must be finalized");
    const qsizetype start = pdf.lastIndexOf("startxref\n") + 10;
    const qint64 xref = pdf.mid(start, pdf.indexOf('\n', start) - start).toLongLong();
    require(pdf.mid(xref, 5) == "xref\n", "startxref must point to the xref table");
    const auto lines = pdf.mid(xref).split('\n');
    const int objects = lines[1].split(' ')[1].toInt();
    require(objects > 5, "PDF must have a page, image, catalog and metadata");
    for (int id = 1; id < objects; ++id) {
        const qint64 offset = lines[id + 2].left(10).toLongLong();
        require(pdf.mid(offset).startsWith(QByteArray::number(id) + " 0 obj\n"),
                "every xref entry must identify its object");
    }
    require(pdf.count("/Type /Page ") == pages &&
                pdf.contains("/Count " + QByteArray::number(pages)),
            "PDF must contain its expected number of pages");
    require(pdf.contains("1 1 1 rg\n0 0 "), "PDF must explicitly paint a white page");
    require(pdf.contains("/Creator <feff0053006e006f0077002000530068006f0074>"),
            "PDF creator must identify Snow Shot");
}
void layoutRules() {
    for (QSize size :
         {QSize(1, 1), QSize(96, 192), QSize(4000, 100), QSize(100, 60000), QSize(3000, 4000)}) {
        const auto original = screenshot_pdf::layout(size, ScreenshotPdfPageSize::ImageSize);
        require(original.pagePoints == QSizeF(size) * 0.75 &&
                    original.imagePoints.topLeft() == QPointF(),
                "image-size pages must use exactly 96 DPI");
        for (auto paper : {ScreenshotPdfPageSize::PortraitA4, ScreenshotPdfPageSize::LandscapeA4}) {
            const auto geometry = screenshot_pdf::layout(size, paper);
            const QSizeF expected =
                paper == ScreenshotPdfPageSize::PortraitA4 ? QSizeF(210, 297) : QSizeF(297, 210);
            require(std::abs(geometry.pagePoints.width() - expected.width() * 72 / 25.4) < 1e-8 &&
                        std::abs(geometry.pagePoints.height() - expected.height() * 72 / 25.4) <
                            1e-8,
                    "A4 dimensions must be physical millimeters");
            require(geometry.imagePoints.left() >= -1e-8 && geometry.imagePoints.top() >= -1e-8 &&
                        geometry.imagePoints.right() <= geometry.pagePoints.width() + 1e-8 &&
                        geometry.imagePoints.bottom() <= geometry.pagePoints.height() + 1e-8,
                    "the whole image must fit without cropping");
            require(std::abs(geometry.imagePoints.center().x() - geometry.pagePoints.width() / 2) <
                            1e-8 &&
                        std::abs(geometry.imagePoints.center().y() -
                                 geometry.pagePoints.height() / 2) < 1e-8,
                    "image must be centered");
            require(std::abs(geometry.imagePoints.width() / geometry.imagePoints.height() -
                             double(size.width()) / size.height()) < 1e-8,
                    "page fitting must preserve aspect ratio");
        }
    }
}
void losslessTilesAndMetadata(const QString& directory) {
    const QImage source = fixture({2051, 2063});
    auto rows = snow_shot::image_codec::srgbRowSource(source);
    rows.backingImage = {};
    std::vector<int> reads(static_cast<std::size_t>(source.height()));
    rows.readRows = [readRows = rows.readRows, &reads](int first, int count, qsizetype stride,
                                                       uchar* target, qsizetype capacity) {
        for (int row = first; row < first + count; ++row)
            ++reads[static_cast<std::size_t>(row)];
        return readRows(first, count, stride, target, capacity);
    };
    QString error;
    auto payload = screenshot_pdf::prepare(rows, 100, &error);
    require(payload && payload->tiles.size() == 4, "large images must use bounded tiles");
    require(std::all_of(reads.cbegin(), reads.cend(), [](int count) { return count == 1; }),
            "horizontal PDF tiles must read every source row only once");
    int count = 0;
    require(screenshot_pdf::decodeTiles(
                *payload,
                [&](QRect rect, const QImage& image) {
                    ++count;
                    for (int row = 0; row < rect.height(); ++row)
                        require(std::memcmp(image.constScanLine(row),
                                            source.constScanLine(rect.y() + row) + rect.x() * 4,
                                            size_t(rect.width()) * 4) == 0,
                                "lossless payload must preserve every RGB and alpha byte, "
                                "including hidden RGB");
                    return true;
                },
                &error) &&
                count == 4,
            "all lossless tiles must decode");
    const QDateTime time(QDate(2026, 9, 13), QTime(11, 12, 13),
                         QTimeZone::fromSecondsAheadOfUtc(8 * 3600));
    for (auto paper : {ScreenshotPdfPageSize::ImageSize, ScreenshotPdfPageSize::PortraitA4,
                       ScreenshotPdfPageSize::LandscapeA4}) {
        const QString name = QStringLiteral("lossless-%1.pdf").arg(int(paper));
        const auto saved = ScreenshotImageFileService::writePdf(
            *payload, QDir(directory).filePath(name), {paper, 100, {}, time});
        require(saved.succeeded(), "lossless PDF must save");
        const auto pdf = read(saved.path);
        validateStructure(pdf);
        require(pdf.contains("/SMask") && !pdf.contains("/DCTDecode"),
                "lossless transparency requires a soft mask");
        require(pdf.contains("/CreationDate (D:20260913031213Z)"),
                "creation time must retain its timezone meaning");
    }
    const auto unicode = ScreenshotImageFileService::writePdf(
        *payload, QDir(directory).filePath(QString::fromUtf8("capture-\xe9\x9b\xaa.pdf")), {});
    require(unicode.succeeded() && read(unicode.path).contains("96ea>"),
            "Unicode filename must become PDF title");
}
void wideRowBands() {
    // A 2048-high band exceeds the memory budget and must use the bounded disk fallback.
    const QImage source = fixture({4099, 2049});
    for (int quality : {99, 100}) {
        auto rows = snow_shot::image_codec::srgbRowSource(source);
        rows.backingImage = {};
        std::vector<int> reads(static_cast<std::size_t>(source.height()));
        rows.readRows = [readRows = rows.readRows, &reads](int first, int count, qsizetype stride,
                                                           uchar* target, qsizetype capacity) {
            for (int row = first; row < first + count; ++row)
                ++reads[static_cast<std::size_t>(row)];
            return readRows(first, count, stride, target, capacity);
        };
        QString error;
        const auto spooled = screenshot_pdf::prepare(rows, quality, &error);
        const auto backed =
            screenshot_pdf::prepare(snow_shot::image_codec::srgbRowSource(source), quality, &error);
        require(
            spooled && backed && spooled->tiles.size() == 6 &&
                std::all_of(reads.cbegin(), reads.cend(), [](int count) { return count == 1; }) &&
                read(spooled->path()) == read(backed->path()),
            "wide row-backed PDF bands must read each row once and match image-backed tiles");
        require(rows.backingImage.isNull(), "PDF preparation must not materialize its source");
    }
}
void verifiedOpaqueTiles() {
    QImage source = fixture({2051, 19});
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            source.scanLine(y)[x * 4 + 3] = 255;
    for (int quality : {99, 100}) {
        QString error;
        auto rows = snow_shot::image_codec::srgbRowSource(source);
        int sourceReads = 0;
        rows.readRows = [readRows = rows.readRows, &sourceReads](int first, int count,
                                                                 qsizetype stride, uchar* target,
                                                                 qsizetype capacity) {
            ++sourceReads;
            return readRows(first, count, stride, target, capacity);
        };
        const auto normal = screenshot_pdf::prepare(rows, quality, &error);
        const auto verified = screenshot_pdf::prepare(rows, quality, &error, true);
        require(normal && verified && sourceReads == 0 &&
                    read(normal->path()) == read(verified->path()),
                "verified opaque PDF tiles must exactly match normal alpha handling");
    }
}
void lossyAndAutomatic(const QString& directory) {
    QImage source = fixture({257, 257});
    for (int quality : {1, 99}) {
        QString error;
        const auto payload =
            screenshot_pdf::prepare(snow_shot::image_codec::srgbRowSource(source), quality, &error);
        if (!payload)
            throw std::runtime_error("lossy payload: " + error.toStdString());
        bool differs = false;
        require(screenshot_pdf::decodeTiles(
                    *payload,
                    [&](QRect, const QImage& image) {
                        require(image.pixelColor(250, 120) == QColor(Qt::white),
                                "transparent JPEG areas must be white");
                        for (int y = 0; y < image.height(); ++y)
                            for (int x = 0; x < image.width(); ++x) {
                                require(image.pixelColor(x, y).alpha() == 255,
                                        "lossy PDF image must be opaque");
                                differs =
                                    differs || image.pixelColor(x, y) != source.pixelColor(x, y);
                            }
                        return true;
                    },
                    &error) &&
                    differs,
                "quality 1 and 99 must use the lossy path");
        const auto saved = ScreenshotImageFileService::writePdf(
            *payload, QDir(directory).filePath(QStringLiteral("lossy-%1.pdf").arg(quality)), {});
        require(saved.succeeded(), "lossy PDF must save");
        const QByteArray pdf = read(saved.path);
        validateStructure(pdf);
        require(pdf.contains("/DCTDecode") && !pdf.contains("/SMask"),
                "lossy PDF must directly embed opaque JPEG");
    }
    source.setDotsPerMeterX(30000);
    source.setDotsPerMeterY(40000);
    source.setDevicePixelRatio(2);
    const auto saved = ScreenshotImageFileService::saveAutomatically(
        source, {directory}, ScreenshotImageFileFormat::Pdf, QStringLiteral("automatic"),
        QDateTime::currentDateTime(), {ScreenshotPdfPageSize::ImageSize, 1});
    require(saved.succeeded(), "automatic PDF must save");
    const auto pdf = read(saved.path);
    require(pdf.contains("/MediaBox [0 0 192.75000000 192.75000000]") &&
                !pdf.contains("/DCTDecode"),
            "automatic PDF must force lossless and use pixels at 96 DPI regardless of source DPI");
    const auto second = ScreenshotImageFileService::saveAutomatically(
        source, {directory}, ScreenshotImageFileFormat::Pdf, QStringLiteral("automatic"));
    require(second.succeeded() && second.path.endsWith(QStringLiteral("automatic_1.pdf")),
            "PDF collision naming must preserve files");
}
void cancellationAndFailures(const QString& directory) {
    auto rows = snow_shot::image_codec::srgbRowSource(fixture({10, 10}));
    QString error;
    auto payload = screenshot_pdf::prepare(rows, 100, &error);
    require(bool(payload), "cancellation fixture must encode");
    const QString path = QDir(directory).filePath(QStringLiteral("existing.pdf"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write("existing") == 8,
            "existing file fixture must write");
    file.close();
    const auto cancelled =
        ScreenshotImageFileService::writePdf(*payload, path, {}, [] { return true; });
    require(!cancelled.succeeded() && !cancelled.error.isEmpty() && read(path) == "existing",
            "cancel must preserve existing file");
    const auto cancelledImage = ScreenshotImageFileService::write(
        fixture({10, 10}), path, ScreenshotImageFileFormat::Pdf, {}, [] { return true; });
    require(!cancelledImage.succeeded() && read(path) == "existing",
            "image-based PDF saves must honor cancellation");
    const auto nested = ScreenshotImageFileService::writePdf(
        *payload, QDir(directory).filePath(QStringLiteral("new/nested/output.pdf")), {});
    require(nested.succeeded(), "manual PDF must create missing output directories");
    rows.cancellationRequested = [] { return true; };
    require(!screenshot_pdf::prepare(rows, 100, &error) && !error.isEmpty(),
            "preparation must support cancellation");
    require(!screenshot_pdf::prepare(rows, 100, nullptr) &&
                !screenshot_pdf::prepare({}, 100, nullptr),
            "PDF preparation must tolerate an omitted error receiver");
    bool stopped = false;
    int readsBeforeCancellation = 0;
    ScreenshotImageRowSource wide;
    wide.size = QSize(4099, 2049);
    wide.cancellationRequested = [&] { return stopped; };
    wide.readRows = [&](int, int count, qsizetype stride, uchar* target, qsizetype capacity) {
        require(capacity >= count * stride, "PDF staging must reserve its whole strip");
        std::memset(target, 255, static_cast<std::size_t>(count * stride));
        ++readsBeforeCancellation;
        stopped = true;
        return true;
    };
    error.clear();
    require(!screenshot_pdf::prepare(wide, 100, &error) && !error.isEmpty() &&
                readsBeforeCancellation == 1,
            "cancelling wide PDF staging must stop before the next source read");
    rows.cancellationRequested = {};
    rows.backingImage = {};
    rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
    require(!ScreenshotImageFileService::write(rows, path, ScreenshotImageFileFormat::Pdf)
                    .succeeded() &&
                read(path) == "existing",
            "failed source reads must not publish files");
    class FailingDevice final : public QIODevice {
        qint64 readData(char*, qint64) override {
            return -1;
        }
        qint64 writeData(const char*, qint64) override {
            return -1;
        }
    } output;
    output.open(QIODevice::WriteOnly);
    require(!screenshot_pdf::write(*payload, &output, {}, &error),
            "output write failures must propagate");
}
void tallPage(const QString& directory) {
    QImage image(1, 60000, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    const auto saved = ScreenshotImageFileService::write(
        image, QDir(directory).filePath(QStringLiteral("tall.pdf")), ScreenshotImageFileFormat::Pdf,
        {ScreenshotPdfPageSize::ImageSize});
    require(saved.succeeded(), "tall scrolling image must export");
    const auto pdf = read(saved.path);
    require(pdf.contains("/UserUnit 4.00000000") &&
                pdf.contains("/MediaBox [0 0 0.18750000 11250.00000000]"),
            "large physical pages must retain dimensions using UserUnit");
    validateStructure(pdf);
}
void incrementalDocumentAndAdmission() {
    QString error;
    ScreenshotImageRowSource unaccountedLong;
    unaccountedLong.size = QSize(1, 1000001);
    unaccountedLong.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return true; };
    require(!screenshot_pdf::prepare(unaccountedLong, 100, &error),
            "generic unaccounted PDF sources must retain the existing axis guard");
    QByteArray bytes;
    QBuffer output(&bytes);
    require(output.open(QIODevice::WriteOnly), "document fixture must be writable");
    std::weak_ptr<screenshot_pdf::Payload> previous;
    qsizetype visited = 0;
    require(screenshot_pdf::writeDocument(
                3,
                [&](qsizetype pageIndex, screenshot_pdf::Page* page, QString* pageError) {
                    require(
                        previous.expired(),
                        "document writer must retire the preceding page before preparing another");
                    const auto source = fixture({19 + int(pageIndex), 23});
                    auto payload = screenshot_pdf::prepare(
                        snow_shot::image_codec::srgbRowSource(source), 100, pageError);
                    previous = payload;
                    page->payload = std::move(payload);
                    page->geometry = {QSizeF(200, 300), QRectF(10, 20, 100, 150)};
                    ++visited;
                    return bool(page->payload);
                },
                &output, {}, &error) &&
                visited == 3 && previous.expired(),
            "incremental multi-page writer must prepare and retire every payload once");
    require(bytes.count("/Type /Page ") == 3 && bytes.contains("/Count 3 /Kids [") &&
                bytes.count("/MediaBox [0 0 200.00000000 300.00000000]") == 3 &&
                bytes.endsWith("%%EOF\n"),
            "document writer must finalize a shared page tree with caller-supplied geometry");
    validateStructure(bytes, 3);
    auto source = snow_shot::image_codec::srgbRowSource(fixture({19, 23}));
    bool admitted = false;
    source.reserveResources = [&admitted](qint64 retained, qint64 scratch,
                                          QString* admissionError) -> std::shared_ptr<void> {
        admitted = true;
        require(retained == 0 && scratch > 0 && scratch < 100 * 1024 * 1024,
                "PDF preparation must admit bounded scratch rather than full-image storage");
        *admissionError = QStringLiteral("scratch denied");
        return {};
    };
    error.clear();
    require(!screenshot_pdf::prepare(source, 100, &error) && admitted &&
                error == QStringLiteral("scratch denied"),
            "resource admission failure must stop PDF preparation before allocating tiles");
    const auto payload = screenshot_pdf::prepare(
        snow_shot::image_codec::srgbRowSource(fixture({19, 23})), 100, &error);
    require(bool(payload), "multi-page failure fixture must prepare");
    const screenshot_pdf::PageProducer producer = [payload](qsizetype, screenshot_pdf::Page* page,
                                                            QString*) {
        page->payload = payload;
        page->geometry = screenshot_pdf::layout(payload->size, ScreenshotPdfPageSize::PortraitA4);
        return true;
    };
    class FailingDevice final : public QIODevice {
        qint64 readData(char*, qint64) override {
            return -1;
        }
        qint64 writeData(const char*, qint64) override {
            return -1;
        }
    } failing;
    failing.open(QIODevice::WriteOnly);
    error.clear();
    require(!screenshot_pdf::writeDocument(2, producer, &failing, {}, &error) && !error.isEmpty(),
            "multi-page write failures must propagate");
    QByteArray incomplete;
    QBuffer destination(&incomplete);
    destination.open(QIODevice::WriteOnly);
    error.clear();
    require(!screenshot_pdf::writeDocument(
                2,
                [&](qsizetype index, screenshot_pdf::Page* page, QString* pageError) {
                    if (index == 1) {
                        *pageError = QStringLiteral("source denied");
                        return false;
                    }
                    return producer(index, page, pageError);
                },
                &destination, {}, &error) &&
                error == QStringLiteral("source denied") && !incomplete.contains("%%EOF"),
            "later source failures must leave an unfinalized document for atomic cleanup");
    incomplete.clear();
    destination.seek(0);
    bool stopped = false;
    require(!screenshot_pdf::writeDocument(
                2, producer, &destination, {}, &error, [&] { return stopped; },
                [&](qsizetype index, qreal fraction) {
                    if (index == 0 && fraction == 1)
                        stopped = true;
                }) &&
                !incomplete.contains("%%EOF"),
            "multi-page cancellation must stop before finalizing the page tree");
}
void composedLayersAndClipping() {
    QString error;
    const auto payload = screenshot_pdf::prepare(
        snow_shot::image_codec::srgbRowSource(fixture({16, 10})), 100, &error);
    require(bool(payload), "composed layer fixture must prepare");
    const auto writeLayers = [&](const QList<screenshot_pdf::Layer>& layers, QByteArray* bytes,
                                 QSizeF size = QSizeF(200, 300)) {
        QBuffer output(bytes);
        require(output.open(QIODevice::WriteOnly), "layer destination must open");
        return screenshot_pdf::writeDocument(
            1,
            [&](qsizetype, screenshot_pdf::Page* page, QString*) {
                page->geometry.pagePoints = size;
                page->layers = layers;
                return true;
            },
            &output, {}, &error);
    };
    QByteArray bytes;
    require(writeLayers({{payload, QRectF(-20, 10, 160, 100), QRectF(10, 20, 60, 50)},
                         {payload, QRectF(110, 130, 40, 25), QRectF(100, 100, 80, 100)}},
                        &bytes),
            "composed PDF must accept independent clipped layers, including page overflow");
    validateStructure(bytes);
    require(bytes.count(" re W n\n") == 2 &&
                bytes.contains("10.00000000 230.00000000 60.00000000 50.00000000 re W n\n") &&
                bytes.contains("100.00000000 100.00000000 80.00000000 100.00000000 re W n\n") &&
                bytes.contains("160.00000000 0 0 100.00000000 -20.00000000 190.00000000 cm\n") &&
                bytes.contains("40.00000000 0 0 25.00000000 110.00000000 145.00000000 cm\n"),
            "each layer must preserve its exact placement and PDF-coordinate cell clip");
    require(bytes.count("/Subtype /Image /ColorSpace /DeviceRGB") == 2,
            "each source remains an independently embedded full-resolution image");
    bytes.clear();
    require(
        writeLayers({{payload, QRectF(-1000, -1000, 16, 10), QRectF(10, 20, 60, 50)}}, &bytes) &&
            bytes.endsWith("%%EOF\n") && !bytes.contains("/Subtype /Image"),
        "fully clipped images must export a valid blank page without unused image objects");
    for (const auto& layer :
         {screenshot_pdf::Layer{payload, QRectF(-20, 10, 160, 100), std::nullopt},
          screenshot_pdf::Layer{payload, QRectF(0, 0, 160, 100), QRectF(-1, 0, 20, 20)},
          screenshot_pdf::Layer{payload, QRectF(0, 0, 160, 100), QRectF(0, 0, 201, 20)},
          screenshot_pdf::Layer{payload, QRectF(0, 0, 0, 100), QRectF(0, 0, 20, 20)}}) {
        bytes.clear();
        require(!writeLayers({layer}, &bytes), "invalid layer and clip geometry must be rejected");
    }
    bytes.clear();
    require(writeLayers({{payload, QRectF(0, 0, 20000, 12500), QRectF(300, 600, 3000, 6000)}},
                        &bytes, QSizeF(20000, 30000)) &&
                bytes.contains("/UserUnit 3.00000000") &&
                bytes.contains("100.00000000 7800.00000000 1000.00000000 2000.00000000 re W n"),
            "layer clips must use the same UserUnit as large paper and image transforms");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        const QString directory =
            app.arguments().size() > 1 ? app.arguments()[1] : temporary.path();
        require(QDir().mkpath(directory), "PDF fixture directory must exist");
        layoutRules();
        losslessTilesAndMetadata(directory);
        wideRowBands();
        verifiedOpaqueTiles();
        lossyAndAutomatic(directory);
        cancellationAndFailures(directory);
        tallPage(directory);
        incrementalDocumentAndAdmission();
        composedLayersAndClipping();
        std::cout << "PDF export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
