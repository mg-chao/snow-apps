// SPDX-License-Identifier: Apache-2.0
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <snow/memory/pixel_buffer.h>

#include <QPainter>
#include <QColorTransform>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>

namespace {
struct ImageStorageRegistry {
    std::mutex mutex;
    std::unordered_set<const void*> writableImages;
};

std::shared_ptr<ImageStorageRegistry> imageStorageRegistry() {
    static const auto registry = std::make_shared<ImageStorageRegistry>();
    return registry;
}

struct ImageStorage {
    snow::memory::PixelBuffer pixels;
    std::shared_ptr<ImageStorageRegistry> registry = imageStorageRegistry();
    const void* imageIdentity = nullptr;

    ~ImageStorage() {
        const std::lock_guard<std::mutex> lock(registry->mutex);
        registry->writableImages.erase(imageIdentity);
    }
};

void releaseImageStorage(void* owner) {
    delete static_cast<ImageStorage*>(owner);
}

bool ownsWritableStorage(QImage& image) {
    const auto registry = imageStorageRegistry();
    const std::lock_guard<std::mutex> lock(registry->mutex);
    // The public opaque data identity distinguishes a read-only external view
    // from the writable image owning the same address. No Qt private fields are
    // inspected. Cleanup unregisters it before the pixel address can be reused.
    return registry->writableImages.find(image.data_ptr()) != registry->writableImages.end();
}

void copyMetadata(const QImage& source, QImage& destination) {
    destination.setColorTable(source.colorTable());
    const QColorSpace colorSpace = source.colorSpace();
    const bool cmykProfile = colorSpace.colorModel() == QColorSpace::ColorModel::Cmyk;
    if (destination.depth() == 32 && colorSpace.isValid() &&
        cmykProfile != (destination.format() == QImage::Format_CMYK8888)) {
        // Qt carries the original profile through format-only conversions,
        // even RGB/CMYK conversions. Its public setter validates the current
        // model; a same-depth metadata-only retag preserves that existing value.
        const auto format = destination.format();
        const auto carrier = cmykProfile ? QImage::Format_CMYK8888 : QImage::Format_ARGB32;
        if (destination.reinterpretAsFormat(carrier)) {
            destination.setColorSpace(colorSpace);
            destination.reinterpretAsFormat(format);
        }
    } else
        destination.setColorSpace(colorSpace);
    destination.setDevicePixelRatio(source.devicePixelRatio());
    destination.setDotsPerMeterX(source.dotsPerMeterX());
    destination.setDotsPerMeterY(source.dotsPerMeterY());
    destination.setOffset(source.offset());
    for (const QString& key : source.textKeys())
        destination.setText(key, source.text(key));
}

quint64 storageDepth(QImage::Format format) {
    // Pixel-format channel bits omit unused storage bits (RGB444 is stored in
    // sixteen bits, RGB666 in twenty-four). Use Qt's public raster depth once
    // per format for allocation, stride validation, and threshold decisions.
    static const auto depths = [] {
        std::array<quint8, QImage::NImageFormats> result{};
        for (int index = 1; index < QImage::NImageFormats; ++index)
            result[static_cast<std::size_t>(index)] =
                static_cast<quint8>(QImage(1, 1, static_cast<QImage::Format>(index)).depth());
        return result;
    }();
    return depths[static_cast<std::size_t>(format)];
}

bool largeRaster(const QSize& size, QImage::Format format) {
    const quint64 depth = storageDepth(format);
    const quint64 stride = ((static_cast<quint64>(size.width()) * depth + 31) / 32) * 4;
    return stride != 0 && static_cast<quint64>(size.height()) >=
                              (snow::memory::kMappedPixelBufferMinimum + stride - 1) / stride;
}

template <typename Work> void processChunks(qsizetype chunks, qsizetype pixels, Work work) {
    // Borrow available workers instead of queuing jobs behind a calling worker.
    // At most eight bounded work items run together; no pixels survive this call.
    const int workers = static_cast<int>(std::min(
        chunks, std::min(qsizetype{8},
                         std::max(qsizetype{1}, qsizetype(QThread::idealThreadCount()) / 2))));
    if (pixels < 256 * 1024 || workers <= 1) {
        for (qsizetype chunk = 0; chunk < chunks; ++chunk)
            work(chunk);
        return;
    }
    std::atomic<qsizetype> next{0};
    QSemaphore finished;
    const auto consume = [&] {
        for (qsizetype chunk = next.fetch_add(1, std::memory_order_relaxed); chunk < chunks;
             chunk = next.fetch_add(1, std::memory_order_relaxed))
            work(chunk);
    };
    int started = 0;
    QThreadPool* pool = QThreadPool::globalInstance();
    for (int worker = 1; worker < workers; ++worker) {
        try {
            if (pool->tryStart([&] {
                    consume();
                    finished.release();
                }))
                ++started;
        } catch (const std::bad_alloc&) {
            // Already-started workers still refer to this call's state. Finish
            // their work before returning, even if another runnable cannot fit.
            break;
        }
    }
    consume();
    finished.acquire(started);
}

template <typename Convert>
QImage convertStrips(const QImage& source, QImage::Format format, Convert convert,
                     Qt::Orientations mirrored = {}, bool orderedDither = false) {
    QImage result = snowCanvasAllocateImage(source.size(), format);
    if (result.isNull())
        return {};
    // Include Qt's highest-precision intermediate format in the work limit.
    // Independent chunks restore parallel conversion without a full heap raster.
    constexpr qsizetype kWorkingBytes = 256 * 1024;
    constexpr qsizetype kWorkingPixelBytes = 16;
    // Bayer dithering repeats every sixteen pixels in each direction. Aligned
    // tiles preserve Qt's global phase when its local converter starts at zero.
    const int alignment = orderedDither ? 16 : 1;
    const int maximumColumns = static_cast<int>(kWorkingBytes / (kWorkingPixelBytes * alignment));
    const int columns = std::min(source.width(), maximumColumns / 16 * 16);
    const int maximumRows =
        static_cast<int>(kWorkingBytes / (qsizetype(columns) * kWorkingPixelBytes));
    const int rows = orderedDither ? std::max(16, maximumRows / 16 * 16) : maximumRows;
    const qsizetype columnsOfTiles = (qsizetype(source.width()) + columns - 1) / columns;
    const qsizetype rowsOfTiles = (qsizetype(source.height()) + rows - 1) / rows;
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype destinationStride = result.bytesPerLine();
    const uchar* sourcePixels = source.constBits();
    uchar* destinationPixels = result.bits();
    const int sourceDepth = source.depth();
    const int destinationPixelBytes = result.depth() / 8;
    const auto palette = source.colorTable();
    const auto colorSpace = source.colorSpace();
    std::atomic<bool> failed{false};
    processChunks(columnsOfTiles * rowsOfTiles, qsizetype(source.width()) * source.height(),
                  [&](qsizetype chunk) {
                      if (failed.load(std::memory_order_relaxed))
                          return;
                      try {
                          const int x = static_cast<int>(chunk % columnsOfTiles) * columns;
                          const int y = static_cast<int>(chunk / columnsOfTiles) * rows;
                          const int width = std::min(columns, source.width() - x);
                          const int height = std::min(rows, source.height() - y);
                          const int sourceX =
                              mirrored.testFlag(Qt::Horizontal) ? source.width() - x - width : x;
                          const int sourceY =
                              mirrored.testFlag(Qt::Vertical) ? source.height() - y - height : y;
                          QImage view(sourcePixels + qsizetype(sourceY) * sourceStride +
                                          qsizetype(sourceX) * sourceDepth / 8,
                                      width, height, sourceStride, source.format());
                          view.setColorTable(palette);
                          view.setColorSpace(colorSpace);
                          const QImage converted = convert(view);
                          if (converted.isNull()) {
                              failed.store(true, std::memory_order_relaxed);
                              return;
                          }
                          const uchar* convertedPixels = converted.constBits();
                          for (int row = 0; row < height; ++row)
                              std::memcpy(
                                  destinationPixels + qsizetype(y + row) * destinationStride +
                                      qsizetype(x) * destinationPixelBytes,
                                  convertedPixels + qsizetype(row) * converted.bytesPerLine(),
                                  static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(destinationPixelBytes));
                      } catch (const std::bad_alloc&) {
                          failed.store(true, std::memory_order_relaxed);
                      }
                  });
    if (failed.load(std::memory_order_relaxed))
        return {};
    copyMetadata(source, result);
    return result;
}

template <int PixelBytes>
void transformPixels(const QImage& source, QImage& result, int a, int b, int c, int d) {
    constexpr int kTileSize = 32;
    const uchar* sourcePixels = source.constBits();
    uchar* destinationPixels = result.bits();
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype destinationStride = result.bytesPerLine();
    const int offsetX = (a < 0 ? source.width() - 1 : 0) + (c < 0 ? source.height() - 1 : 0);
    const int offsetY = (b < 0 ? source.width() - 1 : 0) + (d < 0 ? source.height() - 1 : 0);
    const qsizetype columns = (qsizetype(source.width()) + kTileSize - 1) / kTileSize;
    const qsizetype rows = (qsizetype(source.height()) + kTileSize - 1) / kTileSize;
    processChunks(
        columns * rows, qsizetype(source.width()) * source.height(), [&](qsizetype chunk) {
            const int firstX = static_cast<int>(chunk % columns) * kTileSize;
            const int firstY = static_cast<int>(chunk / columns) * kTileSize;
            const int lastX = std::min(firstX + kTileSize, source.width());
            const int lastY = std::min(firstY + kTileSize, source.height());
            const auto copyPixel = [&](int x, int y) {
                const int targetX = a * x + c * y + offsetX;
                const int targetY = b * x + d * y + offsetY;
                std::memcpy(destinationPixels + qsizetype(targetY) * destinationStride +
                                qsizetype(targetX) * PixelBytes,
                            sourcePixels + qsizetype(y) * sourceStride + qsizetype(x) * PixelBytes,
                            PixelBytes);
            };
            if (b != 0) {
                for (int x = firstX; x < lastX; ++x)
                    for (int y = firstY; y < lastY; ++y)
                        copyPixel(x, y);
            } else {
                for (int y = firstY; y < lastY; ++y)
                    for (int x = firstX; x < lastX; ++x)
                        copyPixel(x, y);
            }
        });
}

void transformPixels(const QImage& source, QImage& result, const QTransform& matrix) {
    const int a = qRound(matrix.m11());
    const int b = qRound(matrix.m12());
    const int c = qRound(matrix.m21());
    const int d = qRound(matrix.m22());
    switch (source.depth()) {
    case 8:
        transformPixels<1>(source, result, a, b, c, d);
        break;
    case 16:
        transformPixels<2>(source, result, a, b, c, d);
        break;
    case 24:
        transformPixels<3>(source, result, a, b, c, d);
        break;
    case 32:
        transformPixels<4>(source, result, a, b, c, d);
        break;
    case 64:
        transformPixels<8>(source, result, a, b, c, d);
        break;
    case 128:
        transformPixels<16>(source, result, a, b, c, d);
        break;
    default:
        Q_UNREACHABLE();
    }
}

template <bool SwapChannels>
void premultiplyRows(const uchar* source, uchar* destination, qsizetype sourceStride,
                     qsizetype destinationStride, int width, int first, int last) {
    // Scalar bounds and a fixed channel order let the compiler vectorize the
    // byte-safe loads/stores without treating worker captures as pixel aliases.
    for (int y = first; y < last; ++y) {
        const uchar* input = source + qsizetype(y) * sourceStride;
        uchar* output = destination + qsizetype(y) * destinationStride;
        for (int x = 0; x < width; ++x) {
            QRgb pixel;
            std::memcpy(&pixel, input + qsizetype(x) * 4, 4);
            pixel = qPremultiply(pixel);
            if constexpr (SwapChannels)
                pixel = (pixel & 0xff00ff00u) | ((pixel & 0xffu) << 16) | ((pixel >> 16) & 0xffu);
            std::memcpy(output + qsizetype(x) * 4, &pixel, 4);
        }
    }
}

QImage premultiplyImage(const QImage& source, QImage::Format format) {
    QImage result = snowCanvasAllocateImage(source.size(), format);
    if (result.isNull())
        return {};
    const uchar* sourcePixels = source.constBits();
    uchar* destinationPixels = result.bits();
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype destinationStride = result.bytesPerLine();
    const int width = source.width();
    const int height = source.height();
    const bool swapChannels = (source.format() == QImage::Format_RGBA8888) !=
                              (format == QImage::Format_RGBA8888_Premultiplied);
    constexpr int kRows = 16;
    processChunks((qsizetype(height) + kRows - 1) / kRows, qsizetype(width) * height,
                  [=](qsizetype chunk) {
                      const int first = static_cast<int>(chunk) * kRows;
                      const int last = std::min(first + kRows, height);
                      if (swapChannels)
                          premultiplyRows<true>(sourcePixels, destinationPixels, sourceStride,
                                                destinationStride, width, first, last);
                      else
                          premultiplyRows<false>(sourcePixels, destinationPixels, sourceStride,
                                                 destinationStride, width, first, last);
                  });
    copyMetadata(source, result);
    return result;
}
} // namespace

QImage snowCanvasAllocateImage(const QSize& size, QImage::Format format) {
    if (size.width() <= 0 || size.height() <= 0 || format <= QImage::Format_Invalid ||
        format >= QImage::NImageFormats)
        return {};
    const quint64 depth = storageDepth(format);
    const quint64 stride = ((static_cast<quint64>(size.width()) * depth + 31) / 32) * 4;
    const auto maximum = static_cast<quint64>(std::numeric_limits<qsizetype>::max());
    if (stride == 0 || stride > maximum / static_cast<quint64>(size.height()))
        return {};
    const quint64 bytes = stride * static_cast<quint64>(size.height());
    if (bytes < snow::memory::kMappedPixelBufferMinimum)
        return QImage(size, format);

    try {
        auto storage = snow::memory::allocatePixelBuffer(static_cast<std::size_t>(bytes));
        if (!storage)
            return {};
        auto owner =
            std::unique_ptr<ImageStorage>(new (std::nothrow) ImageStorage{std::move(storage)});
        if (!owner)
            return {};
        QImage image(owner->pixels.get(), size.width(), size.height(),
                     static_cast<qsizetype>(stride), format, releaseImageStorage, owner.get());
        if (!image.isNull()) {
            auto* holder = owner.release();
            holder->imageIdentity = image.data_ptr();
            const std::lock_guard<std::mutex> lock(holder->registry->mutex);
            holder->registry->writableImages.insert(holder->imageIdentity);
        }
        return image;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

QImage snowCanvasCopyImage(const QImage& image, const QRect& rect) {
    if (image.isNull())
        return {};
    const QRect requested = rect.isNull() ? image.rect() : rect;
    if (requested.isEmpty())
        return {};
    QImage copy = snowCanvasAllocateImage(requested.size(), image.format());
    if (copy.isNull())
        return {};
    copyMetadata(image, copy);
    const QRect available = requested.intersected(image.rect());
    const int destinationX = available.left() - requested.left();
    const int destinationY = available.top() - requested.top();
    const int depth = image.depth();
    if (available != requested || depth % 8 != 0)
        copy.fill(0);
    uchar* destination = copy.bits();
    const uchar* source = image.constBits();
    const qsizetype destinationStride = copy.bytesPerLine();
    const qsizetype sourceStride = image.bytesPerLine();
    if (requested == image.rect() && destinationStride == sourceStride) {
        std::memcpy(destination, source, static_cast<std::size_t>(copy.sizeInBytes()));
        return copy;
    }
    for (int row = 0; row < available.height(); ++row) {
        if (depth % 8 == 0) {
            const qsizetype bytesPerPixel = depth / 8;
            std::memcpy(destination + qsizetype(destinationY + row) * destinationStride +
                            destinationX * bytesPerPixel,
                        source + qsizetype(available.top() + row) * sourceStride +
                            available.left() * bytesPerPixel,
                        static_cast<std::size_t>(available.width() * bytesPerPixel));
        } else {
            // Packed monochrome crops can begin at any bit; using the public
            // pixel API preserves both MSB/LSB ordering without a second raster.
            for (int column = 0; column < available.width(); ++column)
                copy.setPixel(destinationX + column, destinationY + row,
                              static_cast<uint>(image.pixelIndex(available.left() + column,
                                                                 available.top() + row)));
        }
    }
    return copy;
}

bool snowCanvasDetachImage(QImage& image, bool writableStorage) {
    if (image.isNull())
        return false;
    if (image.isDetached() && (writableStorage || ownsWritableStorage(image)))
        return true;
    if (image.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)) {
        image.detach();
        return !image.isNull();
    }
    QImage copy = snowCanvasCopyImage(image);
    if (copy.isNull())
        return false;
    image = std::move(copy);
    return true;
}

QImage snowCanvasConvertImage(const QImage& image, QImage::Format format,
                              Qt::ImageConversionFlags flags) {
    if (image.isNull() || format <= QImage::Format_Invalid || format >= QImage::NImageFormats)
        return {};
    if (format == image.format())
        return image;
    if (format <= QImage::Format_Indexed8) {
        QImage result = image.convertToFormat(format, flags);
        return result.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
                   ? result
                   : snowCanvasCopyImage(result);
    }
    if (!largeRaster(image.size(), image.format()) && !largeRaster(image.size(), format))
        return image.convertToFormat(format, flags);
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    if ((image.format() == QImage::Format_ARGB32 || image.format() == QImage::Format_RGBA8888) &&
        (format == QImage::Format_ARGB32_Premultiplied ||
         format == QImage::Format_RGBA8888_Premultiplied))
        return premultiplyImage(image, format);
#endif
    return convertStrips(
        image, format,
        [format, flags](const QImage& strip) { return strip.convertToFormat(format, flags); }, {},
        (flags & Qt::PreferDither) && (flags & Qt::Dither_Mask) != Qt::ThresholdDither);
}

QImage snowCanvasColorConvertedImage(const QImage& image, const QColorSpace& colorSpace,
                                     QImage::Format format, Qt::ImageConversionFlags flags) {
    if (image.isNull() || !image.colorSpace().isValid() || !colorSpace.isValid() ||
        format <= QImage::Format_Invalid || format >= QImage::NImageFormats)
        return {};
    if (image.colorSpace() == colorSpace)
        return snowCanvasConvertImage(image, format, flags);
    if (format <= QImage::Format_Indexed8) {
        QImage result = image.convertedToColorSpace(colorSpace, format, flags);
        return result.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
                   ? result
                   : snowCanvasCopyImage(result);
    }
    if (!largeRaster(image.size(), image.format()) && !largeRaster(image.size(), format))
        return image.convertedToColorSpace(colorSpace, format, flags);
    const QColorTransform transform = image.colorSpace().transformationToColorSpace(colorSpace);
    QImage result = convertStrips(
        image, format,
        [&transform, format, flags](const QImage& strip) {
            return strip.colorTransformed(transform, format, flags);
        },
        {}, (flags & Qt::PreferDither) && (flags & Qt::Dither_Mask) != Qt::ThresholdDither);
    result.setColorSpace(colorSpace);
    return result;
}

QImage snowCanvasTransformImage(const QImage& image, const QTransform& transform,
                                Qt::TransformationMode mode) {
    if (image.isNull() || transform.isIdentity())
        return image;
    const QTransform matrix = QImage::trueMatrix(transform, image.width(), image.height());
    const QRectF bounds = matrix.mapRect(QRectF(image.rect()));
    if (!std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) || bounds.width() <= 0 ||
        bounds.height() <= 0 || bounds.width() > std::numeric_limits<int>::max() ||
        bounds.height() > std::numeric_limits<int>::max())
        return {};
    const QSize size(qCeil(bounds.width()), qCeil(bounds.height()));
    const auto unit = [](qreal value) { return value == 0 || value == 1 || value == -1; };
    const bool orthogonal = matrix.isAffine() && unit(matrix.m11()) && unit(matrix.m12()) &&
                            unit(matrix.m21()) && unit(matrix.m22()) &&
                            matrix.m11() * matrix.m11() + matrix.m12() * matrix.m12() == 1 &&
                            matrix.m21() * matrix.m21() + matrix.m22() * matrix.m22() == 1 &&
                            matrix.m11() * matrix.m21() + matrix.m12() * matrix.m22() == 0;
    const bool scaleTransform = matrix.type() <= QTransform::TxScale;
    const bool reflection = matrix.determinant() < 0;
    const bool nativePainterFormat = image.format() == QImage::Format_RGB32 ||
                                     image.format() == QImage::Format_ARGB32 ||
                                     image.format() == QImage::Format_ARGB32_Premultiplied ||
                                     image.format() == QImage::Format_RGBA8888 ||
                                     image.format() == QImage::Format_RGBA8888_Premultiplied;
    const bool integralOrigin =
        matrix.m31() == std::floor(matrix.m31()) && matrix.m32() == std::floor(matrix.m32());
    const bool exactQtFallback =
        !orthogonal || !integralOrigin || image.format() < QImage::Format_RGB32 ||
        image.format() == QImage::Format_CMYK8888 ||
        // Qt's identity smooth scaler still computes sample origins using a
        // signed 0x8000 * dimension expression. Beyond this range its native
        // resampling is not equivalent to a byte mirror; retain that behavior.
        (reflection && scaleTransform && mode == Qt::SmoothTransformation &&
         (image.width() > 65535 || image.height() > 65535)) ||
        (reflection && mode == Qt::FastTransformation &&
         ((!scaleTransform && !image.hasAlphaChannel()) ||
          image.format() == QImage::Format_RGBA64)) ||
        (reflection && !scaleTransform && mode == Qt::SmoothTransformation && !nativePainterFormat);
    if (exactQtFallback) {
        QImage transformed = image.transformed(transform, mode);
        return transformed.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
                   ? transformed
                   : snowCanvasCopyImage(transformed);
    }
    if (orthogonal && reflection && scaleTransform && mode == Qt::SmoothTransformation &&
        qint64(image.width()) * image.height() >= (1 << 20)) {
        // Qt's large smooth mirror normalizes to premultiplied color before
        // flipping. A unit scale needs no resampling, so only those conversions
        // and the final mapped mirror are necessary.
        auto workingFormat = image.format();
        switch (workingFormat) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32_Premultiplied:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888_Premultiplied:
        case QImage::Format_RGBX64:
        case QImage::Format_RGBA64_Premultiplied:
        case QImage::Format_RGBX32FPx4:
        case QImage::Format_RGBA32FPx4_Premultiplied:
            break;
        case QImage::Format_RGBA64:
        case QImage::Format_Grayscale16:
            workingFormat = QImage::Format_RGBA64_Premultiplied;
            break;
        case QImage::Format_RGBX16FPx4:
            workingFormat = QImage::Format_RGBX32FPx4;
            break;
        case QImage::Format_RGBA16FPx4:
        case QImage::Format_RGBA16FPx4_Premultiplied:
        case QImage::Format_RGBA32FPx4:
            workingFormat = QImage::Format_RGBA32FPx4_Premultiplied;
            break;
        default:
            workingFormat = image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                    : QImage::Format_RGB32;
            break;
        }
        if (workingFormat != image.format()) {
            const Qt::Orientations mirrored =
                (matrix.m11() < 0 ? Qt::Horizontal : Qt::Orientations{}) |
                (matrix.m22() < 0 ? Qt::Vertical : Qt::Orientations{});
            return convertStrips(
                image, image.format(),
                [workingFormat, mirrored](const QImage& strip) {
                    QImage working = strip.convertToFormat(workingFormat);
                    // This bounded temporary is owned by Qt, so its public in-place
                    // flip retains its allocation. Managed external images cannot.
                    working.flip(mirrored);
                    return std::move(working).convertToFormat(strip.format());
                },
                mirrored);
        }
    }
    const bool directReflection =
        scaleTransform &&
        ((mode == Qt::FastTransformation &&
          (image.format() == QImage::Format_RGB32 ||
           image.format() == QImage::Format_ARGB32_Premultiplied ||
           image.format() == QImage::Format_RGBA8888_Premultiplied)) ||
         (mode == Qt::SmoothTransformation && qint64(image.width()) * image.height() >= (1 << 20) &&
          image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_RGBA8888));
    if (orthogonal && (!reflection || directReflection) && image.depth() % 8 == 0) {
        QImage result = snowCanvasAllocateImage(size, image.format());
        if (result.isNull())
            return {};
        copyMetadata(image, result);
        transformPixels(image, result, matrix);
        return result;
    }
    // Qt's paint-based transform has a signed 16-bit coordinate domain. Tiling
    // an image outside it changes native clipping/rounding, so retain its single
    // mapped paint target for those dimensions instead of changing the result.
    if (orthogonal && reflection && scaleTransform && mode == Qt::FastTransformation &&
        image.width() <= 32767 && image.height() <= 32767 &&
        (image.format() == QImage::Format_ARGB32 || image.format() == QImage::Format_RGBA8888)) {
        const Qt::Orientations mirrored = (matrix.m11() < 0 ? Qt::Horizontal : Qt::Orientations{}) |
                                          (matrix.m22() < 0 ? Qt::Vertical : Qt::Orientations{});
        return convertStrips(
            image, image.format(),
            [&transform, mode](const QImage& strip) { return strip.transformed(transform, mode); },
            mirrored);
    }
    // QPainter writes into the final external raster. Choose an alpha-capable
    // format for transparent corners introduced by non-orthogonal transforms.
    auto format = image.format();
    if (format < QImage::Format_RGB32 || (!scaleTransform && !image.hasAlphaChannel())) {
        format = image.depth() > 32 ? QImage::Format_RGBA64_Premultiplied
                                    : QImage::Format_ARGB32_Premultiplied;
    }
    QImage result = snowCanvasAllocateImage(size, format);
    if (result.isNull())
        return {};
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, mode == Qt::SmoothTransformation);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, mode == Qt::SmoothTransformation);
    painter.setTransform(matrix);
    QImage view(image.constBits(), image.width(), image.height(), image.bytesPerLine(),
                image.format());
    view.setColorTable(image.colorTable());
    painter.drawImage(QPoint(), view);
    painter.end();
    copyMetadata(image, result);
    return result;
}

QImage snowCanvasScaleImage(const QImage& image, const QSize& size, Qt::AspectRatioMode aspectRatio,
                            Qt::TransformationMode mode) {
    if (image.isNull())
        return {};
    const QSize outputSize = image.size().scaled(size, aspectRatio);
    if (outputSize == image.size())
        return image;
    QImage result = image.scaled(size, aspectRatio, mode);
    return result.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
               ? result
               : snowCanvasCopyImage(result);
}
