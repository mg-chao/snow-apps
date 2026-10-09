#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/virtualmemory.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <cstdlib>
#include <algorithm>
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

void persistedStylePreservesQt611Format() {
    // Captured using the Qt 6.11.1 default stream format. Keep fixtures independent of the
    // running Qt version so an upgrade cannot silently change restored pin appearance.
    const QByteArray legacy = QByteArray::fromHex("0000000c0000000601dcdc14141e1e28280000");
    const auto restored = decodeScreenshotResultStyle(legacy);
    require(restored && restored->cornerRadius == 12 && restored->shadowWidth == 6 &&
                restored->shadowColor == QColor(20, 30, 40, 220) && !restored->region &&
                restored->regionScale == 1.0,
            "Qt 6.11 style records must restore their original effects");

    const QByteArray extended = QByteArray::fromHex(
        "0000000c0000000601dcdc14141e1e2828000053535247013ff4000000000000ffffffff");
    const auto current = decodeScreenshotResultStyle(extended);
    require(current && current->regionScale == 1.25 && !current->region,
            "Qt 6.11 extended style records must restore their geometry scale");
    require(encodeScreenshotResultStyle(*current) == extended,
            "writing a restored style must preserve the existing Qt 6.11 bytes");
    require(!decodeScreenshotResultStyle(extended.left(extended.size() - 1)),
            "truncated persisted geometry must remain rejected after a Qt upgrade");
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

void exportAndLiveShadowShareSoftFalloff() {
    for (const int radius : {0, 16}) {
        for (const int width : {8, 16, 32}) {
            const QColor color(40, 80, 120, 190);
            QImage content = solidContent();
            content.setDevicePixelRatio(1.0);
            const QImage result =
                ScreenshotSelectionShadowRenderer::composeExport(content, radius, width, color);
            QImage live(result.size(), QImage::Format_ARGB32_Premultiplied);
            live.fill(Qt::transparent);
            {
                QPainter painter(&live);
                ScreenshotSelectionShadowRenderer::renderResultShadow(
                    painter, QRectF(QPointF(width, width), QSizeF(content.size())), radius, width,
                    color, 1.0);
            }
            const int centerX = width + content.width() / 2;
            for (int y = 0; y < width; ++y)
                require(result.pixelColor(centerX, y) == live.pixelColor(centerX, y),
                        "exported and live shadow profiles disagree");
            const int edge = result.pixelColor(centerX, width - 1).alpha();
            require(result.pixelColor(centerX, width - 1 - (width + 5) / 6).alpha() <
                        qRound(color.alpha() * 0.18),
                    "exported shadow retains an overly wide dense band");
            require(result.pixelColor(centerX, width - 1 - width / 4).alpha() * 2 < edge &&
                        result.pixelColor(centerX, width - 1 - width / 2).alpha() * 5 < edge,
                    "exported shadow has a broad opaque shelf");
            require(result.pixelColor(centerX, 0).alpha() == 0,
                    "exported shadow has a visible outer cutoff");
            require(result.pixelColor(centerX, width + content.height() / 2) ==
                        content.pixelColor(content.width() / 2, content.height() / 2),
                    "soft shadow changed the screenshot content");
        }
    }
}

void roundedAlphaMaskMatchesArgbReference() {
    for (const QSize size : {QSize(41, 29), QSize(1025, 513)}) {
        QImage source(size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                source.setPixelColor(x, y,
                                     QColor((x * 17 + y) % 256, (x + y * 13) % 256,
                                            (x * 3 + y * 5) % 256, (x + y * 7) % 256));
            }
        }
        const QImage original = source.copy();
        for (const qreal dpr : {1.0, 1.25, 1.75, 2.0}) {
            for (const int radius : {1, 13, 2000}) {
                const ScreenshotResultStyle style{radius, 0, QColor()};
                const auto layout = ScreenshotResultCompositor::layoutForContent(size, style, dpr);
                QImage expected(layout.outputRect.size(), QImage::Format_ARGB32_Premultiplied);
                expected.fill(Qt::transparent);
                QImage mask(expected.size(), QImage::Format_ARGB32_Premultiplied);
                mask.fill(Qt::transparent);
                QPainterPath path;
                const qreal physicalRadius = std::min<qreal>(
                    ScreenshotResultCompositor::normalizedStyle(style).cornerRadius *
                        layout.devicePixelRatio,
                    std::min(layout.contentRect.width(), layout.contentRect.height()) / 2.0);
                path.addRoundedRect(layout.contentRect, physicalRadius, physicalRadius,
                                    Qt::AbsoluteSize);
                {
                    QPainter painter(&mask);
                    painter.setRenderHint(QPainter::Antialiasing, true);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(Qt::white);
                    painter.drawPath(path);
                }
                {
                    QPainter painter(&expected);
                    painter.setRenderHint(QPainter::Antialiasing, true);
                    painter.drawImage(layout.contentRect, source);
                    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                    painter.drawImage(QPoint(), mask);
                }
                const QImage actual = ScreenshotResultCompositor::compose(source, style, dpr);
                require(actual == expected,
                        "Alpha8 rounded masks must exactly preserve ARGB mask pixels and alpha");
                require(source == original, "rounded composition must preserve its source");
            }
        }
    }
}

void compoundCompositionStartsTransparentForSmallAndLargeImages() {
    for (const QSize size : {QSize(80, 48), QSize(1025, 513)}) {
        QImage source(size, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(30, 100, 210, 128));
        const QRect inset = source.rect().adjusted(4, 6, -8, -10);
        QPainterPath ellipse;
        ellipse.addEllipse(inset);
        for (const ScreenshotRegionGeometry& region :
             {ScreenshotRegionGeometry(QRegion(inset).subtracted(QRect(10, 10, 8, 8))),
              ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)}) {
            ScreenshotResultStyle style;
            style.region = region;
            QImage expected(size, QImage::Format_ARGB32_Premultiplied);
            expected.fill(Qt::transparent);
            QImage mask(size, QImage::Format_Alpha8);
            mask.fill(0);
            {
                QPainter painter(&mask);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::white);
                painter.drawPath(region.custom() ? region.path() : screenshotRegionPath(region));
            }
            {
                QPainter painter(&expected);
                painter.drawImage(QPoint(), source);
                painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                painter.drawImage(QPoint(), mask);
            }
            for (int iteration = 0; iteration < 3; ++iteration) {
                if (iteration == 0)
                    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
                require(ScreenshotResultCompositor::compose(source, style) == expected,
                        "compound composition must start transparent and preserve source alpha");
            }
        }
    }
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
}

void previewAssetsAreReleasedAfterCapture() {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    const QImage exported = ScreenshotResultCompositor::compose(
        solidContent(), ScreenshotResultStyle{16, 12, QColor(20, 30, 40, 220)});
    require(!exported.isNull(), "the shadowed export must render");
    const auto afterExport = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(afterExport.retainedEntries == 0 && afterExport.regionCacheRetainedBytes == 0,
            "one-off export composition must not retain preview assets");

    QImage preview(QSize(104, 72), QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::transparent);
    {
        QPainter painter(&preview);
        ScreenshotSelectionShadowRenderer::renderPreview(painter, QRectF(12, 12, 80, 48), 16, 12,
                                                         QColor(20, 30, 40, 220), 1.0);
    }
    const auto retained = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(retained.retainedEntries == 1 && retained.retainedBytes > 0,
            "an active preview must retain its shadow asset");

    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    const auto released = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(released.retainedEntries == 0 && released.retainedBytes == 0 &&
                released.regionCacheRetainedBytes == 0,
            "capture cleanup must release selection preview caches");

    {
        QPainter painter(&preview);
        ScreenshotSelectionShadowRenderer::renderPreview(painter, QRectF(12, 12, 80, 48), 16, 12,
                                                         QColor(20, 30, 40, 220), 1.0);
    }
    const auto rebuilt = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(rebuilt.retainedEntries == 1 && rebuilt.retainedBytes > 0,
            "the next capture must be able to rebuild the shadow preview asset");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
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

void liveSurfacePreservesContentBeyondRoundedWidgetBounds() {
    // A native client is sized in physical pixels. Its smallest covering Qt
    // widget can end at a fractional device pixel before the content ends.
    // In particular, 381 DIPs at 1.75x cover a 667-pixel client after rounding.
    for (const qreal dpr : {1.25, 1.5, 1.75, 2.0}) {
        for (const QSize size : {QSize(869, 937), QSize(1000, 667), QSize(667, 1000)}) {
            QImage surface(size, QImage::Format_ARGB32_Premultiplied);
            surface.setDevicePixelRatio(dpr);
            surface.fill(QColor(30, 100, 210, 255));
            const QImage expected = surface.copy();
            const QRectF content(QPointF(), QSizeF(size) / dpr);
            const QRectF widget(0, 0, qRound(content.width()), qRound(content.height()));
            {
                QPainter painter(&surface);
                ScreenshotResultCompositor::finishLiveSurface(painter, widget, content, {}, dpr,
                                                              1.0 / dpr);
            }
            require(surface == expected,
                    "rounded widget bounds must not erase fractional strips of content");
        }
    }
}

void liveSurfaceSubtractsIntersectingContent() {
    const QRect viewport(8, 8, 32, 24);
    for (const QRect content :
         {QRect(4, 4, 44, 36), QRect(12, 12, 20, 12), QRect(4, 12, 24, 28), QRect(24, 4, 24, 24)}) {
        QImage surface(QSize(52, 44), QImage::Format_ARGB32_Premultiplied);
        const QColor color(30, 100, 210, 128);
        surface.fill(color);
        QImage expected = surface.copy();
        for (int y = 0; y < surface.height(); ++y) {
            for (int x = 0; x < surface.width(); ++x) {
                if (viewport.contains(x, y) && !content.contains(x, y))
                    expected.setPixel(x, y, 0);
            }
        }
        {
            QPainter painter(&surface);
            ScreenshotResultCompositor::finishLiveSurface(painter, viewport, content, {}, 1.0);
        }
        require(surface == expected,
                "live clipping must subtract content instead of XORing intersecting rectangles");
    }
}

void liveSurfacePreservesTranslatedFractionalContent() {
    QImage surface(QSize(868, 936), QImage::Format_ARGB32_Premultiplied);
    surface.setDevicePixelRatio(1.25);
    surface.fill(QColor(30, 100, 210, 255));
    const QImage expected = surface.copy();
    const QTransform transform(0.79999999999999993, 0, 0, 0.79999999999999993, 312.80000000000001,
                               -29.599999999999966);
    const QRectF content = transform.mapRect(QRectF(-391, 37, 868, 936));
    {
        QPainter painter(&surface);
        ScreenshotResultCompositor::finishLiveSurface(painter, QRectF(0, 0, 694, 749), content, {},
                                                      1.25, transform.m11());
    }
    require(surface == expected, "camera roundoff must not erase translated content");
}

void sharedMetadataAndOpacityKeepMappedOwnership() {
    QImage source = snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied);
    require(!source.isNull(), "large composition fixture must allocate");
    source.fill(QColor(200, 100, 50));
    source.setDevicePixelRatio(2.0);
    QImage result = ScreenshotResultCompositor::compose(source, {}, 1.0, 0.5);
    require(!result.isNull() && source.devicePixelRatio() == 2.0 &&
                source.pixelColor(0, 0).alpha() == 255 && result.pixelColor(0, 0).alpha() == 128,
            "metadata normalization and opacity must preserve the shared source");
    const auto* middle = result.constBits() + result.sizeInBytes() / 2;
    require(snow::test_support::virtualMemoryMapped(middle),
            "compositor mutations must retain managed output storage");
    result = {};
    require(!snow::test_support::virtualMemoryMapped(middle),
            "compositor output must release its final pixel pages");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        persistedStylePreservesQt611Format();
        squareResultPreservesPhysicalPixels();
        sharedMetadataAndOpacityKeepMappedOwnership();
        roundedAndShadowedResultHasRealTransparency();
        exportAndLiveShadowShareSoftFalloff();
        roundedAlphaMaskMatchesArgbReference();
        compoundCompositionStartsTransparentForSmallAndLargeImages();
        previewAssetsAreReleasedAfterCapture();
        layoutScalesOnlyEffectsForFractionalDpr();
        noEffectResultSharesNormalizedStorage();
        outputOpacityScalesTheCompleteComposition();
        liveSurfaceClipsExistingCanvasPixelsBeforeAddingShadow();
        liveSurfacePreservesContentBeyondRoundedWidgetBounds();
        liveSurfaceSubtractsIntersectingContent();
        liveSurfacePreservesTranslatedFractionalContent();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
