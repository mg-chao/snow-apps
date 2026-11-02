// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"

#include "collageresourcebudget_p.h"
#include "collagetransformedrows_p.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snowimageqtcodec.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QPainter>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace snow_shot::presentation {
struct CollageImageMetadata final {
    explicit CollageImageMetadata(qint64 key) : cacheKey(key) {}
    const qint64 cacheKey;
    std::atomic<int> opacity{-1};
};

namespace {
constexpr int previewExtent = 256;
constexpr qsizetype rowScratchBytes = 8 * 1024 * 1024;
constexpr qint64 conversionCacheBytes = 2 * 1024 * 1024;

QString resourceError() {
    return QCoreApplication::translate(
        "CollageDocument",
        "The collage exceeds the 8 GiB image limit. Try fewer images or smaller layout settings.");
}

QString preparationError() {
    return QCoreApplication::translate("CollageDocument",
                                       "The collage images could not be prepared for export.");
}

QString cancellationError() {
    return QCoreApplication::translate("CollageDocument", "The collage export was cancelled.");
}

void report(QString* destination, const QString& error) {
    if (destination)
        *destination = error;
}

struct CapturedImageLifetime final {
    std::shared_ptr<collage_detail::ResourceReservation> reservation;
    QImage image;
};

struct SourceLifetime final {
    std::shared_ptr<collage_detail::ResourceReservation> source;
    std::shared_ptr<collage_detail::ResourceReservation> preview;
    std::shared_ptr<CollageImageMetadata> metadata;
};

std::shared_ptr<SourceLifetime> sourceLifetime(const QImage& image, const QImage& preview,
                                               QString* error) {
    try {
        auto owner = std::make_shared<SourceLifetime>();
        owner->metadata = std::make_shared<CollageImageMetadata>(image.cacheKey());
        owner->source = collage_detail::reserve(image.sizeInBytes(), error);
        if (!owner->source)
            return {};
        owner->preview = collage_detail::reservePreview(preview.sizeInBytes(), error);
        return owner->preview ? owner : nullptr;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}

bool settingsValid(const CollageSettings& settings) {
    return settings.border >= 0 && settings.spacing >= 0 && settings.background.isValid() &&
           settings.rows >= 1 && settings.columns >= 1 &&
           (settings.direction == CollageDirection::Vertical ||
            settings.direction == CollageDirection::Horizontal ||
            settings.direction == CollageDirection::Custom) &&
           (settings.sizing == CollageSizing::Original ||
            settings.sizing == CollageSizing::Uniform);
}

qint64 columnsForImages(qsizetype imageCount, int rows) {
    return imageCount / rows + (imageCount % rows != 0 ? 1 : 0);
}

QStringList normalizeCustomGrid(CollageSettings& settings, qsizetype imageCount) {
    QStringList fields;
    if (settings.direction != CollageDirection::Custom)
        return fields;
    if (!settings.customGridInitialized) {
        const auto columns = std::max<qint64>(
            1, static_cast<qint64>(std::ceil(std::sqrt(static_cast<double>(imageCount)))));
        settings.columns =
            static_cast<int>(std::min<qint64>(columns, std::numeric_limits<int>::max()));
        settings.rows = static_cast<int>(std::clamp<qint64>(
            columnsForImages(imageCount, settings.columns), 1, std::numeric_limits<int>::max()));
        settings.customGridInitialized = true;
        fields = {QStringLiteral("rows"), QStringLiteral("columns"),
                  QStringLiteral("custom_grid_initialized")};
    }
    const auto columns = std::min<qint64>(columnsForImages(imageCount, settings.rows),
                                          std::numeric_limits<int>::max());
    if (columns > settings.columns) {
        settings.columns = static_cast<int>(columns);
        if (!fields.contains(QStringLiteral("columns")))
            fields.append(QStringLiteral("columns"));
    }
    return fields;
}

int itemIndex(const QVector<CollageItem>& items, quint64 id) {
    for (qsizetype index = 0; index < items.size(); ++index) {
        if (items[index].id == id)
            return static_cast<int>(index);
    }
    return -1;
}

QImage boundedPreview(const QImage& original, const QImage& supplied = {}) {
    QImage preview = supplied.isNull() ? original : supplied;
    if (preview.isNull())
        return {};
    if (preview.width() > previewExtent || preview.height() > previewExtent)
        preview = snowCanvasScaleImage(preview, QSize(previewExtent, previewExtent),
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!preview.isNull() && preview.colorSpace().isValid() &&
        preview.colorSpace() != QColorSpace(QColorSpace::SRgb))
        preview = snowCanvasColorConvertedImage(preview, QColorSpace(QColorSpace::SRgb),
                                                QImage::Format_ARGB32_Premultiplied);
    if (!preview.isNull() && preview.devicePixelRatio() != 1)
        preview.setDevicePixelRatio(1);
    return preview;
}

QTransform canonicalTransform(const QTransform& transform) {
    return {static_cast<qreal>(qRound(transform.m11())),
            static_cast<qreal>(qRound(transform.m12())),
            static_cast<qreal>(qRound(transform.m21())),
            static_cast<qreal>(qRound(transform.m22())),
            0,
            0};
}

bool outputBytes(QSize size, qint64* bytes) {
    if (size.isEmpty() || size.width() > std::numeric_limits<int>::max() / 4)
        return false;
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels > collage_detail::maximumRasterBytes / 4)
        return false;
    *bytes = pixels * 4;
    return true;
}

CollageLayout calculateCustomGridLayout(const QVector<CollageItem>& items,
                                        const CollageSettings& settings, QSize reference) {
    CollageLayout result;
    QVector<QSize> sizes;
    sizes.reserve(items.size());
    QSize cell = reference;
    for (const auto& item : items) {
        const QSize size = transformedSize(item);
        if (size.isEmpty()) {
            result.error = preparationError();
            return result;
        }
        sizes.append(size);
        if (settings.sizing == CollageSizing::Original) {
            cell.setWidth(std::max(cell.width(), size.width()));
            cell.setHeight(std::max(cell.height(), size.height()));
        }
    }
    const qint64 columns =
        std::max<qint64>(settings.columns, columnsForImages(items.size(), settings.rows));
    if (columns > std::numeric_limits<int>::max()) {
        result.error = resourceError();
        return result;
    }
    const qint64 width =
        columns * cell.width() + (columns - 1) * settings.spacing + qint64(settings.border) * 2;
    const qint64 height = qint64(settings.rows) * cell.height() +
                          qint64(settings.rows - 1) * settings.spacing +
                          qint64(settings.border) * 2;
    if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        result.error = resourceError();
        return result;
    }
    result.size = QSize(static_cast<int>(width), static_cast<int>(height));
    qint64 bytes = 0;
    if (!outputBytes(result.size, &bytes)) {
        result.error = resourceError();
        result.size = {};
        return result;
    }
    result.rects.reserve(items.size());
    for (qsizetype index = 0; index < sizes.size(); ++index) {
        QSize size = sizes[index];
        if (settings.sizing == CollageSizing::Uniform) {
            if (qint64(size.width()) * cell.height() >= qint64(size.height()) * cell.width()) {
                const qint64 numerator = qint64(size.height()) * cell.width();
                size = QSize(cell.width(), static_cast<int>(std::max<qint64>(
                                               1, (numerator + size.width() / 2) / size.width())));
            } else {
                const qint64 numerator = qint64(size.width()) * cell.height();
                size = QSize(static_cast<int>(std::max<qint64>(1, (numerator + size.height() / 2) /
                                                                      size.height())),
                             cell.height());
            }
        }
        const qint64 x = settings.border +
                         (index % columns) * (qint64(cell.width()) + settings.spacing) +
                         (cell.width() - size.width()) / 2;
        const qint64 y = settings.border +
                         (index / columns) * (qint64(cell.height()) + settings.spacing) +
                         (cell.height() - size.height()) / 2;
        result.rects.append(QRect(QPoint(static_cast<int>(x), static_cast<int>(y)), size));
    }
    return result;
}

bool imageIsOpaque(const QImage& image, const std::function<bool()>& cancelled) {
    if (!image.hasAlphaChannel())
        return true;
    const bool rgba = image.format() == QImage::Format_RGBA8888 ||
                      image.format() == QImage::Format_RGBA8888_Premultiplied;
    const bool argb = image.format() == QImage::Format_ARGB32 ||
                      image.format() == QImage::Format_ARGB32_Premultiplied;
    if (!rgba && !argb)
        return false;
    for (int y = 0; y < image.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled())
            return false;
        const uchar* pixels = image.constScanLine(y);
        if (rgba) {
            for (int x = 0; x < image.width(); ++x)
                if (pixels[x * 4 + 3] != 255)
                    return false;
        } else {
            const auto* packed = reinterpret_cast<const QRgb*>(pixels);
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(packed[x]) != 255)
                    return false;
        }
    }
    return true;
}

bool cachedImageIsOpaque(const CollageItem& item, const QImage& pixels,
                         const std::function<bool()>& cancelled) {
    const bool canCache = item.metadata && item.metadata->cacheKey == pixels.cacheKey();
    if (canCache) {
        const int known = item.metadata->opacity.load(std::memory_order_acquire);
        if (known >= 0)
            return known != 0;
    }
    const bool opaque = imageIsOpaque(pixels, cancelled);
    if (canCache && (!cancelled || !cancelled()))
        item.metadata->opacity.store(opaque ? 1 : 0, std::memory_order_release);
    return opaque;
}

struct PreparedItem final {
    ScreenshotImageRowSource rows;
    QRect rect;
    bool opaque = false;
};

struct PreparedRows final {
    struct StorageRange final {
        std::uintptr_t first = 0;
        std::uintptr_t end = 0;
    };
    // Declare leases first so reverse destruction retires readers, rasters and scratch
    // before their accounted capacity becomes available to another export.
    std::shared_ptr<collage_detail::ResourceReservation> reservation;
    std::shared_ptr<collage_detail::ResourceReservation> working;
    QVector<std::shared_ptr<void>> sourceOwners;
    std::shared_ptr<collage_detail::ResourceReservation> previewOwner;
    QVector<PreparedItem> items;
    CollageLayout layout;
    QColor background;
    std::function<bool()> cancelled;
    QVector<QImage> inputImages;
    QVector<StorageRange> storageRanges;
    QVector<std::uintptr_t> storageEnds;
    QVector<qsizetype> rowOrder;
    QVector<int> prefixEnds;
    QVector<int> edges;
    QVector<qsizetype> active;
    int activeFirst = -1;
    int activeEnd = -1;
    std::mutex mutex;
    std::vector<uchar> scratch;

    bool cancellationRequested() const {
        return cancelled && cancelled();
    }

    bool indexRows() {
        rowOrder.reserve(items.size());
        active.reserve(items.size());
        edges.reserve(items.size() * 2 + 2);
        edges.append(0);
        edges.append(layout.size.height());
        for (qsizetype index = 0; index < items.size(); ++index) {
            rowOrder.append(index);
            edges.append(items[index].rect.top());
            edges.append(items[index].rect.top() + items[index].rect.height());
        }
        std::sort(rowOrder.begin(), rowOrder.end(), [this](qsizetype a, qsizetype b) {
            return items[a].rect.top() < items[b].rect.top();
        });
        prefixEnds.reserve(items.size());
        int end = 0;
        for (const auto index : rowOrder) {
            end = std::max(end, items[index].rect.top() + items[index].rect.height());
            prefixEnds.append(end);
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        storageRanges.reserve(inputImages.size());
        for (const auto& image : inputImages) {
            const auto first = reinterpret_cast<std::uintptr_t>(image.constBits());
            const auto bytes = static_cast<std::uintptr_t>(image.sizeInBytes());
            if (bytes > std::numeric_limits<std::uintptr_t>::max() - first)
                return false;
            storageRanges.append({first, first + bytes});
        }
        std::sort(storageRanges.begin(), storageRanges.end(),
                  [](const StorageRange& a, const StorageRange& b) { return a.first < b.first; });
        std::uintptr_t previous = 0;
        storageEnds.reserve(storageRanges.size());
        for (const auto& range : storageRanges) {
            previous = std::max(previous, range.end);
            storageEnds.append(previous);
        }
        return true;
    }

    bool validDestination(const uchar* destination, qsizetype extent) const {
        const auto first = reinterpret_cast<std::uintptr_t>(destination);
        const auto bytes = static_cast<std::uintptr_t>(extent);
        if (bytes > std::numeric_limits<std::uintptr_t>::max() - first)
            return false;
        const auto after = std::lower_bound(
            storageRanges.begin(), storageRanges.end(), first + bytes,
            [](const StorageRange& range, std::uintptr_t end) { return range.first < end; });
        return after == storageRanges.begin() ||
               storageEnds[after - storageRanges.begin() - 1] <= first;
    }

    void selectRows(int first, int end) {
        if (first >= activeFirst && end <= activeEnd)
            return;
        active.clear();
        const auto begin = std::upper_bound(prefixEnds.begin(), prefixEnds.end(), first);
        for (auto position = begin - prefixEnds.begin(); position < rowOrder.size(); ++position) {
            const auto index = rowOrder[position];
            const auto& rect = items[index].rect;
            if (rect.top() >= end)
                break;
            if (rect.top() + rect.height() > first)
                active.append(index);
        }
        std::sort(active.begin(), active.end(), [this](qsizetype a, qsizetype b) {
            return items[a].rect.left() < items[b].rect.left();
        });
        const auto next = std::upper_bound(edges.begin(), edges.end(), first);
        activeFirst = first;
        activeEnd = next != edges.end() && end <= *next ? *next : end;
    }

    bool read(int first, int count, qsizetype stride, uchar* destination, qsizetype capacity) {
        return readRegion(QRect(0, first, layout.size.width(), count), stride, destination,
                          capacity);
    }

    bool readRegion(QRect region, qsizetype stride, uchar* destination, qsizetype capacity) {
        const std::lock_guard lock(mutex);
        const int first = region.top();
        const int count = region.height();
        if (region.width() <= 0 || region.left() < 0 || region.width() > layout.size.width() ||
            region.left() > layout.size.width() - region.width())
            return false;
        const qsizetype rowBytes = qsizetype(region.width()) * 4;
        if (cancellationRequested() || !destination || first < 0 || count <= 0 ||
            count > layout.size.height() || first > layout.size.height() - count ||
            stride < rowBytes || capacity < rowBytes ||
            qsizetype(count - 1) > (capacity - rowBytes) / stride)
            return false;
        if (!validDestination(destination, qsizetype(count - 1) * stride + rowBytes))
            return false;

        const uchar red = static_cast<uchar>(background.red());
        const uchar green = static_cast<uchar>(background.green());
        const uchar blue = static_cast<uchar>(background.blue());
        const uchar alpha = static_cast<uchar>(background.alpha());
        selectRows(first, first + count);
        const auto fill = [red, green, blue, alpha](uchar* pixels, int countPixels) {
            const auto bytes = static_cast<size_t>(countPixels) * 4;
            if (alpha == 0)
                std::memset(pixels, 0, bytes);
            else if (red == green && green == blue && blue == alpha)
                std::memset(pixels, red, bytes);
            else
                for (int x = 0; x < countPixels; ++x) {
                    pixels[x * 4] = red;
                    pixels[x * 4 + 1] = green;
                    pixels[x * 4 + 2] = blue;
                    pixels[x * 4 + 3] = alpha;
                }
        };
        for (int row = 0; row < count; ++row) {
            if (cancellationRequested())
                return false;
            uchar* pixels = destination + qsizetype(row) * stride;
            int filled = region.left();
            for (const auto index : active) {
                const auto& item = items[index];
                if ((alpha != 0 && !item.opaque) || first + row < item.rect.top() ||
                    first + row >= item.rect.top() + item.rect.height() ||
                    item.rect.left() >= region.left() + region.width() ||
                    item.rect.left() + item.rect.width() <= region.left())
                    continue;
                const int left = std::max(region.left(), item.rect.left());
                fill(pixels + qsizetype(filled - region.left()) * 4, left - filled);
                filled =
                    std::min(region.left() + region.width(), item.rect.left() + item.rect.width());
            }
            fill(pixels + qsizetype(filled - region.left()) * 4,
                 region.left() + region.width() - filled);
        }

        for (const auto index : active) {
            const auto& item = items[index];
            const auto intersection = item.rect.intersected(region);
            if (intersection.isEmpty())
                continue;
            const int begin = intersection.top();
            const int end = intersection.top() + intersection.height();
            const qsizetype itemStride = qsizetype(item.rect.width()) * 4;
            if ((alpha == 0 || item.opaque) &&
                (intersection.width() == item.rect.width() || item.rows.readRegion)) {
                const qsizetype offset = qsizetype(begin - first) * stride +
                                         qsizetype(intersection.left() - region.left()) * 4;
                const QRect source(intersection.topLeft() - item.rect.topLeft(),
                                   intersection.size());
                const bool succeeded =
                    item.rows.readRegion
                        ? item.rows.readRegion(source, stride, destination + offset,
                                               capacity - offset)
                        : item.rows.readRows(source.top(), source.height(), stride,
                                             destination + offset, capacity - offset);
                if (!succeeded)
                    return false;
                continue;
            }
            const bool croppedRead = static_cast<bool>(item.rows.readRegion);
            const qsizetype scratchStride =
                croppedRead ? qsizetype(intersection.width()) * 4 : itemStride;
            const int batch = static_cast<int>(
                std::max<qsizetype>(1, std::min<qsizetype>(64, rowScratchBytes / scratchStride)));
            for (int start = begin; start < end; start += batch) {
                if (cancellationRequested())
                    return false;
                const int rows = std::min(batch, end - start);
                const qsizetype bytes = scratchStride * rows;
                try {
                    scratch.resize(static_cast<size_t>(bytes));
                } catch (const std::bad_alloc&) {
                    return false;
                }
                const bool succeeded =
                    croppedRead ? item.rows.readRegion(QRect(intersection.left() - item.rect.left(),
                                                             start - item.rect.top(),
                                                             intersection.width(), rows),
                                                       scratchStride, scratch.data(), bytes)
                                : item.rows.readRows(start - item.rect.top(), rows, scratchStride,
                                                     scratch.data(), bytes);
                if (!succeeded)
                    return false;
                for (int row = 0; row < rows; ++row) {
                    const uchar* source =
                        scratch.data() + qsizetype(row) * scratchStride +
                        (croppedRead ? 0 : qsizetype(intersection.left() - item.rect.left()) * 4);
                    uchar* target = destination + qsizetype(start - first + row) * stride +
                                    qsizetype(intersection.left() - region.left()) * 4;
                    if (alpha == 0 || item.opaque) {
                        std::memcpy(target, source, size_t(intersection.width()) * 4);
                        continue;
                    }
                    for (int x = 0; x < intersection.width(); ++x) {
                        const unsigned sourceAlpha = source[3];
                        if (sourceAlpha == 255) {
                            std::memcpy(target, source, 4);
                        } else if (sourceAlpha != 0) {
                            // Keep all products until the final division, including background
                            // alpha, so translucent colors remain straight-alpha RGBA.
                            const unsigned backgroundWeight = unsigned(alpha) * (255 - sourceAlpha);
                            const unsigned coverage = sourceAlpha * 255 + backgroundWeight;
                            const unsigned values[3] = {red, green, blue};
                            for (int channel = 0; channel < 3; ++channel) {
                                const unsigned numerator =
                                    unsigned(source[channel]) * sourceAlpha * 255 +
                                    values[channel] * backgroundWeight;
                                target[channel] =
                                    static_cast<uchar>((numerator + coverage / 2) / coverage);
                            }
                            target[3] = static_cast<uchar>((coverage + 127) / 255);
                        }
                        source += 4;
                        target += 4;
                    }
                }
            }
        }
        return !cancellationRequested();
    }
};
} // namespace

bool operator==(const CollagePdfSettings& first, const CollagePdfSettings& second) {
    const auto equalNumber = [](qreal a, qreal b) {
        return a == b || (std::isnan(a) && std::isnan(b));
    };
    return first.paperSize == second.paperSize && first.orientation == second.orientation &&
           first.exportMode == second.exportMode &&
           equalNumber(first.customWidthMm, second.customWidthMm) &&
           equalNumber(first.customHeightMm, second.customHeightMm) &&
           equalNumber(first.marginTopMm, second.marginTopMm) &&
           equalNumber(first.marginBottomMm, second.marginBottomMm) &&
           equalNumber(first.marginLeftMm, second.marginLeftMm) &&
           equalNumber(first.marginRightMm, second.marginRightMm);
}

namespace {
constexpr qreal maximumPdfDimensionMm = 381'000'000;

QSizeF pdfPaperMillimeters(const CollagePdfSettings& settings) {
    QSizeF page;
    switch (settings.paperSize) {
    case CollagePdfPaperSize::A3:
        page = {297, 420};
        break;
    case CollagePdfPaperSize::A4:
        page = {210, 297};
        break;
    case CollagePdfPaperSize::A5:
        page = {148, 210};
        break;
    case CollagePdfPaperSize::B5:
        page = {176, 250};
        break;
    case CollagePdfPaperSize::Letter:
        page = {215.9, 279.4};
        break;
    case CollagePdfPaperSize::Legal:
        page = {215.9, 355.6};
        break;
    case CollagePdfPaperSize::Custom:
        page = {settings.customWidthMm, settings.customHeightMm};
        break;
    default:
        return {};
    }
    if (settings.orientation == CollagePdfOrientation::Landscape)
        page.transpose();
    return page;
}
} // namespace

QMap<QString, QString> validateCollagePdfSettings(const CollagePdfSettings& settings) {
    QMap<QString, QString> errors;
    const auto positiveDimension = [&](const QString& key, qreal value) {
        if (!std::isfinite(value) || value <= 0)
            errors.insert(key, QCoreApplication::translate("CollageDocument",
                                                           "Enter a positive paper dimension."));
        // PDF 1.7 permits at most 14400 default user-space units and a UserUnit of 75000.
        // Reject unrepresentable physical paper before converting millimeters to points.
        else if (value > maximumPdfDimensionMm)
            errors.insert(
                key, QCoreApplication::translate(
                         "CollageDocument", "The paper dimension exceeds the PDF size limit."));
    };
    const auto nonnegativeMargin = [&](const QString& key, qreal value) {
        if (!std::isfinite(value) || value < 0)
            errors.insert(key, QCoreApplication::translate("CollageDocument",
                                                           "Enter a nonnegative page margin."));
    };
    if (settings.paperSize < CollagePdfPaperSize::A3 ||
        settings.paperSize > CollagePdfPaperSize::ImageSize)
        errors.insert(QStringLiteral("paper_size"),
                      QCoreApplication::translate("CollageDocument", "Select a paper size."));
    if (settings.paperSize != CollagePdfPaperSize::ImageSize &&
        settings.orientation != CollagePdfOrientation::Portrait &&
        settings.orientation != CollagePdfOrientation::Landscape)
        errors.insert(
            QStringLiteral("orientation"),
            QCoreApplication::translate("CollageDocument", "Select a paper orientation."));
    if (settings.exportMode != CollagePdfExportMode::Combined &&
        settings.exportMode != CollagePdfExportMode::PerPage)
        errors.insert(QStringLiteral("export_mode"),
                      QCoreApplication::translate("CollageDocument", "Select an export mode."));
    if (settings.paperSize == CollagePdfPaperSize::ImageSize)
        return errors;
    if (settings.paperSize == CollagePdfPaperSize::Custom) {
        positiveDimension(QStringLiteral("custom_width_mm"), settings.customWidthMm);
        positiveDimension(QStringLiteral("custom_height_mm"), settings.customHeightMm);
    }
    nonnegativeMargin(QStringLiteral("margin_top_mm"), settings.marginTopMm);
    nonnegativeMargin(QStringLiteral("margin_bottom_mm"), settings.marginBottomMm);
    nonnegativeMargin(QStringLiteral("margin_left_mm"), settings.marginLeftMm);
    nonnegativeMargin(QStringLiteral("margin_right_mm"), settings.marginRightMm);
    const QSizeF page = pdfPaperMillimeters(settings);
    const auto usableDimension = [&](qreal dimension, qreal before, qreal after,
                                     const QString& beforeKey, const QString& afterKey) {
        if (!std::isfinite(dimension) || dimension <= 0 || errors.contains(beforeKey) ||
            errors.contains(afterKey))
            return;
        const qreal remaining = dimension - before - after;
        if (std::isfinite(remaining) && remaining > 0)
            return;
        const QString message = QCoreApplication::translate(
            "CollageDocument", "The margins must leave a usable content area.");
        errors.insert(beforeKey, message);
        errors.insert(afterKey, message);
    };
    usableDimension(page.width(), settings.marginLeftMm, settings.marginRightMm,
                    QStringLiteral("margin_left_mm"), QStringLiteral("margin_right_mm"));
    usableDimension(page.height(), settings.marginTopMm, settings.marginBottomMm,
                    QStringLiteral("margin_top_mm"), QStringLiteral("margin_bottom_mm"));
    return errors;
}

CollagePdfPageLayout calculateCollagePdfPageLayout(QSize transformedPixels,
                                                   const CollagePdfSettings& settings) {
    CollagePdfPageLayout layout;
    layout.fieldErrors = validateCollagePdfSettings(settings);
    if (!layout.fieldErrors.isEmpty()) {
        layout.error =
            QCoreApplication::translate("CollageDocument", "The PDF page settings are invalid.");
        return layout;
    }
    if (transformedPixels.isEmpty()) {
        layout.error = preparationError();
        return layout;
    }
    if (settings.paperSize == CollagePdfPaperSize::ImageSize) {
        layout.pageMillimeters = QSizeF(transformedPixels) * (25.4 / 96.0);
        if (layout.pageMillimeters.width() > maximumPdfDimensionMm ||
            layout.pageMillimeters.height() > maximumPdfDimensionMm) {
            layout.error = QCoreApplication::translate(
                "CollageDocument", "The paper dimension exceeds the PDF size limit.");
            return layout;
        }
        layout.contentMillimeters = QRectF(QPointF(0, 0), layout.pageMillimeters);
        layout.imageMillimeters = layout.contentMillimeters;
        return layout;
    }
    layout.pageMillimeters = pdfPaperMillimeters(settings);
    layout.contentMillimeters = {
        settings.marginLeftMm, settings.marginTopMm,
        layout.pageMillimeters.width() - settings.marginLeftMm - settings.marginRightMm,
        layout.pageMillimeters.height() - settings.marginTopMm - settings.marginBottomMm};
    const qreal scale = std::min(layout.contentMillimeters.width() / transformedPixels.width(),
                                 layout.contentMillimeters.height() / transformedPixels.height());
    const QSizeF fitted(transformedPixels.width() * scale, transformedPixels.height() * scale);
    layout.imageMillimeters = {layout.contentMillimeters.left() +
                                   (layout.contentMillimeters.width() - fitted.width()) / 2,
                               layout.contentMillimeters.top() +
                                   (layout.contentMillimeters.height() - fitted.height()) / 2,
                               fitted.width(), fitted.height()};
    if (layout.imageMillimeters.isEmpty() || !std::isfinite(fitted.width()) ||
        !std::isfinite(fitted.height()))
        layout.error =
            QCoreApplication::translate("CollageDocument", "The PDF page settings are invalid.");
    return layout;
}

QSize transformedSize(const CollageItem& item) {
    if (item.image.isNull())
        return {};
    const QRectF bounds = item.transform.mapRect(QRectF(QPointF(0, 0), item.image.size()));
    if (!std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) || bounds.width() <= 0 ||
        bounds.height() <= 0 || bounds.width() > std::numeric_limits<int>::max() ||
        bounds.height() > std::numeric_limits<int>::max())
        return {};
    return {qRound(bounds.width()), qRound(bounds.height())};
}

QImage transformedPreview(const CollageItem& item) {
    return snowCanvasTransformImage(boundedPreview(item.image, item.preview), item.transform,
                                    Qt::FastTransformation);
}

CollageLayout calculateCollageLayout(const QVector<CollageItem>& items,
                                     const CollageSettings& settings) {
    CollageLayout result;
    if (items.isEmpty())
        return result;
    if (!settingsValid(settings)) {
        result.error = QCoreApplication::translate("CollageDocument",
                                                   "The collage layout settings are invalid.");
        return result;
    }
    const bool vertical = settings.direction == CollageDirection::Vertical;
    const QSize reference = transformedSize(items.front());
    if (reference.isEmpty()) {
        result.error = preparationError();
        return result;
    }
    if (settings.direction == CollageDirection::Custom)
        return calculateCustomGridLayout(items, settings, reference);
    QVector<QSize> sizes;
    sizes.reserve(items.size());
    qint64 axis = 0;
    int cross = 0;
    for (const auto& item : items) {
        QSize size = transformedSize(item);
        if (size.isEmpty()) {
            result.error = preparationError();
            return result;
        }
        if (settings.sizing == CollageSizing::Uniform) {
            const qint64 numerator = vertical ? qint64(size.height()) * reference.width()
                                              : qint64(size.width()) * reference.height();
            const int divisor = vertical ? size.width() : size.height();
            const qint64 scaled = std::max<qint64>(1, (numerator + divisor / 2) / divisor);
            if (scaled > std::numeric_limits<int>::max()) {
                result.error = resourceError();
                return result;
            }
            size = vertical ? QSize(reference.width(), static_cast<int>(scaled))
                            : QSize(static_cast<int>(scaled), reference.height());
        }
        sizes.append(size);
        axis += vertical ? size.height() : size.width();
        if (sizes.size() > 1)
            axis += settings.spacing;
        cross = std::max(cross, vertical ? size.width() : size.height());
        if (axis > std::numeric_limits<int>::max()) {
            result.error = resourceError();
            return result;
        }
    }
    axis += qint64(settings.border) * 2;
    const qint64 fullCross = qint64(cross) + qint64(settings.border) * 2;
    if (axis > std::numeric_limits<int>::max() || fullCross > std::numeric_limits<int>::max()) {
        result.error = resourceError();
        return result;
    }
    result.size = vertical ? QSize(static_cast<int>(fullCross), static_cast<int>(axis))
                           : QSize(static_cast<int>(axis), static_cast<int>(fullCross));
    qint64 bytes = 0;
    if (!outputBytes(result.size, &bytes)) {
        result.error = resourceError();
        result.size = {};
        return result;
    }
    qint64 offset = settings.border;
    result.rects.reserve(sizes.size());
    for (const QSize& size : sizes) {
        const int centered =
            settings.border + (cross - (vertical ? size.width() : size.height())) / 2;
        result.rects.append(vertical ? QRect(QPoint(centered, static_cast<int>(offset)), size)
                                     : QRect(QPoint(static_cast<int>(offset), centered), size));
        offset += (vertical ? size.height() : size.width()) + settings.spacing;
    }
    return result;
}

struct CollageDocument::Impl final {
    QVector<CollageItem> items;
    CollageSettings settings;
    CollageOutputType outputType = CollageOutputType::StaticImage;
    CollagePdfSettings pdfSettings;
    CollagePdfSettings lastValidPdfSettings;
    quint64 contentRevision = 0;
    quint64 selected = 0;
    quint64 nextId = 1;
    QString error;
    qint64 sourceBytes = 0;
    qint64 previewBytes = 0;
    mutable bool layoutDirty = true;
    mutable CollageLayout cachedLayout;

    bool admit(qint64 sources, qint64 previews) {
        if (sources < 0 || sources > collage_detail::maximumRasterBytes || previews < 0 ||
            previews > collage_detail::maximumWindowPreviewBytes) {
            error = resourceError();
            return false;
        }
        sourceBytes = sources;
        previewBytes = previews;
        error.clear();
        return true;
    }
};

CollageDocument::CollageDocument(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {}
CollageDocument::~CollageDocument() = default;

const QVector<CollageItem>& CollageDocument::items() const {
    return m_impl->items;
}

quint64 CollageDocument::contentRevision() const {
    return m_impl->contentRevision;
}

CollageSettings CollageDocument::settings() const {
    return m_impl->settings;
}

void CollageDocument::setSettings(const CollageSettings& settings) {
    if (!settingsValid(settings))
        return;
    auto normalized = settings;
    const QStringList adjustedFields = normalizeCustomGrid(normalized, m_impl->items.size());
    if (normalized == m_impl->settings)
        return;
    m_impl->settings = normalized;
    m_impl->layoutDirty = true;
    emit changed();
    if (!adjustedFields.isEmpty())
        emit customGridAdjusted(adjustedFields);
}

CollageOutputType CollageDocument::outputType() const {
    return m_impl->outputType;
}

void CollageDocument::setOutputType(CollageOutputType type) {
    if ((type != CollageOutputType::StaticImage && type != CollageOutputType::PdfDocument) ||
        type == m_impl->outputType)
        return;
    m_impl->outputType = type;
    emit changed();
}

CollagePdfSettings CollageDocument::pdfSettings() const {
    return m_impl->pdfSettings;
}

void CollageDocument::setPdfSettings(const CollagePdfSettings& settings) {
    if (settings == m_impl->pdfSettings)
        return;
    m_impl->pdfSettings = settings;
    if (validateCollagePdfSettings(settings).isEmpty())
        m_impl->lastValidPdfSettings = settings;
    emit changed();
}

CollagePdfSettings CollageDocument::lastValidPdfSettings() const {
    return m_impl->lastValidPdfSettings;
}

CollageLayout CollageDocument::layout() const {
    if (m_impl->layoutDirty) {
        m_impl->cachedLayout = calculateCollageLayout(m_impl->items, m_impl->settings);
        m_impl->layoutDirty = false;
    }
    return m_impl->cachedLayout;
}

QString CollageDocument::lastError() const {
    return m_impl->error;
}

quint64 CollageDocument::append(QImage image, QString label, QImage preview) {
    if (image.isNull()) {
        m_impl->error = preparationError();
        return 0;
    }
    preview = boundedPreview(image, preview);
    if (preview.isNull()) {
        m_impl->error = preparationError();
        return 0;
    }
    try {
        if (m_impl->items.size() == m_impl->items.capacity()) {
            const auto capacity = m_impl->items.capacity();
            const auto growth = capacity <= std::numeric_limits<qsizetype>::max() / 2
                                    ? capacity + std::max<qsizetype>(8, capacity / 2)
                                    : m_impl->items.size() + 1;
            m_impl->items.reserve(growth);
        }
    } catch (const std::bad_alloc&) {
        m_impl->error = preparationError();
        return 0;
    }
    auto owner = sourceLifetime(image, preview, &m_impl->error);
    if (!owner)
        return 0;
    if (!m_impl->admit(m_impl->sourceBytes + image.sizeInBytes(),
                       m_impl->previewBytes + preview.sizeInBytes()))
        return 0;
    const quint64 id = m_impl->nextId++;
    auto metadata = owner->metadata;
    m_impl->items.append({id,
                          1,
                          std::move(image),
                          std::move(preview),
                          {},
                          std::move(label),
                          std::move(owner),
                          std::move(metadata)});
    ++m_impl->contentRevision;
    const QStringList adjustedFields = normalizeCustomGrid(m_impl->settings, m_impl->items.size());
    const bool selectFirst = m_impl->selected == 0;
    if (selectFirst)
        m_impl->selected = id;
    m_impl->layoutDirty = true;
    emit changed();
    if (!adjustedFields.isEmpty())
        emit customGridAdjusted(adjustedFields);
    if (selectFirst)
        emit selectionChanged();
    return id;
}

bool CollageDocument::replace(quint64 id, QImage image, QString label, QImage preview) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0 || image.isNull()) {
        m_impl->error = preparationError();
        return false;
    }
    preview = boundedPreview(image, preview);
    if (preview.isNull()) {
        m_impl->error = preparationError();
        return false;
    }
    auto owner = sourceLifetime(image, preview, &m_impl->error);
    if (!owner)
        return false;
    const auto& old = m_impl->items[index];
    if (!m_impl->admit(m_impl->sourceBytes - old.image.sizeInBytes() + image.sizeInBytes(),
                       m_impl->previewBytes - old.preview.sizeInBytes() + preview.sizeInBytes()))
        return false;
    auto& item = m_impl->items[index];
    item.image = std::move(image);
    item.preview = std::move(preview);
    item.label = std::move(label);
    item.metadata = owner->metadata;
    item.resourceOwner = std::move(owner);
    item.transform = {};
    ++item.revision;
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::remove(quint64 id) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    const auto& item = m_impl->items[index];
    static_cast<void>(m_impl->admit(m_impl->sourceBytes - item.image.sizeInBytes(),
                                    m_impl->previewBytes - item.preview.sizeInBytes()));
    m_impl->items.removeAt(index);
    ++m_impl->contentRevision;
    const bool selectionRemoved = m_impl->selected == id;
    if (selectionRemoved)
        m_impl->selected = m_impl->items.isEmpty()
                               ? 0
                               : m_impl->items[std::min(index, int(m_impl->items.size()) - 1)].id;
    m_impl->layoutDirty = true;
    emit changed();
    if (selectionRemoved)
        emit selectionChanged();
    return true;
}

void CollageDocument::clear() {
    if (m_impl->items.isEmpty())
        return;
    m_impl->items.clear();
    ++m_impl->contentRevision;
    m_impl->selected = 0;
    static_cast<void>(m_impl->admit(0, 0));
    m_impl->layoutDirty = true;
    emit changed();
    emit selectionChanged();
}

bool CollageDocument::reverseItems() {
    if (m_impl->items.size() < 2)
        return false;
    std::reverse(m_impl->items.begin(), m_impl->items.end());
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::swapItems(quint64 first, quint64 second) {
    const int a = itemIndex(m_impl->items, first);
    const int b = itemIndex(m_impl->items, second);
    if (a < 0 || b < 0 || a == b)
        return false;
    std::swap(m_impl->items[a], m_impl->items[b]);
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::moveItem(quint64 id, int finalIndex) {
    const int sourceIndex = itemIndex(m_impl->items, id);
    if (sourceIndex < 0 || finalIndex < 0 || finalIndex >= m_impl->items.size() ||
        sourceIndex == finalIndex)
        return false;
    m_impl->items.move(sourceIndex, finalIndex);
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::rotate(quint64 id, bool clockwise) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    const QTransform operation =
        clockwise ? QTransform(0, 1, -1, 0, 0, 0) : QTransform(0, -1, 1, 0, 0, 0);
    item.transform = canonicalTransform(item.transform * operation);
    ++item.revision;
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::flip(quint64 id, bool horizontal) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    item.transform = canonicalTransform(
        item.transform * QTransform(horizontal ? -1 : 1, 0, 0, horizontal ? 1 : -1, 0, 0));
    ++item.revision;
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

bool CollageDocument::resetTransform(quint64 id) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    if (item.transform.isIdentity())
        return true;
    item.transform = {};
    ++item.revision;
    ++m_impl->contentRevision;
    m_impl->layoutDirty = true;
    emit changed();
    return true;
}

quint64 CollageDocument::selectedId() const {
    return m_impl->selected;
}

void CollageDocument::select(quint64 id) {
    const quint64 selected = itemIndex(m_impl->items, id) >= 0 ? id : 0;
    if (selected == m_impl->selected)
        return;
    m_impl->selected = selected;
    emit selectionChanged();
}

ScreenshotImageRowSource prepareCollageRows(const QVector<CollageItem>& items,
                                            const CollageSettings& settings,
                                            std::function<bool()> cancelled, QString* error) {
    if (error)
        error->clear();
    const auto stopped = [&] { return cancelled && cancelled(); };
    if (stopped()) {
        report(error, cancellationError());
        return {};
    }
    const CollageLayout layout = calculateCollageLayout(items, settings);
    if (!layout.isValid()) {
        report(error, layout.error.isEmpty() ? preparationError() : layout.error);
        return {};
    }
    qint64 preparedBytes = 0;
    qint64 readerCacheBytes = 0;
    qint64 transformScratchBytes = 0;
    qint64 unownedSourceBytes = 0;
    qint64 maximumRowBytes = std::max(qint64(rowScratchBytes), qint64(layout.size.width()) * 4);
    for (qsizetype index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const bool transformed = !item.transform.isIdentity();
        const bool resized = transformedSize(item) != layout.rects[index].size();
        const bool streamed = collage_detail::streamableTransform(item);
        qint64 itemScratchBytes = 0;
        if (transformed && !streamed)
            preparedBytes +=
                ((qint64(transformedSize(item).width()) * std::max(32, item.image.depth()) + 31) /
                 32) *
                4 * transformedSize(item).height();
        if (streamed) {
            readerCacheBytes += collage_detail::transformCacheBytes(item);
            itemScratchBytes = collage_detail::transformWorkingBytes(item);
        }
        if (!streamed && (item.image.format() != QImage::Format_RGBA8888 ||
                          (item.image.colorSpace().isValid() &&
                           item.image.colorSpace() != QColorSpace(QColorSpace::SRgb)))) {
            readerCacheBytes +=
                std::min(conversionCacheBytes, qint64(transformedSize(item).width()) *
                                                   transformedSize(item).height() * 4);
            itemScratchBytes += 8 * 1024 * 1024;
        }
        if (resized) {
            const auto resources =
                image_codec::resizeResources(transformedSize(item), layout.rects[index].size());
            if (!resources) {
                report(error, resourceError());
                return {};
            }
            readerCacheBytes += resources->retainedBytes;
            itemScratchBytes += resources->scratchBytes;
        }
        maximumRowBytes = std::max(maximumRowBytes, qint64(layout.rects[index].width()) * 4);
        if (transformed && !streamed)
            itemScratchBytes +=
                std::max(qint64(item.image.sizeInBytes()), qint64(transformedSize(item).width()) *
                                                               transformedSize(item).height() *
                                                               std::max(4, item.image.depth() / 8));
        transformScratchBytes = std::max(transformScratchBytes, itemScratchBytes);
        if (!item.resourceOwner)
            unownedSourceBytes += item.image.sizeInBytes() + item.preview.sizeInBytes();
    }
    const qint64 transientBytes = readerCacheBytes + transformScratchBytes + maximumRowBytes;
    if (transientBytes > collage_detail::maximumWorkingBytes) {
        report(error, QCoreApplication::translate(
                          "CollageDocument",
                          "This collage requires too much temporary memory. Try fewer images or "
                          "smaller image sizes."));
        return {};
    }
    if (preparedBytes > collage_detail::maximumRasterBytes) {
        report(error, resourceError());
        return {};
    }
    try {
        auto prepared = std::make_shared<PreparedRows>();
        prepared->reservation = collage_detail::reserve(preparedBytes + unownedSourceBytes, error);
        if (!prepared->reservation)
            return {};
        prepared->working = collage_detail::reserveWorking(transientBytes, error);
        if (!prepared->working)
            return {};
        prepared->layout = layout;
        prepared->background = settings.background;
        prepared->cancelled = std::move(cancelled);
        prepared->items.reserve(items.size());
        prepared->sourceOwners.reserve(items.size());
        prepared->inputImages.reserve(items.size() * 2);
        for (qsizetype index = 0; index < items.size(); ++index) {
            if (prepared->cancellationRequested()) {
                report(error, cancellationError());
                return {};
            }
            const auto& item = items[index];
            prepared->sourceOwners.append(item.resourceOwner);
            prepared->inputImages.append(item.image);
            const bool streamed = collage_detail::streamableTransform(item);
            QImage pixels = streamed ? item.image
                                     : snowCanvasTransformImage(item.image, item.transform,
                                                                Qt::FastTransformation);
            auto rows = streamed ? collage_detail::transformedRowSource(item, prepared->cancelled)
                                 : image_codec::srgbRowSource(pixels);
            if (!rows.isValid()) {
                report(error, preparationError());
                return {};
            }
            if (!streamed && !item.transform.isIdentity())
                prepared->inputImages.append(pixels);
            const QSize size = layout.rects[index].size();
            if (rows.size != size) {
                rows.cancellationRequested = prepared->cancelled;
                rows = image_codec::resizedRowSource(std::move(rows), size, error);
                if (!rows.isValid()) {
                    if (!error || error->isEmpty())
                        report(error, preparationError());
                    return {};
                }
            }
            const bool opaque = settings.background.alpha() != 0 &&
                                cachedImageIsOpaque(item, pixels, prepared->cancelled);
            if (prepared->cancellationRequested()) {
                report(error, cancellationError());
                return {};
            }
            prepared->items.append({std::move(rows), layout.rects[index], opaque});
        }
        if (!prepared->indexRows()) {
            report(error, preparationError());
            return {};
        }
        ScreenshotImageRowSource rows;
        rows.size = layout.size;
        rows.isKnownOpaque = settings.background.alpha() == 255;
        rows.cancellationRequested = prepared->cancelled;
        rows.readRows = [prepared](int first, int count, qsizetype stride, uchar* destination,
                                   qsizetype capacity) {
            return prepared->read(first, count, stride, destination, capacity);
        };
        rows.readRegion = [prepared](QRect region, qsizetype stride, uchar* destination,
                                     qsizetype capacity) {
            return prepared->readRegion(region, stride, destination, capacity);
        };
        rows.reserveResources = [](qint64 retained, qint64 scratch,
                                   QString* admissionError) -> std::shared_ptr<void> {
            if (retained < 0 || scratch < 0 || retained > collage_detail::maximumWorkingBytes ||
                scratch > collage_detail::maximumWorkingBytes - retained) {
                report(admissionError, resourceError());
                return {};
            }
            return collage_detail::reserveWorking(retained + scratch, admissionError);
        };
        if ((layout.size.width() > 2048 || layout.size.height() > 2048) &&
            std::all_of(items.begin(), items.end(),
                        [](const CollageItem& item) { return !item.preview.isNull(); })) {
            QSize size = layout.size.scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo({1, 1});
            prepared->previewOwner =
                collage_detail::reservePreview(qint64(size.width()) * size.height() * 4);
            if (prepared->previewOwner) {
                auto preview = snowCanvasAllocateImage(size, QImage::Format_ARGB32_Premultiplied);
                if (!preview.isNull()) {
                    preview.fill(settings.background);
                    QPainter painter(&preview);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.scale(qreal(size.width()) / layout.size.width(),
                                  qreal(size.height()) / layout.size.height());
                    for (qsizetype index = 0; index < items.size(); ++index) {
                        if (prepared->cancellationRequested()) {
                            report(error, cancellationError());
                            return {};
                        }
                        painter.drawImage(layout.rects[index], transformedPreview(items[index]));
                    }
                    painter.end();
                    preview.setColorSpace(QColorSpace(QColorSpace::SRgb));
                    rows.previewImage = std::move(preview);
                } else {
                    prepared->previewOwner.reset();
                }
            }
        }
        return rows;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}

QImage materializeCollageRows(const ScreenshotImageRowSource& rows, qsizetype maximumBytes) {
    qint64 bytes = 0;
    if (!rows.isValid() || !outputBytes(rows.size, &bytes) || bytes > maximumBytes ||
        (rows.cancellationRequested && rows.cancellationRequested()))
        return {};
    QImage image = snowCanvasAllocateImage(rows.size, QImage::Format_RGBA8888);
    if (image.isNull() ||
        !rows.readRows(0, image.height(), image.bytesPerLine(), image.bits(), image.sizeInBytes()))
        return {};
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

CollageClipboardEstimate estimateCollageClipboard(QSize outputSize) {
    CollageClipboardEstimate result;
    qint64 rawBytes = 0;
    if (!outputBytes(outputSize, &rawBytes)) {
        result.error = resourceError();
        return result;
    }
    constexpr qint64 encodingOverhead = 64 * 1024 * 1024;
#if defined(Q_OS_WIN)
    const qint64 nativeStride = (qint64(outputSize.width()) * 3 + 3) & ~qint64{3};
    result.dibBytes = nativeStride * outputSize.height();
    result.rowScratchBytes = qint64(outputSize.width()) * 4 * std::min(64, outputSize.height());
    result.bytes = rawBytes * 2 + result.dibBytes + result.rowScratchBytes + encodingOverhead;
#else
    result.bytes = rawBytes + encodingOverhead;
#endif
    if (result.bytes > qint64{16} * 1024 * 1024 * 1024 ||
        result.dibBytes > std::numeric_limits<quint32>::max())
        result.error = QCoreApplication::translate(
            "CollageDocument",
            "The collage is too large for the clipboard. Save it to a file instead.");
    return result;
}

std::shared_ptr<void> reserveCollageClipboard(QSize outputSize, QString* error) {
    const auto estimate = estimateCollageClipboard(outputSize);
    if (!estimate.isValid()) {
        report(error, estimate.error);
        return {};
    }
    return collage_detail::reserve(estimate.bytes, error);
}

CollageResourceUsage collageResourceUsage() {
    return {collage_detail::reservedBytes(), collage_detail::budgetLimit()};
}

qint64 collageImageImportBytes(QSize size, QString* error) {
    if (error)
        error->clear();
    if (size.isEmpty()) {
        report(error, preparationError());
        return 0;
    }
    qint64 bytes = 0;
    if (!outputBytes(size, &bytes)) {
        report(error, QCoreApplication::translate(
                          "CollageDocument",
                          "This collage requires too much temporary memory. Try fewer images "
                          "or smaller image sizes."));
        return 0;
    }
    return bytes * 2;
}

QImage captureCollageImage(QSize size, const std::function<QImage()>& capture, QString* error) {
    const qint64 bytes = collageImageImportBytes(size, error) / 2;
    if (bytes == 0)
        return {};
    if (!capture) {
        report(error, preparationError());
        return {};
    }
    try {
        auto working = collage_detail::reserveWorking(bytes, error);
        if (!working)
            return {};
        auto lifetime = std::make_unique<CapturedImageLifetime>();
        lifetime->reservation = collage_detail::reserve(bytes, error);
        if (!lifetime->reservation)
            return {};
        lifetime->image = capture();
        const QImage image = lifetime->image;
        if (image.isNull() || image.size() != size || image.sizeInBytes() > bytes) {
            report(error, preparationError());
            return {};
        }
        static_cast<void>(lifetime->reservation->resize(image.sizeInBytes()));
        // A const view keeps admission attached to QImage's implicitly shared pixel lifetime.
        // Metadata changes below do not duplicate the raster; writers detach as usual.
        QImage result(
            image.constBits(), image.width(), image.height(), image.bytesPerLine(), image.format(),
            [](void* context) { delete static_cast<CapturedImageLifetime*>(context); },
            lifetime.get());
        if (result.isNull()) {
            report(error, preparationError());
            return {};
        }
        [[maybe_unused]] const auto* imageOwner = lifetime.release();
        result.setColorSpace(image.colorSpace());
        result.setDevicePixelRatio(image.devicePixelRatio());
        result.setDotsPerMeterX(image.dotsPerMeterX());
        result.setDotsPerMeterY(image.dotsPerMeterY());
        result.setOffset(image.offset());
        result.setColorTable(image.colorTable());
        for (const auto& key : image.textKeys())
            result.setText(key, image.text(key));
        return result;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}
} // namespace snow_shot::presentation
