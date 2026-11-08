#ifndef SNOW_SHOT_APP_FEATUREAVAILABILITY_H
#define SNOW_SHOT_APP_FEATUREAVAILABILITY_H

#include "snow_shot/presentation/globalmousetypes.h"
#include "snow_shot/presentation/globalactiontraits.h"

#include <functional>
#include <optional>

namespace snow_shot::app {
using FeatureFamily = presentation::GlobalActionFamily;

[[nodiscard]] std::optional<FeatureFamily>
featureFamilyFor(presentation::GlobalShortcutAction action);
[[nodiscard]] FeatureFamily
featureFamilyFor(presentation::settings::SettingsGlobalMouseAction action);
// Routes feature-family actions through reversible application suspension.
// Edition availability is enforced by the actual build and feature owners.
class FeatureActionRouter final {
  public:
    using Action = std::function<void()>;

    void setSuspended(bool suspended);
    [[nodiscard]] bool dispatch(FeatureFamily feature, Action action = {}) const;
    [[nodiscard]] bool beginGesture(FeatureFamily feature, Action cancel, Action action = {}) const;

  private:
    bool m_suspended = false;
};
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_FEATUREAVAILABILITY_H
