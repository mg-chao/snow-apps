#include "screenshotpinneddecorationrenderer.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage render(const QRect& outline, const QSizeF& radii, int width, const QColor& color,
              ScreenshotPinnedShadowCache& cache, const QRect& clip = {}) {
    const QRect outer = outline.adjusted(-width - 4, -width - 4, width + 4, width + 4);
    const QRect surface = clip.isEmpty() ? outer : clip;
    QImage image(QSize(surface.right() + 1, surface.bottom() + 1),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    ScreenshotPinnedDecorationRenderer::renderShadow(painter, outline, radii, width, color,
                                                     clip.isEmpty() ? image.rect() : clip, cache);
    return image;
}

void squareShadowMatchesExistingAssets() {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotPinnedShadowCache cache;
    const QRect outline(24, 24, 160, 100);
    const QColor color(12, 73, 210, 190);
    const QImage actual = render(outline, {}, 16, color, cache);
    QImage expected(actual.size(), actual.format());
    expected.fill(Qt::transparent);
    {
        QPainter painter(&expected);
        ScreenshotSelectionShadowRenderer::renderResultShadow(painter, outline, 0, 16, color, 1.0);
    }
    require(actual == expected, "square pinned shadow differs from the shared falloff");
    require(cache.diagnostics().nineSliceDraws == 1 && cache.diagnostics().geometryBuilds == 0,
            "square shadow should use only the shared nine-slice cache");
    for (int y = outline.top(); y < outline.y() + outline.height(); ++y)
        for (int x = outline.left(); x < outline.x() + outline.width(); ++x)
            require(actual.pixelColor(x, y).alpha() == 0, "shadow paints into square content");
    require(actual.pixelColor(outline.center().x(), outline.top() - 17).alpha() == 0,
            "square shadow exceeds its finite width");
    require(actual.pixelColor(outline.center().x(), outline.top() - 1).alpha() > 60,
            "square shadow has no peak near the outline");
}

void radiusIsClampedBeforeAssetLookup() {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotPinnedShadowCache cache;
    const QImage image =
        render(QRect(24, 24, 32, 32), QSizeF(1000000, 1000000), 16, Qt::black, cache);
    require(!image.isNull() && cache.diagnostics().nineSliceDraws == 1,
            "oversized radius was not clamped to the small content");
    const auto diagnostics = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(diagnostics.retainedBytes <= 65u * 65u * 4u,
            "unclamped radius caused a radius-squared shadow asset");
}

void ellipticalGeometryIsCachedAcrossColors() {
    ScreenshotPinnedShadowCache cache;
    const QRect outline(28, 28, 800, 640);
    const QSizeF radii(320, 110);
    const QImage normal = render(outline, radii, 24, QColor(0, 0, 0, 255), cache);
    const auto first = cache.diagnostics();
    const QImage active = render(outline, radii, 24, QColor(105, 177, 255, 255), cache);
    const QImage locked = render(outline, radii, 24, QColor(250, 173, 20, 128), cache);
    const auto warm = cache.diagnostics();
    require(first.geometryBuilds == 1 && warm.geometryBuilds == 1 && warm.geometryHits == 2,
            "state colors rebuilt elliptical shadow geometry");
    require(warm.retainedBytes == first.retainedBytes && warm.retainedSpanRectangles > 0 &&
                warm.retainedSpanRectangles < 4u * 24u * (320u + 110u + 24u),
            "elliptical shadow retains memory proportional to its image area");
    require(warm.paletteBuilds == 3, "independent colors did not populate the bounded palettes");
    render(outline, radii, 24, QColor(0, 0, 0, 255), cache);
    require(cache.diagnostics().paletteBuilds == 3 && cache.diagnostics().paletteHits == 1,
            "warm recoloring rebuilt the cached solid brushes");
    const QPoint peak(outline.center().x(), outline.top() - 1);
    require(std::abs(normal.pixelColor(peak).alpha() - 92) <= 5,
            "vector shadow does not reach the shared 0.36 peak");
    require(std::abs(active.pixelColor(peak).alpha() - normal.pixelColor(peak).alpha()) <= 1,
            "active color changed alpha despite equal configured alpha");
    if (std::abs(locked.pixelColor(peak).alpha() * 2 - normal.pixelColor(peak).alpha()) > 7) {
        throw std::runtime_error("configured shadow alpha is not preserved by vector layers: "
                                 "normal=" +
                                 std::to_string(normal.pixelColor(peak).alpha()) +
                                 ", locked=" + std::to_string(locked.pixelColor(peak).alpha()));
    }
    require(active.pixelColor(peak).blue() > active.pixelColor(peak).red(),
            "active shadow does not use its independent color");
    require(locked.pixelColor(peak).red() > locked.pixelColor(peak).blue(),
            "locked shadow does not use its independent color");
    const QImage translucent = render(outline, radii, 24, QColor(40, 80, 120, 190), cache);
    require(translucent.pixelColor(peak).alpha() == qRound(0.36 * 190),
            "coverage coalescing exceeded the configured 0.36 alpha peak");
    require(normal.pixelColor(outline.center()).alpha() == 0,
            "elliptical shadow paints into the content interior");
    require(normal.pixelColor(outline.center().x(), outline.top() - 25).alpha() == 0,
            "elliptical shadow exceeds its finite width");
    for (int distance = 2; distance < 24; ++distance) {
        require(normal.pixelColor(peak.x(), outline.top() - distance).alpha() <=
                    normal.pixelColor(peak.x(), outline.top() - distance + 1).alpha(),
                "elliptical shadow falloff is not monotonic");
    }
    render(outline.adjusted(0, 0, 1, 0), radii, 24, Qt::black, cache);
    require(cache.diagnostics().geometryBuilds == 2,
            "changing the outline did not rebuild vector geometry");
}

qreal ellipseExteriorDistance(const QPointF& point, const QPointF& center, const QSizeF& radii) {
    const qreal x = std::abs(point.x() - center.x());
    const qreal y = std::abs(point.y() - center.y());
    const qreal rx = radii.width();
    const qreal ry = radii.height();
    if (x * x / (rx * rx) + y * y / (ry * ry) <= 1.0)
        return 0.0;
    qreal angle = std::atan2(y * rx, x * ry);
    constexpr qreal halfPi = 1.5707963267948966;
    for (int step = 0; step < 20; ++step) {
        const qreal cosine = std::cos(angle);
        const qreal sine = std::sin(angle);
        const qreal difference = ry * ry - rx * rx;
        const qreal slope = difference * sine * cosine + rx * x * sine - ry * y * cosine;
        const qreal curvature =
            difference * (cosine * cosine - sine * sine) + rx * x * cosine + ry * y * sine;
        if (std::abs(curvature) < 1.0e-9)
            break;
        angle = std::clamp(angle - slope / curvature, 0.0, halfPi);
    }
    return std::hypot(x - rx * std::cos(angle), y - ry * std::sin(angle));
}

void exteriorBandsHaveNoLeaksOrGaps() {
    const QRect outline(28, 28, 800, 640);
    for (const QSizeF& radii :
         {QSizeF(300, 300), QSizeF(320, 110), QSizeF(320.25, 110.75), QSizeF(320, 12)}) {
        for (const int width : {16, 24}) {
            ScreenshotPinnedShadowCache cache;
            const QImage image = render(outline, radii, width, Qt::black, cache);
            const QPointF center(outline.x() + radii.width(), outline.y() + radii.height());
            for (const qreal angle : {0.5235987755982988, 0.7853981633974483, 1.0471975511965976}) {
                const qreal cosine = std::cos(angle);
                const qreal sine = std::sin(angle);
                const QPointF boundary(center.x() - radii.width() * cosine,
                                       center.y() - radii.height() * sine);
                QPointF normal(-cosine / radii.width(), -sine / radii.height());
                normal /= std::hypot(normal.x(), normal.y());
                int previousAlpha = 255;
                for (int distance = 2; distance <= width + 2; ++distance) {
                    const QPointF sample = boundary + normal * distance;
                    const QPoint pixel(qFloor(sample.x()), qFloor(sample.y()));
                    const qreal actualDistance =
                        ellipseExteriorDistance(QPointF(pixel) + QPointF(0.5, 0.5), center, radii);
                    const int alpha = image.pixelColor(pixel).alpha();
                    const qreal progress = std::clamp(1.0 - actualDistance / width, 0.0, 1.0);
                    const int expected =
                        qRound(255.0 * 0.36 * progress * progress * (3.0 - 2.0 * progress));
                    // Spans sample the analytic distance at physical pixel centers. Quantized
                    // coverage and configured alpha can introduce at most one alpha unit each.
                    require(std::abs(alpha - expected) <= 2,
                            "curved shadow band has an opacity seam or wrong falloff");
                    require(alpha <= previousAlpha + 2,
                            "curved shadow opacity rises toward the exterior");
                    if (actualDistance < width * 0.6)
                        require(alpha > 15, "curved shadow has a gap between exterior bands");
                    if (actualDistance > width + 0.75)
                        require(alpha == 0, "curved shadow exceeds its finite physical width");
                    previousAlpha = alpha;
                }
                const QPointF interior = boundary - normal * 2.0;
                require(image.pixelColor(qFloor(interior.x()), qFloor(interior.y())).alpha() == 0,
                        "curved exterior bands leak into the content interior");
            }
            const auto first = cache.diagnostics();
            render(outline, radii, width, QColor(105, 177, 255, 128), cache);
            require(cache.diagnostics().geometryBuilds == 1 &&
                        cache.diagnostics().retainedBytes == first.retainedBytes,
                    "recoloring exterior bands rebuilt or grew their cached geometry");
        }
    }
}

void largeEqualRadiusUsesBoundedVectorGeometry() {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    ScreenshotPinnedShadowCache cache;
    render(QRect(24, 24, 800, 700), QSizeF(300, 300), 16, Qt::black, cache);
    require(cache.diagnostics().vectorDraws == 1 && cache.diagnostics().geometryBuilds == 1,
            "large equal corner radius should use vector shadow geometry");
    require(ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread().cacheBuilds == 0,
            "large equal radius created a bitmap shadow asset");
    const std::size_t bytes = cache.diagnostics().retainedBytes;
    render(QRect(24, 24, 16000, 12000), QSizeF(3000, 4000), 16, Qt::black, cache,
           QRect(0, 0, 100, 100));
    require(cache.diagnostics().retainedBytes <= bytes * 20,
            "exterior vector storage grows faster than the finite-width perimeter");
    require(cache.diagnostics().retainedSpanRectangles < 4u * 16u * (3000u + 4000u + 16u),
            "large radius retained a corner-area shadow representation");
    cache.clear();
    require(cache.diagnostics().retainedBytes == 0 && cache.diagnostics().geometryBuilds == 0,
            "pooled renderer cache did not release its retained geometry");
}

void transparentAndClippedShadowsPreservePainterState() {
    ScreenshotPinnedShadowCache cache;
    QImage image(QSize(220, 160), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setOpacity(0.7);
    painter.setClipRect(QRect(0, 0, 120, 160));
    const QTransform transform = painter.transform();
    const QRegion clip = painter.clipRegion();
    ScreenshotPinnedDecorationRenderer::renderShadow(
        painter, QRect(24, 24, 160, 100), QSizeF(30, 20), 16, Qt::transparent, image.rect(), cache);
    require(cache.diagnostics().geometryBuilds == 0,
            "transparent disabled shadow prepared vector geometry");
    ScreenshotPinnedDecorationRenderer::renderShadow(painter, QRect(24, 24, 160, 100),
                                                     QSizeF(30, 20), 16, Qt::black,
                                                     QRect(0, 0, 80, 160), cache);
    require(painter.transform() == transform && painter.clipRegion() == clip &&
                painter.opacity() == 0.7,
            "shadow renderer leaked painter state");
    painter.end();
    require(image.pixelColor(100, 23).alpha() == 0,
            "shadow renderer did not respect the device outer clip");
}

void physicalCoordinatesRemainExactAtFractionalDpi() {
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        QImage image(QSize(240, 180), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        ScreenshotPinnedShadowCache cache;
        QPainter painter(&image);
        painter.scale(1.0 / dpr, 1.0 / dpr);
        ScreenshotPinnedDecorationRenderer::renderShadow(painter, QRect(16, 16, 208, 148), {}, 16,
                                                         Qt::black, image.rect(), cache);
        painter.end();
        require(image.pixelColor(120, 15).alpha() > 85,
                "fractional DPI moved the physical shadow outline");
        require(image.pixelColor(120, 16).alpha() == 0,
                "fractional DPI shadow crossed into the content");
        require(image.pixelColor(120, 0).alpha() == 0,
                "fractional DPI did not preserve the 16-pixel finite width");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        squareShadowMatchesExistingAssets();
        radiusIsClampedBeforeAssetLookup();
        ellipticalGeometryIsCachedAcrossColors();
        exteriorBandsHaveNoLeaksOrGaps();
        largeEqualRadiusUsesBoundedVectorGeometry();
        transparentAndClippedShadowsPreservePainterState();
        physicalCoordinatesRemainExactAtFractionalDpi();
        std::cout << "Pinned shadow renderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
