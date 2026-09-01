#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEHOST_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEHOST_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QWidget>

class QEvent;
class QPaintEvent;
class QWheelEvent;

class ScreenshotToolPaletteHost final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotToolPaletteHost(const ScreenshotToolPalette::Options& options,
                                       QWidget* parent = nullptr);

    static QMargins defaultShadowMargins();

    ScreenshotToolPalette* palette() const;
    QSize contentSizeHint() const;
    QRect occupiedContentRect() const;
    QRect visualContentRect() const;
    QRect fullContentRect() const;
    QRect bottomPlacementContentRect() const;
    QRect topPlacementContentRect() const;
    QRect topRightMainToolbarContentRect() const;
    QRect mainToolbarContentRect() const;
    ScreenshotToolbarPlacementSnapshot placementSnapshot() const;
    QRect occupiedHostRect() const;
    QRegion interactiveHostRegion() const;
    QPoint contentOffset() const;
    quint64 layoutRevision() const;
    QPoint contentPosition() const;
    void moveContentTo(const QPoint& position);
    void prepareForDisplay();
    void resetStyleState();
    void setCreationStyleDefaults(const SnowCanvasStyleDefaults& defaults);
    [[nodiscard]] SnowCanvasStyleDefaults creationStyleDefaults() const;
    bool stepStrokeWidth(int direction);
    bool stepSelectionOpacity(int direction);
    bool stepSpotlightOpacity(int direction);
    bool stepFilterIntensity(int direction);
    bool stepPenFilterStrokeWidth(int direction);
    bool stepWatermarkFontSize(int direction);
    void setShadowMargins(const QMargins& margins);
    void setPhysicalScale(qreal scale);
    qreal physicalScale() const;
    void commitDpiScale(qreal scale, const QMargins& shadowMargins);
    void setFrameSize(const QSize& frameSize, bool anchorToBottom);
    void setStyleToolbarAboveMain(bool above);
    void setStyleToolbarVisible(bool visible);
    void setScrollingScreenshotMode(bool enabled);
    void setActiveTool(ScreenshotToolPalette::Tool tool);
    void clearActiveTool();
    bool handleToolbarWheel(QWheelEvent* event);
    void cancelDrag();

  signals:
    void visibleContentChanged();
    void dragStarted(const QPoint& globalPosition);
    void dragMoved(const QPoint& globalPosition);
    void dragFinished(const QPoint& globalPosition);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

    void handlePaletteVisibleContentChanged();
    void applyHostSize();
    void syncHostSize();
    void syncPalettePosition();
    QMargins currentShadowMargins() const;

    ScreenshotToolPalette* m_palette = nullptr;
    QSize m_frameSize;
    bool m_anchorPaletteToBottom = false;
    bool m_dragging = false;

#if defined(SNOW_SHOT_TEST_HOOKS)
    quint64 m_sizeSynchronizationCount = 0;
#endif
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTEHOST_H
