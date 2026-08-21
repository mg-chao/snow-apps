#include "idlememoryreclaimpolicy.h"

#include <cstdlib>
#include <iostream>

namespace {
using snow_shot::app::IdleMemoryReclaimPolicy;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void idleCleanupIsArmedAndConsumedExactlyOnce() {
    IdleMemoryReclaimPolicy policy;
    require(!policy.shouldArmTimer(false), "an empty policy must not arm the timer");

    policy.request();
    require(policy.shouldArmTimer(false) && policy.hasPendingRequest() &&
                !policy.hasPendingWorkingSetTrim(),
            "an idle cleanup request must arm without requesting a working-set trim");

    const auto decision = policy.takeIfIdle(false, false);
    require(decision.shouldReclaim && !decision.trimWorkingSet,
            "an idle cleanup request must produce a cache-only reclaim decision");
    require(!policy.hasPendingRequest() && !policy.shouldArmTimer(false) &&
                !policy.takeIfIdle(false, false).shouldReclaim,
            "a completed cleanup request must be consumed exactly once");
}

void visibleWindowsDeferWithoutDiscardingRequests() {
    IdleMemoryReclaimPolicy policy;
    policy.request(true);
    require(!policy.shouldArmTimer(false, true),
            "a visible window must keep a pending request dormant");

    const auto deferred = policy.takeIfIdle(false, true);
    require(!deferred.shouldReclaim && policy.hasPendingRequest() &&
                policy.hasPendingWorkingSetTrim(),
            "a visible window must defer cleanup without consuming its trim request");

    require(policy.shouldArmTimer(false, false),
            "hiding the last window must make the retained request armable");
    const auto resumed = policy.takeIfIdle(false, false);
    require(resumed.shouldReclaim && resumed.trimWorkingSet,
            "the deferred working-set trim must run after the last window hides");
}

void foregroundWorkDefersTrayHideCleanupUntilRelease() {
    IdleMemoryReclaimPolicy policy;

    // A prior successful pin requested a working-set trim. Foreground capture stops the armed
    // timer, then the tray menu's deferred aboutToHide callback requests cache cleanup again.
    policy.request(true);
    require(policy.shouldArmTimer(false), "the successful pin must initially arm reclamation");
    policy.request(false);
    require(!policy.shouldArmTimer(true),
            "tray-hide cleanup must not arm while a foreground operation is active");
    require(!policy.takeIfIdle(true, false).shouldReclaim && policy.hasPendingRequest() &&
                policy.hasPendingWorkingSetTrim(),
            "foreground deferral must retain the accumulated working-set trim request");

    // The foreground operation's idle transition schedules the same accumulated request.
    policy.request(false);
    require(policy.shouldArmTimer(false),
            "the foreground idle transition must re-arm the deferred request");
    const auto resumed = policy.takeIfIdle(false, false);
    require(resumed.shouldReclaim && resumed.trimWorkingSet && !policy.hasPendingRequest(),
            "the resumed reclaim must consume the successful pin's trim exactly once");
}

void repeatedRequestsCoalesceAndPreserveStrongestAction() {
    IdleMemoryReclaimPolicy policy;
    policy.request(false);
    policy.request(true);
    policy.request(false);

    const auto decision = policy.takeIfIdle(false, false);
    require(decision.shouldReclaim && decision.trimWorkingSet,
            "coalescing cleanup requests must preserve any requested working-set trim");
}
} // namespace

int main() {
    idleCleanupIsArmedAndConsumedExactlyOnce();
    visibleWindowsDeferWithoutDiscardingRequests();
    foregroundWorkDefersTrayHideCleanupUntilRelease();
    repeatedRequestsCoalesceAndPreserveStrongestAction();
    return 0;
}
