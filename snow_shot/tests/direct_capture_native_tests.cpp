#include "directcapturenative.h"
#include "captureframeimage.h"
#include "snow_capture.h"
#include "snowimageqtcodec.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"

#include <QTemporaryDir>

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

struct SnowCaptureDesktopSessionImpl {};
struct SnowCaptureFrameLeaseImpl {
    std::shared_ptr<std::array<uint8_t, 16>> pixels = std::make_shared<std::array<uint8_t, 16>>();
};
struct SnowCaptureScreenshotResultImpl {
    std::array<SnowCaptureFrameLease, 3> frames;
};

namespace {
int leases = 0;
int sessions = 0;
int snapshots = 0;
intptr_t capturedWindow = 0;
uint32_t capturedFlags = 0;
bool failCapture = false;
bool failSecondDisplay = false;
bool failWindow = false;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template <typename Info> void fillInfo(Info* info, const SnowCaptureFrameLease& frame) {
    info->width = 2;
    info->height = 2;
    info->stride_bytes = 8;
    info->rgba_len = 16;
    info->rgba_bytes = frame.pixels->data();
    info->pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_BGRA8;
}
} // namespace

extern "C" {
const char* snow_capture_last_error_message() {
    return "fixture capture failure";
}
SnowCaptureDesktopSession*
snow_capture_desktop_session_create(const SnowCaptureDesktopSessionConfig* config) {
    require(config->pixel_format == SNOW_CAPTURE_PIXEL_FORMAT_BGRA8,
            "direct desktop capture did not request BGRA pixels");
    ++sessions;
    return new SnowCaptureDesktopSession;
}
void snow_capture_desktop_session_destroy(SnowCaptureDesktopSession* session) {
    --sessions;
    delete session;
}
SnowCaptureScreenshotResult*
snow_capture_desktop_session_capture(SnowCaptureDesktopSession*,
                                     const SnowCaptureScreenshotRequest* request) {
    require(request->version == SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION &&
                request->struct_size == sizeof(*request),
            "invalid desktop capture transaction request");
    capturedWindow = request->focused_window;
    capturedFlags = request->flags;
    if (failCapture)
        return nullptr;
    ++snapshots;
    auto* result = new SnowCaptureScreenshotResult;
    for (size_t i = 0; i < result->frames.size(); ++i)
        result->frames[i].pixels->fill(static_cast<uint8_t>(40 + i * 30));
    *result->frames[2].pixels = {10, 20, 30, 0,   40,  50,  100, 64,
                                 70, 80, 90, 128, 100, 110, 120, 255};
    return result;
}
void snow_capture_screenshot_result_destroy(SnowCaptureScreenshotResult* result) {
    --snapshots;
    delete result;
}
size_t snow_capture_screenshot_result_display_count(const SnowCaptureScreenshotResult*) {
    return 2;
}
uint8_t snow_capture_screenshot_result_display_info(const SnowCaptureScreenshotResult* result,
                                                    size_t index, SnowCaptureFrameInfo* info) {
    if (failSecondDisplay && index == 1)
        return 0;
    fillInfo(info, result->frames[index]);
    info->x = index == 0 ? -2 : 0;
    info->y = -1;
    info->stable_id = index == 0 ? "stable-A" : "stable-B";
    info->name = index == 0 ? "device-A" : "device-B";
    info->backend_kind = SNOW_CAPTURE_BACKEND_DXGI;
    return 1;
}
uint8_t snow_capture_screenshot_result_display_geometry(const SnowCaptureScreenshotResult*,
                                                        size_t index,
                                                        SnowCaptureFrameGeometry* geometry) {
    geometry->coordinate_space = 1;
    geometry->display_id = static_cast<uint32_t>(index + 1);
    geometry->x = 0;
    geometry->y = 0;
    geometry->width = 2;
    geometry->height = 2;
    geometry->backing_scale = 1;
    return 1;
}
uint8_t snow_capture_screenshot_result_focused_window_geometry(const SnowCaptureScreenshotResult*,
                                                               SnowCaptureFrameGeometry* geometry) {
    return snow_capture_screenshot_result_display_geometry(nullptr, 0, geometry);
}

SnowCaptureFrameLease*
snow_capture_screenshot_result_display_retain(const SnowCaptureScreenshotResult* result,
                                              size_t index) {
    ++leases;
    return new SnowCaptureFrameLease(result->frames[index]);
}
uint8_t
snow_capture_screenshot_result_focused_window_info(const SnowCaptureScreenshotResult* result,
                                                   SnowCaptureWindowFrameInfo* info) {
    if (failWindow)
        return 0;
    fillInfo(info, result->frames[2]);
    info->x = -1;
    info->backend_kind = SNOW_CAPTURE_BACKEND_WGC;
    return 1;
}
SnowCaptureFrameLease*
snow_capture_screenshot_result_focused_window_retain(const SnowCaptureScreenshotResult* result) {
    ++leases;
    return new SnowCaptureFrameLease(result->frames[2]);
}
void snow_capture_frame_lease_release(SnowCaptureFrameLease* lease) {
    --leases;
    delete lease;
}
}

int main() {
    using namespace snow_shot::presentation;
    {
        auto* lease = new SnowCaptureFrameLease;
        ++leases;
        *lease->pixels = {25, 50, 100, 128, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255};
        const auto image = capture::imageFromFrameLease(lease, lease->pixels->data(), 16, 2, 2, 8,
                                                        SNOW_CAPTURE_PIXEL_FORMAT_BGRA8,
                                                        capture::FrameAlphaMode::Premultiplied);
        require(image.pixelColor(0, 0).red() >= 199 && image.pixelColor(0, 0).alpha() == 128,
                "ScreenCaptureKit colors must be decoded from premultiplied alpha");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("alpha.png"));
        require(image.save(path) &&
                    QImage(path).pixelColor(0, 0).rgba() == image.pixelColor(0, 0).rgba(),
                "window PNG must preserve straight colors and alpha");
    }
    require(leases == 0, "premultiplied image lease leaked");
    for (const auto target :
         {DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor}) {
        DirectCaptureRequest request;
        request.target = target;
        request.window = 123;
        request.monitorName = QStringLiteral("device-B");
        {
            const auto frame = captureDirectTarget(request);
            require(frame.isValid() && frame.displays.size() == 2,
                    "direct capture lost the full desktop snapshot");
            require(sessions == 0 && snapshots == 0 && leases > 0,
                    "captured images did not outlive the native transaction");
            require(frame.displays[0].physicalBounds == QRect(-2, -1, 2, 2) &&
                        frame.displays[0].stableId == QStringLiteral("stable-A") &&
                        frame.displays[0].image.pixelColor(0, 0).red() == 40 &&
                        frame.displays[1].image.pixelColor(0, 0).red() == 70,
                    "direct capture lost display identity, geometry, or pixels");
            if (target == DirectCaptureTarget::FocusedWindow) {
                require(capturedWindow == 123 && frame.physicalBounds == QRect(-1, 0, 2, 2) &&
                            frame.image.pixelColor(1, 0).red() == 100,
                        "window image was not captured with the desktop transaction");
                const QImage saved =
                    snow_shot::image_codec::decode(snow_shot::image_codec::encodePng(frame.image),
                                                   snow::image::Format::png, "focused-window.png");
                require(!saved.isNull(), "focused-window PNG encoding failed");
                const std::array<int, 4> expectedAlpha{0, 64, 128, 255};
                for (int pixel = 0; pixel < 4; ++pixel) {
                    require(saved.pixelColor(pixel % 2, pixel / 2).alpha() ==
                                expectedAlpha[static_cast<size_t>(pixel)],
                            "focused-window PNG lost native transparency");
                    require(frame.image.pixelColor(pixel % 2, pixel / 2) ==
                                saved.pixelColor(pixel % 2, pixel / 2),
                            "focused-window PNG changed native colors or alpha");
                }
                QTemporaryDir directory;
                require(directory.isValid(), "automatic-save directory could not be created");
                for (const auto format :
                     {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Webp}) {
                    const auto file = ScreenshotImageFileService::saveAutomatically(
                        frame.image, {directory.path()}, format, QStringLiteral("window"));
                    require(file.succeeded(), "focused-window automatic save failed");
                    const QImage decoded = snow_shot::image_codec::decodeFile(
                        file.path, ScreenshotImageFileService::snowImageFormat(format));
                    require(!decoded.isNull(), "focused-window saved image could not be decoded");
                    for (int pixel = 0; pixel < 4; ++pixel) {
                        require(decoded.pixelColor(pixel % 2, pixel / 2).alpha() ==
                                    expectedAlpha[static_cast<size_t>(pixel)],
                                "focused-window automatic save lost native transparency");
                    }
                }
            } else {
                require(capturedWindow == 0 && frame.physicalBounds == QRect(0, -1, 2, 2) &&
                            frame.image.cacheKey() == frame.displays[1].image.cacheKey() &&
                            frame.image.format() == QImage::Format_RGB32 &&
                            frame.image.pixelColor(0, 0).alpha() == 255,
                        "monitor result did not reuse its captured desktop frame");
            }
        }
        require(leases == 0, "direct capture leaked retained desktop or window frames");
        for (const bool restoreColors : {true, false}) {
            request.restoreOriginalScreenColors = restoreColors;
            require(captureDirectTarget(request).isValid(), "color restoration capture failed");
            require(((capturedFlags & SNOW_CAPTURE_SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS) !=
                     0) == restoreColors,
                    "direct capture must preserve the requested color restoration policy");
        }
        for (bool* failure : {&failCapture, &failSecondDisplay, &failWindow}) {
            if (failure == &failWindow && target != DirectCaptureTarget::FocusedWindow)
                continue;
            *failure = true;
            require(!captureDirectTarget(request).isValid(),
                    "partial native transaction was accepted as a successful capture");
            *failure = false;
            require(leases == 0 && sessions == 0 && snapshots == 0,
                    "failed direct capture leaked native resources");
        }
    }
    DirectCaptureRequest missingMonitor;
    missingMonitor.monitorName = QStringLiteral("disconnected-device");
    require(!captureDirectTarget(missingMonitor).isValid(),
            "missing target monitor silently captured another display");
    require(leases == 0 && sessions == 0 && snapshots == 0,
            "missing target monitor leaked native resources");
    return 0;
}
