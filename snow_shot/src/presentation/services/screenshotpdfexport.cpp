#include "snow_shot/presentation/screenshotpdfexport.h"
#include "snowimageqtcodec.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>

namespace screenshot_pdf {
namespace {
bool fail(QString* error) {
    *error = QCoreApplication::translate("ScreenshotPdfExport", "The PDF could not be exported");
    return false;
}
bool isCancelled(const Cancelled& cancelled, QString* error) {
    if (!cancelled || !cancelled())
        return false;
    *error = QCoreApplication::translate("ScreenshotPdfExport", "The PDF export was cancelled");
    return true;
}
QByteArray number(double value) {
    return QByteArray::number(value, 'f', 8);
}
QByteArray pdfString(const QString& value) {
    QByteArray bytes("\xfe\xff", 2);
    for (const QChar character : value) {
        const auto code = character.unicode();
        bytes.append(char(code >> 8));
        bytes.append(char(code & 255));
    }
    return '<' + bytes.toHex() + '>';
}
QByteArray inflate(QByteArray data, qsizetype expectedSize) {
    QByteArray header(4, '\0');
    qToBigEndian(quint32(expectedSize), header.data());
    return qUncompress(header + data);
}
class Writer {
  public:
    explicit Writer(QIODevice* device) : m_device(device) {}
    bool put(const QByteArray& bytes) {
        if (!m_ok)
            return false;
        m_ok = m_device->write(bytes) == bytes.size();
        m_position += bytes.size();
        return m_ok;
    }
    int object(const QByteArray& contents) {
        const int id = int(m_offsets.size());
        m_offsets.push_back(m_position);
        put(QByteArray::number(id) + " 0 obj\n" + contents + "\nendobj\n");
        return id;
    }
    int stream(const QByteArray& dictionary, const QByteArray& data) {
        const int id = int(m_offsets.size());
        m_offsets.push_back(m_position);
        put(QByteArray::number(id) + " 0 obj\n<< " + dictionary + " /Length " +
            QByteArray::number(data.size()) + " >>\nstream\n");
        put(data);
        put("\nendstream\nendobj\n");
        return id;
    }
    bool finish(int root, int info) {
        const qint64 start = m_position;
        put("xref\n0 " + QByteArray::number(m_offsets.size()) + "\n0000000000 65535 f \n");
        for (qsizetype index = 1; index < m_offsets.size(); ++index) {
            if (m_offsets[index] > 9999999999LL)
                return false;
            put(QByteArray::number(m_offsets[index]).rightJustified(10, '0') + " 00000 n \n");
        }
        return put("trailer\n<< /Size " + QByteArray::number(m_offsets.size()) + " /Root " +
                   QByteArray::number(root) + " 0 R /Info " + QByteArray::number(info) +
                   " 0 R >>\nstartxref\n" + QByteArray::number(start) + "\n%%EOF\n");
    }
    int nextId() const {
        return int(m_offsets.size());
    }

  private:
    QIODevice* m_device;
    QList<qint64> m_offsets{0};
    qint64 m_position = 0;
    bool m_ok = true;
};
} // namespace

ScreenshotPdfPageSize pageSizeForKey(const QString& key) {
    if (key == QStringLiteral("image_size"))
        return ScreenshotPdfPageSize::ImageSize;
    if (key == QStringLiteral("a4_landscape"))
        return ScreenshotPdfPageSize::LandscapeA4;
    return ScreenshotPdfPageSize::PortraitA4;
}
Layout layout(QSize pixels, ScreenshotPdfPageSize pageSize) {
    if (pixels.isEmpty())
        return {};
    QSizeF page = QSizeF(pixels) * (72.0 / 96.0);
    if (pageSize != ScreenshotPdfPageSize::ImageSize) {
        page = QSizeF(210.0 * 72.0 / 25.4, 297.0 * 72.0 / 25.4);
        if (pageSize == ScreenshotPdfPageSize::LandscapeA4)
            page.transpose();
    }
    const double scale = qMin(page.width() / pixels.width(), page.height() / pixels.height());
    const QSizeF image = QSizeF(pixels) * scale;
    return {page, QRectF(QPointF((page.width() - image.width()) / 2,
                                 (page.height() - image.height()) / 2),
                         image)};
}
QString Payload::path() const {
    return directory.filePath(QStringLiteral("tiles.bin"));
}

std::shared_ptr<Payload> prepare(const ScreenshotImageRowSource& source, int quality,
                                 QString* error) {
    if (!source.isValid() || source.size.width() > 1000000 || source.size.height() > 1000000 ||
        qint64(source.size.width()) * source.size.height() > qint64{2} * 1024 * 1024 * 1024) {
        fail(error);
        return {};
    }
    auto result = std::make_shared<Payload>();
    result->size = source.size;
    result->quality = qBound(1, quality, 100);
    QFile file(result->path());
    if (!result->directory.isValid() || !file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return {};
    }
    const qsizetype stride = qsizetype(source.size.width()) * 4;
    const int stripRows = int(qMax(qsizetype{1}, qsizetype{1024 * 1024} / stride));
    QByteArray strip(stride * stripRows, Qt::Uninitialized);
    for (int y = 0; y < source.size.height(); y += 2048) {
        for (int x = 0; x < source.size.width(); x += 2048) {
            if (isCancelled(source.cancellationRequested, error))
                return {};
            const QRect rect(x, y, qMin(2048, source.size.width() - x),
                             qMin(2048, source.size.height() - y));
            QImage image(rect.size(), QImage::Format_RGBA8888);
            if (image.isNull()) {
                fail(error);
                return {};
            }
            for (int row = 0; row < rect.height(); row += stripRows) {
                const int count = qMin(stripRows, rect.height() - row);
                if (isCancelled(source.cancellationRequested, error))
                    return {};
                if (!source.readRows(y + row, count, stride, reinterpret_cast<uchar*>(strip.data()),
                                     strip.size())) {
                    fail(error);
                    return {};
                }
                for (int line = 0; line < count; ++line)
                    std::memcpy(image.scanLine(row + line),
                                strip.constData() + line * stride + x * 4,
                                size_t(rect.width()) * 4);
            }
            QByteArray color;
            QByteArray alpha;
            if (result->quality == 100) {
                color.resize(qsizetype(rect.width()) * rect.height() * 3);
                alpha.resize(qsizetype(rect.width()) * rect.height());
                bool opaque = true;
                for (int row = 0; row < rect.height(); ++row) {
                    const uchar* pixels = image.constScanLine(row);
                    for (int column = 0; column < rect.width(); ++column) {
                        const qsizetype index = qsizetype(row) * rect.width() + column;
                        std::memcpy(color.data() + index * 3, pixels + column * 4, 3);
                        alpha[index] = char(pixels[column * 4 + 3]);
                        opaque = opaque && pixels[column * 4 + 3] == 255;
                    }
                }
                color = qCompress(color).mid(4);
                alpha = opaque ? QByteArray{} : qCompress(alpha).mid(4);
            } else {
                // Flatten explicitly, including hidden RGB, before the one JPEG encode.
                for (int row = 0; row < rect.height(); ++row) {
                    uchar* pixels = image.scanLine(row);
                    for (int column = 0; column < rect.width(); ++column) {
                        uchar* pixel = pixels + column * 4;
                        const int opacity = pixel[3];
                        for (int channel = 0; channel < 3; ++channel)
                            pixel[channel] = uchar(
                                (pixel[channel] * opacity + 255 * (255 - opacity) + 127) / 255);
                        pixel[3] = 255;
                    }
                }
                QBuffer buffer(&color);
                buffer.open(QIODevice::WriteOnly);
                snow::image::EncodeOptions options;
                options.format = snow::image::Format::jpeg;
                options.quality = result->quality;
                options.preserve_metadata = false;
                if (!snow_shot::image_codec::encodeToDevice(image, &buffer, options.format, options,
                                                            error))
                    return {};
            }
            const Tile tile{rect, file.pos(), color.size(), alpha.size()};
            if (color.isEmpty() || file.write(color) != color.size() ||
                file.write(alpha) != alpha.size()) {
                fail(error);
                return {};
            }
            result->tiles.push_back(tile);
        }
    }
    if (!file.flush()) {
        *error = file.errorString();
        return {};
    }
    if (isCancelled(source.cancellationRequested, error))
        return {};
    return result;
}

bool write(const Payload& payload, QIODevice* output, const ScreenshotPdfOptions& options,
           QString* error, const Cancelled& cancelled) {
    if (!output || !output->isWritable() || payload.tiles.isEmpty())
        return fail(error);
    QFile input(payload.path());
    if (!input.open(QIODevice::ReadOnly)) {
        *error = input.errorString();
        return false;
    }
    const Layout geometry = layout(payload.size, options.pageSize);
    const double unit = qMax(
        1.0, std::ceil(qMax(geometry.pagePoints.width(), geometry.pagePoints.height()) / 14400.0));
    const QSizeF page = geometry.pagePoints / unit;
    const double scale = geometry.imagePoints.width() / payload.size.width() / unit;
    QByteArray content =
        "1 1 1 rg\n0 0 " + number(page.width()) + ' ' + number(page.height()) + " re f\n";
    QByteArray resources;
    Writer writer(output);
    writer.put("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n");
    for (const Tile& tile : payload.tiles) {
        if (isCancelled(cancelled, error))
            return false;
        if (!input.seek(tile.offset))
            return fail(error);
        const QByteArray color = input.read(tile.colorBytes);
        const QByteArray alpha = input.read(tile.alphaBytes);
        if (color.size() != tile.colorBytes || alpha.size() != tile.alphaBytes)
            return fail(error);
        const QByteArray dimensions = " /Width " + QByteArray::number(tile.rect.width()) +
                                      " /Height " + QByteArray::number(tile.rect.height()) +
                                      " /BitsPerComponent 8";
        int mask = 0;
        if (!alpha.isEmpty())
            mask = writer.stream(
                "/Type /XObject /Subtype /Image /ColorSpace /DeviceGray /Filter /FlateDecode" +
                    dimensions,
                alpha);
        const int id = writer.stream(
            "/Type /XObject /Subtype /Image /ColorSpace /DeviceRGB /Interpolate false /Filter " +
                QByteArray(payload.quality == 100 ? "/FlateDecode" : "/DCTDecode") + dimensions +
                (mask ? " /SMask " + QByteArray::number(mask) + " 0 R" : QByteArray{}),
            color);
        const QByteArray name = "/Im" + QByteArray::number(id);
        resources += name + ' ' + QByteArray::number(id) + " 0 R ";
        const double left = geometry.imagePoints.left() / unit + tile.rect.x() * scale;
        const double bottom = page.height() - geometry.imagePoints.top() / unit -
                              (tile.rect.y() + tile.rect.height()) * scale;
        content += "q\n" + number(tile.rect.width() * scale) + " 0 0 " +
                   number(tile.rect.height() * scale) + ' ' + number(left) + ' ' + number(bottom) +
                   " cm\n" + name + " Do\nQ\n";
    }
    const int contents = writer.stream({}, content);
    const int pageId = writer.nextId();
    writer.object("<< /Type /Page /Parent " + QByteArray::number(pageId + 1) +
                  " 0 R /MediaBox [0 0 " + number(page.width()) + ' ' + number(page.height()) +
                  "] /UserUnit " + number(unit) + " /Resources << /XObject << " + resources +
                  ">> >> /Contents " + QByteArray::number(contents) + " 0 R >>");
    const int pages =
        writer.object("<< /Type /Pages /Count 1 /Kids [" + QByteArray::number(pageId) + " 0 R] >>");
    const int root =
        writer.object("<< /Type /Catalog /Pages " + QByteArray::number(pages) + " 0 R >>");
    const QDateTime time =
        options.creationTime.isValid() ? options.creationTime : QDateTime::currentDateTimeUtc();
    const int info =
        writer.object("<< /Title " + pdfString(options.title) + " /Creator " +
                      pdfString(QStringLiteral("Snow Shot")) + " /CreationDate (D:" +
                      time.toUTC().toString(QStringLiteral("yyyyMMddHHmmss")).toLatin1() + "Z) >>");
    if (isCancelled(cancelled, error))
        return false;
    return writer.finish(root, info) || fail(error);
}

bool decodeTiles(const Payload& payload, const std::function<bool(QRect, const QImage&)>& consume,
                 QString* error, const Cancelled& cancelled) {
    QFile input(payload.path());
    if (!input.open(QIODevice::ReadOnly))
        return fail(error);
    for (const Tile& tile : payload.tiles) {
        if (isCancelled(cancelled, error))
            return false;
        if (!input.seek(tile.offset))
            return fail(error);
        QByteArray color = input.read(tile.colorBytes);
        QByteArray alpha = input.read(tile.alphaBytes);
        if (color.size() != tile.colorBytes || alpha.size() != tile.alphaBytes)
            return fail(error);
        QImage image;
        if (payload.quality < 100) {
            image =
                snow_shot::image_codec::decode(color, snow::image::Format::jpeg, "pdf-tile.jpg");
            image = image.convertToFormat(QImage::Format_RGBA8888);
        } else {
            const qsizetype count = qsizetype(tile.rect.width()) * tile.rect.height();
            color = inflate(color, count * 3);
            if (!alpha.isEmpty())
                alpha = inflate(alpha, count);
            if (color.size() != count * 3 || (tile.alphaBytes && alpha.size() != count))
                return fail(error);
            image = QImage(tile.rect.size(), QImage::Format_RGBA8888);
            if (image.isNull())
                return fail(error);
            for (int row = 0; row < image.height(); ++row) {
                uchar* pixels = image.scanLine(row);
                for (int column = 0; column < image.width(); ++column) {
                    const qsizetype index = qsizetype(row) * image.width() + column;
                    std::memcpy(pixels + column * 4, color.constData() + index * 3, 3);
                    pixels[column * 4 + 3] = alpha.isEmpty() ? 255 : uchar(alpha[index]);
                }
            }
        }
        if (image.isNull() || image.size() != tile.rect.size() || !consume(tile.rect, image))
            return fail(error);
    }
    return true;
}
} // namespace screenshot_pdf
