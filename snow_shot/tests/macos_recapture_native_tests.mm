#include "snow_shot/platform/macos/recapturefocus.h"
#include "snow_shot/platform/macos/applicationactivation.h"
#include "snow_shot/platform/screenshotnative.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QApplication>
#include <QImage>
#include "snow_capture.h"
#include <future>
#include <QEventLoop>
#include <QProcess>
#include <QShortcut>
#include <QTextEdit>
#include <QTimer>
#include <QWindow>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 3000) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return ready();
}
QImage captureCursorRegion(CGPoint point, uint32_t overlay, bool cursor) {
    auto job = std::async(std::launch::async, [=]() -> QImage {
        SnowCaptureDesktopSessionConfig config{};
        config.capture_backend = SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT;
        config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
        config.exclusions.windows = &overlay;
        config.exclusions.window_count = 1;
        std::unique_ptr<SnowCaptureDesktopSession, decltype(&snow_capture_desktop_session_destroy)>
            session(snow_capture_desktop_session_create(&config),
                    snow_capture_desktop_session_destroy);
        if (!session)
            return {};
        SnowCaptureScreenshotRequest request{};
        request.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
        request.struct_size = sizeof(request);
        request.flags = cursor ? SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR : 0;
        std::unique_ptr<SnowCaptureScreenshotResult,
                        decltype(&snow_capture_screenshot_result_destroy)>
            result(snow_capture_desktop_session_capture(session.get(), &request),
                   snow_capture_screenshot_result_destroy);
        if (!result)
            return {};
        for (size_t index = 0; index < snow_capture_screenshot_result_display_count(result.get());
             ++index) {
            SnowCaptureFrameInfo frame{};
            SnowCaptureFrameGeometry geometry{};
            geometry.version = SNOW_CAPTURE_FRAME_GEOMETRY_VERSION;
            geometry.struct_size = sizeof(geometry);
            if (!snow_capture_screenshot_result_display_info(result.get(), index, &frame) ||
                !snow_capture_screenshot_result_display_geometry(result.get(), index, &geometry))
                return {};
            if (!CGRectContainsPoint(
                    CGRectMake(geometry.x, geometry.y, geometry.width, geometry.height), point))
                continue;
            QImage image(frame.rgba_bytes, static_cast<int>(frame.width),
                         static_cast<int>(frame.height), static_cast<int>(frame.stride_bytes),
                         QImage::Format_RGBA8888);
            const int x = qRound((point.x - geometry.x) * geometry.backing_scale);
            const int y = qRound((point.y - geometry.y) * geometry.backing_scale);
            return image.copy(x - 40, y - 40, 80, 80);
        }
        return {};
    });
    require(waitFor([&] {
                return job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            }),
            "native capture did not complete");
    return job.get();
}

bool capturedIBeam(const QImage& cursor, const QImage& background) {
    if (cursor.isNull() || cursor.size() != background.size())
        return false;
    QRect difference;
    for (int y = 0; y < cursor.height(); ++y)
        for (int x = 0; x < cursor.width(); ++x)
            if (qAbs(qGray(cursor.pixel(x, y)) - qGray(background.pixel(x, y))) > 40)
                difference |= QRect(x, y, 1, 1);
    // On the blank editor viewport an I-beam is tall and narrow; the default
    // arrow has a broad diagonal body. Compare actual captured cursor pixels.
    return difference.width() > 0 && difference.height() > difference.width() * 2;
}

void pressRecapture() {
    for (bool down : {true, false}) {
        CGEventRef event = CGEventCreateKeyboardEvent(nullptr, 15, down); // R
        CGEventPost(kCGSessionEventTap, event);
        CFRelease(event);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const bool floating = app.arguments().contains(QStringLiteral("--floating")) ||
                          app.arguments().contains(QStringLiteral("--local-floating"));
    const bool local = app.arguments().contains(QStringLiteral("--local")) ||
                       app.arguments().contains(QStringLiteral("--local-floating"));
    if (app.arguments().contains(QStringLiteral("--target"))) {
        QTextEdit editor;
        if (floating) {
            editor.setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
            editor.setAttribute(Qt::WA_MacAlwaysShowToolWindow);
        }
        editor.setGeometry(100, 100, 500, 300);
        editor.show();
        QTextEdit second;
        second.setGeometry(700, 100, 300, 300);
        second.show();
        snow_shot::platform::macos::activateWindow(&second);
        QTimer::singleShot(100, &app, [&] {
            const QPoint point = editor.viewport()->mapToGlobal(QPoint(50, 50));
            const auto native = reinterpret_cast<NSView*>(editor.winId()).window;
            std::cout << point.x() << ' ' << point.y() << ' ' << native.windowNumber << std::endl;
        });
        // Ensure a failing parent cannot leave the helper behind.
        QTimer::singleShot(15000, &app, &QApplication::quit);
        return app.exec();
    }
    if (!AXIsProcessTrusted() || !CGPreflightScreenCaptureAccess())
        return 77;
    NSRunningApplication* previousApplication =
        [NSWorkspace.sharedWorkspace.frontmostApplication retain];
    QProcess target;
    QStringList targetArguments{QStringLiteral("--target")};
    if (floating)
        targetArguments.push_back(QStringLiteral("--floating"));
    target.start(app.applicationFilePath(), targetArguments);
    require(target.waitForStarted(), "could not launch external cursor owner");
    require(waitFor([&] { return target.canReadLine(); }), "target did not become ready");
    const auto coordinates = target.readLine().trimmed().split(' ');
    require(coordinates.size() == 3, "invalid target position");
    CGEventRef previous = CGEventCreate(nullptr);
    const CGPoint oldPosition = CGEventGetLocation(previous);
    CFRelease(previous);
    const CGPoint point = CGPointMake(coordinates[0].toInt(), coordinates[1].toInt());
    CGWarpMouseCursorPosition(point);

    // The foreign window remains underneath, so excluding the entire
    // current process would visibly choose the wrong cursor owner.
    std::unique_ptr<QTextEdit> localEditor;
    if (local) {
        localEditor = std::make_unique<QTextEdit>();
        if (floating) {
            localEditor->setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
            localEditor->setAttribute(Qt::WA_MacAlwaysShowToolWindow);
        }
        localEditor->setGeometry(100, 100, 500, 300);
        localEditor->show();
        snow_shot::platform::macos::activateWindow(localEditor.get());
        require(waitFor([&] { return localEditor->isActiveWindow(); }),
                "local cursor owner did not become ready");
    }

    // A visible surface may cover the pointer without owning mouse input. It
    // must remain in the captured scene but never become the recapture target.
    QWidget decoration(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                    Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    decoration.setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    decoration.setGeometry(100, 100, 200, 120);
    decoration.show();
    require(reinterpret_cast<NSView*>(decoration.winId()).window.ignoresMouseEvents,
            "decorative fixture must be natively click-through");

    QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay.setGeometry(100, 100, 500, 300);
    overlay.setCursor(Qt::ArrowCursor);
    snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
    overlay.show();
    snow_shot::platform::macos::activateWindow(&overlay);
    require(waitFor([&] { return overlay.isActiveWindow(); }),
            "overlay did not take initial focus");
    NSWindow* native = reinterpret_cast<NSView*>(overlay.winId()).window;
    if (floating && !local) {
        const CGWindowID targetId = coordinates[2].toUInt();
        CFArrayRef windows =
            CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, targetId);
        bool elevated = false;
        for (NSDictionary* info in reinterpret_cast<NSArray*>(windows)) {
            elevated |=
                [info[reinterpret_cast<NSString*>(kCGWindowNumber)] unsignedIntValue] == targetId &&
                [info[reinterpret_cast<NSString*>(kCGWindowLayer)] intValue] != 0;
        }
        if (windows)
            CFRelease(windows);
        require(elevated,
                "foreign floating fixture must retain a nonzero WindowServer layer while inactive");
    }
    int captures = 0;
    bool succeeded = true;
    bool cursorCorrect = true;
    std::unique_ptr<snow_shot::platform::macos::RecaptureFocus> focus;
    QShortcut recapture(QKeySequence(Qt::Key_R), &overlay);
    QObject::connect(&recapture, &QShortcut::activated, &overlay, [&] {
        focus = snow_shot::platform::macos::createRecaptureFocus({&overlay});
        focus->prepare([&](bool ready) {
            succeeded &=
                ready && native.ignoresMouseEvents &&
                overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput) &&
                reinterpret_cast<NSView*>(overlay.winId()).window == native &&
                NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier ==
                    (localEditor ? QCoreApplication::applicationPid() : target.processId());
            if (localEditor)
                succeeded &= localEditor->isActiveWindow();
            if (ready) {
                const auto cursor =
                    captureCursorRegion(point, static_cast<uint32_t>(native.windowNumber), true);
                const auto background =
                    captureCursorRegion(point, static_cast<uint32_t>(native.windowNumber), false);
                cursorCorrect &= capturedIBeam(cursor, background);
            }
            focus.reset();
            snow_shot::platform::macos::activateWindow(&overlay);
            ++captures;
        });
    });
    for (int iteration = 1; iteration <= 2; ++iteration) {
        CGWarpMouseCursorPosition(point);
        pressRecapture();
        require(waitFor([&] { return captures == iteration; }),
                "keyboard shortcut did not trigger repeated recapture");
        require(waitFor([&] { return overlay.isActiveWindow() && NSApp.active; }),
                "overlay did not regain native keyboard focus");
        require(!native.ignoresMouseEvents &&
                    !overlay.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput),
                "native and Qt overlay input must both be restored");
        require(decoration.windowHandle()->flags().testFlag(Qt::WindowTransparentForInput) &&
                    reinterpret_cast<NSView*>(decoration.winId()).window.ignoresMouseEvents,
                "recapture must preserve unrelated click-through surfaces");
    }
    CGWarpMouseCursorPosition(oldPosition);
    target.terminate();
    target.waitForFinished();
    [previousApplication activateWithOptions:0];
    [previousApplication release];
    require(succeeded, "target focus or cursor preparation failed");
    require(cursorCorrect, "recapture did not capture the target's I-beam cursor");
    std::cout << "macOS native recapture focus and cursor tests passed\n";
}
