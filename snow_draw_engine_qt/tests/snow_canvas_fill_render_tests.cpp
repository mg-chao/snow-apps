#include "snow_canvas_fill_render.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

struct HatchCase {
    double strokeWidth;
    double lineWidth;
    double spacing;
    double zoom;
    double coordinateScale;
    bool smooth = true;
};

double stripeDistance(const QPointF& point, double period, bool mirrored) {
    const double phase = point.x() + (mirrored ? -point.y() : point.y());
    return std::abs(std::remainder(phase, period)) / std::sqrt(2.0);
}

void patternedFillsKeepUniformCoverageAcrossTileBoundaries() {
    // Include fractional tile periods, the upper width clamp, and independently scaled callers.
    const HatchCase cases[] = {
        {8.0, 5.2, 20.8, 1.0, 1.0},  {3.0, 2.2, 8.8, 3.0, 1.0},
        {8.0, 5.2, 20.8, 0.75, 1.3}, {8.0, 5.2, 20.8, 1.25, 0.75},
        {10.0, 6.0, 21.0, 1.0, 2.0}, {8.0, 5.2, 20.8, 1.0, 1.0, false},
    };
    for (const HatchCase& test : cases) {
        for (double dpr : {1.0, 1.25, 1.5, 2.0}) {
            for (double rotation : {0.0, 27.0}) {
                for (SnowFillStyle style : {SNOW_FILL_STYLE_LINE, SNOW_FILL_STYLE_CROSS_LINE}) {
                    for (std::uint8_t alpha : {std::uint8_t{255}, std::uint8_t{96}}) {
                        QImage image(QSize(320, 256), QImage::Format_ARGB32_Premultiplied);
                        image.setDevicePixelRatio(dpr);
                        image.fill(Qt::transparent);
                        QPainter painter(&image);
                        painter.setRenderHint(QPainter::Antialiasing, true);
                        painter.setRenderHint(QPainter::SmoothPixmapTransform, test.smooth);
                        painter.translate(160.0 / dpr + 0.37, 128.0 / dpr - 0.23);
                        painter.rotate(rotation);
                        painter.scale(test.zoom, test.zoom);
                        const QTransform deviceToLocal = painter.deviceTransform().inverted();
                        QPainterPath path;
                        path.addRect(QRectF(-1000.0, -1000.0, 2000.0, 2000.0));
                        snow_canvas_fill_render::drawStyledFill(
                            painter, path, SnowColorRgba8{255, 80, 80, alpha}, style,
                            test.strokeWidth, test.coordinateScale);
                        painter.end();

                        const double period = test.spacing * std::sqrt(2.0);
                        // Stay clear of antialiased stripe edges; test the fully covered core.
                        const double margin = 1.25 / (dpr * test.zoom * test.coordinateScale) + 0.5;
                        const int maximumAlpha =
                            style == SNOW_FILL_STYLE_LINE
                                ? alpha
                                : qRound(alpha + alpha * (255 - alpha) / 255.0);
                        int corePixels = 0;
                        int gapPixels = 0;
                        for (int y = 8; y < image.height() - 8; ++y) {
                            for (int x = 8; x < image.width() - 8; ++x) {
                                const QPointF point = deviceToLocal.map(QPointF(x + 0.5, y + 0.5)) /
                                                      test.coordinateScale;
                                const double first = stripeDistance(point, period, false);
                                const double second = style == SNOW_FILL_STYLE_CROSS_LINE
                                                          ? stripeDistance(point, period, true)
                                                          : first;
                                const double distance = std::min(first, second);
                                const int actualAlpha = qAlpha(image.pixel(x, y));
                                if (distance < test.lineWidth / 2.0 - margin) {
                                    ++corePixels;
                                    if (actualAlpha < alpha - 2) {
                                        std::fprintf(stderr,
                                                     "stripe core lost coverage: stroke=%g, "
                                                     "dpr=%g, zoom=%g, "
                                                     "coordinateScale=%g, rotation=%g, style=%d, "
                                                     "pixel=%d,%d, "
                                                     "alpha=%d\n",
                                                     test.strokeWidth, dpr, test.zoom,
                                                     test.coordinateScale, rotation,
                                                     static_cast<int>(style), x, y, actualAlpha);
                                        std::exit(1);
                                    }
                                } else if (distance > test.lineWidth / 2.0 + margin) {
                                    ++gapPixels;
                                    require(actualAlpha <= 2, "patterned fills should keep the "
                                                              "spaces between stripes transparent");
                                }
                                require(actualAlpha <= maximumAlpha + 2,
                                        "tile joins should not darken translucent stripes");
                            }
                        }
                        require(corePixels > 100 && gapPixels > 100,
                                "each transformed fill should exercise stripe cores and spaces");
                    }
                }
            }
        }
    }
}

void thinStripesRemainConnectedAcrossTileCorners() {
    // Thin strokes have partially covered texture pixels even at their center. Verify
    // continuity by following their visible cores rather than requiring full opacity.
    for (double strokeWidth : {0.0, 1.0}) {
        for (double dpr : {1.0, 1.25, 2.0}) {
            for (double rotation : {0.0, 27.0}) {
                QImage image(QSize(240, 240), QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(dpr);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.translate(120.0 / dpr + 0.37, 120.0 / dpr - 0.23);
                painter.rotate(rotation);
                painter.scale(8.0, 8.0);
                QPainterPath path;
                path.addRect(QRectF(-100.0, -100.0, 200.0, 200.0));
                snow_canvas_fill_render::drawStyledFill(painter, path,
                                                        SnowColorRgba8{255, 80, 80, 255},
                                                        SNOW_FILL_STYLE_LINE, strokeWidth);
                painter.end();

                QImage visited(image.size(), QImage::Format_Grayscale8);
                visited.fill(0);
                int stripeCount = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (visited.constScanLine(y)[x] != 0 || qAlpha(image.pixel(x, y)) < 128) {
                            continue;
                        }
                        ++stripeCount;
                        std::vector<QPoint> pending{QPoint(x, y)};
                        visited.scanLine(y)[x] = 1;
                        bool reachesBoundary = false;
                        while (!pending.empty()) {
                            const QPoint point = pending.back();
                            pending.pop_back();
                            reachesBoundary = reachesBoundary || point.x() == 0 || point.y() == 0 ||
                                              point.x() == image.width() - 1 ||
                                              point.y() == image.height() - 1;
                            for (int dy = -1; dy <= 1; ++dy) {
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const QPoint next = point + QPoint(dx, dy);
                                    if (!image.rect().contains(next) ||
                                        visited.constScanLine(next.y())[next.x()] != 0 ||
                                        qAlpha(image.pixel(next)) < 128) {
                                        continue;
                                    }
                                    visited.scanLine(next.y())[next.x()] = 1;
                                    pending.push_back(next);
                                }
                            }
                        }
                        require(reachesBoundary,
                                "continuous thin stripes should not terminate inside the fill");
                    }
                }
                require(stripeCount > 2, "thin-stripe checks should cover several texture periods");
            }
        }
    }
}

QImage renderClippedFill(SnowFillStyle style, bool textBackground, double coordinateScale) {
    QImage image(QSize(200, 140), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setClipRect(QRectF(40.0, 10.0, 130.0, 120.0));
    painter.translate(100.0, 70.0);
    painter.rotate(9.0);
    QPainterPath path;
    path.addRoundedRect(QRectF(-73.5, -47.75, 145.0, 94.0), 14.0, 14.0);
    const SnowColorRgba8 fill{255, 80, 80, 96};
    if (textBackground) {
        snow_canvas_fill_render::drawTextBackgroundFill(painter, path, fill, style, 192.0,
                                                        coordinateScale);
    } else {
        snow_canvas_fill_render::drawStyledFill(painter, path, fill, style, 8.0, coordinateScale);
    }
    painter.end();
    return image;
}

void shapeAndTextPatternsRespectClippingAndShareTheirScale() {
    const QImage solid = renderClippedFill(SNOW_FILL_STYLE_SOLID, false, 1.0);
    for (SnowFillStyle style : {SNOW_FILL_STYLE_LINE, SNOW_FILL_STYLE_CROSS_LINE}) {
        for (double coordinateScale : {0.75, 1.0, 1.3}) {
            const QImage shape = renderClippedFill(style, false, coordinateScale);
            const QImage text = renderClippedFill(style, true, coordinateScale);
            require(
                shape == text,
                "shape and text fills with matching reference widths should share their pattern");
            int filledPixels = 0;
            int gapPixels = 0;
            for (int y = 0; y < shape.height(); ++y) {
                for (int x = 0; x < shape.width(); ++x) {
                    const int alpha = qAlpha(shape.pixel(x, y));
                    if (qAlpha(solid.pixel(x, y)) == 0) {
                        require(alpha == 0,
                                "patterned fills should respect path and painter clips");
                    } else {
                        filledPixels += alpha > 0 ? 1 : 0;
                        gapPixels += alpha == 0 ? 1 : 0;
                    }
                }
            }
            require(filledPixels > 100 && gapPixels > 100,
                    "clipped shape and text fills should retain stripes and transparent spaces");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    patternedFillsKeepUniformCoverageAcrossTileBoundaries();
    thinStripesRemainConnectedAcrossTileCorners();
    shapeAndTextPatternsRespectClippingAndShareTheirScale();
    return 0;
}
