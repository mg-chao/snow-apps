#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H

#include "snow_shot/presentation/screenshotscrollingtypes.h"

#include <QImage>
#include <QRect>
#include <QWidget>

#include <deque>

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
  public:
    explicit ScreenshotScrollingThumbnailWidget(QWidget& parent);

    void reset();
    void setRecognitionMode(ScreenshotScrollingRecognitionMode mode);
    void setMaximumPreviewHeight(int height);
    void setMaximumPreviewExtent(int extent);
    void setStitchedImage(const QImage& previewImage, const QSize& sourceSize,
                          ScreenshotScrollingStitchChange change, int addedRows,
                          bool replacePreview = false, int replacedPreviewRows = 0);
    [[nodiscard]] int trimTop() const;
    [[nodiscard]] int trimBottom() const;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] QImage previewImageForTesting() const;
    [[nodiscard]] qsizetype previewLogicalBytesForTesting() const;
    [[nodiscard]] qsizetype previewAllocatedBytesForTesting() const;
    [[nodiscard]] QRect highlightedRowsForTesting() const;
#endif

  protected:
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
    };

    [[nodiscard]] bool hasPreview() const;
    [[nodiscard]] QRect previewRect() const;
    [[nodiscard]] qreal imageScale() const;
    [[nodiscard]] int scaledImageExtent() const;
    [[nodiscard]] int sourceExtent() const;
    [[nodiscard]] int previewPosition(const QPointF& position) const;
    [[nodiscard]] int handlePosition(int sourcePosition) const;
    [[nodiscard]] int sourcePositionForPreviewPosition(int position) const;
    [[nodiscard]] bool isTrimHandleAtPosition(int position) const;
    void updateWidgetMetrics();
    void updateScrollBarGeometry();
    void updateCursorForPosition(int position);
    void updateTrimFromPosition(int position);
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
    QRect m_highlightedRows;
    int m_captureImageExtent = 0;
    adqt::widgets::AdScrollBar* m_scrollBar = nullptr;
    int m_maximumPreviewExtent = 640;
    int m_trimTop = 0;
    int m_trimBottom = 0;
    DragHandle m_dragHandle = DragHandle::None;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSCROLLINGTHUMBNAILWIDGET_H
