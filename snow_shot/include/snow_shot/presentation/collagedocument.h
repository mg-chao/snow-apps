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

enum class CollageDirection { Vertical, Horizontal, Custom };
enum class CollageSizing { Original, Uniform };
enum class CollageOutputType { StaticImage, PdfDocument };
enum class CollagePdfPaperSize { A3, A4, A5, B5, Letter, Legal, Custom, ImageSize };
enum class CollagePdfOrientation { Portrait, Landscape };
enum class CollagePdfExportMode { Combined, PerPage };
enum class CollagePdfStitchingMethod { PageBased, TemplateBased };
enum class CollagePdfTemplateType {
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

struct CollagePdfSettings final {
    CollagePdfStitchingMethod stitchingMethod = CollagePdfStitchingMethod::PageBased;
    CollagePdfTemplateType templateType = CollagePdfTemplateType::TwoImages;
    qreal imageSpacingMm = 5;
    CollagePdfPaperSize paperSize = CollagePdfPaperSize::A4;
    CollagePdfOrientation orientation = CollagePdfOrientation::Portrait;
    qreal customWidthMm = 210;
    qreal customHeightMm = 297;
    qreal marginTopMm = 10;
    qreal marginBottomMm = 10;
    qreal marginLeftMm = 10;
    qreal marginRightMm = 10;
    CollagePdfExportMode exportMode = CollagePdfExportMode::Combined;

    // Treat two NaNs as the same incomplete field so UI refreshes do not emit repeatedly.
    friend bool operator==(const CollagePdfSettings&, const CollagePdfSettings&);
};

struct CollagePdfPageLayout final {
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

// Validation keys match CollageSettingsStore's PDF field names. Geometry is top-left based,
// in millimeters, and is shared by screen previews and PDF export. ImageSize uses transformed
// pixels at 96 DPI with no margins or paper-orientation adjustment.
[[nodiscard]] QMap<QString, QString> validateCollagePdfSettings(const CollagePdfSettings& settings);
[[nodiscard]] CollagePdfPageLayout
calculateCollagePdfPageLayout(QSize transformedPixels, const CollagePdfSettings& settings);

struct CollageSettings final {
    CollageDirection direction = CollageDirection::Vertical;
    CollageSizing sizing = CollageSizing::Uniform;
    int border = 0;
    int spacing = 0;
    QColor background = Qt::white;
    int rows = 1;
    int columns = 1;
    bool customGridInitialized = false;

    friend bool operator==(const CollageSettings&, const CollageSettings&) = default;
};

struct CollageImageMetadata;

struct CollagePdfPlacement final {
    QPointF centerOffsetMillimeters;
    // An unset scale fits the complete image inside its cell, centered and aspect-preserving.
    // Resolve against full-resolution transformed pixels.
    std::optional<qreal> millimetersPerPixel;

    friend bool operator==(const CollagePdfPlacement&, const CollagePdfPlacement&) = default;
};

struct CollageStaticPlacement final {
    QPointF centerOffsetPixels;
    // Unset uses the automatic layout's image bounds. Scale is relative to transformed source
    // pixels.
    std::optional<qreal> pixelsPerPixel;

    friend bool operator==(const CollageStaticPlacement&, const CollageStaticPlacement&) = default;
};

struct CollageItem final {
    quint64 id = 0;
    quint64 revision = 0;
    QImage image;
    QImage preview;
    QTransform transform;
    QString label;
    std::shared_ptr<void> resourceOwner;
    // Shared memoization belongs to the immutable original, independently of layout/transforms.
    std::shared_ptr<CollageImageMetadata> metadata;
    CollagePdfPlacement pdfPlacement{};
    CollageStaticPlacement staticPlacement{};
};

struct CollagePdfCell final {
    quint64 itemId = 0;
    qsizetype itemIndex = -1;
    QRectF clipMillimeters;
    QRectF imageMillimeters;
};

struct CollagePdfComposedPage final {
    QSizeF pageMillimeters;
    QRectF contentMillimeters;
    QVector<CollagePdfCell> cells;
    QString error;

    [[nodiscard]] bool isValid() const {
        return pageMillimeters.isValid() && !pageMillimeters.isEmpty() &&
               !contentMillimeters.isEmpty() && !cells.isEmpty() && error.isEmpty();
    }
};

[[nodiscard]] int collagePdfTemplateCapacity(CollagePdfTemplateType type);
[[nodiscard]] qsizetype collagePdfPageCount(qsizetype itemCount,
                                            const CollagePdfSettings& settings);
// Computes only the requested page (at most nine cells), without copying input rasters.
[[nodiscard]] CollagePdfComposedPage
calculateCollagePdfComposedPage(const QVector<CollageItem>& items,
                                const CollagePdfSettings& settings, qsizetype pageIndex);

struct CollageLayout final {
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

[[nodiscard]] QSize transformedSize(const CollageItem& item);
[[nodiscard]] QImage transformedPreview(const CollageItem& item);
// Quantize once in output pixels so the preview and row exporter use identical placement.
[[nodiscard]] QRect resolveCollageStaticImageRect(const CollageItem& item,
                                                  const QRect& automaticBounds,
                                                  const QRect& clipBounds);
[[nodiscard]] CollageLayout calculateCollageLayout(const QVector<CollageItem>& items,
                                                   const CollageSettings& settings);

class CollageDocument final : public QObject {
    Q_OBJECT

  public:
    explicit CollageDocument(QObject* parent = nullptr);
    ~CollageDocument() override;

    [[nodiscard]] const QVector<CollageItem>& items() const;
    [[nodiscard]] int indexOf(quint64 id) const;
    // Changes only when the ordered image collection or one of its transforms changes.
    [[nodiscard]] quint64 contentRevision() const;
    [[nodiscard]] CollageSettings settings() const;
    void setSettings(const CollageSettings& settings);
    [[nodiscard]] CollageOutputType outputType() const;
    void setOutputType(CollageOutputType type);
    [[nodiscard]] CollagePdfSettings pdfSettings() const;
    void setPdfSettings(const CollagePdfSettings& settings);
    [[nodiscard]] CollagePdfSettings lastValidPdfSettings() const;
    [[nodiscard]] CollageLayout layout() const;
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
    bool setPdfPlacement(quint64 id, const CollagePdfPlacement& placement);
    bool setStaticPlacement(quint64 id, const CollageStaticPlacement& placement);
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
[[nodiscard]] ScreenshotImageRowSource prepareCollageRows(const QVector<CollageItem>& items,
                                                          const CollageSettings& settings,
                                                          std::function<bool()> cancelled = {},
                                                          QString* error = nullptr);

// Full image materialization is deliberately bounded and intended for small verification fixtures.
[[nodiscard]] QImage materializeCollageRows(const ScreenshotImageRowSource& rows,
                                            qsizetype maximumBytes = 16 * 1024 * 1024);

// Reserve native clipboard additions before asking the existing clipboard workflow to prepare
// its PNG and native bitmap representations. Keep this owner until publication finishes.
[[nodiscard]] std::shared_ptr<void> reserveCollageClipboard(QSize outputSize,
                                                            QString* error = nullptr);

struct CollageClipboardEstimate final {
    qint64 bytes = 0;
    qint64 dibBytes = 0;
    qint64 rowScratchBytes = 0;
    QString error;

    [[nodiscard]] bool isValid() const {
        return bytes > 0 && error.isEmpty();
    }
};
[[nodiscard]] CollageClipboardEstimate estimateCollageClipboard(QSize outputSize);

struct CollageResourceUsage final {
    qint64 reservedBytes = 0;
    qint64 limitBytes = 0;
};
[[nodiscard]] CollageResourceUsage collageResourceUsage();

// A single imported image is limited to 64 million pixels. Rendering reserves room for two
// four-byte rasters; use the same admission policy before capturing or importing live pins.
[[nodiscard]] qint64 collageImageImportBytes(QSize size, QString* error = nullptr);

// Admit pixels and painting scratch before capture() allocates its four-byte raster. The returned
// immutable image owns the pixel reservation, including while queued or held by an export worker.
[[nodiscard]] QImage captureCollageImage(QSize size, const std::function<QImage()>& capture,
                                         QString* error = nullptr);

// Import storage is charged to the shared memory-aware budget, including after preparation
// shrinks it to retained pixels. Bounded painting/preview scratch has its own admission pool.
class CollageImportReservation final {
  public:
    CollageImportReservation();
    ~CollageImportReservation();
    bool resize(qint64 bytes, QString* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
[[nodiscard]] std::shared_ptr<CollageImportReservation>
reserveCollageImport(qint64 bytes, QString* error = nullptr);
[[nodiscard]] std::shared_ptr<void> reserveCollagePreview(qint64 bytes, QString* error = nullptr);

} // namespace snow_shot::presentation

#endif
