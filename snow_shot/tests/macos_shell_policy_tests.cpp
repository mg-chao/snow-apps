#include "snow_shot/app/featureavailability.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {
using snow_shot::app::FeatureFamily;
using snow_shot::presentation::GlobalShortcutAction;
using snow_shot::presentation::settings::SettingsGlobalMouseAction;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void shortcutFamiliesAreComplete() {
    using Pair = std::pair<GlobalShortcutAction, std::optional<FeatureFamily>>;
    const std::vector<Pair> expected{
        {GlobalShortcutAction::Screenshot, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotDelay, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotFixed, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::ScreenshotOcr, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotTranslation, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotCopy, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotFullScreen, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotFocusedWindow, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenRecord, FeatureFamily::ScreenRecording},
        {GlobalShortcutAction::ScreenRecordCopy, FeatureFamily::ScreenRecording},
        {GlobalShortcutAction::OpenScreenRecordingFolder, std::nullopt},
        {GlobalShortcutAction::OpenCaptureHistory, std::nullopt},
        {GlobalShortcutAction::OpenSettings, std::nullopt},
        {GlobalShortcutAction::PinClipboardContent, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::TranslateSelectedText, std::nullopt},
        {GlobalShortcutAction::PinSelectedFiles, FeatureFamily::PinToScreen},
    };
    for (const auto& [action, family] : expected) {
        require(snow_shot::app::featureFamilyFor(action) == family,
                "every shortcut action must have the expected feature family");
    }
}

void globalMouseFamiliesAreComplete() {
    using Pair = std::pair<SettingsGlobalMouseAction, FeatureFamily>;
    const std::vector<Pair> expected{
        {SettingsGlobalMouseAction::ScreenshotCopy, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotFixed, FeatureFamily::PinToScreen},
        {SettingsGlobalMouseAction::ScreenshotOcr, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotTranslation, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotQuickSave, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotSave, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenRecording, FeatureFamily::ScreenRecording},
    };
    for (const auto& [action, family] : expected) {
        require(snow_shot::app::featureFamilyFor(action) == family,
                "every global-mouse action must have the expected feature family");
    }
}

void macosRouterStopsDispatchAndNotifiesOnce() {
    int notices = 0;
    int screenshotDispatches = 0;
    int pinDispatches = 0;
    int recordingDispatches = 0;
    int restorationDispatches = 0;
    std::optional<FeatureFamily> lastNotice;
    const snow_shot::app::FeatureActionRouter router([&](FeatureFamily feature) {
        ++notices;
        lastNotice = feature;
    });

    require(router.dispatch(FeatureFamily::Screenshot, [&]() { ++screenshotDispatches; }),
            "macOS screenshot routing must reach capture");
    require(!lastNotice.has_value(), "supported screenshots must not show an unavailable notice");
    require(!router.dispatch(FeatureFamily::PinToScreen, [&]() { ++pinDispatches; }),
            "macOS pin routing must be rejected");
    require(lastNotice == FeatureFamily::PinToScreen,
            "the router must identify an unavailable pin operation");
    require(!router.dispatch(FeatureFamily::ScreenRecording, [&]() { ++recordingDispatches; }),
            "macOS recording routing must be rejected");
    require(lastNotice == FeatureFamily::ScreenRecording,
            "the router must identify an unavailable recording operation");
    require(screenshotDispatches == 1 && pinDispatches == 0 && recordingDispatches == 0,
            "unavailable macOS actions must never reach feature handlers");
    require(notices == 2, "each explicit unavailable action must emit one notice");

    require(!router.dispatch(
                FeatureFamily::PinToScreen, [&]() { ++restorationDispatches; }, false) &&
                restorationDispatches == 0 && notices == 2,
            "silent startup restoration must not run or emit a notice");
}

void macosRouterCancelsBlockedGestureOnce() {
    int notices = 0;
    int cancellations = 0;
    int gestureDispatches = 0;
    const snow_shot::app::FeatureActionRouter router([&](FeatureFamily feature) {
        require(feature == FeatureFamily::ScreenRecording,
                "a screenshot gesture must report the screenshot family");
        ++notices;
    });

    require(!router.beginGesture(
                FeatureFamily::ScreenRecording, [&]() { ++cancellations; },
                [&]() { ++gestureDispatches; }),
            "an unavailable global-mouse gesture must be rejected");
    require(notices == 1, "a blocked gesture must emit exactly one notice at begin");
    require(cancellations == 1, "a blocked gesture must be cancelled exactly once");
    require(gestureDispatches == 0, "a blocked gesture must not reach capture handling");
}
} // namespace

int main() {
    shortcutFamiliesAreComplete();
    globalMouseFamiliesAreComplete();
    macosRouterStopsDispatchAndNotifiesOnce();
    macosRouterCancelsBlockedGestureOnce();
    return 0;
}
