#include "screenshotcaptureworker.h"

#include "screenshotcaptureperfinstrumentation.h"
#include "snow_shot/presentation/screenshotcapturecoordinator.h"

#include "snow_capture.h"

#include <QDebug>
#include <QImage>
#include <QMetaObject>

#include <limits>
#include <utility>

namespace {
void releaseFrameLease(void* lease) {
    snow_capture_frame_lease_release(static_cast<SnowCaptureFrameLease*>(lease));
}

bool validFrameInfo(const SnowCaptureFrameInfo& info) {
    if (info.rgba_bytes == nullptr || info.rgba_len == 0 || info.width == 0 || info.height == 0) {
        return false;
    }

    if (info.pixel_format != SNOW_CAPTURE_PIXEL_FORMAT_RGBA8 &&
        info.pixel_format != SNOW_CAPTURE_PIXEL_FORMAT_BGRA8) {
        return false;
    }

    if (info.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        info.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const quint64 expectedStride = static_cast<quint64>(info.width) * 4ULL;
    if (expectedStride > std::numeric_limits<std::uint32_t>::max() ||
        expectedStride > static_cast<quint64>(std::numeric_limits<int>::max()) ||
        static_cast<quint64>(info.height) >
            std::numeric_limits<quint64>::max() / expectedStride) {
        return false;
    }
    const quint64 expectedLen = expectedStride * static_cast<quint64>(info.height);
    return info.stride_bytes == static_cast<std::uint32_t>(expectedStride) &&
           info.rgba_len >= expectedLen;
}

QImage imageFromFrameLease(SnowCaptureFrameLease* lease, const std::uint8_t* rgbaBytes,
                           std::size_t rgbaLen, std::uint32_t width, std::uint32_t height,
                           std::uint32_t strideBytes, std::uint8_t pixelFormat) {
    if (lease == nullptr || rgbaBytes == nullptr || rgbaLen == 0 || width == 0 || height == 0 ||
        (pixelFormat != SNOW_CAPTURE_PIXEL_FORMAT_RGBA8 &&
         pixelFormat != SNOW_CAPTURE_PIXEL_FORMAT_BGRA8)) {
        if (lease != nullptr) {
            snow_capture_frame_lease_release(lease);
        }
        return {};
    }

    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        static_cast<quint64>(width) * 4ULL >
            static_cast<quint64>(std::numeric_limits<int>::max())) {
        snow_capture_frame_lease_release(lease);
        return {};
    }

    const quint64 expectedStride = static_cast<quint64>(width) * 4ULL;
    if (static_cast<quint64>(height) >
        std::numeric_limits<quint64>::max() / expectedStride) {
        snow_capture_frame_lease_release(lease);
        return {};
    }
    const quint64 expectedLen = expectedStride * static_cast<quint64>(height);
    if (strideBytes != static_cast<std::uint32_t>(expectedStride) || rgbaLen < expectedLen) {
        snow_capture_frame_lease_release(lease);
        return {};
    }

    const QImage::Format format =
        pixelFormat == SNOW_CAPTURE_PIXEL_FORMAT_BGRA8 ? QImage::Format_ARGB32
                                                       : QImage::Format_RGBA8888;
    QImage image(rgbaBytes, static_cast<int>(width), static_cast<int>(height),
                 static_cast<int>(strideBytes), format, &releaseFrameLease, lease);
    if (image.isNull()) {
        snow_capture_frame_lease_release(lease);
    }
    return image;
}

ScreenshotCaptureBackend backendFromNative(std::uint8_t backend) {
    switch (backend) {
    case SNOW_CAPTURE_BACKEND_DXGI:
        return ScreenshotCaptureBackend::Dxgi;
    case SNOW_CAPTURE_BACKEND_WGC:
        return ScreenshotCaptureBackend::WindowsGraphicsCapture;
    case SNOW_CAPTURE_BACKEND_GDI:
        return ScreenshotCaptureBackend::Gdi;
    default:
        return ScreenshotCaptureBackend::Auto;
    }
}

QString nativeCaptureError(const char* fallback) {
    const char* message = snow_capture_last_error_message();
    return QString::fromUtf8(message != nullptr && *message != '\0' ? message : fallback);
}
} // namespace

ScreenshotCaptureWorker::~ScreenshotCaptureWorker() {
    if (m_session != nullptr) {
        snow_capture_desktop_session_destroy(m_session);
        m_session = nullptr;
    }
}

void ScreenshotCaptureWorker::prepare(quint64 requestId,
                                      const QPointer<ScreenshotCaptureCoordinator>& coordinator) {
    const bool ok = prepareSessionIfNeeded();
    postPrepared(requestId, coordinator, ok);
}

void ScreenshotCaptureWorker::refreshLayout(quint64 requestId) {
    Q_UNUSED(requestId);
    if (ensureSession()) {
        snow_capture_desktop_session_refresh_layout(m_session);
    }
}

void ScreenshotCaptureWorker::releaseIdleResources(quint64 requestId) {
    Q_UNUSED(requestId);
    if (m_session != nullptr &&
        snow_capture_desktop_session_release_idle_resources(m_session) == 0) {
        qWarning("Failed to release desktop capture idle resources: %s",
                 snow_capture_last_error_message());
    }
}

void ScreenshotCaptureWorker::capture(
    const ScreenshotCaptureRequest& request,
    const QPointer<ScreenshotCaptureCoordinator>& coordinator,
    SnowCaptureCancellationToken* cancellationToken) {
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.worker_entry");
    ScreenshotCaptureResult captureResult;
    captureResult.requestId = request.requestId;
    if (!ensureSession()) {
        captureResult.errorMessage = nativeCaptureError("Failed to create desktop capture session");
        postCaptureResult(coordinator, std::move(captureResult));
        return;
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.session_ready");

    SnowCaptureScreenshotRequestV1 nativeRequest{};
    nativeRequest.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
    nativeRequest.struct_size = sizeof(nativeRequest);
    nativeRequest.flags = request.refreshLayout ? SNOW_CAPTURE_SCREENSHOT_REQUEST_REFRESH_LAYOUT : 0;
    nativeRequest.focused_window = static_cast<intptr_t>(request.focusedWindowHandle);
    nativeRequest.cancellation_token = cancellationToken;

    SnowCaptureScreenshotResult* nativeResult = nullptr;
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("capture.native_ffi");
        nativeResult = snow_capture_desktop_session_capture_v1(m_session, &nativeRequest);
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.native_returned");
    if (nativeResult == nullptr) {
        captureResult.errorMessage = nativeCaptureError("Screenshot capture failed");
        postCaptureResult(coordinator, std::move(captureResult));
        return;
    }

    const size_t count = snow_capture_screenshot_result_display_count(nativeResult);
    captureResult.displays.reserve(static_cast<int>(count));
    bool valid = count != 0;
    for (size_t index = 0; index < count; ++index) {
        SnowCaptureFrameInfo info{};
        if (snow_capture_screenshot_result_display_info(nativeResult, index, &info) == 0 ||
            !validFrameInfo(info)) {
            valid = false;
            break;
        }

        SnowCaptureFrameLease* lease =
            snow_capture_screenshot_result_display_retain(nativeResult, index);
        QImage image = imageFromFrameLease(lease, info.rgba_bytes, info.rgba_len, info.width,
                                           info.height, info.stride_bytes, info.pixel_format);
        if (image.isNull()) {
            valid = false;
            break;
        }

        CapturedDisplayModel display;
        display.stableId = QString::fromUtf8(info.stable_id != nullptr ? info.stable_id : "");
        display.name = QString::fromUtf8(info.name != nullptr ? info.name : "");
        display.physicalRect =
            QRect(info.x, info.y, static_cast<int>(info.width), static_cast<int>(info.height));
        display.canvasRect = display.physicalRect;
        display.image = std::move(image);
        display.active = true;
        display.backend = backendFromNative(info.backend_kind);
        captureResult.displays.push_back(std::move(display));
    }

    if (valid && request.focusedWindowHandle != 0) {
        SnowCaptureWindowFrameInfoV1 info{};
        info.version = SNOW_CAPTURE_WINDOW_FRAME_INFO_VERSION;
        info.struct_size = sizeof(info);
        if (snow_capture_screenshot_result_focused_window_info_v1(nativeResult, &info) == 0) {
            valid = false;
        } else {
            SnowCaptureFrameLease* lease =
                snow_capture_screenshot_result_focused_window_retain(nativeResult);
            QImage image = imageFromFrameLease(lease, info.rgba_bytes, info.rgba_len, info.width,
                                               info.height, info.stride_bytes, info.pixel_format);
            ScreenshotWindowCaptureFrame focused;
            focused.image = std::move(image);
            focused.physicalRect = QRect(info.x, info.y, static_cast<int>(info.width),
                                         static_cast<int>(info.height));
            focused.backend = backendFromNative(info.backend_kind);
            if (!focused.isValid()) {
                valid = false;
            } else {
                captureResult.focusedWindow = std::move(focused);
            }
        }
    }

    if (!valid && captureResult.errorMessage.isEmpty()) {
        captureResult.errorMessage = nativeCaptureError("Screenshot capture returned invalid data");
    }
    if (!valid) {
        captureResult.displays.clear();
        captureResult.focusedWindow.reset();
    }
    captureResult.succeeded = valid;
    snow_capture_screenshot_result_destroy(nativeResult);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.frames_wrapped");
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.result_posted");
    postCaptureResult(coordinator, std::move(captureResult));
}

bool ScreenshotCaptureWorker::ensureSession() {
    if (m_session != nullptr) {
        return true;
    }

    SnowCaptureDesktopSessionConfig config{};
    config.capture_retry_count = 1;
#if defined(Q_OS_WIN) || defined(_WIN32)
    config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_BGRA8;
#endif
    m_session = snow_capture_desktop_session_create(&config);
    if (m_session == nullptr) {
        qWarning("Failed to create desktop capture session: %s", snow_capture_last_error_message());
        return false;
    }
    return true;
}

bool ScreenshotCaptureWorker::sessionPrepared() const {
    if (m_session == nullptr) {
        return false;
    }

    SnowCaptureDesktopSessionState state{};
    if (snow_capture_desktop_session_state(m_session, &state) == 0) {
        return false;
    }
    return state.prepared != 0;
}

bool ScreenshotCaptureWorker::prepareSessionIfNeeded() {
    if (!ensureSession()) {
        return false;
    }
    if (sessionPrepared()) {
        return true;
    }
    return snow_capture_desktop_session_prepare(m_session) != 0;
}

void ScreenshotCaptureWorker::postPrepared(
    quint64 requestId, const QPointer<ScreenshotCaptureCoordinator>& coordinator, bool ok) {
    if (coordinator.isNull()) {
        return;
    }

    QMetaObject::invokeMethod(
        coordinator,
        [coordinator, requestId, ok]() {
            if (!coordinator.isNull()) {
                emit coordinator->prepared(requestId, ok);
            }
        },
        Qt::QueuedConnection);
}

void ScreenshotCaptureWorker::postCaptureResult(
    const QPointer<ScreenshotCaptureCoordinator>& coordinator, ScreenshotCaptureResult result) {
    if (coordinator.isNull()) {
        return;
    }

    QMetaObject::invokeMethod(
        coordinator,
        [coordinator, result = std::move(result)]() mutable {
            if (!coordinator.isNull()) {
                emit coordinator->captureFinished(std::move(result));
            }
        },
        Qt::QueuedConnection);
}
