#include "snow_shot/presentation/screenshotregiontypeshortcut.h"
#include "snow_shot/presentation/windowshortcutmanager.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}

bool postOptionTab(bool reverse) {
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 48, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 48, false);
    if (down == nullptr || up == nullptr) {
        if (down != nullptr)
            CFRelease(down);
        if (up != nullptr)
            CFRelease(up);
        return false;
    }
    const CGEventFlags flags =
        kCGEventFlagMaskAlternate | (reverse ? kCGEventFlagMaskShift : CGEventFlags{0});
    CGEventSetFlags(down, flags);
    CGEventSetFlags(up, flags);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    uint32_t displays = 0;
    if (QApplication::platformName() != QStringLiteral("cocoa") ||
        CGGetActiveDisplayList(0, nullptr, &displays) != kCGErrorSuccess || displays == 0 ||
        !AXIsProcessTrusted()) {
        std::cout << "SKIP: native display and Accessibility permission required\n";
        return 77;
    }

    NSRunningApplication* previous = [[[NSWorkspace sharedWorkspace] frontmostApplication] retain];
    QWidget overlay;
    overlay.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    overlay.setAttribute(Qt::WA_ShowWithoutActivating, true);
    overlay.setFocusPolicy(Qt::StrongFocus);
    overlay.resize(180, 80);

    snow_shot::presentation::WindowShortcutManager manager;
    manager.addScopeWindow(&overlay);
    int forwardCount = 0;
    int reverseCount = 0;
    for (const bool reverse : {false, true}) {
        snow_shot::presentation::WindowShortcutManager::Binding binding;
        binding.id = reverse ? QStringLiteral("region.previous") : QStringLiteral("region.next");
        binding.keyCombinations = {screenshotRegionTypeCycleKey(reverse)};
        binding.activate = [&forwardCount, &reverseCount, reverse](const auto&) {
            ++(reverse ? reverseCount : forwardCount);
            return true;
        };
        require(manager.addBinding(&overlay, std::move(binding)) != 0,
                "native region shortcut registration failed");
    }

    overlay.show();
    [NSApp activate];
    overlay.raise();
    overlay.activateWindow();
    overlay.setFocus();
    const bool active =
        waitFor([&] { return overlay.isActiveWindow() && [NSApp keyWindow] != nil; });
    const bool forwardPosted = active && postOptionTab(false);
    const bool forwardDelivered = forwardPosted && waitFor([&] { return forwardCount == 1; });
    const bool reversePosted = forwardDelivered && postOptionTab(true);
    const bool reverseDelivered = reversePosted && waitFor([&] { return reverseCount == 1; });
    overlay.hide();
    if (previous != nil) {
        [previous activateWithOptions:0];
        [previous release];
    }

    require(active, "native overlay did not own keyboard focus");
    require(forwardPosted && reversePosted, "native Option+Tab events could not be posted");
    require(forwardDelivered && reverseDelivered && forwardCount == 1 && reverseCount == 1,
            "native Option+Tab and Option+Shift+Tab must reach the region shortcuts");
    std::cout << "macOS native region shortcuts passed\n";
}
