#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include "snow_shot/presentation/mousereleaseactioncontroller.h"

#include <QColor>
#include <QJsonObject>
#include <QRectF>
#include <QRegion>
#include "snow_shot/image/screenshotregiongeometry.h"
#include <QWidget>

#include <memory>
#include <optional>

class QEvent;
class CanvasStatusReadout;
class QImage;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QRectF;
class QResizeEvent;
class QWheelEvent;
class SnowCanvasWidget;
class ScreenshotCanvasRenderer;
class ScreenshotOcrPresentation;
class ScreenshotOverlayEventSink;
class ScreenshotOverlayFramePresenter;
class ScreenshotScrollingThumbnailWidget;
class ScreenshotRegionTypeControl;
struct ScreenshotSelectionVisualState;

struct ScreenshotImageSource;

class ScreenshotOverlayWindow final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotOverlayWindow(ScreenshotOverlayEventSink& eventSink,
                                     SnowCanvasWidget* canvas, QWidget* parent = nullptr);
    ~ScreenshotOverlayWindow() override;

    SnowCanvasWidget* canvas() const;
    // Display coordinates describe the canvas, independently of native frame padding.
    void setCaptureGeometry(const QRect& displayGeometry);
    [[nodiscard]] QRect captureGeometry() const;
    [[nodiscard]] QPoint canvasLocalPosition(const QPoint& globalPosition) const;
    void setScreenshotImage(QImage image, const QRectF& canvasRect);
    void setScreenshotImageSource(ScreenshotImageSource source, const QRectF& damage = {});
    void setScreenshotMaskVisible(bool visible);
    void setScreenshotSelectionBorderColor(const QColor& color);
    void setScreenshotMaskColor(const QColor& color);
    void setScreenshotGuideLines(const QPointF& cursorPosition, const QColor& cursorColor,
                                 const QColor& monitorCenterColor);
    void setSelectionCenterGuideLineColor(const QColor& color);
    [[nodiscard]] QRectF screenshotSelection() const;
    void clearScreenshotGuideLines();
    void setScreenshotSelection(const QRectF& selection, bool handlesVisible, int cornerRadius,
                                int shadowWidth = 0,
                                const QColor& shadowColor = QColor(0x33, 0x33, 0x33),
                                bool selectionToolbarHovered = false);
    void setScreenshotSelectionState(const ScreenshotSelectionVisualState& state);
    void setRegionTypeControlVisible(bool visible, ScreenshotRegionType type,
                                     const QRectF& selectionGlobal = {},
                                     const QPointF& cursorGlobal = {});
    void setSelectionDraft(const QPainterPath& path, const QVector<QPointF>& vertices);
    void clearScreenshotSelection();
    void setScreenshotSelectionRegion(const ScreenshotRegionGeometry& region,
                                      const ScreenshotRegionGeometry& confirmed,
                                      const QRectF& marquee, bool subtracting,
                                      const QColor& danger);
    [[nodiscard]] bool hasScreenshotSelection() const;
    [[nodiscard]] bool screenshotSelectionHandlesVisible() const;
    void setScreenshotSelectionBorderVisible(bool visible);
    [[nodiscard]] bool screenshotSelectionBorderVisible() const;
    void setScreenshotOcrBackground(std::shared_ptr<ScreenshotOcrPresentation> presentation);
    void setScreenshotOcrFilteredImage(QImage image, const QRectF& canvasRect);
    void clearScreenshotOcrBackground();
    void setScreenshotOcrVisible(bool visible);
    void setHistoryLoadingVisible(bool visible);
    void resetScreenshotRendering();
    void commitInitialSelectionCursor();
    void setCanvasClearBackgroundEnabled(bool enabled);
    [[nodiscard]] QJsonObject scrollingDiagnostics() const;
    void setScrollingVisualHole(const QRect& localRect);
    void clearScrollingVisualHole();
    void setScrollingCaptureMode(bool enabled);
    void beginScrollingThumbnail(
        const QRect& localSelection,
        ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical,
        const QSize& captureViewportSize = {});
    void updateScrollingThumbnail(const QImage& previewImage, const QSize& sourceSize,
                                  ScreenshotScrollingStitchChange change, int addedRows,
                                  bool replacePreview = false, int replacedPreviewRows = 0);
    void reanchorScrollingThumbnail(const QRect& localSelection);
    void clearScrollingThumbnail();
    [[nodiscard]] QWidget* scrollingThumbnailWindow() const;
    [[nodiscard]] ScreenshotScrollingTrimRange scrollingThumbnailTrim() const;
    void setScrollingTrimModel(std::shared_ptr<ScreenshotScrollingTrimRange> trim);
    void setScrollingResultPreview(const QImage& image, const QRectF& canvasRect,
                                   bool showStatus = true,
                                   std::optional<Qt::Orientation> cropGuide = std::nullopt);
    // Repaint the cleared surface before the controller resumes capture.
    void clearScrollingResultPreview();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 windowMaskApplicationCountForTesting() const;
    [[nodiscard]] quint64 transparentClearCountForTesting() const;
    [[nodiscard]] ScreenshotCanvasRenderer* screenshotRendererForTesting() const;
#endif
    void warmPresentationSurface();
    void showPreparedFrame(bool deferFirstPaint = false);
    // Release the native window and backing store while retaining the QObject,
    // canvas, renderer, and signal wiring for the next capture.
    void releaseNativeSurface();
    // Recreate the native window and backing store after releaseNativeSurface().
    void restoreNativeSurface();

  signals:
    void scrollingThumbnailHoverChanged(const QRect& sourceRect, bool cropping);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    bool handleCanvasEvent(QEvent* event);
    bool handleCanvasKeyPress(QKeyEvent* event);
    bool handleCanvasMouseEvent(QMouseEvent* event);
    bool handleCanvasWheel(QWheelEvent* event);
    bool dispatchHandledMouseEvent(QMouseEvent* event);
    void initializeScreenshotSurface();
    void layoutRegionTypeControl();
    void layoutScrollingThumbnail();
    void updateWindowMask();
    void updateScrollingInputTransparency();
    void updateScrollingResultPreviewReadout();

    ScreenshotOverlayEventSink& m_eventSink;
    QMargins m_captureFrameMargins;
    snow_shot::presentation::MouseReleaseActionController m_mouseReleaseAction;
    SnowCanvasWidget* m_canvas = nullptr;
    ScreenshotRegionTypeControl* m_regionTypeControl = nullptr;
    ScreenshotScrollingThumbnailWidget* m_scrollingThumbnail = nullptr;
    CanvasStatusReadout* m_scrollingResultPreviewReadout = nullptr;
    std::unique_ptr<ScreenshotOverlayFramePresenter> m_framePresenter;
    std::unique_ptr<ScreenshotCanvasRenderer> m_screenshotRenderer;
    QRect m_scrollingVisualHole;
    QRegion m_appliedWindowMask;
    QRect m_scrollingThumbnailAnchor;
    QRectF m_scrollingResultPreviewCanvasRect;
    bool m_scrollingResultPreviewStatusVisible = false;
    ScreenshotScrollingRecognitionMode m_scrollingThumbnailMode =
        ScreenshotScrollingRecognitionMode::Vertical;
    bool m_scrollingCaptureMode = false;
    bool m_canvasContentWasVisible = true;
    bool m_canvasClearBackgroundWasEnabled = true;
    bool m_canvasInteractionWasEnabled = true;
    bool m_windowMaskInitialized = false;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_windowMaskApplicationCount = 0;
    quint64 m_transparentClearCount = 0;
#endif
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H
