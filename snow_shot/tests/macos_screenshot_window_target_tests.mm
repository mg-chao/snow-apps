#include "../src/platform/macos/screenshotwindowtarget_p.h"

#import <Foundation/Foundation.h>
#include <cstdlib>
#include <iostream>
#include <array>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
NSDictionary* window(int number, int owner, int layer = 0, double alpha = 1,
                     CGRect bounds = CGRectMake(10, 20, 200, 100)) {
    CFDictionaryRef rectangle = CGRectCreateDictionaryRepresentation(bounds);
    NSDictionary* result = @{
        reinterpret_cast<id>(kCGWindowNumber) : @(number),
        reinterpret_cast<id>(kCGWindowOwnerPID) : @(owner),
        reinterpret_cast<id>(kCGWindowLayer) : @(layer),
        reinterpret_cast<id>(kCGWindowAlpha) : @(alpha),
        reinterpret_cast<id>(kCGWindowBounds) : reinterpret_cast<NSDictionary*>(rectangle)
    };
    CFRelease(rectangle);
    return result;
}
} // namespace

int main() {
    using namespace snow_shot::platform::detail;
    @autoreleasepool {
        constexpr pid_t self = 42;
        constexpr pid_t other = 84;
        // WindowServer order: overlay, hidden/empty surfaces, our main window,
        // and two external windows. No live desktop or permission is required.
        NSArray* windows = @[
            window(1, self, 100), window(2, self, 0, 0), window(3, self, 0, 1, CGRectZero),
            window(4, self), window(5, other), window(6, other)
        ];
        const auto snapshot = reinterpret_cast<CFArrayRef>(windows);
        require(focusedWindowTarget(snapshot, self).id == 4,
                "focused capture must allow Snow Shot's own foreground window");
        require(focusedWindowTarget(snapshot, other).id == 5,
                "focused capture must select the frontmost eligible window of its owner");
        require(focusedWindowTarget(snapshot, 0).id == 0 &&
                    focusedWindowTarget(snapshot, 99).id == 0 &&
                    focusedWindowTarget(nullptr, self).id == 0,
                "missing focus or windows must not capture an unrelated application");
        const auto scroll = scrollWindowTarget(snapshot, self, QPoint(50, 50));
        require(scroll.id == 5 && scroll.pid == other && scroll.bounds.origin.x == 10,
                "scroll input must exclude our windows and retain the external target geometry");
        require(scrollWindowTarget(snapshot, self, QPoint(500, 500)).id == 0,
                "scroll input must only target a window under the selection");
        require(scrollWindowTarget(reinterpret_cast<CFArrayRef>(@[ window(4, self) ]), self,
                                   QPoint(50, 50))
                        .id == 0,
                "scroll input must never be sent back to our own window");
        const std::array<CGWindowID, 2> excluded{1, 7};
        // Simulate AppKit's ordered mouse hits. Its native implementation skips
        // transparent/decorative windows before reporting a hit to this policy.
        const auto hitTest = [](CGWindowID below) -> CGWindowID {
            switch (below) {
            case 0:
                return 1;
            case 1:
                return 7;
            case 7:
                return 8;
            default:
                return 0;
            }
        };
        require(recaptureWindowAtPoint(excluded, hitTest) == 8,
                "recapture must step below each excluded editing surface");
        require(recaptureWindowAtPoint({}, hitTest) == 1,
                "recapture must accept a hit unless its exact window ID is excluded");
        const std::array<CGWindowID, 1> excludeFirst{1};
        require(recaptureWindowAtPoint(excludeFirst, hitTest) == 7,
                "recapture must not step below another surface from the same process");
        require(recaptureWindowAtPoint(excluded, [](CGWindowID) { return 8U; }) == 8,
                "already click-through editing surfaces need no additional traversal");
        const std::array<CGWindowID, 3> excludeAll{1, 7, 8};
        require(recaptureWindowAtPoint(excludeAll, hitTest) == 0 &&
                    recaptureWindowAtPoint(excluded, [](CGWindowID) { return 0U; }) == 0 &&
                    recaptureWindowAtPoint(excluded, [](CGWindowID) { return 1U; }) == 0,
                "empty or changing native hit chains must terminate without an unrelated target");
        require(recaptureWindowTarget(snapshot, 4).id == 4,
                "recapture must retain our non-overlay windows as cursor owners");
        NSArray* floating =
            @[ window(1, self, 100), window(7, self, 101), window(8, other, 3), window(9, 99) ];
        const auto floatingSnapshot = reinterpret_cast<CFArrayRef>(floating);
        require(
            recaptureWindowTarget(floatingSnapshot, recaptureWindowAtPoint(excluded, hitTest)).id ==
                8,
            "recapture must select the hit floating window above a normal window");
        require(scrollWindowTarget(floatingSnapshot, self, QPoint(50, 50)).id == 9,
                "recapture policy must not change scrolling's normal-window contract");
        require(recaptureWindowTarget(floatingSnapshot, 9).id == 9,
                "native mouse hit identity must take precedence over decorative window bounds");
        require(recaptureWindowTarget(nullptr, 8).id == 0 &&
                    recaptureWindowTarget(floatingSnapshot, 99).id == 0 &&
                    recaptureWindowTarget(floatingSnapshot, 0).id == 0,
                "a vanished hit or desktop must not fall through to another application");
    }
    std::cout << "macOS focused-window, scroll, and recapture target tests passed\n";
}
