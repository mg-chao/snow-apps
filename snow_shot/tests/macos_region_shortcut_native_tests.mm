#include "snow_shot/presentation/windowcloseshortcut.h"
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

bool postKey(CGKeyCode code, CGEventFlags flags) {
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, code, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, code, false);
    if (down == nullptr || up == nullptr) {
        if (down != nullptr)
            CFRelease(down);
        if (up != nullptr)
            CFRelease(up);
        return false;
    }
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

    int closes = 0;
    int selects = 0;
    snow_shot::presentation::installWindowCloseShortcut(&overlay, [&] { ++closes; });
    snow_shot::presentation::WindowShortcutManager::Binding select;
    select.id = QStringLiteral("native.select");
    select.shortcutBindings = {
        snow_shot::shortcuts::bindingFromPortableText(QStringLiteral("Ctrl+A"))};
    select.activate = [&](const auto&) {
        ++selects;
        return true;
    };
    require(manager.addBinding(&overlay, std::move(select)) != 0, "register physical A");
    overlay.show();
    [NSApp activate];
    overlay.raise();
    overlay.activateWindow();
    overlay.setFocus();
    const bool active =
        waitFor([&] { return overlay.isActiveWindow() && [NSApp keyWindow] != nil; });
    const bool forwardPosted = active && postKey(48, kCGEventFlagMaskControl);
    const bool forwardDelivered = forwardPosted && waitFor([&] { return forwardCount == 1; });
    const bool reversePosted =
        forwardDelivered && postKey(48, kCGEventFlagMaskControl | kCGEventFlagMaskShift);
    const bool reverseDelivered = reversePosted && waitFor([&] { return reverseCount == 1; });
    const bool selectDelivered = reverseDelivered && postKey(0, kCGEventFlagMaskCommand) &&
                                 waitFor([&] { return selects == 1; });
    const bool closeDelivered = selectDelivered && postKey(13, kCGEventFlagMaskCommand) &&
                                waitFor([&] { return closes == 1; });
    overlay.hide();
    if (previous != nil) {
        [previous activateWithOptions:0];
        [previous release];
    }

    require(active, "native overlay did not own keyboard focus");
    require(forwardPosted && reversePosted, "native Control+Tab events could not be posted");
    require(forwardDelivered && reverseDelivered && forwardCount == 1 && reverseCount == 1,
            "native Control+Tab and Control+Shift+Tab must reach the region shortcuts");
    require(selectDelivered && closeDelivered, "native A and fixed close must dispatch physically");
    std::cout << "macOS native region shortcuts passed\n";
}
