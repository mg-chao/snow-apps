#include "snow_shot/platform/screenshotnative.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QCursor>
#include <QRegion>
#include <QWidget>
#include <QWheelEvent>
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

void nativeTransparencyRoutesToUnderlyingWindow() {
    using snow_shot::platform::setScreenshotInputTransparent;
    // Place the selection under the existing pointer: activation must work without
    // movement, and this fixture need not warp the user's cursor or post input.
    const QPoint cursor = QCursor::pos();
    ScrollTarget underlying;
    underlying.setGeometry(QRect(cursor - QPoint(100, 100), QSize(240, 240)));
    underlying.show();
    Overlay overlay;
    snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
    overlay.setGeometry(underlying.geometry());
    overlay.show();
    QApplication::processEvents();
    for (int reuse = 0; reuse != 2; ++reuse) {
        overlay.show();
        overlay.raise();
        QApplication::processEvents();
        NSWindow* native = reinterpret_cast<NSView*>(overlay.winId()).window;
        NSWindow* target = reinterpret_cast<NSView*>(underlying.winId()).window;
        require(native.styleMask & NSWindowStyleMaskNonactivatingPanel,
                "scrolling overlay must retain its nonactivating role after native reuse");
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
    snow_shot::platform::initializeScreenshotWindowPolicy();
    if (QApplication::platformName() == QStringLiteral("cocoa")) {
        if (!CGPreflightPostEventAccess()) {
            std::cout << "SKIP: native wheel fixture requires existing event-posting permission\n";
            return 77;
        }
        nativeTransparencyRoutesToUnderlyingWindow();
    }
    return 0;
}
