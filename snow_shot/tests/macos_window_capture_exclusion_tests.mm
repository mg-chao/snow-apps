#include "snow_shot/platform/windowcaptureexclusion.h"
#include "presentation/capture/windowcaptureexclusion.h"
#include "snow_capture.h"
#include "snow_capture_macos.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QScreen>
#include <QThread>
#include <QElapsedTimer>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}
void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}
template <class Work> auto onWorker(Work work) {
    auto future = std::async(std::launch::async, std::move(work));
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        pump(5);
    return future.get();
}
bool green(const uint8_t* bytes, uint32_t width, uint32_t height, uint32_t stride) {
    const auto* pixel = bytes + (height / 2) * stride + (width / 2) * 4;
    return pixel[1] > 170 && pixel[0] < 80 && pixel[2] < 80;
}
bool snapshot(QRect region, uint32_t excluded) {
    return onWorker([=] {
        SnowCaptureRegionSessionConfig config{};
        config.x = region.x();
        config.y = region.y();
        config.width = static_cast<uint32_t>(region.width());
        config.height = static_cast<uint32_t>(region.height());
        config.exclusions = {&excluded, excluded ? 1u : 0u, nullptr, 0};
        auto* session = snow_capture_region_session_create(&config);
        require(session != nullptr, snow_capture_last_error_message());
        SnowCaptureRegionFrameInfo info{};
        const bool captured = snow_capture_region_session_capture(session, &info) != 0;
        const bool result =
            captured && green(info.rgba_bytes, info.width, info.height, info.stride_bytes);
        snow_capture_region_session_destroy(session);
        require(captured, "region snapshot failed");
        return result;
    });
}
bool stream(QRect region, uint32_t excluded) {
    return onWorker([=] {
        SnowCaptureStreamConfig config{};
        config.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.x = region.x();
        config.y = region.y();
        config.width = static_cast<uint32_t>(region.width());
        config.height = static_cast<uint32_t>(region.height());
        config.target_fps = 30;
        config.min_fps = 1;
        config.buffer_depth = 3;
        config.max_consecutive_errors = 3;
        config.exclusions = {&excluded, 1, nullptr, 0};
        auto* session = snow_capture_stream_create_region(&config);
        require(session != nullptr, snow_capture_last_error_message());
        bool found = false;
        for (int attempt = 0; attempt < 30 && !found; ++attempt) {
            SnowCaptureStreamEvent event{};
            if (!snow_capture_stream_receive(session, 200, &event))
                break;
            if (event.frame) {
                SnowCaptureStreamFrameInfo info{};
                info.version = SNOW_CAPTURE_STREAM_FRAME_INFO_VERSION;
                info.struct_size = sizeof(info);
                if (snow_capture_stream_frame_info(event.frame, &info))
                    found = green(info.rgba_bytes, info.width, info.height, info.stride_bytes);
                snow_capture_stream_frame_release(event.frame);
            }
        }
        snow_capture_stream_destroy(session);
        return found;
    });
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        using namespace snow_shot::platform;
        QWidget background(nullptr, Qt::FramelessWindowHint);
        QWidget overlay(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        auto bounds = app.primaryScreen()->availableGeometry();
        background.setGeometry(QRect(bounds.center() - QPoint(120, 100), QSize(240, 200)));
        background.setStyleSheet(QStringLiteral("background: rgb(0,255,0)"));
        overlay.setGeometry(QRect(bounds.center() - QPoint(40, 40), QSize(80, 80)));
        overlay.setStyleSheet(QStringLiteral("background: rgb(255,0,0)"));
        background.show();
        overlay.show();
        pump(200);
        NSWindow* window = native(overlay);
        window.sharingType = NSWindowSharingReadOnly;
        require(!setWindowExcludedFromCapture(nullptr, true), "null window must fail");
        require(!captureWindowId(nullptr), "null window has no ID");
        {
            snow_shot::presentation::WindowCaptureExclusion lease(setWindowExcludedFromCapture);
            require(lease.exclude(&overlay), "native exclusion succeeds");
            require(lease.exclude(&overlay), "duplicate exclusion succeeds");
            require(window.sharingType == NSWindowSharingNone, "sharing policy disabled");
            require(lease.windowIds(captureWindowId) ==
                        QVector<std::uint32_t>{static_cast<uint32_t>(window.windowNumber)},
                    "WindowServer ID is returned");
        }
        require(window.sharingType == NSWindowSharingReadOnly, "prior sharing policy restored");
        // Numeric 2 is the legacy read/write policy, deprecated in the macOS 15 SDK.
        for (const auto policy :
             {NSWindowSharingNone, NSWindowSharingReadOnly, static_cast<NSWindowSharingType>(2)}) {
            window.sharingType = policy;
            const auto original = window.sharingType;
            require(setWindowExcludedFromCapture(&overlay, true), "direct exclusion succeeds");
            require(setWindowExcludedFromCapture(&overlay, true), "native repeat is idempotent");
            require(setWindowExcludedFromCapture(&overlay, false), "direct restore succeeds");
            require(window.sharingType == original, "restore preserves the exact native policy");
            require(setWindowExcludedFromCapture(&overlay, false),
                    "restore without a lease succeeds");
            require(window.sharingType == original, "repeated restore preserves sharing policy");
        }
        require(onWorker([&overlay] {
                    return !setWindowExcludedFromCapture(&overlay, true) &&
                           !captureWindowId(&overlay);
                }),
                "native window operations reject worker threads");
        auto* temporary = new QWidget;
        temporary->show();
        pump(20);
        NSWindow* retained = [native(*temporary) retain];
        retained.sharingType = NSWindowSharingReadOnly;
        require(setWindowExcludedFromCapture(temporary, true), "temporary exclusion succeeds");
        delete temporary;
        require(retained.sharingType == NSWindowSharingReadOnly,
                "destruction restores retained original window");
        [retained release];
        if (app.arguments().contains(QStringLiteral("--pixels"))) {
            if (!snow_capture_macos_permission_check())
                return 77;
            window.sharingType = NSWindowSharingReadOnly;
            const QRect region(bounds.center() - QPoint(16, 16), QSize(32, 32));
            require(!snapshot(region, 0), "visible overlay must appear before exclusion");
            require(setWindowExcludedFromCapture(&overlay, true),
                    "pixel fixture exclusion succeeds");
            const auto id = captureWindowId(&overlay).value();
            require(snapshot(region, id), "excluded snapshot reveals background pixels");
            require(stream(region, id), "excluded continuous stream reveals background pixels");
            require(overlay.isVisible(), "exclusion must never hide the UI");
            require(setWindowExcludedFromCapture(&overlay, false), "restore succeeds");
            pump(100);
            require(!snapshot(region, 0), "restored overlay is captured again");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
