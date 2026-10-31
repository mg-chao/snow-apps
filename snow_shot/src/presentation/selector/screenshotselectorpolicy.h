#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include "snow_ui_selector.h"
#include "snow_shot/smartselectionmode.h"

#include <QByteArray>

struct ScreenshotSelectorLookupPolicy {
#ifdef Q_OS_MACOS
    SnowUiSelectorBackend backend = SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY;
#else
    SnowUiSelectorBackend backend = SNOW_UI_SELECTOR_BACKEND_UIA;
#endif
    SnowUiSelectorHitTestMode mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
};

inline ScreenshotSelectorLookupPolicy
screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode mode,
                               const QByteArray& configuredBackend) {
#ifdef Q_OS_MACOS
    Q_UNUSED(configuredBackend);
    return {SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY,
            mode == snow_shot::SmartSelectionMode::WindowSubElement
                ? SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT
                : SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW};
#else
    const QByteArray backend = configuredBackend.trimmed().toLower();
    SnowUiSelectorBackend selectedBackend = SNOW_UI_SELECTOR_BACKEND_UIA;
    if (backend == "msaa") {
        selectedBackend = SNOW_UI_SELECTOR_BACKEND_MSAA;
    }
    return {selectedBackend, mode == snow_shot::SmartSelectionMode::WindowSubElement
                                 ? SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT
                                 : SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW};
#endif
}

inline SnowUiSelectorHitTestMode
screenshotSelectorHitTestMode(snow_shot::SmartSelectionMode mode,
                              ScreenshotSelectorHitTestMode requestedMode) {
    return mode != snow_shot::SmartSelectionMode::WindowSubElement ||
                   requestedMode == ScreenshotSelectorHitTestMode::Window
               ? SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW
               : SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORPOLICY_H
