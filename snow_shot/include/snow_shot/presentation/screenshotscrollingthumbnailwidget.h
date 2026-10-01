#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QImage>
#include <QRect>
#include <QWidget>

#include <deque>
#include <memory>
#include <optional>

class QMouseEvent;
class QPainter;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QEvent;

namespace adqt::widgets {
class AdScrollBar;
}

class ScreenshotScrollingThumbnailWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotScrollingThumbnailWidget(QWidget& parent);

    void reset();
    void clearHover();
    void releaseNativeSurface();
    void setTrimModel(std::shared_ptr<ScreenshotScrollingTrimRange> trim);
    void setRecognitionMode(ScreenshotScrollingRecognitionMode mode);
    void setMaximumPreviewHeight(int height);
    void setMaximumPreviewExtent(int extent);
    void setCaptureViewportSize(const QSize& size);
    void setStitchedImage(const QImage& previewImage, const QSize& sourceSize,
                          ScreenshotScrollingStitchChange change, int addedRows,
                          bool replacePreview = false, int replacedPreviewRows = 0);
    [[nodiscard]] bool hasPreview() const;
    [[nodiscard]] int trimTop() const;
    [[nodiscard]] int trimBottom() const;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] QImage previewImageForTesting() const;
    [[nodiscard]] qsizetype previewLogicalBytesForTesting() const;
    [[nodiscard]] qsizetype previewAllocatedBytesForTesting() const;
    [[nodiscard]] QRect highlightedRowsForTesting() const;
    [[nodiscard]] QRectF hoverPreviewRectForTesting() const;
    [[nodiscard]] QRect hoverSourceRectForTesting() const;
#endif

  signals:
    void hoverSourceRectChanged(const QRect& sourceRect, bool cropping);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    enum class DragHandle {
        None,
        Head,
        Tail,
    };

    enum class TileDirection {
        None,
        Append,
        Prepend,
    };

    struct PreviewTile {
        QImage image;
        int firstSpan = 0;
        int spanCount = 0;
        qint64 firstPosition = 0;
    };

    [[nodiscard]] QRect previewRect() const;
    [[nodiscard]] qreal imageScale() const;
    [[nodiscard]] QRectF imageTargetRect() const;
    [[nodiscard]] int scaledImageExtent() const;
    [[nodiscard]] int sourceExtent() const;
    [[nodiscard]] int previewPosition(const QPointF& position) const;
    [[nodiscard]] int handlePosition(int sourcePosition) const;
    [[nodiscard]] int sourcePositionForPreviewPosition(int position) const;
    [[nodiscard]] bool isTrimHandleAtPosition(int position) const;
    void updateWidgetMetrics(bool refreshHover = true);
    void updateScrollBarGeometry();
    void createScrollBar();
    void updateCursorForPosition(int position);
    void updateTrimFromPosition(int position);
    void cancelDrag();
    void updateHover();
    void setHoverRect(const QRectF& preview, const QRect& source, bool cropping = false);
    void drawTrimHandle(QPainter& painter, int position, bool head) const;
    void replacePreview(const QImage& image);
    void discardPreviewBack(int rows);
    void discardPreviewFront(int rows);
    void appendPreview(const QImage& image);
    void prependPreview(const QImage& image);
    void prepareTileDirection(TileDirection direction);
    void compactActiveTile();
    void drawPreviewTiles(QPainter& painter, const QRectF& imageTarget,
                          const QRectF& visibleRect) const;

    std::deque<PreviewTile> m_previewTiles;
    int m_previewExtent = 0;
    TileDirection m_tileDirection = TileDirection::None;
    ScreenshotScrollingRecognitionMode m_mode = ScreenshotScrollingRecognitionMode::Vertical;
    QSize m_sourceSize;
    QSize m_captureViewportSize;
    bool m_captureViewportSizeConfigured = false;
    bool m_updatingMetrics = false;
    QRect m_highlightedRows;
    int m_captureImageExtent = 0;
    adqt::widgets::AdScrollBar* m_scrollBar = nullptr;
    int m_maximumPreviewExtent = 640;
    std::shared_ptr<ScreenshotScrollingTrimRange> m_trim =
        std::make_shared<ScreenshotScrollingTrimRange>();
    DragHandle m_dragHandle = DragHandle::None;
    std::optional<QPointF> m_hoverPosition;
    QRectF m_hoverPreviewRect;
    QRect m_hoverSourceRect;
    bool m_cropPreviewActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H
