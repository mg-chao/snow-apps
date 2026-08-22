#include "snow_shot/presentation/screenshotwindowcapture.h"

#include <cstdint>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "snow_capture.h"
#endif

namespace {

#if defined(Q_OS_WIN) || defined(_WIN32)
QString nativeCaptureError(const char* fallback) {
    const char* message = snow_capture_last_error_message();
    if (message != nullptr && *message != '\0') {
        return QString::fromUtf8(message);
    }
    return QString::fromUtf8(fallback);
}

void releaseFrameLease(void* lease) {
    snow_capture_frame_lease_release(static_cast<SnowCaptureFrameLease*>(lease));
}
#endif

} // namespace

struct ScreenshotWindowCapture::Impl final {
    explicit Impl(quintptr nativeWindowHandle) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (nativeWindowHandle == 0) {
            error = QStringLiteral("Window handle is null");
            return;
        }

        SnowCaptureWindowSessionConfig config{};
        config.hwnd = static_cast<intptr_t>(nativeWindowHandle);
        config.capture_retry_count = 1;
        config.wgc_update_mode = SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY;
        config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
        session = snow_capture_window_session_create(&config);
        if (session == nullptr) {
            error = nativeCaptureError("Failed to create window capture session");
        }
#else
        Q_UNUSED(nativeWindowHandle);
        error = QStringLiteral("Window capture is only supported on Windows");
#endif
    }

    ~Impl() {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (session != nullptr) {
            snow_capture_window_session_destroy(session);
            session = nullptr;
        }
#endif
    }

    bool isValid() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
        return session != nullptr;
#else
        return false;
#endif
    }

    bool prepare() {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (session == nullptr) {
            return false;
        }
        if (prepared) {
            return true;
        }
        if (snow_capture_window_session_prepare(session) == 0) {
            error = nativeCaptureError("Failed to prepare window capture session");
            return false;
        }
        prepared = true;
        error.clear();
        return true;
#else
        return false;
#endif
    }

    std::optional<ScreenshotWindowCaptureFrame> capture() {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (!prepare()) {
            return std::nullopt;
        }

        SnowCaptureWindowFrameInfo info{};
        if (snow_capture_window_session_capture(session, &info) == 0) {
            error = nativeCaptureError("Failed to capture focused window");
            return std::nullopt;
        }

        constexpr quint64 kBytesPerPixel = 4;
        const quint64 width = static_cast<quint64>(info.width);
        const quint64 height = static_cast<quint64>(info.height);
        const quint64 expectedStride = width * kBytesPerPixel;
        const bool byteCountOverflow =
            expectedStride != 0 && height > std::numeric_limits<quint64>::max() / expectedStride;
        const quint64 expectedBytes = byteCountOverflow ? 0 : expectedStride * height;
        const quint64 maxInt = static_cast<quint64>(std::numeric_limits<int>::max());
        if (info.rgba_bytes == nullptr || byteCountOverflow || info.rgba_len < expectedBytes ||
            width == 0 || height == 0 ||
            expectedStride > std::numeric_limits<std::uint32_t>::max() ||
            info.stride_bytes != static_cast<std::uint32_t>(expectedStride) || width > maxInt ||
            height > maxInt || expectedStride > maxInt ||
            expectedBytes > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
            error = QStringLiteral("Window capture returned an invalid RGBA frame");
            return std::nullopt;
        }

        const int imageWidth = static_cast<int>(width);
        const int imageHeight = static_cast<int>(height);
        SnowCaptureFrameLease* lease = snow_capture_window_session_frame_retain(session);
        if (lease == nullptr) {
            error = nativeCaptureError("Failed to retain window capture frame");
            return std::nullopt;
        }
        QImage image(info.rgba_bytes, imageWidth, imageHeight,
                     static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
                     &releaseFrameLease, lease);
        if (image.isNull()) {
            snow_capture_frame_lease_release(lease);
            error = QStringLiteral("Failed to lease window capture frame");
            return std::nullopt;
        }

        const qint64 rightExclusive = static_cast<qint64>(info.x) + width;
        const qint64 bottomExclusive = static_cast<qint64>(info.y) + height;
        if (rightExclusive > static_cast<qint64>(std::numeric_limits<int>::max()) + 1 ||
            bottomExclusive > static_cast<qint64>(std::numeric_limits<int>::max()) + 1) {
            error = QStringLiteral("Window capture geometry is outside the supported range");
            return std::nullopt;
        }

        ScreenshotWindowCaptureFrame result;
        result.image = std::move(image);
        result.physicalRect = QRect(info.x, info.y, imageWidth, imageHeight);
        error.clear();
        return result;
#else
        return std::nullopt;
#endif
    }

    QString errorMessage() const {
        return error;
    }

    QString error;
    bool prepared = false;
#if defined(Q_OS_WIN) || defined(_WIN32)
    SnowCaptureWindowSession* session = nullptr;
#endif
};

ScreenshotWindowCapture::ScreenshotWindowCapture(quintptr nativeWindowHandle)
    : m_impl(std::make_unique<Impl>(nativeWindowHandle)) {}

ScreenshotWindowCapture::~ScreenshotWindowCapture() = default;

ScreenshotWindowCapture::ScreenshotWindowCapture(ScreenshotWindowCapture&&) noexcept = default;

ScreenshotWindowCapture&
ScreenshotWindowCapture::operator=(ScreenshotWindowCapture&&) noexcept = default;

bool ScreenshotWindowCapture::isValid() const {
    return m_impl != nullptr && m_impl->isValid();
}

bool ScreenshotWindowCapture::prepare() {
    return m_impl != nullptr && m_impl->prepare();
}

std::optional<ScreenshotWindowCaptureFrame> ScreenshotWindowCapture::capture() {
    return m_impl != nullptr ? m_impl->capture() : std::nullopt;
}

QString ScreenshotWindowCapture::errorMessage() const {
    return m_impl != nullptr ? m_impl->errorMessage() : QStringLiteral("Capture helper is empty");
}
