#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/platform/macos/applicationactivation.h"
#include "platform/macos/capturewindowlayers_p.h"
#include "../../cmake/test-support/macos_native_input.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <iostream>
#include <stdexcept>

namespace {
using namespace snow_shot::platform;
using namespace snow_shot::platform::detail;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void settle(int milliseconds = 50) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timeout;
    timeout.start();
    while (!ready() && timeout.elapsed() < 5000)
        settle(10);
    return ready();
}

NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.internalWinId()).window;
}

void typeCharacter() {
    for (bool down : {true, false}) {
        CGEventRef key = CGEventCreateKeyboardEvent(nullptr, 0, down);
        const UniChar character = 'a';
        CGEventKeyboardSetUnicodeString(key, 1, &character);
        CGEventPost(kCGSessionEventTap, key);
        CFRelease(key);
    }
}

class Surface : public QWidget {
  public:
    explicit Surface(QWidget* owner = nullptr)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        resize(160, 100);
    }
    void releaseSurface() {
        hide();
        destroy();
    }
    int moves = 0;
    QPoint lastPosition;

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        ++moves;
        lastPosition = event->globalPosition().toPoint();
    }
};

void rolesExistBeforeNativeCreation() {
    Surface overlay;
    overlay.setProperty(kScreenshotLayer, kOverlayLayer);
    Surface toolbar(&overlay);
    Surface popup(&toolbar);
    Surface recording;
    recording.setProperty(kScreenshotLayer, kOverlayLayer);
    recording.setProperty(kCaptureFamily, static_cast<int>(CaptureFamily::Recording));
    require(!overlay.internalWinId() && !toolbar.internalWinId() && !popup.internalWinId(),
            "role resolution must not create native surfaces");
    require(widgetCaptureLayer(&popup).family == CaptureFamily::Screenshot &&
                widgetCaptureLayer(&popup).layer > widgetCaptureLayer(&toolbar).layer,
            "new toolbar descendants must inherit capture before native initialization");
    static_cast<void>(popup.winId());
    static_cast<void>(recording.winId());
    popup.windowHandle()->setTransientParent(recording.windowHandle());
    // Match the roles copied by the native stacking policy on Cocoa.
    recording.windowHandle()->setProperty(kScreenshotLayer, kOverlayLayer);
    recording.windowHandle()->setProperty(kCaptureFamily,
                                          static_cast<int>(CaptureFamily::Recording));
    require(widgetCaptureLayer(&popup).family == CaptureFamily::Recording,
            "live recording ownership must override the old screenshot QObject parent");
    QWindow ordinary;
    popup.windowHandle()->setTransientParent(&ordinary);
    require(!widgetCaptureLayer(&popup).valid(),
            "a pooled popup with an ordinary transient owner must release capture policy");
}

void creationOwnerOverridesPooledOwnership() {
    Surface screenshot;
    prepareScreenshotToolbarWindow(&screenshot);
    require(!screenshot.internalWinId(),
            "preparing a toolbar must not allocate its native surface");
    Surface pooled(&screenshot);
    QWidget ordinary;
    using adqt::widgets::ScopedWindowCreationOwner;
    {
        const ScopedWindowCreationOwner outer(&pooled, &ordinary);
        require(!widgetCaptureLayer(&pooled).valid(),
                "an ordinary creation owner must override the pooled QObject parent");
        {
            const ScopedWindowCreationOwner inner(&pooled, &screenshot);
            require(widgetCaptureLayer(&pooled).valid(), "nested creation must use its own owner");
        }
        require(!widgetCaptureLayer(&pooled).valid(),
                "nested creation must restore the enclosing owner");
    }
    require(!ScopedWindowCreationOwner::ownerFor(&pooled) && widgetCaptureLayer(&pooled).valid(),
            "completed creation must restore normal ownership resolution");
    auto* destroyedOwner = new Surface;
    prepareScreenshotToolbarWindow(destroyedOwner);
    {
        const ScopedWindowCreationOwner owner(&pooled, destroyedOwner);
        delete destroyedOwner;
        require(!widgetCaptureLayer(&pooled).valid(),
                "destroying the pending owner must not leave a dangling or stale capture role");
    }
    static_cast<void>(screenshot.winId());
    screenshot.releaseSurface();
    require(widgetCaptureLayer(&screenshot).layer == kToolbarLayer,
            "screenshot-only toolbars must retain their role after native release");
}

class DesktopFixture final {
  public:
    explicit DesktopFixture(bool fullscreen = true)
        : previous([NSWorkspace.sharedWorkspace.frontmostApplication retain]) {
        target.start(QCoreApplication::applicationFilePath(),
                     {fullscreen ? QStringLiteral("--fullscreen-target")
                                 : QStringLiteral("--foreground-target"),
                      QStringLiteral("-platform"), QStringLiteral("cocoa")});
        require(target.waitForStarted(), "fullscreen fixture must start");
        QByteArray output;
        require(waitFor([&] {
                    output += target.readAllStandardOutput();
                    return output.contains("READY");
                }),
                "foreground fixture must finish showing its window");
    }
    ~DesktopFixture() {
        target.terminate();
        if (!target.waitForFinished(2000)) {
            target.kill();
            target.waitForFinished();
        }
        [NSApp yieldActivationToApplication:previous];
        [previous activateWithOptions:0];
        [previous release];
    }
    bool remainsForeground() const {
        return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier ==
               target.processId();
    }
    void takeFocus() const {
        NSRunningApplication* application = [NSRunningApplication
            runningApplicationWithProcessIdentifier:static_cast<pid_t>(target.processId())];
        [NSApp deactivate];
        [NSApp yieldActivationToApplication:application];
        [application activateWithOptions:0];
        require(waitFor([&] { return remainsForeground() && !NSApp.active; }),
                "activation regression must start with the tested application inactive");
    }

  private:
    NSRunningApplication* previous;
    QProcess target;
};

void ordinaryWindowsStillActivate() {
    DesktopFixture desktop(false);
    Surface screenshot;
    configureScreenshotOverlayWindow(&screenshot);
    screenshot.show();

    QWidget ordinary;
    Surface tool;
    Surface recording;
    configureScreenRecordingToolbarWindow(&recording);
    Surface reused;
    static_cast<void>(reused.winId());
    reused.windowHandle()->setTransientParent(screenshot.windowHandle());
    require(native(reused).styleMask & NSWindowStyleMaskNonactivatingPanel,
            "pooled tool must first acquire screenshot ownership");
    reused.windowHandle()->setTransientParent(nullptr);

    for (QWidget* window : {&ordinary, static_cast<QWidget*>(&tool),
                            static_cast<QWidget*>(&recording), static_cast<QWidget*>(&reused)}) {
        window->setAttribute(Qt::WA_ShowWithoutActivating);
        window->setGeometry(350, 300, 240, 120);
        QLineEdit editor(window);
        editor.setGeometry(10, 10, 160, 40);
        editor.show();
        for (bool nativeRaise : {false, true}) {
            desktop.takeFocus();
            window->show();
            // A plain native orderFront is not Qt raise and must not activate.
            [native(*window) orderFront:nil];
            settle();
            require(desktop.remainsForeground() && !NSApp.active,
                    "show without activation and native ordering must stay nonactivating");
            if (nativeRaise)
                window->windowHandle()->raise();
            else
                window->raise();
            window->activateWindow();
            editor.setFocus();
            const bool activated = waitFor([&] {
                return NSApp.active && native(*window).keyWindow &&
                       NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier ==
                           QCoreApplication::applicationPid();
            });
            if (!activated)
                std::cerr << "activation failure: ordinary=" << (window == &ordinary)
                          << " recording=" << (window == &recording)
                          << " reused=" << (window == &reused) << " QWindow.raise=" << nativeRaise
                          << " active=" << bool(NSApp.active)
                          << " key=" << bool(native(*window).keyWindow)
                          << " style=" << native(*window).styleMask << '\n';
            require(activated,
                    "ordinary, recording and released tools must retain native activation");
            typeCharacter();
            require(waitFor([&] { return editor.text() == QStringLiteral("a"); }),
                    "ordinary activation must deliver keyboard input without a click");
            editor.clear();
            window->hide();
        }
    }
}

void hoverOverFullscreen() {
    MacCursorRestore cursor;
    DesktopFixture desktop;
    Surface overlay;
    overlay.setGeometry(qApp->primaryScreen()->geometry());
    configureScreenshotOverlayWindow(&overlay);
    Surface toolbar(&overlay);
    configureScreenshotToolbarWindow(&toolbar);
    toolbar.move(overlay.geometry().center() + QPoint(200, 100));
    QLineEdit editor(&toolbar);
    editor.setGeometry(10, 10, 130, 35);
    editor.show();

    // This popup already has an ordinary native surface before entering capture.
    Surface pooled;
    pooled.show();
    pooled.hide();
    require(!(native(pooled).styleMask & NSWindowStyleMaskNonactivatingPanel),
            "ordinary tools must keep their original activation behavior");
    pooled.windowHandle()->setTransientParent(toolbar.windowHandle());
    require(native(pooled).styleMask & NSWindowStyleMaskNonactivatingPanel,
            "adopting an existing popup must recreate it as a nonactivating panel");

    for (int capture = 0; capture != 2; ++capture) {
        // Destroying the overlay also destroys its toolbar's QWindow, clearing
        // the independent pooled tool's transient parent. Reattach it to this
        // capture's new owner, as the UI host does for a reused popup.
        configureScreenshotToolbarWindow(&toolbar);
        pooled.windowHandle()->setTransientParent(toolbar.windowHandle());
        overlay.show();
        overlay.raise();
        overlay.activateWindow();
        overlay.setFocus();
        require(waitFor([&] { return bool(native(overlay).keyWindow); }),
                "capture must acquire keyboard focus without foreground activation");
        require(native(overlay).styleMask & NSWindowStyleMaskNonactivatingPanel,
                "every newly created overlay surface must be nonactivating");
        native(overlay).styleMask &= ~NSWindowStyleMaskNonactivatingPanel;
        require(native(overlay).styleMask & NSWindowStyleMaskNonactivatingPanel,
                "later Qt style changes must retain the panel's construction contract");
        const QPoint top(overlay.geometry().center().x(), overlay.geometry().top());
        const QPoint center = overlay.geometry().center() + QPoint(-50, capture * 20);
        macPostMove(top);
        settle(600); // Exercise the fullscreen menu-bar activation transition.
        const int before = overlay.moves;
        macPostMove(center);
        require(waitFor([&] { return overlay.moves > before && overlay.lastPosition == center; }),
                "hover must reach Qt after top edge -> center, without a click");
        require(desktop.remainsForeground() && native(overlay).keyWindow,
                "fullscreen app must stay foreground while capture retains key focus");

        toolbar.show();
        toolbar.raise();
        macPostClick(editor.mapToGlobal(editor.rect().center()));
        require(waitFor([&] { return editor.hasFocus(); }),
                "nonactivating screenshot tools must still accept keyboard focus");
        typeCharacter();
        require(waitFor([&] { return editor.text() == QStringLiteral("a"); }),
                "native keyboard input must reach a nonactivating screenshot editor");
        editor.clear();
        require(desktop.remainsForeground(),
                "clicking screenshot toolbar controls must not activate the application");
        pooled.move(toolbar.pos() + QPoint(0, 120));
        pooled.show();
        require(native(pooled).styleMask & NSWindowStyleMaskNonactivatingPanel,
                "a reused popup must retain its current screenshot ownership when shown");
        pooled.raise();
        macPostClick(pooled.geometry().center());
        macPostMove(top);
        settle(600);
        const int afterPopup = overlay.moves;
        macPostMove(center);
        const bool popupHover = waitFor([&] { return overlay.moves > afterPopup; });
        if (!popupHover || !desktop.remainsForeground())
            std::cerr << "popup failure: capture=" << capture << " hover=" << popupHover
                      << " foreground=" << desktop.remainsForeground()
                      << " key=" << bool(native(overlay).keyWindow)
                      << " style=" << native(pooled).styleMask << '\n';
        require(popupHover && desktop.remainsForeground(),
                "hover must survive the top edge after clicking a reused screenshot popup");
        // Recapture uses the shared activation helper to restore keyboard focus.
        // It must honor the same nonactivating panel contract as the initial show.
        snow_shot::platform::macos::activateWindow(&overlay);
        require(desktop.remainsForeground() && native(overlay).keyWindow,
                "explicit screenshot focus restoration must preserve the foreground app");
        macPostMove(top);
        settle(600);
        const int afterRestore = overlay.moves;
        macPostMove(center);
        require(waitFor([&] { return overlay.moves > afterRestore; }) &&
                    desktop.remainsForeground(),
                "hover must survive the top edge after explicit focus restoration");
        pooled.hide();
        toolbar.hide();
        overlay.releaseSurface();
        configureScreenshotOverlayWindow(&overlay);
    }
    pooled.windowHandle()->setTransientParent(nullptr);
    require(!(native(pooled).styleMask & NSWindowStyleMaskNonactivatingPanel),
            "a popup leaving capture must restore ordinary native activation behavior");
    Surface recording;
    configureScreenRecordingAreaWindow(&recording);
    require(!(native(recording).styleMask & NSWindowStyleMaskNonactivatingPanel),
            "recording windows must retain their existing native activation behavior");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool fullscreen = app.arguments().contains(QStringLiteral("--fullscreen-target"));
    if (fullscreen || app.arguments().contains(QStringLiteral("--foreground-target"))) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        QWidget target;
        target.setGeometry(qApp->primaryScreen()->availableGeometry());
        QWidget* targetPointer = &target;
        id entered = [NSNotificationCenter.defaultCenter
            addObserverForName:NSWindowDidEnterFullScreenNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      if (notification.object == native(*targetPointer))
                          std::cout << "READY\n" << std::flush;
                    }];
        if (fullscreen) {
            target.showFullScreen();
        } else {
            target.resize(240, 150);
            target.show();
            QTimer::singleShot(0, &app, [] { std::cout << "READY\n" << std::flush; });
        }
        [NSApp activate];
        target.activateWindow();
        // Do not leave a fullscreen helper behind if its parent test crashes.
        QTimer::singleShot(25000, &app, &QApplication::quit);
        const int result = app.exec();
        [NSNotificationCenter.defaultCenter removeObserver:entered];
        return result;
    }
    initializeScreenshotWindowPolicy();
    try {
        if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
            if (!macCanPostMouseEvents())
                return 77;
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            ordinaryWindowsStillActivate();
            hoverOverFullscreen();
        } else {
            rolesExistBeforeNativeCreation();
            creationOwnerOverridesPooledOwnership();
        }
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
