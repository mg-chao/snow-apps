#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QImage solidContent(const QSize& size = QSize(80, 48)) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(30, 100, 210, 255));
    image.setDevicePixelRatio(2.0);
    return image;
}

void squareResultPreservesPhysicalPixels() {
    const QImage result = ScreenshotResultCompositor::compose(solidContent(), {});
    require(result.size() == QSize(80, 48), "square output dimensions changed");
    require(result.format() == QImage::Format_ARGB32_Premultiplied, "result is not premultiplied");
    require(result.devicePixelRatio() == 1.0, "result DPR is not normalized");
    require(result.pixelColor(0, 0) == QColor(30, 100, 210, 255),
            "square output changed a source pixel");
}

void roundedAndShadowedResultHasRealTransparency() {
    const QImage roundedOnly = ScreenshotResultCompositor::compose(
        solidContent(), ScreenshotResultStyle{16, 0, QColor(20, 30, 40, 220)});
    require(roundedOnly.pixelColor(0, 0).alpha() == 0,
            "rounded-only content corner is not transparent");

    const ScreenshotResultStyle style{16, 12, QColor(20, 30, 40, 220)};
    const QImage result = ScreenshotResultCompositor::compose(solidContent(), style);
    require(result.size() == QSize(104, 72), "effect insets were not added to output");
    require(result.pixelColor(0, 0).alpha() == 0, "outer corner is not transparent");
    require(result.pixelColor(12, 12).alpha() < 255,
            "rounded content leaked opaque pixels into the shadow");
    require(result.pixelColor(12 + 40, 12 + 24).alpha() == 255,
            "content center became transparent");
    const int shadowAlpha = result.pixelColor(11, 12 + 24).alpha();
    require(shadowAlpha > 0 && shadowAlpha < 255, "shadow edge is not semitransparent");
}

void layoutScalesOnlyEffectsForFractionalDpr() {
    const ScreenshotResultStyle style{10, 8, QColor()};
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        const ScreenshotResultLayout layout =
            ScreenshotResultCompositor::layoutForContent(QSize(100, 50), style, dpr);
        const int effect = qRound(8.0 * dpr);
        require(layout.isValid(), "fractional-DPR layout is invalid");
        require(layout.contentRect == QRect(effect, effect, 100, 50),
                "fractional-DPR content rect is wrong");
        require(layout.outputRect.size() == QSize(100 + effect * 2, 50 + effect * 2),
                "fractional-DPR output bounds are wrong");
    }
}

void noEffectResultSharesNormalizedStorage() {
    QImage source(QSize(32, 24), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 100, 210, 255));
    const QImage result = ScreenshotResultCompositor::compose(source, {});
    require(result.constBits() == source.constBits(), "no-effect result detached source pixels");
    require(result.size() == source.size(), "no-effect result changed dimensions");
}

void outputOpacityScalesTheCompleteComposition() {
    QImage translucent(QSize(32, 24), QImage::Format_ARGB32_Premultiplied);
    translucent.fill(QColor(30, 100, 210, 120));
    const QImage translucentResult = ScreenshotResultCompositor::compose(translucent, {}, 1.0, 0.5);
    require(qAbs(translucentResult.pixelColor(10, 10).alpha() - 60) <= 1,
            "output opacity did not scale existing source alpha");

    const ScreenshotResultStyle style{10, 8, QColor(20, 30, 40, 220)};
    const QImage opaque = ScreenshotResultCompositor::compose(solidContent(), style);
    const QImage faded = ScreenshotResultCompositor::compose(solidContent(), style, 1.0, 0.5);
    require(faded.size() == opaque.size() &&
                qAbs(faded.pixelColor(8 + 40, 8 + 24).alpha() - 128) <= 1,
            "output opacity did not scale composed content");
    const int opaqueShadowAlpha = opaque.pixelColor(7, 8 + 24).alpha();
    const int fadedShadowAlpha = faded.pixelColor(7, 8 + 24).alpha();
    require(opaqueShadowAlpha > 0 &&
                qAbs(fadedShadowAlpha - qRound(opaqueShadowAlpha * 128.0 / 255.0)) <= 1,
            "output opacity did not scale the composed shadow exactly once");

    const QImage transparent = ScreenshotResultCompositor::compose(solidContent(), {}, 1.0, -1.0);
    const QImage clampedOpaque = ScreenshotResultCompositor::compose(solidContent(), {}, 1.0, 2.0);
    const QImage invalidOpaque = ScreenshotResultCompositor::compose(
        solidContent(), {}, 1.0, std::numeric_limits<qreal>::quiet_NaN());
    require(transparent.pixelColor(10, 10).alpha() == 0,
            "negative output opacity was not clamped to transparent");
    require(clampedOpaque.pixelColor(10, 10).alpha() == 255 &&
                invalidOpaque.pixelColor(10, 10).alpha() == 255,
            "oversized or invalid output opacity did not use a safe opaque value");
}

void liveSurfaceClipsExistingCanvasPixelsBeforeAddingShadow() {
    QImage surface(QSize(70, 60), QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    {
        QPainter painter(&surface);
        painter.fillRect(QRect(6, 6, 58, 48), QColor(220, 20, 20, 255));
        ScreenshotResultCompositor::finishLiveSurface(
            painter, surface.rect(), QRectF(12, 12, 46, 36),
            ScreenshotResultStyle{10, 6, QColor(0, 0, 0, 220)}, 1.0);
    }
    require(surface.pixelColor(6, 6).alpha() == 0,
            "canvas pixels leaked outside the result and shadow bounds");
    require(surface.pixelColor(12, 12).alpha() < 255,
            "canvas pixels leaked through the rounded corner");
    require(surface.pixelColor(35, 30).alpha() == 255, "live result center was clipped");
    const int shadowAlpha = surface.pixelColor(11, 30).alpha();
    require(shadowAlpha > 0 && shadowAlpha < 255,
            "live surface did not add a semitransparent shadow behind content");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        squareResultPreservesPhysicalPixels();
        roundedAndShadowedResultHasRealTransparency();
        layoutScalesOnlyEffectsForFractionalDpr();
        noEffectResultSharesNormalizedStorage();
        outputOpacityScalesTheCompleteComposition();
        liveSurfaceClipsExistingCanvasPixelsBeforeAddingShadow();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
