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
        {GlobalShortcutAction::ScreenshotPinToScreen, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::ScreenshotOcr, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotOcrCopy, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotTranslation, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotScrolling, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotCopy, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotSave, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotQuickSave, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotFullScreen, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenshotFocusedWindow, FeatureFamily::Screenshot},
        {GlobalShortcutAction::ScreenRecord, FeatureFamily::ScreenRecording},
        {GlobalShortcutAction::ScreenRecordCopy, FeatureFamily::ScreenRecording},
        {GlobalShortcutAction::OpenScreenRecordingFolder, std::nullopt},
        {GlobalShortcutAction::OpenCaptureHistory, std::nullopt},
        {GlobalShortcutAction::OpenPinToScreenManagement, std::nullopt},
        {GlobalShortcutAction::GlobalCanvas, std::nullopt},
        {GlobalShortcutAction::StitchSelectedImages, std::nullopt},
        {GlobalShortcutAction::SplitSelectedImages, std::nullopt},
        {GlobalShortcutAction::OpenSettings, std::nullopt},
        {GlobalShortcutAction::PinClipboardContent, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::TranslateSelectedText, std::nullopt},
        {GlobalShortcutAction::PinSelectedFiles, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::HideShowAllWindows, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::RestoreLastClosedWindows, FeatureFamily::PinToScreen},
        {GlobalShortcutAction::SwitchWindowGroup, std::nullopt},
        {GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop, std::nullopt},
        {GlobalShortcutAction::ToggleGlobalShortcuts, std::nullopt},
        {GlobalShortcutAction::ToggleDisableOnFocusedFullscreenWindow, std::nullopt},
    };
    require(expected.size() == snow_shot::presentation::globalActionTraitsCatalog.size(),
            "every stable shortcut has an explicit family contract");
    for (const auto& [action, family] : expected) {
        require(snow_shot::app::featureFamilyFor(action) == family,
                "every shortcut action must have the expected feature family");
    }
}

void globalMouseFamiliesAreComplete() {
    using Pair = std::pair<SettingsGlobalMouseAction, FeatureFamily>;
    const std::vector<Pair> expected{
        {SettingsGlobalMouseAction::ScreenshotCopy, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotPinToScreen, FeatureFamily::PinToScreen},
        {SettingsGlobalMouseAction::ScreenshotOcr, FeatureFamily::Screenshot},
        {SettingsGlobalMouseAction::ScreenshotOcrCopy, FeatureFamily::Screenshot},
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

void macosRouterAllowsAllFeatures() {
    int screenshotDispatches = 0;
    int pinDispatches = 0;
    int recordingDispatches = 0;
    int restorationDispatches = 0;
    const snow_shot::app::FeatureActionRouter router;

    require(router.dispatch(FeatureFamily::Screenshot, [&]() { ++screenshotDispatches; }),
            "macOS screenshot routing must reach capture");
    require(router.dispatch(FeatureFamily::PinToScreen, [&]() { ++pinDispatches; }),
            "macOS pin routing must reach capture");
    require(router.dispatch(FeatureFamily::ScreenRecording, [&]() { ++recordingDispatches; }),
            "macOS recording routing must reach capture");
    require(screenshotDispatches == 1 && pinDispatches == 1 && recordingDispatches == 1,
            "macOS actions must reach every supported feature handler");

    require(router.dispatch(FeatureFamily::PinToScreen, [&]() { ++restorationDispatches; }) &&
                restorationDispatches == 1,
            "startup restoration must run through feature dispatch");
}

void macosRouterDispatchesRecordingGesture() {
    int cancellations = 0;
    int gestureDispatches = 0;
    const snow_shot::app::FeatureActionRouter router;

    require(router.beginGesture(
                FeatureFamily::ScreenRecording, [&]() { ++cancellations; },
                [&]() { ++gestureDispatches; }),
            "an available global-mouse recording gesture must be dispatched");
    require(cancellations == 0, "an available recording gesture must not be cancelled");
    require(gestureDispatches == 1, "an available recording gesture must reach capture handling");
}
} // namespace

int main() {
    shortcutFamiliesAreComplete();
    globalMouseFamiliesAreComplete();
    macosRouterAllowsAllFeatures();
    macosRouterDispatchesRecordingGesture();
    return 0;
}
