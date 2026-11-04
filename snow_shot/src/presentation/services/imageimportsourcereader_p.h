// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotpinnedrenderpreparation.h"

namespace snow_shot::presentation {

struct ImageImportSource final {
    QImage image;
    QString originalFileName;
    QDateTime createdUtc;
    QString error;
};

// Worker-side source restoration only. Each editor retains its own admission policy, native pixel
// storage and preview preparation; allocation callbacks reserve before decoding or rasterization.
[[nodiscard]] QImage
readImageImportFile(const QString& path,
                    const std::optional<ScreenshotClipboardLocalImage>& snapshot,
                    ScreenshotClipboardContentReader::CancellationCheck cancelled,
                    ScreenshotClipboardContentReader::AllocationCheck allocate);

[[nodiscard]] ImageImportSource
readImageImportPinnedRecord(const QString& recordId,
                            ScreenshotClipboardContentReader::CancellationCheck cancelled,
                            ScreenshotClipboardContentReader::AllocationCheck allocate,
                            const ScreenshotPinnedRenderPreparation& preparation);

[[nodiscard]] QImage
readImageImportClipboard(ScreenshotClipboardContentSnapshot snapshot,
                         ScreenshotClipboardContentReader::CancellationCheck cancelled,
                         ScreenshotClipboardContentReader::AllocationCheck allocate);

} // namespace snow_shot::presentation
