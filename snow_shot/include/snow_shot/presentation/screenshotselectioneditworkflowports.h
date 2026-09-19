#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOWPORTS_H

#include "snow_shot/presentation/screenshotselectionparams.h"

#include <QRect>

#include <functional>

class QObject;
class QWidget;

using ScreenshotApplySelectionCallback =
    std::function<void(const ScreenshotSelectionParams& params)>;
using ScreenshotSelectionResizeFinishedCallback = std::function<void()>;

struct ScreenshotSelectionResizeRequest {
    ScreenshotSelectionParams currentParams;
    QRect selectionBounds;
    QWidget* ownerWindow = nullptr;
    ScreenshotSelectionResizeFinishedCallback onFinished;
    bool canvasUsesPoints = false;
};

struct ScreenshotSelectionEditUiActions {
    std::function<void()> updateOverlayState;
    std::function<void()> showSelectionToolbar;
    std::function<void()> moveToolbar;
    std::function<void()> repositionSelectionToolbarForContentChange;
    std::function<void()> showToolbar;
    std::function<bool(QObject* modalParent, const ScreenshotSelectionResizeRequest& request,
                       ScreenshotApplySelectionCallback applySelection)>
        openResizeModal;
    std::function<void()> hideColorPicker;
    std::function<void(bool)> setColorPickerSuppressed;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEDITWORKFLOWPORTS_H
