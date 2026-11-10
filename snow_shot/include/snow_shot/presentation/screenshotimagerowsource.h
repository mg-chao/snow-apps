#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H

#include <QImage>
#include <QSize>
#include <QRect>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>

enum class ScreenshotImageAlphaContent { Unknown, Opaque, NonOpaque };

struct ScreenshotImageRowSource final {
    QSize size;
    // Rows contain straight-alpha RGBA8888 pixels in sRGB, ordered top to bottom.
    std::function<bool(int firstRow, int rowCount, qsizetype destinationStride, uchar* destination,
                       qsizetype destinationSize)>
        readRows;
    std::function<bool()> cancellationRequested;
    // Optional immutable storage for the same pixels exposed by readRows. A null image denotes a
    // genuinely row-backed source that should not be materialized merely to provide a preview.
    QImage backingImage;
    // Optional bounded, approximate sRGB display hint. Its pixel dimensions do not replace size;
    // native pixels remain available through row/region reads for zooming and export.
    QImage previewImage;
    // True only when opacity is verified or structurally guaranteed. False leaves it unknown.
    bool isKnownOpaque = false;
    // Optional native RGBA region read, avoiding offscreen work for wide row-backed sources.
    std::function<bool(QRect region, qsizetype destinationStride, uchar* destination,
                       qsizetype destinationSize)>
        readRegion;
    // Source-specific admission for additional consumer storage. Keep the returned owner with
    // that storage; producers that already preflight their own caches do not reserve twice.
    std::function<std::shared_ptr<void>(qint64 retainedBytes, qint64 scratchBytes, QString* error)>
        reserveResources;
    // Optional cached classification of immutable pixels. Invoke on an export worker, not while
    // snapshotting the source on the GUI thread. Unsupported layouts remain Unknown.
    std::function<ScreenshotImageAlphaContent()> classifyAlpha;

    [[nodiscard]] bool isValid() const {
        return size.isValid() && !size.isEmpty() && static_cast<bool>(readRows);
    }

    [[nodiscard]] ScreenshotImageAlphaContent verifiedAlphaContent() const {
        return isKnownOpaque   ? ScreenshotImageAlphaContent::Opaque
               : classifyAlpha ? classifyAlpha()
                               : ScreenshotImageAlphaContent::Unknown;
    }

    [[nodiscard]] bool hasVerifiedOpaquePixels() const {
        return verifiedAlphaContent() == ScreenshotImageAlphaContent::Opaque;
    }
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEROWSOURCE_H
