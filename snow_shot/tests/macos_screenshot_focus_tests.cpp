// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/platform/macos/screenshotsessionfocus.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>
#include <vector>

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

struct BackgroundFixture {
    struct Window final : ScreenshotBackgroundWindow {
        explicit Window(BackgroundFixture& fixture) : f(fixture), identity(f.nativeIdentity) {}
        void suppress() override {
            ++f.suppressions;
        }
        void restoreBehind() override {
            require(f.presentationHidden, "background restoration must follow overlay teardown");
            ++f.restorations;
        }
        bool isValid() const override {
            return identity == f.nativeIdentity;
        }
        BackgroundFixture& f;
        int identity;
    };
    struct Target final : ScreenshotFocusTarget {
        explicit Target(BackgroundFixture& fixture) : f(fixture), external(f.external) {}
        bool canRestore() const override {
            return f.focusAllowed;
        }
        bool isExternalApplication() const override {
            return external;
        }
        void restore() override {
            require(f.presentationHidden, "focus must follow presentation teardown");
            ++f.focusRestores;
        }
        BackgroundFixture& f;
        bool external;
    };
    bool external = true;
    bool eligible = true;
    bool focusAllowed = true;
    bool presentationHidden = false;
    int captures = 0;
    int suppressions = 0;
    int restorations = 0;
    int focusRestores = 0;
    int nativeIdentity = 1;
    std::vector<std::function<void()>> timers;
    std::unique_ptr<ScreenshotBackgroundWindowGuard> background =
        std::make_unique<ScreenshotBackgroundWindowGuard>(ScreenshotBackgroundWindowGuard::Ports{
            [this]() -> std::unique_ptr<ScreenshotBackgroundWindow> {
                ++captures;
                return eligible ? std::make_unique<Window>(*this) : nullptr;
            },
            [this] { return external; },
            [this](QObject*, std::function<void()> callback) {
                timers.push_back(std::move(callback));
            }});
    ScreenshotBackgroundWindowGuard* guard = background.get();
    ScreenshotSessionFocus focus{[this] { return std::make_unique<Target>(*this); },
                                 std::move(background)};

    void end(bool restorePrevious = true) {
        focus.beforeHide(restorePrevious);
        require(restorations == 0, "beforeHide must retain background suppression");
        presentationHidden = true;
        focus.afterHide();
        QCoreApplication::processEvents();
    }
};

void backgroundEligibilityAndAcceptedStarts() {
    for (bool external : {false, true}) {
        for (bool eligible : {false, true}) {
            BackgroundFixture f;
            f.external = external;
            f.eligible = eligible;
            f.focus.prepare();
            require(f.suppressions == 0, "preparation must not change native visibility");
            f.external = !external; // Popup cleanup must not change invocation eligibility.
            f.focus.begin(true);
            f.focus.begin(true);
            require(f.suppressions == int(external && eligible),
                    "only an accepted external capture suppresses an eligible window, once");
            f.end(false);
            require(f.restorations == f.suppressions && f.focusRestores == 0,
                    "destination handoff restores visibility independently of focus");
        }
    }
    BackgroundFixture f;
    f.focus.prepare();
    f.focus.abandonPrepared();
    f.focus.begin(false);
    require(f.suppressions == 0 && f.restorations == 0,
            "rejected and silent starts must not change background visibility");
}

void backgroundRestorationDoesNotDependOnFocus() {
    for (bool allowed : {false, true}) {
        BackgroundFixture f;
        f.focus.begin(true);
        f.focusAllowed = allowed;
        f.end();
        require(f.restorations == 1 && f.focusRestores == int(allowed),
                "switching applications or losing a focus target must not lose visibility");
    }
    BackgroundFixture f;
    f.focus.begin(true);
    f.external = false;
    f.end();
    require(f.restorations == 0 && f.timers.size() == 1,
            "asynchronous activation must defer background restoration");
    f.guard->applicationActivated();
    require(f.restorations == 0, "our own activation is not the restoration boundary");
    f.external = true;
    f.guard->applicationActivated();
    f.timers.front()();
    require(f.restorations == 1, "activation and fallback must restore exactly once");
}

void backgroundInheritanceAndStaleCallbacks() {
    for (bool accept : {false, true}) {
        BackgroundFixture f;
        f.focus.begin(true);
        f.external = false;
        f.end();
        f.focus.prepare();
        f.timers.front()();
        require(f.restorations == 0, "new preparation invalidates the old fallback");
        if (accept) {
            f.focus.begin(true);
            require(f.suppressions == 1 && f.captures == 1,
                    "a new capture inherits the hidden native identity without resampling");
            f.end(false);
        } else {
            f.focus.abandonPrepared();
            f.timers.back()();
        }
        require(f.restorations == 1, "accepted or rejected next captures must release visibility");
        f.timers.front()();
        require(f.restorations == 1, "an old fallback cannot restore twice");
    }
    BackgroundFixture f;
    f.focus.begin(true);
    f.focus.beforeHide(true);
    f.focus.afterHide();
    f.focus.prepare();
    f.focus.begin(true);
    QCoreApplication::processEvents();
    require(f.restorations == 0 && f.suppressions == 1,
            "new capture must also invalidate the queued focus completion");
    f.end(false);
    require(f.restorations == 1, "the inherited lease must survive queued focus cancellation");
}

void backgroundExplicitReopenAndShutdown() {
    for (bool reopen : {false, true}) {
        BackgroundFixture f;
        f.focus.begin(true);
        f.external = false;
        f.end();
        if (reopen)
            f.focus.releaseBackgroundWindow();
        else
            f.focus.discard();
        f.external = true;
        f.guard->applicationActivated();
        f.timers.front()();
        require(f.restorations == 0,
                "explicit reopening and shutdown must cancel all automatic ordering");
    }
    BackgroundFixture f;
    f.focus.begin(true);
    f.focus.beforeHide(true);
    f.focus.afterHide();
    f.focus.releaseBackgroundWindow();
    QCoreApplication::processEvents();
    require(f.focusRestores == 0 && f.restorations == 0,
            "explicit reopening also supersedes a queued focus restoration");
}

void backgroundRejectedAndSilentStartsWhileRestoreQueued() {
    for (bool silent : {false, true}) {
        BackgroundFixture f;
        f.focus.begin(true);
        f.presentationHidden = true;
        f.focus.beforeHide(true);
        f.focus.afterHide();
        f.focus.prepare();
        if (silent)
            f.focus.begin(false);
        else
            f.focus.abandonPrepared();
        QCoreApplication::processEvents();
        require(f.restorations == 1 && f.focusRestores == 0,
                "a rejected or silent next start must release an inherited window");
    }
    std::function<void()> fallback;
    {
        BackgroundFixture f;
        f.focus.begin(true);
        f.external = false;
        f.end();
        fallback = f.timers.front();
    }
    fallback(); // The injected scheduler deliberately outlives the QObject context.

    BackgroundFixture f;
    f.focus.begin(true);
    f.external = false;
    f.end();
    ++f.nativeIdentity; // The same Qt widget can acquire a different native surface.
    f.external = true;
    f.focus.prepare();
    f.focus.begin(true);
    f.timers.front()();
    require(f.suppressions == 2 && f.restorations == 0,
            "a retired lease must not prevent suppression of a replacement native window");
    f.end(false);
    require(f.restorations == 1, "only the replacement identity may be restored");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    invocationIdentityAndSingleRestoration();
    rejectedSilentAndDiscardedSessions();
    switchingApplicationsAndClosingTargets();
    newerSessionInvalidatesQueuedRestoration();
    destructionCancelsQueuedWork();
    backgroundEligibilityAndAcceptedStarts();
    backgroundRestorationDoesNotDependOnFocus();
    backgroundInheritanceAndStaleCallbacks();
    backgroundExplicitReopenAndShutdown();
    backgroundRejectedAndSilentStartsWhileRestoreQueued();
    return 0;
}
