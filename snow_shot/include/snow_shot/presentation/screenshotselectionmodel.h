#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H

#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectionparams.h"

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QtGlobal>

class ScreenshotSelectionModel final {
  public:
    void reset();

    [[nodiscard]] QRectF normalizedSelection() const;
    [[nodiscard]] QRect pixelSelection() const;
    [[nodiscard]] bool hasPixelSelection() const;

    void clearSelection();
    void setSelectionRect(const QRectF& selection);
    void setSelectionStartEnd(const QPointF& start, const QPointF& end);
    void setSelectionStart(const QPointF& start);
    void setSelectionEnd(const QPointF& end);

    void beginMoveDrag(const QPointF& startPosition);
    // Rebase a move/resize gesture without changing the visible selection.
    // This is used when a transient modifier switches an active resize into
    // moving the whole selection, avoiding a cursor jump.
    void rebaseMoveDrag(const QPointF& startPosition);
    [[nodiscard]] QRectF moveOriginalSelection() const;
    [[nodiscard]] QPointF moveStart() const;
    [[nodiscard]] QRectF selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                              const QPointF& position, const QRectF& bounds,
                                              qreal minimumSelectionSize,
                                              qreal lockedAspectRatioOverride = -1.0) const;

    [[nodiscard]] QRectF boundedSelectionRect(const QRectF& selection, const QRectF& bounds,
                                              bool preserveSize, qreal minimumSelectionSize) const;
    [[nodiscard]] bool adjustFromToolbar(int minDx, int minDy, int maxDx, int maxDy,
                                         const QRectF& bounds, qreal minimumSelectionSize);

    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] int shadowWidth() const;
    [[nodiscard]] QColor shadowColor() const;
    [[nodiscard]] bool aspectRatioLocked() const;

    [[nodiscard]] bool setCornerRadius(int radius);
    [[nodiscard]] bool setShadowWidth(int shadowWidth);
    void setShadowColor(const QColor& color);
    void toggleAspectRatioLock(qreal minimumSelectionSize);

    [[nodiscard]] ScreenshotSelectionParams params(const QRect& bounds) const;
    [[nodiscard]] bool applyParams(const ScreenshotSelectionParams& params, const QRect& bounds);

  private:
    QPointF m_start;
    QPointF m_end;
    QPointF m_moveStart;
    QRectF m_moveOriginalSelection;
    int m_cornerRadius = 0;
    int m_shadowWidth = 0;
    QColor m_shadowColor = QColor(0x33, 0x33, 0x33);
    double m_lockedAspectRatio = 0.0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
