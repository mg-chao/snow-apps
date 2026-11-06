#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "screenshotsaveexportpipeline.h"
#include "snowimagecodecbridge.h"
#include "snowimageqtcodec.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <QStorageInfo>
#include <QPainter>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace screenshot_save_export {
namespace {
constexpr qint64 kMaximumRasterBytes = qint64{8} * 1024 * 1024 * 1024;
constexpr int kPreviewExtent = 2048;
constexpr qint64 kMaximumPreviewBytes = qint64{kPreviewExtent} * kPreviewExtent * 4;
constexpr qint64 kReaderCacheBytes = qint64{2} * 1024 * 1024;
int32_t SNOW_SHOT_IMAGE_CODEC_CALL cancelled(void* context) {
    return static_cast<const ScreenshotExportCancellation*>(context)->isCancellationRequested() ? 1
                                                                                                : 0;
}
uint32_t bridgeFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Pdf:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
    case ScreenshotImageFileFormat::Png:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    case ScreenshotImageFileFormat::Jpeg:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG;
    case ScreenshotImageFileFormat::Bmp:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_BMP;
    case ScreenshotImageFileFormat::Webp:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP;
    case ScreenshotImageFileFormat::Jxl:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL;
    case ScreenshotImageFileFormat::Avif:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF;
    case ScreenshotImageFileFormat::Tiff:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_TIFF;
    }
    return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
}

void classifyAlpha(const uchar* pixels, int width, int height, qsizetype stride,
                   snow::image::AlphaContent* classification) {
    if (*classification == snow::image::AlphaContent::non_opaque)
        return;
    for (int row = 0; row < height; ++row) {
        const uchar* line = pixels + static_cast<qsizetype>(row) * stride;
        for (int column = 0; column < width; ++column) {
            if (line[static_cast<qsizetype>(column) * 4 + 3] != 255) {
                *classification = snow::image::AlphaContent::non_opaque;
                return;
            }
        }
    }
}

ScreenshotImageRowSource withCancellation(const ScreenshotImageRowSource& source,
                                          const ScreenshotExportCancellation& cancellation) {
    ScreenshotImageRowSource result = source;
    const auto sourceCancellation = source.cancellationRequested;
    result.cancellationRequested = [sourceCancellation, cancellation] {
        return cancellation.isCancellationRequested() ||
               (sourceCancellation && sourceCancellation());
    };
    return result;
}

bool reserveResources(const ScreenshotImageRowSource& rows, qint64 retained, qint64 scratch,
                      std::shared_ptr<void>* owner, QString* error,
                      std::shared_ptr<void>* temporary = nullptr) {
    if (!rows.reserveResources)
        return true;
    if (temporary) {
        if (retained > 0) {
            *owner = rows.reserveResources(retained, 0, error);
            if (!*owner)
                return false;
        }
        if (scratch > 0) {
            *temporary = rows.reserveResources(0, scratch, error);
            if (!*temporary)
                return false;
        }
        return true;
    }
    *owner = rows.reserveResources(retained, scratch, error);
    return static_cast<bool>(*owner);
}

QImage retainResources(QImage image, std::shared_ptr<void> owner) {
    if (image.isNull() || !owner)
        return image;
    struct Storage {
        std::shared_ptr<void> owner;
        QImage image;
    };
    auto storage = std::make_unique<Storage>(Storage{std::move(owner), std::move(image)});
    const auto& pixels = storage->image;
    QImage result(
        pixels.constBits(), pixels.width(), pixels.height(), pixels.bytesPerLine(), pixels.format(),
        [](void* context) { delete static_cast<Storage*>(context); }, storage.get());
    result.setColorSpace(pixels.colorSpace());
    result.setDevicePixelRatio(pixels.devicePixelRatio());
    if (!result.isNull())
        storage.release();
    return result;
}

ScreenshotImageRowSource retainResources(ScreenshotImageRowSource rows,
                                         const std::shared_ptr<void>& owner) {
    if (!owner)
        return rows;
    struct Storage {
        std::shared_ptr<void> owner;
        ScreenshotImageRowSource source;
    };
    auto storage = std::make_shared<Storage>(Storage{owner, rows});
    rows.readRows = [storage](int first, int count, qsizetype stride, uchar* target,
                              qsizetype capacity) {
        return storage->source.readRows(first, count, stride, target, capacity);
    };
    if (rows.readRegion)
        rows.readRegion = [storage](QRect region, qsizetype stride, uchar* target,
                                    qsizetype capacity) {
            return storage->source.readRegion(region, stride, target, capacity);
        };
    return rows;
}

QSize previewSize(QSize size) {
    if (size.width() > kPreviewExtent || size.height() > kPreviewExtent)
        size.scale(kPreviewExtent, kPreviewExtent, Qt::KeepAspectRatio);
    return size.expandedTo(QSize(1, 1));
}

QSize detailSize(QSize size) {
    constexpr qint64 maximumPixels = kMaximumPreviewBytes / 4;
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels <= maximumPixels)
        return size;
    const double scale = std::sqrt(double(maximumPixels) / double(pixels));
    QSize result(qMax(1, int(size.width() * scale)), qMax(1, int(size.height() * scale)));
    // An extremely narrow region can round its short axis up to one pixel.
    if (qint64(result.width()) * result.height() > maximumPixels) {
        if (result.width() >= result.height())
            result.setWidth(int(maximumPixels / result.height()));
        else
            result.setHeight(int(maximumPixels / result.width()));
    }
    return result;
}

// Consume ordered rows without retaining the decoded frame. Alpha is averaged in
// premultiplied space; native-size regions preserve every RGBA byte, including hidden RGB.
class PreviewRows final {
  public:
    explicit PreviewRows(QRect region, snow::image::AlphaContent* alpha = nullptr,
                         bool detail = false)
        : m_region(region), m_alpha(alpha), m_expected(region.top()) {
        const QSize size = detail ? detailSize(region.size()) : previewSize(region.size());
        m_native = size == region.size();
        m_image = snowCanvasAllocateImage(size, m_native ? QImage::Format_RGBA8888
                                                         : QImage::Format_RGBA8888_Premultiplied);
        if (!m_native)
            m_sums.resize(size_t(size.width()) * 4);
    }
    bool valid() const {
        return !m_image.isNull() && !m_region.isEmpty();
    }
    uchar* storage(QSize size) {
        return m_native && m_region == QRect(QPoint{}, size) ? m_image.bits() : nullptr;
    }
    bool consume(int first, int count, qsizetype stride, const uchar* pixels) {
        if (!valid() || !pixels)
            return false;
        const int start = qMax(first, m_region.top());
        const int end = qMin(first + count, m_region.bottom() + 1);
        if (start >= end)
            return true;
        if (start != m_expected)
            return false;
        const int stripRows =
            int(qMax(qsizetype{1}, qsizetype{1024 * 1024} / (qsizetype(m_region.width()) * 4)));
        for (int row = start; row < end; row += stripRows) {
            const int rows = qMin(stripRows, end - row);
            const uchar* input =
                pixels + qsizetype(row - first) * stride + qsizetype(m_region.x()) * 4;
            if (m_alpha)
                classifyAlpha(input, m_region.width(), rows, stride, m_alpha);
            if (m_native) {
                for (int offset = 0; offset < rows; ++offset) {
                    uchar* target = m_image.scanLine(row + offset - m_region.top());
                    const uchar* source = input + qsizetype(offset) * stride;
                    if (target != source)
                        std::memcpy(target, source, size_t(m_region.width()) * 4);
                }
            } else {
                QImage strip(input, m_region.width(), rows, stride, QImage::Format_RGBA8888);
                strip = snowCanvasConvertImage(strip, QImage::Format_RGBA8888_Premultiplied);
                if (strip.width() != m_image.width())
                    strip = snowCanvasScaleImage(strip, QSize(m_image.width(), rows),
                                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                if (strip.isNull())
                    return false;
                for (int offset = 0; offset < rows; ++offset) {
                    const uchar* inputRow = strip.constScanLine(offset);
                    for (size_t x = 0; x < m_sums.size(); ++x)
                        m_sums[x] += inputRow[x];
                    ++m_accumulated;
                    const int localEnd = row + offset + 1 - m_region.top();
                    if (localEnd ==
                        int(qint64(m_outputRow + 1) * m_region.height() / m_image.height())) {
                        uchar* output = m_image.scanLine(m_outputRow++);
                        for (size_t x = 0; x < m_sums.size(); ++x) {
                            output[x] = uchar((m_sums[x] + quint64(m_accumulated / 2)) /
                                              quint64(m_accumulated));
                            m_sums[x] = 0;
                        }
                        m_accumulated = 0;
                    }
                }
            }
        }
        m_expected = end;
        return true;
    }
    QImage finish() {
        if (m_expected != m_region.bottom() + 1)
            return {};
        if (!m_native)
            m_image = snowCanvasConvertImage(m_image, QImage::Format_RGBA8888);
        m_image.setColorSpace(QColorSpace::SRgb);
        return std::move(m_image);
    }

  private:
    QRect m_region;
    snow::image::AlphaContent* m_alpha;
    QImage m_image;
    std::vector<quint64> m_sums;
    int m_expected;
    int m_outputRow = 0;
    int m_accumulated = 0;
    bool m_native;
};

QImage readRegionRows(const ScreenshotImageRowSource& rows, QRect region,
                      const ScreenshotExportCancellation& cancellation, QString* error,
                      snow::image::AlphaContent* alpha = nullptr, bool detail = false) {
    if (!rows.isValid() || region.isEmpty() || !QRect(QPoint{}, rows.size).contains(region) ||
        cancellation.isCancellationRequested())
        return {};
    const bool backing = !rows.backingImage.isNull() && rows.backingImage.size() == rows.size &&
                         rows.backingImage.format() == QImage::Format_RGBA8888;
    const bool cropped = !backing && static_cast<bool>(rows.readRegion);
    const qsizetype stride = backing ? rows.backingImage.bytesPerLine()
                                     : qsizetype(cropped ? region.width() : rows.size.width()) * 4;
    const QSize outputSize = detail ? detailSize(region.size()) : previewSize(region.size());
    const qint64 outputBytes = qint64(outputSize.width()) * outputSize.height() * 4;
    std::shared_ptr<void> owner;
    std::shared_ptr<void> temporary;
    if (!reserveResources(rows, outputBytes,
                          outputBytes + 3 * qMax(qint64(stride), qint64{1024 * 1024}) +
                              qint64{8} * 1024 * 1024 +
                              (cropped ? qint64(rows.size.width()) * 4 : 0) +
                              qint64(outputSize.width()) * 4 * qint64(sizeof(quint64)),
                          &owner, error, &temporary))
        return {};
    PreviewRows result(cropped ? QRect(0, region.y(), region.width(), region.height()) : region,
                       alpha, detail);
    if (!result.valid())
        return {};
    const int stripRows = int(qMax(qsizetype{1}, qsizetype{1024 * 1024} / stride));
    QImage strip;
    if (!backing) {
        strip = snowCanvasAllocateImage(
            QSize(cropped ? region.width() : rows.size.width(), qMin(stripRows, region.height())),
            QImage::Format_RGBA8888);
        if (strip.isNull())
            return {};
    }
    for (int first = region.top(); first <= region.bottom(); first += stripRows) {
        if (cancellation.isCancellationRequested() ||
            (rows.cancellationRequested && rows.cancellationRequested()))
            return {};
        const int count = qMin(stripRows, region.bottom() + 1 - first);
        const uchar* pixels = backing ? rows.backingImage.constScanLine(first) : strip.constBits();
        const bool read =
            backing ||
            (cropped ? rows.readRegion(QRect(region.x(), first, region.width(), count), stride,
                                       strip.bits(), strip.sizeInBytes())
                     : rows.readRows(first, count, stride, strip.bits(), strip.sizeInBytes()));
        if (!read || !result.consume(first, count, stride, pixels)) {
            if (error)
                *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                     "The screenshot pixels could not be read");
            return {};
        }
    }
    return retainResources(result.finish(), std::move(owner));
}

struct DecodePreviewContext final {
    const ScreenshotExportCancellation& cancellation;
    ScreenshotImageFileFormat format;
    QRect requested;
    bool detail;
    QString* error;
    QSize frameSize;
    std::unique_ptr<PreviewRows> rows;
    std::shared_ptr<MappedRaster> storage;
};

int32_t SNOW_SHOT_IMAGE_CODEC_CALL beginPreview(void* raw, uint32_t width, uint32_t height) {
    auto& context = *static_cast<DecodePreviewContext*>(raw);
    if (width > uint32_t(std::numeric_limits<int>::max() / 4) ||
        height > uint32_t(std::numeric_limits<int>::max()))
        return 0;
    context.frameSize = QSize(int(width), int(height));
    const QRect bounds(QPoint{}, context.frameSize);
    const QRect region = context.requested.isEmpty() ? bounds : context.requested;
    if (!bounds.contains(region))
        return 0;
    context.rows = std::make_unique<PreviewRows>(region, nullptr, context.detail);
    return context.rows->valid() ? 1 : 0;
}
uint8_t* SNOW_SHOT_IMAGE_CODEC_CALL previewStorage(void* raw, uint64_t stride, uint64_t bytes) {
    auto& context = *static_cast<DecodePreviewContext*>(raw);
    if (auto* direct = context.rows->storage(context.frameSize))
        return direct;
    if (context.format != ScreenshotImageFileFormat::Jpeg &&
        context.format != ScreenshotImageFileFormat::Webp &&
        context.format != ScreenshotImageFileFormat::Jxl)
        return nullptr;
    if (stride != uint64_t(context.frameSize.width()) * 4 ||
        bytes != stride * uint64_t(context.frameSize.height()))
        return nullptr;
    // Buffer-based decoders need complete storage during decoding, but the viewport
    // retains only its bounded image after this synchronous call retires.
    context.storage = MappedRaster::create(context.frameSize, context.error);
    return context.storage ? context.storage->pixels : nullptr;
}
int32_t SNOW_SHOT_IMAGE_CODEC_CALL consumePreview(void* raw, uint32_t first, uint32_t count,
                                                  uint64_t stride, const uint8_t* pixels,
                                                  uint64_t) {
    auto& context = *static_cast<DecodePreviewContext*>(raw);
    if (context.cancellation.isCancellationRequested() ||
        stride > uint64_t(std::numeric_limits<qsizetype>::max()))
        return 0;
    return context.rows->consume(int(first), int(count), qsizetype(stride), pixels) ? 1 : 0;
}
int32_t SNOW_SHOT_IMAGE_CODEC_CALL previewCancelled(void* raw) {
    return static_cast<DecodePreviewContext*>(raw)->cancellation.isCancellationRequested() ? 1 : 0;
}

QImage decodeDisplay(const Encoded& encoded, QRect region, bool initialPreview,
                     const ScreenshotExportCancellation& cancellation, QString* error) {
    if (cancellation.isCancellationRequested())
        return {};
    const QSize dimensions = region.isEmpty() ? encoded.options.size : region.size();
    const QSize size = initialPreview ? previewSize(dimensions) : detailSize(dimensions);
    const qint64 outputBytes = qint64(size.width()) * size.height() * 4;
    qint64 storageBytes = 0;
    if (encoded.pdf) {
        for (const auto& tile : encoded.pdf->tiles)
            if (region.isEmpty() || tile.rect.intersects(region))
                storageBytes =
                    qMax(storageBytes, qint64(tile.rect.width()) * tile.rect.height() * 12);
    } else if (encoded.options.format == ScreenshotImageFileFormat::Jpeg ||
               encoded.options.format == ScreenshotImageFileFormat::Webp ||
               encoded.options.format == ScreenshotImageFileFormat::Jxl ||
               encoded.options.format == ScreenshotImageFileFormat::Avif) {
        storageBytes = qint64(encoded.options.size.width()) * encoded.options.size.height() * 4;
        if (initialPreview && (encoded.options.format == ScreenshotImageFileFormat::Jpeg ||
                               encoded.options.format == ScreenshotImageFileFormat::Webp))
            storageBytes = qMin(storageBytes, kMaximumPreviewBytes);
        if (encoded.options.format == ScreenshotImageFileFormat::Avif)
            storageBytes *= 2;
    }
    std::shared_ptr<void> owner;
    std::shared_ptr<void> temporary;
    const qint64 rowWorkingBytes =
        3 * qMax(qint64(encoded.options.size.width()) * 4, qint64{1024 * 1024}) +
        qint64(size.width()) * 4 * qint64(sizeof(quint64));
    if (encoded.pixels &&
        !reserveResources(encoded.pixels->rows, outputBytes,
                          storageBytes + outputBytes + rowWorkingBytes + qint64{16} * 1024 * 1024,
                          &owner, error, &temporary))
        return {};
    if (encoded.pdf) {
        if (region.isEmpty())
            region = QRect(QPoint{}, encoded.options.size);
        const bool native = size == region.size();
        QImage image = snowCanvasAllocateImage(
            size, native ? QImage::Format_RGBA8888 : QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull())
            return {};
        image.fill(Qt::transparent);
        QPainter painter;
        if (!native) {
            painter.begin(&image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
        }
        const qreal scaleX = qreal(image.width()) / region.width();
        const qreal scaleY = qreal(image.height()) / region.height();
        const bool decoded = screenshot_pdf::decodeTiles(
            *encoded.pdf,
            [&](QRect rect, const QImage& tile) {
                if (native) {
                    const QRect intersection = rect.intersected(region);
                    for (int row = intersection.top(); row <= intersection.bottom(); ++row)
                        std::memcpy(image.scanLine(row - region.top()) +
                                        qsizetype(intersection.left() - region.left()) * 4,
                                    tile.constScanLine(row - rect.top()) +
                                        qsizetype(intersection.left() - rect.left()) * 4,
                                    size_t(intersection.width()) * 4);
                } else {
                    painter.drawImage(QRectF((rect.x() - region.x()) * scaleX,
                                             (rect.y() - region.y()) * scaleY,
                                             rect.width() * scaleX, rect.height() * scaleY),
                                      tile);
                }
                return !cancellation.isCancellationRequested();
            },
            error, [&cancellation] { return cancellation.isCancellationRequested(); }, region);
        if (painter.isActive())
            painter.end();
        if (!decoded)
            return {};
        image = snowCanvasConvertImage(image, QImage::Format_RGBA8888);
        image.setColorSpace(QColorSpace::SRgb);
        return retainResources(std::move(image), std::move(owner));
    }
    DecodePreviewContext context{
        cancellation, encoded.options.format, region, !initialPreview, error, {}, {}, {}};
    const SnowShotImageCodecRgba8Sink sink{sizeof(SnowShotImageCodecRgba8Sink),
                                           SNOW_SHOT_IMAGE_CODEC_ABI_VERSION,
                                           &context,
                                           beginPreview,
                                           previewStorage,
                                           consumePreview,
                                           previewCancelled};
    // JPEG and WebP reduce within their decoders. JPEG XL's extent option allocates
    // a native owning raster, so keep its native decode in temporary mapped storage.
    const uint32_t extent =
        initialPreview && (encoded.options.format == ScreenshotImageFileFormat::Jpeg ||
                           encoded.options.format == ScreenshotImageFileFormat::Webp ||
                           encoded.options.format == ScreenshotImageFileFormat::Avif)
            ? 2048
            : 0;
    std::array<char, 1024> backendError{};
    if (!snow_shot_image_codec_decode_file_rgba8_stream(
            encoded.path.toUtf8().constData(), bridgeFormat(encoded.options.format), extent, &sink,
            backendError.data(), backendError.size())) {
        if (error && error->isEmpty())
            *error = QString::fromUtf8(backendError.data());
        return {};
    }
    return cancellation.isCancellationRequested() || !context.rows
               ? QImage{}
               : retainResources(context.rows->finish(), std::move(owner));
}
} // namespace

MappedRaster::~MappedRaster() {
    if (pixels)
        m_file.unmap(pixels);
}
std::shared_ptr<MappedRaster> MappedRaster::create(QSize size, QString* error) {
    if (size.isEmpty() || size.width() > std::numeric_limits<int>::max() / 4 ||
        qint64(size.width()) * size.height() > kMaximumRasterBytes / 4) {
        *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                             "The image dimensions exceed the export limit");
        return {};
    }
    const qint64 bytes = qint64(size.width()) * size.height() * 4;
    auto raster = std::make_shared<MappedRaster>();
    if (!raster->m_directory.isValid()) {
        *error = raster->m_directory.errorString();
        return {};
    }
    const QStorageInfo storage(raster->m_directory.path());
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < bytes) {
        *error =
            QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                        "There is not enough temporary disk space for this export");
        return {};
    }
    raster->size = size;
    raster->m_file.setFileName(raster->m_directory.filePath(QStringLiteral("pixels.rgba")));
    if (!raster->m_file.open(QIODevice::ReadWrite) || !raster->m_file.resize(bytes) ||
        !(raster->pixels = raster->m_file.map(0, bytes))) {
        *error = raster->m_file.errorString();
        return {};
    }
    return raster;
}
ScreenshotImageRowSource MappedRaster::rows(std::function<bool()> cancellation) const {
    ScreenshotImageRowSource result;
    result.size = size;
    result.cancellationRequested = std::move(cancellation);
    result.backingImage = image();
    const uchar* data = pixels;
    const QSize dimensions = size;
    result.readRows = [data, dimensions](int first, int count, qsizetype stride, uchar* target,
                                         qsizetype capacity) {
        const qsizetype rowBytes = qsizetype(dimensions.width()) * 4;
        if (!target || first < 0 || count < 1 || first > dimensions.height() ||
            count > dimensions.height() - first || stride < rowBytes || capacity < rowBytes ||
            count - 1 > (capacity - rowBytes) / stride)
            return false;
        for (int row = 0; row < count; ++row)
            std::memcpy(target + row * stride, data + (first + row) * rowBytes, size_t(rowBytes));
        return true;
    };
    return result;
}
QImage MappedRaster::image() const {
    // The immutable image keeps its mapping alive across worker and canvas lifetimes.
    auto owner = std::make_unique<std::shared_ptr<const MappedRaster>>(shared_from_this());
    QImage result(
        static_cast<const uchar*>(pixels), size.width(), size.height(), qsizetype(size.width()) * 4,
        QImage::Format_RGBA8888,
        [](void* context) { delete static_cast<std::shared_ptr<const MappedRaster>*>(context); },
        owner.get());
    if (!result.isNull())
        owner.release();
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
Source prepare(const ScreenshotImageRowSource& rows,
               const ScreenshotExportCancellation& cancellation, QString* error) {
    if (!rows.isValid() || cancellation.isCancellationRequested())
        return {};
    QSize size = rows.size;
    if (size.width() > 2048 || size.height() > 2048)
        size.scale(2048, 2048, Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(1, 1));
    snow::image::AlphaContent alpha = snow::image::AlphaContent::opaque;
    const bool hasBacking = !rows.backingImage.isNull() && rows.backingImage.size() == rows.size &&
                            rows.backingImage.format() == QImage::Format_RGBA8888;
    if (size != rows.size && !rows.previewImage.isNull() && rows.previewImage.width() <= 2048 &&
        rows.previewImage.height() <= 2048) {
        if (!rows.isKnownOpaque) {
            const qsizetype stride = qsizetype(rows.size.width()) * 4;
            const int batch = int(qMax(qsizetype{1}, qsizetype{1024 * 1024} / stride));
            std::shared_ptr<void> working;
            if (!reserveResources(rows, 0,
                                  qMax(qint64(stride), qint64{1024 * 1024}) + kReaderCacheBytes,
                                  &working, error))
                return {};
            QImage scratch;
            if (!hasBacking) {
                scratch = snowCanvasAllocateImage(
                    QSize(rows.size.width(), qMin(batch, rows.size.height())),
                    QImage::Format_RGBA8888);
                if (scratch.isNull())
                    return {};
            }
            for (int first = 0; first < rows.size.height(); first += batch) {
                if (cancellation.isCancellationRequested() ||
                    (rows.cancellationRequested && rows.cancellationRequested()))
                    return {};
                const int count = qMin(batch, rows.size.height() - first);
                if (!hasBacking && !rows.readRows(first, count, scratch.bytesPerLine(),
                                                  scratch.bits(), scratch.sizeInBytes())) {
                    *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                         "The screenshot pixels could not be read");
                    return {};
                }
                classifyAlpha(
                    hasBacking ? rows.backingImage.constScanLine(first) : scratch.constBits(),
                    rows.size.width(), count,
                    hasBacking ? rows.backingImage.bytesPerLine() : scratch.bytesPerLine(), &alpha);
                if (alpha == snow::image::AlphaContent::non_opaque)
                    break;
            }
        }
        return {rows, rows.previewImage, alpha};
    }
    if (hasBacking) {
        for (int first = 0; first < rows.size.height(); first += 64) {
            if (cancellation.isCancellationRequested())
                return {};
            const int count = qMin(64, rows.size.height() - first);
            if (!rows.isKnownOpaque)
                classifyAlpha(rows.backingImage.constScanLine(first), rows.size.width(), count,
                              rows.backingImage.bytesPerLine(), &alpha);
        }
        std::shared_ptr<void> owner;
        std::shared_ptr<void> temporary;
        const qint64 bytes = size == rows.size ? 0 : qint64(size.width()) * size.height() * 4;
        if (bytes && !reserveResources(rows, bytes,
                                       rows.backingImage.sizeInBytes() + bytes + kReaderCacheBytes,
                                       &owner, error, &temporary))
            return {};
        QImage preview = size == rows.size
                             ? rows.backingImage
                             : snowCanvasScaleImage(rows.backingImage, size, Qt::IgnoreAspectRatio,
                                                    Qt::SmoothTransformation);
        return preview.isNull() ? Source{}
                                : Source{rows, retainResources(std::move(preview), owner), alpha};
    }
    if (size == rows.size) {
        std::shared_ptr<void> owner;
        std::shared_ptr<void> temporary;
        const qint64 bytes = qint64(size.width()) * size.height() * 4;
        if (!reserveResources(rows, bytes, kReaderCacheBytes, &owner, error, &temporary))
            return {};
        QImage preview = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
        if (preview.isNull() || !rows.readRows(0, size.height(), preview.bytesPerLine(),
                                               preview.bits(), preview.sizeInBytes())) {
            *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                 "The screenshot pixels could not be read");
            return {};
        }
        if (!rows.isKnownOpaque)
            classifyAlpha(preview.constBits(), preview.width(), preview.height(),
                          preview.bytesPerLine(), &alpha);
        preview.setColorSpace(QColorSpace::SRgb);
        preview = retainResources(std::move(preview), owner);
        // The bounded preview already contains every source pixel. Retain it as exact storage
        // instead of reading the source again and decoding the lossless export for display.
        auto retainedRows = snow_shot::image_codec::srgbRowSource(preview);
        retainedRows.isKnownOpaque = alpha == snow::image::AlphaContent::opaque;
        retainedRows.cancellationRequested = rows.cancellationRequested;
        retainedRows.reserveResources = rows.reserveResources;
        return {std::move(retainedRows), preview, alpha};
    }
    QImage preview = readRegionRows(rows, QRect(QPoint{}, rows.size), cancellation, error,
                                    rows.isKnownOpaque ? nullptr : &alpha);
    return preview.isNull() ? Source{} : Source{rows, std::move(preview), alpha};
}
QSize encoderLimits(ScreenshotImageFileFormat format) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return {1000000, 1000000};
    uint32_t width = 0;
    uint32_t height = 0;
    if (!snow_shot_image_codec_export_limits(bridgeFormat(format), &width, &height))
        return {};
    return {int(qMin(width, uint32_t(std::numeric_limits<int>::max() / 4))),
            int(qMin(height, uint32_t(std::numeric_limits<int>::max() / 4)))};
}
ScreenshotSaveExportOptions normalizedOptions(ScreenshotSaveExportOptions options) {
    options.quality = ScreenshotImageFileService::supportsQuality(options.format)
                          ? qBound(0, options.quality, 100)
                          : 100;
    if (!ScreenshotImageFileService::supportsCompressionLevel(options.format))
        options.compressionLevel = ScreenshotCompressionLevel::Low;
    if (options.format != ScreenshotImageFileFormat::Pdf) {
        options.pdfPageSize = ScreenshotPdfPageSize::PortraitA4;
        options.pdfTitle.clear();
    }
    return options;
}

std::shared_ptr<PreparedPixels> preparePixels(const Source& source, QSize size,
                                              const ScreenshotExportCancellation& cancellation,
                                              QString* error) {
    if (!source.rows.isValid() || size.isEmpty() || cancellation.isCancellationRequested())
        return {};
    auto result = std::make_shared<PreparedPixels>();
    result->size = size;
    if (size == source.rows.size) {
        result->rows = source.rows;
        result->exactImage = source.rows.backingImage;
        result->alphaContent = source.alphaContent;
        return result;
    }
    if (size.width() > std::numeric_limits<int>::max() / 4 ||
        qint64(size.width()) * size.height() > kMaximumRasterBytes / 4) {
        *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                             "The image dimensions exceed the export limit");
        return {};
    }
    const qint64 bytes = qint64(size.width()) * size.height() * 4;
    const ScreenshotImageRowSource input = withCancellation(source.rows, cancellation);
    if (bytes <= kMaximumPreparedPixelCacheBytes) {
        const auto working = snow_shot::image_codec::resizeWorkingBytes(input.size, size);
        if (!working) {
            *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                 "The image dimensions exceed the export limit");
            return {};
        }
        std::shared_ptr<void> owner;
        std::shared_ptr<void> temporary;
        QString admissionError;
        if (reserveResources(source.rows, bytes, *working + kReaderCacheBytes, &owner,
                             &admissionError, &temporary)) {
            QImage pixels = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
            if (!pixels.isNull()) {
                if (!snow_shot::image_codec::resizeToRgba8(input, size, pixels.bits(),
                                                           pixels.bytesPerLine(),
                                                           pixels.sizeInBytes(), error) ||
                    cancellation.isCancellationRequested())
                    return {};
                result->alphaContent = snow::image::AlphaContent::opaque;
                if (source.alphaContent != snow::image::AlphaContent::opaque)
                    classifyAlpha(pixels.constBits(), size.width(), size.height(),
                                  pixels.bytesPerLine(), &result->alphaContent);
                if (cancellation.isCancellationRequested())
                    return {};
                pixels.setColorSpace(QColorSpace::SRgb);
                result->exactImage = retainResources(std::move(pixels), owner);
                result->rows = snow_shot::image_codec::srgbRowSource(result->exactImage);
                result->rows.cancellationRequested = source.rows.cancellationRequested;
                result->rows.reserveResources = source.rows.reserveResources;
                result->rows.isKnownOpaque =
                    result->alphaContent == snow::image::AlphaContent::opaque;
                return result;
            }
        }
        // A reusable raster is optional. Release partial cache admission before trying
        // the bounded row plan, whose working set does not include the output raster.
    }
    if (cancellation.isCancellationRequested())
        return {};
    const auto resources = snow_shot::image_codec::resizeResources(input.size, size);
    if (!resources) {
        *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                             "The image dimensions exceed the export limit");
        return {};
    }
    std::shared_ptr<void> owner;
    if (!reserveResources(source.rows, resources->retainedBytes + kReaderCacheBytes,
                          resources->scratchBytes, &owner, error))
        return {};
    auto resized = snow_shot::image_codec::resizedRowSource(input, size, error);
    if (!resized.isValid() || cancellation.isCancellationRequested())
        return {};
    result->alphaContent = source.alphaContent;
    resized.isKnownOpaque = source.alphaContent == snow::image::AlphaContent::opaque;
    result->rows = retainResources(std::move(resized), owner);
    return result;
}

std::shared_ptr<Encoded> render(std::shared_ptr<PreparedPixels> pixels,
                                const ScreenshotSaveExportOptions& options,
                                const ScreenshotExportCancellation& cancellation, QString* error,
                                std::shared_ptr<screenshot_pdf::Payload> cachedPdf,
                                snow_shot::storage::PreparedPngImage cachedPng) {
    if (!pixels || !pixels->rows.isValid() || pixels->size != options.size ||
        cancellation.isCancellationRequested()) {
        return {};
    }
    const QSize limits = encoderLimits(options.format);
    if (options.size.isEmpty() || options.size.width() > limits.width() ||
        options.size.height() > limits.height()) {
        *error = QCoreApplication::translate(
            "ScreenshotSaveAsFileDialog", "The dimensions are not supported by this image format");
        return {};
    }
    auto result = std::make_shared<Encoded>();
    if (!result->directory.isValid()) {
        *error = result->directory.errorString();
        return {};
    }
    result->options = normalizedOptions(options);
    result->pixels = std::move(pixels);
    result->path = result->directory.filePath(
        QStringLiteral("export.") + ScreenshotImageFileService::extension(options.format));
    QFile output(result->path);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = output.errorString();
        return {};
    }
    if (options.format == ScreenshotImageFileFormat::Png && cachedPng.isValid() &&
        cachedPng.pixelSize() == options.size) {
        if (output.write(cachedPng.bytes()) != cachedPng.bytes().size() || !output.flush()) {
            *error = output.errorString();
            return {};
        }
        result->codecResult.roundTrip = snow::image::PixelRoundTrip::exact;
        return cancellation.isCancellationRequested() ? nullptr : result;
    }
    ScreenshotImageRowSource rows = withCancellation(result->pixels->rows, cancellation);
    if (options.format == ScreenshotImageFileFormat::Pdf) {
        if (cachedPdf && cachedPdf->size == options.size &&
            cachedPdf->quality == result->options.quality)
            result->pdf = std::move(cachedPdf);
        else
            result->pdf = screenshot_pdf::prepare(rows, result->options.quality, error,
                                                  result->pixels->alphaContent ==
                                                      snow::image::AlphaContent::opaque);
        if (!result->pdf ||
            !screenshot_pdf::write(
                *result->pdf, &output,
                ScreenshotPdfOptions{options.pdfPageSize, result->options.quality, options.pdfTitle,
                                     QDateTime::currentDateTimeUtc()},
                error, [&cancellation] { return cancellation.isCancellationRequested(); }))
            return {};
        if (!output.flush()) {
            *error = output.errorString();
            return {};
        }
        result->codecResult.roundTrip = result->options.quality == 100
                                            ? snow::image::PixelRoundTrip::exact
                                            : snow::image::PixelRoundTrip::codec_artifact;
        return result;
    }
    snow::image::EncodeOptions encodeOptions = ScreenshotImageFileService::encodeOptions(
        result->options.format,
        ScreenshotImageEncodingOptions{result->options.quality, result->options.compressionLevel});
    encodeOptions.verified_alpha_content = result->pixels->alphaContent;
    if (!snow_shot::image_codec::encodeToDevice(
            rows, &output, ScreenshotImageFileService::snowImageFormat(result->options.format),
            encodeOptions, error, &result->codecResult)) {
        return {};
    }
    if (!output.flush()) {
        *error = output.errorString();
        return {};
    }
    output.close();
    if (cancellation.isCancellationRequested())
        return {};
    return result;
}

std::shared_ptr<Encoded> render(const Source& source, const ScreenshotSaveExportOptions& options,
                                const ScreenshotExportCancellation& cancellation, QString* error) {
    auto pixels = preparePixels(source, options.size, cancellation, error);
    return pixels ? render(std::move(pixels), options, cancellation, error) : nullptr;
}

QImage decode(const Encoded& encoded, const ScreenshotExportCancellation& cancellation,
              QString* error) {
    if (cancellation.isCancellationRequested())
        return {};
    auto decoded = MappedRaster::create(encoded.options.size, error);
    if (!decoded)
        return {};
    if (encoded.pdf) {
        if (!screenshot_pdf::decodeTiles(
                *encoded.pdf,
                [decoded](QRect rect, const QImage& tile) {
                    for (int row = 0; row < rect.height(); ++row)
                        std::memcpy(
                            decoded->pixels +
                                (qsizetype(rect.y() + row) * decoded->size.width() + rect.x()) * 4,
                            tile.constScanLine(row), size_t(rect.width()) * 4);
                    return true;
                },
                error, [&cancellation] { return cancellation.isCancellationRequested(); }))
            return {};
        return decoded->image();
    }
    std::array<char, 1024> backendError{};
    auto* context = const_cast<ScreenshotExportCancellation*>(&cancellation);
    if (!snow_shot_image_codec_decode_file_into(
            encoded.path.toUtf8().constData(), decoded->pixels, uint32_t(decoded->size.width()),
            uint32_t(decoded->size.height()), context, cancelled, backendError.data(),
            backendError.size())) {
        *error = QString::fromUtf8(backendError.data());
        return {};
    }
    return cancellation.isCancellationRequested() ? QImage{} : decoded->image();
}

QImage readRegion(const ScreenshotImageRowSource& rows, QRect region,
                  const ScreenshotExportCancellation& cancellation, QString* error) {
    return readRegionRows(rows, region, cancellation, error, nullptr, true);
}

QImage preview(const Encoded& encoded, const Source& source,
               const ScreenshotExportCancellation& cancellation, QString* error) {
    if (cancellation.isCancellationRequested())
        return {};
    if (encoded.codecResult.roundTrip == snow::image::PixelRoundTrip::exact && encoded.pixels) {
        if (encoded.options.size == source.rows.size && !source.preview.isNull())
            return source.preview;
        const QImage& exact = encoded.pixels->exactImage;
        if (!exact.isNull() && exact.width() <= kPreviewExtent && exact.height() <= kPreviewExtent)
            return exact;
        if (!source.preview.isNull()) {
            const QSize size = previewSize(encoded.options.size);
            if (source.preview.size() == size)
                return source.preview;
            std::shared_ptr<void> owner;
            std::shared_ptr<void> temporary;
            const qint64 bytes = qint64(size.width()) * size.height() * 4;
            if (!reserveResources(encoded.pixels->rows, bytes,
                                  source.preview.sizeInBytes() + bytes + kReaderCacheBytes, &owner,
                                  error, &temporary))
                return {};
            // A fit preview is approximate; native viewport rows always come from the
            // actual prepared pixels. Avoid another full resize solely for this image.
            return retainResources(snowCanvasScaleImage(source.preview, size, Qt::IgnoreAspectRatio,
                                                        Qt::SmoothTransformation),
                                   std::move(owner));
        }
        return readRegionRows(encoded.pixels->rows, QRect(QPoint{}, encoded.options.size),
                              cancellation, error);
    }
    return decodeDisplay(encoded, {}, true, cancellation, error);
}

QImage decodeRegion(const Encoded& encoded, QRect region,
                    const ScreenshotExportCancellation& cancellation, QString* error) {
    if (region.isEmpty() || !QRect(QPoint{}, encoded.options.size).contains(region))
        return {};
    if (encoded.codecResult.roundTrip == snow::image::PixelRoundTrip::exact && encoded.pixels)
        return readRegion(encoded.pixels->rows, region, cancellation, error);
    return decodeDisplay(encoded, region, false, cancellation, error);
}
} // namespace screenshot_save_export
