#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"
#include "snow_draw_engine_qt/snow_canvas_image_source_types.h"
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H

#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagerowsource.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/preparedpngimage.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <variant>

class ScreenshotScrollingSnapshot;
struct ScreenshotRecognitionImageSnapshot;

struct ScreenshotPinnedViewportExportSource final {
    QByteArray documentSession;
    QImage backgroundImage;
    QRectF backgroundCanvasRect;
    QSize contentPixelSize;
    ScreenshotResultStyle resultStyle;
    SnowCanvasSmartEraseSnapshot smartErase;
    qreal outputOpacity = 1.0;
    std::optional<ScreenshotClipboardPlacement> clipboardPlacement = std::nullopt;
    std::optional<ScreenshotClipboardAppearance> clipboardAppearance = std::nullopt;
    // Original sampling context may extend beyond the visible background image.
    QList<SnowCanvasBaseImageSource> samplingSources = {};
    // Optional output clip in the unchanged document coordinate system.
    std::optional<QRectF> visibleCanvasRect = std::nullopt;
};

struct ScreenshotExportImageResult final {
    QImage image;
    QString error;

    [[nodiscard]] bool succeeded() const {
        return !image.isNull() && error.isEmpty();
    }
};

struct ScreenshotExportEncodingResult final {
    snow_shot::storage::PreparedPngImage image;
    QString error;

    [[nodiscard]] bool succeeded() const {
        return image.isValid() && error.isEmpty();
    }
};

struct ScreenshotExportClipboardResult final {
    ScreenshotClipboardPayload payload;
    QString error;

    [[nodiscard]] bool succeeded() const {
        return payload.isValid() && error.isEmpty();
    }
};

class ScreenshotExportSource final {
  public:
    using ImageLoader = std::function<bool(QObject*, std::function<void(QImage)>)>;
    using ImageProducer = std::function<QImage(const ScreenshotExportCancellation& cancellation)>;
    using RowSourceFactory =
        std::function<ScreenshotImageRowSource(std::function<bool()> cancellationRequested)>;

    ScreenshotExportSource() = default;

    [[nodiscard]] static ScreenshotExportSource
    fromImage(QImage image, std::optional<ScreenshotClipboardPlacement> placement = {},
              std::optional<ScreenshotClipboardAppearance> appearance = {});
    [[nodiscard]] static ScreenshotExportSource
    fromRecognitionImage(ScreenshotRecognitionImageSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromScrollingSnapshot(ScreenshotScrollingSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromPinnedViewport(ScreenshotPinnedViewportExportSource source);
    [[nodiscard]] static ScreenshotExportSource
    fromImageLoader(ImageLoader loader, std::optional<ScreenshotClipboardPlacement> placement = {},
                    std::optional<ScreenshotClipboardAppearance> appearance = {});
    [[nodiscard]] static ScreenshotExportSource
    fromProducer(ImageProducer producer, RowSourceFactory rowSourceFactory = {},
                 std::optional<ScreenshotClipboardPlacement> placement = {},
                 std::optional<ScreenshotClipboardAppearance> appearance = {});

    [[nodiscard]] bool isValid() const;

  private:
    ImageLoader m_imageLoader;
    ImageProducer m_imageProducer;
    RowSourceFactory m_rowSourceFactory;
    std::optional<ScreenshotClipboardPlacement> m_clipboardPlacement;
    std::optional<ScreenshotClipboardAppearance> m_clipboardAppearance;

    friend class ScreenshotExportArtifact;
};

class ScreenshotExportArtifact final : public QObject {
  public:
    struct PngCachePolicy {
        // Bounds retained PNG and other encoded bytes together, not in-flight
        // encoders or consumer-owned bytes. Larger file encodings spill to disk.
        qsizetype maximumBytes = 64 * 1024 * 1024;
        // Optional internal instrumentation, invoked on the encoder worker only when encoding.
        std::function<void()> encodingStarted;
        // Prepared file/PDF entries have an independent retained-output bound;
        // memory-backed entries also count toward maximumBytes. In-flight
        // consumers keep outputs alive even when too large to retain.
        qint64 maximumFileBytes = 256 * 1024 * 1024;
        std::function<void(ScreenshotImageFileFormat)> fileEncodingStarted = {};
    };
    using ImageCallback = std::function<void(ScreenshotExportImageResult)>;
    using EncodingCallback = std::function<void(ScreenshotExportEncodingResult)>;
    using ClipboardCallback = std::function<void(ScreenshotExportClipboardResult)>;

    explicit ScreenshotExportArtifact(ScreenshotExportSource source, QObject* parent = nullptr);
    ScreenshotExportArtifact(ScreenshotExportSource source,
                             ScreenshotCompressionLevel compressionLevel,
                             QObject* parent = nullptr);
    ScreenshotExportArtifact(ScreenshotExportSource source,
                             ScreenshotCompressionLevel compressionLevel,
                             PngCachePolicy cachePolicy, QObject* parent = nullptr);
    ~ScreenshotExportArtifact() override;

    ScreenshotExportArtifact(const ScreenshotExportArtifact&) = delete;
    ScreenshotExportArtifact& operator=(const ScreenshotExportArtifact&) = delete;

    // Set before starting a live capture export; never persisted with image content.
    void setWindowNames(snow_shot::storage::ScreenshotWindowNames names);
    [[nodiscard]] snow_shot::storage::ScreenshotWindowNames windowNames() const;

    [[nodiscard]] bool requestImage(QObject* receiver, ImageCallback callback);
    using RowSourceCallback = std::function<void(ScreenshotImageRowSource, QString)>;
    [[nodiscard]] bool requestRowSource(QObject* receiver, RowSourceCallback callback);
    [[nodiscard]] bool requestCanonicalPng(QObject* receiver, EncodingCallback callback);
    // Manual exports can stream large images instead of allocating a complete PNG.
    [[nodiscard]] bool shouldCachePng(QSize pixelSize) const;
    [[nodiscard]] snow_shot::storage::PreparedPngImage
    cachedPng(ScreenshotCompressionLevel compression);
    // PNG requests with the same effective compression share one in-flight/cached encoding.
    [[nodiscard]] bool requestPng(QObject* receiver, ScreenshotCompressionLevel compression,
                                  EncodingCallback callback);
    [[nodiscard]] bool requestClipboard(QObject* receiver, ClipboardCallback callback);
    // Prepare image data and a temporary exported file for one clipboard publication.
    [[nodiscard]] bool requestClipboardFile(QObject* receiver, ScreenshotImageFileFormat format,
                                            QString filenameFormat,
                                            ScreenshotImageEncodingOptions encoding,
                                            ClipboardCallback callback,
                                            ScreenshotPdfOptions pdf = {},
                                            QDateTime requestedAt = {});
    [[nodiscard]] std::optional<ScreenshotClipboardPlacement> clipboardPlacement() const;
    [[nodiscard]] std::optional<ScreenshotClipboardAppearance> clipboardAppearance() const;
    // Retain temporary output ownership until asynchronous writers finish after cancellation.
    [[nodiscard]] bool requestSaveToPath(QObject* receiver, QString path,
                                         ScreenshotImageFileFormat format,
                                         ScreenshotImageEncodingOptions encoding,
                                         ScreenshotExportCoordinator::Completion callback,
                                         ScreenshotPdfOptions pdf = {},
                                         std::shared_ptr<void> keepAlive = {});
    [[nodiscard]] bool requestAutomaticSave(QObject* receiver, QStringList directories,
                                            ScreenshotImageFileFormat format,
                                            QString filenameFormat,
                                            ScreenshotImageEncodingOptions encoding,
                                            ScreenshotExportCoordinator::Completion callback,
                                            ScreenshotPdfOptions pdf = {},
                                            QDateTime requestedAt = {});
    // Capture the configured path without filesystem access; resolve existing-directory
    // preference and platform fallbacks on the export worker when saving.
    [[nodiscard]] bool requestAutomaticSaveToConfiguredDirectory(
        QObject* receiver, QString configuredDirectory, ScreenshotImageFileFormat format,
        QString filenameFormat, ScreenshotImageEncodingOptions encoding,
        ScreenshotExportCoordinator::Completion callback, ScreenshotPdfOptions pdf = {},
        QDateTime requestedAt = {});
    [[nodiscard]] bool requestQuickSave(QObject* receiver,
                                        ScreenshotExportCoordinator::Completion callback,
                                        QDateTime requestedAt = {});
    // Retire pending and queued deliveries on every receiver thread. Callbacks that
    // have already started may finish; cancellation never waits for consumer code.
    void cancel();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancelled() const;
    [[nodiscard]] QString diagnosticId() const;

  private:
    struct FileSource;
    using FileSourceCallback = std::function<void(std::shared_ptr<FileSource>, QString)>;
    [[nodiscard]] bool requestFileSource(ScreenshotImageFileFormat format,
                                         ScreenshotImageEncodingOptions encoding,
                                         FileSourceCallback callback);
    void requestPreparedFile(ScreenshotImageRowSource rows, ScreenshotImageFileFormat format,
                             ScreenshotImageEncodingOptions encoding, FileSourceCallback callback);
    struct ClipboardFileDestination {};
    using FileSaveDestination = std::variant<QStringList, QString, ClipboardFileDestination>;
    [[nodiscard]] bool requestFileSave(QObject* receiver, FileSaveDestination destination,
                                       ScreenshotImageFileFormat format, QString filenameFormat,
                                       ScreenshotImageEncodingOptions encoding,
                                       ScreenshotExportCoordinator::Completion callback,
                                       ScreenshotPdfOptions pdf, QDateTime requestedAt,
                                       ScreenshotExportCoordinator::Priority priority);
    // Freeze naming context before asynchronous preparation; all sources use the same writer.
    [[nodiscard]] FileSourceCallback
    makeFileSaveCallback(QObject* receiver, FileSaveDestination destination,
                         ScreenshotImageFileFormat format, QString filenameFormat,
                         ScreenshotExportCoordinator::Completion callback, ScreenshotPdfOptions pdf,
                         QDateTime requestedAt, ScreenshotExportCoordinator::Priority priority);
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    void startImage();
    void completeImage(ScreenshotExportImageResult result);
    void startRowSource();
    void startRowSourceFromImage(QImage image);
    void completeRowSource(ScreenshotImageRowSource source, QString error);
    void startPng(int compressionLevel);
    void startPngFromRows(int compressionLevel, ScreenshotImageRowSource source);
    void completePng(int compressionLevel, ScreenshotExportEncodingResult result,
                     std::shared_ptr<ScreenshotClipboardPayload> clipboard = {});
    [[nodiscard]] bool prepareClipboard(QObject* receiver, QByteArray canonicalPng,
                                        ClipboardCallback callback);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
