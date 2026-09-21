#include "presentation/pinned/pinnedwindowplatform.h"
#include "snow_shot/presentation/mousereleaseactioncontroller.h"
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
            QPoint expectedOrigin =
                (QPointF(screen->geometry().topLeft()) + requested.position).toPoint();
            expectedOrigin.setY(qMax(screen->availableGeometry().top(), expectedOrigin.y()));
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

void nativePolicies(bool focus) {
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
    require(constrained && constrained->windowSize == edgePlacement.windowSize &&
                constrained->position == QPointF(70, usableTop) &&
                widget.geometry().topLeft() ==
                    screen->geometry().topLeft() + constrained->position.toPoint(),
            "native menu-bar constraints must be reflected in Qt geometry and placement readback");
    require(platform->applyPlacement(originalPlacement, screen), "restore edge placement");
    require(window.level == NSModalPanelWindowLevel && !window.hidesOnDeactivate,
            "always-on-top pin must stay above floating tools and remain visible when inactive");
    QWidget floatingTool(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    NSWindow* tool = reinterpret_cast<NSView*>(floatingTool.winId()).window;
    require(window.level > tool.level,
            "an ordinary floating tool must not occupy the always-on-top pin's stacking band");
    require(platform->setStaysOnTop(false) && window.level == NSNormalWindowLevel,
            "disabling always-on-top must restore the normal window band");
    require(platform->attach() && window.level == NSNormalWindowLevel,
            "reattaching must preserve the always-on-top opt-out");
    require(platform->setStaysOnTop(true) && window.level > tool.level,
            "re-enabling always-on-top must move the pin above floating tools");
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
        require(panel.level == NSModalPanelWindowLevel &&
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
    require(recreated.level == NSModalPanelWindowLevel && !recreated.hidesOnDeactivate,
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
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
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
            if (app.arguments().contains(QStringLiteral("--delivery")))
                result = delivery();
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
