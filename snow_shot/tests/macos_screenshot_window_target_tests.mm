#include "../src/platform/macos/screenshotwindowtarget_p.h"

#import <Foundation/Foundation.h>
#include <cstdlib>
#include <iostream>

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
    }
    std::cout << "macOS focused-window and scroll target tests passed\n";
}
