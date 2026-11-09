#include "snow_shot/app/applicationactiongate.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using snow_shot::app::ApplicationActionGate;
    bool paused = false;
    bool storageChanging = false;
    bool handoffPending = false;
    int invocations = 0;
    int authorizations = 0;
    int cancellations = 0;
    const ApplicationActionGate gate(
        [&] { return !paused && !storageChanging && !handoffPending; });
    const auto invoke = [&] { ++invocations; };
    const auto authorize = [&] {
        ++authorizations;
        return true;
    };
    for (bool* blocker : {&paused, &storageChanging, &handoffPending}) {
        *blocker = true;
        require(!gate.dispatch(invoke, authorize) && authorizations == 0,
                "closed admission must not query permissions or start work");
        require(!gate.beginGesture([&] { ++cancellations; }, invoke, authorize),
                "rejected gesture cancels its native producer");
        *blocker = false;
        require(!gate.dispatch(invoke,
                               [&] {
                                   *blocker =
                                       true; // models a synchronous permission-refresh observer
                                   return true;
                               }),
                "work cannot start when permission observers acquire a maintenance reservation");
        *blocker = false;
    }
    require(invocations == 0 && cancellations == 3,
            "reentrant admission changes never invoke a feature");
    require(!gate.dispatch(invoke, [] { return false; }), "denied permissions refuse work");
    require(gate.dispatch(invoke, authorize) && invocations == 1 && authorizations == 1,
            "a fresh authorized intent starts after restoration without replaying rejected work");
    require(gate.beginGesture([&] { ++cancellations; }, invoke, authorize) && invocations == 2 &&
                cancellations == 3,
            "accepted gestures preserve their invocation and cancel only on refusal");

    bool alive = true;
    auto owner = std::make_unique<ApplicationActionGate>([&] { return alive; });
    require(!owner->dispatch(invoke,
                             [&] {
                                 alive = false;
                                 owner.reset();
                                 return true;
                             }) &&
                invocations == 2,
            "authorization may destroy the guarded owner without invoking stale callbacks");
    return EXIT_SUCCESS;
}
