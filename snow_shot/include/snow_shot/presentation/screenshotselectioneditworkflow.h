#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOW_H

#include "snow_shot/presentation/screenshotselectioneditworkflowports.h"

#include <QRect>

class QObject;
class QWidget;
class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotInteractionState;
class ScreenshotSelectionModel;
struct ScreenshotCaptureState;
struct ScreenshotSelectionParams;

struct ScreenshotSelectionEditWorkflowContext {
    QObject& modalParent;
    ScreenshotCaptureState& captureState;
    const ScreenshotDisplaySession& displaySession;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotInteractionState& interaction;
    ScreenshotSelectionModel& selection;
    ScreenshotSelectionEditUiActions ui;
    std::function<void(int cornerRadius, int shadowWidth)> persistSelectionEffects = [](int, int) {
    };
    std::function<void(bool locked)> persistSelectionAspectRatioLock = [](bool) {};
};

class ScreenshotSelectionEditWorkflow final {
  public:
    explicit ScreenshotSelectionEditWorkflow(ScreenshotSelectionEditWorkflowContext context);

    void adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy);
    void setSelectionCornerRadiusFromToolbar(int radius);
    void setSelectionShadowWidthFromToolbar(int shadowWidth);
    void toggleSelectionAspectRatioLockFromToolbar();
    void openSelectionResizeModalFromToolbar();
    void repositionToolbarForContentChange();
    void hideColorPickersForScreenshotUi() const;

  private:
    void applySelectionParams(const ScreenshotSelectionParams& params);
    void setColorPickerSuppressedForScreenshotUi(bool suppressed) const;
    [[nodiscard]] QRect selectionBounds() const;
    [[nodiscard]] ScreenshotSelectionParams currentSelectionParams() const;
    [[nodiscard]] QWidget* ownerWindowForSelectionResizeModal() const;

    ScreenshotSelectionEditWorkflowContext m_context;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOW_H
