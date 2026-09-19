#ifndef SNOW_SHOT_APP_FEATUREAVAILABILITY_H
#define SNOW_SHOT_APP_FEATUREAVAILABILITY_H

#include "snow_shot/presentation/globalmousetypes.h"
#include "snow_shot/presentation/globalshortcuttypes.h"

#include <functional>
#include <optional>

namespace snow_shot::app {
enum class FeatureFamily { Screenshot, PinToScreen, ScreenRecording };

[[nodiscard]] std::optional<FeatureFamily>
featureFamilyFor(presentation::GlobalShortcutAction action);
[[nodiscard]] FeatureFamily
featureFamilyFor(presentation::settings::SettingsGlobalMouseAction action);
[[nodiscard]] bool isFeatureAvailable(FeatureFamily feature);

class FeatureGate final {
  public:
    using UnavailableHandler = std::function<void(FeatureFamily)>;

    explicit FeatureGate(UnavailableHandler unavailableHandler = {});
    [[nodiscard]] bool allow(FeatureFamily feature, bool notify = true) const;

  private:
    UnavailableHandler m_unavailableHandler;
};

class FeatureActionRouter final {
  public:
    using Action = std::function<void()>;

    explicit FeatureActionRouter(FeatureGate::UnavailableHandler unavailableHandler = {});
    [[nodiscard]] bool dispatch(FeatureFamily feature, Action action = {},
                                bool notify = true) const;
    [[nodiscard]] bool beginGesture(FeatureFamily feature, Action cancel, Action action = {}) const;

  private:
    FeatureGate m_gate;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_FEATUREAVAILABILITY_H
