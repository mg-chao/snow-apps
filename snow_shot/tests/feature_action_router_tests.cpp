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
    int dispatches = 0;
    int cancellations = 0;
    int notices = 0;
    snow_shot::app::FeatureActionRouter router([&](FeatureFamily) { ++notices; });
    router.setSuspended(true);
    for (const auto feature :
         {FeatureFamily::Screenshot, FeatureFamily::PinToScreen, FeatureFamily::ScreenRecording}) {
        require(!router.dispatch(feature, [&] { ++dispatches; }),
                "queued capture actions cannot start during handoff");
        require(!router.beginGesture(
                    feature, [&] { ++cancellations; }, [&] { ++dispatches; }),
                "queued mouse gestures cannot start during handoff");
    }
    require(dispatches == 0 && cancellations == 3 && notices == 0,
            "suspended actions stay idle and gestures cancel without unavailable notices");
    router.setSuspended(false);
    require(
        router.dispatch(FeatureFamily::Screenshot, [&] { ++dispatches; }) &&
            router.beginGesture(
                FeatureFamily::ScreenRecording, [&] { ++cancellations; }, [&] { ++dispatches; }) &&
            dispatches == 2 && cancellations == 3,
        "failed handoff restores capture and recording actions");
    return EXIT_SUCCESS;
}
