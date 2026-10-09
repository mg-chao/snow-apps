#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONSERVICES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONSERVICES_H

#include "snow_shot/presentation/screenshotsmartselectiontransition.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotuipreferences.h"
#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QPoint>
#include <QPointF>
#include <QMap>
#include <QSet>
#include <QRect>
#include <QRectF>
#include <QStringList>

#include <optional>
#include <functional>
#include <memory>

struct ScreenshotCaptureState;
struct ScreenshotColorPickerContext;
class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotIntelligentSelectionModel;
class ScreenshotOverlayCoordinator;
class ScreenshotOverlayWindow;
class ScreenshotSelectionModel;
class ScreenshotToolbarPresenter;
struct ScreenshotToolbarPresentationState;

struct ScreenshotPresentationServicesContext {
    ScreenshotCaptureState& captureState;
    ScreenshotOverlayCoordinator& overlayCoordinator;
    ScreenshotToolbarPresenter& toolbarPresenter;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotDisplaySession& displaySession;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
    QSet<SnowCanvasTool> quickSelectionDisabledTools;
    std::function<void()> stateChanged = [] {};
    // Optional monotonic clock for deterministic frame scheduling tests.
    std::function<qint64()> monotonicNanoseconds = {};
};

class ScreenshotPresentationServices final {
  public:
    explicit ScreenshotPresentationServices(ScreenshotPresentationServicesContext context);
    ~ScreenshotPresentationServices();

    void hideToolbar();
    void hideMainToolbar();
    void showToolbar();
    void showSelectionToolbar();
    void moveToolbar();
    void repositionToolbarForContentChange();
    void raiseToolbarForCanvasInteraction();
    void setSelectionToolbarHovered(bool hovered);
    void setUiPreferences(const ScreenshotUiPreferences& preferences);
    void setGuideLinesVisible(bool visible);
    void setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    void reloadConfiguredShortcuts();

    void setSelectionMovementActive(bool active);
    // Release capture-owned snapshots and pending frames while retaining the scheduler.
    void resetPresentation();
    void updateOverlayState();
    void updatePointerPresentation(ScreenshotOverlayWindow* overlay, const QPointF& localPosition);
    void flushPendingFrame();
    void updateOverlayCursors() const;

    [[nodiscard]] ScreenshotColorPickerContext colorPickerContext() const;

  private:
    struct State;
    void scheduleFrame();
    [[nodiscard]] qint64 nowNanoseconds() const;
    void presentOverlayState(const QRectF& selection, bool semanticChanged, bool geometryChanged);
    [[nodiscard]] ScreenshotToolbarPresentationState toolbarPresentationState() const;

    ScreenshotPresentationServicesContext m_context;
    std::unique_ptr<State> m_state;
    ScreenshotSmartSelectionTransition m_smartSelectionTransition;
    ScreenshotUiPreferences m_uiPreferences;
    bool m_guideLinesVisible = false;
    std::optional<snow_shot::shortcuts::ShortcutBindingMap> m_configuredShortcuts;
    bool m_selectionToolbarHovered = false;
    bool m_selectionMovementActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPRESENTATIONSERVICES_H
