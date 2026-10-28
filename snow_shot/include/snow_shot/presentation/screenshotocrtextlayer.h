#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H

#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QColor>
#include <QFont>
#include <QGraphicsView>
#include <QPainterPath>
#include <QRect>
#include <QRegion>
#include <QTransform>

#include <limits>
#include <memory>
#include <vector>

class ScreenshotOcrGraphicsScene;
class ScreenshotOcrGraphicsTextItem;
class ScreenshotOcrGlyphRasterCache;
class ScreenshotOcrSelectionPreviewItem;
class ScreenshotOcrSelectionMarqueeItem;

class ScreenshotOcrTextLayer final : public QGraphicsView {
  public:
    enum class RenderingMode { Normal, SelectionOnly };
    // Pins scale a single 100% image raster; capture overlays retain device-pixel rendering.
    enum class HighlightingResolution { Device, Image };

    explicit ScreenshotOcrTextLayer(
        QWidget* parent = nullptr,
        HighlightingResolution highlightingResolution = HighlightingResolution::Device);
    ~ScreenshotOcrTextLayer() override;

    void setPresentation(std::shared_ptr<ScreenshotOcrPresentation> presentation,
                         RenderingMode mode = RenderingMode::Normal);
    void clearPresentation();
    void setTextHighlighting(bool enabled, const QColor& maskColor);
    [[nodiscard]] QColor textColor() const {
        return m_textColor;
    }
    void synchronize(const QTransform& canvasToViewTransform, const QRect& viewportRect);
    void updateSelection();
    void updateLineText(int lineIndex);
    [[nodiscard]] ScreenshotOcrTextPosition textPositionAt(const QPointF& canvasPosition,
                                                           bool useClosestLine) const;

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 geometrySynchronizationCount() const {
        return m_geometrySynchronizationCount;
    }
    [[nodiscard]] quint64 selectionItemUpdateCount() const {
        return m_selectionItemUpdateCount;
    }
    [[nodiscard]] quint64 overlayUpdateCount() const {
        return m_overlayUpdateCount;
    }
    [[nodiscard]] quint64 glyphRasterBuildCount() const;
    [[nodiscard]] quint64 glyphRasterHitCount() const;
    [[nodiscard]] quint64 glyphDirectPaintCount() const;
    [[nodiscard]] quint64 glyphBackgroundPaintCount() const;
    [[nodiscard]] quint64 backgroundRasterBuildCount() const;
    [[nodiscard]] quint64 backgroundRasterHitCount() const;
    [[nodiscard]] qsizetype backgroundRasterBytes() const;
    [[nodiscard]] quint64 highlightingRasterBuildCount() const;
    [[nodiscard]] QImage highlightingRasterForTesting() const;
    [[nodiscard]] qsizetype glyphRasterBytes() const;
    [[nodiscard]] quint64 previewRasterBuildCount() const;
    [[nodiscard]] quint64 previewRasterHitCount() const;
    [[nodiscard]] qsizetype previewRasterBytes() const;
    [[nodiscard]] quint64 previewPaintNanoseconds() const;
    [[nodiscard]] quint64 marqueePaintNanoseconds() const;
    [[nodiscard]] QRectF marqueeRectangleForTesting() const;
    [[nodiscard]] QRegion takeSelectionDamageForTesting();
    [[nodiscard]] QPainterPath
    selectionPreviewPathForTesting(ScreenshotOcrLineSelectionPreview preview) const;
    void setGlyphCachingEnabledForTesting(bool enabled);
    void setBackgroundRasterCachingEnabledForTesting(bool enabled);
    void setRasterMarginTrimmingEnabledForTesting(bool enabled);
    void setRasterCacheBudgetForTesting(qsizetype bytes);
#endif

  protected:
    void changeEvent(QEvent* event) override;

  private:
    struct TextItem {
        int lineIndex = -1;
        ScreenshotOcrGraphicsTextItem* graphicsText = nullptr;
        QPolygonF viewQuad;
        quint64 selectionRevision = std::numeric_limits<quint64>::max();
    };

    void rebuildTextItems();
    void synchronizeTextItem(TextItem& item, const QTransform& canvasToViewTransform);
    void updateSelectionItem(TextItem& item);
    void updateSelectionOverlay();
    void flushSelectionPreviewDamage();
    void invalidateSelectionRegion(const QRegion& damage);
    void refreshTextAppearance();
    void updateTextHighlighting();

    ScreenshotOcrGraphicsScene* m_scene = nullptr;
    std::unique_ptr<ScreenshotOcrGlyphRasterCache> m_glyphRasterCache;
    std::shared_ptr<ScreenshotOcrPresentation> m_presentation;
    QColor m_textColor;
    QFont m_layoutFont;
    QColor m_textHighlightColor = QColor(Qt::transparent);
    QPainterPath m_textHighlightMask;
    const HighlightingResolution m_highlightingResolution;
    bool m_textHighlightGeometryDirty = true;
    RenderingMode m_renderingMode = RenderingMode::Normal;
    std::vector<TextItem> m_textItems;
    QTransform m_canvasToViewTransform;
    QRect m_viewportRect;
    ScreenshotOcrSelectionMarqueeItem* m_selectionMarquee = nullptr;
    ScreenshotOcrSelectionPreviewItem* m_selectionPreview = nullptr;
    quint64 m_selectionRevision = std::numeric_limits<quint64>::max();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_geometrySynchronizationCount = 0;
    quint64 m_selectionItemUpdateCount = 0;
    quint64 m_overlayUpdateCount = 0;
    QRegion m_selectionDamage;
#endif
    bool m_synchronized = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H
