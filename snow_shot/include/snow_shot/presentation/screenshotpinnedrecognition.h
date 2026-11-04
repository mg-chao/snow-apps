// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include <QFont>

[[nodiscard]] ScreenshotRecognitionResults
decodeScreenshotPinnedRecognition(const QByteArray& bytes, const QFont& font);
[[nodiscard]] std::shared_ptr<ScreenshotOcrPresentation> transformScreenshotPinnedOcrPresentation(
    const ScreenshotOcrPresentation& source, const QRectF& sourceRect, const QSize& sourcePixels,
    const QTransform& imageTransform, const QSize& transformedPixels, const QRectF& contentRect);
[[nodiscard]] ScreenshotRecognitionResults
transformScreenshotPinnedRecognition(ScreenshotRecognitionResults results, const QRectF& sourceRect,
                                     const QSize& sourcePixels, const QTransform& imageTransform,
                                     const QSize& transformedPixels, const QRectF& contentRect);
