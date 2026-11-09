#include "snow_shot/app/applicationactiongate.h"
#include "snow_shot/presentation/globalactiontraits.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {
using FeatureFamily = snow_shot::presentation::GlobalActionFamily;
using snow_shot::presentation::GlobalShortcutAction;

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
        {GlobalShortcutAction::EditSelectedImages, std::nullopt},
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
        require(snow_shot::presentation::globalActionTraits(action)->family == family,
                "every shortcut action must have the expected feature family");
    }
}

void macosRouterAllowsAllFeatures() {
    int screenshotDispatches = 0;
    int pinDispatches = 0;
    int recordingDispatches = 0;
    int restorationDispatches = 0;
    const snow_shot::app::ApplicationActionGate gate([] { return true; });

    require(gate.dispatch([&]() { ++screenshotDispatches; }),
            "macOS screenshot routing must reach capture");
    require(gate.dispatch([&]() { ++pinDispatches; }), "macOS pin routing must reach capture");
    require(gate.dispatch([&]() { ++recordingDispatches; }),
            "macOS recording routing must reach capture");
    require(screenshotDispatches == 1 && pinDispatches == 1 && recordingDispatches == 1,
            "macOS actions must reach every supported feature handler");

    require(gate.dispatch([&]() { ++restorationDispatches; }) && restorationDispatches == 1,
            "startup restoration must run through feature dispatch");
}

void macosRouterDispatchesRecordingGesture() {
    int cancellations = 0;
    int gestureDispatches = 0;
    const snow_shot::app::ApplicationActionGate gate([] { return true; });

    require(gate.beginGesture([&]() { ++cancellations; }, [&]() { ++gestureDispatches; }),
            "an available global-mouse recording gesture must be dispatched");
    require(cancellations == 0, "an available recording gesture must not be cancelled");
    require(gestureDispatches == 1, "an available recording gesture must reach capture handling");
}
} // namespace

int main() {
    shortcutFamiliesAreComplete();
    macosRouterAllowsAllFeatures();
    macosRouterDispatchesRecordingGesture();
    return 0;
}
