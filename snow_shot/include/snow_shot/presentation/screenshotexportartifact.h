#include "snow_draw_engine_qt/snow_canvas_smart_erase.h"
#include "snow_draw_engine_qt/snow_canvas_image_source_types.h"
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H

#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagerowsource.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshottextannotationrenderer.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/storage/preparedpngimage.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <variant>

class ScreenshotScrollingSnapshot;
class ScreenshotExportArtifact;
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
    ScreenshotTextAnnotationRenderSnapshot textAnnotations = {};
    std::optional<ScreenshotRecognitionImageSnapshot> recognitionOverlay = std::nullopt;
    QImage presentationImage = {};
    QRectF presentationCanvasRect = {};
    // Runs on the export worker if the display background is still being prepared.
    std::function<bool(ScreenshotRecognitionImageSnapshot&, const std::function<bool()>&)>
        prepareRecognitionOverlay = {};
};

// A styled crop retains the full composition domain and its original pixel phase.
// Admission and rendering must use this same plan, including intermediate rasters.
struct ScreenshotPinnedViewportRenderPlan final {
    QRectF renderCanvasRect;
    QSize renderPixelSize;
    ScreenshotResultLayout resultLayout;
    QRect outputPixelRect;
    bool cropAfterRender = false;
    bool composeVisibleOnly = false;
    std::optional<QRect> contentPixelRect;
    qint64 workingBytes = 0;

    [[nodiscard]] QSize outputSize() const {
        return outputPixelRect.size();
    }
};

[[nodiscard]] std::optional<ScreenshotPinnedViewportRenderPlan>
screenshotPinnedViewportRenderPlan(const ScreenshotPinnedViewportExportSource& source);

// Render the immutable source on the calling export worker, without scheduling or retaining it.
[[nodiscard]] QImage
renderScreenshotPinnedViewport(const ScreenshotPinnedViewportExportSource& source,
                               const std::function<bool()>& cancelled = {});

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
    // Called on the artifact's GUI thread. False rejects without a completion; true accepts
    // and permits an inline completion. An accepted, live, uncancelled request must complete
    // once on that same thread; receiver destruction may retire its callback. Freeze mutable
    // UI state before construction; the loader must not hand live widgets to a worker.
    using ImageLoader = std::function<bool(QObject*, std::function<void(QImage)>)>;
    // These synchronous callbacks run on export workers. Captured sources/row readers must
    // own their immutable inputs and remain valid until execution actually retires, including
    // after cancellation. They must not access GUI objects or artifact/cache request APIs.
    using ImageProducer = std::function<QImage(const ScreenshotExportCancellation& cancellation)>;
    using ImagePreparation = std::function<QImage(QImage, const ScreenshotExportCancellation&)>;
    using RowSourceFactory =
        std::function<ScreenshotImageRowSource(std::function<bool()> cancellationRequested)>;

    ScreenshotExportSource() = default;

    [[nodiscard]] static ScreenshotExportSource
    fromImage(QImage image, std::optional<ScreenshotClipboardPlacement> placement = {},
              std::optional<ScreenshotClipboardAppearance> appearance = {});
    // Derive an independent output from shared prepared rows, retaining the original artifact.
    // Cancelling the derived output does not cancel the original pin or its history subscribers.
    [[nodiscard]] static ScreenshotExportSource
    fromArtifact(std::shared_ptr<ScreenshotExportArtifact> artifact);
    [[nodiscard]] static ScreenshotExportSource
    fromRecognitionImage(ScreenshotRecognitionImageSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromScrollingSnapshot(ScreenshotScrollingSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromPinnedViewport(ScreenshotPinnedViewportExportSource source);
    [[nodiscard]] static ScreenshotExportSource
    fromImageLoader(ImageLoader loader, std::optional<ScreenshotClipboardPlacement> placement = {},
                    std::optional<ScreenshotClipboardAppearance> appearance = {});
    // Acquire asynchronously without a worker, then admit and prepare the delivered pixels.
    [[nodiscard]] static ScreenshotExportSource
    fromDeferredImage(ImageLoader loader, ImagePreparation preparation,
                      std::function<qint64(QSize)> workingBytes,
                      std::optional<ScreenshotClipboardPlacement> placement = {},
                      std::optional<ScreenshotClipboardAppearance> appearance = {});
    [[nodiscard]] static ScreenshotExportSource
    fromProducer(ImageProducer producer, RowSourceFactory rowSourceFactory = {},
                 std::optional<ScreenshotClipboardPlacement> placement = {},
                 std::optional<ScreenshotClipboardAppearance> appearance = {});

    // GUI-thread pinned snapshots only. The key identifies immutable pixels across commands;
    // it excludes placement/publication metadata. Change it whenever any exported pixel changes.
    void setEncodingCacheKey(QByteArray key) {
        m_encodingCacheKey = std::move(key);
    }

    // Freeze on the source before constructing an artifact. Capture pixels stay unchanged.
    void setOutputScale(qreal scale) {
        m_outputScale = scale;
    }

    [[nodiscard]] ScreenshotExportSource withOutputScale(qreal scale) && {
        setOutputScale(scale);
        return std::move(*this);
    }

    [[nodiscard]] bool isValid() const;

  private:
    ImageLoader m_imageLoader;
    ImageProducer m_imageProducer;
    ImagePreparation m_loadedImagePreparation;
    std::function<qint64(QSize)> m_loadedImageWorkingBytes;
    RowSourceFactory m_rowSourceFactory;
    std::function<bool(QObject*, std::function<void(ScreenshotImageRowSource, QString)>)>
        m_rowSourceLoader;
    std::optional<ScreenshotClipboardPlacement> m_clipboardPlacement;
    std::optional<ScreenshotClipboardAppearance> m_clipboardAppearance;
    qint64 m_renderingBytes = 0;
    QByteArray m_encodingCacheKey;
    qreal m_outputScale = 1.0;

    void prepareOutputScale();

    friend class ScreenshotExportArtifact;
};

// Construct, request, inspect cached products, cancel and destroy on the GUI thread. Internal
// mutexes protect worker/delivery handoffs, not arbitrary-thread access to the shared brokers
// and completed-output caches. Keep the artifact's QObject affinity on that thread.
//
// A false request result rejects without invoking its callback. Accepted results may arrive
// inline on the receiver's thread, otherwise delivery is queued there. Destroying the receiver
// or cancelling the artifact suppresses undelivered callbacks; it does not join active workers
// or release their source/resource leases early. Consumer-owned completed results survive
// artifact destruction and application idle-cache trimming independently.
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
    struct SharedEncodings;
    static SharedEncodings& sharedEncodings();
    std::shared_ptr<ScreenshotExportArtifact> encodingBroker(const ScreenshotImageRowSource& rows);

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
    void assertOwnerThread() const;

    void startImage();
    void prepareLoadedImage(QImage image);
    void completeImage(ScreenshotExportImageResult result);
    void startRowSource();
    void prepareScaledRowSource(ScreenshotImageRowSource source);
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
