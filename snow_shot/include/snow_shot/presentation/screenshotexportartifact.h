#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H

#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagerowsource.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/preparedpngimage.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class ScreenshotScrollingSnapshot;

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
    fromScrollingSnapshot(ScreenshotScrollingSnapshot snapshot);
    [[nodiscard]] static ScreenshotExportSource
    fromPinnedViewport(ScreenshotPinnedViewportExportSource source);
    [[nodiscard]] static ScreenshotExportSource fromImageLoader(ImageLoader loader);
    [[nodiscard]] static ScreenshotExportSource
    fromProducer(ImageProducer producer, RowSourceFactory rowSourceFactory = {});

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool hasDirectRowSource() const;

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
    [[nodiscard]] bool requestCanonicalPng(QObject* receiver, EncodingCallback callback);
    [[nodiscard]] bool requestClipboard(QObject* receiver, ScreenshotClipboardFormatMode formatMode,
                                        ClipboardCallback callback);
    void cancel();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancelled() const;
    [[nodiscard]] bool hasDirectRowSource() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    void startImage();
    void completeImage(ScreenshotExportImageResult result);
    void startCanonicalPng();
    void startCanonicalPngFromImage(QImage image);
    void completeCanonicalPng(ScreenshotExportEncodingResult result);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTARTIFACT_H
