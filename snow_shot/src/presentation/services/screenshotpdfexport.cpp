#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotpdfexport.h"
#include "snowimageqtcodec.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>

namespace screenshot_pdf {
namespace {
bool fail(QString* error) {
    if (error)
        *error =
            QCoreApplication::translate("ScreenshotPdfExport", "The PDF could not be exported");
    return false;
}
bool isCancelled(const Cancelled& cancelled, QString* error) {
    if (!cancelled || !cancelled())
        return false;
    if (error)
        *error = QCoreApplication::translate("ScreenshotPdfExport", "The PDF export was cancelled");
    return true;
}
QByteArray number(double value) {
    if (value == 0 || std::abs(value) >= 1e-6)
        return QByteArray::number(value, 'f', 8);
    // PDF real numbers cannot use exponent notation. Expand seventeen significant digits
    // explicitly so a valid small paper dimension or image matrix never rounds down to zero.
    const QByteArray scientific = QByteArray::number(std::abs(value), 'e', 16);
    const qsizetype exponentStart = scientific.indexOf('e');
    const int exponent = scientific.mid(exponentStart + 1).toInt();
    QByteArray digits = scientific.left(exponentStart);
    digits.remove(digits.indexOf('.'), 1);
    QByteArray result = (value < 0 ? QByteArray("-0.") : QByteArray("0.")) +
                        QByteArray(-exponent - 1, '0') + digits;
    while (result.endsWith('0'))
        result.chop(1);
    return result;
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
// A horizontal tile band reads each source row once. Small bands stay in bounded memory;
// exceptionally wide row-backed sources use temporary storage instead of a full raster.
class RasterBand {
  public:
    bool load(const ScreenshotImageRowSource& source, int first, int count,
              const QString& directory, QString* error) {
        m_first = first;
        m_stride = qsizetype(source.size.width()) * 4;
        if (!source.backingImage.isNull() && source.backingImage.size() == source.size &&
            source.backingImage.format() == QImage::Format_RGBA8888) {
            m_backing = source.backingImage;
            m_stride = m_backing.bytesPerLine();
            return true;
        }
        if (source.size.width() <= 2048) {
            m_source = source;
            return true;
        }
        const qsizetype bytes = m_stride * count;
        constexpr qsizetype maximumMemoryBytes = 32 * 1024 * 1024;
        if (bytes <= maximumMemoryBytes)
            m_pixels.resize(bytes);
        else {
            m_file.setFileTemplate(directory + QStringLiteral("/band-XXXXXX.rgba"));
            if (!m_file.open()) {
                if (error)
                    *error = m_file.errorString();
                return false;
            }
        }
        const int stripRows = int(qMax(qsizetype{1}, qsizetype{1024 * 1024} / m_stride));
        QByteArray scratch;
        if (m_file.isOpen())
            scratch.resize(m_stride * qMin(stripRows, count));
        for (int row = 0; row < count; row += stripRows) {
            if (isCancelled(source.cancellationRequested, error))
                return false;
            const int rows = qMin(stripRows, count - row);
            uchar* destination = reinterpret_cast<uchar*>(
                m_file.isOpen() ? scratch.data() : m_pixels.data() + row * m_stride);
            if (!source.readRows(first + row, rows, m_stride, destination, m_stride * rows))
                return fail(error);
            if (m_file.isOpen() &&
                m_file.write(scratch.constData(), m_stride * rows) != m_stride * rows)
                return fail(error);
        }
        return !m_file.isOpen() || m_file.flush() || fail(error);
    }
    QImage tile(const QRect& rect, QString* error, const Cancelled& cancelled) {
        QImage image = snowCanvasAllocateImage(rect.size(), QImage::Format_RGBA8888);
        if (image.isNull()) {
            fail(error);
            return {};
        }
        const qint64 rowBytes = qint64(rect.width()) * 4;
        if (m_source.isValid()) {
            const int stripRows = int(qMax(qint64{1}, qint64{1024 * 1024} / rowBytes));
            for (int row = 0; row < rect.height(); row += stripRows) {
                if (isCancelled(cancelled, error))
                    return {};
                const int count = qMin(stripRows, rect.height() - row);
                if (!m_source.readRows(m_first + row, count, image.bytesPerLine(),
                                       image.scanLine(row), image.sizeInBytes() - row * rowBytes)) {
                    fail(error);
                    return {};
                }
            }
            return image;
        }
        const uchar* pixels = !m_backing.isNull()
                                  ? m_backing.constScanLine(m_first)
                                  : reinterpret_cast<const uchar*>(m_pixels.constData());
        for (int row = 0; row < rect.height(); ++row) {
            if (isCancelled(cancelled, error))
                return {};
            if (!m_file.isOpen()) {
                std::memcpy(image.scanLine(row),
                            pixels + qsizetype(row) * m_stride + qsizetype(rect.x()) * 4,
                            static_cast<std::size_t>(rowBytes));
            } else if (!m_file.seek(qint64(row) * m_stride + qint64(rect.x()) * 4) ||
                       m_file.read(reinterpret_cast<char*>(image.scanLine(row)), rowBytes) !=
                           rowBytes) {
                fail(error);
                return {};
            }
        }
        return image;
    }

  private:
    QImage m_backing;
    ScreenshotImageRowSource m_source;
    QByteArray m_pixels;
    QTemporaryFile m_file;
    qsizetype m_stride = 0;
    int m_first = 0;
};
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
    bool isOk() const {
        return m_ok;
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
                                 QString* error, bool verifiedOpaque, const Progress& progress) {
    if (!source.isValid() ||
        ((source.size.width() > 1000000 || source.size.height() > 1000000) &&
         !source.reserveResources) ||
        qint64(source.size.width()) * source.size.height() > qint64{2} * 1024 * 1024 * 1024) {
        fail(error);
        return {};
    }
    // Account for one raster tile, RGB/alpha buffers plus their compression copies, and the
    // bounded band cache. Compressed payloads live on disk and are not retained in memory.
    const qint64 bandBytes = qint64(source.size.width()) * qMin(2048, source.size.height()) * 4;
    const qint64 bandWorking = bandBytes <= 32 * 1024 * 1024
                                   ? bandBytes
                                   : qMax(qint64{1024 * 1024}, qint64(source.size.width()) * 4);
    const qint64 tilePixels =
        qint64(qMin(2048, source.size.width())) * qMin(2048, source.size.height());
    std::shared_ptr<void> resources;
    if (source.reserveResources) {
        resources =
            source.reserveResources(0, bandWorking + tilePixels * 12 + 2 * 1024 * 1024, error);
        if (!resources)
            return {};
    }
    if (progress)
        progress(0);
    auto result = std::make_shared<Payload>();
    result->size = source.size;
    result->quality = qBound(1, quality, 100);
    QFile file(result->path());
    if (!result->directory.isValid() || !file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    for (int y = 0; y < source.size.height();) {
        if (isCancelled(source.cancellationRequested, error))
            return {};
        RasterBand band;
        const int bandRows = qMin(2048, source.size.height() - y);
        if (!band.load(source, y, bandRows, result->directory.path(), error))
            return {};
        for (int x = 0; x < source.size.width();) {
            if (isCancelled(source.cancellationRequested, error))
                return {};
            const QRect rect(x, y, qMin(2048, source.size.width() - x),
                             qMin(2048, source.size.height() - y));
            QImage image = band.tile(rect, error, source.cancellationRequested);
            if (image.isNull()) {
                if (!error || error->isEmpty())
                    fail(error);
                return {};
            }
            QByteArray color;
            QByteArray alpha;
            if (result->quality == 100) {
                color.resize(qsizetype(rect.width()) * rect.height() * 3);
                if (!verifiedOpaque)
                    alpha.resize(qsizetype(rect.width()) * rect.height());
                bool opaque = true;
                for (int row = 0; row < rect.height(); ++row) {
                    if ((row & 63) == 0 && isCancelled(source.cancellationRequested, error))
                        return {};
                    const uchar* pixels = image.constScanLine(row);
                    for (int column = 0; column < rect.width(); ++column) {
                        const qsizetype index = qsizetype(row) * rect.width() + column;
                        std::memcpy(color.data() + index * 3, pixels + column * 4, 3);
                        if (!verifiedOpaque) {
                            alpha[index] = char(pixels[column * 4 + 3]);
                            opaque = opaque && pixels[column * 4 + 3] == 255;
                        }
                    }
                }
                color = qCompress(color).mid(4);
                alpha = opaque ? QByteArray{} : qCompress(alpha).mid(4);
            } else {
                // Flatten explicitly, including hidden RGB, before the one JPEG encode.
                for (int row = 0; !verifiedOpaque && row < rect.height(); ++row) {
                    if ((row & 63) == 0 && isCancelled(source.cancellationRequested, error))
                        return {};
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
            if (progress) {
                const qint64 completed =
                    qint64(y) * source.size.width() + qint64(x + rect.width()) * rect.height();
                progress(qreal(completed) / (qreal(source.size.width()) * source.size.height()));
            }
            x += rect.width();
        }
        y += bandRows;
    }
    if (!file.flush()) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (isCancelled(source.cancellationRequested, error))
        return {};
    return result;
}

bool writeDocument(qsizetype pageCount, const PageProducer& produce, QIODevice* output,
                   const ScreenshotPdfOptions& options, QString* error, const Cancelled& cancelled,
                   const DocumentProgress& progress) {
    if (!output || !output->isWritable() || !produce || pageCount <= 0 ||
        pageCount > (std::numeric_limits<int>::max() - 4) / 2)
        return fail(error);
    struct WrittenPage {
        QSizeF size;
        double unit = 1;
        QByteArray resources;
        int contents = 0;
    };
    QList<WrittenPage> written;
    Writer writer(output);
    if (!writer.put("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"))
        return fail(error);
    for (qsizetype index = 0; index < pageCount; ++index) {
        if (isCancelled(cancelled, error))
            return false;
        Page prepared;
        if (!produce(index, &prepared, error))
            return false;
        const auto& geometry = prepared.geometry;
        const auto positive = [](qreal value) { return std::isfinite(value) && value > 0; };
        const auto validRect = [&positive](const QRectF& rect) {
            return positive(rect.width()) && positive(rect.height()) &&
                   std::isfinite(rect.left()) && std::isfinite(rect.top()) &&
                   std::isfinite(rect.right()) && std::isfinite(rect.bottom());
        };
        if (!positive(geometry.pagePoints.width()) || !positive(geometry.pagePoints.height()))
            return fail(error);
        if (prepared.layers.isEmpty() && prepared.payload)
            prepared.layers.append({prepared.payload, geometry.imagePoints, std::nullopt});
        if (prepared.layers.isEmpty())
            return fail(error);
        qsizetype totalTiles = 0;
        for (const auto& layer : prepared.layers) {
            const QRectF bounds = layer.clipPoints.value_or(layer.imagePoints);
            if (!layer.payload || layer.payload->size.isEmpty() || layer.payload->tiles.isEmpty() ||
                !validRect(layer.imagePoints) || !validRect(bounds) || bounds.left() < -1e-7 ||
                bounds.top() < -1e-7 || bounds.right() > geometry.pagePoints.width() + 1e-7 ||
                bounds.bottom() > geometry.pagePoints.height() + 1e-7 ||
                layer.payload->tiles.size() > std::numeric_limits<qsizetype>::max() - totalTiles)
                return fail(error);
            totalTiles += layer.payload->tiles.size();
        }
        WrittenPage page;
        page.unit =
            qMax(1.0, std::ceil(qMax(geometry.pagePoints.width(), geometry.pagePoints.height()) /
                                14400.0));
        if (page.unit > 75000)
            return fail(error);
        page.size = geometry.pagePoints / page.unit;
        QByteArray content = "1 1 1 rg\n0 0 " + number(page.size.width()) + ' ' +
                             number(page.size.height()) + " re f\n";
        qsizetype tileIndex = 0;
        if (progress)
            progress(index, 0);
        for (const auto& layer : prepared.layers) {
            if (isCancelled(cancelled, error))
                return false;
            const auto& payload = *layer.payload;
            QFile input(payload.path());
            if (!input.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = input.errorString();
                return false;
            }
            const double scaleX = layer.imagePoints.width() / payload.size.width() / page.unit;
            const double scaleY = layer.imagePoints.height() / payload.size.height() / page.unit;
            content += "q\n";
            if (layer.clipPoints) {
                const auto& clip = *layer.clipPoints;
                content += number(clip.left() / page.unit) + ' ' +
                           number(page.size.height() - clip.bottom() / page.unit) + ' ' +
                           number(clip.width() / page.unit) + ' ' +
                           number(clip.height() / page.unit) + " re W n\n";
            }
            for (const Tile& tile : payload.tiles) {
                if (isCancelled(cancelled, error))
                    return false;
                const double left = layer.imagePoints.left() / page.unit + tile.rect.x() * scaleX;
                const double top = layer.imagePoints.top() / page.unit + tile.rect.y() * scaleY;
                const QSizeF tileSize(tile.rect.width() * scaleX, tile.rect.height() * scaleY);
                // Fully clipped tiles need no PDF objects or disk reads. Advance progress even
                // when an image has been moved entirely outside its cell.
                if (layer.clipPoints &&
                    !QRectF(QPointF(left, top), tileSize)
                         .intersects(QRectF(layer.clipPoints->topLeft() / page.unit,
                                            layer.clipPoints->size() / page.unit))) {
                    if (progress)
                        progress(index, qreal(++tileIndex) / qreal(totalTiles));
                    continue;
                }
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
                    mask = writer.stream("/Type /XObject /Subtype /Image /ColorSpace /DeviceGray "
                                         "/Filter /FlateDecode" +
                                             dimensions,
                                         alpha);
                const int id = writer.stream(
                    "/Type /XObject /Subtype /Image /ColorSpace /DeviceRGB /Interpolate false "
                    "/Filter " +
                        QByteArray(payload.quality == 100 ? "/FlateDecode" : "/DCTDecode") +
                        dimensions +
                        (mask ? " /SMask " + QByteArray::number(mask) + " 0 R" : QByteArray{}),
                    color);
                const QByteArray name = "/Im" + QByteArray::number(id);
                page.resources += name + ' ' + QByteArray::number(id) + " 0 R ";
                const double bottom = page.size.height() - top - tileSize.height();
                content += "q\n" + number(tileSize.width()) + " 0 0 " + number(tileSize.height()) +
                           ' ' + number(left) + ' ' + number(bottom) + " cm\n" + name + " Do\nQ\n";
                if (!writer.isOk())
                    return fail(error);
                if (progress)
                    progress(index, qreal(++tileIndex) / qreal(totalTiles));
            }
            content += "Q\n";
        }
        page.contents = writer.stream({}, content);
        written.append(std::move(page));
        // 'prepared' owns only this page's staging files, which are retired here.
    }
    if (writer.nextId() > std::numeric_limits<int>::max() - pageCount - 3)
        return fail(error);
    const int pagesId = writer.nextId() + static_cast<int>(pageCount);
    QByteArray kids;
    for (const auto& page : written) {
        if (isCancelled(cancelled, error))
            return false;
        const int pageId = writer.object(
            "<< /Type /Page /Parent " + QByteArray::number(pagesId) + " 0 R /MediaBox [0 0 " +
            number(page.size.width()) + ' ' + number(page.size.height()) + "] /UserUnit " +
            number(page.unit) + " /Resources << /XObject << " + page.resources +
            ">> >> /Contents " + QByteArray::number(page.contents) + " 0 R >>");
        kids += QByteArray::number(pageId) + " 0 R ";
    }
    const int pages = writer.object("<< /Type /Pages /Count " + QByteArray::number(pageCount) +
                                    " /Kids [" + kids + "] >>");
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

bool write(const Payload& payload, QIODevice* output, const ScreenshotPdfOptions& options,
           QString* error, const Cancelled& cancelled) {
    return writeDocument(
        1,
        [&payload, &options](qsizetype, Page* page, QString*) {
            page->payload = std::shared_ptr<const Payload>(&payload, [](const Payload*) {});
            page->geometry = layout(payload.size, options.pageSize);
            return true;
        },
        output, options, error, cancelled);
}

bool decodeTiles(const Payload& payload, const std::function<bool(QRect, const QImage&)>& consume,
                 QString* error, const Cancelled& cancelled, QRect region) {
    QFile input(payload.path());
    if (!input.open(QIODevice::ReadOnly))
        return fail(error);
    for (const Tile& tile : payload.tiles) {
        if (isCancelled(cancelled, error))
            return false;
        if (!region.isEmpty() && !tile.rect.intersects(region))
            continue;
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
            image = snowCanvasConvertImage(image, QImage::Format_RGBA8888);
        } else {
            const qsizetype count = qsizetype(tile.rect.width()) * tile.rect.height();
            color = inflate(color, count * 3);
            if (!alpha.isEmpty())
                alpha = inflate(alpha, count);
            if (color.size() != count * 3 || (tile.alphaBytes && alpha.size() != count))
                return fail(error);
            image = snowCanvasAllocateImage(tile.rect.size(), QImage::Format_RGBA8888);
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
