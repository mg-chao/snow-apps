// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <tuple>
#include <vector>

namespace snow_shot::presentation {
namespace {
constexpr int radius = 21;
constexpr int maximumAxisBatch = 4096;
constexpr int maximumTilePixels = 128 * 1024;

// All analysis colors are premultiplied RGBA8. Never inspect invisible straight-alpha RGB.
template <int Format> QRgb pixelAt(const uchar* source) {
    QRgb pixel;
    if constexpr (Format >= 3) {
        pixel = qRgba(source[0], source[1], source[2], Format == 3 ? 255 : source[3]);
    } else {
        std::memcpy(&pixel, source, sizeof(pixel));
        if constexpr (Format == 0)
            pixel |= 0xff000000u;
    }
    if constexpr (Format == 2 || Format == 5)
        pixel = qPremultiply(pixel);
    return pixel;
}

quint64 difference(QRgb first, QRgb second) {
    return static_cast<quint64>(std::max(
        {std::abs(qRed(first) - qRed(second)), std::abs(qGreen(first) - qGreen(second)),
         std::abs(qBlue(first) - qBlue(second)), std::abs(qAlpha(first) - qAlpha(second))}));
}

class Projection final {
  public:
    Projection(const QImage& image, bool horizontal, const std::function<bool()>& cancelled)
        : m_image(image), m_horizontal(horizontal), m_cancelled(cancelled) {}

    bool calculate(int first, int last) {
        m_first = first;
        m_values.assign(static_cast<size_t>(last - first + 1), 0);
        m_previous.resize(m_values.size());
        const int length = m_horizontal ? m_image.width() : m_image.height();
        // Tiles own their native scanline padding before any Qt conversion. At most 128K
        // pixels: even RGBA32FPx4 native input plus RGBA8 conversion fits well below 8 MiB.
        const int axisStep = m_horizontal ? 64 : maximumAxisBatch;
        for (int axis = first; axis <= last;) {
            const int axisCount = std::min(axisStep, last - axis + 1);
            const int tangentStep = std::min(2048, maximumTilePixels / axisCount);
            for (int tangent = 0; tangent < length;) {
                if (m_cancelled && m_cancelled())
                    return false;
                const int tangentCount = std::min(tangentStep, length - tangent);
                const QRect region = m_horizontal ? QRect(tangent, axis, tangentCount, axisCount)
                                                  : QRect(axis, tangent, axisCount, tangentCount);
                const QImage* source = &m_image;
                QImage converted;
                QPoint origin = region.topLeft();
                switch (source->format()) {
                case QImage::Format_RGB32:
                case QImage::Format_ARGB32_Premultiplied:
                case QImage::Format_ARGB32:
                case QImage::Format_RGBX8888:
                case QImage::Format_RGBA8888_Premultiplied:
                case QImage::Format_RGBA8888:
                    break;
                default:
                    converted =
                        m_image.copy(region).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    if (converted.isNull())
                        throw std::bad_alloc();
                    source = &converted;
                    origin = {};
                    break;
                }
                const auto scan = [&]<int Format>() {
                    if (m_horizontal)
                        scanTile<Format, true>(*source, origin, axis - first, axisCount, tangent,
                                               tangentCount);
                    else
                        scanTile<Format, false>(*source, origin, axis - first, axisCount, tangent,
                                                tangentCount);
                };
                switch (source->format()) {
                case QImage::Format_RGB32:
                    scan.template operator()<0>();
                    break;
                case QImage::Format_ARGB32_Premultiplied:
                    scan.template operator()<1>();
                    break;
                case QImage::Format_ARGB32:
                    scan.template operator()<2>();
                    break;
                case QImage::Format_RGBX8888:
                    scan.template operator()<3>();
                    break;
                case QImage::Format_RGBA8888_Premultiplied:
                    scan.template operator()<4>();
                    break;
                case QImage::Format_RGBA8888:
                    scan.template operator()<5>();
                    break;
                default:
                    Q_UNREACHABLE();
                }
                tangent += tangentCount;
            }
            axis += axisCount;
        }
        return true;
    }

    quint64 at(int coordinate) const {
        const int dimension = m_horizontal ? m_image.height() : m_image.width();
        return m_values[static_cast<size_t>(std::clamp(coordinate, 0, dimension - 1) - m_first)];
    }

  private:
    template <int Format, bool Horizontal>
    void scanTile(const QImage& source, QPoint origin, int offset, int axisCount, int tangent,
                  int tangentCount) {
        if constexpr (Horizontal) {
            for (int row = 0; row < axisCount; ++row) {
                const auto* data =
                    source.constScanLine(origin.y() + row) + qsizetype(origin.x()) * 4;
                const auto index = static_cast<size_t>(offset + row);
                QRgb previous = m_previous[index];
                quint64 sum = 0;
                for (int column = 0; column < tangentCount; ++column) {
                    const QRgb pixel = pixelAt<Format>(data + qsizetype(column) * 4);
                    if (tangent != 0 || column != 0)
                        sum += difference(pixel, previous);
                    previous = pixel;
                }
                m_previous[index] = previous;
                m_values[index] += sum;
            }
        } else {
            // Iterate across adjacent columns in each source row; never walk an entire
            // image column with a large stride or transpose/copy the full source.
            for (int row = 0; row < tangentCount; ++row) {
                const auto* data =
                    source.constScanLine(origin.y() + row) + qsizetype(origin.x()) * 4;
                for (int column = 0; column < axisCount; ++column) {
                    const auto index = static_cast<size_t>(offset + column);
                    const QRgb pixel = pixelAt<Format>(data + qsizetype(column) * 4);
                    if (tangent != 0 || row != 0)
                        m_values[index] += difference(pixel, m_previous[index]);
                    m_previous[index] = pixel;
                }
            }
        }
    }

    const QImage& m_image;
    bool m_horizontal;
    const std::function<bool()>& m_cancelled;
    int m_first = 0;
    std::vector<quint64> m_values;
    std::vector<QRgb> m_previous;
};

struct Domain final {
    int nominal;
    int first;
    int last;
};

class RunBuilder final {
  public:
    void append(int coordinate) {
        if (m_count == 0) {
            m_start = coordinate;
            m_count = 1;
        } else if (m_count == 1) {
            m_step = coordinate - m_start;
            ++m_count;
        } else if (qint64(m_start) + qint64(m_step) * m_count == coordinate) {
            ++m_count;
        } else {
            flush();
            append(coordinate);
        }
    }
    void flush() {
        if (m_count != 0) {
            if (!lines.appendArithmetic(m_start, m_step, m_count))
                throw std::bad_alloc();
            m_count = 0;
        }
    }
    ImageSplitLines lines;

  private:
    int m_start = 0;
    int m_step = 0;
    qsizetype m_count = 0;
};

bool refine(const QImage& image, const ImageSplitSettings& settings, ImageSplitLayout& layout,
            const std::function<bool()>& cancelled) {
    const bool horizontal = settings.direction == ImageSplitDirection::Horizontal;
    const int dimension = horizontal ? image.height() : image.width();
    const int length = horizontal ? image.width() : image.height();
    auto& lines = horizontal ? layout.horizontalLines : layout.verticalLines;
    if (lines.isEmpty() || length == 1 || settings.count == dimension)
        return true;
    const int section = dimension / settings.count;
    const int extra = dimension % settings.count;
    const auto nominal = [section, extra](int index) {
        const qint64 part = qint64(index) + 1;
        return static_cast<int>(part * section + std::min(part, qint64(extra)));
    };
    const auto domain = [&](int index) {
        const int center = nominal(index);
        const int first = static_cast<int>(
            std::max({qint64(1), qint64(center) - radius,
                      index == 0 ? qint64(1) : (qint64(nominal(index - 1)) + center) / 2 + 1}));
        const int last = static_cast<int>(std::min(
            {qint64(dimension - 1), qint64(center) + radius,
             index + 1 == settings.count - 1 ? qint64(dimension - 1)
                                             : (qint64(center) + nominal(index + 1)) / 2}));
        return Domain{center, first, last};
    };
    Projection projection(image, horizontal, cancelled);
    RunBuilder builder;
    std::vector<Domain> domains;
    domains.reserve(maximumAxisBatch);
    // When sections become one pixel wide, all remaining domains are singletons.
    // Append that entire arithmetic tail without enumerating millions of guides.
    const int movableCount = section == 1 ? extra : settings.count - 1;
    for (int index = 0; index < movableCount;) {
        if (cancelled && cancelled())
            return false;
        const auto initial = domain(index);
        if (initial.first == initial.last) {
            builder.append(initial.nominal);
            ++index;
            continue;
        }
        const int first = std::max(0, initial.first - 2);
        int last = std::min(dimension - 1, initial.last + 1);
        domains.clear();
        domains.push_back(initial);
        ++index;
        while (index < movableCount) {
            const auto next = domain(index);
            const int nextLast = std::min(dimension - 1, next.last + 1);
            // Fixed domains need no scoring; leave them to the direct append above.
            if (next.first == next.last || qint64(next.first) - 2 > qint64(last) + 1 ||
                qint64(nextLast) - first + 1 > maximumAxisBatch)
                break;
            domains.push_back(next);
            last = nextLast;
            ++index;
        }
        if (!projection.calculate(first, last))
            return false;
        for (const auto& candidate : domains) {
            const auto score = [&](int boundary) {
                const auto before = projection.at(boundary - 1);
                const auto after = projection.at(boundary);
                return std::tuple{std::min(before, after),
                                  projection.at(boundary - 2) + before + after +
                                      projection.at(boundary + 1),
                                  std::abs(boundary - candidate.nominal), boundary};
            };
            int best = candidate.nominal;
            auto bestScore = score(best);
            for (int boundary = candidate.first; boundary <= candidate.last; ++boundary) {
                const auto value = score(boundary);
                if (value < bestScore) {
                    best = boundary;
                    bestScore = value;
                }
            }
            builder.append(best);
        }
    }
    builder.flush();
    if (movableCount < settings.count - 1 &&
        !builder.lines.appendArithmetic(nominal(movableCount), 1,
                                        settings.count - 1 - movableCount))
        throw std::bad_alloc();
    lines = std::move(builder.lines);
    return true;
}
} // namespace

std::optional<ImageSplitPreparedLayout> prepareImageSplitLayout(const QImage& image,
                                                                const ImageSplitSettings& settings,
                                                                std::function<bool()> cancelled) {
    if (cancelled && cancelled())
        return std::nullopt;
    ImageSplitPreparedLayout result{image.cacheKey(), settings,
                                    calculateImageSplitLayout(image.size(), settings)};
    if (!result.layout.isValid() || settings.type != ImageSplitType::Smart ||
        settings.direction == ImageSplitDirection::Grid)
        return result;
    try {
        if (!refine(image, settings, result.layout, cancelled) || (cancelled && cancelled()))
            return std::nullopt;
    } catch (const std::bad_alloc&) {
        result.layout.verticalLines = {};
        result.layout.horizontalLines = {};
        result.layout.errors.insert(
            QStringLiteral("image"),
            QCoreApplication::translate("ImageSplitDocument",
                                        "The split lines require too much memory"));
    }
    return result;
}
} // namespace snow_shot::presentation
