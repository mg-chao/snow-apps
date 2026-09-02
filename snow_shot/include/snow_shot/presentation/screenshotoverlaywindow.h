#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QColor>
#include <QRegion>
#include <QWidget>

#include <memory>

class QEvent;
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

class ScreenshotOverlayWindow final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotOverlayWindow(ScreenshotOverlayEventSink& eventSink,
                                     SnowCanvasWidget* canvas, QWidget* parent = nullptr);
    ~ScreenshotOverlayWindow() override;

    SnowCanvasWidget* canvas() const;
    void setScreenshotImage(QImage image, const QRectF& canvasRect);
    void setScreenshotMaskVisible(bool visible);
    void setScreenshotMaskColor(const QColor& color);
    void setScreenshotGuideLines(const QPointF& cursorPosition, const QColor& cursorColor,
                                 const QColor& monitorCenterColor);
    void clearScreenshotGuideLines();
    void setScreenshotSelection(const QRectF& selection, bool handlesVisible, int cornerRadius,
                                int shadowWidth = 0,
                                const QColor& shadowColor = QColor(0x33, 0x33, 0x33),
                                bool selectionToolbarHovered = false);
    void clearScreenshotSelection();
    [[nodiscard]] bool hasScreenshotSelection() const;
    [[nodiscard]] bool screenshotSelectionHandlesVisible() const;
    void setScreenshotSelectionBorderVisible(bool visible);
    [[nodiscard]] bool screenshotSelectionBorderVisible() const;
    void setScreenshotOcrBackground(std::shared_ptr<ScreenshotOcrPresentation> presentation);
    void setScreenshotOcrFilteredImage(QImage image, const QRectF& canvasRect);
    void clearScreenshotOcrBackground();
    void setHistoryLoadingVisible(bool visible);
    void resetScreenshotRendering();
    void commitInitialSelectionCursor();
    void setCanvasClearBackgroundEnabled(bool enabled);
    void setInputPassThroughRect(const QRect& localRect);
    void clearInputPassThroughRect();
    void setScrollingCaptureMode(bool enabled);
    void beginScrollingThumbnail(
        const QRect& localSelection,
        ScreenshotScrollingRecognitionMode mode = ScreenshotScrollingRecognitionMode::Vertical);
    void updateScrollingThumbnail(const QImage& previewImage, const QSize& sourceSize,
                                  ScreenshotScrollingStitchChange change, int addedRows,
                                  bool replacePreview = false, int replacedPreviewRows = 0);
    void clearScrollingThumbnail();
    [[nodiscard]] ScreenshotScrollingTrimRange scrollingThumbnailTrim() const;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 windowMaskApplicationCountForTesting() const;
    [[nodiscard]] ScreenshotCanvasRenderer* screenshotRendererForTesting() const;
#endif
    void showPreparedFrame();
    // Release the native window and backing store while retaining the QObject,
    // canvas, renderer, and signal wiring for the next capture.
    void releaseNativeSurface();
    // Recreate the native window and backing store after releaseNativeSurface().
    void restoreNativeSurface();

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
    void layoutScrollingThumbnail();
    void updateWindowMask();

    ScreenshotOverlayEventSink& m_eventSink;
    SnowCanvasWidget* m_canvas = nullptr;
    ScreenshotScrollingThumbnailWidget* m_scrollingThumbnail = nullptr;
    std::unique_ptr<ScreenshotOverlayFramePresenter> m_framePresenter;
    std::unique_ptr<ScreenshotCanvasRenderer> m_screenshotRenderer;
    QRect m_inputPassThroughRect;
    QRegion m_appliedWindowMask;
    QRect m_scrollingThumbnailAnchor;
    ScreenshotScrollingRecognitionMode m_scrollingThumbnailMode =
        ScreenshotScrollingRecognitionMode::Vertical;
    bool m_scrollingThumbnailSessionActive = false;
    bool m_scrollingCaptureMode = false;
    bool m_canvasContentWasVisible = true;
    bool m_canvasClearBackgroundWasEnabled = true;
    bool m_canvasInteractionWasEnabled = true;
    bool m_windowMaskInitialized = false;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_windowMaskApplicationCount = 0;
#endif
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYWINDOW_H
