#include "snow_shot/app/featureavailability.h"

#include <utility>

namespace snow_shot::app {
std::optional<FeatureFamily> featureFamilyFor(presentation::GlobalShortcutAction action) {
    using Action = presentation::GlobalShortcutAction;
    switch (action) {
    case Action::Screenshot:
    case Action::ScreenshotDelay:
    case Action::ScreenshotOcr:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotCopy:
    case Action::ScreenshotFullScreen:
    case Action::ScreenshotFocusedWindow:
        return FeatureFamily::Screenshot;
    case Action::ScreenshotFixed:
    case Action::PinClipboardContent:
    case Action::PinSelectedFiles:
        return FeatureFamily::PinToScreen;
    case Action::ScreenRecord:
    case Action::ScreenRecordCopy:
        return FeatureFamily::ScreenRecording;
    case Action::OpenScreenRecordingFolder:
    case Action::OpenCaptureHistory:
    case Action::OpenSettings:
    case Action::TranslateSelectedText:
        return std::nullopt;
    }
    return std::nullopt;
}

FeatureFamily featureFamilyFor(presentation::settings::SettingsGlobalMouseAction action) {
    using Action = presentation::settings::SettingsGlobalMouseAction;
    switch (action) {
    case Action::ScreenshotFixed:
        return FeatureFamily::PinToScreen;
    case Action::ScreenRecording:
        return FeatureFamily::ScreenRecording;
    case Action::ScreenshotCopy:
    case Action::ScreenshotOcr:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotQuickSave:
    case Action::ScreenshotSave:
        return FeatureFamily::Screenshot;
    }
    return FeatureFamily::Screenshot;
}

bool isFeatureAvailable(FeatureFamily feature) {
    (void)feature;
#ifdef Q_OS_MACOS
    return false;
#else
    return true;
#endif
}

FeatureGate::FeatureGate(UnavailableHandler unavailableHandler)
    : m_unavailableHandler(std::move(unavailableHandler)) {}

bool FeatureGate::allow(FeatureFamily feature, bool notify) const {
    if (isFeatureAvailable(feature)) {
        return true;
    }
    if (notify && m_unavailableHandler) {
        m_unavailableHandler(feature);
    }
    return false;
}

FeatureActionRouter::FeatureActionRouter(FeatureGate::UnavailableHandler unavailableHandler)
    : m_gate(std::move(unavailableHandler)) {}

bool FeatureActionRouter::dispatch(FeatureFamily feature, Action action, bool notify) const {
    if (!m_gate.allow(feature, notify)) {
        return false;
    }
    if (action) {
        action();
    }
    return true;
}

bool FeatureActionRouter::beginGesture(FeatureFamily feature, Action cancel, Action action) const {
    if (!m_gate.allow(feature)) {
        if (cancel) {
            cancel();
        }
        return false;
    }
    if (action) {
        action();
    }
    return true;
}
} // namespace snow_shot::app
