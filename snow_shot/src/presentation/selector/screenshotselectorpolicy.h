#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include "snow_ui_selector.h"

#include <QByteArray>

struct ScreenshotSelectorLookupPolicy {
    SnowUiSelectorBackend backend = SNOW_UI_SELECTOR_BACKEND_UIA;
    SnowUiSelectorHitTestMode mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
};

inline ScreenshotSelectorLookupPolicy
screenshotSelectorLookupPolicy(bool smartSelectionEnabled, const QByteArray& configuredBackend) {
    const QByteArray backend = configuredBackend.trimmed().toLower();
    SnowUiSelectorBackend selectedBackend = SNOW_UI_SELECTOR_BACKEND_UIA;
    if (backend == "msaa") {
        selectedBackend = SNOW_UI_SELECTOR_BACKEND_MSAA;
    }
    return {selectedBackend, smartSelectionEnabled ? SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT
                                                   : SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW};
}

inline SnowUiSelectorHitTestMode
screenshotSelectorHitTestMode(bool smartSelectionEnabled,
                              ScreenshotSelectorHitTestMode requestedMode) {
    return !smartSelectionEnabled || requestedMode == ScreenshotSelectorHitTestMode::Window
               ? SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW
               : SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
