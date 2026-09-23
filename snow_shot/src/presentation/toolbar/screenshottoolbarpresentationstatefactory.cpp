#include "snow_shot/presentation/screenshottoolbarpresentationstatefactory.h"

#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

ScreenshotToolbarPresentationState
makeScreenshotToolbarPresentationState(const ScreenshotInteractionState& interaction,
                                       const ScreenshotSelectionModel& selection) {
    ScreenshotToolbarPresentationState state;
    state.selectionPixels = selection.pixelSelection();
    state.selectionCanvas = selection.normalizedSelection();
    state.inactive = interaction.inactive();
    state.selectionToolbarMode =
        interaction.selectionToolbarMode() && !selection.regionOperationActive();
    state.selectionResizable = selection.rectangular() && interaction.canResizeSelection();
    state.intelligentSelecting = interaction.intelligentSelecting();
    state.editing = interaction.editing();
    const QRect selectionPixels = state.selectionPixels;
    state.ocrAvailable = selectionPixels.width() < 1 || selectionPixels.height() < 1 ||
                         screenshotOcrImageWithinPixelLimit(selectionPixels.size());
    state.aspectRatioLocked = selection.aspectRatioLocked();
    state.cornerRadius = selection.cornerRadius();
    state.cornerRadiusApplicable = selection.cornerRadiusApplicable();
    state.shadowWidth = selection.shadowWidth();
    state.shadowColor = selection.shadowColor();
    return state;
}
