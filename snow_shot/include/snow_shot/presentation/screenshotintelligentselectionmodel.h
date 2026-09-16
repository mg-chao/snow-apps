#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTINTELLIGENTSELECTIONMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTINTELLIGENTSELECTIONMODEL_H

#include <QPointF>
#include <QRectF>
#include <QVector>

enum class ScreenshotIntelligentSelectionTarget {
    Window,
    WindowSubElement,
};

class ScreenshotIntelligentSelectionModel final {
  public:
    void beginCaptureSession(bool smartSelectionEnabled,
                             ScreenshotIntelligentSelectionTarget preferredTarget =
                                 ScreenshotIntelligentSelectionTarget::WindowSubElement);
    void reset();
    void clearTransientState();
    void clearHitPath();
    void clearPress();
    [[nodiscard]] bool
    updateSmartSelectionEnabled(bool enabled,
                                ScreenshotIntelligentSelectionTarget preferredTarget =
                                    ScreenshotIntelligentSelectionTarget::WindowSubElement);

    [[nodiscard]] bool applyCanvasHitPath(const QVector<QRectF>& canvasHitRects,
                                          const QRectF& selectableBounds,
                                          qreal minimumSelectionSize);
    [[nodiscard]] bool applyCanvasRefinementPath(const QVector<QRectF>& canvasHitRects,
                                                 const QRectF& selectableBounds,
                                                 qreal minimumSelectionSize);
    void resetTargetPreference();
    [[nodiscard]] bool selectIndex(int index);
    [[nodiscard]] bool setIndex(int index);
    [[nodiscard]] bool toggleSelectionTarget();
    [[nodiscard]] bool smartSelectionEnabled() const;
    [[nodiscard]] ScreenshotIntelligentSelectionTarget selectionTarget() const;
    [[nodiscard]] int index() const;
    [[nodiscard]] bool hasCurrentSelection() const;
    [[nodiscard]] QRectF currentSelection() const;

    void beginPress(const QPointF& position, const QRectF& selection);
    [[nodiscard]] bool pressActive() const;
    [[nodiscard]] QPointF pressPosition() const;
    [[nodiscard]] bool shouldStartManualDrag(const QPointF& position,
                                             double dragStartDistance) const;
    [[nodiscard]] QRectF takePressSelection();

  private:
    QVector<QRectF> m_hitRects;
    int m_index = -1;
    ScreenshotIntelligentSelectionTarget m_selectionTarget =
        ScreenshotIntelligentSelectionTarget::Window;
    bool m_smartSelectionEnabled = false;
    bool m_explicitSelection = false;
    bool m_pressActive = false;
    QPointF m_pressPosition;
    QRectF m_pressSelection;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTINTELLIGENTSELECTIONMODEL_H
