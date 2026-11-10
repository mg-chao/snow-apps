// SPDX-License-Identifier: GPL-3.0-only
#include "imageimportsourcereader_p.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QFileInfo>
#include <algorithm>
#include <utility>

namespace snow_shot::presentation {

QImage readImageImportFile(const QString& path,
                           const std::optional<ScreenshotClipboardLocalImage>& snapshot,
                           ScreenshotClipboardContentReader::CancellationCheck cancelled,
                           ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto file = snapshot;
    if (!file && ScreenshotClipboardContentReader::supportedImageFileExtensions().contains(
                     QFileInfo(path).suffix().toLower())) {
        const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles(
            {path}, cancelled, ScreenshotClipboardContentReader::FilePurpose::ImageImport);
        if (!files.isEmpty())
            file = files.first();
    }
    return file ? ScreenshotClipboardContentReader::decodeImageFile(*file, std::move(cancelled),
                                                                    std::move(allocate))
                : QImage();
}

ImageImportSource
readImageImportPinnedRecord(const QString& recordId,
                            ScreenshotClipboardContentReader::CancellationCheck cancelled,
                            ScreenshotClipboardContentReader::AllocationCheck allocate,
                            const ScreenshotPinnedRenderPreparation& preparation) {
    ImageImportSource result;
    if (cancelled && cancelled())
        return result;
    qint64 allocatedBytes = 0;
    const auto reserve = [&allocate, &allocatedBytes, &cancelled](qint64 bytes) {
        if (cancelled && cancelled())
            return false;
        // Pin restoration and original-text rendering can request different transient capacities.
        // Keep the largest reservation until preparation replaces it with retained image bytes.
        allocatedBytes = std::max(allocatedBytes, bytes);
        return !allocate || allocate(allocatedBytes);
    };
    auto record =
        storage::ApplicationStorage::instance().pinnedWindows().loadRecord(recordId, reserve);
    if (!record || (cancelled && cancelled()))
        return result;
    result.originalFileName = record->originalFileName;
    result.createdUtc = record->createdUtc;
    auto rendered =
        renderScreenshotPinnedImageRecord(std::move(*record), preparation, cancelled, reserve);
    result.image = std::move(rendered.image);
    result.error = std::move(rendered.error);
    return result;
}

QImage readImageImportClipboard(ScreenshotClipboardContentSnapshot snapshot,
                                ScreenshotClipboardContentReader::CancellationCheck cancelled,
                                ScreenshotClipboardContentReader::AllocationCheck allocate) {
    auto content = ScreenshotClipboardContentReader::decode(
        std::move(snapshot), std::move(cancelled), std::move(allocate));
    // Retain only native pixels. Formatted documents and other clipboard payloads are unnecessary
    // once the editor has accepted the decoded source.
    return content ? std::move(content->image) : QImage();
}

} // namespace snow_shot::presentation
