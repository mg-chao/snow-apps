#include "snow_shot/platform/macos/recapturefocus.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void drainEvents() {
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
}
void exerciseCapture() {
    using snow_shot::platform::macos::RecaptureFocus;
    bool active = false;
    bool cursorDelivered = false;
    bool transparent = false;
    bool editorFocused = true;
    int captures = 0;
    int refreshes = 0;
    int restores = 0;
    // Exercise the ownership lifecycle of two consecutive captures.
    for (int iteration = 0; iteration < 2; ++iteration) {
        require(editorFocused, "the editor must own keyboard focus before each recapture");
        active = false;
        cursorDelivered = false;
        auto focus = std::make_unique<RecaptureFocus>(RecaptureFocus::Backend{
            [&] {
                transparent = true;
                editorFocused = false;
                return true;
            },
            [&] { return active; },
            [&] {
                require(active && transparent, "cursor refresh requires target focus and input");
                ++refreshes;
                return true;
            },
            [&] {
                transparent = false;
                editorFocused = true;
                ++restores;
            },
            [&] { return cursorDelivered; }});
        focus->prepare([&](bool ready) {
            require(ready && !editorFocused && transparent,
                    "capture must retain the foreign application's cursor ownership");
            ++captures;
        });
        drainEvents();
        require(captures == iteration && refreshes == iteration,
                "capture must wait for the target window to acquire focus");
        active = true;
        drainEvents();
        require(captures == iteration && refreshes == iteration + 1,
                "activation alone must not dispatch before the cursor refresh reaches the target");
        cursorDelivered = true;
        drainEvents();
        require(captures == iteration + 1 && refreshes == iteration + 1 && transparent,
                "capture must dispatch once and retain transparency until completion");
        focus.reset();
        require(editorFocused && !transparent && restores == iteration + 1,
                "completion must restore input and allow another keyboard capture");
    }
}
void cancellationAndFailure() {
    using snow_shot::platform::macos::RecaptureFocus;
    for (const bool beginSucceeds : {false, true}) {
        int completions = 0;
        int restores = 0;
        auto focus = std::make_unique<RecaptureFocus>(
            RecaptureFocus::Backend{[=] { return beginSucceeds; }, [] { return true; },
                                    [] { return true; }, [&] { ++restores; }});
        focus->prepare([&](bool ready) {
            require(!ready, "failed preparation must never report capture readiness");
            ++completions;
        });
        focus.reset();
        drainEvents();
        require(restores == 1 && completions == (beginSucceeds ? 0 : 1),
                "cancellation must restore partial state and suppress queued capture callbacks");
    }
    int failures = 0;
    auto focus = std::make_unique<RecaptureFocus>(RecaptureFocus::Backend{
        [] { return true; }, [] { return true; }, [] { return false; }, [] {}});
    focus->prepare([&](bool ready) {
        require(!ready, "failed cursor refresh must abort capture");
        ++failures;
        focus.reset();
    });
    drainEvents();
    require(failures == 1 && !focus, "failure completion may safely destroy the transaction");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    exerciseCapture();
    cancellationAndFailure();
    std::cout << "macOS recapture focus tests passed\n";
}
