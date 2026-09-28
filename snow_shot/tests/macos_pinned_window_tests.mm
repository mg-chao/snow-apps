#include "presentation/pinned/pinnedwindowplatform.h"
#include "platform/macos/capturewindowlayers_p.h"
#include "snow_shot/presentation/mousereleaseactioncontroller.h"
#include "snow_shot/platform/screenshotnative.h"
#include "widgets/modal.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QWindow>
#include <QPushButton>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace snow_shot::presentation;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void events(int milliseconds = 50) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        qApp->processEvents();
        QThread::msleep(1);
    }
}
class Receiver final : public QWidget {
  public:
    Receiver() {
        setGeometry(150, 150, 300, 200);
    }
    void mousePressEvent(QMouseEvent*) override {
        std::puts("press");
        std::fflush(stdout);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        std::puts("release");
        std::fflush(stdout);
    }
};
class DismissWindow final : public QWidget {
  public:
    MouseReleaseActionController release;
    void mousePressEvent(QMouseEvent* event) override {
        static_cast<void>(release.arm(this, event->button(), [this] { hide(); }));
    }
};
class ControlledWindow final : public QWidget {
  public:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
        m_origin = event->globalPosition();
        m_geometry = geometry();
        m_resizing = event->position().x() >= width() - 6;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!event->buttons().testFlag(Qt::LeftButton) || !m_origin.has_value())
            return;
        const QPoint delta = (event->globalPosition() - *m_origin).toPoint();
        if (m_resizing)
            setGeometry(m_geometry.adjusted(0, 0, delta.x(), delta.y()));
        else
            setGeometry(m_geometry.translated(delta));
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        m_origin.reset();
    }

  private:
    std::optional<QPointF> m_origin;
    QRect m_geometry;
    bool m_resizing = false;
};
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
void hiddenPlacementCommitsBeforeShow() {
    for (QScreen* screen : QGuiApplication::screens()) {
        QWidget widget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        widget.setGeometry(QRect(screen->geometry().topLeft() + QPoint(100, 100), QSize(200, 150)));
        widget.winId();
        auto platform = createPinnedWindowPlatform(&widget);
        require(platform->attach(), "hidden pin platform attachment failed");
        const int usableTop = screen->availableGeometry().top() - screen->geometry().top();
        PinnedPlacement requested{screen->name(), screen->serialNumber(),
                                  QPointF(70.5, usableTop - 10), QSize(321, 181)};
        for (int presentation = 0; presentation != 2; ++presentation) {
            require(!widget.isVisible(), "placement preparation must not expose the pin");
            require(platform->applyStablePlacement(requested, screen),
                    "hidden placement must commit to the native window before verification");
            const auto actual = platform->placement();
            const QPoint expectedOrigin =
                (QPointF(screen->geometry().topLeft()) + requested.position).toPoint();
            const QRect expected(expectedOrigin, requested.windowSize);
            require(actual && platform->windowGeometry() == expected &&
                        widget.geometry() == expected && !widget.isVisible(),
                    "hidden QWidget and native geometry must agree without showing the window");
            widget.show();
            events();
            require(widget.geometry() == expected && platform->windowGeometry() == expected,
                    "the first visible frame must preserve the prepared pin geometry");
            widget.hide();
            requested.position += QPointF(43.25, 71.5);
            requested.windowSize += QSize(30, 20);
        }
    }
}

void auxiliaryOwnershipRequiresARealOwner() {
    QWidget pin;
    auto platform = createPinnedWindowPlatform(&pin);
    require(pin.windowHandle() == nullptr, "the pin must start without a native window");
    const auto auxiliaryPlatform = [](QWidget& widget) {
        return widget.findChild<QObject*>(QStringLiteral("snowPinnedWindowPlatform"),
                                          Qt::FindDirectChildrenOnly);
    };
    QWidget unrelated;
    unrelated.show();
    require(auxiliaryPlatform(unrelated) == nullptr,
            "missing native handles must not associate unrelated top-level windows");
    QWidget decoration(&unrelated, Qt::Tool | Qt::WindowTransparentForInput);
    decoration.show();
    require(auxiliaryPlatform(decoration) == nullptr,
            "unrelated descendants must not inherit a pinned auxiliary policy");
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        require(reinterpret_cast<NSView*>(decoration.winId()).window.ignoresMouseEvents,
                "unrelated transparent tools must retain native click-through");
    }

    QWidget ownedChild(&pin, Qt::Tool);
    ownedChild.show();
    require(auxiliaryPlatform(ownedChild) != nullptr,
            "a QWidget-owned auxiliary must still receive its pin's policy");
    QWidget transient(nullptr, Qt::Tool);
    static_cast<void>(transient.winId());
    require(pin.windowHandle() != nullptr,
            "showing the owned child must create its owner's surface");
    transient.windowHandle()->setTransientParent(pin.windowHandle());
    transient.show();
    require(auxiliaryPlatform(transient) != nullptr,
            "a native transient auxiliary must still receive its pin's policy");
    QWidget other;
    other.show();
    require(auxiliaryPlatform(other) == nullptr,
            "a created pin must not adopt an unrelated top-level window either");
}

void nativePolicies(bool focus) {
    auxiliaryOwnershipRequiresARealOwner();
    hiddenPlacementCommitsBeforeShow();
    QWidget widget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    widget.setAttribute(Qt::WA_TranslucentBackground);
    widget.winId();
    NSView* view = reinterpret_cast<NSView*>(widget.internalWinId());
    NSWindow* window = view.window;
    const NSWindowStyleMask originalStyleMask = window.styleMask;
    const bool originalMovable = window.movable;
    const bool originalMovableByWindowBackground = window.movableByWindowBackground;
    const bool originalHasShadow = window.hasShadow;
    auto platform = createPinnedWindowPlatform(&widget);
    require(platform->attach(), "Cocoa panel attachment failed");
    constexpr NSWindowStyleMask nativeChrome = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskMiniaturizable |
                                               NSWindowStyleMaskResizable;
    require((window.styleMask & nativeChrome) == 0,
            "pin must leave border interactions to its controlled geometry path");
    require(!window.movable && !window.movableByWindowBackground,
            "pin must leave background dragging to its controlled geometry path");
    require(!window.hasShadow, "pin must not use the native window shadow");
    widget.show();
    events();
    require(window.level > CGWindowLevelForKey(kCGMainMenuWindowLevelKey) &&
                window.level > CGWindowLevelForKey(kCGDockWindowLevelKey) &&
                !window.hidesOnDeactivate,
            "always-on-top pin must stay above the menu bar and Dock when inactive");
    QScreen* screen = widget.screen();
    PinnedPlacement placement{
        screen->name(), screen->serialNumber(),
        QPointF(120 + 1 / screen->devicePixelRatio(), 120 + 1 / screen->devicePixelRatio()),
        QSize(321, 181)};
    require(platform->applyPlacement(placement, screen), "fractional Cocoa placement failed");
    events();
    const auto actual = platform->placement();
    require(actual && actual->windowSize == placement.windowSize &&
                actual->position == QPointF(placement.position.toPoint()),
            "Cocoa placement must quantize the logical origin once and preserve size");
    require(window.frame.size.width == 321 && window.frame.size.height == 181,
            "native Cocoa frame must use logical points even on Retina");
    const auto originalPlacement = placement;
    placement.position += QPointF(.13, .21);
    require(platform->applyPlacement(placement, screen),
            "subpixel pointer positions must accept logical-frame rounding");
    const auto aligned = platform->placement();
    require(aligned && aligned->windowSize == placement.windowSize &&
                widget.size() == placement.windowSize &&
                aligned->position == QPointF(placement.position.toPoint()),
            "subpixel movement must preserve size and read back the committed logical position");
    require(platform->applyPlacement(originalPlacement, screen), "restore fractional placement");
    auto edgePlacement = originalPlacement;
    const int usableTop = screen->availableGeometry().top() - screen->geometry().top();
    edgePlacement.position = QPointF(70, usableTop - 10);
    require(platform->applyPlacement(edgePlacement, screen),
            "pin movement must remain valid at the menu-bar boundary");
    const auto constrained = platform->placement();
    require(
        constrained && constrained->windowSize == edgePlacement.windowSize &&
            constrained->position == edgePlacement.position &&
            widget.geometry().topLeft() ==
                screen->geometry().topLeft() + constrained->position.toPoint(),
        "topmost pins must overlap the menu bar with matching Qt geometry and placement readback");
    auto dockPlacement = originalPlacement;
    dockPlacement.position = QPointF(70, screen->geometry().height() - 20);
    require(platform->applyPlacement(dockPlacement, screen),
            "topmost pin placement must allow overlapping the Dock");
    const auto dockActual = platform->placement();
    require(dockActual && dockActual->position == dockPlacement.position &&
                widget.geometry().topLeft() ==
                    screen->geometry().topLeft() + dockPlacement.position.toPoint(),
            "Dock overlap must preserve native and Qt geometry");
    require(platform->applyPlacement(originalPlacement, screen), "restore edge placement");
    QWidget floatingTool(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    NSWindow* tool = reinterpret_cast<NSView*>(floatingTool.winId()).window;
    require(window.level > tool.level,
            "an ordinary floating tool must not occupy the always-on-top pin's stacking band");
    require(platform->setStaysOnTop(false) && window.level == NSNormalWindowLevel,
            "disabling always-on-top must restore the normal window band");
    require(platform->attach() && window.level == NSNormalWindowLevel,
            "reattaching must preserve the always-on-top opt-out");
    require(platform->applyPlacement(edgePlacement, screen),
            "normal pins must still accept placement near the menu bar");
    const auto normalPlacement = platform->placement();
    require(normalPlacement && normalPlacement->position == QPointF(70, usableTop),
            "disabling always-on-top must retain AppKit's normal menu-bar constraint");
    require(platform->setStaysOnTop(true) && window.level > tool.level,
            "re-enabling always-on-top must move the pin above floating tools");
    require(platform->applyPlacement(originalPlacement, screen), "restore topmost placement");
    require((window.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces) &&
                (window.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary),
            "pin must join desktop and full-screen Spaces");
    require(platform->setInputTransparent(true) && window.ignoresMouseEvents,
            "click-through must change the native input policy");
    require(!platform->activate(), "click-through pin must not activate");
    require(platform->setInputTransparent(false) && !window.ignoresMouseEvents,
            "exit must restore native input");
    if (focus) {
        require(platform->activate(), "explicit pin activation request failed");
        events(200);
        require(window.keyWindow, "explicit pin activation must acquire keyboard focus");
        QLineEdit editor(&widget);
        editor.setGeometry(10, 10, 90, 24);
        editor.show();
        editor.setFocus();
        events();
        require(editor.hasFocus(), "pin text editor must own Qt keyboard focus");
        QInputMethodEvent input;
        input.setCommitString(QString::fromUtf8("测试"));
        QCoreApplication::sendEvent(&editor, &input);
        require(editor.text() == QString::fromUtf8("测试"),
                "pin text editor must accept input-method commits");
        QWidget auxiliary(nullptr,
                          Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        auxiliary.setAttribute(Qt::WA_ShowWithoutActivating);
        configurePinnedAuxiliary(&auxiliary);
        auxiliary.show();
        events();
        NSWindow* panel = reinterpret_cast<NSView*>(auxiliary.winId()).window;
        require(panel.level == snow_shot::platform::detail::pinnedWindowLevel() &&
                    (panel.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) &&
                    window.keyWindow && editor.hasFocus(),
                "passive auxiliary controls must share Space policy without taking focus");
        auxiliary.close();
    }
    platform->detach();
    require(window.styleMask == originalStyleMask,
            "detachment must restore the original native window style");
    require(window.movable == originalMovable,
            "detachment must restore the original native movement policy");
    require(window.movableByWindowBackground == originalMovableByWindowBackground,
            "detachment must restore the original native background movement policy");
    require(window.hasShadow == originalHasShadow,
            "detachment must restore the original native shadow policy");
    require(platform->attach(), "reattachment failed");
    widget.setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    widget.show();
    events();
    require(platform->attach() && platform->setInputTransparent(true),
            "surface recreation must restore platform ownership");
    NSWindow* recreated = [reinterpret_cast<NSView*>(widget.winId()).window retain];
    require(recreated.level == snow_shot::platform::detail::pinnedWindowLevel() &&
                !recreated.hidesOnDeactivate,
            "surface recreation must preserve the always-on-top native policy");
    int notifications = 0;
    platform->environmentChanged = [&](bool) { ++notifications; };
    platform.reset();
    require(!recreated.ignoresMouseEvents,
            "backend cleanup must restore the original input policy");
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeBackingPropertiesNotification
                      object:recreated];
    [NSWorkspace.sharedWorkspace.notificationCenter
        postNotificationName:NSWorkspaceDidWakeNotification
                      object:nil];
    events();
    require(notifications == 0, "destroyed backend must unregister every observer");
    [recreated release];
    widget.close();
}
int delivery() {
    if (!CGPreflightPostEventAccess())
        return 77;
    CGEventRef initial = CGEventCreate(nullptr);
    const CGPoint original = CGEventGetLocation(initial);
    CFRelease(initial);
    struct CursorRestore {
        CGPoint p;
        ~CursorRestore() {
            CGWarpMouseCursorPosition(p);
        }
    } restore{original};
    const auto drag = [](CGPoint from, CGPoint to) {
        for (const auto& [type, position] : {
                 std::pair{kCGEventMouseMoved, from},
                 std::pair{kCGEventLeftMouseDown, from},
                 std::pair{kCGEventLeftMouseDragged, to},
                 std::pair{kCGEventLeftMouseUp, to},
             }) {
            CGEventRef event = CGEventCreateMouseEvent(nullptr, type, position, kCGMouseButtonLeft);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
            events(100);
        }
    };
    ControlledWindow controlled;
    controlled.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    controlled.setGeometry(150, 150, 300, 200);
    auto controlledPlatform = createPinnedWindowPlatform(&controlled);
    controlled.show();
    events();
    require(controlledPlatform->attach(), "controlled pin setup failed");
    const QRect originalGeometry = controlled.geometry();
    drag(CGPointMake(250, 230), CGPointMake(280, 250));
    require(controlled.geometry() == originalGeometry.translated(30, 20),
            "native background policy must deliver the complete drag to the pin controller");
    const QRect movedGeometry = controlled.geometry();
    drag(CGPointMake(movedGeometry.right() - 2, movedGeometry.center().y()),
         CGPointMake(movedGeometry.right() + 38, movedGeometry.center().y() + 20));
    require(controlled.geometry().topLeft() == movedGeometry.topLeft() &&
                controlled.width() == movedGeometry.width() + 40 &&
                controlled.height() == movedGeometry.height() + 20,
            "native border policy must deliver the complete resize to the pin controller");
    controlled.close();
    events();
    QProcess receiver;
    receiver.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--receiver"), QStringLiteral("-platform"), QStringLiteral("cocoa")});
    require(receiver.waitForStarted(5000) && receiver.waitForReadyRead(5000),
            "receiver application did not start");
    receiver.readAllStandardOutput();
    struct ProcessStop {
        QProcess& process;
        ~ProcessStop() {
            process.kill();
            process.waitForFinished(5000);
        }
    } stop{receiver};
    DismissWindow pin;
    pin.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    pin.setGeometry(150, 150, 300, 200);
    auto platform = createPinnedWindowPlatform(&pin);
    pin.show();
    events();
    require(platform->attach() && platform->setInputTransparent(true), "delivery pin setup failed");
    events(); // Commit input transparency before the next native gesture.
    const auto click = [] {
        for (CGEventType type : {kCGEventMouseMoved, kCGEventLeftMouseDown, kCGEventLeftMouseUp}) {
            CGEventRef event =
                CGEventCreateMouseEvent(nullptr, type, CGPointMake(230, 230), kCGMouseButtonLeft);
            CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
            events(100);
        }
    };
    click();
    events(100);
    receiver.waitForReadyRead(500);
    const QByteArray passed = receiver.readAllStandardOutput();
    require(passed.contains("press") && passed.contains("release"),
            "native click-through did not deliver a complete click to another application");
    require(platform->setInputTransparent(false) && platform->activate(),
            "interactive pin setup failed");
    // Deliver the AppKit input-policy/activation transaction before posting the
    // next gesture, just as separate user interactions yield to the event loop.
    events();
    click();
    events(100);
    receiver.waitForReadyRead(300);
    require(!pin.isVisible() && receiver.readAllStandardOutput().isEmpty(),
            "dismissing a pin must own the release and not click the application underneath");
    return 0;
}

int modalDelivery() {
    if (sessionLocked() || !CGPreflightPostEventAccess())
        return 77;
    CGEventRef initial = CGEventCreate(nullptr);
    const CGPoint original = CGEventGetLocation(initial);
    CFRelease(initial);
    struct CursorRestore {
        CGPoint position;
        ~CursorRestore() {
            CGWarpMouseCursorPosition(position);
        }
    } restore{original};
    const auto pointer = [](CGEventType type, const QPoint& position) {
        CGEventRef event = CGEventCreateMouseEvent(
            nullptr, type, CGPointMake(position.x(), position.y()), kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        events(80);
    };
    ControlledWindow pin;
    pin.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    pin.setGeometry(200, 200, 640, 480);
    auto platform = createPinnedWindowPlatform(&pin);
    pin.show();
    require(platform->activate(), "pin must activate before opening its confirmation");
    events();
    adqt::widgets::AdModal modal(&pin);
    modal.setMode(adqt::widgets::AdModal::Mode::Window);
    modal.setPreset(adqt::widgets::AdModal::Preset::Confirm);
    modal.setWindowTitle(QStringLiteral("Destroy pinned window"));
    modal.setText(QStringLiteral("Destroy this pinned window?"));
    modal.open();
    events();
    QWidget* surface = modal.acceptButton()->window();
    NSWindow* nativePin = reinterpret_cast<NSView*>(pin.winId()).window;
    NSWindow* nativeModal = reinterpret_cast<NSView*>(surface->winId()).window;
    require(!nativeModal.isSheet, "the confirmation must stay independently movable");
    const QRect originalGeometry = pin.geometry();
    const QPoint from = pin.mapToGlobal(QPoint(30, pin.height() - 30));
    pointer(kCGEventMouseMoved, from);
    pointer(kCGEventLeftMouseDown, from);
    pointer(kCGEventLeftMouseDragged, from + QPoint(50, 30));
    pointer(kCGEventLeftMouseUp, from + QPoint(50, 30));
    require(pin.geometry() == originalGeometry, "a native drag must not move the modal's pin");
    require(NSApp.keyWindow != nativePin, "clicking a blocked pin must not activate it");
    const QPoint center = surface->mapToGlobal(surface->rect().center());
    const NSPoint nativeCenter =
        NSMakePoint(center.x(), NSMaxY(NSScreen.screens.firstObject.frame) - center.y());
    require([NSWindow windowNumberAtPoint:nativeCenter
                belowWindowWithWindowNumber:0] == nativeModal.windowNumber,
            "clicking a blocked pin must not cover its confirmation");
    const QPoint modalPosition = surface->pos();
    const QPoint header = surface->mapToGlobal(QPoint(8, 8));
    pointer(kCGEventMouseMoved, header);
    pointer(kCGEventLeftMouseDown, header);
    pointer(kCGEventLeftMouseDragged, header + QPoint(40, -25));
    pointer(kCGEventLeftMouseUp, header + QPoint(40, -25));
    require(surface->pos() == modalPosition + QPoint(40, -25) && pin.geometry() == originalGeometry,
            "native dragging must move the confirmation independently of the blocked pin");
    const QPoint cancel = modal.rejectButton()->mapToGlobal(modal.rejectButton()->rect().center());
    pointer(kCGEventLeftMouseDown, cancel);
    pointer(kCGEventLeftMouseUp, cancel);
    require(!modal.isOpen(), "Cancel must receive native mouse input");
    pointer(kCGEventLeftMouseDown, from);
    pointer(kCGEventLeftMouseDragged, from + QPoint(50, 30));
    pointer(kCGEventLeftMouseUp, from + QPoint(50, 30));
    require(pin.geometry() == originalGeometry.translated(50, 30),
            "canceling the confirmation must restore native pin dragging");
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    snow_shot::platform::initializeScreenshotWindowPolicy();
    QApplication::setQuitOnLastWindowClosed(false);
    if (app.arguments().contains(QStringLiteral("--receiver"))) {
        Receiver receiver;
        receiver.show();
        receiver.activateWindow();
        events();
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    int result = 0;
    QTimer::singleShot(100, &app, [&] {
        try {
            if (app.arguments().contains(QStringLiteral("--ownership-only")))
                auxiliaryOwnershipRequiresARealOwner();
            else if (app.arguments().contains(QStringLiteral("--delivery")))
                result = delivery();
            else if (app.arguments().contains(QStringLiteral("--modal-delivery")))
                result = modalDelivery();
            else if (app.arguments().contains(QStringLiteral("--focus")) && sessionLocked()) {
                std::cerr << "Cocoa focus qualification requires an unlocked desktop\n";
                result = 77;
            } else
                nativePolicies(app.arguments().contains(QStringLiteral("--focus")));
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
        app.quit();
    });
    app.exec();
    return result;
}
