#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include "snow_ui_selector.h"

#include <QByteArray>

struct ScreenshotSelectorLookupPolicy {
    SnowUiSelectorBackend backend = SNOW_UI_SELECTOR_BACKEND_MSAA;
    SnowUiSelectorHitTestMode mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
};

inline ScreenshotSelectorLookupPolicy screenshotSelectorLookupPolicy(
    bool smartSelectionEnabled, const QByteArray& configuredBackend) {
    if (smartSelectionEnabled) {
        return {SNOW_UI_SELECTOR_BACKEND_MSAA, SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT};
    }

    const QByteArray backend = configuredBackend.trimmed().toLower();
    SnowUiSelectorBackend selectedBackend = SNOW_UI_SELECTOR_BACKEND_MSAA;
    if (backend == "uia" || backend == "ui_automation" || backend == "ui-automation") {
        selectedBackend = SNOW_UI_SELECTOR_BACKEND_UIA;
    }
    return {selectedBackend, SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW};
}

inline SnowUiSelectorHitTestMode
screenshotSelectorHitTestMode(bool smartSelectionEnabled,
                              ScreenshotSelectorHitTestMode requestedMode) {
    return !smartSelectionEnabled || requestedMode == ScreenshotSelectorHitTestMode::Window
               ? SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW
               : SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
