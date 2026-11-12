// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDIMAGESNAPSHOT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDIMAGESNAPSHOT_H

#include "snow_shot/presentation/screenshotexportartifact.h"

#include <QDateTime>
#include <functional>
#include <optional>

enum class ScreenshotPinnedRenderMode { WindowAppearance, SourceResolution };

// Owned image-export state captured on the GUI thread; rendering never reads a live pin.
struct ScreenshotPinnedImageSnapshot final {
    ScreenshotExportSource source;
    QSize outputSize;
    QString label;
    QString error = {};
    QString originalFileName;
    QDateTime createdUtc;
    // Conservative rendering raster estimate; zero for sources without an estimate.
    qint64 renderingBytes = 0;
};

// GUI-thread lookup. An absent live pin returns nullopt; a failed live snapshot retains its error.
using ScreenshotPinnedImageSnapshotRequest = std::function<void(
    const QString&, std::function<void(std::optional<ScreenshotPinnedImageSnapshot>)>)>;

#endif
