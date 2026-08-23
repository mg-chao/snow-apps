#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <functional>

struct ScreenshotCaptureState;
class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotSelectionModel;

struct ScreenshotSelectorPresentationCallbacks {
    std::function<void()> updateOverlayState;
    std::function<void()> updateColorPicker;
    std::function<void()> hideToolbar;
    std::function<void()> updateOverlayCursors;
    std::function<bool(quint64 sessionId, const QPoint& physicalPoint, bool ok,
                       const QVector<QRectF>& hitRects)>
        consumeInitialSmartSelectionResult;
};

struct ScreenshotSelectorWorkflowContext {
    ScreenshotCaptureState& captureState;
    ScreenshotSelectorServicePort& selectorService;
    ScreenshotOverlayExclusionPort& overlayExclusions;
    ScreenshotDisplaySession& displaySession;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
    ScreenshotSelectorPresentationCallbacks presentation;
};

class ScreenshotSelectorWorkflow final {
  public:
    explicit ScreenshotSelectorWorkflow(ScreenshotSelectorWorkflowContext context);

    void startRefresh();
    void handleRefreshFinished(bool ok);
    [[nodiscard]] QVector<std::uintptr_t> excludedHwnds() const;

    [[nodiscard]] bool updateSelectionAt(const QPoint& physicalPoint);
    [[nodiscard]] bool requestHitTest(const QPoint& physicalPoint);
    void startNextHitTest();
    void handleHitTestFinished(bool ok, const QPoint& physicalPoint,
                               const QVector<QRectF>& hitRects);

    [[nodiscard]] bool applyHitPath(const QVector<QRectF>& hitRects);
    void setSelectionIndex(int index);
    void clearSelection();
    [[nodiscard]] QRectF currentSelection() const;
    [[nodiscard]] bool returnToSelection(const QPoint& physicalPoint);

  private:
    ScreenshotSelectorWorkflowContext m_context;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H
