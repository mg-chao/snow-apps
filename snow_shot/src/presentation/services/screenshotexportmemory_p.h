// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snowimageqtcodec.h"
#include "screenshotpinnedrenderbytes_p.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"

#include <algorithm>
#include <limits>

namespace screenshot_export_memory {
inline qint64 clipboardWorkingBytes(QSize size, qint64 pngBytes) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const auto raster = screenshot_pinned_render::rasterBytes(size);
    if (!raster)
        return std::numeric_limits<qint64>::max();
    qint64 bytes = *raster; // DIB storage, including alpha when present.
    if (!screenshot_pinned_render::addBytes(bytes, pngBytes) ||
        !screenshot_pinned_render::addBytes(bytes, 2 * 1024 * 1024))
        return std::numeric_limits<qint64>::max();
    return bytes;
#else
    Q_UNUSED(size);
    Q_UNUSED(pngBytes);
    return 0; // Qt MIME data retains shared encoded bytes without a native raster copy.
#endif
}

inline qint64 encodingWorkingBytes(QSize size, ScreenshotImageFileFormat format,
                                   ScreenshotImageEncodingOptions encoding, qint64 outputLimit,
                                   bool clipboard = false) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return qint64{96} * 1024 * 1024; // Bounded tile band, masks, JPEG/Flate and row cache.
    const auto raster = screenshot_pinned_render::rasterBytes(size);
    const auto codec = snow_shot::image_codec::encodeWorkingBytes(
        size, ScreenshotImageFileService::snowImageFormat(format),
        ScreenshotImageFileService::encodeOptions(format, encoding));
    if (!raster || !codec)
        return std::numeric_limits<qint64>::max();
    qint64 bytes = *codec;
    // Allow compression overhead and QByteArray growth. File encodings spill at outputLimit;
    // canonical clipboard PNGs have no spill destination and must account for the full output.
    qint64 encoded = *raster;
    if (!screenshot_pinned_render::addBytes(encoded, *raster / 8 + 64 * 1024))
        return std::numeric_limits<qint64>::max();
    const qint64 retained = std::min(encoded, outputLimit);
    if (!screenshot_pinned_render::addBytes(bytes, retained) ||
        !screenshot_pinned_render::addBytes(bytes, retained) ||
        (clipboard &&
         !screenshot_pinned_render::addBytes(bytes, clipboardWorkingBytes(size, encoded))))
        return std::numeric_limits<qint64>::max();
    return bytes;
}

inline qint64 resizedEncodingWorkingBytes(QSize sourceSize, QSize outputSize,
                                          ScreenshotImageFileFormat format,
                                          ScreenshotImageEncodingOptions encoding,
                                          bool pixelsPrepared) {
    qint64 bytes = encodingWorkingBytes(outputSize, format, encoding, 0);
    if (sourceSize == outputSize)
        return bytes;
    const auto resize = snow_shot::image_codec::resizeResources(sourceSize, outputSize);
    const auto raster = screenshot_pinned_render::rasterBytes(outputSize);
    if (!resize || !raster || !screenshot_pinned_render::addBytes(bytes, resize->retainedBytes) ||
        !screenshot_pinned_render::addBytes(bytes, resize->scratchBytes) ||
        (!pixelsPrepared && *raster <= qint64{64} * 1024 * 1024 &&
         !screenshot_pinned_render::addBytes(bytes, *raster)))
        return std::numeric_limits<qint64>::max();
    return bytes;
}

inline qint64 previewWorkingBytes(QSize sourceSize) {
    const auto raster = screenshot_pinned_render::rasterBytes(sourceSize);
    if (!raster)
        return std::numeric_limits<qint64>::max();
    // A native decoded frame can coexist with the bounded 16 MiB display raster, row conversion,
    // and decoder scratch. Row-backed sources use less, but do not declare that until preparation.
    qint64 bytes = *raster;
    if (!screenshot_pinned_render::addBytes(bytes, *raster) ||
        !screenshot_pinned_render::addBytes(bytes, qint64{64} * 1024 * 1024))
        return std::numeric_limits<qint64>::max();
    return bytes;
}
} // namespace screenshot_export_memory
