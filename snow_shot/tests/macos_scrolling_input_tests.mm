#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "physical_key_test_support.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QCursor>
#include <QRegion>
#include <QWidget>
#include <QWindow>
#include <QWheelEvent>
#include <QEventLoop>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ScrollTarget : public QWidget {
  public:
    ScrollTarget() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint) {}
    int wheels = 0;

  protected:
    void wheelEvent(QWheelEvent* event) override {
        ++wheels;
        event->accept();
    }
};

class Overlay final : public ScrollTarget {
  public:
    void releaseSurface() {
        destroy();
    }
};

void qtTransparencyTracksWindowAndSurfaceLifecycle() {
    using snow_shot::platform::setScreenshotInputTransparent;
    Overlay overlay;
    setScreenshotInputTransparent(&overlay, true);
    require(!overlay.internalWinId(), "enabling transparency must not create a native surface");
    overlay.show();
    QApplication::processEvents();
    const WId surface = overlay.internalWinId();
    QWindow* handle = overlay.windowHandle();
    require(handle && !handle->flags().testFlag(Qt::WindowTransparentForInput),
            "scrolling capture must preserve Qt keyboard input when shown");
    setScreenshotInputTransparent(&overlay, true);
    overlay.move(overlay.pos() + QPoint(20, 20));
    require(overlay.internalWinId() == surface && overlay.windowHandle() == handle,
            "input ownership must preserve the current native surface during activation and move");
    overlay.hide();
    require(!handle->flags().testFlag(Qt::WindowTransparentForInput),
            "hiding scrolling capture must preserve the original Qt input flag");
    overlay.show();
    QApplication::processEvents();
    require(!overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
            "showing an active capture must preserve Qt keyboard input");
    setScreenshotInputTransparent(&overlay, false);
    require(!overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
            "stopping scrolling capture must preserve Qt keyboard input");
    overlay.windowHandle()->setFlag(Qt::WindowTransparentForInput, true);
    setScreenshotInputTransparent(&overlay, true);
    setScreenshotInputTransparent(&overlay, false);
    require(overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
            "a preexisting Qt transparency flag must survive capture cleanup");
    overlay.windowHandle()->setFlag(Qt::WindowTransparentForInput, false);
    setScreenshotInputTransparent(&overlay, true);
    overlay.hide();
    overlay.releaseSurface();
    require(!overlay.internalWinId(), "native surface release must remain complete");
    overlay.show();
    QApplication::processEvents();
    require(!overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
            "a replacement surface must preserve Qt keyboard input");
    setScreenshotInputTransparent(&overlay, false);
    require(!overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
            "replacement surface cleanup must preserve its original input flag");
    overlay.hide();
    overlay.releaseSurface();
    setScreenshotInputTransparent(&overlay, false);
    require(!overlay.internalWinId(), "cleanup must never recreate a released native surface");
}

void scrollingPreservesShortcutDispatch() {
    using snow_shot::platform::setScreenshotInputTransparent;
    using snow_shot::presentation::WindowShortcutManager;
    Overlay overlay;
    overlay.setFocusPolicy(Qt::StrongFocus);
    overlay.resize(240, 160);
    WindowShortcutManager manager;
    manager.addScopeWindow(&overlay);
    int activations = 0;
    const QKeyCombination combinations[] = {
        QKeyCombination(Qt::Key_Escape), QKeyCombination(Qt::Key_Return),
        QKeyCombination(Qt::ControlModifier, Qt::Key_C), QKeyCombination(Qt::Key_Left)};
    for (const auto combination : combinations) {
        WindowShortcutManager::Binding binding;
        binding.id = QString::number(combination.toCombined());
        binding.keyCombinations = {combination};
        if (combination.key() == Qt::Key_Escape)
            binding.activationTrigger = WindowShortcutManager::Binding::ActivationTrigger::Release;
        binding.activate = [&](const auto&) {
            ++activations;
            return true;
        };
        require(manager.addBinding(&overlay, std::move(binding)) != 0,
                "register scrolling capture shortcut");
    }
    overlay.show();
    QApplication::processEvents();
    const WId surface = overlay.internalWinId();
    NSView* view = QApplication::platformName() == QStringLiteral("cocoa")
                       ? reinterpret_cast<NSView*>(surface)
                       : nil;
    for (const bool scrolling : {false, true, false, true}) {
        setScreenshotInputTransparent(&overlay, scrolling);
        require(overlay.internalWinId() == surface &&
                    !overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
                "entering scrolling mode must preserve the keyboard-enabled surface");
        if (view) {
            require(view.window.canBecomeKeyWindow && view.acceptsFirstResponder,
                    "scrolling capture must retain native key window and responder eligibility");
            require(view.window.ignoresMouseEvents == scrolling,
                    "mouse pass-through must track scrolling without disabling the keyboard");
        }
        const int before = activations;
        for (int index = 0; index != 4; ++index) {
            const auto combination = combinations[index];
            for (const auto type : {QEvent::KeyPress, QEvent::KeyRelease}) {
                if (view) {
                    // Deliver through Qt's real Cocoa responder path without posting
                    // system input, changing application activation, or moving the pointer.
                    const unsigned short codes[] = {53, 36, 8, 123};
                    NSString* characters[] = {@"\x1b", @"\r", @"c", @"\uf702"};
                    NSEvent* event =
                        [NSEvent keyEventWithType:type == QEvent::KeyPress ? NSEventTypeKeyDown
                                                                           : NSEventTypeKeyUp
                                               location:NSZeroPoint
                                          modifierFlags:combination.keyboardModifiers() ==
                                                                Qt::ControlModifier
                                                            ? NSEventModifierFlagCommand
                                                            : 0
                                              timestamp:1
                                           windowNumber:view.window.windowNumber
                                                context:nil
                                             characters:characters[index]
                            charactersIgnoringModifiers:characters[index]
                                              isARepeat:NO
                                                keyCode:codes[index]];
                    if (type == QEvent::KeyPress)
                        [view keyDown:event];
                    else
                        [view keyUp:event];
                } else {
                    PhysicalKeyEvent event(type, combination.key(),
                                           combination.keyboardModifiers());
                    QCoreApplication::sendEvent(&overlay, &event);
                }
            }
            require(activations == before + index + 1,
                    "each shortcut must dispatch once immediately after a scrolling transition");
        }
    }
    setScreenshotInputTransparent(&overlay, false);
}

void nativeTransparencyRoutesToUnderlyingWindow() {
    using snow_shot::platform::setScreenshotInputTransparent;
    // Place the selection under the existing pointer: activation must work without
    // movement, and this fixture need not warp the user's cursor or post input.
    const QPoint cursor = QCursor::pos();
    ScrollTarget underlying;
    underlying.setGeometry(QRect(cursor - QPoint(100, 100), QSize(240, 240)));
    underlying.show();
    Overlay overlay;
    overlay.setGeometry(underlying.geometry());
    overlay.show();
    QApplication::processEvents();
    for (int reuse = 0; reuse != 2; ++reuse) {
        overlay.show();
        overlay.raise();
        QApplication::processEvents();
        NSWindow* native = reinterpret_cast<NSView*>(overlay.winId()).window;
        NSWindow* target = reinterpret_cast<NSView*>(underlying.winId()).window;
        snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
        target.level = native.level - 1;
        [target orderFrontRegardless];
        [native orderFrontRegardless];
        const auto wheelRecipient = [&] {
            const int beforeOverlay = overlay.wheels;
            const int beforeTarget = underlying.wheels;
            CGEventRef move =
                CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                        CGPointMake(cursor.x(), cursor.y()), kCGMouseButtonLeft);
            CGEventPost(kCGHIDEventTap, move);
            CFRelease(move);
            CGEventRef wheel =
                CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, -1);
            CGEventSetLocation(wheel, CGPointMake(cursor.x(), cursor.y()));
            CGEventPost(kCGHIDEventTap, wheel);
            CFRelease(wheel);
            QEventLoop loop;
            QTimer poll;
            poll.setInterval(1);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (overlay.wheels != beforeOverlay || underlying.wheels != beforeTarget)
                    loop.quit();
            });
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            poll.start();
            loop.exec();
            if (underlying.wheels > beforeTarget)
                return 1;
            return overlay.wheels > beforeOverlay ? 2 : 0;
        };
        const QRect hole(40, 40, 140, 140);
        overlay.setMask(QRegion(overlay.rect()).subtracted(QRegion(hole)));
        require(!native.ignoresMouseEvents, "a QWidget mask alone leaves native input enabled");
        if (reuse == 0)
            require(wheelRecipient() != 1,
                    "masked overlay blocks wheel delivery to the underlying window");
        setScreenshotInputTransparent(&overlay, true);
        require(native.ignoresMouseEvents,
                "scrolling enables whole-window transparency without pointer movement");
        require(!overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
                "mouse transparency must preserve Qt keyboard input");
        native.ignoresMouseEvents = NO;
        require(native.ignoresMouseEvents,
                "Qt/AppKit mouse tracking must not undo scrolling pass-through");
        require(wheelRecipient() == 1, "WindowServer must deliver the original wheel underneath");
        overlay.clearMask();
        require(native.ignoresMouseEvents && wheelRecipient() == 1,
                "whole-window transparency must not depend on the visual mask");
        const WId surface = overlay.winId();
        overlay.move(overlay.pos() + QPoint(20, 20));
        require(native.ignoresMouseEvents && overlay.winId() == surface,
                "moving the overlay must preserve transparency and its native surface");
        setScreenshotInputTransparent(&overlay, false);
        require(!native.ignoresMouseEvents, "stopping scrolling restores native input");
        setScreenshotInputTransparent(&overlay, true);
        overlay.hide();
        require(!native.ignoresMouseEvents, "hiding restores the native input state");
        overlay.show();
        QApplication::processEvents();
        require(native.ignoresMouseEvents,
                "showing an active scrolling session restores pass-through");
        overlay.hide();
        overlay.releaseSurface();
        setScreenshotInputTransparent(&overlay, false);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qtTransparencyTracksWindowAndSurfaceLifecycle();
    scrollingPreservesShortcutDispatch();
    if (app.arguments().contains(QStringLiteral("--shortcuts-only")))
        return 0;
    if (QApplication::platformName() == QStringLiteral("cocoa")) {
        if (!CGPreflightPostEventAccess()) {
            std::cout << "SKIP: native wheel fixture requires existing event-posting permission\n";
            return 77;
        }
        nativeTransparencyRoutesToUnderlyingWindow();
    }
    return 0;
}
