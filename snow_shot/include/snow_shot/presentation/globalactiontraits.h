// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/globalshortcuttypes.h"
#include <array>
#include <optional>

namespace snow_shot::presentation {
enum class GlobalActionFamily { Screenshot, PinToScreen, ScreenRecording };
enum class InitialActionPermission { None, ScreenRecording, Accessibility };

struct GlobalActionTraits {
    GlobalShortcutAction action;
    std::optional<GlobalActionFamily> family;
    InitialActionPermission permission;
};

// Stable action classification only. Origins and execution stay with application
// routing; final recording options are checked by the recording owner at start.
inline constexpr auto globalActionTraitsCatalog = [] {
    using A = GlobalShortcutAction;
    using F = GlobalActionFamily;
    using P = InitialActionPermission;
    return std::array{
        GlobalActionTraits{A::Screenshot, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotDelay, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotPinToScreen, F::PinToScreen, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotOcr, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotTranslation, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotCopy, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotFullScreen, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotFocusedWindow, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenRecord, F::ScreenRecording, P::ScreenRecording},
        GlobalActionTraits{A::ScreenRecordCopy, F::ScreenRecording, P::ScreenRecording},
        GlobalActionTraits{A::OpenScreenRecordingFolder, {}, P::None},
        GlobalActionTraits{A::OpenCaptureHistory, {}, P::None},
        GlobalActionTraits{A::OpenSettings, {}, P::None},
        GlobalActionTraits{A::PinClipboardContent, F::PinToScreen, P::None},
        GlobalActionTraits{A::TranslateSelectedText, {}, P::Accessibility},
        GlobalActionTraits{A::PinSelectedFiles, F::PinToScreen, P::None},
        GlobalActionTraits{A::RestoreLastClosedWindows, F::PinToScreen, P::None},
        GlobalActionTraits{A::ToggleGlobalShortcuts, {}, P::None},
        GlobalActionTraits{A::ToggleDisableOnFocusedFullscreenWindow, {}, P::None},
        GlobalActionTraits{A::OpenPinToScreenManagement, {}, P::None},
        GlobalActionTraits{A::GlobalCanvas, {}, P::None},
        GlobalActionTraits{A::SwitchWindowGroup, {}, P::None},
        GlobalActionTraits{A::ScreenshotSave, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ScreenshotQuickSave, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::HideShowAllWindows, F::PinToScreen, P::None},
        GlobalActionTraits{A::StitchSelectedImages, {}, P::None},
        GlobalActionTraits{A::SplitSelectedImages, {}, P::None},
        GlobalActionTraits{A::ScreenshotOcrCopy, F::Screenshot, P::ScreenRecording},
        GlobalActionTraits{A::ToggleWindowUnderCursorAlwaysOnTop, {}, P::None},
        GlobalActionTraits{A::ScreenshotScrolling, F::Screenshot, P::ScreenRecording}};
}();

static_assert(globalActionTraitsCatalog.size() ==
              static_cast<std::size_t>(GlobalShortcutAction::ScreenshotScrolling) + 1);
static_assert([] {
    for (std::size_t index = 0; index < globalActionTraitsCatalog.size(); ++index)
        if (static_cast<std::size_t>(globalActionTraitsCatalog[index].action) != index)
            return false;
    return true;
}());

[[nodiscard]] constexpr const GlobalActionTraits* globalActionTraits(GlobalShortcutAction action) {
    const auto index = static_cast<std::size_t>(action);
    return index < globalActionTraitsCatalog.size() ? &globalActionTraitsCatalog[index] : nullptr;
}
} // namespace snow_shot::presentation
