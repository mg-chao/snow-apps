// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/presentation/services/screenshotdelayedcapture_p.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Fixture {
    snow_shot::runtime::RuntimeActivityTracker tracker;
    bool available = true;
    int starts = 0;
    std::vector<int> delays;
    std::vector<ScreenshotDelayedCapture::Action> timers;
    std::function<void(bool)> onActivity;
    std::unique_ptr<ScreenshotDelayedCapture> request = std::make_unique<ScreenshotDelayedCapture>(
        ScreenshotDelayedCapture::Hooks{[this] { return available; }, [this] { ++starts; },
                                        [this](bool active) {
                                            if (onActivity)
                                                onActivity(active);
                                        },
                                        [this](int milliseconds, auto task) {
                                            delays.push_back(milliseconds);
                                            timers.push_back(std::move(task));
                                        }},
        tracker);

    void expire(std::size_t index) {
        // An inline notification may schedule another timer and reallocate the vector.
        const auto task = timers[index];
        task();
    }
};

void replacementAndInvalidationOwnOnlyOneReservation() {
    Fixture fixture;
    require(fixture.request->request(0) && fixture.request->request(30) &&
                fixture.delays == std::vector<int>({1000, 10000}) &&
                fixture.tracker.snapshot().activeCount == 1,
            "replacement preserves clamping and owns exactly one activity reservation");
    fixture.expire(0);
    require(fixture.starts == 0 && fixture.tracker.snapshot().activeCount == 1,
            "superseded expiry cannot release the replacement's reservation");
    fixture.expire(1);
    fixture.expire(1);
    require(fixture.starts == 1 && fixture.tracker.snapshot().activeCount == 0,
            "one current expiry releases its reservation and starts at most once");
    require(fixture.request->request(5), "schedule a request before explicit capture or history");
    fixture.request->invalidate();
    fixture.expire(2);
    require(fixture.starts == 1 && fixture.tracker.snapshot().activeCount == 0,
            "explicit start/history/reset invalidation retires timer and activity together");
}

void suspensionAndBusyExpiryPreserveCurrentPolicy() {
    Fixture fixture;
    fixture.available = false;
    require(!fixture.request->request(2) && fixture.timers.empty() &&
                fixture.tracker.snapshot().activeCount == 0,
            "suspended or preparing admission cannot schedule new work");
    fixture.available = true;
    require(fixture.request->request(2), "schedule while idle");
    fixture.available = false;
    fixture.expire(0);
    fixture.available = true;
    fixture.expire(0);
    require(fixture.starts == 0 && fixture.tracker.snapshot().activeCount == 0,
            "busy expiry drops the request without a resume retry");
    require(fixture.request->request(2), "schedule a request across temporary suspension");
    fixture.available = false;
    fixture.available = true;
    fixture.expire(1);
    require(fixture.starts == 1,
            "suspension ending before expiry preserves the existing delayed-capture policy");
}

void notificationsCannotCommitAnObsoleteExpiry() {
    Fixture fixture;
    require(fixture.request->request(2), "schedule original expiry");
    bool replaced = false;
    fixture.onActivity = [&](bool active) {
        if (!active && !std::exchange(replaced, true))
            require(fixture.request->request(4), "idle observer can request a new delay");
    };
    fixture.expire(0);
    require(fixture.starts == 0 && fixture.timers.size() == 2 &&
                fixture.tracker.snapshot().activeCount == 1,
            "replacement from expiry notification retains its entire delay and reservation");
    fixture.expire(1);
    require(fixture.starts == 1 && fixture.tracker.snapshot().activeCount == 0,
            "only the replacement expiry starts capture");

    fixture.onActivity = [&](bool active) {
        if (!active)
            fixture.available = false;
    };
    require(fixture.request->request(2), "schedule before notification closes admission");
    fixture.expire(2);
    require(fixture.starts == 1 && fixture.tracker.snapshot().activeCount == 0,
            "expiry revalidates admission after activity observers run");
}

void invalidationAndDestructionRemainSafeUnderReentry() {
    Fixture fixture;
    require(fixture.request->request(2), "schedule before invalidation");
    bool replaced = false;
    fixture.onActivity = [&](bool active) {
        if (!active && !std::exchange(replaced, true))
            require(fixture.request->request(3), "invalidation observer can replace request");
    };
    fixture.request->invalidate();
    fixture.expire(0);
    fixture.expire(1);
    require(fixture.starts == 1,
            "invalidation retires its generation before notifying replacement producers");
    fixture.onActivity = {};
    require(fixture.request->request(2), "schedule before destruction");
    fixture.request.reset();
    fixture.expire(2);
    require(fixture.starts == 1 && fixture.tracker.snapshot().activeCount == 0,
            "owner destruction releases the reservation and leaves harmless queued callbacks");

    Fixture retiring;
    require(retiring.request->request(2), "schedule before inline destruction");
    retiring.onActivity = [&](bool active) {
        if (!active)
            retiring.request.reset();
    };
    retiring.expire(0);
    require(!retiring.request && retiring.starts == 0 &&
                retiring.tracker.snapshot().activeCount == 0,
            "destruction inside expiry notification cannot access the retired controller");
}
} // namespace

int main() {
    replacementAndInvalidationOwnOnlyOneReservation();
    suspensionAndBusyExpiryPreserveCurrentPolicy();
    notificationsCannotCommitAnObsoleteExpiry();
    invalidationAndDestructionRemainSafeUnderReentry();
    return EXIT_SUCCESS;
}
