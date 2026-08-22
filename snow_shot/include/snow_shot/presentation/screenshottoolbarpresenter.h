#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARPRESENTER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARPRESENTER_H

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>

struct CapturedDisplayModel;
class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotOverlayCoordinator;

struct ScreenshotToolbarPresentationState {
    QRect selectionPixels;
    QRectF selectionCanvas;
    bool inactive = true;
    bool selectionToolbarMode = false;
    bool intelligentSelecting = false;
    bool editing = false;
    bool ocrAvailable = true;
    bool aspectRatioLocked = false;
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
};

class ScreenshotToolbarPresenter final {
  public:
    ScreenshotToolbarPresenter(ScreenshotOverlayCoordinator& overlayCoordinator,
                               const ScreenshotGeometryMapper& geometry,
                               const ScreenshotDisplaySession& displaySession);

    void hideToolbar();
    void hideMainToolbar();
    void showToolbar(const ScreenshotToolbarPresentationState& state);
    void hideSelectionToolbar();
    void showSelectionToolbar(const ScreenshotToolbarPresentationState& state);
    void repositionForContentChange(const ScreenshotToolbarPresentationState& state);
    void updateSelectionToolbarState(const ScreenshotToolbarPresentationState& state);
    void raiseToolbarForCanvasInteraction(const ScreenshotToolbarPresentationState& state);
    void moveToolbar(const ScreenshotToolbarPresentationState& state);
    [[nodiscard]] bool moveSelectionToolbar(const ScreenshotToolbarPresentationState& state);

  private:
    [[nodiscard]] const CapturedDisplayModel* displayForCanvasPoint(const QPointF& point) const;
    [[nodiscard]] const CapturedDisplayModel* displayForCanvasRect(const QRectF& rect) const;
    [[nodiscard]] QPoint logicalPositionForCanvasPoint(const CapturedDisplayModel& display,
                                                       const QPointF& point) const;

    ScreenshotOverlayCoordinator& m_overlayCoordinator;
    const ScreenshotGeometryMapper& m_geometry;
    const ScreenshotDisplaySession& m_displaySession;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARPRESENTER_H
