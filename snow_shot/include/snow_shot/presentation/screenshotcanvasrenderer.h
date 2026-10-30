#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H

#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_snapping.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectioneffectgeometry.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QColor>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include "snow_shot/image/screenshotregiongeometry.h"
#include <QSize>
#include <QTransform>

#include <cstddef>
#include <array>
#include <memory>
#include <optional>

class SnowCanvasWidget;
class ScreenshotOcrPresentation;
class ScreenshotOcrTextLayer;
struct ScreenshotOcrTextPosition;

struct ScreenshotSelectionVisualState {
    QRectF bounds;
    bool present = false;
    bool handlesVisible = true;
    bool borderVisible = true;
    int backgroundOpacityPercent = 0;
    int cornerRadius = 0;
    ScreenshotSelectionBorderType borderType = ScreenshotSelectionBorderType::Shadow;
    // Resolved values for the active selection effect.
    int effectWidth = 0;
    QColor effectColor = QColor(0x33, 0x33, 0x33);
    bool toolbarHovered = false;
    bool effectEditorsVisible = false;
    ScreenshotSelectionEffectHandle hoveredEffectHandle = ScreenshotSelectionEffectHandle::None;
    ScreenshotSelectionEffectHandle activeEffectHandle = ScreenshotSelectionEffectHandle::None;
    bool effectPreviewVisible = false;
    std::optional<ScreenshotRegionGeometry> region;
    ScreenshotRegionGeometry confirmedRegion;
    std::optional<ScreenshotRegionGeometry> highlightedRegion;
    // Immutable for the duration of an individual-region drag; bounds alone cannot
    // establish that every other part of a compound mask stayed stationary.
    std::optional<ScreenshotRegionGeometry> stationaryRegions;
    QRectF marquee;
    bool subtracting = false;
    QColor dangerColor;
    QPainterPath draftPath;
    QVector<QPointF> draftVertices;
    QVector<SnowCanvasSnapGuide> snapGuides;

    [[nodiscard]] bool operator==(const ScreenshotSelectionVisualState& other) const {
        return backgroundOpacityPercent == other.backgroundOpacityPercent &&
               bounds == other.bounds && present == other.present &&
               handlesVisible == other.handlesVisible && borderVisible == other.borderVisible &&
               cornerRadius == other.cornerRadius && borderType == other.borderType &&
               effectWidth == other.effectWidth && effectColor == other.effectColor &&
               toolbarHovered == other.toolbarHovered &&
               effectEditorsVisible == other.effectEditorsVisible &&
               hoveredEffectHandle == other.hoveredEffectHandle &&
               activeEffectHandle == other.activeEffectHandle &&
               effectPreviewVisible == other.effectPreviewVisible && region == other.region &&
               confirmedRegion == other.confirmedRegion &&
               highlightedRegion == other.highlightedRegion &&
               stationaryRegions == other.stationaryRegions && marquee == other.marquee &&
               subtracting == other.subtracting && dangerColor == other.dangerColor &&
               draftPath == other.draftPath && draftVertices == other.draftVertices &&
               snapGuides == other.snapGuides;
    }

    [[nodiscard]] bool operator!=(const ScreenshotSelectionVisualState& other) const {
        return !(*this == other);
    }
};

QRegion planScreenshotSelectionDamage(const ScreenshotSelectionVisualState& previous,
                                      const ScreenshotSelectionVisualState& next,
                                      const QRect& viewportRect,
                                      const QTransform& canvasToViewTransform, bool maskVisible);
QRegion planScreenshotGuideLineDamage(const QRect& viewportRect,
                                      const QPoint& previousCursorPosition,
                                      const QColor& previousCursorColor,
                                      const QColor& previousMonitorCenterColor,
                                      const QPoint& nextCursorPosition,
                                      const QColor& nextCursorColor,
                                      const QColor& nextMonitorCenterColor);

#if defined(SNOW_SHOT_BENCH_INTERNALS)
struct ScreenshotSelectionRenderDiagnostics {
    std::size_t requestedDamagePixels = 0;
    std::size_t pathFallbacks = 0;
    std::size_t backgroundTileBuilds = 0;
    std::size_t previewMaskBuilds = 0;
    std::size_t geometryBuilds = 0;
};

struct ScreenshotGuideLineRenderDiagnostics {
    std::size_t requestedDamagePixels = 0;
    std::size_t updateRequests = 0;
};

ScreenshotSelectionRenderDiagnostics selectionRenderDiagnosticsForCurrentThread();
void resetSelectionRenderDiagnosticsForCurrentThread();
ScreenshotGuideLineRenderDiagnostics guideLineRenderDiagnosticsForCurrentThread();
void resetGuideLineRenderDiagnosticsForCurrentThread();
#endif

class ScreenshotCanvasRenderer final : public SnowCanvasCustomRenderer {
  public:
    enum class RenderMode {
        Standard,
        ScrollingCapture,
        PinnedResult,
    };
    enum class OcrPresentationMode {
        BackgroundOnly,
        BackgroundAndText,
    };

    explicit ScreenshotCanvasRenderer(SnowCanvasWidget& canvas);
    ~ScreenshotCanvasRenderer() override;

    void setRenderMode(RenderMode mode);
    void setImage(QImage image, const QRectF& canvasRect);
    // Sampling can use captures outside this overlay's background viewport.
    void setImageSource(ScreenshotImageSource source, const QRectF& damage = {},
                        const ScreenshotImageSource& samplingSource = {});
    // Pinned animation bypasses canvas reconstruction and annotation caches.
    void setPinnedAnimationFrame(const QImage& image, const QRectF& canvasRect);
    // Live pixels retain their capture orientation. The canvas rectangle is the
    // displayed extent after imageTransform; painting applies it without a new raster.
    void setPinnedLiveFrame(ScreenshotImageSource source, const QTransform& imageTransform = {});
    [[nodiscard]] ScreenshotImageSource imageSourceSnapshot() const {
        return m_imageSource;
    }
    void setScrollingResultPreview(QImage image, const QRectF& canvasRect,
                                   std::optional<Qt::Orientation> cropGuide = std::nullopt);
    void clearScrollingResultPreview();
    [[nodiscard]] bool hasScrollingResultPreview() const;
    void setImageViewportPhysicalSize(const QSize& size);
    // View-space bounds of a pinned window's content, excluding its permanent shadow margin.
    // Fractional coordinates are intentional: 16 Windows device pixels need not be integral dp.
    void setPinnedViewportRect(const QRectF& rect);
    void setPinnedContentClip(std::optional<QRectF> rect);
    void setPinnedResultSurface(const QRectF& contentCanvasRect, const QRectF& surfaceCanvasRect,
                                const ScreenshotResultStyle& style);
    qsizetype selectionOutlineCacheBytes() const {
        return m_outlineCache.bytes();
    }
    qsizetype selectionMaskCacheBytes() const {
        return m_maskCache.bytes() + m_selectionCoverage.mask.sizeInBytes() +
               m_selectionCoverage.tiles.totalCost();
    }
    qsizetype selectionBackgroundCacheBytes() const {
        return m_selectionBackgroundTiles.totalCost();
    }
    qsizetype selectionRegionHoverCacheBytes() const {
        return static_cast<qsizetype>(m_regionShadowRenderer.retainedBytes());
    }
    void setPinnedCheckerboardEnabled(bool enabled);
    [[nodiscard]] bool pinnedCheckerboardEnabled() const {
        return m_pinnedCheckerboardEnabled;
    }
    void setPinnedBackgroundColor(const QColor& color);
    void setMaskVisible(bool visible);
    void setSelectionBorderColor(const QColor& color);
    void setMaskColor(const QColor& color);
    void setGuideLines(const QPointF& cursorPosition, const QColor& cursorColor,
                       const QColor& monitorCenterColor);
    void setGuideCursorPosition(const QPointF& cursorPosition);
    void setSelectionCenterGuideLineColor(const QColor& color);
    void clearGuideLines();
    void setSelection(const QRectF& selection, bool handlesVisible = true, int cornerRadius = 0,
                      int shadowWidth = 0, const QColor& shadowColor = QColor(0x33, 0x33, 0x33));
    void setSelectionRegion(const ScreenshotRegionGeometry& region,
                            const ScreenshotRegionGeometry& confirmed, const QRectF& marquee,
                            bool subtracting, const QColor& danger);
    void setSelectionDraft(const QPainterPath& path, const QVector<QPointF>& vertices);
    void applySelectionState(const ScreenshotSelectionVisualState& state);
    void setSelectionToolbarHovered(bool hovered);
    void setSelectionBorderVisible(bool visible);
    void clearSelection();
    void setOcrPresentation(std::shared_ptr<ScreenshotOcrPresentation> presentation,
                            OcrPresentationMode mode = OcrPresentationMode::BackgroundAndText);
    // Suppress rendering without discarding the latest presentation or filtered image.
    void setOcrVisible(bool visible);
    void setOcrFilteredImage(QImage image, const QRectF& canvasRect);
    void clearOcrFilteredImage();
    [[nodiscard]] QImage ocrFilteredImage() const {
        return m_ocrFilteredImage;
    }
    [[nodiscard]] QRectF ocrFilteredCanvasRect() const {
        return m_ocrFilteredCanvasRect;
    }
    [[nodiscard]] ScreenshotOcrTextPosition ocrTextPositionAt(const QPointF& canvasPosition,
                                                              bool useClosestLine = false) const;
    void updateOcrSelection();
    void clearOcrPresentation();
    void reset();
    void clearRenderState() override;

    [[nodiscard]] std::uint64_t contentRevision() const override;
    [[nodiscard]] std::uint64_t originalBackgroundRevision() const override;
    [[nodiscard]] QList<SnowCanvasBaseImageSource> baseImageSources() const override;
    [[nodiscard]] std::optional<SnowCanvasFilterRenderReference>
    filterRenderReference() const override;
    [[nodiscard]] RenderMode renderMode() const;
    [[nodiscard]] bool maskVisible() const;
    [[nodiscard]] QColor maskColor() const;
    [[nodiscard]] bool guideLinesVisible() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] bool selectionHandlesVisible() const;
    [[nodiscard]] int selectionCornerRadius() const;
    [[nodiscard]] int selectionEffectWidth() const;
    [[nodiscard]] bool selectionToolbarHovered() const;
    [[nodiscard]] bool selectionBorderVisible() const;
    [[nodiscard]] QRectF selection() const;
    // True when the next paint will Source-fill or blit screenshot content over
    // every pixel of widgetRect, so a parent translucent clear is redundant.
    [[nodiscard]] bool coversWidgetRect(const QRect& widgetRect) const override;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 ocrGeometrySynchronizationCountForTesting() const;
#endif

    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;
    void renderOriginalBackground(QPainter& painter,
                                  const SnowCanvasRenderContext& context) override;
    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;

  private:
    struct SelectionViewGeometry {
        bool valid = false;
        QTransform canvasToViewTransform;
        QRect viewportRect;
        int cornerRadius = 0;
        int borderCornerRadius = 0;
        QRectF effectiveRect;
        QRectF outlineRect;
        bool rectangular = false;
        QPainterPath effectivePath;
        QPainterPath outlinePath;
        QPainterPath draftPath;
        QPainterPath maskPath;
        bool maskCacheEligible = false;
        bool sharedDraftOutline = false;
    };
    // Coverage belongs to the effective path, independently of outline/draft geometry.
    struct SelectionCoverageCache {
        enum class TileKind { Empty, Interior, Boundary };
        QPainterPath path;
        QPainterPath devicePath;
        QTransform transform;
        QRect pixels;
        QImage mask;
        QCache<QPoint, QImage> tiles{16 * 1024 * 1024};
        QHash<QPoint, TileKind> tileKinds;
        void clear() {
            path = {};
            devicePath = {};
            transform = {};
            pixels = {};
            mask = {};
            tiles.clear();
            tileKinds.clear();
        }
    };
    struct PathRasterCache {
        struct Entry {
            QPainterPath path;
            QPainterPath viewPath;
            QPointF origin;
            QColor color;
            qreal scale = 0;
            QImage image;
        };
        std::array<Entry, 2> entries;
        qsizetype bytes() const {
            return entries[0].image.sizeInBytes() + entries[1].image.sizeInBytes();
        }
        bool draw(QPainter& painter, const QPainterPath& path, const QColor& color, bool exterior,
                  const QRect& viewport);
    };
    void invalidateCachedContent(bool originalChanged = true);
    void refreshBaseImageSources();
    void paintSelectionBackground(QPainter& painter, const SnowCanvasRenderContext& context,
                                  const QRectF& bounds);
    void paintSelectionPreview(QPainter& painter, const SnowCanvasRenderContext& context);
    void maskSelectionPreview(QPainter& painter, const SnowCanvasRenderContext& context,
                              const QRectF& bounds);
    void paintBackground(QPainter& painter, const SnowCanvasRenderContext& context,
                         bool originalOnly);
    void paintPinnedRegionEffects(QPainter& painter, const SnowCanvasRenderContext& context);
    [[nodiscard]] QRectF pinnedViewportForContext(const SnowCanvasRenderContext& context) const;
    SelectionViewGeometry& selectionViewGeometry(const SnowCanvasRenderContext& context,
                                                 int cornerRadius, int borderCornerRadius);
    [[nodiscard]] ScreenshotOcrTextLayer* ensureOcrTextLayer();
    // Widget-space repaint region for an image canvas rect; empty when the
    // rect maps outside the viewport, the full viewport when the display cache is
    // unsynchronized.
    [[nodiscard]] QRegion canvasImageDamageRegion(const QRectF& canvasRect) const;

    SnowCanvasWidget& m_canvas;
    QPointer<SnowCanvasWidget> m_canvasGuard;
    QMetaObject::Connection m_themeConnection;
    std::uint64_t m_contentRevision = 0;
    std::uint64_t m_originalBackgroundRevision = 0;
    ScreenshotImageSource m_imageSource;
    ScreenshotImageSource m_samplingImageSource;
    bool m_pinnedLiveFrames = false;
    QTransform m_pinnedLiveTransform;
    QList<SnowCanvasBaseImageSource> m_baseImageSources;
    // Source opacity is checked once per image, independently of moving selection damage.
    QList<QRectF> m_opaqueImageCanvasRects;
    mutable QRegion m_opaqueImageViewCoverage;
    mutable QTransform m_opaqueCoverageTransform;
    mutable QRect m_opaqueCoverageViewport;
    mutable bool m_opaqueCoverageValid = false;
    QImage m_scrollingResultPreviewImage;
    QRectF m_scrollingResultPreviewCanvasRect;
    std::optional<Qt::Orientation> m_scrollingCropGuide;
    QSize m_imageViewportPhysicalSize;
    std::optional<QRectF> m_pinnedViewportRect;
    std::optional<QRectF> m_pinnedContentClip;
    QRectF m_pinnedContentCanvasRect;
    QRectF m_pinnedSurfaceCanvasRect;
    ScreenshotResultStyle m_pinnedResultStyle;
    QColor m_pinnedBackgroundColor;
    PathRasterCache m_outlineCache;
    PathRasterCache m_maskCache;
    ScreenshotRegionShadowRenderer m_regionShadowRenderer;
    SelectionCoverageCache m_selectionCoverage;
    QCache<QPoint, QImage> m_selectionBackgroundTiles{64 * 1024 * 1024};
    QRect m_selectionBackgroundCacheViewport;
    qreal m_selectionBackgroundCacheDpr = 0;
    QTransform m_selectionBackgroundCacheTransform;
    QPainterPath m_selectionBackgroundCachePath;
    QPainterPath m_selectionBackgroundDevicePath;
    QHash<QPoint, bool> m_selectionBackgroundInteriorTiles;
    bool m_pinnedCheckerboardEnabled = false;
    ScreenshotSelectionVisualState m_selectionState;
    SelectionViewGeometry m_selectionViewGeometry;
    RenderMode m_renderMode = RenderMode::Standard;
    bool m_maskVisible = false;
    QColor m_selectionBorderColor = QColor(0x40, 0x96, 0xff);
    QColor m_maskColor = QColor(0, 0, 0, 128);
    QPoint m_guideLineCursorPosition;
    QColor m_cursorGuideLineColor = QColor(0, 0, 0, 0);
    QColor m_selectionCenterGuideLineColor = QColor(0, 0, 0, 0);
    QColor m_monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    bool m_guideLinesVisible = false;
    bool m_ocrVisible = true;
    std::shared_ptr<ScreenshotOcrPresentation> m_ocrPresentation;
    QImage m_ocrFilteredImage;
    QRectF m_ocrFilteredCanvasRect;
    QColor m_ocrBackgroundColor;
    OcrPresentationMode m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    // The canvas owns this widget through QObject parenting.
    QPointer<ScreenshotOcrTextLayer> m_ocrTextLayer;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H
