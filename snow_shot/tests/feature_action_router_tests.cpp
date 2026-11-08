#include "snow_shot/app/featureavailability.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using snow_shot::app::FeatureFamily;
    using snow_shot::presentation::globalActionTraits;
    using snow_shot::presentation::GlobalShortcutAction;
    for (const auto& traits : snow_shot::presentation::globalActionTraitsCatalog) {
        require(globalActionTraits(traits.action) == &traits,
                "every finite action has exactly one stable classification");
        require(snow_shot::app::featureFamilyFor(traits.action) == traits.family,
                "application routing consumes the common action family");
    }
    require(!globalActionTraits(static_cast<GlobalShortcutAction>(-1)) &&
                !globalActionTraits(static_cast<GlobalShortcutAction>(9999)),
            "unknown action values cannot acquire a valid classification");
    int dispatches = 0;
    int cancellations = 0;
    snow_shot::app::FeatureActionRouter router;
    router.setSuspended(true);
    for (const auto feature :
         {FeatureFamily::Screenshot, FeatureFamily::PinToScreen, FeatureFamily::ScreenRecording}) {
        require(!router.dispatch(feature, [&] { ++dispatches; }),
                "queued capture actions cannot start during handoff");
        require(!router.beginGesture(
                    feature, [&] { ++cancellations; }, [&] { ++dispatches; }),
                "queued mouse gestures cannot start during handoff");
    }
    require(dispatches == 0 && cancellations == 3,
            "suspended actions stay idle and gestures cancel");
    router.setSuspended(false);
    require(
        router.dispatch(FeatureFamily::Screenshot, [&] { ++dispatches; }) &&
            router.beginGesture(
                FeatureFamily::ScreenRecording, [&] { ++cancellations; }, [&] { ++dispatches; }) &&
            dispatches == 2 && cancellations == 3,
        "failed handoff restores capture and recording actions");
    return EXIT_SUCCESS;
}
