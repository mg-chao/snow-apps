#ifndef SNOW_SHOT_PRESENTATION_CAPTUREFRAMEIMAGE_H
#define SNOW_SHOT_PRESENTATION_CAPTUREFRAMEIMAGE_H

#include "snow_capture.h"
#include <QImage>
#include <limits>

namespace snow_shot::presentation::capture {
enum class FrameAlphaMode { Preserve, Premultiplied, Opaque };

inline void releaseFrameLease(void* lease) {
    snow_capture_frame_lease_release(static_cast<SnowCaptureFrameLease*>(lease));
}

inline bool validFrameInfo(const SnowCaptureFrameInfo& info) {
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
        static_cast<quint64>(info.height) > std::numeric_limits<quint64>::max() / expectedStride) {
        return false;
    }
    const quint64 expectedLen = expectedStride * static_cast<quint64>(info.height);
    return info.stride_bytes == static_cast<std::uint32_t>(expectedStride) &&
           info.rgba_len >= expectedLen;
}

inline QImage imageFromFrameLease(SnowCaptureFrameLease* lease, const std::uint8_t* rgbaBytes,
                                  std::size_t rgbaLen, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t strideBytes, std::uint8_t pixelFormat,
                                  FrameAlphaMode alphaMode) {
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
    if (static_cast<quint64>(height) > std::numeric_limits<quint64>::max() / expectedStride) {
        snow_capture_frame_lease_release(lease);
        return {};
    }
    const quint64 expectedLen = expectedStride * static_cast<quint64>(height);
    if (strideBytes != static_cast<std::uint32_t>(expectedStride) || rgbaLen < expectedLen) {
        snow_capture_frame_lease_release(lease);
        return {};
    }

    // Only desktop frames guarantee opaque alpha. Window frames must retain alpha
    // in the format tag as well as the bytes, including during image conversion.
    const QImage::Format format =
        pixelFormat == SNOW_CAPTURE_PIXEL_FORMAT_BGRA8
            ? (alphaMode == FrameAlphaMode::Opaque          ? QImage::Format_RGB32
               : alphaMode == FrameAlphaMode::Premultiplied ? QImage::Format_ARGB32_Premultiplied
                                                            : QImage::Format_ARGB32)
            : (alphaMode == FrameAlphaMode::Premultiplied ? QImage::Format_RGBA8888_Premultiplied
                                                          : QImage::Format_RGBA8888);
    QImage image(rgbaBytes, static_cast<int>(width), static_cast<int>(height),
                 static_cast<int>(strideBytes), format, &releaseFrameLease, lease);
    if (image.isNull()) {
        snow_capture_frame_lease_release(lease);
    }
    return image;
}
} // namespace snow_shot::presentation::capture
#endif
