// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEPREVIEWROWS_P_H
#define SNOW_SHOT_PRESENTATION_COLLAGEPREVIEWROWS_P_H

#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snowimageqtcodec.h"

#include <QImage>
#include <QColorSpace>
#include <cstring>
#include <limits>
#include <mutex>

namespace snow_shot::presentation::image_stitching_detail {
// Borrow only the crop's pixels. Set metadata before sharing its header so Qt does not detach
// the raster. The caller retains the original and its resource owner throughout the resize.
inline ScreenshotImageRowSource croppedRows(const QImage& image, const QRect& crop,
                                            const ScreenshotExportCancellation& cancellation) {
    const int firstX = image.depth() < 8 ? crop.left() / 8 * 8 : crop.left();
    const int prefix = crop.left() - firstX;
    QImage view(image.constScanLine(crop.top()) + qsizetype(firstX) * image.depth() / 8,
                crop.width() + prefix, crop.height(), image.bytesPerLine(), image.format());
    view.setColorTable(image.colorTable());
    view.setColorSpace(image.colorSpace());
    auto source = image_codec::srgbRowSource(view);
    source.cancellationRequested = [cancellation] {
        return cancellation.isCancellationRequested();
    };
    if (prefix == 0)
        return source;
    struct AlignedCrop final {
        ScreenshotImageRowSource source;
        QByteArray row;
        std::mutex mutex;
    };
    auto aligned = std::make_shared<AlignedCrop>();
    aligned->source = std::move(source);
    aligned->row.resize(qsizetype(view.width()) * 4);
    ScreenshotImageRowSource result;
    result.size = crop.size();
    result.cancellationRequested = [cancellation] {
        return cancellation.isCancellationRequested();
    };
    result.readRows = [aligned, prefix, size = crop.size(),
                       cancellation](int first, int count, qsizetype stride, uchar* destination,
                                     qsizetype capacity) {
        const qsizetype rowBytes = qsizetype(size.width()) * 4;
        if (first < 0 || count <= 0 || count > size.height() || first > size.height() - count ||
            !destination || stride < rowBytes ||
            qsizetype(count - 1) > (std::numeric_limits<qsizetype>::max() - rowBytes) / stride ||
            capacity < qsizetype(count - 1) * stride + rowBytes)
            return false;
        const std::lock_guard lock(aligned->mutex);
        for (int row = 0; row < count; ++row) {
            if (cancellation.isCancellationRequested() ||
                !aligned->source.readRows(first + row, 1, aligned->row.size(),
                                          reinterpret_cast<uchar*>(aligned->row.data()),
                                          aligned->row.size()))
                return false;
            std::memcpy(destination + qsizetype(row) * stride,
                        aligned->row.constData() + prefix * 4, size_t(rowBytes));
        }
        return true;
    };
    return result;
}

} // namespace snow_shot::presentation::image_stitching_detail

#endif
