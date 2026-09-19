#include "snow_capture.h"
#include "../src/presentation/capture/captureframeimage.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include "snow_shot/platform/screenshotnative.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QPainter>
#include <QProcess>
#include <QWheelEvent>
#include <QScopeGuard>
#include <future>
#include <iostream>
#include <memory>

namespace {
class ScrollFixture : public QWidget {
  public:
    void wheelEvent(QWheelEvent* event) override {
        const auto delta = event->pixelDelta().isNull() ? event->angleDelta() : event->pixelDelta();
        vertical |= delta.y() != 0;
        horizontal |= delta.x() != 0;
        event->accept();
        if (vertical && horizontal)
            QCoreApplication::exit(0);
    }
    bool vertical = false;
    bool horizontal = false;
};

bool scrollInputSmoke() {
    using namespace snow_shot::platform;
    if (!screenshotScrollPermission()) {
        std::cout << "SKIP: automatic scroll dispatch requires an existing Accessibility grant\n";
        return true;
    }
    NSRunningApplication* previous = [NSWorkspace.sharedWorkspace.frontmostApplication retain];
    const auto restoreFocus = qScopeGuard([previous] {
        [previous activateWithOptions:0];
        [previous release];
    });
    QProcess target;
    target.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--scroll-target")});
    if (!target.waitForStarted(5000) || !target.waitForReadyRead(10000))
        return false;
    const auto fields = target.readLine().trimmed().split(' ');
    if (fields.size() != 5) {
        target.kill();
        target.waitForFinished();
        return false;
    }
    const QRect region(fields[0].toInt(), fields[1].toInt(), fields[2].toInt(), fields[3].toInt());
    const auto window = fields[4].toUInt();
    CGEventRef before = CGEventCreate(nullptr);
    if (!before)
        return false;
    const auto pointer = CGEventGetLocation(before);
    CFRelease(before);
    QCoreApplication::processEvents();
    const auto focused = screenshotFocusedWindow();
    const auto display = screenshotDisplayAtCursor();
    const bool targets = focused == window && display != 0;
    const auto vertical = sendScreenshotScroll(region, QPoint(0, -120));
    const auto horizontal = sendScreenshotScroll(region, QPoint(120, 0));
    const bool finished = target.waitForFinished(10000);
    if (!finished) {
        target.kill();
        target.waitForFinished();
    }
    CGEventRef after = CGEventCreate(nullptr);
    const bool stationary = after && CGPointEqualToPoint(pointer, CGEventGetLocation(after));
    if (after)
        CFRelease(after);
    if (!stationary)
        CGWarpMouseCursorPosition(pointer);
    std::cerr << target.readAllStandardError().constData();
    const bool ok = targets && finished && target.exitCode() == 0 && stationary &&
                    vertical.status == ScrollInputResult::Status::Posted &&
                    horizontal.status == ScrollInputResult::Status::Posted;
    if (!ok)
        std::cerr << "native direct target/scroll dispatch failed: " << targets << ',' << finished
                  << ',' << target.exitCode() << ',' << stationary << '\n';
    return ok;
}

class CaptureFixture : public QWidget {
  public:
    CaptureFixture() : QWidget(nullptr, Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(200, 100, 50, 128));
    }
};
QImage capture(CGWindowID window, bool cursor) {
    SnowCaptureDesktopSessionConfig config{};
    config.capture_backend = SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT;
    config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
    std::unique_ptr<SnowCaptureDesktopSession, decltype(&snow_capture_desktop_session_destroy)>
        session(snow_capture_desktop_session_create(&config), snow_capture_desktop_session_destroy);
    if (!session) {
        std::cerr << "session: " << snow_capture_last_error_message() << "\n";
        return {};
    }
    SnowCaptureScreenshotRequest request{};
    request.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
    request.struct_size = sizeof(request);
    request.focused_window = window;
    request.flags = cursor ? SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR : 0;
    std::unique_ptr<SnowCaptureScreenshotResult, decltype(&snow_capture_screenshot_result_destroy)>
        result(snow_capture_desktop_session_capture(session.get(), &request),
               snow_capture_screenshot_result_destroy);
    if (!result) {
        std::cerr << "capture: " << snow_capture_last_error_message() << "\n";
        return {};
    }
    ScreenshotDisplaySession displays;
    QRect bounds;
    for (size_t i = 0; i < snow_capture_screenshot_result_display_count(result.get()); ++i) {
        SnowCaptureFrameInfo frame{};
        SnowCaptureFrameGeometry geometry{};
        geometry.version = SNOW_CAPTURE_FRAME_GEOMETRY_VERSION;
        geometry.struct_size = sizeof(geometry);
        if (!snow_capture_screenshot_result_display_info(result.get(), i, &frame) ||
            !snow_capture_screenshot_result_display_geometry(result.get(), i, &geometry) ||
            geometry.coordinate_space != 1 || geometry.display_id == 0 ||
            geometry.backing_scale <= 0) {
            std::cerr << "display geometry\n";
            return {};
        }
        CapturedDisplayModel display;
        display.canvasRect = QRect(qRound(geometry.x), qRound(geometry.y), qRound(geometry.width),
                                   qRound(geometry.height));
        display.image =
            QImage(frame.rgba_bytes, static_cast<int>(frame.width), static_cast<int>(frame.height),
                   static_cast<int>(frame.stride_bytes), QImage::Format_RGBA8888)
                .copy();
        display.active = true;
        display.canvasUsesPoints = true;
        display.backingScale = geometry.backing_scale;
        bounds = bounds.united(display.canvasRect);
        displays.appendDisplay(std::move(display));
    }
    SnowCaptureWindowFrameInfo windowFrame{};
    windowFrame.version = SNOW_CAPTURE_WINDOW_FRAME_INFO_VERSION;
    windowFrame.struct_size = sizeof(windowFrame);
    SnowCaptureFrameGeometry geometry{};
    geometry.version = SNOW_CAPTURE_FRAME_GEOMETRY_VERSION;
    geometry.struct_size = sizeof(geometry);
    if (!snow_capture_screenshot_result_focused_window_info(result.get(), &windowFrame) ||
        !snow_capture_screenshot_result_focused_window_geometry(result.get(), &geometry) ||
        windowFrame.width == 0 || geometry.width <= 0) {
        std::cerr << "window geometry: " << snow_capture_last_error_message() << "\n";
        return {};
    }
    const QImage windowImage = snow_shot::presentation::capture::imageFromFrameLease(
        snow_capture_screenshot_result_focused_window_retain(result.get()), windowFrame.rgba_bytes,
        windowFrame.rgba_len, windowFrame.width, windowFrame.height, windowFrame.stride_bytes,
        windowFrame.pixel_format, snow_shot::presentation::capture::FrameAlphaMode::Premultiplied);
    const auto color = windowImage.pixelColor(windowImage.width() / 2, windowImage.height() / 2);
    if (color.alpha() != 128 || color.red() < 190 || color.green() < 90 || color.blue() < 40) {
        std::cerr << "native window lost alpha or was interpreted as straight-alpha pixels\n";
        return {};
    }
    const auto spec = screenshotSelectionRenderSpec(displays, bounds);
    QImage image = composeScreenshotSourceSelection(displays, bounds);
    if (image.size() != spec.pixelSize) {
        std::cerr << "composition\n";
        return {};
    }
    SnowCaptureDesktopSessionState state{};
    if (!snow_capture_desktop_session_state(session.get(), &state) ||
        state.active_capture_access_count != 0) {
        std::cerr << "capture leases: " << state.active_capture_access_count << "\n";
        return {};
    }
    // The returned lease owns pixels independently of both the result and session.
    std::unique_ptr<SnowCaptureFrameLease, decltype(&snow_capture_frame_lease_release)> lease(
        snow_capture_screenshot_result_focused_window_retain(result.get()),
        snow_capture_frame_lease_release);
    if (!lease)
        return {};
    const auto firstPixel = windowFrame.rgba_bytes[0];
    result.reset();
    if (windowFrame.rgba_bytes[0] != firstPixel)
        return {};
    std::unique_ptr<SnowCaptureCancellationToken,
                    decltype(&snow_capture_cancellation_token_destroy)>
        canceled(snow_capture_cancellation_token_create(), snow_capture_cancellation_token_destroy);
    snow_capture_cancellation_token_cancel(canceled.get());
    request.cancellation_token = canceled.get();
    result.reset(snow_capture_desktop_session_capture(session.get(), &request));
    if (result)
        return {};
    session.reset();
    if (windowFrame.rgba_bytes[0] != firstPixel)
        return {};
    // A scrolling source receives desktop points but delivers a fixed pixel viewport.
    SnowCaptureStreamConfig streamConfig{};
    streamConfig.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
    streamConfig.struct_size = sizeof(streamConfig);
    streamConfig.x = qRound(geometry.x);
    streamConfig.y = qRound(geometry.y);
    streamConfig.width = 80;
    streamConfig.height = 60;
    streamConfig.target_fps = 30;
    streamConfig.min_fps = 1;
    streamConfig.buffer_depth = 3;
    streamConfig.capture_retry_count = 1;
    streamConfig.max_consecutive_errors = 30;
    streamConfig.capture_backend = SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT;
    streamConfig.exclusions.windows = &window;
    streamConfig.exclusions.window_count = 1;
    std::unique_ptr<SnowCaptureStream, decltype(&snow_capture_stream_destroy)> stream(
        snow_capture_stream_create_region(&streamConfig), snow_capture_stream_destroy);
    if (!stream) {
        std::cerr << "region stream: " << snow_capture_last_error_message() << '\n';
        return {};
    }
    bool received = false;
    for (int i = 0; i < 10 && !received; ++i) {
        SnowCaptureStreamEvent event{};
        if (!snow_capture_stream_receive(stream.get(), 500, &event))
            return {};
        if (event.kind == SNOW_CAPTURE_STREAM_EVENT_FRAME) {
            SnowCaptureStreamFrameInfo info{};
            const auto regionSpec = screenshotSelectionRenderSpec(
                displays, QRect(streamConfig.x, streamConfig.y, 80, 60));
            received = snow_capture_stream_frame_info(event.frame, &info) &&
                       QSize(static_cast<int>(info.width), static_cast<int>(info.height)) ==
                           regionSpec.pixelSize;
            snow_capture_stream_frame_release(event.frame);
            if (!received) {
                std::cerr << "region viewport mismatch\n";
                return {};
            }
        } else if (event.kind == SNOW_CAPTURE_STREAM_EVENT_ERROR ||
                   event.kind == SNOW_CAPTURE_STREAM_EVENT_ENDED) {
            std::cerr << "region acquisition: " << snow_capture_last_error_message() << '\n';
            return {};
        }
    }
    if (!received || !snow_capture_stream_stop(stream.get()))
        return {};
    return image;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--scroll-target"))) {
        ScrollFixture target;
        target.resize(240, 180);
        target.show();
        target.raise();
        target.activateWindow();
        [NSApp activateIgnoringOtherApps:YES];
        QTimer ready;
        QObject::connect(&ready, &QTimer::timeout, &app, [&] {
            if (app.applicationState() != Qt::ApplicationActive)
                return;
            ready.stop();
            const auto frame = target.frameGeometry();
            auto* view = reinterpret_cast<NSView*>(target.winId());
            std::cout << frame.x() << ' ' << frame.y() << ' ' << frame.width() << ' '
                      << frame.height() << ' ' << view.window.windowNumber << std::endl;
        });
        ready.start(10);
        QTimer::singleShot(15000, &app, [] { QCoreApplication::exit(2); });
        return app.exec();
    }
    uint32_t activeDisplays = 0;
    const bool desktopAvailable =
        CGGetActiveDisplayList(0, nullptr, &activeDisplays) == kCGErrorSuccess &&
        activeDisplays > 0;
    if (!desktopAvailable || QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        !CGPreflightScreenCaptureAccess()) {
        std::cout << "SKIP: requires an unlocked active desktop and an existing Screen Recording "
                     "grant; no permission is requested.\n";
        return 77;
    }
    CaptureFixture fixture;
    fixture.resize(160, 120);
    fixture.setWindowTitle(QStringLiteral("Snow Shot screenshot fixture"));
    fixture.show();
    app.processEvents();
    auto* view = reinterpret_cast<NSView*>(fixture.winId());
    const auto window = static_cast<CGWindowID>(view.window.windowNumber);
    auto result = std::async(std::launch::async, [window] {
        if (capture(window, false).isNull())
            return QImage();
        return capture(window, true);
    });
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            loop.quit();
    });
    poll.start(10);
    loop.exec();
    poll.stop();
    const QImage image = result.get();
    if (image.isNull()) {
        std::cerr << "native screenshot failed\n";
        return 1;
    }
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("selection.png"));
    if (!image.save(path) || QImage(path).size() != image.size())
        return 1;
    auto original = std::make_unique<QMimeData>();
    const auto* mime = app.clipboard()->mimeData();
    if (mime)
        for (const auto& format : mime->formats())
            original->setData(format, mime->data(format));
    app.clipboard()->setImage(image);
    app.processEvents();
    const bool clipboardMatches = app.clipboard()->image().size() == image.size();
    app.clipboard()->setMimeData(original.release());
    if (!clipboardMatches)
        return 1;
    fixture.hide();
    if (!scrollInputSmoke())
        return 1;
    std::cout << "ScreenCaptureKit desktop/window, cursor modes, region stream, exclusions, "
                 "cancellation, retained leases, direct targets, scroll input, PNG and clipboard "
                 "smoke passed: "
              << image.width() << 'x' << image.height() << '\n';
    return 0;
}
