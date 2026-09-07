#include "screenshotsaveexportpipeline.h"
#include "snowimagecodecbridge.h"
#include "snowimageqtcodec.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <QStorageInfo>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace screenshot_save_export {
namespace {
constexpr qint64 kMaximumRasterBytes = qint64{8} * 1024 * 1024 * 1024;
int32_t SNOW_SHOT_IMAGE_CODEC_CALL cancelled(void* context) {
    return static_cast<const ScreenshotExportCancellation*>(context)->isCancellationRequested() ? 1
                                                                                                : 0;
}
uint32_t bridgeFormat(ScreenshotImageFileFormat format) {
    switch (format) {
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
    result.cancellationRequested = [sourceCancellation, &cancellation] {
        return cancellation.isCancellationRequested() ||
               (sourceCancellation && sourceCancellation());
    };
    return result;
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
    if (hasBacking) {
        for (int first = 0; first < rows.size.height(); first += 64) {
            if (cancellation.isCancellationRequested())
                return {};
            const int count = qMin(64, rows.size.height() - first);
            classifyAlpha(rows.backingImage.constScanLine(first), rows.size.width(), count,
                          rows.backingImage.bytesPerLine(), &alpha);
        }
        QImage preview = size == rows.size ? rows.backingImage
                                           : rows.backingImage.scaled(size, Qt::IgnoreAspectRatio,
                                                                      Qt::SmoothTransformation);
        return preview.isNull() ? Source{} : Source{rows, std::move(preview), alpha};
    }
    if (size == rows.size) {
        QImage preview(size, QImage::Format_RGBA8888);
        if (preview.isNull() || !rows.readRows(0, size.height(), preview.bytesPerLine(),
                                               preview.bits(), preview.sizeInBytes())) {
            *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                 "The screenshot pixels could not be read");
            return {};
        }
        classifyAlpha(preview.constBits(), preview.width(), preview.height(),
                      preview.bytesPerLine(), &alpha);
        preview.setColorSpace(QColorSpace::SRgb);
        return {rows, preview, alpha};
    }
    QImage preview(size, QImage::Format_RGBA8888_Premultiplied);
    if (preview.isNull())
        return {};
    // Retain the immutable row source. Downsample in bounded strips without materializing
    // a scrolling capture or keeping a second full-resolution pixel buffer.
    std::vector<quint64> sums(size_t(size.width()) * 4);
    for (int y = 0; y < size.height(); ++y) {
        std::fill(sums.begin(), sums.end(), 0);
        const int first = int(qint64(y) * rows.size.height() / size.height());
        const int end = int(qint64(y + 1) * rows.size.height() / size.height());
        for (int row = first; row < end; row += 64) {
            if (cancellation.isCancellationRequested())
                return {};
            const int count = qMin(64, end - row);
            QImage strip(rows.size.width(), count, QImage::Format_RGBA8888);
            if (strip.isNull() || !rows.readRows(row, count, strip.bytesPerLine(), strip.bits(),
                                                 strip.sizeInBytes())) {
                *error = QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                                     "The screenshot pixels could not be read");
                return {};
            }
            classifyAlpha(strip.constBits(), strip.width(), strip.height(), strip.bytesPerLine(),
                          &alpha);
            strip = strip.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            if (strip.width() != size.width())
                strip = strip.scaled(size.width(), count, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            for (int line = 0; line < count; ++line) {
                const uchar* pixels = strip.constScanLine(line);
                for (size_t x = 0; x < sums.size(); ++x)
                    sums[x] += pixels[x];
            }
        }
        uchar* pixels = preview.scanLine(y);
        for (size_t x = 0; x < sums.size(); ++x)
            pixels[x] = uchar((sums[x] + (end - first) / 2) / (end - first));
    }
    preview = preview.convertToFormat(QImage::Format_RGBA8888);
    preview.setColorSpace(QColorSpace::SRgb);
    return {rows, preview, alpha};
}
QSize encoderLimits(ScreenshotImageFileFormat format) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (!snow_shot_image_codec_export_limits(bridgeFormat(format), &width, &height))
        return {};
    return {int(qMin(width, uint32_t(std::numeric_limits<int>::max() / 4))),
            int(qMin(height, uint32_t(std::numeric_limits<int>::max() / 4)))};
}
ScreenshotSaveExportOptions normalizedOptions(ScreenshotSaveExportOptions options) {
    const bool supportsQuality = options.format != ScreenshotImageFileFormat::Png &&
                                 options.format != ScreenshotImageFileFormat::Bmp;
    options.quality = supportsQuality ? qBound(1, options.quality, 100) : 100;
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

    auto raster = MappedRaster::create(size, error);
    if (!raster)
        return {};
    const ScreenshotImageRowSource input = withCancellation(source.rows, cancellation);
    const qsizetype stride = static_cast<qsizetype>(size.width()) * 4;
    if (!snow_shot::image_codec::resizeToRgba8(input, size, raster->pixels, stride,
                                               stride * size.height(), error)) {
        return {};
    }
    if (cancellation.isCancellationRequested())
        return {};
    result->alphaContent = snow::image::AlphaContent::opaque;
    classifyAlpha(raster->pixels, size.width(), size.height(), stride, &result->alphaContent);
    result->rows = raster->rows(source.rows.cancellationRequested);
    result->exactImage = result->rows.backingImage;
    return result;
}

std::shared_ptr<Encoded> render(std::shared_ptr<PreparedPixels> pixels,
                                const ScreenshotSaveExportOptions& options,
                                const ScreenshotExportCancellation& cancellation, QString* error) {
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
    ScreenshotImageRowSource rows = withCancellation(result->pixels->rows, cancellation);
    snow::image::EncodeOptions encodeOptions =
        ScreenshotImageFileService::encodeOptions(result->options.format, result->options.quality);
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
} // namespace screenshot_save_export
