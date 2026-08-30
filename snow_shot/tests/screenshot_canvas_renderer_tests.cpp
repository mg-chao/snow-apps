#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotguidelinerendering.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snow_shot/presentation/screenshotshortcuthints.h"
#include "snow_shot/presentation/screenshotuipreferences.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "widgets/message.h"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFontDatabase>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QScrollBar>
#include <QWheelEvent>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class NoopOverlayEventSink final : public ScreenshotOverlayEventSink {
  public:
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }

    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}

    bool handleOverlayRightClick(ScreenshotOverlayWindow*, const QPointF&) override {
        return false;
    }

    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QPointF&, const QPoint&,
                            const QPoint&) override {
        return false;
    }

    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }

    void raiseToolbarForCanvasInteraction() override {}
};

class WheelTestCanvas final : public SnowCanvasWidget {
  public:
    void dispatchWheel(QWheelEvent* event) {
        wheelEvent(event);
    }
};

class CanvasPaintObserver final : public QObject {
  public:
    explicit CanvasPaintObserver(ScreenshotOverlayWindow& overlay) : m_overlay(overlay) {}

    void begin() {
        m_observing = true;
        m_sawPaint = false;
        m_maskWasEmpty = true;
    }

    [[nodiscard]] bool sawPaint() const {
        return m_sawPaint;
    }

    [[nodiscard]] bool maskWasEmpty() const {
        return m_maskWasEmpty;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (m_observing && event != nullptr && event->type() == QEvent::Paint) {
            m_sawPaint = true;
            m_maskWasEmpty = m_maskWasEmpty && m_overlay.mask().isEmpty();
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    ScreenshotOverlayWindow& m_overlay;
    bool m_observing = false;
    bool m_sawPaint = false;
    bool m_maskWasEmpty = true;
};

class CanvasPaintRegionObserver final : public QObject {
  public:
    void begin() {
        m_region = {};
        m_observing = true;
    }

    [[nodiscard]] QRegion region() const {
        return m_region;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (m_observing && event != nullptr && event->type() == QEvent::Paint) {
            m_region += static_cast<QPaintEvent*>(event)->region();
        }
        return false;
    }

  private:
    QRegion m_region;
    bool m_observing = false;
};

QImage renderCanvas(SnowCanvasWidget& canvas, qreal devicePixelRatio = 1.0) {
    const QSize deviceSize(qCeil(canvas.width() * devicePixelRatio),
                           qCeil(canvas.height() * devicePixelRatio));
    QImage output(deviceSize, QImage::Format_RGBA8888);
    output.setDevicePixelRatio(devicePixelRatio);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    painter.end();
    return output;
}

void layeredImageSourceMatchesMaterializedOutput() {
    QImage leftImage(QSize(80, 60), QImage::Format_ARGB32_Premultiplied);
    QImage rightImage(QSize(70, 50), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < leftImage.height(); ++y) {
        for (int x = 0; x < leftImage.width(); ++x) {
            leftImage.setPixelColor(x, y, QColor((x * 5) % 256, (y * 7) % 256, 31, 255));
        }
    }
    for (int y = 0; y < rightImage.height(); ++y) {
        for (int x = 0; x < rightImage.width(); ++x) {
            rightImage.setPixelColor(x, y, QColor(197, (x * 3) % 256, (y * 11) % 256, 255));
        }
    }

    const QRectF selection(0.0, 0.0, 100.0, 50.0);
    ScreenshotImageSource source = ScreenshotImageSource::fromLayers({
        ScreenshotImageLayer{leftImage, QRectF(-20.0, -10.0, 80.0, 60.0),
                             QRectF(0.0, 0.0, 60.0, 50.0)},
        ScreenshotImageLayer{rightImage, QRectF(60.0, 0.0, 70.0, 50.0),
                             QRectF(60.0, 0.0, 40.0, 50.0)},
    });
    require(source.isLayered(), "layered screenshot source fixture should be valid");

    const QImage materialized =
        materializeScreenshotImageSource(source, selection, selection.size().toSize());
    require(!materialized.isNull() && materialized.size() == QSize(100, 50),
            "layered screenshot source should materialize at selection size");

    SnowCanvasWidget canvas;
    ScreenshotCanvasRenderer renderer(canvas);
    renderer.setImageSource(source);
    QImage direct(materialized.size(), QImage::Format_ARGB32_Premultiplied);
    direct.fill(Qt::transparent);
    QPainter painter(&direct);
    renderer.renderBeforeCanvas(
        painter, SnowCanvasRenderContext{direct.rect(), QRegion(direct.rect()), QTransform(), 1.0});
    painter.end();

    require(direct == materialized,
            "direct layered rendering and lazy materialization should be pixel equivalent");
    require(direct.pixelColor(0, 0) == leftImage.pixelColor(20, 10) &&
                direct.pixelColor(99, 49) == rightImage.pixelColor(39, 49),
            "layer mapping should preserve cropped source coordinates on both displays");
}

void physicalViewportRenderingPreservesEveryPixelAtFractionalDprs() {
    const QSize physicalSize(321, 181);
    QImage source(physicalSize, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y,
                                 QColor((x * 37 + y * 17 + 11) % 256, (x * 13 + y * 43 + 29) % 256,
                                        (x * 53 + y * 7 + 47) % 256, 255));
        }
    }

    SnowCanvasWidget canvas;
    ScreenshotCanvasRenderer renderer(canvas);
    renderer.setImage(source, QRectF(QPointF(), QSizeF(physicalSize)));
    renderer.setImageViewportPhysicalSize(physicalSize);

    constexpr std::array<qreal, 3> devicePixelRatios{1.25, 1.5, 1.75};
    for (const qreal devicePixelRatio : devicePixelRatios) {
        QImage output(physicalSize, QImage::Format_RGBA8888);
        output.setDevicePixelRatio(devicePixelRatio);
        output.fill(QColor(1, 2, 3));

        QPainter painter(&output);
        const QRect logicalViewport(QPoint(),
                                    QSize(qCeil(physicalSize.width() / devicePixelRatio),
                                          qCeil(physicalSize.height() / devicePixelRatio)));
        const SnowCanvasRenderContext context{
            logicalViewport,
            QRegion(logicalViewport),
            QTransform(),
            devicePixelRatio,
        };
        renderer.renderBeforeCanvas(painter, context);
        painter.end();

        require(output.size() == source.size(),
                "fractional-DPI physical rendering should preserve raw dimensions");
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                require(output.pixel(x, y) == source.pixel(x, y),
                        "fractional-DPI physical rendering should preserve every raw pixel");
            }
        }
    }
}

QImage renderMaterializedImage(const QImage& source, const QSize& targetSize,
                               const QRegion& exposedRegion, const QColor& background,
                               bool smooth = false) {
    SnowCanvasWidget canvas;
    ScreenshotCanvasRenderer renderer(canvas);
    renderer.setImage(source, QRectF(QPointF(), QSizeF(targetSize)));

    QImage output(targetSize, QImage::Format_ARGB32_Premultiplied);
    output.fill(background);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    renderer.renderBeforeCanvas(
        painter, SnowCanvasRenderContext{output.rect(), exposedRegion, QTransform(), 1.0});
    painter.end();
    return output;
}

QImage verticalRasterPattern(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        const QRgb pixel = qRgb((y * 17 + 3) % 256, (y * 29 + 11) % 256, (y * 47 + 19) % 256);
        std::fill_n(reinterpret_cast<QRgb*>(image.scanLine(y)), image.width(), pixel);
    }
    return image;
}

QImage horizontalRasterPattern(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] = qRgb((x * 13 + 5) % 256, (x * 31 + 7) % 256, (x * 43 + 23) % 256);
        }
    }
    return image;
}

QImage renderWithSafeReferenceTiles(const QImage& source, const QSize& targetSize,
                                    bool smooth = false, qreal devicePixelRatio = 1.0) {
    const QSize deviceSize(qCeil(targetSize.width() * devicePixelRatio),
                           qCeil(targetSize.height() * devicePixelRatio));
    QImage output(deviceSize, QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(devicePixelRatio);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    constexpr int kTargetTileSize = 100;
    for (int targetTop = 0; targetTop < targetSize.height(); targetTop += kTargetTileSize) {
        const int targetBottom = qMin(targetTop + kTargetTileSize, targetSize.height());
        for (int targetLeft = 0; targetLeft < targetSize.width(); targetLeft += kTargetTileSize) {
            const int targetRight = qMin(targetLeft + kTargetTileSize, targetSize.width());
            const QRectF targetTile(targetLeft, targetTop, targetRight - targetLeft,
                                    targetBottom - targetTop);
            const QRectF sampledTarget =
                smooth ? targetTile.adjusted(-1.0, -1.0, 1.0, 1.0)
                             .intersected(QRectF(QPointF(), QSizeF(targetSize)))
                       : targetTile;
            const QRectF sourceTile(
                static_cast<qreal>(source.width()) * sampledTarget.left() / targetSize.width(),
                static_cast<qreal>(source.height()) * sampledTarget.top() / targetSize.height(),
                static_cast<qreal>(source.width()) * sampledTarget.width() / targetSize.width(),
                static_cast<qreal>(source.height()) * sampledTarget.height() / targetSize.height());
            const QRect sourceBounds = sourceTile.toAlignedRect().intersected(source.rect());
            const QImage sourceWindow = source.copy(sourceBounds);
            require(!sourceWindow.isNull(), "the safe reference source tile should be available");

            painter.save();
            painter.setClipRect(targetTile);
            painter.drawImage(sampledTarget, sourceWindow,
                              sourceTile.translated(-sourceBounds.topLeft()));
            painter.restore();
        }
    }
    painter.end();
    return output;
}

void smoothLargeImageChunkBoundariesRemainPixelEquivalent() {
    constexpr int kLargeDimension = 70000;
    const QImage source = verticalRasterPattern(QSize(5, kLargeDimension));
    const QSize targetSize(5, 7000);
    const QImage expected = renderWithSafeReferenceTiles(source, targetSize, true);
    const QImage actual = renderMaterializedImage(
        source, targetSize, QRegion(QRect(QPoint(), targetSize)), QColor(1, 2, 3), true);
    require(actual == expected,
            "smooth large-image chunks should preserve sampling across every chunk boundary");
}

void largeRasterSourceExtentsRenderWithoutFixedPointWrap() {
    constexpr int kLargeDimension = 70000;
    constexpr int kScaledDimension = 7000;
    const QColor background(1, 2, 3);

    const QImage tall = verticalRasterPattern(QSize(5, kLargeDimension));
    const QImage expectedTall = renderWithSafeReferenceTiles(tall, QSize(5, kScaledDimension));
    const QImage actualTall = renderMaterializedImage(tall, expectedTall.size(),
                                                      QRegion(expectedTall.rect()), background);
    require(actualTall == expectedTall,
            "a source taller than the raster fixed-point range should downscale without wrapping");

    const QImage wide = horizontalRasterPattern(QSize(kLargeDimension, 5));
    const QImage expectedWide = renderWithSafeReferenceTiles(wide, QSize(kScaledDimension, 5));
    const QImage actualWide = renderMaterializedImage(wide, expectedWide.size(),
                                                      QRegion(expectedWide.rect()), background);
    require(actualWide == expectedWide,
            "a source wider than the raster fixed-point range should downscale without wrapping");
}

void extremeImageDownscaleUsesSafePreprocessing() {
    constexpr int kLargeDimension = 70000;
    const QImage source = verticalRasterPattern(QSize(3, kLargeDimension));
    const QImage expected =
        source.scaled(QSize(3, 2), Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const QImage actual =
        renderMaterializedImage(source, expected.size(), QRegion(expected.rect()), QColor(1, 2, 3));
    require(actual == expected,
            "an extreme source-to-target ratio should be reduced before raster painting");

    const QImage smoothExpected =
        source.scaled(QSize(3, 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage smoothActual = renderMaterializedImage(
        source, smoothExpected.size(), QRegion(smoothExpected.rect()), QColor(1, 2, 3), true);
    require(smoothActual == smoothExpected,
            "an extreme smooth downscale should use the matching QImage transformation mode");
}

void indexedLargeImageWindowsPreserveTheirColorTable() {
    constexpr int kLargeDimension = 70000;
    QImage source(QSize(4, kLargeDimension), QImage::Format_Indexed8);
    source.setColorTable(
        {qRgb(17, 31, 47), qRgb(83, 97, 113), qRgb(149, 163, 179), qRgb(211, 223, 239)});
    for (int y = 0; y < source.height(); ++y) {
        std::fill_n(source.scanLine(y), source.width(), static_cast<uchar>((y / 7) % 4));
    }

    const QSize targetSize(4, 7000);
    const QImage expected = renderWithSafeReferenceTiles(source, targetSize);
    const QImage actual = renderMaterializedImage(
        source, targetSize, QRegion(QRect(QPoint(), targetSize)), QColor(1, 2, 3));
    require(actual == expected,
            "rebased indexed image windows should preserve the source color table");
}

void disjointLargeImageExposureDoesNotPaintItsBoundingInterval() {
    constexpr int kLargeDimension = 70000;
    const QImage source = verticalRasterPattern(QSize(4, kLargeDimension));
    const QSize targetSize(4, 7000);
    const QImage reference = renderWithSafeReferenceTiles(source, targetSize);
    const QColor background(9, 7, 5);
    QRegion exposed(QRect(0, 100, targetSize.width(), 8));
    exposed += QRect(0, 6980, targetSize.width(), 8);

    const QImage actual = renderMaterializedImage(source, targetSize, exposed, background);
    for (int y = 0; y < targetSize.height(); ++y) {
        for (int x = 0; x < targetSize.width(); ++x) {
            const QColor expected =
                exposed.contains(QPoint(x, y)) ? reference.pixelColor(x, y) : background;
            require(actual.pixelColor(x, y) == expected,
                    "disjoint exposure must not repaint the interval between damaged rectangles");
        }
    }
}

void ordinaryExposedImageRenderingRemainsPixelEquivalent() {
    const QImage source = verticalRasterPattern(QSize(31, 23));
    const QSize targetSize(47, 37);
    const QColor background(12, 14, 16);
    QRegion exposed(QRect(2, 3, 11, 9));
    exposed += QRect(31, 20, 13, 12);

    QImage expected(targetSize, QImage::Format_ARGB32_Premultiplied);
    expected.fill(background);
    QPainter expectedPainter(&expected);
    expectedPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    expectedPainter.setClipRegion(exposed);
    expectedPainter.drawImage(QRectF(QPointF(), QSizeF(targetSize)), source, QRectF(source.rect()));
    expectedPainter.end();

    const QImage actual = renderMaterializedImage(source, targetSize, exposed, background, true);
    require(actual == expected,
            "ordinary exposed image rendering should remain pixel equivalent to one drawImage");
}

void chunkedImagePaintersRenderPastTheRasterCoordinateLimit() {
    constexpr int kLargeDimension = 70000;
    const QImage source = verticalRasterPattern(QSize(4, kLargeDimension));
    SnowCanvasWidget canvas;
    canvas.resize(source.size());
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(source.width() / 2.0, source.height() / 2.0, 1.0),
            "the tall image camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    renderer.setImage(source, QRectF(QPointF(), QSizeF(source.size())));
    canvas.setCustomRenderer(&renderer);
    const qreal devicePixelRatio = canvas.devicePixelRatioF();
    const QImage output = renderCanvas(canvas, devicePixelRatio);
    const QImage expected =
        renderWithSafeReferenceTiles(source, source.size(), false, devicePixelRatio);
    require(output.size() == expected.size(),
            "the tall image render should keep its physical dimensions");
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            const QColor actualColor = output.pixelColor(x, y);
            const QColor expectedColor = expected.pixelColor(x, y);
            const bool withinTileCompositingTolerance =
                std::abs(actualColor.red() - expectedColor.red()) <= 1 &&
                std::abs(actualColor.green() - expectedColor.green()) <= 1 &&
                std::abs(actualColor.blue() - expectedColor.blue()) <= 1 &&
                std::abs(actualColor.alpha() - expectedColor.alpha()) <= 1;
            require(
                withinTileCompositingTolerance,
                "translated tile painters should preserve pixels above source coordinate 65535");
        }
    }
    canvas.setCustomRenderer(nullptr);
}

void partialRoundedMaskMatchesFullViewportMaskAtFractionalDpr() {
    SnowCanvasWidget canvas;
    ScreenshotCanvasRenderer renderer(canvas);
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(25.0, 25.0, 30.0, 30.0), false, 12);
    renderer.setSelectionBorderVisible(false);

    constexpr qreal devicePixelRatio = 1.5;
    const QRect viewport(0, 0, 100, 100);
    const QRegion partialExposure(QRect(10, 13, 57, 61));
    const auto renderPartial = [&](const QRegion& contextExposure) {
        QImage output(QSize(150, 150), QImage::Format_ARGB32_Premultiplied);
        output.setDevicePixelRatio(devicePixelRatio);
        output.fill(QColor(0, 80, 240));
        QPainter painter(&output);
        painter.setClipRegion(partialExposure);
        renderer.renderAfterCanvas(painter, SnowCanvasRenderContext{
                                                viewport,
                                                contextExposure,
                                                QTransform(),
                                                devicePixelRatio,
                                            });
        painter.end();
        return output;
    };

    require(renderPartial(partialExposure) == renderPartial(QRegion(viewport)),
            "a partial rounded-mask repaint must not create antialiased edges at the damage "
            "region's bottom or right boundary");
}

int ocrTextItemCount(SnowCanvasWidget& canvas) {
    const auto* textLayer =
        canvas.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    return textLayer != nullptr && textLayer->scene() != nullptr
               ? static_cast<int>(textLayer->scene()->items().size())
               : 0;
}

QRect paintedInkBounds(const QImage& image, const QRect& region, int maximumLightness = 224) {
    QRect bounds;
    const QRect scanRegion = region.intersected(image.rect());
    for (int y = scanRegion.top(); y <= scanRegion.bottom(); ++y) {
        for (int x = scanRegion.left(); x <= scanRegion.right(); ++x) {
            if (image.pixelColor(x, y).lightness() < maximumLightness) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

void requireChangedPixelsCoveredByDirtyRegion(const QImage& previous, const QImage& next,
                                              const QRegion& dirty, const char* message) {
    require(previous.size() == next.size(), "rendered frames should have equal sizes");
    require(qFuzzyCompare(previous.devicePixelRatio(), next.devicePixelRatio()),
            "rendered frames should have equal device pixel ratios");

    const qreal devicePixelRatio = previous.devicePixelRatio();
    for (int y = 0; y < previous.height(); ++y) {
        for (int x = 0; x < previous.width(); ++x) {
            if (previous.pixel(x, y) == next.pixel(x, y)) {
                continue;
            }

            const QPoint logicalPoint(qFloor(x / devicePixelRatio), qFloor(y / devicePixelRatio));
            if (!dirty.contains(logicalPoint)) {
                std::cerr << message << " at device pixel (" << x << ", " << y
                          << "), logical pixel (" << logicalPoint.x() << ", " << logicalPoint.y()
                          << "), dirty bounds " << dirty.boundingRect().x() << ","
                          << dirty.boundingRect().y() << " " << dirty.boundingRect().width() << "x"
                          << dirty.boundingRect().height() << '\n';
                std::exit(1);
            }
        }
    }
}

QColor sourceOverOpaqueBackground(const QColor& source, const QColor& background) {
    const qreal alpha = source.alphaF();
    return QColor(qRound(source.red() * alpha + background.red() * (1.0 - alpha)),
                  qRound(source.green() * alpha + background.green() * (1.0 - alpha)),
                  qRound(source.blue() * alpha + background.blue() * (1.0 - alpha)), 255);
}

void screenshotUiPreferencesNormalizeAndApplyPickerVisibilityPolicies() {
    ScreenshotUiPreferences preferences;
    preferences.selectionMaskColor = QColor();
    preferences.shortcutHintOpacity = 1.5;
    preferences.cursorGuideLineColor = QColor();
    preferences.monitorCenterGuideLineColor = QColor();
    preferences.colorPickerCenterGuideLineColor = QColor();
    const ScreenshotUiPreferences normalized = preferences.normalized();

    require(normalized.selectionMaskColor == QColor(0, 0, 0, 128),
            "invalid screenshot mask colors must normalize to the default mask");
    require(normalized.shortcutHintOpacity == 1.0,
            "shortcut hint opacity must normalize to its maximum");
    require(normalized.cursorGuideLineColor == QColor(0, 0, 0, 0) &&
                normalized.monitorCenterGuideLineColor == QColor(0, 0, 0, 0) &&
                normalized.colorPickerCenterGuideLineColor == QColor(0, 0, 0, 0),
            "invalid screenshot guide colors must normalize to transparent");
    preferences.shortcutHintOpacity = -0.25;
    require(preferences.normalized().shortcutHintOpacity == 0.0,
            "shortcut hint opacity must normalize to its minimum");
    require(screenshotColorPickerDisplayModeFromString(QStringLiteral("always_show")) ==
                    ScreenshotColorPickerDisplayMode::AlwaysShow &&
                screenshotColorPickerDisplayModeFromString(QStringLiteral("always_hide")) ==
                    ScreenshotColorPickerDisplayMode::AlwaysHide &&
                screenshotColorPickerDisplayModeFromString(QStringLiteral("unknown")) ==
                    ScreenshotColorPickerDisplayMode::HideOutsideSelection,
            "color picker display-mode strings must use the documented values and fallback");

    ScreenshotColorPickerVisibilityState state;
    state.manualSelecting = true;
    state.hasSelection = true;
    state.pointInsideSelection = false;
    require(screenshotColorPickerOpacity(ScreenshotColorPickerDisplayMode::HideOutsideSelection,
                                         state) == 0.0,
            "hide-outside-selection mode must hide the picker outside the selection");
    require(screenshotColorPickerOpacity(ScreenshotColorPickerDisplayMode::AlwaysShow, state) ==
                1.0,
            "always-show mode must keep the picker visible outside the selection");

    state.dragging = true;
    state.selectionDrag = true;
    require(screenshotColorPickerOpacity(ScreenshotColorPickerDisplayMode::AlwaysHide, state) ==
                0.0,
            "always-hide mode must override selection-drag picker visibility without changing "
            "the underlying color-sampling feature");
}

void shortcutHintStagesUseTheExactRequiredLines() {
    const QStringList cursorLines{
        QStringLiteral("Move cursor up: W / Up"),
        QStringLiteral("Move cursor down: S / Down"),
        QStringLiteral("Move cursor left: A / Left"),
        QStringLiteral("Move cursor right: D / Right"),
    };
    const QStringList commonLines{
        QStringLiteral("Select Previously Selected Area: R"),
        QStringLiteral("Copy Color: C"),
        QStringLiteral("Switch Color Format: Shift"),
        QStringLiteral("Switch Screenshot History: , / ."),
    };
    QStringList selectionLines = cursorLines;
    selectionLines.append({
        QStringLiteral("Move Entire Selection: Space"),
        QStringLiteral("Keep Selection Width and Height Consistent: Shift"),
    });
    selectionLines.append(commonLines);
    QStringList smartLines = cursorLines;
    smartLines.append({
        QStringLiteral("Switch element level: mouse wheel"),
        QStringLiteral("Select Window/Window Sub-element: Tab"),
    });
    smartLines.append(commonLines);

    ScreenshotShortcutHintContext hintContext;
    hintContext.activeTool = ScreenshotActiveTool::Move;

    hintContext.captureMode = ScreenshotCaptureMode::IntelligentSelecting;
    const ScreenshotShortcutHintMode smartMode = screenshotShortcutHintModeForContext(hintContext);
    const QStringList smartContextLines = screenshotShortcutHintLines(hintContext);

    hintContext.captureMode = ScreenshotCaptureMode::ManualSelecting;
    const ScreenshotShortcutHintMode manualMode = screenshotShortcutHintModeForContext(hintContext);
    const QStringList manualContextLines = screenshotShortcutHintLines(hintContext);

    hintContext.captureMode = ScreenshotCaptureMode::MovingSelection;
    const ScreenshotShortcutHintMode confirmedMoveMode =
        screenshotShortcutHintModeForContext(hintContext);
    const QStringList confirmedMoveContextLines = screenshotShortcutHintLines(hintContext);

    require(smartMode == ScreenshotShortcutHintMode::SmartSelection &&
                smartContextLines == smartLines,
            "smart selection must show the context-appropriate shortcut hint lines");
    require(manualMode == ScreenshotShortcutHintMode::Selection &&
                manualContextLines == selectionLines,
            "manual selection must show the exact shortcut hint lines");
    require(confirmedMoveMode == ScreenshotShortcutHintMode::Selection &&
                confirmedMoveContextLines == selectionLines,
            "a confirmed selection with Move active must show the manual hint lines");

    hintContext.activeTool = ScreenshotActiveTool::Select;
    require(screenshotShortcutHintModeForContext(hintContext) ==
                    ScreenshotShortcutHintMode::Hidden &&
                screenshotShortcutHintLines(hintContext).isEmpty(),
            "shortcut hints must be hidden outside the three required stages");
}

void configurableSelectionMaskUsesRequestedPixels() {
    SnowCanvasWidget canvas;
    canvas.resize(40, 30);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0),
            "the custom mask test should initialize the camera");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QColor maskColor(17, 93, 201, 181);
    renderer.setMaskColor(maskColor);
    renderer.setMaskVisible(true);

    require(renderer.maskColor() == maskColor,
            "the screenshot renderer must retain a custom mask color");
    require(renderCanvas(canvas).pixelColor(4, 5) == maskColor,
            "the screenshot renderer must paint the exact custom mask pixels");

    renderer.setMaskColor(QColor());
    require(renderer.maskColor() == QColor(0, 0, 0, 128),
            "an invalid custom mask color must restore the safe default");
    canvas.setCustomRenderer(nullptr);
}

int maximumPixelsOfColorInAnyColumn(const QImage& image, const QColor& color) {
    int maximum = 0;
    for (int x = 0; x < image.width(); ++x) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            count += image.pixelColor(x, y) == color ? 1 : 0;
        }
        maximum = std::max(maximum, count);
    }
    return maximum;
}

bool imageRectContainsColor(const QImage& image, const QRect& rect, const QColor& color) {
    const QRect bounded = rect.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (image.pixelColor(x, y) == color) {
                return true;
            }
        }
    }
    return false;
}

void cursorAndMonitorGuideLinesUseDashedAndSolidPixels() {
    constexpr int kGuideSize = 48;
    const QRectF bounds(0.0, 0.0, kGuideSize, kGuideSize);
    const QColor cursorColor(220, 30, 40);
    const QColor monitorColor(30, 80, 220);

    QImage cursorGuide(kGuideSize, kGuideSize, QImage::Format_RGBA8888);
    cursorGuide.fill(Qt::transparent);
    {
        QPainter painter(&cursorGuide);
        paintScreenshotGuideLineCrosshair(painter, bounds, QPointF(13.0, 19.0), cursorColor, true);
    }
    const int dashedColumnPixels = maximumPixelsOfColorInAnyColumn(cursorGuide, cursorColor);
    require(dashedColumnPixels > 0 && dashedColumnPixels < kGuideSize - 2,
            "the cursor guide line must contain visible dash gaps");

    QImage monitorGuide(kGuideSize, kGuideSize, QImage::Format_RGBA8888);
    monitorGuide.fill(Qt::transparent);
    {
        QPainter painter(&monitorGuide);
        paintScreenshotGuideLineCrosshair(painter, bounds, bounds.center(), monitorColor, false);
    }
    require(maximumPixelsOfColorInAnyColumn(monitorGuide, monitorColor) >= kGuideSize - 1,
            "the monitor-center guide line must be solid across the viewport");

    QImage disabledGuide(kGuideSize, kGuideSize, QImage::Format_RGBA8888);
    disabledGuide.fill(Qt::transparent);
    {
        QPainter painter(&disabledGuide);
        paintScreenshotGuideLineCrosshair(painter, bounds, bounds.center(), QColor(10, 20, 30, 0),
                                          true);
    }
    for (int y = 0; y < disabledGuide.height(); ++y) {
        for (int x = 0; x < disabledGuide.width(); ++x) {
            require(disabledGuide.pixelColor(x, y).alpha() == 0,
                    "transparent guide colors must disable guide rendering");
        }
    }
}

qint64 regionArea(const QRegion& region) {
    qint64 area = 0;
    for (const QRect& rect : region) {
        area += static_cast<qint64>(rect.width()) * rect.height();
    }
    return area;
}

void cursorGuideLineMovementInvalidatesOnlyChangedAxes() {
    const QRect viewport(0, 0, 640, 480);
    const QColor cursorColor(220, 30, 40);
    const QColor monitorColor(30, 80, 220);
    const QRegion horizontalMovementDamage =
        planScreenshotGuideLineDamage(viewport, QPoint(100, 120), cursorColor, monitorColor,
                                      QPoint(104, 120), cursorColor, monitorColor);

    require(!horizontalMovementDamage.isEmpty(),
            "moving a visible cursor guide horizontally should repaint");
    require(horizontalMovementDamage.contains(QPoint(100, 20)) &&
                horizontalMovementDamage.contains(QPoint(104, 20)),
            "horizontal cursor movement must repaint the old and new vertical guides");
    require(!horizontalMovementDamage.contains(QPoint(20, 120)),
            "horizontal cursor movement must not repaint the unchanged horizontal guide");
    require(!horizontalMovementDamage.contains(QPoint(viewport.width() / 2, 20)) &&
                !horizontalMovementDamage.contains(QPoint(20, viewport.height() / 2)),
            "cursor movement must not invalidate unchanged monitor-center guide strips");
    require(regionArea(horizontalMovementDamage) <
                static_cast<qint64>(viewport.width()) * viewport.height() / 20,
            "horizontal cursor movement must repaint only narrow vertical strips");

    const QRegion verticalMovementDamage =
        planScreenshotGuideLineDamage(viewport, QPoint(104, 120), cursorColor, monitorColor,
                                      QPoint(104, 125), cursorColor, monitorColor);

    require(!verticalMovementDamage.isEmpty(),
            "moving a visible cursor guide vertically should repaint");
    require(verticalMovementDamage.contains(QPoint(20, 120)) &&
                verticalMovementDamage.contains(QPoint(20, 125)),
            "vertical cursor movement must repaint the old and new horizontal guides");
    require(!verticalMovementDamage.contains(QPoint(104, 20)),
            "vertical cursor movement must not repaint the unchanged vertical guide");
    require(regionArea(verticalMovementDamage) <
                static_cast<qint64>(viewport.width()) * viewport.height() / 20,
            "vertical cursor movement must repaint only narrow horizontal strips");
}

void hiddenAndSamePixelCursorMovementDoesNotRepaintGuideLines() {
    SnowCanvasWidget canvas;
    canvas.resize(640, 480);
    ScreenshotCanvasRenderer renderer(canvas);
    const QColor cursorColor(220, 30, 40);
    const QColor monitorColor(30, 80, 220);
    renderer.setGuideLines(QPointF(100.1, 120.1), Qt::transparent, monitorColor);
    resetGuideLineRenderDiagnosticsForCurrentThread();
    renderer.setGuideLines(QPointF(420.9, 310.9), Qt::transparent, monitorColor);
    const ScreenshotGuideLineRenderDiagnostics monitorOnlyDiagnostics =
        guideLineRenderDiagnosticsForCurrentThread();
    require(monitorOnlyDiagnostics.updateRequests == 0 &&
                monitorOnlyDiagnostics.requestedDamagePixels == 0,
            "cursor motion must not repaint a monitor-center-only guide");

    renderer.setGuideLines(QPointF(100.1, 120.1), cursorColor, monitorColor);
    resetGuideLineRenderDiagnosticsForCurrentThread();
    renderer.setGuideLines(QPointF(100.9, 120.9), cursorColor, monitorColor);
    const ScreenshotGuideLineRenderDiagnostics samePixelDiagnostics =
        guideLineRenderDiagnosticsForCurrentThread();
    require(samePixelDiagnostics.updateRequests == 0 &&
                samePixelDiagnostics.requestedDamagePixels == 0,
            "subpixel cursor motion within one painted pixel must not repaint guide lines");
}

void cursorGuideLineDamageCoversChangedPixelsAtFractionalDprs() {
    constexpr std::array<qreal, 4> devicePixelRatios{1.0, 1.25, 1.5, 1.75};
    const QColor cursorColor(220, 30, 40);
    const QColor monitorColor(30, 80, 220);

    for (const qreal devicePixelRatio : devicePixelRatios) {
        SnowCanvasWidget canvas;
        canvas.resize(240, 180);
        canvas.setClearBackgroundEnabled(false);
        require(canvas.setViewportCamera(0.0, 0.0, 1.0),
                "the fractional-DPR guide test should initialize the camera");

        ScreenshotCanvasRenderer renderer(canvas);
        canvas.setCustomRenderer(&renderer);
        renderer.setGuideLines(QPointF(40.1, 50.1), cursorColor, monitorColor);
        const QImage previous = renderCanvas(canvas, devicePixelRatio);

        renderer.setGuideLines(QPointF(43.8, 54.7), cursorColor, monitorColor);
        const QRegion dirty =
            planScreenshotGuideLineDamage(canvas.rect(), QPoint(40, 50), cursorColor, monitorColor,
                                          QPoint(43, 54), cursorColor, monitorColor);

        require(!dirty.isEmpty(), "fractional-DPR cursor guide movement should request damage");
        const QImage next = renderCanvas(canvas, devicePixelRatio);
        requireChangedPixelsCoveredByDirtyRegion(
            previous, next, dirty, "fractional-DPR guide damage must cover every changed pixel");
        canvas.setCustomRenderer(nullptr);
    }
}

void colorPickerCenterGuidesLeaveTheSampleUntouched() {
    QImage image(25, 25, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    const QRectF preview(0.0, 0.0, 25.0, 25.0);
    const QRect samplePixels(10, 10, 5, 5);
    const QColor guideColor(35, 190, 90);
    {
        QPainter painter(&image);
        paintScreenshotColorPickerCenterGuideLines(painter, preview, QRectF(samplePixels),
                                                   guideColor);
    }

    require(imageRectContainsColor(image, QRect(0, 0, 25, 10), guideColor) &&
                imageRectContainsColor(image, QRect(0, 15, 25, 10), guideColor) &&
                imageRectContainsColor(image, QRect(0, 0, 10, 25), guideColor) &&
                imageRectContainsColor(image, QRect(15, 0, 10, 25), guideColor),
            "the picker center guide must paint all four surrounding segments");
    for (int y = samplePixels.top(); y <= samplePixels.bottom(); ++y) {
        for (int x = samplePixels.left(); x <= samplePixels.right(); ++x) {
            require(image.pixelColor(x, y).alpha() == 0,
                    "picker center guides must not cover the sampled pixels");
        }
    }
}

void onlyTheInputOverlayOwnsGuideLines() {
    NoopOverlayEventSink eventSink;
    auto* firstCanvas = new SnowCanvasWidget;
    auto* secondCanvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow firstOverlay(eventSink, firstCanvas);
    ScreenshotOverlayWindow secondOverlay(eventSink, secondCanvas);

    CapturedDisplayModel firstDisplay;
    firstDisplay.active = true;
    CapturedDisplayModel secondDisplay;
    secondDisplay.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(firstDisplay, &firstOverlay);
    displays.appendDisplay(secondDisplay, &secondOverlay);

    ScreenshotOverlayCanvasPresenter presenter({});
    auto* firstRenderer = firstOverlay.screenshotRendererForTesting();
    auto* secondRenderer = secondOverlay.screenshotRendererForTesting();
    require(firstRenderer != nullptr && secondRenderer != nullptr,
            "the guide ownership test requires both overlay renderers");

    presenter.updateGuideLines(displays, &firstOverlay, QPointF(12.0, 14.0), true,
                               QColor(220, 30, 40), QColor(30, 80, 220));
    require(firstRenderer->guideLinesVisible() && !secondRenderer->guideLinesVisible(),
            "only the overlay receiving selection input may own guide lines");

    presenter.updateGuideLines(displays, &secondOverlay, QPointF(4.0, 6.0), true,
                               QColor(220, 30, 40), QColor(30, 80, 220));
    require(!firstRenderer->guideLinesVisible() && secondRenderer->guideLinesVisible(),
            "guide ownership must move with the input overlay");

    presenter.updateGuideLines(displays, &secondOverlay, QPointF(4.0, 6.0), false,
                               QColor(220, 30, 40), QColor(30, 80, 220));
    require(!firstRenderer->guideLinesVisible() && !secondRenderer->guideLinesVisible(),
            "guide lines must clear outside smart and manual selection");

    presenter.updateGuideLines(displays, &firstOverlay, QPointF(12.0, 14.0), true, Qt::transparent,
                               Qt::transparent);
    require(!firstRenderer->guideLinesVisible() && !secondRenderer->guideLinesVisible(),
            "transparent configured colors must keep every overlay guide-free");
}

void guideLinesInitializeFromGlobalCursorPosition() {
    NoopOverlayEventSink eventSink;
    auto* firstCanvas = new SnowCanvasWidget;
    auto* secondCanvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow firstOverlay(eventSink, firstCanvas);
    ScreenshotOverlayWindow secondOverlay(eventSink, secondCanvas);

    CapturedDisplayModel firstDisplay;
    firstDisplay.active = true;
    firstDisplay.logicalRect = QRect(100, 100, 80, 60);
    CapturedDisplayModel secondDisplay;
    secondDisplay.active = true;
    secondDisplay.logicalRect = QRect(180, 100, 80, 60);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(firstDisplay, &firstOverlay);
    displays.appendDisplay(secondDisplay, &secondOverlay);
    firstOverlay.setGeometry(firstDisplay.logicalRect);
    secondOverlay.setGeometry(secondDisplay.logicalRect);

    ScreenshotOverlayCanvasPresenter presenter({});
    auto* firstRenderer = firstOverlay.screenshotRendererForTesting();
    auto* secondRenderer = secondOverlay.screenshotRendererForTesting();
    require(firstRenderer != nullptr && secondRenderer != nullptr,
            "the initial guide test requires both overlay renderers");

    presenter.updateGuideLinesAtGlobalPosition(displays, QPoint(112, 114), true,
                                               QColor(220, 30, 40), QColor(30, 80, 220));
    require(firstRenderer->guideLinesVisible() && !secondRenderer->guideLinesVisible(),
            "initial guide synchronization should choose the overlay under the cursor");

    presenter.updateGuideLinesAtGlobalPosition(displays, QPoint(192, 124), true,
                                               QColor(220, 30, 40), QColor(30, 80, 220));
    require(!firstRenderer->guideLinesVisible() && secondRenderer->guideLinesVisible(),
            "initial guide synchronization should use the cursor's current display");

    presenter.updateGuideLinesAtGlobalPosition(displays, QPoint(20, 20), true, QColor(220, 30, 40),
                                               QColor(30, 80, 220));
    require(!firstRenderer->guideLinesVisible() && !secondRenderer->guideLinesVisible(),
            "initial guide synchronization should clear guides outside captured displays");
}

void screenshotImageMaskAndSelectionRenderInTheirOwnedPasses() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    require(renderer.renderMode() == ScreenshotCanvasRenderer::RenderMode::Standard,
            "standard rendering should be the default");

    QImage screenshot(80, 80, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-40.0, -40.0, 80.0, 80.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(20.0, 20.0, -40.0, -40.0), false);

    require(renderer.maskVisible(), "mask state should live in snow_shot");
    require(renderer.hasSelection(), "selection state should live in snow_shot");
    require(renderer.selection() == QRectF(-20.0, -20.0, 40.0, 40.0),
            "selection should be normalized");
    require(!renderer.selectionHandlesVisible(), "selection handle visibility should be retained");

    const QImage output = renderCanvas(canvas);
    const QColor outside = output.pixelColor(5, 5);
    const QColor inside = output.pixelColor(40, 40);
    require(outside.blue() < inside.blue() && outside.red() == 0,
            "mask should dim the screenshot outside the selection");
    require(inside == QColor(0, 80, 240),
            "selection hole should reveal the original screenshot image");
    require(output.pixelColor(20, 40).blue() > 150,
            "selection border should render over the screenshot and mask");

    renderer.setRenderMode(ScreenshotCanvasRenderer::RenderMode::ScrollingCapture);
    renderer.setRenderMode(ScreenshotCanvasRenderer::RenderMode::ScrollingCapture);
    require(renderer.renderMode() == ScreenshotCanvasRenderer::RenderMode::ScrollingCapture,
            "scrolling capture rendering should be selectable");
    const QImage scrollingOutput = renderCanvas(canvas);
    require(scrollingOutput.pixelColor(5, 5) == QColor(0, 0, 0, 128),
            "scrolling capture should retain only the dim mask outside the selection");
    require(scrollingOutput.pixelColor(40, 40).alpha() == 0,
            "scrolling capture should expose a transparent selection hole");
    require(scrollingOutput.pixelColor(20, 40).blue() == 0,
            "scrolling capture should hide the selection border");
    require(renderer.hasSelection(), "scrolling capture should retain selection state");

    renderer.setRenderMode(ScreenshotCanvasRenderer::RenderMode::Standard);
    require(renderCanvas(canvas) == output,
            "standard rendering should restore the retained screenshot presentation");

    renderer.setRenderMode(ScreenshotCanvasRenderer::RenderMode::ScrollingCapture);
    renderer.reset();
    require(renderer.renderMode() == ScreenshotCanvasRenderer::RenderMode::Standard,
            "reset should restore standard rendering");
    require(!renderer.maskVisible(), "reset should clear mask state");
    require(!renderer.hasSelection(), "reset should clear selection state");
    canvas.setCustomRenderer(nullptr);
}

void overlayWatermarkRendersOnlyInsideScreenshotSelection() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(120, 100);
    overlay.show();
    QApplication::processEvents();
    require(canvas->setViewportCamera(0.0, 0.0, 1.0),
            "the watermark area test should initialize the camera");

    SnowCanvasWatermarkConfig config;
    config.text = QStringLiteral("AREA");
    config.color = Qt::white;
    config.fontSize = 18.0;
    config.angle = 0.0;
    config.gap = 10.0;
    config.opacity = 1.0;
    require(canvas->setCanvasWatermarkConfig(config),
            "the watermark area test should configure a visible watermark");
    const QRectF selection(-30.0, -20.0, 60.0, 40.0);
    overlay.setScreenshotSelection(selection, false, 0);
    overlay.setScreenshotSelectionBorderVisible(false);
    require(canvas->hasWatermarkRenderArea() && canvas->watermarkRenderArea() == selection,
            "the screenshot overlay should bind the watermark area to its selection");

    const QImage selectedOutput = renderCanvas(*canvas);
    const QRect selectionView = canvas->viewRectForCanvasRect(selection);
    bool visibleInside = false;
    for (int y = 0; y < selectedOutput.height(); ++y) {
        for (int x = 0; x < selectedOutput.width(); ++x) {
            const bool visible = selectedOutput.pixelColor(x, y).alpha() > 0;
            if (selectionView.contains(x, y)) {
                visibleInside = visibleInside || visible;
            } else {
                require(!visible,
                        "the overlay watermark must not render outside the screenshot selection");
            }
        }
    }
    require(visibleInside,
            "the overlay watermark should remain visible inside the screenshot selection");

    overlay.clearScreenshotSelection();
    require(canvas->hasWatermarkRenderArea() && canvas->watermarkRenderArea().isEmpty(),
            "an overlay without a selection should retain an explicitly empty watermark area");
    const QImage clearedOutput = renderCanvas(*canvas);
    for (int y = 0; y < clearedOutput.height(); ++y) {
        for (int x = 0; x < clearedOutput.width(); ++x) {
            require(clearedOutput.pixelColor(x, y).alpha() == 0,
                    "an overlay without a selection must render no watermark");
        }
    }
}

void reusedRendererReplacesScreenshotImage() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QRectF canvasRect(-40.0, -40.0, 80.0, 80.0);

    QImage firstScreenshot(80, 80, QImage::Format_RGBA8888);
    firstScreenshot.fill(QColor(210, 30, 20));
    renderer.setImage(std::move(firstScreenshot), canvasRect);
    require(renderCanvas(canvas).pixelColor(40, 40) == QColor(210, 30, 20),
            "the first screenshot should be rendered");

    QImage secondScreenshot(80, 80, QImage::Format_RGBA8888);
    secondScreenshot.fill(QColor(20, 170, 70));
    renderer.setImage(std::move(secondScreenshot), canvasRect);
    require(renderCanvas(canvas).pixelColor(40, 40) == QColor(20, 170, 70),
            "reusing an overlay must replace the prior screenshot");
    canvas.setCustomRenderer(nullptr);
}

void bgraScreenshotImagesRenderWithCorrectColors() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    QImage source(2, 1, QImage::Format_ARGB32);
    auto* pixels = source.bits();
    pixels[0] = 0;
    pixels[1] = 0;
    pixels[2] = 255;
    pixels[3] = 255;
    pixels[4] = 255;
    pixels[5] = 0;
    pixels[6] = 0;
    pixels[7] = 255;

    const QImage rendered =
        renderMaterializedImage(source, source.size(), QRegion(source.rect()), Qt::transparent);
    require(rendered.pixelColor(0, 0) == QColor(255, 0, 0, 255) &&
                rendered.pixelColor(1, 0) == QColor(0, 0, 255, 255),
            "BGRA screenshot pixels should render with their original colors");
#endif
}

void hoveredSelectionToolbarHidesBorderAndRendersShadowPreview() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QColor screenshotColor(0, 80, 240);
    QImage screenshot(80, 80, QImage::Format_RGBA8888);
    screenshot.fill(screenshotColor);
    renderer.setImage(std::move(screenshot), QRectF(-40.0, -40.0, 80.0, 80.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(-20.0, -20.0, 40.0, 40.0), true, 10, 4, QColor(0x59, 0x59, 0x59));

    const QImage bordered = renderCanvas(canvas);
    require(bordered.pixelColor(20, 40) != screenshotColor,
            "a normal selection should render its border");

    renderer.setSelectionToolbarHovered(true);
    require(renderer.selectionToolbarHovered(),
            "toolbar hover state should be retained by the renderer");
    require(renderer.selectionShadowWidth() == 4,
            "selection shadow width should be retained by the renderer");

    const QImage preview = renderCanvas(canvas);
    const QColor checkerLight(QStringLiteral("#ffffff"));
    const QColor checkerDark(QStringLiteral("#f0f0f0"));
    require(preview.pixelColor(20, 40) == screenshotColor,
            "hovering the selection toolbar should hide the selection border");
    require(preview.pixelColor(17, 17) == checkerLight || preview.pixelColor(17, 17) == checkerDark,
            "the expanded shadow area should match the color picker checkerboard");

    const QColor shadow = preview.pixelColor(18, 23);
    require(shadow != checkerLight && shadow != checkerDark,
            "the shadow should composite over the transparency checkerboard");
    require(preview.pixelColor(12, 40).blue() < shadow.blue(),
            "pixels beyond the expanded mask should remain dimmed");

    renderer.setSelectionToolbarHovered(false);
    require(!renderer.selectionToolbarHovered(),
            "leaving the selection toolbar should clear the hover state");
    require(renderCanvas(canvas).pixelColor(20, 40) != screenshotColor,
            "leaving the selection toolbar should restore the selection border");
    canvas.setCustomRenderer(nullptr);
}

void roundedSelectionPreviewKeepsTheSameContentBoundsWithAndWithoutShadow() {
    SnowCanvasWidget canvas;
    canvas.resize(120, 120);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QColor screenshotColor(0, 80, 240);
    QImage screenshot(120, 120, QImage::Format_RGBA8888);
    screenshot.fill(screenshotColor);
    renderer.setImage(std::move(screenshot), QRectF(-60.0, -60.0, 120.0, 120.0));
    renderer.setMaskVisible(true);
    const QRectF selection(-30.0, -25.0, 60.0, 50.0);
    renderer.setSelection(selection, false, 18, 0);
    renderer.setSelectionToolbarHovered(true);

    const QImage withoutShadow = renderCanvas(canvas);
    require(withoutShadow.pixelColor(30, 35).blue() < screenshotColor.blue(),
            "a hovered rounded selection must retain its corner mask when shadow is disabled");
    require(withoutShadow.pixelColor(38, 43) == screenshotColor,
            "the zero-shadow rounded preview should preserve content inside the corner curve");

    renderer.setSelection(selection, false, 18, 10);
    const QImage withShadow = renderCanvas(canvas);
    constexpr std::array<QPoint, 5> stableContentPoints = {
        QPoint(32, 60), QPoint(88, 60), QPoint(60, 37), QPoint(60, 83), QPoint(38, 43)};
    for (const QPoint& point : stableContentPoints) {
        require(withoutShadow.pixelColor(point) == screenshotColor &&
                    withShadow.pixelColor(point) == screenshotColor,
                "enabling shadow must not inset or shrink the rounded selection content");
    }
    canvas.setCustomRenderer(nullptr);
}

void squareSelectionPreviewKeepsTheSameContentBoundsWithAndWithoutShadow() {
    SnowCanvasWidget canvas;
    canvas.resize(120, 120);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QColor screenshotColor(0, 80, 240);
    QImage screenshot(120, 120, QImage::Format_RGBA8888);
    screenshot.fill(screenshotColor);
    renderer.setImage(std::move(screenshot), QRectF(-60.0, -60.0, 120.0, 120.0));
    renderer.setMaskVisible(true);
    const QRectF selection(-30.0, -25.0, 60.0, 50.0);
    renderer.setSelection(selection, false, 0, 0);
    renderer.setSelectionToolbarHovered(true);

    const QImage withoutShadow = renderCanvas(canvas);
    renderer.setSelection(selection, false, 0, 10);
    const QImage withShadow = renderCanvas(canvas);
    constexpr std::array<QPoint, 5> stableContentPoints = {
        QPoint(32, 60), QPoint(87, 60), QPoint(60, 37), QPoint(60, 82), QPoint(60, 60)};
    for (const QPoint& point : stableContentPoints) {
        require(withoutShadow.pixelColor(point) == screenshotColor &&
                    withShadow.pixelColor(point) == screenshotColor,
                "enabling shadow at zero corner radius must not cover or inset the selection");
    }
    canvas.setCustomRenderer(nullptr);
}

void hoveredSelectionToolbarInvalidatesOnlyPreviewRing() {
    SnowCanvasWidget canvas;
    canvas.resize(500, 400);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 2.0 / 3.0), "scaled camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    QImage screenshot(750, 600, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-375.0, -300.0, 750.0, 600.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(-225.0, -150.0, 450.0, 300.0), true, 64, 16,
                          QColor(0x59, 0x59, 0x59));

    canvas.show();
    QApplication::processEvents();
    constexpr qreal devicePixelRatio = 1.5;
    const QImage bordered = renderCanvas(canvas, devicePixelRatio);

    CanvasPaintRegionObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    renderer.setSelectionToolbarHovered(true);
    QApplication::processEvents();
    const QRegion dirty = observer.region();
    canvas.removeEventFilter(&observer);

    require(!dirty.isEmpty(), "hovering the toolbar should schedule a repaint");
    require(!dirty.contains(QPoint(250, 200)),
            "the stable selection interior should not be repainted for the shadow preview");

    qint64 dirtyArea = 0;
    for (const QRect& rect : dirty) {
        dirtyArea += static_cast<qint64>(rect.width()) * rect.height();
    }
    require(dirtyArea < 300LL * 200LL,
            "the shadow preview should repaint less than the full selection interior");

    const QImage preview = renderCanvas(canvas, devicePixelRatio);
    requireChangedPixelsCoveredByDirtyRegion(
        bordered, preview, dirty, "the preview dirty ring should cover every changed pixel");
    canvas.setCustomRenderer(nullptr);
}

void hiddenSelectionBorderRetainsSelectionAndMask() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    const QColor screenshotColor(0, 80, 240);
    QImage screenshot(80, 80, QImage::Format_RGBA8888);
    screenshot.fill(screenshotColor);
    renderer.setImage(std::move(screenshot), QRectF(-40.0, -40.0, 80.0, 80.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(-20.0, -20.0, 40.0, 40.0), false);

    renderer.setSelectionBorderVisible(false);
    require(!renderer.selectionBorderVisible(), "selection border visibility should be retained");
    const QImage borderless = renderCanvas(canvas);
    require(borderless.pixelColor(20, 40) == screenshotColor,
            "hidden selection border should reveal the unmodified screenshot");
    require(borderless.pixelColor(5, 5).blue() < screenshotColor.blue(),
            "hiding the selection border should retain the dim mask");
    require(renderer.hasSelection(), "hiding the selection border should retain the selection");

    renderer.setSelectionBorderVisible(true);
    require(renderCanvas(canvas).pixelColor(20, 40) != screenshotColor,
            "restoring selection border visibility should redraw the border");
    canvas.setCustomRenderer(nullptr);
}

void changingSelectionCornerRadiusRepaintsRoundedMaskAndBorder() {
    SnowCanvasWidget canvas;
    canvas.resize(100, 100);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    QImage screenshot(100, 100, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-50.0, -50.0, 100.0, 100.0));
    renderer.setMaskVisible(true);
    const QRectF selection(-30.0, -30.0, 60.0, 60.0);
    renderer.setSelection(selection, false);

    canvas.show();
    QApplication::processEvents();
    const QImage squareOutput = renderCanvas(canvas);

    CanvasPaintRegionObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    renderer.setSelection(selection, false, 18);
    QApplication::processEvents();
    const QRegion dirty = observer.region();
    canvas.removeEventFilter(&observer);

    require(renderer.selectionCornerRadius() == 18,
            "selection renderer should retain the current corner radius");
    require(dirty.contains(QPoint(20, 20)),
            "changing the corner radius should repaint the old square corner");
    require(!dirty.contains(QPoint(50, 50)),
            "changing the corner radius should not repaint the stable selection center");

    const QImage roundedOutput = renderCanvas(canvas);
    requireChangedPixelsCoveredByDirtyRegion(
        squareOutput, roundedOutput, dirty,
        "corner-radius dirty region should cover every changed pixel");
    require(roundedOutput.pixelColor(20, 20).blue() < squareOutput.pixelColor(20, 20).blue(),
            "rounded selection corners should be covered by the outside mask");
    require(roundedOutput.pixelColor(25, 25).blue() > roundedOutput.pixelColor(20, 20).blue() + 50,
            "selection border should follow the configured rounded corner");
    require(roundedOutput.pixelColor(50, 50) == QColor(0, 80, 240),
            "rounded selection should preserve pixels away from its corners");
    canvas.setCustomRenderer(nullptr);
}

void ocrPresentationSelectionBorderIgnoresRoundedCorners() {
    SnowCanvasWidget canvas;
    canvas.resize(100, 100);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    QImage screenshot(100, 100, QImage::Format_RGBA8888);
    screenshot.fill(QColor(180, 40, 40));
    renderer.setImage(std::move(screenshot), QRectF(-50.0, -50.0, 100.0, 100.0));
    const QRectF selection(-30.0, -30.0, 60.0, 60.0);
    renderer.setSelection(selection, false);

    canvas.show();
    QApplication::processEvents();
    const QImage squareBorder = renderCanvas(canvas);

    renderer.setSelection(selection, false, 18);
    const QImage roundedBorder = renderCanvas(canvas);
    require(roundedBorder != squareBorder,
            "a configured corner radius should normally round the selection border");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = selection.toAlignedRect();
    renderer.setOcrPresentation(presentation);
    require(renderer.selectionCornerRadius() == 18,
            "displaying OCR should preserve the configured selection corner radius");
    require(renderCanvas(canvas) == squareBorder,
            "the selection border should be square while OCR results are displayed");

    renderer.clearOcrPresentation();
    require(renderCanvas(canvas) == roundedBorder,
            "clearing OCR should restore the configured rounded selection border");
    canvas.setCustomRenderer(nullptr);
}

void movingSelectionInvalidatesOnlyChangedMaskAndDecorations() {
    SnowCanvasWidget canvas;
    canvas.resize(500, 400);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    QImage screenshot(500, 400, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-250.0, -200.0, 500.0, 400.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(-150.0, -100.0, 300.0, 200.0), true);

    canvas.show();
    QApplication::processEvents();

    CanvasPaintRegionObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    renderer.setSelection(QRectF(-149.0, -100.0, 300.0, 200.0), true);
    QApplication::processEvents();

    const QRegion dirty = observer.region();
    require(!dirty.isEmpty(), "moving a selection should schedule a repaint");
    require(dirty.contains(QPoint(100, 200)), "the old selection edge should be repainted");
    require(dirty.contains(QPoint(400, 200)), "the new selection edge should be repainted");
    require(!dirty.contains(QPoint(250, 200)),
            "the unchanged selection interior should not be repainted");

    qint64 dirtyArea = 0;
    for (const QRect& rect : dirty) {
        dirtyArea += static_cast<qint64>(rect.width()) * rect.height();
    }
    require(dirtyArea < 300LL * 200LL / 2LL,
            "a one-pixel move should repaint less than half the selection area");
    canvas.removeEventFilter(&observer);
    canvas.setCustomRenderer(nullptr);
}

void overlaySelectionMoveDoesNotExpandForInactiveDecorations() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(500, 400);
    overlay.show();
    require(canvas->setViewportCamera(0.0, 0.0, 1.0),
            "the overlay damage test should initialize the camera");
    overlay.setScreenshotMaskVisible(true);
    const QRectF initialSelection(-150.0, -100.0, 300.0, 200.0);
    overlay.setScreenshotSelection(initialSelection, true, 12);
    QApplication::processEvents();

    CanvasPaintRegionObserver observer;
    canvas->installEventFilter(&observer);
    observer.begin();
    overlay.setScreenshotSelection(QRectF(-149.0, -100.0, 300.0, 200.0), true, 12);
    QApplication::processEvents();
    const QRegion dirty = observer.region();
    canvas->removeEventFilter(&observer);

    require(!dirty.isEmpty(), "an overlay selection move should repaint");
    require(dirty.contains(QPoint(100, 200)) && dirty.contains(QPoint(400, 200)),
            "an overlay selection move should repaint both changed selection edges");
    require(!dirty.contains(QPoint(250, 200)),
            "a rounded selection move should not repaint the stable selection interior");

    qint64 dirtyArea = 0;
    for (const QRect& rect : dirty) {
        dirtyArea += static_cast<qint64>(rect.width()) * rect.height();
    }
    require(dirtyArea < 300LL * 200LL / 2LL,
            "inactive overlay decorations must keep rounded-selection damage sparse");
}

void selectionDamagePlannerAvoidsFullCanvasFallback() {
    const QRect viewport(0, 0, 640, 480);
    const QTransform transform;

    ScreenshotSelectionVisualState offscreenBefore;
    offscreenBefore.bounds = QRectF(-2000.0, -1600.0, 120.0, 80.0);
    offscreenBefore.present = true;
    ScreenshotSelectionVisualState offscreenAfter = offscreenBefore;
    offscreenAfter.bounds.moveLeft(-1999.0);
    require(
        planScreenshotSelectionDamage(offscreenBefore, offscreenAfter, viewport, transform, true)
            .isEmpty(),
        "off-screen selection changes must not fall back to a full-canvas repaint");

    const QTransform unavailable(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    require(planScreenshotSelectionDamage(offscreenBefore, offscreenAfter, viewport, unavailable,
                                          true) == QRegion(viewport),
            "an unavailable canvas transform is the only full-viewport fallback");

    ScreenshotSelectionVisualState before;
    before.bounds = QRectF(120.2, 90.2, 240.4, 180.4);
    before.present = true;
    before.handlesVisible = false;
    before.cornerRadius = 18;
    ScreenshotSelectionVisualState after = before;
    after.bounds.moveTopLeft(QPointF(120.7, 90.7));
    const QRegion dirty = planScreenshotSelectionDamage(before, after, viewport, transform, true);
    require(!dirty.isEmpty(), "a subpixel selection move should produce damage");
    require(!dirty.contains(QPoint(240, 180)),
            "a subpixel selection move must preserve the stable selection interior");

    qint64 dirtyArea = 0;
    for (const QRect& rect : dirty) {
        dirtyArea += static_cast<qint64>(rect.width()) * rect.height();
    }
    require(dirtyArea < static_cast<qint64>(viewport.width()) * viewport.height() / 2,
            "subpixel selection damage must remain bounded by the changed perimeter");

    ScreenshotSelectionVisualState square = before;
    square.bounds = QRectF(100.0, 100.0, 200.0, 200.0);
    square.cornerRadius = 0;
    ScreenshotSelectionVisualState rounded = square;
    rounded.cornerRadius = 64;
    const QRegion roundedDamage =
        planScreenshotSelectionDamage(square, rounded, viewport, transform, true);
    require(roundedDamage.contains(QPoint(130, 130)) && !roundedDamage.contains(QPoint(200, 200)),
            "rounded-corner transitions must cover changed corner pixels without repainting the "
            "center");
}

void activeWatermarkAreaMovementUsesUnionDamage() {
    SnowCanvasWidget canvas;
    canvas.resize(320, 240);
    canvas.show();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    SnowCanvasWatermarkConfig config;
    config.text = QStringLiteral("VISIBLE");
    config.color = Qt::white;
    config.fontSize = 18.0;
    config.opacity = 1.0;
    require(canvas.setCanvasWatermarkConfig(config),
            "the watermark union test should configure a visible watermark");
    const QRectF first(-120.0, -80.0, 160.0, 120.0);
    const QRectF second(-40.0, -20.0, 160.0, 120.0);
    canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::optional<QRectF>(first),
        std::nullopt,
    });
    QApplication::processEvents();

    CanvasPaintRegionObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::optional<QRectF>(second),
        std::nullopt,
    });
    QApplication::processEvents();
    const QRegion dirty = observer.region();
    canvas.removeEventFilter(&observer);

    const QPoint overlapView = canvas.viewRectForCanvasRect(first.intersected(second)).center();
    require(!dirty.isEmpty(), "moving an active watermark area should repaint");
    require(dirty.contains(overlapView),
            "an active watermark area move must invalidate the old/new union");
}

void activeSpotlightAreaMovementUsesSymmetricDifferenceDamage() {
    SnowCanvasRuntime runtime;
    require(runtime.isValid(), "the spotlight damage test runtime should initialize");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");
    require(canvas.setCanvasTool(SnowCanvasTool::Spotlight),
            "the spotlight damage test should activate the spotlight tool");

    const QPointF start(80.0, 60.0);
    const QPointF end(240.0, 180.0);
    QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &release);
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Select),
            "the spotlight damage test should return to the selection tool");

    const QRectF first(-120.0, -80.0, 160.0, 120.0);
    const QRectF second(-40.0, -20.0, 160.0, 120.0);
    canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::nullopt,
        std::optional<QRectF>(first),
    });
    QApplication::processEvents();

    CanvasPaintRegionObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    canvas.setDecorationRenderAreas(SnowCanvasDecorationRenderAreas{
        std::nullopt,
        std::optional<QRectF>(second),
    });
    QApplication::processEvents();
    const QRegion dirty = observer.region();
    canvas.removeEventFilter(&observer);

    const QPoint overlapView = canvas.viewRectForCanvasRect(first.intersected(second)).center();
    require(!dirty.isEmpty(), "moving an active spotlight area should repaint");
    require(!dirty.contains(overlapView),
            "an active spotlight area move must invalidate only the symmetric difference");
}

void selectionTransitionsCoverChangedPixelsAtFractionalDprs() {
    constexpr std::array<qreal, 4> devicePixelRatios = {1.0, 1.25, 1.5, 2.0};
    for (const qreal devicePixelRatio : devicePixelRatios) {
        SnowCanvasWidget canvas;
        canvas.resize(240, 180);
        canvas.setClearBackgroundEnabled(false);
        require(canvas.setViewportCamera(0.0, 0.0, 1.0),
                "the fractional-DPR selection test should initialize the camera");

        ScreenshotCanvasRenderer renderer(canvas);
        canvas.setCustomRenderer(&renderer);
        QImage screenshot(240, 180, QImage::Format_RGBA8888);
        screenshot.fill(QColor(0, 80, 240));
        renderer.setImage(std::move(screenshot), QRectF(-120.0, -90.0, 240.0, 180.0));
        renderer.setMaskVisible(true);
        renderer.setSelection(QRectF(-80.25, -50.25, 160.5, 100.5), true, 18, 16,
                              QColor(0x59, 0x59, 0x59));
        canvas.show();
        QApplication::processEvents();

        QImage previous = renderCanvas(canvas, devicePixelRatio);
        const QList<QRectF> transitions = {
            QRectF(-79.75, -49.75, 160.5, 100.5),
            QRectF(-79.75, -49.75, 161.25, 101.25),
        };
        for (const QRectF& selection : transitions) {
            CanvasPaintRegionObserver observer;
            canvas.installEventFilter(&observer);
            observer.begin();
            renderer.setSelection(selection, true, 18, 16, QColor(0x59, 0x59, 0x59));
            QApplication::processEvents();
            const QRegion dirty = observer.region();
            canvas.removeEventFilter(&observer);

            const QImage next = renderCanvas(canvas, devicePixelRatio);
            requireChangedPixelsCoveredByDirtyRegion(
                previous, next, dirty,
                "fractional-DPR selection damage must cover every changed pixel");
            previous = next;
        }

        CanvasPaintRegionObserver observer;
        canvas.installEventFilter(&observer);
        observer.begin();
        renderer.setSelectionToolbarHovered(true);
        QApplication::processEvents();
        const QRegion dirty = observer.region();
        canvas.removeEventFilter(&observer);
        const QImage preview = renderCanvas(canvas, devicePixelRatio);
        requireChangedPixelsCoveredByDirtyRegion(
            previous, preview, dirty,
            "fractional-DPR shadow changes must cover every changed pixel");
        canvas.setCustomRenderer(nullptr);
    }
}

void sharedShadowPreviewMatchesExportAndCacheStaysBounded() {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const QColor shadowColor(0x59, 0x59, 0x59, 220);

    const auto renderPreview = [](int radius, int width, const QColor& color) {
        const QSize size(96 + width * 2, 72 + width * 2);
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ScreenshotSelectionShadowRenderer::renderPreview(painter, QRectF(width, width, 96, 72),
                                                         radius, width, color, 1.0);
        painter.end();
        return image;
    };

    for (const int radius : {0, 18}) {
        for (const int width : {1, 16, 64}) {
            QImage content(96, 72, QImage::Format_ARGB32_Premultiplied);
            content.fill(QColor(24, 88, 160));
            const QImage exported = ScreenshotSelectionShadowRenderer::composeExport(
                content, radius, width, shadowColor);
            const QImage preview = renderPreview(radius, width, QColor(0x59, 0x59, 0x59, 220));
            const QImage checkerboard = renderPreview(radius, width, QColor(0x59, 0x59, 0x59, 0));
            const QPoint sample(std::max(0, width - 1), width + content.height() / 2);
            const QColor expected = sourceOverOpaqueBackground(exported.pixelColor(sample),
                                                               checkerboard.pixelColor(sample));
            const QColor actual = preview.pixelColor(sample);
            require(std::abs(actual.red() - expected.red()) <= 2 &&
                        std::abs(actual.green() - expected.green()) <= 2 &&
                        std::abs(actual.blue() - expected.blue()) <= 2,
                    "preview and export must use the same shadow falloff");
            require(exported.pixelColor(sample).alpha() > 0,
                    "each configured shadow width must produce an export shadow");
        }
    }

    const auto first = renderPreview(18, 16, QColor(0x59, 0x59, 0x59, 220));
    Q_UNUSED(first);
    const auto cold = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(cold.cacheBuilds >= 1, "the first shadow preview must build an asset");
    require(cold.retainedEntries <= 8 && cold.retainedBytes <= 16u * 1024u * 1024u,
            "the shadow cache must stay within its entry and byte limits");

    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto warmed = renderPreview(18, 16, QColor(0x59, 0x59, 0x59, 220));
    Q_UNUSED(warmed);
    const auto warm = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(
        warm.cacheHits >= 1 && warm.cacheBuilds == 0 &&
            warm.selectionSizedTransientAllocations == 0,
        "a warmed shadow preview must hit the compact cache without selection-sized allocation");

    for (int style = 0; style < 32; ++style) {
        const auto image =
            renderPreview(style % 32, 1 + (style % 64), QColor(0x59, 0x59, 0x59, 220));
        Q_UNUSED(image);
    }
    const auto bounded = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(bounded.retainedEntries <= 8 && bounded.retainedBytes <= 16u * 1024u * 1024u,
            "repeated shadow style changes must keep retained memory bounded");

    // This is outside the persisted settings range. It must be rendered as a
    // transient style asset without allowing one entry to exceed the cache
    // byte budget.
    const auto oversized = renderPreview(0, 1025, QColor(0x59, 0x59, 0x59, 220));
    Q_UNUSED(oversized);
    const auto afterOversized = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(afterOversized.retainedBytes <= 16u * 1024u * 1024u,
            "an oversized shadow asset must not exceed the retained byte cap");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
}

void unchangedOverlaySelectionDoesNotScheduleRepaint() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(1920, 1080);
    overlay.show();
    require(canvas->setViewportCamera(0.0, 0.0, 1.0),
            "the stable selection test should initialize the camera");

    const QRectF selection(-800.0, -450.0, 1600.0, 900.0);
    overlay.setScreenshotSelection(selection, false, 0);
    QApplication::processEvents();

    CanvasPaintRegionObserver observer;
    canvas->installEventFilter(&observer);
    observer.begin();
    overlay.setScreenshotSelection(selection, false, 0);
    QApplication::processEvents();
    canvas->removeEventFilter(&observer);

    require(observer.region().isEmpty(),
            "an unchanged overlay selection must not schedule a repaint");
}

void selectionTransitionDirtyRegionCoversEveryChangedPixel() {
    SnowCanvasWidget canvas;
    canvas.resize(500, 400);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 2.0 / 3.0), "scaled camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    QImage screenshot(750, 600, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-375.0, -300.0, 750.0, 600.0));
    renderer.setMaskVisible(true);
    renderer.setSelection(QRectF(-225.0, -150.0, 450.0, 300.0), true);

    canvas.show();
    QApplication::processEvents();

    constexpr qreal devicePixelRatio = 1.5;
    const QList<QRectF> selections = {
        QRectF(-224.0, -150.0, 450.0, 300.0),
        QRectF(-224.0, -150.0, 451.0, 300.0),
        QRectF(-223.0, -149.0, 450.0, 299.0),
        QRectF(-225.0, -151.0, 452.0, 302.0),
    };

    QImage previous = renderCanvas(canvas, devicePixelRatio);
    for (const QRectF& selection : selections) {
        CanvasPaintRegionObserver observer;
        canvas.installEventFilter(&observer);
        observer.begin();
        renderer.setSelection(selection, true);
        QApplication::processEvents();
        const QRegion dirty = observer.region();
        canvas.removeEventFilter(&observer);

        const QImage next = renderCanvas(canvas, devicePixelRatio);
        requireChangedPixelsCoveredByDirtyRegion(
            previous, next, dirty,
            "selection transition dirty region should cover every changed pixel");
        previous = next;
    }
    canvas.setCustomRenderer(nullptr);
}

void ocrPresentationRendersWhileCanvasContentIsHidden() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 80);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();
    QImage screenshot(80, 80, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-40.0, -40.0, 80.0, 80.0));

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(-20, -10, 40, 20);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Wj");
    line.quad = QPolygonF({
        QPointF(-16.0, -8.0),
        QPointF(16.0, -8.0),
        QPointF(16.0, 8.0),
        QPointF(-16.0, 8.0),
    });
    presentation->lines.push_back(line);
    renderer.setOcrPresentation(presentation,
                                ScreenshotCanvasRenderer::OcrPresentationMode::BackgroundOnly);
    require(canvas.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer")) == nullptr,
            "background-only OCR should not create a text layer on the screenshot canvas");
    renderer.clearOcrPresentation();
    renderer.setOcrPresentation(presentation);
    require(ocrTextItemCount(canvas) == 1,
            "each OCR line should use one layout-backed graphics item");
    canvas.setCanvasContentVisible(false);

    const QImage output = renderCanvas(canvas);
    const quint64 initialGeometrySynchronizationCount =
        renderer.ocrGeometrySynchronizationCountForTesting();
    require(initialGeometrySynchronizationCount == 1,
            "the initial OCR frame should synchronize each text item once");
    require(renderCanvas(canvas) == output &&
                renderer.ocrGeometrySynchronizationCountForTesting() ==
                    initialGeometrySynchronizationCount,
            "a stable OCR frame should reuse text geometry without resynchronizing it");
    require(output.pixelColor(21, 31) == QColor(0, 80, 240),
            "OCR fill must not alter selection pixels outside recognized lines");
    require(output.pixelColor(10, 10) == QColor(0, 80, 240),
            "OCR fill should preserve screenshot pixels outside the selected region");
    bool foundRegionFilter = false;
    for (int y = 32; y < 49 && !foundRegionFilter; ++y) {
        for (int x = 31; x < 58; ++x) {
            const QColor color = output.pixelColor(x, y);
            if (color.red() > 20 && color.green() > 100 && color.blue() > 200) {
                foundRegionFilter = true;
                break;
            }
        }
    }
    require(foundRegionFilter,
            "OCR regions should be blurred and blended toward the theme background inside "
            "recognized quads");
    const QRect paintedTextBounds = paintedInkBounds(output, QRect(24, 32, 32, 16), 80);
    require(!paintedTextBounds.isEmpty(),
            "OCR graphics items should render text over the region fill");
    require(paintedTextBounds.width() >= 30 && paintedTextBounds.height() >= 10,
            "OCR character spacing should expand short text across the recognized line");
    require(std::abs(paintedTextBounds.center().x() - 39.5) <= 1.0,
            "character-spaced OCR text should remain horizontally centered");
    auto* textLayer = canvas.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible(),
            "OCR text widgets should live in a visible layer above the canvas");
    require(!textLayer->scene()->items().isEmpty() &&
                textLayer->scene()->items().constFirst()->sceneBoundingRect().width() >= 31.0,
            "the OCR text layout should use the recognized region's available width");

    const ScreenshotOcrTextPosition lineStart = renderer.ocrTextPositionAt(QPointF(-15.5, 0.0));
    const ScreenshotOcrTextPosition lineMiddle = renderer.ocrTextPositionAt(QPointF(0.0, 0.0));
    require(lineStart.lineIndex == 0 && lineStart.characterIndex == 0 &&
                lineMiddle.lineIndex == 0 && lineMiddle.characterIndex > 0 &&
                lineMiddle.characterIndex < line.text.size(),
            "OCR hit testing should resolve character positions from the rendered text layout");
    presentation->beginTextSelection(lineMiddle);
    require(!presentation->hasTextSelection(),
            "pressing OCR text should not create a whole-line selection");
    const QImage pressedOutput = renderCanvas(canvas);
    require(pressedOutput == output,
            "pressing OCR text should not horizontally shift its rendered content");
    presentation->finishTextSelection();

    presentation->beginTextSelection(lineStart);
    presentation->updateTextSelection(lineMiddle);
    presentation->finishTextSelection();
    require(!presentation->selectedText().isEmpty() && presentation->selectedText() != line.text,
            "a drag over part of a rendered OCR line should not select the entire line");
    renderer.updateOcrSelection();
    require(renderer.ocrGeometrySynchronizationCountForTesting() ==
                initialGeometrySynchronizationCount,
            "an OCR selection update should not resynchronize text geometry");
    const QImage partialSelectionOutput = renderCanvas(canvas);
    bool foundPartialSelection = false;
    for (int y = 32; y < 49 && !foundPartialSelection; ++y) {
        for (int x = 24; x < 42; ++x) {
            const QColor color = partialSelectionOutput.pixelColor(x, y);
            if (color != output.pixelColor(x, y) && color.blue() > color.red() + 30) {
                foundPartialSelection = true;
                break;
            }
        }
    }
    require(foundPartialSelection,
            "the OCR graphics item should paint the model's partial character range");

    presentation->selectAll();
    const QImage selectedOutput = renderCanvas(canvas);
    bool foundWidgetSelection = false;
    for (int y = 32; y < 49 && !foundWidgetSelection; ++y) {
        for (int x = 24; x < 57; ++x) {
            const QColor color = selectedOutput.pixelColor(x, y);
            if (color != output.pixelColor(x, y) && color.blue() > color.red() + 40 &&
                color.blue() > color.green() + 20) {
                foundWidgetSelection = true;
                break;
            }
        }
    }
    require(foundWidgetSelection,
            "OCR selection highlighting should be painted by the text widgets");

    renderer.clearOcrPresentation();
    require(ocrTextItemCount(canvas) == 0 && textLayer != nullptr && textLayer->isHidden(),
            "clearing OCR should destroy its graphics text items");
    renderer.setOcrPresentation(presentation,
                                ScreenshotCanvasRenderer::OcrPresentationMode::BackgroundOnly);
    const QImage backgroundOnlyOutput = renderCanvas(canvas);
    require(ocrTextItemCount(canvas) == 0 && textLayer->isHidden(),
            "background-only OCR should not create or show text widgets");
    const QColor backgroundOnlyPixel = backgroundOnlyOutput.pixelColor(40, 40);
    require(backgroundOnlyPixel.red() > 20 && backgroundOnlyPixel.green() > 100 &&
                backgroundOnlyPixel.blue() > 200,
            "background-only OCR should still apply the region filter without text widgets");
    renderer.clearOcrPresentation();
    canvas.setCanvasContentVisible(true);
    require(renderCanvas(canvas).pixelColor(40, 40) == QColor(0, 80, 240),
            "clearing OCR should restore the immutable screenshot presentation");
    canvas.setCustomRenderer(nullptr);
}

void ocrFilteredImageBlendsTowardTheSuppliedThemeBackground() {
    QImage source(20, 20, QImage::Format_RGBA8888);
    const QColor screenshotColor(0, 80, 240);
    const QColor themeBackground(20, 30, 40);
    source.fill(screenshotColor);

    ScreenshotOcrPresentation presentation;
    presentation.selection = QRect(0, 0, 20, 20);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("OCR");
    line.quad = QPolygonF({QPointF(5.0, 5.0), QPointF(15.0, 5.0), QPointF(15.0, 15.0),
                           QPointF(5.0, 15.0)});
    presentation.lines.push_back(line);

    const QImage filtered = renderScreenshotOcrFilteredImage(
        source, QRectF(0.0, 0.0, 20.0, 20.0), presentation, themeBackground);
    require(!filtered.isNull(), "OCR filtering should produce an image for a valid source");
    const QColor center = filtered.pixelColor(10, 10);
    require(center.red() < 30 && center.green() < 60 && center.blue() < 160,
            "OCR filtering should blend toward the supplied theme background color");
    require(center != QColor(127, 167, 247),
            "OCR filtering should not use the former white blend destination");
}

void ocrPresentationRendersTextInPinnedResultMode() {
    SnowCanvasWidget canvas;
    canvas.resize(100, 60);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();

    const QRectF contentRect(-50.0, -30.0, 100.0, 60.0);
    QImage screenshot(100, 60, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), contentRect);
    renderer.setPinnedResultSurface(contentRect, contentRect, {});

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = contentRect.toAlignedRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Pinned OCR");
    line.quad = QPolygonF({
        QPointF(-40.0, -10.0),
        QPointF(40.0, -10.0),
        QPointF(40.0, 10.0),
        QPointF(-40.0, 10.0),
    });
    presentation->lines.push_back(line);
    renderer.setOcrPresentation(presentation);
    canvas.setCanvasContentVisible(false);

    const QImage output = renderCanvas(canvas);
    const QRect paintedTextBounds = paintedInkBounds(output, QRect(10, 20, 80, 20), 80);
    require(!paintedTextBounds.isEmpty(),
            "pinned-result OCR should paint recognized text over its filled background");
    auto* textLayer = canvas.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible(),
            "pinned-result mode should keep the OCR text layer visible");

    renderer.clearOcrPresentation();
    canvas.setCanvasContentVisible(true);
    canvas.setCustomRenderer(nullptr);
}

void ocrTextAspectFitUsesWidthConstraintWithoutVerticalStretch() {
    SnowCanvasWidget canvas;
    canvas.resize(100, 60);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();

    QImage screenshot(100, 60, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-50.0, -30.0, 100.0, 60.0));

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(-40, -20, 80, 40);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("MMMMMM");
    line.quad = QPolygonF({
        QPointF(-35.0, -10.0),
        QPointF(35.0, -10.0),
        QPointF(35.0, 10.0),
        QPointF(-35.0, 10.0),
    });
    presentation->lines.push_back(line);
    renderer.setOcrPresentation(presentation);

    const QImage output = renderCanvas(canvas);
    const QRect inkBounds = paintedInkBounds(output, QRect(15, 20, 70, 20), 80);
    require(inkBounds.width() >= 67 && inkBounds.height() <= 15,
            "wide OCR text should use the width-limited uniform fit without vertical stretching");
    require(std::abs(inkBounds.center().y() - 29.5) <= 1.0,
            "width-limited OCR text should remain vertically centered");
    canvas.setCustomRenderer(nullptr);
}

void verticalOcrTextKeepsCjkGraphemesUprightAndSelectable() {
    SnowCanvasWidget canvas;
    canvas.resize(80, 100);
    canvas.setClearBackgroundEnabled(false);
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "camera should update");

    ScreenshotCanvasRenderer renderer(canvas);
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();

    QImage screenshot(80, 100, QImage::Format_RGBA8888);
    screenshot.fill(QColor(0, 80, 240));
    renderer.setImage(std::move(screenshot), QRectF(-40.0, -50.0, 80.0, 100.0));

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(-20, -40, 40, 80);
    ScreenshotOcrLine line;
    line.text = QString::fromUcs4(U"\u4e00\u4e00");
    line.direction = ScreenshotOcrTextDirection::Vertical;
    line.quad = QPolygonF({
        QPointF(-12.0, -30.0),
        QPointF(12.0, -30.0),
        QPointF(12.0, 30.0),
        QPointF(-12.0, 30.0),
    });
    presentation->lines.push_back(line);
    renderer.setOcrPresentation(presentation);

    const QImage output = renderCanvas(canvas);
    const QRect upperInk = paintedInkBounds(output, QRect(28, 20, 24, 30), 80);
    const QRect lowerInk = paintedInkBounds(output, QRect(28, 50, 24, 30), 80);
    require(!upperInk.isEmpty() && !lowerInk.isEmpty(),
            "vertical OCR should paint every CJK grapheme in its own cell");
    require(upperInk.width() > upperInk.height() * 2 && lowerInk.width() > lowerInk.height() * 2,
            "vertical OCR should keep upright CJK glyphs upright instead of rotating the line");

    const ScreenshotOcrTextPosition start = renderer.ocrTextPositionAt(QPointF(0.0, -29.0));
    const ScreenshotOcrTextPosition middle = renderer.ocrTextPositionAt(QPointF(0.0, 0.0));
    const ScreenshotOcrTextPosition end = renderer.ocrTextPositionAt(QPointF(0.0, 29.0));
    require(start.characterIndex == 0 && middle.characterIndex == 1 && end.characterIndex == 2,
            "rendered vertical OCR hit testing should advance from top to bottom");

    presentation->beginTextSelection(start);
    presentation->updateTextSelection(middle);
    presentation->finishTextSelection();
    require(presentation->selectedText() == QString::fromUcs4(U"\u4e00"),
            "vertical OCR selection should retain exact source-text offsets");
    const QImage selected = renderCanvas(canvas);
    bool upperCellChanged = false;
    bool lowerCellChanged = false;
    for (int y = 20; y < 50; ++y) {
        for (int x = 28; x < 52; ++x) {
            upperCellChanged = upperCellChanged || selected.pixel(x, y) != output.pixel(x, y);
            lowerCellChanged =
                lowerCellChanged || selected.pixel(x, y + 30) != output.pixel(x, y + 30);
        }
    }
    require(upperCellChanged && !lowerCellChanged,
            "vertical OCR selection highlighting should cover only selected cells");
    canvas.setCustomRenderer(nullptr);
}

void scrollingModeClearsPassThroughMaskBeforeRestoringRenderer() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(80, 80);
    overlay.show();
    QApplication::processEvents();

    overlay.setInputPassThroughRect(QRect(20, 20, 40, 40));
    require(!overlay.mask().isEmpty(),
            "scrolling capture should install a selection pass-through mask");
    overlay.setScrollingCaptureMode(true);
    QApplication::processEvents();

    CanvasPaintObserver paintObserver(overlay);
    canvas->installEventFilter(&paintObserver);
    paintObserver.begin();
    overlay.setScrollingCaptureMode(false);

    require(paintObserver.sawPaint(),
            "restoring standard rendering should repaint the canvas immediately");
    require(paintObserver.maskWasEmpty(),
            "the pass-through mask should be clear before standard rendering repaints");
    require(overlay.mask().isEmpty(),
            "leaving scrolling capture should clear the pass-through mask");
    canvas->removeEventFilter(&paintObserver);
}

void scrollingThumbnailIsAnEmbeddedScreenshotWidget() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(180, 240);
    overlay.show();
    QApplication::processEvents();

    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "screenshot window should own the thumbnail widget");
    require(thumbnail->parentWidget() == &overlay,
            "thumbnail should be a direct child of the screenshot window");
    require(!thumbnail->isWindow(), "thumbnail must not be a top-level window");
    require(thumbnail->window() == &overlay,
            "thumbnail should render on the screenshot window surface");

    const QRect selection(8, 20, 20, 160);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);

    QImage preview(128, 384, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    overlay.updateScrollingThumbnail(preview, QSize(100, 300),
                                     ScreenshotScrollingStitchChange::Initial, 300);
    QApplication::processEvents();

    require(thumbnail->isVisible(), "thumbnail should become visible after a frame");
    require(overlay.rect().contains(thumbnail->geometry()),
            "embedded thumbnail should stay within the screenshot window");
    require(overlay.mask().contains(thumbnail->geometry().center()),
            "window mask should keep an overlapping thumbnail interactive");
    require(!overlay.mask().contains(QPoint(20, 100)),
            "selection should remain pass-through outside the thumbnail");
    require(!QApplication::topLevelWidgets().contains(thumbnail),
            "thumbnail should not create another application window");

    overlay.setScrollingCaptureMode(false);
    require(thumbnail->isHidden(), "leaving scrolling mode should hide the thumbnail");
    require(!overlay.scrollingThumbnailTrim().isValid(),
            "leaving scrolling mode should clear thumbnail trim state");
}

void scrollingThumbnailStaysWithinHostDisplayWhenNeitherSideFits() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(180, 240);
    overlay.show();
    QApplication::processEvents();

    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "screenshot window should own the thumbnail widget");

    const QRect selection(8, 20, 164, 160);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);

    QImage preview(128, 384, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    overlay.updateScrollingThumbnail(preview, QSize(100, 300),
                                     ScreenshotScrollingStitchChange::Initial, 300);
    QApplication::processEvents();

    require(thumbnail->isVisible(), "thumbnail should become visible after a frame");
    require(overlay.rect().contains(thumbnail->geometry()),
            "thumbnail should stay within the display that hosts the capture selection");
}

void scrollingThumbnailAlignsWithTopEdgeSelection() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(300, 240);
    overlay.show();
    QApplication::processEvents();

    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "screenshot window should own the thumbnail widget");

    const QRect selection(20, 0, 100, 160);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);

    QImage preview(128, 384, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    overlay.updateScrollingThumbnail(preview, QSize(100, 300),
                                     ScreenshotScrollingStitchChange::Initial, 300);
    QApplication::processEvents();

    require(thumbnail->geometry().top() == selection.top(),
            "a thumbnail beside a top-edge selection should align with its top edge");
}

void scrollingThumbnailCropHandlesUseVerticalResizeCursor() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    QImage preview(128, 100, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    thumbnail.setStitchedImage(preview, QSize(100, 100), ScreenshotScrollingStitchChange::Initial,
                               100);

    const QPoint handlePosition(64, 0);
    QMouseEvent hoverHandle(QEvent::MouseMove, QPointF(handlePosition),
                            QPointF(thumbnail.mapToGlobal(handlePosition)), Qt::NoButton,
                            Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&thumbnail, &hoverHandle);
    require(thumbnail.cursor().shape() == Qt::SizeVerCursor,
            "hovering a thumbnail crop handle should show the vertical resize cursor");

    const QPoint previewPosition(64, 50);
    QMouseEvent hoverPreview(QEvent::MouseMove, QPointF(previewPosition),
                             QPointF(thumbnail.mapToGlobal(previewPosition)), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&thumbnail, &hoverPreview);
    require(thumbnail.cursor().shape() == Qt::ArrowCursor,
            "leaving a thumbnail crop handle should restore the default cursor");
}

void scrollingThumbnailCropHandlesStayInsidePaintBounds() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    QImage preview(128, 100, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    thumbnail.setStitchedImage(preview, QSize(100, 100), ScreenshotScrollingStitchChange::Initial,
                               100);

    QImage rendered(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    thumbnail.render(&rendered);

    const QColor handleColor(QStringLiteral("#faad14"));
    require(rendered.pixelColor(64, 0) == handleColor &&
                rendered.pixelColor(64, 1) == handleColor &&
                rendered.pixelColor(64, 2) == handleColor &&
                rendered.pixelColor(64, 3) == handleColor &&
                rendered.pixelColor(64, rendered.height() - 4) == handleColor &&
                rendered.pixelColor(64, rendered.height() - 3) == handleColor &&
                rendered.pixelColor(64, rendered.height() - 2) == handleColor &&
                rendered.pixelColor(64, rendered.height() - 1) == handleColor,
            "thumbnail crop handles should remain fully visible without an edge gap");
    require(rendered.pixelColor(64, 4) != handleColor &&
                rendered.pixelColor(64, rendered.height() - 5) != handleColor,
            "thumbnail crop handles should have symmetric edge geometry");
}

void scrollingThumbnailHighlightUsesCaptureImageHeight() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    QImage initialPreview(128, 100, QImage::Format_RGBA8888);
    initialPreview.fill(QColor(30, 90, 180));
    thumbnail.setStitchedImage(initialPreview, QSize(100, 100),
                               ScreenshotScrollingStitchChange::Initial, 100);

    QImage appendedPreview(128, 50, QImage::Format_RGBA8888);
    appendedPreview.fill(QColor(30, 90, 180));
    thumbnail.setStitchedImage(appendedPreview, QSize(100, 150),
                               ScreenshotScrollingStitchChange::AppendedDown, 50);
    require(thumbnail.highlightedRowsForTesting() == QRect(0, 50, 100, 100),
            "downward scrolling should highlight a full captured image, not only added rows");

    QImage prependedPreview(128, 25, QImage::Format_RGBA8888);
    prependedPreview.fill(QColor(30, 90, 180));
    thumbnail.setStitchedImage(prependedPreview, QSize(100, 175),
                               ScreenshotScrollingStitchChange::PrependedUp, 25);
    require(thumbnail.highlightedRowsForTesting() == QRect(0, 0, 100, 100),
            "upward scrolling should highlight a full captured image at the top");
}

void scrollingThumbnailTilesPreserveRowsAndBoundStorage() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);

    QImage initial(128, 300, QImage::Format_RGBA8888);
    initial.fill(QColor(20, 40, 60));
    thumbnail.setStitchedImage(initial, QSize(128, 300), ScreenshotScrollingStitchChange::Initial,
                               300);

    QImage appended(128, 300, QImage::Format_RGBA8888);
    appended.fill(QColor(80, 100, 120));
    thumbnail.setStitchedImage(appended, QSize(128, 600),
                               ScreenshotScrollingStitchChange::AppendedDown, 300);

    QImage prepended(128, 100, QImage::Format_RGBA8888);
    prepended.fill(QColor(140, 160, 180));
    thumbnail.setStitchedImage(prepended, QSize(128, 700),
                               ScreenshotScrollingStitchChange::PrependedUp, 100);

    QImage expected(128, 700, QImage::Format_RGBA8888);
    QPainter painter(&expected);
    painter.drawImage(QPoint(0, 0), prepended);
    painter.drawImage(QPoint(0, 100), initial);
    painter.drawImage(QPoint(0, 400), appended);
    painter.end();
    require(thumbnail.previewImageForTesting() == expected,
            "preview tiles should preserve prepended and appended row order");

    constexpr qsizetype tileBytes = 128 * 256 * 4;
    require(thumbnail.previewAllocatedBytesForTesting() <=
                thumbnail.previewLogicalBytesForTesting() + tileBytes,
            "preview tile allocation should stay within one tile of logical bytes");
}

void scrollingThumbnailReplacementDiscardsStaleTiles() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);

    QImage initial(128, 520, QImage::Format_RGBA8888);
    initial.fill(QColor(20, 40, 60));
    thumbnail.setStitchedImage(initial, QSize(640, 2600), ScreenshotScrollingStitchChange::Initial,
                               2600);

    QImage replacement(128, 650, QImage::Format_RGBA8888);
    for (int row = 0; row < replacement.height(); ++row) {
        std::fill(replacement.scanLine(row), replacement.scanLine(row) + replacement.bytesPerLine(),
                  static_cast<uchar>(row & 0xff));
    }
    thumbnail.setStitchedImage(replacement, QSize(640, 3250),
                               ScreenshotScrollingStitchChange::AppendedDown, 650, true);

    require(thumbnail.previewImageForTesting() == replacement,
            "a complete stitch snapshot should replace every stale preview tile");
}

void scrollingThumbnailEdgePatchesRefreshOverlap() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);

    QImage initial(128, 520, QImage::Format_RGBA8888);
    initial.fill(QColor(20, 40, 60));
    thumbnail.setStitchedImage(initial, QSize(128, 520), ScreenshotScrollingStitchChange::Initial,
                               520);

    QImage appendedPatch(128, 180, QImage::Format_RGBA8888);
    appendedPatch.fill(QColor(80, 100, 120));
    thumbnail.setStitchedImage(appendedPatch, QSize(128, 600),
                               ScreenshotScrollingStitchChange::AppendedDown, 80, false, 100);
    QImage expected(128, 600, QImage::Format_RGBA8888);
    QPainter appendPainter(&expected);
    appendPainter.drawImage(QPoint(0, 0), initial, QRect(0, 0, 128, 420));
    appendPainter.drawImage(QPoint(0, 420), appendedPatch);
    appendPainter.end();
    require(thumbnail.previewImageForTesting() == expected,
            "an appended edge patch should refresh its rewritten overlap");

    QImage prependedPatch(128, 150, QImage::Format_RGBA8888);
    prependedPatch.fill(QColor(140, 160, 180));
    thumbnail.setStitchedImage(prependedPatch, QSize(128, 660),
                               ScreenshotScrollingStitchChange::PrependedUp, 60, false, 90);
    QImage prependedExpected(128, 660, QImage::Format_RGBA8888);
    QPainter prependPainter(&prependedExpected);
    prependPainter.drawImage(QPoint(0, 0), prependedPatch);
    prependPainter.drawImage(QPoint(0, 150), expected, QRect(0, 90, 128, 510));
    prependPainter.end();
    require(thumbnail.previewImageForTesting() == prependedExpected,
            "a prepended edge patch should refresh its rewritten overlap");

    constexpr qsizetype tileBytes = 128 * 256 * 4;
    require(thumbnail.previewAllocatedBytesForTesting() <=
                thumbnail.previewLogicalBytesForTesting() + tileBytes,
            "edge patch storage should remain bounded by one spare tile");
}

void stableScrollingGeometryDoesNotReapplyWindowMask() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(300, 300);
    overlay.show();
    QApplication::processEvents();

    const QRect selection(20, 20, 180, 180);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);
    QImage preview(128, 128, QImage::Format_RGBA8888);
    preview.fill(QColor(30, 90, 180));
    overlay.updateScrollingThumbnail(preview, QSize(128, 128),
                                     ScreenshotScrollingStitchChange::Initial, 128);
    const quint64 stableCount = overlay.windowMaskApplicationCountForTesting();

    for (int update = 0; update < 20; ++update) {
        overlay.updateScrollingThumbnail(preview, QSize(128, 128),
                                         ScreenshotScrollingStitchChange::Replaced, 0);
    }
    require(overlay.windowMaskApplicationCountForTesting() == stableCount,
            "stable scrolling geometry should not reapply the native window mask");
}

void scrollingThumbnailPresentsAcceptedFrameWithoutInput() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(420, 300);
    overlay.show();
    QApplication::processEvents();

    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "overlay should own the scrolling thumbnail");

    const QRect selection(20, 20, 180, 180);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);

    QImage initial(128, 256, QImage::Format_RGBA8888);
    initial.fill(QColor(30, 90, 180));
    overlay.updateScrollingThumbnail(initial, QSize(128, 256),
                                     ScreenshotScrollingStitchChange::Initial, 256);
    QApplication::processEvents();

    const QSize stableSize = thumbnail->size();
    CanvasPaintRegionObserver paintObserver;
    thumbnail->installEventFilter(&paintObserver);
    paintObserver.begin();

    QImage appended(128, 64, QImage::Format_RGBA8888);
    appended.fill(QColor(210, 50, 40));
    overlay.updateScrollingThumbnail(appended, QSize(128, 320),
                                     ScreenshotScrollingStitchChange::AppendedDown, 64);

    require(thumbnail->size() == stableSize,
            "the refresh regression requires unchanged thumbnail geometry");
    require(thumbnail->previewImageForTesting().pixelColor(64, 300) == QColor(210, 50, 40),
            "the accepted patch should already be present in thumbnail state");
    require(paintObserver.region().contains(thumbnail->rect()),
            "an accepted scrolling frame must repaint without waiting for mouse input");
    thumbnail->removeEventFilter(&paintObserver);
}

#if defined(Q_OS_WIN)
class CursorPositionRestorer final {
  public:
    CursorPositionRestorer() : m_valid(GetCursorPos(&m_position) != FALSE) {}

    ~CursorPositionRestorer() {
        if (m_valid) {
            static_cast<void>(SetCursorPos(m_position.x, m_position.y));
        }
    }

  private:
    POINT m_position{};
    bool m_valid = false;
};

COLORREF desktopPixel(const QPoint& globalPosition) {
    HDC screen = GetDC(nullptr);
    require(screen != nullptr, "native thumbnail test could not access the desktop surface");
    const COLORREF pixel = GetPixel(screen, globalPosition.x(), globalPosition.y());
    ReleaseDC(nullptr, screen);
    require(pixel != CLR_INVALID, "native thumbnail test could not read the desktop pixel");
    return pixel;
}

QPoint nativeGlobalPosition(QWidget& window, const QPoint& localPosition) {
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    require(hwnd != nullptr, "native thumbnail test could not access the overlay HWND");
    RECT client{};
    require(GetClientRect(hwnd, &client) != FALSE && client.right > client.left &&
                client.bottom > client.top && window.width() > 0 && window.height() > 0,
            "native thumbnail test could not determine the overlay client geometry");
    POINT native{
        static_cast<LONG>(std::lround(static_cast<double>(localPosition.x()) *
                                      static_cast<double>(client.right - client.left) /
                                      static_cast<double>(window.width()))),
        static_cast<LONG>(std::lround(static_cast<double>(localPosition.y()) *
                                      static_cast<double>(client.bottom - client.top) /
                                      static_cast<double>(window.height()))),
    };
    require(ClientToScreen(hwnd, &native) != FALSE,
            "native thumbnail test could not map the overlay point to the desktop");
    return QPoint(native.x, native.y);
}

bool colorNear(COLORREF actual, const QColor& expected, int tolerance = 12) {
    return std::abs(GetRValue(actual) - expected.red()) <= tolerance &&
           std::abs(GetGValue(actual) - expected.green()) <= tolerance &&
           std::abs(GetBValue(actual) - expected.blue()) <= tolerance;
}

void scrollingThumbnailPublishesAcceptedFrameToNativeSurface(bool excludedFromCapture) {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native thumbnail test requires the Windows Qt platform");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "native thumbnail test requires a primary screen");

    constexpr QSize overlaySize(420, 300);
    const QRect available = screen->availableGeometry();
    require(available.width() >= overlaySize.width() + 64 &&
                available.height() >= overlaySize.height() + 64,
            "native thumbnail test requires a 484 by 364 pixel desktop area");

    CursorPositionRestorer restoreCursor;
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.setGeometry(QRect(available.topLeft() + QPoint(32, 32), overlaySize));
    overlay.showPreparedFrame();
    QApplication::processEvents();

    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "native thumbnail test could not find the thumbnail");

    const QRect selection(20, 20, 180, 180);
    overlay.setInputPassThroughRect(selection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(selection);

    const QColor initialColor(30, 90, 180);
    QImage initial(128, 256, QImage::Format_RGBA8888);
    initial.fill(initialColor);
    overlay.updateScrollingThumbnail(initial, QSize(128, 256),
                                     ScreenshotScrollingStitchChange::Initial, 256);
    QApplication::processEvents();
    const QSize stableThumbnailSize = thumbnail->size();

    const QPoint cursorPosition = nativeGlobalPosition(overlay, selection.center());
    require(SetCursorPos(cursorPosition.x(), cursorPosition.y()) != FALSE,
            "native thumbnail test could not move the cursor into the pass-through region");
    QApplication::processEvents();

    const QPoint samplePosition = nativeGlobalPosition(
        overlay,
        thumbnail->mapTo(&overlay, QPoint(thumbnail->width() / 2, thumbnail->height() - 20)));
    const COLORREF initialPixel = desktopPixel(samplePosition);
    if (!colorNear(initialPixel, initialColor)) {
        std::cerr << "native thumbnail initial pixel at " << samplePosition.x() << ','
                  << samplePosition.y() << ": rgb(" << static_cast<int>(GetRValue(initialPixel))
                  << ',' << static_cast<int>(GetGValue(initialPixel)) << ','
                  << static_cast<int>(GetBValue(initialPixel))
                  << "), thumbnail=" << thumbnail->geometry().x() << ','
                  << thumbnail->geometry().y() << ' ' << thumbnail->width() << 'x'
                  << thumbnail->height() << '\n';
    }
    require(colorNear(initialPixel, initialColor),
            "native thumbnail test could not observe the initial composed preview");

    if (excludedFromCapture) {
        constexpr DWORD excludeFromCapture = 0x00000011;
        require(SetWindowDisplayAffinity(reinterpret_cast<HWND>(overlay.winId()),
                                         excludeFromCapture) != FALSE,
                "native thumbnail test could not exclude the overlay from capture");
    }

    const QColor appendedColor(210, 50, 40);
    QImage appended(128, 64, QImage::Format_RGBA8888);
    appended.fill(appendedColor);
    overlay.updateScrollingThumbnail(appended, QSize(128, 320),
                                     ScreenshotScrollingStitchChange::AppendedDown, 64);
    require(thumbnail->size() == stableThumbnailSize,
            "native presentation regression requires fixed thumbnail geometry");

    if (excludedFromCapture) {
        require(SetWindowDisplayAffinity(reinterpret_cast<HWND>(overlay.winId()), WDA_NONE) !=
                    FALSE,
                "native thumbnail test could not restore overlay capture visibility");
    }

    Sleep(100);
    require(colorNear(desktopPixel(samplePosition), appendedColor),
            "accepted thumbnail pixels were painted but not published to the native surface");
}
#endif

void historyLoadingMessageFollowsVisibility() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget();
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(420, 240);
    overlay.show();
    QApplication::processEvents();

    adqt::widgets::AdMessage* messages = adqt::widgets::AdMessageService::instance(&overlay);
    require(messages != nullptr, "history message service was not created");
    require(messages->count() == 0, "history message should start closed");

    overlay.setHistoryLoadingVisible(true);
    require(messages->count() == 1, "history loading message was not shown");
    auto* label = overlay.findChild<QLabel*>(QStringLiteral("ad-message-content"));
    require(label != nullptr, "history message content was not created");
    require(label->text() == QStringLiteral("Loading screenshot history"),
            "history loading message text is incorrect");
    require(overlay.findChild<QWidget*>(QStringLiteral("ad-message-notice")) != nullptr,
            "history loading prompt did not use the ant_design_qt message component");

    overlay.setHistoryLoadingVisible(false);
    require(messages->count() == 0, "history loading message was not hidden");
}

void horizontalScrollingThumbnailUsesColumnTilesAndHorizontalInteraction() {
    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    thumbnail.setRecognitionMode(ScreenshotScrollingRecognitionMode::Horizontal);
    thumbnail.setMaximumPreviewExtent(220);

    QImage initial(300, 128, QImage::Format_RGBA8888);
    initial.fill(QColor(20, 40, 60));
    thumbnail.setStitchedImage(initial, QSize(300, 128), ScreenshotScrollingStitchChange::Initial,
                               300);
    parent.show();
    thumbnail.show();
    QApplication::processEvents();
    require(thumbnail.size() == QSize(220, 128),
            "horizontal preview should be wide with a fixed 128-pixel height");
    auto* scrollBar = thumbnail.findChild<QScrollBar*>();
    require(scrollBar != nullptr && scrollBar->orientation() == Qt::Horizontal &&
                !scrollBar->isHidden(),
            "horizontal preview should expose a horizontal scrollbar when clipped");

    QImage appended(180, 128, QImage::Format_RGBA8888);
    appended.fill(QColor(80, 100, 120));
    thumbnail.setStitchedImage(appended, QSize(480, 128),
                               ScreenshotScrollingStitchChange::AppendedRight, 180);
    QImage prepended(90, 128, QImage::Format_RGBA8888);
    prepended.fill(QColor(140, 160, 180));
    thumbnail.setStitchedImage(prepended, QSize(570, 128),
                               ScreenshotScrollingStitchChange::PrependedLeft, 90);

    QImage expected(570, 128, QImage::Format_RGBA8888);
    QPainter painter(&expected);
    painter.drawImage(QPoint(0, 0), prepended);
    painter.drawImage(QPoint(90, 0), initial);
    painter.drawImage(QPoint(390, 0), appended);
    painter.end();
    require(thumbnail.previewImageForTesting() == expected,
            "horizontal preview tiles should preserve prepended and appended column order");
    constexpr qsizetype tileBytes = 128 * 256 * 4;
    require(thumbnail.previewAllocatedBytesForTesting() <=
                thumbnail.previewLogicalBytesForTesting() + tileBytes,
            "horizontal preview allocation should stay within one spare column tile");

    scrollBar->setValue(0);
    const QPoint handlePosition(0, 64);
    QMouseEvent hoverHandle(QEvent::MouseMove, QPointF(handlePosition),
                            QPointF(thumbnail.mapToGlobal(handlePosition)), Qt::NoButton,
                            Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&thumbnail, &hoverHandle);
    require(thumbnail.cursor().shape() == Qt::SizeHorCursor,
            "horizontal trim handles should use the horizontal resize cursor");

    const int beforeWheel = scrollBar->value();
    QWheelEvent wheel(QPointF(thumbnail.rect().center()),
                      thumbnail.mapToGlobal(thumbnail.rect().center()), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&thumbnail, &wheel);
    require(wheel.isAccepted() && scrollBar->value() > beforeWheel,
            "vertical wheel input should scroll a horizontal preview as a fallback");
}

void horizontalScrollingThumbnailPrefersAboveThenBelowSelection() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(520, 420);
    overlay.show();
    QApplication::processEvents();
    auto* thumbnail = dynamic_cast<ScreenshotScrollingThumbnailWidget*>(overlay.findChild<QWidget*>(
        QStringLiteral("screenshot-scrolling-thumbnail"), Qt::FindDirectChildrenOnly));
    require(thumbnail != nullptr, "overlay should own the horizontal scrolling preview");

    const QImage preview(360, 128, QImage::Format_RGBA8888);
    const QRect upperSelection(40, 30, 320, 100);
    overlay.setInputPassThroughRect(upperSelection);
    overlay.setScrollingCaptureMode(true);
    overlay.beginScrollingThumbnail(upperSelection, ScreenshotScrollingRecognitionMode::Horizontal);
    overlay.updateScrollingThumbnail(preview, QSize(360, 128),
                                     ScreenshotScrollingStitchChange::Initial, 360);
    QApplication::processEvents();
    require(thumbnail->geometry().top() > upperSelection.bottom(),
            "horizontal preview should fall below a selection when above does not fit");

    const QRect lowerSelection(40, 290, 320, 100);
    overlay.beginScrollingThumbnail(lowerSelection, ScreenshotScrollingRecognitionMode::Horizontal);
    overlay.updateScrollingThumbnail(preview, QSize(360, 128),
                                     ScreenshotScrollingStitchChange::Initial, 360);
    QApplication::processEvents();
    require(thumbnail->geometry().bottom() < lowerSelection.top(),
            "horizontal preview should move above a low selection when below does not fit");
}

void screenshotMessagesFollowSelectionAndRememberTheirOwner() {
    NoopOverlayEventSink eventSink;
    auto* firstCanvas = new SnowCanvasWidget;
    auto* secondCanvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow firstOverlay(eventSink, firstCanvas);
    ScreenshotOverlayWindow secondOverlay(eventSink, secondCanvas);
    firstOverlay.resize(320, 240);
    secondOverlay.resize(320, 240);
    firstOverlay.show();
    secondOverlay.show();

    CapturedDisplayModel firstDisplay;
    firstDisplay.canvasRect = QRect(0, 0, 320, 240);
    firstDisplay.active = true;
    CapturedDisplayModel secondDisplay;
    secondDisplay.canvasRect = QRect(320, 0, 320, 240);
    secondDisplay.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(firstDisplay, &firstOverlay);
    displays.appendDisplay(secondDisplay, &secondOverlay);

    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(40, 40, 120, 80));
    QWidget toolbarFallback;
    ScreenshotMessageService messages(displays, geometry, selection,
                                      [&toolbarFallback]() { return &toolbarFallback; });
    const QString key = QStringLiteral("screenshot-message-owner-test");

    messages.loading(key, QStringLiteral("Preparing screenshot"));
    auto* firstMessages = adqt::widgets::AdMessageService::instance(&firstOverlay);
    auto* secondMessages = adqt::widgets::AdMessageService::instance(&secondOverlay);
    require(firstMessages != nullptr && firstMessages->count() == 1,
            "a selection message should use the overlay containing the selection");
    require(secondMessages != nullptr && secondMessages->count() == 0,
            "a selection message should not appear on another display");

    messages.loading(key, QStringLiteral("Preparing screenshot"), QRectF(360, 40, 120, 80));
    require(firstMessages->count() == 0,
            "moving a keyed message should clear it from its previous overlay");
    require(secondMessages->count() == 1,
            "explicit asynchronous geometry should choose the matching display overlay");

    selection.setSelectionRect(QRectF(40, 40, 120, 80));
    messages.destroy(key);
    require(secondMessages->count() == 0,
            "destroying a keyed message should use its remembered owner");
}

void screenshotMessagesFallBackWhenNoOverlayIsAvailable() {
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(40, 40, 120, 80));
    QWidget toolbarFallback;
    ScreenshotMessageService messages(displays, geometry, selection,
                                      [&toolbarFallback]() { return &toolbarFallback; });

    messages.error(QStringLiteral("screenshot-message-fallback-test"),
                   QStringLiteral("Unable to prepare screenshot"));
    auto* fallbackMessages = adqt::widgets::AdMessageService::instance(&toolbarFallback);
    require(fallbackMessages != nullptr && fallbackMessages->count() == 1,
            "a screenshot message should use the toolbar fallback when no overlay exists");

    QWidget preferredOwner;
    messages.warning(QStringLiteral("screenshot-message-preferred-owner-test"),
                     QStringLiteral("Recognition unavailable"), {}, &preferredOwner);
    auto* preferredMessages = adqt::widgets::AdMessageService::instance(&preferredOwner);
    require(preferredMessages != nullptr && preferredMessages->count() == 1,
            "an explicit recognition window should take precedence over the fallback");
}

void canvasWheelZoomCanBeDisabled() {
    WheelTestCanvas canvas;
    canvas.resize(200, 200);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0),
            "the wheel zoom test should initialize the camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Select),
            "the wheel zoom test should use the generic selection tool");

    const QRectF probe(-10.0, -10.0, 20.0, 20.0);
    const QRect initialViewRect = canvas.viewRectForCanvasRect(probe);
    require(!initialViewRect.isEmpty(), "the wheel zoom test should resolve a view rect");
    const auto sendWheel = [&canvas]() {
        const QPointF localPosition(100.0, 100.0);
        QWheelEvent event(localPosition, canvas.mapToGlobal(localPosition.toPoint()), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        event.ignore();
        canvas.dispatchWheel(&event);
        return event.isAccepted();
    };

    require(canvas.wheelZoomEnabled(), "wheel zoom should remain enabled by default");
    canvas.setWheelZoomEnabled(false);
    require(!canvas.wheelZoomEnabled(), "wheel zoom should report its disabled state");
    require(sendWheel(), "a disabled canvas should consume wheel zoom input");
    require(canvas.viewRectForCanvasRect(probe) == initialViewRect,
            "a disabled canvas should ignore wheel zoom");
}

void disabledCanvasBlocksWidgetLevelToolInput() {
    WheelTestCanvas canvas;
    canvas.resize(200, 160);
    canvas.show();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0),
            "the interaction-gate test should initialize the camera");
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
            "the interaction-gate test should activate Serial Number");

    const auto sendFontSizeWheel = [&canvas]() {
        const QPointF position(canvas.rect().center());
        QWheelEvent event(position, canvas.mapToGlobal(position.toPoint()), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        event.ignore();
        canvas.dispatchWheel(&event);
    };

    const double initialFontSize = canvas.canvasStyleToolbarState().serialNumberStyle.fontSize;
    canvas.setInteractionEnabled(false);
    sendFontSizeWheel();
    require(canvas.canvasStyleToolbarState().serialNumberStyle.fontSize == initialFontSize,
            "disabled interaction must block widget-level Serial Number wheel edits");

    canvas.setInteractionEnabled(true);
    sendFontSizeWheel();
    require(canvas.canvasStyleToolbarState().serialNumberStyle.fontSize > initialFontSize,
            "re-enabled interaction should restore widget-level tool input");
}

void overlayCanvasesAreDisabledUntilCanvasInteractionIsEnabled() {
    NoopOverlayEventSink eventSink;
    auto* activeCanvas = new SnowCanvasWidget;
    auto* reusableCanvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow activeOverlay(eventSink, activeCanvas);
    ScreenshotOverlayWindow reusableOverlay(eventSink, reusableCanvas);

    require(!activeCanvas->interactionEnabled() && !reusableCanvas->interactionEnabled(),
            "new screenshot overlays must not accept canvas input while Move is active");

    CapturedDisplayModel activeDisplay;
    activeDisplay.active = true;
    CapturedDisplayModel reusableDisplay;
    reusableDisplay.active = false;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(activeDisplay, &activeOverlay);
    displays.appendDisplay(reusableDisplay, &reusableOverlay);

    ScreenshotOverlayCanvasPresenter presenter({});
    presenter.setCanvasInteractionEnabled(displays, true);
    require(activeCanvas->interactionEnabled() && reusableCanvas->interactionEnabled(),
            "enabling a drawing tool must enable every reusable overlay canvas");

    presenter.setCanvasInteractionEnabled(displays, false);
    require(!activeCanvas->interactionEnabled() && !reusableCanvas->interactionEnabled(),
            "activating a non-drawing tool must disable every reusable overlay canvas");
}

void overlayNativeSurfaceIsReleasedBeforeDeferredObjectDeletion() {
    NoopOverlayEventSink eventSink;
    auto* overlay = new ScreenshotOverlayWindow(eventSink, new SnowCanvasWidget);
    overlay->resize(640, 360);
    overlay->show();
    QApplication::processEvents();
    static_cast<void>(overlay->winId());

    require(overlay->internalWinId() != 0 && overlay->testAttribute(Qt::WA_WState_Created),
            "the teardown test must begin with a live native overlay surface");

    QPointer<ScreenshotOverlayWindow> guard(overlay);
    overlay->releaseNativeSurface();
    require(guard != nullptr,
            "native surface release must keep the event receiver alive until deferred deletion");
    require(overlay->internalWinId() == 0 && !overlay->testAttribute(Qt::WA_WState_Created),
            "native surface release must synchronously destroy the platform window");

    overlay->deleteLater();
    QCoreApplication::sendPostedEvents(overlay, QEvent::DeferredDelete);
    require(guard == nullptr, "the retired overlay must still support normal deferred deletion");
}

void overlayNativeSurfaceRetirementPreservesReusableRenderState() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    overlay.resize(96, 72);

    QImage capturedImage(96, 72, QImage::Format_ARGB32_Premultiplied);
    capturedImage.fill(QColor(37, 113, 211));
    overlay.setScreenshotImage(capturedImage, QRectF(0.0, 0.0, 96.0, 72.0));
    overlay.setScreenshotMaskVisible(true);
    overlay.setScreenshotSelection(QRectF(8.0, 6.0, 64.0, 48.0), true, 0);
    overlay.setInputPassThroughRect(QRect(16, 12, 48, 36));

    ScreenshotCanvasRenderer* renderer = overlay.screenshotRendererForTesting();
    require(renderer != nullptr, "the retirement test requires an overlay renderer");
    const std::uint64_t contentRevision = renderer->contentRevision();
    require(renderer->hasSelection() && renderer->maskVisible(),
            "the retirement test must begin with reusable render state");

    overlay.show();
    QApplication::processEvents();
    require(overlay.internalWinId() != 0 && overlay.testAttribute(Qt::WA_WState_Created),
            "the retirement test must begin with a live native surface");
    require(!canvas->isHidden(),
            "the retirement test must begin with the canvas in its normal visible state");

    overlay.releaseNativeSurface();

    require(overlay.internalWinId() == 0 && !overlay.testAttribute(Qt::WA_WState_Created),
            "retiring an export presentation must synchronously drop its native surface");
    require(!overlay.updatesEnabled() && !canvas->updatesEnabled(),
            "a retired overlay must not schedule paints without a native surface");
    require(renderer->contentRevision() == contentRevision && renderer->hasSelection() &&
                renderer->maskVisible(),
            "native-surface retirement must preserve renderer state needed by export");
    require(canvas->customRenderer() == renderer,
            "native-surface retirement must preserve the reusable canvas-renderer binding");

    overlay.restoreNativeSurface();

    require(overlay.internalWinId() != 0 && overlay.testAttribute(Qt::WA_WState_Created),
            "the next capture must be able to recreate the retired native surface");
    require(!overlay.isVisible() && overlay.updatesEnabled() && canvas->updatesEnabled(),
            "a restored overlay must match its hidden, update-ready pre-capture state");
    require(!canvas->isHidden(),
            "native-surface restoration must preserve the canvas visibility state");
    require(overlay.mask().isEmpty(),
            "native-surface restoration must not reapply the previous capture's input mask");
    require(renderer->contentRevision() == contentRevision && renderer->hasSelection() &&
                renderer->maskVisible(),
            "native-surface restoration must not mutate the retained export state");
}

void canvasCursorLayersKeepToolCursorAfterScreenshotSelection() {
    SnowCanvasWidget toolCanvas;
    require(toolCanvas.setCanvasTool(SnowCanvasTool::Shape),
            "the shape cursor test should activate the shape tool");
    require(toolCanvas.cursor().shape() == Qt::CrossCursor,
            "the shape tool should publish a crosshair cursor immediately");

    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);
    CapturedDisplayModel display;
    display.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);
    ScreenshotOverlayCanvasPresenter presenter({});

    canvas->setCursorForLayer(SnowCanvasCursorLayer::CanvasTool, QCursor(Qt::IBeamCursor));
    require(canvas->cursor().shape() == Qt::IBeamCursor,
            "the canvas tool cursor should be applied to the widget");

    presenter.updateOverlayCursors(displays, true, false);
    require(canvas->cursor().shape() == Qt::CrossCursor,
            "the screenshot selection cursor should override the canvas tool cursor");

    canvas->setCursorForLayer(SnowCanvasCursorLayer::CanvasTool, QCursor(Qt::ArrowCursor));
    require(canvas->cursor().shape() == Qt::CrossCursor,
            "canvas tool updates must not contend with an active screenshot cursor");

    presenter.updateOverlayCursors(displays, false, false);
    require(canvas->cursor().shape() == Qt::ArrowCursor,
            "clearing the screenshot cursor should reveal the latest canvas tool cursor");
}

void overlayPresenterRespectsSelectionHandleVisibility() {
    NoopOverlayEventSink eventSink;
    auto* canvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow overlay(eventSink, canvas);

    CapturedDisplayModel display;
    display.canvasRect = QRect(0, 0, 100, 100);
    display.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);

    ScreenshotOverlayCanvasPresenter presenter({});
    const QRectF selection(10.0, 10.0, 60.0, 40.0);
    presenter.updateOverlayState(displays, selection, 0, 0, QColor(0x33, 0x33, 0x33), false, false,
                                 false, false, false);
    require(overlay.hasScreenshotSelection() && !overlay.screenshotSelectionHandlesVisible() &&
                overlay.screenshotSelectionBorderVisible(),
            "hidden selection control points must retain the recognition selection border");

    presenter.updateOverlayState(displays, selection, 0, 0, QColor(0x33, 0x33, 0x33), false, true,
                                 false, false, false);
    require(overlay.screenshotSelectionHandlesVisible(),
            "the overlay presenter must restore explicitly visible selection control points");
}

void resettingDisplaySessionEditingStateResetsEveryCanvas() {
    NoopOverlayEventSink eventSink;
    auto* activeCanvas = new SnowCanvasWidget;
    auto* reusableCanvas = new SnowCanvasWidget;
    ScreenshotOverlayWindow activeOverlay(eventSink, activeCanvas);
    ScreenshotOverlayWindow reusableOverlay(eventSink, reusableCanvas);

    require(activeCanvas->setCanvasTool(SnowCanvasTool::Shape) &&
                reusableCanvas->setCanvasTool(SnowCanvasTool::Text),
            "the editing reset test should activate drawing tools");

    CapturedDisplayModel activeDisplay;
    activeDisplay.active = true;
    CapturedDisplayModel reusableDisplay;
    reusableDisplay.active = false;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(activeDisplay, &activeOverlay);
    displays.appendDisplay(reusableDisplay, &reusableOverlay);

    ScreenshotOverlayCanvasPresenter presenter({});
    require(presenter.resetEditingState(displays),
            "resetting display editing state should succeed");
    require(activeCanvas->canvasTool() == SnowCanvasTool::Select &&
                reusableCanvas->canvasTool() == SnowCanvasTool::Select,
            "resetting display editing state must include active and reusable canvases");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--overlay-native-surface-release"))) {
        overlayNativeSurfaceIsReleasedBeforeDeferredObjectDeletion();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--overlay-native-surface-retirement"))) {
        overlayNativeSurfaceIsReleasedBeforeDeferredObjectDeletion();
        overlayNativeSurfaceRetirementPreservesReusableRenderState();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--large-image-slice-rendering"))) {
        largeRasterSourceExtentsRenderWithoutFixedPointWrap();
        smoothLargeImageChunkBoundariesRemainPixelEquivalent();
        extremeImageDownscaleUsesSafePreprocessing();
        indexedLargeImageWindowsPreserveTheirColorTable();
        disjointLargeImageExposureDoesNotPaintItsBoundingInterval();
        ordinaryExposedImageRenderingRemainsPixelEquivalent();
        chunkedImagePaintersRenderPastTheRasterCoordinateLimit();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--guide-line-initialization"))) {
        guideLinesInitializeFromGlobalCursorPosition();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-theme-background"))) {
        ocrFilteredImageBlendsTowardTheSuppliedThemeBackground();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--cursor-layer-priority"))) {
        canvasCursorLayersKeepToolCursorAfterScreenshotSelection();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--screenshot-ui-preferences"))) {
        screenshotUiPreferencesNormalizeAndApplyPickerVisibilityPolicies();
        shortcutHintStagesUseTheExactRequiredLines();
        configurableSelectionMaskUsesRequestedPixels();
        cursorAndMonitorGuideLinesUseDashedAndSolidPixels();
        cursorGuideLineMovementInvalidatesOnlyChangedAxes();
        hiddenAndSamePixelCursorMovementDoesNotRepaintGuideLines();
        cursorGuideLineDamageCoversChangedPixelsAtFractionalDprs();
        colorPickerCenterGuidesLeaveTheSampleUntouched();
        onlyTheInputOverlayOwnsGuideLines();
        guideLinesInitializeFromGlobalCursorPosition();
        return 0;
    }
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "the renderer test requires a system TrueType font");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "the vertical OCR renderer test requires a system CJK font");
#endif
#if defined(Q_OS_WIN)
    if (application.arguments().contains(
            QStringLiteral("--native-scrolling-thumbnail-presentation"))) {
        scrollingThumbnailPublishesAcceptedFrameToNativeSurface(false);
        return 0;
    }
    if (application.arguments().contains(
            QStringLiteral("--native-excluded-scrolling-thumbnail-presentation"))) {
        scrollingThumbnailPublishesAcceptedFrameToNativeSurface(true);
        return 0;
    }
#endif
    screenshotImageMaskAndSelectionRenderInTheirOwnedPasses();
    layeredImageSourceMatchesMaterializedOutput();
    physicalViewportRenderingPreservesEveryPixelAtFractionalDprs();
    largeRasterSourceExtentsRenderWithoutFixedPointWrap();
    smoothLargeImageChunkBoundariesRemainPixelEquivalent();
    extremeImageDownscaleUsesSafePreprocessing();
    indexedLargeImageWindowsPreserveTheirColorTable();
    disjointLargeImageExposureDoesNotPaintItsBoundingInterval();
    ordinaryExposedImageRenderingRemainsPixelEquivalent();
    chunkedImagePaintersRenderPastTheRasterCoordinateLimit();
    partialRoundedMaskMatchesFullViewportMaskAtFractionalDpr();
    overlayWatermarkRendersOnlyInsideScreenshotSelection();
    reusedRendererReplacesScreenshotImage();
    bgraScreenshotImagesRenderWithCorrectColors();
    hoveredSelectionToolbarHidesBorderAndRendersShadowPreview();
    roundedSelectionPreviewKeepsTheSameContentBoundsWithAndWithoutShadow();
    squareSelectionPreviewKeepsTheSameContentBoundsWithAndWithoutShadow();
    hoveredSelectionToolbarInvalidatesOnlyPreviewRing();
    hiddenSelectionBorderRetainsSelectionAndMask();
    changingSelectionCornerRadiusRepaintsRoundedMaskAndBorder();
    ocrPresentationSelectionBorderIgnoresRoundedCorners();
    movingSelectionInvalidatesOnlyChangedMaskAndDecorations();
    overlaySelectionMoveDoesNotExpandForInactiveDecorations();
    selectionDamagePlannerAvoidsFullCanvasFallback();
    activeWatermarkAreaMovementUsesUnionDamage();
    activeSpotlightAreaMovementUsesSymmetricDifferenceDamage();
    selectionTransitionsCoverChangedPixelsAtFractionalDprs();
    sharedShadowPreviewMatchesExportAndCacheStaysBounded();
    unchangedOverlaySelectionDoesNotScheduleRepaint();
    selectionTransitionDirtyRegionCoversEveryChangedPixel();
    ocrPresentationRendersWhileCanvasContentIsHidden();
    ocrFilteredImageBlendsTowardTheSuppliedThemeBackground();
    ocrPresentationRendersTextInPinnedResultMode();
    ocrTextAspectFitUsesWidthConstraintWithoutVerticalStretch();
    verticalOcrTextKeepsCjkGraphemesUprightAndSelectable();
    scrollingModeClearsPassThroughMaskBeforeRestoringRenderer();
    scrollingThumbnailIsAnEmbeddedScreenshotWidget();
    scrollingThumbnailStaysWithinHostDisplayWhenNeitherSideFits();
    scrollingThumbnailAlignsWithTopEdgeSelection();
    scrollingThumbnailCropHandlesUseVerticalResizeCursor();
    scrollingThumbnailCropHandlesStayInsidePaintBounds();
    scrollingThumbnailHighlightUsesCaptureImageHeight();
    scrollingThumbnailTilesPreserveRowsAndBoundStorage();
    scrollingThumbnailReplacementDiscardsStaleTiles();
    scrollingThumbnailEdgePatchesRefreshOverlap();
    horizontalScrollingThumbnailUsesColumnTilesAndHorizontalInteraction();
    horizontalScrollingThumbnailPrefersAboveThenBelowSelection();
    stableScrollingGeometryDoesNotReapplyWindowMask();
    scrollingThumbnailPresentsAcceptedFrameWithoutInput();
    historyLoadingMessageFollowsVisibility();
    screenshotMessagesFollowSelectionAndRememberTheirOwner();
    screenshotMessagesFallBackWhenNoOverlayIsAvailable();
    canvasWheelZoomCanBeDisabled();
    disabledCanvasBlocksWidgetLevelToolInput();
    overlayCanvasesAreDisabledUntilCanvasInteractionIsEnabled();
    overlayNativeSurfaceIsReleasedBeforeDeferredObjectDeletion();
    overlayNativeSurfaceRetirementPreservesReusableRenderState();
    canvasCursorLayersKeepToolCursorAfterScreenshotSelection();
    overlayPresenterRespectsSelectionHandleVisibility();
    resettingDisplaySessionEditingStateResetsEveryCanvas();
    screenshotUiPreferencesNormalizeAndApplyPickerVisibilityPolicies();
    shortcutHintStagesUseTheExactRequiredLines();
    configurableSelectionMaskUsesRequestedPixels();
    cursorAndMonitorGuideLinesUseDashedAndSolidPixels();
    cursorGuideLineMovementInvalidatesOnlyChangedAxes();
    hiddenAndSamePixelCursorMovementDoesNotRepaintGuideLines();
    cursorGuideLineDamageCoversChangedPixelsAtFractionalDprs();
    colorPickerCenterGuidesLeaveTheSampleUntouched();
    onlyTheInputOverlayOwnsGuideLines();
    guideLinesInitializeFromGlobalCursorPosition();
    return 0;
}
