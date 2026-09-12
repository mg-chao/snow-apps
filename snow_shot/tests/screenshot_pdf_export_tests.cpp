#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTimeZone>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

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
void validateStructure(const QByteArray& pdf) {
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
    require(pdf.count("/Type /Page ") == 1 && pdf.contains("/Count 1"),
            "PDF must contain one page");
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
    QString error;
    auto payload = screenshot_pdf::prepare(rows, 100, &error);
    require(payload && payload->tiles.size() == 4, "large images must use bounded tiles");
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
    rows.cancellationRequested = {};
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
        lossyAndAutomatic(directory);
        cancellationAndFailures(directory);
        tallPage(directory);
        std::cout << "PDF export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
