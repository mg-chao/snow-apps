#pragma once

#include "snow_shot/presentation/screenshotimagerowsource.h"
#include <QDateTime>
#include <QRectF>
#include <QTemporaryDir>
#include <functional>
#include <memory>

class QIODevice;

enum class ScreenshotPdfPageSize { ImageSize, PortraitA4, LandscapeA4 };

struct ScreenshotPdfOptions {
    ScreenshotPdfPageSize pageSize = ScreenshotPdfPageSize::PortraitA4;
    int quality = 100;
    QString title;
    QDateTime creationTime;
};

namespace screenshot_pdf {
[[nodiscard]] ScreenshotPdfPageSize pageSizeForKey(const QString& key);
struct Layout {
    QSizeF pagePoints;
    QRectF imagePoints;
};
[[nodiscard]] Layout layout(QSize pixels, ScreenshotPdfPageSize pageSize);

// Compressed tiles live on disk, independently of document layout and metadata.
struct Tile {
    QRect rect;
    qint64 offset = 0;
    qsizetype colorBytes = 0;
    qsizetype alphaBytes = 0;
};
struct Payload {
    QTemporaryDir directory;
    QSize size;
    int quality = 100;
    QList<Tile> tiles;
    [[nodiscard]] QString path() const;
};
using Cancelled = std::function<bool()>;
[[nodiscard]] std::shared_ptr<Payload> prepare(const ScreenshotImageRowSource& source, int quality,
                                               QString* error);
[[nodiscard]] bool write(const Payload& payload, QIODevice* output,
                         const ScreenshotPdfOptions& options, QString* error,
                         const Cancelled& cancelled = {});
[[nodiscard]] bool decodeTiles(const Payload& payload,
                               const std::function<bool(QRect, const QImage&)>& consume,
                               QString* error, const Cancelled& cancelled = {});
} // namespace screenshot_pdf
