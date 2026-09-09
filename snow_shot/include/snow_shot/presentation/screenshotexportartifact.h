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

class ScreenshotScrollingSnapshot;
struct ScreenshotRecognitionImageSnapshot;

struct ScreenshotPinnedViewportExportSource final {
    QByteArray documentSession;
    QImage backgroundImage;
    QRectF backgroundCanvasRect;
    QSize contentPixelSize;
    ScreenshotResultStyle resultStyle;
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

    [[nodiscard]] static ScreenshotExportSource fromImage(QImage image);
    [[nodiscard]] static ScreenshotExportSource
    fromRecognitionImage(ScreenshotRecognitionImageSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromScrollingSnapshot(ScreenshotScrollingSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromPinnedViewport(ScreenshotPinnedViewportExportSource source);
    [[nodiscard]] static ScreenshotExportSource fromImageLoader(ImageLoader loader);
    [[nodiscard]] static ScreenshotExportSource
    fromProducer(ImageProducer producer, RowSourceFactory rowSourceFactory = {});

    [[nodiscard]] bool isValid() const;

  private:
    ImageLoader m_imageLoader;
    ImageProducer m_imageProducer;
    RowSourceFactory m_rowSourceFactory;

    friend class ScreenshotExportArtifact;
};

class ScreenshotExportArtifact final : public QObject {
  public:
    using ImageCallback = std::function<void(ScreenshotExportImageResult)>;
    using EncodingCallback = std::function<void(ScreenshotExportEncodingResult)>;
    using ClipboardCallback = std::function<void(ScreenshotExportClipboardResult)>;

    explicit ScreenshotExportArtifact(ScreenshotExportSource source, QObject* parent = nullptr);
    ~ScreenshotExportArtifact() override;

    ScreenshotExportArtifact(const ScreenshotExportArtifact&) = delete;
    ScreenshotExportArtifact& operator=(const ScreenshotExportArtifact&) = delete;

    [[nodiscard]] bool requestImage(QObject* receiver, ImageCallback callback);
    using RowSourceCallback = std::function<void(ScreenshotImageRowSource, QString)>;
    [[nodiscard]] bool requestRowSource(QObject* receiver, RowSourceCallback callback);
    [[nodiscard]] bool requestCanonicalPng(QObject* receiver, EncodingCallback callback);
    [[nodiscard]] bool adoptCanonicalPng(snow_shot::storage::PreparedPngImage image);
    [[nodiscard]] bool requestClipboard(QObject* receiver, ClipboardCallback callback);
    [[nodiscard]] bool requestAutomaticSave(QObject* receiver, QStringList directories,
                                            ScreenshotImageFileFormat format,
                                            QString filenameFormat,
                                            ScreenshotExportCoordinator::Completion callback);
    [[nodiscard]] bool requestQuickSave(QObject* receiver,
                                        ScreenshotExportCoordinator::Completion callback);
    void cancel();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancelled() const;
    [[nodiscard]] QString diagnosticId() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    void startImage();
    void completeImage(ScreenshotExportImageResult result);
    void startRowSource();
    void startRowSourceFromImage(QImage image);
    void completeRowSource(ScreenshotImageRowSource source, QString error);
    void startCanonicalPng();
    void startCanonicalPngFromRows(ScreenshotImageRowSource source);
    void completeCanonicalPng(ScreenshotExportEncodingResult result);
    [[nodiscard]] bool prepareClipboard(QObject* receiver, QByteArray canonicalPng,
                                        ClipboardCallback callback);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
