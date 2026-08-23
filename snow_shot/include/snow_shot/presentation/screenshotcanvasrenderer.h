#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H

#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointer>
#include <QRectF>
#include <QRegion>
#include <QSize>
#include <QTransform>

#include <cstddef>
#include <memory>

class SnowCanvasWidget;
class ScreenshotOcrPresentation;
class ScreenshotOcrTextLayer;
struct ScreenshotOcrTextPosition;

struct ScreenshotSelectionVisualState {
    QRectF bounds;
    bool present = false;
    bool handlesVisible = true;
    bool borderVisible = true;
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    bool toolbarHovered = false;

    [[nodiscard]] bool operator==(const ScreenshotSelectionVisualState& other) const {
        return bounds == other.bounds && present == other.present &&
               handlesVisible == other.handlesVisible && borderVisible == other.borderVisible &&
               cornerRadius == other.cornerRadius && shadowWidth == other.shadowWidth &&
               shadowColor == other.shadowColor && toolbarHovered == other.toolbarHovered;
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
    void setImageSource(ScreenshotImageSource source);
    void setImageViewportPhysicalSize(const QSize& size);
    void setPinnedResultSurface(const QRectF& contentCanvasRect,
                                const QRectF& surfaceCanvasRect,
                                const ScreenshotResultStyle& style);
    void clearImage();
    void setMaskVisible(bool visible);
    void setMaskColor(const QColor& color);
    void setGuideLines(const QPointF& cursorPosition, const QColor& cursorColor,
                       const QColor& monitorCenterColor);
    void clearGuideLines();
    void setSelection(const QRectF& selection, bool handlesVisible = true, int cornerRadius = 0,
                      int shadowWidth = 0, const QColor& shadowColor = QColor(0x33, 0x33, 0x33));
    void applySelectionState(const ScreenshotSelectionVisualState& state);
    void setSelectionToolbarHovered(bool hovered);
    void setSelectionBorderVisible(bool visible);
    void clearSelection();
    void setOcrPresentation(
        std::shared_ptr<ScreenshotOcrPresentation> presentation,
        OcrPresentationMode mode = OcrPresentationMode::BackgroundAndText);
    [[nodiscard]] ScreenshotOcrTextPosition ocrTextPositionAt(const QPointF& canvasPosition,
                                                              bool useClosestLine = false) const;
    void updateOcrSelection();
    void clearOcrPresentation();
    void reset();

    [[nodiscard]] std::uint64_t contentRevision() const override;
    [[nodiscard]] RenderMode renderMode() const;
    [[nodiscard]] bool maskVisible() const;
    [[nodiscard]] QColor maskColor() const;
    [[nodiscard]] bool guideLinesVisible() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] bool selectionHandlesVisible() const;
    [[nodiscard]] int selectionCornerRadius() const;
    [[nodiscard]] int selectionShadowWidth() const;
    [[nodiscard]] bool selectionToolbarHovered() const;
    [[nodiscard]] bool selectionBorderVisible() const;
    [[nodiscard]] QRectF selection() const;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 ocrGeometrySynchronizationCountForTesting() const;
#endif

    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;
    void renderAfterCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override;

  private:
    void invalidateCachedContent();
    [[nodiscard]] ScreenshotOcrTextLayer* ensureOcrTextLayer();

    SnowCanvasWidget& m_canvas;
    std::uint64_t m_contentRevision = 0;
    ScreenshotImageSource m_imageSource;
    QSize m_imageViewportPhysicalSize;
    QRectF m_pinnedContentCanvasRect;
    QRectF m_pinnedSurfaceCanvasRect;
    ScreenshotResultStyle m_pinnedResultStyle;
    ScreenshotSelectionVisualState m_selectionState;
    RenderMode m_renderMode = RenderMode::Standard;
    bool m_maskVisible = false;
    QColor m_maskColor = QColor(0, 0, 0, 128);
    QPoint m_guideLineCursorPosition;
    QColor m_cursorGuideLineColor = QColor(0, 0, 0, 0);
    QColor m_monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    bool m_guideLinesVisible = false;
    std::shared_ptr<ScreenshotOcrPresentation> m_ocrPresentation;
    OcrPresentationMode m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    // The canvas owns this widget through QObject parenting.
    QPointer<ScreenshotOcrTextLayer> m_ocrTextLayer;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASRENDERER_H
