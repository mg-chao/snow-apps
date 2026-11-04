// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "imagesplitimagepreparation_p.h"

#include <QColorSpace>
#include <QColorTransform>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace snow_shot::presentation {
namespace {
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageSplitExport", "Export was canceled"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The image or split configuration is invalid"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The image could not be prepared for export"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The output folder could not be created"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "Image %1 could not be saved: %2"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "There is not enough memory to export the image"),
};
void setError(ImageSplitExportResult& result, const char* source, QStringList arguments = {}) {
    result.errorSource = QString::fromUtf8(source);
    result.errorArguments = std::move(arguments);
    result.error = result.errorMessage();
}

QString createExportDirectory(const QString& parentDirectory, const QString& imageName) {
    const QFileInfo parentInfo(parentDirectory);
    if (!parentInfo.isDir())
        return {};
    QDir parent(parentInfo.absoluteFilePath());
    const QString base = QStringLiteral("Split Images_%1")
                             .arg(storage::sanitizeExportFileName(imageName, u"Screenshot"));
    for (qint64 suffix = 1; suffix <= std::numeric_limits<int>::max(); ++suffix) {
        const QString name = suffix == 1 ? base : QStringLiteral("%1 (%2)").arg(base).arg(suffix);
        if (parent.mkdir(name))
            return parent.absoluteFilePath(name);
        if (!QFileInfo::exists(parent.filePath(name)))
            return {};
    }
    return {};
}

class CropRowReader final {
  public:
    CropRowReader(QImage image, QRect region, std::function<bool()> cancelled,
                  QColorTransform transform)
        : m_image(std::move(image)), m_region(region), m_cancelled(std::move(cancelled)),
          m_transform(std::move(transform)),
          m_convertColor(m_image.colorSpace().isValid() &&
                         m_image.colorSpace() != QColorSpace(QColorSpace::SRgb)),
          m_cachedRgbTransform(m_image.colorSpace().colorModel() == QColorSpace::ColorModel::Rgb) {}

    bool operator()(int firstRow, int rowCount, qsizetype stride, uchar* destination,
                    qsizetype destinationSize) {
        if (cancelled() || firstRow < 0 || rowCount < 0 || firstRow > m_region.height() ||
            rowCount > m_region.height() - firstRow)
            return false;
        const qsizetype rowBytes = qsizetype(m_region.width()) * 4;
        if (rowCount == 0)
            return true;
        if (!destination || stride < rowBytes || destinationSize < rowBytes ||
            qsizetype(rowCount - 1) > (destinationSize - rowBytes) / stride)
            return false;

        if (!m_convertColor && m_image.format() == QImage::Format_RGBA8888) {
            for (int row = 0; row < rowCount; ++row) {
                if (cancelled())
                    return false;
                const auto* source = m_image.constScanLine(m_region.y() + firstRow + row) +
                                     qsizetype(m_region.x()) * 4;
                std::memcpy(destination + qsizetype(row) * stride, source, size_t(rowBytes));
            }
            return true;
        }

        // Look ahead only within this crop. Converting complete source rows for each column
        // would repeat color/format conversion across the whole image once per column.
        constexpr qsizetype maximumCacheBytes = 2 * 1024 * 1024;
        const int cacheRows = static_cast<int>(std::min(
            qsizetype(m_region.height()), std::max(qsizetype{1}, maximumCacheBytes / rowBytes)));
        if (rowCount >= cacheRows)
            return convertRows(firstRow, rowCount, stride, destination);
        for (int row = 0; row < rowCount; ++row) {
            if (cancelled())
                return false;
            const int sourceRow = firstRow + row;
            if (sourceRow < m_cachedFirst || sourceRow >= m_cachedFirst + m_cachedRows) {
                if (m_cache.isNull())
                    m_cache = QImage(m_region.width(), cacheRows, QImage::Format_RGBA8888);
                if (m_cache.isNull())
                    return false;
                m_cachedRows = 0;
                const int count = std::min(cacheRows, m_region.height() - sourceRow);
                if (!convertRows(sourceRow, count, m_cache.bytesPerLine(), m_cache.bits()))
                    return false;
                m_cachedFirst = sourceRow;
                m_cachedRows = count;
            }
            const auto* source = m_cache.constScanLine(sourceRow - m_cachedFirst);
            std::memcpy(destination + qsizetype(row) * stride, source, size_t(rowBytes));
        }
        return true;
    }

  private:
    bool cancelled() const {
        return m_cancelled && m_cancelled();
    }

    bool convertRows(int firstRow, int rowCount, qsizetype stride, uchar* destination) const {
        // Let ordinary 32-bit surfaces use the bounded cache as one color-conversion batch.
        // Batches above 64K pixels enable Qt's parallel color transform; tiny 16K-pixel tiles
        // repeatedly construct serial work. Higher native precision reduces the tile dimensions.
        constexpr int maximumColumns = 256 * 1024 / 16;
        const int depth = m_image.depth();
        // A color-model conversion can promote a gray/CMYK source to higher-precision RGB.
        const int workingBytesPerPixel =
            m_convertColor && !m_cachedRgbTransform ? 16 : std::max(4, depth / 8);
        for (int x = 0; x < m_region.width();) {
            const int width = std::min(maximumColumns, m_region.width() - x);
            const int sourceX = m_region.x() + x;
            const int maximumRows = 2 * 1024 * 1024 / (width * workingBytesPerPixel);
            for (int y = 0; y < rowCount;) {
                if (cancelled())
                    return false;
                const int count = std::min(maximumRows, rowCount - y);
                // Copy from the original image, not an offset borrowed view. Qt may clone
                // aligned scanline padding during color conversion; that padding need not
                // exist after the final row of a borrowed crop. The native tile helper owns
                // valid padding and preserves native pixels, palettes and bit offsets.
                QImage converted = imageSplitCopyTile(
                    m_image, QRect(sourceX, m_region.y() + firstRow + y, width, count));
                // Preserve color-before-format conversion, including premultiplied alpha
                // rounding. Qt's combined conversion can differ for native input formats.
                // Non-RGB profiles retain Qt's complete color-model conversion/retagging path.
                // Move the owned tile through RGB conversion to avoid Qt's extra clone.
                if (!converted.isNull() && m_convertColor)
                    converted = m_cachedRgbTransform
                                    ? std::move(converted).colorTransformed(m_transform)
                                    : converted.convertedToColorSpace(QColorSpace::SRgb);
                // Keep the out-of-place format converter: Qt's in-place path can round
                // low-alpha channels differently from full-image export normalization.
                if (!converted.isNull() && converted.format() != QImage::Format_RGBA8888)
                    converted = converted.convertToFormat(QImage::Format_RGBA8888);
                if (converted.isNull())
                    return false;
                for (int row = 0; row < count; ++row)
                    std::memcpy(destination + qsizetype(y + row) * stride + qsizetype(x) * 4,
                                converted.constScanLine(row), size_t(width) * 4);
                y += count;
            }
            x += width;
        }
        return true;
    }

    QImage m_image;
    QRect m_region;
    std::function<bool()> m_cancelled;
    QColorTransform m_transform;
    bool m_convertColor;
    bool m_cachedRgbTransform;
    QImage m_cache;
    int m_cachedFirst = -1;
    int m_cachedRows = 0;
};

ScreenshotImageRowSource cropRows(QImage image, QRect region,
                                  std::function<bool()> cancellationRequested,
                                  const QColorTransform& transform) {
    ScreenshotImageRowSource rows;
    rows.size = region.size();
    rows.cancellationRequested = cancellationRequested;
    rows.isKnownOpaque = !image.hasAlphaChannel();
    auto reader = std::make_shared<CropRowReader>(std::move(image), region,
                                                  std::move(cancellationRequested), transform);
    rows.readRows = [reader = std::move(reader)](int first, int count, qsizetype stride,
                                                 uchar* destination, qsizetype capacity) {
        return (*reader)(first, count, stride, destination, capacity);
    };
    return rows;
}
} // namespace

bool ImageSplitExportResult::succeeded() const {
    return !directory.isEmpty() && error.isEmpty() && totalCount > 0 && savedCount == totalCount;
}

QString ImageSplitExportResult::errorMessage() const {
    if (errorSource.isEmpty())
        return error;
    const auto source = errorSource.toUtf8();
    QString message = QCoreApplication::translate("ImageSplitExport", source.constData());
    for (const auto& argument : errorArguments)
        message = message.arg(argument);
    return message;
}

ImageSplitExportResult exportSplitImages(const QImage& image, const ImageSplitLayout& layout,
                                         const QString& parentDirectory, const QString& imageName,
                                         std::function<bool()> cancelled,
                                         std::function<void(qint64, qint64)> progress) {
    ImageSplitExportResult result;
    result.totalCount = layout.outputCount();
    const auto cancellationRequested = [&cancelled] { return cancelled && cancelled(); };
    if (cancellationRequested()) {
        setError(result, "Export was canceled");
        return result;
    }
    if (image.isNull() || image.size() != layout.size || !layout.isValid()) {
        setError(result, "The image or split configuration is invalid");
        return result;
    }
    try {
        const QColorSpace srgb(QColorSpace::SRgb);
        const QColorTransform transform = image.colorSpace().isValid() && image.colorSpace() != srgb
                                              ? image.colorSpace().transformationToColorSpace(srgb)
                                              : QColorTransform{};
        if (cancellationRequested()) {
            setError(result, "Export was canceled");
            return result;
        }
        result.directory = createExportDirectory(parentDirectory, imageName);
        if (result.directory.isEmpty()) {
            setError(result, "The output folder could not be created");
            return result;
        }
        if (progress)
            progress(0, result.totalCount);
        const QDir output(result.directory);
        for (qint64 index = 0; index < result.totalCount; ++index) {
            if (cancellationRequested()) {
                setError(result, "Export was canceled");
                return result;
            }
            const auto rows = cropRows(image, layout.regionAt(index), cancelled, transform);
            const auto saved = ScreenshotImageFileService::write(
                rows, output.filePath(QStringLiteral("%1.png").arg(index + 1)),
                ScreenshotImageFileFormat::Png);
            if (!saved.succeeded()) {
                if (cancellationRequested())
                    setError(result, "Export was canceled");
                else
                    setError(result, "Image %1 could not be saved: %2",
                             {QString::number(index + 1), saved.error});
                return result;
            }
            ++result.savedCount;
            if (progress)
                progress(result.savedCount, result.totalCount);
        }
    } catch (const std::bad_alloc&) {
        setError(result, "There is not enough memory to export the image");
    }
    return result;
}

} // namespace snow_shot::presentation
