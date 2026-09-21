#include "snow_shot/platform/screenshotnative.h"
#include "../src/platform/macos/screenshotinputregion_p.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QCursor>
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

void inputRegionRetainsOverlayInteractions() {
    using snow_shot::platform::detail::ScreenshotInputRegion;
    ScreenshotInputRegion region;
    const QRect selection(20, 30, 200, 160);
    const QRect preview(180, 40, 80, 140);
    region.passThrough = QRegion(selection).subtracted(QRegion(preview));
    require(region.transparentAt(QPoint(100, 100), true), "selection forwards native input");
    require(!region.transparentAt(QPoint(200, 100), true),
            "overlapping preview remains interactive");
    require(!region.transparentAt(QPoint(19, 100), true), "outside border remains interactive");
    require(!region.transparentAt(QPoint(100, 100), false), "hidden overlays restore input");
    region.press(0);
    require(!region.transparentAt(QPoint(100, 100), true), "border and trim drags retain input");
    region.press(1);
    region.release(0);
    require(!region.transparentAt(QPoint(100, 100), true), "another held button retains input");
    region.release(1);
    require(region.transparentAt(QPoint(100, 100), true), "release restores pass-through");
    region.passThrough = QRegion(QRect(0, 0, 400, 300));
    require(region.transparentAt(QPoint(0, 0), true), "full-display selections forward input");
    region.passThrough = {};
    require(!region.transparentAt(QPoint(100, 100), true), "leaving scrolling restores input");
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

void nativeRegionRoutesToUnderlyingWindow() {
    using snow_shot::platform::setScreenshotInputPassThroughRegion;
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
        setScreenshotInputPassThroughRegion(&overlay, QRegion(hole));
        require(native.ignoresMouseEvents,
                "stationary pointer inside selection enables transparency");
        require(wheelRecipient() == 1, "WindowServer must deliver the original wheel underneath");
        const QRegion thumbnail(QRect(80, 80, 50, 50));
        setScreenshotInputPassThroughRegion(&overlay, QRegion(hole).subtracted(thumbnail));
        require(!native.ignoresMouseEvents, "preview overlapping the pointer receives input");
        overlay.clearMask();
        setScreenshotInputPassThroughRegion(&overlay, QRegion(overlay.rect()));
        require(native.ignoresMouseEvents && wheelRecipient() == 1,
                "full-display hole routes input even though its Qt mask is empty");
        // Move out through the hole and back. Both transitions must be observed
        // even though the transparent window no longer receives pointer events.
        const auto moveAndWait = [&](const QPoint& point, bool transparent) {
            QEventLoop loop;
            QTimer poll;
            poll.setInterval(1);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (QCursor::pos() == point && native.ignoresMouseEvents == transparent)
                    loop.quit();
            });
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            CGEventRef move = CGEventCreateMouseEvent(
                nullptr, kCGEventMouseMoved, CGPointMake(point.x(), point.y()), kCGMouseButtonLeft);
            CGEventPost(kCGHIDEventTap, move);
            CFRelease(move);
            poll.start();
            loop.exec();
            require(native.ignoresMouseEvents == transparent,
                    "native pointer monitoring must update regional transparency");
        };
        setScreenshotInputPassThroughRegion(&overlay, QRegion(hole));
        moveAndWait(overlay.mapToGlobal(QPoint(20, 100)), false);
        moveAndWait(cursor, true);
        require(wheelRecipient() == 1, "scrolling works after leaving and reentering the hole");
        setScreenshotInputPassThroughRegion(&overlay, {});
        require(!native.ignoresMouseEvents, "stopping scrolling restores native input");
        setScreenshotInputPassThroughRegion(&overlay, QRegion(hole));
        overlay.hide();
        require(!native.ignoresMouseEvents, "hiding restores the native input state");
        overlay.show();
        QApplication::processEvents();
        require(native.ignoresMouseEvents, "showing an active selection restores pass-through");
        overlay.hide();
        overlay.releaseSurface();
        setScreenshotInputPassThroughRegion(&overlay, {});
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    inputRegionRetainsOverlayInteractions();
    if (QApplication::platformName() == QStringLiteral("cocoa")) {
        if (!CGPreflightPostEventAccess()) {
            std::cout << "SKIP: native wheel fixture requires existing event-posting permission\n";
            return 77;
        }
        nativeRegionRoutesToUnderlyingWindow();
    }
    return 0;
}
