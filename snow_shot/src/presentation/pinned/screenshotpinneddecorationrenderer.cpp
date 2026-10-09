#include "screenshotpinneddecorationrenderer.h"

#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QBrush>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
constexpr qreal kPeakAlphaScale = 0.36;
constexpr qreal kMaximumNineSliceRadius = 256.0;
constexpr qreal kHalfPi = 1.5707963267948966;
// A fully opaque configured color peaks at 92 alpha, so finer cached coverage levels do not
// produce additional visible detail. Grouping at that precision coalesces adjacent spans and
// avoids hundreds of redundant paint calls. Recoloring introduces at most one alpha-unit error.
constexpr std::size_t kCoverageCount = 93;
// Qt can dispatch solid fills by scanline count. A narrow but tall shadow rectangle otherwise
// creates thread-pool jobs for only a few hundred pixels. Bounded strips keep this work local.
constexpr int kMaximumSpanHeight = 64;

qreal boundedRadius(qreal radius, qreal maximum) {
    return std::isfinite(radius) ? std::clamp(radius, 0.0, maximum) : 0.0;
}

qreal falloffAt(qreal distance, int width) {
    const qreal progress = std::clamp(1.0 - distance / width, 0.0, 1.0);
    return progress * progress * (3.0 - 2.0 * progress);
}

int firstPixel(qreal boundary) {
    return qCeil(boundary - 0.5);
}

qreal ellipseDistance(qreal x, qreal y, qreal rx, qreal ry) {
    if (rx == ry)
        return std::hypot(x, y) - rx;
    qreal angle = std::atan2(y * rx, x * ry);
    for (int iteration = 0; iteration < 16; ++iteration) {
        const qreal cosine = std::cos(angle);
        const qreal sine = std::sin(angle);
        const qreal difference = ry * ry - rx * rx;
        const qreal slope = difference * sine * cosine + rx * x * sine - ry * y * cosine;
        const qreal curvature =
            difference * (cosine * cosine - sine * sine) + rx * x * cosine + ry * y * sine;
        if (std::abs(curvature) < 1.0e-12)
            break;
        const qreal next = std::clamp(angle - slope / curvature, 0.0, kHalfPi);
        if (std::abs(next - angle) < 1.0e-10) {
            angle = next;
            break;
        }
        angle = next;
    }
    return std::hypot(x - rx * std::cos(angle), y - ry * std::sin(angle));
}

// Solve the monotonic y coordinate of the ellipse's exterior parallel curve. This lets the
// cold builder visit only the finite-width band rather than scanning a corner's entire area.
qreal exteriorCornerLeft(qreal y, qreal cx, qreal cy, qreal rx, qreal ry, int width) {
    const qreal height = cy - y;
    if (rx == ry)
        return cx - std::sqrt(std::max(0.0, (rx + width) * (rx + width) - height * height));
    qreal low = 0.0;
    qreal high = kHalfPi;
    for (int iteration = 0; iteration < 28; ++iteration) {
        const qreal angle = (low + high) * 0.5;
        const qreal sine = std::sin(angle);
        const qreal cosine = std::cos(angle);
        const qreal length = std::hypot(cosine / rx, sine / ry);
        const qreal offsetY = ry * sine + width * (sine / ry) / length;
        if (offsetY < height)
            low = angle;
        else
            high = angle;
    }
    const qreal angle = (low + high) * 0.5;
    const qreal cosine = std::cos(angle);
    const qreal length = std::hypot(cosine / rx, std::sin(angle) / ry);
    return cx - rx * cosine - width * (cosine / rx) / length;
}

void appendSpan(std::vector<QRect>& spans, const QRect& span) {
    // Four symmetric corner streams are appended together. Their matching preceding rows
    // therefore lie among the last four rectangles, allowing tall flat runs to coalesce.
    const std::size_t begin = spans.size() > 4 ? spans.size() - 4 : 0;
    for (std::size_t index = spans.size(); index > begin; --index) {
        QRect& previous = spans[index - 1];
        if (previous.x() == span.x() && previous.width() == span.width() &&
            previous.height() + span.height() <= kMaximumSpanHeight) {
            if (previous.bottom() + 1 == span.top()) {
                previous.setBottom(span.bottom());
                return;
            }
            if (span.bottom() + 1 == previous.top()) {
                previous.setTop(span.top());
                return;
            }
        }
    }
    spans.push_back(span);
}
} // namespace

struct ScreenshotPinnedShadowCache::Geometry {
    struct Palette {
        QColor color;
        std::array<QBrush, kCoverageCount> brushes;
    };
    std::array<std::vector<QRect>, kCoverageCount> spans;
    std::array<Palette, 3> palettes;
    std::size_t nextPalette = 0;

    void build(const QRect& outline, const QSizeF& radii, int width) {
        for (auto& band : spans)
            band.clear();
        const qreal left = outline.x();
        const qreal top = outline.y();
        const qreal right = left + outline.width();
        const qreal bottom = top + outline.height();
        const qreal rx = radii.width();
        const qreal ry = radii.height();
        const qreal cx = left + rx;
        const qreal cy = top + ry;
        const int horizontalStart = firstPixel(cx);
        const int horizontalEnd = firstPixel(right - rx);
        const int verticalStart = firstPixel(cy);
        const int verticalEnd = firstPixel(bottom - ry);
        for (int distance = 0; distance < width; ++distance) {
            const int coverage = qRound(255 * kPeakAlphaScale * falloffAt(distance + 0.5, width));
            if (coverage == 0)
                continue;
            auto& band = spans[static_cast<std::size_t>(coverage)];
            if (horizontalStart < horizontalEnd) {
                band.emplace_back(horizontalStart, outline.y() - distance - 1,
                                  horizontalEnd - horizontalStart, 1);
                band.emplace_back(horizontalStart, outline.y() + outline.height() + distance,
                                  horizontalEnd - horizontalStart, 1);
            }
            if (verticalStart < verticalEnd) {
                for (int y = verticalStart; y < verticalEnd; y += kMaximumSpanHeight) {
                    const int height = std::min(kMaximumSpanHeight, verticalEnd - y);
                    band.emplace_back(outline.x() - distance - 1, y, 1, height);
                    band.emplace_back(outline.x() + outline.width() + distance, y, 1, height);
                }
            }
        }

        const int mirrorX = outline.x() * 2 + outline.width() - 1;
        const int mirrorY = outline.y() * 2 + outline.height() - 1;
        for (int y = outline.y() - width; y < verticalStart; ++y) {
            const qreal py = y + 0.5;
            const int start = firstPixel(exteriorCornerLeft(py, cx, cy, rx, ry, width));
            const qreal innerLeft =
                py < top
                    ? cx
                    : cx - rx * std::sqrt(std::max(0.0, 1.0 - (py - cy) * (py - cy) / (ry * ry)));
            const int end = std::min(horizontalStart, firstPixel(innerLeft));
            int runStart = start;
            int runCoverage = -1;
            const auto flushRun = [&](int runEnd) {
                if (runCoverage <= 0 || runEnd <= runStart)
                    return;
                auto& band = spans[static_cast<std::size_t>(runCoverage)];
                const int length = runEnd - runStart;
                const int reflectedX = mirrorX - runEnd + 1;
                appendSpan(band, QRect(runStart, y, length, 1));
                appendSpan(band, QRect(reflectedX, y, length, 1));
                appendSpan(band, QRect(runStart, mirrorY - y, length, 1));
                appendSpan(band, QRect(reflectedX, mirrorY - y, length, 1));
            };
            for (int x = start; x < end; ++x) {
                const qreal distance = ellipseDistance(cx - x - 0.5, cy - py, rx, ry);
                const int coverage =
                    distance >= width || distance < 0.0
                        ? 0
                        : qRound(255 * kPeakAlphaScale * falloffAt(distance, width));
                if (coverage != runCoverage) {
                    flushRun(x);
                    runStart = x;
                    runCoverage = coverage;
                }
            }
            flushRun(end);
        }
    }

    const Palette& palette(const QColor& color, ScreenshotPinnedShadowDiagnostics& diagnostics) {
        for (const auto& palette : palettes) {
            if (palette.color == color) {
                ++diagnostics.paletteHits;
                return palette;
            }
        }
        auto& palette = palettes[nextPalette];
        nextPalette = (nextPalette + 1) % palettes.size();
        palette.color = color;
        int lastAlpha = -1;
        for (std::size_t coverage = 0; coverage < kCoverageCount; ++coverage) {
            QColor sample = color;
            sample.setAlpha(std::min(qRound(kPeakAlphaScale * color.alpha()),
                                     qRound(color.alpha() * static_cast<qreal>(coverage) / 255.0)));
            if (sample.alpha() == lastAlpha)
                palette.brushes[coverage] = palette.brushes[coverage - 1];
            else
                palette.brushes[coverage] = QBrush(sample);
            lastAlpha = sample.alpha();
        }
        ++diagnostics.paletteBuilds;
        return palette;
    }
};

ScreenshotPinnedShadowCache::ScreenshotPinnedShadowCache() = default;
ScreenshotPinnedShadowCache::~ScreenshotPinnedShadowCache() = default;

void ScreenshotPinnedShadowCache::clear() {
    m_outline = {};
    m_radii = {};
    m_width = 0;
    m_geometry.reset();
    m_diagnostics = {};
}

ScreenshotPinnedShadowDiagnostics ScreenshotPinnedShadowCache::diagnostics() const {
    auto result = m_diagnostics;
    if (m_geometry) {
        result.retainedBytes = sizeof(Geometry) - sizeof(Geometry::palettes);
        result.retainedPaletteBytes = sizeof(Geometry::palettes);
        for (const auto& band : m_geometry->spans) {
            result.retainedSpanRectangles += band.size();
            result.retainedBytes += band.capacity() * sizeof(QRect);
        }
    }
    return result;
}

void ScreenshotPinnedDecorationRenderer::renderShadow(QPainter& painter,
                                                      const QRect& deviceContentOutline,
                                                      const QSizeF& physicalCornerRadii,
                                                      int physicalShadowWidth, const QColor& color,
                                                      const QRect& deviceOuter,
                                                      ScreenshotPinnedShadowCache& cache) {
    if (deviceContentOutline.isEmpty() || deviceOuter.isEmpty() || physicalShadowWidth <= 0 ||
        !color.isValid() || color.alpha() == 0)
        return;

    const QRectF outline(deviceContentOutline);
    QSizeF radii(boundedRadius(physicalCornerRadii.width(), outline.width() / 2.0),
                 boundedRadius(physicalCornerRadii.height(), outline.height() / 2.0));
    if (radii.width() == 0.0 || radii.height() == 0.0)
        radii = {};
    const bool nineSlice = radii.width() == radii.height() &&
                           radii.width() <= kMaximumNineSliceRadius &&
                           radii.width() == std::floor(radii.width());
    painter.save();
    painter.setClipRect(deviceOuter, Qt::IntersectClip);
    if (nineSlice) {
        ++cache.m_diagnostics.nineSliceDraws;
        ScreenshotSelectionShadowRenderer::renderResultShadow(painter, outline, radii.width(),
                                                              physicalShadowWidth, color, 1.0);
        painter.restore();
        return;
    }

    if (!cache.m_geometry)
        cache.m_geometry = std::make_unique<ScreenshotPinnedShadowCache::Geometry>();
    if (cache.m_outline != deviceContentOutline || cache.m_radii != radii ||
        cache.m_width != physicalShadowWidth) {
        cache.m_outline = deviceContentOutline;
        cache.m_radii = radii;
        cache.m_width = physicalShadowWidth;
        cache.m_geometry->build(deviceContentOutline, radii, physicalShadowWidth);
        ++cache.m_diagnostics.geometryBuilds;
    } else {
        ++cache.m_diagnostics.geometryHits;
    }

    ++cache.m_diagnostics.vectorDraws;
    const auto& palette = cache.m_geometry->palette(color, cache.m_diagnostics);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    for (std::size_t coverage = 1; coverage < kCoverageCount; ++coverage) {
        const auto& spans = cache.m_geometry->spans[coverage];
        if (spans.empty() || palette.brushes[coverage].color().alpha() == 0)
            continue;
        painter.setBrush(palette.brushes[coverage]);
        painter.drawRects(spans.data(), static_cast<int>(spans.size()));
    }
    painter.restore();
}
