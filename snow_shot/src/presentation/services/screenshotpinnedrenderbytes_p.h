// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <limits>
#include <optional>

namespace screenshot_pinned_render {
inline bool addBytes(qint64& total, qint64 bytes) {
    if (bytes < 0 || total > std::numeric_limits<qint64>::max() - bytes)
        return false;
    total += bytes;
    return true;
}

// QImage scanlines are aligned to four bytes, including Alpha8 masks.
inline std::optional<qint64> rasterBytes(QSize size, int depth = 32) {
    if (size.isEmpty() || depth <= 0 ||
        qint64(size.width()) > (std::numeric_limits<qint64>::max() - 31) / depth)
        return std::nullopt;
    const qint64 stride = ((qint64(size.width()) * depth + 31) / 32) * 4;
    if (stride > std::numeric_limits<qint64>::max() / size.height())
        return std::nullopt;
    return stride * size.height();
}
} // namespace screenshot_pinned_render
