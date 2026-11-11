// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEDOCUMENT_H
#define SNOW_SHOT_PRESENTATION_COLLAGEDOCUMENT_H

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QColor>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace snow_shot::presentation {

enum class ImageStitchingDirection { Vertical, Horizontal, Custom };
enum class ImageStitchingSizing { Original, Uniform };
enum class ImageStitchingOutputType { StaticImage, PdfDocument };
enum class ImageStitchingPdfPaperSize { A3, A4, A5, B5, Letter, Legal, Custom, ImageSize };
enum class ImageStitchingPdfOrientation { Portrait, Landscape };
enum class ImageStitchingPdfExportMode { Combined, PerPage };
enum class ImageStitchingPdfStitchingMethod { PageBased, TemplateBased };
enum class ImageStitchingPdfTemplateType {
    TwoImages,
    IdCard,
    ThreeImages,
    FourImages,
    FiveImages,
    SixImages,
    SevenImages,
    EightImages,
    NineImages
};

struct ImageStitchingPdfSettings final {
    ImageStitchingPdfStitchingMethod stitchingMethod = ImageStitchingPdfStitchingMethod::PageBased;
    ImageStitchingPdfTemplateType templateType = ImageStitchingPdfTemplateType::TwoImages;
    qreal imageSpacingMm = 5;
    ImageStitchingPdfPaperSize paperSize = ImageStitchingPdfPaperSize::A4;
    ImageStitchingPdfOrientation orientation = ImageStitchingPdfOrientation::Portrait;
    qreal customWidthMm = 210;
    qreal customHeightMm = 297;
    qreal marginTopMm = 10;
    qreal marginBottomMm = 10;
    qreal marginLeftMm = 10;
    qreal marginRightMm = 10;
    ImageStitchingPdfExportMode exportMode = ImageStitchingPdfExportMode::Combined;
    int imageQuality = 100;

    // Treat two NaNs as the same incomplete field so UI refreshes do not emit repeatedly.
    friend bool operator==(const ImageStitchingPdfSettings&, const ImageStitchingPdfSettings&);
};

struct ImageStitchingPdfPageLayout final {
    QSizeF pageMillimeters;
    QRectF contentMillimeters;
    QRectF imageMillimeters;
    QString error;
    QMap<QString, QString> fieldErrors;

    [[nodiscard]] bool isValid() const {
        return pageMillimeters.isValid() && !pageMillimeters.isEmpty() &&
               !contentMillimeters.isEmpty() && !imageMillimeters.isEmpty() && error.isEmpty() &&
               fieldErrors.isEmpty();
    }
};

// Validation keys match ImageStitchingSettingsStore's PDF field names. Geometry is top-left based,
// in millimeters, and is shared by screen previews and PDF export. ImageSize uses transformed
// pixels at 96 DPI with no margins or paper-orientation adjustment.
[[nodiscard]] QMap<QString, QString>
validateImageStitchingPdfSettings(const ImageStitchingPdfSettings& settings);
[[nodiscard]] ImageStitchingPdfPageLayout
calculateImageStitchingPdfPageLayout(QSize transformedPixels,
                                     const ImageStitchingPdfSettings& settings);

struct ImageStitchingSettings final {
    ImageStitchingDirection direction = ImageStitchingDirection::Vertical;
    ImageStitchingSizing sizing = ImageStitchingSizing::Uniform;
    int border = 0;
    int spacing = 0;
    QColor background = Qt::white;
    int rows = 1;
    int columns = 1;
    bool customGridInitialized = false;

    friend bool operator==(const ImageStitchingSettings&, const ImageStitchingSettings&) = default;
};

struct ImageStitchingImageMetadata;

struct ImageStitchingPdfPlacement final {
    QPointF centerOffsetMillimeters;
    // An unset scale fits the complete image inside its cell, centered and aspect-preserving.
    // Resolve against full-resolution transformed pixels.
    std::optional<qreal> millimetersPerPixel;

    friend bool operator==(const ImageStitchingPdfPlacement&,
                           const ImageStitchingPdfPlacement&) = default;
};

struct ImageStitchingStaticPlacement final {
    QPointF centerOffsetPixels;
    // Unset uses the automatic layout's image bounds. Scale is relative to transformed source
    // pixels.
    std::optional<qreal> pixelsPerPixel;

    friend bool operator==(const ImageStitchingStaticPlacement&,
                           const ImageStitchingStaticPlacement&) = default;
};

struct ImageStitchingItem final {
    quint64 id = 0;
    quint64 revision = 0;
    QImage image;
    QImage preview;
    QTransform transform;
    QString label;
    std::shared_ptr<void> resourceOwner;
    // Shared memoization belongs to the immutable original, independently of layout/transforms.
    std::shared_ptr<ImageStitchingImageMetadata> metadata;
    ImageStitchingPdfPlacement pdfPlacement{};
    ImageStitchingStaticPlacement staticPlacement{};
};

struct ImageStitchingPdfCell final {
    quint64 itemId = 0;
    qsizetype itemIndex = -1;
    QRectF clipMillimeters;
    QRectF imageMillimeters;
};

struct ImageStitchingPdfComposedPage final {
    QSizeF pageMillimeters;
    QRectF contentMillimeters;
    QVector<ImageStitchingPdfCell> cells;
    QString error;

    [[nodiscard]] bool isValid() const {
        return pageMillimeters.isValid() && !pageMillimeters.isEmpty() &&
               !contentMillimeters.isEmpty() && !cells.isEmpty() && error.isEmpty();
    }
};

[[nodiscard]] int imageStitchingPdfTemplateCapacity(ImageStitchingPdfTemplateType type);
[[nodiscard]] qsizetype imageStitchingPdfPageCount(qsizetype itemCount,
                                                   const ImageStitchingPdfSettings& settings);
// Computes only the requested page (at most nine cells), without copying input rasters.
[[nodiscard]] ImageStitchingPdfComposedPage
calculateImageStitchingPdfComposedPage(const QVector<ImageStitchingItem>& items,
                                       const ImageStitchingPdfSettings& settings,
                                       qsizetype pageIndex);

struct ImageStitchingLayout final {
    QSize size;
    // Automatic bounds continue to determine output size and layout independently of placement.
    QVector<QRect> rects;
    QString error;
    QVector<QRect> clipRects;
    QVector<QRect> imageRects;

    [[nodiscard]] bool isValid() const {
        return size.isValid() && !size.isEmpty() && !rects.isEmpty() && error.isEmpty();
    }
};

[[nodiscard]] QSize transformedSize(const ImageStitchingItem& item);
[[nodiscard]] QImage transformedPreview(const ImageStitchingItem& item);
// Quantize once in output pixels so the preview and row exporter use identical placement.
[[nodiscard]] QRect resolveImageStitchingStaticImageRect(const ImageStitchingItem& item,
                                                         const QRect& automaticBounds,
                                                         const QRect& clipBounds);
[[nodiscard]] ImageStitchingLayout
calculateImageStitchingLayout(const QVector<ImageStitchingItem>& items,
                              const ImageStitchingSettings& settings);

class ImageStitchingDocument final : public QObject {
    Q_OBJECT

  public:
    explicit ImageStitchingDocument(QObject* parent = nullptr);
    ~ImageStitchingDocument() override;

    [[nodiscard]] const QVector<ImageStitchingItem>& items() const;
    [[nodiscard]] int indexOf(quint64 id) const;
    // Changes only when the ordered image collection or one of its transforms changes.
    [[nodiscard]] quint64 contentRevision() const;
    [[nodiscard]] ImageStitchingSettings settings() const;
    void setSettings(const ImageStitchingSettings& settings);
    [[nodiscard]] ImageStitchingOutputType outputType() const;
    void setOutputType(ImageStitchingOutputType type);
    [[nodiscard]] ImageStitchingPdfSettings pdfSettings() const;
    void setPdfSettings(const ImageStitchingPdfSettings& settings);
    [[nodiscard]] ImageStitchingPdfSettings lastValidPdfSettings() const;
    [[nodiscard]] ImageStitchingLayout layout() const;
    [[nodiscard]] QString lastError() const;

    quint64 append(QImage image, QString label, QImage preview = {});
    bool replace(quint64 id, QImage image, QString label, QImage preview = {});
    bool remove(quint64 id);
    void clear();
    bool reverseItems();
    bool swapItems(quint64 first, quint64 second);
    // finalIndex names the desired position after moving, rather than an insertion boundary.
    bool moveItem(quint64 id, int finalIndex);
    bool rotate(quint64 id, bool clockwise);
    bool flip(quint64 id, bool horizontal);
    bool resetTransform(quint64 id);
    bool setPdfPlacement(quint64 id, const ImageStitchingPdfPlacement& placement);
    bool setStaticPlacement(quint64 id, const ImageStitchingStaticPlacement& placement);
    [[nodiscard]] quint64 selectedId() const;
    void select(quint64 id);

  signals:
    void changed();
    void selectionChanged();
    void pdfPlacementChanged(quint64 id);
    void staticPlacementChanged(quint64 id);
    void customGridAdjusted(const QStringList& fields);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Immutable originals are shared. Only transformed/resized items allocate new rasters;
// the result owns these rasters and its resource reservation until the last reader retires.
[[nodiscard]] ScreenshotImageRowSource
prepareImageStitchingRows(const QVector<ImageStitchingItem>& items,
                          const ImageStitchingSettings& settings,
                          std::function<bool()> cancelled = {}, QString* error = nullptr);

// Full image materialization is deliberately bounded and intended for small verification fixtures.
[[nodiscard]] QImage materializeImageStitchingRows(const ScreenshotImageRowSource& rows,
                                                   qsizetype maximumBytes = 16 * 1024 * 1024);

// Reserve native clipboard additions before asking the existing clipboard workflow to prepare
// its PNG and native bitmap representations. Keep this owner until publication finishes.
[[nodiscard]] std::shared_ptr<void> reserveImageStitchingClipboard(QSize outputSize,
                                                                   QString* error = nullptr);

struct ImageStitchingClipboardEstimate final {
    qint64 bytes = 0;
    qint64 dibBytes = 0;
    qint64 rowScratchBytes = 0;
    QString error;

    [[nodiscard]] bool isValid() const {
        return bytes > 0 && error.isEmpty();
    }
};
[[nodiscard]] ImageStitchingClipboardEstimate estimateImageStitchingClipboard(QSize outputSize);

struct ImageStitchingResourceUsage final {
    qint64 reservedBytes = 0;
    qint64 limitBytes = 0;
};
[[nodiscard]] ImageStitchingResourceUsage imageStitchingResourceUsage();

// A single imported image is limited to 64 million pixels. Rendering reserves room for two
// four-byte rasters; use the same admission policy before capturing or importing live pins.
[[nodiscard]] qint64 imageStitchingImageImportBytes(QSize size, QString* error = nullptr);

// Admit pixels and painting scratch before capture() allocates its four-byte raster. The returned
// immutable image owns the pixel reservation, including while queued or held by an export worker.
[[nodiscard]] QImage captureImageStitchingImage(QSize size, const std::function<QImage()>& capture,
                                                QString* error = nullptr);

// Import storage is charged to the shared memory-aware budget, including after preparation
// shrinks it to retained pixels. Bounded painting/preview scratch has its own admission pool.
class ImageStitchingImportReservation final {
  public:
    ImageStitchingImportReservation();
    ~ImageStitchingImportReservation();
    bool resize(qint64 bytes, QString* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
[[nodiscard]] std::shared_ptr<ImageStitchingImportReservation>
reserveImageStitchingImport(qint64 bytes, QString* error = nullptr);
[[nodiscard]] std::shared_ptr<void> reserveImageStitchingPreview(qint64 bytes,
                                                                 QString* error = nullptr);

} // namespace snow_shot::presentation

#endif
