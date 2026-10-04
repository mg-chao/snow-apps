// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>
#include <QImage>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace snow_shot::image_codec::detail {
// Each reader copy starts with its own empty cache. Calls to one callback
// serialize access without publishing a mutable cache QImage. Large requests
// bypass lookahead and retain the bounded converter's direct path.
class BoundedSrgbRowReader {
  public:
    static constexpr qsizetype maximumCacheBytes = 2 * 1024 * 1024;

    explicit BoundedSrgbRowReader(QImage image)
        : m_source(std::move(image)),
          m_target(m_source.colorSpace().isValid() &&
                           m_source.colorSpace() != QColorSpace(QColorSpace::SRgb)
                       ? QColorSpace(QColorSpace::SRgb)
                       : QColorSpace{}) {}

    BoundedSrgbRowReader(const BoundedSrgbRowReader& other)
        : m_source(other.m_source), m_target(other.m_target) {}

    bool operator()(int first, int count, qsizetype stride, uchar* destination,
                    qsizetype capacity) const {
        const std::lock_guard lock(m_mutex);
        if (!validDestination(first, count, stride, destination, capacity))
            return false;
        const qsizetype rowBytes = qsizetype(m_source.width()) * 4;
        const int maximumRows =
            static_cast<int>(std::min(qsizetype(m_source.height()), maximumCacheBytes / rowBytes));
        if (maximumRows == 0 || count >= maximumRows)
            return snowCanvasCopyRgba8888Rows(m_source, first, count, destination, capacity, stride,
                                              m_target);
        int copied = 0;
        while (copied < count) {
            const int sourceRow = first + copied;
            if (sourceRow < m_cachedFirst || sourceRow >= m_cachedFirst + m_cachedRows) {
                if (m_cache.isNull())
                    m_cache = snowCanvasAllocateImage(QSize(m_source.width(), maximumRows),
                                                      QImage::Format_RGBA8888);
                if (m_cache.isNull())
                    return false;
                m_cachedRows = 0;
                const int rows = std::min(maximumRows, m_source.height() - sourceRow);
                if (!snowCanvasCopyRgba8888Rows(m_source, sourceRow, rows, m_cache.bits(),
                                                m_cache.sizeInBytes(), m_cache.bytesPerLine(),
                                                m_target))
                    return false;
                m_cachedFirst = sourceRow;
                m_cachedRows = rows;
            }
            const int localFirst = sourceRow - m_cachedFirst;
            const int rows = std::min(count - copied, m_cachedRows - localFirst);
            const qsizetype offset = qsizetype(copied) * stride;
            if (!snowCanvasCopyRgba8888Rows(m_cache, localFirst, rows, destination + offset,
                                            capacity - offset, stride))
                return false;
            copied += rows;
        }
        return true;
    }

    const uchar* cachedPixels() const {
        const std::lock_guard lock(m_mutex);
        return m_cache.constBits();
    }

    qsizetype cachedBytes() const {
        const std::lock_guard lock(m_mutex);
        return m_cache.sizeInBytes();
    }

  private:
    bool validDestination(int first, int count, qsizetype stride, const uchar* destination,
                          qsizetype capacity) const {
        if (m_source.isNull() || first < 0 || count <= 0 || count > m_source.height() ||
            first > m_source.height() - count || destination == nullptr || capacity < 0)
            return false;
        constexpr auto maximum = std::numeric_limits<qsizetype>::max();
        if (qsizetype(m_source.width()) > maximum / 4)
            return false;
        const qsizetype rowBytes = qsizetype(m_source.width()) * 4;
        if (stride < rowBytes || qsizetype(count - 1) > (maximum - rowBytes) / stride)
            return false;
        const qsizetype extent = qsizetype(count - 1) * stride + rowBytes;
        if (capacity < extent)
            return false;
        const auto output = reinterpret_cast<std::uintptr_t>(destination);
        constexpr auto addressMaximum = std::numeric_limits<std::uintptr_t>::max();
        const auto outputBytes = static_cast<std::uintptr_t>(extent);
        if (outputBytes > addressMaximum - output)
            return false;
        const auto outside = [output, outputBytes](const QImage& image) {
            const auto pixels = reinterpret_cast<std::uintptr_t>(image.constBits());
            const auto bytes = static_cast<std::uintptr_t>(image.sizeInBytes());
            return bytes <= addressMaximum - pixels &&
                   !(pixels < output + outputBytes && output < pixels + bytes);
        };
        return outside(m_source) && (m_cache.isNull() || outside(m_cache));
    }

    QImage m_source;
    QColorSpace m_target;
    mutable std::mutex m_mutex;
    mutable QImage m_cache;
    mutable int m_cachedFirst = -1;
    mutable int m_cachedRows = 0;
};

} // namespace snow_shot::image_codec::detail
