#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

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
    std::function<void(quint64 sessionId)> smartSelectionResultReady;
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
    void handleInitialResult(bool ok, const QVector<QRectF>& hitRects, quint32 displayId = 0);
    void handleRefinement(const QVector<QRectF>& hitRects, quint32 displayId = 0,
                          bool replacePath = false);
    void handleTargetChanged();

    void applyHitPath(const QVector<QRectF>& hitRects, quint32 displayId = 0);
    void clearSelection();
    [[nodiscard]] bool returnToSelection(const QPoint& physicalPoint);

  private:
    ScreenshotSelectorWorkflowContext m_context;
    std::optional<quint64> m_initialNotifiedSession;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOW_H
