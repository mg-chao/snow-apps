#pragma once

#include "snow_shot/presentation/screenshotimagerowsource.h"
#include <QDateTime>
#include <QRectF>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <optional>

class QIODevice;

enum class ScreenshotPdfPageSize { ImageSize, PortraitA4, LandscapeA4 };

struct ScreenshotPdfOptions {
    ScreenshotPdfPageSize pageSize = ScreenshotPdfPageSize::PortraitA4;
    int quality = 100;
    QString title{};
    QDateTime creationTime{};
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
using Progress = std::function<void(qreal)>;
// verifiedOpaque may be set only after every source pixel's alpha was established as 255.
[[nodiscard]] std::shared_ptr<Payload> prepare(const ScreenshotImageRowSource& source, int quality,
                                               QString* error, bool verifiedOpaque = false,
                                               const Progress& progress = {});
struct Layer {
    std::shared_ptr<const Payload> payload;
    QRectF imagePoints;
    // Top-left based, like imagePoints. A clip allows the image to extend beyond the page.
    std::optional<QRectF> clipPoints;
};
// The producer is invoked once per page, immediately before writing that page. Its payloads
// retire before the next invocation, keeping document working memory independent of page count.
struct Page {
    // Single-image producers can continue to use payload and geometry.imagePoints.
    std::shared_ptr<const Payload> payload;
    Layout geometry;
    QList<Layer> layers;
};
using PageProducer = std::function<bool(qsizetype, Page*, QString*)>;
using DocumentProgress = std::function<void(qsizetype, qreal)>;
[[nodiscard]] bool writeDocument(qsizetype pageCount, const PageProducer& produce,
                                 QIODevice* output, const ScreenshotPdfOptions& options,
                                 QString* error, const Cancelled& cancelled = {},
                                 const DocumentProgress& progress = {});
[[nodiscard]] bool write(const Payload& payload, QIODevice* output,
                         const ScreenshotPdfOptions& options, QString* error,
                         const Cancelled& cancelled = {});
[[nodiscard]] bool decodeTiles(const Payload& payload,
                               const std::function<bool(QRect, const QImage&)>& consume,
                               QString* error, const Cancelled& cancelled = {}, QRect region = {});
} // namespace screenshot_pdf
