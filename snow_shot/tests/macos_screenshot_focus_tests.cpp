// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/macos/screenshotsessionfocus.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform::macos;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Fixture {
    struct Target final : ScreenshotFocusTarget {
        explicit Target(Fixture& fixture) : f(fixture), identity(f.source) {
            ++f.live;
        }
        ~Target() override {
            --f.live;
        }
        bool canRestore() const override {
            return f.allowed && f.targetAlive;
        }
        void restore() override {
            require(f.hidden, "restoration must follow presentation teardown");
            ++f.restored;
            f.restoredIdentity = identity;
        }
        Fixture& f;
        int identity;
    };
    bool hidden = false;
    bool allowed = true;
    bool targetAlive = true;
    bool targetAvailable = true;
    int source = 1;
    int captures = 0;
    int restored = 0;
    int restoredIdentity = 0;
    int live = 0;
    ScreenshotSessionFocus focus{[this]() -> std::unique_ptr<ScreenshotFocusTarget> {
        ++captures;
        return targetAvailable ? std::make_unique<Target>(*this) : nullptr;
    }};

    void end(bool restore = true) {
        focus.beforeHide(restore);
        hidden = true;
        focus.afterHide();
    }
    void flush() {
        QCoreApplication::processEvents();
    }
};

void invocationIdentityAndSingleRestoration() {
    Fixture f;
    f.focus.prepare();
    f.source = 2; // Dismissing a popup can change the foreground window.
    f.focus.begin(true);
    f.focus.begin(true); // Recapture/restart must not sample the overlay.
    require(f.captures == 1, "accepted starts must use the pre-cleanup invocation snapshot");
    f.end();
    require(f.restored == 0, "teardown must finish before queued restoration");
    f.end();
    f.flush();
    require(f.restored == 1 && f.restoredIdentity == 1 && f.live == 0,
            "duplicate cleanup must restore the original identity exactly once and release it");
    f.flush();
    require(f.restored == 1, "export completion must not own another restoration");
}

void rejectedSilentAndDiscardedSessions() {
    Fixture f;
    f.focus.prepare();
    f.focus.abandonPrepared();
    f.end();
    f.flush();
    require(f.restored == 0 && f.live == 0, "rejected capture must release its candidate");
    f.focus.begin(false);
    f.end();
    f.flush();
    require(f.captures == 1, "silent capture must not sample foreground state");
    f.focus.begin(true);
    f.end(false);
    f.flush();
    require(f.restored == 0 && f.live == 0,
            "pinning and other destination transitions must discard the target");
    f.focus.begin(true);
    f.end();
    f.focus.discard();
    f.flush();
    require(f.restored == 0 && f.live == 0, "shutdown must cancel pending restoration");
}

void switchingApplicationsAndClosingTargets() {
    for (int stage = 0; stage < 3; ++stage) {
        Fixture f;
        f.focus.begin(true);
        if (stage == 0)
            f.allowed = false;
        f.end();
        if (stage == 1)
            f.allowed = false;
        if (stage == 2)
            f.targetAlive = false;
        f.flush();
        require(f.restored == 0 && f.live == 0,
                "an application switch or terminated target must cancel focus restoration");
    }
    Fixture f;
    f.targetAvailable = false;
    f.focus.begin(true);
    f.end();
    f.flush();
    require(f.restored == 0, "unavailable foreground state must be harmless");
}

void newerSessionInvalidatesQueuedRestoration() {
    Fixture f;
    f.focus.begin(true);
    f.end();
    f.source = 2;
    f.focus.prepare();
    f.focus.begin(true);
    f.flush();
    require(f.restored == 0 && f.live == 1, "a new session must invalidate old queued work");
    f.end();
    f.flush();
    require(f.restored == 1 && f.restoredIdentity == 2 && f.live == 0,
            "the new session must restore only its own target");
}

void destructionCancelsQueuedWork() {
    int restores = 0;
    struct Target final : ScreenshotFocusTarget {
        explicit Target(int& count) : count(count) {}
        bool canRestore() const override {
            return true;
        }
        void restore() override {
            ++count;
        }
        int& count;
    };
    {
        ScreenshotSessionFocus focus([&] { return std::make_unique<Target>(restores); });
        focus.begin(true);
        focus.beforeHide(true);
        focus.afterHide();
    }
    QCoreApplication::processEvents();
    require(restores == 0, "QObject lifetime must cancel callbacks after controller destruction");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    invocationIdentityAndSingleRestoration();
    rejectedSilentAndDiscardedSessions();
    switchingApplicationsAndClosingTargets();
    newerSessionInvalidatesQueuedRestoration();
    destructionCancelsQueuedWork();
    return 0;
}
