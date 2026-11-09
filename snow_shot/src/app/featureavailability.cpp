#include "snow_shot/app/featureavailability.h"

namespace snow_shot::app {
std::optional<FeatureFamily> featureFamilyFor(presentation::GlobalShortcutAction action) {
    const auto* traits = presentation::globalActionTraits(action);
    return traits ? traits->family : std::nullopt;
}

FeatureFamily featureFamilyFor(presentation::settings::SettingsGlobalMouseAction action) {
    using Action = presentation::settings::SettingsGlobalMouseAction;
    switch (action) {
    case Action::ScreenshotPinToScreen:
        return FeatureFamily::PinToScreen;
    case Action::ScreenRecording:
        return FeatureFamily::ScreenRecording;
    case Action::ScreenshotCopy:
    case Action::ScreenshotOcr:
    case Action::ScreenshotOcrCopy:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotQuickSave:
    case Action::ScreenshotSave:
        return FeatureFamily::Screenshot;
    }
    return FeatureFamily::Screenshot;
}

void FeatureActionRouter::setSuspended(bool suspended) {
    m_suspended = suspended;
}

bool FeatureActionRouter::dispatch(FeatureFamily /*feature*/, Action action) const {
    if (m_suspended) {
        return false;
    }
    if (action) {
        action();
    }
    return true;
}

bool FeatureActionRouter::beginGesture(FeatureFamily /*feature*/, Action cancel,
                                       Action action) const {
    if (m_suspended) {
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
