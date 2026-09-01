#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H

#include <QImage>
#include <QColor>
#include <QObject>
#include <QRectF>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>

inline constexpr qint64 kScreenshotOcrMaximumPixels = 3840LL * 2160LL;

[[nodiscard]] inline bool screenshotOcrImageWithinPixelLimit(const QSize& size) {
    if (size.width() < 1 || size.height() < 1) {
        return false;
    }
    return static_cast<qint64>(size.width()) * static_cast<qint64>(size.height()) <=
           kScreenshotOcrMaximumPixels;
}

class ScreenshotOcrPresentation;
struct ScreenshotOcrRecognitionResult {
    std::shared_ptr<ScreenshotOcrPresentation> presentation;
    QString error;
    // Transient worker-produced image. It is never part of the OCR cache.
    QImage filteredImage;
    // Canvas-space rect covered by filteredImage; the image is a crop of the
    // request image covering the text regions plus the blur support margin.
    QRectF filteredImageCanvasRect;
};

enum class ScreenshotOcrRequestPriority { Interactive, Prefetch };

enum class ScreenshotOcrBackendPreference { Cpu, DirectMl };

struct ScreenshotOcrRequest {
    QImage image;
    QRectF canvasRect;
    ScreenshotOcrRequestPriority priority = ScreenshotOcrRequestPriority::Interactive;
    bool renderFilteredImage = false;
    bool renderOnly = false;
    std::shared_ptr<ScreenshotOcrPresentation> presentation;
    QColor backgroundColor;
};

class ScreenshotOcrRecognitionPort : public QObject {
  public:
    explicit ScreenshotOcrRecognitionPort(QObject* parent = nullptr) : QObject(parent) {}
    using RequestToken = quint64;
    using Completion = std::function<void(ScreenshotOcrRecognitionResult)>;

    virtual ~ScreenshotOcrRecognitionPort() = default;
    virtual RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                                   Completion completion) = 0;
    // Optional render-only operation. The default keeps lightweight test and
    // alternate implementations source-compatible; the production service
    // performs this Qt-side image work off the caller thread.
    virtual RequestToken render(ScreenshotOcrRequest request, QObject* receiver,
                                Completion completion) {
        Q_UNUSED(request);
        Q_UNUSED(receiver);
        Q_UNUSED(completion);
        return 0;
    }
    // Updates whether an in-flight recognition job should render its transient
    // filtered image before leaving the worker. This lets a prefetch become
    // visible, or an abandoned interactive request suppress rendering, without
    // cancelling recognition and recreating its worker.
    virtual bool setRenderFilteredImage(RequestToken token, bool enabled,
                                        const QColor& backgroundColor = {}) {
        Q_UNUSED(token);
        Q_UNUSED(enabled);
        Q_UNUSED(backgroundColor);
        return false;
    }
    virtual void cancel(RequestToken token) = 0;
    virtual bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) = 0;
    // Implementations that manage disk-backed models report whether the
    // required files are already available before a request is queued.
    virtual bool modelFilesReady() const { return true; }
};

class ScreenshotOcrRecognitionService final : public ScreenshotOcrRecognitionPort {
    Q_OBJECT

  public:
    struct Options {
        // Maximum concurrent workers in the OCR child process.
        int workerCount = 2;
        QString modelStoreDirectory;
        // Resolved HTTP(S) proxy URL for model downloads. Empty means direct access.
        QString proxyUrl;
        // Optional executable override, primarily for tests and portable bundles.
        QString processPath;
    };

    explicit ScreenshotOcrRecognitionService(QObject* parent = nullptr);
    explicit ScreenshotOcrRecognitionService(ScreenshotOcrBackendPreference backendPreference,
                                             QObject* parent = nullptr);
    explicit ScreenshotOcrRecognitionService(const Options& options,
                                             ScreenshotOcrBackendPreference backendPreference =
                                                 ScreenshotOcrBackendPreference::Cpu,
                                             QObject* parent = nullptr);
    ~ScreenshotOcrRecognitionService() override;

    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override;
    RequestToken render(ScreenshotOcrRequest request, QObject* receiver,
                        Completion completion) override;
    bool setRenderFilteredImage(RequestToken token, bool enabled,
                                const QColor& backgroundColor = {}) override;
    void cancel(RequestToken token) override;
    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) override;
    [[nodiscard]] bool modelFilesReady() const override;
    void setBackendPreference(ScreenshotOcrBackendPreference preference);
    void setProxyUrl(const QString& proxyUrl);
    [[nodiscard]] int liveWorkerCount() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    RequestToken m_nextToken = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRRECOGNITIONSERVICE_H
