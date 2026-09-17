#include "screenshotcaptureworker.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include "captureframeimage.h"

#include "screenshotcaptureperfinstrumentation.h"
#include "snow_shot/presentation/screenshotcapturecoordinator.h"
#include "snow_shot/presentation/capture/screenshotcapturepolicy.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_capture.h"

#include <QDebug>
#include <QImage>
#include <QMetaObject>

#include <limits>
#include <utility>

namespace {
using snow_shot::presentation::capture::imageFromFrameLease;
using snow_shot::presentation::capture::validFrameInfo;

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

void ScreenshotCaptureWorker::refreshLayout(
    quint64 requestId, const QPointer<ScreenshotCaptureCoordinator>& coordinator) {
    const bool ok = ensureSession() && snow_capture_desktop_session_refresh_layout(m_session) != 0;
    if (!coordinator.isNull()) {
        QMetaObject::invokeMethod(
            coordinator,
            [coordinator, requestId, ok]() {
                if (!coordinator.isNull()) {
                    emit coordinator->layoutRefreshed(requestId, ok);
                }
            },
            Qt::QueuedConnection);
    }
}

void ScreenshotCaptureWorker::capture(const ScreenshotCaptureRequest& request,
                                      const QPointer<ScreenshotCaptureCoordinator>& coordinator,
                                      SnowCaptureCancellationToken* cancellationToken) {
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.worker_entry");
    ScreenshotCaptureResult captureResult;
    captureResult.requestId = request.requestId;
    captureResult.purpose = request.purpose;
    if (!ensureSession()) {
        captureResult.errorMessage = nativeCaptureError("Failed to create desktop capture session");
        postCaptureResult(coordinator, std::move(captureResult));
        return;
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.session_ready");

    SnowCaptureScreenshotRequest nativeRequest{};
    nativeRequest.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
    nativeRequest.struct_size = sizeof(nativeRequest);
    nativeRequest.flags =
        request.refreshLayout ? SNOW_CAPTURE_SCREENSHOT_REQUEST_REFRESH_LAYOUT : 0;
    if (request.restoreOriginalScreenColors) {
        nativeRequest.flags |= SNOW_CAPTURE_SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS;
    }
    if (request.captureCursor) {
        nativeRequest.flags |= SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR;
    }
    nativeRequest.cancellation_token = cancellationToken;

    SnowCaptureScreenshotResult* nativeResult = nullptr;
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("capture.native_ffi");
        nativeResult = snow_capture_desktop_session_capture(m_session, &nativeRequest);
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.native_returned");
    if (nativeResult == nullptr) {
        const QString captureError = nativeCaptureError("Screenshot capture failed");
        static_cast<void>(snow_capture_desktop_session_reset_to_prepared(m_session));
        captureResult.errorMessage = captureError;
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
        QImage image = imageFromFrameLease(
            lease, info.rgba_bytes, info.rgba_len, info.width, info.height, info.stride_bytes,
            info.pixel_format, snow_shot::presentation::capture::FrameAlphaMode::Opaque);
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

    if (!valid && captureResult.errorMessage.isEmpty()) {
        captureResult.errorMessage = nativeCaptureError("Screenshot capture returned invalid data");
    }
    if (!valid) {
        captureResult.displays.clear();
    }
    captureResult.succeeded = valid;
    snow_capture_screenshot_result_destroy(nativeResult);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.frames_wrapped");
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("capture.result_posted");
    postCaptureResult(coordinator, std::move(captureResult));
}

bool ScreenshotCaptureWorker::ensureSession() {
    const QString apiMode = snow_shot::storage::ScreenshotSettings().apiMode();
    auto requested = snow_shot::presentation::capture::screenshotApiModeFromValue(
        apiMode.toLatin1().constData());
    if (requested == snow_shot::presentation::capture::ScreenshotApiMode::Auto) {
        requested = snow_shot::presentation::capture::resolveAutoScreenshotApiMode();
    }
    const auto backend =
        snow_shot::presentation::capture::nativeBackendForNormalScreenshot(requested);
    if (m_session != nullptr && m_sessionBackend == backend) {
        return true;
    }

    // Native backend policy is fixed at creation, including for prewarmed sessions.
    if (m_session != nullptr) {
        snow_capture_desktop_session_destroy(m_session);
        m_session = nullptr;
    }

    SnowCaptureDesktopSessionConfig config{};
    config.capture_retry_count = 1;
    config.capture_backend = backend;
#if defined(Q_OS_WIN) || defined(_WIN32)
    config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_BGRA8;
#endif
    m_session = snow_capture_desktop_session_create(&config);
    if (m_session == nullptr) {
        qWarning("Failed to create desktop capture session: %s", snow_capture_last_error_message());
        return false;
    }
    m_sessionBackend = backend;
    snow_shot::diagnostics::logEvent(
        QStringLiteral("snow_shot.capture"), QStringLiteral("capture.backend_ready"),
        {{QStringLiteral("backend"), static_cast<int>(backend)},
         {QStringLiteral("count"), static_cast<qint64>(config.capture_retry_count)}});
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
    snow_shot::diagnostics::logEvent(
        QStringLiteral("snow_shot.capture"), QStringLiteral("capture.finished"),
        {{QStringLiteral("operation"), QString::number(result.requestId)},
         {QStringLiteral("count"), result.displays.size()},
         {QStringLiteral("outcome"), result.succeeded       ? QStringLiteral("succeeded")
                                     : coordinator.isNull() ? QStringLiteral("cancelled")
                                                            : QStringLiteral("failed")}},
        result.succeeded || coordinator.isNull() ? QtInfoMsg : QtWarningMsg);
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
