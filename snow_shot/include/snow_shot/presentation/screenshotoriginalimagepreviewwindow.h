#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTORIGINALIMAGEPREVIEWWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTORIGINALIMAGEPREVIEWWINDOW_H

#include <QImage>
#include <QColor>
#include <QPointer>
#include <QRectF>
#include <QWidget>

#include <memory>

namespace adqt::widgets {
class AdDpiStableWindowController;
}

class ScreenshotOriginalImagePreviewWindowTestAccess;

// Geometry is in desktop physical pixels on the native Windows platform and logical pixels
// elsewhere. The image is shared through QImage's implicit sharing; its rectangle describes the
// original image in the recognition viewport, including any pan, zoom, and letterboxing.
struct ScreenshotOriginalImagePreviewState final {
    QImage image;
    QRectF imageRectInViewport;
    QRect resultRect;
    QWidget* transientOwner = nullptr;
    bool pinned = false;
    bool staysOnTop = true;
    QWidget* aboveSibling = nullptr;
    // Formula previews share the native companion surface, including placeholder/error states.
    bool formula = false;
    bool dimmed = false;
    QColor background;
    QColor statusColor;
    QString status;
};

class ScreenshotOriginalImagePreviewWindow final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotOriginalImagePreviewWindow(QWidget* parent = nullptr);
    ~ScreenshotOriginalImagePreviewWindow() override;

    [[nodiscard]] bool present(const ScreenshotOriginalImagePreviewState& state);
    void clear();
    void refreshStacking();

    [[nodiscard]] static QRect placement(const QRect& resultRect, const QRect& workArea, int gap);
    [[nodiscard]] static QRect nativeClientRect(QWidget* widget);
    [[nodiscard]] static bool usesPhysicalGeometry();

  signals:
    // Presentation can briefly unmap the window while applying native flags.
    void hidden();

  protected:
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

  private:
    friend class ScreenshotOriginalImagePreviewWindowTestAccess;
    struct NativeSurface;
    void scheduleRefresh();
    [[nodiscard]] bool refreshRaster();
    [[nodiscard]] bool applyGeometry(const QRect& target, QScreen* screen);
    void reconcileNativeGeometry();
    [[nodiscard]] bool beginDrag(const QPointF& cursor);
    void moveDrag(const QPointF& cursor);
    void endDrag();

    QImage m_sourceImage;
    QRectF m_imageRectInViewport;
    QRect m_resultRect;
    QRect m_targetGeometry;
    QPointF m_dragOffset;
    QPointer<QWidget> m_transientOwner;
    QPointer<QWidget> m_aboveSibling;
    QImage m_viewportImage;
    std::shared_ptr<NativeSurface> m_nativeSurface;
    qint64 m_cachedSourceKey = 0;
    QRectF m_cachedImageRect;
    QSize m_cachedPixelSize;
    qreal m_cachedOutputDpr = 0;
    quint64 m_rasterGeneration = 0;
    adqt::widgets::AdDpiStableWindowController* m_dpiController = nullptr;
    bool m_pinned = false;
    bool m_staysOnTop = true;
    bool m_refreshPending = false;
    bool m_presenting = false;
    bool m_applyingGeometry = false;
    bool m_adjustingPaintSurface = false;
    bool m_forwardingWindowEvent = false;
    bool m_siblingStackedAbove = false;
    bool m_dragging = false;
    bool m_userPositioned = false;
    bool m_formula = false;
    bool m_dimmed = false;
    QColor m_background;
    QColor m_statusColor;
    QString m_status;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTORIGINALIMAGEPREVIEWWINDOW_H
