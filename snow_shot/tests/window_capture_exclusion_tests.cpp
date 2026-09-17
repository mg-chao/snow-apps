#include "presentation/capture/windowcaptureexclusion.h"

#include <QApplication>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using snow_shot::presentation::WindowCaptureExclusion;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void scrollingExclusionAttemptsAreIndependent(bool overlaySucceeds, bool toolbarSucceeds) {
    QWidget overlay;
    QWidget toolbar;
    overlay.show();
    toolbar.show();
    std::vector<std::pair<QWidget*, bool>> calls;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        calls.emplace_back(window, excluded);
        return window == &overlay ? overlaySucceeds : toolbarSucceeds;
    });

    exclusion.exclude(&overlay);
    exclusion.exclude(&toolbar);
    require(calls == std::vector<std::pair<QWidget*, bool>>{{&overlay, true}, {&toolbar, true}},
            "scrolling must attempt both exclusions without rolling back a partial success");
    require(overlay.isVisible() && toolbar.isVisible(),
            "scrolling windows must stay visible regardless of exclusion results");

    calls.clear();
    exclusion.restore();
    std::vector<std::pair<QWidget*, bool>> expected;
    if (toolbarSucceeds) {
        expected.emplace_back(&toolbar, false);
    }
    if (overlaySucceeds) {
        expected.emplace_back(&overlay, false);
    }
    require(calls == expected, "cleanup must restore each successful exclusion");
    exclusion.restore();
    require(calls == expected, "repeated cleanup must not restore exclusions again");
}

void recordingToolbarRemainsVisible(bool succeeds) {
    QWidget toolbar;
    toolbar.show();
    int attempts = 0;
    int restores = 0;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        require(window == &toolbar, "recording must target its toolbar");
        excluded ? ++attempts : ++restores;
        return succeeds;
    });

    exclusion.exclude(&toolbar);
    require(attempts == 1 && restores == 0, "recording must attempt exclusion once");
    require(toolbar.isVisible(), "failed exclusion must not hide the recording toolbar");
    exclusion.restore();
    require(restores == (succeeds ? 1 : 0), "recording must clean up successful exclusion only");
    require(toolbar.isVisible(), "cleanup must preserve recording toolbar visibility");
}

void cleanupToleratesDestroyedWindowsAndRestoreFailures() {
    auto destroyedWindow = std::make_unique<QWidget>();
    QWidget toolbar;
    QWidget overlay;
    std::vector<QWidget*> restored;
    WindowCaptureExclusion exclusion([&](QWidget* window, bool excluded) {
        if (!excluded) {
            restored.push_back(window);
        }
        return excluded;
    });
    exclusion.exclude(destroyedWindow.get());
    exclusion.exclude(&overlay);
    exclusion.exclude(&toolbar);
    exclusion.exclude(nullptr);
    destroyedWindow.reset();
    exclusion.restore();
    require(restored == std::vector<QWidget*>{&toolbar, &overlay},
            "cleanup must skip destroyed windows and continue after restore failure");
    exclusion.restore();
    require(restored.size() == 2, "cleanup must clear tracking even after restore failure");
}

void unavailableExclusionPreservesVisibility() {
    QWidget toolbar;
    toolbar.show();
    WindowCaptureExclusion exclusion;
    exclusion.exclude(&toolbar);
    exclusion.restore();
    require(toolbar.isVisible(), "unavailable exclusion must not hide the toolbar");
}
void successfulIdsAndScopeCleanup() {
    QWidget first, second, failed;
    int attempts = 0;
    std::vector<QWidget*> restored;
    {
        WindowCaptureExclusion exclusion([&](QWidget* window, bool exclude) {
            if (exclude)
                ++attempts;
            else
                restored.push_back(window);
            return window != &failed;
        });
        require(exclusion.exclude(&first), "first exclusion succeeds");
        require(exclusion.exclude(&first), "repeated exclusion succeeds without another lease");
        require(exclusion.exclude(&second), "second exclusion succeeds");
        require(!exclusion.exclude(&failed), "failed exclusion reports failure");
        require(attempts == 3, "duplicate exclusion is idempotent");
        const auto resolve = [&](QWidget* window) -> std::optional<std::uint32_t> {
            return window == &first ? 17 : 23;
        };
        auto ids = exclusion.windowIds(resolve);
        require(ids == QVector<std::uint32_t>{17, 23},
                "only successful exclusions reach capture configs");
        require(exclusion.windowIds(resolve) == ids,
                "mode changes and pause snapshots retain exclusions");
    }
    require(restored == std::vector<QWidget*>{&second, &first},
            "scope exit restores in reverse order");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    for (bool overlaySucceeds : {false, true}) {
        for (bool toolbarSucceeds : {false, true}) {
            scrollingExclusionAttemptsAreIndependent(overlaySucceeds, toolbarSucceeds);
        }
        recordingToolbarRemainsVisible(overlaySucceeds);
    }
    cleanupToleratesDestroyedWindowsAndRestoreFailures();
    unavailableExclusionPreservesVisibility();
    successfulIdsAndScopeCleanup();
    return 0;
}
