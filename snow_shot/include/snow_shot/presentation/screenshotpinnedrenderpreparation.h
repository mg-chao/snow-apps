// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotpinnedimagesnapshot.h"
#include "snow_shot/presentation/screenshotpinnedtextpreparation.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <algorithm>
#include <cmath>
#include <limits>

struct ScreenshotPinnedRenderPreparation final {
    ScreenshotPinnedTextPreparation text;
    QFont font;
    QColor textColor;
    QColor backgroundColor;
    bool backgroundFill = false;
};

// Capture presentation preferences before handing owned pinned state to an import worker.
inline ScreenshotPinnedRenderPreparation
captureScreenshotPinnedRenderPreparation(QWidget* owner = nullptr) {
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(owner);
    const bool fill = snow_shot::storage::ApplicationStorage::instance()
                          .configuration()
                          .value(QStringLiteral("text_recognition/fill_style"))
                          .toString() == QStringLiteral("background_fill");
    return {captureScreenshotPinnedTextPreparation(), QApplication::font(), theme.colorText,
            theme.colorBgContainer.isValid() ? theme.colorBgContainer : QColor(Qt::white), fill};
}

inline QSizeF screenshotPinnedScaleReferenceSize(QSize initialWindowSize,
                                                 QSizeF exactInitialWindowSize, int quarterTurns,
                                                 const std::optional<QRectF>& cropRect,
                                                 QSize originalPixelSize, QRectF canvasSourceRect,
                                                 const ScreenshotResultStyle& style) {
    QSizeF full = exactInitialWindowSize.isValid() && !exactInitialWindowSize.isEmpty()
                      ? exactInitialWindowSize
                      : QSizeF(initialWindowSize);
    if (quarterTurns % 2)
        full.transpose();
    if (!cropRect || originalPixelSize.isEmpty())
        return full;
    const qreal effect = style.effectWidth();
    QSizeF fraction(cropRect->width() / originalPixelSize.width() * canvasSourceRect.width() /
                        (canvasSourceRect.width() + 2 * effect),
                    cropRect->height() / originalPixelSize.height() * canvasSourceRect.height() /
                        (canvasSourceRect.height() + 2 * effect));
    if (quarterTurns % 2)
        fraction.transpose();
    return {full.width() * fraction.width(), full.height() * fraction.height()};
}

inline double screenshotPinnedRestoredScalePercent(QSizeF baseline, QSize encodedSize,
                                                   double savedScalePercent,
                                                   bool preserveExactScale) {
    const double derived = 100.0 * encodedSize.width() / std::max(qreal(1e-6), baseline.width());
    if (preserveExactScale && std::isfinite(savedScalePercent) && savedScalePercent > 0 &&
        savedScalePercent <= 1000 &&
        QSize(std::max(1, qRound(baseline.width() * savedScalePercent / 100)),
              std::max(1, qRound(baseline.height() * savedScalePercent / 100))) == encodedSize)
        return savedScalePercent;
    return derived;
}

inline qreal screenshotPinnedRenderScale(double scalePercent, QSize transformedPixels,
                                         QRectF backgroundRect) {
    qreal scale = scalePercent / 100.0;
    if constexpr (snow_shot::storage::kPinnedGeometryUnits ==
                  snow_shot::storage::PinnedGeometryUnits::LogicalPixels) {
        if (backgroundRect.isEmpty())
            return 0;
        scale *= std::min(transformedPixels.width() / backgroundRect.width(),
                          transformedPixels.height() / backgroundRect.height());
    }
    return scale;
}

namespace snow_shot::presentation {

[[nodiscard]] ScreenshotPinnedImageSnapshot readScreenshotPinnedImageSnapshot(
    storage::PinnedWindowRecord record, const ScreenshotPinnedRenderPreparation& preparation,
    ScreenshotClipboardContentReader::CancellationCheck cancelled = {},
    ScreenshotClipboardContentReader::AllocationCheck allocate = {});

// Synchronous counterpart for callers already running on an admitted export/import worker.
[[nodiscard]] ScreenshotExportImageResult renderScreenshotPinnedImageRecord(
    storage::PinnedWindowRecord record, const ScreenshotPinnedRenderPreparation& preparation,
    ScreenshotClipboardContentReader::CancellationCheck cancelled = {},
    ScreenshotClipboardContentReader::AllocationCheck allocate = {});

} // namespace snow_shot::presentation
