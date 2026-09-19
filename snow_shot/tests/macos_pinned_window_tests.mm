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
#include <stdexcept>
#include <iostream>

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
void nativePolicies(bool focus) {
    QWidget widget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    widget.setAttribute(Qt::WA_TranslucentBackground);
    auto platform = createPinnedWindowPlatform(&widget);
    widget.winId();
    require(platform->attach(), "Cocoa panel attachment failed");
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
    require(actual && actual->pixelSize == placement.pixelSize &&
                actual->position == placement.position,
            "Cocoa placement readback must preserve half points and odd pixel sizes");
    NSView* view = reinterpret_cast<NSView*>(widget.internalWinId());
    NSWindow* window = view.window;
    require(window.level == NSFloatingWindowLevel && !window.hidesOnDeactivate,
            "pin must float and remain visible in inactive applications");
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
        require(panel.level == NSFloatingWindowLevel &&
                    (panel.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) &&
                    window.keyWindow && editor.hasFocus(),
                "passive auxiliary controls must share Space policy without taking focus");
        auxiliary.close();
    }
    platform->detach();
    require(platform->attach(), "reattachment failed");
    widget.setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    widget.show();
    events();
    require(platform->attach() && platform->setInputTransparent(true),
            "surface recreation must restore platform ownership");
    NSWindow* recreated = [reinterpret_cast<NSView*>(widget.winId()).window retain];
    int notifications = 0;
    platform->environmentChanged = [&] { ++notifications; };
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
    const auto click = [] {
        for (CGEventType type : {kCGEventLeftMouseDown, kCGEventLeftMouseUp}) {
            CGEventRef event =
                CGEventCreateMouseEvent(nullptr, type, CGPointMake(230, 230), kCGMouseButtonLeft);
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
