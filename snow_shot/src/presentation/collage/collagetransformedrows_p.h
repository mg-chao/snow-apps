// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "snow_shot/presentation/collagedocument.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>

namespace snow_shot::presentation::image_stitching_detail {
inline constexpr qint64 maximumTransformCacheBytes = 2 * 1024 * 1024;

inline bool streamableTransform(const ImageStitchingItem& item) {
    const auto& matrix = item.transform;
    const auto unit = [](qreal value) { return value == 0 || value == 1 || value == -1; };
    return !matrix.isIdentity() && matrix.isAffine() && matrix.dx() == 0 && matrix.dy() == 0 &&
           unit(matrix.m11()) && unit(matrix.m12()) && unit(matrix.m21()) && unit(matrix.m22()) &&
           matrix.m11() * matrix.m11() + matrix.m12() * matrix.m12() == 1 &&
           matrix.m21() * matrix.m21() + matrix.m22() * matrix.m22() == 1 &&
           matrix.m11() * matrix.m21() + matrix.m12() * matrix.m22() == 0 &&
           item.image.depth() >= 8 && item.image.depth() % 8 == 0 &&
           item.image.devicePixelRatio() == 1;
}

inline int transformCacheRows(const ImageStitchingItem& item) {
    const auto size = transformedSize(item);
    const qint64 row = qint64(size.width()) * std::max(4, item.image.depth() / 8);
    return static_cast<int>(std::max<qint64>(
        1, std::min<qint64>({64, size.height(), maximumTransformCacheBytes / row})));
}

inline qint64 transformCacheBytes(const ImageStitchingItem& item) {
    return qint64(transformedSize(item).width()) * transformCacheRows(item) * 4;
}

inline qint64 transformWorkingBytes(const ImageStitchingItem& item) {
    // Native crop and native transformed tile can coexist. The bounded color converter owns
    // additional temporary strips; the persistent RGBA tile is accounted for separately.
    return qint64(transformedSize(item).width()) * transformCacheRows(item) *
               std::max(4, item.image.depth() / 8) * 2 +
           8 * 1024 * 1024;
}

class TransformedRows final {
  public:
    TransformedRows(QImage image, QTransform transform, QSize size, int cacheRows,
                    std::function<bool()> cancelled)
        : m_image(std::move(image)), m_transform(transform), m_size(size), m_cacheRows(cacheRows),
          m_cancelled(std::move(cancelled)),
          m_inverse(QImage::trueMatrix(transform, m_image.width(), m_image.height()).inverted()) {}

    bool read(int first, int count, qsizetype stride, uchar* destination, qsizetype capacity,
              int left = 0, int width = -1) {
        const std::lock_guard lock(m_mutex);
        if (width < 0)
            width = m_size.width();
        if (width <= 0 || left < 0 || width > m_size.width() || left > m_size.width() - width)
            return false;
        const qsizetype rowBytes = qsizetype(width) * 4;
        if (!destination || first < 0 || count <= 0 || count > m_size.height() ||
            first > m_size.height() - count || stride < rowBytes || capacity < rowBytes ||
            qsizetype(count - 1) > (capacity - rowBytes) / stride)
            return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(destination);
        const auto extent = static_cast<std::uintptr_t>(qsizetype(count - 1) * stride + rowBytes);
        const auto source = reinterpret_cast<std::uintptr_t>(m_image.constBits());
        const auto sourceBytes = static_cast<std::uintptr_t>(m_image.sizeInBytes());
        constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
        if (extent > maximum - begin || sourceBytes > maximum - source ||
            (source < begin + extent && begin < source + sourceBytes))
            return false;
        int copied = 0;
        while (copied < count) {
            if (stopped())
                return false;
            const int row = first + copied;
            if (row < m_first || row >= m_first + m_cache.height()) {
                if (!prepare(row / m_cacheRows * m_cacheRows))
                    return false;
            }
            const int rows = std::min(count - copied, m_first + m_cache.height() - row);
            for (int local = 0; local < rows; ++local)
                std::memcpy(destination + qsizetype(copied + local) * stride,
                            m_cache.constScanLine(row - m_first + local) + qsizetype(left) * 4,
                            size_t(rowBytes));
            copied += rows;
        }
        return !stopped();
    }

  private:
    bool stopped() const {
        return m_cancelled && m_cancelled();
    }

    bool prepare(int first) {
        m_first = -1;
        m_cache = {};
        const int rows = std::min(m_cacheRows, m_size.height() - first);
        const QRect sourceRect =
            m_inverse.mapRect(QRectF(0, first, m_size.width(), rows)).toAlignedRect();
        auto cropped = snowCanvasCopyImage(m_image, sourceRect);
        if (cropped.isNull() || stopped())
            return false;
        auto tile = snowCanvasTransformImage(cropped, m_transform);
        cropped = {};
        if (tile.isNull() || tile.size() != QSize(m_size.width(), rows) || stopped())
            return false;
        m_cache = snowCanvasAllocateImage(tile.size(), QImage::Format_RGBA8888);
        const QColorSpace target =
            tile.colorSpace().isValid() && tile.colorSpace() != QColorSpace(QColorSpace::SRgb)
                ? QColorSpace(QColorSpace::SRgb)
                : QColorSpace{};
        if (m_cache.isNull() ||
            !snowCanvasCopyRgba8888Rows(tile, 0, rows, m_cache.bits(), m_cache.sizeInBytes(),
                                        m_cache.bytesPerLine(), target) ||
            stopped()) {
            m_cache = {};
            return false;
        }
        m_first = first;
        return true;
    }

    QImage m_image;
    QTransform m_transform;
    QSize m_size;
    int m_cacheRows;
    std::function<bool()> m_cancelled;
    QTransform m_inverse;
    std::mutex m_mutex;
    QImage m_cache;
    int m_first = -1;
};

inline ScreenshotImageRowSource transformedRowSource(const ImageStitchingItem& item,
                                                     std::function<bool()> cancelled) {
    ScreenshotImageRowSource result;
    result.size = transformedSize(item);
    result.cancellationRequested = cancelled;
    auto reader = std::make_shared<TransformedRows>(item.image, item.transform, result.size,
                                                    transformCacheRows(item), std::move(cancelled));
    result.readRows = [reader](int first, int count, qsizetype stride, uchar* destination,
                               qsizetype capacity) {
        return reader->read(first, count, stride, destination, capacity);
    };
    result.readRegion = [reader](QRect region, qsizetype stride, uchar* destination,
                                 qsizetype capacity) {
        if (region.width() <= 0 || region.height() <= 0)
            return false;
        return reader->read(region.top(), region.height(), stride, destination, capacity,
                            region.left(), region.width());
    };
    return result;
}
} // namespace snow_shot::presentation::image_stitching_detail
