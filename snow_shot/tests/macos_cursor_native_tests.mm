#include "../src/platform/macos/windowcursorcoordinator.h"
#include "../src/presentation/pinned/pinnedwindowplatform.h"
#include "../../cmake/test-support/macos_native_input.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QEventLoop>
#include <QLineEdit>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()>& predicate) {
    if (predicate())
        return true;
    QEventLoop loop;
    QTimer poll;
    bool satisfied = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        satisfied = predicate();
        if (satisfied)
            loop.quit();
    });
    poll.start(1);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    // Native callbacks can run while a nested event loop unwinds. Preserve the
    // observed success; stability across display cycles is tested separately.
    return satisfied || predicate();
}

void settle() {
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
}

bool sessionLocked() {
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session)
        return true;
    const auto locked =
        static_cast<CFBooleanRef>(CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked")));
    const bool result =
        locked && CFGetTypeID(locked) == CFBooleanGetTypeID() && CFBooleanGetValue(locked);
    CFRelease(session);
    return result;
}

NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.internalWinId()).window;
}

class Surface final : public QWidget {
  public:
    Surface() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint) {
        setAutoFillBackground(true);
        setMouseTracking(true);
        resize(240, 180);
    }
    void releaseSurface() {
        destroy();
    }
};

class Fixture final {
  public:
    explicit Fixture(bool enabled) {
        previous = [NSWorkspace.sharedWorkspace.frontmostApplication retain];
        image.setObjectName(QStringLiteral("image"));
        toolbar.setObjectName(QStringLiteral("toolbar"));
        recognition.setObjectName(QStringLiteral("recognition"));
        const QRect available = qApp->primaryScreen()->availableGeometry();
        image.move(available.center() - QPoint(280, 130));
        toolbar.move(available.center() + QPoint(50, 120));
        recognition.move(available.center() + QPoint(50, -130));
        editor = new QLineEdit(&toolbar);
        editor->setGeometry(15, 20, 200, 35);
        editor->setText(QStringLiteral("focus must remain here"));
        for (Surface* window : {&image, &toolbar, &recognition})
            window->show();
        toolbar.windowHandle()->setTransientParent(image.windowHandle());
        recognition.windowHandle()->setTransientParent(image.windowHandle());
        if (enabled)
            snow_shot::platform::macos::configureWindowCursorUpdates(&image);
        for (Surface* window : {&image, &toolbar, &recognition})
            macRaiseTestWindow(window, NSScreenSaverWindowLevel + 20);
        macActivateApplication();
        require(waitFor([] { return bool(NSApp.active); }), "fixture application must activate");
        settle();
        focus(toolbar);
    }

    ~Fixture() {
        image.hide();
        toolbar.hide();
        recognition.hide();
        [NSApp yieldActivationToApplication:previous];
        [previous activateWithOptions:0];
        [previous release];
    }

    void focus(Surface& window) {
        window.activateWindow();
        [native(window) makeKeyAndOrderFront:nil];
        if (&window == &toolbar)
            editor->setFocus();
        const bool focused =
            waitFor([&] { return NSApp.active && NSApp.keyWindow == native(window); });
        if (!focused)
            NSLog(@"cursor fixture focus: active=%d key=%@ target=%@", NSApp.active,
                  NSApp.keyWindow, native(window));
        require(focused, "fixture could not establish keyboard focus");
    }

    void hover(QWidget& target, QPoint localPoint = QPoint(60, 100)) {
        const QPoint point = target.mapToGlobal(localPoint);
        macPostMove(point);
        const bool hovered = waitFor(
            [&] { return macWindowReceivesPoint(target.window(), point) && target.underMouse(); });
        if (!hovered) {
            const NSWindow* window = native(*target.window());
            NSLog(@"cursor hover failed: widget=%s point=(%d,%d) hit=%ld target=%ld "
                   "level=%ld underMouse=%d",
                  qPrintable(target.objectName()), point.x(), point.y(),
                  long([NSWindow windowNumberAtPoint:NSEvent.mouseLocation
                         belowWindowWithWindowNumber:0]),
                  long(window.windowNumber), long(window.level), target.underMouse());
        }
        require(hovered, "fixture window must receive native hover input");
    }

    void expect(QWidget& target, const QCursor& cursor, NSCursor* expected) {
        NSWindow* key = NSApp.keyWindow;
        QWidget* focusWidget = QApplication::focusWidget();
        target.setCursor(cursor);
        require(waitFor([&] { return NSCursor.currentCursor == expected; }),
                "non-key hovered window did not apply its changed cursor");
        require(NSApp.keyWindow == key && QApplication::focusWidget() == focusWidget,
                "cursor updates must not change key-window or editor focus");
    }

    Surface image, toolbar, recognition;
    QLineEdit* editor = nullptr;

  private:
    MacCursorRestore restore;
    NSRunningApplication* previous = nil;
};

void familyHoverAndCache(bool enabled) {
    Fixture f(enabled);
    f.image.setCursor(Qt::CrossCursor);
    f.hover(f.image);
    f.expect(f.image, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
    f.expect(f.image, QCursor(Qt::OpenHandCursor), NSCursor.openHandCursor);
    f.expect(f.image, QCursor(Qt::SizeHorCursor),
             [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionLeft
                                        inDirections:NSCursorFrameResizeDirectionsAll]);
    require(f.editor->hasFocus(), "toolbar editor must retain focus over the screenshot canvas");
    f.focus(f.recognition);
    f.expect(f.image, QCursor(Qt::CrossCursor), NSCursor.crosshairCursor);
    f.hover(f.toolbar);
    f.expect(f.toolbar, QCursor(Qt::PointingHandCursor), NSCursor.pointingHandCursor);
    f.hover(f.image);
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.crosshairCursor; }),
            "returning to an unchanged cached cursor must restore it");
    f.focus(f.toolbar);
    f.hover(f.recognition);
    f.expect(f.recognition, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);

    // Pin registration uses the real platform adapter; recording/screenshot roots
    // use the same shared entry point. Isolated fixture levels avoid manipulating
    // any windows of the user's running application.
    Surface pin, recording;
    pin.setObjectName(QStringLiteral("pin"));
    recording.setObjectName(QStringLiteral("recording"));
    pin.setGeometry(f.image.geometry());
    recording.setGeometry(f.recognition.geometry());
    auto platform = snow_shot::presentation::createPinnedWindowPlatform(&pin);
    pin.show();
    recording.show();
    require(platform->attach(), "pin platform attachment failed");
    snow_shot::platform::macos::configureWindowCursorUpdates(&recording);
    macRaiseTestWindow(&pin, NSScreenSaverWindowLevel + 21);
    macRaiseTestWindow(&recording, NSScreenSaverWindowLevel + 21);
    f.toolbar.windowHandle()->setTransientParent(pin.windowHandle());
    f.focus(f.toolbar);
    f.hover(pin);
    f.expect(pin, QCursor(Qt::OpenHandCursor), NSCursor.openHandCursor);
    f.toolbar.windowHandle()->setTransientParent(recording.windowHandle());
    f.hover(recording);
    f.expect(recording, QCursor(Qt::CrossCursor), NSCursor.crosshairCursor);
    f.expect(recording, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);

    require(platform->setInputTransparent(true), "pin click-through failed");
    // attach() restores the production pin level during both transparency changes.
    // Reapply the fixture's isolation band so the overlapping windows still test
    // click-through, rather than accidentally testing an occluded pin.
    macRaiseTestWindow(&pin, NSScreenSaverWindowLevel + 21);
    f.hover(f.image);
    f.expect(f.image, QCursor(Qt::PointingHandCursor), NSCursor.pointingHandCursor);
    require(platform->setInputTransparent(false), "pin click-through restoration failed");
    macRaiseTestWindow(&pin, NSScreenSaverWindowLevel + 21);
    f.hover(pin);
    f.expect(pin, QCursor(Qt::CrossCursor), NSCursor.crosshairCursor);
    recording.setAttribute(Qt::WA_TransparentForMouseEvents);
    recording.windowHandle()->setFlag(Qt::WindowTransparentForInput);
    f.hover(f.recognition);
    f.expect(f.recognition, QCursor(Qt::OpenHandCursor), NSCursor.openHandCursor);
}

void customOverrideAndSurface() {
    Fixture f(true);
    f.hover(f.image);
    QPixmap pixels(32, 32);
    pixels.fill(Qt::red);
    pixels.setDevicePixelRatio(2);
    f.image.setCursor(QCursor(pixels, 8, 8));
    const auto customCursorApplied = [] {
        NSCursor* cursor = NSCursor.currentCursor;
        NSBitmapImageRep* bitmap =
            [NSBitmapImageRep imageRepWithData:cursor.image.TIFFRepresentation];
        // Compare source samples, not a conversion from AppKit's calibrated RGB
        // to the display profile. Pure source red need not remain (1, 0, 0) there.
        NSUInteger sample[4] = {};
        if (bitmap.bitsPerSample != 8 || bitmap.samplesPerPixel < 3 || bitmap.samplesPerPixel > 4)
            return false;
        [bitmap getPixel:sample atX:8 y:8];
        return cursor.image.size.width == 16 && cursor.image.size.height == 16 &&
               bitmap.pixelsWide == 32 && bitmap.pixelsHigh == 32 && sample[0] == 255 &&
               sample[1] == 0 && sample[2] == 0;
    };
    require(waitFor(customCursorApplied),
            "custom bitmap cursor must preserve color and device-independent size");

    {
        // Let AppKit's deferred display-cycle tracking run with no pointer or
        // QWidget cursor changes. This catches resets after the first update.
        QEventLoop idle;
        QTimer::singleShot(1000, &idle, &QEventLoop::quit);
        idle.exec();
        require(customCursorApplied(),
                "stationary bitmap cursor must survive native display updates");
    }
    QApplication::setOverrideCursor(Qt::CrossCursor);
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.crosshairCursor; }),
            "stationary override cursor must be applied");
    QApplication::setOverrideCursor(Qt::IBeamCursor);
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.IBeamCursor; }),
            "nested override cursor must be applied");
    QApplication::restoreOverrideCursor();
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.crosshairCursor; }),
            "nested override restoration must be applied");
    QApplication::restoreOverrideCursor();
    require(waitFor(customCursorApplied),
            "final override restoration must restore the bitmap cursor");
    f.expect(f.image, QCursor(Qt::OpenHandCursor), NSCursor.openHandCursor);

    Surface popup;
    popup.setObjectName(QStringLiteral("popup"));
    popup.setGeometry(f.image.geometry());
    popup.show();
    popup.windowHandle()->setTransientParent(f.image.windowHandle());
    macRaiseTestWindow(&popup, NSScreenSaverWindowLevel + 21);
    f.focus(f.toolbar);
    f.hover(popup);
    f.expect(popup, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
    popup.hide();
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.openHandCursor; }),
            "popup closure must restore the cursor under a stationary pointer");

    f.image.hide();
    f.image.releaseSurface();
    f.image.show();
    macRaiseTestWindow(&f.image, NSScreenSaverWindowLevel + 20);
    f.focus(f.toolbar);
    f.hover(f.image);
    f.expect(f.image, QCursor(Qt::CrossCursor), NSCursor.crosshairCursor);

    for (QScreen* screen : QGuiApplication::screens()) {
        f.image.move(screen->availableGeometry().center() - QPoint(120, 90));
        f.focus(f.toolbar);
        f.hover(f.image);
        f.expect(f.image, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
        f.expect(f.image, QCursor(Qt::CrossCursor), NSCursor.crosshairCursor);
    }
}

void childCursorSelection() {
    Fixture f(true);
    f.focus(f.recognition);
    f.toolbar.setCursor(Qt::OpenHandCursor);
    f.editor->setObjectName(QStringLiteral("editor"));
    f.hover(*f.editor, QPoint(10, 10));
    require(!f.editor->internalWinId(), "ordinary editor must remain a non-native child");
    f.expect(*f.editor, QCursor(Qt::PointingHandCursor), NSCursor.pointingHandCursor);
    f.expect(*f.editor, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
    f.toolbar.setCursor(Qt::CrossCursor);
    settle();
    require(NSCursor.currentCursor == NSCursor.IBeamCursor,
            "parent cursor changes must preserve the hovered child's cursor");

    f.editor->setAttribute(Qt::WA_NativeWindow);
    require(f.editor->internalWinId(), "native child fixture must materialize its own view");
    f.hover(*f.editor, QPoint(10, 10));
    f.expect(*f.editor, QCursor(Qt::PointingHandCursor), NSCursor.pointingHandCursor);
    f.expect(*f.editor, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
    f.hover(f.toolbar);
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.crosshairCursor; }),
            "leaving a native child must restore the parent cursor");
}

void grabsAndOcclusion() {
    Fixture f(true);
    f.hover(f.recognition);
    f.expect(f.recognition, QCursor(Qt::IBeamCursor), NSCursor.IBeamCursor);
    f.hover(f.image);
    f.expect(f.image, QCursor(Qt::ClosedHandCursor), NSCursor.closedHandCursor);
    f.image.grabMouse();
    macPostMove(f.recognition.mapToGlobal(QPoint(60, 100)));
    settle();
    require(NSCursor.currentCursor == NSCursor.closedHandCursor,
            "explicit drag ownership must survive crossing another managed window");
    f.image.releaseMouse();
    f.hover(f.recognition);
    require(waitFor([] { return NSCursor.currentCursor == NSCursor.IBeamCursor; }),
            "releasing an explicit grab must restore hover ownership");

    {
        MacMouseDrag drag(f.image.mapToGlobal(QPoint(60, 100)));
        drag.moveTo(f.recognition.mapToGlobal(QPoint(60, 100)));
        f.image.setCursor(Qt::OpenHandCursor);
        require(waitFor([] { return NSCursor.currentCursor == NSCursor.openHandCursor; }),
                "implicit pressed-window ownership must survive crossing another window");
        drag.finish();
        require(waitFor([] { return NSCursor.currentCursor == NSCursor.IBeamCursor; }),
                "the last native release must restore hover ownership");
    }

    Surface unrelated;
    unrelated.setObjectName(QStringLiteral("unrelated"));
    unrelated.setGeometry(f.image.geometry());
    unrelated.show();
    macRaiseTestWindow(&unrelated, NSScreenSaverWindowLevel + 22);
    f.focus(f.toolbar);
    f.hover(unrelated);
    [[NSCursor arrowCursor] set];
    f.image.setCursor(Qt::CrossCursor);
    settle();
    require(NSCursor.currentCursor != NSCursor.crosshairCursor,
            "an unrelated foreground window must shield managed windows underneath");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (sessionLocked()) {
        std::cout << "SKIP: native cursor fixture requires an unlocked desktop\n";
        return 77;
    }
    if (!macCanPostMouseEvents()) {
        std::cout << "SKIP: native cursor fixture requires existing event-posting permission\n";
        return 77;
    }
    try {
        familyHoverAndCache(!app.arguments().contains(QStringLiteral("--without-coordinator")));
        customOverrideAndSurface();
        childCursorSelection();
        grabsAndOcclusion();
        settle();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "macOS native cursor and focus tests passed\n";
}
